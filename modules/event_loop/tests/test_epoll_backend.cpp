// Real-fd integration vectors for mbun.event_loop.epoll_backend. Scenarios
// mirror bun uws_sys/Loop.rs semantics: ready-poll token dispatch, timer-aware
// tick timeout, and us_wakeup_loop interrupting a blocked wait.
#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
#endif

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

#if !defined(_WIN32)

struct PipePair {
    int readFd{-1};
    int writeFd{-1};

    PipePair() {
        int fds[2]{-1, -1};
        if (::pipe(fds) == 0) {
            readFd = fds[0];
            writeFd = fds[1];
        }
    }
    PipePair(const PipePair&) = delete;
    PipePair& operator=(const PipePair&) = delete;
    ~PipePair() {
        if (readFd >= 0) ::close(readFd);
        if (writeFd >= 0) ::close(writeFd);
    }

    void write_byte() const {
        const char byte{'x'};
        static_cast<void>(::write(writeFd, &byte, 1));
    }
};

void test_pipe_readiness_maps_token() {
    HostReadinessBackend backend{};
    check(backend.status() == BackendStatus::ready, "epoll backend is ready on linux");
    PipePair pipe{};
    check(pipe.readFd >= 0, "pipe created");
    check(backend.add(pipe.readFd, PollInterest::read, 42), "pipe read end registered");
    check(backend.poll(std::chrono::milliseconds{0}).empty(),
          "no readiness before write");
    pipe.write_byte();
    const auto events{backend.poll(std::chrono::milliseconds{1000})};
    check(events.size() == 1 && events.front().token == 42,
          "write end wakes epoll with registered token");
    check(backend.remove(pipe.readFd), "pipe read end deregistered");
    check(backend.poll(std::chrono::milliseconds{0}).empty(),
          "removed fd no longer reports readiness");
}

void test_socketpair_modify_and_dispatch() {
    HostReadinessBackend backend{};
    int fds[2]{-1, -1};
    check(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair created");
    // Writable-only interest first: an idle stream socket is writable.
    check(backend.add(fds[0], PollInterest::write, 7), "socket registered for write");
    auto events{backend.poll(std::chrono::milliseconds{1000})};
    check(events.size() == 1 && events.front().token == 7, "idle socket is writable");
    // Swap to read interest and re-bind the token: silence until peer writes.
    check(backend.modify(fds[0], PollInterest::read, 8), "interest modified to read");
    check(backend.poll(std::chrono::milliseconds{0}).empty(),
          "read interest quiet before peer writes");
    const char byte{'y'};
    static_cast<void>(::write(fds[1], &byte, 1));
    events = backend.poll(std::chrono::milliseconds{1000});
    check(events.size() == 1 && events.front().token == 8,
          "peer write reported under re-bound token");
    ::close(fds[0]);
    ::close(fds[1]);
}

void test_loop_dispatches_fd_events_to_watch() {
    HostReadinessBackend backend{};
    PipePair pipe{};
    check(backend.add(pipe.readFd, PollInterest::read, 21), "loop pipe registered");
    EventLoop loop{backend.seam()};
    std::vector<std::uint64_t> seen{};
    check(loop.add_watch(21, [&](const BackendEvent& event) {
              seen.push_back(event.token);
          }),
          "watch added");
    check(!loop.add_watch(21, [](const BackendEvent&) {}), "duplicate watch rejected");
    pipe.write_byte();
    const auto ran{loop.run_once(std::chrono::steady_clock::now())};
    check(ran == 1 && seen == std::vector<std::uint64_t>{21},
          "run_once routes backend event token to its handler");
    check(loop.remove_watch(21), "watch removed");
    check(!loop.remove_watch(21), "watch removal is idempotent");
}

void test_timer_deadline_bounds_poll_wait() {
    HostReadinessBackend backend{};
    PipePair pipe{};  // Registered but never written: poll must not block on it.
    check(backend.add(pipe.readFd, PollInterest::read, 5), "quiet pipe registered");
    EventLoop loop{backend.seam()};
    int fired{0};
    auto now{std::chrono::steady_clock::now()};
    static_cast<void>(loop.set_timeout(std::chrono::milliseconds{30},
                                       [&] { ++fired; }, now));
    // set_timeout wakes the backend (pending wakeup ⇒ non-blocking poll, per
    // Loop.rs pending_wakeups); drain it before measuring the blocking tick.
    static_cast<void>(loop.run_once(now));
    const auto start{std::chrono::steady_clock::now()};
    static_cast<void>(loop.run_once(start));  // Blocks ≈ remaining deadline.
    const auto blocked{std::chrono::steady_clock::now() - start};
    check(blocked >= std::chrono::milliseconds{10},
          "poll waits toward the timer deadline instead of busy-polling");
    check(blocked < std::chrono::seconds{5},
          "timer deadline caps the blocking poll");
    // post() keeps the final tick non-blocking once no timers remain.
    loop.post([] {});
    static_cast<void>(loop.run_once(std::chrono::steady_clock::now()));
    check(fired == 1, "timer fires once its deadline passes");
}

void test_wake_interrupts_blocked_poll() {
    HostReadinessBackend backend{};
    std::thread waker{[&backend] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        backend.wake();
    }};
    const auto start{std::chrono::steady_clock::now()};
    const auto events{backend.poll(std::chrono::milliseconds{-1})};
    const auto blocked{std::chrono::steady_clock::now() - start};
    waker.join();
    check(events.empty(), "wakeup is consumed internally, not surfaced as an event");
    check(blocked < std::chrono::seconds{5}, "wake() interrupts an unbounded poll");
    // Loop.rs: pending_wakeups non-zero before the wait → return immediately.
    backend.wake();
    const auto restart{std::chrono::steady_clock::now()};
    static_cast<void>(backend.poll(std::chrono::milliseconds{-1}));
    check(std::chrono::steady_clock::now() - restart < std::chrono::seconds{1},
          "pending wakeup makes the next poll non-blocking");
}

void test_post_from_another_thread_unblocks_run_once() {
    HostReadinessBackend backend{};
    EventLoop loop{backend.seam()};
    std::atomic_bool posted{false};
    std::thread producer{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        loop.post([&posted] { posted.store(true); });
    }};
    const auto start{std::chrono::steady_clock::now()};
    static_cast<void>(loop.run_once(start));  // No timers: blocks until wake().
    const auto blocked{std::chrono::steady_clock::now() - start};
    producer.join();
    static_cast<void>(loop.run_once(std::chrono::steady_clock::now()));
    check(blocked < std::chrono::seconds{5}, "post() wakes a blocked run_once");
    check(posted.load(), "posted task runs after wakeup");
}

#else  // !__linux__

void test_deferred_stub() {
    HostReadinessBackend backend{};
    check(backend.status() == BackendStatus::deferred,
          "non-linux epoll backend reports deferred");
    check(!backend.add(0, PollInterest::read, 1), "deferred add refuses registration");
    check(backend.poll(std::chrono::milliseconds{0}).empty(),
          "deferred poll yields no events");
    EventLoop loop{backend.seam()};
    std::vector<BackendEvent> events{};
    BackendSeam seam{backend.seam()};
    check(seam.poll_once(std::chrono::milliseconds{0}, events) == BackendStatus::deferred,
          "deferred seam reports deferred status");
}

#endif

}  // namespace

int main() {
#if !defined(_WIN32)
    test_pipe_readiness_maps_token();
    test_socketpair_modify_and_dispatch();
    test_loop_dispatches_fd_events_to_watch();
    test_timer_deadline_bounds_poll_wait();
    test_wake_interrupts_blocked_poll();
    test_post_from_another_thread_unblocks_run_once();
#else
    test_deferred_stub();
#endif
    std::println("event_loop epoll backend: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
