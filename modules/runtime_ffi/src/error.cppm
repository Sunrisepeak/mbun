// Error model derived from bun runtime/ffi getDlError and TCC diagnostics.
export module mbun.runtime_ffi.error;

import std;

export namespace mbun::runtime_ffi {

enum class ErrorKind : std::uint8_t {
    missing_library,
    missing_symbol,
    invalid_call,
    backend_unavailable,
};

struct Error {
    ErrorKind kind{};
    std::string subject{};
    std::string detail{};

    static Error missing_library(std::string_view name, std::string_view detail = {}) {
        return {ErrorKind::missing_library, std::string{name}, std::string{detail}};
    }

    static Error missing_symbol(std::string_view name, std::string_view detail = {}) {
        return {ErrorKind::missing_symbol, std::string{name}, std::string{detail}};
    }

    static Error invalid_call(std::string_view name, std::string_view detail = {}) {
        return {ErrorKind::invalid_call, std::string{name}, std::string{detail}};
    }

    static Error backend_unavailable(std::string_view detail) {
        return {ErrorKind::backend_unavailable, {}, std::string{detail}};
    }
};

}  // namespace mbun::runtime_ffi
