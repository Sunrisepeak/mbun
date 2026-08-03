// backend.cppm — injected native I/O/TLS backend seam.
//
// ref: bun-ref/src/uws_sys/{socket,ListenSocket}.rs and
// bun-zig-src/src/uws_sys/{socket,ListenSocket}.zig.  The C ABI and concrete
// BoringSSL/libuv/uSockets implementation are DEFERRED(S-net).
export module mbun.tls.backend;

import std;

namespace mbun::tls {

export enum class BackendCode : std::uint8_t {
    ok,
    would_block,
    closed,
    failed,
};

export struct BackendResult {
    BackendCode code {BackendCode::ok};
    std::size_t bytes {0};

    [[nodiscard]] constexpr bool succeeded() const noexcept {
        return code == BackendCode::ok;
    }
};

export class Backend {
public:
    virtual ~Backend() = default;

    virtual BackendResult read(std::uintptr_t handle, std::span<std::uint8_t> out) = 0;
    virtual BackendResult write(std::uintptr_t handle, std::span<const std::uint8_t> data) = 0;
    virtual void close(std::uintptr_t handle) noexcept = 0;
};

} // namespace mbun::tls
