// value.cppm — mbun.ini.value: the INI value model + JS-style to_string.
//
// Mechanical port of the JS-value subset used by bun's src/ini/lib.rs
// (`bun_ast::Expr` / `E::{Object,Array,EString,ENumber,Boolean,Null}`).
// The INI parser only ever produces these six shapes, so instead of pulling in
// the full AST we model them directly. Object preserves insertion order (JS
// objects do) and does linear lookup — INI configs are tiny.
//
// `ToStringFormatter` mirrors `ini::ToStringFormatter` (the npm-quirk path that
// stringifies a JSON array/object/number used as a section header or key).
export module mbun.ini.value;

import std;

namespace mbun::ini {

export enum class Type { Object, Array, String, Number, Boolean, Null };

export class Value {
public:
    using Array = std::vector<Value>;

    // Ordered object: JS objects keep insertion order; lookup is linear.
    struct Object {
        std::vector<std::pair<std::string, Value>> properties;

        Value* find(std::string_view key) {
            for (auto& [k, v] : properties) {
                if (k == key) {
                    return &v;
                }
            }
            return nullptr;
        }
        const Value* find(std::string_view key) const {
            for (const auto& [k, v] : properties) {
                if (k == key) {
                    return &v;
                }
            }
            return nullptr;
        }
    };

public:
    Value() : type_{Type::Object}, data_{Object{}} {}

    static Value make_object() {
        Value v;
        v.type_ = Type::Object;
        v.data_ = Object{};
        return v;
    }
    static Value make_array() {
        Value v;
        v.type_ = Type::Array;
        v.data_ = Array{};
        return v;
    }
    static Value string(std::string s) {
        Value v;
        v.type_ = Type::String;
        v.data_ = std::move(s);
        return v;
    }
    static Value number(double d) {
        Value v;
        v.type_ = Type::Number;
        v.data_ = d;
        return v;
    }
    static Value boolean(bool b) {
        Value v;
        v.type_ = Type::Boolean;
        v.data_ = b;
        return v;
    }
    static Value null() {
        Value v;
        v.type_ = Type::Null;
        v.data_ = std::monostate{};
        return v;
    }

public:
    Type type() const { return type_; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_string() const { return type_ == Type::String; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_boolean() const { return type_ == Type::Boolean; }
    bool is_null() const { return type_ == Type::Null; }

    Object& object() { return std::get<Object>(data_); }
    const Object& object() const { return std::get<Object>(data_); }
    Array& array() { return std::get<Array>(data_); }
    const Array& array() const { return std::get<Array>(data_); }

    const std::string& as_string() const { return std::get<std::string>(data_); }
    double as_number() const { return std::get<double>(data_); }
    bool as_bool() const { return std::get<bool>(data_); }

    // Object property lookup by exact key; nullptr if not an object / absent.
    const Value* get(std::string_view key) const {
        if (type_ != Type::Object) {
            return nullptr;
        }
        return std::get<Object>(data_).find(key);
    }
    Value* get(std::string_view key) {
        if (type_ != Type::Object) {
            return nullptr;
        }
        return std::get<Object>(data_).find(key);
    }

    // Insert or overwrite `key`. Preserves position on overwrite.
    void put(std::string_view key, Value value) {
        auto& obj = std::get<Object>(data_);
        if (Value* existing = obj.find(key)) {
            *existing = std::move(value);
            return;
        }
        obj.properties.emplace_back(std::string{key}, std::move(value));
    }

    std::size_t size() const {
        if (type_ == Type::Array) {
            return std::get<Array>(data_).size();
        }
        if (type_ == Type::Object) {
            return std::get<Object>(data_).properties.size();
        }
        return 0;
    }

private:
    Type type_;
    std::variant<Object, Array, std::string, double, bool, std::monostate> data_;
};

// JS `Number.prototype.toString` for the finite/typical range used by INI.
export std::string number_to_string(double d) {
    if (std::isnan(d)) {
        return "NaN";
    }
    if (std::isinf(d)) {
        return d < 0 ? "-Infinity" : "Infinity";
    }
    if (d == std::floor(d) && std::abs(d) < 1e21) {
        // Integral: print without a fractional part (matches JS).
        return std::format("{}", static_cast<long long>(d));
    }
    return std::format("{}", d);
}

// Mirrors `ini::ToStringFormatter::fmt` — used when a JSON array/object/number
// is (ab)used as a section header or key (npm/ini quirk).
export std::string to_string(const Value& v) {
    switch (v.type()) {
        case Type::Array: {
            const auto& items = v.array();
            std::string out;
            for (std::size_t i = 0; i < items.size(); ++i) {
                out += to_string(items[i]);
                if (i + 1 != items.size()) {
                    out += ',';
                }
            }
            return out;
        }
        case Type::Object:
            return "[Object object]";
        case Type::Boolean:
            return v.as_bool() ? "true" : "false";
        case Type::Number:
            return number_to_string(v.as_number());
        case Type::String:
            return v.as_string();
        case Type::Null:
            return "null";
    }
    return {};
}

}  // namespace mbun::ini
