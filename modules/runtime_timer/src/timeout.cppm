export module mbun.runtime_timer.timeout;

import std;
import mbun.runtime_timer.timer_wheel;

export namespace mbun::runtime_timer {

class Timeout {
private:
    TimerWheel* wheel_{nullptr};
    TimerId id_{0};

public:
    Timeout() = default;
    Timeout(TimerWheel& wheel, TimerId id) : wheel_{&wheel}, id_{id} {}

    [[nodiscard]] TimerId id() const noexcept { return id_; }
    bool cancel() {
        return wheel_ != nullptr && wheel_->cancel(id_);
    }
};

inline Timeout set_timeout(TimerWheel& wheel, std::uint64_t delayMs,
                           TimerCallback callback) {
    return Timeout{wheel, wheel.set_timeout(delayMs, std::move(callback))};
}

}  // namespace mbun::runtime_timer
