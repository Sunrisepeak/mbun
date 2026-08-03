export module mbun.runtime_allocators.arena;

import std;
import mbun.runtime_allocators.backend;
import mbun.runtime_allocators.stats;

export namespace mbun::runtime_allocators {

// Ref: Bun's runtime allocator split keeps arena ownership separate from the
// tier-0 allocator.  This first port preserves that boundary with a bulk
// record list; mimalloc arena and safety-vtable registration are DEFERRED.

class Arena {
private:
    struct Record {
        std::byte* pointer { nullptr };
        std::size_t size_ { 0 };
        std::size_t alignment_ { 1 };
        Backend backend {};
    };
    Backend backend_ {};
    Stats* stats_ { nullptr };
    std::vector<Record> records_ {};

public:
    Arena(Backend backend, Stats& stats) noexcept : backend_ { backend }, stats_ { &stats } {}
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    ~Arena() { (void)reset(); }

    // The returned pointer is borrowed; Arena::reset() releases all records.
    [[nodiscard]] std::expected<std::byte*, Error> allocate(std::size_t size,
                                                             std::size_t alignment) noexcept {
        auto result { backend_.allocate(size, alignment) };
        if (!result) {
            return std::unexpected { result.error() };
        }
        try {
            records_.push_back({ *result, size, alignment, backend_ });
        } catch (...) {
            backend_.deallocate(*result, size, alignment);
            return std::unexpected { Error::out_of_memory };
        }
        stats_->record_allocate(size);
        return *result;
    }

    [[nodiscard]] std::expected<void, Error> reset() noexcept {
        for (const auto& record : records_) {
            record.backend.deallocate(record.pointer, record.size_, record.alignment_);
            stats_->record_deallocate(record.size_);
        }
        records_.clear();
        return {};
    }
};

}
