// lockfile/tree.cppm — mbun.install.lockfile.tree
//
// The hoisting tree builder: the thing that decides where in node_modules each
// resolved package physically lands. Port of bun
// src/install/lockfile/Tree.rs (1153 lines) — `Tree::process_subtree` (:685),
// `Tree::hoist_dependency` (:981), `Builder::clean` (:495) and
// `relative_path_and_depth` (:310) — driven the way `Lockfile::hoist`
// (lockfile.rs:1470-1522) drives them: seed the root subtree, then run the
// `TreeFiller` queue breadth-first, then flatten.
//
// WHY THIS EXISTS: a flat "first resolution of a name wins" installer cannot
// express a tree where two packages need different versions of the same
// dependency. bun's answer is not a flat map — it is this: each *edge*
// resolves independently (`resolutions[dep_id] -> PackageID`, so one name can
// hold many versions), and the tree then hoists each edge as far toward the
// root as it can go without colliding with a *different* resolution of the same
// name. Where it collides, the package nests under its dependent instead. That
// is why bun's itty-router install has `@typescript-eslint/eslint-plugin/
// node_modules/ignore@7.0.5` sitting under a top-level `ignore@5.3.1`.
//
// THE ORDER IS LOAD-BEARING. `DepSorter` (lockfile.rs:228-245) sorts each
// package's edges by `Behavior::cmp` (resolver_hooks.rs:266-288 — workspace <
// dev < optional < prod < peer) and then by name, bytewise ascending. Whoever
// is visited first gets the hoisted slot; everyone else nests. Sorting
// differently produces a tree that is still self-consistent and still wrong.
//
// This is a pure function: graph in, `{trees, hoistedDependencies}` out. It
// performs no I/O and reads no lockfile encoding, so it is testable against
// hand-built graphs and reusable by both the registry installer and a lockfile
// `hoist()`/`resolve()` path.
//
// MC++ notes vs the reference:
//   - Rust's `const METHOD: BuilderMethod` / `const AS_DEFINED: bool` generics
//     become `if constexpr` on template parameters, so the same monomorphization
//     collapse happens without the trait plumbing.
//   - `MultiArrayList<BuilderEntry>` is a plain vector of entries here; the
//     builder is short-lived and the SoA split buys nothing at this size.
//   - `is_filtered_dependency_or_workspace` (:547) stays a caller-supplied
//     predicate rather than dragging `PackageManager`/`WorkspaceFilter` in: the
//     builder calls it at exactly bun's call site (:756), the *rule* is the
//     caller's to port. Unset ⇒ nothing is filtered.
export module mbun.install.lockfile.tree;

import std;
import mbun.install.dependency;
import mbun.semver;
export import mbun.install.lockfile.model;
import mbun.install.lockfile.decode;

namespace mbun::install::lockfile {

using Dependency = mbun::install::dependency::Dependency;

// ── inputs ──────────────────────────────────────────────────────────────────

// `BuilderMethod` (Tree.rs:408-419).
export enum class HoistMethod : std::uint8_t {
    // Hoist, but include every dependency so it stays resolvable if the
    // configuration changes. What gets saved to a lockfile.
    Resolvable,
    // Filter out disabled dependencies (os/cpu/libc, omitted dependency types),
    // hoisting more aggressively as a result. Dependencies of a disabled package
    // are not included.
    Filter,
};

// The read-only graph the hoister walks — the fields `tree::Builder`
// (Tree.rs:424-450) borrows out of `Lockfile`, threaded explicitly.
//
// `dependencies` is the global edge array and `resolutions` is parallel to it:
// `resolutions[dep_id]` is the PackageID that edge resolved to, which is what
// lets one name hold several versions. `resolutionLists[pkg_id]` is the window
// of `dependencies` that package owns — bun's `items_resolutions()`, whose
// `begin()..end()` range *is* a range of dependency IDs (Tree.rs:728-732).
export struct HoistGraph {
    std::span<const Dependency> dependencies;
    // Mutable: the hoister binds unresolved optional peers in place
    // (Tree.rs:858, :886, :879, :899).
    std::span<PackageID> resolutions;
    std::span<const Slice> resolutionLists;

    // Per package. `packageResolutionTags` drives the Folder placement gate
    // (:836) and the peer `satisfies` check (:1064); the name/version columns
    // are read for the dependency-loop error text (:1094-1101) and the peer
    // version compare (:1065).
    std::span<const ResolutionTag> packageResolutionTags;
    std::span<const std::string_view> packageNames;
    std::span<const std::string_view> packageVersions;

    // `is_filtered_dependency_or_workspace` (:547). Only consulted when
    // METHOD == Filter. Unset ⇒ nothing is filtered.
    std::function<bool(DependencyID, PackageID)> isFiltered{};

    // `Lockfile::is_workspace_root_dependency` (lockfile.rs:951-953) is
    // `packages.items_dependencies()[0].contains(id)`. Package.rs:568-569
    // builds a package's `dependencies` and `resolutions` windows from the same
    // `(prev_len, end - prev_len)` pair, so `resolutionLists[0]` is that same
    // window and no extra column is needed.
    bool is_workspace_root_dependency(DependencyID id) const {
        return !resolutionLists.empty() && resolutionLists[0].contains(id);
    }
};

// `SubtreeError` (Tree.rs:153-159), minus OutOfMemory — allocation failure
// throws here rather than being a value.
export enum class HoistError : std::uint8_t { DependencyLoop };

// `Lockfile::hoist`'s two outputs (lockfile.rs:1518-1520): `buffers.trees` and
// `buffers.hoisted_dependencies`. Diagnostics that bun routes into its `Log`
// come back as `errors`.
export struct Hoisted {
    std::vector<Tree> trees;
    std::vector<DependencyID> hoistedDependencies;
    std::vector<std::string> errors;
};

// ── internals ───────────────────────────────────────────────────────────────

namespace detail {

// `HoistDependencyResult` (Tree.rs:133-151) flattened into a tagged struct.
enum class HoistKind : std::uint8_t {
    DependencyLoop,
    Hoisted,
    Resolve,
    ResolveReplace,
    ResolveLater,
    Placement,
};

struct HoistDependencyResult {
    HoistKind kind{HoistKind::DependencyLoop};
    PackageID resolveId{INVALID_PACKAGE_ID};      // Resolve
    TreeId id{INVALID_TREE_ID};                   // ResolveReplace / Placement
    DependencyID depId{INVALID_DEPENDENCY_ID};    // ResolveReplace
    bool bundled{false};                          // Placement

    static HoistDependencyResult dependency_loop() {
        return {HoistKind::DependencyLoop, {}, {}, {}, {}};
    }
    static HoistDependencyResult hoisted() {
        return {HoistKind::Hoisted, {}, {}, {}, {}};
    }
    static HoistDependencyResult resolve(PackageID id) {
        HoistDependencyResult out{};
        out.kind = HoistKind::Resolve;
        out.resolveId = id;
        return out;
    }
    static HoistDependencyResult resolve_replace(TreeId treeId, DependencyID depId) {
        HoistDependencyResult out{};
        out.kind = HoistKind::ResolveReplace;
        out.id = treeId;
        out.depId = depId;
        return out;
    }
    static HoistDependencyResult resolve_later() {
        return {HoistKind::ResolveLater, {}, {}, {}, {}};
    }
    static HoistDependencyResult placement(TreeId treeId, bool bundled) {
        HoistDependencyResult out{};
        out.kind = HoistKind::Placement;
        out.id = treeId;
        out.bundled = bundled;
        return out;
    }
};

// `FillItem` (Tree.rs:1142-1149).
struct FillItem {
    TreeId treeId{INVALID_TREE_ID};
    DependencyID dependencyId{INVALID_DEPENDENCY_ID};
    // If valid, dependencies will not hoist beyond this tree.
    TreeId hoistRootId{INVALID_TREE_ID};
};

// `BuilderEntry` (Tree.rs:452-455).
struct BuilderEntry {
    Tree tree{};
    std::vector<DependencyID> dependencies;
};

// Port of `tree::Builder` (Tree.rs:424-450) + the `Lockfile::hoist` driver.
template <HoistMethod METHOD>
class Builder {
private:
    const HoistGraph& graph_;
    std::vector<BuilderEntry> list_;
    std::deque<FillItem> queue_;  // `TreeFiller` — a heap-backed FIFO ring
    std::vector<DependencyID> sortBuf_;
    // `pending_optional_peers` (Tree.rs:444). Unresolved optional peers that may
    // resolve later; a dep-id *set* because the same unresolved edge can be
    // visited from several places in the tree before it resolves. Insertion
    // order is preserved (bun's ArrayHashMap does too) so the walk is
    // deterministic.
    std::map<PackageNameHash, std::vector<DependencyID>> pendingOptionalPeers_;
    std::vector<std::string> errors_;

public:
    explicit Builder(const HoistGraph& graph) : graph_{graph} {}

    std::expected<Hoisted, HoistError> run() {
        // `Tree::default()` — id INVALID_ID, so the first appended tree gets
        // parent INVALID_ID and id 0 (lockfile.rs:1508).
        Tree root{};
        if (auto ok{process_subtree_(root, ROOT_DEP_ID, INVALID_TREE_ID)}; !ok) {
            return std::unexpected(ok.error());
        }
        // Breadth-first (lockfile.rs:1511-1516).
        while (!queue_.empty()) {
            const FillItem item{queue_.front()};
            queue_.pop_front();
            // Copy the tree out rather than indexing in place: `process_subtree`
            // appends to `list_`, which may reallocate (bun's note at :1512).
            const Tree tree{list_[item.treeId].tree};
            if (auto ok{process_subtree_(tree, item.dependencyId, item.hoistRootId)}; !ok) {
                return std::unexpected(ok.error());
            }
        }
        return clean_();
    }

private:
    // `Builder::clean` (Tree.rs:495-537). Flattens the per-tree dependency lists
    // into one array and rewrites each tree's (off, len) window into it.
    Hoisted clean_() {
        Hoisted out{};
        std::size_t total{0};
        for (const BuilderEntry& entry : list_) {
            total += entry.tree.dependencies.len;
        }
        out.trees.reserve(list_.size());
        out.hoistedDependencies.reserve(total);

        for (BuilderEntry& entry : list_) {
            const std::uint32_t off{static_cast<std::uint32_t>(out.hoistedDependencies.size())};
            for (const DependencyID depId : entry.dependencies) {
                // Optional peers that never resolved are dropped here (:518-521).
                if (graph_.resolutions[depId] == INVALID_PACKAGE_ID) {
                    continue;
                }
                out.hoistedDependencies.push_back(depId);
            }
            Tree tree{entry.tree};
            tree.dependencies.off = off;
            tree.dependencies.len =
                static_cast<std::uint32_t>(out.hoistedDependencies.size()) - off;
            out.trees.push_back(tree);
        }
        out.errors = std::move(errors_);
        return out;
    }

    void report_error_(std::string message) {
        errors_.push_back(std::move(message));
    }

    // `Tree::process_subtree` (Tree.rs:685-969).
    std::expected<void, HoistError> process_subtree_(const Tree& self, DependencyID dependencyId,
                                                     TreeId hoistRootId) {
        const PackageID parentPkgId{dependencyId == ROOT_DEP_ID ? PackageID{0}
                                                                : graph_.resolutions[dependencyId]};
        if (parentPkgId >= graph_.resolutionLists.size()) {
            return {};
        }
        const Slice resolutionList{graph_.resolutionLists[parentPkgId]};
        if (resolutionList.len == 0) {
            return {};
        }

        const TreeId nextId{static_cast<TreeId>(list_.size())};
        list_.push_back(BuilderEntry{
            Tree{/*id=*/nextId, /*dependencyId=*/dependencyId, /*parent=*/self.id, /*deps=*/{}},
            {}});

        // `DepSorter` (lockfile.rs:228-245): behavior group, then name bytewise
        // ascending. This order decides who wins the hoisted slot.
        sortBuf_.clear();
        sortBuf_.reserve(resolutionList.len);
        for (std::uint32_t depId{resolutionList.begin()}; depId < resolutionList.end(); ++depId) {
            sortBuf_.push_back(depId);
        }
        std::ranges::sort(sortBuf_, [this](DependencyID a, DependencyID b) {
            return Dependency::is_less_than(graph_.dependencies[a], graph_.dependencies[b]);
        });

        // Iterate a snapshot: the loop body mutates builder state.
        const std::vector<DependencyID> sorted{sortBuf_};
        for (const DependencyID depId : sorted) {
            const PackageID pkgId{graph_.resolutions[depId]};

            if constexpr (METHOD == HoistMethod::Filter) {
                if (graph_.isFiltered && graph_.isFiltered(depId, parentPkgId)) {
                    continue;
                }
                // Unresolved packages are skipped when filtering — they already
                // had their chance to resolve (:770-772).
                if (pkgId == INVALID_PACKAGE_ID) {
                    continue;
                }
            }

            const Dependency& dependency{graph_.dependencies[depId]};

            // An empty alias has no `node_modules/<name>` folder to escape, so
            // it is not treated as unsafe (:796-810).
            if (!dependency.name.empty() &&
                !mbun::install::dependency::is_safe_install_folder_name(dependency.name)) {
                report_error_(std::format("Invalid dependency name \"{}\"", dependency.name));
                continue;
            }

            HoistDependencyResult hoisted{};
            if (dependency.behavior.is_bundled()) {
                // Don't hoist a bundled dependency (:814-819).
                hoisted = HoistDependencyResult::placement(nextId, /*bundled=*/true);
            } else if (pkgId == INVALID_PACKAGE_ID) {
                if (!dependency.behavior.is_optional_peer()) {
                    continue;  // skip unresolvable dependencies (:832-833)
                }
                auto r{hoist_dependency_<true>(nextId, hoistRootId, pkgId, depId)};
                if (!r) {
                    return std::unexpected(r.error());
                }
                hoisted = *r;
            } else if (pkgId < graph_.packageResolutionTags.size() &&
                       graph_.packageResolutionTags[pkgId] == ResolutionTag::Folder) {
                // Don't hoist a folder dependency (:836-841).
                hoisted = HoistDependencyResult::placement(nextId, /*bundled=*/false);
            } else {
                auto r{hoist_dependency_<true>(nextId, hoistRootId, pkgId, depId)};
                if (!r) {
                    return std::unexpected(r.error());
                }
                hoisted = *r;
            }

            switch (hoisted.kind) {
                case HoistKind::DependencyLoop:
                case HoistKind::Hoisted:
                    continue;

                case HoistKind::Resolve: {
                    graph_.resolutions[depId] = hoisted.resolveId;
                    bind_pending_peers_(dependency.name_hash, hoisted.resolveId);
                    break;
                }

                case HoistKind::ResolveReplace: {
                    graph_.resolutions[hoisted.depId] = pkgId;
                    bind_pending_peers_(dependency.name_hash, pkgId);
                    // The placed edge now points at this dependency instead
                    // (:902-910).
                    for (DependencyID& placed : list_[hoisted.id].dependencies) {
                        if (placed == hoisted.depId) {
                            placed = depId;
                        }
                    }
                    if (pkgId != INVALID_PACKAGE_ID && pkgId < graph_.resolutionLists.size() &&
                        graph_.resolutionLists[pkgId].len > 0) {
                        queue_.push_back(FillItem{hoisted.id, depId, hoistRootId});
                    }
                    break;
                }

                case HoistKind::ResolveLater: {
                    // Deduplicated with another unresolved optional peer while
                    // hoisting; remember it so it can be resolved later
                    // (:921-933).
                    std::vector<DependencyID>& peers{pendingOptionalPeers_[dependency.name_hash]};
                    if (std::ranges::find(peers, depId) == peers.end()) {
                        peers.push_back(depId);
                    }
                    break;
                }

                case HoistKind::Placement: {
                    list_[hoisted.id].dependencies.push_back(depId);
                    list_[hoisted.id].tree.dependencies.len += 1;
                    if (pkgId != INVALID_PACKAGE_ID && pkgId < graph_.resolutionLists.size() &&
                        graph_.resolutionLists[pkgId].len > 0) {
                        queue_.push_back(FillItem{
                            hoisted.id, depId,
                            // A bundled placement starts a new hoist root (:952).
                            hoisted.bundled ? hoisted.id : hoistRootId});
                    }
                    break;
                }
            }
        }

        // An empty subtree is popped again (:960-966).
        if (list_[nextId].tree.dependencies.len == 0) {
            list_.pop_back();
        }
        return {};
    }

    // `Resolve` / `ResolveReplace` both bind every optional peer parked under
    // this name (:867-882, :888-901).
    void bind_pending_peers_(PackageNameHash nameHash, PackageID pkgId) {
        auto it{pendingOptionalPeers_.find(nameHash)};
        if (it == pendingOptionalPeers_.end()) {
            return;
        }
        for (const DependencyID unresolved : it->second) {
            graph_.resolutions[unresolved] = pkgId;
        }
        pendingOptionalPeers_.erase(it);
    }

    // `Tree::hoist_dependency` (Tree.rs:981-1135). Does one of three things:
    //   1. de-duplicate (skip) the package        → Hoisted
    //   2. move the package to the top directory  → Placement
    //   3. leave it at the same relative directory → DependencyLoop
    template <bool AS_DEFINED>
    std::expected<HoistDependencyResult, HoistError> hoist_dependency_(TreeId selfId,
                                                                      TreeId hoistRootId,
                                                                      PackageID packageId,
                                                                      DependencyID inputDepId) {
        const Dependency& dependency{graph_.dependencies[inputDepId]};
        const Tree self{list_[selfId].tree};
        const PackageNameHash targetNameHash{dependency.name_hash};

        // `list_` is not mutated for the duration of this loop (the recursive
        // call happens after it), so the view is stable (:996-1003).
        const std::vector<DependencyID>& selfDeps{list_[selfId].dependencies};
        const std::size_t depsLen{
            std::min<std::size_t>(self.dependencies.len, selfDeps.size())};

        for (std::size_t i{0}; i < depsLen; ++i) {
            const DependencyID depId{selfDeps[i]};
            const Dependency& dep{graph_.dependencies[depId]};
            if (dep.name_hash != targetNameHash) {
                continue;
            }

            const PackageID resId{graph_.resolutions[depId]};

            if (resId == INVALID_PACKAGE_ID && packageId == INVALID_PACKAGE_ID) {
                // Both optional peers; resolve both later if either can (:1019-1025).
                return HoistDependencyResult::resolve_later();
            }
            if (resId == INVALID_PACKAGE_ID) {
                return HoistDependencyResult::resolve_replace(self.id, depId);
            }
            if (packageId == INVALID_PACKAGE_ID) {
                // Resolve this optional peer to the existing resolution (:1035-1040).
                return HoistDependencyResult::resolve(resId);
            }
            if (resId == packageId) {
                // Same package as the other — hoist (:1042-1045).
                return HoistDependencyResult::hoisted();
            }

            if constexpr (AS_DEFINED) {
                if (dep.behavior.is_dev() != dependency.behavior.is_dev()) {
                    // Only happens in workspaces and the root package, because
                    // dev dependencies are not included in other dependency
                    // types (:1047-1054).
                    return HoistDependencyResult::hoisted();
                }
            }

            // Either keep the dependency here, or hoist if the peer version
            // allows it.
            if (dependency.behavior.is_peer()) {
                if (dependency.version.tag == mbun::install::dependency::Tag::Npm) {
                    if (resId < graph_.packageResolutionTags.size() &&
                        graph_.packageResolutionTags[resId] == ResolutionTag::Npm &&
                        resId < graph_.packageVersions.size() &&
                        mbun::semver::satisfies(graph_.packageVersions[resId],
                                                dependency.version.npm.version)) {
                        return HoistDependencyResult::hoisted();
                    }
                }
                // Root dependencies are chosen by the user; let them hoist other
                // peers even when they don't satisfy the version (:1071-1076).
                if (graph_.is_workspace_root_dependency(depId)) {
                    return HoistDependencyResult::hoisted();
                }
            }

            if constexpr (AS_DEFINED) {
                if (!dep.behavior.is_peer()) {
                    report_error_(std::format(
                        "Package \"{}@{}\" has a dependency loop\n  Resolution: \"{}@{}\"\n  "
                        "Dependency: \"{}@{}\"",
                        package_name_(packageId), package_version_(packageId),
                        package_name_(resId), package_version_(resId), dependency.name,
                        dependency.version.literal));
                    return std::unexpected(HoistError::DependencyLoop);
                }
            }

            return HoistDependencyResult::dependency_loop();
        }

        // Not found in this tree — try hoisting into the parent (:1110-1128).
        if (self.parent != INVALID_TREE_ID && self.id != hoistRootId) {
            // `hoist_dependency_<false>` never returns an error: the only
            // DependencyLoop error site is gated on AS_DEFINED (:1119-1123).
            auto r{hoist_dependency_<false>(self.parent, hoistRootId, packageId, inputDepId)};
            if (!r) {
                return std::unexpected(r.error());
            }
            if constexpr (!AS_DEFINED) {
                return *r;
            } else {
                if (r->kind != HoistKind::DependencyLoop) {
                    return *r;
                }
            }
        }

        // Place the dependency in the current tree (:1130-1134).
        return HoistDependencyResult::placement(self.id, /*bundled=*/false);
    }

    std::string_view package_name_(PackageID id) const {
        return id < graph_.packageNames.size() ? graph_.packageNames[id] : std::string_view{};
    }
    std::string_view package_version_(PackageID id) const {
        return id < graph_.packageVersions.size() ? graph_.packageVersions[id] : std::string_view{};
    }
};

}  // namespace detail

// ── entry point ─────────────────────────────────────────────────────────────

// `Lockfile::hoist` (lockfile.rs:1470-1522). Builds the node_modules tree for
// `graph`, mutating `graph.resolutions` in place where optional peers bind.
export std::expected<Hoisted, HoistError> hoist(const HoistGraph& graph, HoistMethod method) {
    if (method == HoistMethod::Filter) {
        return detail::Builder<HoistMethod::Filter>{graph}.run();
    }
    return detail::Builder<HoistMethod::Resolvable>{graph}.run();
}

// `Lockfile::resolve` (lockfile.rs:1449-1451).
export std::expected<Hoisted, HoistError> resolve_trees(const HoistGraph& graph) {
    return hoist(graph, HoistMethod::Resolvable);
}

// ── paths ───────────────────────────────────────────────────────────────────

// `IteratorPathStyle` (Tree.rs:169-177).
export enum class TreePathStyle : std::uint8_t {
    // `node_modules/jquery/node_modules/zod`
    NodeModules,
    // `jquery/zod`, always posix separators
    PkgPath,
};

// Max number of node_modules folders (Tree.rs:71).
export inline constexpr std::size_t MAX_PATH_BYTES{4096};
export inline constexpr std::size_t MAX_TREE_DEPTH{(MAX_PATH_BYTES / std::string_view{"node_modules"}.size()) + 1};

// `Tree::folder_name` (Tree.rs:91-97).
export std::string_view tree_folder_name(const Tree& tree, std::span<const Dependency> deps) {
    if (tree.dependencyId == INVALID_DEPENDENCY_ID || tree.dependencyId >= deps.size()) {
        return {};
    }
    return deps[tree.dependencyId].name;
}

export struct TreePath {
    std::string path;
    std::size_t depth{0};
};

// `relative_path_and_depth` (Tree.rs:310-402). Walks `tree_id` up to the root
// collecting folder names, then emits them outermost-first.
export TreePath tree_relative_path(std::span<const Tree> trees, std::span<const Dependency> deps,
                                   TreeId treeId, TreePathStyle style) {
    TreePath out{};
    if (treeId >= trees.size()) {
        return out;
    }
    const Tree tree{trees[treeId]};

    if (style == TreePathStyle::NodeModules) {
        out.path = "node_modules";
    }
    if (tree.id == 0 || tree.id == INVALID_TREE_ID) {
        return out;
    }

    std::vector<TreeId> depthBuf;
    depthBuf.push_back(0);
    TreeId parentId{tree.id};
    while (parentId > 0 && parentId < trees.size()) {
        if (depthBuf.size() == MAX_TREE_DEPTH) {
            return out;  // malformed: path too long
        }
        depthBuf.push_back(parentId);
        parentId = trees[parentId].parent;
    }

    std::size_t depthBufLen{depthBuf.size() - 1};
    out.depth = depthBufLen;
    while (depthBufLen > 0) {
        if (style == TreePathStyle::PkgPath) {
            if (depthBufLen != out.depth) {
                out.path.push_back('/');
            }
        } else {
            out.path.push_back('/');
        }
        const std::string_view name{tree_folder_name(trees[depthBuf[depthBufLen]], deps)};
        out.path.append(name);
        if (style == TreePathStyle::NodeModules) {
            out.path.append("/node_modules");
        }
        --depthBufLen;
    }
    return out;
}

}  // namespace mbun::install::lockfile
