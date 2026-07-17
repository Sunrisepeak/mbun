// Captured process output and exit status.
export module mbun.runtime_shell.output;

import std;

export namespace mbun::runtime_shell {

struct CommandOutput {
    std::string stdout_text;
    std::string stderr_text;
    std::int32_t exit_code { 0 };
    bool signaled { false };

    [[nodiscard]] bool succeeded() const noexcept {
        return !signaled && exit_code == 0;
    }
};

} // namespace mbun::runtime_shell
