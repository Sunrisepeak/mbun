// types.cppm — bunfig data model, translated from bun's configuration targets.
//
// Sources (read-only):
//   .mbun/bun-ref/src/bunfig/bunfig.rs
//   .mbun/bun-zig-src/src/cli/bunfig.zig
//   .mbun/bun-ref/src/options_types/schema.rs (api.BunInstall / NpmRegistry)
// Optionals preserve bun's absent-vs-set distinction; concrete defaults are
// otherwise materialised in the eventual Context.
export module mbun.bunfig.types;

import std;

namespace mbun::bunfig {

export enum class Command { Run, Auto, Test, Build, Install, Other };

export enum class LogLevel { Debug, Error, Warn, Info };
export enum class Shell { Bun, System };
export enum class OfflineMode { Online, Offline, Latest };
export enum class AutoInstall { Allow, Disable, Force, Fallback, Auto };
export enum class EnvBehavior { Default, Disable, LoadAll, Prefix };
export enum class JsxRuntime { Automatic, Classic, Solid };

export struct StringMap {
    std::vector<std::pair<std::string, std::string>> entries;

    const std::string* find(std::string_view key) const noexcept {
        for (const auto& [k, v] : entries) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }
};

// api.NpmRegistry — a resolved registry endpoint plus optional credentials.
export struct NpmRegistry {
    std::string url;
    std::string username;
    std::string password;
    std::string token;
    std::string email;
};

// A single `[install.scopes]` entry; scope is stored without the leading '@'.
export struct ScopedRegistry {
    std::string scope;
    NpmRegistry registry;
};

export struct Coverage {
    bool enabled{false};
    bool ignore_sourcemaps{false};
    bool skip_test_files{false};
    bool fail_on_low_coverage{false};
    std::optional<double> functions;
    std::optional<double> lines;
    std::optional<double> statements;
    std::vector<std::string> ignore_patterns;
    bool reporter_text{true};
    bool reporter_lcov{false};
    std::string reports_directory;
};

export struct TestOptions {
    std::string root;
    std::vector<std::string> preloads;
    bool smol{false};
    bool only_failures{false};
    bool reporter_dots{false};
    bool reporter_junit{false};
    std::string reporter_outfile;
    Coverage coverage;
    bool randomize{false};
    std::optional<std::uint32_t> seed;
    std::uint32_t rerun_each{0};
    std::uint32_t retry{0};
    std::vector<std::string> concurrent_test_glob;
    std::vector<std::string> path_ignore_patterns;
};

export struct InstallOptions {
    std::optional<AutoInstall> auto_install;
    std::optional<OfflineMode> prefer;
    std::string cafile;
    std::vector<std::string> ca;
    bool exact{false};
    std::optional<NpmRegistry> default_registry;
    std::vector<ScopedRegistry> scopes;
    bool dry_run{false};
    bool production{false};
    bool frozen_lockfile{false};
    bool save_text_lockfile{true};
    std::optional<std::uint32_t> concurrent_scripts;
    bool ignore_scripts{false};
    std::string linker;
    bool global_store{false};
    bool save_lockfile{true};
    bool save_yarn_lockfile{false};
    std::string lockfile_path;
    std::string save_lockfile_path;
    bool save_optional{true};
    bool save_peer{true};
    bool save_dev{true};
    std::string global_dir;
    std::string global_bin_dir;
    bool disable_cache{false};
    bool disable_manifest_cache{false};
    std::string cache_directory;
    std::optional<bool> link_workspace_packages;
    std::string security_scanner;
    std::optional<std::uint64_t> minimum_release_age_ms;
    std::vector<std::string> minimum_release_age_excludes;
    std::vector<std::string> public_hoist_pattern;
    std::vector<std::string> hoist_pattern;
};

export struct ServeOptions {
    std::vector<std::string> plugins;
    bool hmr{false};
    bool minify_syntax{false};
    bool minify_whitespace{false};
    bool minify_identifiers{false};
    StringMap define;
    std::string public_path;
    EnvBehavior env{EnvBehavior::Default};
    std::string env_prefix;
};

export struct BundleOptions {
    std::string outdir;
    std::vector<std::string> entry_points;
    StringMap packages;
};

export struct BunfigConfig {
    std::optional<LogLevel> log_level;
    StringMap define;
    std::string origin;
    bool disable_default_env_files{false};
    std::optional<std::uint16_t> serve_port;
    bool telemetry{true};
    bool smol{false};
    std::vector<std::string> preloads;
    TestOptions test;
    std::optional<InstallOptions> install;
    bool run_silent{false};
    std::optional<std::size_t> run_elide_lines;
    std::optional<Shell> run_shell;
    bool run_in_bun{false};
    bool no_orphans{false};
    std::optional<std::uint16_t> console_depth;
    ServeOptions serve;
    BundleOptions bundle;
    std::string jsx;
    JsxRuntime jsx_runtime{JsxRuntime::Automatic};
    bool jsx_development{true};
    std::string jsx_import_source;
    std::string jsx_fragment;
    std::string jsx_factory;
    std::string editor;
    std::vector<std::string> external;
    StringMap macros;
    StringMap loaders;
};

// options.Loader.fromString — case-insensitive, tolerates a leading '.'. Returns
// the canonical loader token, or nullopt if the name is not a known loader.
export inline std::optional<std::string_view> loader_from_string(std::string_view name) {
    if (!name.empty() && name.front() == '.') {
        name.remove_prefix(1);
    }
    static constexpr std::string_view kNames[]{
        "js",   "mjs",  "cjs",   "cts",  "mts",     "jsx",  "ts",
        "tsx",  "css",  "file",  "json", "jsonc",   "toml", "yaml",
        "json5", "wasm", "napi", "node", "dataurl", "base64", "txt",
        "text", "sh",   "sqlite", "sqlite_embedded", "html", "md", "markdown"};
    for (std::string_view candidate : kNames) {
        if (candidate.size() != name.size()) {
            continue;
        }
        bool eq{true};
        for (std::size_t i{0}; i < name.size(); ++i) {
            char a{name[i]};
            char b{candidate[i]};
            if (a >= 'A' && a <= 'Z') {
                a = static_cast<char>(a - 'A' + 'a');
            }
            if (a != b) {
                eq = false;
                break;
            }
        }
        if (eq) {
            return candidate;
        }
    }
    return std::nullopt;
}

} // namespace mbun::bunfig
