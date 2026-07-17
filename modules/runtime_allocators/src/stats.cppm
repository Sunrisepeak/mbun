export module mbun.runtime_allocators.stats;

import std;

export namespace mbun::runtime_allocators {

class Stats {
private:
    std::atomic<std::size_t> allocations_ { 0 };
    std::atomic<std::size_t> bytesInUse_ { 0 };
    std::atomic<std::size_t> fallbackAllocations_ { 0 };

public:
    void record_allocate(std::size_t bytes, bool fallback = false) noexcept {
        allocations_.fetch_add(1, std::memory_order_relaxed);
        bytesInUse_.fetch_add(bytes, std::memory_order_relaxed);
        if (fallback) {
            fallbackAllocations_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void record_deallocate(std::size_t bytes, bool fallback = false) noexcept {
        allocations_.fetch_sub(1, std::memory_order_relaxed);
        bytesInUse_.fetch_sub(bytes, std::memory_order_relaxed);
        if (fallback) {
            fallbackAllocations_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::size_t allocations() const noexcept { return allocations_.load(); }
    [[nodiscard]] std::size_t bytes_in_use() const noexcept { return bytesInUse_.load(); }
    [[nodiscard]] std::size_t fallback_allocations() const noexcept {
        return fallbackAllocations_.load();
    }
};

}
