// database.cppm — database facade and deferred sqlite3 binding seam.
//
// PORT-SOURCE: bun `src/js/bun/sqlite.ts` Database/query/prepare/run and Zig
// `src/jsc/bindings/sqlite/JSSQLStatement.cpp`. This layer deliberately does
// not include sqlite3.h: a native adapter can be added without changing the API.
export module mbun.sqlite.database;

import std;
import mbun.sqlite.statement;
import mbun.sqlite.transaction;
import mbun.sqlite.value;

namespace mbun::sqlite {

export struct DatabaseBackend {
    ExecuteFn execute;
    FinalizeFn finalize;
    ExpandedSqlFn expanded_sql;
    std::function<bool()> in_transaction;
    TransactionBackend transaction;
    std::function<void()> close;
    // ---- node:sqlite surface -------------------------------------------------
    // These have no SQL/PRAGMA equivalent, so they cannot be expressed through
    // execute(); each is a direct sqlite3 C-API call supplied by the native
    // adapter. Kept as optional hooks so the deferred (no-sqlite3) build still
    // links -- an unset hook reports "unsupported" rather than crashing.
    //
    // sqlite3_db_config(op, value) -> resulting value. DEFENSIVE (1010) and
    // DQS_DML/DQS_DDL (1013/1014) are the ones node exposes.
    std::function<std::expected<int, SqlError>(int, int)> db_config;
    // sqlite3_limit(id, value); a negative value queries without changing.
    std::function<int(int, int)> limit;
    // sqlite3_busy_timeout(ms) for DatabaseSync's `timeout` option.
    std::function<void(int)> busy_timeout;
    // sqlite3_serialize / sqlite3_deserialize of a named schema ("main").
    std::function<std::expected<std::vector<std::byte>, SqlError>(std::string)> serialize;
    std::function<std::expected<void, SqlError>(std::string, std::vector<std::byte>)> deserialize;
    // Prepare-only column introspection for StatementSync.prototype.columns(),
    // which must not execute the statement (an INSERT would run twice).
    std::function<std::expected<std::vector<ColumnInfo>, SqlError>(std::string)> column_info;
    // sqlite3_db_filename(name) for DatabaseSync.prototype.location(); empty for
    // a temporary/in-memory schema, which JS surfaces as null.
    std::function<std::string(std::string)> db_filename;
    // sqlite3_set_authorizer. The callback fires DURING prepare, once per action
    // sqlite is about to compile, and its four arguments are absent (nullopt)
    // rather than empty for actions that do not carry them. An empty std::function
    // clears the authorizer.
    std::function<void(AuthorizerFn)> set_authorizer;
    // sqlite3_create_function_v2. `nargs` is -1 for varargs; `deterministic` and
    // `direct_only` become SQLITE_DETERMINISTIC / SQLITE_DIRECTONLY, which sqlite
    // only honours at registration time.
    std::function<std::expected<void, SqlError>(std::string, int, bool, bool, ScalarFn)>
        create_function;
};

export struct DatabaseOptions {
    bool readonly{false};
    bool create{true};
    bool strict{false};
    bool safe_integers{false};
    // Refuse `ATTACH DATABASE '<path>'`, which is a second file-open that bypasses
    // whatever check the caller made on `filename`. Set by the bun:sqlite binding
    // when node's permission model is active without a wholesale fs grant: the
    // binding can gate the path it is handed, but not a path that arrives later
    // inside a SQL string.
    bool no_attach{false};
};

export class Database {
public:
    Database() = default;
    explicit Database(std::string filename, DatabaseBackend backend = {})
        : filename_{std::move(filename)}, backend_{std::move(backend)} {}
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    // Explicit moves: the moved-from object must become INERT. The destructor
    // calls close(), and libc++'s small-buffer std::function move leaves the
    // source callable intact (libstdc++ empties it) — with the defaulted move,
    // the temporary open() returns through closed the shared connection the
    // moment it was destroyed (every later op failed SQLITE_MISUSE on clang).
    Database(Database&& other) noexcept
        : filename_{std::move(other.filename_)},
          backend_{std::move(other.backend_)},
          closed_{other.closed_} {
        other.backend_ = {};
        other.closed_ = true;
    }
    Database& operator=(Database&& other) noexcept {
        if (this != &other) {
            close();
            filename_ = std::move(other.filename_);
            backend_ = std::move(other.backend_);
            closed_ = other.closed_;
            other.backend_ = {};
            other.closed_ = true;
        }
        return *this;
    }
    ~Database() { close(); }

    const std::string& filename() const noexcept { return filename_; }
    bool is_closed() const noexcept { return closed_; }
    bool in_transaction() const noexcept {
        return backend_.in_transaction ? backend_.in_transaction() : false;
    }

    Statement prepare(std::string_view sql) const {
        return Statement{std::string{sql}, backend_.execute, backend_.finalize};
    }
    std::expected<ExecutionResult, SqlError> run(std::string_view sql,
                                                  std::span<const SqlValue> params = {}) const {
        return prepare(sql).bind(std::vector<SqlValue>{params.begin(), params.end()}).execute();
    }
    std::expected<std::string, SqlError> expanded_sql(std::string_view sql,
                                                      std::span<const SqlValue> params = {}) const {
        if (!backend_.expanded_sql)
            return std::unexpected(SqlError{"expandedSQL unavailable", "SQLITE_MISUSE"});
        return backend_.expanded_sql(sql, params);
    }
    Transaction transaction() const { return Transaction{backend_.transaction}; }

    static SqlError unsupported() {
        return SqlError{"sqlite3 backend is deferred", "SQLITE_NOTFOUND"};
    }
    std::expected<int, SqlError> db_config(int op, int value) const {
        if (!backend_.db_config) return std::unexpected(unsupported());
        return backend_.db_config(op, value);
    }
    int limit(int id, int value) const { return backend_.limit ? backend_.limit(id, value) : -1; }
    void busy_timeout(int ms) const { if (backend_.busy_timeout) backend_.busy_timeout(ms); }
    std::expected<std::vector<std::byte>, SqlError> serialize(std::string schema) const {
        if (!backend_.serialize) return std::unexpected(unsupported());
        return backend_.serialize(std::move(schema));
    }
    std::expected<void, SqlError> deserialize(std::string schema,
                                              std::vector<std::byte> bytes) const {
        if (!backend_.deserialize) return std::unexpected(unsupported());
        return backend_.deserialize(std::move(schema), std::move(bytes));
    }
    std::expected<std::vector<ColumnInfo>, SqlError> column_info(std::string sql) const {
        if (!backend_.column_info) return std::unexpected(unsupported());
        return backend_.column_info(std::move(sql));
    }
    std::string db_filename(std::string name) const {
        return backend_.db_filename ? backend_.db_filename(std::move(name)) : std::string{};
    }
    void set_authorizer(AuthorizerFn fn) const {
        if (backend_.set_authorizer) backend_.set_authorizer(std::move(fn));
    }
    std::expected<void, SqlError> create_function(std::string name, int nargs, bool deterministic,
                                                  bool direct_only, ScalarFn fn) const {
        if (!backend_.create_function) return std::unexpected(unsupported());
        return backend_.create_function(std::move(name), nargs, deterministic, direct_only,
                                        std::move(fn));
    }

    void close() noexcept {
        if (closed_) return;
        if (backend_.close) backend_.close();
        closed_ = true;
    }

private:
    std::string filename_{":memory:"};
    DatabaseBackend backend_;
    bool closed_{false};
};

} // namespace mbun::sqlite
