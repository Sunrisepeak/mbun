export module mbun.runtime_cli.config;
import std;
namespace mbun::runtime_cli {
export enum class ConfigSource : std::uint8_t { Default, Project, Environment, CommandLine };
export struct ConfigValue { std::string path; ConfigSource source{ConfigSource::Default}; };
export struct ConfigInputs { std::optional<std::string_view> projectPath; std::optional<std::string_view> environmentPath; std::optional<std::string_view> commandLinePath; };
export inline ConfigValue resolve_config(const ConfigInputs& i, std::string_view defaultPath = "bunfig.toml") {
    if (i.commandLinePath) return {std::string{*i.commandLinePath}, ConfigSource::CommandLine};
    if (i.environmentPath) return {std::string{*i.environmentPath}, ConfigSource::Environment};
    if (i.projectPath) return {std::string{*i.projectPath}, ConfigSource::Project};
    return {std::string{defaultPath}, ConfigSource::Default};
}
}
