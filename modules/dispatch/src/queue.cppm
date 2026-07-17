export module mbun.dispatch.queue;

export import mbun.dispatch.task;
import std;

namespace mbun::dispatch {

class TaskOrder {
public:
    bool operator()(const std::unique_ptr<Task>& left, const std::unique_ptr<Task>& right) const noexcept {
        if (left->priority() != right->priority()) {
            return left->priority() < right->priority();
        }
        return left->id() > right->id();
    }
};

export class TaskQueue {
private:
    std::priority_queue<std::unique_ptr<Task>, std::vector<std::unique_ptr<Task>>, TaskOrder> tasks_;

public:
    void enqueue(std::unique_ptr<Task> task) { tasks_.push(std::move(task)); }

    std::unique_ptr<Task> take() {
        while (!tasks_.empty()) {
            auto task{std::move(const_cast<std::unique_ptr<Task>&>(tasks_.top()))};
            tasks_.pop();
            if (!task->is_cancelled()) return task;
        }
        return {};
    }

    bool empty() const noexcept { return tasks_.empty(); }
    std::size_t size() const noexcept { return tasks_.size(); }
};

} // namespace mbun::dispatch
