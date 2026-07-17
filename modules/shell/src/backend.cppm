// backend.cppm — mbun.shell.backend
//
// Backend seam for the execution plan. Bun's Zig implementation supplies
// platform FD/pipe/spawn behavior through ShellSubprocess, IOWriter/IOReader,
// and the event loop (src/shell/subproc.zig, src/shell/IOWriter.zig,
// src/shell/IOReader.zig). The seam keeps those effects injectable and leaves
// POSIX/Windows system calls for the runtime milestone.
export module mbun.shell.backend;

import std;
import mbun.shell.execution_plan;
import mbun.shell.pipeline;
import mbun.shell.redirection;

namespace mbun::shell::execution {

export using BackendHandle = std::int64_t;

export enum class BackendErrorCode : std::uint8_t {
    Unsupported,
    OpenFailed,
    PipeFailed,
    SpawnFailed,
    CloseFailed,
};

export struct BackendError {
    BackendErrorCode code{BackendErrorCode::Unsupported};
    std::string message;
};

export struct PipeHandles {
    BackendHandle read{0};
    BackendHandle write{0};
};

export struct SpawnedProcess {
    BackendHandle process{0};
};

export class ShellBackend {
public:
    virtual ~ShellBackend() = default;

    virtual std::expected<BackendHandle, BackendError>
    open_redirect(const RedirectPlan& redirect) = 0;
    virtual std::expected<PipeHandles, BackendError> create_pipe() = 0;
    virtual std::expected<SpawnedProcess, BackendError> spawn(const PipelineStage& stage,
                                                              BackendHandle stdin_handle,
                                                              BackendHandle stdout_handle,
                                                              BackendHandle stderr_handle) = 0;
    virtual std::expected<void, BackendError> close(BackendHandle handle) = 0;
};

export class UnwiredShellBackend final : public ShellBackend {
private:
    static std::unexpected<BackendError> unsupported(BackendErrorCode code, std::string_view what) {
        return std::unexpected(BackendError{code, std::string{what} + " backend is not wired"});
    }
public:
    std::expected<BackendHandle, BackendError> open_redirect(const RedirectPlan&) override {
        return unsupported(BackendErrorCode::OpenFailed, "redirect");
    }

    std::expected<PipeHandles, BackendError> create_pipe() override {
        return unsupported(BackendErrorCode::PipeFailed, "pipe");
    }

    std::expected<SpawnedProcess, BackendError> spawn(const PipelineStage&, BackendHandle,
                                                      BackendHandle, BackendHandle) override {
        return unsupported(BackendErrorCode::SpawnFailed, "spawn");
    }

    std::expected<void, BackendError> close(BackendHandle) override {
        return unsupported(BackendErrorCode::CloseFailed, "close");
    }
};

}  // namespace mbun::shell::execution
