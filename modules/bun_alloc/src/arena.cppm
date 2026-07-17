export module mbun.bun_alloc.arena;

import std;
import mbun.bun_alloc.ownership;
import mbun.bun_alloc.stats;

export namespace mbun::bun_alloc {

class Arena {
public:
    struct Mark {
        std::uint64_t generation;
    };

private:
    Stats stats_ {};
    std::uint64_t ownerId_ { 0 };
    std::uint64_t generation_ { 1 };
    std::unordered_set<std::byte*> live_ {};
    inline static std::atomic<std::uint64_t> nextOwnerId_ { 2 };

public:
    Arena() noexcept : ownerId_ { nextOwnerId_.fetch_add(1, std::memory_order_relaxed) } {}
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    ~Arena() { reset(); }

    [[nodiscard]] std::expected<Block, Error> allocate(std::size_t size,
                                                        std::size_t alignment) noexcept {
        if (!valid_alignment(alignment)) {
            return std::unexpected { Error::invalid_alignment };
        }
        try {
            void* pointer { ::operator new(std::max(size, 1uz), std::align_val_t { alignment },
                                            std::nothrow) };
            if (pointer == nullptr) {
                return std::unexpected { Error::out_of_memory };
            }
            auto* bytes { static_cast<std::byte*>(pointer) };
            live_.insert(bytes);
            stats_.record_allocate(size);
            return Block::make(bytes, size, alignment, ownerId_, generation_);
        } catch (...) {
            return std::unexpected { Error::out_of_memory };
        }
    }

    [[nodiscard]] bool owns(const Block& block) const noexcept {
        return block.owner_id() == ownerId_ && block.generation() == generation_ &&
               live_.contains(block.raw_data());
    }

    [[nodiscard]] Mark mark() const noexcept { return { generation_ }; }

    [[nodiscard]] std::expected<void, Error> reset(Mark markValue) noexcept {
        if (markValue.generation != generation_) {
            return std::unexpected { Error::stale_generation };
        }
        reset();
        return {};
    }

    void reset() noexcept {
        for (auto* pointer : live_) {
            ::operator delete(pointer);
        }
        stats_.reset();
        live_.clear();
        ++generation_;
    }

    [[nodiscard]] std::expected<void, Error> deallocate(Block& block) noexcept {
        if (block.owner_id() != ownerId_) {
            return std::unexpected { Error::invalid_owner };
        }
        if (block.generation() != generation_) {
            block.consume();
            return std::unexpected { Error::stale_generation };
        }
        if (!live_.erase(block.raw_data())) {
            return std::unexpected { Error::invalid_owner };
        }
        ::operator delete(block.raw_data(), std::align_val_t { block.raw_alignment() });
        stats_.record_deallocate(block.raw_size());
        block.consume();
        return {};
    }
};

}
