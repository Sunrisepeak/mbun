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
