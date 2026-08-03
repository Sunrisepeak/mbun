// Bun `$` execution bridge: run a compiled POSIX shell script through mbun's own
// interpreter (modules/shell — real fork/exec/pipe/redirect + in-process builtins)
// instead of delegating to the host /bin/sh. This is the execution-binding half of
// the Bun.$ wiring; the template-tag → script compilation lives in shell.inc and the
// JS-facing ShellPromise/ShellError model lives in the builtins/shell.cppm partition.
//
// Scope (safety-first, no-regression): only fully-literal command graphs are lowered
// to the interpreter here — commands, pipes (`|`), sequences/conditionals
// (`; && ||`), assignments, simple file redirects (`> >> < 2>`) and command
// substitution (`$(...)`, expanded eagerly through this same path — see LowerCtx).
// Any construct the lowering cannot faithfully model (variable/tilde/glob/brace
// expansion, subshell, if, fd-duplication, `&`, JS-buffer redirect targets) makes
// exec return null so the JS layer falls back to the existing spawn path.
// Interpolated JS values are single-quoted literals after compile, so the common
// `$`cmd ${value}`` forms route through the real interpreter.
//
// References (blueprint, not black-box): bun src/js/builtins/shell.ts (ShellPromise
// run/quiet/throws), src/shell/interpreter.zig + states/{Pipeline,Cmd}.zig (exit /
// pipe / redirect rules) mirrored by modules/shell/src/interpreter.cppm.

// `environ` is already declared by <unistd.h> (pulled in via prelude.hpp); the child
// re-points it at the caller-provided block before constructing the interpreter.

namespace bunsh {

using namespace mbun::shell;
using mbun::shell::execution::ExecutionNode;
using mbun::shell::execution::ExecutionPlan;
using mbun::shell::execution::IoChannel;
using mbun::shell::execution::PipelinePlan;
using mbun::shell::execution::PipelineStage;
using mbun::shell::execution::PipelineStageKind;
using mbun::shell::execution::PlanNodeKind;
using mbun::shell::execution::RedirectAction;
using mbun::shell::execution::RedirectPlan;

// Resolve a parser Atom to its literal text. Returns nullopt when the atom carries
// any expansion (variable/glob/brace/tilde/command-substitution) the literal-only
// lowering must not silently drop — the caller then punts to the fallback path.
// `{`, `}` and `,` are lexed as brace tokens unconditionally (bun's lexer does the
// same), but a word that is not a *complete* `{a,b}` expansion carries them as plain
// characters in every POSIX shell — `seq -s, 0 5` passes the literal word "-s,".
// The parser only sets brace_expansion_hint when open+comma+close are all present,
// so with the hint clear these three tokens are safe to fold back into text; with
// the hint set the word still punts to the fallback path.
std::optional<std::string> brace_token_text(SAKind kind) {
    switch (kind) {
        case SAKind::BraceBegin:
            return std::string{"{"};
        case SAKind::BraceEnd:
            return std::string{"}"};
        case SAKind::Comma:
            return std::string{","};
        default:
            return std::nullopt;
    }
}

struct ExecResult {
    int exit_code{0};
    std::string stdout_bytes;
    std::string stderr_bytes;
};

std::optional<ExecResult> run_plan_captured(const ExecutionPlan& plan, const std::string& cwd,
                                            std::vector<char*>& envp, bool have_env);

// Everything the lowering needs in order to *evaluate* a `$(...)` while it is still
// building the plan: a command substitution is expanded eagerly, in the parent, by
// running its own lowered plan through the same captured-fork path the outer script
// will use. `state_mutated` records that a `cd`/`export`/`unset`/assignment stage was
// already lowered — after one of those the eager evaluation would observe the *pre*
// -mutation cwd/env, so any later `$(...)` punts to the /bin/sh fallback rather than
// silently substituting the wrong text. `stderr_sink` collects what the substituted
// commands wrote to fd 2, which belongs in the outer result's stderr.
struct LowerCtx {
    std::string cwd;
    std::vector<char*>* envp{nullptr};
    bool have_env{false};
    std::string* stderr_sink{nullptr};
    bool state_mutated{false};
};

// One piece of a word under construction. `splittable` marks text that came out of an
// *unquoted* command substitution and therefore undergoes IFS field splitting;
// literal text and `"$(...)"` never split.
struct WordSegment {
    std::string text;
    bool splittable{false};
};

std::optional<ExecutionPlan> lower_script(const Script& script, LowerCtx& ctx);

constexpr bool is_ifs_space(char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

// Expand one `$(...)`: lower the inner Script with the same context (so nesting works
// by recursion), run it captured, then strip trailing newlines the way POSIX command
// substitution does. A construct the lowering cannot model inside the substitution
// punts the whole outer script, exactly as it would at the top level.
bool eval_cmdsubst(const SimpleAtom& atom, LowerCtx& ctx, std::vector<WordSegment>& out) {
    if (!atom.cmdsubst_script || ctx.state_mutated || ctx.envp == nullptr) {
        return false;
    }
    auto inner = lower_script(*atom.cmdsubst_script, ctx);
    if (!inner) {
        return false;
    }
    auto result = run_plan_captured(*inner, ctx.cwd, *ctx.envp, ctx.have_env);
    if (!result) {
        return false;
    }
    if (ctx.stderr_sink != nullptr) {
        *ctx.stderr_sink += result->stderr_bytes;
    }
    std::string text{std::move(result->stdout_bytes)};
    while (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    out.push_back(WordSegment{std::move(text), !atom.cmdsubst_quoted});
    return true;
}

bool collect_simple_segments(const SimpleAtom& atom, LowerCtx& ctx,
                             std::vector<WordSegment>& out) {
    switch (atom.kind) {
        case SAKind::Text:
            out.push_back(WordSegment{std::string{atom.text}, false});
            return true;
        case SAKind::QuotedEmpty:
            out.push_back(WordSegment{std::string{}, false});
            return true;
        case SAKind::CmdSubst:
            return eval_cmdsubst(atom, ctx, out);
        default: {
            auto literal = brace_token_text(atom.kind);
            if (!literal) {
                return false;
            }
            out.push_back(WordSegment{std::move(*literal), false});
            return true;
        }
    }
}

// Read one variable out of the environment the lowered plan will run under.
// `have_env` means the caller supplied an explicit envp (`$\`...\`.env({...})`),
// in which case the process environment is NOT what the command will see.
// Returns nullopt when the value cannot be read *as the command will see it*.
// `state_mutated` is the same guard eval_cmdsubst uses and is load-bearing here
// for the same reason: `HOME=lmao && echo ~` lowers the assignment stage first,
// so a lookup performed while building the plan would still observe the pre-
// assignment value and bake it into the argv (bunshell.test.ts tilde_expansion
// "modified $HOME or $USERPROFILE"). After a mutation the word punts, as before.
std::optional<std::string> lookup_env(const LowerCtx& ctx, std::string_view name) {
    if (ctx.state_mutated) return std::nullopt;
    if (ctx.have_env && ctx.envp != nullptr) {
        for (char* entry : *ctx.envp) {
            if (entry == nullptr) continue;
            std::string_view kv{entry};
            const auto eq = kv.find('=');
            if (eq != std::string_view::npos && kv.substr(0, eq) == name) {
                return std::string{kv.substr(eq + 1)};
            }
        }
        return std::nullopt;
    }
    const char* value = ::getenv(std::string{name}.c_str());
    return value != nullptr ? std::optional<std::string>{value} : std::nullopt;
}

// A leading `~` expands to $HOME only when its *tilde-prefix* -- the characters
// between the `~` and the first `/` -- is empty. `~user` is a passwd lookup the
// lowering does not model, so it still punts.
//
// The prefix is empty for a bare `~`, for `~/path`, and also for `~""` / `~''`:
// quote removal leaves nothing between the tilde and the end of the word. That
// last shape is why this exists. The parser records an empty quoted token as its
// own `QuotedEmpty` atom precisely so it stays distinguishable from "no atom at
// all", and every POSIX shell stops tilde expansion the moment it sees one --
// `bash -c 'echo ~""'` prints `~`. bun does not: its lexer performs tilde
// expansion during tokenisation, before quote handling, so the empty quotes
// cannot suppress it. Punting the word to /bin/sh therefore produced the POSIX
// answer where the corpus wants bun's (regression/issue/17294).
bool tilde_prefix_is_empty(const std::vector<SimpleAtom>& atoms) {
    for (std::size_t i = 1; i < atoms.size(); ++i) {
        if (atoms[i].kind == SAKind::QuotedEmpty) continue;
        return atoms[i].kind == SAKind::Text && !atoms[i].text.empty() &&
               atoms[i].text.front() == '/';
    }
    return true;
}

bool collect_atom_segments(const Atom& atom, LowerCtx& ctx, std::vector<WordSegment>& out) {
    if (atom.kind == AtomKind::Simple) {
        if (atom.simple.kind == SAKind::Tilde) {
            auto home = lookup_env(ctx, "HOME");
            if (!home) return false;
            out.push_back(WordSegment{std::move(*home), false});
            return true;
        }
        return collect_simple_segments(atom.simple, ctx, out);
    }
    if (atom.compound.glob_hint || atom.compound.brace_expansion_hint) {
        return false;
    }
    std::size_t first = 0;
    if (!atom.compound.atoms.empty() && atom.compound.atoms.front().kind == SAKind::Tilde) {
        if (!tilde_prefix_is_empty(atom.compound.atoms)) {
            return false;
        }
        auto home = lookup_env(ctx, "HOME");
        if (!home) return false;
        out.push_back(WordSegment{std::move(*home), false});
        first = 1;
    }
    for (std::size_t i = first; i < atom.compound.atoms.size(); ++i) {
        if (!collect_simple_segments(atom.compound.atoms[i], ctx, out)) {
            return false;
        }
    }
    return true;
}

// Resolve a parser Atom to a single literal word. Returns nullopt when the atom
// carries an expansion (variable/glob/brace/tilde) the lowering must not silently
// drop — the caller then punts to the fallback path. Command substitutions ARE
// resolved, but without field splitting: this is the assignment-value / redirect-
// target / cond-operand context, where POSIX performs no splitting.
std::optional<std::string> atom_literal(const Atom& atom, LowerCtx& ctx) {
    std::vector<WordSegment> segments;
    if (!collect_atom_segments(atom, ctx, segments)) {
        return std::nullopt;
    }
    std::string out;
    for (const auto& segment : segments) {
        out += segment.text;
    }
    return out;
}

// Resolve a parser Atom to the argv words it produces. Identical to atom_literal
// except that text from an unquoted `$(...)` is split on IFS whitespace, so
// `echo $(seq 0 5)` passes six arguments and `echo "$(seq 0 5)"` passes one. Literal
// text adjacent to the substitution stays attached to the neighbouring field, which
// is why splitting runs over the assembled segment list rather than per segment.
bool lower_word(const Atom& atom, LowerCtx& ctx, std::vector<std::string>& out) {
    std::vector<WordSegment> segments;
    if (!collect_atom_segments(atom, ctx, segments)) {
        return false;
    }
    bool splits = false;
    for (const auto& segment : segments) {
        splits = splits || segment.splittable;
    }
    if (!splits) {
        std::string word;
        for (const auto& segment : segments) {
            word += segment.text;
        }
        out.push_back(std::move(word));
        return true;
    }
    std::string current;
    bool started{false};  // an empty word is still a word once literal text opened it
    for (const auto& segment : segments) {
        if (!segment.splittable) {
            current += segment.text;
            started = true;
            continue;
        }
        std::size_t i{0};
        while (i < segment.text.size()) {
            if (is_ifs_space(segment.text[i])) {
                if (started) {
                    out.push_back(current);
                    current.clear();
                    started = false;
                }
                while (i < segment.text.size() && is_ifs_space(segment.text[i])) {
                    ++i;
                }
                continue;
            }
            std::size_t j{i};
            while (j < segment.text.size() && !is_ifs_space(segment.text[j])) {
                ++j;
            }
            current.append(segment.text, i, j - i);
            started = true;
            i = j;
        }
    }
    if (started) {
        out.push_back(std::move(current));
    }
    return true;
}

// Map a parser redirect (flags + target atom) onto a value-layer RedirectPlan.
// Only single-file stdin/stdout/stderr forms are modeled; fd-duplication (`2>&1`),
// combined `&>` and JS-buffer targets punt.
bool lower_redirect(std::uint8_t flags, const Redirect& redirect, LowerCtx& ctx,
                    std::vector<RedirectPlan>& out) {
    if (redirect.kind != RedirKind::Atom) {
        return false;
    }
    if (flags & rf::DUPLICATE_OUT) {
        return false;
    }
    const auto target = atom_literal(redirect.atom, ctx);
    if (!target) {
        return false;
    }
    const bool append = (flags & rf::APPEND) != 0;
    const bool in = (flags & rf::STDIN) != 0;
    const bool stdout_ = (flags & rf::STDOUT) != 0;
    const bool stderr_ = (flags & rf::STDERR) != 0;

    if (in && !stdout_ && !stderr_) {
        if (append) {
            return false;  // `<<` heredoc: unsupported.
        }
        RedirectPlan plan;
        plan.channel = IoChannel::Stdin;
        plan.action = RedirectAction::Read;
        plan.target = *target;
        out.push_back(std::move(plan));
        return true;
    }
    // `&>` / `&>>` set BOTH stdout+stderr — lower to two plans; apply_redirects_
    // opens/dup2s each channel independently (bun sends both to the same target).
    const auto act = append ? RedirectAction::Append : RedirectAction::Write;
    if (stdout_) {
        RedirectPlan p;
        p.channel = IoChannel::Stdout;
        p.action = act;
        p.target = *target;
        out.push_back(std::move(p));
    }
    if (stderr_) {
        RedirectPlan p;
        p.channel = IoChannel::Stderr;
        p.action = act;
        p.target = *target;
        out.push_back(std::move(p));
    }
    return stdout_ || stderr_;
}

bool lower_assignments(const std::vector<Assign>& assigns, LowerCtx& ctx,
                       PipelineStage& stage) {
    for (const auto& assign : assigns) {
        const auto value = atom_literal(assign.value, ctx);
        if (!value) {
            return false;
        }
        stage.assignments.emplace_back(std::string{assign.label}, *value);
    }
    if (!assigns.empty()) {
        ctx.state_mutated = true;
    }
    return true;
}

std::optional<PipelineStage> lower_cmd(const Cmd& cmd, LowerCtx& ctx) {
    PipelineStage stage;
    stage.kind = PipelineStageKind::Command;
    if (!lower_assignments(cmd.assigns, ctx, stage)) {
        return std::nullopt;
    }
    for (const auto& atom : cmd.name_and_args) {
        if (!lower_word(atom, ctx, stage.argv)) {
            return std::nullopt;
        }
    }
    // A stage that repoints cwd or the exported environment invalidates the eager
    // `$(...)` evaluation for everything after it — see LowerCtx::state_mutated.
    if (!stage.argv.empty() && (stage.argv.front() == "cd" || stage.argv.front() == "export" ||
                                stage.argv.front() == "unset")) {
        ctx.state_mutated = true;
    }
    if (cmd.has_redirect_file) {
        if (!lower_redirect(cmd.redirect, cmd.redirect_file, ctx, stage.redirects)) {
            return std::nullopt;
        }
    } else if (cmd.redirect != 0) {
        return std::nullopt;  // redirect operator without a modeled file target.
    }
    if (stage.argv.empty() && !stage.assignments.empty()) {
        stage.kind = PipelineStageKind::Assignments;
    }
    return stage;
}

std::optional<ExecutionNode> lower_expr(const Expr& expr, LowerCtx& ctx);

// Lower a `[[ EXPR ]]` into an argv-form command stage the interpreter's `[[`
// builtin evaluates: argv = ["[[", op, operand...]. Only the parser-supported
// operators reach here; operands carrying an expansion punt to the fallback.
std::optional<PipelineStage> lower_condexpr(const CondExpr& cond, LowerCtx& ctx) {
    PipelineStage stage;
    stage.kind = PipelineStageKind::Command;
    stage.argv.emplace_back("[[");
    stage.argv.push_back(cond.op);
    for (const auto& atom : cond.args) {
        const auto word = atom_literal(atom, ctx);
        if (!word) {
            return std::nullopt;
        }
        stage.argv.push_back(*word);
    }
    return stage;
}

std::optional<ExecutionNode> lower_pipeline(const Pipeline& pipeline, LowerCtx& ctx) {
    PipelinePlan plan;
    for (const auto& item : pipeline.items) {
        if (item.kind == PIKind::Cmd) {
            if (!item.cmd) {
                return std::nullopt;
            }
            auto stage = lower_cmd(*item.cmd, ctx);
            if (!stage) {
                return std::nullopt;
            }
            plan.stages.push_back(std::move(*stage));
        } else if (item.kind == PIKind::Assigns) {
            PipelineStage stage;
            stage.kind = PipelineStageKind::Assignments;
            if (!lower_assignments(item.assigns, ctx, stage)) {
                return std::nullopt;
            }
            plan.stages.push_back(std::move(stage));
        } else if (item.kind == PIKind::CondExpr) {
            if (!item.condexpr) {
                return std::nullopt;
            }
            auto stage = lower_condexpr(*item.condexpr, ctx);
            if (!stage) {
                return std::nullopt;
            }
            plan.stages.push_back(std::move(*stage));
        } else {
            return std::nullopt;  // subshell / if pipeline items punt.
        }
    }
    ExecutionNode node;
    node.kind = PlanNodeKind::Pipeline;
    node.pipeline = std::move(plan);
    return node;
}

std::optional<ExecutionNode> lower_expr(const Expr& expr, LowerCtx& ctx) {
    switch (expr.kind) {
        case ExprKind::Cmd: {
            if (!expr.cmd) {
                return std::nullopt;
            }
            auto stage = lower_cmd(*expr.cmd, ctx);
            if (!stage) {
                return std::nullopt;
            }
            ExecutionNode node;
            node.kind = PlanNodeKind::Pipeline;
            node.pipeline.stages.push_back(std::move(*stage));
            return node;
        }
        case ExprKind::Assign: {
            PipelineStage stage;
            stage.kind = PipelineStageKind::Assignments;
            if (!lower_assignments(expr.assigns, ctx, stage)) {
                return std::nullopt;
            }
            ExecutionNode node;
            node.kind = PlanNodeKind::Pipeline;
            node.pipeline.stages.push_back(std::move(stage));
            return node;
        }
        case ExprKind::Pipeline: {
            if (!expr.pipeline) {
                return std::nullopt;
            }
            return lower_pipeline(*expr.pipeline, ctx);
        }
        case ExprKind::Binary: {
            if (!expr.binary) {
                return std::nullopt;
            }
            auto left = lower_expr(expr.binary->left, ctx);
            auto right = lower_expr(expr.binary->right, ctx);
            if (!left || !right) {
                return std::nullopt;
            }
            ExecutionNode node;
            node.kind = expr.binary->op == BinaryOp::And ? PlanNodeKind::And
                                                         : PlanNodeKind::Or;
            node.left = std::make_shared<ExecutionNode>(std::move(*left));
            node.right = std::make_shared<ExecutionNode>(std::move(*right));
            return node;
        }
        case ExprKind::CondExpr: {
            if (!expr.condexpr) {
                return std::nullopt;
            }
            auto stage = lower_condexpr(*expr.condexpr, ctx);
            if (!stage) {
                return std::nullopt;
            }
            ExecutionNode node;
            node.kind = PlanNodeKind::Pipeline;
            node.pipeline.stages.push_back(std::move(*stage));
            return node;
        }
        default:
            return std::nullopt;  // Subshell / If / Async.
    }
}

// Lower a parsed Script into an ExecutionPlan, or nullopt if any node is outside
// the interpreter-backed subset (so the JS layer keeps the /bin/sh fallback).
std::optional<ExecutionPlan> lower_script(const Script& script, LowerCtx& ctx) {
    ExecutionPlan plan;
    for (const auto& stmt : script.stmts) {
        for (const auto& expr : stmt.exprs) {
            auto node = lower_expr(expr, ctx);
            if (!node) {
                return std::nullopt;
            }
            plan.nodes.push_back(std::move(*node));
        }
    }
    if (plan.nodes.empty()) {
        return std::nullopt;
    }
    return plan;
}

std::string drain_fd(int fd) {
    std::string out;
    char buf[4096];
    ssize_t n{0};
    while ((n = ::read(fd, buf, sizeof buf)) > 0) {
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

// Fork a child, run the plan through the shell interpreter with stdout/stderr
// captured into pipes, honoring cwd and the caller-provided environment. stdin is
// wired to /dev/null (Bun.$ ignores stdin). stderr is drained on a helper thread so
// a child that fills both pipes cannot deadlock.
std::optional<ExecResult> run_plan_captured(const ExecutionPlan& plan,
                                            const std::string& cwd,
                                            std::vector<char*>& envp, bool have_env) {
    int outPipe[2];
    int errPipe[2];
    if (::pipe(outPipe) != 0) {
        return std::nullopt;
    }
    if (::pipe(errPipe) != 0) {
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        return std::nullopt;
    }

    const pid_t pid{::fork()};
    if (pid < 0) {
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::close(errPipe[0]);
        ::close(errPipe[1]);
        return std::nullopt;
    }
    if (pid == 0) {  // child
        ::close(outPipe[0]);
        ::close(errPipe[0]);
        const int devnull{::open("/dev/null", O_RDONLY)};
        if (devnull >= 0) {
            ::dup2(devnull, 0);
            if (devnull > 0) {
                ::close(devnull);
            }
        }
        ::dup2(outPipe[1], 1);
        ::dup2(errPipe[1], 2);
        ::close(outPipe[1]);
        ::close(errPipe[1]);
        if (!cwd.empty()) {
            if (::chdir(cwd.c_str()) != 0) {
                ::_exit(1);
            }
        }
        if (have_env) {
            environ = envp.data();
        }
        mbun::shell::execution::ShellInterpreter interpreter;
        const int code{interpreter.run_plan(plan)};
        ::_exit(code & 0xff);
    }

    // parent
    ::close(outPipe[1]);
    ::close(errPipe[1]);
    ExecResult result;
    std::thread errThread{[&] { result.stderr_bytes = drain_fd(errPipe[0]); }};
    result.stdout_bytes = drain_fd(outPipe[0]);
    errThread.join();

    int status{0};
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            break;
        }
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    } else {
        result.exit_code = 1;
    }
    return result;
}

// `mbun exec <script>` — bun's standalone-shell entrypoint. bun's ExecCommand
// (src/cli/exec_command.rs:78-84) hands the positional straight to
// Interpreter::init_and_run_from_source (src/runtime/shell/interpreter.rs:681-689)
// and then exits with the script's own code (exec_command.rs:102). Two ways this
// differs from run_plan_captured above: the script inherits the CLI's stdio rather
// than being fenced behind pipes, and there is no wrapper fork -- builtins write to
// fd 1/2 directly and the interpreter forks per external command itself. Diagnostics
// name the script "[eval]", matching the synthetic path at exec_command.rs:71-72.
int run_source_inherited(const std::string& source) {
#if defined(_WIN32)
    (void)source;
    std::cerr << "error: `mbun exec` is not supported on Windows yet\n";
    return 1;
#else
    // A script off the command line carries no JS object refs or template markers,
    // so both lexer counts are zero (same reasoning as bunsh_exec_cb).
    mbun::shell::Lexer lexer{source, 0, 0};
    lexer.run();
    if (!lexer.errors().empty()) {
        std::cerr << "error: Failed to run script [eval] due to error " << lexer.errors().front()
                  << "\n";
        return 1;
    }
    mbun::shell::Parser parser{lexer.tokens(), lexer.strpool()};
    mbun::shell::Script parsed{parser.parse()};
    if (!parser.errors().empty()) {
        std::cerr << "error: Failed to run script [eval] due to error " << parser.errors().front()
                  << "\n";
        return 1;
    }

    // `mbun exec` inherits the process cwd and environ, so the eager `$(...)`
    // evaluation runs with an empty cwd override and no env block: the substituted
    // child then sees exactly what the outer script will see.
    static std::vector<char*> noEnv;
    LowerCtx ctx;
    ctx.envp = &noEnv;
    auto plan = lower_script(parsed, ctx);
    if (!plan) {
        // bun's interpreter covers the whole grammar; mbun's lowering does not yet
        // (subshell / if / cond-expr / async -- see lower_expr's default arm). Say so
        // instead of silently running something else: Bun.$'s /bin/sh fallback is a JS
        // layer decision and would make `exec` claim a fidelity it does not have.
        std::cerr << "error: Failed to run script [eval] due to error "
                     "unsupported shell construct (mbun's shell interpreter does not "
                     "implement this yet)\n";
        return 1;
    }

    mbun::shell::execution::ShellInterpreter interpreter;
    return interpreter.run_plan(*plan);
#endif
}

}  // namespace bunsh

// __mbunShellNative.exec(script, cwd, env) — run a compiled shell script through the
// mbun interpreter. Returns { exitCode, stdout, stderr } (stdout/stderr base64) on
// success, or null when the script uses a construct outside the interpreter-backed
// subset (JS then falls back to the spawn-based path).
JSValueRef bunsh_exec_cb(JSContextRef context, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef arguments[], JSValueRef* exception) {
#if defined(_WIN32)
    (void)argc;
    (void)arguments;
    (void)exception;
    return JSValueMakeNull(context);
#else
    if (argc < 1 || !JSValueIsString(context, arguments[0])) {
        return JSValueMakeNull(context);
    }
    // THE PERMISSION GATE FOR Bun.$.
    //
    // This is the single point at which the in-process shell interpreter is
    // reached from JS, and it forks/execvpe's external commands, opens redirect
    // targets, and runs file-touching builtins (cat/cp/rm/mv) — all through plain
    // POSIX syscalls in mbun.shell, which is pure logic and knows nothing about
    // the permission model. Without this check `--permission` was a no-op for
    // Bun.$: `Bun.$`cat /etc/shadow`` read files, `> file` wrote them, and
    // ``${process.execPath} -e ...`` re-entered the runtime with no sandbox at
    // all, while fs.readFileSync / child_process / Bun.spawn were all being
    // denied.
    //
    // Charged to ChildProcess, which is the scope node uses for every
    // "run a program" capability (src/permission/child_process_permission.cc):
    // Bun.$ is exactly that, and node's model already treats a granted
    // ChildProcess as the end of the sandbox. Returning null here would make the
    // JS layer fall back to its spawn-based path, so raise the error instead —
    // the caller must see ERR_ACCESS_DENIED, not a silent fallback.
    if (permission_deny_scope(context, exception, mbun::permission::Scope::ChildProcess,
                              "Bun.$")) {
        return JSValueMakeUndefined(context);
    }
    std::string script;
    if (!shell_value_to_string(context, arguments[0], script, exception)) {
        return JSValueMakeNull(context);
    }

    // Lex + parse the compiled POSIX script (no JS object refs or template markers
    // survive compile_template, so both counts are zero).
    mbun::shell::Lexer lexer{script, 0, 0};
    lexer.run();
    if (!lexer.errors().empty()) {
        return JSValueMakeNull(context);
    }
    mbun::shell::Parser parser{lexer.tokens(), lexer.strpool()};
    mbun::shell::Script parsed{parser.parse()};
    if (!parser.errors().empty()) {
        return JSValueMakeNull(context);
    }

    std::string cwd;
    if (argc > 1 && JSValueIsString(context, arguments[1])) {
        shell_value_to_string(context, arguments[1], cwd, nullptr);
    }

    // Materialize the environment block in the parent so the forked child does not
    // allocate for it (the char* array is inherited copy-on-write).
    std::vector<std::string> envStrings;
    std::vector<char*> envp;
    bool haveEnv{false};
    if (argc > 2 && JSValueIsObject(context, arguments[2])) {
        JSObjectRef envObj{JSValueToObject(context, arguments[2], nullptr)};
        if (envObj != nullptr) {
            haveEnv = true;
            JSPropertyNameArrayRef names{JSObjectCopyPropertyNames(context, envObj)};
            const std::size_t count{JSPropertyNameArrayGetCount(names)};
            for (std::size_t i = 0; i < count; ++i) {
                JSStringRef key{JSPropertyNameArrayGetNameAtIndex(names, i)};
                JSValueRef value{JSObjectGetProperty(context, envObj, key, nullptr)};
                if (JSValueIsUndefined(context, value) || JSValueIsNull(context, value)) {
                    continue;
                }
                std::string keyStr{to_utf8(key)};
                std::string valStr;
                if (!shell_value_to_string(context, value, valStr, nullptr)) {
                    continue;
                }
                envStrings.push_back(keyStr + "=" + valStr);
            }
            JSPropertyNameArrayRelease(names);
        }
    }
    if (haveEnv) {
        envp.reserve(envStrings.size() + 1);
        for (auto& entry : envStrings) {
            envp.push_back(entry.data());
        }
        envp.push_back(nullptr);
    }

    // Lowering must run *after* cwd/env are materialized: a `$(...)` is expanded
    // eagerly here and the substituted child needs the same cwd and environment the
    // outer script will run under. Its stderr is captured into `substStderr` and
    // prepended below, since those commands genuinely ran before the outer plan.
    std::string substStderr;
    bunsh::LowerCtx ctx;
    ctx.cwd = cwd;
    ctx.envp = &envp;
    ctx.have_env = haveEnv;
    ctx.stderr_sink = &substStderr;
    auto plan = bunsh::lower_script(parsed, ctx);
    if (!plan) {
        return JSValueMakeNull(context);
    }

    auto result = bunsh::run_plan_captured(*plan, cwd, envp, haveEnv);
    if (!result) {
        return JSValueMakeNull(context);
    }
    if (!substStderr.empty()) {
        result->stderr_bytes.insert(0, substStderr);
    }

    JSObjectRef out{JSObjectMake(context, nullptr, nullptr)};
    set_prop(context, out, "exitCode",
             JSValueMakeNumber(context, static_cast<double>(result->exit_code)));
    set_prop(context, out, "stdout",
             make_string(context, b64_encode(reinterpret_cast<const unsigned char*>(
                                                  result->stdout_bytes.data()),
                                              result->stdout_bytes.size())));
    set_prop(context, out, "stderr",
             make_string(context, b64_encode(reinterpret_cast<const unsigned char*>(
                                                  result->stderr_bytes.data()),
                                              result->stderr_bytes.size())));
    return out;
#endif
}
