// Attribute vocabulary derived from Bun's macro_rules/comptime declarations.
// The reference checkout has no standalone src/bun_core_macros directory;
// relevant declarations are distributed across bun_core/{result,env_var,
// comptime_string_map,atomic_cell}.rs and bun_core/{env_var,util}.zig.
export module mbun.bun_core_macros.attribute;

import std;

export namespace mbun::bun_core_macros {

enum class AttributeKind : std::uint8_t {
    unknown,
    derive,
    field,
    platform,
    feature,
    transparent,
};

struct Attribute {
    AttributeKind kind { AttributeKind::unknown };
    std::string_view name {};
    std::string_view value {};
};

[[nodiscard]] constexpr auto classify_attribute(std::string_view name) -> AttributeKind {
    if (name == "derive") return AttributeKind::derive;
    if (name == "field") return AttributeKind::field;
    if (name == "platform") return AttributeKind::platform;
    if (name == "feature") return AttributeKind::feature;
    if (name == "transparent") return AttributeKind::transparent;
    return AttributeKind::unknown;
}

[[nodiscard]] constexpr auto make_attribute(std::string_view name,
                                            std::string_view value = {}) -> Attribute {
    return Attribute { classify_attribute(name), name, value };
}

} // namespace mbun::bun_core_macros
