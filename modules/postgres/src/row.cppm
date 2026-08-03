// PostgreSQL field and row shapes, independent of JSC and socket ownership.
// Reference: bun-ref sql/postgres protocol FieldDescription/DataRow and
// sql_jsc/postgres/PostgresSQLStatement.rs.
export module mbun.postgres.row;

import std;
import mbun.postgres.parameters;
import mbun.postgres.types;

namespace mbun::postgres {

export class ColumnIdentifier {
private:
    std::variant<std::string, std::uint32_t> value_ {};

public:
    explicit ColumnIdentifier(std::string name) {
        bool numeric { !name.empty() && name.size() <= 10 && std::ranges::all_of(name, [](unsigned char c) { return std::isdigit(c); }) };
        if (numeric) {
            std::uint64_t index {};
            for (char c : name) index = index * 10 + static_cast<unsigned>(c - '0');
            if (index <= std::numeric_limits<std::uint32_t>::max()) value_ = static_cast<std::uint32_t>(index);
            else value_ = std::move(name);
        } else value_ = std::move(name);
    }
    [[nodiscard]] bool is_index() const { return std::holds_alternative<std::uint32_t>(value_); }
    [[nodiscard]] std::optional<std::uint32_t> index() const { return is_index() ? std::optional { std::get<std::uint32_t>(value_) } : std::nullopt; }
    [[nodiscard]] std::string_view name() const { return is_index() ? std::string_view {} : std::get<std::string>(value_); }
};

export struct FieldDescription {
    ColumnIdentifier name;
    std::uint32_t table_oid {};
    std::uint16_t column_index {};
    TypeId type { TypeId::Text };
    std::int16_t type_size { -1 };
    std::int32_t type_modifier { -1 };
    std::int16_t format_code {};
};

export using RowValue = std::variant<std::monostate, bool, std::int64_t, double, std::string, Bytes>;

export struct Row {
    std::vector<ColumnIdentifier> columns {};
    std::vector<RowValue> values {};
    [[nodiscard]] std::size_t size() const { return values.size(); }
    [[nodiscard]] const RowValue* at(std::size_t index) const { return index < values.size() ? &values[index] : nullptr; }
};

} // namespace mbun::postgres
