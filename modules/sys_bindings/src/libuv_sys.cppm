export module mbun.sys_bindings.libuv_sys;

import std;

namespace mbun::libuv_sys {

// Ref: bun-ref/src/libuv_sys/libuv.rs and bun-zig-src/src/libuv_sys/libuv.zig.
// The integer table is kept independent from a libuv installation.
export enum class Error : std::int32_t {
    success = 0,
    again = -4088,
    canceled = -4081,
    invalid_argument = -4071,
    no_entry = -4058,
    permission_denied = -4092,
    timed_out = -4039,
    unknown = -4094,
    eof = -4095,
};

export using Loop = void*;
export using Handle = void*;
export using FileDescriptor = std::int32_t;

export struct Backend {
    virtual ~Backend() = default;
    virtual int run(Loop loop, int mode) = 0;
    virtual void stop(Loop loop) = 0;
};

export constexpr bool is_error(int value) { return value < 0; }

} // namespace mbun::libuv_sys
