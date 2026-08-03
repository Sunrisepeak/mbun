export module mbun.runtime_cli.command;
import std;
import mbun.runtime_cli.config;
import mbun.runtime_cli.diagnostic;
import mbun.runtime_cli.subcommand;
namespace mbun::runtime_cli {
export struct CommandLine { std::string executable; Subcommand subcommand{Subcommand::Auto}; std::vector<std::string> positionals; std::optional<std::string> configPath; bool help{}; bool version{}; bool revision{}; };
export inline std::expected<CommandLine, Diagnostic> parse_command_line(std::span<const std::string_view> argv) {
    CommandLine result;
    if (argv.empty()) { result.executable = "mbun"; return result; }
    result.executable = argv.front();
    bool parseOptions{true};
    for (std::size_t i{1}; i < argv.size(); ++i) {
        const auto argument{argv[i]};
        if (parseOptions && argument == "--") { parseOptions = false; continue; }
        if (parseOptions && (argument == "-h" || argument == "--help")) { result.help = true; continue; }
        if (parseOptions && (argument == "-v" || argument == "--version")) { result.version = true; continue; }
        if (parseOptions && argument == "--revision") { result.revision = true; continue; }
        if (parseOptions && (argument == "-c" || argument == "--config")) {
            if (++i == argv.size() || argv[i].empty() || argv[i].front() == '-') return std::unexpected(missing_value(argument, i - 1));
            result.configPath = std::string{argv[i]}; continue;
        }
        if (parseOptions && argument.starts_with('-')) return std::unexpected(invalid_option(argument, i));
        result.positionals.emplace_back(argument);
    }
    if (!result.positionals.empty()) {
        const auto candidate{parse_subcommand(result.positionals.front())};
        if (candidate) { result.subcommand = *candidate; result.positionals.erase(result.positionals.begin()); }
        else if (result.positionals.size() > 1 || result.positionals.front().find('.') == std::string::npos) return std::unexpected(unknown_command(result.positionals.front(), 1));
    }
    if (result.help && result.subcommand == Subcommand::Auto) result.subcommand = Subcommand::Help;
    return result;
}
}
