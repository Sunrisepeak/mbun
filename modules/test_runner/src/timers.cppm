// Deterministic timer queue, based on bun-zig/src/test_runner/timers/FakeTimers.zig.
// Host event-loop/JSC callbacks are DEFERRED; this module only owns scheduling state.
export module mbun.test_runner.timers;

import std;

namespace mbun::test_runner {

export using TimerId = std::uint64_t;

export struct Timer {
    TimerId id { 0 };
    std::uint64_t deadline { 0 };
    std::uint64_t interval { 0 };
    bool cancelled { false };
};

export class FakeClock {
private:
    std::uint64_t now_ { 0 };
    TimerId nextId_ { 1 };
    std::vector<Timer> timers_ {};

public:
    [[nodiscard]] std::uint64_t now() const noexcept { return now_; }

    [[nodiscard]] TimerId set_timeout(std::uint64_t delay) {
        const auto id { nextId_++ };
        timers_.push_back(Timer { id, now_ + delay, 0 });
        return id;
    }

    [[nodiscard]] TimerId set_interval(std::uint64_t delay) {
        const auto id { nextId_++ };
        timers_.push_back(Timer { id, now_ + delay, delay });
        return id;
    }

    void clear(TimerId id) noexcept {
        for (auto& timer : timers_) {
            if (timer.id == id) timer.cancelled = true;
        }
    }

    [[nodiscard]] std::vector<Timer> advance_to(std::uint64_t target) {
        if (target < now_) return {};
        now_ = target;
        std::vector<Timer> fired {};
        for (auto& timer : timers_) {
            if (!timer.cancelled && timer.deadline <= now_) {
                fired.push_back(timer);
                if (timer.interval != 0) timer.deadline += timer.interval;
                else timer.cancelled = true;
            }
        }
        std::erase_if(timers_, [](const Timer& timer) { return timer.cancelled; });
        std::ranges::sort(fired, {}, &Timer::id);
        return fired;
    }
};

}  // namespace mbun::test_runner
