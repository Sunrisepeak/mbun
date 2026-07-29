// Bun `$` execution bridge: run a compiled POSIX shell script through mbun's own
// interpreter (modules/shell — real fork/exec/pipe/redirect + in-process builtins)
// instead of delegating to the host /bin/sh. This is the execution-binding half of
// the Bun.$ wiring; the template-tag → script compilation lives in shell.inc and the
// JS-facing ShellPromise/ShellError model lives in the builtins/shell.cppm partition.
//
// Scope (safety-first, no-regression): only fully-literal command graphs are lowered
// to the interpreter here — commands, pipes (`|`), sequences/conditionals
// (`; && ||`), assignments and simple file redirects (`> >> < 2>`). Any construct the
// lowering cannot faithfully model (variable/tilde/glob/brace/command-substitution
// expansion, subshell, if, `[[ ]]`, fd-duplication, `&`, JS-buffer redirect targets)
// makes exec return null so the JS layer falls back to the existing spawn path.
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

// Resolve a parser Atom to its literal text. Returns nullopt when the atom carries
// any expansion (variable/glob/brace/tilde/command-substitution) the literal-only
// lowering must not silently drop — the caller then punts to the fallback path.
std::optional<std::string> atom_literal(const Atom& atom) {
    if (atom.kind == AtomKind::Simple) {
        switch (atom.simple.kind) {
            case SAKind::Text:
                return std::string{atom.simple.text};
            case SAKind::QuotedEmpty:
                return std::string{};
            default:
                return brace_token_text(atom.simple.kind);
        }
    }
    if (atom.compound.glob_hint || atom.compound.brace_expansion_hint) {
        return std::nullopt;
    }
    std::string out;
    for (const auto& sub : atom.compound.atoms) {
        if (sub.kind == SAKind::Text) {
            out += sub.text;
        } else if (sub.kind != SAKind::QuotedEmpty) {
            const auto literal = brace_token_text(sub.kind);
            if (!literal) return std::nullopt;
            out += *literal;
        }
    }
    return out;
}

// Map a parser redirect (flags + target atom) onto a value-layer RedirectPlan.
// Only single-file stdin/stdout/stderr forms are modeled; fd-duplication (`2>&1`),
// combined `&>` and JS-buffer targets punt.
bool lower_redirect(std::uint8_t flags, const Redirect& redirect,
                    std::vector<RedirectPlan>& out) {
    if (redirect.kind != RedirKind::Atom) {
        return false;
    }
    if (flags & rf::DUPLICATE_OUT) {
        return false;
    }
    const auto target = atom_literal(redirect.atom);
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

bool lower_assignments(const std::vector<Assign>& assigns, PipelineStage& stage) {
    for (const auto& assign : assigns) {
        const auto value = atom_literal(assign.value);
        if (!value) {
            return false;
        }
        stage.assignments.emplace_back(std::string{assign.label}, *value);
    }
    return true;
}

std::optional<PipelineStage> lower_cmd(const Cmd& cmd) {
    PipelineStage stage;
    stage.kind = PipelineStageKind::Command;
    if (!lower_assignments(cmd.assigns, stage)) {
        return std::nullopt;
    }
    for (const auto& atom : cmd.name_and_args) {
        const auto word = atom_literal(atom);
        if (!word) {
            return std::nullopt;
        }
        stage.argv.push_back(*word);
    }
    if (cmd.has_redirect_file) {
        if (!lower_redirect(cmd.redirect, cmd.redirect_file, stage.redirects)) {
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

std::optional<ExecutionNode> lower_expr(const Expr& expr);

// Lower a `[[ EXPR ]]` into an argv-form command stage the interpreter's `[[`
// builtin evaluates: argv = ["[[", op, operand...]. Only the parser-supported
// operators reach here; operands carrying an expansion punt to the fallback.
std::optional<PipelineStage> lower_condexpr(const CondExpr& cond) {
    PipelineStage stage;
    stage.kind = PipelineStageKind::Command;
    stage.argv.emplace_back("[[");
    stage.argv.push_back(cond.op);
    for (const auto& atom : cond.args) {
        const auto word = atom_literal(atom);
        if (!word) {
            return std::nullopt;
        }
        stage.argv.push_back(*word);
    }
    return stage;
}

std::optional<ExecutionNode> lower_pipeline(const Pipeline& pipeline) {
    PipelinePlan plan;
    for (const auto& item : pipeline.items) {
        if (item.kind == PIKind::Cmd) {
            if (!item.cmd) {
                return std::nullopt;
            }
            auto stage = lower_cmd(*item.cmd);
            if (!stage) {
                return std::nullopt;
            }
            plan.stages.push_back(std::move(*stage));
        } else if (item.kind == PIKind::Assigns) {
            PipelineStage stage;
            stage.kind = PipelineStageKind::Assignments;
            if (!lower_assignments(item.assigns, stage)) {
                return std::nullopt;
            }
            plan.stages.push_back(std::move(stage));
        } else if (item.kind == PIKind::CondExpr) {
            if (!item.condexpr) {
                return std::nullopt;
            }
            auto stage = lower_condexpr(*item.condexpr);
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

std::optional<ExecutionNode> lower_expr(const Expr& expr) {
    switch (expr.kind) {
        case ExprKind::Cmd: {
            if (!expr.cmd) {
                return std::nullopt;
            }
            auto stage = lower_cmd(*expr.cmd);
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
            if (!lower_assignments(expr.assigns, stage)) {
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
            return lower_pipeline(*expr.pipeline);
        }
        case ExprKind::Binary: {
            if (!expr.binary) {
                return std::nullopt;
            }
            auto left = lower_expr(expr.binary->left);
            auto right = lower_expr(expr.binary->right);
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
            auto stage = lower_condexpr(*expr.condexpr);
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
std::optional<ExecutionPlan> lower_script(const Script& script) {
    ExecutionPlan plan;
    for (const auto& stmt : script.stmts) {
        for (const auto& expr : stmt.exprs) {
            auto node = lower_expr(expr);
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

struct ExecResult {
    int exit_code{0};
    std::string stdout_bytes;
    std::string stderr_bytes;
};

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

    auto plan = lower_script(parsed);
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

    auto plan = bunsh::lower_script(parsed);
    if (!plan) {
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

    auto result = bunsh::run_plan_captured(*plan, cwd, envp, haveEnv);
    if (!result) {
        return JSValueMakeNull(context);
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
