// Pure child_process spawn argument normalization.
// ref: bun src/js/node/child_process.ts normalizeSpawnArguments/validateTimeout
export module mbun.jsc.node_process_options;

import std;
import mbun.jsc.node_process_descriptor;

export namespace mbun::jsc::node_process {

enum class Stdio : std::uint8_t { pipe, ignore, inherit };

using Shell = std::variant<bool, std::string>;

struct SpawnOptions {
    std::optional<std::string> cwd;
    // Absence inherits process.env; an explicitly empty map clears the child env.
    std::optional<Environment> env;
    std::optional<std::string> argv0;
    std::array<Stdio, 3> stdio { Stdio::pipe, Stdio::pipe, Stdio::pipe };
    bool detached { false };
    Shell shell { false };
    bool windowsHide { false };
    bool windowsVerbatimArguments { false };
    std::optional<std::int32_t> uid;
    std::optional<std::int32_t> gid;
    // Zero is valid and disables the timeout in Node/Bun.
    std::optional<std::uint64_t> timeoutMs;
    std::int32_t killSignal { 15 };
};

struct SpawnRequest {
    std::string file;
    // Bun's normalized args include argv0 at index zero.
    std::vector<std::string> args;
    SpawnOptions options;
};

enum class OptionError : std::uint8_t {
    emptyFile,
    nulInFile,
    nulInArgument,
    nulInArgv0,
};

[[nodiscard]] inline std::expected<SpawnRequest, OptionError>
normalize_spawn(std::string_view file, std::span<const std::string> args,
                SpawnOptions options = {}) {
    if (file.empty()) {
        return std::unexpected(OptionError::emptyFile);
    }
    if (file.contains('\0')) {
        return std::unexpected(OptionError::nulInFile);
    }
    if (options.argv0 && options.argv0->contains('\0')) {
        return std::unexpected(OptionError::nulInArgv0);
    }

    SpawnRequest request;
    request.file = file;
    request.args.reserve(args.size() + 1);
    request.args.emplace_back(options.argv0.value_or(request.file));
    for (const std::string& argument : args) {
        if (argument.contains('\0')) {
            return std::unexpected(OptionError::nulInArgument);
        }
        request.args.push_back(argument);
    }
    request.options = std::move(options);
    return request;
}

} // namespace mbun::jsc::node_process
