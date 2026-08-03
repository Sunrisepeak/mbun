// Task value types. ref: bun event_loop/{AnyTask,DeferredTaskQueue}.rs and
// threading/work_pool.rs; ownership and execution are expressed with standard
// library callables so the seam can later be adapted to JSC tasks.
export module mbun.event_loop.task;

import std;
import mbun.event_loop.cancellation;

export namespace mbun::event_loop {

using Task = std::function<void()>;

enum class TaskState : std::uint8_t { pending, running, completed, cancelled };

struct TaskItem {
    std::uint64_t id{0};
    Task callback{};
    CancellationToken cancellation{};
    TaskState state{TaskState::pending};

    [[nodiscard]] bool is_cancelled() const noexcept {
        return state == TaskState::cancelled || cancellation.is_cancelled();
    }

    bool run() {
        if (is_cancelled() || !callback) {
            state = TaskState::cancelled;
            return false;
        }
        state = TaskState::running;
        callback();
        state = TaskState::completed;
        return true;
    }
};

}  // namespace mbun::event_loop
