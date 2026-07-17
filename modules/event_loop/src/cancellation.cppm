// Cancellation primitives, re-expressed from bun threading cancellation call
// sites as a small shared state. The state is intentionally independent of
// JSC, libuv and platform wait handles.
export module mbun.event_loop.cancellation;

import std;

export namespace mbun::event_loop {

class CancellationToken {
private:
    std::shared_ptr<const std::atomic_bool> state_{};

    explicit CancellationToken(std::shared_ptr<const std::atomic_bool> state)
        : state_(std::move(state)) {}

    friend class CancellationSource;

public:
    CancellationToken() = default;

    [[nodiscard]] bool is_cancelled() const noexcept {
        return state_ != nullptr && state_->load(std::memory_order_acquire);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return is_cancelled();
    }
};

class CancellationSource {
private:
    std::shared_ptr<std::atomic_bool> state_{
        std::make_shared<std::atomic_bool>(false)};

public:
    CancellationSource() = default;

    [[nodiscard]] CancellationToken token() const noexcept {
        return CancellationToken{state_};
    }

    bool cancel() noexcept {
        return !state_->exchange(true, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return state_->load(std::memory_order_acquire);
    }
};

}  // namespace mbun::event_loop
