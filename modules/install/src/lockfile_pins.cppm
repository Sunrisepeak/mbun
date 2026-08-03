// lockfile_pins.cppm — mbun.install.lockfile_pins
//
// Which registry resolutions the lockfile *pins*, and which it does not.
//
// Without this, `mbun install` re-resolved every registry edge against the live
// packument and installed whatever `find_best_version` picked — a lockfile
// pinning `typescript@5.8.3` still got 5.9.3. bun does not do that, and the
// mechanism it uses instead is worth naming precisely, because it is not a
// "prefer the lockfile" flag anywhere in the codebase.
//
// ── how bun actually pins ───────────────────────────────────────────────────
// bun never asks "is this dep pinned?". It loads the lockfile into
// `Lockfile.packages` + `buffers.resolutions`, and resolution is simply a no-op
// for any edge that already has a PackageID:
//
//   PackageManagerEnqueue.rs:2308-2313 — `get_or_put_resolved_package`:
//       if (resolution as usize) < this.lockfile.packages.len() {
//           return Ok(Some(ResolvedPackageResult { package: ..., .. }));
//       }
//   — returns the locked package *before* the manifest is ever consulted
//   (the `match version.tag` that reads `this.manifests` is below it, :2315+).
//
// So the real question bun answers is the inverse: *which locked resolutions
// survive into the new lockfile*. That is `Diff::generate`
// (lockfile/Package.rs:1053), which fills `id_mapping[to_dep_i] = from_dep_i`,
// and install_with_manager.rs:390-391 carries the PackageID across:
//       if mapping[i] != invalid_package_id {
//           lf.resolutions[off + i] = old_resolutions[mapping[i]];
//       }
// Only edges left at `invalid_package_id` are enqueued at all
// (install_with_manager.rs:539-541). A preserved edge never hits the network.
//
// `Diff::generate` preserves the mapping in exactly two cases:
//
//   1. lockfile/Package.rs:1459-1480 — `Dependency::eql(to_dep, from_dep)`:
//      the package.json literal is byte-identical to the one the lockfile
//      recorded. Mapping preserved unconditionally (no satisfies check).
//
//   2. lockfile/Package.rs:1591-1627 — the literal CHANGED, but the locked
//      resolution still satisfies the new range:
//          if to_dep.version.tag == Npm && from_pkg_resolution.tag == Npm
//             && to_dep.version.npm().version.satisfies(from_pkg_resolution.npm().version, ..)
//          { mapping[cur_to_i] = i as PackageID; }
//      The comment there states the intent outright: "If only the *version
//      literal* changed and the previously-resolved package still satisfies the
//      new range, keep the existing resolution. Otherwise widening a range
//      (e.g. `"4.0.0"` -> `"*"`) re-resolves to latest ... This matches npm's
//      sticky-lockfile behaviour".
//
// Both are gated off for an explicit `bun update <pkg>` target
// (Package.rs:1604-1607 and :1464-1477) — the user asked for a fresh resolve.
// `mbun install` has no update-request path, so that gate is not modelled here;
// when `mbun update` grows one it belongs in `pin_for`'s caller.
//
// ── overrides vs the pin ────────────────────────────────────────────────────
// Not "override always wins" and not "lockfile always wins" — it is keyed on
// whether the override SET CHANGED since the lockfile was written:
//
//   * unchanged (Package.rs:1093-1136 compares count, then key-by-key with
//     `Dependency::eql`): `summary.overrides_changed` stays false and NOTHING
//     re-resolves. The pin wins — and it is not a conflict, because the locked
//     resolution was itself produced under that same override.
//
//   * changed: install_with_manager.rs:485-504 walks EVERY edge at EVERY depth
//     and, for any name in `all_name_hashes` (the union of the old lockfile's
//     override keys and the new package.json's, built at :313-338), does
//         manager.lockfile.buffers.resolutions[dependency_i] = invalid_package_id;
//     then re-enqueues it. The pin is explicitly destroyed; the override wins.
//
// `overrides_changed` is deliberately name-keyed and depth-blind, matching
// OverrideMap's own note that there is "no fast way to trace a Dependency ID to
// its package" (lockfile/OverrideMap.rs:29-34).
//
// ── the one modelled deviation ──────────────────────────────────────────────
// bun skips the manifest GET for a pinned edge; this installer still fetches it
// and then asks for the pinned version exactly (`PendingDep::spec` is rewritten
// to the locked version, so `find_best_version` can only return that version).
// What gets INSTALLED is identical — the deviation is one HTTP request per
// pinned name, which mbun pays today regardless since it has no manifest cache
// (registry_install.cppm's DEFERRED note). Buying the skip requires integrity /
// os / cpu / bin / deps to come off the lockfile instead of the packument, and
// the binary lockfile's `Meta` column is not decoded yet (adapt.cppm), so the
// packument remains the only source for them. Fetch-then-pin keeps the os/cpu
// gate, SRI verification and dep discovery on the data that is actually there.
export module mbun.install.lockfile_pins;

import std;
import mbun.install;
import mbun.install.override_map;
import mbun.semver;

namespace mbun::install::lockfile_pins {

// Is `key` a TOP-LEVEL lockfile entry — i.e. is it an installName rather than a
// "<parent>/<name>" nesting path?
//
// A bare '/' test does not answer this, because a scoped package's own name
// contains one: `@nodelib/fs.walk` hoisted to the top level is keyed exactly
// that, and rejecting it would silently refuse to pin every scoped package in
// the tree (in itty-router: all of @eslint/*, @jridgewell/*, @typescript-eslint/*).
// The node_modules layout is the discriminator — `node_modules/@scope/name` is
// one package directory, which is the same rule `registry_install`'s
// `module_dir` and `is_safe_install_folder_name` already apply — so a scoped key
// is top-level with exactly one slash, an unscoped key with none.
constexpr bool is_top_level_key_(std::string_view key) noexcept {
    if (key.empty()) {
        return false;
    }
    const std::size_t first{key.find('/')};
    if (key.front() == '@') {
        // "@scope/name" => top-level; "@scope/name/dep" or "@a/b/@c/d" => nested.
        return first != std::string_view::npos &&
               key.find('/', first + 1) == std::string_view::npos;
    }
    return first == std::string_view::npos;
}

// The node_modules FOLDER a lockfile key installs into — its last path
// component, which is what the installer knows an edge by (`installName`).
//
// A key is a `/`-joined sequence of components, each either "name" or
// "@scope/name" ("ignore", "globby/ignore",
// "@typescript-eslint/eslint-plugin/ignore",
// "@typescript-eslint/utils/@eslint-community/eslint-utils"). Neither "text
// after the last slash" nor "text after the last /@" gets all four right, so
// walk the components left to right and keep the last.
constexpr std::string_view install_name_of_key_(std::string_view key) noexcept {
    std::string_view rest{key};
    std::string_view last{};
    while (!rest.empty()) {
        std::size_t take{rest.size()};
        if (rest.front() == '@') {
            // A scope is only half a component: "@scope/name" is one folder.
            if (const std::size_t slash{rest.find('/')}; slash != std::string_view::npos) {
                const std::size_t next{rest.find('/', slash + 1)};
                take = next == std::string_view::npos ? rest.size() : next;
            }
        } else if (const std::size_t slash{rest.find('/')}; slash != std::string_view::npos) {
            take = slash;
        }
        last = rest.substr(0, take);
        rest = take >= rest.size() ? std::string_view{} : rest.substr(take + 1);
    }
    return last;
}

// One locked resolution. A single name can now hold several — `ignore` is 5.3.1
// at the top and 7.0.5 under @typescript-eslint/eslint-plugin — which is exactly
// what the nested tree exists to express.
struct Pin {
    std::string version;     // the locked npm version ("5.8.3")
    std::string literal;     // the literal the lockfile recorded for this ROOT edge
    bool hasLiteral{false};  // false => not a root dep, so case 1 cannot apply
    bool topLevel{false};    // keyed bare ("ignore"), not nested ("globby/ignore")
};

export class LockfilePins {
public:
    LockfilePins() = default;

    // `lf` is the loaded lockfile (text `bun.lock` via parse_text, or binary
    // `bun.lockb` via lockfile::adapt::to_install_lockfile — the same model
    // either way, which is why this treats both identically);
    // `pkgJsonOverrides` is the root package.json's overrides/resolutions.
    static LockfilePins build(const mbun::install::Lockfile& lf,
                              const mbun::install::override_map::OverrideMap& pkgJsonOverrides) {
        LockfilePins out;

        // NESTED KEYS PIN TOO. This used to keep only top-level entries, on the
        // grounds that a nested key was "a resolution this flat installer cannot
        // place anyway". The installer is no longer flat — it hoists per lockfile
        // tree and nests conflicting versions — and dropping the nested keys is
        // now a live bug rather than a documented gap: itty-router's lockfile
        // pins
        //
        //     "ignore"                                  -> 5.3.1
        //     "@typescript-eslint/eslint-plugin/ignore" -> 7.0.5
        //     "globby/ignore"                           -> 7.0.5
        //
        // and keeping only the first leaves the `^7.0.0` edges with a pin that
        // does not satisfy them, so they re-resolve against the live registry and
        // install 7.0.6. MEASURED: 20 of itty-router's 258 directories drifted off
        // the lockfile exactly this way before this changed; afterwards, zero.
        //
        // Indexing is by the key's LAST component — the node_modules folder,
        // which is what the installer calls `installName`. Not by `Package::name`:
        // an `npm:` alias (`"a": "npm:b@1"`) is keyed "a" (the folder) while its
        // name is "b", and the installer looks it up by "a".
        for (const mbun::install::Package& p : lf.packages) {
            if (p.tag != mbun::install::Resolution::Npm) {
                continue;  // only npm resolutions carry a semver to pin
            }
            if (p.version.empty() || p.key.empty()) {
                continue;
            }
            const std::string_view folder{install_name_of_key_(p.key)};
            if (folder.empty()) {
                continue;
            }
            out.pins_[std::string{folder}].push_back(
                Pin{.version = std::string{p.version}, .topLevel = is_top_level_key_(p.key)});
        }

        // The literals the lockfile recorded for the ROOT workspace's own edges
        // — the `from_deps` side of `Diff::generate`'s `Dependency::eql`
        // (Package.rs:1459). Absent for a transitive edge, which is why `Pin`
        // carries `hasLiteral` instead of an empty-string sentinel: an edge with
        // no recorded literal must fall through to the satisfies rule, not
        // compare equal to "". The literal belongs to the TOP-LEVEL pin: it is
        // the root's own edge that eql compares against, and a nested entry is by
        // definition somebody else's edge.
        if (const mbun::install::Workspace* root{lf.root_workspace()}) {
            for (const mbun::install::Dep& d : root->deps) {
                const auto it{out.pins_.find(d.name)};
                if (it == out.pins_.end()) {
                    continue;
                }
                for (Pin& pin : it->second) {
                    if (pin.topLevel) {
                        pin.literal.assign(d.version);
                        pin.hasLiteral = true;
                    }
                }
            }
        }

        out.build_override_invalidation_(lf, pkgJsonOverrides);
        return out;
    }

    bool empty() const noexcept { return pins_.empty(); }

    // bun's per-edge answer. `installName` is the node_modules folder (the
    // lockfile key's last component); `range` is the literal as written on the
    // edge being resolved. Returns the version to pin to, or nullopt to resolve
    // normally.
    std::optional<std::string_view> pin_for(std::string_view installName,
                                            std::string_view range) const {
        // The override rewrote this name's range, so the locked resolution was
        // produced under a *different* override set and must not be reused
        // (install_with_manager.rs:485-504 zeroes exactly these edges).
        if (overrideInvalidated_.contains(installName)) {
            return std::nullopt;
        }
        const auto it{pins_.find(installName)};
        if (it == pins_.end()) {
            return std::nullopt;
        }
        const std::vector<Pin>& candidates{it->second};

        // Case 1 — Package.rs:1459: literal unchanged => preserve, no satisfies
        // check. A hand-edited lockfile whose pin does not satisfy its own
        // recorded literal is still preserved, exactly as bun preserves it. Only
        // a top-level pin carries a literal, so this cannot fire on a nested one.
        for (const Pin& pin : candidates) {
            if (pin.hasLiteral && pin.literal == range) {
                return pin.version;
            }
        }
        // Case 2 — Package.rs:1617: literal changed but a locked resolution still
        // satisfies the new range => preserve it.
        //
        // With nesting there can be SEVERAL locked resolutions for one folder
        // name, so this picks the one that fits the edge in hand: itty-router's
        // `ignore@^5.2.0` edges take 5.3.1 and its `ignore@^7.0.0` edges take
        // 7.0.5, out of the same lockfile.
        //
        // DEVIATION, named — and it is the one thing standing between this and a
        // byte-identical itty-router tree. bun does not answer this per NAME at
        // all: `Diff::generate` matches each edge structurally against the
        // lockfile's list for the same package and carries the PackageID across
        // per DependencyID (Package.rs:1053, install_with_manager.rs:390-391), so
        // it never has to guess. This is a name-keyed approximation, and it is
        // only visible when two locked versions BOTH satisfy one range: terser's
        // `acorn@^8.8.2` is satisfied by the hoisted acorn@8.15.0 AND by the
        // locked terser/acorn@8.11.3, and bun keeps the latter. The top-level pin
        // wins that tie here, because it is the resolution the hoisted slot holds
        // and therefore the one an unconstrained edge de-duplicates onto.
        // MEASURED cost: 11 of itty-router's 258 directories, all of this shape.
        // Closing it needs the pin keyed by (parent, name) — see the handoff note
        // in docs/design/20260716-nested-dependency-tree-per-edge-resolution.md;
        // a first attempt regressed the tree because the (parent -> deps) data it
        // needs is not trustworthy on the bun.lockb path yet.
        const Pin* fallback{nullptr};
        for (const Pin& pin : candidates) {
            if (!mbun::semver::satisfies(pin.version, range)) {
                continue;
            }
            if (pin.topLevel) {
                return pin.version;
            }
            if (fallback == nullptr) {
                fallback = &pin;
            }
        }
        if (fallback != nullptr) {
            return fallback->version;
        }
        // Nothing locked fits: the range moved off every pin (`^5` -> `^6`). bun
        // leaves the mapping invalid and the edge re-resolves against the
        // registry.
        return std::nullopt;
    }

private:
    // `summary.overrides_changed` + `all_name_hashes`, by name instead of by
    // hash (Package.rs:1093-1136 and install_with_manager.rs:313-338/:485-504).
    void build_override_invalidation_(
        const mbun::install::Lockfile& lf,
        const mbun::install::override_map::OverrideMap& pkgJsonOverrides) {
        // bun compares count first, then key-by-key in sorted order. Both sides
        // here are already name-keyed maps, so compare them as maps: same keys
        // AND same literals => unchanged.
        bool changed{lf.overrides.size() != pkgJsonOverrides.map.size()};
        if (!changed) {
            for (const auto& [name, literal] : lf.overrides) {
                const std::string* current{pkgJsonOverrides.get(name)};
                if (current == nullptr || *current != literal) {
                    changed = true;
                    break;
                }
            }
        }
        if (!changed) {
            return;  // pins survive untouched
        }
        // The union of both key sets — a REMOVED override must re-resolve just
        // as an added one does, which is why bun unions rather than taking the
        // new map's keys (install_with_manager.rs:322-337).
        for (const auto& [name, literal] : lf.overrides) {
            overrideInvalidated_.emplace(name);
        }
        for (const auto& [name, literal] : pkgJsonOverrides.map) {
            overrideInvalidated_.insert(name);
        }
    }

    std::map<std::string, std::vector<Pin>, std::less<>> pins_;
    std::set<std::string, std::less<>> overrideInvalidated_;
};

}  // namespace mbun::install::lockfile_pins
