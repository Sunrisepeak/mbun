export module mbun.runtime_socket.connection;

import std;
import mbun.runtime_socket.address;
import mbun.runtime_socket.backend;

export namespace mbun::runtime_socket {

enum class ConnectionState : std::uint8_t { idle, connecting, open, paused, closing, closed, failed };

class Connection {
private:
    std::shared_ptr<SocketBackend> backend_ {};
    std::optional<NativeHandle> handle_ {};
    ConnectionState state_ { ConnectionState::idle };
    std::size_t bytes_written_ { 0 };

    BackendError state_error_(std::string message) const { return { 0, std::move(message) }; }

public:
    explicit Connection(std::shared_ptr<SocketBackend> backend)
        : backend_ { std::move(backend) }
    {
    }

    std::expected<void, BackendError> connect(const Address& address) {
        if (!backend_ || state_ != ConnectionState::idle)
            return std::unexpected { state_error_("connection is not idle") };
        state_ = ConnectionState::connecting;
        auto result { backend_->connect(address) };
        if (!result) {
            state_ = ConnectionState::failed;
            return std::unexpected { result.error() };
        }
        handle_ = *result;
        state_ = ConnectionState::open;
        return {};
    }

    std::expected<std::size_t, BackendError> write(std::span<const std::byte> bytes) {
        if (!handle_ || (state_ != ConnectionState::open && state_ != ConnectionState::paused))
            return std::unexpected { state_error_("connection is not open") };
        auto result { backend_->write(*handle_, bytes) };
        if (result)
            bytes_written_ += *result;
        return result;
    }

    bool pause() {
        if (!handle_ || state_ != ConnectionState::open || !backend_->pause(*handle_))
            return false;
        state_ = ConnectionState::paused;
        return true;
    }
    bool resume() {
        if (!handle_ || state_ != ConnectionState::paused || !backend_->resume(*handle_))
            return false;
        state_ = ConnectionState::open;
        return true;
    }
    bool shutdown() {
        if (!handle_ || (state_ != ConnectionState::open && state_ != ConnectionState::paused))
            return false;
        state_ = ConnectionState::closing;
        return backend_->shutdown(*handle_);
    }
    void close() {
        if (handle_) {
            backend_->close(*handle_);
            handle_.reset();
        }
        state_ = ConnectionState::closed;
    }
    ~Connection() { close(); }

    ConnectionState state() const { return state_; }
    std::optional<NativeHandle> native_handle() const { return handle_; }
    std::size_t bytes_written() const { return bytes_written_; }
};

}
