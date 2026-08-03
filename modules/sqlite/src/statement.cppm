// statement.cppm — prepared SQL and result-shape API.
//
// PORT-SOURCE: bun `src/js/bun/sqlite.ts` Statement and
// `src/sql/shared/StatementStatus.rs`. Native sqlite3_prepare/step/finalize
// are supplied later through ExecuteFn/FinalizeFn (DEFERRED(sqlite3)).
export module mbun.sqlite.statement;

import std;
import mbun.sqlite.row;
import mbun.sqlite.value;

namespace mbun::sqlite {

export struct SqlError {
    std::string message;
    std::string code{"SQLITE_ERROR"};
    // sqlite3_extended_errcode(). node:sqlite surfaces this verbatim as
    // `err.errcode` (e.g. 1299 SQLITE_CONSTRAINT_NOTNULL, 1555
    // SQLITE_CONSTRAINT_PRIMARYKEY) alongside `err.errstr` = sqlite3_errstr of
    // it, which resolves to the PRIMARY code's text ("constraint failed").
    // `code` above stays the masked primary token for bun:sqlite parity.
    int errcode{0};
    std::string errstr;
};

// sqlite3_set_authorizer's callback, in engine-neutral terms: the action code
// plus its four optional string arguments, answering SQLITE_OK/DENY/IGNORE.
export using AuthorizerFn = std::function<int(int, std::optional<std::string>,
                                              std::optional<std::string>,
                                              std::optional<std::string>,
                                              std::optional<std::string>)>;

// A user-defined scalar SQL function, in engine-neutral terms: it receives the
// evaluated arguments and answers a value, or an error string that becomes
// sqlite3_result_error (which aborts the running statement and rolls its changes
// back). DatabaseSync.prototype.function() is built on this.
export using ScalarFn =
    std::function<std::expected<SqlValue, std::string>(std::span<const SqlValue>)>;

// One column of a prepared statement, read without stepping. `database`/`table`/
// `origin` come from sqlite3_column_{database,table,origin}_name and are empty
// for a computed column (expression, literal, function call); the amalgamation is
// built with SQLITE_ENABLE_COLUMN_METADATA=1 so they are available.
export struct ColumnInfo {
    std::string name;
    std::string declared_type;
    std::string database;
    std::string table;
    std::string origin;
};

export struct ExecutionResult {
    // Column names of the compiled statement, read via sqlite3_column_name once
    // the statement is prepared. bun's initializeColumnNames does the same and
    // never steps first (JSSQLStatement.cpp), so a query returning zero rows
    // still reports its columns -- they cannot be recovered from `rows` alone.
    std::vector<std::string> columns;
    // Declared column types from sqlite3_column_decltype, captured at prepare
    // time like bun's declaredTypes getter (JSSQLStatement.cpp:2741-2781).
    // An empty string means the column has no declared type (expression/func),
    // which the JS layer surfaces as null.
    std::vector<std::string> declared_types;
    std::vector<Row> rows;
    std::int64_t changes{0};
    std::int64_t last_insert_rowid{0};
};

export enum class StatementStatus : std::uint8_t { pending, prepared, stepped, finalized, failed };

export using ExecuteFn = std::function<std::expected<ExecutionResult, SqlError>(
    std::string_view, std::span<const SqlValue>)>;
export using FinalizeFn = std::function<void(std::string_view)>;
// Renders a statement's SQL with its bound parameters substituted in, the way
// bun's Statement.toString() does (sqlite3_expanded_sql). Never steps.
export using ExpandedSqlFn = std::function<std::expected<std::string, SqlError>(
    std::string_view, std::span<const SqlValue>)>;

export class Statement {
public:
    Statement() = default;
    Statement(std::string sql, ExecuteFn execute, FinalizeFn finalize = {})
        : sql_{std::move(sql)}, execute_{std::move(execute)}, finalize_{std::move(finalize)},
          status_{StatementStatus::prepared} {}
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&&) noexcept = default;
    Statement& operator=(Statement&&) noexcept = default;
    ~Statement() { finalize(); }

    const std::string& sql() const noexcept { return sql_; }
    StatementStatus status() const noexcept { return status_; }
    bool is_finalized() const noexcept { return status_ == StatementStatus::finalized; }
    Statement& bind(std::vector<SqlValue> values) {
        params_ = std::move(values);
        return *this;
    }
    Statement& bind(std::initializer_list<SqlValue> values) {
        params_ = values;
        return *this;
    }
    void reset() noexcept { params_.clear(); status_ = StatementStatus::prepared; }
    void finalize() noexcept {
        if (status_ == StatementStatus::finalized) return;
        if (finalize_) finalize_(sql_);
        status_ = StatementStatus::finalized;
    }

    std::expected<ExecutionResult, SqlError> execute() {
        if (is_finalized()) return std::unexpected(SqlError{"statement is finalized", "SQLITE_MISUSE"});
        if (!execute_) return std::unexpected(SqlError{"sqlite3 backend is deferred", "SQLITE_NOTFOUND"});
        auto result = execute_(sql_, params_);
        status_ = result ? StatementStatus::stepped : StatementStatus::failed;
        return result;
    }
    std::expected<std::optional<Row>, SqlError> get() {
        auto result = execute();
        if (!result) return std::unexpected(result.error());
        if (result->rows.empty()) return std::optional<Row>{};
        return std::optional<Row>{std::move(result->rows.front())};
    }
    std::expected<std::vector<Row>, SqlError> all() {
        auto result = execute();
        if (!result) return std::unexpected(result.error());
        return std::move(result->rows);
    }

private:
    std::string sql_;
    std::vector<SqlValue> params_;
    ExecuteFn execute_;
    FinalizeFn finalize_;
    StatementStatus status_{StatementStatus::pending};
};

} // namespace mbun::sqlite
