// install_graph.cppm — mbun.install.install_graph
//
// The per-edge resolution graph the installer accumulates while it resolves,
// and hands to the hoister (`mbun.install.lockfile.tree`) once it is complete.
// This is the missing upstream half of nested node_modules: a tree builder is
// useless without a graph in which one *name* can hold several *versions*, and
// a flat "first resolution of a name wins" installer never builds one.
//
// Blueprint (ref: .mbun/bun-ref/src/install/):
//   * `Lockfile::package_index` (lockfile.rs) — name-hash -> PackageID(s),
//     `PackageIndexEntry::Id | ::Ids`. Modelled here as `nameIndex_`.
//   * `get_or_put_resolved_package` (PackageManagerEnqueue.rs:2189-2313) — the
//     early return at :2308-2313 is keyed on a *PackageID already existing*, not
//     on a name having been seen. `get_or_put` is that rule.
//   * `Package.rs:568-569` — a package's `dependencies` / `resolutions` windows
//     are built from the same `(prev_len, end - prev_len)` pair, so an edge
//     window is contiguous and `resolutions[dep_id]` is parallel to it. That
//     contiguity is why `open_window` takes every edge of a package at once and
//     is callable only once per package: the task-driven pipeline completes out
//     of order, so edges cannot be appended as they finish.
//   * `String::hash` (semver/lib.rs:763 `bun_wyhash::hash(str)`) — name_hash is
//     wyhash seed 0, which `mbun.crypto.wyhash` already is bit-for-bit.
//
// WHAT `claimed_` BECAME. The old installer kept `std::set<std::string>
// claimed_` and returned early when a *name* was already spoken for
// (registry_install.cppm:549). That set carried two unrelated jobs:
//   1. the flat hoist decision ("this name is taken, drop the edge") — which is
//      the tree's job, and is gone;
//   2. concurrent manifest/tarball de-duplication ("this is already in flight"),
//      which is still load-bearing under the async engine.
// Job 2 survives here, re-keyed from `name` to `(name, version)`: `get_or_put`
// returns `inserted == false` for a resolution that already has a PackageID, and
// the caller skips re-fetching its tarball and re-walking its dependencies. A
// separate `claimedVersions_` set would be a second copy of `nameIndex_`'s key,
// so the index *is* the dedupe — exactly as it is in bun, where the early return
// is a `package_index` hit.
export module mbun.install.install_graph;

import std;
import mbun.crypto.wyhash;
import mbun.install.dependency;
import mbun.install.lockfile.decode;  // ResolutionTag
export import mbun.install.lockfile.tree;

namespace mbun::install::install_graph {

namespace lf = mbun::install::lockfile;
namespace dep = mbun::install::dependency;

using Dependency = dep::Dependency;

// bun's `String::hash` — wyhash with seed 0 (semver/lib.rs:763).
export std::uint64_t name_hash(std::string_view name) {
    return mbun::crypto::wyhash(
        {reinterpret_cast<const std::uint8_t*>(name.data()), name.size()});
}

// One dependency edge, in the terms the resolver already has them: the folder
// name it installs under, the registry name it resolves against (they differ for
// an `npm:` alias) and the spec that selects a version.
export struct EdgeSpec {
    std::string_view name;      // installName — the node_modules folder
    std::string_view realName;  // registry name; empty ⇒ same as `name`
    std::string_view spec;      // semver range / dist-tag / remote tarball URL
    dep::Tag tag{dep::Tag::Npm};
    dep::Behavior behavior{};
};

export class InstallGraph {
private:
    // Every `string_view` in `edges_` / `pkgNames_` / `pkgVersions_` points in
    // here. std::set nodes are address-stable, so views stay valid across
    // inserts — which a vector<string> would not give.
    std::set<std::string, std::less<>> arena_;

    // The global edge array and its parallel resolution column.
    // `resolutions_[depId]` is the PackageID that edge resolved to; several
    // edges named "ignore" can point at different PackageIDs, which is the whole
    // point.
    std::vector<Dependency> edges_;
    std::vector<lf::PackageID> resolutions_;

    // Package columns (bun's `MultiArrayList<Package>` subset the hoister reads).
    std::vector<std::string_view> pkgNames_;
    std::vector<std::string_view> pkgVersions_;
    std::vector<lf::ResolutionTag> pkgTags_;
    std::vector<lf::Slice> pkgDeps_;
    std::vector<bool> pkgWindowOpen_;

    // `Lockfile::package_index` — see the module header on `claimed_`.
    std::map<std::string_view, std::vector<lf::PackageID>> nameIndex_;

public:
    std::string_view intern(std::string_view s) {
        if (auto it{arena_.find(s)}; it != arena_.end()) {
            return *it;
        }
        return *arena_.insert(std::string{s}).first;
    }

    std::size_t package_count() const noexcept { return pkgNames_.size(); }
    std::size_t edge_count() const noexcept { return edges_.size(); }

    std::string_view package_name(lf::PackageID id) const { return pkgNames_[id]; }
    std::string_view package_version(lf::PackageID id) const { return pkgVersions_[id]; }
    lf::ResolutionTag package_tag(lf::PackageID id) const { return pkgTags_[id]; }

    const Dependency& edge(lf::DependencyID id) const { return edges_[id]; }
    lf::PackageID resolution(lf::DependencyID id) const { return resolutions_[id]; }
    void resolve_edge(lf::DependencyID id, lf::PackageID pkg) { resolutions_[id] = pkg; }

    // `package_index` lookup by name. Empty ⇒ nothing has resolved under it yet.
    std::span<const lf::PackageID> ids_for_name(std::string_view name) const {
        auto it{nameIndex_.find(name)};
        if (it == nameIndex_.end()) {
            return {};
        }
        return it->second;
    }

    struct PutResult {
        lf::PackageID id{lf::INVALID_PACKAGE_ID};
        // false ⇒ this (name, version) already had a PackageID; the caller must
        // NOT re-fetch its tarball or re-walk its dependencies. bun's early
        // return at PackageManagerEnqueue.rs:2308-2313.
        bool inserted{false};
    };

    // `get_or_put_resolved_package`, keyed by the resolution — (name, version) —
    // rather than by name. The scan over `nameIndex_[name]` is bun's walk of
    // `PackageIndexEntry::Ids`; the lists are a handful of entries at most.
    PutResult get_or_put(std::string_view name, std::string_view version,
                         lf::ResolutionTag tag) {
        const std::string_view internedName{intern(name)};
        std::vector<lf::PackageID>& ids{nameIndex_[internedName]};
        for (const lf::PackageID id : ids) {
            if (pkgVersions_[id] == version && pkgTags_[id] == tag) {
                return PutResult{id, false};
            }
        }
        const auto id{static_cast<lf::PackageID>(pkgNames_.size())};
        pkgNames_.push_back(internedName);
        pkgVersions_.push_back(intern(version));
        pkgTags_.push_back(tag);
        pkgDeps_.push_back(lf::Slice{0, 0});
        pkgWindowOpen_.push_back(false);
        ids.push_back(id);
        return PutResult{id, true};
    }

    // The root is PackageID 0 — `process_subtree` seeds `ROOT_DEP_ID ->
    // parentPkgId = 0` (Tree.rs:691-694), so nothing else may take that slot.
    // It goes into `nameIndex_` like any package, which is what lets a peer edge
    // naming this project bind to it instead of fetching a copy of it from the
    // registry (bun keeps the root in `package_index` for the same reason).
    lf::PackageID put_root(std::string_view name, std::string_view version) {
        const PutResult r{get_or_put(name, version, lf::ResolutionTag::Root)};
        return r.id;
    }

    // Claim `pkg`'s contiguous window of the edge array (Package.rs:568-569) and
    // return the dependency IDs in it. Call once per package, with every edge it
    // owns: the window must be contiguous, and the pipeline resolves out of
    // order, so edges cannot trickle in.
    std::vector<lf::DependencyID> open_window(lf::PackageID pkg,
                                              std::span<const EdgeSpec> specs) {
        // Defensive: a second window would silently overwrite the first and
        // corrupt every later package's view of its own edges.
        if (pkgWindowOpen_[pkg]) {
            return {};
        }
        pkgWindowOpen_[pkg] = true;
        const auto off{static_cast<std::uint32_t>(edges_.size())};
        std::vector<lf::DependencyID> out;
        out.reserve(specs.size());
        for (const EdgeSpec& spec : specs) {
            out.push_back(static_cast<lf::DependencyID>(edges_.size()));
            edges_.push_back(make_edge_(spec));
            resolutions_.push_back(lf::INVALID_PACKAGE_ID);
        }
        pkgDeps_[pkg] = lf::Slice{off, static_cast<std::uint32_t>(specs.size())};
        return out;
    }

    // The read-only view the hoister walks. Rebuilt on demand because every
    // column may have grown; `resolutions` stays mutable so the hoister can bind
    // unresolved optional peers in place (Tree.rs:858/:886).
    lf::HoistGraph view() {
        lf::HoistGraph out{};
        out.dependencies = edges_;
        out.resolutions = resolutions_;
        out.resolutionLists = pkgDeps_;
        out.packageResolutionTags = pkgTags_;
        out.packageNames = pkgNames_;
        out.packageVersions = pkgVersions_;
        return out;
    }

private:
    Dependency make_edge_(const EdgeSpec& spec) {
        Dependency out{};
        out.name = intern(spec.name);
        out.name_hash = name_hash(out.name);
        out.behavior = spec.behavior;
        out.version.tag = spec.tag;
        out.version.literal = intern(spec.spec);
        const std::string_view real{
            intern(spec.realName.empty() ? spec.name : spec.realName)};
        switch (spec.tag) {
            case dep::Tag::Npm:
                out.version.npm.name = real;
                out.version.npm.version = out.version.literal;
                out.version.npm.is_alias = real != out.name;
                break;
            case dep::Tag::DistTag:
                out.version.dist_tag.name = real;
                out.version.dist_tag.tag = out.version.literal;
                break;
            case dep::Tag::Tarball:
                out.version.tarball.package_name = real;
                out.version.tarball.uri =
                    dep::URI{dep::URI::Kind::Remote, out.version.literal};
                break;
            default:
                break;
        }
        return out;
    }
};

}  // namespace mbun::install::install_graph
