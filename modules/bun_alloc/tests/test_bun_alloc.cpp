import std;
import mbun.bun_alloc;

namespace alloc = mbun::bun_alloc;

int gChecks { 0 };
int gFailures { 0 };

void check(bool condition, std::string_view what) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::println("  FAIL {}", what);
    }
}

void test_allocator_and_stats() {
    alloc::Stats stats;
    alloc::Allocator allocator { stats };
    auto block { allocator.allocate(32, 16) };
    check(block.has_value(), "allocator returns an aligned block");
    if (!block) {
        return;
    }
    check(reinterpret_cast<std::uintptr_t>(block->view().data()) % 16 == 0, "block alignment");
    check(stats.allocations() == 1 && stats.bytes_in_use() == 32, "allocation stats");
    check(allocator.deallocate(*block).has_value(), "owner deallocates its block");
    check(stats.allocations() == 0 && stats.bytes_in_use() == 0, "free stats");
    check(!allocator.deallocate(*block), "double free is rejected");
}

void test_arena_generation_and_ownership() {
    alloc::Arena arena;
    auto block { arena.allocate(24, 8) };
    check(block.has_value(), "arena allocates");
    if (!block) {
        return;
    }
    auto owner { std::move(*block) };
    check(arena.owns(owner), "arena owns current token");
    const auto mark { arena.mark() };
    check(arena.reset(mark).has_value(), "arena reset accepts mark");
    check(!arena.owns(owner), "reset invalidates stale generation");
    check(!arena.deallocate(owner), "stale token is rejected");
}

void test_foreign_arena_and_maybe_owned() {
    alloc::Arena first;
    alloc::Arena second;
    auto block { first.allocate(8, 8) };
    check(block.has_value(), "foreign setup");
    if (block) {
        auto owner { std::move(*block) };
        check(!second.deallocate(owner), "foreign arena rejects token");
        check(first.deallocate(owner).has_value(), "original arena remains owner");
    }
    auto owned { alloc::MaybeOwned<int>::owned(7) };
    auto borrowed { alloc::MaybeOwned<int>::borrowed() };
    check(owned.is_owned() && !borrowed.is_owned(), "owned and borrowed seam");
    check(owned.value() == 7, "owned value");
}

int main() {
    test_allocator_and_stats();
    test_arena_generation_and_ownership();
    test_foreign_arena_and_maybe_owned();
    std::println("bun_alloc checks: {} passed, {} failed", gChecks - gFailures, gFailures);
    return gFailures == 0 ? 0 : 1;
}
