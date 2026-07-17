// Opt-in sanitizer oracle. The normal test run exits successfully; the
// explicit environment modes intentionally trigger ASAN/LSAN and must exit
// nonzero when the sanitizer profile is wired correctly.
import std;
import mbun.core.alloc;

namespace alloc = mbun::core::alloc;

namespace {

void leak_one_block() {
    auto result{alloc::allocate(4096, 64)};
    if (!result) {
        std::exit(4);
    }
    alloc::Block leaked{std::move(*result)};
    std::ranges::fill(leaked.view().bytes(), std::byte{0x5A});
}

[[gnu::noinline]] void leak_one_arena() {
    auto* arena{new (std::nothrow) alloc::Arena};
    if (arena == nullptr) {
        std::exit(4);
    }
    auto result{arena->allocate(4096, 64)};
    if (!result) {
        std::exit(4);
    }
    alloc::Block leaked{std::move(*result)};
    std::ranges::fill(leaked.view().bytes(), std::byte{0xA5});
}

}  // namespace

int main() {
    if (std::getenv("MBUN_ASAN_GLOBAL_UAF_PROBE") != nullptr) {
        if (!alloc::SANITIZER_SYSTEM_ALLOCATOR) {
            return 3;
        }
        auto result{alloc::allocate(64, 16)};
        if (!result) {
            return 4;
        }
        alloc::Block block{std::move(*result)};
        volatile std::byte* pointer{block.view().data()};
        if (!alloc::deallocate(block)) {
            return 5;
        }
        return std::to_integer<int>(pointer[0]);  // intentional UAF
    }

    if (std::getenv("MBUN_ASAN_ARENA_DEALLOC_UAF_PROBE") != nullptr) {
        if (!alloc::SANITIZER_SYSTEM_ALLOCATOR) {
            return 3;
        }
        alloc::Arena arena;
        auto result{arena.allocate(64, 16)};
        if (!result) {
            return 4;
        }
        alloc::Block block{std::move(*result)};
        if (!arena.reallocate(block, 128)) {
            return 5;
        }
        volatile std::byte* pointer{block.view().data()};
        if (!arena.deallocate(block)) {
            return 5;
        }
        return std::to_integer<int>(pointer[0]);  // intentional Arena UAF
    }

    if (std::getenv("MBUN_ASAN_ARENA_RESET_UAF_PROBE") != nullptr) {
        if (!alloc::SANITIZER_SYSTEM_ALLOCATOR) {
            return 3;
        }
        alloc::Arena arena;
        auto result{arena.allocate(64, 16)};
        if (!result) {
            return 4;
        }
        alloc::Block block{std::move(*result)};
        volatile std::byte* pointer{block.view().data()};
        if (!arena.reset()) {
            return 5;
        }
        return std::to_integer<int>(pointer[0]);  // intentional post-reset UAF
    }

    if (std::getenv("MBUN_LSAN_GLOBAL_LEAK_PROBE") != nullptr) {
        if (!alloc::SANITIZER_SYSTEM_ALLOCATOR) {
            return 3;
        }
        leak_one_block();  // intentional leak
    }

    if (std::getenv("MBUN_LSAN_ARENA_LEAK_PROBE") != nullptr) {
        if (!alloc::SANITIZER_SYSTEM_ALLOCATOR) {
            return 3;
        }
        leak_one_arena();  // intentional Arena + allocation leak
    }

    return 0;
}
