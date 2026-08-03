// Lifecycle state and scheduling shell. Process spawning and package-manager
// ownership remain behind NativeBackend, matching Bun's lifecycle tick seam.
// ref: bun-ref/src/install/PackageManager/PackageManagerLifecycle.rs;
//      bun-zig-src/src/install/PackageManager.zig lifecycle methods.
export module mbun.install_jsc.lifecycle;

import std;
import mbun.install_jsc.native_backend;

export namespace mbun::install_jsc {

enum class PreinstallState : std::uint8_t { Unknown, Extract, ApplyPatch, Done, Failed };
enum class LifecycleState : std::uint8_t { Sleeping, Running, Complete, Failed };

struct LifecycleRequest {
    std::string package_name;
    ScriptKind script { ScriptKind::Install };
    std::string command;
};

struct LifecycleLogEntry {
    std::string package_name;
    ScriptKind script { ScriptKind::Install };
    std::chrono::nanoseconds duration {};
    int exit_code { 0 };
};

class Lifecycle {
private:
    NativeBackend backend_;
    std::vector<PreinstallState> preinstall_states_;
    std::vector<LifecycleLogEntry> log_;
    LifecycleState state_ { LifecycleState::Sleeping };

public:
    explicit Lifecycle(NativeBackend backend = deferred_backend()) : backend_ { std::move(backend) } {}

    void ensure_capacity(std::size_t count) {
        if (preinstall_states_.size() < count) {
            preinstall_states_.resize(count, PreinstallState::Unknown);
        }
    }

    void set_preinstall_state(std::size_t packageId, PreinstallState state) {
        ensure_capacity(packageId + 1);
        preinstall_states_[packageId] = state;
    }

    [[nodiscard]] PreinstallState preinstall_state(std::size_t packageId) const noexcept {
        if (packageId >= preinstall_states_.size()) return PreinstallState::Unknown;
        return preinstall_states_[packageId];
    }

    [[nodiscard]] std::expected<void, BackendError> run(const LifecycleRequest& request) {
        state_ = LifecycleState::Running;
        if (!backend_.run_script) {
            state_ = LifecycleState::Failed;
            return std::unexpected(BackendError {
                .kind = ErrorKind::Unavailable,
                .operation = "runLifecycle",
                .message = "lifecycle runner is not installed",
            });
        }
        auto result{backend_.run_script(request.package_name, request.script, request.command)};
        if (!result) {
            state_ = LifecycleState::Failed;
            return std::unexpected(std::move(result.error()));
        }
        log_.push_back(LifecycleLogEntry {
            .package_name = request.package_name,
            .script = request.script,
            .duration = result->duration,
            .exit_code = result->exit_code,
        });
        state_ = result->exit_code == 0 ? LifecycleState::Complete : LifecycleState::Failed;
        return result->exit_code == 0
            ? std::expected<void, BackendError> {}
            : std::unexpected(BackendError {
                  .kind = ErrorKind::BackendFailure,
                  .operation = "runLifecycle",
                  .message = "lifecycle script exited non-zero",
              });
    }

    void sleep() noexcept { state_ = LifecycleState::Sleeping; }
    void wake() noexcept { if (state_ == LifecycleState::Sleeping) state_ = LifecycleState::Running; }
    [[nodiscard]] LifecycleState state() const noexcept { return state_; }
    [[nodiscard]] const std::vector<LifecycleLogEntry>& log() const noexcept { return log_; }
};

}  // namespace mbun::install_jsc
