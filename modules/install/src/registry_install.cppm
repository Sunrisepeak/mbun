// registry_install.cppm — mbun.install.registry_install
//
// End-to-end registry install pipeline for `mbun install`, composing the
// already-ported building blocks into bun's resolution→extract→link flow:
//
//   manifest GET  (network_task::for_manifest → async_http::Engine)
//     → packument parse            (npm::parse_manifest)
//     → version selection          (npm::find_best_version / find_by_dist_tag)
//     → dist.tarball + integrity   (PackageVersion.tarball_url / .integrity)
//     → tarball GET                (network_task::for_tarball → async_http::Engine)
//     → SRI verification           (install::Integrity::verify)
//     → extraction                 (extract_tgz_to_dir, staging + rename)
//     → bin links                  (bin::plan_links + apply_link_plan)
//     → transitive dependencies    (enqueued on completion, not on a DFS stack)
//
// Concurrency: this is task-driven, not a blocking DFS. Every manifest/tarball
// GET is handed to async_http::Engine — one epoll loop multiplexing all of them
// — and the continuations below run as those requests complete. That mirrors
// bun's shape: the install side does not limit anything itself, it hands the
// whole discovered frontier to the HTTP thread at once (schedule_tasks,
// runTasks.rs:1655-1678) and lets the global 64-request budget meter it
// (DEFAULT_MAX_SIMULTANEOUS_REQUESTS_FOR_BUN_INSTALL, PackageManager.rs:335);
// completions are drained between ticks (runTasks.rs:329).
//
// The retry gate lives here rather than in the engine, which is also bun's
// layering: runTasks.rs:381-390 classifies the response and calls
// enqueue_network_task again (PackageManagerEnqueue.rs:600) — immediately, with
// no backoff. The engine stays a transport and knows nothing about manifests.
//
// Blueprint correspondence (ref: .mbun/bun-ref/src/install/):
//   * PackageManager/PackageManagerEnqueue.rs `enqueue_dependency_list` →
//     manifest network task → `find_best_version` → `assign_resolution` +
//     `enqueue_extract_npm_package` (which forwards dist.integrity and the
//     dist.tarball URL into the ExtractTarball task);
//   * PackageManager/runTasks.rs — the response classification/retry gate is
//     classify_manifest_response / classify_tarball_response applied in the
//     completion continuations below (:381-390). Dependency DISCOVERY for a
//     registry package hangs off the *manifest* task's completion
//     (`process_dependency_list_for_ctx` in the `Task::Tag::PackageManifest`
//     arm, runTasks.rs:590 and :1019) — never off the extract, because the
//     packument already carries the dep list. The `Task::Tag::Extract` arm's two
//     `process_dependency_list*` calls are NOT that: :1214 serves the
//     Git/Github/Tarball version tags (which have no packument, so their deps
//     genuinely can only come out of the extracted package.json) and :1237 is
//     the peer-dependency case ("Peer dependencies do not initiate any downloads
//     of their own, thus need to be resolved here instead"). mbun mirrors this
//     split: `select_version_` enqueues a registry package's deps off the
//     packument, `commit_remote_tarball_` stays extract-gated;
//   * extract_tarball.rs `ExtractTarball::run` — integrity verification BEFORE
//     extraction, failure wording "Integrity check failed for tarball: {name}"
//     (error IntegrityCheckFailed);
//   * PackageInstall.rs / bin.rs — node_modules/<name> layout (scoped packages
//     under node_modules/@scope/name) + `.bin` symlink linking.
//
// Hoisting is REAL, not flat. Resolution is per-EDGE: every dependency edge
// resolves on its own and lands in `install_graph::InstallGraph` as
// `resolutions[dep_id] -> PackageID`, so one name can hold several versions
// (`ignore@5.3.1` at the top, `ignore@7.0.5` under
// @typescript-eslint/eslint-plugin — that is what bun 1.4.0 actually writes for
// itty-router). Once the frontier runs dry, `lockfile::hoist` (the Tree.rs port)
// decides where each edge physically lands and `node_modules_placer` puts it
// there. The old "first resolution of a name wins" flat hoist is gone, and with
// it `claimed_` — see install_graph.cppm's header for what replaced it.
//
// Deviations / DEFERRED (documented, not silent):
//   * lifecycle scripts (preinstall/install/postinstall) are never run —
//     equivalent to a permanent --ignore-scripts for registry packages
//     (DEFERRED; bun's default also refuses untrusted lifecycle scripts);
//   * manifest cache (ETag/If-None-Match 304 reuse) is not persisted — every
//     run re-fetches manifests (DEFERRED with the on-disk cache ABI);
//   * peer dependencies ARE installed, matching bun: `Features::base()` sets
//     `peer_dependencies: true` (resolver_hooks.rs:1302) and every preset but
//     `LINK` inherits it, so the root (`Features::main()`,
//     install_with_manager.rs:1584) and registry packages (`Features::NPM`)
//     both resolve their REQUIRED peers, in a deferred second phase mirroring
//     install_with_manager.rs:1629-1646 (see `Installer::run`). An OPTIONAL
//     peer is never resolved or fetched — PackageManagerEnqueue.rs:666-668
//     drops it on entry — it only binds to a package another edge supplied.
//     What stays DEFERRED is the *tree*: peer-aware nesting and the
//     "incorrect peer dependency" mismatch warning
//     (PackageManagerEnqueue.rs:2230-2249) both need the lockfile tree this
//     flat installer does not build (DEFERRED(nested-conflicts));
//   * the engine's epoll backend is Linux-only, so this pipeline is too. The
//     blocking http_executor remains as the portable fallback and keeps its own
//     tests; it is simply no longer what an install runs on. Off Linux the
//     resolver reports a DEFERRED error rather than installing — the same
//     posture the blocking path already had on Windows;
//   * extraction and SRI hashing run inline on the loop thread. bun offloads
//     them to a thread pool. MEASURED, not assumed: a full elysia install
//     (176 packages) burns 3.5s user + 0.8s sys = 4.3s of CPU against ~96s of
//     wall clock — 4% CPU, with 98.65% of wall time parked in epoll_wait
//     (strace -c -w: 106.7s across 12041 calls). Extraction+SRI together are
//     therefore bounded above by those 4.3s and cannot be what bounds an
//     install; offloading them is worth at most that, and only once the network
//     side is saturated (DEFERRED — still not the bottleneck).
export module mbun.install.registry_install;

import std;
import mbun.install.async_http;
import mbun.install.bin;
import mbun.install.dependency;
import mbun.install.extract_tarball;
import mbun.install.http_executor;
import mbun.install.install_graph;
import mbun.install.integrity;
import mbun.install.lockfile.decode;
import mbun.install.lockfile_pins;
import mbun.install.network_task;
import mbun.install.node_modules_placer;
import mbun.install.npm.json;
import mbun.install.npm.manifest;
import mbun.install.npm.negatable;
import mbun.install.npm.parse;
import mbun.install.npm.registry;
import mbun.install.npm.version_map;
import mbun.install.override_map;
import mbun.semver;

namespace mbun::install::registry_install {

namespace ah = mbun::install::async_http;
namespace nt = mbun::install::network_task;
namespace hx = mbun::install::http_executor;
namespace npm = mbun::install::npm;
namespace registry = mbun::install::npm::registry;

// ── errors ──────────────────────────────────────────────────────────────────

export enum class ErrorCode : std::uint8_t {
    InvalidDependency,
    ManifestFetchFailed,     // transport error / HTTP 4xx-5xx on the packument
    ManifestParseFailed,     // malformed packument JSON / npm error body
    NoMatchingVersion,       // no version satisfies the range / dist-tag
    TarballFetchFailed,      // transport error / HTTP 4xx-5xx on the tarball
    IntegrityCheckFailed,    // SRI mismatch (bun: "Integrity check failed ...")
    ExtractFailed,           // gzip/tar/extraction failure
    UnsupportedDependency,   // transitive dep of a kind this pipeline can't do
    IoError,
};

export struct Error {
    ErrorCode code{ErrorCode::IoError};
    std::string message;
};

// ── inputs / outputs ────────────────────────────────────────────────────────

// One dependency edge to resolve against the registry. `installName` is the
// node_modules folder (the package.json alias); `packageName` is the registry
// name (differs for `npm:` aliases).
export struct PendingDep {
    enum class Kind : std::uint8_t { Range, DistTag, TarballUrl };
    std::string installName;
    std::string packageName;
    std::string spec;  // semver range / dist-tag name / remote tarball URL
    Kind kind{Kind::Range};
    // "Failing this edge must not fail the install" — NOT the same thing as
    // `behavior.is_optional()`. An optionalDependencies edge sets both; a
    // REQUIRED peer sets only this one, because bun's failed-resolution report
    // `continue`s on `is_peer()` before it ever looks at `is_optional()`
    // (PackageManagerResolution.rs:370-374). Keeping them apart matters now that
    // the hoister exists: `DepSorter` groups by `behavior`, so a required peer
    // that claimed the OPTIONAL bit would be visited in the wrong group and win
    // (or lose) a hoisted slot bun gives to someone else.
    bool optional{false};
    // A REQUIRED `peerDependencies` edge. Peers resolve in a deferred second
    // phase so an already-resolved package of the same name wins the edge — see
    // `Installer::run`. Optional peers never become a PendingDep at all
    // (PackageManagerEnqueue.rs:666-668); see `collect_transitive_`.
    bool peer{false};
    // The real `Behavior` bits, which is what `DepSorter` (lockfile.rs:228-245)
    // sorts on and therefore what decides who wins a hoisted slot.
    mbun::install::dependency::Behavior behavior{};
    // Which edge of the graph this is — the index `resolutions[dep_id]` is keyed
    // by. Assigned by `InstallGraph::open_window`, never by the caller.
    mbun::install::lockfile::DependencyID depId{
        mbun::install::lockfile::INVALID_DEPENDENCY_ID};
};

export struct Options {
    registry::Scope scope{};                       // default registry scope
    // Scope override per package name (Options::scope_for_package_name seam);
    // when unset, `scope` is used for every package.
    std::function<registry::Scope(std::string_view)> scopeFor{};
    nt::RetryPolicy retry{};
    hx::ExecutorOptions executor{};
    bool verifyIntegrity{true};                    // Do::VERIFY_INTEGRITY

    // The platform we resolve `os`/`cpu` against — bun's `options.cpu`/
    // `options.os` (PackageManager/PackageManagerOptions.rs:88-90), defaulted to
    // the host at :153-154 and overridable with `--cpu`/`--os`
    // (CommandLineArguments.rs:120/:123, combined through `Negatable`). Kept as
    // options rather than read off the host at the point of use so a test can
    // exercise the filter for another platform without cross-compiling.
    npm::Architecture cpu{npm::CURRENT_ARCH};
    npm::OperatingSystem os{npm::CURRENT_OS};

    // The root package's own `name`. bun appends the root to the lockfile, so it
    // sits in `package_index` like any other package and a peer edge naming it
    // binds there (PackageManagerEnqueue.rs:2203-2218) instead of resolving.
    // `@elysiajs/openapi` peers on `elysia` for exactly this reason; without the
    // name, this installer would fetch a *copy of the root package* from the
    // registry into the project's own node_modules.
    std::string rootPackageName;
    // The root's own `version`, recorded alongside the name now that the root is
    // a real PackageID 0 in the graph rather than a name in a set. Only used to
    // describe package 0; nothing resolves against it.
    std::string rootPackageVersion;

    // The root package.json's `overrides`/`resolutions`, already parsed. Applies
    // to EVERY edge at EVERY depth (the map is keyed by name alone —
    // lockfile/OverrideMap.rs:22-26 and the "no fast way to trace a Dependency
    // ID to its package" note at :29-34), which is why it lives on the installer
    // rather than being applied once at the root: `classify_transitive_` is the
    // funnel every transitive edge passes through, mirroring bun's single
    // enqueue funnel (PackageManagerEnqueue.rs:714-750). The root's own direct
    // edges are overridden by the caller before they become PendingDeps.
    mbun::install::override_map::OverrideMap overrides{};

    // The registry resolutions the existing lockfile pins, or nullptr when the
    // project has no lockfile (then every edge resolves against the registry,
    // which is what `create_new_lockfile_and_enqueue` does upstream —
    // install_with_manager.rs:570).
    //
    // This is what makes an install reproducible: bun returns the locked package
    // from `get_or_put_resolved_package` before it ever looks at a manifest
    // (PackageManagerEnqueue.rs:2308-2313), so a lockfile pinning
    // `typescript@5.8.3` installs 5.8.3 even though the range `^5` would resolve
    // to 5.9.3 today. Which pins survive — and how they rank against `overrides`
    // — is bun's `Diff::generate` rule set, modelled in `LockfilePins`; the
    // pointer is non-owning and must outlive the Installer.
    const mbun::install::lockfile_pins::LockfilePins* pins{nullptr};

    // How many requests may be in flight at once. bun overrides its generic HTTP
    // default (256) to 64 for install at startup
    // (DEFAULT_MAX_SIMULTANEOUS_REQUESTS_FOR_BUN_INSTALL, PackageManager.rs:335,
    // applied :2198-2213) and halves it toward a floor of 4 on a network error
    // (runTasks.rs:370-378). The engine owns the metering; this is the value it
    // starts at.
    std::size_t networkConcurrency{
        mbun::install::async_http::DEFAULT_MAX_SIMULTANEOUS_REQUESTS_FOR_BUN_INSTALL};

    // The ONLY timeout on this path, and it is per-request, not per-install:
    // bun bounds a request with a single idle timer (IDLE_TIMEOUT_SECONDS = 300,
    // src/http/lib.rs:283, env BUN_CONFIG_HTTP_IDLE_TIMEOUT), armed on open and
    // re-armed by any byte of progress; 0 disables it.
    //
    // There is deliberately no total-install budget. mbun used to carry a 25s
    // one (BUN_CONFIG_INSTALL_TIMEOUT) because the blocking executor made a
    // large tree take minutes and hang its caller. That was a workaround for the
    // missing concurrency, not a bun concept — `grep INSTALL_TIMEOUT` over both
    // reference trees is empty — and it cannot survive now that the frontier
    // runs concurrently: a legitimately large install would trip a wall-clock
    // cap that bun does not have. The bound is per-request idleness instead,
    // exactly as upstream.
    std::chrono::milliseconds idleTimeout{
        std::chrono::seconds{mbun::install::async_http::IDLE_TIMEOUT_SECONDS}};
};

export struct Summary {
    std::size_t installed{0};
    std::vector<std::string> packages;  // "name@version" in install order
    // Resolved but skipped as os/cpu-mismatched (see `Installer::select_version_`).
    // bun surfaces the same event as a verbose "Skip installing {} - os mismatch"
    // line (lockfile/Tree.rs:576-594); we count it so the filter is observable.
    std::size_t skippedForPlatform{0};
};

export using Result = std::expected<Summary, Error>;

// ── detail ──────────────────────────────────────────────────────────────────

namespace detail {

inline std::unexpected<Error> fail(ErrorCode code, std::string message) {
    return std::unexpected(Error{code, std::move(message)});
}

inline std::filesystem::path module_dir(const std::filesystem::path& nodeModules,
                                        std::string_view installName) {
    // Scoped packages live under node_modules/@scope/name.
    std::filesystem::path out{nodeModules};
    if (installName.starts_with('@')) {
        const std::size_t slash{installName.find('/')};
        if (slash != std::string_view::npos) {
            out /= std::string{installName.substr(0, slash)};
            out /= std::string{installName.substr(slash + 1)};
            return out;
        }
    }
    out /= std::string{installName};
    return out;
}

// Convert the packument's textual integrity (npm::Integrity keeps the base64
// SRI payload / hex shasum verbatim) into the verifying install::Integrity.
inline mbun::install::Integrity to_verifier(const npm::Integrity& in) {
    switch (in.tag) {
        case npm::IntegrityTag::Sha256:
            return mbun::install::Integrity::parse("sha256-" + in.value);
        case npm::IntegrityTag::Sha384:
            return mbun::install::Integrity::parse("sha384-" + in.value);
        case npm::IntegrityTag::Sha512:
            return mbun::install::Integrity::parse("sha512-" + in.value);
        case npm::IntegrityTag::Sha1:
            if (auto sha1{mbun::install::Integrity::parse_sha_sum(in.value)}) {
                return *sha1;
            }
            return mbun::install::Integrity{};
        case npm::IntegrityTag::Unknown:
            break;
    }
    return mbun::install::Integrity{};
}

// Convert the packument bin shape into the linker's Bin model.
inline mbun::install::Bin to_linker_bin(const npm::Bin& in) {
    switch (in.kind) {
        case npm::BinKind::File:
            return in.entries.empty() ? mbun::install::Bin::none()
                                      : mbun::install::Bin::from_file(in.entries[0]);
        case npm::BinKind::NamedFile:
            return in.entries.size() < 2
                       ? mbun::install::Bin::none()
                       : mbun::install::Bin::from_named_file(in.entries[0], in.entries[1]);
        case npm::BinKind::Dir:
            return in.entries.empty() ? mbun::install::Bin::none()
                                      : mbun::install::Bin::from_dir(in.entries[0]);
        case npm::BinKind::Map:
            return mbun::install::Bin::from_map(in.entries);
        case npm::BinKind::None:
            break;
    }
    return mbun::install::Bin::none();
}

// Parse a package.json `bin` / `directories.bin` (used for remote-tarball
// installs, where there is no packument to read the bin shape from).
inline mbun::install::Bin bin_from_package_json(const npm::json::Value& root) {
    if (const npm::json::Value* b{root.get("bin")}) {
        if (auto s{b->as_str()}) {
            return mbun::install::bin_parse_string(*s);
        }
        if (b->is_object()) {
            std::vector<std::pair<std::string_view, std::string_view>> pairs;
            pairs.reserve(b->members.size());
            for (const auto& m : b->members) {
                std::string_view value{};
                if (auto s{m.value->as_str()}) {
                    value = *s;
                }
                pairs.emplace_back(m.key, value);
            }
            return mbun::install::bin_parse_object(pairs);
        }
    }
    if (const npm::json::Value* dirs{root.get("directories")}) {
        if (const npm::json::Value* db{dirs->get("bin")}) {
            if (auto s{db->as_str()}) {
                return mbun::install::bin_parse_directories(*s);
            }
        }
    }
    return mbun::install::Bin::none();
}

inline std::string_view tar_error_name(TarError e) {
    switch (e) {
        case TarError::Truncated: return "truncated archive";
        case TarError::BadChecksum: return "bad tar checksum";
        case TarError::BadOctalField: return "bad tar numeric field";
        case TarError::NameTooLong: return "tar entry name too long";
        case TarError::GzipError: return "gzip inflate failed";
        case TarError::PathEscape: return "tar entry escapes destination";
        case TarError::IoError: return "filesystem write failed";
    }
    return "tar error";
}

// Split "1.2.3-pre+build" for build_tarball_url (only used when the packument
// carried no dist.tarball URL).
struct SemverParts {
    std::uint64_t major{0};
    std::uint64_t minor{0};
    std::uint64_t patch{0};
    std::string pre;
    std::string build;
};

inline SemverParts split_version(std::string_view v) {
    SemverParts out{};
    if (auto plus{v.find('+')}; plus != std::string_view::npos) {
        out.build.assign(v.substr(plus + 1));
        v = v.substr(0, plus);
    }
    if (auto dash{v.find('-')}; dash != std::string_view::npos) {
        out.pre.assign(v.substr(dash + 1));
        v = v.substr(0, dash);
    }
    std::array<std::uint64_t*, 3> slots{&out.major, &out.minor, &out.patch};
    std::size_t start{0};
    for (std::uint64_t* slot : slots) {
        std::size_t dot{v.find('.', start)};
        std::string_view part{v.substr(start, dot == std::string_view::npos ? std::string_view::npos
                                                                            : dot - start)};
        (void)std::from_chars(part.data(), part.data() + part.size(), *slot);
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    return out;
}

// Map a settled request to the pipeline's error wording. Mirrors runTasks.rs:
// a transport failure surfaces its own message (bun has no status to report
// when metadata.is_none()), anything else reports "GET {url} - {status}".
inline Error fetch_error(const hx::ExecuteResult& result, std::string_view url,
                         ErrorCode failCode) {
    if (!result) {
        return Error{failCode, result.error().message};
    }
    return Error{failCode, std::format("GET {} - {}", url, result->status)};
}

}  // namespace detail

// ── the pipeline ────────────────────────────────────────────────────────────

class Installer {
private:
    std::filesystem::path root_;
    std::filesystem::path nodeModules_;
    Options options_;
    ah::Engine engine_;
    // The per-edge resolution graph: `resolutions[dep_id] -> PackageID`, one
    // contiguous edge window per package. This is what replaced `installed_`
    // (installName → the one version that won) and `claimed_` (the flat
    // name-keyed hoist decision) — see install_graph.cppm's header.
    mbun::install::install_graph::InstallGraph graph_;
    // packageName → parsed packument (per-run manifest dedupe, the in-memory
    // half of bun's DedupeMap/manifest cache). Node-based on purpose: version
    // continuations hold a PackageVersion* into it across later inserts, which
    // std::map keeps valid and an unordered_map would not.
    std::map<std::string, npm::PackageManifest, std::less<>> manifests_;
    // packageName → deps parked until its packument lands, so N dependents of
    // one package cost one manifest GET, not N.
    std::map<std::string, std::vector<PendingDep>, std::less<>> manifestWaiters_;
    std::set<std::string, std::less<>> manifestInFlight_;
    // Required peer edges parked by phase 1. bun holds the same queue as
    // `PackageManager.peer_dependencies` (PackageManager.rs:444) and drains it
    // only once everything else has resolved.
    std::vector<PendingDep> deferredPeers_;
    // PackageID → where its tarball was extracted, and what bins it declares.
    // Extraction is per PackageID, placement is per tree node: the same package
    // can legitimately land in several trees, and re-downloading it per
    // placement would refetch identical bytes (PackageInstall.rs links from
    // bun's cache for the same reason).
    std::map<mbun::install::lockfile::PackageID, std::filesystem::path> store_;
    std::map<mbun::install::lockfile::PackageID, mbun::install::Bin> bins_;
    std::filesystem::path storeRoot_;
    std::optional<Error> error_;  // first fatal error wins
    Summary summary_;
    std::size_t stagingNonce_{0};

public:
    Installer(std::filesystem::path root, Options options)
        : root_{std::move(root)},
          nodeModules_{root_ / "node_modules"},
          options_{std::move(options)},
          engine_{options_.networkConcurrency},
          storeRoot_{nodeModules_ / ".mbun-store"} {
        // PackageID 0 is the root, and must be: `process_subtree` maps
        // ROOT_DEP_ID onto `parentPkgId = 0` (Tree.rs:691-694), so the root's
        // edge window has to live at index 0 of the package table. This also
        // puts the root in `package_index` under its own name, standing in for
        // its presence in bun's (see Options::rootPackageName).
        graph_.put_root(options_.rootPackageName, options_.rootPackageVersion);
    }

    // Seed the frontier, then turn the loop until it runs dry. Completions do
    // the work and enqueue whatever they discover, so `has_work()` going false
    // *is* the termination condition — there is no worklist to drain separately
    // (bun: flush_dependency_queue + schedule_tasks + the runTasks drain,
    // runTasks.rs:1640-1678 / :329).
    Result run(std::span<const PendingDep> initial) {
        // The root's edges are its window of the graph, exactly like any other
        // package's — that is what makes `resolutionLists[0]` the thing
        // `is_workspace_root_dependency` tests (lockfile.rs:951-953).
        std::vector<PendingDep> rootDeps{initial.begin(), initial.end()};
        for (const PendingDep& dep : open_window_(mbun::install::lockfile::PackageID{0},
                                                  rootDeps)) {
            enqueue_dep_(dep);
        }
        drain_();  // bun: wait_for_everything_except_peers

        // Phase 2 — the deferred peer drain, install_with_manager.rs:1629-1646.
        // Resolving a peer can pull in a package whose *own* peers park here in
        // turn, so this iterates until the queue stops refilling rather than
        // walking the vector once.
        while (!deferredPeers_.empty()) {
            std::vector<PendingDep> peers;
            peers.swap(deferredPeers_);
            for (PendingDep& peer : peers) {
                if (bind_peer_(peer)) {
                    continue;
                }
                // Nothing holds the name: resolve it for real. Clearing `peer`
                // is what `install_peer = true` does at
                // processDependencyList.rs:413-419 — the edge stops deferring and
                // takes the normal resolve path. Peers its tree brings in park in
                // `deferredPeers_` again and the next turn of this loop picks
                // them up.
                peer.peer = false;
                enqueue_dep_(peer);
            }
            drain_();
        }

        if (error_) {
            return std::unexpected(*error_);
        }
        // The frontier is dry, so `graph_` is complete: hoist it and put the
        // packages where the tree says they go.
        if (auto placed{hoist_and_place_()}; !placed) {
            return std::unexpected(placed.error());
        }
        return summary_;
    }

private:
    // ── task graph ──────────────────────────────────────────────────────────

    // Once an error is latched no new work is enqueued, so this keeps draining
    // what is already in flight rather than abandoning live sockets. Every
    // in-flight request is bounded by its own idle timer, which is what bounds
    // this loop.
    void drain_() {
        while (engine_.has_work()) {
            engine_.run_once();
        }
    }

    void set_error_(ErrorCode code, std::string message) {
        if (!error_) {
            error_ = Error{code, std::move(message)};
        }
    }

    // An optional dependency's failure is not the install's failure — it just
    // does not get installed (unchanged from the DFS, only the timing moved).
    void fail_dep_(const PendingDep& dep, ErrorCode code, std::string message) {
        if (dep.optional) {
            return;
        }
        set_error_(code, std::move(message));
    }

    ah::ConnectionOptions conn_options_() const {
        ah::ConnectionOptions out{};
        out.idleTimeout = options_.idleTimeout;
        out.maxResponseBytes = options_.executor.maxResponseBytes;
        out.tlsCaBundle = options_.executor.tlsCaBundle;
        out.tlsVerifyPeer = options_.executor.tlsVerifyPeer;
        return out;
    }

    void enqueue_dep_(const PendingDep& dep) {
        if (error_) {
            return;  // failing: stop growing the frontier
        }
        if (!mbun::install::dependency::is_safe_install_folder_name(dep.installName)) {
            fail_dep_(dep, ErrorCode::InvalidDependency,
                      std::format("unsafe dependency name: '{}'", dep.installName));
            return;
        }
        // Phase 1 defers every peer edge instead of claiming a name for it
        // (PackageManagerEnqueue.rs:2056-2058 `else if behavior.is_peer() &&
        // !install_peer { return Ok(None) }`, queued at :1262/:1294/:1356).
        // Claiming here would let a peer's range beat the real `dependencies`
        // edge to the name — the reuse-first order bun's defer exists to get.
        if (dep.peer) {
            deferredPeers_.push_back(dep);
            return;
        }
        // NOTE: no name-keyed early return. Every edge resolves on its own —
        // that is the whole of stage 2. De-duplication happens one step later,
        // keyed on the *resolution* (name, version) rather than on the name, in
        // `get_or_put` (bun: PackageManagerEnqueue.rs:2308-2313).
        if (dep.kind == PendingDep::Kind::TarballUrl) {
            // A remote tarball's resolution IS its URL — there is no packument to
            // pick a version out of — so the URL is what identifies the package.
            const auto put{graph_.get_or_put(dep.packageName, dep.spec,
                                             mbun::install::lockfile::ResolutionTag::RemoteTarball)};
            graph_.resolve_edge(dep.depId, put.id);
            if (!put.inserted) {
                return;  // same URL already fetched; the edge just points at it
            }
            start_tarball_(dep, put.id, dep.spec, /*version=*/{}, /*pv=*/nullptr, /*retried=*/0);
            return;
        }
        need_manifest_(pin_(dep));
    }

    // Give `pkg` its contiguous edge window and stamp each PendingDep with the
    // DependencyID it was assigned. Returns the same deps, now addressable.
    std::vector<PendingDep> open_window_(mbun::install::lockfile::PackageID pkg,
                                         std::vector<PendingDep> deps) {
        std::vector<mbun::install::install_graph::EdgeSpec> specs;
        specs.reserve(deps.size());
        for (const PendingDep& dep : deps) {
            using Tag = mbun::install::dependency::Tag;
            const Tag tag{dep.kind == PendingDep::Kind::DistTag  ? Tag::DistTag
                          : dep.kind == PendingDep::Kind::TarballUrl ? Tag::Tarball
                                                                     : Tag::Npm};
            specs.push_back(mbun::install::install_graph::EdgeSpec{
                dep.installName, dep.packageName, dep.spec, tag, dep.behavior});
        }
        const std::vector<mbun::install::lockfile::DependencyID> ids{
            graph_.open_window(pkg, specs)};
        for (std::size_t i{0}; i < deps.size() && i < ids.size(); ++i) {
            deps[i].depId = ids[i];
        }
        return deps;
    }

    // `get_or_put_resolved_package` with `install_peer = true`
    // (PackageManagerEnqueue.rs:2203-2262): a peer binds to a package the graph
    // already holds under that name rather than resolving. Returns false when
    // nothing holds the name and the edge has to resolve for real.
    bool bind_peer_(const PendingDep& peer) {
        const std::span<const mbun::install::lockfile::PackageID> ids{
            graph_.ids_for_name(peer.installName)};
        // PackageID 0 is this project. bun's `package_index` holds the root too,
        // and a peer naming it binds there instead of fetching a copy of the
        // project from the registry — `@elysiajs/openapi` peers on `elysia` for
        // exactly this reason. Binding it to package 0 would ask the placer to
        // install the root into its own node_modules, so the edge resolves to
        // nothing and the hoister drops it (`clean`, Tree.rs:518-521). bun
        // instead links the root/workspace directory in; that link is DEFERRED
        // with workspaces, and the observable result here — no copy of the root
        // in node_modules — is the same.
        if (!options_.rootPackageName.empty() && peer.installName == options_.rootPackageName) {
            return true;
        }
        // 1. Any existing resolution the peer's range accepts wins
        //    (`resolution_satisfies_dependency`, :2216).
        for (const mbun::install::lockfile::PackageID id : ids) {
            if (id == 0) {
                continue;
            }
            if (graph_.package_tag(id) == mbun::install::lockfile::ResolutionTag::Npm &&
                peer.kind == PendingDep::Kind::Range &&
                mbun::semver::satisfies(graph_.package_version(id), peer.spec)) {
                graph_.resolve_edge(peer.depId, id);
                return true;
            }
        }
        // 2. Otherwise bun warns "incorrect peer dependency" (:2230-2249) and
        //    binds the existing package anyway, provided the resolution and the
        //    version speak the same dialect. The warning itself is DEFERRED —
        //    this installer has no Log to route it to — but the *binding* is the
        //    observable half and it happens here.
        for (const mbun::install::lockfile::PackageID id : ids) {
            if (id == 0) {
                continue;
            }
            if (graph_.package_tag(id) == mbun::install::lockfile::ResolutionTag::Npm &&
                peer.kind != PendingDep::Kind::TarballUrl) {
                graph_.resolve_edge(peer.depId, id);
                return true;
            }
        }
        return false;
    }

    // Apply the lockfile's pin to one edge, mirroring the early return in
    // `get_or_put_resolved_package` (PackageManagerEnqueue.rs:2308-2313): a
    // locked resolution is used instead of resolving the range.
    //
    // Rewriting `spec` to the exact locked version — rather than plumbing a
    // separate "pinned" field down to `select_version_` — is deliberate: an
    // exact version IS a semver range that only that version satisfies, so
    // `find_best_version` can return nothing else, and every downstream rule
    // (os/cpu gate, SRI verification, dep discovery off the packument) keeps
    // running on real packument data. It also collapses the dist-tag case for
    // free: bun's early return fires before `version.tag` is examined at all
    // (:2315), so a lockfile-pinned `"latest"` edge does not consult the tag
    // either — and `Kind::Range` here is what stops `find_by_dist_tag` from
    // being called with a version string.
    //
    // Called at the funnel every edge passes through (root, transitive, and the
    // phase-2 peer drain, which re-enters `enqueue_dep_`), matching bun's single
    // enqueue funnel rather than pinning only the root's direct edges.
    PendingDep pin_(const PendingDep& dep) const {
        if (options_.pins == nullptr) {
            return dep;
        }
        const std::optional<std::string_view> locked{
            options_.pins->pin_for(dep.installName, dep.spec)};
        if (!locked) {
            return dep;  // no pin, or the range moved off it: resolve normally
        }
        PendingDep out{dep};
        out.spec.assign(*locked);
        out.kind = PendingDep::Kind::Range;
        return out;
    }

    void need_manifest_(const PendingDep& dep) {
        if (auto it{manifests_.find(dep.packageName)}; it != manifests_.end()) {
            select_version_(dep, it->second);  // packument already in hand
            return;
        }
        manifestWaiters_[dep.packageName].push_back(dep);
        if (!manifestInFlight_.insert(dep.packageName).second) {
            return;  // someone is already fetching it; this dep rides along
        }
        start_manifest_(dep.packageName, /*retried=*/0);
    }

    void start_manifest_(const std::string& packageName, std::uint16_t retried) {
        registry::Scope scope{scope_for_(packageName)};
        auto request{nt::for_manifest(packageName, scope, /*etag=*/{},
                                      /*last_modified=*/{}, /*needs_extended=*/false)};
        if (!request) {
            manifest_failed_(packageName,
                             Error{ErrorCode::ManifestFetchFailed, request.error().message});
            return;
        }
        std::string url{request->url};
        engine_.fetch(*request, conn_options_(),
                      [this, packageName, url = std::move(url), retried](
                          hx::ExecuteResult result) mutable {
                          on_manifest_(packageName, url, retried, std::move(result));
                      });
    }

    void on_manifest_(const std::string& packageName, const std::string& url,
                      std::uint16_t retried, hx::ExecuteResult result) {
        if (error_) {
            return;
        }
        const bool hasMetadata{result.has_value()};
        const int status{hasMetadata ? result->status : 0};
        const nt::Classification verdict{
            nt::classify_manifest_response(hasMetadata, status, retried, options_.retry)};
        if (verdict.verdict == nt::ResponseVerdict::Retry) {
            // Immediate re-enqueue, no backoff — bun does exactly this
            // (runTasks.rs:397-399 → PackageManagerEnqueue.rs:600).
            start_manifest_(packageName, static_cast<std::uint16_t>(retried + 1));
            return;
        }
        if (verdict.verdict != nt::ResponseVerdict::Success) {
            manifest_failed_(packageName,
                             detail::fetch_error(result, url, ErrorCode::ManifestFetchFailed));
            return;
        }
        std::string_view body{reinterpret_cast<const char*>(result->body.data()),
                              result->body.size()};
        auto manifest{npm::parse_manifest(body, packageName)};
        if (!manifest) {
            manifest_failed_(
                packageName,
                Error{ErrorCode::ManifestParseFailed,
                      std::format("received malformed package manifest for \"{}\"", packageName)});
            return;
        }
        manifestInFlight_.erase(packageName);
        auto [it, inserted]{manifests_.emplace(packageName, std::move(*manifest))};
        for (const PendingDep& dep : take_waiters_(packageName)) {
            select_version_(dep, it->second);
        }
    }

    // A packument that never arrives fails every dep waiting on it — each judged
    // on its own `optional` flag, so one optional dependent cannot mask a
    // required one (or vice versa).
    void manifest_failed_(const std::string& packageName, Error error) {
        manifestInFlight_.erase(packageName);
        for (const PendingDep& dep : take_waiters_(packageName)) {
            fail_dep_(dep, error.code, error.message);
        }
    }

    std::vector<PendingDep> take_waiters_(const std::string& packageName) {
        auto it{manifestWaiters_.find(packageName)};
        if (it == manifestWaiters_.end()) {
            return {};
        }
        std::vector<PendingDep> out{std::move(it->second)};
        manifestWaiters_.erase(it);
        return out;
    }

    // Packument → version → tarball URL. `manifest` is owned by manifests_ and
    // outlives every continuation, so &pv stays valid.
    void select_version_(const PendingDep& dep, const npm::PackageManifest& manifest) {
        if (error_) {
            return;
        }
        npm::FindResult found{dep.kind == PendingDep::Kind::DistTag
                                  ? npm::find_by_dist_tag(manifest, dep.spec)
                                  : npm::find_best_version(manifest, dep.spec)};
        if (!found) {
            // Wording from PackageManagerEnqueue.rs (colour markup stripped).
            fail_dep_(dep, ErrorCode::NoMatchingVersion,
                      std::format("No version matching \"{}\" found for specifier \"{}\" (but "
                                  "package exists)",
                                  dep.spec, dep.packageName));
            return;
        }
        const npm::PackageVersion& pv{*found.package};

        // os/cpu gate. bun drops platform-mismatched packages when it builds the
        // install tree (lockfile/Tree.rs:574, `BuilderMethod::Filter` — "We skip
        // dependencies based on 'os', 'cpu' ... Dependencies of a disabled
        // package are not included", :416-419), which is *before* anything is
        // fetched for them. Gating here — after the packument (which carries
        // `os`/`cpu`) but before the tarball GET — is the same point in the
        // pipeline for this task-driven shape, and is why it is a speed fix as
        // much as a correctness one: the tarball is never requested and the
        // package's own dependencies are never enqueued, since only a completed
        // extract calls `enqueue_dep_` on them.
        //
        // Not conditioned on `dep.optional`: Meta.rs:67 states the check is
        // "completely unrelated to ... optionalDependencies", and bun 1.3.14
        // agrees — a *required* fsevents (os: darwin) on linux is silently
        // absent, exit 0. So this returns instead of routing through
        // `fail_dep_`, which would turn a required mismatch into an error.
        //
        // The edge is left unresolved (`resolutions[dep_id]` stays
        // INVALID_PACKAGE_ID), which is precisely how the hoister expresses
        // "filtered out": `process_subtree` skips it under
        // `BuilderMethod::Filter` (Tree.rs:770-772) and `Builder::clean` drops it
        // (:518-521). Because no PackageID is ever created for it, the package's
        // own dependencies are never enqueued either — bun's "Dependencies of a
        // disabled package are not included" (Tree.rs:416-419) falls out for
        // free rather than needing a second rule.
        if (npm::is_disabled(pv.cpu, pv.os, options_.cpu, options_.os)) {
            ++summary_.skippedForPlatform;
            return;
        }

        // ── per-edge resolution ─────────────────────────────────────────────
        // This is the line the whole nested-tree story hangs off: the edge
        // resolves to a PackageID keyed by (name, version), so two edges named
        // "ignore" asking for `^5` and `^7` get two different PackageIDs and the
        // hoister can put them in two different directories. The old code
        // returned early here when the *name* was taken and the second edge never
        // existed at all.
        const auto put{graph_.get_or_put(dep.packageName, found.version,
                                         mbun::install::lockfile::ResolutionTag::Npm)};
        graph_.resolve_edge(dep.depId, put.id);
        if (!put.inserted) {
            // This exact (name, version) already has a PackageID: its tarball is
            // fetched or fetching and its dependencies are already in the graph.
            // Re-walking them would duplicate every edge below it.
            // (PackageManagerEnqueue.rs:2308-2313.)
            return;
        }

        std::string url{pv.tarball_url};
        if (url.empty()) {
            // extract_tarball.rs build_url default format.
            registry::Scope scope{scope_for_(dep.packageName)};
            detail::SemverParts parts{detail::split_version(pv.version)};
            url = nt::build_tarball_url(scope.url, dep.packageName, parts.major, parts.minor,
                                        parts.patch, parts.pre, parts.build);
        }
        // Graph discovery is manifest-driven, NOT extract-driven.
        //
        // `pv.dependencies` is already in hand right here — it came from the
        // packument. Waiting for this package's tarball to download, verify and
        // extract before enqueueing its dependencies serializes the whole install
        // into a chain of depth = dependency-tree depth: every level costs a
        // tarball round-trip before the next level's manifests are even
        // requested. Measured on elysia: the frontier collapses to a median of 3
        // concurrent requests against a budget of 64, and the process sits idle
        // in epoll_wait 96% of the wall clock (4% CPU) — not compute-bound, just
        // starved of work it already knew about.
        //
        // bun resolves the graph from packuments alone: a manifest task's
        // completion enqueues that version's dependencies
        // (getOrPutResolvedPackage → processDependencyList), while
        // `enqueue_extract_npm_package` is a *separate* task whose completion
        // only puts files on disk. Extraction never gates discovery for a
        // registry package, because the manifest already carries the dep list.
        // Enqueueing here restores that split: manifests (cheap, cacheable) race
        // ahead and discover the graph, tarballs stream in parallel behind them.
        //
        // The remote-tarball case is genuinely extract-gated and stays that way —
        // it has no packument, so its deps really do come from the extracted
        // package.json (commit_remote_tarball_).
        auto transitive{collect_transitive_(pv.dependencies, pv.optional_dependencies,
                                            pv.peer_dependencies,
                                            pv.non_optional_peer_dependencies_start)};
        if (!transitive) {
            fail_dep_(dep, transitive.error().code, transitive.error().message);
            return;
        }
        // Claim this package's contiguous edge window in one shot, BEFORE any
        // child is enqueued. Package.rs:568-569 builds a package's window from a
        // single `(prev_len, end - prev_len)` pair, so the edges have to be
        // adjacent; this pipeline completes out of order, and a child enqueued
        // below can re-enter `select_version_` synchronously (its packument may
        // already be in `manifests_`) and open a window of its own. Pushing the
        // whole window first is what keeps the two from interleaving.
        const std::vector<PendingDep> children{open_window_(put.id, std::move(*transitive))};
        // Enqueue order is no longer load-bearing. It used to be — `claimed_` was
        // first-wins, so whoever asked first won the top-level slot — but the
        // hoister decides placement now, from `DepSorter` order alone
        // (lockfile.rs:228-245). Resolution order cannot change the layout.
        start_tarball_(dep, put.id, url, std::string{found.version}, &pv, /*retried=*/0);
        for (const PendingDep& next : children) {
            enqueue_dep_(next);
        }
    }

    // `pv == nullptr` is the remote-tarball case: no packument, so version, bin
    // and deps come from the extracted package.json instead.
    void start_tarball_(const PendingDep& dep, mbun::install::lockfile::PackageID pkgId,
                        const std::string& url, std::string version,
                        const npm::PackageVersion* pv, std::uint16_t retried) {
        registry::Scope scope{scope_for_(dep.packageName)};
        auto request{nt::for_tarball(url, dep.packageName, scope,
                                     nt::Authorization::AllowAuthorization)};
        if (!request) {
            fail_dep_(dep, ErrorCode::TarballFetchFailed, request.error().message);
            return;
        }
        engine_.fetch(*request, conn_options_(),
                      [this, dep, pkgId, url, version = std::move(version), pv, retried](
                          hx::ExecuteResult result) mutable {
                          on_tarball_(dep, pkgId, url, version, pv, retried, std::move(result));
                      });
    }

    void on_tarball_(const PendingDep& dep, mbun::install::lockfile::PackageID pkgId,
                     const std::string& url, const std::string& version,
                     const npm::PackageVersion* pv, std::uint16_t retried,
                     hx::ExecuteResult result) {
        if (error_) {
            return;
        }
        const bool hasMetadata{result.has_value()};
        const int status{hasMetadata ? result->status : 0};
        const nt::Classification verdict{
            nt::classify_tarball_response(hasMetadata, status, retried, options_.retry)};
        if (verdict.verdict == nt::ResponseVerdict::Retry) {
            start_tarball_(dep, pkgId, url, version, pv, static_cast<std::uint16_t>(retried + 1));
            return;
        }
        if (verdict.verdict != nt::ResponseVerdict::Success) {
            fail_dep_(dep, ErrorCode::TarballFetchFailed,
                      detail::fetch_error(result, url, ErrorCode::TarballFetchFailed).message);
            return;
        }
        auto installed{pv != nullptr
                           ? commit_registry_package_(dep, pkgId, *pv, version, result->body)
                           : commit_remote_tarball_(dep, pkgId, result->body)};
        if (!installed) {
            fail_dep_(dep, installed.error().code, installed.error().message);
            return;
        }
        // A remote tarball's dependencies genuinely can only come out of its
        // extracted package.json (it has no packument), so this is where its edge
        // window opens. A registry package's window opened in `select_version_`
        // off the packument; `transitive` is empty for it.
        if (!installed->transitive.empty()) {
            for (const PendingDep& next : open_window_(pkgId, std::move(installed->transitive))) {
                enqueue_dep_(next);
            }
        }
    }

    struct InstalledPackage {
        std::string version;
        std::vector<PendingDep> transitive;
    };

    registry::Scope scope_for_(std::string_view packageName) const {
        if (options_.scopeFor) {
            return options_.scopeFor(packageName);
        }
        return options_.scope;
    }

    // Extract `.tgz` bytes into the PackageID-keyed store via a staging dir +
    // rename (the atomic-commit stance of PackageInstall.rs, minus fd-relative
    // syscalls).
    //
    // The store, not node_modules/<name>, because placement is the tree's
    // decision and it has not been made yet: the frontier is still resolving,
    // and the same PackageID may end up in several directories. bun extracts to
    // its global cache and links from there for the same reason
    // (PackageInstall.rs); this store is the per-install equivalent and is
    // removed once every placement is linked.
    std::expected<void, Error> extract_into_store_(std::span<const std::uint8_t> bytes,
                                                   std::string_view installName,
                                                   mbun::install::lockfile::PackageID pkgId) {
        const std::filesystem::path destination{storeRoot_ / std::to_string(pkgId)};
        std::string sanitized{installName};
        std::ranges::replace(sanitized, '/', '+');
        const auto nonce{std::chrono::steady_clock::now().time_since_epoch().count()};
        const std::filesystem::path staging{
            storeRoot_ /
            std::format(".{}.mbun-registry-{}-{}", sanitized, nonce, stagingNonce_++)};

        std::error_code ec;
        std::filesystem::create_directories(storeRoot_, ec);
        std::filesystem::remove_all(staging, ec);
        auto extracted{extract_tgz_to_dir(
            {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()}, staging)};
        if (!extracted) {
            std::filesystem::remove_all(staging, ec);
            return detail::fail(ErrorCode::ExtractFailed,
                                std::format("failed to extract tarball for '{}': {}",
                                            installName,
                                            detail::tar_error_name(extracted.error())));
        }
        std::filesystem::create_directories(destination.parent_path(), ec);
        std::filesystem::remove_all(destination, ec);
        if (ec) {
            std::filesystem::remove_all(staging, ec);
            return detail::fail(ErrorCode::IoError,
                                std::format("failed to replace package '{}'", installName));
        }
        std::filesystem::rename(staging, destination, ec);
        if (ec) {
            std::filesystem::remove_all(staging, ec);
            return detail::fail(ErrorCode::IoError,
                                std::format("failed to commit package install '{}': {}",
                                            installName, ec.message()));
        }
        store_[pkgId] = destination;
        return {};
    }

    // Link the package's bins into the `.bin` of the node_modules directory it
    // was actually placed in (bin.rs Linker::link) — for a nested placement that
    // is the nested tree's `.bin`, not the root's. Missing targets are skipped,
    // matching bun's skipped_due_to_missing_bin.
    void link_bins_(const mbun::install::Bin& bin, std::string_view installName,
                    const std::filesystem::path& nodeModulesDir) {
        if (bin.tag == BinTag::None) {
            return;
        }
        const std::string nodeModules{nodeModulesDir.string()};
        LinkPlan plan{plan_links(bin, nodeModules, nodeModules, installName, /*globalBinPath=*/"",
                                 /*global=*/false)};
        if (plan.needsDirListing && !plan.dirTarget.empty()) {
            // Expand the Dir-tag seam: every regular file in directories.bin.
            const std::string destDir{build_destination_dir(nodeModules, "", false)};
            std::error_code ec;
            std::filesystem::directory_iterator it{plan.dirTarget, ec};
            if (!ec) {
                for (const auto& entry : it) {
                    if (!entry.is_regular_file(ec) && !entry.is_symlink(ec)) {
                        continue;
                    }
                    std::string filename{entry.path().filename().string()};
                    std::string_view name{normalized_bin_name(filename)};
                    if (name.empty()) {
                        continue;
                    }
                    plan.actions.push_back(LinkAction{entry.path().string(),
                                                      destDir + std::string{name}, false});
                }
            }
        }
        (void)apply_link_plan(plan);
    }

    // Map a transitive package.json dependency edge into a PendingDep.
    // Non-registry kinds are unsupported here (folder/git/workspace transitive
    // deps of registry packages — DEFERRED); optional ones are skipped.
    std::expected<std::optional<PendingDep>, Error>
    classify_transitive_(std::string_view name, std::string_view range, bool optional) {
        auto parsed{mbun::install::dependency::parse(name, range)};
        if (!parsed) {
            return detail::fail(ErrorCode::InvalidDependency,
                                std::format("invalid dependency '{}': {}", name, range));
        }
        using Tag = mbun::install::dependency::Tag;
        // A root `overrides`/`resolutions` entry replaces this edge's version
        // wholesale, at any depth (PackageManagerEnqueue.rs:714-750). The one
        // exemption reachable here is a direct `npm:` alias — bun keys the lookup
        // on the *real* package name and skips aliases outright, so `"a":
        // "npm:b@1"` is never rewritten by an override on `a`. (bun's other
        // exemption, `behavior.is_workspace()`, cannot occur on a registry
        // package's transitive edge.) The override literal may itself be an
        // alias (`npm:other@1`), which re-parsing below turns into a packageName
        // redirect — bun's `update_name_and_name_hash_from_version_replacement`
        // (PackageManagerEnqueue.rs:1945-1968).
        const bool isAlias{parsed->tag == Tag::Npm && parsed->npm.is_alias};
        if (!isAlias) {
            if (const std::string* replacement{options_.overrides.get(name)}) {
                range = *replacement;
                parsed = mbun::install::dependency::parse(name, range);
                if (!parsed) {
                    return detail::fail(
                        ErrorCode::InvalidDependency,
                        std::format("invalid override for '{}': {}", name, range));
                }
            }
        }
        PendingDep dep{};
        dep.installName.assign(name);
        dep.optional = optional;
        switch (parsed->tag) {
            case Tag::Npm:
                dep.packageName.assign(parsed->npm.name.empty() ? name : parsed->npm.name);
                dep.spec.assign(parsed->npm.version);
                dep.kind = PendingDep::Kind::Range;
                return std::optional{std::move(dep)};
            case Tag::DistTag:
                dep.packageName.assign(parsed->dist_tag.name.empty() ? name
                                                                     : parsed->dist_tag.name);
                dep.spec.assign(parsed->dist_tag.tag);
                dep.kind = PendingDep::Kind::DistTag;
                return std::optional{std::move(dep)};
            case Tag::Tarball:
                if (parsed->tarball.uri.kind == mbun::install::dependency::URI::Kind::Remote) {
                    dep.packageName.assign(name);
                    dep.spec.assign(parsed->tarball.uri.value);
                    dep.kind = PendingDep::Kind::TarballUrl;
                    return std::optional{std::move(dep)};
                }
                [[fallthrough]];
            default:
                if (optional) {
                    return std::optional<PendingDep>{};
                }
                return detail::fail(
                    ErrorCode::UnsupportedDependency,
                    std::format("transitive dependency '{}@{}' of a registry package is not "
                                "implemented yet",
                                name, range));
        }
    }

    // `Features::NPM` — dependencies + optionalDependencies + peerDependencies,
    // no devDependencies (resolver_hooks.rs:1340-1342, inheriting
    // `peer_dependencies: true` from `base()`).
    std::expected<std::vector<PendingDep>, Error>
    collect_transitive_(const npm::DepMap& dependencies, const npm::DepMap& optionalDependencies,
                        const npm::DepMap& peerDependencies,
                        std::uint32_t nonOptionalPeerStart) {
        std::vector<PendingDep> out;
        out.reserve(dependencies.size() + optionalDependencies.size() + peerDependencies.size());
        // The `Behavior` bits below are not decoration: `DepSorter`
        // (lockfile.rs:228-245) sorts each package's edges by behavior group and
        // then by name, and whoever `process_subtree` visits first wins the
        // hoisted slot. Getting the group wrong yields a tree that is
        // self-consistent and still not bun's.
        for (const auto& [name, range] : dependencies) {
            auto dep{classify_transitive_(name, range, /*optional=*/false)};
            if (!dep) {
                return std::unexpected(dep.error());
            }
            if (*dep) {
                (*dep)->behavior = mbun::install::dependency::Behavior{
                    mbun::install::dependency::Behavior::PROD};
                out.push_back(std::move(**dep));
            }
        }
        for (const auto& [name, range] : optionalDependencies) {
            auto dep{classify_transitive_(name, range, /*optional=*/true)};
            if (dep && *dep) {
                (*dep)->behavior = mbun::install::dependency::Behavior{
                    mbun::install::dependency::Behavior::OPTIONAL};
                out.push_back(std::move(**dep));
            }
        }
        // An OPTIONAL peer is never resolved and never downloaded. bun drops it
        // on entry to the enqueue path — PackageManagerEnqueue.rs:666-668:
        //
        //     if dependency.behavior.is_optional_peer() { return Ok(()); }
        //
        // It can only ever be *bound* to a package something else already
        // supplied, which the tree does by hoisting (`pending_optional_peers`,
        // Tree.rs:440-444, :1017-1045). In a flat node_modules that binding is
        // implicit — if the name is installed, resolution finds it — so an
        // optional peer needs no edge at all here. Enqueueing them anyway is
        // exactly how an install ends up dragging in `@swc/core` /
        // `@microsoft/api-extractor` that `bun install` does not: tsup marks all
        // four of its peers optional and bun's lockfile records none of them.
        //
        // The first `nonOptionalPeerStart` entries are the optional ones —
        // npm.rs:709 ("the first N items of `peer_dependencies` are optional"),
        // the same split lockfile/Package.rs:904 reads to set the OPTIONAL bit.
        //
        // A REQUIRED peer does resolve, and its failure is still not fatal: the
        // failed-resolution report `continue`s on `is_peer()`
        // (PackageManagerResolution.rs:370-374) before it ever reaches the
        // `is_optional()` check, and that TODO records erroring on required peers
        // as a future lockfile-rewrite change rather than today's behavior. Hence
        // `optional=true` — it means "not fatal", not "optional peer".
        for (std::size_t i{nonOptionalPeerStart}; i < peerDependencies.size(); ++i) {
            const auto& [name, range] = peerDependencies[i];
            auto dep{classify_transitive_(name, range, /*optional=*/true)};
            if (dep && *dep) {
                (*dep)->peer = true;
                // PEER, not OPTIONAL: `optional` above means only "failure is not
                // fatal" (see PendingDep). `Behavior::cmp` puts peers last
                // (resolver_hooks.rs:266-288 — workspace < dev < optional < prod
                // < peer), which is why a peer never steals a hoisted slot from
                // the real `dependencies` edge that names the same package.
                (*dep)->behavior = mbun::install::dependency::Behavior{
                    mbun::install::dependency::Behavior::PEER};
                out.push_back(std::move(**dep));
            }
        }
        return out;
    }

    // Npm range / dist-tag: integrity → extract → bins. Discovery of this
    // package's own dependencies does NOT happen here — `select_version_`
    // already enqueued them straight off the packument, so that a tarball
    // download never gates the next level of the graph (see the note there).
    // This path is now purely "put the files on disk", which is the job bun
    // gives its extract task.
    std::expected<InstalledPackage, Error>
    commit_registry_package_(const PendingDep& dep, mbun::install::lockfile::PackageID pkgId,
                             const npm::PackageVersion& pv, std::string_view version,
                             std::span<const std::uint8_t> bytes) {
        // Integrity gate BEFORE extraction (ExtractTarball::run), wording
        // aligned with bun: "Integrity check failed for tarball: {name}".
        if (options_.verifyIntegrity) {
            mbun::install::Integrity verifier{detail::to_verifier(pv.integrity)};
            if (verifier.tag.is_supported() && !verifier.verify(bytes)) {
                return detail::fail(
                    ErrorCode::IntegrityCheckFailed,
                    std::format("Integrity check failed for tarball: {}", dep.packageName));
            }
        }

        auto extracted{extract_into_store_(bytes, dep.installName, pkgId)};
        if (!extracted) {
            return std::unexpected(extracted.error());
        }
        // Bins are recorded, not linked: which `.bin` they belong in is a
        // property of the tree node this package lands in, which the hoister has
        // not decided yet. `hoist_and_place_` links them.
        bins_[pkgId] = detail::to_linker_bin(pv.bin);

        // No transitive list: `select_version_` owns discovery for this path.
        // The packument key, not pv.version: the DFS recorded find_best_version's
        // FindResult::version and the two can differ on a malformed packument.
        return InstalledPackage{std::string{version}, /*transitive=*/{}};
    }

    // Remote tarball dependency ("pkg": "http://.../pkg.tgz"): no packument.
    // Version/bin/deps come from the extracted package.json. bun computes and
    // records a sha512 for the lockfile here; with lockfile writing DEFERRED
    // there is no prior pin to verify against.
    std::expected<InstalledPackage, Error>
    commit_remote_tarball_(const PendingDep& dep, mbun::install::lockfile::PackageID pkgId,
                           std::span<const std::uint8_t> bytes) {
        auto extracted{extract_into_store_(bytes, dep.installName, pkgId)};
        if (!extracted) {
            return std::unexpected(extracted.error());
        }

        const std::filesystem::path packageJsonPath{store_[pkgId] / "package.json"};
        std::ifstream stream{packageJsonPath, std::ios::binary};
        std::string source{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
        auto doc{npm::json::parse(source)};
        if (!stream || !doc || !doc->root || !doc->root->is_object()) {
            return detail::fail(ErrorCode::ExtractFailed,
                                std::format("tarball for '{}' has no valid package.json",
                                            dep.installName));
        }
        const npm::json::Value& root{*doc->root};

        std::string version{"0.0.0"};
        if (const npm::json::Value* v{root.get("version")}) {
            if (auto s{v->as_str()}; s && !s->empty()) {
                version.assign(*s);
            }
        }
        bins_[pkgId] = detail::bin_from_package_json(root);

        auto read_deps{[&root](std::string_view field) {
            npm::DepMap out;
            if (const npm::json::Value* obj{root.get(field)}; obj && obj->is_object()) {
                out.reserve(obj->members.size());
                for (const auto& m : obj->members) {
                    std::string_view value{};
                    if (auto s{m.value->as_str()}) {
                        value = *s;
                    }
                    out.emplace_back(std::string{m.key}, std::string{value});
                }
            }
            return out;
        }};
        // `Features::TARBALL` is `Features::NPM` (resolver_hooks.rs:1344), so a
        // remote tarball's peers resolve like a registry package's. The packument
        // parser applies the `peerDependenciesMeta` optional split for us
        // (npm/parse.cppm); reading a raw package.json means doing it here, and it
        // is not optional to do — an optional peer must never be enqueued
        // (PackageManagerEnqueue.rs:666-668). Dropping them on the spot leaves
        // only required peers, hence the `0` split below.
        npm::DepMap requiredPeers;
        if (const npm::json::Value* peers{root.get("peerDependencies")};
            peers != nullptr && peers->is_object()) {
            const npm::json::Value* meta{root.get("peerDependenciesMeta")};
            for (const auto& m : peers->members) {
                bool optionalPeer{false};
                if (meta != nullptr && meta->is_object()) {
                    if (const npm::json::Value* entry{meta->get(m.key)}) {
                        if (const npm::json::Value* opt{entry->get("optional")}) {
                            auto flag{opt->as_bool()};
                            optionalPeer = flag && *flag;
                        }
                    }
                }
                if (optionalPeer) {
                    continue;
                }
                std::string_view value{};
                if (auto s{m.value->as_str()}) {
                    value = *s;
                }
                requiredPeers.emplace_back(std::string{m.key}, std::string{value});
            }
        }
        auto transitive{collect_transitive_(read_deps("dependencies"),
                                            read_deps("optionalDependencies"), requiredPeers,
                                            /*nonOptionalPeerStart=*/0)};
        if (!transitive) {
            return std::unexpected(transitive.error());
        }
        return InstalledPackage{std::move(version), std::move(*transitive)};
    }

    // ── stage 3: hoist, then put the packages where the tree says ───────────
    //
    // `Lockfile::hoist` (lockfile.rs:1470-1522) with `BuilderMethod::Filter`,
    // which is what an *install* uses: it drops disabled dependencies and hoists
    // more aggressively as a result (Tree.rs:408-419). `Resolvable` is the
    // lockfile's method — it keeps everything resolvable across configuration
    // changes — and is not what should decide directories.
    //
    // `isFiltered` is deliberately left unset. bun's
    // `is_filtered_dependency_or_workspace` (Tree.rs:547-678) gates os/cpu/libc
    // and workspace filters at this point; mbun applies the os/cpu rule earlier,
    // in `select_version_`, where the packument that carries `os`/`cpu` has just
    // landed and the tarball has not been requested yet. A filtered edge arrives
    // here already unresolved, which `Filter` skips at Tree.rs:770-772 — the same
    // outcome by the same code path. The workspace half of that predicate is
    // DEFERRED with workspaces themselves.
    std::expected<void, Error> hoist_and_place_() {
        namespace lf = mbun::install::lockfile;
        namespace placer = mbun::install::node_modules_placer;

        lf::HoistGraph view{graph_.view()};
        auto hoisted{lf::hoist(view, lf::HoistMethod::Filter)};
        if (!hoisted) {
            return detail::fail(ErrorCode::UnsupportedDependency,
                                "dependency loop while building the node_modules tree");
        }
        // `hoisted->errors` are diagnostics, NOT failures. bun pushes them into
        // its `Log` and `continue`s (Tree.rs:796-810 for an unsafe folder name);
        // the one condition that actually aborts a build is the dependency loop,
        // and that came back as `unexpected` above. Latching them into `error_`
        // would fail installs bun completes — and the only one reachable here,
        // the unsafe-name check, has already been applied to every edge by
        // `enqueue_dep_`, which fails the dependency properly and with its own
        // wording. Routing them to a user-visible log is DEFERRED with the Log
        // itself; there is nothing here to print them to.
        (void)hoisted->errors;

        const std::vector<placer::Placement> placements{
            placer::plan_placements(*hoisted, view, root_)};

        std::size_t nonce{0};
        for (const placer::Placement& p : placements) {
            auto it{store_.find(p.pkg)};
            if (it == store_.end()) {
                // Resolved but never extracted. An optional dependency whose
                // tarball failed lands here (`fail_dep_` swallowed the error), so
                // it is not fatal — it simply is not on disk.
                continue;
            }
            if (auto put{placer::materialize(it->second, p.dir, nonce++)}; !put) {
                return detail::fail(ErrorCode::IoError, put.error());
            }
            if (auto bin{bins_.find(p.pkg)}; bin != bins_.end()) {
                link_bins_(bin->second, p.installName, p.nodeModules);
            }
            summary_.packages.push_back(
                std::format("{}@{}", p.installName, graph_.package_version(p.pkg)));
            ++summary_.installed;
        }

        // The store is an implementation detail of this run; leaving it behind
        // would put a `.mbun-store` directory in the user's node_modules holding
        // a second hard link to every file.
        std::error_code ec;
        std::filesystem::remove_all(storeRoot_, ec);
        return {};
    }
};

// Install `initial` (and their transitive dependency closure) into
// `<root>/node_modules`, flat/hoisted. Returns what was installed.
export Result install_tree(const std::filesystem::path& root,
                           std::span<const PendingDep> initial, const Options& options) {
    Installer installer{root, options};
    return installer.run(initial);
}

}  // namespace mbun::install::registry_install
