// test_bunfig.cpp — mbun.bunfig semantic parser test suite.
//
// Each case feeds a bunfig.toml fragment through mbun.toml -> Parser and asserts
// the resulting config object. Expectations are pinned to bun's blueprint:
//   .mbun/bun-zig-src/src/cli/bunfig.zig  (Bunfig.Parser.parse)
//   .mbun/bun-ref/src/bunfig/bunfig.rs
// Command gates match the blueprint's `comptime cmd == ...` branches.
import std;
import mbun.toml;
import mbun.bunfig;

namespace {

using mbun::bunfig::BunfigConfig;
using mbun::bunfig::Command;
using mbun::bunfig::Parser;

int gChecks{0};
int gFailures{0};
constexpr int MAX_FAILURE_PRINTS{40};

void report_failure(std::string_view what) {
    ++gFailures;
    if (gFailures <= MAX_FAILURE_PRINTS) {
        std::println("  FAIL {}", what);
    }
}

void check(bool cond, std::string_view what) {
    ++gChecks;
    if (!cond) {
        report_failure(what);
    }
}

// Parse `src` for `cmd`, expecting success; returns the config or reports.
std::optional<BunfigConfig> parse_ok(std::string_view src, Command cmd, std::string_view label) {
    auto toml{mbun::toml::parse(src)};
    if (!toml) {
        report_failure(std::format("[{}] TOML parse failed: {}", label, toml.error().message));
        return std::nullopt;
    }
    Parser parser{cmd};
    auto cfg{parser.parse(*toml)};
    if (!cfg) {
        report_failure(std::format("[{}] bunfig parse failed: {}", label, cfg.error().message));
        return std::nullopt;
    }
    return std::move(*cfg);
}

// Parse expecting a bunfig ParseError with a key containing `key_frag`.
void parse_err(std::string_view src, Command cmd, std::string_view key_frag, std::string_view label) {
    ++gChecks;
    auto toml{mbun::toml::parse(src)};
    if (!toml) {
        report_failure(std::format("[{}] expected bunfig error but TOML failed", label));
        return;
    }
    Parser parser{cmd};
    auto cfg{parser.parse(*toml)};
    if (cfg) {
        report_failure(std::format("[{}] expected error, got success", label));
        return;
    }
    if (cfg.error().key.find(key_frag) == std::string::npos) {
        report_failure(std::format("[{}] error key '{}' missing '{}'", label, cfg.error().key, key_frag));
    }
}

// --- top-level ------------------------------------------------------------

void test_common() {
    if (auto c{parse_ok("logLevel = \"warn\"\norigin = \"https://ex.com\"", Command::Run, "common")}) {
        check(c->log_level == mbun::bunfig::LogLevel::Warn, "logLevel=warn");
        check(c->origin == "https://ex.com", "origin");
    }
    parse_err("logLevel = \"loud\"", Command::Run, "logLevel", "logLevel-invalid");
    parse_err("logLevel = 3", Command::Run, "logLevel", "logLevel-type");

    if (auto c{parse_ok("define = { A = \"1\", B = 2, C = \"x\" }", Command::Run, "define")}) {
        // non-string values dropped
        check(c->define.entries.size() == 2, "define keeps 2 string entries");
        check(c->define.find("A") && *c->define.find("A") == "1", "define A");
        check(c->define.find("C") && *c->define.find("C") == "x", "define C");
        check(c->define.find("B") == nullptr, "define B dropped (int)");
    }

    // env = false -> disable default env files
    if (auto c{parse_ok("env = false", Command::Run, "env-false")}) {
        check(c->disable_default_env_files, "env=false disables");
    }
    if (auto c{parse_ok("env = { file = false }", Command::Run, "env-file")}) {
        check(c->disable_default_env_files, "env.file=false disables");
    }
    if (auto c{parse_ok("env = true", Command::Run, "env-true")}) {
        check(!c->disable_default_env_files, "env=true keeps");
    }
}

void test_run_top() {
    if (auto c{parse_ok("preload = [\"./a.ts\", \"\", \"./b.ts\"]\ntelemetry = false\nsmol = true",
                        Command::Run, "run-top")}) {
        check(c->preloads.size() == 2, "preload drops empties");
        check(c->preloads[0] == "./a.ts" && c->preloads[1] == "./b.ts", "preload values");
        check(!c->telemetry, "telemetry=false");
        check(c->smol, "smol=true");
    }
    // serve.port: 0 -> 3000
    if (auto c{parse_ok("[serve]\nport = 0", Command::Run, "port-zero")}) {
        check(c->serve_port.has_value() && *c->serve_port == 3000, "port 0 -> 3000");
    }
    if (auto c{parse_ok("[serve]\nport = 8080", Command::Run, "port")}) {
        check(c->serve_port.has_value() && *c->serve_port == 8080, "port 8080");
    }
    // preload not parsed for Install command
    if (auto c{parse_ok("preload = [\"./a.ts\"]", Command::Install, "preload-gate")}) {
        check(c->preloads.empty(), "preload gated off for install");
    }
}

// --- [test] ---------------------------------------------------------------

void test_test_section() {
    constexpr std::string_view src{R"(
[test]
root = "test"
preload = "./setup.ts"
smol = true
coverage = true
onlyFailures = true
coverageDir = "cov"
coverageThreshold = 0.9
)"};
    if (auto c{parse_ok(src, Command::Test, "test-basic")}) {
        check(c->test.root == "test", "test.root");
        check(c->test.preloads.size() == 1 && c->test.preloads[0] == "./setup.ts", "test.preload");
        check(c->test.smol, "test.smol");
        check(c->test.coverage.enabled, "test.coverage");
        check(c->test.only_failures, "test.onlyFailures");
        check(c->test.coverage.reports_directory == "cov", "test.coverageDir");
        check(c->test.coverage.functions.has_value() && *c->test.coverage.functions == 0.9,
              "coverageThreshold scalar -> functions");
        check(c->test.coverage.fail_on_low_coverage, "coverageThreshold sets fail flag");
    }

    if (auto c{parse_ok("[test]\ncoverageReporter = [\"lcov\", \"text\"]", Command::Test, "cov-rep")}) {
        check(c->test.coverage.reporter_lcov && c->test.coverage.reporter_text, "coverageReporter both");
    }
    if (auto c{parse_ok("[test]\ncoverageReporter = \"lcov\"", Command::Test, "cov-rep-str")}) {
        check(c->test.coverage.reporter_lcov && !c->test.coverage.reporter_text,
              "coverageReporter lcov only resets text");
    }
    parse_err("[test]\ncoverageReporter = \"bogus\"", Command::Test, "coverageReporter", "cov-rep-bad");

    if (auto c{parse_ok("[test]\ncoverageThreshold = { lines = 0.5, statements = 0.6 }", Command::Test,
                        "cov-thresh-obj")}) {
        check(c->test.coverage.lines.has_value() && *c->test.coverage.lines == 0.5, "threshold lines");
        check(c->test.coverage.statements.has_value() && *c->test.coverage.statements == 0.6,
              "threshold statements");
        check(!c->test.coverage.functions.has_value(), "threshold functions unset");
    }

    // reporter.junit + dots
    if (auto c{parse_ok("[test.reporter]\njunit = \"out.xml\"\ndots = true", Command::Test, "reporter")}) {
        check(c->test.reporter_junit && c->test.reporter_outfile == "out.xml", "reporter.junit");
        check(c->test.reporter_dots, "reporter.dots");
    }

    // seed requires randomize
    parse_err("[test]\nseed = 42", Command::Test, "test.seed", "seed-no-randomize");
    if (auto c{parse_ok("[test]\nrandomize = true\nseed = 42", Command::Test, "seed-ok")}) {
        check(c->test.seed.has_value() && *c->test.seed == 42, "seed value");
    }
    // retry/rerunEach mutual exclusion
    // rerunEach is parsed before retry, so retry is where the conflict trips.
    parse_err("[test]\nretry = 2\nrerunEach = 3", Command::Test, "test.retry", "retry-rerun");

    if (auto c{parse_ok("[test]\nconcurrentTestGlob = [\"**/*.test.ts\"]", Command::Test, "glob")}) {
        check(c->test.concurrent_test_glob.size() == 1, "concurrentTestGlob");
    }
    parse_err("[test]\nconcurrentTestGlob = \"\"", Command::Test, "concurrentTestGlob", "glob-empty");

    // [test] gated off for Run command
    if (auto c{parse_ok("[test]\nroot = \"test\"", Command::Run, "test-gate")}) {
        check(c->test.root.empty(), "test section gated off for run");
    }
}

// --- [install] ------------------------------------------------------------

void test_install_section() {
    constexpr std::string_view src{R"(
[install]
production = true
frozenLockfile = true
exact = true
cafile = "/etc/ca.pem"
ca = ["-----A-----", "-----B-----"]
linker = "isolated"
globalStore = false
[install.cache]
disable = true
dir = "/tmp/cache"
[install.lockfile]
save = false
print = "yarn"
savePath = "custom.lock"
)"};
    if (auto c{parse_ok(src, Command::Install, "install")}) {
        check(c->install.has_value(), "install present");
        if (c->install) {
            const auto& i{*c->install};
            check(i.production, "production");
            check(i.frozen_lockfile, "frozenLockfile");
            check(i.exact, "exact");
            check(i.cafile == "/etc/ca.pem", "cafile");
            check(i.ca.size() == 2, "ca array");
            check(i.linker == "isolated", "linker");
            check(i.disable_cache, "cache.disable");
            check(i.cache_directory == "/tmp/cache", "cache.dir");
            check(!i.save_lockfile, "lockfile.save=false");
            check(i.save_yarn_lockfile, "lockfile.print=yarn");
            check(i.save_lockfile_path == "custom.lock", "lockfile.savePath string");
        }
    }

    // registry string with no auth -> url == href
    if (auto c{parse_ok("[install]\nregistry = \"https://registry.npmjs.org/\"", Command::Install, "reg-str")}) {
        check(c->install && c->install->default_registry.has_value(), "registry present");
        if (c->install && c->install->default_registry) {
            check(c->install->default_registry->url == "https://registry.npmjs.org/", "registry url");
            check(c->install->default_registry->token.empty(), "registry no token");
        }
    }
    // registry string with token (empty user, password present)
    if (auto c{parse_ok("[install]\nregistry = \"https://:secret@reg.example.com/path/\"",
                        Command::Install, "reg-token")}) {
        if (c->install && c->install->default_registry) {
            check(c->install->default_registry->token == "secret", "registry token extracted");
            check(c->install->default_registry->url == "https://reg.example.com/path/",
                  "registry url without auth");
        }
    }
    // registry object
    if (auto c{parse_ok("[install.registry]\nurl = \"https://r/\"\ntoken = \"tok\"", Command::Install,
                        "reg-obj")}) {
        if (c->install && c->install->default_registry) {
            check(c->install->default_registry->url == "https://r/", "registry obj url");
            check(c->install->default_registry->token == "tok", "registry obj token");
        }
    }

    // scopes: leading @ stripped
    if (auto c{parse_ok("[install.scopes]\n\"@acme\" = \"https://acme.reg/\"", Command::Install,
                        "scopes")}) {
        if (c->install) {
            check(c->install->scopes.size() == 1, "one scope");
            if (!c->install->scopes.empty()) {
                check(c->install->scopes[0].scope == "acme", "scope @ stripped");
                check(c->install->scopes[0].registry.url == "https://acme.reg/", "scope url");
            }
        }
    }

    // auto install enum coercions
    if (auto c{parse_ok("[install]\nauto = \"fallback\"", Command::Install, "auto")}) {
        check(c->install && c->install->auto_install == mbun::bunfig::AutoInstall::Fallback, "auto=fallback");
    }
    if (auto c{parse_ok("[install]\nauto = false", Command::Install, "auto-bool")}) {
        check(c->install && c->install->auto_install == mbun::bunfig::AutoInstall::Disable, "auto=false");
    }
    parse_err("[install]\nauto = \"nope\"", Command::Install, "install.auto", "auto-bad");
    parse_err("[install]\nlinker = \"weird\"", Command::Install, "install.linker", "linker-bad");
    parse_err("[install]\nprefer = \"maybe\"", Command::Install, "install.prefer", "prefer-bad");

    // minimumReleaseAge seconds -> ms
    if (auto c{parse_ok("[install]\nminimumReleaseAge = 3\nminimumReleaseAgeExcludes = [\"typescript\"]",
                        Command::Install, "min-age")}) {
        check(c->install && c->install->minimum_release_age_ms == 3000, "minimumReleaseAge s->ms");
        check(c->install && c->install->minimum_release_age_excludes.size() == 1, "excludes");
    }

    // install gated off for Build command
    if (auto c{parse_ok("[install]\nproduction = true", Command::Build, "install-gate")}) {
        check(!c->install.has_value(), "install gated off for build");
    }
}

// --- [run] ----------------------------------------------------------------

void test_run_section() {
    constexpr std::string_view src{R"(
[run]
silent = true
bun = true
shell = "system"
elide-lines = 5
noOrphans = true
)"};
    if (auto c{parse_ok(src, Command::Run, "run")}) {
        check(c->run_silent, "run.silent");
        check(c->run_in_bun, "run.bun");
        check(c->run_shell == mbun::bunfig::Shell::System, "run.shell=system");
        check(c->run_elide_lines.has_value() && *c->run_elide_lines == 5, "run.elide-lines");
        check(c->no_orphans, "run.noOrphans");
    }
    if (auto c{parse_ok("[run]\nshell = \"bun\"", Command::Run, "shell-bun")}) {
        check(c->run_shell == mbun::bunfig::Shell::Bun, "run.shell=bun");
    }
    parse_err("[run]\nshell = \"fish\"", Command::Run, "run.shell", "shell-bad");
    parse_err("[run]\nsilent = 1", Command::Run, "run.silent", "silent-type");
}

// --- [serve.static] -------------------------------------------------------

void test_serve_static() {
    constexpr std::string_view src{R"(
[serve.static]
plugins = ["./plug.ts"]
hmr = true
publicPath = "/assets"
minify = true
[serve.static.define]
X = "1"
)"};
    if (auto c{parse_ok(src, Command::Run, "serve-static")}) {
        check(c->serve.plugins.size() == 1, "serve plugins");
        check(c->serve.hmr, "serve hmr");
        check(c->serve.public_path == "/assets", "serve publicPath");
        check(c->serve.minify_syntax && c->serve.minify_whitespace && c->serve.minify_identifiers,
              "serve minify=true all");
        check(c->serve.define.entries.size() == 1, "serve define");
    }
    // minify object form
    if (auto c{parse_ok("[serve.static.minify]\nsyntax = true\nwhitespace = false", Command::Run,
                        "minify-obj")}) {
        check(c->serve.minify_syntax && !c->serve.minify_whitespace, "minify object");
    }
    // env behaviors
    if (auto c{parse_ok("[serve.static]\nenv = \"PUBLIC_*\"", Command::Run, "env-prefix")}) {
        check(c->serve.env == mbun::bunfig::EnvBehavior::Prefix && c->serve.env_prefix == "PUBLIC_",
              "serve env prefix");
    }
    if (auto c{parse_ok("[serve.static]\nenv = \"inline\"", Command::Run, "env-inline")}) {
        check(c->serve.env == mbun::bunfig::EnvBehavior::LoadAll, "serve env inline");
    }
    parse_err("[serve.static]\nenv = \"noStar\"", Command::Run, "serve.static.env", "env-bad");
}

// --- [debug] / jsx / loader ----------------------------------------------

void test_frontend() {
    if (auto c{parse_ok("[debug]\neditor = \"nvim\"", Command::Run, "debug")}) {
        check(c->editor == "nvim", "debug.editor");
    }
    if (auto c{parse_ok("jsx = \"react-jsx\"", Command::Run, "jsx")}) {
        check(c->jsx_runtime == mbun::bunfig::JsxRuntime::Automatic, "jsx react-jsx runtime");
        check(!c->jsx_development, "jsx react-jsx dev=false");
    }
    if (auto c{parse_ok("jsx = \"react\"\njsxImportSource = \"preact\"\njsxFactory = \"h\"",
                        Command::Run, "jsx2")}) {
        check(c->jsx_runtime == mbun::bunfig::JsxRuntime::Classic, "jsx react=classic");
        check(c->jsx_import_source == "preact", "jsxImportSource");
        check(c->jsx_factory == "h", "jsxFactory");
    }
    parse_err("jsx = \"vue\"", Command::Run, "jsx", "jsx-bad");

    if (auto c{parse_ok("[loader]\n\".mdx\" = \"jsx\"\n\".data\" = \"text\"", Command::Run, "loader")}) {
        check(c->loaders.entries.size() == 2, "loader entries");
        check(c->loaders.find(".mdx") && *c->loaders.find(".mdx") == "jsx", "loader .mdx=jsx");
    }
    parse_err("[loader]\n\"mdx\" = \"jsx\"", Command::Run, "loader", "loader-no-dot");
    parse_err("[loader]\n\".mdx\" = \"bogus\"", Command::Run, "loader", "loader-bad");

    // external string or array
    if (auto c{parse_ok("external = \"react\"", Command::Run, "external-str")}) {
        check(c->external.size() == 1 && c->external[0] == "react", "external string");
    }
    if (auto c{parse_ok("external = [\"a\", \"b\"]", Command::Run, "external-arr")}) {
        check(c->external.size() == 2, "external array");
    }
}

void test_root_type() {
    // Non-table root rejected. Use a TOML array-of-tables? Simpler: an array
    // value cannot be a document root, so feed a bare array through a crafted
    // Value is not possible via TOML text; instead assert an empty doc parses.
    if (auto c{parse_ok("", Command::Run, "empty-doc")}) {
        check(c->telemetry, "empty doc keeps telemetry default true");
    }
}

} // namespace

int main() {
    test_common();
    test_run_top();
    test_test_section();
    test_install_section();
    test_run_section();
    test_serve_static();
    test_frontend();
    test_root_type();

    if (gFailures > MAX_FAILURE_PRINTS) {
        std::println("  ... {} more failures not shown", gFailures - MAX_FAILURE_PRINTS);
    }
    std::println("test_bunfig: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
