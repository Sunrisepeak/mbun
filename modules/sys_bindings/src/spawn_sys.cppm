export module mbun.sys_bindings.spawn_sys;

import std;

namespace mbun::spawn_sys {

// Ref: bun-ref/src/spawn_sys/{spawn_process,posix_spawn}.rs and the Zig spawn
// implementation. The platform process call remains a backend seam.
export enum class Stdio : std::uint8_t { inherit, pipe, ignore, fd };

export struct Spec {
    std::string executable;
    std::vector<std::string> arguments;
    std::vector<std::pair<std::string, std::string>> environment;
    Stdio stdin_mode {Stdio::inherit};
    Stdio stdout_mode {Stdio::inherit};
    Stdio stderr_mode {Stdio::inherit};
};

export struct Status {
    std::int32_t exit_code {-1};
    std::int32_t signal {};
    bool exited {false};
};

export struct Backend {
    virtual ~Backend() = default;
    virtual std::expected<Status, std::error_code> spawn(const Spec&) = 0;
};

} // namespace mbun::spawn_sys
