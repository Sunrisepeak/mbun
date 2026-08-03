// Listener lifecycle seam derived from bun runtime/server/ServerConfig and
// NewServer start/stop/listen. Socket creation is injected for portability.
export module mbun.runtime_server.listener;

import std;

namespace mbun::runtime_server {

export struct ListenerConfig {
    std::string hostname{};
    std::uint16_t port{};
    bool reusePort{};
    bool ipv6Only{};
    bool tls{};
};

export enum class ListenerState : std::uint8_t { stopped, starting, listening, stopping };

export class ListenerBackend {
public:
    virtual ~ListenerBackend() = default;
    virtual std::optional<std::uint16_t> listen(const ListenerConfig& config) = 0;
    virtual void close() = 0;
};

export class Listener {
private:
    ListenerBackend* backend_{};
    ListenerConfig config_{};
    ListenerState state_{ListenerState::stopped};
    std::uint16_t boundPort_{};

public:
    Listener(ListenerBackend& backend, ListenerConfig config = {})
        : backend_{&backend}, config_{std::move(config)} {}

    bool start() {
        if (state_ != ListenerState::stopped) return false;
        state_ = ListenerState::starting;
        const auto port{backend_->listen(config_)};
        if (!port) {
            state_ = ListenerState::stopped;
            return false;
        }
        boundPort_ = *port;
        state_ = ListenerState::listening;
        return true;
    }

    bool stop() {
        if (state_ != ListenerState::listening) return false;
        state_ = ListenerState::stopping;
        backend_->close();
        state_ = ListenerState::stopped;
        boundPort_ = 0;
        return true;
    }

    ListenerState state() const { return state_; }
    std::uint16_t bound_port() const { return boundPort_; }
    const ListenerConfig& config() const { return config_; }
};

}
