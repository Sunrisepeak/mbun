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

// The amalgamation is COMPILED with -DSQLITE_ENABLE_COLUMN_METADATA=1 (see the
// mbun.sqlite3 package), but sqlite3.h hides the three
// sqlite3_column_{database,table,origin}_name declarations behind the same macro
// for CONSUMERS. Without this, the symbols exist in the archive and are simply
// invisible here -- which is how StatementSync.prototype.columns() silently
// reported database/table/column as null for every column.
#ifndef SQLITE_ENABLE_COLUMN_METADATA
#define SQLITE_ENABLE_COLUMN_METADATA 1
#endif
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
    // The extended code is what node reports as `err.errcode`; `rc` from a
    // step/prepare is already extended (sqlite3_extended_result_codes is on),
    // but sqlite3_extended_errcode is the authoritative source when db is live.
    const int ext = db ? sqlite3_extended_errcode(db) : rc;
    const char* estr = sqlite3_errstr(ext);
    return SqlError{msg ? std::string{msg} : std::string{"sqlite3 error"}, result_code_name(rc),
                    ext, estr ? std::string{estr} : std::string{}};
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

    // A user-defined function or authorizer can call db.close() from inside
    // sqlite3_step. Finalizing the cache there would free the very statement the
    // enclosing execute() is still holding, so the close is DEFERRED until the
    // step unwinds; the JS layer has already flipped isOpen, so the delay is not
    // observable.
    void close() noexcept {
        if (!db_) return;
        if (!running_.empty()) { close_pending_ = true; return; }
        do_close();
    }

    void do_close() noexcept {
        if (!db_) return;
        for (auto& [sql, stmt] : cache_) sqlite3_finalize(stmt);
        cache_.clear();
        sqlite3_close_v2(db_);
        db_ = nullptr;
        close_pending_ = false;
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
            // Re-entered from a user-defined function running inside this very
            // statement's sqlite3_step. sqlite3_reset on a VDBE that is mid-step
            // corrupts its state, so refuse instead.
            if (running_.contains(stmt)) {
                return std::unexpected(SqlError{"statement is already running", "SQLITE_MISUSE"});
            }
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            ExecutionResult result;
            running_.insert(stmt);
            auto ok = run_one(stmt, params, result);
            if (ok) fill_counters(result);
            sqlite3_reset(stmt);
            running_.erase(stmt);
            if (close_pending_ && running_.empty()) do_close();
            if (!ok) return std::unexpected(ok.error());
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
            // Cache and run. Tracked as running for the same reason as the fast
            // path: a callback may try to close the connection mid-step.
            ExecutionResult result;
            running_.insert(stmt);
            auto ok = run_one(stmt, params, result);
            if (ok) { fill_counters(result); sqlite3_reset(stmt); }
            running_.erase(stmt);
            if (!ok) {
                sqlite3_finalize(stmt);
                if (close_pending_ && running_.empty()) do_close();
                return std::unexpected(ok.error());
            }
            if (close_pending_ && running_.empty()) {
                sqlite3_finalize(stmt);
                do_close();
                return result;
            }
            cache_.emplace(std::string{sql}, stmt);
            return result;
        }

        // Multi-statement (DDL batch etc.): run each without caching. Rows from
        // the last row-producing statement win.
        ExecutionResult result;
        bool first = true;
        while (stmt) {
            running_.insert(stmt);
            auto ok = first ? run_one(stmt, params, result) : run_one(stmt, {}, result);
            running_.erase(stmt);
            first = false;
            if (!ok) {
                sqlite3_finalize(stmt);
                if (close_pending_ && running_.empty()) do_close();
                return std::unexpected(ok.error());
            }
            sqlite3_finalize(stmt);
            stmt = nullptr;
            if (close_pending_ && running_.empty()) {
                do_close();
                return result;
            }
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

    // ---- node:sqlite direct C-API surface ----------------------------------
    // sqlite3_db_config takes (op, int, int*) for the boolean ops node exposes:
    // passing -1 queries without changing, which is how the resulting value is
    // read back for DatabaseSync's getters.
    std::expected<int, SqlError> db_config(int op, int value) {
        if (!db_) return std::unexpected(SqlError{"database is closed", "SQLITE_MISUSE"});
        int out = 0;
        int rc = sqlite3_db_config(db_, op, value, &out);
        if (rc != SQLITE_OK) return std::unexpected(make_error(db_, rc));
        return out;
    }

    int limit(int id, int value) {
        if (!db_) return -1;
        return sqlite3_limit(db_, id, value);
    }

    void busy_timeout(int ms) {
        if (db_) sqlite3_busy_timeout(db_, ms);
    }

    std::string db_filename(const std::string& name) {
        if (!db_) return {};
        const char* p = sqlite3_db_filename(db_, name.c_str());
        return p ? std::string{p} : std::string{};
    }

    std::expected<std::vector<std::byte>, SqlError> serialize(const std::string& schema) {
        if (!db_) return std::unexpected(SqlError{"database is closed", "SQLITE_MISUSE"});
#ifdef SQLITE_OMIT_DESERIALIZE
        return std::unexpected(SqlError{"serialize is not available", "SQLITE_NOTFOUND"});
#else
        sqlite3_int64 size = 0;
        unsigned char* raw = sqlite3_serialize(db_, schema.c_str(), &size, 0);
        if (!raw) {
            // A never-written :memory: database has no page 1 yet, and
            // sqlite3_serialize answers NULL/0 for it -- node still returns a
            // valid header-bearing image. Force page 1 into existence with a
            // schema write that leaves no trace, then retry once.
            sqlite3_exec(db_, "CREATE TABLE IF NOT EXISTS __mbun_serialize_seed(x);"
                              "DROP TABLE IF EXISTS __mbun_serialize_seed;",
                         nullptr, nullptr, nullptr);
            raw = sqlite3_serialize(db_, schema.c_str(), &size, 0);
        }
        if (!raw) return std::unexpected(SqlError{"unable to serialize database", "SQLITE_ERROR"});
        const auto* p = reinterpret_cast<const std::byte*>(raw);
        std::vector<std::byte> out{p, p + static_cast<std::size_t>(size)};
        sqlite3_free(raw);
        return out;
#endif
    }

    std::expected<void, SqlError> deserialize(const std::string& schema,
                                              std::span<const std::byte> bytes) {
        if (!db_) return std::unexpected(SqlError{"database is closed", "SQLITE_MISUSE"});
#ifdef SQLITE_OMIT_DESERIALIZE
        return std::unexpected(SqlError{"deserialize is not available", "SQLITE_NOTFOUND"});
#else
        // The buffer must outlive the connection and be sqlite3_malloc'd, since
        // FREEONCLOSE hands ownership to sqlite; RESIZEABLE lets the db grow.
        const auto n = static_cast<sqlite3_int64>(bytes.size());
        auto* buf = static_cast<unsigned char*>(sqlite3_malloc64(static_cast<sqlite3_uint64>(n ? n : 1)));
        if (!buf) return std::unexpected(SqlError{"out of memory", "SQLITE_NOMEM"});
        if (n > 0) std::memcpy(buf, bytes.data(), static_cast<std::size_t>(n));
        // Cached statements were compiled against the schema being replaced.
        drop_cache();
        int rc = sqlite3_deserialize(db_, schema.c_str(), buf, n, n,
                                     SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE);
        if (rc != SQLITE_OK) return std::unexpected(make_error(db_, rc));
        return {};
#endif
    }

    // sqlite3_set_authorizer. Installing or clearing one must also drop every
    // cached compiled statement: authorization happens at PREPARE time, so a
    // statement compiled under the previous policy would otherwise keep running
    // unauthorized (and would never call the new callback).
    void set_authorizer(AuthorizerFn fn) {
        if (!db_) return;
        drop_cache();
        authorizer_ = std::move(fn);
        if (!authorizer_) {
            sqlite3_set_authorizer(db_, nullptr, nullptr);
            return;
        }
        sqlite3_set_authorizer(db_, &NativeConnection::auth_trampoline, this);
    }

    // sqlite3_create_function_v2. The holder is owned by sqlite and released
    // through xDestroy, so re-registering the same name/arity frees the previous
    // callback. Cached statements are dropped because a compiled program has the
    // OLD function pointer baked into its bytecode.
    std::expected<void, SqlError> create_function(const std::string& name, int nargs,
                                                  bool deterministic, bool direct_only,
                                                  ScalarFn fn) {
        if (!db_) return std::unexpected(SqlError{"database is closed", "SQLITE_MISUSE"});
        drop_cache();
        int flags = SQLITE_UTF8;
        if (deterministic) flags |= SQLITE_DETERMINISTIC;
        if (direct_only) flags |= SQLITE_DIRECTONLY;
        auto* holder = new ScalarFn{std::move(fn)};
        int rc = sqlite3_create_function_v2(db_, name.c_str(), nargs, flags, holder,
                                            &NativeConnection::scalar_trampoline, nullptr, nullptr,
                                            &NativeConnection::scalar_destroy);
        if (rc != SQLITE_OK) {
            // v2 runs xDestroy itself on failure, so the holder is already gone.
            return std::unexpected(make_error(db_, rc));
        }
        return {};
    }

    // Compile the statement and read column metadata WITHOUT stepping it.
    std::expected<std::vector<ColumnInfo>, SqlError> column_info(const std::string& sql) {
        if (!db_) return std::unexpected(SqlError{"database is closed", "SQLITE_MISUSE"});
        sqlite3_stmt* stmt = nullptr;
        bool cached = false;
        if (auto it = cache_.find(sql); it != cache_.end()) {
            stmt = it->second;
            cached = true;
        } else {
            const char* tail = nullptr;
            int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt, &tail);
            if (rc != SQLITE_OK) {
                if (stmt) sqlite3_finalize(stmt);
                return std::unexpected(make_error(db_, rc));
            }
        }
        std::vector<ColumnInfo> out;
        if (stmt) {
            const int ncol = sqlite3_column_count(stmt);
            out.reserve(static_cast<std::size_t>(ncol));
            for (int i = 0; i < ncol; ++i) {
                ColumnInfo info;
                if (const char* p = sqlite3_column_name(stmt, i)) info.name = p;
                if (const char* p = sqlite3_column_decltype(stmt, i)) info.declared_type = p;
#ifdef SQLITE_ENABLE_COLUMN_METADATA
                if (const char* p = sqlite3_column_database_name(stmt, i)) info.database = p;
                if (const char* p = sqlite3_column_table_name(stmt, i)) info.table = p;
                if (const char* p = sqlite3_column_origin_name(stmt, i)) info.origin = p;
#endif
                out.push_back(std::move(info));
            }
            if (!cached) sqlite3_finalize(stmt);
        }
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
    static SqlValue value_of(sqlite3_value* v) {
        switch (sqlite3_value_type(v)) {
        case SQLITE_INTEGER:
            return SqlValue{static_cast<std::int64_t>(sqlite3_value_int64(v))};
        case SQLITE_FLOAT:
            return SqlValue{sqlite3_value_double(v)};
        case SQLITE_TEXT: {
            const auto* p = sqlite3_value_text(v);
            const int n = sqlite3_value_bytes(v);
            return SqlValue{std::string{reinterpret_cast<const char*>(p),
                                        static_cast<std::size_t>(n)}};
        }
        case SQLITE_BLOB: {
            const auto* p = static_cast<const std::byte*>(sqlite3_value_blob(v));
            const int n = sqlite3_value_bytes(v);
            if (p == nullptr) return SqlValue{std::vector<std::byte>{}};
            return SqlValue{std::vector<std::byte>{p, p + n}};
        }
        default:
            return SqlValue{nullptr};
        }
    }

    static void result_of(sqlite3_context* c, const SqlValue& v) {
        switch (v.kind()) {
        case SQLValueKind::Null:    sqlite3_result_null(c); return;
        case SQLValueKind::Integer: sqlite3_result_int64(c, std::get<std::int64_t>(v.value())); return;
        case SQLValueKind::Real:    sqlite3_result_double(c, std::get<double>(v.value())); return;
        case SQLValueKind::Boolean: sqlite3_result_int(c, std::get<bool>(v.value()) ? 1 : 0); return;
        case SQLValueKind::Text: {
            const auto& s = std::get<std::string>(v.value());
            sqlite3_result_text(c, s.data(), static_cast<int>(s.size()), SQLITE_TRANSIENT);
            return;
        }
        case SQLValueKind::Blob: {
            const auto& b = std::get<std::vector<std::byte>>(v.value());
            static constexpr std::byte kEmpty{};
            const void* data = b.empty() ? static_cast<const void*>(&kEmpty) : b.data();
            sqlite3_result_blob(c, data, static_cast<int>(b.size()), SQLITE_TRANSIENT);
            return;
        }
        }
        sqlite3_result_null(c);
    }

    static void scalar_trampoline(sqlite3_context* c, int argc, sqlite3_value** argv) {
        auto* fn = static_cast<ScalarFn*>(sqlite3_user_data(c));
        if (fn == nullptr || !*fn) { sqlite3_result_null(c); return; }
        std::vector<SqlValue> args;
        args.reserve(static_cast<std::size_t>(argc));
        for (int i = 0; i < argc; ++i) args.push_back(value_of(argv[i]));
        auto r = (*fn)(args);
        if (!r) {
            sqlite3_result_error(c, r.error().c_str(), -1);
            return;
        }
        result_of(c, *r);
    }
    static void scalar_destroy(void* p) { delete static_cast<ScalarFn*>(p); }

    static int auth_trampoline(void* p, int action, const char* a1, const char* a2,
                               const char* a3, const char* a4) {
        auto* self = static_cast<NativeConnection*>(p);
        if (!self || !self->authorizer_) return SQLITE_OK;
        auto opt = [](const char* s) {
            return s ? std::optional<std::string>{s} : std::optional<std::string>{};
        };
        return self->authorizer_(action, opt(a1), opt(a2), opt(a3), opt(a4));
    }

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
                // A null pointer binds SQL NULL even with a zero length, so an
                // EMPTY blob (`new Uint8Array()`) must still pass a real address
                // -- node round-trips it back as a zero-length Uint8Array.
                static constexpr std::byte kEmpty{};
                const void* data = b.empty() ? static_cast<const void*>(&kEmpty) : b.data();
                rc = sqlite3_bind_blob(stmt, idx, data, static_cast<int>(b.size()),
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

    // Finalize and forget every cached statement EXCEPT any that is currently
    // stepping -- freeing one of those would leave the enclosing execute() with a
    // dangling sqlite3_stmt. A still-running one is simply dropped from the cache
    // and released by sqlite3_close_v2.
    void drop_cache() noexcept {
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (running_.contains(it->second)) { ++it; continue; }
            sqlite3_finalize(it->second);
            it = cache_.erase(it);
        }
    }

    sqlite3* db_{nullptr};
    std::unordered_map<std::string, sqlite3_stmt*> cache_;
    AuthorizerFn authorizer_;
    std::unordered_set<sqlite3_stmt*> running_;
    bool close_pending_{false};
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
    backend.db_config = [conn](int op, int value) { return conn->db_config(op, value); };
    backend.limit = [conn](int id, int value) { return conn->limit(id, value); };
    backend.busy_timeout = [conn](int ms) { conn->busy_timeout(ms); };
    backend.db_filename = [conn](std::string name) { return conn->db_filename(name); };
    backend.serialize = [conn](std::string schema) { return conn->serialize(schema); };
    backend.deserialize = [conn](std::string schema, std::vector<std::byte> bytes) {
        return conn->deserialize(schema, bytes);
    };
    backend.column_info = [conn](std::string sql) { return conn->column_info(sql); };
    backend.set_authorizer = [conn](AuthorizerFn fn) { conn->set_authorizer(std::move(fn)); };
    backend.create_function = [conn](std::string name, int nargs, bool det, bool direct,
                                     ScalarFn fn) {
        return conn->create_function(name, nargs, det, direct, std::move(fn));
    };

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
