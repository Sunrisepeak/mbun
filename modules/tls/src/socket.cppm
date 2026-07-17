// socket.cppm — uWS-shaped connection state machine without native handles.
//
// ref: bun-ref/src/uws_sys/socket.rs (InternalSocket/NewSocketHandler) and
// bun-zig-src/src/uws_sys/socket.zig.  The tagged handle deliberately stays
// opaque: uSockets/libuv integration is DEFERRED(S-net).
export module mbun.tls.socket;

export import mbun.tls.backend;
export import mbun.tls.buffer;
export import mbun.tls.tls;

import std;
import mbun.tls.buffer;
import mbun.tls.tls;

namespace mbun::tls {

export enum class SocketKind : std::uint8_t { detached, tcp, tls };
export enum class ConnectionState : std::uint8_t { detached, connecting, open, closing, closed };

export class Connection {
private:
    std::uintptr_t handle_ {0};
    SocketKind kind_ {SocketKind::detached};
    ConnectionState state_ {ConnectionState::detached};
    ByteBuffer inbound_ {};
    ByteBuffer outbound_ {};
    std::optional<Session> tls_ {};

public:
    static Connection detached() noexcept { return {}; }

    static Connection tcp(std::uintptr_t handle) noexcept {
        Connection result;
        result.handle_ = handle;
        result.kind_ = SocketKind::tcp;
        result.state_ = ConnectionState::connecting;
        return result;
    }

    static Connection tls(std::uintptr_t handle, Config config) {
        Connection result {tcp(handle)};
        result.kind_ = SocketKind::tls;
        result.tls_.emplace(std::move(config));
        return result;
    }

    [[nodiscard]] std::uintptr_t handle() const noexcept { return handle_; }
    [[nodiscard]] SocketKind kind() const noexcept { return kind_; }
    [[nodiscard]] ConnectionState state() const noexcept { return state_; }
    [[nodiscard]] bool is_tls() const noexcept { return kind_ == SocketKind::tls; }
    [[nodiscard]] ByteBuffer& inbound() noexcept { return inbound_; }
    [[nodiscard]] ByteBuffer& outbound() noexcept { return outbound_; }
    [[nodiscard]] const std::optional<Session>& tls_session() const noexcept { return tls_; }

    void mark_open() noexcept {
        state_ = ConnectionState::open;
        if (tls_) {
            tls_->start();
        }
    }

    void mark_closed() noexcept {
        state_ = ConnectionState::closed;
        handle_ = 0;
    }

    void queue(std::span<const std::uint8_t> data) { outbound_.append(data); }
};

} // namespace mbun::tls
