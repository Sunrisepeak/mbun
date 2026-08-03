export module mbun.spawn.options;

import std;
import mbun.spawn.stdio;

namespace mbun::spawn {

export struct EnvEntry {
    std::string key;
    std::string value;
    // Explicit member (not `= default` friend): a defaulted friend operator==
    // on an exported struct ICEs GCC 16 when instantiated across a module import.
    [[nodiscard]] bool operator==(const EnvEntry& other) const {
        return key == other.key && value == other.value;
    }
};

export struct SpawnOptions {
    std::vector<std::string> argv;
    std::vector<EnvEntry> env;
    std::string cwd;
    std::string argv0;
    std::array<StdioSpec, 3> stdio {
        StdioSpec {StdioKind::Pipe, -1},
        StdioSpec {StdioKind::Pipe, -1},
        StdioSpec {StdioKind::Pipe, -1},
    };
    bool detached {false};
    bool shell {false};
};

export enum class OptionError { EmptyArgv, EmbeddedNul, EmptyEnvKey, DuplicateEnvKey, InvalidStdio };

export std::expected<void, OptionError> validate(const SpawnOptions& options) {
    if (options.argv.empty()) return std::unexpected(OptionError::EmptyArgv);
    auto contains_nul = [](std::string_view value) { return value.find('\0') != std::string_view::npos; };
    for (const auto& arg : options.argv) {
        if (contains_nul(arg)) return std::unexpected(OptionError::EmbeddedNul);
    }
    if (contains_nul(options.cwd) || contains_nul(options.argv0)) {
        return std::unexpected(OptionError::EmbeddedNul);
    }
    std::set<std::string_view> keys;
    for (const auto& entry : options.env) {
        if (entry.key.empty()) return std::unexpected(OptionError::EmptyEnvKey);
        if (contains_nul(entry.key) || contains_nul(entry.value)) {
            return std::unexpected(OptionError::EmbeddedNul);
        }
        if (!keys.emplace(entry.key).second) return std::unexpected(OptionError::DuplicateEnvKey);
    }
    for (const auto& stdio : options.stdio) {
        if (stdio.kind == StdioKind::Fd && stdio.fd < 0) {
            return std::unexpected(OptionError::InvalidStdio);
        }
    }
    return {};
}

} // namespace mbun::spawn
