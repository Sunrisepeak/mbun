// command.cppm — filesystem execution layer for the `mbun install` command.
//
// This is the real vertical slice of Bun's PackageManager path:
//   ref: bun-ref/src/runtime/cli/install_command.rs install_with_cli
//   ref: bun-ref/src/install/PackageManager/install_with_manager.rs
//   ref: bun-ref/src/install/resolvers/folder_resolver.rs
//   ref: bun-ref/src/install/PackageInstall.rs (folder hardlink/copy install)
//
// Folder (file:) dependencies install via hardlink/copy clone; registry
// dependencies (npm range / dist-tag / remote tarball) install via the
// registry_install pipeline (manifest → resolve → tarball → verify → extract
// → bin links → transitive deps). Lifecycle scripts, workspaces, local
// tarballs and binary bun.lockb are explicit errors — never silently skipped.
export module mbun.install.command;

import std;
import mbun.bunfig;
import mbun.install;
import mbun.install.async_http;
import mbun.install.dependency;
import mbun.install.lockfile;
import mbun.install.lockfile.text_writer;
import mbun.install.lockfile_pins;
import mbun.install.npm.json;
import mbun.install.override_map;
import mbun.install.package_manager.options;
import mbun.install.registry_install;
import mbun.install.workspace_map;
import mbun.toml;

namespace mbun::install::command {

export enum class ErrorCode : std::uint8_t {
    PackageJsonNotFound,
    PackageJsonReadFailed,
    InvalidPackageJson,
    InvalidDependency,
    UnsafePackageName,
    MissingDependencyPackageJson,
    InvalidDependencyPackageJson,
    LockfileReadFailed,
    InvalidTextLockfile,
    LockfileOutOfDate,
    UnsupportedRegistryDependency,
    UnsupportedLocalTarball,
    UnsupportedRemoteDependency,
    UnsupportedWorkspaceDependency,
    UnsupportedLifecycleScripts,
    InvalidBinaryLockfile,
    ManifestFetchFailed,
    ManifestParseFailed,
    NoMatchingVersion,
    TarballFetchFailed,
    IntegrityCheckFailed,
    ExtractFailed,
    IoError,
};

export struct InstallError {
    ErrorCode code{ErrorCode::IoError};
    std::string message;
};

export struct InstallOptions {
    bool frozenLockfile{false};
    bool ignoreScripts{false};
    bool noProgress{false};
    // --lockfile-only: resolve and save the lockfile without installing
    // (install_with_manager.rs:754-765 save_lockfile_only).
    bool lockfileOnly{false};
    // --save-text-lockfile: force the text bun.lock save format. Text is
    // already mbun's only save format, so this is accepted for CLI parity.
    bool saveTextLockfile{false};
    // --force: bun re-resolves latest versions and reinstalls. mbun keeps no
    // manifest cache yet, so every install already behaves force-fresh; the
    // flag routes into CommandLineArguments::force for Options::load parity.
    bool force{false};
    // --registry <url> override. Empty → BUN_CONFIG_REGISTRY / NPM_CONFIG_REGISTRY
    // env, else the default https://registry.npmjs.org/ (Options::load order).
    std::string registry;
    // --network-concurrency <NUM>: max concurrent network requests, max(n, 1)
    // (PackageManager.rs:2200-2211). Unset → BUN_CONFIG_MAX_HTTP_REQUESTS, else
    // the install default of 64. Nothing sets this yet — the flag itself lives
    // in app/cli's parser (bun's help text for it says "default 48" while the
    // constant is 64; that is a bug in bun's docs, so port both verbatim).
    std::optional<std::size_t> networkConcurrency{};
};

export struct InstallSummary {
    std::filesystem::path root;
    std::size_t installed{0};
    bool loaded_text_lockfile{false};
    // "name@version" for every registry package installed, in install order
    // (registry_install::Summary::packages). `mbun add` reads the concrete
    // version back out of this to write `^<version>` into package.json — the
    // seam standing in for bun's post-install re-edit, which reads the
    // resolution out of the lockfile instead
    //   (ref: bun-ref/src/install/PackageManager/PackageJSONEditor.rs:1204-1226,
    //    `resolutions[request.package_id].npm().version`).
    std::vector<std::string> packages;
    // bun prints "Saved lockfile" (stderr) when it writes bun.lock, and
    // `--lockfile-only` prints "Saved bun.lock (N packages)" where N counts
    // the root package too (install_with_manager.rs:1767-1782).
    bool savedLockfile{false};
    std::size_t lockPackageCount{0};
};

export using InstallResult = std::expected<InstallSummary, InstallError>;

namespace detail {

using JsonDocument = mbun::install::npm::json::Document;
using JsonValue = mbun::install::npm::json::Value;

struct DependencyPlan {
    std::string alias;
    std::string specifier;
    std::filesystem::path source;
    std::filesystem::path destination;
};

enum class DependencyKind : std::uint8_t { Production, Development, Optional, Peer };

// Neither an optionalDependencies edge nor a peer edge may fail the install. For
// peers that is bun's behavior for *every* peer it resolves: the
// failed-resolution report skips them wholesale
// (PackageManagerResolution.rs:370-374 `if failed_dep.behavior.is_peer() {
// continue; }`, ahead of the `is_optional()` check, with a TODO noting that
// erroring on required peers is a future lockfile-rewrite change).
bool tolerates_failure(DependencyKind kind) {
    return kind == DependencyKind::Optional || kind == DependencyKind::Peer;
}

// The root's edges carry real `Behavior` bits into the graph, because the
// hoister sorts on them: `DepSorter` (lockfile.rs:228-245) visits a package's
// edges in `Behavior::cmp` order — workspace < dev < optional < prod < peer
// (resolver_hooks.rs:266-288) — and first visit wins the hoisted slot. The root
// is the one package that legitimately holds `dev` edges, and
// `hoist_dependency`'s AS_DEFINED branch keys off exactly that
// (`dep.behavior.is_dev() != dependency.behavior.is_dev()`, Tree.rs:1047-1054),
// so mislabelling them changes where packages land.
mbun::install::dependency::Behavior behavior_for(DependencyKind kind) {
    using Behavior = mbun::install::dependency::Behavior;
    switch (kind) {
        case DependencyKind::Development:
            return Behavior{Behavior::DEV};
        case DependencyKind::Optional:
            return Behavior{Behavior::OPTIONAL};
        case DependencyKind::Peer:
            return Behavior{Behavior::PEER};
        case DependencyKind::Production:
            break;
    }
    return Behavior{Behavior::PROD};
}

struct RootDependency {
    std::string specifier;
    DependencyKind kind{DependencyKind::Production};
};

struct LoadedPackageJson {
    std::unique_ptr<std::string> source;
    JsonDocument document;
};

std::unexpected<InstallError> fail(ErrorCode code, std::string message) {
    return std::unexpected(InstallError{code, std::move(message)});
}

std::expected<std::string, InstallError> read_file(const std::filesystem::path& path,
                                                   ErrorCode code,
                                                   std::string_view description) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return fail(code, std::format("failed to read {}: {}", description, path.string()));
    }
    std::string contents{std::istreambuf_iterator<char>{stream},
                         std::istreambuf_iterator<char>{}};
    if (!stream.eof() && stream.fail()) {
        return fail(code, std::format("failed to read {}: {}", description, path.string()));
    }
    return contents;
}

std::expected<std::filesystem::path, InstallError>
find_package_root(std::filesystem::path startDirectory) {
    std::error_code ec;
    startDirectory = std::filesystem::absolute(startDirectory, ec);
    if (ec) {
        return fail(ErrorCode::PackageJsonNotFound,
                    std::format("failed to resolve working directory: {}", ec.message()));
    }
    if (!std::filesystem::is_directory(startDirectory, ec)) {
        startDirectory = startDirectory.parent_path();
    }
    for (std::filesystem::path current{startDirectory}; !current.empty();) {
        if (std::filesystem::is_regular_file(current / "package.json", ec)) {
            return current;
        }
        const std::filesystem::path parent{current.parent_path()};
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return fail(ErrorCode::PackageJsonNotFound, "could not find package.json");
}

std::expected<LoadedPackageJson, InstallError>
load_package_json(const std::filesystem::path& path, bool dependency) {
    auto source{read_file(path,
                          dependency ? ErrorCode::MissingDependencyPackageJson
                                     : ErrorCode::PackageJsonReadFailed,
                          "package.json")};
    if (!source) {
        return std::unexpected(source.error());
    }
    auto stableSource{std::make_unique<std::string>(std::move(*source))};
    auto document{mbun::install::npm::json::parse(*stableSource)};
    if (!document || !document->root || !document->root->is_object()) {
        return fail(dependency ? ErrorCode::InvalidDependencyPackageJson
                               : ErrorCode::InvalidPackageJson,
                    std::format("failed to parse package.json: {}", path.string()));
    }
    return LoadedPackageJson{std::move(stableSource), std::move(*document)};
}

bool has_lifecycle_scripts(const JsonValue& root) {
    const JsonValue* scripts{root.get("scripts")};
    if (scripts == nullptr || !scripts->is_object()) {
        return false;
    }
    constexpr std::array LIFECYCLE_NAMES{
        std::string_view{"preinstall"},  std::string_view{"install"},
        std::string_view{"postinstall"}, std::string_view{"preprepare"},
        std::string_view{"prepare"},     std::string_view{"postprepare"}};
    return std::ranges::any_of(LIFECYCLE_NAMES, [&](std::string_view name) {
        const JsonValue* script{scripts->get(name)};
        return script != nullptr && script->as_str().has_value() && !script->str.empty();
    });
}

std::expected<void, InstallError>
append_dependency_map(const JsonValue* value, std::string_view field, DependencyKind kind,
                      std::map<std::string, RootDependency, std::less<>>& dependencies) {
    if (value == nullptr || !value->is_object()) {
        return {};
    }
    for (const auto& member : value->members) {
        if (!member.value) {
            return fail(ErrorCode::InvalidPackageJson,
                        std::format("package.json dependency '{}' in '{}' has no value",
                                    member.key, field));
        }
        auto specifier{member.value->as_str()};
        if (!specifier) {
            return fail(ErrorCode::InvalidPackageJson,
                        std::format("package.json dependency '{}' in '{}' must be a string",
                                    member.key, field));
        }
        dependencies.insert_or_assign(
            std::string{member.key}, RootDependency{std::string{*specifier}, kind});
    }
    return {};
}

// `peerDependencies`, with `peerDependenciesMeta` folded in. A peer whose name
// an earlier group already took is dropped in favour of that entry — bun:
// "Duplicate peer & dev dependencies are promoted to whichever appeared first"
// (lockfile/Package.rs:867-884), with the root-specific twin at
// PackageManagerEnqueue.rs:107-119 ("if dependency is peer and is going to be
// installed through 'dependencies', skip it"). `try_emplace` is what encodes
// that first-wins rule; `insert_or_assign` would invert it.
std::expected<void, InstallError>
append_peer_dependency_map(const JsonValue& root,
                           std::map<std::string, RootDependency, std::less<>>& dependencies) {
    const JsonValue* peers{root.get("peerDependencies")};
    if (peers != nullptr && !peers->is_object()) {
        return fail(ErrorCode::InvalidPackageJson,
                    "package.json field 'peerDependencies' must be an object");
    }
    const JsonValue* meta{root.get("peerDependenciesMeta")};
    if (meta != nullptr && !meta->is_object()) {
        return fail(ErrorCode::InvalidPackageJson,
                    "package.json field 'peerDependenciesMeta' must be an object");
    }
    if (peers == nullptr) {
        return {};
    }
    for (const auto& member : peers->members) {
        if (!member.value) {
            return fail(ErrorCode::InvalidPackageJson,
                        std::format("package.json dependency '{}' in 'peerDependencies' has no "
                                    "value",
                                    member.key));
        }
        auto specifier{member.value->as_str()};
        if (!specifier) {
            return fail(ErrorCode::InvalidPackageJson,
                        std::format("package.json dependency '{}' in 'peerDependencies' must be "
                                    "a string",
                                    member.key));
        }
        // `peerDependenciesMeta.optional` — an optional peer is never resolved
        // and never fetched, at the root as much as anywhere else: root deps
        // reach the same enqueue entry point that drops them
        // (processDependencyList.rs:376 → PackageManagerEnqueue.rs:666-668
        // `if dependency.behavior.is_optional_peer() { return Ok(()); }`). It
        // binds only to a package another edge already supplied, which in a flat
        // node_modules needs no edge of its own. That is also why there is no
        // meta-only synthesis here: bun's synthetic `"*"` entries
        // (lockfile/Package.rs:2949-2974) exist to carry the OPTIONAL bit, and an
        // optional peer is precisely what never gets installed.
        if (meta != nullptr) {
            if (const JsonValue* entry{meta->get(member.key)}) {
                if (const JsonValue* optional{entry->get("optional")}) {
                    auto flag{optional->as_bool()};
                    if (flag && *flag) {
                        continue;
                    }
                }
            }
        }
        dependencies.try_emplace(std::string{member.key},
                                 RootDependency{std::string{*specifier}, DependencyKind::Peer});
    }
    return {};
}

// The root parses with the hardcoded `Features::main()`
// (install_with_manager.rs:1584), which inherits `peer_dependencies: true` from
// `Features::base()` (resolver_hooks.rs:1302,1310) — so bun does install the
// root's own required peers, and `--omit=peer` (not `--no-peer`) is what turns
// them off (CommandLineArguments.rs:1123, PackageManagerOptions.rs:734-737;
// honouring it is DEFERRED with the flag itself). Group order is dependencies →
// dev → optional → peer, matching lockfile/Package.rs:2268-2286, and peer goes
// last because `append_peer_dependency_map` resolves duplicates in favour of
// whatever an earlier group already registered.
std::expected<std::map<std::string, RootDependency, std::less<>>, InstallError>
root_dependencies(const JsonValue& root) {
    std::map<std::string, RootDependency, std::less<>> dependencies;
    constexpr std::array fields{
        std::pair{std::string_view{"dependencies"}, DependencyKind::Production},
        std::pair{std::string_view{"devDependencies"}, DependencyKind::Development},
        std::pair{std::string_view{"optionalDependencies"}, DependencyKind::Optional}};
    for (const auto& [field, kind] : fields) {
        const JsonValue* value{root.get(field)};
        if (value != nullptr && !value->is_object()) {
            return fail(ErrorCode::InvalidPackageJson,
                        std::format("package.json field '{}' must be an object", field));
        }
        auto appended{append_dependency_map(value, field, kind, dependencies)};
        if (!appended) {
            return std::unexpected(appended.error());
        }
    }
    auto peers{append_peer_dependency_map(root, dependencies)};
    if (!peers) {
        return std::unexpected(peers.error());
    }
    return dependencies;
}

std::filesystem::path destination_for(std::filesystem::path root, std::string_view alias) {
    root /= "node_modules";
    if (alias.starts_with('@')) {
        const std::size_t slash{alias.find('/')};
        root /= std::string{alias.substr(0, slash)};
        root /= std::string{alias.substr(slash + 1)};
        return root;
    }
    root /= std::string{alias};
    return root;
}

// `binaryLockfile` is an out-param, not a local: the returned
// `install::Lockfile`'s string_views borrow the binary lockfile's string arena
// (as the text path's views borrow `lockSource`), so the caller must keep both
// alive for as long as it uses the result.
std::expected<std::optional<mbun::install::Lockfile>, InstallError>
load_lockfile(const std::filesystem::path& root, std::string& lockSource,
              std::optional<mbun::install::lockfile::Lockfile>& binaryLockfile) {
    const std::filesystem::path textPath{root / "bun.lock"};
    const std::filesystem::path binaryPath{root / "bun.lockb"};
    std::error_code ec;
    if (std::filesystem::exists(textPath, ec)) {
        auto source{read_file(textPath, ErrorCode::LockfileReadFailed, "bun.lock")};
        if (!source) {
            return std::unexpected(source.error());
        }
        lockSource = std::move(*source);
        auto parsed{mbun::install::parse_text(lockSource)};
        if (!parsed) {
            // The v1+/v2 strictness errors carry bun's exact messages
            // (bun.lock.rs parse_into_binary_lockfile); everything else keeps
            // the generic parse failure.
            switch (parsed.error()) {
                case mbun::install::ParseError::MissingIntegrityHash:
                    return fail(ErrorCode::InvalidTextLockfile,
                                "Missing integrity hash for npm package resolved to a tarball "
                                "URL outside the configured registry");
                case mbun::install::ParseError::InvalidGitDependencyTag:
                    return fail(ErrorCode::InvalidTextLockfile, "Invalid git dependency tag");
                case mbun::install::ParseError::MissingGitDependencyTag:
                    return fail(ErrorCode::InvalidTextLockfile, "Missing git dependency tag");
                default:
                    return fail(ErrorCode::InvalidTextLockfile,
                                std::format("failed to parse lockfile: {}", textPath.string()));
            }
        }
        return std::optional<mbun::install::Lockfile>{std::move(*parsed)};
    }
    if (std::filesystem::exists(binaryPath, ec)) {
        auto source{read_file(binaryPath, ErrorCode::LockfileReadFailed, "bun.lockb")};
        if (!source) {
            return std::unexpected(source.error());
        }
        lockSource = std::move(*source);
        const std::span<const std::uint8_t> bytes{
            reinterpret_cast<const std::uint8_t*>(lockSource.data()), lockSource.size()};
        // Checked-in bun.lockb files are format v2 far more often than v3 (bun
        // only writes v3 since the format bump, but the files committed to repos
        // predate it), so the v2 migration path is the common case, not a legacy
        // corner. ref: bun.lockb.rs:376-402.
        auto binary{mbun::install::lockfile::load(bytes)};
        if (!binary) {
            return fail(ErrorCode::InvalidBinaryLockfile,
                        std::format("failed to parse binary lockfile: {} ({})",
                                    binaryPath.string(),
                                    mbun::install::lockfile::binary_error_message(binary.error())));
        }
        binaryLockfile = std::move(*binary);
        return std::optional<mbun::install::Lockfile>{
            mbun::install::lockfile::to_install_lockfile(*binaryLockfile)};
    }
    return std::optional<mbun::install::Lockfile>{};
}

// One root dependency, classified: a folder (file:) source to clone, a
// workspace to symlink, or a registry edge for the registry_install pipeline.
struct ResolvedDependency {
    std::optional<std::filesystem::path> folderSource;
    std::optional<mbun::install::registry_install::PendingDep> registryDep;
    // posix path of the matched workspace, relative to the root.
    std::optional<std::string> workspaceRelPath;
};

std::expected<ResolvedDependency, InstallError>
resolve_dependency(const std::filesystem::path& root, std::string_view alias,
                   std::string_view specifier, bool optional,
                   const mbun::install::override_map::OverrideMap& overrides,
                   const std::vector<mbun::install::workspace_map::Entry>& workspaces) {
    auto parsed{mbun::install::dependency::parse(alias, specifier)};
    if (!parsed) {
        return fail(ErrorCode::InvalidDependency,
                    std::format("invalid dependency '{}': {}", alias, specifier));
    }
    using Tag = mbun::install::dependency::Tag;
    using PendingDep = mbun::install::registry_install::PendingDep;
    // The root's own direct edges are overridden too — bun's lookup is a bare
    // name hit in the one enqueue funnel every edge passes through, with no
    // depth condition (PackageManagerEnqueue.rs:714-750; the flat name-keyed map
    // is lockfile/OverrideMap.rs:22-26). A direct `npm:` alias is exempt, which
    // is the same gate `classify_transitive_` applies to deeper edges. An
    // override may retarget a `file:` root dep at the registry (or the reverse),
    // so this runs before the tag switch, not inside the Npm arm.
    if (!(parsed->tag == Tag::Npm && parsed->npm.is_alias)) {
        if (const std::string* replacement{overrides.get(alias)}) {
            specifier = *replacement;
            parsed = mbun::install::dependency::parse(alias, specifier);
            if (!parsed) {
                return fail(ErrorCode::InvalidDependency,
                            std::format("invalid override for '{}': {}", alias, specifier));
            }
        }
    }
    switch (parsed->tag) {
        case Tag::Folder: {
            std::error_code ec;
            std::filesystem::path source{
                std::filesystem::weakly_canonical(root / std::string{parsed->folder}, ec)};
            if (ec) {
                return fail(ErrorCode::MissingDependencyPackageJson,
                            std::format("failed to resolve file dependency '{}': {}", alias,
                                        ec.message()));
            }
            return ResolvedDependency{.folderSource = std::move(source)};
        }
        case Tag::Npm: {
            PendingDep dep{};
            dep.installName.assign(alias);
            dep.packageName.assign(parsed->npm.name.empty() ? alias : parsed->npm.name);
            dep.spec.assign(parsed->npm.version);
            dep.kind = PendingDep::Kind::Range;
            dep.optional = optional;
            return ResolvedDependency{.registryDep = std::move(dep)};
        }
        case Tag::DistTag: {
            PendingDep dep{};
            dep.installName.assign(alias);
            dep.packageName.assign(parsed->dist_tag.name.empty() ? alias
                                                                 : parsed->dist_tag.name);
            dep.spec.assign(parsed->dist_tag.tag);
            dep.kind = PendingDep::Kind::DistTag;
            dep.optional = optional;
            return ResolvedDependency{.registryDep = std::move(dep)};
        }
        case Tag::Tarball:
            if (parsed->tarball.uri.kind == mbun::install::dependency::URI::Kind::Local) {
                return fail(ErrorCode::UnsupportedLocalTarball,
                            std::format("local tarball dependency '{}' is not implemented yet", alias));
            }
            {
                PendingDep dep{};
                dep.installName.assign(alias);
                dep.packageName.assign(alias);
                dep.spec.assign(parsed->tarball.uri.value);
                dep.kind = PendingDep::Kind::TarballUrl;
                dep.optional = optional;
                return ResolvedDependency{.registryDep = std::move(dep)};
            }
        case Tag::Workspace: {
            // A `workspace:` edge binds by NAME to a package the root's
            // `workspaces` array registered (WorkspaceMap; the version part is
            // a protocol like `*`/`^`/`~` or a range against the workspace's
            // own version).
            const auto entry{std::ranges::find_if(
                workspaces, [&](const mbun::install::workspace_map::Entry& ws) {
                    return ws.name == alias;
                })};
            if (entry == workspaces.end()) {
                return fail(ErrorCode::UnsupportedWorkspaceDependency,
                            std::format("Workspace dependency \"{}\" not found", alias));
            }
            const std::string_view range{parsed->workspace};
            const bool protocolMatch{range.empty() || range == "*" || range == "^" ||
                                     range == "~"};
            if (!protocolMatch && !entry->version.empty() &&
                !mbun::install::Lockfile::satisfies(entry->version, range)) {
                return fail(ErrorCode::UnsupportedWorkspaceDependency,
                            std::format("No matching version for workspace dependency \"{}\"",
                                        alias));
            }
            return ResolvedDependency{.workspaceRelPath = entry->relPath};
        }
        case Tag::Git:
        case Tag::Github:
        case Tag::Symlink:
        case Tag::Catalog:
        case Tag::Uninitialized:
            return fail(ErrorCode::UnsupportedRemoteDependency,
                        std::format("dependency type for '{}' is not implemented yet", alias));
    }
    return fail(ErrorCode::InvalidDependency, std::format("invalid dependency '{}'", alias));
}

// Map a registry_install error into the command error space (message intact).
InstallError map_registry_error(mbun::install::registry_install::Error error) {
    using RegistryCode = mbun::install::registry_install::ErrorCode;
    ErrorCode code{ErrorCode::IoError};
    switch (error.code) {
        case RegistryCode::InvalidDependency: code = ErrorCode::InvalidDependency; break;
        case RegistryCode::ManifestFetchFailed: code = ErrorCode::ManifestFetchFailed; break;
        case RegistryCode::ManifestParseFailed: code = ErrorCode::ManifestParseFailed; break;
        case RegistryCode::NoMatchingVersion: code = ErrorCode::NoMatchingVersion; break;
        case RegistryCode::TarballFetchFailed: code = ErrorCode::TarballFetchFailed; break;
        case RegistryCode::IntegrityCheckFailed: code = ErrorCode::IntegrityCheckFailed; break;
        case RegistryCode::ExtractFailed: code = ErrorCode::ExtractFailed; break;
        case RegistryCode::UnsupportedDependency: code = ErrorCode::UnsupportedRemoteDependency; break;
        case RegistryCode::IoError: code = ErrorCode::IoError; break;
    }
    return InstallError{code, std::move(error.message)};
}

// Project bunfig.toml → the BunInstallConfig subset Options::load reads.
// Returns nullopt when there is no bunfig.toml; a present-but-unparseable
// file is a loud error (bun refuses to run on malformed bunfig).
std::expected<std::optional<mbun::install::package_manager::BunInstallConfig>, InstallError>
load_bunfig_config(const std::filesystem::path& root) {
    const std::filesystem::path path{root / "bunfig.toml"};
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return std::optional<mbun::install::package_manager::BunInstallConfig>{};
    }
    std::string source{std::istreambuf_iterator<char>{stream},
                       std::istreambuf_iterator<char>{}};
    auto tomlRoot{mbun::toml::parse(source)};
    if (!tomlRoot) {
        return fail(ErrorCode::InvalidPackageJson,
                    std::format("failed to parse bunfig.toml: {}", tomlRoot.error().message));
    }
    mbun::bunfig::Parser parser{mbun::bunfig::Command::Install};
    auto config{parser.parse(*tomlRoot)};
    if (!config) {
        return fail(ErrorCode::InvalidPackageJson,
                    std::format("failed to parse bunfig.toml: {}", config.error().message));
    }
    mbun::install::package_manager::BunInstallConfig out{};
    if (config->install) {
        const auto& install{*config->install};
        if (install.default_registry && !install.default_registry->url.empty()) {
            out.default_registry_url = install.default_registry->url;
            if (!install.default_registry->token.empty()) {
                out.default_registry_token = install.default_registry->token;
            }
        }
        for (const auto& scoped : install.scopes) {
            out.scoped.emplace_back(scoped.scope, scoped.registry.url, scoped.registry.token);
        }
        if (!install.cache_directory.empty()) {
            out.cache_directory = install.cache_directory;
        }
        if (install.disable_cache) {
            out.disable_cache = true;
        }
        if (install.disable_manifest_cache) {
            out.disable_manifest_cache = true;
        }
        // Plain-bool bunfig fields whose false state is indistinguishable from
        // "unset": forward only the non-default value (bun's own default is
        // false for each), so Options::load sees them exactly when configured.
        if (install.frozen_lockfile) {
            out.frozen_lockfile = true;
        }
        if (install.ignore_scripts) {
            out.ignore_scripts = true;
        }
        if (install.exact) {
            out.exact = true;
        }
        if (install.production) {
            out.production = true;
        }
    }
    return std::optional{std::move(out)};
}

// Resolve the registry scope with the ported Options::load order
// (bunfig config → env → CLI): project bunfig.toml (when present), env
// (BUN_CONFIG_REGISTRY / NPM_CONFIG_REGISTRY / npm_config_registry, *_TOKEN,
// BUN_CONFIG_HTTP_RETRY_COUNT), then the --registry CLI override.
mbun::install::registry_install::Options
resolve_registry_options(const InstallOptions& options,
                         const mbun::install::package_manager::BunInstallConfig* config) {
    namespace pm = mbun::install::package_manager;
    pm::LoadEnvironment env{};
    env.get = [](std::string_view key) -> std::optional<std::string_view> {
        const std::string keyZ{key};
        const char* value{std::getenv(keyZ.c_str())};
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::string_view{value};
    };
    pm::CommandLineArguments cli{};
    cli.registry = options.registry;
    cli.frozen_lockfile = options.frozenLockfile;
    cli.ignore_scripts = options.ignoreScripts;
    cli.no_progress = options.noProgress;
    cli.force = options.force;
    cli.lockfile_only = options.lockfileOnly;
    pm::Options loaded{};
    loaded.load(env, &cli, config, pm::Subcommand::Install);

    mbun::install::registry_install::Options out{};
    out.scope = loaded.scope;
    out.scopeFor = [loaded](std::string_view name) {
        return loaded.scope_for_package_name(name);
    };
    out.retry.max_retry_count = loaded.max_retry_count;
    out.verifyIntegrity = loaded.do_.contains(pm::Do::VERIFY_INTEGRITY);

    // The per-request idle bound (bun: BUN_CONFIG_HTTP_IDLE_TIMEOUT, default
    // 300s, normalized once at HTTPThread startup — HTTPThread.rs:1265-1272).
    // A value bun cannot parse falls back to the default rather than failing the
    // install. This replaces mbun's old BUN_CONFIG_INSTALL_TIMEOUT total-budget
    // guard, which bun has no equivalent of and which the concurrent path made
    // both unnecessary and wrong (see registry_install's Options).
    if (auto v{env.get("BUN_CONFIG_HTTP_IDLE_TIMEOUT")}) {
        std::uint64_t seconds{};
        auto [p, ec]{std::from_chars(v->data(), v->data() + v->size(), seconds)};
        if (ec == std::errc{} && p == v->data() + v->size()) {
            out.idleTimeout = std::chrono::seconds{
                mbun::install::async_http::normalize_idle_timeout_seconds(seconds)};
        }
    }

    // The concurrency budget. Priority is CLI → proxy → default
    // (PackageManager.rs:2200-2211); BUN_CONFIG_MAX_HTTP_REQUESTS is parsed as
    // u16 1..65535 with 0/garbage ignored (AsyncHTTP.rs:229-257).
    //
    // DEFERRED: bun's `--network-concurrency <NUM>` takes precedence over the
    // env var (CommandLineArguments.rs:366, parsed :1074). The seam is
    // InstallOptions::networkConcurrency below; hooking the actual flag up needs
    // a change in app/cli's argument parser, which is outside this module.
    std::optional<std::size_t> envConcurrency{};
    if (auto v{env.get("BUN_CONFIG_MAX_HTTP_REQUESTS")}) {
        envConcurrency = mbun::install::async_http::parse_max_http_requests_env(*v);
    }
    out.networkConcurrency = mbun::install::async_http::resolve_max_simultaneous_requests(
        options.networkConcurrency, /*hasProxy=*/false, envConcurrency);
    return out;
}

// Is the lockfile's record of this root edge identical to package.json?
// (name + specifier + group). For folder edges the recorded resolution must
// also still point at the same source directory.
bool lock_edge_unchanged(const mbun::install::Lockfile& lockfile, std::string_view alias,
                         std::string_view specifier, DependencyKind kind) {
    const mbun::install::Workspace* workspace{lockfile.root_workspace()};
    if (workspace == nullptr) {
        return false;
    }
    const auto depIt{std::ranges::find_if(workspace->deps, [&](const mbun::install::Dep& dep) {
        return dep.name == alias;
    })};
    if (depIt == workspace->deps.end() || depIt->version != specifier) {
        return false;
    }
    using LockKind = mbun::install::DepKind;
    const LockKind expected{[&] {
        switch (kind) {
            case DependencyKind::Development: return LockKind::Dev;
            case DependencyKind::Optional: return LockKind::Optional;
            case DependencyKind::Peer: return LockKind::Peer;
            case DependencyKind::Production: break;
        }
        return LockKind::Prod;
    }()};
    return depIt->kind == expected;
}

bool lock_folder_resolution_matches(const mbun::install::Lockfile& lockfile,
                                    std::string_view alias,
                                    const std::filesystem::path& source,
                                    const std::filesystem::path& root) {
    const mbun::install::Package* package{lockfile.resolve_dep("", alias)};
    if (package == nullptr || package->tag != mbun::install::Resolution::Folder) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path lockedSource{
        std::filesystem::weakly_canonical(root / std::string{package->resolution}, ec)};
    return !ec && lockedSource == source;
}

// ── lock model construction ─────────────────────────────────────────────────
// The write model mirrors what bun's Stringifier walks: the root workspace's
// declared edges, every registered workspace package, and one package entry
// per resolved edge — carried verbatim from the loaded lockfile when the edge
// is unchanged (the lock is the pin authority), fresh otherwise.

struct OwnedDep {
    std::string name;
    std::string version;
    mbun::install::DepKind kind{mbun::install::DepKind::Prod};
};

struct FolderResolution {
    std::string path;  // normalized: "file:" and leading "./" stripped
    std::string name;  // the folder package.json's name (falls back to alias)
    std::vector<OwnedDep> deps;
};

// One root edge's contribution to the lock model.
struct RootEdge {
    std::string alias;
    bool carried{false};  // loaded lock entry (and its nested keys) reused
    std::optional<FolderResolution> folder;
    bool registry{false};
};

mbun::install::DepKind model_dep_kind(DependencyKind kind) {
    switch (kind) {
        case DependencyKind::Development: return mbun::install::DepKind::Dev;
        case DependencyKind::Optional: return mbun::install::DepKind::Optional;
        case DependencyKind::Peer: return mbun::install::DepKind::Peer;
        case DependencyKind::Production: break;
    }
    return mbun::install::DepKind::Prod;
}

std::string normalize_folder_path(std::string_view specifier) {
    std::string_view path{specifier};
    if (path.starts_with("file:")) {
        path.remove_prefix(std::string_view{"file:"}.size());
    }
    while (path.starts_with("./")) {
        path.remove_prefix(2);
    }
    std::string normalized{path};
    std::ranges::replace(normalized, '\\', '/');
    while (normalized.ends_with('/')) {
        normalized.pop_back();
    }
    return normalized;
}

std::vector<OwnedDep> owned_dep_groups(const JsonValue& packageRoot) {
    std::vector<OwnedDep> out;
    constexpr std::array GROUPS{
        std::pair{std::string_view{"dependencies"}, mbun::install::DepKind::Prod},
        std::pair{std::string_view{"devDependencies"}, mbun::install::DepKind::Dev},
        std::pair{std::string_view{"optionalDependencies"}, mbun::install::DepKind::Optional},
        std::pair{std::string_view{"peerDependencies"}, mbun::install::DepKind::Peer}};
    for (const auto& [field, kind] : GROUPS) {
        const JsonValue* group{packageRoot.get(field)};
        if (group == nullptr || !group->is_object()) {
            continue;
        }
        for (const auto& member : group->members) {
            if (!member.value) {
                continue;
            }
            if (auto text{member.value->as_str()}) {
                out.push_back(OwnedDep{std::string{member.key}, std::string{*text}, kind});
            }
        }
    }
    return out;
}

// Deep-copy a parsed package entry into the write model, interning every view
// so the model outlives the loaded lockfile's source buffer.
void copy_package_into(mbun::install::Lockfile& model, const mbun::install::Package& src) {
    mbun::install::Package pkg{};
    auto intern{[&model](std::string_view s) {
        return s.empty() ? std::string_view{} : model.intern_(std::string{s});
    }};
    pkg.key = model.intern_(std::string{src.key});
    pkg.name = intern(src.name);
    pkg.version = intern(src.version);
    pkg.resolution = intern(src.resolution);
    pkg.tag = src.tag;
    pkg.registry = src.registry.data() == nullptr ? std::string_view{}
                                                  : model.intern_(std::string{src.registry});
    pkg.integrity = src.integrity.data() == nullptr ? std::string_view{}
                                                    : model.intern_(std::string{src.integrity});
    pkg.bunTag = intern(src.bunTag);
    pkg.bundled = src.bundled;
    for (const auto& dep : src.deps) {
        pkg.deps.push_back(mbun::install::Dep{model.intern_(std::string{dep.name}),
                                              intern(dep.version), dep.kind});
    }
    for (std::string_view os : src.os) {
        pkg.os.push_back(model.intern_(std::string{os}));
    }
    for (std::string_view cpu : src.cpu) {
        pkg.cpu.push_back(model.intern_(std::string{cpu}));
    }
    for (std::string_view peer : src.optionalPeers) {
        pkg.optionalPeers.push_back(model.intern_(std::string{peer}));
    }
    model.packages.push_back(std::move(pkg));
}

bool model_has_key(const mbun::install::Lockfile& model, std::string_view key) {
    return std::ranges::any_of(model.packages, [key](const mbun::install::Package& pkg) {
        return pkg.key == key;
    });
}

mbun::install::Lockfile
build_lock_model(const JsonValue& rootPackageJson,
                 const std::map<std::string, RootDependency, std::less<>>& rootDeps,
                 const std::vector<RootEdge>& edges,
                 const std::vector<mbun::install::workspace_map::Entry>& workspaces,
                 const std::optional<mbun::install::Lockfile>& loaded,
                 const std::vector<std::string>& registryPackages) {
    mbun::install::Lockfile model{};

    mbun::install::Workspace rootWs{};
    if (const JsonValue* name{rootPackageJson.get("name")}) {
        if (auto text{name->as_str()}) {
            rootWs.name = model.intern_(std::string{*text});
        }
    }
    for (const auto& [alias, dependency] : rootDeps) {
        rootWs.deps.push_back(mbun::install::Dep{model.intern_(std::string{alias}),
                                                 model.intern_(std::string{dependency.specifier}),
                                                 model_dep_kind(dependency.kind)});
    }
    model.workspaces.push_back(std::move(rootWs));

    // Every workspace registers in the lock — with or without a dep edge
    // (bun appends them to the graph; verified against bun v1.4.0 output).
    for (const auto& entry : workspaces) {
        mbun::install::Workspace ws{};
        ws.path = model.intern_(std::string{entry.relPath});
        ws.name = model.intern_(std::string{entry.name});
        if (!entry.version.empty()) {
            ws.version = model.intern_(std::string{entry.version});
        }
        model.workspaces.push_back(std::move(ws));

        mbun::install::Package pkg{};
        pkg.key = model.intern_(std::string{entry.name});
        pkg.name = model.intern_(std::string{entry.name});
        pkg.resolution = model.intern_(std::string{entry.relPath});
        pkg.tag = mbun::install::Resolution::Workspace;
        model.packages.push_back(std::move(pkg));
    }

    for (const RootEdge& edge : edges) {
        if (edge.carried && loaded.has_value()) {
            const std::string nestedPrefix{edge.alias + "/"};
            for (const mbun::install::Package& src : loaded->packages) {
                if (src.key == edge.alias || src.key.starts_with(nestedPrefix)) {
                    if (!model_has_key(model, src.key)) {
                        copy_package_into(model, src);
                    }
                }
            }
            continue;
        }
        if (edge.folder.has_value()) {
            if (model_has_key(model, edge.alias)) {
                continue;
            }
            mbun::install::Package pkg{};
            pkg.key = model.intern_(std::string{edge.alias});
            pkg.name = model.intern_(std::string{
                edge.folder->name.empty() ? edge.alias : edge.folder->name});
            pkg.resolution = model.intern_(std::string{edge.folder->path});
            pkg.tag = mbun::install::Resolution::Folder;
            for (const OwnedDep& dep : edge.folder->deps) {
                pkg.deps.push_back(mbun::install::Dep{model.intern_(std::string{dep.name}),
                                                      model.intern_(std::string{dep.version}),
                                                      dep.kind});
            }
            model.packages.push_back(std::move(pkg));
        }
        // workspace edges were registered above; registry edges are appended
        // from the install summary below.
    }

    // Registry packages resolved this run, root and transitive alike, pinned
    // as minimal npm entries ("" = default registry). The pipeline does not
    // yet surface per-package integrity or dependency lists into the summary,
    // so those fields stay empty — an honest (re-resolvable) pin, never a
    // fabricated hash. DEFERRED: thread the resolved graph out of
    // registry_install so these entries carry integrity + deps like bun's.
    for (const std::string& nameAtVersion : registryPackages) {
        const std::size_t at{nameAtVersion.rfind('@')};
        if (at == std::string::npos || at == 0) {
            continue;
        }
        const std::string name{nameAtVersion.substr(0, at)};
        const std::string version{nameAtVersion.substr(at + 1)};
        if (model_has_key(model, name)) {
            continue;
        }
        mbun::install::Package pkg{};
        pkg.key = model.intern_(std::string{name});
        pkg.name = model.intern_(std::string{name});
        pkg.version = model.intern_(std::string{version});
        pkg.tag = mbun::install::Resolution::Npm;
        pkg.registry = model.intern_(std::string{});
        pkg.integrity = model.intern_(std::string{});
        model.packages.push_back(std::move(pkg));
    }

    if (loaded.has_value()) {
        for (const auto& [name, value] : loaded->overrides) {
            model.overrides.emplace_back(model.intern_(std::string{name}),
                                         model.intern_(std::string{value}));
        }
        for (std::string_view name : loaded->trustedDependencies) {
            model.trustedDependencies.push_back(model.intern_(std::string{name}));
        }
        for (const auto& [key, value] : loaded->patchedDependencies) {
            model.patchedDependencies.emplace_back(model.intern_(std::string{key}),
                                                   model.intern_(std::string{value}));
        }
    }
    return model;
}

std::expected<void, InstallError> write_lock_file(const std::filesystem::path& root,
                                                  std::string_view contents) {
    const std::filesystem::path target{root / "bun.lock"};
    const std::filesystem::path staging{root / ".bun.lock.mbun-tmp"};
    {
        std::ofstream stream{staging, std::ios::binary | std::ios::trunc};
        if (!stream) {
            return fail(ErrorCode::IoError,
                        std::format("failed to write lockfile: {}", target.string()));
        }
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            return fail(ErrorCode::IoError,
                        std::format("failed to write lockfile: {}", target.string()));
        }
    }
    std::error_code ec;
    std::filesystem::rename(staging, target, ec);
    if (ec) {
        std::filesystem::remove(staging, ec);
        return fail(ErrorCode::IoError,
                    std::format("failed to write lockfile: {}", target.string()));
    }
    return {};
}

std::expected<void, InstallError> clone_package_tree(const std::filesystem::path& source,
                                                     const std::filesystem::path& staging) {
    std::error_code ec;
    std::filesystem::create_directories(staging, ec);
    if (ec) {
        return fail(ErrorCode::IoError,
                    std::format("failed to create install staging directory: {}", ec.message()));
    }

    std::filesystem::recursive_directory_iterator iterator{
        source, std::filesystem::directory_options::skip_permission_denied, ec};
    const std::filesystem::recursive_directory_iterator end{};
    if (ec) {
        return fail(ErrorCode::IoError,
                    std::format("failed to scan dependency folder: {}", ec.message()));
    }
    for (; iterator != end; iterator.increment(ec)) {
        if (ec) {
            return fail(ErrorCode::IoError,
                        std::format("failed to scan dependency folder: {}", ec.message()));
        }
        const std::filesystem::path relative{iterator->path().lexically_relative(source)};
        const auto first{relative.begin()};
        if (first != relative.end() && (*first == "node_modules" || *first == ".git")) {
            if (iterator->is_directory(ec)) {
                iterator.disable_recursion_pending();
            }
            continue;
        }
        const std::filesystem::path destination{staging / relative};
        if (iterator->is_symlink(ec)) {
            std::filesystem::create_directories(destination.parent_path(), ec);
            if (!ec) {
                std::filesystem::copy_symlink(iterator->path(), destination, ec);
            }
        } else if (iterator->is_directory(ec)) {
            std::filesystem::create_directories(destination, ec);
        } else if (iterator->is_regular_file(ec)) {
            std::filesystem::create_directories(destination.parent_path(), ec);
            if (!ec) {
                std::filesystem::create_hard_link(iterator->path(), destination, ec);
                if (ec) {
                    ec.clear();
                    std::filesystem::copy_file(iterator->path(), destination,
                                               std::filesystem::copy_options::overwrite_existing, ec);
                }
            }
        } else {
            return fail(ErrorCode::IoError,
                        std::format("unsupported file type in dependency: {}",
                                    iterator->path().string()));
        }
        if (ec) {
            return fail(ErrorCode::IoError,
                        std::format("failed to install '{}': {}", iterator->path().string(),
                                    ec.message()));
        }
    }
    return {};
}

std::filesystem::path staging_for(const std::filesystem::path& destination,
                                  std::size_t index) {
    const auto nonce{std::chrono::steady_clock::now().time_since_epoch().count()};
    return destination.parent_path() /
           std::format(".{}.mbun-install-{}-{}", destination.filename().string(), nonce, index);
}

}  // namespace detail

// The package NAME a non-npm `add` positional resolves to, read out of the
// target's own package.json. `specifier` is the raw positional
// (`file:../pkg`, `./pkg`, `link:../pkg`, a bare directory path…) and `root`
// the directory holding the package.json being edited.
//
// PORT-SOURCE: bun-ref/src/install/PackageManager/UpdateRequest.rs:274-286
// leaves `name = ""` for a Folder/Symlink positional and hashes the LITERAL
// instead; the real name is back-patched from the resolved package by
// bun-ref/src/install/lockfile.rs:1267-1283 (`update.matches(dep, ..)` →
// `update.package_id = package_id`) and only then written as the package.json
// key by PackageJSONEditor.rs:826-876 (`get_resolved_name`). That indirection
// exists because bun defers folder resolution to the install pass. mbun's
// folder resolver is synchronous (`detail::resolve_dependency`, Tag::Folder
// arm), so the same answer is available up front: for a folder the resolved
// package IS the target directory, and its name is its package.json "name".
export std::optional<std::string> folder_positional_name(const std::filesystem::path& root,
                                                         std::string_view specifier) {
    using Tag = mbun::install::dependency::Tag;
    const Tag tag{mbun::install::dependency::infer_tag(specifier)};
    if (tag != Tag::Folder && tag != Tag::Symlink) {
        return std::nullopt;
    }
    // `parse` needs an alias only to fill Npm/DistTag names; the sentinel keeps
    // it out of the folder path (UpdateRequest.rs:202 passes `b"@@@"`).
    auto parsed{mbun::install::dependency::parse("@@@", specifier)};
    if (!parsed) {
        return std::nullopt;
    }
    const std::string_view relative{tag == Tag::Folder ? parsed->folder : parsed->symlink};
    std::error_code ec;
    const std::filesystem::path target{
        std::filesystem::weakly_canonical(root / std::string{relative}, ec)};
    if (ec) {
        return std::nullopt;
    }
    auto loaded{detail::load_package_json(target / "package.json", true)};
    if (!loaded || !loaded->document.root) {
        return std::nullopt;
    }
    const auto* name{loaded->document.root->get("name")};
    if (name == nullptr || name->str.empty()) {
        return std::nullopt;
    }
    return std::string{name->str};
}

export InstallResult install_project(const std::filesystem::path& startDirectory,
                                     const InstallOptions& callerOptions = {}) {
    auto rootResult{detail::find_package_root(startDirectory)};
    if (!rootResult) {
        return std::unexpected(rootResult.error());
    }
    const std::filesystem::path root{*rootResult};

    // bunfig config → CLI, in bun's Options::load order: the project
    // bunfig.toml seeds the effective options, explicit CLI flags win.
    auto bunfig{detail::load_bunfig_config(root)};
    if (!bunfig) {
        return std::unexpected(bunfig.error());
    }
    InstallOptions options{callerOptions};
    (void)options.noProgress;
    if (bunfig->has_value()) {
        options.frozenLockfile |= (*bunfig)->frozen_lockfile.value_or(false);
        options.ignoreScripts |= (*bunfig)->ignore_scripts.value_or(false);
    }

    auto packageJson{detail::load_package_json(root / "package.json", false)};
    if (!packageJson) {
        return std::unexpected(packageJson.error());
    }
    auto dependencies{detail::root_dependencies(*packageJson->document.root)};
    if (!dependencies) {
        return std::unexpected(dependencies.error());
    }
    // `overrides`/`resolutions` come from the ROOT package.json only — bun gates
    // the parse on `FEATURES.is_main` (lockfile/Package.rs:3021-3030). The `$name`
    // form resolves against the root's own declared deps (OverrideMap.rs:413-437),
    // which `dependencies` above already has in hand.
    auto overrides{mbun::install::override_map::parse_from_package_json(
        *packageJson->document.root,
        [&dependencies](std::string_view name) -> std::optional<std::string_view> {
            const auto it{dependencies->find(name)};
            if (it == dependencies->end()) {
                return std::nullopt;
            }
            return std::string_view{it->second.specifier};
        })};
    if (!overrides) {
        return detail::fail(ErrorCode::InvalidPackageJson, overrides.error());
    }
    // Workspace registration comes ahead of everything the graph needs it for
    // (workspace: edges, the lock model, the frozen comparison).
    auto workspaces{mbun::install::workspace_map::collect(
        root, packageJson->document.root->get("workspaces"))};
    if (!workspaces) {
        return detail::fail(ErrorCode::InvalidPackageJson,
                            std::move(workspaces.error().message));
    }
    if (!options.ignoreScripts && detail::has_lifecycle_scripts(*packageJson->document.root)) {
        return detail::fail(ErrorCode::UnsupportedLifecycleScripts,
                            "root lifecycle scripts are not implemented yet; use --ignore-scripts");
    }

    // Both outlive `lockfile`: its string_views borrow `lockSource` (text) or
    // `binaryLockfile`'s string arena (bun.lockb).
    std::string lockSource;
    std::optional<mbun::install::lockfile::Lockfile> binaryLockfile;
    auto lockfile{detail::load_lockfile(root, lockSource, binaryLockfile)};
    if (!lockfile) {
        return std::unexpected(lockfile.error());
    }
    if (options.frozenLockfile && !lockfile->has_value()) {
        return detail::fail(ErrorCode::LockfileOutOfDate,
                            "--frozen-lockfile requires an existing bun.lock or bun.lockb");
    }

    // Compare the lock against package.json up front: the root's edge set and
    // the workspace set. Any drift makes a frozen install fail closed
    // (install_with_manager.rs:708-741) and a normal install re-save.
    bool lockOutOfDate{!lockfile->has_value()};
    // Format migration is a save trigger that is NOT frozen drift: a v0 text
    // lockfile always re-saves (the writer can only emit v1+ content —
    // bun.lock.rs version_to_write), and a bun.lockb migrates to text
    // (install_with_manager.rs:845-848).
    const bool lockNeedsMigration{
        lockfile->has_value() &&
        (binaryLockfile.has_value() || (*lockfile)->lockfileVersion == 0)};
    if (lockfile->has_value()) {
        const mbun::install::Workspace* lockRoot{(*lockfile)->root_workspace()};
        std::size_t lockRootDeps{lockRoot == nullptr ? 0 : lockRoot->deps.size()};
        if (lockRootDeps != dependencies->size()) {
            lockOutOfDate = true;
        }
        std::size_t lockWorkspaceCount{0};
        for (const mbun::install::Workspace& ws : (*lockfile)->workspaces) {
            if (ws.path.empty()) {
                continue;
            }
            ++lockWorkspaceCount;
            const bool known{std::ranges::any_of(
                *workspaces, [&ws](const mbun::install::workspace_map::Entry& entry) {
                    return entry.relPath == ws.path && entry.name == ws.name;
                })};
            if (!known) {
                lockOutOfDate = true;
            }
        }
        if (lockWorkspaceCount != workspaces->size()) {
            lockOutOfDate = true;
        }
    }

    struct WorkspaceLink {
        std::string alias;
        std::filesystem::path target;  // symlink value (relative)
        std::filesystem::path destination;
    };

    std::vector<detail::DependencyPlan> plans;
    plans.reserve(dependencies->size());
    std::vector<WorkspaceLink> workspaceLinks;
    std::vector<mbun::install::registry_install::PendingDep> registryDeps;
    std::vector<detail::RootEdge> edges;
    edges.reserve(dependencies->size());
    for (const auto& [alias, dependency] : *dependencies) {
        const std::string& specifier{dependency.specifier};
        if (!mbun::install::dependency::is_safe_install_folder_name(alias)) {
            return detail::fail(ErrorCode::UnsafePackageName,
                                std::format("unsafe dependency name: '{}'", alias));
        }
        const bool edgeUnchanged{lockfile->has_value() &&
                                 detail::lock_edge_unchanged(**lockfile, alias, specifier,
                                                             dependency.kind)};
        if (!edgeUnchanged) {
            lockOutOfDate = true;
        }
        // `--lockfile-only` never touches node_modules: an edge the lockfile
        // already records is carried verbatim (this is how a v1 git edge
        // round-trips without a git client), the rest resolve below without
        // installing.
        if (options.lockfileOnly && edgeUnchanged &&
            (*lockfile)->resolve_dep("", alias) != nullptr) {
            edges.push_back(detail::RootEdge{.alias = alias, .carried = true});
            continue;
        }
        auto resolved{detail::resolve_dependency(
            root, alias, specifier, detail::tolerates_failure(dependency.kind),
            overrides->map, *workspaces)};
        if (!resolved) {
            if (detail::tolerates_failure(dependency.kind) &&
                resolved.error().code == ErrorCode::MissingDependencyPackageJson) {
                continue;
            }
            return std::unexpected(resolved.error());
        }
        if (resolved->registryDep) {
            if (options.lockfileOnly) {
                return detail::fail(
                    ErrorCode::UnsupportedRegistryDependency,
                    std::format("--lockfile-only for unresolved registry dependency '{}' is "
                                "not implemented yet",
                                alias));
            }
            // Deferred to the registry installer's peer phase, where an edge a
            // real dependency already satisfies costs nothing.
            resolved->registryDep->peer = dependency.kind == detail::DependencyKind::Peer;
            resolved->registryDep->behavior = detail::behavior_for(dependency.kind);
            // Registry edges go through the network pipeline below, where the
            // lockfile's pin is applied per-edge (registry_install::Options::pins)
            // — bun resolves a locked edge out of the lockfile rather than
            // validating it here, because the pin has to reach transitive edges
            // too, not just the root's. Their lifecycle scripts are still
            // never run (permanent --ignore-scripts stance, DEFERRED).
            registryDeps.push_back(std::move(*resolved->registryDep));
            edges.push_back(detail::RootEdge{
                .alias = alias, .carried = edgeUnchanged, .registry = true});
            continue;
        }
        if (resolved->workspaceRelPath) {
            const std::string& relPath{*resolved->workspaceRelPath};
            auto workspaceJson{
                detail::load_package_json(root / relPath / "package.json", true)};
            if (workspaceJson && !options.ignoreScripts &&
                detail::has_lifecycle_scripts(*workspaceJson->document.root)) {
                return detail::fail(ErrorCode::UnsupportedLifecycleScripts,
                                    std::format("lifecycle scripts for '{}' are not implemented "
                                                "yet; use --ignore-scripts",
                                                alias));
            }
            if (!options.lockfileOnly) {
                const std::filesystem::path destination{detail::destination_for(root, alias)};
                // Relative symlink, exactly what bun leaves behind
                // (node_modules/pkg1 -> ../packages/pkg1).
                const std::filesystem::path target{
                    (root / relPath).lexically_relative(destination.parent_path())};
                workspaceLinks.push_back(WorkspaceLink{alias, target, destination});
            }
            edges.push_back(detail::RootEdge{.alias = alias, .carried = false});
            continue;
        }
        const std::filesystem::path source{*std::move(resolved->folderSource)};
        auto dependencyJson{detail::load_package_json(source / "package.json", true)};
        if (!dependencyJson) {
            if (detail::tolerates_failure(dependency.kind) &&
                dependencyJson.error().code == ErrorCode::MissingDependencyPackageJson) {
                continue;
            }
            // A `file:` dependency whose target has no readable package.json is a
            // resolution failure, and bun reports it as one: the dependency name
            // and the literal spec the user wrote ("<name>@<spec> failed to
            // resolve", PackageManagerResolution.rs:396-406), then the underlying
            // package.json miss ('"package.json" for "<name>" failed to resolve',
            // repository.rs:984). Reporting only the resolved absolute path left
            // the reader with no way back to the offending dependency entry.
            return detail::fail(
                ErrorCode::MissingDependencyPackageJson,
                std::format("{}@{} failed to resolve\n"
                            "error: \"package.json\" for \"{}\" failed to resolve: {}",
                            alias, specifier, alias, (source / "package.json").string()));
        }
        if (!options.ignoreScripts &&
            detail::has_lifecycle_scripts(*dependencyJson->document.root)) {
            return detail::fail(
                ErrorCode::UnsupportedLifecycleScripts,
                std::format("lifecycle scripts for '{}' are not implemented yet; use --ignore-scripts",
                            alias));
        }
        if (edgeUnchanged &&
            !detail::lock_folder_resolution_matches(**lockfile, alias, source, root)) {
            lockOutOfDate = true;
        }
        detail::FolderResolution folder{};
        folder.path = detail::normalize_folder_path(specifier);
        if (const detail::JsonValue* name{dependencyJson->document.root->get("name")}) {
            if (auto text{name->as_str()}) {
                folder.name.assign(*text);
            }
        }
        folder.deps = detail::owned_dep_groups(*dependencyJson->document.root);
        edges.push_back(detail::RootEdge{
            .alias = alias, .carried = false, .folder = std::move(folder)});
        if (!options.lockfileOnly) {
            plans.push_back(detail::DependencyPlan{alias, specifier, source,
                                                   detail::destination_for(root, alias)});
        }
    }

    if (options.frozenLockfile && lockOutOfDate) {
        return detail::fail(ErrorCode::LockfileOutOfDate,
                            "lockfile had changes, but lockfile is frozen");
    }

    struct StagedPackage {
        std::filesystem::path staging;
        std::filesystem::path destination;
    };
    std::vector<StagedPackage> staged;
    staged.reserve(plans.size());
    for (std::size_t i{0}; i < plans.size(); ++i) {
        const auto& plan{plans[i]};
        std::error_code ec;
        std::filesystem::create_directories(plan.destination.parent_path(), ec);
        if (ec) {
            return detail::fail(ErrorCode::IoError,
                                std::format("failed to create node_modules: {}", ec.message()));
        }
        const std::filesystem::path staging{detail::staging_for(plan.destination, i)};
        std::filesystem::remove_all(staging, ec);
        auto cloned{detail::clone_package_tree(plan.source, staging)};
        if (!cloned) {
            std::filesystem::remove_all(staging, ec);
            for (const auto& package : staged) {
                std::filesystem::remove_all(package.staging, ec);
            }
            return std::unexpected(cloned.error());
        }
        staged.push_back({staging, plan.destination});
    }

    for (const auto& package : staged) {
        std::error_code ec;
        std::filesystem::remove_all(package.destination, ec);
        if (ec) {
            return detail::fail(ErrorCode::IoError,
                                std::format("failed to replace package: {}", ec.message()));
        }
        std::filesystem::rename(package.staging, package.destination, ec);
        if (ec) {
            return detail::fail(ErrorCode::IoError,
                                std::format("failed to commit package install: {}", ec.message()));
        }
    }

    for (const WorkspaceLink& link : workspaceLinks) {
        std::error_code ec;
        std::filesystem::create_directories(link.destination.parent_path(), ec);
        if (ec) {
            return detail::fail(ErrorCode::IoError,
                                std::format("failed to create node_modules: {}", ec.message()));
        }
        std::filesystem::remove_all(link.destination, ec);
        std::filesystem::create_directory_symlink(link.target, link.destination, ec);
        if (ec) {
            return detail::fail(ErrorCode::IoError,
                                std::format("failed to link workspace '{}': {}", link.alias,
                                            ec.message()));
        }
    }

    std::size_t installedCount{plans.size() + workspaceLinks.size()};
    std::vector<std::string> installedPackages;
    if (!registryDeps.empty()) {
        auto registryOptions{detail::resolve_registry_options(
            options, bunfig->has_value() ? &**bunfig : nullptr)};
        // Lets a peer edge naming this project bind to it instead of fetching a
        // copy of it from the registry (registry_install::Options).
        if (const detail::JsonValue* name{packageJson->document.root->get("name")}) {
            if (auto text{name->as_str()}) {
                registryOptions.rootPackageName.assign(*text);
            }
        }
        // The root is PackageID 0 in the resolution graph now, so it gets its own
        // version alongside its name (nothing resolves against it; it describes
        // package 0).
        if (const detail::JsonValue* version{packageJson->document.root->get("version")}) {
            if (auto text{version->as_str()}) {
                registryOptions.rootPackageVersion.assign(*text);
            }
        }
        // Every transitive edge is overridden by the same root-level map the
        // direct edges above already went through.
        registryOptions.overrides = overrides->map;
        // Honour the resolutions the lockfile already pinned. Same model for
        // `bun.lock` (parse_text) and `bun.lockb` (adapt::to_install_lockfile),
        // so the two paths pin identically. `pins` must outlive `install_tree`.
        mbun::install::lockfile_pins::LockfilePins pins{};
        if (lockfile->has_value()) {
            pins = mbun::install::lockfile_pins::LockfilePins::build(**lockfile, overrides->map);
            registryOptions.pins = &pins;
        }
        auto registryResult{mbun::install::registry_install::install_tree(root, registryDeps,
                                                                          registryOptions)};
        if (!registryResult) {
            return std::unexpected(detail::map_registry_error(std::move(registryResult).error()));
        }
        installedCount += registryResult->installed;
        installedPackages = std::move(registryResult->packages);
    }

    // Save the lockfile after a successful install (install_with_manager.rs:
    // save when missing/changed; `--lockfile-only` always saves; frozen never
    // does — its drift check already ran above).
    InstallSummary summary{root, installedCount, lockfile->has_value(),
                           std::move(installedPackages)};
    const bool shouldSave{!options.frozenLockfile &&
                          (options.lockfileOnly || lockOutOfDate || lockNeedsMigration)};
    if (shouldSave) {
        const mbun::install::Lockfile model{detail::build_lock_model(
            *packageJson->document.root, *dependencies, edges, *workspaces, *lockfile,
            summary.packages)};
        // A lockfile loaded from bun.lockb has no text version — it migrates
        // at the current version, like a fresh install (bun.lock.rs:182-197).
        std::optional<std::uint32_t> loadedVersion{};
        std::optional<std::uint32_t> loadedConfig{};
        if (lockfile->has_value()) {
            loadedConfig = (*lockfile)->configVersion;
            if (!binaryLockfile.has_value()) {
                loadedVersion = (*lockfile)->lockfileVersion;
            }
        }
        const std::uint32_t version{
            mbun::install::lockfile::text_writer::version_to_write(model, loadedVersion)};
        // A text lockfile without configVersion re-saves as 0 (bun v1.4.0
        // observable); everything else (fresh install, lockb migration)
        // stamps the current config version.
        const std::uint32_t configVersion{
            loadedVersion.has_value()
                ? loadedConfig.value_or(0)
                : loadedConfig.value_or(
                      mbun::install::lockfile::text_writer::CURRENT_CONFIG_VERSION)};
        const std::string contents{
            mbun::install::lockfile::text_writer::write_text(model, version, configVersion)};
        auto written{detail::write_lock_file(root, contents)};
        if (!written) {
            return std::unexpected(written.error());
        }
        summary.savedLockfile = true;
        summary.lockPackageCount = model.packages.size() + 1;  // + the root package
    }
    return summary;
}

}  // namespace mbun::install::command
