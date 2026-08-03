// test_sqlite_native.cpp — exercises the real sqlite3 backend end to end:
// open/close, DDL, insert/bind of every value kind, query/get/all, changes and
// lastInsertRowid, transactions (commit + rollback) and nested savepoints.
import std;
import mbun.sqlite;

namespace {

int failed{0};

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failed;
    }
}

using namespace mbun::sqlite;

template <typename T>
const T& value_as(const SqlValue& v) {
    return std::get<T>(v.value());
}

} // namespace

int main() {
    // libversion is wired to the real amalgamation.
    expect(libversion().rfind("3.", 0) == 0, "sqlite libversion looks like 3.x");

    auto opened = open(":memory:");
    expect(opened.has_value(), "open(:memory:) succeeds");
    if (!opened) {
        std::cerr << "open error: " << opened.error().message << '\n';
        return 1;
    }
    Database& db = *opened;
    expect(!db.is_closed(), "db open");

    // ---- DDL ----
    {
        auto r = db.run("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, weight REAL, "
                        "data BLOB, flag INTEGER)");
        expect(r.has_value(), "create table");
    }

    // ---- insert with bound params of each kind ----
    {
        std::vector<SqlValue> params;
        params.emplace_back(std::string{"alice"});           // TEXT
        params.emplace_back(3.5);                             // REAL
        params.emplace_back(std::vector<std::byte>{           // BLOB
            std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}});
        params.emplace_back(true);                            // Boolean -> int 1
        auto r = db.run("INSERT INTO t (name, weight, data, flag) VALUES (?, ?, ?, ?)", params);
        expect(r.has_value(), "insert bound row 1");
        if (r) {
            expect(r->changes == 1, "insert changes == 1");
            expect(r->last_insert_rowid == 1, "first rowid == 1");
        }
    }
    {
        // NULL binding + integer.
        std::vector<SqlValue> params;
        params.emplace_back(std::string{"bob"});
        params.emplace_back(nullptr);                         // NULL weight
        params.emplace_back(nullptr);                         // NULL data
        params.emplace_back(std::int64_t{0});
        auto r = db.run("INSERT INTO t (name, weight, data, flag) VALUES (?, ?, ?, ?)", params);
        expect(r.has_value(), "insert bound row 2");
        if (r) expect(r->last_insert_rowid == 2, "second rowid == 2");
    }

    // ---- query all ----
    {
        auto stmt = db.prepare("SELECT id, name, weight, data, flag FROM t ORDER BY id");
        auto rows = stmt.all();
        expect(rows.has_value(), "select all ok");
        if (rows) {
            expect(rows->size() == 2, "two rows returned");
            const Row& a = rows->front();
            expect(a.columns().size() == 5, "five columns");
            expect(a.columns()[1] == "name", "column name is 'name'");
            expect(value_as<std::int64_t>(*a.at("id")) == 1, "row1 id == 1");
            expect(value_as<std::string>(*a.at("name")) == "alice", "row1 name alice");
            expect(value_as<double>(*a.at("weight")) == 3.5, "row1 weight 3.5");
            const auto& blob = value_as<std::vector<std::byte>>(*a.at("data"));
            expect(blob.size() == 4 && blob[0] == std::byte{0xDE} && blob[3] == std::byte{0xEF},
                   "row1 blob roundtrip");
            expect(value_as<std::int64_t>(*a.at("flag")) == 1, "row1 flag true->1");

            const Row& b = (*rows)[1];
            expect(b.at("weight")->is_null(), "row2 weight NULL");
            expect(b.at("data")->is_null(), "row2 data NULL");
            expect(value_as<std::string>(*b.at("name")) == "bob", "row2 name bob");
        }
    }

    // ---- get (single row) with a positional param ----
    {
        auto stmt = db.prepare("SELECT name FROM t WHERE id = ?");
        stmt.bind({SqlValue{std::int64_t{2}}});
        auto row = stmt.get();
        expect(row.has_value(), "get ok");
        if (row) {
            expect(row->has_value(), "get found a row");
            if (*row) expect(value_as<std::string>(*(**row).at("name")) == "bob", "get name bob");
        }
    }

    // ---- statement caching: same SQL executed twice returns fresh results ----
    {
        auto count_stmt = db.prepare("SELECT COUNT(*) AS n FROM t");
        auto r1 = count_stmt.get();
        expect(r1 && *r1 && value_as<std::int64_t>(*(**r1).at("n")) == 2, "count == 2");
    }

    // ---- transaction commit ----
    {
        auto tx = db.transaction();
        auto result = tx.run(TransactionMode::deferred, [&]() -> std::expected<void, SqlError> {
            auto ins = db.run("INSERT INTO t (name, flag) VALUES ('carol', 1)");
            if (!ins) return std::unexpected(ins.error());
            expect(db.in_transaction(), "in transaction during body");
            return {};
        });
        expect(result.has_value(), "transaction commit ok");
        expect(!db.in_transaction(), "not in transaction after commit");
        auto n = db.prepare("SELECT COUNT(*) AS n FROM t").get();
        expect(n && *n && value_as<std::int64_t>(*(**n).at("n")) == 3, "count == 3 after commit");
    }

    // ---- transaction rollback on error ----
    {
        auto tx = db.transaction();
        auto result = tx.run(TransactionMode::immediate, [&]() -> std::expected<void, SqlError> {
            (void)db.run("INSERT INTO t (name, flag) VALUES ('dave', 1)");
            return std::unexpected(SqlError{"forced rollback", "SQLITE_ERROR"});
        });
        expect(!result.has_value(), "transaction body error propagates");
        expect(!db.in_transaction(), "not in transaction after rollback");
        auto n = db.prepare("SELECT COUNT(*) AS n FROM t").get();
        expect(n && *n && value_as<std::int64_t>(*(**n).at("n")) == 3, "rollback undid insert");
    }

    // ---- nested transaction via savepoint ----
    {
        auto outer = db.transaction();
        auto result = outer.run(TransactionMode::default_mode, [&]() -> std::expected<void, SqlError> {
            (void)db.run("INSERT INTO t (name, flag) VALUES ('erin', 1)");
            auto inner = db.transaction();
            auto inner_res = inner.run(TransactionMode::default_mode,
                                       [&]() -> std::expected<void, SqlError> {
                (void)db.run("INSERT INTO t (name, flag) VALUES ('frank', 1)");
                return std::unexpected(SqlError{"inner fail", "SQLITE_ERROR"});
            });
            expect(!inner_res.has_value(), "inner savepoint rolled back");
            return {};  // outer commits
        });
        expect(result.has_value(), "outer commit with failed inner ok");
        auto n = db.prepare("SELECT COUNT(*) AS n FROM t").get();
        // erin committed (4), frank rolled back to savepoint.
        expect(n && *n && value_as<std::int64_t>(*(**n).at("n")) == 4,
               "savepoint kept erin, dropped frank");
    }

    // ---- error surfaces from bad SQL ----
    {
        auto r = db.run("SELECT * FROM no_such_table");
        expect(!r.has_value(), "query on missing table errors");
        if (!r) expect(!r.error().message.empty(), "error carries a message");
    }

    // ---- close is idempotent ----
    db.close();
    expect(db.is_closed(), "db closed");
    db.close();
    expect(db.is_closed(), "double close is safe");

    if (failed == 0) std::cout << "test_sqlite_native: all checks passed\n";
    return failed == 0 ? 0 : 1;
}
