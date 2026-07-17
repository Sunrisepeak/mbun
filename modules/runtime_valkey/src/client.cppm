export module mbun.runtime_valkey.client;

import std;
import mbun.runtime_valkey.backend;
import mbun.runtime_valkey.command;
import mbun.runtime_valkey.result;

export namespace mbun::runtime_valkey {

class Client {
public:
    explicit Client(Backend backend = {}) : backend_(std::move(backend)) {}

    [[nodiscard]] auto status() const noexcept -> ConnectionStatus { return status_; }
    [[nodiscard]] auto pending_count() const noexcept -> std::size_t { return queue_.size(); }
    auto connect() -> bool {
        status_ = ConnectionStatus::connecting;
        if (!backend_.connect || !backend_.connect()) { status_ = ConnectionStatus::disconnected; return false; }
        status_ = ConnectionStatus::connected;
        return flush_();
    }
    auto disconnect() -> void { if (backend_.close) backend_.close(); status_ = ConnectionStatus::disconnected; }
    [[nodiscard]] auto send(Command command) -> std::expected<std::string, RedisError> {
        command = prepare(std::move(command));
        auto serialized = command.serialize();
        if (status_ != ConnectionStatus::connected || !backend_.write) {
            queue_.push_back(OfflineEntry { serialized, command.flags });
            return serialized;
        }
        if (!backend_.write(serialized)) return std::unexpected(RedisError::invalid_response);
        return serialized;
    }

private:
    auto flush_() -> bool {
        if (!backend_.write) return queue_.empty();
        for (const auto& entry : queue_) if (!backend_.write(entry.serialized_data)) return false;
        queue_.clear();
        return true;
    }
    Backend backend_ {};
    ConnectionStatus status_ { ConnectionStatus::disconnected };
    std::deque<OfflineEntry> queue_ {};
};

}
