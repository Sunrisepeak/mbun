// Native seam for bun install_jsc. JSC, filesystem and process adapters are
// intentionally injected; their platform implementations are DEFERRED.
// ref: bun-ref/src/install_jsc/install_binding.rs and
//      bun-ref/src/install/PackageManager/PackageManagerLifecycle.rs;
//      bun-zig-src/src/install_jsc/install_binding.zig.
export module mbun.install_jsc.native_backend;

import std;

export namespace mbun::install_jsc {

enum class ErrorKind : std::uint8_t { Unavailable, InvalidInput, BackendFailure };

struct BackendError {
    ErrorKind kind { ErrorKind::Unavailable };
    std::string operation;
    std::string message;
};

enum class ScriptKind : std::uint8_t { Preinstall, Install, Postinstall, Prepare };

struct ScriptResult {
    int exit_code { 0 };
    std::chrono::nanoseconds duration {};
};

struct NativeBackend {
    using ParseLockfile = std::function<std::expected<std::string, BackendError>(std::string_view)>;
    using RunScript = std::function<std::expected<ScriptResult, BackendError>(
        std::string_view, ScriptKind, std::string_view)>;

    ParseLockfile parse_lockfile {};
    RunScript run_script {};

    [[nodiscard]] bool has_lockfile_parser() const noexcept { return static_cast<bool>(parse_lockfile); }
    [[nodiscard]] bool has_script_runner() const noexcept { return static_cast<bool>(run_script); }
};

inline NativeBackend deferred_backend() {
    return {
        .parse_lockfile = [](std::string_view) -> std::expected<std::string, BackendError> {
            return std::unexpected(BackendError {
                .kind = ErrorKind::Unavailable,
                .operation = "parseLockfile",
                .message = "install_jsc native backend is DEFERRED(S-install-jsc)",
            });
        },
        .run_script = [](std::string_view, ScriptKind, std::string_view)
            -> std::expected<ScriptResult, BackendError> {
            return std::unexpected(BackendError {
                .kind = ErrorKind::Unavailable,
                .operation = "runLifecycle",
                .message = "install_jsc lifecycle backend is DEFERRED(S-install-jsc)",
            });
        },
    };
}

}  // namespace mbun::install_jsc
