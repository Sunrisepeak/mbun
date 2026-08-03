// parser.cppm — bunfig.toml semantic parser.
//
// Blueprint (read-only, 1:1 control-flow reference):
//   .mbun/bun-zig-src/src/cli/bunfig.zig  (Bunfig.Parser.parse)
//   .mbun/bun-ref/src/bunfig/bunfig.rs
// The TOML value tree is produced by mbun.toml; this layer applies bun's type
// checks, command gates and enum coercions on top of it. Command gating follows
// the `comptime cmd == ...` branches in the blueprint exactly.
export module mbun.bunfig.parser;

import std;
import mbun.toml;
import mbun.bunfig.types;

namespace mbun::bunfig {

export struct ParseError {
    std::string message;
    std::string key;
};

namespace {

// Internal error carrier — keeps the phase methods readable; the single catch in
// Parser::parse turns it into the std::expected error channel.
struct BunfigException {
    ParseError err;
};

[[noreturn]] void fail(std::string key, std::string message) {
    throw BunfigException{ParseError{std::move(message), std::move(key)}};
}

double number_(const toml::Value& v) {
    return v.is_integer() ? static_cast<double>(v.as_integer()) : v.as_float();
}

std::uint16_t to_u16_(const toml::Value& v) {
    return static_cast<std::uint16_t>(static_cast<std::int64_t>(number_(v)));
}

std::uint32_t to_u32_(const toml::Value& v) {
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(number_(v)));
}

std::string trim_slashes_(std::string_view s) {
    std::size_t begin{0};
    std::size_t end{s.size()};
    while (begin < end && s[begin] == '/') {
        ++begin;
    }
    while (end > begin && s[end - 1] == '/') {
        --end;
    }
    return std::string{s.substr(begin, end - begin)};
}

// Minimal URL split covering what registry credential handling needs: scheme,
// userinfo (user[:password]) and host[:port] separated from the pathname.
struct ParsedUrl {
    std::string scheme;
    std::string username;
    std::string password;
    std::string host;
    std::string pathname;
    std::string href;
};

ParsedUrl parse_url_(std::string_view input) {
    ParsedUrl out;
    out.href = std::string{input};
    std::string_view rest{input};
    if (auto pos{rest.find("://")}; pos != std::string_view::npos) {
        out.scheme = std::string{rest.substr(0, pos)};
        rest = rest.substr(pos + 3);
    }
    std::string_view authority{rest};
    if (auto slash{rest.find('/')}; slash != std::string_view::npos) {
        authority = rest.substr(0, slash);
        out.pathname = std::string{rest.substr(slash)};
    }
    if (auto at{authority.rfind('@')}; at != std::string_view::npos) {
        std::string_view userinfo{authority.substr(0, at)};
        out.host = std::string{authority.substr(at + 1)};
        if (auto colon{userinfo.find(':')}; colon != std::string_view::npos) {
            out.username = std::string{userinfo.substr(0, colon)};
            out.password = std::string{userinfo.substr(colon + 1)};
        } else {
            out.username = std::string{userinfo};
        }
    } else {
        out.host = std::string{authority};
    }
    return out;
}

std::string href_without_auth_(const ParsedUrl& u) {
    return u.scheme + "://" + u.host + "/" + trim_slashes_(u.pathname) + "/";
}

} // namespace

export class Parser {
public:
    explicit Parser(Command command) : command_{command} {}

    std::expected<BunfigConfig, ParseError> parse(const toml::Value& root) const {
        if (!root.is_table()) {
            return std::unexpected(ParseError{"bunfig expects an object { } at the root", ""});
        }
        BunfigConfig result{};
        try {
            parse_common_(root, result);
            if (is_run_like_()) {
                parse_run_like_top_(root, result);
            }
            if (command_ == Command::Test) {
                parse_test_(root, result);
            }
            if (is_install_related_()) {
                parse_install_(root, result);
                parse_run_(root, result);
                parse_console_(root, result);
            }
            parse_serve_static_(root, result);
            parse_bundle_(root, result);
            parse_frontend_(root, result);
        } catch (const BunfigException& ex) {
            return std::unexpected(ex.err);
        }
        return result;
    }

private:
    Command command_;

    bool is_run_like_() const {
        return command_ == Command::Run || command_ == Command::Auto;
    }
    bool is_install_related_() const {
        return command_ == Command::Run || command_ == Command::Auto ||
               command_ == Command::Test || command_ == Command::Install;
    }
    bool is_build_like_() const {
        return command_ == Command::Build || command_ == Command::Run || command_ == Command::Auto;
    }

    // --- shared coercions --------------------------------------------------

    static void expect_string_(const toml::Value& v, std::string_view key) {
        if (!v.is_string()) {
            fail(std::string{key}, "expected string");
        }
    }
    static void expect_object_(const toml::Value& v, std::string_view key) {
        if (!v.is_table()) {
            fail(std::string{key}, "expected object");
        }
    }
    static void expect_bool_(const toml::Value& v, std::string_view key) {
        if (!v.is_boolean()) {
            fail(std::string{key}, "expected boolean");
        }
    }
    static void expect_number_(const toml::Value& v, std::string_view key) {
        if (!v.is_integer() && !v.is_float()) {
            fail(std::string{key}, "expected number");
        }
    }

    static LogLevel parse_log_level_(const toml::Value& v, std::string_view key) {
        expect_string_(v, key);
        const std::string& s{v.as_string()};
        if (s == "debug") {
            return LogLevel::Debug;
        }
        if (s == "error") {
            return LogLevel::Error;
        }
        if (s == "warn") {
            return LogLevel::Warn;
        }
        if (s == "info") {
            return LogLevel::Info;
        }
        fail(std::string{key}, "Invalid log level, must be one of debug, error, or warn");
    }

    // preload: a string or array of strings; empties are dropped.
    static std::vector<std::string> parse_preload_(const toml::Value& v, std::string_view key) {
        std::vector<std::string> out;
        if (v.is_array()) {
            for (std::size_t i{0}; i < v.size(); ++i) {
                const toml::Value& item{v.at(i)};
                expect_string_(item, key);
                if (!item.as_string().empty()) {
                    out.push_back(item.as_string());
                }
            }
        } else if (v.is_string()) {
            if (!v.as_string().empty()) {
                out.push_back(v.as_string());
            }
        } else {
            fail(std::string{key}, "Expected preload to be an array");
        }
        return out;
    }

    // define/loader-style tables keep only string-valued keys.
    static StringMap parse_string_map_(const toml::Value& v, std::string_view key) {
        expect_object_(v, key);
        StringMap map;
        for (const auto& [k, val] : v.table().entries) {
            if (!val.is_string()) {
                continue;
            }
            map.entries.emplace_back(k, val.as_string());
        }
        return map;
    }

    // string | array<string> -> flat vector (used by ca, external, excludes...).
    static std::vector<std::string> parse_string_or_array_(const toml::Value& v,
                                                            std::string_view key,
                                                            std::string_view err,
                                                            std::string_view item_err = {}) {
        std::vector<std::string> out;
        if (v.is_string()) {
            out.push_back(v.as_string());
        } else if (v.is_array()) {
            for (std::size_t i{0}; i < v.size(); ++i) {
                const toml::Value& item{v.at(i)};
                if (!item.is_string()) {
                    // bun reports the wrong *shape* ("must be a string or array of
                    // strings") differently from a correctly shaped array carrying a
                    // non-string element ("array must contain only strings").
                    fail(std::string{key},
                         std::string{item_err.empty() ? err : item_err});
                }
                out.push_back(item.as_string());
            }
        } else {
            fail(std::string{key}, std::string{err});
        }
        return out;
    }

    // --- common (all commands) --------------------------------------------

    void parse_common_(const toml::Value& root, BunfigConfig& out) const {
        if (const toml::Value* v{root.get("logLevel")}) {
            out.log_level = parse_log_level_(*v, "logLevel");
        }
        if (const toml::Value* v{root.get("define")}) {
            out.define = parse_string_map_(*v, "define");
        }
        if (const toml::Value* v{root.get("origin")}) {
            expect_string_(*v, "origin");
            out.origin = v->as_string();
        }
        if (const toml::Value* v{root.get("env")}) {
            parse_env_(*v, out);
        }
    }

    // env = false|null -> disable default .env; env = { file: false|null } too.
    static void parse_env_(const toml::Value& v, BunfigConfig& out) {
        if (v.is_boolean()) {
            if (!v.as_bool()) {
                out.disable_default_env_files = true;
            }
        } else if (v.is_table()) {
            if (const toml::Value* file{v.get("file")}) {
                if (file->is_boolean()) {
                    if (!file->as_bool()) {
                        out.disable_default_env_files = true;
                    }
                } else {
                    fail("env.file", "Expected 'file' to be a boolean or null");
                }
            }
        } else {
            fail("env", "Expected 'env' to be a boolean, null, or an object");
        }
    }

    // --- run/auto top-level (serve.port, preload, telemetry, smol) --------

    void parse_run_like_top_(const toml::Value& root, BunfigConfig& out) const {
        if (const toml::Value* serve{root.get("serve")}) {
            if (const toml::Value* port{serve->get("port")}) {
                expect_number_(*port, "serve.port");
                std::uint16_t p{to_u16_(*port)};
                out.serve_port = (p == 0) ? std::uint16_t{3000} : p;
            }
        }
        if (const toml::Value* v{root.get("preload")}) {
            out.preloads = parse_preload_(*v, "preload");
        }
        if (const toml::Value* v{root.get("telemetry")}) {
            expect_bool_(*v, "telemetry");
            out.telemetry = v->as_bool();
        }
        if (const toml::Value* v{root.get("smol")}) {
            expect_bool_(*v, "smol");
            out.smol = v->as_bool();
        }
    }

    // --- [test] ------------------------------------------------------------

    void parse_test_(const toml::Value& root, BunfigConfig& out) const {
        const toml::Value* test{root.get("test")};
        if (test == nullptr) {
            return;
        }
        TestOptions& t{out.test};
        if (const toml::Value* v{test->get("root")}) {
            if (v->is_string()) {
                t.root = v->as_string();
            }
        }
        if (const toml::Value* v{test->get("preload")}) {
            t.preloads = parse_preload_(*v, "test.preload");
        }
        if (const toml::Value* v{test->get("smol")}) {
            expect_bool_(*v, "test.smol");
            t.smol = v->as_bool();
        }
        if (const toml::Value* v{test->get("coverage")}) {
            expect_bool_(*v, "test.coverage");
            t.coverage.enabled = v->as_bool();
        }
        if (const toml::Value* v{test->get("onlyFailures")}) {
            expect_bool_(*v, "test.onlyFailures");
            t.only_failures = v->as_bool();
        }
        if (const toml::Value* v{test->get("reporter")}) {
            expect_object_(*v, "test.reporter");
            if (const toml::Value* junit{v->get("junit")}) {
                expect_string_(*junit, "test.reporter.junit");
                if (!junit->as_string().empty()) {
                    t.reporter_junit = true;
                    t.reporter_outfile = junit->as_string();
                }
            }
            const toml::Value* dots{v->get("dots")};
            if (dots == nullptr) {
                dots = v->get("dot");
            }
            if (dots != nullptr) {
                expect_bool_(*dots, "test.reporter.dots");
                t.reporter_dots = dots->as_bool();
            }
        }
        if (const toml::Value* v{test->get("coverageReporter")}) {
            parse_coverage_reporter_(*v, t.coverage);
        }
        if (const toml::Value* v{test->get("coverageDir")}) {
            expect_string_(*v, "test.coverageDir");
            t.coverage.reports_directory = v->as_string();
        }
        if (const toml::Value* v{test->get("coverageThreshold")}) {
            parse_coverage_threshold_(*v, t.coverage);
        }
        if (const toml::Value* v{test->get("coverageIgnoreSourcemaps")}) {
            expect_bool_(*v, "test.coverageIgnoreSourcemaps");
            t.coverage.ignore_sourcemaps = v->as_bool();
        }
        if (const toml::Value* v{test->get("coverageSkipTestFiles")}) {
            expect_bool_(*v, "test.coverageSkipTestFiles");
            t.coverage.skip_test_files = v->as_bool();
        }
        bool randomize_set{false};
        if (const toml::Value* v{test->get("randomize")}) {
            expect_bool_(*v, "test.randomize");
            t.randomize = v->as_bool();
            randomize_set = true;
        }
        if (const toml::Value* v{test->get("seed")}) {
            expect_number_(*v, "test.seed");
            (void)randomize_set;
            if (!t.randomize) {
                fail("test.seed", "\"seed\" can only be used when \"randomize\" is true");
            }
            t.seed = to_u32_(*v);
        }
        if (const toml::Value* v{test->get("rerunEach")}) {
            expect_number_(*v, "test.rerunEach");
            if (t.retry != 0) {
                fail("test.rerunEach", "\"rerunEach\" cannot be used with \"retry\"");
            }
            t.rerun_each = to_u32_(*v);
        }
        if (const toml::Value* v{test->get("retry")}) {
            expect_number_(*v, "test.retry");
            if (t.rerun_each != 0) {
                fail("test.retry", "\"retry\" cannot be used with \"rerunEach\"");
            }
            t.retry = to_u32_(*v);
        }
        if (const toml::Value* v{test->get("concurrentTestGlob")}) {
            parse_concurrent_glob_(*v, t);
        }
        if (const toml::Value* v{test->get("coveragePathIgnorePatterns")}) {
            t.coverage.ignore_patterns =
                parse_string_or_array_(*v, "test.coveragePathIgnorePatterns",
                                       "coveragePathIgnorePatterns must be a string or array of strings",
                                       "coveragePathIgnorePatterns array must contain only strings");
        }
        if (const toml::Value* v{test->get("pathIgnorePatterns")}) {
            t.path_ignore_patterns =
                parse_string_or_array_(*v, "test.pathIgnorePatterns",
                                       "pathIgnorePatterns must be a string or array of strings",
                                       "pathIgnorePatterns array must contain only strings");
        }
    }

    static void parse_coverage_reporter_item_(std::string_view item, std::string_view key,
                                              Coverage& cov) {
        if (item == "text") {
            cov.reporter_text = true;
        } else if (item == "lcov") {
            cov.reporter_lcov = true;
        } else {
            fail(std::string{key}, std::format("Invalid coverage reporter \"{}\"", item));
        }
    }

    static void parse_coverage_reporter_(const toml::Value& v, Coverage& cov) {
        cov.reporter_text = false;
        cov.reporter_lcov = false;
        if (v.is_string()) {
            parse_coverage_reporter_item_(v.as_string(), "test.coverageReporter", cov);
            return;
        }
        if (!v.is_array()) {
            fail("test.coverageReporter", "expected array");
        }
        for (std::size_t i{0}; i < v.size(); ++i) {
            const toml::Value& item{v.at(i)};
            expect_string_(item, "test.coverageReporter");
            parse_coverage_reporter_item_(item.as_string(), "test.coverageReporter", cov);
        }
    }

    static void parse_coverage_threshold_(const toml::Value& v, Coverage& cov) {
        if (v.is_integer() || v.is_float()) {
            double n{number_(v)};
            cov.functions = n;
            cov.lines = n;
            cov.statements = n;
            cov.fail_on_low_coverage = true;
            return;
        }
        expect_object_(v, "test.coverageThreshold");
        if (const toml::Value* f{v.get("functions")}) {
            expect_number_(*f, "test.coverageThreshold.functions");
            cov.functions = number_(*f);
            cov.fail_on_low_coverage = true;
        }
        if (const toml::Value* l{v.get("lines")}) {
            expect_number_(*l, "test.coverageThreshold.lines");
            cov.lines = number_(*l);
            cov.fail_on_low_coverage = true;
        }
        if (const toml::Value* s{v.get("statements")}) {
            expect_number_(*s, "test.coverageThreshold.statements");
            cov.statements = number_(*s);
            cov.fail_on_low_coverage = true;
        }
    }

    static void parse_concurrent_glob_(const toml::Value& v, TestOptions& t) {
        if (v.is_string()) {
            if (v.as_string().empty()) {
                fail("test.concurrentTestGlob", "concurrentTestGlob cannot be an empty string");
            }
            t.concurrent_test_glob.push_back(v.as_string());
            return;
        }
        if (v.is_array()) {
            if (v.size() == 0) {
                fail("test.concurrentTestGlob", "concurrentTestGlob array cannot be empty");
            }
            for (std::size_t i{0}; i < v.size(); ++i) {
                const toml::Value& item{v.at(i)};
                if (!item.is_string()) {
                    fail("test.concurrentTestGlob",
                         "concurrentTestGlob array must contain only strings");
                }
                if (item.as_string().empty()) {
                    fail("test.concurrentTestGlob",
                         "concurrentTestGlob patterns cannot be empty strings");
                }
                t.concurrent_test_glob.push_back(item.as_string());
            }
            return;
        }
        fail("test.concurrentTestGlob", "concurrentTestGlob must be a string or array of strings");
    }

    // --- [install] ---------------------------------------------------------

    void parse_install_(const toml::Value& root, BunfigConfig& out) const {
        const toml::Value* obj{root.get("install")};
        if (obj == nullptr) {
            return;
        }
        InstallOptions install{};
        if (const toml::Value* v{obj->get("auto")}) {
            install.auto_install = parse_auto_install_(*v);
        }
        if (const toml::Value* v{obj->get("cafile")}) {
            expect_string_(*v, "install.cafile");
            install.cafile = v->as_string();
        }
        if (const toml::Value* v{obj->get("ca")}) {
            install.ca = parse_string_or_array_(*v, "install.ca",
                                                "Invalid CA. Expected a string or an array of strings.");
        }
        if (const toml::Value* v{obj->get("exact")}) {
            if (v->is_boolean()) {
                install.exact = v->as_bool();
            }
        }
        if (const toml::Value* v{obj->get("prefer")}) {
            install.prefer = parse_prefer_(*v);
        }
        if (const toml::Value* v{obj->get("registry")}) {
            install.default_registry = parse_registry_(*v, "install.registry");
        }
        if (const toml::Value* v{obj->get("scopes")}) {
            parse_scopes_(*v, install);
        }
        if (const toml::Value* v{obj->get("dryRun")}) {
            if (v->is_boolean()) {
                install.dry_run = v->as_bool();
            }
        }
        if (const toml::Value* v{obj->get("production")}) {
            if (v->is_boolean()) {
                install.production = v->as_bool();
            }
        }
        if (const toml::Value* v{obj->get("frozenLockfile")}) {
            if (v->is_boolean()) {
                install.frozen_lockfile = v->as_bool();
            }
        }
        if (const toml::Value* v{obj->get("saveTextLockfile")}) {
            if (v->is_boolean()) {
                install.save_text_lockfile = v->as_bool();
            }
        }
        if (const toml::Value* v{obj->get("concurrentScripts")}) {
            if (v->is_integer() || v->is_float()) {
                std::uint32_t n{to_u32_(*v)};
                if (n != 0) {
                    install.concurrent_scripts = n;
                }
            }
        }
        if (const toml::Value* v{obj->get("ignoreScripts")}) {
            if (v->is_boolean()) {
                install.ignore_scripts = v->as_bool();
            }
        }
        if (const toml::Value* v{obj->get("linker")}) {
            expect_string_(*v, "install.linker");
            const std::string& s{v->as_string()};
            if (s != "isolated" && s != "hoisted") {
                fail("install.linker", "Expected one of \"isolated\" or \"hoisted\"");
            }
            install.linker = s;
        }
        if (const toml::Value* v{obj->get("globalStore")}) {
            if (v->is_boolean()) {
                install.global_store = v->as_bool();
            }
        }
        if (const toml::Value* v{obj->get("lockfile")}) {
            parse_lockfile_(*v, install);
        }
        if (const toml::Value* v{obj->get("optional")}) {
            if (v->is_boolean()) {
                install.save_optional = v->as_bool();
            }
        }
        if (const toml::Value* v{obj->get("peer")}) {
            if (v->is_boolean()) {
                install.save_peer = v->as_bool();
            }
        }
        if (const toml::Value* v{obj->get("dev")}) {
            if (v->is_boolean()) {
                install.save_dev = v->as_bool();
            }
        }
        if (const toml::Value* v{obj->get("globalDir")}) {
            if (v->is_string()) {
                install.global_dir = v->as_string();
            }
        }
        if (const toml::Value* v{obj->get("globalBinDir")}) {
            if (v->is_string()) {
                install.global_bin_dir = v->as_string();
            }
        }
        if (const toml::Value* v{obj->get("logLevel")}) {
            out.log_level = parse_log_level_(*v, "install.logLevel");
        }
        if (const toml::Value* v{obj->get("cache")}) {
            parse_cache_(*v, install);
        }
        if (const toml::Value* v{obj->get("linkWorkspacePackages")}) {
            if (v->is_boolean()) {
                install.link_workspace_packages = v->as_bool();
            }
        }
        if (const toml::Value* v{obj->get("security")}) {
            expect_object_(*v, "install.security");
            if (const toml::Value* scanner{v->get("scanner")}) {
                expect_string_(*scanner, "install.security.scanner");
                install.security_scanner = scanner->as_string();
            }
        }
        if (const toml::Value* v{obj->get("minimumReleaseAge")}) {
            if (!v->is_integer() && !v->is_float()) {
                fail("install.minimumReleaseAge", "Expected number of seconds for minimumReleaseAge");
            }
            double seconds{number_(*v)};
            if (seconds < 0) {
                fail("install.minimumReleaseAge",
                     "Expected positive number of seconds for minimumReleaseAge");
            }
            install.minimum_release_age_ms = static_cast<std::uint64_t>(seconds * 1000.0);
        }
        if (const toml::Value* v{obj->get("minimumReleaseAgeExcludes")}) {
            if (!v->is_array()) {
                fail("install.minimumReleaseAgeExcludes", "Expected array for minimumReleaseAgeExcludes");
            }
            for (std::size_t i{0}; i < v->size(); ++i) {
                const toml::Value& item{v->at(i)};
                expect_string_(item, "install.minimumReleaseAgeExcludes");
                install.minimum_release_age_excludes.push_back(item.as_string());
            }
        }
        if (const toml::Value* v{obj->get("publicHoistPattern")}) {
            install.public_hoist_pattern =
                parse_string_or_array_(*v, "install.publicHoistPattern", "Expected string or array");
        }
        if (const toml::Value* v{obj->get("hoistPattern")}) {
            install.hoist_pattern =
                parse_string_or_array_(*v, "install.hoistPattern", "Expected string or array");
        }
        out.install = std::move(install);
    }

    static AutoInstall parse_auto_install_(const toml::Value& v) {
        if (v.is_string()) {
            const std::string& s{v.as_string()};
            if (s == "auto") {
                return AutoInstall::Auto;
            }
            if (s == "force") {
                return AutoInstall::Force;
            }
            if (s == "disable") {
                return AutoInstall::Disable;
            }
            if (s == "fallback") {
                return AutoInstall::Fallback;
            }
            fail("install.auto",
                 "Invalid auto install setting, must be one of true, false, or \"force\" \"fallback\" \"disable\"");
        }
        if (v.is_boolean()) {
            return v.as_bool() ? AutoInstall::Allow : AutoInstall::Disable;
        }
        fail("install.auto",
             "Invalid auto install setting, must be one of true, false, or \"force\" \"fallback\" \"disable\"");
    }

    static OfflineMode parse_prefer_(const toml::Value& v) {
        expect_string_(v, "install.prefer");
        const std::string& s{v.as_string()};
        if (s == "offline") {
            return OfflineMode::Offline;
        }
        if (s == "online") {
            return OfflineMode::Online;
        }
        if (s == "latest") {
            return OfflineMode::Latest;
        }
        fail("install.prefer", "Invalid prefer setting, must be one of online or offline");
    }

    static NpmRegistry parse_registry_(const toml::Value& v, std::string_view key) {
        if (v.is_string()) {
            return parse_registry_url_(v.as_string());
        }
        if (v.is_table()) {
            NpmRegistry r;
            if (const toml::Value* url{v.get("url")}) {
                expect_string_(*url, "url");
                r.url = url->as_string();
            }
            if (const toml::Value* u{v.get("username")}) {
                expect_string_(*u, "username");
                r.username = u->as_string();
            }
            if (const toml::Value* p{v.get("password")}) {
                expect_string_(*p, "password");
                r.password = p->as_string();
            }
            if (const toml::Value* tok{v.get("token")}) {
                expect_string_(*tok, "token");
                r.token = tok->as_string();
            }
            return r;
        }
        fail(std::string{key}, "Expected registry to be a URL string or an object");
    }

    static NpmRegistry parse_registry_url_(std::string_view str) {
        ParsedUrl u{parse_url_(str)};
        NpmRegistry r;
        if (u.username.empty() && !u.password.empty()) {
            r.token = u.password;
            r.url = href_without_auth_(u);
        } else if (!u.username.empty() && !u.password.empty()) {
            r.username = u.username;
            r.password = u.password;
            r.url = href_without_auth_(u);
        } else {
            r.url = u.href;
        }
        return r;
    }

    static void parse_scopes_(const toml::Value& v, InstallOptions& install) {
        expect_object_(v, "install.scopes");
        for (const auto& [name, value] : v.table().entries) {
            if (name.empty()) {
                continue;
            }
            std::string_view scope{name};
            if (scope.front() == '@') {
                scope.remove_prefix(1);
            }
            install.scopes.push_back(
                ScopedRegistry{std::string{scope}, parse_registry_(value, "install.scopes")});
        }
    }

    static void parse_lockfile_(const toml::Value& v, InstallOptions& install) {
        if (const toml::Value* print{v.get("print")}) {
            expect_string_(*print, "install.lockfile.print");
            const std::string& s{print->as_string()};
            if (s != "bun") {
                if (s != "yarn") {
                    fail("install.lockfile.print",
                         "Invalid lockfile format, only 'yarn' output is implemented");
                }
                install.save_yarn_lockfile = true;
            }
        }
        if (const toml::Value* save{v.get("save")}) {
            if (save->is_boolean()) {
                install.save_lockfile = save->as_bool();
            }
        }
        if (const toml::Value* path{v.get("path")}) {
            if (path->is_string()) {
                install.lockfile_path = path->as_string();
            }
        }
        if (const toml::Value* save_path{v.get("savePath")}) {
            if (save_path->is_string()) {
                install.save_lockfile_path = save_path->as_string();
            }
        }
    }

    // cache: bool | string(dir) | { disable, disableManifest, dir }.
    static void parse_cache_(const toml::Value& v, InstallOptions& install) {
        if (v.is_boolean()) {
            if (!v.as_bool()) {
                install.disable_cache = true;
                install.disable_manifest_cache = true;
            }
            return;
        }
        if (v.is_string()) {
            install.cache_directory = v.as_string();
            return;
        }
        if (v.is_table()) {
            if (const toml::Value* d{v.get("disable")}) {
                if (d->is_boolean()) {
                    install.disable_cache = d->as_bool();
                }
            }
            if (const toml::Value* d{v.get("disableManifest")}) {
                if (d->is_boolean()) {
                    install.disable_manifest_cache = d->as_bool();
                }
            }
            if (const toml::Value* d{v.get("dir")}) {
                if (d->is_string()) {
                    install.cache_directory = d->as_string();
                }
            }
        }
    }

    // --- [run] -------------------------------------------------------------

    void parse_run_(const toml::Value& root, BunfigConfig& out) const {
        const toml::Value* run{root.get("run")};
        if (run == nullptr) {
            return;
        }
        if (const toml::Value* v{run->get("silent")}) {
            expect_bool_(*v, "run.silent");
            out.run_silent = v->as_bool();
        }
        if (const toml::Value* v{run->get("elide-lines")}) {
            expect_number_(*v, "run.elide-lines");
            out.run_elide_lines = static_cast<std::size_t>(number_(*v));
        }
        if (const toml::Value* v{run->get("shell")}) {
            if (!v->is_string()) {
                fail("run.shell", "Expected string");
            }
            const std::string& s{v->as_string()};
            if (s == "bun") {
                out.run_shell = Shell::Bun;
            } else if (s == "system") {
                out.run_shell = Shell::System;
            } else {
                fail("run.shell", "Invalid shell, only 'bun' and 'system' are supported");
            }
        }
        if (const toml::Value* v{run->get("bun")}) {
            expect_bool_(*v, "run.bun");
            out.run_in_bun = v->as_bool();
        }
        if (const toml::Value* v{run->get("noOrphans")}) {
            expect_bool_(*v, "run.noOrphans");
            out.no_orphans = v->as_bool();
        }
    }

    // --- [console] ---------------------------------------------------------

    void parse_console_(const toml::Value& root, BunfigConfig& out) const {
        const toml::Value* console{root.get("console")};
        if (console == nullptr) {
            return;
        }
        if (const toml::Value* depth{console->get("depth")}) {
            expect_number_(*depth, "console.depth");
            std::uint16_t d{to_u16_(*depth)};
            out.console_depth = (d == 0) ? std::numeric_limits<std::uint16_t>::max() : d;
        }
    }

    // --- [serve.static] ----------------------------------------------------

    void parse_serve_static_(const toml::Value& root, BunfigConfig& out) const {
        const toml::Value* serve{root.get("serve")};
        if (serve == nullptr) {
            return;
        }
        const toml::Value* stat{serve->get("static")};
        if (stat == nullptr) {
            return;
        }
        ServeOptions& s{out.serve};
        if (const toml::Value* v{stat->get("plugins")}) {
            s.plugins = parse_string_or_array_(*v, "serve.static.plugins", "expected string");
        }
        if (const toml::Value* v{stat->get("hmr")}) {
            if (v->is_boolean()) {
                s.hmr = v->as_bool();
            }
        }
        if (const toml::Value* v{stat->get("minify")}) {
            parse_serve_minify_(*v, s);
        }
        if (const toml::Value* v{stat->get("define")}) {
            s.define = parse_string_map_(*v, "serve.static.define");
        }
        if (const toml::Value* v{stat->get("publicPath")}) {
            if (v->is_string()) {
                s.public_path = v->as_string();
            }
        }
        if (const toml::Value* v{stat->get("env")}) {
            parse_serve_env_(*v, s);
        }
    }

    static void parse_serve_minify_(const toml::Value& v, ServeOptions& s) {
        if (v.is_boolean()) {
            bool value{v.as_bool()};
            s.minify_syntax = value;
            s.minify_whitespace = value;
            s.minify_identifiers = value;
        } else if (v.is_table()) {
            if (const toml::Value* x{v.get("syntax")}) {
                s.minify_syntax = x->is_boolean() && x->as_bool();
            }
            if (const toml::Value* x{v.get("whitespace")}) {
                s.minify_whitespace = x->is_boolean() && x->as_bool();
            }
            if (const toml::Value* x{v.get("identifiers")}) {
                s.minify_identifiers = x->is_boolean() && x->as_bool();
            }
        } else {
            fail("serve.static.minify", "Expected minify to be boolean or object");
        }
    }

    static void parse_serve_env_(const toml::Value& v, ServeOptions& s) {
        if (v.is_boolean()) {
            s.env = v.as_bool() ? EnvBehavior::LoadAll : EnvBehavior::Disable;
        } else if (v.is_string()) {
            const std::string& str{v.as_string()};
            if (str == "inline") {
                s.env = EnvBehavior::LoadAll;
            } else if (str == "disable") {
                s.env = EnvBehavior::Disable;
            } else if (auto star{str.find('*')}; star != std::string::npos) {
                if (star > 0) {
                    s.env_prefix = str.substr(0, star);
                    s.env = EnvBehavior::Prefix;
                } else {
                    s.env = EnvBehavior::LoadAll;
                }
            } else {
                fail("serve.static.env",
                     "Invalid env behavior, must be 'inline', 'disable', or a string with a '*' character");
            }
        } else {
            fail("serve.static.env",
                 "Invalid env behavior, must be 'inline', 'disable', or a string with a '*' character");
        }
    }

    // --- [bundle] ----------------------------------------------------------

    void parse_bundle_(const toml::Value& root, BunfigConfig& out) const {
        const toml::Value* bundle{root.get("bundle")};
        if (bundle == nullptr) {
            return;
        }
        if (is_build_like_()) {
            if (const toml::Value* v{bundle->get("outdir")}) {
                expect_string_(*v, "bundle.outdir");
                out.bundle.outdir = v->as_string();
            }
        }
        if (command_ != Command::Build) {
            return;
        }
        if (const toml::Value* v{bundle->get("logLevel")}) {
            out.log_level = parse_log_level_(*v, "bundle.logLevel");
        }
        if (const toml::Value* v{bundle->get("entryPoints")}) {
            if (!v->is_array()) {
                fail("bundle.entryPoints", "expected array");
            }
            for (std::size_t i{0}; i < v->size(); ++i) {
                const toml::Value& item{v->at(i)};
                expect_string_(item, "bundle.entryPoints");
                out.bundle.entry_points.push_back(item.as_string());
            }
        }
        if (const toml::Value* v{bundle->get("packages")}) {
            expect_object_(*v, "bundle.packages");
            for (const auto& [k, val] : v->table().entries) {
                if (!val.is_boolean()) {
                    continue;
                }
                out.bundle.packages.entries.emplace_back(k, val.as_bool() ? "always" : "never");
            }
        }
    }

    // --- jsx / debug / macros / external / loader --------------------------

    void parse_frontend_(const toml::Value& root, BunfigConfig& out) const {
        if (const toml::Value* v{root.get("jsx")}) {
            if (v->is_string()) {
                parse_jsx_(v->as_string(), out);
            }
        }
        if (const toml::Value* v{root.get("jsxImportSource")}) {
            if (v->is_string()) {
                out.jsx_import_source = v->as_string();
            }
        }
        if (const toml::Value* v{root.get("jsxFragment")}) {
            if (v->is_string()) {
                out.jsx_fragment = v->as_string();
            }
        }
        if (const toml::Value* v{root.get("jsxFactory")}) {
            if (v->is_string()) {
                out.jsx_factory = v->as_string();
            }
        }
        if (const toml::Value* v{root.get("debug")}) {
            if (const toml::Value* editor{v->get("editor")}) {
                if (editor->is_string()) {
                    out.editor = editor->as_string();
                }
            }
        }
        if (const toml::Value* v{root.get("macros")}) {
            if (v->is_table()) {
                out.macros = parse_string_map_(*v, "macros");
            }
        }
        if (const toml::Value* v{root.get("external")}) {
            out.external = parse_string_or_array_(*v, "external", "Expected string or array");
        }
        if (const toml::Value* v{root.get("loader")}) {
            parse_loader_(*v, out);
        }
    }

    static void parse_jsx_(std::string_view value, BunfigConfig& out) {
        out.jsx = std::string{value};
        if (value == "react") {
            out.jsx_runtime = JsxRuntime::Classic;
        } else if (value == "solid") {
            out.jsx_runtime = JsxRuntime::Solid;
        } else if (value == "react-jsx") {
            out.jsx_runtime = JsxRuntime::Automatic;
            out.jsx_development = false;
        } else if (value == "react-jsxDEV") {
            out.jsx_runtime = JsxRuntime::Automatic;
            out.jsx_development = true;
        } else {
            fail("jsx",
                 "Invalid jsx runtime, only 'react', 'solid', 'react-jsx', and 'react-jsxDEV' are supported");
        }
    }

    static void parse_loader_(const toml::Value& v, BunfigConfig& out) {
        expect_object_(v, "loader");
        for (const auto& [key, val] : v.table().entries) {
            if (key.empty()) {
                continue;
            }
            if (key.front() != '.') {
                fail("loader", "file extension for loader must start with a '.'");
            }
            expect_string_(val, "loader");
            std::optional<std::string_view> loader{loader_from_string(val.as_string())};
            if (!loader.has_value()) {
                fail("loader", "Invalid loader");
            }
            out.loaders.entries.emplace_back(key, std::string{*loader});
        }
    }
};

} // namespace mbun::bunfig
