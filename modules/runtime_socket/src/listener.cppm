export module mbun.runtime_socket.listener;

import std;
import mbun.runtime_socket.address;
import mbun.runtime_socket.backend;

export namespace mbun::runtime_socket {

enum class ListenerState : std::uint8_t { idle, listening, closing, closed, failed };

class Listener {
private:
    std::shared_ptr<SocketBackend> backend_ {};
    std::optional<NativeHandle> handle_ {};
    Address address_ {};
    ListenerState state_ { ListenerState::idle };
    std::size_t active_connections_ { 0 };

public:
    explicit Listener(std::shared_ptr<SocketBackend> backend)
        : backend_ { std::move(backend) }
    {
    }

    std::expected<void, BackendError> listen(Address address) {
        if (!backend_ || state_ != ListenerState::idle)
            return std::unexpected { BackendError { 0, "listener is not idle" } };
        auto result { backend_->listen(address) };
        if (!result) {
            state_ = ListenerState::failed;
            return std::unexpected { result.error() };
        }
        address_ = std::move(address);
        handle_ = *result;
        state_ = ListenerState::listening;
        return {};
    }

    void add_connection() {
        if (state_ == ListenerState::listening)
            ++active_connections_;
    }
    void remove_connection() { active_connections_ = active_connections_ == 0 ? 0 : active_connections_ - 1; }
    void close() {
        if (handle_) {
            backend_->close(*handle_);
            handle_.reset();
        }
        state_ = ListenerState::closed;
    }
    ~Listener() { close(); }

    ListenerState state() const { return state_; }
    std::optional<NativeHandle> native_handle() const { return handle_; }
    const Address& address() const { return address_; }
    std::size_t active_connections() const { return active_connections_; }
};

}
