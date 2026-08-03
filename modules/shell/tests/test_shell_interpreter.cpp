// Real POSIX execution backend checks (U.3): fork/exec, pipes, redirects,
// sequences, env, cwd, exit codes, and builtins. These spawn real subprocesses
// and MUST be run through tools/integration/safe-test.sh.
import std;
import mbun.shell;

namespace {

using namespace mbun::shell::execution;

int gChecks{0};
int gFailures{0};

void check(bool condition, std::string_view label) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::println("FAIL: {}", label);
    }
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in{p, std::ios::binary};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const std::filesystem::path& p, std::string_view data) {
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    out << data;
}

PipelineStage cmd(std::vector<std::string> argv, std::vector<RedirectPlan> redirects = {}) {
    PipelineStage s;
    s.kind = PipelineStageKind::Command;
    s.argv = std::move(argv);
    s.redirects = std::move(redirects);
    return s;
}

PipelinePlan one(PipelineStage s) {
    PipelinePlan p;
    p.stages.push_back(std::move(s));
    return p;
}

RedirectPlan out_to(const std::filesystem::path& path, RedirectAction action = RedirectAction::Write) {
    RedirectPlan r;
    r.channel = IoChannel::Stdout;
    r.action = action;
    r.target = path.string();
    return r;
}

RedirectPlan in_from(const std::filesystem::path& path) {
    RedirectPlan r;
    r.channel = IoChannel::Stdin;
    r.action = RedirectAction::Read;
    r.target = path.string();
    return r;
}

RedirectPlan err_to(const std::filesystem::path& path) {
    RedirectPlan r;
    r.channel = IoChannel::Stderr;
    r.action = RedirectAction::Write;
    r.target = path.string();
    return r;
}

std::filesystem::path make_tmpdir() {
    auto base = std::filesystem::temp_directory_path();
    std::random_device rd;
    auto dir = base / ("mbun-shell-" + std::to_string(rd()) + "-" +
                       std::to_string(rd()));
    std::filesystem::create_directories(dir);
    return std::filesystem::weakly_canonical(dir);
}

void test_builtins_exit_codes() {
    ShellInterpreter sh;
    check(sh.run_pipeline(one(cmd({"true"}))) == 0, "true builtin exit 0");
    check(sh.run_pipeline(one(cmd({"false"}))) == 1, "false builtin exit 1");
    check(sh.run_pipeline(one(cmd({":"}))) == 0, "colon builtin exit 0");
}

void test_echo_redirect(const std::filesystem::path& dir) {
    ShellInterpreter sh;
    auto f = dir / "echo.txt";
    check(sh.run_pipeline(one(cmd({"echo", "hello", "world"}, {out_to(f)}))) == 0,
          "echo redirect exit 0");
    check(read_file(f) == "hello world\n", "echo writes space-joined + newline");

    auto f2 = dir / "echo_n.txt";
    sh.run_pipeline(one(cmd({"echo", "-n", "hi"}, {out_to(f2)})));
    check(read_file(f2) == "hi", "echo -n suppresses newline");

    auto f3 = dir / "append.txt";
    sh.run_pipeline(one(cmd({"echo", "a"}, {out_to(f3)})));
    sh.run_pipeline(one(cmd({"echo", "b"}, {out_to(f3, RedirectAction::Append)})));
    check(read_file(f3) == "a\nb\n", "append preserves existing content");
}

void test_external_exit_code() {
    ShellInterpreter sh;
    check(sh.run_pipeline(one(cmd({"sh", "-c", "exit 3"}))) == 3, "external exit code 3");
    check(sh.run_pipeline(one(cmd({"sh", "-c", "exit 0"}))) == 0, "external exit code 0");
    check(sh.run_pipeline(one(cmd({"this-command-does-not-exist-xyz"}))) == 127,
          "missing command is 127");
}

void test_pipe(const std::filesystem::path& dir) {
    ShellInterpreter sh;
    auto f = dir / "pipe.txt";
    // echo hello | cat > f   (cat is an external command on POSIX)
    PipelinePlan p;
    p.stages.push_back(cmd({"echo", "piped"}));
    p.stages.push_back(cmd({"cat"}, {out_to(f)}));
    check(sh.run_pipeline(p) == 0, "pipe exit code from last stage");
    check(read_file(f) == "piped\n", "pipe carries data echo|cat");

    // Three-stage pipe: printf | tr | tr, capture result.
    auto f2 = dir / "pipe3.txt";
    PipelinePlan p3;
    p3.stages.push_back(cmd({"printf", "abc"}));
    p3.stages.push_back(cmd({"tr", "a-z", "A-Z"}));
    p3.stages.push_back(cmd({"tr", "B", "X"}, {out_to(f2)}));
    sh.run_pipeline(p3);
    check(read_file(f2) == "AXC", "three-stage pipeline");
}

void test_input_redirect(const std::filesystem::path& dir) {
    ShellInterpreter sh;
    auto in = dir / "in.txt";
    auto out = dir / "out.txt";
    write_file(in, "from-file\n");
    // cat < in > out
    check(sh.run_pipeline(one(cmd({"cat"}, {in_from(in), out_to(out)}))) == 0,
          "input redirect exit 0");
    check(read_file(out) == "from-file\n", "stdin redirect feeds command");
}

void test_stderr_redirect(const std::filesystem::path& dir) {
    ShellInterpreter sh;
    auto ferr = dir / "err.txt";
    check(sh.run_pipeline(one(cmd({"sh", "-c", "echo oops 1>&2"}, {err_to(ferr)}))) == 0,
          "stderr redirect exit 0");
    check(read_file(ferr) == "oops\n", "2> captures stderr");

    // 2>&1 duplicate: stdout redirected to file, stderr duplicates stdout.
    auto fboth = dir / "both.txt";
    RedirectPlan dup;
    dup.channel = IoChannel::Stderr;
    dup.action = RedirectAction::Duplicate;
    dup.source_fd = 1;
    check(sh.run_pipeline(one(cmd({"sh", "-c", "echo out; echo err 1>&2"},
                                  {out_to(fboth), dup}))) == 0,
          "2>&1 exit 0");
    check(read_file(fboth) == "out\nerr\n", "2>&1 merges stderr into stdout target");
}

void test_sequences(const std::filesystem::path& dir) {
    // true && echo ok > f  → f written
    {
        ShellInterpreter sh;
        auto f = dir / "and_ok.txt";
        ExecutionNode andNode;
        andNode.kind = PlanNodeKind::And;
        andNode.left = std::make_shared<ExecutionNode>(
            ExecutionNode{PlanNodeKind::Pipeline, one(cmd({"true"})), {}, {}});
        andNode.right = std::make_shared<ExecutionNode>(
            ExecutionNode{PlanNodeKind::Pipeline, one(cmd({"echo", "ok"}, {out_to(f)})), {}, {}});
        check(sh.run_node(andNode) == 0, "true && echo -> 0");
        check(read_file(f) == "ok\n", "&& runs right when left succeeds");
    }
    // false && echo no > f  → f NOT written
    {
        ShellInterpreter sh;
        auto f = dir / "and_skip.txt";
        ExecutionNode andNode;
        andNode.kind = PlanNodeKind::And;
        andNode.left = std::make_shared<ExecutionNode>(
            ExecutionNode{PlanNodeKind::Pipeline, one(cmd({"false"})), {}, {}});
        andNode.right = std::make_shared<ExecutionNode>(
            ExecutionNode{PlanNodeKind::Pipeline, one(cmd({"echo", "no"}, {out_to(f)})), {}, {}});
        check(sh.run_node(andNode) == 1, "false && echo -> 1");
        check(!std::filesystem::exists(f), "&& short-circuits right when left fails");
    }
    // false || echo ok > f  → f written
    {
        ShellInterpreter sh;
        auto f = dir / "or_ok.txt";
        ExecutionNode orNode;
        orNode.kind = PlanNodeKind::Or;
        orNode.left = std::make_shared<ExecutionNode>(
            ExecutionNode{PlanNodeKind::Pipeline, one(cmd({"false"})), {}, {}});
        orNode.right = std::make_shared<ExecutionNode>(
            ExecutionNode{PlanNodeKind::Pipeline, one(cmd({"echo", "recovered"}, {out_to(f)})), {}, {}});
        check(sh.run_node(orNode) == 0, "false || echo -> 0");
        check(read_file(f) == "recovered\n", "|| runs right when left fails");
    }
    // Sequence `;` via ExecutionPlan: two pipelines, last exit code wins.
    {
        ShellInterpreter sh;
        ExecutionPlan plan;
        plan.nodes.push_back(ExecutionNode{PlanNodeKind::Pipeline, one(cmd({"true"})), {}, {}});
        plan.nodes.push_back(ExecutionNode{PlanNodeKind::Pipeline, one(cmd({"false"})), {}, {}});
        check(sh.run_plan(plan) == 1, "; sequence returns last exit code");
    }
}

void test_cd_pwd(const std::filesystem::path& dir) {
    ShellInterpreter sh;
    auto sub = dir / "cwdtest";
    std::filesystem::create_directories(sub);
    auto canonical = std::filesystem::weakly_canonical(sub);

    check(sh.run_pipeline(one(cmd({"cd", sub.string()}))) == 0, "cd succeeds");
    check(std::filesystem::weakly_canonical(sh.cwd()) == canonical, "cd updates interp cwd");

    auto f = dir / "pwd.txt";
    sh.run_pipeline(one(cmd({"pwd"}, {out_to(f)})));
    check(read_file(f) == canonical.string() + "\n", "pwd reflects cwd after cd");

    check(sh.run_pipeline(one(cmd({"cd", "/no/such/dir/mbun"}))) != 0, "cd to missing dir fails");
}

void test_env_export(const std::filesystem::path& dir) {
    ShellInterpreter sh;
    // export FOO=bar; then external command inherits it.
    check(sh.run_pipeline(one(cmd({"export", "FOO=bar"}))) == 0, "export exit 0");
    check(sh.get_env("FOO") == std::optional<std::string>{"bar"}, "export sets shell env");

    auto f = dir / "env.txt";
    sh.run_pipeline(one(cmd({"sh", "-c", "printf %s \"$FOO\""}, {out_to(f)})));
    check(read_file(f) == "bar", "external command inherits exported env");

    // Per-command assignment: BAZ=qux only for that command.
    auto f2 = dir / "env2.txt";
    PipelineStage s = cmd({"sh", "-c", "printf %s \"$BAZ\""}, {out_to(f2)});
    s.assignments.emplace_back("BAZ", "qux");
    sh.run_pipeline(one(std::move(s)));
    check(read_file(f2) == "qux", "per-command assignment reaches child env");
    check(!sh.get_env("BAZ").has_value(), "per-command assignment does not leak to shell");

    // Bare assignment stage sets shell env.
    PipelinePlan bare;
    PipelineStage as;
    as.kind = PipelineStageKind::Assignments;
    as.assignments.emplace_back("SHELLVAR", "yes");
    bare.stages.push_back(std::move(as));
    sh.run_pipeline(bare);
    check(sh.get_env("SHELLVAR") == std::optional<std::string>{"yes"},
          "bare assignment sets shell env");

    // unset removes it.
    sh.run_pipeline(one(cmd({"unset", "SHELLVAR"})));
    check(!sh.get_env("SHELLVAR").has_value(), "unset removes shell env");
}

}  // namespace

int main() {
    auto dir = make_tmpdir();

    test_builtins_exit_codes();
    test_echo_redirect(dir);
    test_external_exit_code();
    test_pipe(dir);
    test_input_redirect(dir);
    test_stderr_redirect(dir);
    test_sequences(dir);
    test_cd_pwd(dir);
    test_env_export(dir);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    std::println("mbun.shell interpreter (POSIX backend): {} checks, {} failures",
                 gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
