export module mbun.runtime_allocators.fallback;

import std;
import mbun.runtime_allocators.arena;
import mbun.runtime_allocators.backend;
import mbun.runtime_allocators.stats;

export namespace mbun::runtime_allocators {

// Ref: bun runtime allocator vtable selection.  Native memfd/mmap fallback
// remains a backend concern; this layer only preserves primary-then-fallback
// ordering and the selected-backend ownership token.

class FallbackAllocator {
private:
    class Block {
    private:
        std::byte* pointer_ { nullptr };
        std::size_t size_ { 0 };
        std::size_t alignment_ { 1 };
        Backend backend_ {};
        bool fallback_ { false };

    public:
        Block(std::byte* pointer, std::size_t size, std::size_t alignment, Backend backend,
              bool fallback) noexcept
            : pointer_ { pointer }, size_ { size }, alignment_ { alignment }, backend_ { backend },
              fallback_ { fallback } {}
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;
        Block(Block&&) noexcept = default;
        Block& operator=(Block&&) noexcept = default;
        [[nodiscard]] std::byte* data() const noexcept { return pointer_; }
        [[nodiscard]] std::size_t size() const noexcept { return size_; }
        [[nodiscard]] bool is_fallback() const noexcept { return fallback_; }
        [[nodiscard]] std::string_view backend_name() const noexcept { return backend_.name; }
        void release() noexcept {
            if (pointer_ != nullptr) {
                backend_.deallocate(pointer_, size_, alignment_);
                pointer_ = nullptr;
            }
        }
    };
    Backend primary_ {};
    Backend fallback_ {};
    Stats* stats_ { nullptr };

public:
    FallbackAllocator(Backend primary, Backend fallback, Stats& stats) noexcept
        : primary_ { primary }, fallback_ { fallback }, stats_ { &stats } {}

    [[nodiscard]] std::expected<Block, Error> allocate(std::size_t size, std::size_t alignment) noexcept {
        auto result { primary_.allocate(size, alignment) };
        bool usedFallback { false };
        Backend selected { primary_ };
        if (!result) {
            result = fallback_.allocate(size, alignment);
            selected = fallback_;
            usedFallback = true;
        }
        if (!result) {
            return std::unexpected { result.error() };
        }
        stats_->record_allocate(size, usedFallback);
        return Block { *result, size, alignment, selected, usedFallback };
    }

    [[nodiscard]] std::expected<void, Error> deallocate(Block& block) noexcept {
        if (block.data() == nullptr) {
            return std::unexpected { Error::invalid_block };
        }
        stats_->record_deallocate(block.size(), block.is_fallback());
        block.release();
        return {};
    }
};

}
