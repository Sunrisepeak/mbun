export module mbun.dispatch.executor;

export import mbun.dispatch.queue;
import std;

namespace mbun::dispatch {

export class QueueExecutor {
private:
    TaskQueue queue_{};
    TaskId nextId_{1};
    std::unordered_map<std::uint64_t, TaskHandle> deferred_{};

    CancellationToken ensure_token_(CancellationToken token) {
        return token ? token : CancellationSource::create().token();
    }

public:
    TaskHandle submit(Priority priority, TaskCallback callback, CancellationToken token = {}) {
        auto actualToken{ensure_token_(std::move(token))};
        TaskId id{nextId_++};
        Task task{id, priority, std::move(callback), actualToken};
        auto handle{task.handle()};
        queue_.enqueue(std::make_unique<Task>(std::move(task)));
        return handle;
    }

    TaskHandle post_deferred(std::uint64_t key, Priority priority, TaskCallback callback,
                             CancellationToken token = {}) {
        if (auto found{deferred_.find(key)}; found != deferred_.end()) {
            return found->second;
        }
        auto actualToken{ensure_token_(std::move(token))};
        TaskId id{nextId_++};
        Task task{id, priority, std::move(callback), actualToken, key};
        auto handle{task.handle()};
        deferred_.emplace(key, handle);
        queue_.enqueue(std::make_unique<Task>(std::move(task)));
        return handle;
    }

    bool cancel(const TaskHandle& handle) noexcept {
        if (!handle.valid() || handle.is_cancelled()) return false;
        handle.cancel();
        return true;
    }

    bool unregister_deferred(std::uint64_t key) {
        auto found{deferred_.find(key)};
        if (found == deferred_.end()) return false;
        auto handle{found->second};
        deferred_.erase(found);
        handle.cancel();
        return true;
    }

    bool run_one() {
        auto task{queue_.take()};
        if (!task) return false;
        if (auto key{task->deferred_key()}; key) deferred_.erase(*key);
        bool repeat{task->run()};
        if (repeat && !task->is_cancelled()) {
            if (auto key{task->deferred_key()}; key) deferred_.insert_or_assign(*key, task->handle());
            queue_.enqueue(std::move(task));
        }
        return true;
    }

    std::size_t drain() {
        std::size_t count{0};
        while (run_one()) ++count;
        return count;
    }

    bool empty() const noexcept { return queue_.empty(); }
    std::size_t size() const noexcept { return queue_.size(); }
};

} // namespace mbun::dispatch
