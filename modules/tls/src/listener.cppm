// listener.cppm — listen configuration and lifecycle seam.
//
// ref: bun-ref/src/uws_sys/ListenSocket.rs and runtime/socket/Listener.rs;
// bun-zig-src/src/uws_sys/ListenSocket.zig and runtime/socket/Listener.zig.
// Actual bind/accept and SSL_CTX ownership are DEFERRED(S-net).
export module mbun.tls.listener;

export import mbun.tls.socket;

import std;
import mbun.tls.tls;

namespace mbun::tls {

export struct ListenAddress {
    std::string host {};
    std::uint16_t port {0};

    [[nodiscard]] bool valid() const noexcept {
        return !host.empty() && port != 0;
    }
};

export enum class ListenerState : std::uint8_t { idle, listening, closing, closed };

export class Listener {
private:
    ListenAddress address_ {};
    std::optional<Config> tlsConfig_ {};
    ListenerState state_ {ListenerState::idle};

public:
    Listener(ListenAddress address, std::optional<Config> tlsConfig = {})
        : address_ {std::move(address)}, tlsConfig_ {std::move(tlsConfig)} {}

    [[nodiscard]] const ListenAddress& address() const noexcept { return address_; }
    [[nodiscard]] const std::optional<Config>& tls_config() const noexcept { return tlsConfig_; }
    [[nodiscard]] ListenerState state() const noexcept { return state_; }
    [[nodiscard]] bool secure() const noexcept { return tlsConfig_.has_value(); }

    [[nodiscard]] bool start() noexcept {
        if (!address_.valid() || state_ != ListenerState::idle) {
            return false;
        }
        state_ = ListenerState::listening;
        return true;
    }

    void close() noexcept { state_ = ListenerState::closed; }
};

} // namespace mbun::tls
