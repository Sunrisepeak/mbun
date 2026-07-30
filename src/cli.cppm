// src/cli/cli.cppm — module mbun.cli
// CLI 子命令分发骨架。热路径参考 bun 的 fast-path 策略（--version/--help 不进
// 完整参数解析），单遍手写分发；复杂 flag 解析后续接 mcpplibs.cmdline。
export module mbun.cli;

import std;

export namespace mbun::cli {

// mbun's own version (date-based release id). First line of `mbun --version`;
// must match the runtime's process.versions.mbun (engine.inc).
inline constexpr std::string_view MBUN_VERSION { "2026.07.18.0" };

// Node.js compatibility claim: the version of the pinned native corpus
// (compat/node, src/node_version.h — the corpus itself asserts
// `node --version` → "v26.3.0" in js/node/process/process.test.js). Printed by
// `--version`, returned by node-emulation `node --version`, exposed as
// process.version/versions.node (engine.inc must match), and used to judge
// package.json engines.node when running/installing a project.
inline constexpr std::string_view NODE_COMPAT_VERSION { "26.3.0" };

// Bun compatibility claim — must match the runtime's Bun.version (engine.inc);
// `mbun --version` output must CONTAIN this string, which is what JS sees via
// Bun.version (regression/issue/10170 asserts toContain).
// This is the bun API contract mbun targets, not mbun's own version (which is
// process.versions.mbun); see the rationale block at engine.inc's Bun.version.
inline constexpr std::string_view VERSION { "1.3.14" };

// Must match the runtime's Bun.revision (engine.inc). bun exposes the full 40-hex
// build git sha (BunObject.cpp constructBunRevision → Bun__version_sha = GIT_SHA).
// mbun has no build-time git-sha injection yet, so this is a fixed placeholder;
// upgrade to a build-time-generated constant when that lands. It must stay a real
// 40-hex string and MUST NOT be empty: harness.ts normalizeBunSnapshot does
// `.replaceAll(Bun.revision, "<revision>")`, and replaceAll("") splices the marker
// between every character of the snapshot, corrupting it corpus-wide.
inline constexpr std::string_view REVISION { "7a3f9c2e1b8d4056e9f2c7a1b3d58e460c9f2a71" };

// Must match the runtime's Bun.version_with_sha (engine.inc). bun composes it as
// "v" + version + " (" + sha[0..9] + ")" (node_process.rs Bun__version_with_sha =
// concatcp!("v", package_json_version_with_sha); env.rs GIT_SHA_SHORT = sha[0..9]).
// `bun test`'s banner is "bun test " + this string, which is how harness.ts
// normalizes the banner to "bun test <version> (<revision>)".
inline constexpr std::string_view VERSION_WITH_SHA { "v1.3.14 (7a3f9c2e1)" };

enum class Action {
    Help,
    Version,
    Test,            // `mbun test <file>` — bun:test runner (T3.4)
    Install,         // `mbun install` — modules/install filesystem executor
    Add,             // `mbun add <pkg>...` — resolve + install + edit package.json
    Build,           // `mbun build <entry> [...flags]` — bundler CLI (R7)
    Exec,            // `mbun exec <script>` — shell script via mbun's shell interpreter
    Publish,         // `mbun publish [flags] [dist]` — help screen only (see run_publish)
    NotImplemented,  // 已规划子命令占位（当前为空），由后续任务逐步实现
    Unknown,
};

struct Parsed {
    Action      action { Action::Help };
    std::string argument {};  // Test: 测试文件路径；Exec: shell 脚本；NotImplemented/Unknown: 原始命令名
};

Parsed parse(std::span<const std::string_view> args) {
    if (args.empty()) return { Action::Help };

    auto first = args[0];
    if (first == "--version" || first == "-v") return { Action::Version };
    if (first == "--help" || first == "-h" || first == "help") return { Action::Help };

    // `mbun test [file|dir|filter]... [flags]` — operands/flags re-parsed by parse_test().
    if (first == "test") return { Action::Test, std::string { first } };

    // `i` is bun's short alias for install (runtime/cli/mod.rs:1008).
    if (first == "install" || first == "i") return { Action::Install };

    // `mbun add <pkg>...` — operands/flags are re-parsed by parse_add().
    if (first == "add") return { Action::Add, std::string { first } };

    // `mbun build <entry> [...flags]` — operands/flags are re-parsed by parse_build().
    if (first == "build") return { Action::Build, std::string { first } };

    // `mbun exec <script>` — bun takes exactly one positional and ignores the rest
    // (src/cli/exec_command.rs:39); with none it prints exec's help (src/cli/mod.rs:1566-1568).
    if (first == "exec") {
        return { Action::Exec, args.size() > 1 ? std::string { args[1] } : std::string {} };
    }

    // `publish` is a reserved subcommand in bun (PackageManager Subcommand::Publish),
    // so it must never fall through to package.json script resolution — that is
    // what made `mbun publish --help` report `Script not found "publish"`.
    // Only the help screen is implemented; see mbun::app::run_publish.
    if (first == "publish") return { Action::Publish, std::string { first } };

    // `run` handled ahead of dispatch in main; `test`/`install`/`build`/`exec` handled above.
    return { Action::Unknown, std::string { first } };
}

Parsed parse(std::initializer_list<std::string_view> args) {
    return parse(std::span<const std::string_view> { args.begin(), args.size() });
}

// ─── node runtime flags / process.execArgv ─────────────────────────────────
// Node-emulation (`mbun <flags> script.js`, argv[0] == node) must accept node's
// own flag vocabulary without rejecting it, and must report exactly those flags
// as process.execArgv. Both are pure command-line analysis, so they live here
// rather than in mbun.app and are unit-tested without booting the runtime.

// A node runtime flag whose value is a SEPARATE token (`--flag value`), not
// `--flag=value`. Node's V8/bootstrap option table decides this per flag; the
// corpus re-spawns `process.execPath` with the space form for these, so unless
// we consume the value token too it is mistaken for the script to run (the
// `test-*.js` re-exec cluster: `--snapshot-blob X`, `-r X`, `--test-reporter X`).
// Everything not listed here is treated as a boolean flag (single token).
bool node_flag_takes_value(std::string_view flag) {
    static constexpr std::string_view kValued[]{
        "-r", "--require", "--snapshot-blob", "--build-snapshot-config",
        "--test-reporter", "--test-reporter-destination", "--test-name-pattern",
        "--test-skip-pattern", "--test-shard", "--test-concurrency",
        // node's remaining valued --test* flags. The corpus passes several of
        // them in the space form (`--test-timeout 10`), and without this the
        // value token is mistaken for a test file / the script to run.
        "--test-timeout", "--test-isolation", "--experimental-test-isolation",
        "--test-global-setup", "--experimental-test-global-setup",
        "--test-tag-filter", "--experimental-test-tag-filter",
        "--test-rerun-failures", "--test-random-seed", "--test-coverage-include",
        "--test-coverage-exclude",
        "--heap-prof-interval", "--heap-prof-dir", "--heap-prof-name",
        "--cpu-prof-interval", "--cpu-prof-dir", "--cpu-prof-name",
        "--trace-event-categories", "--trace-event-file-pattern",
        "--localstorage-file", "--env-file", "--env-file-if-exists",
        "--max-old-space-size", "--max-semi-space-size", "--stack-size",
        "--stack-trace-limit", "--v8-pool-size", "--title", "--icu-data-dir",
        "--openssl-config", "--tls-cipher-list", "--tls-keylog",
        "--heapsnapshot-signal", "--heapsnapshot-near-heap-limit",
        "--diagnostic-dir", "--redirect-warnings", "--disk-cache-dir",
        "--experimental-policy", "--policy-integrity", "--conditions",
        "-C", "--report-dir", "--report-directory", "--report-filename",
        "--report-signal", "--secure-heap", "--secure-heap-min", "--dns-result-order",
        // Permission Model path lists. node's own corpus passes these in the
        // space form as often as the `=` form (test-permission-fs-wildcard does
        // `--allow-fs-read /tmp/*`), and treating the path as a positional made
        // mbun try to RUN it.
        "--allow-fs-read", "--allow-fs-write"};
    for (std::string_view f : kValued) {
        if (flag == f) return true;
    }
    return false;
}

// ── process.execArgv ────────────────────────────────────────────────────────
// The runtime flags that precede the entry point. They are NEVER part of
// process.argv, and until now mbun reported an always-empty execArgv for every
// non-compiled invocation, which is what made 19% of the node corpus unrunnable:
// test/common/index.js re-execs the test through `process.execPath` whenever a
// flag from the file's `// Flags:` header is missing from process.execArgv, so an
// empty execArgv meant the child re-spawned itself, forever, until the group was
// aborted (`process.kill(0, result.signal)`).
//
// Derivation is a straight port of bun's own re-parser
// (src/runtime/node/node_process.rs `create_exec_argv`): walk the raw command
// line, take every leading `-…` token as a runtime flag, skip one `run`
// subcommand, keep the value token of a value-taking flag, and stop at the first
// remaining positional (the script). bun deliberately re-parses argv here rather
// than threading state out of the CLI, and so do we — the CLI's own flag loops
// consume flags in several places, and execArgv must not depend on which one won.

// A flag whose value is a SEPARATE token, for the execArgv re-parser: the union
// of node's table (node_flag_takes_value) and bun's value-taking AUTO_PARAMS
// (cli/Arguments.rs BASE_/TRANSPILER_/RUNTIME_PARAMS_), which is exactly the set
// bun's create_exec_argv consults.
bool exec_argv_flag_takes_value(std::string_view flag) {
    if (node_flag_takes_value(flag)) return true;
    static constexpr std::string_view kValued[]{
        "-e",     "--eval",   "-p",       "--print",  "--preload", "--import",
        "--cwd",  "-c",       "--config", "--shell",  "--install", "--port",
        "-u",     "--origin", "-F",       "--filter", "--user-agent",
        "--unhandled-rejections", "--console-depth",  "--elide-lines",
        "--fetch-preconnect",     "--cron-period",    "--cron-title",
        "--inspect", "--inspect-brk", "--inspect-wait",
        "--main-fields", "--extension-order", "--tsconfig-override",
        "-d", "--define", "--drop", "--feature", "-l", "--loader",
        "--jsx-factory", "--jsx-fragment", "--jsx-import-source", "--jsx-runtime",
        "--breakpoint-resolve", "--breakpoint-print"};
    for (std::string_view f : kValued) {
        if (flag == f) return true;
    }
    return false;
}

std::vector<std::string> derive_exec_argv(std::span<const std::string_view> args) {
    std::vector<std::string> execArgv{};
    bool seenRun{false};
    std::string_view prev{};
    for (const std::string_view a : args) {
        // `--` is node's end-of-options marker: the option parser CONSUMES it,
        // so it never reaches process.execArgv (test-process-exec-argv spawns
        // `mbun --pending-deprecation -- file` and asserts the child reports
        // exactly ["--pending-deprecation"]). Everything after it is the entry
        // point and its arguments, so stop here — unless the previous token is
        // a value-taking flag, which owns `--` as its value.
        if (a == "--" && (prev.empty() || !exec_argv_flag_takes_value(prev))) break;
        if (!a.empty() && a[0] == '-') {
            execArgv.emplace_back(a);
            prev = a;
            continue;
        }
        if (!seenRun && a == "run") {
            seenRun = true;
            prev = a;
            continue;
        }
        // A value-taking flag owns the next token, so it is not the script.
        if (!prev.empty() && exec_argv_flag_takes_value(prev)) {
            execArgv.emplace_back(a);
            prev = a;
            continue;
        }
        break;  // the entry point — everything after it belongs to the script
    }
    return execArgv;
}

// ── the Permission Model's command line ─────────────────────────────────────
// node reads --permission/--allow-* from NODE_OPTIONS first and the command line
// second, and gives the entry point plus every --require an implicit fs.read
// grant (env.cc:952-967). All three facts are command-line analysis, so they are
// derived here, from the SAME raw argv the execArgv re-parser walks — a second,
// divergent walk is how a flag ends up honoured in one place and not the other.
struct PermissionCommandLine {
    std::vector<std::string> tokens{};    // NODE_OPTIONS words, then argv's flags
    bool hasEvalString{false};            // -e / --eval / -p / --print
    std::string entry{};                  // node's argv_[1]
    std::vector<std::string> preloads{};  // -r / --require / --preload / --import
};

PermissionCommandLine derive_permission_cli(std::span<const std::string_view> args) {
    PermissionCommandLine out{};

    // NODE_OPTIONS is whitespace-separated. node allows the permission flags in
    // it (kAllowedInEnvvar), and node's own child_process propagates the sandbox
    // to a subprocess exactly this way — so ignoring it would let any spawned
    // child escape.
    if (const char* nodeOptions{std::getenv("NODE_OPTIONS")}; nodeOptions != nullptr) {
        std::string token{};
        for (const char c : std::string_view{nodeOptions}) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!token.empty()) out.tokens.push_back(std::exchange(token, {}));
            } else {
                token.push_back(c);
            }
        }
        if (!token.empty()) out.tokens.push_back(std::move(token));
    }

    // The command line's own leading flags, stopping at the entry point — the
    // same walk as derive_exec_argv.
    bool seenRun{false};
    std::string_view prev{};
    for (const std::string_view a : args) {
        if (!a.empty() && a[0] == '-') {
            out.tokens.emplace_back(a);
            if (a == "-e" || a == "--eval" || a == "-p" || a == "--print") {
                out.hasEvalString = true;
            }
            prev = a;
            continue;
        }
        if (!seenRun && a == "run") {
            seenRun = true;
            prev = a;
            continue;
        }
        if (!prev.empty() && exec_argv_flag_takes_value(prev)) {
            out.tokens.emplace_back(a);
            if (prev == "-r" || prev == "--require" || prev == "--preload" || prev == "--import") {
                out.preloads.emplace_back(a);
            }
            prev = a;
            continue;
        }
        out.entry = std::string{a};
        break;
    }

    // `--flag=value` forms of the preload flags (the loop above only sees the
    // separate-token form).
    for (const std::string_view a : args) {
        for (const std::string_view f : {"-r=", "--require=", "--preload=", "--import="}) {
            if (a.starts_with(f)) out.preloads.emplace_back(a.substr(f.size()));
        }
    }

    // An eval string means there is no entry point to grant.
    if (out.hasEvalString) out.entry.clear();
    return out;
}

// Bun groups runtime preload options by kind: --preload, --require/-r,
// --import, then BUN_INSPECT_PRELOAD. The engine's module cache makes a
// repeated specifier execute once while retaining this stable ordering.
std::vector<std::string> derive_runtime_preloads(std::span<const std::string_view> args) {
    std::array<std::vector<std::string>, 3> groups{};
    const auto add{[&](std::size_t group, std::string_view path) {
        if (!path.empty())
            groups[group].emplace_back(path);
    }};
    for (std::size_t i{}; i < args.size(); ++i) {
        const std::string_view arg{args[i]};
        const auto take{[&](std::size_t group, std::string_view flag) {
            if (arg == flag && i + 1 < args.size()) {
                add(group, args[++i]);
                return true;
            }
            if (arg.starts_with(flag) && arg.size() > flag.size() && arg[flag.size()] == '=') {
                add(group, arg.substr(flag.size() + 1));
                return true;
            }
            return false;
        }};
        if (take(0, "--preload"))
            continue;
        if (take(1, "--require") || take(1, "-r"))
            continue;
        (void)take(2, "--import");
    }
    if (const char* inspectPreload{std::getenv("BUN_INSPECT_PRELOAD")};
        inspectPreload != nullptr && *inspectPreload != '\0') {
        groups[2].emplace_back(inspectPreload);
    }
    std::vector<std::string> out{};
    for (auto& group : groups) {
        for (std::string& path : group) {
            if (std::ranges::find(out, path) == out.end())
                out.emplace_back(std::move(path));
        }
    }
    return out;
}

// ─── `mbun test` flags ──────────────────────────────────────────────────────
// Flag names/arity are transcribed from bun's TEST_ONLY_PARAMS table
// (ref: bun-ref/src/cli/Arguments.rs:560-615) and the semantics from the test
// arm at :1647-1649 (--only-failures), :1788 (--randomize) and :1831-1841
// (--seed, which *implies* --randomize). Flags this slice does not honour are
// still recognised and skipped — including their value — so `--grep foo` never
// mistakes `foo` for a test path. Unknown flags stay silently ignored (as
// before), since bun's own harness passes flags mbun has no table entry for.
struct TestFlags {
    // Positional operands: a file, a directory, or a filter (see discover_test_files).
    std::vector<std::string> filters {};

    bool randomize { false };               // --randomize            (Arguments.rs:1788)
    std::optional<std::uint32_t> seed {};   // --seed <INT>           (Arguments.rs:1831-1841)
    bool onlyFailures { false };            // --only-failures        (Arguments.rs:1647-1649)
    bool passWithNoTests { false };         // --pass-with-no-tests   (Arguments.rs:1786)

    // --rerun-each <INT>: run EVERY test file N times, in the same JS realm
    // (test_command.rs:3108-3160 `repeat_count`). "Same realm" is the contract
    // the flag is used for — a file's globals must survive the reruns so a flaky
    // test can accumulate state across them (cli/test/rerun-each.test.ts counts
    // `globalThis.testRunCounter` up to 3); only the module entry is
    // re-evaluated. The file is counted ONCE in the summary regardless of the
    // rerun count (test_command.rs:3162-3164 `if repeat_index == 0 {
    // summary().files += 1 }`), so the report stays "Ran 3 tests across 1 file".
    std::optional<std::uint32_t> rerunEach {};

    // -t / --test-name-pattern / --grep <STR>: a JS RegExp source matched
    // (partial, unanchored) against each test's full "describe > … > test" name.
    // Non-matching tests count as "skipped because label"; a run that filters out
    // every test fails unless --pass-with-no-tests (ref: test_command.rs:2930 +
    // jest.rs:282 did_label_filter_out_all_tests).
    std::optional<std::string> testNamePattern {};

    // `--path-ignore-patterns` is repeatable.  The optional distinguishes no
    // CLI override from an explicit command-line pattern list, which replaces
    // (rather than appends to) bunfig's [test].pathIgnorePatterns.
    std::optional<std::vector<std::string>> pathIgnorePatterns {};

    // `--reporter <STR>` / `--reporter-outfile <STR>`: bun's only reporter here is
    // "junit", and it is written to the outfile IN ADDITION to the normal console
    // report — never instead of it (ref: bun test_command.rs, which installs the
    // JUnit reporter alongside the CLI one). Without an outfile the flag is inert.
    std::optional<std::string> reporter {};
    std::optional<std::string> reporterOutfile {};

    // ─── JSX ────────────────────────────────────────────────────────────────
    // Not TEST_ONLY_PARAMS: these live in bun's TRANSPILER_PARAMS_, which `test`
    // shares with `run`/`build` (ref Arguments.rs:174-176 for the table entries,
    // :1385-1386 `args.option(b"--jsx-import-source")` / `--jsx-runtime` for the
    // read). Transcribed here because mbun parses each subcommand's flags
    // separately.
    //
    // These are not decoration — `--jsx-import-source` is how a project that is
    // not React reaches its own runtime, and it is the ONLY way hono's own
    // `test:bun` script works:
    //   "test:bun": "bun test --jsx-import-source ../../src/jsx runtime-tests/bun/*"
    // Without an entry here the flag fell through to "unrecognised", and its
    // value (`../../src/jsx`) was silently swallowed as a dangling operand.
    std::optional<std::string> jsxImportSource {};  // --jsx-import-source <STR>
    std::optional<std::string> jsxRuntime {};       // --jsx-runtime <STR>

    // Fatal parse problem (bad --seed / --jsx-runtime value); empty when clean.
    std::string parseError {};
};

namespace detail {

// TEST_ONLY_PARAMS entries that consume a following value (`<STR>`/`<NUMBER>`/
// `<INT>`, incl. the `...` multi-value forms). The `<STR>?`/`<NUMBER>?` optional-
// value flags (--bail/--changed/--parallel) are deliberately absent: bun only
// takes their value in the `--flag=value` form, so a following operand is a path.
inline constexpr std::array TEST_VALUE_FLAGS {
    std::string_view { "--seed" },            std::string_view { "--rerun-each" },
    std::string_view { "--retry" },           std::string_view { "--coverage-reporter" },
    std::string_view { "--coverage-dir" },    std::string_view { "--test-name-pattern" },
    std::string_view { "--grep" },            std::string_view { "-t" },
    std::string_view { "--reporter" },        std::string_view { "--reporter-outfile" },
    std::string_view { "--max-concurrency" }, std::string_view { "--path-ignore-patterns" },
    std::string_view { "--timeout" },         std::string_view { "--shard" },
    std::string_view { "--preload" },         std::string_view { "-r" },
    std::string_view { "--parallel-delay" },  std::string_view { "--concurrent-test-glob" },
    // TRANSPILER_PARAMS_ (Arguments.rs:174-176), shared with run/build.
    std::string_view { "--jsx-import-source" }, std::string_view { "--jsx-runtime" },
};

} // namespace detail

// Parse the argument list *after* the `test` subcommand word.
TestFlags parse_test(std::span<const std::string_view> args) {
    TestFlags out {};

    for (std::size_t i { 0 }; i < args.size(); ++i) {
        const std::string_view arg { args[i] };

        // `--` ends flag parsing: everything after it is an operand.
        if (arg == "--") {
            for (std::size_t j { i + 1 }; j < args.size(); ++j) out.filters.emplace_back(args[j]);
            break;
        }

        if (!arg.starts_with("-") || arg == "-") {
            out.filters.emplace_back(arg);
            continue;
        }

        // Split `--flag=value` once, so `--seed=1` and `--seed 1` agree.
        std::string_view name { arg };
        std::string_view inlineValue {};
        bool hasInlineValue { false };
        if (const std::size_t eq { arg.find('=') }; eq != std::string_view::npos) {
            name = arg.substr(0, eq);
            inlineValue = arg.substr(eq + 1);
            hasInlineValue = true;
        }

        if (name == "--randomize") {
            out.randomize = true;
            continue;
        }
        if (name == "--only-failures") {
            out.onlyFailures = true;
            continue;
        }
        if (name == "--pass-with-no-tests") {
            out.passWithNoTests = true;
            continue;
        }

        if (std::ranges::contains(detail::TEST_VALUE_FLAGS, name)) {
            std::string_view value {};
            if (hasInlineValue) {
                value = inlineValue;
            } else if (i + 1 < args.size()) {
                value = args[++i];
            } else {
                continue;  // dangling value flag: nothing to consume
            }

            if (name == "--seed") {
                // bun parses the seed as u32 and `--seed` implies `--randomize`
                // (Arguments.rs:1831-1841); a bad value is a hard error there.
                std::uint32_t parsed {};
                const char* begin { value.data() };
                const char* end { value.data() + value.size() };
                const auto [ptr, ec] { std::from_chars(begin, end, parsed) };
                if (ec != std::errc {} || ptr != end) {
                    out.parseError = std::format("Invalid seed value: {}", value);
                    return out;
                }
                out.randomize = true;
                out.seed = parsed;
            } else if (name == "--rerun-each") {
                // Arguments.rs parses it as a u32; bun clamps to >= 1 at the use
                // site (`repeat_count.max(1)`, test_command.rs:2144), so 0 and a
                // junk value both mean "run once" rather than "run nothing".
                std::uint32_t parsed {};
                const char* begin { value.data() };
                const char* end { value.data() + value.size() };
                const auto [ptr, ec] { std::from_chars(begin, end, parsed) };
                if (ec == std::errc {} && ptr == end) out.rerunEach = parsed;
            } else if (name == "-t" || name == "--test-name-pattern" || name == "--grep") {
                // Capture the label filter (last one wins, matching bun's option()).
                out.testNamePattern = std::string { value };
            } else if (name == "--reporter") {
                out.reporter = std::string { value };
            } else if (name == "--reporter-outfile") {
                out.reporterOutfile = std::string { value };
            } else if (name == "--path-ignore-patterns") {
                if (!out.pathIgnorePatterns) out.pathIgnorePatterns.emplace();
                out.pathIgnorePatterns->emplace_back(value);
            } else if (name == "--jsx-import-source") {
                out.jsxImportSource = std::string { value };
            } else if (name == "--jsx-runtime") {
                // ref Arguments.rs:176 — `"automatic" (default) or "classic"`.
                // The accepted spellings are RUNTIME_MAP's (options_types/
                // jsx.rs:44-53), which is a superset of the help text.
                if (value != "automatic" && value != "classic" && value != "react" &&
                    value != "react-jsx" && value != "react-jsxdev") {
                    out.parseError = std::format("Invalid JSX runtime: {}", value);
                    return out;
                }
                out.jsxRuntime = std::string { value };
            }
            continue;
        }

        // Unrecognised flag: ignored (never treated as an operand).
    }

    return out;
}

TestFlags parse_test(std::initializer_list<std::string_view> args) {
    return parse_test(std::span<const std::string_view> { args.begin(), args.size() });
}

// ─── `mbun build` flags ─────────────────────────────────────────────────────
// Flag names, value arity and defaults are transcribed from bun's build flag
// table (src/cli/Arguments.rs BUILD_ONLY_PARAMS :401-550); the semantics of each
// flag come from build_command.rs. Flags mbun's bundler slice cannot yet honour
// are still *recognised* here (so they parse like bun's) and reported through
// `unsupported`, which main.cpp turns into an explicit error rather than
// silently emitting output that ignores the flag.
struct BuildFlags {
    std::vector<std::string> entryPoints {};

    // ref: Arguments.rs :447-449 (--outdir/--outfile), :445 (--target),
    // :465 (--format), :469 (--root), :472 (--public-path), :459/:462 (banner/footer).
    std::string outdir {};
    std::string outfile {};
    std::string target { "browser" };  // ref: Arguments.rs :1987 default arm
    std::string format { "esm" };      // ref: Arguments.rs :465 "Defaults to \"esm\""
    std::string root {};
    std::string publicPath {};
    std::string banner {};
    std::string footer {};

    // `--sourcemap <STR>?` — bare flag means "linked" (ref: Arguments.rs :2406-2417).
    std::string sourcemap { "none" };

    std::vector<std::string> external {};    // -e/--external (ref: Arguments.rs :475)
    std::vector<std::string> conditions {};  // --conditions   (ref: Arguments.rs :515)

    // ── the shared transpiler flags (TRANSPILER_PARAMS_, Arguments.rs :139-182) ──
    // `--define K=V` / `-d K:V`. Both separators are accepted, first one wins, and
    // the value is the raw text bun substitutes (parsed as JSON when it can be).
    // ref: runtime/cli/colon_list_type.rs `ColonListType::load` :34-56.
    std::vector<std::pair<std::string, std::string>> defines {};
    // `--loader .ext:loader` / `-l`. Keys keep their leading dot, as bun stores them.
    std::vector<std::pair<std::string, std::string>> loaders {};
    // `--env <inline|disable|PREFIX*>` (ref: Arguments.rs :1946-1963). Empty = the
    // default, which is "disable" — no environment variable is inlined.
    std::string env {};
    // JSX pragma overrides (Arguments.rs :166-178).
    std::string jsxRuntime {};
    std::string jsxFactory {};
    std::string jsxFragment {};
    std::string jsxImportSource {};
    bool jsxSideEffects { false };
    // `--no-bundle` — transpile each entry point in place, do not link a graph.
    // ref: Arguments.rs :503 (BUILD_ONLY_PARAMS) and build_command.rs's
    // `transform_only` path, which prints one output per entry point.
    bool noBundle { false };

    // ── `--compile`: a single-file executable ────────────────────────────────
    // ref: Arguments.rs :506 (--compile), :513 (--compile-exec-argv) and
    // build_command.rs, which links the bundle into a copy of the bun binary.
    bool compile { false };
    // `--compile-exec-argv="--smol --title=x"`: one space-separated string that
    // becomes the compiled program's process.execArgv.
    std::vector<std::string> compileExecArgv {};
    // `--compile-autoload-*` / `--no-compile-autoload-*`: whether the compiled
    // program still auto-loads .env / bunfig.toml / tsconfig.json / package.json
    // from its working directory. All default on, matching bun.
    bool compileAutoloadDotenv { true };
    bool compileAutoloadBunfig { true };
    bool compileAutoloadTsconfig { true };
    bool compileAutoloadPackageJson { true };

    bool help { false };

    // Recognised-but-unimplemented flags, in command-line order (e.g. "--minify-syntax").
    std::vector<std::string> unsupported {};
    // Fatal parse problem (unknown flag / missing value); empty when the parse is clean.
    std::string parseError {};
};

namespace detail {

// bun's build flags that consume a following value (`<STR>` / `<STR>...` in the
// param table). `--sourcemap`/`--metafile`/`--metafile-md` are `<STR>?` and are
// handled separately: they only take a value in the `--flag=value` form.
inline constexpr std::array BUILD_VALUE_FLAGS {
    std::string_view { "--target" },       std::string_view { "--outdir" },
    std::string_view { "--outfile" },      std::string_view { "--format" },
    std::string_view { "--root" },         std::string_view { "--public-path" },
    std::string_view { "--banner" },       std::string_view { "--footer" },
    std::string_view { "--packages" },     std::string_view { "--entry-naming" },
    std::string_view { "--chunk-naming" }, std::string_view { "--asset-naming" },
    std::string_view { "--env" },          std::string_view { "--external" },
    std::string_view { "-e" },             std::string_view { "--conditions" },
    std::string_view { "--allow-unresolved" },
    std::string_view { "--compile-exec-argv" },
    std::string_view { "--compile-executable-path" },
    std::string_view { "--windows-icon" },      std::string_view { "--windows-title" },
    std::string_view { "--windows-publisher" }, std::string_view { "--windows-version" },
    std::string_view { "--windows-description" },
    std::string_view { "--windows-copyright" },
    // TRANSPILER_PARAMS_ (Arguments.rs :139-181) — every `bun build` accepts these
    // too, because BUILD_PARAMS concatenates them (Arguments.rs :545).
    std::string_view { "--main-fields" },      std::string_view { "--extension-order" },
    std::string_view { "--tsconfig-override" },
    std::string_view { "--drop" },             std::string_view { "--feature" },
    std::string_view { "--jsx-factory" },      std::string_view { "--jsx-fragment" },
    std::string_view { "--jsx-import-source" },std::string_view { "--jsx-runtime" },
    std::string_view { "--global-name" },
};

// The two `<STR>...` flags whose value is a `key<sep>value` pair; bun parses both
// through ColonListType, which accepts ':' or '=' (whichever comes first) and
// errors with a flag-specific message when neither is present.
// ref: runtime/cli/colon_list_type.rs:34-56.
inline constexpr std::array BUILD_PAIR_FLAGS {
    std::string_view { "--define" }, std::string_view { "-d" },
    std::string_view { "--loader" }, std::string_view { "-l" },
};

// Boolean build flags mbun recognises but whose behaviour the bundler slice does
// not implement yet. Passing any of these is an error (see main.cpp run_build).
inline constexpr std::array BUILD_UNSUPPORTED_BOOL_FLAGS {
    std::string_view { "--bytecode" },
    std::string_view { "--minify" },       std::string_view { "--minify-syntax" },
    std::string_view { "--minify-whitespace" },
    std::string_view { "--minify-identifiers" },
    std::string_view { "--keep-names" },   std::string_view { "--splitting" },
    std::string_view { "--watch" },        std::string_view { "--app" },
    std::string_view { "--server-components" },
    std::string_view { "--react-fast-refresh" },
    std::string_view { "--react-compiler" },
    std::string_view { "--css-chunking" },
    std::string_view { "--emit-dce-annotations" },
    std::string_view { "--reject-unresolved" },
    std::string_view { "--ignore-dce-annotations" },
    std::string_view { "--no-macros" },
    std::string_view { "--preserve-symlinks" },
    std::string_view { "--preserve-symlinks-main" },
};

// Boolean build flags that are accepted and safely ignored: each is a no-op for
// a bundle mbun already emits the same way with or without it.
inline constexpr std::array BUILD_IGNORED_BOOL_FLAGS {
    // `--bundle` is bun's legacy explicit-bundle switch; `bun build` always
    // bundles, so mbun (which also always bundles) accepts it as a no-op.
    // ref: Arguments.rs — `--bundle` is parsed but the bundle path is implied.
    std::string_view { "--bundle" },
    // `--production` enables minify + NODE_ENV=production in bun; mbun's bundler
    // can't minify yet, so it is accepted as a no-op (bun never errors on it).
    std::string_view { "--production" },
    std::string_view { "--no-clear-screen" },
    std::string_view { "--dump-environment-variables" },
    std::string_view { "--windows-hide-console" },
};

} // namespace detail

// Parse the argument list *after* the `build` subcommand word.
BuildFlags parse_build(std::span<const std::string_view> args) {
    BuildFlags out {};

    for (std::size_t i { 0 }; i < args.size(); ++i) {
        const std::string_view arg { args[i] };

        // `--` ends flag parsing: everything after it is an entry point.
        if (arg == "--") {
            for (std::size_t j { i + 1 }; j < args.size(); ++j) out.entryPoints.emplace_back(args[j]);
            break;
        }

        // A bare operand (or "-", meaning stdin in bun) is an entry point.
        if (!arg.starts_with("-") || arg == "-") {
            out.entryPoints.emplace_back(arg);
            continue;
        }

        if (arg == "--help" || arg == "-h") {
            out.help = true;
            continue;
        }

        // Split `--flag=value` once, so `--outdir=out` and `--outdir out` agree.
        std::string_view name { arg };
        std::string_view inlineValue {};
        bool hasInlineValue { false };
        if (const std::size_t eq { arg.find('=') }; eq != std::string_view::npos) {
            name = arg.substr(0, eq);
            inlineValue = arg.substr(eq + 1);
            hasInlineValue = true;
        }

        // `--sourcemap <STR>?` — optional value (ref: Arguments.rs :457, :2406).
        if (name == "--sourcemap") {
            out.sourcemap = hasInlineValue ? std::string { inlineValue } : std::string { "linked" };
            if (out.sourcemap != "none" && out.sourcemap != "linked" && out.sourcemap != "inline" &&
                out.sourcemap != "external") {
                // ref: Arguments.rs :2420 `error: Invalid sourcemap setting: "<v>"`.
                out.parseError = std::format("Invalid sourcemap setting: \"{}\"", out.sourcemap);
                return out;
            }
            continue;
        }
        // `--metafile <STR>?` / `--metafile-md <STR>?` — recognised, not implemented.
        if (name == "--metafile" || name == "--metafile-md") {
            out.unsupported.emplace_back(name);
            continue;
        }

        // `--jsx-side-effects` is the only boolean among the JSX pragma flags.
        if (name == "--jsx-side-effects") {
            out.jsxSideEffects = true;
            continue;
        }
        if (name == "--no-bundle") {
            out.noBundle = true;
            continue;
        }

        // `--compile` and the four `--[no-]compile-autoload-*` switches. bun keeps
        // the autoload switches independent of --compile (they are simply inert
        // without it), so they are parsed the same way here.
        // ref: Arguments.rs :506 / :519-527.
        if (name == "--compile") {
            out.compile = true;
            continue;
        }
        if (name.starts_with("--compile-autoload-") || name.starts_with("--no-compile-autoload-")) {
            const bool enable { !name.starts_with("--no-") };
            const std::string_view what { name.substr(enable ? std::string_view { "--compile-autoload-" }.size()
                                                            : std::string_view { "--no-compile-autoload-" }.size()) };
            if (what == "dotenv") out.compileAutoloadDotenv = enable;
            else if (what == "bunfig") out.compileAutoloadBunfig = enable;
            else if (what == "tsconfig") out.compileAutoloadTsconfig = enable;
            else if (what == "package-json") out.compileAutoloadPackageJson = enable;
            else {
                out.parseError = std::format("unrecognised flag \"{}\"", name);
                return out;
            }
            continue;
        }

        // `--define`/`-d` and `--loader`/`-l` take a `key<sep>value` operand. bun
        // accepts ':' or '=' — whichever appears first — and reports a
        // flag-specific error when neither does.
        // ref: runtime/cli/colon_list_type.rs:34-56.
        if (std::ranges::contains(detail::BUILD_PAIR_FLAGS, name)) {
            std::string_view pair {};
            if (hasInlineValue) {
                pair = inlineValue;
            } else if (i + 1 < args.size()) {
                pair = args[++i];
            } else {
                out.parseError = std::format("Missing value for \"{}\"", name);
                return out;
            }
            const bool isLoader { name == "--loader" || name == "-l" };
            const std::size_t colon { pair.find(':') };
            const std::size_t equals { pair.find('=') };
            const std::size_t mid { std::min(colon, equals) };
            if (mid == std::string_view::npos) {
                out.parseError =
                    isLoader
                        ? std::format("--loader \"{}\" is missing a \":\" separator. Expected "
                                      "--loader .ext:loader, for example --loader .md:text",
                                      pair)
                        : std::format("--define \"{}\" is missing a \":\" or \"=\" separator. "
                                      "Expected --define key=value, for example "
                                      "--define process.env.NODE_ENV='\"production\"'",
                                      pair);
                return out;
            }
            const std::string_view key { pair.substr(0, mid) };
            const std::string_view value { pair.substr(mid + 1) };
            if (isLoader) {
                // ref: colon_list_type.rs:57-64 — an extension must start with '.'.
                if (!key.empty() && !key.starts_with('.')) {
                    out.parseError = std::format(
                        "file extension must start with a '.' (while mapping loader \"{}\")", pair);
                    return out;
                }
                out.loaders.emplace_back(std::string { key }, std::string { value });
            } else {
                out.defines.emplace_back(std::string { key }, std::string { value });
            }
            continue;
        }

        if (std::ranges::contains(detail::BUILD_UNSUPPORTED_BOOL_FLAGS, name)) {
            out.unsupported.emplace_back(name);
            continue;
        }
        if (std::ranges::contains(detail::BUILD_IGNORED_BOOL_FLAGS, name)) continue;

        if (std::ranges::contains(detail::BUILD_VALUE_FLAGS, name)) {
            std::string_view value {};
            if (hasInlineValue) {
                value = inlineValue;
            } else if (i + 1 < args.size()) {
                value = args[++i];
            } else {
                out.parseError = std::format("Missing value for \"{}\"", name);
                return out;
            }

            if (name == "--target") {
                // ref: Arguments.rs :1987-1998 — browser|node|bun|macro, else invalid_target.
                if (value != "browser" && value != "node" && value != "bun" && value != "macro") {
                    out.parseError = std::format("Invalid target: \"{}\"", value);
                    return out;
                }
                out.target = value;
            } else if (name == "--outdir") {
                out.outdir = value;
            } else if (name == "--outfile") {
                out.outfile = value;
            } else if (name == "--format") {
                // ref: Arguments.rs :2316-2319 — the message quotes the value and
                // lists the three accepted formats.
                if (value != "esm" && value != "cjs" && value != "iife") {
                    out.parseError = std::format(
                        "Invalid value for --format: \"{}\". Must be 'esm', 'cjs', or 'iife'.",
                        value);
                    return out;
                }
                out.format = value;
            } else if (name == "--env") {
                // ref: Arguments.rs :1946-1963 — a '*' at index 0 means "everything",
                // a '*' later means "this prefix", else the literal words.
                if (const std::size_t star { value.find('*') }; star != std::string_view::npos) {
                    out.env = star == 0 ? std::string { "inline" }
                                        : std::string { value.substr(0, star) } + "*";
                } else if (value == "inline" || value == "1") {
                    out.env = "inline";
                } else if (value == "disable" || value == "0") {
                    out.env = "disable";
                } else {
                    out.parseError =
                        "Expected 'env' to be 'inline', 'disable', or a prefix with a '*' character";
                    return out;
                }
            } else if (name == "--jsx-runtime") {
                // ref: options_types/jsx.rs — only these two runtimes exist.
                if (value != "automatic" && value != "classic") {
                    out.parseError = std::format("Invalid jsx runtime: \"{}\"", value);
                    return out;
                }
                out.jsxRuntime = value;
            } else if (name == "--jsx-factory") {
                out.jsxFactory = value;
            } else if (name == "--jsx-fragment") {
                out.jsxFragment = value;
            } else if (name == "--jsx-import-source") {
                out.jsxImportSource = value;
            } else if (name == "--root") {
                out.root = value;
            } else if (name == "--public-path") {
                out.publicPath = value;
            } else if (name == "--banner") {
                out.banner = value;
            } else if (name == "--footer") {
                out.footer = value;
            } else if (name == "--external" || name == "-e") {
                out.external.emplace_back(value);
            } else if (name == "--conditions") {
                out.conditions.emplace_back(value);
            } else if (name == "--compile-exec-argv") {
                // One space-separated string, exactly as bun stores it before
                // splitting it into the compiled program's execArgv.
                // ref: Arguments.rs :513 and build_command.rs (compile_exec_argv).
                for (const auto part : std::views::split(value, ' ')) {
                    const std::string_view token { part.begin(), part.end() };
                    if (!token.empty()) out.compileExecArgv.emplace_back(token);
                }
            } else {
                // Recognised value flag whose behaviour is not implemented
                // (--packages / --*-naming / --env / --compile-* / --windows-*).
                out.unsupported.emplace_back(name);
            }
            continue;
        }

        out.parseError = std::format("Unknown flag \"{}\"", name);
        return out;
    }

    return out;
}

BuildFlags parse_build(std::initializer_list<std::string_view> args) {
    return parse_build(std::span<const std::string_view> { args.begin(), args.size() });
}

// ─── `mbun add` flags ───────────────────────────────────────────────────────
// Flag names/arity are transcribed from bun's ADD_PARAMS table
// (ref: bun-ref/src/install/PackageManager/CommandLineArguments.rs:199-216,
// which concatenates SHARED_PARAMS with the add-only flags), and the semantics
// from the `Subcommand::Add | Subcommand::Install` arm at :1307-1314
// (`--development`/`--dev`, `--optional`, `--peer`, `--exact`, `--analyze`,
// `--only-missing`). Which dependency list a flag selects is resolved in
// PackageJSONEditor.rs via the `dependency_list` argument.
struct AddFlags {
    std::vector<std::string> packages {};  // <POS>... — the update requests

    bool dev { false };       // -d / -D / --dev / --development
    bool optional { false };  // --optional
    bool peer { false };      // --peer
    bool exact { false };     // -E / --exact

    bool help { false };

    // Recognised-but-unimplemented flags, in command-line order.
    std::vector<std::string> unsupported {};
    // Fatal parse problem (unknown flag / missing value); empty when clean.
    std::string parseError {};
};

namespace detail {

// SHARED_PARAMS/ADD_PARAMS flags that consume a following value. mbun's
// installer honours none of them yet, so each lands in `unsupported`.
inline constexpr std::array ADD_VALUE_FLAGS {
    std::string_view { "--config" },     std::string_view { "-c" },
    std::string_view { "--cwd" },        std::string_view { "--registry" },
    std::string_view { "--token" },      std::string_view { "--concurrent-scripts" },
    std::string_view { "--network-concurrency" },
    std::string_view { "--cache-dir" },  std::string_view { "--backend" },
    std::string_view { "--linker" },     std::string_view { "--cpu" },
    std::string_view { "--os" },         std::string_view { "--omit" },
};

// Boolean add flags mbun recognises but does not implement.
inline constexpr std::array ADD_UNSUPPORTED_BOOL_FLAGS {
    std::string_view { "--analyze" },      std::string_view { "-a" },
    std::string_view { "--only-missing" }, std::string_view { "--trust" },
    std::string_view { "--global" },       std::string_view { "-g" },
    std::string_view { "--production" },   std::string_view { "-p" },
    std::string_view { "--frozen-lockfile" },
    std::string_view { "--save-text-lockfile" },
    std::string_view { "--lockfile-only" },
    std::string_view { "--dry-run" },      std::string_view { "--force" },
    std::string_view { "-f" },             std::string_view { "--filter" },
};

// Boolean add flags accepted and safely ignored: each is a no-op for an install
// mbun already performs the same way with or without it.
inline constexpr std::array ADD_IGNORED_BOOL_FLAGS {
    std::string_view { "--no-summary" },   std::string_view { "--silent" },
    std::string_view { "--verbose" },      std::string_view { "--no-progress" },
    std::string_view { "--no-cache" },     std::string_view { "--no-verify" },
    std::string_view { "--ignore-scripts" },
    std::string_view { "--no-save" },      std::string_view { "--yarn" },
    std::string_view { "--prefer-offline" }, std::string_view { "--prefer-latest" },
    std::string_view { "--ca" },           std::string_view { "--cafile" },
};

} // namespace detail

// Parse the argument list *after* the `add` subcommand word.
AddFlags parse_add(std::span<const std::string_view> args) {
    AddFlags out {};

    for (std::size_t i { 0 }; i < args.size(); ++i) {
        const std::string_view arg { args[i] };

        // `--` ends flag parsing: everything after it is a package request.
        if (arg == "--") {
            for (std::size_t j { i + 1 }; j < args.size(); ++j) out.packages.emplace_back(args[j]);
            break;
        }

        if (!arg.starts_with("-") || arg == "-") {
            out.packages.emplace_back(arg);
            continue;
        }

        if (arg == "--help" || arg == "-h") {
            out.help = true;
            continue;
        }

        std::string_view name { arg };
        std::string_view inlineValue {};
        bool hasInlineValue { false };
        if (const std::size_t eq { arg.find('=') }; eq != std::string_view::npos) {
            name = arg.substr(0, eq);
            inlineValue = arg.substr(eq + 1);
            hasInlineValue = true;
        }

        // ref: CommandLineArguments.rs:1308-1313.
        if (name == "--dev" || name == "--development" || name == "-d" || name == "-D") {
            out.dev = true;
            continue;
        }
        if (name == "--optional") {
            out.optional = true;
            continue;
        }
        if (name == "--peer") {
            out.peer = true;
            continue;
        }
        if (name == "--exact" || name == "-E") {
            out.exact = true;
            continue;
        }

        if (std::ranges::contains(detail::ADD_UNSUPPORTED_BOOL_FLAGS, name)) {
            out.unsupported.emplace_back(name);
            continue;
        }
        if (std::ranges::contains(detail::ADD_IGNORED_BOOL_FLAGS, name)) continue;

        if (std::ranges::contains(detail::ADD_VALUE_FLAGS, name)) {
            if (!hasInlineValue) {
                if (i + 1 >= args.size()) {
                    out.parseError = std::format("Missing value for \"{}\"", name);
                    return out;
                }
                ++i;
            }
            (void)inlineValue;
            out.unsupported.emplace_back(name);
            continue;
        }

        out.parseError = std::format("Unknown flag \"{}\"", name);
        return out;
    }

    return out;
}

AddFlags parse_add(std::initializer_list<std::string_view> args) {
    return parse_add(std::span<const std::string_view> { args.begin(), args.size() });
}

} // namespace mbun::cli
