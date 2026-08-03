// value.cppm — shared ordered value tree for mbun configuration parsers.
export module mbun.config.value;

import std;

export namespace mbun::config {

enum class Type : std::uint8_t { Object, Array, String, Integer, Float, Boolean, Null };

class Value {
public:
    using Array = std::vector<Value>;

    struct Object {
        std::vector<std::pair<std::string, Value>> entries;

        Value* find(std::string_view key) noexcept {
            for (auto& [candidate, value] : entries) {
                if (candidate == key) {
                    return &value;
                }
            }
            return nullptr;
        }

        const Value* find(std::string_view key) const noexcept {
            for (const auto& [candidate, value] : entries) {
                if (candidate == key) {
                    return &value;
                }
            }
            return nullptr;
        }
    };

    Value() : type_{Type::Null}, data_{std::monostate{}} {}

    static Value make_object() {
        return Value{Type::Object, Object{}};
    }

    static Value make_array() {
        return Value{Type::Array, Array{}};
    }

    static Value string(std::string value) {
        return Value{Type::String, std::move(value)};
    }

    static Value integer(std::int64_t value) {
        return Value{Type::Integer, value};
    }

    static Value floating(double value) {
        return Value{Type::Float, value};
    }

    static Value boolean(bool value) {
        return Value{Type::Boolean, value};
    }

    static Value null() {
        return Value{};
    }

    Type type() const noexcept {
        return type_;
    }

    bool is_object() const noexcept { return type_ == Type::Object; }
    bool is_array() const noexcept { return type_ == Type::Array; }
    bool is_string() const noexcept { return type_ == Type::String; }
    bool is_integer() const noexcept { return type_ == Type::Integer; }
    bool is_float() const noexcept { return type_ == Type::Float; }
    bool is_number() const noexcept { return is_integer() || is_float(); }
    bool is_boolean() const noexcept { return type_ == Type::Boolean; }
    bool is_null() const noexcept { return type_ == Type::Null; }

    Object& object() { return std::get<Object>(data_); }
    const Object& object() const { return std::get<Object>(data_); }
    Array& array() { return std::get<Array>(data_); }
    const Array& array() const { return std::get<Array>(data_); }
    std::string& as_string() { return std::get<std::string>(data_); }
    const std::string& as_string() const { return std::get<std::string>(data_); }
    std::int64_t as_integer() const { return std::get<std::int64_t>(data_); }
    double as_double() const {
        return is_integer() ? static_cast<double>(as_integer()) : std::get<double>(data_);
    }
    bool as_bool() const { return std::get<bool>(data_); }

    Value* get(std::string_view key) noexcept {
        return is_object() ? object().find(key) : nullptr;
    }
    const Value* get(std::string_view key) const noexcept {
        return is_object() ? object().find(key) : nullptr;
    }
    Value& at(std::size_t index) { return array().at(index); }
    const Value& at(std::size_t index) const { return array().at(index); }

    std::size_t size() const noexcept {
        if (is_object()) {
            return object().entries.size();
        }
        if (is_array()) {
            return array().size();
        }
        return 0;
    }

    bool operator==(const Value& other) const;

private:
    using Data = std::variant<Object, Array, std::string, std::int64_t, double, bool, std::monostate>;

    template <typename T>
    Value(Type type, T&& data) : type_{type}, data_{std::forward<T>(data)} {}

    Type type_;
    Data data_;
};

inline bool Value::operator==(const Value& other) const {
    if (is_number() && other.is_number()) {
        return as_double() == other.as_double();
    }
    if (type_ != other.type_) {
        return false;
    }
    switch (type_) {
    case Type::Object: {
        const auto& left{object().entries};
        const auto& right{other.object().entries};
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t i{}; i < left.size(); ++i) {
            if (left[i].first != right[i].first || !(left[i].second == right[i].second)) {
                return false;
            }
        }
        return true;
    }
    case Type::Array: {
        const auto& left{array()};
        const auto& right{other.array()};
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t i{}; i < left.size(); ++i) {
            if (!(left[i] == right[i])) {
                return false;
            }
        }
        return true;
    }
    case Type::String: return as_string() == other.as_string();
    case Type::Integer: return as_integer() == other.as_integer();
    case Type::Float: return as_double() == other.as_double();
    case Type::Boolean: return as_bool() == other.as_bool();
    case Type::Null: return true;
    }
    std::unreachable();
}

namespace detail {

inline constexpr std::uint64_t FNV_OFFSET{0xcbf29ce484222325ULL};
inline constexpr std::uint64_t FNV_PRIME{0x100000001b3ULL};

inline void hash_byte(std::uint64_t& state, std::uint8_t byte) noexcept {
    state = (state ^ byte) * FNV_PRIME;
}

inline void hash_u64(std::uint64_t& state, std::uint64_t value) noexcept {
    for (int i{}; i < 8; ++i) {
        hash_byte(state, static_cast<std::uint8_t>(value));
        value >>= 8;
    }
}

inline void hash_string(std::uint64_t& state, std::string_view value) noexcept {
    hash_byte(state, 0x53);
    hash_u64(state, value.size());
    for (const unsigned char byte : value) {
        hash_byte(state, byte);
    }
}

inline void hash_value(std::uint64_t& state, const Value& value) noexcept {
    switch (value.type()) {
    case Type::Null: hash_byte(state, 0x4E); return;
    case Type::Boolean: hash_byte(state, value.as_bool() ? 0x54 : 0x46); return;
    case Type::Integer:
    case Type::Float: {
        hash_byte(state, 0x44);
        std::uint64_t bits{std::bit_cast<std::uint64_t>(value.as_double())};
        hash_u64(state, bits);
        return;
    }
    case Type::String: hash_string(state, value.as_string()); return;
    case Type::Array:
        hash_byte(state, 0x41);
        hash_u64(state, value.size());
        for (const auto& child : value.array()) {
            hash_value(state, child);
        }
        return;
    case Type::Object:
        hash_byte(state, 0x4F);
        hash_u64(state, value.size());
        for (const auto& [key, child] : value.object().entries) {
            hash_string(state, key);
            hash_value(state, child);
        }
        return;
    }
    std::unreachable();
}

} // namespace detail

inline std::uint64_t canonical_hash(const Value& value) noexcept {
    std::uint64_t state{detail::FNV_OFFSET};
    detail::hash_value(state, value);
    return state;
}

struct ParseError {
    std::string message;
    std::size_t offset{};
};

} // namespace mbun::config
