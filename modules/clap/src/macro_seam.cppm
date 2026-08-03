export module mbun.clap.macro_seam;

import std;
import mbun.clap.argument_schema;

export namespace mbun::clap {

struct MacroArgument {
    std::string_view field {};
    std::string_view cli_name {};
    ArgumentKind kind {ArgumentKind::Option};
    char short_name {};
};

// Keeps macro expansion independent from parsing. A generated adapter can
// materialize ArgumentSpec entries without changing parser or error behavior.
[[nodiscard]] constexpr auto expand_macro_argument(const MacroArgument& argument) -> ArgumentSpec {
    return ArgumentSpec {.name = argument.field,
                          .long_name = argument.cli_name,
                          .short_name = argument.short_name,
                          .kind = argument.kind,
                          .source = MacroSource::ClapMacros};
}

}
