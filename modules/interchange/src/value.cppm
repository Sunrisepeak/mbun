// Value tree shared by Bun interchange formats and config consumers.
//
// Ref: bun-zig-src/src/interchange/{interchange,json,toml}.zig and
// bun-ref/src/parsers/interchange.zig. Bun's parsers produce one expression
// tree; this owned value is the format-independent MC++ equivalent. Concrete
// TOML/JSON parsing remains outside this member.
export module mbun.interchange.value;

import std;

namespace mbun::interchange {

export enum class ValueKind : std::uint8_t { null_value, boolean, integer, number, string, array, object };

export class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;

private:
    ValueKind kind_ { ValueKind::null_value };
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> data_ { nullptr };

public:
    Value() = default;
    explicit Value(std::nullptr_t) : Value {} {}
    explicit Value(bool value) : kind_ { ValueKind::boolean }, data_ { value } {}
    explicit Value(std::int64_t value) : kind_ { ValueKind::integer }, data_ { value } {}
    explicit Value(double value) : kind_ { ValueKind::number }, data_ { value } {}
    explicit Value(std::string value) : kind_ { ValueKind::string }, data_ { std::move(value) } {}
    explicit Value(const char* value) : Value { std::string { value } } {}
    explicit Value(Array value) : kind_ { ValueKind::array }, data_ { std::move(value) } {}
    explicit Value(Object value) : kind_ { ValueKind::object }, data_ { std::move(value) } {}

    static Value null() { return Value {}; }
    static Value boolean(bool value) { return Value { value }; }
    static Value integer(std::int64_t value) { return Value { value }; }
    static Value number(double value) { return Value { value }; }
    static Value string(std::string value) { return Value { std::move(value) }; }
    static Value array(Array value = {}) { return Value { std::move(value) }; }
    static Value object(Object value = {}) { return Value { std::move(value) }; }

    [[nodiscard]] ValueKind kind() const noexcept { return kind_; }
    [[nodiscard]] bool is_scalar() const noexcept { return kind_ <= ValueKind::string; }
    [[nodiscard]] const bool* as_boolean() const noexcept { return std::get_if<bool>(&data_); }
    [[nodiscard]] const std::int64_t* as_integer() const noexcept { return std::get_if<std::int64_t>(&data_); }
    [[nodiscard]] const double* as_number() const noexcept { return std::get_if<double>(&data_); }
    [[nodiscard]] const std::string* as_string() const noexcept { return std::get_if<std::string>(&data_); }
    [[nodiscard]] const Array* as_array() const noexcept { return std::get_if<Array>(&data_); }
    [[nodiscard]] Array* as_array() noexcept { return std::get_if<Array>(&data_); }
    [[nodiscard]] const Object* as_object() const noexcept { return std::get_if<Object>(&data_); }
    [[nodiscard]] Object* as_object() noexcept { return std::get_if<Object>(&data_); }

    [[nodiscard]] const Value* find(std::string_view key) const noexcept {
        const auto* object { as_object() };
        if (object == nullptr) return nullptr;
        for (const auto& [name, value] : *object) {
            if (name == key) return &value;
        }
        return nullptr;
    }

    [[nodiscard]] Value* find(std::string_view key) noexcept {
        return const_cast<Value*>(std::as_const(*this).find(key));
    }

    bool insert(std::string key, Value value) {
        auto* object { as_object() };
        if (object == nullptr) return false;
        if (auto* existing { find(key) }; existing != nullptr) {
            *existing = std::move(value);
        } else {
            object->emplace_back(std::move(key), std::move(value));
        }
        return true;
    }
};

} // namespace mbun::interchange
