// lockfile/adapt.cppm — mbun.install.lockfile.adapt
//
// Bridges the binary lockfile's struct-of-arrays arena
// (`mbun::install::lockfile::Lockfile`) to the installer's model
// (`mbun::install::Lockfile`, the type `parse_text` produces).
//
// The two models exist for good reasons — the binary one mirrors bun's on-disk
// `MultiArrayList<Package>` byte-for-byte, the installer's is a string_view
// graph over an owned arena — and lockfile.cppm's header has always said they'd
// be "reconciled at wire-time". This is that reconciliation; without it a
// successfully parsed bun.lockb is unreachable from `mbun install`.
//
// Key derivation follows bun's tree structure rather than flattening by name:
// src/install/lockfile/Tree.rs models node_modules as a tree whose root (id 0)
// is the top-level node_modules directory. `Tree::dependencies` is a slice into
// `Buffers::hoisted_dependencies`, and each `DependencyID` in it indexes the
// parallel `Buffers::dependencies` (the edge) and `Buffers::resolutions` (the
// PackageID it resolved to). A package hoisted to the top level is keyed by its
// bare name; one nested under another is keyed "<parent-key>/<name>" — the same
// convention `install::Lockfile::resolve_dep` already expects (install.cppm:122).
// Flattening by name would silently drop duplicates: real lockfiles carry the
// same name at several versions (compat/bun/bench/postgres has lru-cache at 7.18.3 and
// 10.4.3).
export module mbun.install.lockfile.adapt;

import std;
import mbun.install;
export import mbun.install.lockfile.decode;

namespace mbun::install::lockfile {

// resolution::Tag -> the installer's Resolution enum.
constexpr mbun::install::Resolution to_install_tag(ResolutionTag t) noexcept {
    switch (t) {
        case ResolutionTag::Uninitialized: return mbun::install::Resolution::Uninitialized;
        case ResolutionTag::Root: return mbun::install::Resolution::Root;
        case ResolutionTag::Npm: return mbun::install::Resolution::Npm;
        case ResolutionTag::Folder: return mbun::install::Resolution::Folder;
        case ResolutionTag::LocalTarball: return mbun::install::Resolution::LocalTarball;
        case ResolutionTag::Github: return mbun::install::Resolution::Github;
        case ResolutionTag::Git: return mbun::install::Resolution::Git;
        case ResolutionTag::Symlink: return mbun::install::Resolution::Symlink;
        case ResolutionTag::Workspace: return mbun::install::Resolution::Workspace;
        case ResolutionTag::RemoteTarball: return mbun::install::Resolution::RemoteTarball;
        // bun's placeholder tag for URL imports (resolution.rs:944-961); it has
        // no installer counterpart and no writer emits it.
        case ResolutionTag::SingleFileModule: return mbun::install::Resolution::Uninitialized;
    }
    return mbun::install::Resolution::Uninitialized;
}

// Behavior bitflags -> DepKind. bun's Behavior is a bitset and an edge can carry
// several bits; the installer's DepKind is one-of, so this picks the same
// precedence bun's own reporting does (resolver_hooks.rs:189 NAMED_FLAGS order):
// a dev edge is dev even when also prod-marked, peer before optional.
constexpr mbun::install::DepKind to_dep_kind(const DecodedDependency& d) noexcept {
    if (d.is_dev()) {
        return mbun::install::DepKind::Dev;
    }
    if (d.is_peer()) {
        return mbun::install::DepKind::Peer;
    }
    if (d.is_optional()) {
        return mbun::install::DepKind::Optional;
    }
    return mbun::install::DepKind::Prod;
}

// Collect the edges of one package (a window into Buffers::dependencies).
std::vector<mbun::install::Dep> collect_deps_(const Lockfile& bin, Slice window) {
    std::vector<mbun::install::Dep> out;
    const auto& all{bin.buffers.dependencies};
    if (window.off > all.size() || window.len > all.size() - window.off) {
        return out;  // corrupt window: prefer an empty edge set over a read past the arena
    }
    out.reserve(window.len);
    for (std::uint32_t i{0}; i < window.len; ++i) {
        const DecodedDependency d{decode_dependency(all[window.off + i], bin.buffers.stringBytes)};
        out.push_back(mbun::install::Dep{
            .name = d.name,
            .version = d.literal,
            .kind = to_dep_kind(d),
        });
    }
    return out;
}

// ── the adapter ────────────────────────────────────────────────────────────
// String views borrow `bin`'s arena, so `bin` must outlive the result; the
// installer's `Lockfile` interns only the keys it synthesizes ("parent/name").
export mbun::install::Lockfile to_install_lockfile(const Lockfile& bin) {
    mbun::install::Lockfile out;
    out.lockfileVersion = bin.format.value;
    if (bin.savedConfigVersion) {
        out.configVersion = static_cast<std::uint32_t>(*bin.savedConfigVersion);
    }

    const std::size_t n{bin.packages.len()};
    const auto& sb{bin.buffers.stringBytes};

    // Walk the node_modules tree to key each package the way resolve_dep looks
    // it up. Trees are stored parent-before-child, but don't rely on it: resolve
    // each tree's path by chasing `parent` up to the root.
    const auto& trees{bin.buffers.trees};
    std::vector<std::string> treePath(trees.size());  // key prefix of each tree
    // The root tree (id 0) is the top-level node_modules: prefix "".
    for (std::size_t t{0}; t < trees.size(); ++t) {
        const Tree& tree{trees[t]};
        if (tree.parent == INVALID_TREE_ID || tree.dependencyId == ROOT_DEP_ID ||
            tree.dependencyId == INVALID_DEPENDENCY_ID) {
            continue;  // root tree keeps the empty prefix
        }
        if (tree.dependencyId >= bin.buffers.dependencies.size()) {
            continue;
        }
        const DecodedDependency edge{
            decode_dependency(bin.buffers.dependencies[tree.dependencyId], sb)};
        std::string prefix;
        if (tree.parent < treePath.size()) {
            prefix = treePath[tree.parent];
        }
        treePath[t] = prefix.empty() ? std::string{edge.name}
                                     : std::format("{}/{}", prefix, edge.name);
    }

    // PackageID -> key. A package reachable at several tree positions keeps the
    // shallowest key, which is the one `resolve_dep` falls back to.
    std::vector<std::optional<std::string>> keyOf(n);
    for (std::size_t t{0}; t < trees.size(); ++t) {
        const Tree& tree{trees[t]};
        const Slice window{tree.dependencies};
        const auto& hoisted{bin.buffers.hoistedDependencies};
        if (window.off > hoisted.size() || window.len > hoisted.size() - window.off) {
            continue;
        }
        for (std::uint32_t i{0}; i < window.len; ++i) {
            const DependencyID depId{hoisted[window.off + i]};
            if (depId >= bin.buffers.dependencies.size() ||
                depId >= bin.buffers.resolutions.size()) {
                continue;
            }
            const PackageID pkgId{bin.buffers.resolutions[depId]};
            if (pkgId >= n) {
                continue;  // unresolved edge (optional dep that was skipped)
            }
            const DecodedDependency edge{decode_dependency(bin.buffers.dependencies[depId], sb)};
            std::string key{treePath[t].empty()
                                ? std::string{edge.name}
                                : std::format("{}/{}", treePath[t], edge.name)};
            if (!keyOf[pkgId] || key.size() < keyOf[pkgId]->size()) {
                keyOf[pkgId] = std::move(key);
            }
        }
    }

    out.packages.reserve(n);
    for (std::size_t i{0}; i < n; ++i) {
        const PackageResolution& res{bin.packages.resolution[i]};
        const ResolutionTag tag{resolution_tag(res)};
        const std::string_view name{package_name(bin, i)};

        // Package 0 is the root project; it is not an installable node and has
        // no node_modules key.
        if (tag == ResolutionTag::Root) {
            continue;
        }

        mbun::install::Package p;
        p.name = name;
        p.tag = to_install_tag(tag);
        p.key = keyOf[i] ? out.intern_(std::string{*keyOf[i]}) : name;
        if (tag == ResolutionTag::Npm) {
            p.version = out.intern_(format_version(resolution_npm_version(res), sb));
            p.registry = resolution_npm_url(res, sb);
        } else {
            p.resolution = resolution_string(res, sb);
        }
        p.deps = collect_deps_(bin, bin.packages.dependencies[i]);
        out.packages.push_back(std::move(p));
    }
    out.build_index_();

    // The root workspace: package 0's edges are the root package.json's deps.
    if (n > 0) {
        mbun::install::Workspace root;
        root.path = {};  // "" == root workspace (install.cppm:102-108)
        root.name = package_name(bin, 0);
        root.deps = collect_deps_(bin, bin.packages.dependencies[0]);
        out.workspaces.push_back(std::move(root));
    }

    // Workspace members: keyed by name hash in the binary format, so recover the
    // path from `workspace_paths` and the version from `workspace_versions`.
    for (std::size_t i{0}; i < bin.workspacePathKeys.size(); ++i) {
        mbun::install::Workspace w;
        w.path = semver_string_slice(bin.workspacePathVals[i], sb);
        if (w.path.empty()) {
            continue;
        }
        const PackageNameHash hash{bin.workspacePathKeys[i]};
        for (std::size_t v{0}; v < bin.workspaceVersionKeys.size(); ++v) {
            if (bin.workspaceVersionKeys[v] == hash) {
                w.version = out.intern_(format_version(bin.workspaceVersionVals[v], sb));
                break;
            }
        }
        // Recover the member's name + edges from its package-table entry.
        for (std::size_t p{0}; p < n; ++p) {
            if (bin.packages.nameHash[p] == hash &&
                resolution_tag(bin.packages.resolution[p]) == ResolutionTag::Workspace) {
                w.name = package_name(bin, p);
                w.deps = collect_deps_(bin, bin.packages.dependencies[p]);
                break;
            }
        }
        out.workspaces.push_back(std::move(w));
    }

    // trusted_dependencies survives the binary format only as truncated 32-bit
    // hashes (bun.lockb.rs:538-543 stores the hash, never the name, and puts an
    // empty name back on load). There is no name to recover, so the installer's
    // name-keyed list stays empty rather than carrying fabricated entries.

    return out;
}

}  // namespace mbun::install::lockfile
