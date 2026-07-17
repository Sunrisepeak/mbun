export module mbun.runtime_timer.interval;

import std;
import mbun.runtime_timer.timer_wheel;

export namespace mbun::runtime_timer {

class Interval {
private:
    TimerWheel* wheel_{nullptr};
    TimerId id_{0};

public:
    Interval() = default;
    Interval(TimerWheel& wheel, TimerId id) : wheel_{&wheel}, id_{id} {}

    [[nodiscard]] TimerId id() const noexcept { return id_; }
    bool cancel() {
        return wheel_ != nullptr && wheel_->cancel(id_);
    }
};

inline Interval set_interval(TimerWheel& wheel, std::uint64_t periodMs,
                             TimerCallback callback) {
    return Interval{wheel, wheel.set_interval(periodMs, std::move(callback))};
}

}  // namespace mbun::runtime_timer
