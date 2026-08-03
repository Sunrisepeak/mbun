import std;
import mbun.runtime_allocators;

namespace alloc = mbun::runtime_allocators;

int gChecks { 0 };
int gFailures { 0 };

void check(bool condition, std::string_view what) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::println("  FAIL {}", what);
    }
}

void test_selection() {
    check(alloc::select_kind({ .size = 64, .preferArena = true }) == alloc::Kind::arena,
          "arena is selected for small preferred allocations");
    check(alloc::select_kind({ .size = 1024 * 1024, .preferArena = false }) == alloc::Kind::fallback,
          "large allocations use fallback policy");
}

void test_arena_and_stats() {
    alloc::Stats stats;
    alloc::Arena arena { alloc::system_backend(), stats };
    auto first { arena.allocate(24, 16) };
    auto second { arena.allocate(48, 32) };
    check(first.has_value() && second.has_value(), "arena allocates through backend seam");
    check(first && reinterpret_cast<std::uintptr_t>(*first) % 16 == 0, "arena preserves alignment");
    check(stats.allocations() == 2 && stats.bytes_in_use() == 72, "arena records allocation stats");
    check(!arena.allocate(8, 3), "arena rejects invalid alignment");
    check(arena.reset().has_value(), "arena reset releases all records");
    check(stats.allocations() == 0 && stats.bytes_in_use() == 0, "reset clears stats");
}

void test_fallback_backend() {
    alloc::Stats stats;
    alloc::CountingBackend primary { 1 };
    alloc::FallbackAllocator fallback { primary.backend(), alloc::system_backend(), stats };
    auto block { fallback.allocate(16, 8) };
    check(block.has_value(), "fallback succeeds when primary rejects allocation");
    check(block && block->backend_name() == "system", "fallback records selected backend");
    check(primary.attempts() == 1, "primary was attempted once");
    if (block) {
        check(fallback.deallocate(*block).has_value(), "fallback deallocates through selected backend");
    }
}

int main() {
    test_selection();
    test_arena_and_stats();
    test_fallback_backend();
    std::println("runtime_allocators checks: {} passed, {} failed", gChecks - gFailures, gFailures);
    return gFailures == 0 ? 0 : 1;
}
