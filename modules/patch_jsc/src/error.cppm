// JSC-facing error taxonomy. Native/JSC conversion is intentionally injected.
// ref: bun-ref/src/patch/error.rs and patch_jsc/testing.rs;
//      bun-zig-src/src/patch_jsc/testing.zig.
export module mbun.patch_jsc.error;

import std;

export namespace mbun::patch_jsc {

enum class ErrorKind : std::uint8_t {
    InvalidInput,
    Parse,
    Apply,
    Backend,
    Unavailable,
};

struct Error {
    ErrorKind kind { ErrorKind::Backend };
    std::string operation;
    std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

using VoidResult = Result<void>;

inline VoidResult ok() { return {}; }

} // namespace mbun::patch_jsc
