export module mbun.dispatch.cancellation;

import std;

namespace mbun::dispatch {

class CancellationState {
public:
    std::atomic_bool cancelled{false};
};

export class CancellationToken {
private:
    std::shared_ptr<CancellationState> state_{};
    explicit CancellationToken(std::shared_ptr<CancellationState> state) : state_{std::move(state)} {}
    friend class CancellationSource;

public:
    CancellationToken() = default;

    bool is_cancelled() const noexcept {
        return state_ != nullptr && state_->cancelled.load(std::memory_order_acquire);
    }

    void cancel() const noexcept {
        if (state_ != nullptr) state_->cancelled.store(true, std::memory_order_release);
    }

    explicit operator bool() const noexcept { return state_ != nullptr; }
};

export class CancellationSource {
private:
    std::shared_ptr<CancellationState> state_;
    explicit CancellationSource(std::shared_ptr<CancellationState> state) : state_{std::move(state)} {}

public:
    static CancellationSource create() {
        return CancellationSource{std::make_shared<CancellationState>()};
    }

    CancellationToken token() const { return CancellationToken{state_}; }
    void cancel() const noexcept { state_->cancelled.store(true, std::memory_order_release); }
    bool is_cancelled() const noexcept { return token().is_cancelled(); }
};

} // namespace mbun::dispatch
