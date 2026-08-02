// mbun executable entrypoint and command dispatch.

import std;

import mbun.app;
import mbun.exe_platform;
import mbun.cli;
import mbun.cli.run_command;
import mbun.install.command;
import mbun.install.dependency;
import mbun.install.npm.json;
import mbun.install.package_json_editor;
import mbun.jsc.module_loader;
import mbun.jsc.runtime;
import mbun.jsc.test_runner;
import mbun.js_parser;
import mbun.toml;
import mbun.bunfig.types;
import mbun.bunfig.parser;
import mbun.http;
import mbun.bundler;
import mbun.resolver;

using namespace mbun::app;

int main(int argc, char* argv[]) {
    mbun::platform::raise_file_descriptor_limit();
    // Worker/fork pass the parent's normalized --tsconfig-override through a
    // one-hop internal environment key while leaving process.execArgv raw. Copy
    // then erase it before any runtime/JS environment snapshot can expose it.
    if (const char* inherited{std::getenv("MBUN_INTERNAL_TSCONFIG_OVERRIDE")};
        inherited != nullptr) {
        std::string path{inherited};
        mbun::platform::unset_env_var("MBUN_INTERNAL_TSCONFIG_OVERRIDE");
        if (!path.empty()) set_inherited_tsconfig_override(std::move(path));
    }
    // process.argv0 — node snapshots the ORIGINAL argv[0] before anything can
    // rewrite it, and the corpus respawns the runtime through it. Recorded first
    // so every dispatch below (compiled program, node emulation, run, -e) agrees.
    if (argc > 0 && argv[0] != nullptr) mbun::jsc::runtime::set_argv0(argv[0]);
    // The process dialect (node vs bun). Resolved ONCE, here, before any
    // dispatch, and read afterwards by the C++ dispatch points and by JS
    // through globalThis.__mbunDialect. See mbun::app::resolve_dialect for the
    // priority order and modules/jsc/src/runtime.cppm for what it is for.
    // Published below — the compiled-executable branch publishes its own.
    const ResolvedDialect resolvedDialect{resolve_dialect(argc, argv)};
    // NODE_PRESERVE_SYMLINKS_MAIN — read before any flag parsing so both the
    // node-emulation path and `run` see it (run_command.rs:2581-2584).
    if (const char* v{std::getenv("NODE_PRESERVE_SYMLINKS_MAIN")};
        v != nullptr && *v != '\0' && std::string_view{v} != "0") {
        gPreserveSymlinksMain = true;
    }
    for (int i{1}; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--preserve-symlinks-main") gPreserveSymlinksMain = true;
        // --no-addons: surface to the runtime (process.dlopen throws the fixed
        // node/bun message; see node_process_extra.cppm).
        if (std::string_view{argv[i]} == "--no-addons") mbun::platform::set_no_addons_env();
    }

    // ── a `bun build --compile` executable → run the embedded program.
    //    Must come before EVERYTHING else, including the node-emulation check and
    //    the global run-flag strip: a compiled program owns its whole command
    //    line, so `./myapp --silent run` passes those through untouched.
    //    ref: cli/mod.rs, which consults StandaloneModuleGraph.fromExecutable()
    //    before any argument parsing.
    if (const auto embedded{embedded_program()}) {
        // A compiled executable owns its whole command line, so the SUBCOMMAND
        // signal cannot be read off it — `./myapp test` is the program's own
        // argument, not a bun subcommand. Only an EXPLICIT dialect survives
        // here; otherwise a `bun build --compile` artifact keeps the bun
        // default, which is what it was built as.
        publish_dialect(resolvedDialect.explicitly_set ? resolvedDialect : ResolvedDialect{});
        std::vector<std::string_view> embeddedArgs(argv + 1, argv + argc);
        return run_embedded_program(*embedded, argc > 0 ? argv[0] : "mbun", embeddedArgs);
    }
    publish_dialect(resolvedDialect);

    // ── --enable-fips / --force-fips on a non-FIPS OpenSSL → refuse to start.
    //    node ProcessFipsOptions() (src/crypto/crypto_util.cc) asks OpenSSL for a
    //    FIPS provider and, when there is none, node.cc:1246 reports
    //    "OpenSSL error when trying to enable FIPS:" and returns
    //    ExitCode::kGenericUserError BEFORE any JS runs. mbun links a stock
    //    OpenSSL 3.1.5 with no FIPS provider, so the request can never be
    //    honoured — accepting the flag silently would be the dangerous answer
    //    (a program that asked for FIPS would run outside it and never know).
    //    Parsed off the raw command line, stopping at the first non-option or an
    //    eval flag, so a `-e` program that merely mentions the string is not a
    //    request.
    {
        for (int i{1}; i < argc; ++i) {
            const std::string_view a{argv[i]};
            if (a == "-e" || a == "--eval" || a == "-p" || a == "--print" || a == "-pe" ||
                a == "-ep") {
                break;
            }
            if (!a.starts_with("-")) break;
            if (a == "--enable-fips" || a == "--force-fips") {
                std::println(std::cerr, "{}: OpenSSL error when trying to enable FIPS:\n",
                             argc > 0 ? argv[0] : "mbun");
                return 1;
            }
        }
    }

    // ── --unhandled-rejections=<mode> with an unknown mode → refuse to start.
    //    node validates the value in EnvironmentOptions::CheckOptions
    //    (src/node_options.cc) and bails out of bootstrap before any JS runs,
    //    which is exactly what test-promise-unhandled-flag spawns a child to
    //    observe. Accepting the bad value instead made that test re-exec itself
    //    forever (the child ran the test, which spawned another child…), so this
    //    is also the fix for a corpus hang, not just a message.
    //    Parsed off the raw command line with the same guards as the FIPS check:
    //    stop at the first non-option or eval flag so an `-e` program that merely
    //    mentions the string is not a request.
    {
        const auto valid_rejection_mode{[](std::string_view v) {
            return v == "throw" || v == "strict" || v == "warn" || v == "none" ||
                   v == "warn-with-error-code";
        }};
        for (int i{1}; i < argc; ++i) {
            const std::string_view a{argv[i]};
            if (a == "-e" || a == "--eval" || a == "-p" || a == "--print" || a == "-pe" ||
                a == "-ep") {
                break;
            }
            if (!a.starts_with("-")) break;
            std::optional<std::string_view> value{};
            if (a.starts_with("--unhandled-rejections=")) {
                value = a.substr(std::string_view{"--unhandled-rejections="}.size());
            } else if (a == "--unhandled-rejections" && i + 1 < argc) {
                value = std::string_view{argv[++i]};
            }
            if (value && !valid_rejection_mode(*value)) {
                std::println(std::cerr, "{}: invalid value for --unhandled-rejections",
                             argc > 0 ? argv[0] : "mbun");
                return 9;
            }
        }
    }

    // ── `--tls-min-v1.3` together with `--tls-max-v1.2` is an EMPTY protocol
    //    window: the floor is above the ceiling, so no version could ever be
    //    negotiated. node rejects it during option parsing rather than letting a
    //    process start that can never complete a handshake.
    //    PORT-SOURCE: compat/node/src/node_options.cc:206 PerProcessOptions::
    //    CheckOptions — `if (tls_min_v1_3 && tls_max_v1_2)` pushes
    //    "either --tls-min-v1.3 or --tls-max-v1.2 can be used, not both", which
    //    node prints and exits 9 on. The other combinations (min-v1.2 with
    //    max-v1.3, …) are legal windows and are NOT checked here, exactly as
    //    node does not check them.
    //    Same raw-command-line guards as the --unhandled-rejections block above:
    //    stop at the first non-option or eval flag so an `-e` program that merely
    //    mentions the strings is not read as a request.
    {
        bool minV13{};
        bool maxV12{};
        for (int i{1}; i < argc; ++i) {
            const std::string_view a{argv[i]};
            if (a == "-e" || a == "--eval" || a == "-p" || a == "--print" || a == "-pe" ||
                a == "-ep") {
                break;
            }
            if (!a.starts_with("-")) break;
            if (a == "--tls-min-v1.3") minV13 = true;
            else if (a == "--tls-max-v1.2") maxV12 = true;
        }
        if (minV13 && maxV12) {
            std::println(std::cerr,
                         "{}: either --tls-min-v1.3 or --tls-max-v1.2 can be used, not both",
                         argc > 0 ? argv[0] : "mbun");
            return 9;
        }
    }

    // process.execArgv — derived from the raw command line before any flag loop
    //    consumes it, exactly as bun does (node_process.rs create_exec_argv).
    //    Every dispatch below (node emulation, `run`, bare script) shares it; a
    //    compiled program overrides it above with its baked --compile-exec-argv.
    {
        std::vector<std::string_view> rawArgs(argv + 1, argv + argc);
        mbun::jsc::runtime::set_exec_argv(mbun::cli::derive_exec_argv(rawArgs));

        // node's Permission Model (--permission / --allow-*). Derived from the
        // same raw command line, before any dispatch, so every path below (node
        // emulation, `run`, a bare script, -e) is gated identically. The model
        // stays DISABLED unless a --permission flag is actually present, so this
        // is inert for an ordinary invocation.
        const mbun::cli::PermissionCommandLine perm{mbun::cli::derive_permission_cli(rawArgs)};
        mbun::jsc::runtime::set_permission_command_line(perm.tokens, perm.hasEvalString,
                                                        perm.entry, perm.preloads);
        set_cli_preloads(mbun::cli::derive_runtime_preloads(rawArgs));
    }

    // ── argv0 == `node` → node emulation (cli/mod.rs:952 → run_command.rs:2981).
    //    Must come before ANY bun-flag parsing: node's flags are not bun's.
    if (argc > 0 && is_node_argv0(argv[0])) {
        std::vector<std::string_view> nodeArgs(argv + 1, argv + argc);
        // `node -i` is a REPL here too — the wrapper's "does not support a repl"
        // message only covers the no-target case.
        if (take_interactive_flag(nodeArgs)) return exec_interactive(nodeArgs);
        return exec_as_if_node(nodeArgs);
    }

    std::vector<std::string_view> args(argv + 1, argv + argc);

    // node's `--test` CLI: the positionals are test files for node:test's
    // runner, not an entry point to execute. Checked before every bun flag loop
    // because `--test` is not a bun flag and the node-emulation fallback below
    // would boot the first positional as an ordinary script.
    if (has_node_test_flag(args)) return exec_node_test_cli(args);

    // `-i` / `--interactive` forces the REPL, before any other flag handling:
    // it is not a run flag (there is no run target) and it must survive
    // alongside `-e`/`--eval`, which the strip loop below stops at.
    if (take_interactive_flag(args)) return exec_interactive(args);

    // node's eval flags. `-pe` / `-ep` are the combined short forms node's own
    // argument parser accepts (`node -pe "expr"` is `-p -e "expr"`), and the
    // corpus spawns children that way — test-tls-cipher-list builds its argv as
    // `[...flags, '-pe', expression]`. Without them the token is not recognised
    // as an eval flag at all and the expression is taken for a script path.
    const auto is_eval_flag{[](std::string_view a) {
        return a == "-e" || a == "--eval" || a == "-p" || a == "--print" || a == "-pe" ||
               a == "-ep";
    }};
    const auto eval_flag_prints{[](std::string_view a) {
        return a == "-p" || a == "--print" || a == "-pe" || a == "-ep";
    }};

    // Node's -c/--check parses stdin without executing it. Keep bun's `-c
    // <config>` spelling intact by only treating short -c as --check when it
    // has no value (or the next token is another flag).
    bool checkSyntax{};
    for (std::size_t i{}; i < args.size(); ++i) {
        const std::string_view a{args[i]};
        if (a == "--check" || (a == "-c" &&
            (i + 1 == args.size() || args[i + 1].starts_with("-")))) {
            checkSyntax = true;
        }
        if (!a.starts_with("-")) break;
        if (a.find('=') == std::string_view::npos && mbun::cli::node_flag_takes_value(a) &&
            i + 1 < args.size()) {
            ++i;
        }
    }
    if (checkSyntax) {
        for (const std::string_view a : args) {
            if (is_eval_flag(a)) {
                std::println(std::cerr, "{}: either --check or --eval can be used, not both",
                             argc > 0 ? argv[0] : "mbun");
                return 9;
            }
        }
        bool moduleInput{};
        for (std::size_t i{}; i < args.size(); ++i) {
            if (args[i] == "--input-type=module") moduleInput = true;
            else if (args[i] == "--input-type" && i + 1 < args.size() &&
                     args[i + 1] == "module") moduleInput = true;
        }
        const std::string source{std::istreambuf_iterator<char>{std::cin}, {}};
        return mbun::jsc::runtime::check_syntax(source, "[stdin]", moduleInput);
    }

    // Strip leading global run flags so `mbun [flags] <script>` runs the script,
    // but never past -e/-p/--eval/--print (those consume the next token as code).
    RunFlags globalFlags{};
    mbun::cli::TsconfigOverrideArg globalTsconfig{};
    while (!args.empty() && !is_eval_flag(args[0])) {
        if (const std::size_t n{take_max_http_header_size_flag(args, 0)}; n > 0) {
            args.erase(args.begin(), args.begin() + static_cast<std::ptrdiff_t>(n));
            continue;
        }
        // `--if-present`: never an error when the entrypoint is missing
        // (run_command.rs:2726 `ctx.runtime_options.if_present` → Ok(true)).
        if (args[0] == "--if-present") {
            globalFlags.ifPresent = true;
            args.erase(args.begin());
            continue;
        }
        // `--dialect=<name>` / `--dialect <name>` — already consumed by
        // resolve_dialect() above; drop it here so it is not mistaken for a run
        // target or forwarded to the script.
        if (args[0].starts_with("--dialect=")) {
            args.erase(args.begin());
            continue;
        }
        if (args[0] == "--dialect" && args.size() > 1) {
            args.erase(args.begin(), args.begin() + 2);
            continue;
        }
        if (args[0] == "--silent") {
            globalFlags.silent = true;
            args.erase(args.begin());
            continue;
        }
        if (args[0] == "--no-env-file") {
            mbun::jsc::runtime::set_disable_env_files(true);
            args.erase(args.begin());
            continue;
        }
        const bool preloadFlag{args[0] == "--preload" || args[0] == "--require" ||
                               args[0] == "-r" || args[0] == "--import" ||
                               args[0].starts_with("--preload=") ||
                               args[0].starts_with("--require=") || args[0].starts_with("-r=") ||
                               args[0].starts_with("--import=")};
        if (preloadFlag) {
            const bool separateValue{args[0] == "--preload" || args[0] == "--require" ||
                                     args[0] == "-r" || args[0] == "--import"};
            const std::size_t count{separateValue && args.size() > 1 ? std::size_t{2} : std::size_t{1}};
            args.erase(args.begin(), args.begin() + static_cast<std::ptrdiff_t>(count));
            continue;
        }
        if (const std::size_t n{take_valued_flag(args, 0, "--env-file",
                                                 mbun::jsc::runtime::add_env_file)};
            n > 0) {
            args.erase(args.begin(), args.begin() + static_cast<std::ptrdiff_t>(n));
            continue;
        }
        if (const std::size_t n{take_valued_flag(args, 0, "--user-agent",
                                                 mbun::jsc::runtime::set_user_agent)};
            n > 0) {
            args.erase(args.begin(), args.begin() + static_cast<std::ptrdiff_t>(n));
            continue;
        }
        if (const std::size_t n{
                mbun::cli::take_tsconfig_override(args, 0, globalTsconfig)};
            n > 0) {
            if (!globalTsconfig.parseError.empty()) {
                std::println(std::cerr, "error: {}", globalTsconfig.parseError);
                return 1;
            }
            args.erase(args.begin(), args.begin() + static_cast<std::ptrdiff_t>(n));
            continue;
        }
        // `--loader .ext:name` / `-l .ext:name` — shared with run/test, not
        // build-only (see apply_loader_flag).
        if (const std::size_t n{take_valued_flag(args, 0, "--loader", apply_loader_flag)};
            n > 0) {
            args.erase(args.begin(), args.begin() + static_cast<std::ptrdiff_t>(n));
            continue;
        }
        if (const std::size_t n{take_valued_flag(args, 0, "-l", apply_loader_flag)}; n > 0) {
            args.erase(args.begin(), args.begin() + static_cast<std::ptrdiff_t>(n));
            continue;
        }
        if (args[0] == "--bun" || args[0] == "-b") {
            globalFlags.forceUsingBun = true;
            args.erase(args.begin());
            continue;
        }
        // Global flags precede the subcommand (`bun --shell=system run x`) —
        // bun's clap consumes them wherever they appear, and test/harness.ts
        // bunRunAsScript() spawns exactly [bun, ...execArgv, "run", script].
        if (args[0].starts_with("--shell=")) {
            globalFlags.useSystemShell = args[0].substr(8) != "bun";
            args.erase(args.begin());
            continue;
        }
        if (args[0] == "--shell" && args.size() > 1) {
            globalFlags.useSystemShell = args[1] != "bun";
            args.erase(args.begin(), args.begin() + 2);
            continue;
        }
        if (args[0] == "--cwd" && args.size() > 1) {
            apply_cwd_flag(args[1]);
            args.erase(args.begin(), args.begin() + 2);
            continue;
        }
        if (args[0].starts_with("--cwd=")) {
            apply_cwd_flag(args[0].substr(6));
            args.erase(args.begin());
            continue;
        }
        if (!is_skippable_run_flag(args[0])) break;
        args.erase(args.begin());
    }

    // Bun applies --cwd before joining the raw tsconfig value, regardless of
    // their command-line order. Keep the option deferred until a runtime/build
    // path is selected; --help/--version do not load resolver configuration.
    const auto applyGlobalTsconfig{[&] {
        return !globalTsconfig.value || apply_tsconfig_override(*globalTsconfig.value);
    }};

    // node's stdin main script (lib/internal/main/eval_stdin.js): with no file to
    // run, node reads the WHOLE of stdin and evaluates it as the main module —
    // either because an explicit `-` operand asked for it, or because a bare
    // invocation found stdin was not a terminal (a terminal is the REPL instead).
    // `-` is not an option, so it keeps its argv slot: `mbun - --opt` must put
    // --opt at process.argv[2], which is what test-stdin-script-child-option
    // asserts.
    if ((!args.empty() && args[0] == "-") ||
        (args.empty() && !mbun::platform::stdin_is_terminal())) {
        if (!applyGlobalTsconfig()) return 1;
        std::vector<std::string> jsArgv{"mbun"};
        for (const std::string_view a : args) jsArgv.emplace_back(a);
        mbun::jsc::runtime::set_argv(std::move(jsArgv));
        const std::string source{std::istreambuf_iterator<char>{std::cin}, {}};
        return mbun::jsc::runtime::run_eval(source);
    }

    // Node-style re-exec: a leading `--flag` that is neither a bun run-flag nor
    // an eval flag, followed later by a positional entry point, is how the Node
    // corpus re-spawns `process.execPath` (`mbun --expose-gc file.js`,
    // `mbun --snapshot-blob b --build-snapshot file.js`, `mbun -r m file.js`).
    // bun's own CLI would reject the unknown flag; node-emulation (which already
    // ignores unmodelled node flags and resolves the first positional as the
    // script) is the correct handler, so route there instead of taking the flag
    // itself as the run target ("Script not found \"--expose-gc\"").
    if (!args.empty() && args[0].starts_with("-") && args[0] != "-" && !is_eval_flag(args[0])) {
        bool hasPositional{false};
        for (std::size_t k{0}; k < args.size(); ++k) {
            if (!args[k].starts_with("-")) {
                // Skip the value token of a known valued node flag so its value
                // is not mistaken for the positional entry point.
                if (k > 0 && args[k - 1].starts_with("-") &&
                    args[k - 1].find('=') == std::string_view::npos &&
                    mbun::cli::node_flag_takes_value(args[k - 1])) {
                    continue;
                }
                hasPositional = true;
                break;
            }
        }
        if (hasPositional) {
            if (!applyGlobalTsconfig()) return 1;
            return exec_as_if_node(args);
        }
    }

    // `mbun run <script> [args...]` and bare `mbun <script.(m)js> [args...]`
    // execute a JS file through the JSC runtime with the Bun.* API in scope.
    if (!args.empty()) {
        // `bun repl` is a command, not a package.json script named "repl".
        // Keep it before auto-command resolution so both piped REPL input and
        // the command's own -e/-p forms reach the dedicated entry point.
        if (args[0] == "repl") {
            if (!applyGlobalTsconfig()) return 1;
            return exec_bun_repl(std::span{args}.subspan(1));
        }
        // `mbun -e <code>` / `mbun --eval <code>`: evaluate a JS/TS string.
        if (is_eval_flag(args[0])) {
            if (!applyGlobalTsconfig()) return 1;
            // node's `--print` is a BOOLEAN option (node_options.cc), separate
            // from `--eval`'s string, so the two compose: `-p -e 42` is one
            // eval whose result is printed. Taking only args[0] made args[1]
            // ("-e") the source, and `mbun -p -e 42` died with
            // "ReferenceError: e is not defined" (test-cli-eval runs exactly
            // that, alongside '-pe' and '--print').
            std::size_t code_at{0};
            bool prints{false};
            while (code_at < args.size() && is_eval_flag(args[code_at])) {
                prints = prints || eval_flag_prints(args[code_at]);
                ++code_at;
            }
            if (args.size() <= code_at) {
                std::println(std::cerr, "{}: {} requires an argument",
                             argc > 0 ? argv[0] : "mbun", args[code_at - 1]);
                return 9;
            }
            // argv omits the script slot in eval mode: bun builds argv as
            // [exe] ++ (main unless it ends in "/[eval]" or "/[stdin]") ++ args
            // (node_process.rs), so `mbun -e <code> foo` puts foo at argv[1],
            // matching node. Emitting "[eval]" there shifted every user arg by one.
            std::vector<std::string> jsArgv{"mbun"};
            {
                // A single leading `--` after the eval string is the option
                // terminator, not a user argument: node's test-cli-eval.js
                // runs `--eval <code> -- <args>` and asserts argv.slice(1) is
                // exactly <args>. Only the FIRST one is consumed — with
                // `-- --` the second `--` is a real argument.
                // ref: regression 17294.
                auto rest{std::span{args}.subspan(code_at + 1)};
                if (!rest.empty() && rest[0] == "--") rest = rest.subspan(1);
                for (std::string_view a : rest) jsArgv.emplace_back(a);
            }
            mbun::jsc::runtime::set_argv(std::move(jsArgv));
            std::string code{args[code_at]};
            // `-p`/`--print` prints the result.
            //
            // node lib/internal/process/execution.js evalScript compiles
            // `return eval(<source>)` inside the CJS module wrapper and prints
            // what that returns — a DIRECT eval, so the printed value is the
            // SCRIPT COMPLETION VALUE and the source may be statements
            // (`const`, `if`, a loop), not just an expression.
            //
            // Wrapping the source in an arrow-function expression body instead
            // made every statement a syntax error: `mbun -pe "const a = 1; a"`
            // died with "error: Unexpected const". The corpus reaches this
            // through common.spawnPromisified(process.execPath, ['-pe', ...])
            // and asserts the child's stderr is empty
            // (test-timers-{timeout,immediate,interval}-promisified).
            if (prints) {
                code = "console.log(eval(" + js_quote(args[code_at]) + "))";
            }
            // node/bun expose the ORIGINAL eval source as process._eval
            // (run-eval.test.ts). Set it on the same first line so source-map
            // line numbers are unchanged; args[1] is the pre-wrap source.
            code = "process._eval=" + js_quote(args[code_at]) + ";" + code;
            // run_eval() prepends node's addBuiltinLibsToObject shim, so both
            // this path and the `node`-argv0 emulation get the builtin globals.
            return mbun::jsc::runtime::run_eval(code);
        }
        // `mbun pm version [args...]` — package.json version bumping (npm-compatible).
        if (args[0] == "pm" && args.size() >= 2 && args[1] == "version") {
            return run_pm_version(std::span{args}.subspan(2));
        }
        if (args[0] == "run") {
            // Skip run-flags placed after `run` (e.g. `mbun run --bun file.js`);
            // they are stripped before the command but not after the subcommand.
            RunFlags flags{globalFlags};
            mbun::cli::TsconfigOverrideArg runTsconfig{globalTsconfig};
            std::size_t i = 1;
            while (i < args.size()) {
                if (const std::size_t n{take_max_http_header_size_flag(args, i)}; n > 0) {
                    args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                               args.begin() + static_cast<std::ptrdiff_t>(i + n));
                    continue;
                }
                if (args[i] == "--help" || args[i] == "-h") {
                    mbun::cli::run::print_help(
                        mbun::cli::run::load_nearest_package_scripts(std::filesystem::current_path()));
                    return 0;
                }
                if (args[i] == "--silent") { flags.silent = true; ++i; continue; }
                if (args[i] == "--no-env-file") { mbun::jsc::runtime::set_disable_env_files(true); ++i; continue; }
                if (const std::size_t n{take_valued_flag(args, i, "--env-file",
                                                         mbun::jsc::runtime::add_env_file)};
                    n > 0) {
                    args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                               args.begin() + static_cast<std::ptrdiff_t>(i + n));
                    continue;
                }
                if (const std::size_t n{take_valued_flag(args, i, "--user-agent",
                                                         mbun::jsc::runtime::set_user_agent)};
                    n > 0) {
                    args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                               args.begin() + static_cast<std::ptrdiff_t>(i + n));
                    continue;
                }
                if (const std::size_t n{
                        mbun::cli::take_tsconfig_override(args, i, runTsconfig)};
                    n > 0) {
                    if (!runTsconfig.parseError.empty()) {
                        std::println(std::cerr, "error: {}", runTsconfig.parseError);
                        return 1;
                    }
                    args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                               args.begin() + static_cast<std::ptrdiff_t>(i + n));
                    continue;
                }
                if (const std::size_t n{take_valued_flag(args, i, "--loader", apply_loader_flag)};
                    n > 0) {
                    args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                               args.begin() + static_cast<std::ptrdiff_t>(i + n));
                    continue;
                }
                if (const std::size_t n{take_valued_flag(args, i, "-l", apply_loader_flag)};
                    n > 0) {
                    args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                               args.begin() + static_cast<std::ptrdiff_t>(i + n));
                    continue;
                }
                if (args[i] == "--if-present") { flags.ifPresent = true; ++i; continue; }
                if (args[i] == "--workspaces") { flags.workspaces = true; ++i; continue; }
                // `--filter <pattern>` / `-F <pattern>` (Arguments.rs:325): the
                // same workspace fan-out as `--workspaces`, narrowed to the
                // members whose package name matches. Unhandled, "--filter" was
                // taken as the script name (issue 26207).
                if ((args[i] == "--filter" || args[i] == "-F") && i + 1 < args.size()) {
                    flags.workspaces = true;
                    flags.workspaceFilters.emplace_back(args[i + 1]);
                    i += 2;
                    continue;
                }
                if (args[i].starts_with("--filter=")) {
                    flags.workspaces = true;
                    flags.workspaceFilters.emplace_back(args[i].substr(9));
                    ++i;
                    continue;
                }
                if (args[i] == "--bun" || args[i] == "-b") { flags.forceUsingBun = true; ++i; continue; }
                // `--cwd <dir>` / `--cwd=<dir>` — chdir before resolving the target
                // (Arguments.rs:773). Two-token form was previously unhandled, which
                // made `bun run --cwd sub script` take "--cwd" as the target.
                if (args[i] == "--cwd" && i + 1 < args.size()) {
                    apply_cwd_flag(args[i + 1]);
                    i += 2;
                    continue;
                }
                if (args[i].starts_with("--cwd=")) {
                    apply_cwd_flag(args[i].substr(6));
                    ++i;
                    continue;
                }
                // `--shell <STR>` / `--shell=<STR>`: 'bun' or 'system' (Arguments.rs:333).
                if (args[i] == "--shell" && i + 1 < args.size()) {
                    flags.useSystemShell = args[i + 1] != "bun";
                    i += 2;
                    continue;
                }
                if (args[i].starts_with("--shell=")) {
                    flags.useSystemShell = args[i].substr(8) != "bun";
                    ++i;
                    continue;
                }
                if (!is_skippable_run_flag(args[i])) break;
                ++i;
            }
            if (runTsconfig.value && !apply_tsconfig_override(*runTsconfig.value)) return 1;
            // `bun run` with no target prints run's help + the script list
            // (run_command.rs:2456-2466), it is NOT an error.
            if (i >= args.size()) {
                return exec_run_target("", {}, flags, /*allowFastRunForExtensions=*/false,
                                       /*binDirsOnly=*/false);
            }
            // `--` ends flag parsing; everything after it is passthrough and the
            // separator itself is consumed (Arguments.rs:917 `args.remaining()`),
            // so `bun run args -- a b` forwards exactly ["a","b"].
            std::size_t firstArg{i + 1};
            if (firstArg < args.size() && args[firstArg] == "--") ++firstArg;
            // `--workspaces` fans the script out over the workspace members
            // instead of resolving it against the cwd (multi_run.rs:839).
            if (flags.workspaces) {
                return exec_run_workspaces(args[i], std::span{args}.subspan(firstArg), flags);
            }
            // Tag::RunCommand — cli/mod.rs:1471-1473: bin_dirs_only=false,
            // allow_fast_run_for_extensions=false.
            return exec_run_target(args[i], std::span{args}.subspan(firstArg), flags,
                                   /*allowFastRunForExtensions=*/false, /*binDirsOnly=*/false);
        }
        // Bare `mbun <file.ts>` — Tag::AutoCommand. cli/mod.rs:1471-1473 sets
        // bin_dirs_only=true and allow_fast_run_for_extensions=true here, so an
        // existing file wins outright (no script lookup).
        if (looks_like_script(args[0]) || is_markdown(args[0])) {
            if (!applyGlobalTsconfig()) return 1;
            return exec_run_target(args[0], std::span{args}.subspan(1), globalFlags,
                                   /*allowFastRunForExtensions=*/true, /*binDirsOnly=*/true);
        }
    }

    auto parsed = mbun::cli::parse(args);

    using mbun::cli::Action;
    switch (parsed.action) {
    case Action::Version:
        // Three lines: mbun's real version, then the bun / node compatibility
        // claims (the corpus-pinned versions). Line 2 keeps `--version` output
        // containing Bun.version (regression/issue/10170 asserts toContain).
        std::println("mbun {}", mbun::cli::MBUN_VERSION);
        std::println("bun {} (compatible)", mbun::cli::VERSION);
        std::println("node v{} (compatible)", mbun::cli::NODE_COMPAT_VERSION);
        return 0;
    case Action::Help:
        std::print("{}", USAGE);
        return 0;
    case Action::Test:
        return run_test(std::span{args}.subspan(1),
                        globalTsconfig.value
                            ? std::optional<std::string_view>{*globalTsconfig.value}
                            : std::nullopt);
    case Action::Install:
        return run_install(std::span{args}.subspan(1));
    case Action::Add:
        return run_add(std::span{args}.subspan(1));
    case Action::Build:
        return run_build(std::span{args}.subspan(1),
                         globalTsconfig.value
                             ? std::optional<std::string_view>{*globalTsconfig.value}
                             : std::nullopt);
    case Action::Exec:
        return run_exec(parsed.argument);
    case Action::Publish:
        return run_publish(std::span{args}.subspan(1));
    case Action::NotImplemented:
        std::println("mbun: '{}' is planned but not implemented yet", parsed.argument);
        return 1;
    case Action::Unknown:
        // Tag::AutoCommand — an unrecognized first positional is not an error in
        // bun: it is a run target. `bun dev` runs the "dev" script, `bun eslint`
        // runs node_modules/.bin/eslint, and only when nothing matches does it
        // report `Script not found` + exit 1 (cli/mod.rs:1469-1481 → exec_with_cfg,
        // run_command.rs:2726-2790). --if-present makes the miss silent/0.
        if (!applyGlobalTsconfig()) return 1;
        return exec_run_target(args[0], std::span{args}.subspan(1), globalFlags,
                               /*allowFastRunForExtensions=*/true, /*binDirsOnly=*/true);
    }
    return 0;
}
