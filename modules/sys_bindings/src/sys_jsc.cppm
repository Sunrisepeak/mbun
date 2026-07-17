export module mbun.sys_bindings.sys_jsc;

import std;

namespace mbun::sys_jsc {

// Ref: bun-ref/src/sys_jsc/{fd_jsc,error_jsc,signal_code_jsc}.rs and the
// corresponding Zig files. JSC-owned values remain opaque at this boundary.
export using JsValue = void*;
export using GlobalObject = void*;

export struct FdJsc {
    static constexpr std::int32_t INVALID = -1;

    static constexpr std::optional<std::int32_t> from_js_integer(std::int64_t value) {
        if (value < 0 || value > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(value);
    }

    static constexpr std::int32_t to_js(std::int64_t value) {
        return value < 0 || value > std::numeric_limits<std::int32_t>::max()
            ? INVALID
            : static_cast<std::int32_t>(value);
    }
};

export enum class SignalCode : std::uint8_t { default_signal = 15 };

export enum class SignalParseError : std::uint8_t {
    unknown,
    non_integral,
    negative,
    out_of_range,
    invalid_type,
};

export std::expected<SignalCode, SignalParseError> parse_signal_number(double value) {
    if (std::isnan(value)) {
        return SignalCode::default_signal;
    }
    if (!std::isfinite(value) || std::trunc(value) != value) {
        return std::unexpected(SignalParseError::non_integral);
    }
    if (value < 0) {
        return std::unexpected(SignalParseError::negative);
    }
    if (value > 31) {
        return std::unexpected(SignalParseError::out_of_range);
    }
    return static_cast<SignalCode>(static_cast<std::uint8_t>(value));
}

export struct ErrorJsc {
    std::int32_t code {};
    bool from_libuv {false};

    [[nodiscard]] constexpr bool is_native() const { return !from_libuv; }
};

} // namespace mbun::sys_jsc
