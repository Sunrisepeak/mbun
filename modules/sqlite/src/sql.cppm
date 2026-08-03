// Pure SQL metadata and command parsing.
// Reference: bun-ref src/js/internal/sql/sqlite.ts and src/sql/shared.
// sqlite3 execution and JSC conversion are intentionally DEFERRED.
export module mbun.sqlite.sql;

import std;

namespace mbun::sqlite {

export enum class SQLCommand : std::int8_t { None = -1, Insert, Update, UpdateSet, Where, In };

export struct ParsedSQL {
    SQLCommand command { SQLCommand::None };
    std::string lastToken {};
    bool canReturnRows { false };
};

export struct Changes {
    std::int64_t count { 0 };
    std::int64_t lastInsertRowid { 0 };
};

export enum class SQLValueKind : std::uint8_t { Null, Integer, Real, Text, Blob, Boolean };

export class SQLValue {
private:
    SQLValueKind kind_ { SQLValueKind::Null };
    std::variant<std::monostate, std::int64_t, double, std::string, std::vector<std::byte>, bool> value_ {};

public:
    SQLValue() = default;
    explicit SQLValue(std::nullptr_t) {}
    explicit SQLValue(std::int64_t value) : kind_ { SQLValueKind::Integer }, value_ { value } {}
    explicit SQLValue(double value) : kind_ { SQLValueKind::Real }, value_ { value } {}
    explicit SQLValue(std::string value) : kind_ { SQLValueKind::Text }, value_ { std::move(value) } {}
    explicit SQLValue(std::vector<std::byte> value) : kind_ { SQLValueKind::Blob }, value_ { std::move(value) } {}
    explicit SQLValue(bool value) : kind_ { SQLValueKind::Boolean }, value_ { value } {}

    [[nodiscard]] SQLValueKind kind() const { return kind_; }
    [[nodiscard]] bool is_null() const { return kind_ == SQLValueKind::Null; }
    [[nodiscard]] const auto& value() const { return value_; }
};

export inline std::string command_to_string(SQLCommand command, std::string_view lastToken = {}) {
    switch (command) {
    case SQLCommand::Insert: return "INSERT";
    case SQLCommand::Update:
    case SQLCommand::UpdateSet: return "UPDATE";
    case SQLCommand::In:
    case SQLCommand::Where: return lastToken.empty() ? "WHERE" : std::string { lastToken };
    default: return std::string { lastToken };
    }
}

export ParsedSQL parse_sql(std::string_view query, bool partial = false) {
    std::string text { query };
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    std::size_t first { 0 };
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;

    ParsedSQL result {};
    std::string token;
    bool quoted { false };
    auto consume = [&]() -> bool {
        if (token.empty()) return false;
        result.lastToken = token;
        if (token == "SELECT" || token == "PRAGMA" || token == "WITH" || token == "EXPLAIN" || token == "RETURNING") {
            result.canReturnRows = true;
        } else if (result.command == SQLCommand::None) {
            if (token == "INSERT") result.command = SQLCommand::Insert;
            else if (token == "UPDATE") result.command = SQLCommand::Update;
            else if (token == "WHERE") result.command = SQLCommand::Where;
            else if (token == "SET") result.command = SQLCommand::UpdateSet;
            else if (token == "IN") result.command = SQLCommand::In;
        }
        token.clear();
        return partial && result.command != SQLCommand::None;
    };
    for (std::size_t i { first }; i < text.size(); ++i) {
        const char c { text[i] };
        if (c == '\'' || c == '"') { quoted = !quoted; continue; }
        if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            if (consume()) return result;
            continue;
        }
        if (!quoted) token.insert(token.begin(), c);
    }
    consume();
    return result;
}

} // namespace mbun::sqlite
