export module mbun.bun_alloc.stats;

import std;

export namespace mbun::bun_alloc {

class Stats {
private:
    std::atomic<std::size_t> allocations_ { 0 };
    std::atomic<std::size_t> bytesInUse_ { 0 };

public:
    void record_allocate(std::size_t bytes) noexcept {
        allocations_.fetch_add(1, std::memory_order_relaxed);
        bytesInUse_.fetch_add(bytes, std::memory_order_relaxed);
    }

    void record_deallocate(std::size_t bytes) noexcept {
        allocations_.fetch_sub(1, std::memory_order_relaxed);
        bytesInUse_.fetch_sub(bytes, std::memory_order_relaxed);
    }

    void reset() noexcept {
        allocations_.store(0, std::memory_order_relaxed);
        bytesInUse_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t allocations() const noexcept {
        return allocations_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t bytes_in_use() const noexcept {
        return bytesInUse_.load(std::memory_order_relaxed);
    }
};

}
