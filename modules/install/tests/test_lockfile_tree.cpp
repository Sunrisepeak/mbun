// test_lockfile_tree.cpp — mbun.install.lockfile.tree tests.
//
// Locks the hoisting semantics ported from .mbun/bun-ref/src/install/lockfile/
// Tree.rs: `process_subtree` (:685), `hoist_dependency` (:981), `Builder::clean`
// (:495), `relative_path_and_depth` (:310), and the `DepSorter` visit order
// (lockfile.rs:228-245 + resolver_hooks.rs:266-288).
//
// THE ORACLE, NOT SELF-CONSISTENCY. The graphs below are not invented shapes
// that happen to round-trip through this builder — the central ones are
// transcribed from what real bun 1.4.0 (.mbun/bin/bun-rust, the same version as
// the blueprint) actually put on disk for itty-router:
//
//     node_modules/ignore                                      -> 5.3.1
//     node_modules/@typescript-eslint/eslint-plugin/node_modules/ignore -> 7.0.5
//     node_modules/maxmin/node_modules/chalk                   -> 1.1.3
//     node_modules/maxmin/node_modules/chalk/node_modules/ansi-styles -> 2.2.1
//
// verified with `bun-rust install --ignore-scripts` + `bun-rust pm ls --all`.
// The assertions state where *bun* puts a package, so a builder that is merely
// self-consistent fails them.

import std;
import mbun.install.dependency;
import mbun.install.lockfile;

namespace dep = mbun::install::dependency;
namespace lf = mbun::install::lockfile;

namespace {

int gChecks{0};
int gFailures{0};

void check(bool ok, std::string_view what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::println("  FAIL: {}", what);
    }
}

// ── graph construction helpers ──────────────────────────────────────────────

// A tiny fixture that owns the strings and lays out the flat arrays the way
// bun's lockfile does: `dependencies` is one global edge array, each package
// owns a contiguous window of it, and `resolutions` is parallel to it.
struct GraphFixture {
    std::vector<std::unique_ptr<std::string>> strings;
    std::vector<dep::Dependency> dependencies;
    std::vector<lf::PackageID> resolutions;
    std::vector<lf::Slice> resolutionLists;
    std::vector<lf::ResolutionTag> packageTags;
    std::vector<std::string_view> packageNames;
    std::vector<std::string_view> packageVersions;

    std::string_view intern(std::string_view s) {
        strings.push_back(std::make_unique<std::string>(s));
        return *strings.back();
    }

    // Only equality of `name_hash` is load-bearing in the hoister, so any
    // deterministic hash of the name works.
    static std::uint64_t name_hash(std::string_view name) {
        return std::hash<std::string_view>{}(name);
    }

    // Register a package; returns its PackageID.
    lf::PackageID add_package(std::string_view name, std::string_view version,
                              lf::ResolutionTag tag = lf::ResolutionTag::Npm) {
        packageNames.push_back(intern(name));
        packageVersions.push_back(intern(version));
        packageTags.push_back(tag);
        // The window is claimed lazily by the first `add_edge` — packages are
        // declared before any edges exist, so it cannot be snapshotted here.
        resolutionLists.push_back(lf::Slice{0, 0});
        return static_cast<lf::PackageID>(packageNames.size() - 1);
    }

    // Append an edge to `owner`'s window. Edges for a package must be added
    // contiguously (as bun's Package.rs:568-569 builds them).
    lf::DependencyID add_edge(lf::PackageID owner, std::string_view name, std::string_view range,
                              lf::PackageID resolvesTo, std::uint8_t behavior) {
        dep::Dependency d{};
        d.name = intern(name);
        d.name_hash = name_hash(d.name);
        d.behavior = dep::Behavior{behavior};
        d.version.tag = dep::Tag::Npm;
        d.version.literal = intern(range);
        d.version.npm.name = d.name;
        d.version.npm.version = d.version.literal;
        if (resolutionLists[owner].len == 0) {
            resolutionLists[owner].off = static_cast<std::uint32_t>(dependencies.size());
        } else if (resolutionLists[owner].end() != dependencies.size()) {
            std::println("  FIXTURE BUG: edges for package {} are not contiguous", owner);
            ++gFailures;
        }
        dependencies.push_back(d);
        resolutions.push_back(resolvesTo);
        resolutionLists[owner].len += 1;
        return static_cast<lf::DependencyID>(dependencies.size() - 1);
    }

    lf::HoistGraph graph() {
        lf::HoistGraph g{};
        g.dependencies = dependencies;
        g.resolutions = resolutions;
        g.resolutionLists = resolutionLists;
        g.packageResolutionTags = packageTags;
        g.packageNames = packageNames;
        g.packageVersions = packageVersions;
        return g;
    }
};

constexpr std::uint8_t PROD{dep::Behavior::PROD};
constexpr std::uint8_t DEV{dep::Behavior::DEV};

// Render the built tree as the set of "<path>/<name>@<version>" strings — the
// same thing `bun pm ls --all` shows, which is what we compare against.
std::vector<std::string> layout(const lf::Hoisted& hoisted, GraphFixture& fx) {
    std::vector<std::string> out;
    for (const lf::Tree& tree : hoisted.trees) {
        const lf::TreePath path{lf::tree_relative_path(hoisted.trees, fx.dependencies, tree.id,
                                                       lf::TreePathStyle::NodeModules)};
        for (std::uint32_t i{tree.dependencies.begin()}; i < tree.dependencies.end(); ++i) {
            const lf::DependencyID depId{hoisted.hoistedDependencies[i]};
            const lf::PackageID pkgId{fx.resolutions[depId]};
            out.push_back(std::format("{}/{}@{}", path.path, fx.dependencies[depId].name,
                                      fx.packageVersions[pkgId]));
        }
    }
    std::ranges::sort(out);
    return out;
}

bool has(const std::vector<std::string>& layout, std::string_view entry) {
    return std::ranges::find(layout, entry) != layout.end();
}

// ── tests ───────────────────────────────────────────────────────────────────

// Two independent root deps both land in the top-level node_modules, and a
// package with no dependencies of its own creates no tree.
void test_flat_hoist() {
    GraphFixture fx;
    const lf::PackageID root{fx.add_package("root", "1.0.0", lf::ResolutionTag::Root)};
    const lf::PackageID a{fx.add_package("a", "1.0.0")};
    const lf::PackageID b{fx.add_package("b", "2.0.0")};
    fx.add_edge(root, "a", "^1", a, PROD);
    fx.add_edge(root, "b", "^2", b, PROD);

    auto g{fx.graph()};
    auto hoisted{lf::hoist(g, lf::HoistMethod::Resolvable)};
    check(hoisted.has_value(), "flat hoist succeeds");
    if (!hoisted) {
        return;
    }
    check(hoisted->trees.size() == 1, "leaf packages create no subtree");
    const auto l{layout(*hoisted, fx)};
    check(has(l, "node_modules/a@1.0.0"), "a is top-level");
    check(has(l, "node_modules/b@2.0.0"), "b is top-level");
}

// Two dependents on the SAME resolution dedupe to one top-level copy — bun's
// `res_id == package_id => Hoisted` (Tree.rs:1042-1045).
void test_dedupe_same_resolution() {
    GraphFixture fx;
    const lf::PackageID root{fx.add_package("root", "1.0.0", lf::ResolutionTag::Root)};
    const lf::PackageID a{fx.add_package("a", "1.0.0")};
    const lf::PackageID b{fx.add_package("b", "1.0.0")};
    const lf::PackageID c{fx.add_package("c", "1.0.0")};
    fx.add_edge(root, "a", "^1", a, PROD);
    fx.add_edge(root, "b", "^1", b, PROD);
    fx.add_edge(a, "c", "^1", c, PROD);
    fx.add_edge(b, "c", "^1", c, PROD);

    auto g{fx.graph()};
    auto hoisted{lf::hoist(g, lf::HoistMethod::Resolvable)};
    check(hoisted.has_value(), "dedupe hoist succeeds");
    if (!hoisted) {
        return;
    }
    const auto l{layout(*hoisted, fx)};
    check(has(l, "node_modules/c@1.0.0"), "shared c hoists to top level");
    check(std::ranges::count(l, std::string{"node_modules/c@1.0.0"}) == 1,
          "c appears exactly once");
    // a and b each get a subtree only if something nests under them; nothing does.
    check(hoisted->trees.size() == 1, "deduped dependency creates no nested tree");
}

// THE ORACLE CASE. Real bun 1.4.0 on itty-router puts ignore@5.3.1 at the top
// level and nests ignore@7.0.5 under @typescript-eslint/eslint-plugin, because
// the two edges resolve to *different* PackageIDs and only one can own the
// hoisted slot. A flat first-wins installer gives eslint-plugin the 5.3.1 it
// does not satisfy; this is the assertion that catches that.
void test_version_conflict_nests_under_dependent() {
    GraphFixture fx;
    const lf::PackageID root{fx.add_package("root", "1.0.0", lf::ResolutionTag::Root)};
    const lf::PackageID plugin{fx.add_package("@typescript-eslint/eslint-plugin", "8.38.0")};
    const lf::PackageID ignore5{fx.add_package("ignore", "5.3.1")};
    const lf::PackageID ignore7{fx.add_package("ignore", "7.0.5")};
    fx.add_edge(root, "@typescript-eslint/eslint-plugin", "^8", plugin, PROD);
    fx.add_edge(root, "ignore", "^5.3.1", ignore5, PROD);
    fx.add_edge(plugin, "ignore", "^7.0.5", ignore7, PROD);

    auto g{fx.graph()};
    auto hoisted{lf::hoist(g, lf::HoistMethod::Resolvable)};
    check(hoisted.has_value(), "conflict hoist succeeds");
    if (!hoisted) {
        return;
    }
    const auto l{layout(*hoisted, fx)};
    check(has(l, "node_modules/ignore@5.3.1"), "bun: ignore@5.3.1 is top-level");
    check(has(l, "node_modules/@typescript-eslint/eslint-plugin/node_modules/ignore@7.0.5"),
          "bun: ignore@7.0.5 nests under @typescript-eslint/eslint-plugin");
    // The whole point: both versions exist. A flat installer has only one.
    check(std::ranges::count_if(l, [](const std::string& s) {
              return s.find("/ignore@") != std::string::npos;
          }) == 2,
          "both ignore versions are installed");
}

// THE ORACLE CASE, 3 levels: bun nests maxmin -> chalk@1.1.3 -> ansi-styles@2.2.1
// while a newer chalk/ansi-styles owns the top level.
void test_three_level_nesting() {
    GraphFixture fx;
    const lf::PackageID root{fx.add_package("root", "1.0.0", lf::ResolutionTag::Root)};
    const lf::PackageID maxmin{fx.add_package("maxmin", "2.1.0")};
    const lf::PackageID chalk4{fx.add_package("chalk", "4.1.2")};
    const lf::PackageID chalk1{fx.add_package("chalk", "1.1.3")};
    const lf::PackageID styles4{fx.add_package("ansi-styles", "4.3.0")};
    const lf::PackageID styles2{fx.add_package("ansi-styles", "2.2.1")};
    fx.add_edge(root, "chalk", "^4", chalk4, PROD);
    fx.add_edge(root, "maxmin", "^2", maxmin, PROD);
    fx.add_edge(maxmin, "chalk", "^1.1.3", chalk1, PROD);
    fx.add_edge(chalk4, "ansi-styles", "^4", styles4, PROD);
    fx.add_edge(chalk1, "ansi-styles", "^2.2.1", styles2, PROD);

    auto g{fx.graph()};
    auto hoisted{lf::hoist(g, lf::HoistMethod::Resolvable)};
    check(hoisted.has_value(), "three-level hoist succeeds");
    if (!hoisted) {
        return;
    }
    const auto l{layout(*hoisted, fx)};
    check(has(l, "node_modules/chalk@4.1.2"), "chalk@4 top-level");
    check(has(l, "node_modules/maxmin/node_modules/chalk@1.1.3"),
          "bun: chalk@1.1.3 nests under maxmin");
    check(has(l, "node_modules/ansi-styles@4.3.0"), "ansi-styles@4 top-level");
    check(has(l, "node_modules/maxmin/node_modules/chalk/node_modules/ansi-styles@2.2.1"),
          "bun: ansi-styles@2.2.1 nests under maxmin/chalk");
}

// WIRING TEST — this is the one that fails if the DepSorter sort is deleted.
//
// `Behavior::cmp` puts dev before prod (resolver_hooks.rs:266-288), so even
// though "b" (prod) is declared FIRST in the edge array, the dev edge "a" is
// visited first and its c@1.0.0 wins the top-level slot. Remove the
// `std::ranges::sort(sortBuf_, ...)` call in `process_subtree_` and declaration
// order takes over: c@2.0.0 hoists instead and this test fails.
void test_depsorter_order_decides_the_winner() {
    GraphFixture fx;
    const lf::PackageID root{fx.add_package("root", "1.0.0", lf::ResolutionTag::Root)};
    const lf::PackageID b{fx.add_package("b", "1.0.0")};
    const lf::PackageID a{fx.add_package("a", "1.0.0")};
    const lf::PackageID c1{fx.add_package("c", "1.0.0")};
    const lf::PackageID c2{fx.add_package("c", "2.0.0")};
    // Declaration order: prod "b" first, dev "a" second.
    fx.add_edge(root, "b", "^1", b, PROD);
    fx.add_edge(root, "a", "^1", a, DEV);
    fx.add_edge(a, "c", "^1", c1, PROD);
    fx.add_edge(b, "c", "^2", c2, PROD);

    auto g{fx.graph()};
    auto hoisted{lf::hoist(g, lf::HoistMethod::Resolvable)};
    check(hoisted.has_value(), "depsorter hoist succeeds");
    if (!hoisted) {
        return;
    }
    const auto l{layout(*hoisted, fx)};
    check(has(l, "node_modules/c@1.0.0"),
          "DepSorter: dev-edge 'a' sorts before prod 'b', so its c@1.0.0 hoists");
    check(has(l, "node_modules/b/node_modules/c@2.0.0"),
          "DepSorter: the later-visited prod edge nests its c@2.0.0");
}

// A dev and a non-dev edge on the same name with different resolutions
// de-duplicate rather than nest (Tree.rs:1047-1054) — "will only happen in
// workspaces and the root package".
void test_dev_vs_prod_same_name_dedupes() {
    GraphFixture fx;
    const lf::PackageID root{fx.add_package("root", "1.0.0", lf::ResolutionTag::Root)};
    const lf::PackageID x1{fx.add_package("x", "1.0.0")};
    const lf::PackageID x2{fx.add_package("x", "2.0.0")};
    fx.add_edge(root, "x", "^1", x1, DEV);
    fx.add_edge(root, "x", "^2", x2, PROD);

    auto g{fx.graph()};
    auto hoisted{lf::hoist(g, lf::HoistMethod::Resolvable)};
    check(hoisted.has_value(), "dev/prod split hoist succeeds");
    if (!hoisted) {
        return;
    }
    const auto l{layout(*hoisted, fx)};
    // dev sorts first and takes the slot; the prod edge is Hoisted (skipped).
    check(l.size() == 1, "dev/prod same-name collision yields a single placement");
    check(has(l, "node_modules/x@1.0.0"), "the dev edge (sorted first) owns the slot");
}

// Two same-name edges with the same behavior and different resolutions on one
// package is bun's hard `DependencyLoop` error (Tree.rs:1079-1104).
void test_dependency_loop_is_an_error() {
    GraphFixture fx;
    const lf::PackageID root{fx.add_package("root", "1.0.0", lf::ResolutionTag::Root)};
    const lf::PackageID x1{fx.add_package("x", "1.0.0")};
    const lf::PackageID x2{fx.add_package("x", "2.0.0")};
    fx.add_edge(root, "x", "^1", x1, PROD);
    fx.add_edge(root, "x", "^2", x2, PROD);

    auto g{fx.graph()};
    auto hoisted{lf::hoist(g, lf::HoistMethod::Resolvable)};
    check(!hoisted.has_value() && hoisted.error() == lf::HoistError::DependencyLoop,
          "same-behavior same-name conflict on one package is a DependencyLoop error");
}

// An unsafe folder name is reported and skipped, not fatal (Tree.rs:796-810).
void test_unsafe_name_is_skipped_not_fatal() {
    GraphFixture fx;
    const lf::PackageID root{fx.add_package("root", "1.0.0", lf::ResolutionTag::Root)};
    const lf::PackageID bad{fx.add_package("..", "1.0.0")};
    const lf::PackageID good{fx.add_package("good", "1.0.0")};
    fx.add_edge(root, "..", "^1", bad, PROD);
    fx.add_edge(root, "good", "^1", good, PROD);

    auto g{fx.graph()};
    auto hoisted{lf::hoist(g, lf::HoistMethod::Resolvable)};
    check(hoisted.has_value(), "unsafe name does not fail the build");
    if (!hoisted) {
        return;
    }
    check(hoisted->errors.size() == 1, "unsafe name is reported");
    const auto l{layout(*hoisted, fx)};
    check(l.size() == 1 && has(l, "node_modules/good@1.0.0"), "only the safe dependency is placed");
}

// A folder-resolution dependency is never hoisted (Tree.rs:836-841): it is
// placed in the tree being built, not lifted into an ancestor.
void test_folder_dependency_is_not_hoisted() {
    GraphFixture fx;
    const lf::PackageID root{fx.add_package("root", "1.0.0", lf::ResolutionTag::Root)};
    const lf::PackageID a{fx.add_package("a", "1.0.0")};
    const lf::PackageID local{fx.add_package("local", "1.0.0", lf::ResolutionTag::Folder)};
    fx.add_edge(root, "a", "^1", a, PROD);
    fx.add_edge(a, "local", "file:./local", local, PROD);

    auto g{fx.graph()};
    auto hoisted{lf::hoist(g, lf::HoistMethod::Resolvable)};
    check(hoisted.has_value(), "folder dep hoist succeeds");
    if (!hoisted) {
        return;
    }
    const auto l{layout(*hoisted, fx)};
    check(has(l, "node_modules/a/node_modules/local@1.0.0"),
          "folder dependency stays under its dependent instead of hoisting");
}

// `Builder::clean` drops edges that never resolved (Tree.rs:518-521) — the
// optional peers that nothing supplied.
void test_clean_drops_unresolved_edges() {
    GraphFixture fx;
    const lf::PackageID root{fx.add_package("root", "1.0.0", lf::ResolutionTag::Root)};
    const lf::PackageID a{fx.add_package("a", "1.0.0")};
    fx.add_edge(root, "a", "^1", a, PROD);
    // An unresolved OPTIONAL PEER edge: bun keeps it in the tree's list while
    // building, then clean() drops it because it never bound.
    fx.add_edge(root, "missing-peer", "^1", lf::INVALID_PACKAGE_ID,
                static_cast<std::uint8_t>(dep::Behavior::OPTIONAL | dep::Behavior::PEER));

    auto g{fx.graph()};
    auto hoisted{lf::hoist(g, lf::HoistMethod::Resolvable)};
    check(hoisted.has_value(), "unresolved optional peer hoist succeeds");
    if (!hoisted) {
        return;
    }
    const auto l{layout(*hoisted, fx)};
    check(l.size() == 1 && has(l, "node_modules/a@1.0.0"),
          "an optional peer that never resolved is not installed");
}

// An unresolved optional peer binds to whatever another edge supplied for the
// same name — bun's `Resolve` (Tree.rs:1035-1040 + :855-883).
void test_optional_peer_binds_to_existing_package() {
    GraphFixture fx;
    const lf::PackageID root{fx.add_package("root", "1.0.0", lf::ResolutionTag::Root)};
    const lf::PackageID plugin{fx.add_package("plugin", "1.0.0")};
    const lf::PackageID host{fx.add_package("host", "3.0.0")};
    fx.add_edge(root, "host", "^3", host, PROD);
    fx.add_edge(root, "plugin", "^1", plugin, PROD);
    const lf::DependencyID peerEdge{
        fx.add_edge(plugin, "host", "^3", lf::INVALID_PACKAGE_ID,
                    static_cast<std::uint8_t>(dep::Behavior::OPTIONAL | dep::Behavior::PEER))};

    auto g{fx.graph()};
    auto hoisted{lf::hoist(g, lf::HoistMethod::Resolvable)};
    check(hoisted.has_value(), "optional peer bind hoist succeeds");
    if (!hoisted) {
        return;
    }
    check(fx.resolutions[peerEdge] == host,
          "the unresolved optional peer bound to the package the root supplied");
}

// A peer edge whose range the existing resolution satisfies hoists rather than
// nesting (Tree.rs:1059-1069).
void test_peer_satisfied_by_existing_resolution_hoists() {
    GraphFixture fx;
    const lf::PackageID root{fx.add_package("root", "1.0.0", lf::ResolutionTag::Root)};
    const lf::PackageID plugin{fx.add_package("plugin", "1.0.0")};
    const lf::PackageID host2{fx.add_package("host", "2.5.0")};
    const lf::PackageID host2other{fx.add_package("host", "2.9.0")};
    fx.add_edge(root, "host", "2.5.0", host2, PROD);
    fx.add_edge(root, "plugin", "^1", plugin, PROD);
    // plugin peers on ^2 — host@2.5.0 satisfies it, so it must NOT nest a copy.
    fx.add_edge(plugin, "host", "^2", host2other,
                static_cast<std::uint8_t>(dep::Behavior::PEER));

    auto g{fx.graph()};
    auto hoisted{lf::hoist(g, lf::HoistMethod::Resolvable)};
    check(hoisted.has_value(), "peer satisfies hoist succeeds");
    if (!hoisted) {
        return;
    }
    const auto l{layout(*hoisted, fx)};
    check(!has(l, "node_modules/plugin/node_modules/host@2.9.0"),
          "a satisfied peer does not nest a second copy");
    check(has(l, "node_modules/host@2.5.0"), "the satisfying host stays top-level");
}

// `relative_path_and_depth` (Tree.rs:310-402) for both path styles.
void test_tree_paths() {
    GraphFixture fx;
    const lf::PackageID root{fx.add_package("root", "1.0.0", lf::ResolutionTag::Root)};
    const lf::PackageID maxmin{fx.add_package("maxmin", "2.1.0")};
    const lf::PackageID chalk4{fx.add_package("chalk", "4.1.2")};
    const lf::PackageID chalk1{fx.add_package("chalk", "1.1.3")};
    const lf::PackageID styles4{fx.add_package("ansi-styles", "4.3.0")};
    const lf::PackageID styles2{fx.add_package("ansi-styles", "2.2.1")};
    fx.add_edge(root, "chalk", "^4", chalk4, PROD);
    fx.add_edge(root, "maxmin", "^2", maxmin, PROD);
    fx.add_edge(maxmin, "chalk", "^1.1.3", chalk1, PROD);
    // chalk@4's own ansi-styles@4 must claim the top-level slot, otherwise
    // ansi-styles@2.2.1 hoists there and no nested chalk tree is created at all.
    // (Verified against bun: itty-router really does have a top-level
    // ansi-styles@4.3.0 alongside maxmin/chalk/node_modules/ansi-styles@2.2.1.)
    fx.add_edge(chalk4, "ansi-styles", "^4", styles4, PROD);
    fx.add_edge(chalk1, "ansi-styles", "^2.2.1", styles2, PROD);

    auto g{fx.graph()};
    auto hoisted{lf::hoist(g, lf::HoistMethod::Resolvable)};
    check(hoisted.has_value(), "path hoist succeeds");
    if (!hoisted) {
        return;
    }
    check(lf::tree_relative_path(hoisted->trees, fx.dependencies, 0,
                                 lf::TreePathStyle::NodeModules)
                  .path == "node_modules",
          "the root tree is 'node_modules'");
    check(lf::tree_relative_path(hoisted->trees, fx.dependencies, 0, lf::TreePathStyle::PkgPath)
                  .path.empty(),
          "the root tree has an empty pkg path");

    // Find the deepest tree and check both renderings.
    bool sawDeep{false};
    for (const lf::Tree& t : hoisted->trees) {
        const lf::TreePath nm{lf::tree_relative_path(hoisted->trees, fx.dependencies, t.id,
                                                     lf::TreePathStyle::NodeModules)};
        if (nm.path == "node_modules/maxmin/node_modules/chalk/node_modules") {
            sawDeep = true;
            const lf::TreePath pkg{lf::tree_relative_path(hoisted->trees, fx.dependencies, t.id,
                                                          lf::TreePathStyle::PkgPath)};
            check(pkg.path == "maxmin/chalk", "PkgPath style joins names with '/'");
            check(nm.depth == 2, "depth counts the nesting level");
        }
    }
    check(sawDeep, "the nested chalk tree exists");
}

// METHOD == Filter consults the caller's predicate at bun's call site
// (Tree.rs:755-765) and drops the dependency *and its subtree*.
void test_filter_predicate_drops_subtree() {
    GraphFixture fx;
    const lf::PackageID root{fx.add_package("root", "1.0.0", lf::ResolutionTag::Root)};
    const lf::PackageID keep{fx.add_package("keep", "1.0.0")};
    const lf::PackageID drop{fx.add_package("drop", "1.0.0")};
    const lf::PackageID onlyChild{fx.add_package("only-child", "1.0.0")};
    fx.add_edge(root, "keep", "^1", keep, PROD);
    fx.add_edge(root, "drop", "^1", drop, PROD);
    fx.add_edge(drop, "only-child", "^1", onlyChild, PROD);

    auto g{fx.graph()};
    g.isFiltered = [&fx](lf::DependencyID depId, lf::PackageID) {
        return fx.dependencies[depId].name == "drop";
    };
    auto hoisted{lf::hoist(g, lf::HoistMethod::Filter)};
    check(hoisted.has_value(), "filtered hoist succeeds");
    if (!hoisted) {
        return;
    }
    const auto l{layout(*hoisted, fx)};
    check(has(l, "node_modules/keep@1.0.0"), "unfiltered dependency survives");
    check(!has(l, "node_modules/drop@1.0.0"), "filtered dependency is dropped");
    check(!has(l, "node_modules/only-child@1.0.0"),
          "dependencies of a filtered package are not included");
    // Resolvable keeps everything — the same graph, the other method.
    auto resolvable{lf::hoist(g, lf::HoistMethod::Resolvable)};
    check(resolvable.has_value() && has(layout(*resolvable, fx), "node_modules/drop@1.0.0"),
          "Resolvable keeps what Filter drops");
}

}  // namespace

int main() {
    test_flat_hoist();
    test_dedupe_same_resolution();
    test_version_conflict_nests_under_dependent();
    test_three_level_nesting();
    test_depsorter_order_decides_the_winner();
    test_dev_vs_prod_same_name_dedupes();
    test_dependency_loop_is_an_error();
    test_unsafe_name_is_skipped_not_fatal();
    test_folder_dependency_is_not_hoisted();
    test_clean_drops_unresolved_edges();
    test_optional_peer_binds_to_existing_package();
    test_peer_satisfied_by_existing_resolution_hoists();
    test_tree_paths();
    test_filter_predicate_drops_subtree();

    std::println("test_lockfile_tree: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
