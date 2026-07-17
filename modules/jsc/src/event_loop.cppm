// src/event_loop.cppm — module mbun.jsc.event_loop
//
// Pure-logic JS event-loop scheduler for the JSC runtime subsystem: task queue,
// microtask + process.nextTick queues, and a virtual-clock timer wheel
// (setTimeout / setInterval / clearTimeout) backed by an intrusive binary
// min-heap. The scheduling core is deliberately decoupled from JavaScriptCore
// (injectable callbacks + injectable clock) so it unit-tests without a VM; the
// JSC microtask hook (Promise-then jobs) is exercised as a smoke test through
// mbun.compat.jsc. Real IO/network readiness (epoll/kqueue) is out of scope
// here — DEFERRED(S1 → T4.5).
//
// Re-expressed in MC++ from bun's event loop / timer implementation (MIT):
//   - Rust: src/jsc/event_loop.rs (EventLoop::tick / drain_microtasks / queues)
//   - Zig:  src/jsc/event_loop.zig, src/runtime/timer/Timer.zig (the min-heap
//           timer wheel, All::next / drainTimers / update reschedule),
//           src/event_loop/EventLoopTimer.zig (State machine, ms-collapse +
//           epoch tiebreak ordering), src/io/heap.zig (intrusive binary heap).
// Design: docs/design/20260710-jsc-integration.md §3.3 (T3.2).
export module mbun.jsc.event_loop;

import std;

export namespace mbun::jsc::event_loop {

// Type-erased scheduler callback. std::function is used (not move_only_function)
// because it is the LLVM/libc++ ∩ GCC feature set — libc++ 22 does not yet ship
// std::move_only_function; swap it in for cheaper small-capture storage once the
// two toolchains' intersection includes it.
using Task = std::function<void()>;

// Nanoseconds per millisecond — JS timer delays are milliseconds; the virtual
// clock stores nanoseconds so sub-ms ordering can be modelled exactly like bun.
inline constexpr std::uint64_t NS_PER_MS{1'000'000};

// ── FIFO ring queue ───────────────────────────────────────────────────────
// Contiguous ring: O(1) push/pop, amortised zero allocation after warmup
// (grows by doubling, then reuses the buffer). Mirrors bun's Vec-backed task
// queues (src/jsc/event_loop.rs `immediate_tasks`).
template <class T>
class RingQueue {
private:
    std::vector<T> buf_;
    std::size_t head_{0};
    std::size_t count_{0};

    void grow_() {
        std::vector<T> next(buf_.size() * 2);
        for (std::size_t i{0}; i < count_; ++i) {
            next[i] = std::move(buf_[(head_ + i) % buf_.size()]);
        }
        buf_ = std::move(next);
        head_ = 0;
    }

public:
    explicit RingQueue(std::size_t initial = 8) : buf_(initial < 1 ? 1 : initial) {}

    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }
    [[nodiscard]] bool empty() const noexcept {
        return count_ == 0;
    }

    void push(T value) {
        if (count_ == buf_.size()) {
            grow_();
        }
        buf_[(head_ + count_) % buf_.size()] = std::move(value);
        ++count_;
    }

    [[nodiscard]] T pop() {
        T value{std::move(buf_[head_])};
        buf_[head_] = T{};  // release captured state promptly
        head_ = (head_ + 1) % buf_.size();
        --count_;
        return value;
    }

    void clear() noexcept {
        while (count_ > 0) {
            static_cast<void>(pop());
        }
    }
};

// ── Timer model ─────────────────────────────────────────────────────────────
// ref: bun src/event_loop/EventLoopTimer.zig `State`
enum class TimerState : std::uint8_t {
    active,     // scheduled, present in the heap
    fired,      // popped from the heap, callback about to run / running
    cancelled,  // clearTimeout/clearInterval before it ran
};

enum class TimerKind : std::uint8_t {
    timeout,
    interval,
};

struct Timer {
    static constexpr std::size_t NPOS{static_cast<std::size_t>(-1)};

    std::uint64_t nextNs{0};      // absolute virtual fire time (ns)
    std::uint64_t epoch{0};       // insertion-order tiebreak for equal deadlines
    std::uint64_t intervalNs{0};  // repeat period (interval only)
    std::int64_t id{0};
    TimerKind kind{TimerKind::timeout};
    TimerState state{TimerState::active};
    std::size_t heapIndex{NPOS};  // position in the heap while active
    Task callback;
};

// ── EventLoop ───────────────────────────────────────────────────────────────
class EventLoop {
private:
    // Macrotask queue and the two microtask tiers. process.nextTick has strict
    // priority over the Promise/queueMicrotask microtask queue, re-checked after
    // every microtask (Node/bun semantics). ref: src/jsc/event_loop.rs tick().
    RingQueue<Task> tasks_{};
    RingQueue<Task> microtasks_{};
    RingQueue<Task> nextTick_{};

    // Timer storage: node-stable map keyed by id (addresses survive rehash, so
    // heap pointers stay valid) + an intrusive binary min-heap of live timers.
    std::unordered_map<std::int64_t, Timer> timers_{};
    std::vector<Timer*> heap_{};

    std::uint64_t nowNs_{0};      // injectable virtual clock
    std::uint64_t nextEpoch_{1};  // monotonic insertion counter
    std::int64_t nextId_{1};      // monotonic timer id (Node ids start at 1)

    // Heap ordering: collapse to millisecond granularity, then break ties by
    // insertion epoch so equal-deadline timers fire FIFO.
    // ref: src/event_loop/EventLoopTimer.zig `less`.
    static bool earlier_(const Timer* a, const Timer* b) noexcept {
        const std::uint64_t aMs{a->nextNs / NS_PER_MS};
        const std::uint64_t bMs{b->nextNs / NS_PER_MS};
        if (aMs != bMs) {
            return aMs < bMs;
        }
        return a->epoch < b->epoch;
    }

    void heap_swap_(std::size_t i, std::size_t j) noexcept {
        std::swap(heap_[i], heap_[j]);
        heap_[i]->heapIndex = i;
        heap_[j]->heapIndex = j;
    }

    void sift_up_(std::size_t i) noexcept {
        while (i > 0) {
            const std::size_t parent{(i - 1) / 2};
            if (!earlier_(heap_[i], heap_[parent])) {
                break;
            }
            heap_swap_(i, parent);
            i = parent;
        }
    }

    void sift_down_(std::size_t i) noexcept {
        const std::size_t n{heap_.size()};
        for (;;) {
            const std::size_t left{2 * i + 1};
            const std::size_t right{2 * i + 2};
            std::size_t smallest{i};
            if (left < n && earlier_(heap_[left], heap_[smallest])) {
                smallest = left;
            }
            if (right < n && earlier_(heap_[right], heap_[smallest])) {
                smallest = right;
            }
            if (smallest == i) {
                break;
            }
            heap_swap_(i, smallest);
            i = smallest;
        }
    }

    void heap_insert_(Timer* t) {
        t->heapIndex = heap_.size();
        heap_.push_back(t);
        sift_up_(t->heapIndex);
    }

    // ref: src/io/heap.zig `remove` — replace with last element, then re-heapify
    // both directions from the vacated slot.
    void heap_remove_(std::size_t i) noexcept {
        const std::size_t last{heap_.size() - 1};
        if (i != last) {
            heap_swap_(i, last);
        }
        heap_.back()->heapIndex = Timer::NPOS;
        heap_.pop_back();
        if (i < heap_.size()) {
            sift_down_(i);
            sift_up_(i);
        }
    }

    [[nodiscard]] Timer* heap_min_() const noexcept {
        return heap_.empty() ? nullptr : heap_.front();
    }

    std::int64_t schedule_(std::uint64_t delayMs, TimerKind kind, Task callback) {
        const std::int64_t id{nextId_++};
        Timer timer{};
        timer.id = id;
        timer.kind = kind;
        timer.intervalNs = kind == TimerKind::interval ? delayMs * NS_PER_MS : 0;
        timer.nextNs = nowNs_ + delayMs * NS_PER_MS;
        timer.epoch = nextEpoch_++;
        timer.state = TimerState::active;
        timer.callback = std::move(callback);
        auto [it, _] = timers_.emplace(id, std::move(timer));
        heap_insert_(&it->second);
        return id;
    }

public:
    EventLoop() = default;
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    // ── task / microtask enqueue ────────────────────────────────────────────
    void enqueue_task(Task task) {
        tasks_.push(std::move(task));
    }
    void enqueue_microtask(Task task) {
        microtasks_.push(std::move(task));
    }
    void enqueue_next_tick(Task task) {
        nextTick_.push(std::move(task));
    }

    [[nodiscard]] std::size_t pending_tasks() const noexcept {
        return tasks_.size();
    }
    [[nodiscard]] std::size_t pending_microtasks() const noexcept {
        return microtasks_.size() + nextTick_.size();
    }
    [[nodiscard]] std::size_t pending_timers() const noexcept {
        return heap_.size();
    }
    [[nodiscard]] bool has_pending() const noexcept {
        return !tasks_.empty() || pending_microtasks() > 0 || !heap_.empty();
    }

    // Drain process.nextTick fully, then one microtask, re-checking nextTick
    // after each — nextTick keeps strict priority as callbacks enqueue more
    // work. ref: src/jsc/event_loop.rs drain_microtasks / Node processTicks.
    void drain_microtasks() {
        while (!nextTick_.empty() || !microtasks_.empty()) {
            while (!nextTick_.empty()) {
                Task t{nextTick_.pop()};
                t();
            }
            if (!microtasks_.empty()) {
                Task t{microtasks_.pop()};
                t();
            }
        }
    }

    // Run one macrotask without draining microtasks (bun's tick_tasks_only).
    bool run_next_task() {
        if (tasks_.empty()) {
            return false;
        }
        Task t{tasks_.pop()};
        t();
        return true;
    }

    // One event-loop tick: run a single macrotask, then drain all microtasks.
    // ref: src/jsc/event_loop.rs tick().
    bool tick() {
        const bool ran{run_next_task()};
        drain_microtasks();
        return ran;
    }

    // ── virtual clock ───────────────────────────────────────────────────────
    [[nodiscard]] std::uint64_t now_ns() const noexcept {
        return nowNs_;
    }
    [[nodiscard]] std::uint64_t now_ms() const noexcept {
        return nowNs_ / NS_PER_MS;
    }
    void advance_ms(std::uint64_t deltaMs) noexcept {
        nowNs_ += deltaMs * NS_PER_MS;
    }
    void advance_ns(std::uint64_t deltaNs) noexcept {
        nowNs_ += deltaNs;
    }

    // ── timers ──────────────────────────────────────────────────────────────
    std::int64_t set_timeout(std::uint64_t delayMs, Task callback) {
        return schedule_(delayMs, TimerKind::timeout, std::move(callback));
    }
    std::int64_t set_interval(std::uint64_t intervalMs, Task callback) {
        return schedule_(intervalMs, TimerKind::interval, std::move(callback));
    }

    // clearTimeout / clearInterval. Handles the in-callback case (a timer that
    // has been popped for firing, or a timer clearing itself). Returns whether
    // a live timer with this id was removed.
    bool clear_timer(std::int64_t id) {
        auto it{timers_.find(id)};
        if (it == timers_.end()) {
            return false;
        }
        Timer& t{it->second};
        if (t.state == TimerState::active && t.heapIndex != Timer::NPOS) {
            heap_remove_(t.heapIndex);
        }
        t.state = TimerState::cancelled;
        timers_.erase(it);
        return true;
    }

    // Absolute virtual fire time (ns) of the earliest scheduled timer.
    [[nodiscard]] std::optional<std::uint64_t> next_timer_deadline_ns() const noexcept {
        const Timer* min{heap_min_()};
        return min == nullptr ? std::nullopt : std::optional{min->nextNs};
    }

    [[nodiscard]] bool has_due_timer() const noexcept {
        const Timer* min{heap_min_()};
        return min != nullptr && min->nextNs / NS_PER_MS <= nowNs_ / NS_PER_MS;
    }

    // Fire every timer whose deadline is <= now, in (deadline, epoch) order.
    // Intervals reschedule to now + period (bun captures the reschedule instant
    // before the callback runs: src/runtime/timer/TimerObjectInternals.zig).
    // Timers scheduled *during* this drain run on the next call (run_until_idle
    // loops), which also bounds a same-instant setInterval from spinning.
    std::size_t run_due_timers() {
        const std::uint64_t now{nowNs_};
        const std::uint64_t nowMs{now / NS_PER_MS};

        // Snapshot the currently-due ids in fire order (removing from the heap).
        std::vector<std::int64_t> batch;
        while (true) {
            Timer* min{heap_min_()};
            if (min == nullptr || min->nextNs / NS_PER_MS > nowMs) {
                break;
            }
            min->state = TimerState::fired;
            heap_remove_(min->heapIndex);
            batch.push_back(min->id);
        }

        std::size_t fired{0};
        for (const std::int64_t id : batch) {
            auto it{timers_.find(id)};
            if (it == timers_.end() || it->second.state == TimerState::cancelled) {
                continue;  // cleared by an earlier callback in this batch
            }
            // Move the callback to a local: a self-clearing interval must not
            // destroy the std::function while it is executing.
            Task cb{std::move(it->second.callback)};
            cb();
            ++fired;

            it = timers_.find(id);  // node may have been erased by a self-clear
            if (it == timers_.end()) {
                continue;
            }
            Timer& t{it->second};
            if (t.kind == TimerKind::interval && t.state != TimerState::cancelled) {
                t.callback = std::move(cb);
                t.nextNs = now + t.intervalNs;
                t.epoch = nextEpoch_++;
                t.state = TimerState::active;
                heap_insert_(&t);
            } else {
                timers_.erase(it);
            }
        }
        return fired;
    }

    // Drive the loop to quiescence over virtual time: run ready macrotasks
    // (draining microtasks after each), fire due timers, then advance the clock
    // to the next timer deadline and repeat. Bounded by maxSteps so a pinned
    // setInterval cannot hang a test. Returns the number of steps consumed.
    std::size_t run_until_idle(std::size_t maxSteps = 1'000'000) {
        std::size_t steps{0};
        drain_microtasks();
        while (steps < maxSteps) {
            ++steps;
            if (run_next_task()) {
                drain_microtasks();
                continue;
            }
            if (has_due_timer()) {
                run_due_timers();
                drain_microtasks();
                continue;
            }
            if (auto deadline{next_timer_deadline_ns()}) {
                nowNs_ = *deadline;
                continue;
            }
            break;
        }
        return steps;
    }
};

}  // namespace mbun::jsc::event_loop
