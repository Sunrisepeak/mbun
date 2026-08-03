// package_manager/options.cppm — mbun.install.package_manager.options
//
// Mechanical port of the pure-logic option/flag state of bun
// src/install/PackageManager.rs + PackageManager/PackageManagerOptions.rs:
//
//   * Subcommand (+ capability predicates), WorkspaceFilter parsing,
//     PackageUpdateInfo, the Update / Features structs;
//   * the Do / Enable bitflag sets with their exact bit layout and defaults;
//   * LogLevel + is_verbose/show_progress;
//   * CommandLineArguments (the pure-logic field subset Options::load reads:
//     --no-save, --production, --frozen-lockfile, --save-dev via omit/config,
//     --dry-run, --force, --exact, --ignore-scripts, --yarn, ...);
//   * Options::load — the full flag-interaction state machine (bunfig config →
//     env → CLI → cross-flag fixups such as production ⇒ frozen-lockfile and
//     frozen-lockfile ⇒ never save), and scope_for_package_name with the
//     hash-collision-safe scoped-registry lookup.
//
// Seams (documented):
//   * env access is injected as an `EnvGetter` callback (bun_dotenv::Loader);
//   * filesystem/global-dir opening (open_global_dir / open_global_bin_dir)
//     is NOT ported — directory I/O belongs to the execution layer;
//   * `Output::stderr_descriptor_type()` TTY probe is injected as a bool
//     (`stderr_is_terminal`) on LoadEnvironment.
export module mbun.install.package_manager.options;

import std;
import mbun.install.npm.registry;
import mbun.install.network_task;

namespace mbun::install::package_manager {

namespace registry = mbun::install::npm::registry;
namespace net = mbun::install::network_task;

// ── Subcommand ───────────────────────────────────────────────────────────────
export enum class Subcommand : std::uint8_t {
    Install,
    Update,
    Pm,
    Add,
    Remove,
    Link,
    Unlink,
    Patch,
    PatchCommit,
    Outdated,
    Pack,
    Publish,
    Audit,
    Info,
    Why,
    Scan,
};

export constexpr bool can_globally_install_packages(Subcommand s) {
    return s == Subcommand::Install || s == Subcommand::Update || s == Subcommand::Add;
}

export constexpr bool supports_workspace_filtering(Subcommand s) {
    return s == Subcommand::Outdated || s == Subcommand::Install || s == Subcommand::Update;
}

export constexpr bool supports_json_output(Subcommand s) {
    return s == Subcommand::Audit || s == Subcommand::Pm || s == Subcommand::Info;
}

export constexpr bool should_chdir_to_root(Subcommand s) {
    return s != Subcommand::Link;
}

export constexpr std::string_view subcommand_name(Subcommand s) {
    switch (s) {
        case Subcommand::Install: return "install";
        case Subcommand::Update: return "update";
        case Subcommand::Pm: return "pm";
        case Subcommand::Add: return "add";
        case Subcommand::Remove: return "remove";
        case Subcommand::Link: return "link";
        case Subcommand::Unlink: return "unlink";
        case Subcommand::Patch: return "patch";
        case Subcommand::PatchCommit: return "patch-commit";
        case Subcommand::Outdated: return "outdated";
        case Subcommand::Pack: return "pack";
        case Subcommand::Publish: return "publish";
        case Subcommand::Audit: return "audit";
        case Subcommand::Info: return "info";
        case Subcommand::Why: return "why";
        case Subcommand::Scan: return "scan";
    }
    return "";
}

// ── WorkspaceFilter ──────────────────────────────────────────────────────────
export struct WorkspaceFilter {
    enum class Kind : std::uint8_t { All, Name, Path } kind{Kind::All};
    std::string pattern;

    // Port of WorkspaceFilter::init. Path filters (leading '.') are resolved
    // against `cwd` with a lexical posix join; `!` prefixes toggle negation and
    // survive as a single leading '!' on the stored pattern.
    static WorkspaceFilter init(std::string_view input, std::string_view cwd) {
        if ((input.size() == 1 && input[0] == '*') || input == "**") {
            return {Kind::All, {}};
        }
        std::string_view remain{input};
        bool prepend_negate{false};
        while (!remain.empty() && remain.front() == '!') {
            prepend_negate = !prepend_negate;
            remain.remove_prefix(1);
        }
        bool is_path{!remain.empty() && remain.front() == '.'};

        std::string filter;
        if (is_path) {
            filter = lexical_posix_join(cwd, remain);
            while (filter.size() > 1 && filter.back() == '/') {
                filter.pop_back();
            }
        } else {
            filter.assign(remain);
        }
        if (filter.empty()) {
            return {Kind::Path, {}};  // won't match anything
        }
        std::string pattern;
        if (prepend_negate) {
            pattern.push_back('!');
        }
        pattern.append(filter);
        return {is_path ? Kind::Path : Kind::Name, std::move(pattern)};
    }

    // Lexical `join_abs_string_buf::<platform::Posix>` equivalent: join then
    // resolve "." / ".." segments. Pure string logic, no filesystem access.
    static std::string lexical_posix_join(std::string_view base, std::string_view rel) {
        std::vector<std::string_view> parts;
        auto push_segments{[&parts](std::string_view p) {
            std::size_t i{0};
            while (i <= p.size()) {
                std::size_t j{p.find('/', i)};
                if (j == std::string_view::npos) {
                    j = p.size();
                }
                std::string_view seg{p.substr(i, j - i)};
                if (seg == "..") {
                    if (!parts.empty()) {
                        parts.pop_back();
                    }
                } else if (!seg.empty() && seg != ".") {
                    parts.push_back(seg);
                }
                i = j + 1;
            }
        }};
        push_segments(base);
        push_segments(rel);
        std::string out;
        for (std::string_view seg : parts) {
            out.push_back('/');
            out.append(seg);
        }
        if (out.empty()) {
            out.push_back('/');
        }
        return out;
    }
};

export struct PackageUpdateInfo {
    std::string original_version_literal;
    bool is_alias{false};
    std::string original_version_string_buf;
    std::optional<std::string> original_version;  // canonical semver string
};

// ── Features (bun_install_types::resolver_hooks::Features) ──────────────────
export struct Features {
    bool dependencies{true};
    bool dev_dependencies{false};
    bool is_main{false};
    bool optional_dependencies{false};
    bool peer_dependencies{true};
    bool trusted_dependencies{false};
    bool workspaces{false};
    bool patched_dependencies{false};
    bool check_for_duplicate_dependencies{false};
};

export enum class NodeLinker : std::uint8_t { Auto, Hoisted, Isolated };

export enum class LogLevel : std::uint8_t {
    Default,
    Verbose,
    Silent,
    Quiet,
    DefaultNoProgress,
    VerboseNoProgress,
};

export constexpr bool log_level_is_verbose(LogLevel l) {
    return l == LogLevel::Verbose || l == LogLevel::VerboseNoProgress;
}

export constexpr bool log_level_show_progress(LogLevel l) {
    return l == LogLevel::Default || l == LogLevel::Verbose;
}

// ── Do / Enable bitflags ─────────────────────────────────────────────────────
// Exact bit layout of PackageManagerOptions.rs (kept for lockfile/debug parity).
export struct Do {
    std::uint16_t bits{DEFAULT};

    static constexpr std::uint16_t SAVE_LOCKFILE{1 << 0};
    static constexpr std::uint16_t LOAD_LOCKFILE{1 << 1};
    static constexpr std::uint16_t INSTALL_PACKAGES{1 << 2};
    static constexpr std::uint16_t WRITE_PACKAGE_JSON{1 << 3};
    static constexpr std::uint16_t RUN_SCRIPTS{1 << 4};
    static constexpr std::uint16_t SAVE_YARN_LOCK{1 << 5};
    static constexpr std::uint16_t PRINT_META_HASH_STRING{1 << 6};
    static constexpr std::uint16_t VERIFY_INTEGRITY{1 << 7};
    static constexpr std::uint16_t SUMMARY{1 << 8};
    static constexpr std::uint16_t TRUST_DEPENDENCIES_FROM_ARGS{1 << 9};
    static constexpr std::uint16_t UPDATE_TO_LATEST{1 << 10};
    static constexpr std::uint16_t ANALYZE{1 << 11};
    static constexpr std::uint16_t RECURSIVE{1 << 12};
    static constexpr std::uint16_t PREFETCH_RESOLVED_TARBALLS{1 << 13};

    static constexpr std::uint16_t DEFAULT{SAVE_LOCKFILE | LOAD_LOCKFILE | INSTALL_PACKAGES |
                                           WRITE_PACKAGE_JSON | RUN_SCRIPTS | VERIFY_INTEGRITY |
                                           SUMMARY | PREFETCH_RESOLVED_TARBALLS};

    constexpr bool contains(std::uint16_t flag) const {
        return (bits & flag) != 0;
    }
    constexpr void set(std::uint16_t flag, bool v) {
        if (v) {
            bits |= flag;
        } else {
            bits &= static_cast<std::uint16_t>(~flag);
        }
    }
    constexpr void remove(std::uint16_t flag) {
        set(flag, false);
    }
};

export struct Enable {
    std::uint16_t bits{DEFAULT};

    static constexpr std::uint16_t MANIFEST_CACHE{1 << 0};
    static constexpr std::uint16_t MANIFEST_CACHE_CONTROL{1 << 1};
    static constexpr std::uint16_t CACHE{1 << 2};
    static constexpr std::uint16_t FAIL_EARLY{1 << 3};
    static constexpr std::uint16_t FROZEN_LOCKFILE{1 << 4};
    // Don't save the lockfile unless there were actual changes, unless...
    static constexpr std::uint16_t FORCE_SAVE_LOCKFILE{1 << 5};
    static constexpr std::uint16_t FORCE_INSTALL{1 << 6};
    static constexpr std::uint16_t EXACT_VERSIONS{1 << 7};
    static constexpr std::uint16_t ONLY_MISSING{1 << 8};
    static constexpr std::uint16_t GLOBAL_VIRTUAL_STORE{1 << 9};

    static constexpr std::uint16_t DEFAULT{MANIFEST_CACHE | MANIFEST_CACHE_CONTROL | CACHE};

    constexpr bool contains(std::uint16_t flag) const {
        return (bits & flag) != 0;
    }
    constexpr void set(std::uint16_t flag, bool v) {
        if (v) {
            bits |= flag;
        } else {
            bits &= static_cast<std::uint16_t>(~flag);
        }
    }
};

export struct Update {
    bool development{false};
    bool optional{false};
    bool peer{false};
};

export enum class Access : std::uint8_t { Public, Restricted };

export std::optional<Access> access_from_str(std::string_view s) {
    if (s == "public") {
        return Access::Public;
    }
    if (s == "restricted") {
        return Access::Restricted;
    }
    return std::nullopt;
}

export enum class AuthType : std::uint8_t { Legacy, Web };

export std::optional<AuthType> auth_type_from_str(std::string_view s) {
    if (s == "legacy") {
        return AuthType::Legacy;
    }
    if (s == "web") {
        return AuthType::Web;
    }
    return std::nullopt;
}

// ── CommandLineArguments (pure-logic field subset Options::load consumes) ───
export struct Omit {
    bool dev{false};
    bool optional{false};
    bool peer{false};
};

export struct CommandLineArguments {
    std::string registry;
    std::string token;
    std::optional<std::string> cache_dir;
    std::vector<std::string> positionals;

    bool analyze{false};
    bool only_missing{false};
    bool exact{false};
    bool no_save{false};
    bool dry_run{false};
    bool no_summary{false};
    bool silent{false};
    bool quiet{false};
    bool verbose{false};
    bool no_progress{false};
    bool no_cache{false};
    bool no_verify{false};
    bool ignore_scripts{false};
    bool trusted{false};
    bool yarn{false};
    bool production{false};
    bool frozen_lockfile{false};
    bool force{false};
    bool development{false};
    bool optional{false};
    bool peer{false};
    bool latest{false};
    bool recursive{false};
    bool lockfile_only{false};
    bool json_output{false};

    std::optional<Omit> omit;
    std::optional<bool> save_text_lockfile;
    std::optional<double> minimum_release_age_ms;
    std::optional<NodeLinker> node_linker;
};

// bunfig `[install]` config subset (Api::BunInstall fields Options::load reads).
export struct BunInstallConfig {
    std::optional<std::string> default_registry_url;
    std::optional<std::string> default_registry_token;
    // scope name -> (url, token); empty url falls back to the default registry.
    std::vector<std::tuple<std::string, std::string, std::string>> scoped;
    std::optional<bool> link_workspace_packages;
    std::optional<std::string> cache_directory;
    std::optional<NodeLinker> node_linker;
    std::optional<bool> global_store;
    std::optional<std::string> security_scanner;
    std::optional<bool> disable_cache;
    std::optional<bool> disable_manifest_cache;
    std::optional<bool> force;
    std::optional<bool> save_yarn_lockfile;
    std::optional<bool> save_lockfile;
    std::optional<bool> save_dev;
    std::optional<bool> save_optional;
    std::optional<bool> save_peer;
    std::optional<bool> exact;
    std::optional<bool> production;
    std::optional<bool> frozen_lockfile;
    std::optional<bool> save_text_lockfile;
    std::optional<std::uint32_t> concurrent_scripts;
    std::optional<bool> ignore_scripts;
    std::optional<double> minimum_release_age_ms;
    std::optional<std::string> global_dir;
};

// Injected environment (bun_dotenv::Loader seam).
export struct LoadEnvironment {
    using Getter = std::function<std::optional<std::string_view>(std::string_view)>;
    Getter get{[](std::string_view) { return std::optional<std::string_view>{}; }};
    bool is_ci{false};
    bool stderr_is_terminal{true};
};

// ── Options ──────────────────────────────────────────────────────────────────
export struct Options {
    LogLevel log_level{LogLevel::Default};
    bool global{false};

    std::string explicit_global_directory;
    std::string bin_path{"node_modules/.bin"};

    bool did_override_default_scope{false};
    registry::Scope scope;
    std::unordered_map<std::uint64_t, registry::Scope> registries;
    std::string cache_directory;
    Enable enable;
    Do do_;
    std::vector<std::string> positionals;
    Update update;
    bool dry_run{false};
    bool link_workspace_packages{true};
    Features remote_package_features{.optional_dependencies = true};
    Features local_package_features{
        .dev_dependencies = true, .optional_dependencies = true, .workspaces = true};

    bool json_output{false};

    std::uint16_t max_retry_count{5};
    std::size_t min_simultaneous_requests{4};
    std::size_t max_concurrent_lifecycle_scripts{0};

    std::optional<bool> save_text_lockfile;
    bool lockfile_only{false};

    NodeLinker node_linker{NodeLinker::Auto};
    std::optional<std::string> security_scanner;
    std::optional<double> minimum_release_age_ms;

    // Also observed by NetworkTask::for_manifest via verbose_install; kept as
    // an out-param on load (the reference writes a process-global).
    bool verbose_install{false};

    bool should_print_command_name() const {
        return log_level != LogLevel::Silent && do_.contains(Do::SUMMARY);
    }

    // Port of Options::scope_for_package_name — collision-safe scoped lookup:
    // a different scope whose hash collides must not inherit this scope's
    // registry or token.
    const registry::Scope& scope_for_package_name(std::string_view name) const {
        if (name.empty() || name.front() != '@') {
            return scope;
        }
        std::string_view scope_name{registry::get_name(name)};
        auto it{registries.find(registry::string_hash(scope_name))};
        if (it != registries.end() && it->second.name == scope_name) {
            return it->second;
        }
        return scope;
    }

    // Port of Options::load. Order matters: bunfig config → env → CLI →
    // cross-flag fixups.
    void load(const LoadEnvironment& env, const CommandLineArguments* maybe_cli,
              const BunInstallConfig* config, Subcommand subcommand) {
        std::string base_url{registry::DEFAULT_URL};
        std::string base_token;
        if (config != nullptr) {
            if (config->default_registry_url && !config->default_registry_url->empty()) {
                base_url = *config->default_registry_url;
            }
            if (config->default_registry_token) {
                base_token = *config->default_registry_token;
            }
            if (config->link_workspace_packages) {
                link_workspace_packages = *config->link_workspace_packages;
            }
        }
        scope = registry::Scope::for_url("", base_url);
        scope.token = base_token;

        if (config != nullptr) {
            if (config->cache_directory) {
                cache_directory = *config->cache_directory;
            }
            for (const auto& [name, url, token] : config->scoped) {
                registry::Scope s{registry::Scope::for_url(name, url.empty() ? base_url : url)};
                s.token = token;
                registries.insert_or_assign(registry::string_hash(name), std::move(s));
            }
            if (config->node_linker) {
                node_linker = *config->node_linker;
            }
            if (config->global_store) {
                enable.set(Enable::GLOBAL_VIRTUAL_STORE, *config->global_store);
            }
            if (config->security_scanner) {
                security_scanner = *config->security_scanner;
                do_.set(Do::PREFETCH_RESOLVED_TARBALLS, false);
            }
            if (config->disable_cache.value_or(false)) {
                enable.set(Enable::CACHE, false);
            }
            if (config->disable_manifest_cache.value_or(false)) {
                enable.set(Enable::MANIFEST_CACHE, false);
            }
            if (config->force.value_or(false)) {
                enable.set(Enable::MANIFEST_CACHE_CONTROL, false);
                enable.set(Enable::FORCE_INSTALL, true);
            }
            if (config->save_yarn_lockfile.value_or(false)) {
                do_.set(Do::SAVE_YARN_LOCK, true);
            }
            if (config->save_lockfile) {
                do_.set(Do::SAVE_LOCKFILE, *config->save_lockfile);
                enable.set(Enable::FORCE_SAVE_LOCKFILE, true);
            }
            if (config->save_dev) {
                // remote packages should never install dev dependencies
                local_package_features.dev_dependencies = *config->save_dev;
            }
            if (config->save_optional) {
                remote_package_features.optional_dependencies = *config->save_optional;
                local_package_features.optional_dependencies = *config->save_optional;
            }
            if (config->save_peer) {
                remote_package_features.peer_dependencies = *config->save_peer;
                local_package_features.peer_dependencies = *config->save_peer;
            }
            if (config->exact) {
                enable.set(Enable::EXACT_VERSIONS, *config->exact);
            }
            if (config->production.value_or(false)) {
                local_package_features.dev_dependencies = false;
                enable.set(Enable::FAIL_EARLY, true);
                enable.set(Enable::FROZEN_LOCKFILE, true);
                enable.set(Enable::FORCE_SAVE_LOCKFILE, false);
            }
            if (config->frozen_lockfile.value_or(false)) {
                enable.set(Enable::FROZEN_LOCKFILE, true);
            }
            if (config->save_text_lockfile) {
                save_text_lockfile = *config->save_text_lockfile;
            }
            if (config->concurrent_scripts) {
                max_concurrent_lifecycle_scripts = *config->concurrent_scripts;
            }
            if (config->ignore_scripts.value_or(false)) {
                do_.set(Do::RUN_SCRIPTS, false);
            }
            if (config->minimum_release_age_ms) {
                minimum_release_age_ms = *config->minimum_release_age_ms;
            }
            if (config->global_dir) {
                explicit_global_directory = *config->global_dir;
            }
        }

        if (auto val{env.get("BUN_INSTALL_GLOBAL_STORE")}) {
            enable.set(Enable::GLOBAL_VIRTUAL_STORE, *val != "0");
        }

        bool default_disable_progress_bar{[&] {
            if (auto prog{env.get("BUN_INSTALL_PROGRESS")}) {
                return *prog == "0";
            }
            if (env.is_ci) {
                return true;
            }
            return !env.stderr_is_terminal;
        }()};

        // technically, npm_config is case in-sensitive
        {
            constexpr std::string_view REGISTRY_KEYS[3]{
                "BUN_CONFIG_REGISTRY", "NPM_CONFIG_REGISTRY", "npm_config_registry"};
            for (std::string_view key : REGISTRY_KEYS) {
                auto reg{env.get(key)};
                if (!reg || reg->empty() ||
                    (!reg->starts_with("https://") && !reg->starts_with("http://"))) {
                    continue;
                }
                // Keep the previous token only when the host matches and the
                // scheme did not downgrade (https → http drops credentials).
                net::UrlParts new_url{net::parse_url(*reg)};
                net::UrlParts prev_url{net::parse_url(scope.url)};
                bool new_https{new_url.protocol == "https"};
                bool prev_https{prev_url.protocol == "https"};
                std::string token{
                    new_url.hostname == prev_url.hostname && (new_https || !prev_https)
                        ? scope.token
                        : std::string{}};
                scope = registry::Scope::for_url("", *reg);
                scope.token = std::move(token);
                break;
            }
        }
        {
            constexpr std::string_view TOKEN_KEYS[3]{"BUN_CONFIG_TOKEN", "NPM_CONFIG_TOKEN",
                                                     "npm_config_token"};
            for (std::string_view key : TOKEN_KEYS) {
                auto tok{env.get(key)};
                if (tok && !tok->empty()) {
                    scope.token = std::string{*tok};
                    break;
                }
            }
        }

        if (env.get("BUN_CONFIG_YARN_LOCKFILE")) {
            do_.set(Do::SAVE_YARN_LOCK, true);
        }
        if (auto retry{env.get("BUN_CONFIG_HTTP_RETRY_COUNT")}) {
            std::uint16_t parsed{};
            auto [p, ec]{std::from_chars(retry->data(), retry->data() + retry->size(), parsed)};
            if (ec == std::errc{} && p == retry->data() + retry->size()) {
                max_retry_count = parsed;
            }
        }
        if (auto v{env.get("BUN_CONFIG_SKIP_SAVE_LOCKFILE")}) {
            do_.set(Do::SAVE_LOCKFILE, *v == "0");
        }
        if (auto v{env.get("BUN_CONFIG_SKIP_LOAD_LOCKFILE")}) {
            do_.set(Do::LOAD_LOCKFILE, *v == "0");
        }
        if (auto v{env.get("BUN_CONFIG_SKIP_INSTALL_PACKAGES")}) {
            do_.set(Do::INSTALL_PACKAGES, *v == "0");
        }
        if (auto v{env.get("BUN_CONFIG_NO_VERIFY")}) {
            do_.set(Do::VERIFY_INTEGRITY, *v != "0");
        }

        // Update should never read from manifest cache
        if (subcommand == Subcommand::Update) {
            enable.set(Enable::MANIFEST_CACHE, false);
            enable.set(Enable::MANIFEST_CACHE_CONTROL, false);
        }

        if (maybe_cli != nullptr) {
            const CommandLineArguments& cli{*maybe_cli};
            do_.set(Do::ANALYZE, cli.analyze);
            enable.set(Enable::ONLY_MISSING, cli.only_missing || cli.analyze);

            if (!cli.registry.empty()) {
                scope.url = cli.registry;
                scope.url_hash = registry::string_hash(registry::without_trailing_slash(scope.url));
            }
            if (cli.cache_dir) {
                cache_directory = *cli.cache_dir;
            }
            if (cli.exact) {
                enable.set(Enable::EXACT_VERSIONS, true);
            }
            if (!cli.token.empty()) {
                scope.token = cli.token;
            }
            if (cli.no_save) {
                do_.set(Do::SAVE_LOCKFILE, false);
                do_.set(Do::WRITE_PACKAGE_JSON, false);
            }
            if (cli.dry_run) {
                do_.set(Do::INSTALL_PACKAGES, false);
                dry_run = true;
                do_.set(Do::WRITE_PACKAGE_JSON, false);
                do_.set(Do::SAVE_LOCKFILE, false);
            }
            if (cli.no_summary || cli.silent) {
                do_.set(Do::SUMMARY, false);
            }
            json_output = cli.json_output;
            if (cli.no_cache) {
                enable.set(Enable::MANIFEST_CACHE, false);
                enable.set(Enable::MANIFEST_CACHE_CONTROL, false);
            }
            if (cli.omit) {
                if (cli.omit->dev) {
                    local_package_features.dev_dependencies = false;
                }
                if (cli.omit->optional) {
                    local_package_features.optional_dependencies = false;
                    remote_package_features.optional_dependencies = false;
                }
                if (cli.omit->peer) {
                    local_package_features.peer_dependencies = false;
                    remote_package_features.peer_dependencies = false;
                }
            }
            if (cli.ignore_scripts) {
                do_.set(Do::RUN_SCRIPTS, false);
            }
            if (cli.trusted) {
                do_.set(Do::TRUST_DEPENDENCIES_FROM_ARGS, true);
            }
            if (cli.save_text_lockfile) {
                save_text_lockfile = *cli.save_text_lockfile;
            }
            if (cli.minimum_release_age_ms) {
                minimum_release_age_ms = *cli.minimum_release_age_ms;
            }
            lockfile_only = cli.lockfile_only;
            if (cli.lockfile_only) {
                do_.set(Do::PREFETCH_RESOLVED_TARBALLS, false);
            }
            if (cli.node_linker) {
                node_linker = *cli.node_linker;
            }

            bool disable_progress_bar{default_disable_progress_bar || cli.no_progress};
            if (cli.verbose) {
                log_level =
                    disable_progress_bar ? LogLevel::VerboseNoProgress : LogLevel::Verbose;
                verbose_install = true;
            } else if (cli.silent) {
                log_level = LogLevel::Silent;
                verbose_install = false;
            } else if (cli.quiet) {
                log_level = LogLevel::Quiet;
                verbose_install = false;
            } else {
                log_level =
                    disable_progress_bar ? LogLevel::DefaultNoProgress : LogLevel::Default;
                verbose_install = false;
            }

            if (cli.no_verify) {
                do_.set(Do::VERIFY_INTEGRITY, false);
            }
            if (cli.yarn) {
                do_.set(Do::SAVE_YARN_LOCK, true);
            }

            do_.set(Do::UPDATE_TO_LATEST, cli.latest);
            do_.set(Do::RECURSIVE, cli.recursive);

            if (!cli.positionals.empty()) {
                positionals = cli.positionals;
            }
            if (cli.production) {
                local_package_features.dev_dependencies = false;
                enable.set(Enable::FAIL_EARLY, true);
                enable.set(Enable::FROZEN_LOCKFILE, true);
            }
            if (cli.frozen_lockfile) {
                enable.set(Enable::FROZEN_LOCKFILE, true);
            }
            if (cli.force) {
                enable.set(Enable::MANIFEST_CACHE_CONTROL, false);
                enable.set(Enable::FORCE_INSTALL, true);
                enable.set(Enable::FORCE_SAVE_LOCKFILE, true);
            }
            if (cli.development) {
                update.development = true;
            } else if (cli.optional) {
                update.optional = true;
            } else if (cli.peer) {
                update.peer = true;
            }
        } else {
            log_level =
                default_disable_progress_bar ? LogLevel::DefaultNoProgress : LogLevel::Default;
            verbose_install = false;
        }

        // If the lockfile is frozen, don't save it to disk.
        if (enable.contains(Enable::FROZEN_LOCKFILE)) {
            do_.set(Do::SAVE_LOCKFILE, false);
            enable.set(Enable::FORCE_SAVE_LOCKFILE, false);
        }

        did_override_default_scope = scope.url_hash != registry::DEFAULT_URL_HASH;
    }
};

}  // namespace mbun::install::package_manager
