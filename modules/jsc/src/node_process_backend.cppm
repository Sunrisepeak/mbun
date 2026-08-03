// Injectable child-process backend boundary.
// ref: bun src/spawn/process.rs Process and src/spawn/lib.rs run
export module mbun.jsc.node_process_backend;

export import mbun.jsc.node_process_options;

import std;

export namespace mbun::jsc::node_process {

enum class SpawnError : std::uint8_t { unavailable, invalidRequest, systemError };

struct ProcessHandle {
    std::int64_t pid { -1 };
    bool running { false };
};

struct SpawnResult {
    ProcessHandle process;
    std::int32_t exitStatus { -1 };
    std::string stdoutData;
    std::string stderrData;
};

struct ProcessBackend {
    using SpawnFn = std::function<std::expected<SpawnResult, SpawnError>(const SpawnRequest&)>;
    using KillFn = std::function<std::expected<void, SpawnError>(ProcessHandle, std::int32_t)>;

    SpawnFn spawn;
    KillFn kill;

    [[nodiscard]] std::expected<SpawnResult, SpawnError>
    spawn_process(const SpawnRequest& request) const {
        if (!spawn) {
            return std::unexpected(SpawnError::unavailable);
        }
        return spawn(request);
    }

    [[nodiscard]] std::expected<void, SpawnError>
    kill_process(ProcessHandle process, std::int32_t signal) const {
        if (!kill) {
            return std::unexpected(SpawnError::unavailable);
        }
        return kill(process, signal);
    }
};

} // namespace mbun::jsc::node_process
