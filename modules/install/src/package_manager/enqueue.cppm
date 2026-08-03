// package_manager/enqueue.cppm — mbun.install.package_manager.enqueue
//
// Pure-logic port of the resolution queue / graph-building state machine of
// bun src/install/PackageManager/PackageManagerEnqueue.rs +
// processDependencyList.rs + runTasks.rs (dedup half):
//
//   * TaskCallbackContext / TaskDependencyQueue — the "many dependencies wait
//     on one manifest download" callback registry, keyed by task id;
//   * the network dedupe map interplay (has_created_network_task upgrades
//     optional→required, is_network_task_required defaults to true);
//   * the walk itself: enqueue dependency → resolve against an (in-memory)
//     manifest → get-or-put the resolved package → enqueue *its* dependency
//     list (dependency_list_queue drain), with package dedup by
//     (name, resolved version);
//   * pending-task accounting (increment/decrement_pending_tasks).
//
// Seams (documented):
//   * Manifests arrive via `add_manifest`/`on_manifest_downloaded` instead of
//     an HTTP fetch. A missing manifest records the request descriptor id in
//     `pending_manifest_requests` — TODO: wire jsc __mbunNetNative (the bridge
//     performs network_task::for_manifest's GET and calls back
//     on_manifest_downloaded with the parsed npm::PackageManifest).
//   * Non-npm dependency tags (workspace/folder/symlink/git/tarball) go
//     through folder/git resolvers + extraction in the reference; here they
//     are recorded in `unresolved` (execution-layer work).
//   * Lockfile integration (get_or_put_resolved_package's lockfile fast path)
//     is deferred to the wave-1 lockfile module wiring.
export module mbun.install.package_manager.enqueue;

import std;
import mbun.install.dependency;
import mbun.install.npm.manifest;
import mbun.install.npm.version_map;
import mbun.install.network_task;
import mbun.install.package_manager.options;

namespace mbun::install::package_manager {

namespace dep = mbun::install::dependency;
namespace npm = mbun::install::npm;
namespace net = mbun::install::network_task;

export using PackageID = std::uint32_t;
export using DependencyID = std::uint32_t;
export constexpr PackageID ENQUEUE_INVALID_PACKAGE_ID{0xFFFF'FFFFU};

// Port of TaskCallbackContext (the Dependency / RootDependency arms that
// process_dependency_list_item consumes; the JS-callback arms are runtime).
export struct TaskCallbackContext {
    enum class Kind : std::uint8_t { Dependency, RootDependency } kind{Kind::Dependency};
    DependencyID dependency_id{0};
};

export using TaskCallbackList = std::vector<TaskCallbackContext>;
export using TaskDependencyQueue = std::unordered_map<std::uint64_t, TaskCallbackList>;

// A resolved package row (lockfile Package subset the walk needs).
export struct ResolvedPackage {
    std::string name;
    std::string version;         // canonical resolved version
    std::uint32_t dep_begin{0};  // range into ResolutionWalker::dependencies
    std::uint32_t dep_count{0};
};

// ── ResolutionWalker ────────────────────────────────────────────────────────
// The manager-state slice that drives resolution: dependency buffer +
// parallel resolution buffer (lockfile.buffers.{dependencies,resolutions}),
// the task queue, the dedupe map and the package store.
export class ResolutionWalker {
  public:
    Features features{.optional_dependencies = true};

    // ── graph state (lockfile buffer analogs) ──
    std::vector<dep::Dependency> dependencies;
    std::vector<PackageID> resolutions;  // parallel to `dependencies`
    std::vector<ResolvedPackage> packages;

    // ── manager task state ──
    TaskDependencyQueue task_queue;
    net::DedupeMap network_dedupe_map;
    // Manifest names whose GET must be issued (network seam, dedup'd).
    std::vector<std::string> pending_manifest_requests;
    // Dependencies whose tag needs a non-npm resolver (folder/git/... seam).
    std::vector<DependencyID> unresolved;

    std::uint32_t total_tasks{0};
    std::uint32_t pending_tasks{0};

    void add_manifest(const npm::PackageManifest* m) {
        manifests_.insert_or_assign(m->name, m);
    }

    DependencyID append_dependency(const dep::Dependency& d) {
        dependencies.push_back(d);
        resolutions.push_back(ENQUEUE_INVALID_PACKAGE_ID);
        return static_cast<DependencyID>(dependencies.size() - 1);
    }

    // Port of enqueue_dependency_with_main (npm/dist-tag arm). Root deps use
    // TaskCallbackContext::Kind::RootDependency (assign_root_resolution path).
    void enqueue_dependency(DependencyID dep_id, bool is_root) {
        worklist_.push_back({dep_id, is_root});
        drain();
    }

    // Port of the manifest half of runTasks + process_dependency_list: a
    // downloaded manifest resolves every dependency parked on its task id.
    void on_manifest_downloaded(const npm::PackageManifest* m) {
        add_manifest(m);
        std::uint64_t task_id{net::task_id::for_manifest(m->name)};
        auto it{task_queue.find(task_id)};
        if (it == task_queue.end()) {
            return;
        }
        TaskCallbackList list{std::move(it->second)};
        task_queue.erase(it);
        network_dedupe_map.remove(task_id);
        if (pending_tasks > 0) {
            --pending_tasks;  // decrement_pending_tasks
        }
        for (const TaskCallbackContext& ctx : list) {
            worklist_.push_back(
                {ctx.dependency_id, ctx.kind == TaskCallbackContext::Kind::RootDependency});
        }
        drain();
    }

    bool is_done() const {
        return pending_tasks == 0 && worklist_.empty();
    }

    const ResolvedPackage* resolution_of(DependencyID dep_id) const {
        PackageID id{resolutions[dep_id]};
        return id == ENQUEUE_INVALID_PACKAGE_ID ? nullptr : &packages[id];
    }

  private:
    struct WorkItem {
        DependencyID dep_id;
        bool is_root;
    };

    std::unordered_map<std::string, const npm::PackageManifest*> manifests_;
    // Package dedup: (name '\n' version) → PackageID (get_or_put_resolved_package).
    std::unordered_map<std::string, PackageID> package_index_;
    // dependency_list_queue analog — breadth-first drain, matching the
    // reference's `lockfile.scratch.dependency_list_queue` FIFO.
    std::deque<WorkItem> worklist_;
    bool draining_{false};

    void drain() {
        if (draining_) {
            return;  // re-entrant enqueue from resolve step; outer loop drains
        }
        draining_ = true;
        while (!worklist_.empty()) {
            WorkItem item{worklist_.front()};
            worklist_.pop_front();
            enqueue_one(item.dep_id, item.is_root);
        }
        draining_ = false;
    }

    void enqueue_one(DependencyID dep_id, bool is_root) {
        const dep::Dependency& d{dependencies[dep_id]};
        switch (d.version.tag) {
            case dep::Tag::Npm:
            case dep::Tag::DistTag: {
                std::string name{d.realname()};
                auto mit{manifests_.find(name)};
                if (mit != manifests_.end()) {
                    resolve_from_manifest(dep_id, d, *mit->second);
                    return;
                }
                // Manifest not in memory: park this dependency on the manifest
                // task id; the first parker creates the network task
                // (has_created_network_task), later parkers only append their
                // callback (the reference's task_queue.getOrPut dedup).
                std::uint64_t task_id{net::task_id::for_manifest(name)};
                bool is_required{!d.behavior.is_optional()};
                bool existed{
                    network_dedupe_map.has_created_network_task(task_id, is_required)};
                task_queue[task_id].push_back(TaskCallbackContext{
                    is_root ? TaskCallbackContext::Kind::RootDependency
                            : TaskCallbackContext::Kind::Dependency,
                    dep_id});
                if (!existed) {
                    pending_manifest_requests.push_back(std::move(name));
                    ++total_tasks;
                    ++pending_tasks;  // increment_pending_tasks
                }
                return;
            }
            default:
                // Workspace / folder / symlink / git / tarball resolvers are
                // execution-layer seams (see module header).
                unresolved.push_back(dep_id);
                return;
        }
    }

    void resolve_from_manifest(DependencyID dep_id, const dep::Dependency& d,
                               const npm::PackageManifest& m) {
        npm::FindResult found{d.version.tag == dep::Tag::DistTag
                                  ? npm::find_by_dist_tag(m, d.version.dist_tag.tag)
                                  : npm::find_best_version(m, d.version.npm.version)};
        if (!found) {
            // DistTagVersionNotFound / NoMatchingVersion — the reference logs
            // and (for required deps) fails the install; surface via
            // `unresolved` so callers can report.
            unresolved.push_back(dep_id);
            return;
        }
        resolutions[dep_id] = get_or_put_resolved_package(m.name, found);
    }

    // Port of get_or_put_resolved_package: dedup by (name, resolved version);
    // a new package appends its dependency list and enqueues it (the
    // "enqueue dependency → resolve → enqueue its deps" walk).
    PackageID get_or_put_resolved_package(std::string_view name, const npm::FindResult& found) {
        std::string key;
        key.reserve(name.size() + found.version.size() + 1);
        key.append(name).push_back('\n');
        key.append(found.version);
        auto it{package_index_.find(key)};
        if (it != package_index_.end()) {
            return it->second;
        }

        PackageID id{static_cast<PackageID>(packages.size())};
        packages.push_back(ResolvedPackage{std::string{name}, std::string{found.version},
                                           static_cast<std::uint32_t>(dependencies.size()), 0});
        package_index_.emplace(std::move(key), id);

        std::uint32_t count{0};
        count += append_dep_map(found.package->dependencies, dep::Behavior{dep::Behavior::PROD});
        if (features.optional_dependencies) {
            count += append_dep_map(found.package->optional_dependencies,
                                    dep::Behavior{dep::Behavior::OPTIONAL});
        }
        if (features.peer_dependencies) {
            count += append_dep_map(found.package->peer_dependencies,
                                    dep::Behavior{dep::Behavior::PEER});
        }
        // dev_dependencies of remote packages are never installed (abbreviated
        // manifests don't even carry them) — matches the reference.
        packages[id].dep_count = count;
        return id;
    }

    std::uint32_t append_dep_map(const npm::DepMap& map, dep::Behavior behavior) {
        std::uint32_t appended{0};
        for (const auto& [dep_name, spec] : map) {
            std::optional<dep::Version> v{dep::parse(dep_name, spec)};
            if (!v) {
                continue;  // reference logs a warning and skips
            }
            dep::Dependency child{};
            child.name = dep_name;  // views into the manifest, which outlives us
            child.name_hash = fnv(dep_name);
            child.version = *v;
            child.behavior = behavior;
            DependencyID child_id{append_dependency(child)};
            worklist_.push_back({child_id, false});
            ++appended;
        }
        return appended;
    }

    static constexpr std::uint64_t fnv(std::string_view s) {
        std::uint64_t h{0xcbf29ce484222325ULL};
        for (char c : s) {
            h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
            h *= 0x100000001b3ULL;
        }
        return h;
    }
};

}  // namespace mbun::install::package_manager
