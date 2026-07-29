// interpreter.cppm — mbun.shell.interpreter
//
// Real POSIX execution backend for the shell value layer (U.3). It consumes the
// expanded value plan (PipelineStage / PipelinePlan / ExecutionNode / ExecutionPlan)
// and actually runs it: fork/exec external commands, wire `|` pipes, apply
// `> >> < 2>` and fd-duplication redirects, sequence `&& || ;`, thread per-command
// environment assignments, honor `cd` cwd state, propagate exit codes, and run the
// coreutils-style builtins Bun implements in-process (cd/echo/pwd/export/true/false/:).
//
// References (behavior lineage, not black-box guessing):
//   - .mbun/bun-zig-src/src/shell/Builtin.zig (builtin kinds, IO model)
//   - .mbun/bun-zig-src/src/shell/states/{Pipeline,Cmd}.zig (pipe/redirect/exit rules)
//   - .mbun/bun-zig-src/src/shell/subproc.zig (spawn semantics)
//
// POSIX-only backend by design (the seam in backend.cppm keeps Windows/event-loop
// wiring separate). JS bindings (Bun.$) live in modules/jsc and are not touched here.
module;

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

export module mbun.shell.interpreter;

import std;
import mbun.shell.pipeline;
import mbun.shell.redirection;
import mbun.shell.execution_plan;
import mbun.shell.coreutils;

extern "C" char** environ;

namespace mbun::shell::execution {

// Deterministic, insertion-order-independent env store. std::map keeps `export`
// output stable and lookups cheap; the shell has no ordering guarantee on env.
export class ShellEnv {
private:
    std::map<std::string, std::string> vars_;

public:
    ShellEnv() = default;

    static ShellEnv from_process() {
        ShellEnv env;
        if (environ != nullptr) {
            for (char** e = environ; *e != nullptr; ++e) {
                std::string_view entry{*e};
                const auto eq = entry.find('=');
                if (eq != std::string_view::npos) {
                    env.vars_.emplace(std::string{entry.substr(0, eq)},
                                      std::string{entry.substr(eq + 1)});
                }
            }
        }
        return env;
    }

    void set(std::string key, std::string value) {
        vars_[std::move(key)] = std::move(value);
    }

    void unset(const std::string& key) {
        vars_.erase(key);
    }

    std::optional<std::string> get(const std::string& key) const {
        if (const auto it = vars_.find(key); it != vars_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    bool contains(const std::string& key) const {
        return vars_.contains(key);
    }

    const std::map<std::string, std::string>& map() const {
        return vars_;
    }

    // Build a NUL-terminated "KEY=VALUE" argv-style block, optionally overlaid with
    // per-command assignments (which win over the exported value for that command).
    std::vector<std::string>
    to_block(const std::vector<std::pair<std::string, std::string>>& overlay = {}) const {
        std::map<std::string, std::string> merged = vars_;
        for (const auto& [k, v] : overlay) {
            merged[k] = v;
        }
        std::vector<std::string> out;
        out.reserve(merged.size());
        for (const auto& [k, v] : merged) {
            out.push_back(k + "=" + v);
        }
        return out;
    }
};

// Small RAII fd guard so an early return never leaks descriptors.
class FdGuard {
private:
    int fd_{-1};

public:
    explicit FdGuard(int fd = -1) : fd_{fd} {}
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    FdGuard(FdGuard&& other) noexcept : fd_{other.fd_} {
        other.fd_ = -1;
    }
    FdGuard& operator=(FdGuard&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    ~FdGuard() {
        reset();
    }
    int get() const {
        return fd_;
    }
    int release() {
        const int f = fd_;
        fd_ = -1;
        return f;
    }
    void reset() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
};

namespace detail {

// Returns false when the destination refused some of the bytes (errno is the
// failing write's). A builtin whose whole job is to emit output has FAILED when
// that happens — `echo a > /dev/full` is a 1, not a 0 (Builtin.zig routes an
// IOWriter error to the builtin's exit code) — so the result is reported, not
// swallowed. Callers writing diagnostics to stderr ignore it, as bun does.
inline bool write_all(int fd, std::string_view data) {
    std::size_t off{0};
    while (off < data.size()) {
        const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// A builtin's stdout write failed: report it the way bun's shell does and hand
// back the exit status the caller must return. `||` in a sequential list keys
// off exactly this status.
inline int report_write_error(std::string_view builtin) {
    const int saved{errno};
    std::string message{builtin};
    message += ": write error: ";
    message += std::strerror(saved);
    message += '\n';
    write_all(STDERR_FILENO, message);
    return 1;
}

// Open flags for a file redirect action.
inline int open_flags_for(RedirectAction action) {
    switch (action) {
        case RedirectAction::Read:
            return O_RDONLY;
        case RedirectAction::Write:
            return O_WRONLY | O_CREAT | O_TRUNC;
        case RedirectAction::Append:
            return O_WRONLY | O_CREAT | O_APPEND;
        case RedirectAction::Duplicate:
            return 0;
    }
    return 0;
}

}  // namespace detail

export enum class BuiltinKind : std::uint8_t {
    Cd,
    Echo,
    Pwd,
    Export,
    Unset,
    True,
    False,
    Colon,
    Basename,
    Exit,
    Test,
    Seq,
    Yes,
    Ls,
    Rm,
    Mv,
    Cp,
};

export inline std::optional<BuiltinKind> builtin_kind(std::string_view name) {
    if (name == "cd") return BuiltinKind::Cd;
    if (name == "echo") return BuiltinKind::Echo;
    if (name == "pwd") return BuiltinKind::Pwd;
    if (name == "export") return BuiltinKind::Export;
    if (name == "unset") return BuiltinKind::Unset;
    if (name == "true") return BuiltinKind::True;
    if (name == "false") return BuiltinKind::False;
    if (name == ":") return BuiltinKind::Colon;
    if (name == "basename") return BuiltinKind::Basename;
    if (name == "exit") return BuiltinKind::Exit;
    // `[[ EXPR ]]` is lowered to an argv-form `[[` builtin by the Bun.$ bridge.
    if (name == "[[") return BuiltinKind::Test;
    if (name == "seq") return BuiltinKind::Seq;
    if (name == "yes") return BuiltinKind::Yes;
    // bun ships its own ls/rm/mv/cp rather than exec'ing coreutils; their message
    // text and exit codes differ from GNU's (see modules/shell/src/coreutils.cppm).
    if (name == "ls") return BuiltinKind::Ls;
    if (name == "rm") return BuiltinKind::Rm;
    if (name == "mv") return BuiltinKind::Mv;
    if (name == "cp") return BuiltinKind::Cp;
    return std::nullopt;
}

// Whether a builtin mutates persistent shell state (cwd / exported env) and thus
// must run in the parent process rather than a forked child. Bun runs these in the
// interpreter process for the same reason.
inline bool builtin_mutates_state(BuiltinKind kind) {
    return kind == BuiltinKind::Cd || kind == BuiltinKind::Export ||
           kind == BuiltinKind::Unset;
}

export class ShellInterpreter {
private:
    ShellEnv env_;
    std::filesystem::path cwd_;
    std::filesystem::path oldpwd_;
    int lastExit_{0};

public:
    ShellInterpreter()
        : env_{ShellEnv::from_process()}, cwd_{std::filesystem::current_path()} {
        if (const auto pwd = env_.get("OLDPWD")) {
            oldpwd_ = *pwd;
        } else {
            oldpwd_ = cwd_;
        }
    }

    const std::filesystem::path& cwd() const {
        return cwd_;
    }

    const ShellEnv& env() const {
        return env_;
    }

    std::optional<std::string> get_env(const std::string& key) const {
        return env_.get(key);
    }

    int last_exit_code() const {
        return lastExit_;
    }

public:  // top-level entry points
    int run_plan(const ExecutionPlan& plan) {
        int code{0};
        for (const auto& node : plan.nodes) {
            code = run_node(node);
        }
        lastExit_ = code;
        return code;
    }

    int run_node(const ExecutionNode& node) {
        int code{0};
        switch (node.kind) {
            case PlanNodeKind::Pipeline:
            case PlanNodeKind::Async:
                // True background (`&`) is deferred; run foreground so the exit code
                // is observable. The Async tag is preserved for a later scheduler.
                code = run_pipeline(node.pipeline);
                break;
            case PlanNodeKind::And: {
                const int left = node.left ? run_node(*node.left) : 0;
                code = (left == 0 && node.right) ? run_node(*node.right) : left;
                break;
            }
            case PlanNodeKind::Or: {
                const int left = node.left ? run_node(*node.left) : 0;
                code = (left != 0 && node.right) ? run_node(*node.right) : left;
                break;
            }
        }
        lastExit_ = code;
        return code;
    }

    int run_pipeline(const PipelinePlan& pipeline) {
        // Collect runnable stages; assignment-only stages mutate shell env in place
        // (Bun: a bare `FOO=bar` sets the environment for the current shell).
        std::vector<const PipelineStage*> stages;
        for (const auto& stage : pipeline.stages) {
            if (stage.is_runnable()) {
                stages.push_back(&stage);
            } else {
                for (const auto& [k, v] : stage.assignments) {
                    env_.set(k, v);
                }
            }
        }

        if (stages.empty()) {
            lastExit_ = 0;
            return 0;
        }

        int code{0};
        if (stages.size() == 1) {
            code = run_single_stage_(*stages.front());
        } else {
            code = run_multi_stage_(stages);
        }
        lastExit_ = code;
        return code;
    }

private:
    // Single command with no pipe. If it is a state-mutating builtin (cd/export/unset)
    // or any builtin, run it in-process so state persists and redirects still apply.
    int run_single_stage_(const PipelineStage& stage) {
        if (stage.argv.empty()) {
            return 0;
        }
        const auto kind = builtin_kind(stage.argv.front());
        if (kind) {
            return run_builtin_in_parent_(*kind, stage);
        }
        return spawn_external_(stage, -1, -1);
    }

    // Multi-stage pipeline: one pipe between adjacent runnable stages. Every stage is
    // forked (builtins included); state-mutating builtins in a pipeline do not persist,
    // which matches shell subshell semantics.
    int run_multi_stage_(const std::vector<const PipelineStage*>& stages) {
        const std::size_t n = stages.size();
        std::vector<std::array<int, 2>> pipes(n - 1);
        for (std::size_t i = 0; i + 1 < n; ++i) {
            if (::pipe(pipes[i].data()) != 0) {
                // Tear down already-created pipes.
                for (std::size_t j = 0; j < i; ++j) {
                    ::close(pipes[j][0]);
                    ::close(pipes[j][1]);
                }
                return 1;
            }
        }

        std::vector<pid_t> pids(n, -1);
        for (std::size_t i = 0; i < n; ++i) {
            const int in_fd = (i > 0) ? pipes[i - 1][0] : -1;
            const int out_fd = (i + 1 < n) ? pipes[i][1] : -1;

            const pid_t pid = ::fork();
            if (pid == 0) {
                child_setup_pipes_(in_fd, out_fd, pipes);
                child_exec_stage_(*stages[i]);
                ::_exit(127);
            }
            pids[i] = pid;
        }

        // Parent closes all pipe fds so readers see EOF, then reaps.
        for (auto& p : pipes) {
            ::close(p[0]);
            ::close(p[1]);
        }

        int lastCode{0};
        for (std::size_t i = 0; i < n; ++i) {
            if (pids[i] < 0) {
                lastCode = 1;
                continue;
            }
            const int c = wait_for_(pids[i]);
            if (i + 1 == n) {
                lastCode = c;
            }
        }
        return lastCode;
    }

    // Fork + exec an external command (no pipe context). stdin/stdout override fds
    // (-1 = inherit) come from callers that already own a pipe.
    int spawn_external_(const PipelineStage& stage, int in_fd, int out_fd) {
        const pid_t pid = ::fork();
        if (pid < 0) {
            return 1;
        }
        if (pid == 0) {
            if (in_fd >= 0) {
                ::dup2(in_fd, STDIN_FILENO);
            }
            if (out_fd >= 0) {
                ::dup2(out_fd, STDOUT_FILENO);
            }
            child_exec_stage_(stage);
            ::_exit(127);
        }
        return wait_for_(pid);
    }

    // ---- child helpers (run in forked child; must only touch async-signal-safe-ish
    // paths — we keep to POSIX fs syscalls and _exit) ----

    void child_setup_pipes_(int in_fd, int out_fd,
                            const std::vector<std::array<int, 2>>& pipes) {
        if (in_fd >= 0) {
            ::dup2(in_fd, STDIN_FILENO);
        }
        if (out_fd >= 0) {
            ::dup2(out_fd, STDOUT_FILENO);
        }
        for (const auto& p : pipes) {
            ::close(p[0]);
            ::close(p[1]);
        }
    }

    // Apply redirects, chdir, then either dispatch a builtin or execvpe. Never returns
    // on success (exec replaces the image); returns via _exit on failure paths.
    [[noreturn]] void child_exec_stage_(const PipelineStage& stage) {
        // chdir first so relative redirect targets resolve against the shell cwd.
        if (!cwd_.empty()) {
            ::chdir(cwd_.c_str());
        }
        if (!apply_redirects_(stage.redirects)) {
            ::_exit(1);
        }

        if (stage.argv.empty()) {
            ::_exit(0);
        }

        if (const auto kind = builtin_kind(stage.argv.front())) {
            const int code = run_builtin_child_(*kind, stage);
            ::_exit(code);
        }

        // Build argv and envp for execvpe (PATH search + custom environment).
        std::vector<std::string> envStrings = env_.to_block(stage.assignments);
        std::vector<char*> argv;
        argv.reserve(stage.argv.size() + 1);
        for (const auto& a : stage.argv) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);

        std::vector<char*> envp;
        envp.reserve(envStrings.size() + 1);
        for (auto& e : envStrings) {
            envp.push_back(const_cast<char*>(e.c_str()));
        }
        envp.push_back(nullptr);

        ::execvpe(argv[0], argv.data(), envp.data());
        // exec failed.
        detail::write_all(STDERR_FILENO,
                          std::string{stage.argv.front()} + ": command not found\n");
        ::_exit(127);
    }

    // Apply a stage's redirects to fds 0/1/2. Returns false on open/dup failure.
    bool apply_redirects_(const std::vector<RedirectPlan>& redirects) {
        for (const auto& r : redirects) {
            const int target = r.channel_fd();
            if (r.duplicates_fd()) {
                if (::dup2(r.source_fd, target) < 0) {
                    return false;
                }
                continue;
            }
            const int flags = detail::open_flags_for(r.action);
            const int fd = ::open(r.target.c_str(), flags, 0644);
            if (fd < 0) {
                detail::write_all(STDERR_FILENO, r.target + ": cannot open\n");
                return false;
            }
            if (fd != target) {
                if (::dup2(fd, target) < 0) {
                    ::close(fd);
                    return false;
                }
                ::close(fd);
            }
        }
        return true;
    }

    // ---- builtins ----

    // Run a builtin in the parent process, applying redirects to saved/restored copies
    // of fds 0/1/2 so `echo x > file` and `cd d > /dev/null` both behave.
    int run_builtin_in_parent_(BuiltinKind kind, const PipelineStage& stage) {
        int saved0 = -1, saved1 = -1, saved2 = -1;
        const bool hasRedirects = !stage.redirects.empty();
        if (hasRedirects) {
            saved0 = ::dup(STDIN_FILENO);
            saved1 = ::dup(STDOUT_FILENO);
            saved2 = ::dup(STDERR_FILENO);
            if (!apply_redirects_(stage.redirects)) {
                restore_std_(saved0, saved1, saved2);
                return 1;
            }
        }

        const int code = dispatch_builtin_(kind, stage);

        if (hasRedirects) {
            restore_std_(saved0, saved1, saved2);
        }
        return code;
    }

    static void restore_std_(int s0, int s1, int s2) {
        if (s0 >= 0) {
            ::dup2(s0, STDIN_FILENO);
            ::close(s0);
        }
        if (s1 >= 0) {
            ::dup2(s1, STDOUT_FILENO);
            ::close(s1);
        }
        if (s2 >= 0) {
            ::dup2(s2, STDERR_FILENO);
            ::close(s2);
        }
    }

    // Run a builtin inside a forked child (redirects/chdir already applied). Mutations
    // to cwd/env here are intentionally not propagated to the parent.
    int run_builtin_child_(BuiltinKind kind, const PipelineStage& stage) {
        return dispatch_builtin_(kind, stage);
    }

    int dispatch_builtin_(BuiltinKind kind, const PipelineStage& stage) {
        switch (kind) {
            case BuiltinKind::True:
                return 0;
            case BuiltinKind::False:
                return 1;
            case BuiltinKind::Colon:
                return 0;
            case BuiltinKind::Echo:
                return builtin_echo_(stage);
            case BuiltinKind::Pwd:
                return builtin_pwd_();
            case BuiltinKind::Cd:
                return builtin_cd_(stage);
            case BuiltinKind::Export:
                return builtin_export_(stage);
            case BuiltinKind::Unset:
                return builtin_unset_(stage);
            case BuiltinKind::Basename:
                return builtin_basename_(stage);
            case BuiltinKind::Exit:
                return builtin_exit_(stage);
            case BuiltinKind::Test:
                return builtin_test_(stage);
            case BuiltinKind::Seq:
                return builtin_seq_(stage);
            case BuiltinKind::Yes:
                return builtin_yes_(stage);
            case BuiltinKind::Ls:
                return coreutils::run_ls(stage.argv);
            case BuiltinKind::Rm:
                return coreutils::run_rm(stage.argv);
            case BuiltinKind::Mv:
                return coreutils::run_mv(stage.argv);
            case BuiltinKind::Cp:
                return coreutils::run_cp(stage.argv);
        }
        return 1;
    }

    // Mirrors bun's shell `basename` builtin (src/runtime/shell/builtin/basename.rs
    // + bun_paths::resolve_path::basename): each operand is reduced independently
    // and printed on its own line, so multi-arg differs from GNU's suffix mode.
    static std::string_view path_basename_(std::string_view path) {
        const auto is_sep = [](char c) { return c == '/' || c == '\\'; };
        if (path.empty()) {
            return path;
        }
        std::size_t end = path.size() - 1;
        while (is_sep(path[end])) {
            if (end == 0) {
                return std::string_view{"/"};
            }
            --end;
        }
        std::size_t start = end;
        ++end;
        while (!is_sep(path[start])) {
            if (start == 0) {
                return path.substr(0, end);
            }
            --start;
        }
        return path.substr(start + 1, end - (start + 1));
    }

    int builtin_basename_(const PipelineStage& stage) {
        if (stage.argv.size() < 2) {
            detail::write_all(STDERR_FILENO, "usage: basename string\n");
            return 1;
        }
        std::string out;
        for (std::size_t i = 1; i < stage.argv.size(); ++i) {
            out += path_basename_(stage.argv[i]);
            out.push_back('\n');
        }
        detail::write_all(STDOUT_FILENO, out);
        return 0;
    }

    // bun `exit` (builtin/exit.rs): 0 args → 0; one numeric arg → n % 256 (bash
    // wrap); non-numeric → error; >1 arg → error. Diverges from bash by completing
    // only the current command (here: the pipeline stage), never unwinding the script.
    static std::optional<int> parse_exit_code_(std::string_view s) {
        if (s.empty()) {
            return std::nullopt;
        }
        std::uint64_t n{0};
        for (const char c : s) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            n = n * 10 + static_cast<std::uint64_t>(c - '0');
        }
        return static_cast<int>(n % 256);
    }

    int builtin_exit_(const PipelineStage& stage) {
        if (stage.argv.size() <= 1) {
            return 0;
        }
        if (stage.argv.size() > 2) {
            detail::write_all(STDERR_FILENO, "exit: too many arguments\n");
            return 1;
        }
        const auto code = parse_exit_code_(stage.argv[1]);
        if (!code) {
            detail::write_all(STDERR_FILENO, "exit: numeric argument required\n");
            return 1;
        }
        return *code;
    }

    // bun `[[ EXPR ]]` (states/CondExpr.rs): the Bun.$ bridge lowers a CondExpr to
    // argv = ["[[", op, operand...]. Only the parser-supported operators reach here
    // (unary -z/-n/-c/-d/-f, binary ==/!=). Exit 0 = true, 1 = false.
    int builtin_test_(const PipelineStage& stage) {
        const auto& argv = stage.argv;
        if (argv.size() < 2) {
            return 1;
        }
        const std::string& op = argv[1];
        const std::size_t nOperands = argv.size() - 2;
        const auto operand = [&](std::size_t i) -> std::string_view {
            return (2 + i < argv.size()) ? std::string_view{argv[2 + i]}
                                         : std::string_view{};
        };

        if (op == "-z") {
            return operand(0).empty() ? 0 : 1;
        }
        if (op == "-n") {
            return operand(0).empty() ? 1 : 0;
        }
        if (op == "==" || op == "=") {
            const bool eq =
                (nOperands == 0) || (nOperands >= 2 && operand(0) == operand(1));
            return eq ? 0 : 1;
        }
        if (op == "!=") {
            const bool neq = (nOperands >= 2) && (operand(0) != operand(1));
            return neq ? 0 : 1;
        }
        // File-test operators: empty path is always false (bash); stat() error → false.
        if (op == "-f" || op == "-d" || op == "-c") {
            const std::string_view path = operand(0);
            if (path.empty()) {
                return 1;
            }
            std::filesystem::path p{path};
            if (p.is_relative()) {
                p = cwd_ / p;
            }
            struct ::stat st {};
            if (::stat(p.c_str(), &st) != 0) {
                return 1;
            }
            if (op == "-f") {
                return S_ISREG(st.st_mode) ? 0 : 1;
            }
            if (op == "-d") {
                return S_ISDIR(st.st_mode) ? 0 : 1;
            }
            return S_ISCHR(st.st_mode) ? 0 : 1;
        }
        return 1;
    }

    // Rust `{}` on an f32: integral values print with no decimal point, otherwise the
    // shortest round-tripping decimal. We only need the integral common case exactly.
    static std::string format_seq_num_(float v) {
        if (std::isfinite(v) && v == static_cast<float>(static_cast<long long>(v))) {
            return std::to_string(static_cast<long long>(v));
        }
        return std::format("{}", v);
    }

    // bun `seq` (builtin/seq.rs): -s/-t/-w flags then `[first [incr]] last`. Direction
    // is inferred (start>end ⇒ -1); a 3-arg form validates increment sign. f32 math so
    // the `next == current` saturation break matches bun.
    int builtin_seq_(const PipelineStage& stage) {
        static constexpr std::string_view SEQ_USAGE =
            "usage: seq [-w] [-f format] [-s string] [-t string] [first [incr]] last\n";
        const auto& argv = stage.argv;
        const std::size_t argc = argv.size();
        if (argc <= 1) {
            detail::write_all(STDERR_FILENO, SEQ_USAGE);
            return 1;
        }

        std::string separator{"\n"};
        std::string terminator{};
        std::size_t idx{1};
        while (idx < argc) {
            const std::string& arg = argv[idx];
            if (arg == "-s" || arg == "--separator") {
                if (++idx >= argc) {
                    detail::write_all(STDERR_FILENO,
                                      "seq: option requires an argument -- s\n");
                    return 1;
                }
                separator = argv[idx++];
                continue;
            }
            if (arg.size() > 2 && arg[0] == '-' && arg[1] == 's') {
                separator = arg.substr(2);
                ++idx;
                continue;
            }
            if (arg == "-t" || arg == "--terminator") {
                if (++idx >= argc) {
                    detail::write_all(STDERR_FILENO,
                                      "seq: option requires an argument -- t\n");
                    return 1;
                }
                terminator = argv[idx++];
                continue;
            }
            if (arg.size() > 2 && arg[0] == '-' && arg[1] == 't') {
                terminator = arg.substr(2);
                ++idx;
                continue;
            }
            if (arg == "-w" || arg == "--fixed-width") {
                ++idx;
                continue;
            }
            break;
        }

        const auto parse_num = [](const std::string& s, float& out) -> bool {
            try {
                std::size_t pos{0};
                const float v = std::stof(s, &pos);
                if (pos != s.size() || !std::isfinite(v)) {
                    return false;
                }
                out = v;
                return true;
            } catch (...) {
                return false;
            }
        };

        float start{1.0F};
        float end{1.0F};
        float increment{1.0F};

        if (idx >= argc) {
            detail::write_all(STDERR_FILENO, SEQ_USAGE);
            return 1;
        }
        float int1{0.0F};
        if (!parse_num(argv[idx++], int1)) {
            detail::write_all(STDERR_FILENO, "seq: invalid argument\n");
            return 1;
        }
        end = int1;
        if (start > end) {
            increment = -1.0F;
        }

        if (idx < argc) {
            float int2{0.0F};
            if (!parse_num(argv[idx++], int2)) {
                detail::write_all(STDERR_FILENO, "seq: invalid argument\n");
                return 1;
            }
            start = int1;
            end = int2;
            increment = (start < end) ? 1.0F : (start > end ? -1.0F : increment);

            if (idx < argc) {
                float int3{0.0F};
                if (!parse_num(argv[idx], int3)) {
                    detail::write_all(STDERR_FILENO, "seq: invalid argument\n");
                    return 1;
                }
                start = int1;
                increment = int2;
                end = int3;
                if (increment == 0.0F) {
                    detail::write_all(STDERR_FILENO, "seq: zero increment\n");
                    return 1;
                }
                if (start > end && increment > 0.0F) {
                    detail::write_all(STDERR_FILENO, "seq: needs negative decrement\n");
                    return 1;
                }
                if (start < end && increment < 0.0F) {
                    detail::write_all(STDERR_FILENO, "seq: needs positive increment\n");
                    return 1;
                }
            }
        }

        std::string out;
        float current = start;
        while (increment > 0.0F ? current <= end : current >= end) {
            out += format_seq_num_(current);
            out += separator;
            const float next = current + increment;
            if (next == current) {
                break;  // f32 saturation guard (bun): avoids an unbounded loop.
            }
            current = next;
        }
        out += terminator;
        detail::write_all(STDOUT_FILENO, out);
        return 0;
    }

    // bun `yes` (builtin/yes.rs): repeat STRING (default "y") + '\n' until the pipe
    // closes. We tile one line to ~BUFSIZ for throughput and loop until a write fails
    // (EPIPE on downstream close) — matching bun's on_io_writer_chunk error path,
    // which terminates silently (no "broken pipe" on stderr).
    int builtin_yes_(const PipelineStage& stage) {
        std::string one;
        if (stage.argv.size() <= 1) {
            one = "y\n";
        } else {
            for (std::size_t i = 1; i < stage.argv.size(); ++i) {
                if (i > 1) {
                    one.push_back(' ');
                }
                one += stage.argv[i];
            }
            one.push_back('\n');
        }

        constexpr std::size_t BUFSIZE{8192};
        std::string buf;
        if (one.size() <= BUFSIZE / 2) {
            while (buf.size() + one.size() <= BUFSIZE) {
                buf += one;
            }
        } else {
            buf = one;
        }

        for (;;) {
            std::size_t off{0};
            while (off < buf.size()) {
                const ssize_t n =
                    ::write(STDOUT_FILENO, buf.data() + off, buf.size() - off);
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    return 1;  // EPIPE / other fatal write error → stop.
                }
                if (n == 0) {
                    return 1;
                }
                off += static_cast<std::size_t>(n);
            }
        }
    }

    int builtin_echo_(const PipelineStage& stage) {
        // bun echo (builtin/echo.rs): consume a run of leading -n/-e/-E flags; on
        // the LAST arg collapse a trailing run of '\n' to one and suppress the
        // appended newline when it already ends in '\n'. (-e escapes omitted: no
        // test exercises them and the prior impl never had them.)
        std::size_t args_start{1};
        bool no_newline{false};
        for (std::size_t i = 1; i < stage.argv.size(); ++i) {
            const std::string& f = stage.argv[i];
            if (f.size() < 2 || f[0] != '-') break;
            if (f.find_first_not_of("neE", 1) != std::string::npos) break;
            for (std::size_t k = 1; k < f.size(); ++k) if (f[k] == 'n') no_newline = true;
            args_start = i + 1;
        }
        std::string out;
        bool ends_nl{false};
        const std::size_t n = stage.argv.size();
        for (std::size_t i = args_start; i < n; ++i) {
            if (i > args_start) out.push_back(' ');
            std::string_view a{stage.argv[i]};
            if (i + 1 == n && !a.empty()) {  // last arg: collapse trailing '\n' run to one
                if (a.back() == '\n') ends_nl = true;
                std::size_t endend = a.size();
                std::size_t end = a.size() - 1;
                while (end > 0 && a[end] == '\n') { endend = end + 1; --end; }
                out += a.substr(0, endend);
            } else {
                out += a;
            }
        }
        if (!ends_nl && !no_newline) out.push_back('\n');
        if (!detail::write_all(STDOUT_FILENO, out)) return detail::report_write_error("echo");
        return 0;
    }

    int builtin_pwd_() {
        detail::write_all(STDOUT_FILENO, cwd_.string() + "\n");
        return 0;
    }

    int builtin_cd_(const PipelineStage& stage) {
        std::filesystem::path dest;
        if (stage.argv.size() < 2 || stage.argv[1].empty()) {
            const auto home = env_.get("HOME");
            if (!home) {
                detail::write_all(STDERR_FILENO, "cd: HOME not set\n");
                return 1;
            }
            dest = *home;
        } else if (stage.argv[1] == "-") {
            dest = oldpwd_;
        } else {
            dest = stage.argv[1];
        }

        std::filesystem::path resolved =
            dest.is_absolute() ? dest : (cwd_ / dest);
        std::error_code ec;
        std::filesystem::path canonical = std::filesystem::weakly_canonical(resolved, ec);
        if (ec) {
            canonical = resolved;
        }

        // chdir the interpreter process too so external children inherit it even if
        // they don't get an explicit chdir (belt and suspenders).
        if (::chdir(canonical.c_str()) != 0) {
            detail::write_all(STDERR_FILENO,
                              "cd: " + dest.string() + ": No such file or directory\n");
            return 1;
        }
        oldpwd_ = cwd_;
        cwd_ = canonical;
        env_.set("OLDPWD", oldpwd_.string());
        env_.set("PWD", cwd_.string());
        return 0;
    }

    int builtin_export_(const PipelineStage& stage) {
        for (std::size_t i = 1; i < stage.argv.size(); ++i) {
            std::string_view arg{stage.argv[i]};
            const auto eq = arg.find('=');
            if (eq != std::string_view::npos) {
                env_.set(std::string{arg.substr(0, eq)}, std::string{arg.substr(eq + 1)});
            }
            // `export NAME` with no `=` marks an existing var exported; our env is a
            // single flat exported map, so a bare name is a no-op unless assigned.
        }
        return 0;
    }

    int builtin_unset_(const PipelineStage& stage) {
        for (std::size_t i = 1; i < stage.argv.size(); ++i) {
            env_.unset(stage.argv[i]);
        }
        return 0;
    }

    static int wait_for_(pid_t pid) {
        int status{0};
        while (::waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) {
                return 1;
            }
        }
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        if (WIFSIGNALED(status)) {
            return 128 + WTERMSIG(status);
        }
        return 1;
    }
};

}  // namespace mbun::shell::execution
