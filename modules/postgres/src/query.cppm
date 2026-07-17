// Query and statement lifecycle shapes. Socket/backend execution is deferred.
// Reference: bun-ref sql_jsc/postgres/PostgresSQLQuery.rs and Statement.rs;
// shared SQLQueryResultMode.rs.
export module mbun.postgres.query;

import std;
import mbun.postgres.parameters;
import mbun.postgres.row;
import mbun.postgres.types;

namespace mbun::postgres {

export enum class QueryState : std::uint8_t { Pending, Running, Done, Failed, Cancelled };

export struct QueryFlags {
    bool done { false };
    bool binary { false };
    bool bigint { false };
    bool simple { false };
    bool pipelined { false };
    ResultMode result_mode { ResultMode::Objects };
};

export struct StatementInfo {
    std::string sql;
    std::vector<FieldDescription> fields;
    std::vector<std::uint32_t> parameter_types;
};

export struct QueryResult {
    std::vector<Row> rows;
    std::string command;
    std::int64_t count { 0 };
};

export class Query {
private:
    std::string sql_;
    std::vector<Parameter> parameters_;
    QueryFlags flags_ {};
    QueryState state_ { QueryState::Pending };

public:
    explicit Query(std::string sql, std::vector<Parameter> parameters = {}, QueryFlags flags = {})
        : sql_ { std::move(sql) }, parameters_ { std::move(parameters) }, flags_ { flags } {}
    [[nodiscard]] std::string_view sql() const { return sql_; }
    [[nodiscard]] const auto& parameters() const { return parameters_; }
    [[nodiscard]] QueryFlags flags() const { return flags_; }
    [[nodiscard]] QueryState state() const { return state_; }
    void cancel() { state_ = QueryState::Cancelled; }
    void mark_running() { if (state_ == QueryState::Pending) state_ = QueryState::Running; }
    void mark_done() { state_ = QueryState::Done; flags_.done = true; }
    void mark_failed() { state_ = QueryState::Failed; }

    // DEFERRED(S-net): replace with the socket/backend request queue.
    [[nodiscard]] std::expected<QueryResult, std::string> execute() const {
        return std::unexpected("PostgreSQL socket backend deferred");
    }
};

export inline std::string command_name(std::string_view tag) {
    constexpr std::array names { "", "INSERT", "DELETE", "UPDATE", "MERGE", "SELECT", "MOVE", "FETCH", "COPY" };
    if (tag.empty()) return {};
    if (tag.size() == 1 && tag.front() >= '0' && tag.front() <= '8') return names[static_cast<std::size_t>(tag.front() - '0')];
    return std::string { tag };
}

} // namespace mbun::postgres
