// capability.cppm — runtime-independent host capability seam.
// PORT-SOURCE: Bun Rust `src/runtime/api/*` and Zig `src/runtime/api/*`.
export module mbun.api.capability;
import std;
namespace mbun::api {
export enum class Capability : std::uint64_t { None = 0, Values = 1ULL << 0, Errors = 1ULL << 1, Scheduling = 1ULL << 2, Filesystem = 1ULL << 3, Network = 1ULL << 4 };
export struct CapabilitySet {
    std::uint64_t bits{0};
    constexpr bool contains(Capability capability) const noexcept { return (bits & static_cast<std::uint64_t>(capability)) == static_cast<std::uint64_t>(capability); }
    constexpr CapabilitySet with(Capability capability) const noexcept { return {bits | static_cast<std::uint64_t>(capability)}; }
};
export using ValueHandle = std::uint64_t;
export struct CapabilityTable {
    void* userData{nullptr};
    CapabilitySet provided{};
    bool (*is_kind)(void*, ValueHandle, std::uint8_t) noexcept{nullptr};
    void (*throw_error)(void*, std::string_view) noexcept{nullptr};
    void (*schedule)(void*, void (*)(void*) noexcept, void*) noexcept{nullptr};
    constexpr bool has(Capability capability) const noexcept { return provided.contains(capability); }
};
}
