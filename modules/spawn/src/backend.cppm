export module mbun.spawn.backend;

import std;
import mbun.spawn.options;
import mbun.spawn.stdio;

namespace mbun::spawn {

export struct SpawnRequest {
    std::vector<std::string> argv;
    std::vector<EnvEntry> env;
    std::string cwd;
    std::string argv0;
    std::array<StdioSpec, 3> stdio;
    bool detached;
    bool shell;
};

export struct ExitStatus {
    bool exited {false};
    int exit_code {-1};
    int signal {0};

    bool successful() const { return exited && exit_code == 0 && signal == 0; }
};

export struct SpawnResult {
    int pid {-1};
    ExitStatus status {};
};

export enum class BackendError { Deferred, Failed };
export using SpawnBackend = std::function<std::expected<SpawnResult, BackendError>(const SpawnRequest&)>;

export std::expected<SpawnResult, BackendError>
spawn(const SpawnOptions& options, const SpawnBackend& backend) {
    auto valid {validate(options)};
    if (!valid) return std::unexpected(BackendError::Failed);
    SpawnRequest request {
        .argv = options.argv,
        .env = options.env,
        .cwd = options.cwd,
        .argv0 = options.argv0,
        .stdio = options.stdio,
        .detached = options.detached,
        .shell = options.shell,
    };
    return backend(request);
}

} // namespace mbun::spawn
