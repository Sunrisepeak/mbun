// test_core_alloc.cpp — T1.1 mbun.core.alloc allocator and capability safety.
//
// T1.1 has no corresponding bun JavaScript test. These white-box C++ vectors
// preserve the contracts in bun's Rust/Zig allocator implementations while
// pinning the additional type-safety required by the MC++ API.
import std;
import mbun.core.alloc;

namespace alloc = mbun::core::alloc;

namespace {

template <class T>
concept GlobalDeallocatable = requires(T& value) {
    { alloc::deallocate(value) } -> std::same_as<std::expected<void, alloc::AllocError>>;
};

template <class T>
concept GlobalReallocatable = requires(T& value) {
    { alloc::reallocate(value, 8) } -> std::same_as<std::expected<void, alloc::AllocError>>;
};

static_assert(!std::is_aggregate_v<alloc::Block>);
static_assert(!std::is_copy_constructible_v<alloc::Block>);
static_assert(!std::is_copy_assignable_v<alloc::Block>);
static_assert(std::is_move_constructible_v<alloc::Block>);
static_assert(!std::is_move_assignable_v<alloc::Block>);
static_assert(!std::is_constructible_v<alloc::Block, std::byte*, std::size_t, std::size_t>);
static_assert(std::is_copy_constructible_v<alloc::BlockView>);
static_assert(!GlobalDeallocatable<alloc::BlockView>);
static_assert(!GlobalReallocatable<alloc::BlockView>);
static_assert(
    std::same_as<decltype(std::declval<const alloc::OwnedBlock&>().block()), alloc::BlockView>);

int gChecks{0};
int gFailures{0};

void check(bool condition, std::string_view what) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::println("  FAIL {}", what);
    }
}

void check_error(const auto& result, alloc::AllocError expected, std::string_view what) {
    check(!result && result.error() == expected, what);
}

void fill(alloc::BlockView view, std::byte value) {
    std::ranges::fill(view.bytes(), value);
}

bool prefix_is(alloc::BlockView view, std::size_t count, std::byte value) {
    return view.size() >= count &&
           std::ranges::all_of(view.bytes().first(count),
                               [value](std::byte byte) { return byte == value; });
}

bool overlaps(alloc::BlockView lhs, alloc::BlockView rhs) {
    const auto lhsBegin{reinterpret_cast<std::uintptr_t>(lhs.data())};
    const auto rhsBegin{reinterpret_cast<std::uintptr_t>(rhs.data())};
    return lhsBegin < rhsBegin + rhs.size() && rhsBegin < lhsBegin + lhs.size();
}

void test_alignment_and_overflow_validation() {
    check(alloc::is_valid_alignment(1), "alignment 1 is valid");
    check(alloc::is_valid_alignment(64), "alignment 64 is valid");
    check(!alloc::is_valid_alignment(0), "alignment 0 is invalid");
    check(!alloc::is_valid_alignment(3), "non-power-of-two alignment is invalid");

    check_error(alloc::allocate(8, 0), alloc::AllocError::invalid_alignment,
                "global allocation rejects zero alignment");
    check_error(alloc::allocate(8, 3), alloc::AllocError::invalid_alignment,
                "global allocation rejects non-power-of-two alignment");
    check_error(alloc::allocate_array(std::numeric_limits<std::size_t>::max(), 2),
                alloc::AllocError::size_overflow, "global array multiplication overflow");

    alloc::Arena arena;
    check_error(arena.allocate_array(std::numeric_limits<std::size_t>::max(), 2),
                alloc::AllocError::size_overflow, "arena array multiplication overflow");

    alloc::StackFallback<64> stack;
    check_error(stack.allocate(std::numeric_limits<std::size_t>::max(), 16),
                alloc::AllocError::size_overflow, "stack cursor arithmetic overflow");
}

void test_global_mimalloc_and_unique_token() {
    for (std::size_t alignment : {1uz, 8uz, 16uz, 64uz, 4096uz}) {
        auto result{alloc::allocate(97, alignment)};
        check(result.has_value(), std::format("global allocate alignment {}", alignment));
        if (!result) {
            continue;
        }
        alloc::Block block{std::move(*result)};
        const alloc::BlockView view{block.view()};
        check(reinterpret_cast<std::uintptr_t>(view.data()) % alignment == 0,
              std::format("global pointer alignment {}", alignment));
        check(view.size() == 97 && view.alignment() == alignment,
              std::format("global block metadata {}", alignment));
        fill(view, std::byte{0xA5});
        check(alloc::deallocate(block).has_value(), "global deallocate accepts owner token");
        check(block.empty(), "global deallocate consumes owner token");
        check_error(alloc::deallocate(block), alloc::AllocError::invalid_block,
                    "double deallocate is diagnosed without touching allocator state");
    }

    auto zeroed{alloc::allocate_zeroed(257, 64)};
    check(zeroed.has_value(), "global zeroed allocation succeeds");
    if (zeroed) {
        alloc::Block block{std::move(*zeroed)};
        check(prefix_is(block.view(), block.view().size(), std::byte{0}),
              "global zeroed allocation is zero-filled");
        check(alloc::deallocate(block).has_value(), "global zeroed deallocate");
    }

    auto result{alloc::allocate(32, 64)};
    check(result.has_value(), "global resize setup");
    if (result) {
        alloc::Block block{std::move(*result)};
        fill(block.view(), std::byte{0x5A});
        check(alloc::reallocate(block, 8192).has_value(), "global grow succeeds");
        check(reinterpret_cast<std::uintptr_t>(block.view().data()) % 64 == 0,
              "global grow preserves alignment");
        check(prefix_is(block.view(), 32, std::byte{0x5A}), "global grow preserves prefix");
        check(alloc::reallocate(block, 8).has_value() && block.view().size() == 8,
              "global shrink succeeds");
        check(prefix_is(block.view(), 8, std::byte{0x5A}), "global shrink preserves prefix");
        check(alloc::deallocate(block).has_value(), "global resized deallocate");
    }
}

void test_owned_block_exposes_view_only() {
    auto result{alloc::OwnedBlock::allocate_zeroed(128, 128)};
    check(result.has_value(), "OwnedBlock zeroed construction");
    if (!result) {
        return;
    }

    alloc::OwnedBlock owned{std::move(*result)};
    check(owned && owned.block().size() == 128, "OwnedBlock owns allocation");
    check(reinterpret_cast<std::uintptr_t>(owned.block().data()) % 128 == 0,
          "OwnedBlock view preserves requested alignment");
    fill(owned.block(), std::byte{0x3C});

    alloc::OwnedBlock moved{std::move(owned)};
    check(!owned && moved, "OwnedBlock move transfers ownership");
    check(moved.resize(4096).has_value(), "OwnedBlock resize succeeds");
    check(prefix_is(moved.block(), 128, std::byte{0x3C}), "OwnedBlock resize preserves prefix");

    alloc::Block released{moved.release()};
    check(!moved && released.view().size() == 4096,
          "OwnedBlock release transfers the unique owner token");
    check(alloc::deallocate(released).has_value(), "released token deallocates once");
}

void test_foreign_allocator_rejection() {
    alloc::Arena first;
    alloc::Arena second;
    auto arenaResult{first.allocate(64, 16)};
    check(arenaResult.has_value(), "cross-arena setup");
    if (arenaResult) {
        alloc::Block block{std::move(*arenaResult)};
        check_error(second.reallocate(block, 128), alloc::AllocError::invalid_block,
                    "cross-arena reallocate is rejected before mimalloc");
        check_error(second.deallocate(block), alloc::AllocError::invalid_block,
                    "cross-arena deallocate is rejected before mimalloc");
        check_error(alloc::reallocate(block, 128), alloc::AllocError::invalid_block,
                    "arena block is rejected by global reallocate");
        check_error(alloc::deallocate(block), alloc::AllocError::invalid_block,
                    "arena block is rejected by global deallocate");
        check(first.owns(block), "foreign rejection leaves token valid for its owner");
        check(first.deallocate(block).has_value(), "owning arena can still deallocate token");
    }

    auto globalResult{alloc::allocate(64, 16)};
    check(globalResult.has_value(), "global-to-arena setup");
    if (globalResult) {
        alloc::Block block{std::move(*globalResult)};
        check_error(first.reallocate(block, 128), alloc::AllocError::invalid_block,
                    "global block is rejected by arena reallocate");
        check_error(first.deallocate(block), alloc::AllocError::invalid_block,
                    "global block is rejected by arena deallocate");
        check(alloc::deallocate(block).has_value(), "global owner remains usable");
    }
}

void test_arena_reset_stale_and_move() {
    alloc::Arena arena;
    check(arena.valid(), "Arena creates an independent mimalloc heap");

    auto staleResult{arena.allocate(1024, 64)};
    check(staleResult.has_value(), "Arena reset stale setup");
    if (staleResult) {
        alloc::Block stale{std::move(*staleResult)};
        check(arena.owns(stale), "Arena owns live token before reset");
        check(arena.reset().has_value(), "Arena reset bulk-frees and advances generation");
        check(!arena.owns(stale), "Arena rejects stale token after reset without pointer probing");
        check_error(arena.reallocate(stale, 2048), alloc::AllocError::invalid_block,
                    "stale reallocate is diagnosed before mimalloc");
        check(stale.empty(), "stale diagnostic consumes dead token");
    }

    auto movedResult{arena.allocate(80, 16)};
    check(movedResult.has_value(), "Arena move setup");
    if (movedResult) {
        alloc::Block block{std::move(*movedResult)};
        alloc::Arena moved{std::move(arena)};
        check(!arena.valid() && moved.valid(), "Arena move transfers heap and owner identity");
        check(moved.owns(block), "pre-move token is valid on moved-to Arena");
        check(!arena.owns(block), "moved-from Arena rejects transferred token");
        check(moved.deallocate(block).has_value(), "moved-to Arena deallocates token");
    }
}

void test_arena_cross_thread_free() {
    alloc::Arena arena;
    auto ownerResult{arena.allocate(32, 8)};
    check(ownerResult.has_value(), "Arena cross-thread free setup");
    if (!ownerResult) {
        return;
    }
    alloc::Block block{std::move(*ownerResult)};

    bool deallocated{false};
    std::thread worker{[&] { deallocated = arena.deallocate(block).has_value(); }};
    worker.join();
    check(deallocated && block.empty(), "Arena permits mimalloc-style cross-thread free");
}

void test_arena_wrong_thread_alloc_and_realloc() {
    if constexpr (!alloc::ARENA_THREAD_GUARDS_ENABLED) {
        return;
    }

    alloc::Arena arena;
    auto ownerResult{arena.allocate(32, 8)};
    check(ownerResult.has_value(), "Arena wrong-thread setup");
    if (!ownerResult) {
        return;
    }
    alloc::Block block{std::move(*ownerResult)};

    std::array observed{alloc::AllocError::invalid_block, alloc::AllocError::invalid_block};
    std::thread worker{[&] {
        auto allocated{arena.allocate(32, 8)};
        observed[0] = allocated ? alloc::AllocError::out_of_memory : allocated.error();
        auto reallocated{arena.reallocate(block, 64)};
        observed[1] = reallocated ? alloc::AllocError::out_of_memory : reallocated.error();
    }};
    worker.join();
    check(std::ranges::all_of(observed,
                              [](alloc::AllocError error) {
                                  return error == alloc::AllocError::wrong_thread;
                              }),
          "Arena guards only wrong-thread alloc and realloc paths");
    check(arena.owns(block), "wrong-thread realloc leaves owner token untouched");
    check(arena.deallocate(block).has_value(), "owner token remains freeable after rejection");
}

void test_arena_move_reset_restamps_owner() {
    if constexpr (!alloc::ARENA_THREAD_GUARDS_ENABLED) {
        return;
    }

    alloc::Arena arena;
    auto oldResult{arena.allocate(48, 16)};
    check(oldResult.has_value(), "Arena move-reset setup");
    if (!oldResult) {
        return;
    }

    struct Results {
        alloc::AllocError beforeReset{alloc::AllocError::invalid_block};
        bool reset{false};
        bool staleRejected{false};
        bool allocateAfterReset{false};
        bool reallocateAfterReset{false};
        bool deallocateAfterReset{false};
    } results;

    std::thread worker{[arena = std::move(arena), old = std::move(*oldResult), &results]() mutable {
        auto beforeReset{arena.allocate(8, 8)};
        results.beforeReset = beforeReset ? alloc::AllocError::out_of_memory : beforeReset.error();
        results.reset = arena.reset().has_value();
        auto stale{arena.reallocate(old, 96)};
        results.staleRejected =
            !stale && stale.error() == alloc::AllocError::invalid_block && old.empty();

        auto freshResult{arena.allocate(64, 16)};
        results.allocateAfterReset = freshResult.has_value();
        if (!freshResult) {
            return;
        }
        alloc::Block fresh{std::move(*freshResult)};
        results.reallocateAfterReset = arena.reallocate(fresh, 128).has_value();
        results.deallocateAfterReset = arena.deallocate(fresh).has_value();
    }};
    worker.join();

    check(results.beforeReset == alloc::AllocError::wrong_thread,
          "moved Arena requires reset before allocating on its new thread");
    check(results.reset && results.staleRejected,
          "cross-thread reset bulk-frees old heap and invalidates its tokens");
    check(results.allocateAfterReset && results.reallocateAfterReset &&
              results.deallocateAfterReset,
          "reset restamps moved Arena for alloc/realloc/free on the new thread");
}

void test_stack_unique_non_lifo_and_underflow() {
    alloc::StackFallback<128> stack;
    auto firstResult{stack.allocate(16, 1)};
    auto secondResult{stack.allocate(16, 1)};
    check(firstResult.has_value() && secondResult.has_value(), "StackFallback safety setup");
    if (!firstResult || !secondResult) {
        return;
    }

    alloc::Block first{std::move(*firstResult)};
    alloc::Block second{std::move(*secondResult)};
    const alloc::BlockView secondView{second.view()};
    const std::size_t usedBefore{stack.inline_used()};
    check(stack.deallocate(first).has_value(), "non-LIFO first free succeeds");
    check(stack.inline_used() == usedBefore, "non-LIFO free does not rewind live tail");
    check_error(stack.deallocate(first), alloc::AllocError::invalid_block,
                "double free cannot underflow live counters");
    check(stack.live_allocations() == 1, "double free leaves live count unchanged");

    auto thirdResult{stack.allocate(16, 1)};
    check(thirdResult.has_value(), "allocation after non-LIFO free succeeds");
    if (thirdResult) {
        alloc::Block third{std::move(*thirdResult)};
        check(!overlaps(secondView, third.view()),
              "new allocation cannot overlap still-live non-LIFO tail");
        check(stack.deallocate(second).has_value(), "middle token deallocates");
        check(stack.deallocate(third).has_value(), "last token deallocates");
    }
    check(stack.live_allocations() == 0 && stack.inline_used() == 0,
          "all unique tokens reclaimed without underflow");
}

void test_stack_foreign_reset_resize_and_fallback() {
    alloc::StackFallback<128> first;
    alloc::StackFallback<128> second;
    auto result{first.allocate(16, 8)};
    check(result.has_value(), "StackFallback foreign setup");
    if (result) {
        alloc::Block block{std::move(*result)};
        fill(block.view(), std::byte{0xC7});
        std::byte* const original{block.view().data()};
        check_error(second.deallocate(block), alloc::AllocError::invalid_block,
                    "foreign StackFallback cannot consume owner token");
        check_error(second.reallocate(block, 48), alloc::AllocError::invalid_block,
                    "foreign StackFallback cannot resize owner token");
        check_error(first.reset(), alloc::AllocError::invalid_block,
                    "StackFallback reset rejects live allocations");
        check(first.reallocate(block, 48).has_value() && block.view().data() == original,
              "last inline grow stays in place");
        check(prefix_is(block.view(), 16, std::byte{0xC7}), "inline grow preserves prefix");
        check(first.reallocate(block, 4).has_value() && block.view().data() == original,
              "last inline shrink stays in place");
        check(first.deallocate(block).has_value(), "resized inline token deallocates");
        check(first.reset().has_value(), "StackFallback reset succeeds when no token is live");
    }

    auto spillResult{first.allocate(96, 16)};
    check(spillResult.has_value(), "StackFallback spill setup");
    if (spillResult) {
        alloc::Block block{std::move(*spillResult)};
        fill(block.view(), std::byte{0xE1});
        check(first.reallocate(block, 512).has_value(), "inline grow spills to fallback");
        check(!first.uses_inline_storage(block), "spilled grow is fallback-owned");
        check(prefix_is(block.view(), 96, std::byte{0xE1}), "spilled grow preserves prefix");
        check(first.deallocate(block).has_value(), "fallback token deallocates through owner");
    }
}

void test_stack_zero_size_at_end() {
    alloc::StackFallback<64> stack;
    auto fullResult{stack.allocate(64, 1)};
    auto zeroResult{stack.allocate(0, 1)};
    check(fullResult.has_value() && zeroResult.has_value(),
          "zero-size allocation succeeds at inline end");
    if (fullResult && zeroResult) {
        alloc::Block full{std::move(*fullResult)};
        alloc::Block zero{std::move(*zeroResult)};
        check(zero.view().size() == 0 && zero.view().data() != nullptr,
              "zero-size allocation has a deallocatable sentinel");
        check(!stack.uses_inline_storage(zero),
              "zero-size sentinel has unambiguous fallback owner");
        check(stack.deallocate(zero).has_value(), "zero-size allocation deallocates safely");
        check(stack.deallocate(full).has_value(), "full inline allocation deallocates");
    }
}

struct FallbackCounts {
    std::size_t allocations{0};
    std::size_t remaps{0};
    std::size_t deallocations{0};
};

class CountingFallback {
private:
    FallbackCounts* counts_{nullptr};
    alloc::GlobalFallbackBackend backend_{};
public:
    explicit CountingFallback(FallbackCounts& counts) noexcept : counts_{&counts} {}

    [[nodiscard]] void* raw_allocate(std::size_t size, std::size_t alignment,
                                     bool zeroed = false) noexcept {
        ++counts_->allocations;
        return backend_.raw_allocate(size, alignment, zeroed);
    }

    [[nodiscard]] bool raw_resize(std::byte* data, std::size_t oldSize, std::size_t alignment,
                                  std::size_t newSize) noexcept {
        return backend_.raw_resize(data, oldSize, alignment, newSize);
    }

    [[nodiscard]] void* raw_remap(std::byte* data, std::size_t oldSize, std::size_t alignment,
                                  std::size_t newSize) noexcept {
        ++counts_->remaps;
        return backend_.raw_remap(data, oldSize, alignment, newSize);
    }

    void raw_deallocate(void* data, std::size_t size, std::size_t alignment) noexcept {
        ++counts_->deallocations;
        backend_.raw_deallocate(data, size, alignment);
    }
};

void test_borrowed_buffer_fallback() {
    // alignas(16) is load-bearing, not decoration. fixed_allocate_ aligns the
    // ABSOLUTE address and sets cursor_ = start + size, so `used()` counts any
    // padding it had to skip. A std::byte array is only alignment-1, and the
    // exact figures below (used() == 32, then == 0) hold only when the base is
    // already 16-aligned. That was true by luck of the stack layout on linux
    // x86_64 and false on macOS arm64, where both checks failed -- a latent
    // assumption in the test, not a defect in the allocator. Stating the
    // alignment makes the arithmetic deterministic everywhere; the unaligned
    // case the failure exposed is covered separately below.
    alignas(16) std::array<std::byte, 64> storage{};
    FallbackCounts counts{};
    alloc::BufferFallbackAllocator fallback{std::span{storage}, CountingFallback{counts}};

    auto* first{static_cast<std::byte*>(fallback.raw_allocate(16, 16, true))};
    check(first != nullptr && fallback.owns(first) && counts.allocations == 0,
          "borrowed buffer serves allocations before fallback");
    check(fallback.raw_resize(first, 16, 16, 32) && fallback.used() == 32,
          "last fixed allocation grows in place");
    check(!fallback.raw_resize(first, 32, 16, 80),
          "fixed allocation cannot grow beyond borrowed capacity");

    auto* spilled{static_cast<std::byte*>(fallback.raw_allocate(96, 64))};
    check(spilled != nullptr && !fallback.owns(spilled) && counts.allocations == 1,
          "borrowed buffer delegates overflow to injected fallback");
    if (spilled != nullptr) {
        auto* remapped{static_cast<std::byte*>(fallback.raw_remap(spilled, 96, 64, 128))};
        check(remapped != nullptr && counts.remaps == 1,
              "foreign remap delegates to injected fallback");
        if (remapped != nullptr) {
            fallback.raw_deallocate(remapped, 128, 64);
        }
    }
    check(counts.deallocations == 1, "foreign free delegates to injected fallback");

    fallback.raw_deallocate(first, 32, 16);
    check(fallback.used() == 0, "last fixed free rewinds borrowed cursor");
    fallback.reset();
    check(fallback.used() == 0 && fallback.capacity() == storage.size(),
          "borrowed fallback reset preserves capacity");
}

// The case above was accidentally never exercised until macOS produced it: a
// borrowed buffer whose base is NOT already aligned to the requested alignment.
// Asserted as relationships rather than constants, since the padding depends on
// where the array lands.
void test_borrowed_buffer_unaligned_base() {
    alignas(16) std::array<std::byte, 96> backing{};
    // +1 guarantees a base that cannot satisfy alignment 16 without padding.
    const std::span<std::byte> storage{backing.data() + 1, backing.size() - 1};
    FallbackCounts counts{};
    alloc::BufferFallbackAllocator fallback{storage, CountingFallback{counts}};

    auto* first{static_cast<std::byte*>(fallback.raw_allocate(16, 16, true))};
    check(first != nullptr && fallback.owns(first) && counts.allocations == 0,
          "unaligned borrowed buffer still serves from the buffer, not the fallback");
    if (first == nullptr) {
        return;
    }
    check(reinterpret_cast<std::uintptr_t>(first) % 16 == 0,
          "unaligned borrowed buffer still returns an aligned pointer");

    const std::size_t pad{static_cast<std::size_t>(first - storage.data())};
    check(pad > 0 && fallback.used() == pad + 16,
          "used() accounts for the padding skipped to reach alignment");
    check(fallback.raw_resize(first, 16, 16, 32) && fallback.used() == pad + 32,
          "last fixed allocation grows in place from an unaligned base");

    fallback.raw_deallocate(first, 32, 16);
    check(fallback.used() == pad,
          "freeing the last fixed allocation rewinds to its start, padding included");
    fallback.reset();
    check(fallback.used() == 0 && fallback.capacity() == storage.size(),
          "reset clears the padding too");
}

void test_stack_injected_fallback() {
    FallbackCounts counts{};
    alloc::StackFallback<32, CountingFallback> stack{CountingFallback{counts}};
    auto result{stack.allocate(96, 16)};
    check(result.has_value() && counts.allocations == 1,
          "StackFallback delegates overflow to injected backend");
    if (!result) {
        return;
    }

    alloc::Block block{std::move(*result)};
    check(stack.reallocate(block, 128).has_value() && counts.remaps == 1,
          "StackFallback delegates fallback remap");
    check(stack.deallocate(block).has_value() && counts.deallocations == 1,
          "StackFallback delegates fallback free");
}

}  // namespace

int main() {
    test_alignment_and_overflow_validation();
    test_global_mimalloc_and_unique_token();
    test_owned_block_exposes_view_only();
    test_foreign_allocator_rejection();
    test_arena_reset_stale_and_move();
    test_arena_cross_thread_free();
    test_arena_wrong_thread_alloc_and_realloc();
    test_arena_move_reset_restamps_owner();
    test_stack_unique_non_lifo_and_underflow();
    test_stack_foreign_reset_resize_and_fallback();
    test_stack_zero_size_at_end();
    test_borrowed_buffer_fallback();
    test_borrowed_buffer_unaligned_base();
    test_stack_injected_fallback();

    std::println("test_core_alloc: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
