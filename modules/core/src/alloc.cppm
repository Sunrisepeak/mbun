// alloc.cppm — mbun.core.alloc: checked allocator ownership capabilities.
//
// Re-expressed in MC++ from bun's allocator implementations (MIT-licensed):
//   - Rust: src/bun_alloc/{basic.rs,MimallocArena.rs,stack_fallback.rs,
//           BufferFallbackAllocator.rs}
//   - Zig:  src/bun_alloc/{basic.zig,MimallocArena.zig,
//           BufferFallbackAllocator.zig} and src/bun.zig StackFallbackAllocator
//
// The raw allocator paths retain bun's mimalloc fast paths. The public MC++
// surface adds an unforgeable, move-only owner token so foreign, stale and
// duplicate frees are rejected before any allocator C API sees a pointer.
module;

#include <mimalloc.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define MBUN_ALLOC_WITH_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define MBUN_ALLOC_WITH_ASAN 1
#endif
#ifndef MBUN_ALLOC_WITH_ASAN
#define MBUN_ALLOC_WITH_ASAN 0
#endif

#ifndef MBUN_ALLOC_DEBUG_GUARDS
#define MBUN_ALLOC_DEBUG_GUARDS 0
#endif

export module mbun.core.alloc;

import std;

export namespace mbun::core::alloc {

constexpr std::size_t DEFAULT_ALIGNMENT{alignof(std::max_align_t)};
constexpr std::size_t MIMALLOC_FAST_ALIGNMENT{16};
constexpr bool SANITIZER_SYSTEM_ALLOCATOR{MBUN_ALLOC_WITH_ASAN != 0};
constexpr bool ARENA_THREAD_GUARDS_ENABLED{MBUN_ALLOC_DEBUG_GUARDS != 0};

enum class AllocError : std::uint8_t {
    invalid_alignment,
    size_overflow,
    out_of_memory,
    invalid_block,
    wrong_thread,
};

[[nodiscard]] constexpr bool is_valid_alignment(std::size_t alignment) noexcept {
    return alignment != 0 && std::has_single_bit(alignment);
}

class Block;
class OwnedBlock;
class Arena;
template <std::size_t N, class Fallback>
class StackFallback;

namespace detail {

enum class Backend : std::uint8_t {
    none,
    global,
    arena,
    stack_inline,
    stack_fallback,
};

constexpr std::uint64_t GLOBAL_OWNER_ID{1};
constexpr std::uint64_t GLOBAL_GENERATION{1};

inline std::atomic<std::uint64_t> nextOwnerId{GLOBAL_OWNER_ID + 1};

[[nodiscard]] inline std::uint64_t next_owner_id() noexcept {
    return nextOwnerId.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline std::expected<std::size_t, AllocError>
checked_product(std::size_t count, std::size_t elementSize) noexcept {
    if (elementSize != 0 && count > std::numeric_limits<std::size_t>::max() / elementSize) {
        return std::unexpected{AllocError::size_overflow};
    }
    return count * elementSize;
}

[[nodiscard]] inline void* mimalloc_allocate(std::size_t size, std::size_t alignment,
                                             bool zeroed) noexcept {
    if (alignment > MIMALLOC_FAST_ALIGNMENT) {
        return zeroed ? mi_zalloc_aligned(size, alignment) : mi_malloc_aligned(size, alignment);
    }
    return zeroed ? mi_zalloc(size) : mi_malloc(size);
}

[[nodiscard]] inline void* system_allocate(std::size_t size, std::size_t alignment,
                                           bool zeroed) noexcept {
    const std::size_t actualSize{std::max(size, 1uz)};
    void* pointer{nullptr};
    if (alignment > DEFAULT_ALIGNMENT) {
        pointer = ::operator new(actualSize, std::align_val_t{alignment}, std::nothrow);
    } else {
        pointer = ::operator new(actualSize, std::nothrow);
    }
    if (pointer != nullptr && zeroed && size != 0) {
        std::memset(pointer, 0, size);
    }
    return pointer;
}

[[nodiscard]] inline void* global_allocate(std::size_t size, std::size_t alignment,
                                           bool zeroed) noexcept {
    if constexpr (SANITIZER_SYSTEM_ALLOCATOR) {
        return system_allocate(size, alignment, zeroed);
    }
    return mimalloc_allocate(size, alignment, zeroed);
}

inline void global_deallocate(void* pointer, std::size_t alignment) noexcept {
    if constexpr (SANITIZER_SYSTEM_ALLOCATOR) {
        if (alignment > DEFAULT_ALIGNMENT) {
            ::operator delete(pointer, std::align_val_t{alignment});
        } else {
            ::operator delete(pointer);
        }
    } else {
        mi_free(pointer);
    }
}

[[nodiscard]] inline void* global_reallocate(void* pointer, std::size_t oldSize,
                                             std::size_t newSize, std::size_t alignment) noexcept {
    if constexpr (SANITIZER_SYSTEM_ALLOCATOR) {
        void* replacement{system_allocate(newSize, alignment, false)};
        if (replacement == nullptr) {
            return nullptr;
        }
        std::memcpy(replacement, pointer, std::min(oldSize, newSize));
        global_deallocate(pointer, alignment);
        return replacement;
    }
    return mi_realloc_aligned(pointer, newSize, alignment);
}

[[nodiscard]] inline void* arena_allocate(void* heap, std::size_t size, std::size_t alignment,
                                          bool zeroed) noexcept {
    auto* typedHeap{static_cast<mi_heap_t*>(heap)};
    if (alignment > MIMALLOC_FAST_ALIGNMENT) {
        return zeroed ? mi_heap_zalloc_aligned(typedHeap, size, alignment)
                      : mi_heap_malloc_aligned(typedHeap, size, alignment);
    }
    return zeroed ? mi_heap_zalloc(typedHeap, size) : mi_heap_malloc(typedHeap, size);
}

}  // namespace detail

// Default fallback adapter. It deliberately reuses this module's selected
// global backend, so ASAN keeps using the system allocator while normal builds
// retain Bun's mimalloc fast path.
class GlobalFallbackBackend {
public:
    [[nodiscard]] void* raw_allocate(std::size_t size, std::size_t alignment,
                                     bool zeroed = false) noexcept {
        return detail::global_allocate(size, alignment, zeroed);
    }

    [[nodiscard]] bool raw_resize(std::byte*, std::size_t oldSize, std::size_t,
                                  std::size_t newSize) noexcept {
        return newSize <= oldSize;
    }

    [[nodiscard]] void* raw_remap(std::byte* data, std::size_t oldSize, std::size_t alignment,
                                  std::size_t newSize) noexcept {
        return detail::global_reallocate(data, oldSize, newSize, alignment);
    }

    void raw_deallocate(void* data, std::size_t, std::size_t alignment) noexcept {
        detail::global_deallocate(data, alignment);
    }
};

template <class Fallback>
concept FallbackBackend =
    requires(Fallback& fallback, std::byte* data, std::size_t size, std::size_t alignment) {
        { fallback.raw_allocate(size, alignment, false) } -> std::same_as<void*>;
        { fallback.raw_resize(data, size, alignment, size) } -> std::same_as<bool>;
        { fallback.raw_remap(data, size, alignment, size) } -> std::same_as<void*>;
        fallback.raw_deallocate(data, size, alignment);
    };

// Re-expressed from Bun's Rust/Zig BufferFallbackAllocator. The buffer is
// borrowed: allocation first bumps through it, then delegates to Fallback.
// Resize/remap/free dispatch by address ownership exactly like Bun's vtable.
template <FallbackBackend Fallback = GlobalFallbackBackend>
class BufferFallbackAllocator {
private:
    std::span<std::byte> buffer_{};
    std::size_t cursor_{0};
    Fallback fallback_{};

    [[nodiscard]] std::byte* fixed_allocate_(std::size_t size, std::size_t alignment) noexcept {
        const auto base{reinterpret_cast<std::uintptr_t>(buffer_.data())};
        if (cursor_ > std::numeric_limits<std::uintptr_t>::max() - base) {
            return nullptr;
        }
        const std::uintptr_t current{base + cursor_};
        if (current > std::numeric_limits<std::uintptr_t>::max() - (alignment - 1)) {
            return nullptr;
        }
        const std::uintptr_t aligned{(current + alignment - 1) & ~(alignment - 1)};
        if (aligned < base) {
            return nullptr;
        }
        const std::size_t start{static_cast<std::size_t>(aligned - base)};
        if (start > buffer_.size() || size > buffer_.size() - start) {
            return nullptr;
        }
        cursor_ = start + size;
        return reinterpret_cast<std::byte*>(aligned);
    }

    [[nodiscard]] std::size_t offset_(const std::byte* data) const noexcept {
        const auto base{reinterpret_cast<std::uintptr_t>(buffer_.data())};
        const auto pointer{reinterpret_cast<std::uintptr_t>(data)};
        return static_cast<std::size_t>(pointer - base);
    }
public:
    explicit BufferFallbackAllocator(std::span<std::byte> buffer, Fallback fallback = {}) noexcept
        : buffer_{buffer}, fallback_{std::move(fallback)} {}

    [[nodiscard]] void* raw_allocate(std::size_t size, std::size_t alignment,
                                     bool zeroed = false) noexcept {
        if (!is_valid_alignment(alignment)) {
            return nullptr;
        }
        if (auto* data{fixed_allocate_(size, alignment)}; data != nullptr) {
            if (zeroed && size != 0) {
                std::memset(data, 0, size);
            }
            return data;
        }
        return fallback_.raw_allocate(size, alignment, zeroed);
    }

    [[nodiscard]] bool owns(const std::byte* data) const noexcept {
        const auto base{reinterpret_cast<std::uintptr_t>(buffer_.data())};
        const auto pointer{reinterpret_cast<std::uintptr_t>(data)};
        return pointer >= base && pointer < base + buffer_.size();
    }

    [[nodiscard]] bool raw_resize(std::byte* data, std::size_t oldSize, std::size_t alignment,
                                  std::size_t newSize) noexcept {
        if (!owns(data)) {
            return fallback_.raw_resize(data, oldSize, alignment, newSize);
        }
        const std::size_t start{offset_(data)};
        if (oldSize > buffer_.size() - start) {
            return false;
        }
        const std::size_t oldEnd{start + oldSize};
        if (oldEnd != cursor_) {
            return newSize <= oldSize;
        }
        if (newSize > buffer_.size() - start) {
            return false;
        }
        cursor_ = start + newSize;
        return true;
    }

    [[nodiscard]] void* raw_remap(std::byte* data, std::size_t oldSize, std::size_t alignment,
                                  std::size_t newSize) noexcept {
        if (owns(data)) {
            return raw_resize(data, oldSize, alignment, newSize) ? data : nullptr;
        }
        return fallback_.raw_remap(data, oldSize, alignment, newSize);
    }

    void raw_deallocate(std::byte* data, std::size_t size, std::size_t alignment) noexcept {
        if (!owns(data)) {
            fallback_.raw_deallocate(data, size, alignment);
            return;
        }
        const std::size_t start{offset_(data)};
        if (size <= buffer_.size() - start && start + size == cursor_) {
            cursor_ = start;
        }
    }

    void reset() noexcept {
        cursor_ = 0;
    }
    [[nodiscard]] std::size_t used() const noexcept {
        return cursor_;
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return buffer_.size();
    }
    [[nodiscard]] Fallback& fallback() noexcept {
        return fallback_;
    }
    [[nodiscard]] const Fallback& fallback() const noexcept {
        return fallback_;
    }
};

class BlockView {
private:
    std::byte* data_{nullptr};
    std::size_t size_{0};
    std::size_t alignment_{DEFAULT_ALIGNMENT};

    constexpr BlockView(std::byte* data, std::size_t size, std::size_t alignment) noexcept
        : data_{data}, size_{size}, alignment_{alignment} {}

    friend class Block;
public:
    constexpr BlockView() noexcept = default;

    [[nodiscard]] constexpr std::byte* data() const noexcept {
        return data_;
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] constexpr std::size_t alignment() const noexcept {
        return alignment_;
    }
    [[nodiscard]] constexpr bool empty() const noexcept {
        return data_ == nullptr;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return !empty();
    }
    [[nodiscard]] constexpr std::span<std::byte> bytes() const noexcept {
        return {data_, size_};
    }
};

class Block {
private:
    std::byte* data_{nullptr};
    std::size_t size_{0};
    std::size_t alignment_{DEFAULT_ALIGNMENT};
    detail::Backend backend_{detail::Backend::none};
    std::uint64_t ownerId_{0};
    std::uint64_t generation_{0};

    constexpr Block(std::byte* data, std::size_t size, std::size_t alignment,
                    detail::Backend backend, std::uint64_t ownerId,
                    std::uint64_t generation) noexcept
        : data_{data},
          size_{size},
          alignment_{alignment},
          backend_{backend},
          ownerId_{ownerId},
          generation_{generation} {}

    constexpr void invalidate_() noexcept {
        data_ = nullptr;
        size_ = 0;
        alignment_ = DEFAULT_ALIGNMENT;
        backend_ = detail::Backend::none;
        ownerId_ = 0;
        generation_ = 0;
    }

    constexpr void take_from_(Block& other) noexcept {
        data_ = other.data_;
        size_ = other.size_;
        alignment_ = other.alignment_;
        backend_ = other.backend_;
        ownerId_ = other.ownerId_;
        generation_ = other.generation_;
        other.invalidate_();
    }

    friend std::expected<Block, AllocError> allocate(std::size_t, std::size_t) noexcept;
    friend std::expected<Block, AllocError> allocate_zeroed(std::size_t, std::size_t) noexcept;
    friend std::expected<void, AllocError> deallocate(Block&) noexcept;
    friend std::expected<void, AllocError> reallocate(Block&, std::size_t) noexcept;
    friend class OwnedBlock;
    friend class Arena;
    template <std::size_t N, class Fallback>
    friend class StackFallback;
public:
    constexpr Block() noexcept = default;
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;

    constexpr Block(Block&& other) noexcept {
        take_from_(other);
    }
    Block& operator=(Block&&) = delete;

    [[nodiscard]] constexpr bool empty() const noexcept {
        return data_ == nullptr;
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return !empty();
    }
    [[nodiscard]] constexpr BlockView view() const noexcept {
        return BlockView{data_, size_, alignment_};
    }
};

[[nodiscard]] inline std::expected<Block, AllocError>
allocate(std::size_t size, std::size_t alignment = DEFAULT_ALIGNMENT) noexcept {
    if (!is_valid_alignment(alignment)) {
        return std::unexpected{AllocError::invalid_alignment};
    }
    void* pointer{detail::global_allocate(size, alignment, false)};
    if (pointer == nullptr) {
        return std::unexpected{AllocError::out_of_memory};
    }
    return Block{static_cast<std::byte*>(pointer),
                 size,
                 alignment,
                 detail::Backend::global,
                 detail::GLOBAL_OWNER_ID,
                 detail::GLOBAL_GENERATION};
}

[[nodiscard]] inline std::expected<Block, AllocError>
allocate_zeroed(std::size_t size, std::size_t alignment = DEFAULT_ALIGNMENT) noexcept {
    if (!is_valid_alignment(alignment)) {
        return std::unexpected{AllocError::invalid_alignment};
    }
    void* pointer{detail::global_allocate(size, alignment, true)};
    if (pointer == nullptr) {
        return std::unexpected{AllocError::out_of_memory};
    }
    return Block{static_cast<std::byte*>(pointer),
                 size,
                 alignment,
                 detail::Backend::global,
                 detail::GLOBAL_OWNER_ID,
                 detail::GLOBAL_GENERATION};
}

[[nodiscard]] inline std::expected<Block, AllocError>
allocate_array(std::size_t count, std::size_t elementSize,
               std::size_t alignment = DEFAULT_ALIGNMENT) noexcept {
    auto size{detail::checked_product(count, elementSize)};
    if (!size) {
        return std::unexpected{size.error()};
    }
    return allocate(*size, alignment);
}

[[nodiscard]] inline std::expected<void, AllocError> deallocate(Block& block) noexcept {
    if (!block || block.backend_ != detail::Backend::global ||
        block.ownerId_ != detail::GLOBAL_OWNER_ID ||
        block.generation_ != detail::GLOBAL_GENERATION) {
        return std::unexpected{AllocError::invalid_block};
    }
    detail::global_deallocate(block.data_, block.alignment_);
    block.invalidate_();
    return {};
}

[[nodiscard]] inline std::expected<void, AllocError> reallocate(Block& block,
                                                                std::size_t newSize) noexcept {
    if (!block || block.backend_ != detail::Backend::global ||
        block.ownerId_ != detail::GLOBAL_OWNER_ID ||
        block.generation_ != detail::GLOBAL_GENERATION) {
        return std::unexpected{AllocError::invalid_block};
    }
    if (newSize == 0) {
        return deallocate(block);
    }
    if (newSize == block.size_) {
        return {};
    }

    void* pointer{detail::global_reallocate(block.data_, block.size_, newSize, block.alignment_)};
    if (pointer == nullptr) {
        return std::unexpected{AllocError::out_of_memory};
    }
    block.data_ = static_cast<std::byte*>(pointer);
    block.size_ = newSize;
    return {};
}

class OwnedBlock {
private:
    Block block_{};

    explicit OwnedBlock(Block&& block) noexcept : block_{std::move(block)} {}
public:
    OwnedBlock() noexcept = default;
    ~OwnedBlock() {
        if (block_) {
            static_cast<void>(deallocate(block_));
        }
    }

    OwnedBlock(const OwnedBlock&) = delete;
    OwnedBlock& operator=(const OwnedBlock&) = delete;

    OwnedBlock(OwnedBlock&& other) noexcept : block_{std::move(other.block_)} {}

    OwnedBlock& operator=(OwnedBlock&& other) noexcept {
        if (this != &other) {
            reset();
            block_.take_from_(other.block_);
        }
        return *this;
    }

    [[nodiscard]] static std::expected<OwnedBlock, AllocError>
    allocate(std::size_t size, std::size_t alignment = DEFAULT_ALIGNMENT) noexcept {
        auto result{alloc::allocate(size, alignment)};
        if (!result) {
            return std::unexpected{result.error()};
        }
        return OwnedBlock{std::move(*result)};
    }

    [[nodiscard]] static std::expected<OwnedBlock, AllocError>
    allocate_zeroed(std::size_t size, std::size_t alignment = DEFAULT_ALIGNMENT) noexcept {
        auto result{alloc::allocate_zeroed(size, alignment)};
        if (!result) {
            return std::unexpected{result.error()};
        }
        return OwnedBlock{std::move(*result)};
    }

    [[nodiscard]] std::expected<void, AllocError> resize(std::size_t newSize) noexcept {
        return alloc::reallocate(block_, newSize);
    }

    [[nodiscard]] BlockView block() const noexcept {
        return block_.view();
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(block_);
    }

    [[nodiscard]] Block release() noexcept {
        return Block{std::move(block_)};
    }

    void reset() noexcept {
        if (block_) {
            static_cast<void>(deallocate(block_));
        }
    }
};

class Arena {
private:
    enum class TokenState : std::uint8_t {
        valid,
        foreign,
        stale,
    };

#if MBUN_ALLOC_WITH_ASAN
    struct SystemAllocation {
        void* pointer;
        std::size_t alignment;
        SystemAllocation* next;
    };

    // Sanitizer-only ownership index. Production arenas remain the same
    // mimalloc heap + capability metadata with no tracking field or branch.
    // Cross-thread free is serialized here; reset still requires exclusive
    // access to the Arena, matching bun's Rust `reset(&mut self)` contract.
    mutable std::mutex allocationsMutex_;
    mutable SystemAllocation* allocations_{nullptr};
#else
    void* heap_{nullptr};
#endif
    std::uint64_t ownerId_{0};
    std::uint64_t generation_{1};
#if MBUN_ALLOC_DEBUG_GUARDS
    std::thread::id ownerThread_{};
#endif

    [[nodiscard]] bool on_owner_thread_() const noexcept {
#if MBUN_ALLOC_DEBUG_GUARDS
        return std::this_thread::get_id() == ownerThread_;
#else
        return true;
#endif
    }

    [[nodiscard]] TokenState token_state_(const Block& block) const noexcept {
        if (!block || block.backend_ != detail::Backend::arena || block.ownerId_ != ownerId_) {
            return TokenState::foreign;
        }
        return block.generation_ == generation_ ? TokenState::valid : TokenState::stale;
    }

#if MBUN_ALLOC_WITH_ASAN
    [[nodiscard]] void* system_allocate_tracked_(std::size_t size, std::size_t alignment,
                                                 bool zeroed) const noexcept {
        void* pointer{detail::system_allocate(size, alignment, zeroed)};
        if (pointer == nullptr) {
            return nullptr;
        }
        auto* allocation{new (std::nothrow) SystemAllocation{pointer, alignment, nullptr}};
        if (allocation == nullptr) {
            detail::global_deallocate(pointer, alignment);
            return nullptr;
        }

        std::scoped_lock lock{allocationsMutex_};
        allocation->next = allocations_;
        allocations_ = allocation;
        return pointer;
    }

    [[nodiscard]] bool system_deallocate_tracked_(void* pointer) const noexcept {
        SystemAllocation* allocation{nullptr};
        {
            std::scoped_lock lock{allocationsMutex_};
            SystemAllocation** link{&allocations_};
            while (*link != nullptr && (*link)->pointer != pointer) {
                link = &(*link)->next;
            }
            if (*link == nullptr) {
                return false;
            }
            allocation = *link;
            *link = allocation->next;
        }
        detail::global_deallocate(allocation->pointer, allocation->alignment);
        delete allocation;
        return true;
    }

    [[nodiscard]] std::expected<void*, AllocError>
    system_reallocate_tracked_(void* pointer, std::size_t oldSize, std::size_t newSize,
                               std::size_t alignment) const noexcept {
        std::scoped_lock lock{allocationsMutex_};
        SystemAllocation* allocation{allocations_};
        while (allocation != nullptr && allocation->pointer != pointer) {
            allocation = allocation->next;
        }
        if (allocation == nullptr) {
            return std::unexpected{AllocError::invalid_block};
        }

        void* replacement{detail::global_reallocate(pointer, oldSize, newSize, alignment)};
        if (replacement == nullptr) {
            return std::unexpected{AllocError::out_of_memory};
        }
        allocation->pointer = replacement;
        return replacement;
    }

    void system_release_all_() noexcept {
        SystemAllocation* allocation{nullptr};
        {
            std::scoped_lock lock{allocationsMutex_};
            allocation = std::exchange(allocations_, nullptr);
        }
        while (allocation != nullptr) {
            SystemAllocation* next{allocation->next};
            detail::global_deallocate(allocation->pointer, allocation->alignment);
            delete allocation;
            allocation = next;
        }
    }
#endif

    [[nodiscard]] std::expected<Block, AllocError>
    allocate_(std::size_t size, std::size_t alignment, bool zeroed) const noexcept {
        if (!on_owner_thread_()) {
            return std::unexpected{AllocError::wrong_thread};
        }
        if (!is_valid_alignment(alignment)) {
            return std::unexpected{AllocError::invalid_alignment};
        }
#if MBUN_ALLOC_WITH_ASAN
        void* pointer{system_allocate_tracked_(size, alignment, zeroed)};
#else
        if (heap_ == nullptr) {
            return std::unexpected{AllocError::out_of_memory};
        }
        void* pointer{detail::arena_allocate(heap_, size, alignment, zeroed)};
#endif
        if (pointer == nullptr) {
            return std::unexpected{AllocError::out_of_memory};
        }
        return Block{static_cast<std::byte*>(pointer), size,     alignment,
                     detail::Backend::arena,           ownerId_, generation_};
    }

    void advance_generation_() noexcept {
        ++generation_;
        if (generation_ == 0) {
            ownerId_ = detail::next_owner_id();
            generation_ = 1;
        }
    }
public:
    Arena() noexcept
#if MBUN_ALLOC_WITH_ASAN
        : ownerId_{detail::next_owner_id()}
#else
        : heap_{mi_heap_new()},
          ownerId_{detail::next_owner_id()}
#endif
#if MBUN_ALLOC_DEBUG_GUARDS
          ,
          ownerThread_{std::this_thread::get_id()}
#endif
    {
    }

    ~Arena() {
#if MBUN_ALLOC_WITH_ASAN
        system_release_all_();
#else
        if (heap_ != nullptr) {
            mi_heap_destroy(static_cast<mi_heap_t*>(heap_));
        }
#endif
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    Arena(Arena&& other) noexcept
#if MBUN_ALLOC_WITH_ASAN
        : ownerId_{std::exchange(other.ownerId_, 0)},
          generation_{std::exchange(other.generation_, 0)}
#else
        : heap_{std::exchange(other.heap_, nullptr)},
          ownerId_{std::exchange(other.ownerId_, 0)},
          generation_{std::exchange(other.generation_, 0)}
#endif
#if MBUN_ALLOC_DEBUG_GUARDS
          ,
          ownerThread_{other.ownerThread_}
#endif
    {
#if MBUN_ALLOC_WITH_ASAN
        std::scoped_lock lock{other.allocationsMutex_};
        allocations_ = std::exchange(other.allocations_, nullptr);
#endif
    }

    Arena& operator=(Arena&&) = delete;

    [[nodiscard]] bool valid() const noexcept {
#if MBUN_ALLOC_WITH_ASAN
        return ownerId_ != 0;
#else
        return heap_ != nullptr;
#endif
    }

    [[nodiscard]] std::expected<Block, AllocError>
    allocate(std::size_t size, std::size_t alignment = DEFAULT_ALIGNMENT) const noexcept {
        return allocate_(size, alignment, false);
    }

    [[nodiscard]] std::expected<Block, AllocError>
    allocate_zeroed(std::size_t size, std::size_t alignment = DEFAULT_ALIGNMENT) const noexcept {
        return allocate_(size, alignment, true);
    }

    [[nodiscard]] std::expected<Block, AllocError>
    allocate_array(std::size_t count, std::size_t elementSize,
                   std::size_t alignment = DEFAULT_ALIGNMENT) const noexcept {
        auto size{detail::checked_product(count, elementSize)};
        if (!size) {
            return std::unexpected{size.error()};
        }
        return allocate(*size, alignment);
    }

    [[nodiscard]] bool owns(const Block& block) const noexcept {
        return token_state_(block) == TokenState::valid;
    }

    [[nodiscard]] std::expected<void, AllocError> deallocate(Block& block) const noexcept {
        const TokenState state{token_state_(block)};
        if (state == TokenState::stale) {
            block.invalidate_();
            return std::unexpected{AllocError::invalid_block};
        }
        if (state != TokenState::valid) {
            return std::unexpected{AllocError::invalid_block};
        }
#if MBUN_ALLOC_WITH_ASAN
        if (!system_deallocate_tracked_(block.data_)) {
            return std::unexpected{AllocError::invalid_block};
        }
#else
        // `mi_free` is explicitly thread-safe even when the allocation came
        // from a thread-affine `mi_heap_t`; bun leaves this path unguarded.
        mi_free(block.data_);
#endif
        block.invalidate_();
        return {};
    }

    [[nodiscard]] std::expected<void, AllocError> reallocate(Block& block,
                                                             std::size_t newSize) const noexcept {
        if (!on_owner_thread_()) {
            return std::unexpected{AllocError::wrong_thread};
        }
        const TokenState state{token_state_(block)};
        if (state == TokenState::stale) {
            block.invalidate_();
            return std::unexpected{AllocError::invalid_block};
        }
        if (state != TokenState::valid) {
            return std::unexpected{AllocError::invalid_block};
        }
        if (newSize == 0) {
            return deallocate(block);
        }
        if (newSize == block.size_) {
            return {};
        }

#if MBUN_ALLOC_WITH_ASAN
        auto result{
            system_reallocate_tracked_(block.data_, block.size_, newSize, block.alignment_)};
        if (!result) {
            return std::unexpected{result.error()};
        }
        void* pointer{*result};
#else
        void* pointer{mi_heap_realloc_aligned(static_cast<mi_heap_t*>(heap_), block.data_, newSize,
                                              block.alignment_)};
        if (pointer == nullptr) {
            return std::unexpected{AllocError::out_of_memory};
        }
#endif
        block.data_ = static_cast<std::byte*>(pointer);
        block.size_ = newSize;
        return {};
    }

    // Existing tokens become stale capabilities and are diagnosed without
    // pointer use. Like bun's `reset(&mut self)`, the caller must provide
    // exclusive access; moving to another thread and resetting there is safe
    // and transfers the debug allocation stamp to that thread.
    [[nodiscard]] std::expected<void, AllocError> reset() noexcept {
#if MBUN_ALLOC_WITH_ASAN
        system_release_all_();
#else
        // Allocate the replacement before destroying the old heap so OOM
        // leaves the existing Arena intact under this expected-based API.
        void* replacement{mi_heap_new()};
        if (replacement == nullptr) {
            return std::unexpected{AllocError::out_of_memory};
        }
        if (heap_ != nullptr) {
            mi_heap_destroy(static_cast<mi_heap_t*>(heap_));
        }
        heap_ = replacement;
#endif
        advance_generation_();
#if MBUN_ALLOC_DEBUG_GUARDS
        ownerThread_ = std::this_thread::get_id();
#endif
        return {};
    }

    [[nodiscard]] std::expected<void, AllocError> collect(bool force = false) const noexcept {
#if MBUN_ALLOC_WITH_ASAN
        static_cast<void>(force);
        return {};
#else
        if (heap_ == nullptr) {
            return std::unexpected{AllocError::invalid_block};
        }
        mi_heap_collect(static_cast<mi_heap_t*>(heap_), force);
        return {};
#endif
    }
};

template <std::size_t N, class Fallback = GlobalFallbackBackend>
class StackFallback {
    static_assert(N > 0, "StackFallback needs a non-empty inline buffer");
    static_assert(FallbackBackend<Fallback>, "StackFallback needs a fallback backend");
private:
    enum class TokenState : std::uint8_t {
        valid,
        foreign,
        stale,
    };

    std::size_t cursor_{0};
    std::size_t liveAllocations_{0};
    std::size_t inlineLive_{0};
    std::uint64_t ownerId_{detail::next_owner_id()};
    std::uint64_t generation_{1};
    alignas(std::max_align_t) std::array<std::byte, N> buffer_{};
    Fallback fallback_{};

    [[nodiscard]] std::byte* base_() noexcept {
        return buffer_.data();
    }

    [[nodiscard]] TokenState token_state_(const Block& block) const noexcept {
        const bool stackBackend{block.backend_ == detail::Backend::stack_inline ||
                                block.backend_ == detail::Backend::stack_fallback};
        if (!block || !stackBackend || block.ownerId_ != ownerId_) {
            return TokenState::foreign;
        }
        return block.generation_ == generation_ ? TokenState::valid : TokenState::stale;
    }

    [[nodiscard]] bool is_last_(const Block& block) const noexcept {
        if (block.backend_ != detail::Backend::stack_inline) {
            return false;
        }
        const auto base{reinterpret_cast<std::uintptr_t>(buffer_.data())};
        const auto pointer{reinterpret_cast<std::uintptr_t>(block.data_)};
        return pointer >= base && pointer - base <= cursor_ &&
               block.size_ <= cursor_ - static_cast<std::size_t>(pointer - base) &&
               static_cast<std::size_t>(pointer - base) + block.size_ == cursor_;
    }

    [[nodiscard]] std::expected<Block, AllocError>
    allocate_fallback_(std::size_t size, std::size_t alignment, bool zeroed) noexcept {
        void* pointer{fallback_.raw_allocate(size, alignment, zeroed)};
        if (pointer == nullptr) {
            return std::unexpected{AllocError::out_of_memory};
        }
        ++liveAllocations_;
        return Block{static_cast<std::byte*>(pointer), size,     alignment,
                     detail::Backend::stack_fallback,  ownerId_, generation_};
    }

    [[nodiscard]] std::expected<Block, AllocError>
    allocate_(std::size_t size, std::size_t alignment, bool zeroed) noexcept {
        if (!is_valid_alignment(alignment)) {
            return std::unexpected{AllocError::invalid_alignment};
        }
        // A zero-length block at cursor == N would use a one-past-end address.
        // A fallback sentinel gives it an unambiguous backend and deallocator.
        if (size == 0) {
            return allocate_fallback_(0, alignment, zeroed);
        }

        const auto base{reinterpret_cast<std::uintptr_t>(base_())};
        if (cursor_ > std::numeric_limits<std::uintptr_t>::max() - base) {
            return std::unexpected{AllocError::size_overflow};
        }
        const std::uintptr_t current{base + cursor_};
        if (current > std::numeric_limits<std::uintptr_t>::max() - (alignment - 1)) {
            return std::unexpected{AllocError::size_overflow};
        }
        const std::uintptr_t aligned{(current + alignment - 1) & ~(alignment - 1)};
        if (aligned < base || size > std::numeric_limits<std::uintptr_t>::max() - aligned) {
            return std::unexpected{AllocError::size_overflow};
        }
        const std::size_t start{static_cast<std::size_t>(aligned - base)};
        if (size > std::numeric_limits<std::size_t>::max() - start) {
            return std::unexpected{AllocError::size_overflow};
        }
        const std::size_t end{start + size};
        if (end > N) {
            return allocate_fallback_(size, alignment, zeroed);
        }

        cursor_ = end;
        ++liveAllocations_;
        ++inlineLive_;
        auto* pointer{reinterpret_cast<std::byte*>(aligned)};
        if (zeroed) {
            std::memset(pointer, 0, size);
        }
        return Block{pointer,  size,       alignment, detail::Backend::stack_inline,
                     ownerId_, generation_};
    }

    void release_valid_(Block& block) noexcept {
        if (block.backend_ == detail::Backend::stack_inline) {
            const bool wasLast{is_last_(block)};
            if (inlineLive_ > 0) {
                --inlineLive_;
            }
            if (wasLast) {
                const auto base{reinterpret_cast<std::uintptr_t>(buffer_.data())};
                const auto pointer{reinterpret_cast<std::uintptr_t>(block.data_)};
                cursor_ = static_cast<std::size_t>(pointer - base);
            }
        } else {
            fallback_.raw_deallocate(block.data_, block.size_, block.alignment_);
        }
        if (liveAllocations_ > 0) {
            --liveAllocations_;
        }
        if (inlineLive_ == 0) {
            cursor_ = 0;
        }
        block.invalidate_();
    }

    void advance_generation_() noexcept {
        ++generation_;
        if (generation_ == 0) {
            ownerId_ = detail::next_owner_id();
            generation_ = 1;
        }
    }
public:
    explicit StackFallback(Fallback fallback = {}) noexcept : fallback_{std::move(fallback)} {}
    ~StackFallback() = default;

    StackFallback(const StackFallback&) = delete;
    StackFallback& operator=(const StackFallback&) = delete;
    StackFallback(StackFallback&&) = delete;
    StackFallback& operator=(StackFallback&&) = delete;

    [[nodiscard]] constexpr std::size_t inline_capacity() const noexcept {
        return N;
    }
    [[nodiscard]] std::size_t inline_used() const noexcept {
        return cursor_;
    }
    [[nodiscard]] std::size_t live_allocations() const noexcept {
        return liveAllocations_;
    }

    [[nodiscard]] bool uses_inline_storage(const Block& block) const noexcept {
        return token_state_(block) == TokenState::valid &&
               block.backend_ == detail::Backend::stack_inline;
    }

    [[nodiscard]] std::expected<Block, AllocError>
    allocate(std::size_t size, std::size_t alignment = DEFAULT_ALIGNMENT) noexcept {
        return allocate_(size, alignment, false);
    }

    [[nodiscard]] std::expected<Block, AllocError>
    allocate_zeroed(std::size_t size, std::size_t alignment = DEFAULT_ALIGNMENT) noexcept {
        return allocate_(size, alignment, true);
    }

    [[nodiscard]] std::expected<Block, AllocError>
    allocate_array(std::size_t count, std::size_t elementSize,
                   std::size_t alignment = DEFAULT_ALIGNMENT) noexcept {
        auto size{detail::checked_product(count, elementSize)};
        if (!size) {
            return std::unexpected{size.error()};
        }
        return allocate(*size, alignment);
    }

    [[nodiscard]] std::expected<void, AllocError> deallocate(Block& block) noexcept {
        const TokenState state{token_state_(block)};
        if (state == TokenState::stale) {
            block.invalidate_();
            return std::unexpected{AllocError::invalid_block};
        }
        if (state != TokenState::valid || liveAllocations_ == 0 ||
            (block.backend_ == detail::Backend::stack_inline && inlineLive_ == 0)) {
            return std::unexpected{AllocError::invalid_block};
        }
        release_valid_(block);
        return {};
    }

    [[nodiscard]] std::expected<void, AllocError> reallocate(Block& block,
                                                             std::size_t newSize) noexcept {
        const TokenState state{token_state_(block)};
        if (state == TokenState::stale) {
            block.invalidate_();
            return std::unexpected{AllocError::invalid_block};
        }
        if (state != TokenState::valid) {
            return std::unexpected{AllocError::invalid_block};
        }
        if (newSize == 0) {
            return deallocate(block);
        }
        if (newSize == block.size_) {
            return {};
        }

        if (block.backend_ == detail::Backend::stack_fallback) {
            void* pointer{fallback_.raw_remap(block.data_, block.size_, block.alignment_, newSize)};
            if (pointer == nullptr) {
                return std::unexpected{AllocError::out_of_memory};
            }
            block.data_ = static_cast<std::byte*>(pointer);
            block.size_ = newSize;
            return {};
        }

        if (newSize < block.size_) {
            if (is_last_(block)) {
                cursor_ -= block.size_ - newSize;
            }
            block.size_ = newSize;
            return {};
        }

        const std::size_t growth{newSize - block.size_};
        if (is_last_(block) && growth <= N - cursor_) {
            cursor_ += growth;
            block.size_ = newSize;
            return {};
        }

        auto replacementResult{allocate(newSize, block.alignment_)};
        if (!replacementResult) {
            return std::unexpected{replacementResult.error()};
        }
        Block replacement{std::move(*replacementResult)};
        std::memcpy(replacement.data_, block.data_, block.size_);
        release_valid_(block);
        block.take_from_(replacement);
        return {};
    }

    [[nodiscard]] std::expected<void, AllocError> reset() noexcept {
        if (liveAllocations_ != 0) {
            return std::unexpected{AllocError::invalid_block};
        }
        cursor_ = 0;
        inlineLive_ = 0;
        advance_generation_();
        return {};
    }
};

}  // namespace mbun::core::alloc
