// Structured failures for the runtime shell binding.
//
// Bun reference note: the available local Rust/Zig snapshots do not contain
// src/runtime/shell.  This type keeps the eventual spawn backend independent
// from the command model until that source becomes available.
export module mbun.runtime_shell.error;

import std;

export namespace mbun::runtime_shell {

enum class ErrorKind : std::uint8_t {
    InvalidCommand,
    BackendUnavailable,
    ExecutionFailed,
};

struct ShellError {
    ErrorKind kind { ErrorKind::ExecutionFailed };
    std::string message;
    std::int32_t code { 0 };

    static ShellError invalid_command(std::string_view detail) {
        return { ErrorKind::InvalidCommand, std::string { detail }, 0 };
    }

    static ShellError backend_unavailable(std::string_view detail = {}) {
        return { ErrorKind::BackendUnavailable, std::string { detail }, 0 };
    }

    static ShellError execution_failed(std::string_view detail, std::int32_t exitCode = 0) {
        return { ErrorKind::ExecutionFailed, std::string { detail }, exitCode };
    }
};

} // namespace mbun::runtime_shell
