// Deferred generator seam. A future parser/procedural-macro adapter can emit
// SchemaEntry values here; no generated code is accepted in this first port.
export module mbun.bun_core_macros.generator;

import std;

export namespace mbun::bun_core_macros {

enum class GeneratorState : std::uint8_t { deferred };

struct GeneratorPlan {
    GeneratorState state { GeneratorState::deferred };
    std::string_view source {};
    std::string_view reason {};
};

[[nodiscard]] consteval auto deferred_generator_plan() -> GeneratorPlan {
    return { GeneratorState::deferred,
             "bun_core_macros",
             "Bun Rust/Zig macro source has no standalone directory in this checkout" };
}

} // namespace mbun::bun_core_macros
