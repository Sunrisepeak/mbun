import std;
import mbun.runtime_timer;

namespace {

int checks{0};
int failures{0};

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}

void test_timeout_and_fifo() {
    mbun::runtime_timer::TimerWheel wheel;
    std::vector<int> log;
    wheel.set_timeout(10, [&] { log.push_back(1); });
    wheel.set_timeout(10, [&] { log.push_back(2); });
    wheel.advance_ms(10);
    check(wheel.run_due() == 2, "two due timeouts fire");
    check(log == std::vector<int>{1, 2}, "same deadline preserves FIFO");
}

void test_interval_reschedules() {
    mbun::runtime_timer::TimerWheel wheel;
    int fires{0};
    auto id = wheel.set_interval(5, [&] { ++fires; });
    wheel.advance_ms(5);
    check(wheel.run_due() == 1 && fires == 1, "interval fires once");
    check(wheel.next_deadline_ms() == 10, "interval reschedules from current time");
    wheel.advance_ms(5);
    check(wheel.run_due() == 1 && fires == 2, "interval fires after reschedule");
    check(wheel.cancel(id), "interval can be cancelled");
}

void test_cancellation_seam() {
    mbun::runtime_timer::CancellationSource source;
    auto token = source.token();
    int callbacks{0};
    token.on_cancel([&] { ++callbacks; });
    check(source.cancel(), "first cancellation changes state");
    check(!source.cancel() && token.cancelled(), "cancellation is idempotent");
    check(callbacks == 1, "cancellation callback runs once");
    token.on_cancel([&] { ++callbacks; });
    check(callbacks == 2, "late callback observes cancelled token");
}

void test_self_and_cross_cancellation() {
    mbun::runtime_timer::TimerWheel wheel;
    int selfFires{0};
    mbun::runtime_timer::TimerId self{0};
    self = wheel.set_interval(1, [&] {
        ++selfFires;
        if (selfFires == 2) {
            check(wheel.cancel(self), "interval can cancel itself while firing");
        }
    });
    wheel.advance_ms(1);
    wheel.run_due();
    wheel.advance_ms(1);
    wheel.run_due();
    check(selfFires == 2 && wheel.pending() == 0, "self-cancelled interval is not reinserted");

    std::vector<int> log;
    mbun::runtime_timer::TimerId other{0};
    wheel.set_timeout(1, [&] {
        log.push_back(1);
        check(wheel.cancel(other), "callback can cancel another due timer");
    });
    other = wheel.set_timeout(1, [&] { log.push_back(2); });
    wheel.advance_ms(1);
    wheel.run_due();
    check(log == std::vector<int>{1}, "cross-cancelled timer does not fire");
}

void test_empty_wheel_and_handles() {
    mbun::runtime_timer::TimerWheel wheel;
    check(wheel.pending() == 0 && !wheel.next_deadline_ms().has_value(), "empty wheel has no deadline");
    int timeoutFires{0};
    auto timeout{mbun::runtime_timer::set_timeout(wheel, 2, [&] { ++timeoutFires; })};
    check(timeout.id() > 0 && timeout.cancel(), "timeout handle cancels scheduled timer");
    wheel.advance_ms(2);
    check(wheel.run_due() == 0 && timeoutFires == 0, "cancelled timeout stays silent");

    int intervalFires{0};
    auto interval{mbun::runtime_timer::set_interval(wheel, 2, [&] { ++intervalFires; })};
    wheel.advance_ms(2);
    wheel.run_due();
    check(intervalFires == 1 && interval.cancel(), "interval handle cancels after firing");
}

}  // namespace

int main() {
    test_timeout_and_fifo();
    test_interval_reschedules();
    test_cancellation_seam();
    test_self_and_cross_cancellation();
    test_empty_wheel_and_handles();
    std::println("runtime_timer: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
