// native.cppm — real sqlite3 backend that fills the mbun.sqlite seam.
//
// PORT-SOURCE: bun's `bun:sqlite` native binding
// `src/bun.js/bindings/sqlite/JSSQLStatement.cpp` (open flags, prepare/step,
// bind_one, column extraction, changes/lastInsertRowid, transaction verbs).
// The pure-logic modules (database/statement/transaction/row/value/sql) stay
// engine-agnostic; this module includes sqlite3.h in a global module fragment
// and wires the injected ExecuteFn / FinalizeFn / TransactionBackend / close to
// a live sqlite3 handle. Statements are cached per SQL text (as bun caches
// prepared statements) so repeated queries reuse the compiled program.
module;

#include <sqlite3.h>

export module mbun.sqlite.native;

import std;
import mbun.sqlite.sql;
import mbun.sqlite.value;
import mbun.sqlite.row;
import mbun.sqlite.statement;
import mbun.sqlite.transaction;
import mbun.sqlite.database;

namespace mbun::sqlite {

// Map a primary sqlite3 result code to bun's SQLITE_* token. Extended codes are
// masked to the primary code; the human message comes from sqlite3_errmsg.
inline std::string result_code_name(int rc) {
    switch (rc & 0xff) {
    case SQLITE_OK:         return "SQLITE_OK";
    case SQLITE_ERROR:      return "SQLITE_ERROR";
    case SQLITE_INTERNAL:   return "SQLITE_INTERNAL";
    case SQLITE_PERM:       return "SQLITE_PERM";
    case SQLITE_ABORT:      return "SQLITE_ABORT";
    case SQLITE_BUSY:       return "SQLITE_BUSY";
    case SQLITE_LOCKED:     return "SQLITE_LOCKED";
    case SQLITE_NOMEM:      return "SQLITE_NOMEM";
    case SQLITE_READONLY:   return "SQLITE_READONLY";
    case SQLITE_INTERRUPT:  return "SQLITE_INTERRUPT";
    case SQLITE_IOERR:      return "SQLITE_IOERR";
    case SQLITE_CORRUPT:    return "SQLITE_CORRUPT";
    case SQLITE_NOTFOUND:   return "SQLITE_NOTFOUND";
    case SQLITE_FULL:       return "SQLITE_FULL";
    case SQLITE_CANTOPEN:   return "SQLITE_CANTOPEN";
    case SQLITE_PROTOCOL:   return "SQLITE_PROTOCOL";
    case SQLITE_SCHEMA:     return "SQLITE_SCHEMA";
    case SQLITE_TOOBIG:     return "SQLITE_TOOBIG";
    case SQLITE_CONSTRAINT: return "SQLITE_CONSTRAINT";
    case SQLITE_MISMATCH:   return "SQLITE_MISMATCH";
    case SQLITE_MISUSE:     return "SQLITE_MISUSE";
    case SQLITE_AUTH:       return "SQLITE_AUTH";
    case SQLITE_RANGE:      return "SQLITE_RANGE";
    case SQLITE_NOTADB:     return "SQLITE_NOTADB";
    default:                return "SQLITE_ERROR";
    }
}

inline SqlError make_error(sqlite3* db, int rc) {
    const char* msg = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    return SqlError{msg ? std::string{msg} : std::string{"sqlite3 error"}, result_code_name(rc)};
}

// Owns the sqlite3 handle plus a compiled-statement cache. Held via shared_ptr
// by the DatabaseBackend closures; the handle closes when the last owner drops
// or when close() is called explicitly (idempotent).
class NativeConnection {
public:
    explicit NativeConnection(sqlite3* db) noexcept : db_{db} {}
    NativeConnection(const NativeConnection&) = delete;
    NativeConnection& operator=(const NativeConnection&) = delete;
    ~NativeConnection() { close(); }

    void close() noexcept {
        if (!db_) return;
        for (auto& [sql, stmt] : cache_) sqlite3_finalize(stmt);
        cache_.clear();
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }

    bool in_transaction() const noexcept {
        return db_ && sqlite3_get_autocommit(db_) == 0;
    }

    std::expected<ExecutionResult, SqlError> execute(std::string_view sql,
                                                     std::span<const SqlValue> params) {
        if (!db_) return std::unexpected(SqlError{"database is closed", "SQLITE_MISUSE"});

        // Fast path: a previously compiled single statement.
        if (auto it = cache_.find(std::string{sql}); it != cache_.end()) {
            sqlite3_stmt* stmt = it->second;
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            ExecutionResult result;
            if (auto ok = run_one(stmt, params, result); !ok) {
                sqlite3_reset(stmt);
                return std::unexpected(ok.error());
            }
            fill_counters(result);
            sqlite3_reset(stmt);
            return result;
        }

        // Compile the first statement; detect whether more follow.
        const char* begin = sql.data();
        const char* tail = nullptr;
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, begin, static_cast<int>(sql.size()), &stmt, &tail);
        if (rc != SQLITE_OK) {
            if (stmt) sqlite3_finalize(stmt);
            return std::unexpected(make_error(db_, rc));
        }
        if (!stmt) {
            // Whitespace / comment only.
            ExecutionResult empty;
            fill_counters(empty);
            return empty;
        }

        const bool single = tail_is_blank(tail, begin + sql.size());
        if (single) {
            // Cache and run.
            ExecutionResult result;
            auto ok = run_one(stmt, params, result);
            if (!ok) {
                sqlite3_finalize(stmt);
                return std::unexpected(ok.error());
            }
            fill_counters(result);
            sqlite3_reset(stmt);
            cache_.emplace(std::string{sql}, stmt);
            return result;
        }

        // Multi-statement (DDL batch etc.): run each without caching. Rows from
        // the last row-producing statement win.
        ExecutionResult result;
        bool first = true;
        while (stmt) {
            if (first) {
                if (auto ok = run_one(stmt, params, result); !ok) {
                    sqlite3_finalize(stmt);
                    return std::unexpected(ok.error());
                }
                first = false;
            } else {
                if (auto ok = run_one(stmt, {}, result); !ok) {
                    sqlite3_finalize(stmt);
                    return std::unexpected(ok.error());
                }
            }
            sqlite3_finalize(stmt);
            stmt = nullptr;
            if (tail_is_blank(tail, begin + sql.size())) break;
            const char* next_tail = nullptr;
            const int remaining = static_cast<int>((begin + sql.size()) - tail);
            rc = sqlite3_prepare_v2(db_, tail, remaining, &stmt, &next_tail);
            if (rc != SQLITE_OK) {
                if (stmt) sqlite3_finalize(stmt);
                return std::unexpected(make_error(db_, rc));
            }
            tail = next_tail;
        }
        fill_counters(result);
        return result;
    }

    // Statement.toString(): the SQL with its bound parameters rendered in, via
    // sqlite3_expanded_sql. Deliberately never steps -- bun's toString() has no
    // side effects, and an unbound parameter renders as NULL rather than erroring.
    std::expected<std::string, SqlError> expanded_sql(std::string_view sql,
                                                      std::span<const SqlValue> params) {
        if (!db_) return std::unexpected(SqlError{"database is closed", "SQLITE_MISUSE"});
        sqlite3_stmt* stmt = nullptr;
        bool cached = false;
        if (auto it = cache_.find(std::string{sql}); it != cache_.end()) {
            stmt = it->second;
            cached = true;
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
        } else {
            const char* tail = nullptr;
            int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt, &tail);
            if (rc != SQLITE_OK || !stmt) {
                if (stmt) sqlite3_finalize(stmt);
                return std::unexpected(make_error(db_, rc));
            }
        }
        if (auto ok = bind_all(stmt, params); !ok) {
            sqlite3_reset(stmt);
            if (!cached) sqlite3_finalize(stmt);
            return std::unexpected(ok.error());
        }
        char* p = sqlite3_expanded_sql(stmt);
        std::string out{p ? p : ""};
        if (p) sqlite3_free(p);
        sqlite3_reset(stmt);
        if (!cached) sqlite3_finalize(stmt);
        return out;
    }

    std::expected<void, SqlError> exec(std::string_view sql_verb) {
        if (!db_) return std::unexpected(SqlError{"database is closed", "SQLITE_MISUSE"});
        char* errmsg = nullptr;
        std::string sql{sql_verb};
        int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            SqlError err{errmsg ? std::string{errmsg} : std::string{"sqlite3 error"},
                         result_code_name(rc)};
            if (errmsg) sqlite3_free(errmsg);
            return std::unexpected(err);
        }
        return {};
    }

private:
    static bool tail_is_blank(const char* tail, const char* end) {
        if (!tail) return true;
        for (const char* p = tail; p < end; ++p) {
            if (!std::isspace(static_cast<unsigned char>(*p)) && *p != ';') return false;
        }
        return true;
    }

    void fill_counters(ExecutionResult& result) {
        result.changes = sqlite3_changes64(db_);
        result.last_insert_rowid = sqlite3_last_insert_rowid(db_);
    }

    std::expected<void, SqlError> bind_all(sqlite3_stmt* stmt,
                                           std::span<const SqlValue> params) {
        const int expected = sqlite3_bind_parameter_count(stmt);
        const int count = std::min<int>(expected, static_cast<int>(params.size()));
        for (int i = 0; i < count; ++i) {
            const SqlValue& v = params[static_cast<std::size_t>(i)];
            const int idx = i + 1;  // sqlite3 params are 1-based
            int rc = SQLITE_OK;
            switch (v.kind()) {
            case SQLValueKind::Null:
                rc = sqlite3_bind_null(stmt, idx);
                break;
            case SQLValueKind::Integer:
                rc = sqlite3_bind_int64(stmt, idx, std::get<std::int64_t>(v.value()));
                break;
            case SQLValueKind::Real:
                rc = sqlite3_bind_double(stmt, idx, std::get<double>(v.value()));
                break;
            case SQLValueKind::Boolean:
                rc = sqlite3_bind_int(stmt, idx, std::get<bool>(v.value()) ? 1 : 0);
                break;
            case SQLValueKind::Text: {
                const auto& s = std::get<std::string>(v.value());
                rc = sqlite3_bind_text(stmt, idx, s.data(), static_cast<int>(s.size()),
                                       SQLITE_TRANSIENT);
                break;
            }
            case SQLValueKind::Blob: {
                const auto& b = std::get<std::vector<std::byte>>(v.value());
                rc = sqlite3_bind_blob(stmt, idx, b.data(), static_cast<int>(b.size()),
                                       SQLITE_TRANSIENT);
                break;
            }
            }
            if (rc != SQLITE_OK) return std::unexpected(make_error(db_, rc));
        }
        return {};
    }

    static SqlValue column_value(sqlite3_stmt* stmt, int i) {
        switch (sqlite3_column_type(stmt, i)) {
        case SQLITE_INTEGER:
            return SqlValue{static_cast<std::int64_t>(sqlite3_column_int64(stmt, i))};
        case SQLITE_FLOAT:
            return SqlValue{sqlite3_column_double(stmt, i)};
        case SQLITE_TEXT: {
            const auto* p = sqlite3_column_text(stmt, i);
            const int n = sqlite3_column_bytes(stmt, i);
            return SqlValue{std::string{reinterpret_cast<const char*>(p),
                                        static_cast<std::size_t>(n)}};
        }
        case SQLITE_BLOB: {
            const auto* p = static_cast<const std::byte*>(sqlite3_column_blob(stmt, i));
            const int n = sqlite3_column_bytes(stmt, i);
            return SqlValue{std::vector<std::byte>{p, p + n}};
        }
        case SQLITE_NULL:
        default:
            return SqlValue{nullptr};
        }
    }

    std::expected<void, SqlError> run_one(sqlite3_stmt* stmt,
                                          std::span<const SqlValue> params,
                                          ExecutionResult& out) {
        if (auto ok = bind_all(stmt, params); !ok) return ok;
        const int ncol = sqlite3_column_count(stmt);
        std::vector<std::string> columns;
        std::vector<std::string> decltypes;
        columns.reserve(static_cast<std::size_t>(ncol));
        decltypes.reserve(static_cast<std::size_t>(ncol));
        for (int i = 0; i < ncol; ++i) {
            const char* name = sqlite3_column_name(stmt, i);
            columns.emplace_back(name ? name : "");
            const char* dt = sqlite3_column_decltype(stmt, i);
            decltypes.emplace_back(dt ? dt : "");
        }
        std::vector<Row> rows;
        for (;;) {
            int rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW) {
                std::vector<SqlValue> values;
                values.reserve(static_cast<std::size_t>(ncol));
                for (int i = 0; i < ncol; ++i) values.push_back(column_value(stmt, i));
                rows.emplace_back(columns, std::move(values));
            } else if (rc == SQLITE_DONE) {
                break;
            } else {
                return std::unexpected(make_error(db_, rc));
            }
        }
        if (ncol > 0) {
            out.rows = std::move(rows);
            out.columns = std::move(columns);
            out.declared_types = std::move(decltypes);
        }
        return {};
    }

    sqlite3* db_{nullptr};
    std::unordered_map<std::string, sqlite3_stmt*> cache_;
};

// Build a Database backed by a live sqlite3 connection. `filename` follows bun:
// ":memory:" for a private in-memory db, a path otherwise. Options map to
// sqlite3_open_v2 flags (readonly/create).
export std::expected<Database, SqlError> open(std::string filename,
                                              DatabaseOptions options = {}) {
    int flags = options.readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
    if (!options.readonly && options.create) flags |= SQLITE_OPEN_CREATE;

    sqlite3* raw = nullptr;
    int rc = sqlite3_open_v2(filename.c_str(), &raw, flags, nullptr);
    if (rc != SQLITE_OK) {
        SqlError err = make_error(raw, rc);
        if (raw) sqlite3_close_v2(raw);
        return std::unexpected(err);
    }
    // Recommended defaults (bun enables extended result codes).
    sqlite3_extended_result_codes(raw, 1);
    // A zero SQLITE_LIMIT_ATTACHED makes every ATTACH fail ("too many attached
    // databases") without touching any other statement, which is the narrowest way
    // to keep a permission-gated connection from opening a second file.
    if (options.no_attach) { sqlite3_limit(raw, SQLITE_LIMIT_ATTACHED, 0); }

    auto conn = std::make_shared<NativeConnection>(raw);

    DatabaseBackend backend;
    backend.execute = [conn](std::string_view sql, std::span<const SqlValue> params) {
        return conn->execute(sql, params);
    };
    backend.expanded_sql = [conn](std::string_view sql, std::span<const SqlValue> params) {
        return conn->expanded_sql(sql, params);
    };
    backend.finalize = [](std::string_view) {
        // The connection owns cached statements; nothing to release per-Statement.
    };
    backend.in_transaction = [conn]() { return conn->in_transaction(); };
    backend.close = [conn]() { conn->close(); };

    TransactionBackend tx;
    tx.begin = [conn](TransactionMode mode) -> std::expected<void, SqlError> {
        std::string_view verb = "BEGIN";
        switch (mode) {
        case TransactionMode::deferred:  verb = "BEGIN DEFERRED"; break;
        case TransactionMode::immediate: verb = "BEGIN IMMEDIATE"; break;
        case TransactionMode::exclusive: verb = "BEGIN EXCLUSIVE"; break;
        case TransactionMode::default_mode: default: verb = "BEGIN"; break;
        }
        return conn->exec(verb);
    };
    tx.commit = [conn]() { return conn->exec("COMMIT"); };
    tx.rollback = [conn]() { return conn->exec("ROLLBACK"); };
    tx.savepoint = [conn](std::string_view name) {
        return conn->exec("SAVEPOINT " + std::string{name});
    };
    tx.release = [conn](std::string_view name) {
        return conn->exec("RELEASE SAVEPOINT " + std::string{name});
    };
    tx.rollback_to = [conn](std::string_view name) {
        return conn->exec("ROLLBACK TO SAVEPOINT " + std::string{name});
    };
    tx.in_transaction = [conn]() { return conn->in_transaction(); };
    backend.transaction = std::move(tx);

    return Database{std::move(filename), std::move(backend)};
}

// SQLite runtime version string (e.g. "3.45.1"), for diagnostics/parity checks.
export std::string libversion() { return std::string{sqlite3_libversion()}; }

} // namespace mbun::sqlite
