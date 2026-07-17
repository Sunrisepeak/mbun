// Deadline scheduler. ref: bun event_loop/EventLoopTimer and io/heap: equal
// millisecond deadlines retain insertion order; cancellation is lazy and
// repeating timers are reinserted from the dispatching deadline.
export module mbun.event_loop.timer;

import std;

export namespace mbun::event_loop {

using TimerId = std::uint64_t;

enum class TimerKind : std::uint8_t { timeout, interval };

struct TimerItem {
    TimerId id{0};
    std::chrono::steady_clock::time_point deadline{};
    std::chrono::milliseconds period{0};
    TimerKind kind{TimerKind::timeout};
    std::uint64_t sequence{0};
    std::function<void()> callback{};
};

struct TimerEarlier {
    bool operator()(const TimerItem& left, const TimerItem& right) const noexcept {
        if (left.deadline != right.deadline) {
            return left.deadline > right.deadline;
        }
        return left.sequence > right.sequence;
    }
};

class TimerQueue {
private:
    std::priority_queue<TimerItem, std::vector<TimerItem>, TimerEarlier> heap_{};
    std::unordered_set<TimerId> cancelled_{};
    TimerId nextId_{1};
    std::uint64_t nextSequence_{1};

public:
    TimerQueue() = default;

    TimerId schedule(std::chrono::milliseconds delay, TimerKind kind,
                     std::function<void()> callback,
                     std::chrono::steady_clock::time_point now) {
        const TimerId id{nextId_++};
        TimerItem item{.id = id,
                       .deadline = now + delay,
                       .period = kind == TimerKind::interval ? delay
                                                              : std::chrono::milliseconds{0},
                       .kind = kind,
                       .sequence = nextSequence_++,
                       .callback = std::move(callback)};
        heap_.push(std::move(item));
        return id;
    }

    bool cancel(TimerId id) {
        return cancelled_.insert(id).second;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return heap_.size() - std::min(heap_.size(), cancelled_.size());
    }

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next_deadline() {
        discard_cancelled_();
        if (heap_.empty()) {
            return std::nullopt;
        }
        return heap_.top().deadline;
    }

    std::size_t run_due(std::chrono::steady_clock::time_point now) {
        std::size_t ran{0};
        discard_cancelled_();
        while (!heap_.empty() && heap_.top().deadline <= now) {
            TimerItem item{std::move(const_cast<TimerItem&>(heap_.top()))};
            heap_.pop();
            if (cancelled_.erase(item.id) != 0) {
                discard_cancelled_();
                continue;
            }
            if (item.callback) {
                item.callback();
                ++ran;
            }
            if (item.kind == TimerKind::interval && !cancelled_.contains(item.id)) {
                item.deadline += item.period;
                item.sequence = nextSequence_++;
                heap_.push(std::move(item));
            }
            discard_cancelled_();
        }
        return ran;
    }

private:
    void discard_cancelled_() {
        while (!heap_.empty() && cancelled_.contains(heap_.top().id)) {
            cancelled_.erase(heap_.top().id);
            heap_.pop();
        }
    }
};

}  // namespace mbun::event_loop
