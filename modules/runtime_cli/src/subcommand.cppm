export module mbun.runtime_cli.subcommand;
import std;
namespace mbun::runtime_cli {
export enum class Subcommand : std::uint8_t { Auto, Help, Run, Test, Install, Add, Remove, Build, Bunx, Create, Init, Upgrade };
export inline std::string_view subcommand_name(Subcommand c) {
    switch (c) {
    case Subcommand::Auto: return ""; case Subcommand::Help: return "help"; case Subcommand::Run: return "run";
    case Subcommand::Test: return "test"; case Subcommand::Install: return "install"; case Subcommand::Add: return "add";
    case Subcommand::Remove: return "remove"; case Subcommand::Build: return "build"; case Subcommand::Bunx: return "bunx";
    case Subcommand::Create: return "create"; case Subcommand::Init: return "init"; case Subcommand::Upgrade: return "upgrade";
    }
    return "";
}
export inline std::optional<Subcommand> parse_subcommand(std::string_view name) {
    constexpr std::array known { std::pair{"help", Subcommand::Help}, std::pair{"run", Subcommand::Run}, std::pair{"test", Subcommand::Test}, std::pair{"install", Subcommand::Install}, std::pair{"add", Subcommand::Add}, std::pair{"remove", Subcommand::Remove}, std::pair{"build", Subcommand::Build}, std::pair{"bunx", Subcommand::Bunx}, std::pair{"create", Subcommand::Create}, std::pair{"init", Subcommand::Init}, std::pair{"upgrade", Subcommand::Upgrade} };
    for (const auto& [candidate, command] : known) if (candidate == name) return command;
    return std::nullopt;
}
}
