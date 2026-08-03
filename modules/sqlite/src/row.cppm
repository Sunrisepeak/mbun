// row.cppm — stable row shape for sqlite result conversion.
//
// Bun exposes object, values, and raw result modes. Keeping names and values
// together here lets the later JSC adapter choose a mode without changing the
// native query layer.
export module mbun.sqlite.row;

import std;
import mbun.sqlite.value;

namespace mbun::sqlite {

export class ColumnIdentifier {
private:
    std::variant<std::string, std::uint32_t> value_;

public:
    explicit ColumnIdentifier(std::string name) {
        if (!name.empty() && name.size() <= 10
            && std::ranges::all_of(name, [](unsigned char c) { return std::isdigit(c); })) {
            std::uint64_t index{0};
            for (char c : name) index = index * 10 + static_cast<unsigned>(c - '0');
            if (index < std::numeric_limits<std::uint32_t>::max()) {
                value_ = static_cast<std::uint32_t>(index);
            } else {
                value_ = std::move(name);
            }
        } else {
            value_ = std::move(name);
        }
    }
    bool is_index() const noexcept { return std::holds_alternative<std::uint32_t>(value_); }
    std::optional<std::uint32_t> index() const noexcept {
        return is_index() ? std::optional{std::get<std::uint32_t>(value_)} : std::nullopt;
    }
    std::string_view name() const noexcept {
        return is_index() ? std::string_view{} : std::get<std::string>(value_);
    }
};

export class Row {
public:
    Row() = default;
    Row(std::vector<std::string> columns, std::vector<SqlValue> values)
        : columns_{std::move(columns)}, values_{std::move(values)} {}

    std::size_t size() const noexcept { return values_.size(); }
    bool empty() const noexcept { return values_.empty(); }
    const std::vector<std::string>& columns() const noexcept { return columns_; }
    const std::vector<SqlValue>& values() const noexcept { return values_; }

    const SqlValue* at(std::size_t index) const noexcept {
        return index < values_.size() ? &values_[index] : nullptr;
    }

    const SqlValue* at(std::string_view column) const noexcept {
        for (std::size_t i{0}; i < columns_.size(); ++i) {
            if (columns_[i] == column) return at(i);
        }
        return nullptr;
    }

private:
    std::vector<std::string> columns_;
    std::vector<SqlValue> values_;
};

export enum class ResultMode : std::uint8_t { objects, values, raw };

} // namespace mbun::sqlite
