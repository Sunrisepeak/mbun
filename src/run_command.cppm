// run_command.cppm — mbun.cli.run_command: `mbun run <script>` (package.json
// scripts) + the shared run-target resolution `bun run` uses.
//
// Ported from bun's real implementation (MIT), read line-by-line, NOT guessed:
//   - .mbun/bun-ref/src/cli/run_command.rs        (== src/runtime/cli/run_command.rs)
//       :2357 exec_with_cfg          — target priority rules
//       :2380 try_fast_run/skip_script_check
//       :2470 script lookup + pre/post hooks
//       :234/:259 run_package_script_foreground[_with_shell_path]
//       :151  SHELLS_TO_SEARCH / :156 find_shell_impl
//       :1932 configure_path_for_run_with_package_json_dir  — node_modules/.bin
//       :1821 bun_node_file_utf8 / :1876 create_fake_temporary_node_executable
//       :2745 error wording + exit codes
//   - .mbun/bun-ref/src/install/lib.rs:473 BUN_NODE_DIR,
//       :565-708 create_fake_temporary_node_executable  — the `--bun` shim.
//   - .mbun/bun-ref/src/dotenv/env_loader.rs:502 load_node_js_config.
//   - .mbun/bun-ref/src/resolver/package_json.rs:988-1024  — scripts map:
//       drops entries whose key OR value is empty / non-string.
//   - .mbun/bun-ref/src/shell_parser/parse.rs:4150 SPECIAL_CHARS,
//       :4215 BACKSLASHABLE_CHARS, :4231 escape_8bit — passthrough escaping.
//   - .mbun/bun-ref/src/bun_core/output.rs:2378 — `$ <cmd>` echo (STDERR).
//   - .mbun/bun-ref/src/bun_core/lib.rs:2365 — without_trailing_slash.
//
// DIVERGENCE (deliberate, documented): bun defaults to its OWN shell
// interpreter (`use_system_shell == false`, run_command.rs:306). mbun's shell
// interpreter (mbun.shell / `mbun exec`) does not yet implement `$VAR`
// expansion or the `exit` builtin, so defaulting to it would break most real
// package.json scripts. mbun therefore defaults to the SYSTEM shell — which is
// bun's own `--shell=system` code path (run_command.rs:355-370), ported
// faithfully — and still honors `--shell=bun`/`--shell=system`.
module;

#include <cerrno>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

export module mbun.cli.run_command;

import std;
import mbun.install.npm.json;
import mbun.which;

namespace mbun::cli::run {

// ─── path helpers ───────────────────────────────────────────────────────────

// ref: bun_core/lib.rs:2365 — note `e > 1`, so "/" stays "/" and "" stays "".
std::string_view without_trailing_slash_(std::string_view s) {
    std::size_t e{s.size()};
    while (e > 1 && (s[e - 1] == '/' || s[e - 1] == '\\')) {
        --e;
    }
    return s.substr(0, e);
}

// ─── passthrough escaping (shell_parser/parse.rs:4150-4260) ─────────────────

// SPECIAL_CHARS (parse.rs:4150). Note digits 0-9, '=' and ' ' are included, so
// `--port=3000` escapes to `"--port=3000"` — same as bun.
inline constexpr std::string_view SPECIAL_CHARS{"~[]#;\n\t\r*?{,}`$=()0123456789|><&'\" \\\x08"};

// parse.rs:4215 — chars needing a backslash inside double quotes.
inline constexpr std::string_view BACKSLASHABLE_CHARS{"$`\"\\"};

// parse.rs:2379 — SPECIAL_JS_CHAR = 8 (backspace).
inline constexpr char SPECIAL_JS_CHAR{'\x08'};

// ref: parse.rs:4333 needs_escape_utf8_ascii_latin1.
export bool needs_escape(std::string_view str) {
    return str.find_first_of(SPECIAL_CHARS) != std::string_view::npos;
}

// ref: parse.rs:4231 escape_8bit<ADD_QUOTES = true>. The result ("..." with
// $ ` " \ backslashed) is valid POSIX-sh double-quoting as well as bun-shell.
export void escape_8bit_quoted(std::string_view str, std::string& out) {
    out.push_back('"');
    for (char c : str) {
        if (BACKSLASHABLE_CHARS.find(c) != std::string_view::npos) {
            out.push_back('\\');
            out.push_back(c);
            continue;
        }
        if (c == SPECIAL_JS_CHAR) {
            out.push_back(SPECIAL_JS_CHAR);
            out.append("\"\"");
            continue;
        }
        out.push_back(c);
    }
    out.push_back('"');
}

// ref: run_command.rs:294-301 — append each passthrough arg, escaped if needed.
export std::string append_passthrough(std::string_view script,
                                      std::span<const std::string_view> passthrough) {
    std::string out{script};
    for (std::string_view part : passthrough) {
        out.push_back(' ');
        if (needs_escape(part)) {
            escape_8bit_quoted(part, out);
            continue;
        }
        out.append(part);
    }
    return out;
}

// ─── package.json scripts ───────────────────────────────────────────────────

export struct ScriptEntry {
    std::string name;
    std::string content;
};

export struct PackageScripts {
    bool found{false};                    // a package.json was located
    std::filesystem::path packageJsonPath{};
    std::filesystem::path packageJsonDir{};
    std::string packageName{};
    std::string packageVersion{};
    std::vector<ScriptEntry> scripts{};   // insertion order (bun prints in order)

    const ScriptEntry* find(std::string_view name) const {
        for (const auto& s : scripts) {
            if (s.name == name) return &s;
        }
        return nullptr;
    }
};

// Parse the `scripts` object out of package.json source.
// ref: resolver/package_json.rs:1002-1015 — an entry is DROPPED when the value
// is not a string, or when key OR value is empty. `{"scripts":{"build":""}}`
// must therefore report `Script not found`, not run an empty command.
export std::vector<ScriptEntry> parse_scripts(std::string_view source) {
    std::vector<ScriptEntry> out;
    auto doc = mbun::install::npm::json::parse(source);
    if (!doc || !doc->root) return out;
    const auto* scripts = doc->root->get("scripts");
    if (scripts == nullptr || !scripts->is_object()) return out;
    for (const auto& m : scripts->members) {
        if (m.value == nullptr) continue;
        auto value = m.value->as_str();
        if (!value) continue;  // non-string → dropped
        if (m.key.empty() || value->empty()) continue;
        out.push_back(ScriptEntry{std::string{m.key}, std::string{*value}});
    }
    return out;
}

// Walk up from `cwd` to the nearest enclosing package.json.
// ref: run_command.rs:2461 `root_dir.enclosing_package_json`.
export PackageScripts load_nearest_package_scripts(const std::filesystem::path& cwd) {
    PackageScripts result{};
    std::error_code ec{};
    std::filesystem::path dir{cwd};
    while (true) {
        std::filesystem::path candidate{dir / "package.json"};
        if (std::filesystem::exists(candidate, ec) && !std::filesystem::is_directory(candidate, ec)) {
            std::ifstream in{candidate, std::ios::binary};
            if (in) {
                std::string src{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
                result.found = true;
                result.packageJsonPath = candidate;
                result.packageJsonDir = dir;
                result.scripts = parse_scripts(src);
                if (auto doc = mbun::install::npm::json::parse(src); doc && doc->root) {
                    if (const auto* n = doc->root->get("name")) {
                        if (auto s = n->as_str()) result.packageName = std::string{*s};
                    }
                    if (const auto* v = doc->root->get("version")) {
                        if (auto s = v->as_str()) result.packageVersion = std::string{*s};
                    }
                }
                return result;
            }
        }
        std::filesystem::path parent{dir.parent_path()};
        if (parent.empty() || parent == dir) break;
        dir = parent;
    }
    return result;
}

// ─── PATH stitching (run_command.rs:1932-2056) ──────────────────────────────

// ─── `--bun` / `-b`: the fake `node` shim ───────────────────────────────────
//
// bun's `--bun` does exactly ONE thing (install/lib.rs:565
// create_fake_temporary_node_executable, reached from run_command.rs:1989 via
// `-b` → ctx.debug.run_in_bun → force_using_bun): it plants
// `<BUN_NODE_DIR>/{node,bun}` symlinks pointing at the running bun binary and
// prepends that dir to PATH. Every `#!/usr/bin/env node` shebang under
// `node_modules/.bin/*` then resolves `node` to bun, and bun's argv0 check
// (cli/mod.rs:952 is_node → PRETEND_TO_BE_NODE → run_command.rs:2981
// exec_as_if_node) boots the script as JS.
//
// There is NO shebang sniffing and NO extensionless-file heuristic in this
// path: the kernel execs the shebang, `/usr/bin/env` walks PATH, and the shim
// wins only because it is PATH[0]. Verified against real bun 1.3.14:
//   `bun run myclil`          → node v24 (the .bin file is exec'd as a binary)
//   `bun run --bun myclil`    → Bun 1.3.14, PATH[0] == /tmp/bun-node-<sha>
//   `bun run ./shbin`         → a `#!/bin/sh` script is parsed as JS and FAILS
//                               (path-like targets are booted as JS regardless
//                               of extension AND regardless of shebang).
//
// DIVERGENCE (deliberate): bun keys the dir on its git SHA
// (`/tmp/bun-node-<sha>`, install/lib.rs:473-491). mbun has no such SHA, and
// reusing bun's name would let an mbun shim re-point the `node` symlink of a
// real `bun` installed on the same machine (install/lib.rs:681-693 relinks any
// target it does not recognise). mbun therefore owns `/tmp/bun-node-mbun`.
export constexpr std::string_view BUN_NODE_DIR{"/tmp/bun-node-mbun"};

// ref: run_command.rs:1821-1827 bun_node_file_utf8. On POSIX this returns the
// DIRECTORY, not `<dir>/node` — that is bun's real behavior, not a typo here:
// under `--bun` real bun 1.3.14 exports NODE=/tmp/bun-node-0d9b296af (verified).
export std::string_view bun_node_file() { return BUN_NODE_DIR; }

// ref: install/lib.rs:604-616. PREFER the OS's answer over argv[0]: on a nested
// `--bun` the inner process is execve'd with argv[0] == <BUN_NODE_DIR>/node —
// the very link we are about to rewrite — so using it as the symlink target
// makes a self-loop and the next `env node` dies with ELOOP (bun #30711).
export std::optional<std::string> self_exe_path() {
    std::error_code ec{};
    std::filesystem::path p{std::filesystem::read_symlink("/proc/self/exe", ec)};
    if (ec || p.empty()) return std::nullopt;
    return p.string();
}

// Set when argv[0] ends in "node" (cli/mod.rs:952-958 is_node →
// PRETEND_TO_BE_NODE). install/lib.rs:570-573 short-circuits the shim on it:
// the OUTER process already planted the links and PATH.
bool gPretendToBeNode{false};
export void set_pretend_to_be_node(bool v) { gPretendToBeNode = v; }
export bool pretend_to_be_node() { return gPretendToBeNode; }

// Port of install/lib.rs:565-708. Appends `BUN_NODE_DIR:` to `path` — the
// caller appends the system PATH afterwards, which is why the dir goes on the
// END of the slice rather than the start (install/lib.rs:700-707). Writes the
// symlink target into `bunSelfPath`. Returns whether the dir was added.
//
// Every failure is deliberately NON-fatal and leaves PATH unmodified, matching
// the reference's `return Ok(())` arms — a `--bun` run on a box with a hostile
// /tmp degrades to the system node rather than dying.
export bool create_fake_node_executable(std::string& path, std::string& bunSelfPath) {
    if (gPretendToBeNode) return false;  // install/lib.rs:570-573

    if (bunSelfPath.empty()) {
        auto self = self_exe_path();
        if (!self) return false;  // install/lib.rs:625-637: no usable target
        bunSelfPath = *self;
    }
    const std::string dir{BUN_NODE_DIR};

    // Don't trust attacker-created entries in a shared temp dir: create it 0700
    // and, if it already exists, refuse to use it unless it is a directory we
    // own with no group/other write bits (install/lib.rs:655-668).
    if (::mkdir(dir.c_str(), 0700) != 0) {
        if (errno != EEXIST) return false;
        struct ::stat st{};
        if (::lstat(dir.c_str(), &st) != 0) return false;
        if (!S_ISDIR(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & 0022) != 0) {
            return false;
        }
    }

    for (std::string_view name : {"/node", "/bun"}) {
        std::string dest{dir};
        dest.append(name);
        bool replaced{false};
        while (true) {
            if (::symlink(bunSelfPath.c_str(), dest.c_str()) == 0) break;
            if (errno != EEXIST) return false;
            // The dir does not identify the binary, so a stale link may point at
            // a DIFFERENT build — blindly reusing it would make every `--bun`
            // child of THIS binary silently exec the other one. Verify the
            // target; replace it at most once (install/lib.rs:681-693).
            std::error_code ec{};
            std::filesystem::path cur{std::filesystem::read_symlink(dest, ec)};
            if ((!ec && cur.string() == bunSelfPath) || replaced) break;
            ::unlink(dest.c_str());
            replaced = true;
        }
    }

    if (!path.empty() && path.back() != ':') path.push_back(':');
    path.append(BUN_NODE_DIR);
    path.push_back(':');
    return true;
}

// `--bun` plumbing for build_path_for_run, mirroring the reference's
// force_using_bun / optional_bun_self_path out-params (run_command.rs:1938).
export struct NodeShim {
    bool forceUsingBun{false};  // in: `--bun` / `-b`
    bool nodeFound{true};       // in: `node` resolvable on the ORIGINAL PATH
    std::string bunSelfPath{};  // out: symlink target == this binary
    bool planted{false};        // out: BUN_NODE_DIR was prepended to PATH
};

// Builds PATH with `node_modules/.bin` for `cwd` and every ancestor prepended
// (deepest first), plus `package_json_dir` in front when the enclosing
// package.json lives outside `cwd`. Mirrors the reference byte-for-byte,
// including the root-level `/node_modules/.bin` the loop's final segment adds.
//
// When `shim` is non-null the bun-node dir lands in FRONT of all of it — the
// reference calls create_fake_temporary_node_executable with a still-empty
// buffer (run_command.rs:1989-1993), so PATH is
// `<BUN_NODE_DIR>:<pkg_json_dir>:<.bin chain>:<original>`.
export std::string build_path_for_run(std::string_view cwd, std::string_view packageJsonDir,
                                      std::string_view originalPath, NodeShim* shim = nullptr) {
    constexpr char DELIMITER{':'};
    constexpr std::string_view BIN_SUFFIX{"/node_modules/.bin"};
    std::string newPath;
    newPath.reserve(originalPath.size() + cwd.size() * 2 + 128);

    // ref: run_command.rs:1966 — `needs_to_force_bun = force_using_bun ||
    // !found_node`. bun plants the shim even WITHOUT `--bun` when `node` is not
    // installed at all (env_loader.rs:520-522 returns false).
    if (shim != nullptr && (shim->forceUsingBun || !shim->nodeFound)) {
        shim->planted = create_fake_node_executable(newPath, shim->bunSelfPath);
    }

    if (!packageJsonDir.empty()) {
        newPath.append(packageJsonDir);
        newPath.push_back(DELIMITER);
    }

    std::string_view remain{cwd};
    while (true) {
        std::size_t i{remain.find_last_of('/')};
        if (i == std::string_view::npos) break;
        newPath.append(without_trailing_slash_(remain));
        newPath.append(BIN_SUFFIX);
        newPath.push_back(DELIMITER);
        remain = remain.substr(0, i);
    }
    // final segment once the loop ends naturally (ref: run_command.rs:2044-2052)
    newPath.append(without_trailing_slash_(remain));
    newPath.append(BIN_SUFFIX);
    newPath.push_back(DELIMITER);

    newPath.append(originalPath);
    return newPath;
}

// ─── shell discovery (run_command.rs:151-213) ───────────────────────────────

bool is_executable_file_(const std::filesystem::path& p) {
    std::error_code ec{};
    if (!std::filesystem::is_regular_file(p, ec)) return false;
    return ::access(p.c_str(), X_OK) == 0;
}

// ref: run_command.rs:151 SHELLS_TO_SEARCH + :173 HARDCODED_POPULAR_ONES.
export std::optional<std::string> find_shell(std::string_view path) {
    constexpr std::array<std::string_view, 3> SHELLS_TO_SEARCH{"bash", "sh", "zsh"};
    for (std::string_view shell : SHELLS_TO_SEARCH) {
        std::size_t start{0};
        while (start <= path.size()) {
            std::size_t end{path.find(':', start)};
            std::string_view dir{path.substr(start, end == std::string_view::npos
                                                        ? std::string_view::npos
                                                        : end - start)};
            if (!dir.empty()) {
                std::filesystem::path candidate{std::filesystem::path{dir} / shell};
                if (is_executable_file_(candidate)) return candidate.string();
            }
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
    }
    constexpr std::array<std::string_view, 8> HARDCODED_POPULAR_ONES{
        "/bin/bash",      "/usr/bin/bash", "/usr/local/bin/bash", "/bin/sh",
        "/usr/bin/sh",    "/usr/bin/zsh",  "/usr/local/bin/zsh",  "/system/bin/sh"};
    for (std::string_view shell : HARDCODED_POPULAR_ONES) {
        if (is_executable_file_(std::filesystem::path{shell})) return std::string{shell};
    }
    return std::nullopt;
}

// ─── script execution ───────────────────────────────────────────────────────

export struct ScriptRunOptions {
    bool silent{false};
};

// ref: bun_core/output.rs:2378 print_command → Destination::Stderr. bun prints
// `$ <cmd>` to STDERR (test/cli/run/if-present.test.ts asserts stderr is
// non-empty for `bun run present` yet empty for `bun run present.js`).
void print_command_(std::string_view command) {
    std::println(std::cerr, "$ {}", command);
}

// Spawn `<shell> -c <script>` with `env`, inheriting stdio; returns the child's
// exit status. ref: run_command.rs:355-375 (system-shell branch).
std::optional<int> spawn_shell_(std::string_view shellBin, std::string_view script,
                                const std::filesystem::path& cwd,
                                const std::vector<std::string>& envStrings, int& signalOut) {
    std::string shellStr{shellBin};
    std::string dashC{"-c"};
    std::string scriptStr{script};
    std::vector<char*> argv{shellStr.data(), dashC.data(), scriptStr.data(), nullptr};

    std::vector<char*> envp;
    envp.reserve(envStrings.size() + 1);
    for (const auto& e : envStrings) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) return std::nullopt;
    // cwd: posix_spawn has no portable chdir; chdir in the parent is safe here
    // because the CLI exits right after the script (bun sets `cwd` on the
    // spawn options — run_command.rs:381).
    std::error_code ec{};
    std::filesystem::path previous{std::filesystem::current_path(ec)};
    if (!cwd.empty()) std::filesystem::current_path(cwd, ec);

    ::pid_t pid{};
    int rc{::posix_spawn(&pid, shellStr.c_str(), &actions, nullptr, argv.data(), envp.data())};
    if (!cwd.empty() && !previous.empty()) std::filesystem::current_path(previous, ec);
    ::posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return std::nullopt;

    int status{};
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return std::nullopt;
    }
    if (WIFSIGNALED(status)) {
        signalOut = WTERMSIG(status);
        return std::nullopt;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return std::nullopt;
}

// Build a null-delimited env from the current environ plus overrides.
std::vector<std::string> build_env_(const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::vector<std::string> out;
    for (char** e = ::environ; e != nullptr && *e != nullptr; ++e) {
        std::string_view entry{*e};
        std::size_t eq{entry.find('=')};
        std::string_view key{eq == std::string_view::npos ? entry : entry.substr(0, eq)};
        bool overridden{false};
        for (const auto& [k, v] : overrides) {
            if (k == key) { overridden = true; break; }
        }
        if (!overridden) out.emplace_back(entry);
    }
    for (const auto& [k, v] : overrides) out.push_back(k + "=" + v);
    return out;
}

// ref: run_command.rs:259-537 run_package_script_foreground_with_shell_path.
// Returns the exit code to propagate; std::nullopt when the script succeeded.
export std::optional<int> run_package_script(std::string_view script, std::string_view name,
                                             const std::filesystem::path& cwd,
                                             std::span<const std::string_view> passthrough,
                                             const std::vector<std::pair<std::string, std::string>>& envOverrides,
                                             std::string_view shellSearchPath,
                                             const ScriptRunOptions& options) {
    auto shell = find_shell(shellSearchPath);
    if (!shell) {
        std::println(std::cerr, "error: Failed to run script {} due to error MissingShell", name);
        return 1;
    }

    std::string copyScript{append_passthrough(script, passthrough)};

    if (!options.silent) print_command_(copyScript);

    // ref: run_command.rs:272-278 — npm_lifecycle_event / npm_lifecycle_script.
    std::vector<std::pair<std::string, std::string>> env{envOverrides};
    env.emplace_back("npm_lifecycle_event", std::string{name});
    env.emplace_back("npm_lifecycle_script", std::string{script});
    std::vector<std::string> envStrings{build_env_(env)};

    int signalNo{0};
    auto code = spawn_shell_(*shell, copyScript, cwd, envStrings, signalNo);

    if (!code) {
        if (signalNo != 0) {
            // ref: run_command.rs:456-470 — SIGINT is silent; others report.
            constexpr int SIGINT_NO{2};
            if (signalNo != SIGINT_NO && !options.silent) {
                std::println(std::cerr, "error: script \"{}\" was terminated by signal {}", name,
                             signalNo);
            }
            return 128 + signalNo;
        }
        if (!options.silent) {
            std::println(std::cerr, "error: Failed to run script {} due to error SpawnFailed", name);
        }
        return 1;
    }

    // ref: run_command.rs:344-353 / :459-469 — exit code 2 is reported silently
    // (bun suppresses the message for code 2 only), everything else prints.
    if (*code > 0) {
        if (*code != 2 && !options.silent) {
            std::println(std::cerr, "error: script \"{}\" exited with code {}", name, *code);
        }
        return *code;
    }
    return std::nullopt;
}

// ─── node_modules/.bin + $PATH binary fallback (run_command.rs:2687-2723) ───

// `which` over the stitched PATH, using the ported bun lookup (mbun.which).
export std::optional<std::string> find_binary_in_path(std::string_view path, std::string_view cwd,
                                                      std::string_view bin) {
    mbun::which::Fs fs{
        .is_executable_file_path = [](std::string_view p) { return is_executable_file_(std::filesystem::path{p}); },
        .exists_os_path = [](std::string_view p) {
            std::error_code ec{};
            return std::filesystem::exists(std::filesystem::path{p}, ec);
        }};
    return mbun::which::which(fs, path, cwd, bin, mbun::which::Platform::Posix);
}

// Exec a resolved binary directly (NOT through a shell), inheriting stdio.
// ref: run_command.rs:2078 run_binary / :2133 run_binary_without_bunx_path.
export int run_binary(std::string_view exe, std::span<const std::string_view> args,
                      const std::vector<std::pair<std::string, std::string>>& envOverrides) {
    std::string exeStr{exe};
    std::vector<std::string> owned;
    owned.push_back(exeStr);
    for (std::string_view a : args) owned.emplace_back(a);
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (auto& s : owned) argv.push_back(s.data());
    argv.push_back(nullptr);

    std::vector<std::string> envStrings{build_env_(envOverrides)};
    std::vector<char*> envp;
    envp.reserve(envStrings.size() + 1);
    for (const auto& e : envStrings) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);

    ::pid_t pid{};
    if (::posix_spawn(&pid, exeStr.c_str(), nullptr, nullptr, argv.data(), envp.data()) != 0) {
        return -1;
    }
    int status{};
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

// ─── `bun run --help` (run_command.rs:97-149) ───────────────────────────────

export void print_help(const PackageScripts& pkg) {
    std::print(
        "Usage: bun run [flags] <file or script>\n"
        "\n"
        "Flags:\n"
        "  -b, --bun                    Force a script or package to use Bun's runtime instead of Node.js\n"
        "      --shell <STR>            Control the shell used for package.json scripts ('bun' or 'system')\n"
        "      --silent                 Don't print the script command\n"
        "      --if-present             Exit without an error if the entrypoint does not exist\n"
        "\n"
        "Examples:\n"
        "  Run a JavaScript or TypeScript file\n"
        "  bun run ./index.js\n"
        "  bun run ./index.tsx\n"
        "\n"
        "  Run a package.json script\n"
        "  bun run dev\n"
        "  bun run lint\n"
        "\n"
        "Full documentation is available at https://bun.com/docs/cli/run\n");

    // ref: run_command.rs:126-147 — the script list, printed in insertion order.
    if (pkg.found) {
        if (!pkg.scripts.empty()) {
            std::println("");
            std::println("package.json scripts ({} found):", pkg.scripts.size());
            for (const auto& s : pkg.scripts) {
                std::println("");
                std::println("  $ bun run {}", s.name);
                std::println("    {}", s.content);
            }
            std::println("");
        } else {
            std::println("");
            std::println("No \"scripts\" found in package.json.");
        }
    }
    std::cout.flush();
}

}  // namespace mbun::cli::run
