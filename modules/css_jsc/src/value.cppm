// JS-facing value conversion seam.
// ref: bun-ref/src/css_jsc/color_js.rs and bun-zig-src/src/css_jsc/color_js.zig.
// Real JSValue access is intentionally deferred until the JSC adapter is wired.
export module mbun.css_jsc.value;

import std;

export namespace mbun::css_jsc {

enum class JsValueKind : std::uint8_t { Undefined, Null, Boolean, Number, String, Array, Object };

struct JsValue {
    JsValueKind kind { JsValueKind::Undefined };
    bool boolean_value { false };
    double number_value { 0.0 };
    std::string string_value;
    std::vector<double> array_values;
    std::vector<std::pair<std::string, double>> object_values;

    static JsValue undefined() { return {}; }
    static JsValue null() { return { .kind = JsValueKind::Null }; }
    static JsValue boolean(bool value) { return { .kind = JsValueKind::Boolean, .boolean_value = value }; }
    static JsValue number(double value) { return { .kind = JsValueKind::Number, .number_value = value }; }
    static JsValue string(std::string_view value) {
        return { .kind = JsValueKind::String, .string_value = std::string { value } };
    }
    static JsValue array(std::vector<double> values) {
        return { .kind = JsValueKind::Array, .array_values = std::move(values) };
    }
    static JsValue object(std::vector<std::pair<std::string, double>> values) {
        return { .kind = JsValueKind::Object, .object_values = std::move(values) };
    }
};

enum class ValueErrorKind : std::uint8_t { Missing, InvalidType, InvalidLength, InvalidNumber };

struct ValueError {
    ValueErrorKind kind { ValueErrorKind::InvalidType };
    std::string property;
    std::string message;
};

struct Rgba {
    std::uint8_t red { 0 };
    std::uint8_t green { 0 };
    std::uint8_t blue { 0 };
    std::uint8_t alpha { 255 };
};

inline std::expected<std::string, ValueError> css_text_from_value(
    const JsValue& value, std::string_view property) {
    if (value.kind == JsValueKind::Undefined || value.kind == JsValueKind::Null) {
        return std::unexpected(ValueError {
            .kind = ValueErrorKind::Missing,
            .property = std::string { property },
            .message = "expected a CSS string",
        });
    }
    if (value.kind != JsValueKind::String) {
        return std::unexpected(ValueError {
            .kind = ValueErrorKind::InvalidType,
            .property = std::string { property },
            .message = "expected a CSS string",
        });
    }
    return value.string_value;
}

// ref: bun-ref/src/jsc/JSValue.rs JSValue::to_int32/to_int64.
// Bun's JSC seam uses saturating truncation: NaN becomes zero, infinities and
// out-of-range values saturate, and finite in-range values truncate toward zero.
inline std::int64_t bun_saturating_to_int64(double value) noexcept {
    if (std::isnan(value)) return 0;

    constexpr double INT64_MIN_AS_DOUBLE { -0x1p63 };
    constexpr double INT64_MAX_EXCLUSIVE_AS_DOUBLE { 0x1p63 };
    if (value <= INT64_MIN_AS_DOUBLE) return std::numeric_limits<std::int64_t>::min();
    if (value >= INT64_MAX_EXCLUSIVE_AS_DOUBLE) return std::numeric_limits<std::int64_t>::max();
    return static_cast<std::int64_t>(value);
}

inline std::int32_t bun_saturating_to_int32(double value) noexcept {
    if (std::isnan(value)) return 0;

    constexpr double INT32_MIN_AS_DOUBLE { static_cast<double>(std::numeric_limits<std::int32_t>::min()) };
    constexpr double INT32_MAX_AS_DOUBLE { static_cast<double>(std::numeric_limits<std::int32_t>::max()) };
    if (value <= INT32_MIN_AS_DOUBLE) return std::numeric_limits<std::int32_t>::min();
    if (value >= INT32_MAX_AS_DOUBLE) return std::numeric_limits<std::int32_t>::max();
    return static_cast<std::int32_t>(value);
}

inline std::expected<std::uint8_t, ValueError> color_channel(double value, std::string_view) {
    return static_cast<std::uint8_t>(std::clamp(bun_saturating_to_int32(value), 0, 255));
}

inline std::optional<double> object_number(const JsValue& value, std::string_view key) {
    for (const auto& [name, number] : value.object_values) {
        if (name == key) return number;
    }
    return std::nullopt;
}

inline std::expected<Rgba, ValueError> rgba_from_value(const JsValue& value) {
    if (value.kind == JsValueKind::Number) {
        // ref: bun-ref/src/css_jsc/color_js.rs numeric input path. Convert as
        // JSValue::to_int64(), then keep the low 32 bits. Unsigned conversion
        // is defined modulo 2^32 and avoids overflow/LLONG_MIN absolute-value UB.
        auto packed { static_cast<std::uint32_t>(bun_saturating_to_int64(value.number_value)) };
        return Rgba {
            .red = static_cast<std::uint8_t>((packed >> 16) & 0xff),
            .green = static_cast<std::uint8_t>((packed >> 8) & 0xff),
            .blue = static_cast<std::uint8_t>(packed & 0xff),
            .alpha = packed > 0x00ff'ffff ? static_cast<std::uint8_t>(packed >> 24) : std::uint8_t { 255 },
        };
    }
    if (value.kind == JsValueKind::Array) {
        if (value.array_values.size() != 3 && value.array_values.size() != 4) {
            return std::unexpected(ValueError { .kind = ValueErrorKind::InvalidLength,
                .message = "expected array length 3 or 4" });
        }
        auto red { color_channel(value.array_values[0], "[0]") };
        auto green { color_channel(value.array_values[1], "[1]") };
        auto blue { color_channel(value.array_values[2], "[2]") };
        if (!red || !green || !blue) return std::unexpected((!red ? red.error() : !green ? green.error() : blue.error()));
        auto alpha { value.array_values.size() == 4 ? color_channel(value.array_values[3], "[3]")
                                                   : std::expected<std::uint8_t, ValueError> { 255 } };
        if (!alpha) return std::unexpected(alpha.error());
        return Rgba { *red, *green, *blue, *alpha };
    }
    if (value.kind == JsValueKind::Object) {
        auto red_value { object_number(value, "r") };
        auto green_value { object_number(value, "g") };
        auto blue_value { object_number(value, "b") };
        if (!red_value || !green_value || !blue_value) {
            return std::unexpected(ValueError { .kind = ValueErrorKind::Missing,
                .message = "expected r, g, and b color fields" });
        }
        auto red { color_channel(*red_value, "r") };
        auto green { color_channel(*green_value, "g") };
        auto blue { color_channel(*blue_value, "b") };
        if (!red || !green || !blue) return std::unexpected((!red ? red.error() : !green ? green.error() : blue.error()));
        auto alpha_value { object_number(value, "a") };
        // ref: bun-ref/src/css_jsc/color_js.rs object alpha path. Rust's
        // rem_euclid(256) is equivalent to the defined conversion to uint8_t.
        auto alpha { alpha_value ? static_cast<std::uint8_t>(bun_saturating_to_int64(*alpha_value * 255.0))
                                 : std::uint8_t { 255 } };
        return Rgba { *red, *green, *blue, alpha };
    }
    return std::unexpected(ValueError { .kind = ValueErrorKind::InvalidType,
        .message = "expected a string, number, array, or object" });
}

}  // namespace mbun::css_jsc
