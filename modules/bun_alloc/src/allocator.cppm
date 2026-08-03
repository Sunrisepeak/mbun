export module mbun.bun_alloc.allocator;

import std;
import mbun.bun_alloc.ownership;
import mbun.bun_alloc.stats;

export namespace mbun::bun_alloc {

class Allocator {
private:
    Stats* stats_ { nullptr };
    std::uint64_t ownerId_ { 1 };
    std::uint64_t generation_ { 1 };

    static std::expected<std::byte*, Error> allocate_bytes(std::size_t size,
                                                            std::size_t alignment) noexcept {
        try {
            void* pointer { ::operator new(std::max(size, 1uz), std::align_val_t { alignment },
                                            std::nothrow) };
            if (pointer == nullptr) {
                return std::unexpected { Error::out_of_memory };
            }
            return static_cast<std::byte*>(pointer);
        } catch (...) {
            return std::unexpected { Error::out_of_memory };
        }
    }

public:
    explicit Allocator(Stats& stats) noexcept : stats_ { &stats } {}
    Allocator() = delete;

    [[nodiscard]] std::expected<Block, Error> allocate(std::size_t size,
                                                        std::size_t alignment) noexcept {
        if (!valid_alignment(alignment)) {
            return std::unexpected { Error::invalid_alignment };
        }
        auto pointer { allocate_bytes(size, alignment) };
        if (!pointer) {
            return std::unexpected { pointer.error() };
        }
        stats_->record_allocate(size);
        return Block::make(*pointer, size, alignment, ownerId_, generation_);
    }

    [[nodiscard]] std::expected<void, Error> deallocate(Block& block) noexcept {
        if (block.owner_id() != ownerId_ || block.generation() != generation_) {
            return std::unexpected { block.owner_id() == ownerId_ ? Error::stale_generation
                                                                 : Error::invalid_owner };
        }
        ::operator delete(block.raw_data(), std::align_val_t { block.raw_alignment() });
        stats_->record_deallocate(block.raw_size());
        block.consume();
        return {};
    }
};

}
