// Pure-logic vectors for mbun.event_loop. The ordering/cancellation cases are
// derived from bun event_loop/EventLoopTimer and threading work-pool seams.
import std;
import mbun.event_loop;

namespace {

using namespace mbun::event_loop;
int checks{0};
int failures{0};

void check(bool condition, std::string_view name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL {}", name);
    }
}

void test_cancellation() {
    CancellationSource source{};
    auto token{source.token()};
    check(!token.is_cancelled(), "token starts live");
    check(source.cancel(), "first cancel reports transition");
    check(!source.cancel(), "second cancel is idempotent");
    check(token.is_cancelled(), "token observes cancellation");
}

void test_queue_and_cancelled_task() {
    TaskQueue queue{};
    std::vector<int> order{};
    CancellationSource source{};
    queue.push(TaskItem{.id = 1, .callback = [&] { order.push_back(1); }});
    queue.push(TaskItem{.id = 2,
                        .callback = [&] { order.push_back(2); },
                        .cancellation = source.token()});
    source.cancel();
    while (auto item{queue.pop()}) {
        static_cast<void>(item->run());
    }
    check(order == std::vector<int>{1}, "queue FIFO skips cancelled task");
}

void test_timer_fifo_cancel_and_interval() {
    TimerQueue timers{};
    const auto start{std::chrono::steady_clock::time_point{}};
    std::vector<int> order{};
    const auto first{timers.schedule(std::chrono::milliseconds{10}, TimerKind::timeout,
                                     [&] { order.push_back(1); }, start)};
    static_cast<void>(timers.schedule(std::chrono::milliseconds{10}, TimerKind::timeout,
                                       [&] { order.push_back(2); }, start));
    const auto interval{timers.schedule(std::chrono::milliseconds{10}, TimerKind::interval,
                                         [&] { order.push_back(3); }, start)};
    check(timers.cancel(first), "timer cancel accepted");
    check(!timers.cancel(first), "timer cancel is idempotent");
    check(timers.run_due(start + std::chrono::milliseconds{10}) == 2,
          "due timer count excludes cancelled timeout");
    check(order == std::vector<int>{2, 3}, "equal timers retain insertion order");
    check(timers.run_due(start + std::chrono::milliseconds{20}) == 1,
          "interval reschedules");
    check(timers.cancel(interval), "interval can be cleared");
}

void test_backend_and_loop() {
    int wakes{0};
    BackendSeam backend{
        [](std::chrono::milliseconds) { return std::vector<BackendEvent>{{7}}; },
        [&] { ++wakes; }};
    EventLoop loop{std::move(backend)};
    int calls{0};
    loop.post([&] { ++calls; });
    check(wakes == 1, "posting wakes backend");
    const auto ran{loop.run_once(std::chrono::steady_clock::time_point{})};
    check(calls == 1 && ran == 2, "loop drains tasks and backend events");
    loop.stop();
    check(loop.run_once(std::chrono::steady_clock::time_point{}) == 0,
          "stopped loop is inert");
}

void test_thread_executor() {
    ThreadExecutor executor{};
    std::atomic_int calls{0};
    executor.start();
    executor.submit(TaskItem{.id = 1, .callback = [&] { ++calls; }});
    for (int i{0}; i < 100 && calls.load() == 0; ++i) {
        std::this_thread::yield();
    }
    executor.request_stop();
    executor.join();
    check(calls == 1, "worker executes submitted task");
}

}  // namespace

int main() {
    test_cancellation();
    test_queue_and_cancelled_task();
    test_timer_fifo_cancel_and_interval();
    test_backend_and_loop();
    test_thread_executor();
    std::println("event_loop: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
