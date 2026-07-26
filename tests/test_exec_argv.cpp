// process.execArgv derivation — mbun::cli::derive_exec_argv.
//
// Acceptance source: bun's own re-parser (compat/bun/src/runtime/node/
// node_process.rs `create_exec_argv`) and node's test/common/index.js, which
// re-execs a test through process.execPath whenever a flag from the file's
// `// Flags:` header is missing from process.execArgv. mbun reported an empty
// execArgv for every invocation, so those files re-spawned themselves without
// bound until the process group was aborted.
import std;
import mbun.cli;

namespace {

int gFailed = 0;

void expect(bool cond, std::string_view what) {
    if (!cond) {
        ++gFailed;
        std::println("  FAIL: {}", what);
    }
}

std::vector<std::string> derive(std::initializer_list<std::string_view> args) {
    return mbun::cli::derive_exec_argv(
        std::span<const std::string_view>{args.begin(), args.size()});
}

std::string join(const std::vector<std::string>& v) {
    std::string out{"["};
    for (std::size_t i{0}; i < v.size(); ++i) {
        if (i != 0) out += ", ";
        out += v[i];
    }
    return out + "]";
}

void expect_eq(const std::vector<std::string>& got, const std::vector<std::string>& want,
               std::string_view what) {
    if (got != want) {
        ++gFailed;
        std::println("  FAIL: {}\n    got  {}\n    want {}", what, join(got), join(want));
    }
}

} // namespace

int main() {
    // No flags at all — a bare script run keeps execArgv empty (node/bun agree).
    expect_eq(derive({"file.js"}), {}, "bare script → empty execArgv");
    expect_eq(derive({}), {}, "no args → empty execArgv");

    // The whole point: the node corpus's `// Flags:` headers must survive into
    // execArgv, or test/common/index.js re-spawns the test forever.
    expect_eq(derive({"--no-warnings", "file.js"}), {"--no-warnings"},
              "--no-warnings reaches execArgv");
    expect_eq(derive({"--expose-internals", "--expose-gc", "file.js"}),
              {"--expose-internals", "--expose-gc"}, "several boolean flags");
    expect_eq(derive({"--permission", "--allow-fs-read=*", "file.js"}),
              {"--permission", "--allow-fs-read=*"}, "--flag=value stays one token");

    // The script and everything after it belong to process.argv, never execArgv.
    expect_eq(derive({"--no-warnings", "file.js", "--not-mine", "arg"}), {"--no-warnings"},
              "parsing stops at the entry point");

    // A value-taking flag owns the next token, so it is not mistaken for the
    // script (node re-execs with the space form: `-r mod file.js`).
    expect_eq(derive({"-r", "mod.js", "file.js"}), {"-r", "mod.js"},
              "-r consumes its value token");
    expect_eq(derive({"--snapshot-blob", "b.bin", "--build-snapshot", "file.js"}),
              {"--snapshot-blob", "b.bin", "--build-snapshot"},
              "--snapshot-blob consumes its value token");
    // …but a boolean flag must NOT swallow the script.
    expect_eq(derive({"--expose-gc", "file.js"}), {"--expose-gc"},
              "a boolean flag never swallows the script");

    // `bun run script` — one `run` subcommand is skipped, as in bun's re-parser.
    expect_eq(derive({"--smol", "run", "start"}), {"--smol"}, "run subcommand is skipped");
    expect_eq(derive({"run", "start"}), {}, "run with no flags → empty execArgv");

    // `-e <code>`: node/bun both report the eval pair in execArgv (child_process
    // fork() relies on it to strip the pair back out).
    expect_eq(derive({"-e", "console.log(1)"}), {"-e", "console.log(1)"},
              "-e and its code land in execArgv");

    // Value-taking table: node's own flags plus bun's value-taking AUTO_PARAMS.
    expect(mbun::cli::node_flag_takes_value("--require"), "--require takes a value");
    expect(mbun::cli::node_flag_takes_value("--test-reporter"), "--test-reporter takes a value");
    expect(!mbun::cli::node_flag_takes_value("--expose-gc"), "--expose-gc is boolean");
    expect(mbun::cli::exec_argv_flag_takes_value("--require"), "node table feeds the union");
    expect(mbun::cli::exec_argv_flag_takes_value("--cwd"), "bun's --cwd takes a value");
    expect(!mbun::cli::exec_argv_flag_takes_value("--no-warnings"), "--no-warnings is boolean");

    if (gFailed > 0) {
        std::println("test_exec_argv: {} failed", gFailed);
        return 1;
    }
    std::println("test_exec_argv: ok");
    return 0;
}
