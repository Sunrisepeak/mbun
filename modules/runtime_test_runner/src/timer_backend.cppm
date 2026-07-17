// Host timer seam, based on bun test_runner/timers and runtime timer behavior.
// The backend owns no JSC state; an adapter can bind these operations later.
export module mbun.runtime_test_runner.timer_backend;

import std;
export import mbun.runtime_timer;

export namespace mbun::runtime_test_runner {

class TimerBackend {
private:
    mbun::runtime_timer::TimerWheel* wheel_ { nullptr };

public:
    explicit TimerBackend(mbun::runtime_timer::TimerWheel& wheel) noexcept : wheel_ { &wheel } {}

    [[nodiscard]] auto now_ms() const noexcept -> std::uint64_t { return wheel_->now_ms(); }
    [[nodiscard]] auto pending() const noexcept -> std::size_t { return wheel_->pending(); }

    [[nodiscard]] auto set_timeout(std::uint64_t delay_ms,
                                   mbun::runtime_timer::TimerCallback callback)
        -> mbun::runtime_timer::TimerId {
        return wheel_->set_timeout(delay_ms, std::move(callback));
    }

    [[nodiscard]] auto set_interval(std::uint64_t period_ms,
                                    mbun::runtime_timer::TimerCallback callback)
        -> mbun::runtime_timer::TimerId {
        return wheel_->set_interval(period_ms, std::move(callback));
    }

    bool cancel(mbun::runtime_timer::TimerId id) { return wheel_->cancel(id); }

    std::size_t advance_ms(std::uint64_t delta_ms) {
        wheel_->advance_ms(delta_ms);
        return wheel_->run_due();
    }
};

}  // namespace mbun::runtime_test_runner
