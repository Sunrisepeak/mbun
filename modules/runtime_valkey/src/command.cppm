export module mbun.runtime_valkey.command;

import std;

export namespace mbun::runtime_valkey {

enum class CommandFlag : unsigned char {
    return_as_bool = 1 << 0,
    supports_auto_pipelining = 1 << 1,
    return_as_buffer = 1 << 2,
    subscription_request = 1 << 3,
};

[[nodiscard]] constexpr auto has_flag(CommandFlag value, CommandFlag flag) noexcept -> bool {
    return (static_cast<unsigned char>(value) & static_cast<unsigned char>(flag)) != 0;
}

struct Command {
    std::string name {};
    std::vector<std::string> arguments {};
    CommandFlag flags { CommandFlag::supports_auto_pipelining };

    [[nodiscard]] auto serialize() const -> std::string {
        std::string result;
        result.reserve(name.size() + arguments.size() * 16 + 32);
        result += '*';
        result += std::to_string(arguments.size() + 1);
        result += "\r\n";
        const auto append = [&result](std::string_view value) {
            result += '$';
            result += std::to_string(value.size());
            result += "\r\n";
            result.append(value);
            result += "\r\n";
        };
        append(name);
        for (const auto& argument : arguments) append(argument);
        return result;
    }
};

struct OfflineEntry {
    std::string serialized_data {};
    CommandFlag flags { CommandFlag::supports_auto_pipelining };
};

[[nodiscard]] constexpr auto is_pipelineable(std::string_view command) noexcept -> bool {
    constexpr std::array blocked {
        std::string_view { "AUTH" }, std::string_view { "INFO" }, std::string_view { "QUIT" },
        std::string_view { "EXEC" }, std::string_view { "MULTI" }, std::string_view { "WATCH" },
        std::string_view { "SCRIPT" }, std::string_view { "SELECT" }, std::string_view { "CLUSTER" },
        std::string_view { "DISCARD" }, std::string_view { "UNWATCH" }, std::string_view { "PIPELINE" },
        std::string_view { "SUBSCRIBE" }, std::string_view { "PSUBSCRIBE" },
        std::string_view { "UNSUBSCRIBE" }, std::string_view { "UNPSUBSCRIBE" },
    };
    return std::ranges::find(blocked, command) == blocked.end();
}

[[nodiscard]] inline auto prepare(Command command) -> Command {
    if (!is_pipelineable(command.name)) {
        command.flags = static_cast<CommandFlag>(static_cast<unsigned char>(command.flags)
            & ~static_cast<unsigned char>(CommandFlag::supports_auto_pipelining));
    }
    return command;
}

}
