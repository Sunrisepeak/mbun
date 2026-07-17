// FIFO task queue. ref: bun event_loop/DeferredTaskQueue.rs and
// threading/unbounded_queue.rs. This is a bounded-storage, single-consumer
// queue; cross-thread ingress is provided by ThreadExecutor, not here.
export module mbun.event_loop.queue;

import std;
import mbun.event_loop.task;

export namespace mbun::event_loop {

template <class T>
class FifoQueue {
private:
    std::deque<T> values_{};

public:
    FifoQueue() = default;
    FifoQueue(const FifoQueue&) = delete;
    FifoQueue& operator=(const FifoQueue&) = delete;
    FifoQueue(FifoQueue&&) noexcept = default;
    FifoQueue& operator=(FifoQueue&&) noexcept = default;

    void push(T value) {
        values_.push_back(std::move(value));
    }

    [[nodiscard]] bool empty() const noexcept {
        return values_.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return values_.size();
    }

    [[nodiscard]] std::optional<T> pop() {
        if (values_.empty()) {
            return std::nullopt;
        }
        T value{std::move(values_.front())};
        values_.pop_front();
        return value;
    }

    void clear() noexcept {
        values_.clear();
    }
};

using TaskQueue = FifoQueue<TaskItem>;

}  // namespace mbun::event_loop
