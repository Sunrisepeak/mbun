export module mbun.runtime_timer.timer_wheel;

import std;

export namespace mbun::runtime_timer {

using TimerId = std::int64_t;
using TimerCallback = std::function<void()>;

inline constexpr std::uint64_t NS_PER_MS{1'000'000};

enum class TimerKind : std::uint8_t {
    timeout,
    interval,
};

class TimerWheel {
private:
    struct TimerRecord {
        static constexpr std::size_t NPOS{static_cast<std::size_t>(-1)};

        TimerId id{0};
        std::uint64_t deadlineNs{0};
        std::uint64_t periodNs{0};
        std::uint64_t epoch{0};
        std::size_t heapIndex{NPOS};
        TimerKind kind{TimerKind::timeout};
        TimerCallback callback{};
    };

    std::unordered_map<TimerId, TimerRecord> records_{};
    std::vector<TimerRecord*> heap_{};
    std::uint64_t nowNs_{0};
    std::uint64_t nextEpoch_{1};
    TimerId nextId_{1};

    [[nodiscard]] static bool earlier_(const TimerRecord* left,
                                       const TimerRecord* right) noexcept {
        // Bun intentionally collapses host timer deadlines to milliseconds;
        // epoch is the stable FIFO tie-break for timers in one millisecond.
        const auto leftMs{left->deadlineNs / NS_PER_MS};
        const auto rightMs{right->deadlineNs / NS_PER_MS};
        return leftMs < rightMs || (leftMs == rightMs && left->epoch < right->epoch);
    }

    void swap_heap_(std::size_t left, std::size_t right) noexcept {
        std::swap(heap_[left], heap_[right]);
        heap_[left]->heapIndex = left;
        heap_[right]->heapIndex = right;
    }

    void sift_up_(std::size_t index) noexcept {
        while (index != 0) {
            const auto parent{(index - 1) / 2};
            if (!earlier_(heap_[index], heap_[parent])) {
                break;
            }
            swap_heap_(index, parent);
            index = parent;
        }
    }

    void sift_down_(std::size_t index) noexcept {
        while (true) {
            const auto left{index * 2 + 1};
            const auto right{left + 1};
            auto smallest{index};
            if (left < heap_.size() && earlier_(heap_[left], heap_[smallest])) {
                smallest = left;
            }
            if (right < heap_.size() && earlier_(heap_[right], heap_[smallest])) {
                smallest = right;
            }
            if (smallest == index) {
                return;
            }
            swap_heap_(index, smallest);
            index = smallest;
        }
    }

    void insert_(TimerRecord* record) {
        record->heapIndex = heap_.size();
        heap_.push_back(record);
        sift_up_(record->heapIndex);
    }

    void remove_(std::size_t index) noexcept {
        const auto last{heap_.size() - 1};
        if (index != last) {
            swap_heap_(index, last);
        }
        heap_.back()->heapIndex = TimerRecord::NPOS;
        heap_.pop_back();
        if (index < heap_.size()) {
            sift_down_(index);
            sift_up_(index);
        }
    }

    [[nodiscard]] TimerId schedule_(std::uint64_t delayMs, TimerKind kind,
                                    TimerCallback callback) {
        const auto id{nextId_++};
        const auto delayNs{delayMs > std::numeric_limits<std::uint64_t>::max() / NS_PER_MS
                               ? std::numeric_limits<std::uint64_t>::max()
                               : delayMs * NS_PER_MS};
        TimerRecord record{};
        record.id = id;
        record.deadlineNs = nowNs_ > std::numeric_limits<std::uint64_t>::max() - delayNs
                                ? std::numeric_limits<std::uint64_t>::max()
                                : nowNs_ + delayNs;
        record.periodNs = kind == TimerKind::interval ? delayNs : 0;
        record.epoch = nextEpoch_++;
        record.kind = kind;
        record.callback = std::move(callback);
        auto [it, inserted]{records_.emplace(id, std::move(record))};
        static_cast<void>(inserted);
        insert_(&it->second);
        return id;
    }

public:
    TimerWheel() = default;
    TimerWheel(const TimerWheel&) = delete;
    TimerWheel& operator=(const TimerWheel&) = delete;

    [[nodiscard]] std::uint64_t now_ns() const noexcept { return nowNs_; }
    [[nodiscard]] std::uint64_t now_ms() const noexcept { return nowNs_ / NS_PER_MS; }
    void advance_ms(std::uint64_t deltaMs) noexcept {
        const auto deltaNs{deltaMs > std::numeric_limits<std::uint64_t>::max() / NS_PER_MS
                               ? std::numeric_limits<std::uint64_t>::max()
                               : deltaMs * NS_PER_MS};
        nowNs_ = nowNs_ > std::numeric_limits<std::uint64_t>::max() - deltaNs
                     ? std::numeric_limits<std::uint64_t>::max()
                     : nowNs_ + deltaNs;
    }
    void advance_ns(std::uint64_t deltaNs) noexcept {
        nowNs_ = nowNs_ > std::numeric_limits<std::uint64_t>::max() - deltaNs
                     ? std::numeric_limits<std::uint64_t>::max()
                     : nowNs_ + deltaNs;
    }

    [[nodiscard]] TimerId set_timeout(std::uint64_t delayMs, TimerCallback callback) {
        return schedule_(delayMs, TimerKind::timeout, std::move(callback));
    }
    [[nodiscard]] TimerId set_interval(std::uint64_t periodMs, TimerCallback callback) {
        return schedule_(periodMs, TimerKind::interval, std::move(callback));
    }

    bool cancel(TimerId id) {
        const auto it{records_.find(id)};
        if (it == records_.end()) {
            return false;
        }
        if (it->second.heapIndex != TimerRecord::NPOS) {
            remove_(it->second.heapIndex);
        }
        records_.erase(it);
        return true;
    }

    [[nodiscard]] std::size_t pending() const noexcept { return heap_.size(); }
    [[nodiscard]] std::optional<std::uint64_t> next_deadline_ns() const noexcept {
        return heap_.empty() ? std::nullopt : std::optional{heap_.front()->deadlineNs};
    }
    [[nodiscard]] std::optional<std::uint64_t> next_deadline_ms() const noexcept {
        if (heap_.empty()) {
            return std::nullopt;
        }
        return heap_.front()->deadlineNs / NS_PER_MS;
    }

    [[nodiscard]] bool has_due() const noexcept {
        return !heap_.empty() && heap_.front()->deadlineNs / NS_PER_MS <= nowNs_ / NS_PER_MS;
    }

    // Mirrors Bun's fire path: pop before invoking user code so cancellation
    // from inside the callback cannot invalidate a heap node being inspected.
    std::size_t run_due() {
        std::size_t fired{0};
        while (has_due()) {
            TimerRecord* record{heap_.front()};
            const auto id{record->id};
            const auto kind{record->kind};
            const auto period{record->periodNs};
            auto callback{std::move(record->callback)};
            remove_(0);
            callback();
            ++fired;

            const auto it{records_.find(id)};
            if (it == records_.end()) {
                continue;
            }
            if (kind == TimerKind::interval) {
                it->second.deadlineNs = nowNs_ > std::numeric_limits<std::uint64_t>::max() - period
                                            ? std::numeric_limits<std::uint64_t>::max()
                                            : nowNs_ + period;
                it->second.epoch = nextEpoch_++;
                it->second.callback = std::move(callback);
                insert_(&it->second);
            } else {
                records_.erase(it);
            }
        }
        return fired;
    }
};

}  // namespace mbun::runtime_timer
