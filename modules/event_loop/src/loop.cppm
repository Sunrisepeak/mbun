// Platform-neutral coordinator. It deliberately exposes polling as a seam;
// libuv/epoll/kqueue/IOCP integration is DEFERRED and must implement
// BackendSeam rather than leaking a native handle into this module.
export module mbun.event_loop.loop;

import std;
import mbun.event_loop.backend;
import mbun.event_loop.cancellation;
import mbun.event_loop.queue;
import mbun.event_loop.task;
import mbun.event_loop.timer;

export namespace mbun::event_loop {

class EventLoop {
public:
    using WatchHandler = std::function<void(const BackendEvent&)>;

private:
    TaskQueue tasks_{};
    TimerQueue timers_{};
    BackendSeam backend_{};
    std::unordered_map<std::uint64_t, WatchHandler> watches_{};
    bool stopped_{false};
    std::uint64_t nextTaskId_{1};

public:
    EventLoop() = default;
    explicit EventLoop(BackendSeam backend) : backend_(std::move(backend)) {}

    std::uint64_t post(Task callback, CancellationToken cancellation = {}) {
        const std::uint64_t id{nextTaskId_++};
        tasks_.push(TaskItem{.id = id, .callback = std::move(callback),
                             .cancellation = std::move(cancellation)});
        backend_.wake();
        return id;
    }

    TimerId set_timeout(std::chrono::milliseconds delay, Task callback,
                        std::chrono::steady_clock::time_point now) {
        const TimerId id{timers_.schedule(delay, TimerKind::timeout,
                                          std::move(callback), now)};
        backend_.wake();
        return id;
    }

    TimerId set_interval(std::chrono::milliseconds period, Task callback,
                         std::chrono::steady_clock::time_point now) {
        const TimerId id{timers_.schedule(period, TimerKind::interval,
                                          std::move(callback), now)};
        backend_.wake();
        return id;
    }

    bool clear_timer(TimerId id) {
        return timers_.cancel(id);
    }

    // Route backend events by token (ref: us_loop_run ready_polls dispatch in
    // bun uws_sys/Loop.rs — each ready poll carries its user data back).
    bool add_watch(std::uint64_t token, WatchHandler handler) {
        return watches_.emplace(token, std::move(handler)).second;
    }

    bool remove_watch(std::uint64_t token) {
        return watches_.erase(token) != 0;
    }

    std::size_t run_once(std::chrono::steady_clock::time_point now) {
        if (stopped_) {
            return 0;
        }
        std::size_t ran{timers_.run_due(now)};
        while (auto item{tasks_.pop()}) {
            ran += item->run() ? 1U : 0U;
        }
        std::vector<BackendEvent> events{};
        static_cast<void>(backend_.poll_once(poll_timeout_(now), events));
        for (const auto& event : events) {
            if (const auto watch{watches_.find(event.token)}; watch != watches_.end()) {
                watch->second(event);
            }
        }
        return ran + events.size();
    }

    void stop() noexcept {
        stopped_ = true;
        backend_.wake();
    }

    [[nodiscard]] bool stopped() const noexcept {
        return stopped_;
    }

    [[nodiscard]] std::size_t pending_tasks() const noexcept {
        return tasks_.size();
    }

private:
    // Block until the next timer deadline instead of busy-polling at 0ms.
    // No deadline → negative timeout, i.e. wait until an event or wake()
    // (ref: us_loop_run_bun_tick with a null timespec in bun uws_sys/Loop.rs).
    [[nodiscard]] std::chrono::milliseconds poll_timeout_(
        std::chrono::steady_clock::time_point now) {
        const auto deadline{timers_.next_deadline()};
        if (!deadline) {
            return std::chrono::milliseconds{-1};
        }
        if (*deadline <= now) {
            return std::chrono::milliseconds{0};
        }
        return std::chrono::ceil<std::chrono::milliseconds>(*deadline - now);
    }
};

}  // namespace mbun::event_loop
