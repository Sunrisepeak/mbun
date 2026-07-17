// Minimal worker seam. ref: bun threading/ThreadPool.rs and work_pool.rs.
// The executor owns the queue and joins on destruction; richer pools and
// platform-specific scheduling remain outside this foundation member.
export module mbun.event_loop.thread;

import std;
import mbun.event_loop.cancellation;
import mbun.event_loop.queue;
import mbun.event_loop.task;

export namespace mbun::event_loop {

class ThreadExecutor {
private:
    mutable std::mutex mutex_{};
    std::condition_variable condition_{};
    TaskQueue queue_{};
    CancellationSource stop_{};
    std::thread worker_{};

    void run_() {
        for (;;) {
            std::optional<TaskItem> item{};
            {
                std::unique_lock lock{mutex_};
                condition_.wait(lock, [this] {
                    return stop_.is_cancelled() || !queue_.empty();
                });
                if (queue_.empty() && stop_.is_cancelled()) {
                    return;
                }
                item = queue_.pop();
            }
            if (item.has_value()) {
                item->run();
            }
        }
    }

public:
    ThreadExecutor() = default;
    ThreadExecutor(const ThreadExecutor&) = delete;
    ThreadExecutor& operator=(const ThreadExecutor&) = delete;

    ~ThreadExecutor() {
        request_stop();
        join();
    }

    void start() {
        if (!worker_.joinable()) {
            worker_ = std::thread([this] { run_(); });
        }
    }

    void submit(TaskItem item) {
        {
            std::lock_guard lock{mutex_};
            queue_.push(std::move(item));
        }
        condition_.notify_one();
    }

    void request_stop() noexcept {
        {
            // Cancel under the queue mutex: flipping the flag outside it can
            // land between the worker's predicate check and its block,
            // losing the wakeup and hanging join() forever.
            std::lock_guard lock{mutex_};
            stop_.cancel();
        }
        condition_.notify_all();
    }

    void join() noexcept {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] bool running() const noexcept {
        return worker_.joinable() && !stop_.is_cancelled();
    }
};

}  // namespace mbun::event_loop
