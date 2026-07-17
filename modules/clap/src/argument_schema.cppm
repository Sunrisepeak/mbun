export module mbun.clap.argument_schema;

import std;

export namespace mbun::clap {

enum class ArgumentKind { Option, Flag, Positional };
enum class MacroSource { Explicit, ClapMacros };

struct ArgumentSpec {
    std::string_view name {};
    std::string_view long_name {};
    char short_name {};
    ArgumentKind kind {ArgumentKind::Option};
    bool required {false};
    bool multiple {false};
    MacroSource source {MacroSource::Explicit};
};

// Boundary for future #[derive(Parser)]/clap_macros output. The parser consumes
// the same schema regardless of whether it was written or generated.
struct ArgumentSchema {
    std::span<const ArgumentSpec> arguments {};
    std::span<const std::string_view> positionals {};
};

[[nodiscard]] constexpr auto matches_long(const ArgumentSpec& spec, std::string_view name) -> bool {
    return spec.long_name == name || (spec.long_name.empty() && spec.name == name);
}

[[nodiscard]] constexpr auto matches_short(const ArgumentSpec& spec, char name) -> bool {
    return spec.short_name != '\0' && spec.short_name == name;
}

}
