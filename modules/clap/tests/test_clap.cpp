import std;
import mbun.clap;

namespace {
int checks {};
int failures {};
void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) { ++failures; std::println("FAIL: {}", message); }
}
}

int main() {
    using namespace mbun::clap;
    const auto generated {expand_macro_argument(MacroArgument {.field = "verbose", .cli_name = "verbose", .kind = ArgumentKind::Flag, .short_name = 'v'})};
    const ArgumentSpec file {.name = "file", .long_name = "file", .kind = ArgumentKind::Option, .required = true};
    const ArgumentSpec verbose {.name = "verbose", .long_name = "verbose", .short_name = 'v', .kind = ArgumentKind::Flag};
    const std::array specs {file, verbose};
    const ArgumentSchema schema {.arguments = specs};
    const std::array argv {std::string_view {"--file=main.js"}, std::string_view {"-v"}, std::string_view {"tail"}};
    const auto parsed {parse(schema, argv)};
    check(generated.source == MacroSource::ClapMacros, "macro source seam");
    check(parsed.has_value(), "long and short parse");
    check(parsed && parsed->arguments.size() == 2, "two options captured");
    check(parsed && parsed->arguments[0].value == "main.js", "equals value is zero-copy");
    check(parsed && parsed->positionals[0] == "tail", "positional captured");

    const std::array missing {std::string_view {"-v"}};
    check(!parse(schema, missing).has_value(), "required argument enforced");
    const std::array bad {std::string_view {"--nope"}};
    check(parse(schema, bad).error().code == ErrorCode::UnknownArgument, "unknown argument error");
    const std::array no_value {std::string_view {"--file"}};
    check(parse(schema, no_value).error().code == ErrorCode::MissingValue, "missing value error");
    const std::array aliases {std::string_view {"r"}};
    const std::array commands {Subcommand {.name = "run", .aliases = aliases}, Subcommand {.name = "test"}};
    const auto selected {select_subcommand(commands, "r")};
    check(selected && selected.value()->name == "run", "subcommand alias selected");
    check(select_subcommand(commands, "build").error().code == ErrorCode::UnknownSubcommand, "unknown subcommand error");
    std::println("clap checks: {}, failures: {}", checks, failures);
    return failures == 0 ? 0 : 1;
}
