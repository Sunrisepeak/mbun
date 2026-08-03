export module mbun.dispatch.task;

export import mbun.dispatch.cancellation;
import std;

namespace mbun::dispatch {

export enum class Priority : std::uint8_t { low = 0, normal = 1, high = 2, critical = 3 };
export using TaskId = std::uint64_t;
export using TaskCallback = std::function<bool()>;

export class TaskHandle {
private:
    TaskId id_{0};
    CancellationToken token_{};
public:
    TaskHandle(TaskId id, CancellationToken token) : id_{id}, token_{std::move(token)} {}
    TaskId id() const noexcept { return id_; }
    bool valid() const noexcept { return id_ != 0; }
    bool is_cancelled() const noexcept { return token_.is_cancelled(); }
    void cancel() const noexcept { token_.cancel(); }
};

export class Task {
private:
    TaskId id_;
    Priority priority_;
    TaskCallback callback_;
    CancellationToken token_;
    std::optional<std::uint64_t> deferredKey_{};

public:
    Task(TaskId id, Priority priority, TaskCallback callback, CancellationToken token,
         std::optional<std::uint64_t> deferredKey = {})
        : id_{id}, priority_{priority}, callback_{std::move(callback)}, token_{std::move(token)},
          deferredKey_{deferredKey} {}
    TaskId id() const noexcept { return id_; }
    Priority priority() const noexcept { return priority_; }
    bool is_cancelled() const noexcept { return token_.is_cancelled(); }
    std::optional<std::uint64_t> deferred_key() const noexcept { return deferredKey_; }
    bool run() { return callback_(); }
    TaskHandle handle() const { return TaskHandle{id_, token_}; }
};

} // namespace mbun::dispatch
