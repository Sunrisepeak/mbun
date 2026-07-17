export module mbun.sys_bindings.cares_sys;

import std;

namespace mbun::cares_sys {

// Ref: bun-ref/src/cares_sys/c_ares.rs and bun-zig-src/src/cares_sys/c_ares.zig.
// c-ares itself is intentionally not linked in this first translation pass.
export using Channel = void*;
export using Query = void*;
export using Callback = void (*)(void*, int, int, const unsigned char*, int);

export enum class Status : std::int32_t {
    success = 0,
    failure = 1,
    timeout = 12,
    canceled = 24,
    not_implemented =  -1,
};

export struct Backend {
    virtual ~Backend() = default;
    virtual Status init(Channel* channel) = 0;
    virtual void destroy(Channel channel) = 0;
    virtual Status query(Channel channel, std::string_view name, Callback callback,
                         void* userData) = 0;
};

} // namespace mbun::cares_sys
