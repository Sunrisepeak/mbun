// Injected command execution backend.
//
// This is the seam for Bun's native child-process/shell implementation.  No
// POSIX, Windows, or JSC headers belong in this pure interface.
export module mbun.runtime_shell.backend;

import std;
import mbun.runtime_shell.command;
import mbun.runtime_shell.error;
import mbun.runtime_shell.output;

export namespace mbun::runtime_shell {

class Backend {
public:
    using Execute = std::function<std::expected<CommandOutput, ShellError>(const Command&)>;

private:
    Execute execute_;

public:
    explicit Backend(Execute execute) : execute_ { std::move(execute) } {}

    [[nodiscard]] std::expected<CommandOutput, ShellError> execute(const Command& command) const {
        if (!execute_) return std::unexpected { ShellError::backend_unavailable("executor is not configured") };
        return execute_(command);
    }
};

inline Backend deferred_backend() {
    return Backend { [](const Command&) -> std::expected<CommandOutput, ShellError> {
        return std::unexpected { ShellError::backend_unavailable("native shell backend is deferred") };
    } };
}

} // namespace mbun::runtime_shell
