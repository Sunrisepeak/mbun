// Runtime-facing command binding over an injected backend.
export module mbun.runtime_shell.binding;

import std;
import mbun.runtime_shell.backend;
import mbun.runtime_shell.command;
import mbun.runtime_shell.error;
import mbun.runtime_shell.output;

export namespace mbun::runtime_shell {

class ShellBinding {
private:
    Backend backend_;

public:
    explicit ShellBinding(Backend backend) : backend_ { std::move(backend) } {}

    [[nodiscard]] std::expected<CommandOutput, ShellError> run(const Command& command) const {
        if (!command.valid()) return std::unexpected { ShellError::invalid_command("program is empty") };
        return backend_.execute(command);
    }
};

} // namespace mbun::runtime_shell
