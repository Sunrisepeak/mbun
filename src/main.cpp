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
        std::vector<std::string_view> embeddedArgs(argv + 1, argv + argc);
        return run_embedded_program(*embedded, argc > 0 ? argv[0] : "mbun", embeddedArgs);
    }

    // process.execArgv — derived from the raw command line before any flag loop
    //    consumes it, exactly as bun does (node_process.rs create_exec_argv).
    //    Every dispatch below (node emulation, `run`, bare script) shares it; a
    //    compiled program overrides it above with its baked --compile-exec-argv.
    {
        std::vector<std::string_view> rawArgs(argv + 1, argv + argc);
        mbun::jsc::runtime::set_exec_argv(mbun::cli::derive_exec_argv(rawArgs));
    }

    // ── argv0 == `node` → node emulation (cli/mod.rs:952 → run_command.rs:2981).
    //    Must come before ANY bun-flag parsing: node's flags are not bun's.
    if (argc > 0 && is_node_argv0(argv[0])) {
        std::vector<std::string_view> nodeArgs(argv + 1, argv + argc);
        return exec_as_if_node(nodeArgs);
    }

    std::vector<std::string_view> args(argv + 1, argv + argc);

    // Strip leading global run flags so `mbun [flags] <script>` runs the script,
    // but never past -e/-p/--eval/--print (those consume the next token as code).
    RunFlags globalFlags{};
    while (!args.empty() && args[0] != "-e" && args[0] != "--eval" && args[0] != "-p" &&
           args[0] != "--print") {
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

    // Node-style re-exec: a leading `--flag` that is neither a bun run-flag nor
    // an eval flag, followed later by a positional entry point, is how the Node
    // corpus re-spawns `process.execPath` (`mbun --expose-gc file.js`,
    // `mbun --snapshot-blob b --build-snapshot file.js`, `mbun -r m file.js`).
    // bun's own CLI would reject the unknown flag; node-emulation (which already
    // ignores unmodelled node flags and resolves the first positional as the
    // script) is the correct handler, so route there instead of taking the flag
    // itself as the run target ("Script not found \"--expose-gc\"").
    if (!args.empty() && args[0].starts_with("-") && args[0] != "-" &&
        args[0] != "-e" && args[0] != "--eval" && args[0] != "-p" && args[0] != "--print") {
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
        if (hasPositional) return exec_as_if_node(args);
    }

    // `mbun run <script> [args...]` and bare `mbun <script.(m)js> [args...]`
    // execute a JS file through the JSC runtime with the Bun.* API in scope.
    if (!args.empty()) {
        // `mbun -e <code>` / `mbun --eval <code>`: evaluate a JS/TS string.
        if (args[0] == "-e" || args[0] == "--eval" || args[0] == "-p" || args[0] == "--print") {
            if (args.size() < 2) {
                std::println(std::cerr, "mbun {}: missing code (usage: mbun {} <code>)", args[0], args[0]);
                return 2;
            }
            // argv omits the script slot in eval mode: bun builds argv as
            // [exe] ++ (main unless it ends in "/[eval]" or "/[stdin]") ++ args
            // (node_process.rs), so `mbun -e <code> foo` puts foo at argv[1],
            // matching node. Emitting "[eval]" there shifted every user arg by one.
            std::vector<std::string> jsArgv{"mbun"};
            for (std::string_view a : std::span{args}.subspan(2)) jsArgv.emplace_back(a);
            mbun::jsc::runtime::set_argv(std::move(jsArgv));
            std::string code{args[1]};
            // `-p`/`--print` prints the expression result.
            if (args[0] == "-p" || args[0] == "--print") {
                code = "console.log((() => (" + code + "))())";
            }
            // node/bun expose the ORIGINAL eval source as process._eval
            // (run-eval.test.ts). Set it on the same first line so source-map
            // line numbers are unchanged; args[1] is the pre-wrap source.
            code = "process._eval=" + js_quote(args[1]) + ";" + code;
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
                if (args[i] == "--if-present") { flags.ifPresent = true; ++i; continue; }
                if (args[i] == "--workspaces") { flags.workspaces = true; ++i; continue; }
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
        return run_test(std::span{args}.subspan(1));
    case Action::Install:
        return run_install(std::span{args}.subspan(1));
    case Action::Add:
        return run_add(std::span{args}.subspan(1));
    case Action::Build:
        return run_build(std::span{args}.subspan(1));
    case Action::Exec:
        return run_exec(parsed.argument);
    case Action::NotImplemented:
        std::println("mbun: '{}' is planned but not implemented yet", parsed.argument);
        return 1;
    case Action::Unknown:
        // Tag::AutoCommand — an unrecognized first positional is not an error in
        // bun: it is a run target. `bun dev` runs the "dev" script, `bun eslint`
        // runs node_modules/.bin/eslint, and only when nothing matches does it
        // report `Script not found` + exit 1 (cli/mod.rs:1469-1481 → exec_with_cfg,
        // run_command.rs:2726-2790). --if-present makes the miss silent/0.
        return exec_run_target(args[0], std::span{args}.subspan(1), globalFlags,
                               /*allowFastRunForExtensions=*/true, /*binDirsOnly=*/true);
    }
    return 0;
}