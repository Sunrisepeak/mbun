// PostgreSQL query parameters and array text serialization.
// Reference: bun-ref src/js/internal/sql/postgres.ts arrayValueSerializer.
export module mbun.postgres.parameters;

import std;

namespace mbun::postgres {

export using Bytes = std::vector<std::byte>;
export using Parameter = std::variant<std::monostate, bool, std::int64_t, double, std::string, Bytes>;

export inline std::string escape_array_text(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char c : value) {
        if (c == '\\' || c == '"') escaped.push_back('\\');
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

export inline std::string parameter_to_text(const Parameter& parameter) {
    return std::visit([](const auto& value) -> std::string {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::monostate>) return "null";
        else if constexpr (std::is_same_v<T, bool>) return value ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::string>) return escape_array_text(value);
        else if constexpr (std::is_same_v<T, Bytes>) {
            std::string text { "\\x" };
            constexpr char hex[] { "0123456789abcdef" };
            for (auto byte : value) {
                auto n { std::to_integer<unsigned>(byte) };
                text.push_back(hex[n >> 4]); text.push_back(hex[n & 0xf]);
            }
            return escape_array_text(text);
        } else return std::to_string(value);
    }, parameter);
}

export inline std::string serialize_array(const std::vector<Parameter>& values, char delimiter = ',') {
    std::string result { "{" };
    for (std::size_t i {}; i < values.size(); ++i) {
        if (i) result.push_back(delimiter);
        result += parameter_to_text(values[i]);
    }
    result.push_back('}');
    return result;
}

} // namespace mbun::postgres
