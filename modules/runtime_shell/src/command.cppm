// Platform-neutral command request.
//
// The request is deliberately owning: a runtime binding may outlive the JS
// call frame that produced it, while the backend can still consume views.
export module mbun.runtime_shell.command;

import std;

export namespace mbun::runtime_shell {

struct Command {
    std::string program;
    std::vector<std::string> arguments;
    std::string working_directory;
    std::vector<std::pair<std::string, std::string>> environment;

    [[nodiscard]] bool valid() const noexcept { return !program.empty(); }

    [[nodiscard]] std::vector<std::string> argv() const {
        std::vector<std::string> result;
        result.reserve(arguments.size() + 1);
        result.push_back(program);
        result.insert(result.end(), arguments.begin(), arguments.end());
        return result;
    }
};

} // namespace mbun::runtime_shell
