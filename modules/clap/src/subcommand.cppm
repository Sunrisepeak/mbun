export module mbun.clap.subcommand;

import std;
import mbun.clap.error;

export namespace mbun::clap {

struct Subcommand {
    std::string_view name {};
    std::span<const std::string_view> aliases {};
};

[[nodiscard]] auto select_subcommand(std::span<const Subcommand> commands, std::string_view token)
    -> std::expected<const Subcommand*, Error> {
    for (const auto& command : commands) {
        if (command.name == token || std::ranges::find(command.aliases, token) != command.aliases.end())
            return &command;
    }
    return std::unexpected(Error {ErrorCode::UnknownSubcommand, token, token});
}

}
