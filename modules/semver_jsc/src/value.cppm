export module mbun.semver_jsc.value;

import std;

export namespace mbun::semver_jsc {
enum class ValueKind : std::uint8_t { string, number, boolean, null_value };

class JsValue {
    std::variant<std::string, double, bool, std::nullptr_t> value_;
public:
    explicit JsValue(std::string value) : value_ { std::move(value) } {}
    explicit JsValue(std::string_view value) : value_ { std::string { value } } {}
    explicit JsValue(const char* value) : value_ { std::string { value } } {}
    explicit JsValue(double value) : value_ { value } {}
    explicit JsValue(bool value) : value_ { value } {}
    explicit JsValue(std::nullptr_t value) : value_ { value } {}
    ValueKind kind() const {
        if (std::holds_alternative<std::string>(value_)) return ValueKind::string;
        if (std::holds_alternative<double>(value_)) return ValueKind::number;
        if (std::holds_alternative<bool>(value_)) return ValueKind::boolean;
        return ValueKind::null_value;
    }
    const auto& raw() const { return value_; }
};

enum class ValueErrorKind : std::uint8_t { unsupported_value, non_finite_number };
struct ValueError { ValueErrorKind kind { ValueErrorKind::unsupported_value }; std::string message {}; };

inline std::expected<std::string, ValueError> to_string(const JsValue& value) {
    if (const auto* text { std::get_if<std::string>(&value.raw()) }) return *text;
    if (const auto* number { std::get_if<double>(&value.raw()) }) {
        if (!std::isfinite(*number)) return std::unexpected(ValueError { ValueErrorKind::non_finite_number, "SemVer argument number is not finite" });
        return std::format("{}", *number);
    }
    if (const auto* boolean { std::get_if<bool>(&value.raw()) }) return *boolean ? "true" : "false";
    return "null";
}

inline bool is_ascii(std::string_view value) {
    return std::ranges::all_of(value, [](unsigned char c) { return c < 0x80U; });
}

struct SemverStringJsc { std::string_view slice {}; JsValue to_js() const { return JsValue { slice }; } };
}  // namespace mbun::semver_jsc
