// redirection.cppm — mbun.shell.redirection
//
// Execution-side translation seam for Bun's shell redirect descriptor.
// References: Rust src/shell_parser/ast.rs (redirect AST) and Zig v1.3.14
// src/shell/states/Cmd.zig + src/shell/shell.zig (execution and fd rules).
//
// This file deliberately does not import or modify mbun.shell's parser AST.
// It is the value layer that a later executor can consume after expansion.
export module mbun.shell.redirection;

import std;

namespace mbun::shell::execution {

export enum class IoChannel : std::uint8_t { Stdin, Stdout, Stderr };

export enum class RedirectAction : std::uint8_t {
    Read,
    Write,
    Append,
    Duplicate,
};

export struct RedirectPlan {
    IoChannel channel{IoChannel::Stdout};
    RedirectAction action{RedirectAction::Write};
    std::string target;
    std::uint8_t source_fd{0};
    bool target_is_js_object{false};
    std::uint32_t js_object_index{0};

    constexpr std::uint8_t channel_fd() const {
        switch (channel) {
            case IoChannel::Stdin:
                return 0;
            case IoChannel::Stdout:
                return 1;
            case IoChannel::Stderr:
                return 2;
        }
        return 1;
    }

    constexpr bool opens_file() const {
        return action == RedirectAction::Read || action == RedirectAction::Write ||
               action == RedirectAction::Append;
    }

    constexpr bool duplicates_fd() const {
        return action == RedirectAction::Duplicate;
    }
};

export enum class RedirectError : std::uint8_t {
    EmptyTarget,
    InvalidSourceFd,
};

export inline std::expected<void, RedirectError> validate_redirect(const RedirectPlan& redirect) {
    if (redirect.duplicates_fd()) {
        if (redirect.source_fd > 2) {
            return std::unexpected(RedirectError::InvalidSourceFd);
        }
    } else if (redirect.target.empty() && !redirect.target_is_js_object) {
        return std::unexpected(RedirectError::EmptyTarget);
    }
    return {};
}

}  // namespace mbun::shell::execution
