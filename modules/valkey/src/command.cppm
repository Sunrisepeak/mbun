export module mbun.valkey.command;

import std;

export namespace mbun::valkey {

enum class CommandMeta : unsigned char {
    none = 0,
    return_as_bool = 1 << 0,
    supports_auto_pipelining = 1 << 1,
    return_as_buffer = 1 << 2,
    subscription_request = 1 << 3,
};

constexpr auto operator|(CommandMeta left, CommandMeta right) noexcept -> CommandMeta {
    return static_cast<CommandMeta>(static_cast<unsigned char>(left) | static_cast<unsigned char>(right));
}

[[nodiscard]] constexpr auto has_meta(CommandMeta value, CommandMeta bit) noexcept -> bool {
    return (static_cast<unsigned char>(value) & static_cast<unsigned char>(bit)) != 0;
}

class Command {
public:
    Command() = default;
    explicit Command(std::string_view name, std::span<const std::string_view> arguments = {})
        : name_(name), arguments_(arguments.begin(), arguments.end()) {}

    [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }
    [[nodiscard]] auto arguments() const noexcept -> const std::vector<std::string_view>& { return arguments_; }
    [[nodiscard]] auto meta() const noexcept -> CommandMeta { return meta_; }
    auto set_meta(CommandMeta meta) noexcept -> void { meta_ = meta; }
    [[nodiscard]] auto serialize() const -> std::string;

private:
    std::string name_ {};
    std::vector<std::string_view> arguments_ {};
    CommandMeta meta_ { CommandMeta::supports_auto_pipelining };
};

inline auto Command::serialize() const -> std::string {
    std::string result;
    result.reserve(name_.size() + arguments_.size() * 16 + 32);
    result += '*'; result += std::to_string(arguments_.size() + 1); result += "\r\n";
    const auto append = [&result](std::string_view value) {
        result += '$'; result += std::to_string(value.size()); result += "\r\n";
        result.append(value); result += "\r\n";
    };
    append(name_);
    for (const auto argument : arguments_) append(argument);
    return result;
}

// Common-command builders returning a serialized RESP bulk-string array (the
// request wire format). Command names and metadata bits follow bun's
// src/runtime/valkey_jsc/js_valkey_functions.rs. Helpers whose reply is a
// RETURN_AS_BOOL integer (EXISTS/SISMEMBER/HEXISTS/HSETNX/...) are noted; apply
// RESPValue::apply_return_as_bool / as_boolean on the decoded reply.
namespace cmd {

[[nodiscard]] inline auto build(std::string_view name, std::span<const std::string_view> args = {}) -> std::string {
    std::string result;
    result.reserve(name.size() + args.size() * 16 + 32);
    result += '*'; result += std::to_string(args.size() + 1); result += "\r\n";
    const auto append = [&result](std::string_view value) {
        result += '$'; result += std::to_string(value.size()); result += "\r\n";
        result.append(value); result += "\r\n";
    };
    append(name);
    for (const auto arg : args) append(arg);
    return result;
}

[[nodiscard]] inline auto ping(std::optional<std::string_view> message = std::nullopt) -> std::string {
    if (message) { std::string_view args[] { *message }; return build("PING", args); }
    return build("PING");
}
[[nodiscard]] inline auto get(std::string_view key) -> std::string {
    std::string_view args[] { key }; return build("GET", args);
}
[[nodiscard]] inline auto set(std::string_view key, std::string_view value) -> std::string {
    std::string_view args[] { key, value }; return build("SET", args);
}
[[nodiscard]] inline auto incr(std::string_view key) -> std::string {
    std::string_view args[] { key }; return build("INCR", args);
}
[[nodiscard]] inline auto del(std::span<const std::string_view> keys) -> std::string {
    return build("DEL", keys);
}
[[nodiscard]] inline auto exists(std::string_view key) -> std::string {  // reply: RETURN_AS_BOOL
    std::string_view args[] { key }; return build("EXISTS", args);
}
[[nodiscard]] inline auto expire(std::string_view key, std::int64_t seconds) -> std::string {
    const auto secs = std::to_string(seconds);
    std::string_view args[] { key, secs }; return build("EXPIRE", args);
}
[[nodiscard]] inline auto ttl(std::string_view key) -> std::string {
    std::string_view args[] { key }; return build("TTL", args);
}
[[nodiscard]] inline auto hset(std::string_view key, std::string_view field, std::string_view value) -> std::string {
    std::string_view args[] { key, field, value }; return build("HSET", args);
}
[[nodiscard]] inline auto hget(std::string_view key, std::string_view field) -> std::string {
    std::string_view args[] { key, field }; return build("HGET", args);
}
[[nodiscard]] inline auto sismember(std::string_view key, std::string_view member) -> std::string {  // reply: RETURN_AS_BOOL
    std::string_view args[] { key, member }; return build("SISMEMBER", args);
}

}

}
