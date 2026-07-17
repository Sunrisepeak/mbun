export module mbun.runtime_timer.cancellation;

import std;

export namespace mbun::runtime_timer {

using TimerCallback = std::function<void()>;

class CancellationState {
public:
    bool cancelled{false};
    std::vector<TimerCallback> callbacks{};
};

class CancellationToken {
private:
    std::shared_ptr<CancellationState> state_{};

    explicit CancellationToken(std::shared_ptr<CancellationState> state)
        : state_{std::move(state)} {}

    friend class CancellationSource;

public:
    CancellationToken() = default;

    [[nodiscard]] bool cancelled() const noexcept {
        return state_ != nullptr && state_->cancelled;
    }

    // Bun's timer cancellation is owner-loop based. This callback seam keeps
    // cancellation observable without pulling JSC, libuv, or a mutex into the
    // pure timer member; cross-thread delivery remains a later adapter.
    void on_cancel(TimerCallback callback) const {
        if (state_ == nullptr) {
            return;
        }
        if (state_->cancelled) {
            callback();
            return;
        }
        state_->callbacks.push_back(std::move(callback));
    }
};

class CancellationSource {
private:
    std::shared_ptr<CancellationState> state_{std::make_shared<CancellationState>()};

public:
    [[nodiscard]] CancellationToken token() const {
        return CancellationToken{state_};
    }

    // Returns true only for the transition from live to cancelled.
    bool cancel() {
        if (state_->cancelled) {
            return false;
        }
        state_->cancelled = true;
        auto callbacks{std::move(state_->callbacks)};
        for (auto& callback : callbacks) {
            callback();
        }
        return true;
    }
};

}  // namespace mbun::runtime_timer
