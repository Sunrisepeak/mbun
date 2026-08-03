export module mbun.runtime_allocators.allocator_selection;

import std;

export namespace mbun::runtime_allocators {

enum class Kind : std::uint8_t { system, arena, fallback };

struct SelectionOptions {
    std::size_t size { 0 };
    bool preferArena { false };
};

[[nodiscard]] constexpr Kind select_kind(SelectionOptions options) noexcept {
    if (options.preferArena && options.size < 64 * 1024) {
        return Kind::arena;
    }
    if (options.size >= 1024 * 1024) {
        return Kind::fallback;
    }
    return Kind::system;
}

}
