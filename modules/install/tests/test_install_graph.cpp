// test_install_graph.cpp — mbun.install.install_graph + mbun.install.node_modules_placer
//
// Stage 2 (per-edge resolution) and stage 3 (turning hoisted trees into
// directories), tested where they are pure: a graph goes in, a list of absolute
// node_modules paths comes out. No socket, no filesystem.
//
// THE ORACLE, NOT SELF-CONSISTENCY. The expected paths below are what real bun
// 1.4.0 (.mbun/bin/bun-rust — the same version as the blueprint in
// .mbun/bun-ref) actually put on disk for itty-router, read back with
// `bun-rust pm ls --all` and by walking node_modules:
//
//     node_modules/chalk                                              -> 4.1.2
//     node_modules/ansi-styles                                        -> 4.3.0
//     node_modules/maxmin                                             -> 2.1.0
//     node_modules/maxmin/node_modules/chalk                          -> 1.1.3
//     node_modules/maxmin/node_modules/chalk/node_modules/ansi-styles -> 2.2.1
//
// reproduced with:
//     git clone --depth 1 https://github.com/kwhitley/itty-router
//     cd itty-router && bun-rust install --ignore-scripts   # keep the committed bun.lockb
//     bun-rust pm ls --all
//
// The committed bun.lockb matters: without it the ranges re-resolve against
// today's registry and you get 5.3.2/7.0.6 instead of the 5.3.1/7.0.5 the
// project is actually pinned to. An install of *this project* is the lockfile's
// install.

import std;
import mbun.install.dependency;
import mbun.install.install_graph;
import mbun.install.lockfile;
import mbun.install.node_modules_placer;

namespace dep = mbun::install::dependency;
namespace ig = mbun::install::install_graph;
namespace lf = mbun::install::lockfile;
namespace placer = mbun::install::node_modules_placer;

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

void check_eq(std::string_view got, std::string_view want, std::string_view what) {
    ++gChecks;
    if (got != want) {
        ++gFailures;
        std::println("  FAIL: {}\n    want: {}\n    got:  {}", what, want, got);
    }
}

dep::Behavior prod() { return dep::Behavior{dep::Behavior::PROD}; }

ig::EdgeSpec npm_edge(std::string_view name, std::string_view range) {
    return ig::EdgeSpec{name, name, range, dep::Tag::Npm, prod()};
}

// ── stage 2: one name, several versions ─────────────────────────────────────

void test_per_edge_resolution_holds_two_versions_of_one_name() {
    ig::InstallGraph graph{};
    graph.put_root("app", "1.0.0");

    const std::vector<ig::EdgeSpec> rootEdges{npm_edge("ignore", "^5.0.0"),
                                              npm_edge("ignore", "^7.0.0")};
    const std::vector<lf::DependencyID> ids{graph.open_window(0, rootEdges)};
    check(ids.size() == 2, "graph: root's window holds both edges");

    // The point of stage 2: each edge resolves on its own, so the same NAME can
    // hold two PackageIDs. The flat installer returned early on the second edge
    // and it never existed.
    const auto a{graph.get_or_put("ignore", "5.3.1", lf::ResolutionTag::Npm)};
    const auto b{graph.get_or_put("ignore", "7.0.5", lf::ResolutionTag::Npm)};
    check(a.inserted && b.inserted, "graph: two versions of one name both get a PackageID");
    check(a.id != b.id, "graph: ignore@5.3.1 and ignore@7.0.5 are different packages");
    graph.resolve_edge(ids[0], a.id);
    graph.resolve_edge(ids[1], b.id);
    check(graph.resolution(ids[0]) != graph.resolution(ids[1]),
          "graph: the two edges resolve to different packages");

    // ...and de-duplication is keyed on the RESOLUTION, not on the name. This is
    // what `claimed_` became: a third edge asking for 5.3.1 reuses the PackageID
    // rather than re-downloading it (PackageManagerEnqueue.rs:2308-2313).
    const auto again{graph.get_or_put("ignore", "5.3.1", lf::ResolutionTag::Npm)};
    check(!again.inserted, "graph: an already-resolved (name, version) is not re-created");
    check(again.id == a.id, "graph: it reuses the original PackageID");

    // `package_index` — name -> every PackageID resolved under it. A peer edge
    // binds through this (get_or_put_resolved_package, :2203-2218).
    check(graph.ids_for_name("ignore").size() == 2, "graph: package_index lists both versions");
    check(graph.ids_for_name("nonexistent").empty(), "graph: unknown name indexes to nothing");
}

void test_edge_windows_are_contiguous_and_open_once() {
    ig::InstallGraph graph{};
    graph.put_root("app", "1.0.0");
    const auto pkg{graph.get_or_put("foo", "1.0.0", lf::ResolutionTag::Npm)};

    const std::vector<ig::EdgeSpec> rootEdges{npm_edge("foo", "^1.0.0")};
    const std::vector<lf::DependencyID> rootIds{graph.open_window(0, rootEdges)};
    const std::vector<ig::EdgeSpec> fooEdges{npm_edge("a", "^1.0.0"), npm_edge("b", "^1.0.0")};
    const std::vector<lf::DependencyID> fooIds{graph.open_window(pkg.id, fooEdges)};

    // Package.rs:568-569 builds a package's window from one (off, len) pair, so
    // its edges must be adjacent. A task-driven pipeline that appended edges as
    // they completed would interleave two packages' windows and corrupt both.
    check(fooIds.size() == 2 && fooIds[1] == fooIds[0] + 1, "graph: a package's edges are adjacent");
    check(rootIds[0] != fooIds[0], "graph: windows do not overlap");

    // Opening a second window for the same package would silently overwrite the
    // first — refuse instead of corrupting.
    check(graph.open_window(pkg.id, fooEdges).empty(), "graph: a window opens only once");
}

// ── stages 2+3 together, against bun's real itty-router layout ──────────────

// Build the maxmin/chalk/ansi-styles corner of itty-router's real graph and
// assert the hoister + placer reproduce bun's directories byte for byte.
//
// The `chalk@4 -> ansi-styles@4` edge is load-bearing and easy to omit: without
// it nothing holds the top-level `ansi-styles` slot, and `ansi-styles@2.2.1`
// correctly hoists to the top instead of nesting. The oracle shows a top-level
// `ansi-styles@4.3.0`, so it is in the fixture.
void test_placements_match_bun_itty_router_layout() {
    ig::InstallGraph graph{};
    graph.put_root("itty-router", "5.0.24");

    const auto chalk4{graph.get_or_put("chalk", "4.1.2", lf::ResolutionTag::Npm)};
    const auto chalk1{graph.get_or_put("chalk", "1.1.3", lf::ResolutionTag::Npm)};
    const auto ansi4{graph.get_or_put("ansi-styles", "4.3.0", lf::ResolutionTag::Npm)};
    const auto ansi2{graph.get_or_put("ansi-styles", "2.2.1", lf::ResolutionTag::Npm)};
    const auto maxmin{graph.get_or_put("maxmin", "2.1.0", lf::ResolutionTag::Npm)};

    const std::vector<ig::EdgeSpec> rootEdges{npm_edge("chalk", "^4.1.2"),
                                              npm_edge("maxmin", "^2.1.0")};
    const std::vector<lf::DependencyID> rootIds{graph.open_window(0, rootEdges)};
    graph.resolve_edge(rootIds[0], chalk4.id);
    graph.resolve_edge(rootIds[1], maxmin.id);

    const std::vector<ig::EdgeSpec> chalk4Edges{npm_edge("ansi-styles", "^4.1.0")};
    graph.resolve_edge(graph.open_window(chalk4.id, chalk4Edges)[0], ansi4.id);

    const std::vector<ig::EdgeSpec> maxminEdges{npm_edge("chalk", "^1.0.0")};
    graph.resolve_edge(graph.open_window(maxmin.id, maxminEdges)[0], chalk1.id);

    const std::vector<ig::EdgeSpec> chalk1Edges{npm_edge("ansi-styles", "^2.2.1")};
    graph.resolve_edge(graph.open_window(chalk1.id, chalk1Edges)[0], ansi2.id);

    lf::HoistGraph view{graph.view()};
    auto hoisted{lf::hoist(view, lf::HoistMethod::Filter)};
    check(hoisted.has_value(), "placer: the graph hoists");
    if (!hoisted) {
        return;
    }

    const std::vector<placer::Placement> placements{
        placer::plan_placements(*hoisted, view, "/p")};

    // path -> the version placed there
    std::map<std::string, std::string> layout;
    for (const placer::Placement& p : placements) {
        layout[p.dir.string()] = std::string{graph.package_version(p.pkg)};
    }

    auto at{[&layout](std::string_view path) -> std::string_view {
        auto it{layout.find(std::string{path})};
        return it == layout.end() ? std::string_view{"<absent>"} : it->second;
    }};

    check_eq(at("/p/node_modules/chalk"), "4.1.2", "placer: bun puts chalk@4.1.2 at the top");
    check_eq(at("/p/node_modules/ansi-styles"), "4.3.0",
             "placer: bun puts ansi-styles@4.3.0 at the top");
    check_eq(at("/p/node_modules/maxmin"), "2.1.0", "placer: bun puts maxmin@2.1.0 at the top");
    check_eq(at("/p/node_modules/maxmin/node_modules/chalk"), "1.1.3",
             "placer: bun nests chalk@1.1.3 under maxmin");
    check_eq(at("/p/node_modules/maxmin/node_modules/chalk/node_modules/ansi-styles"), "2.2.1",
             "placer: bun nests ansi-styles@2.2.1 under maxmin/chalk");
    check(layout.size() == 5, "placer: exactly bun's five directories, no more");

    // The `.bin` a package links into belongs to the tree it landed in, not the
    // root's — a nested package's bins go in the nested node_modules/.bin.
    for (const placer::Placement& p : placements) {
        if (p.dir.string() == "/p/node_modules/maxmin/node_modules/chalk") {
            check_eq(p.nodeModules.string(), "/p/node_modules/maxmin/node_modules",
                     "placer: a nested placement reports its own node_modules");
        }
    }
}

void test_scoped_names_split_into_two_path_components() {
    check_eq(placer::module_dir("/p/node_modules", "@typescript-eslint/eslint-plugin").string(),
             "/p/node_modules/@typescript-eslint/eslint-plugin",
             "placer: a scoped package splits on the slash");
    check_eq(placer::module_dir("/p/node_modules", "ignore").string(), "/p/node_modules/ignore",
             "placer: an unscoped package is one component");
}

// bun's `String::hash` is wyhash seeded 0 (semver/lib.rs:763 `bun_wyhash::hash`).
// The hoister compares name_hash for equality and nothing else, so a collision
// would silently merge two different names into one directory.
void test_name_hash_is_bun_wyhash_and_separates_names() {
    check(ig::name_hash("ignore") == ig::name_hash("ignore"), "hash: stable for one name");
    check(ig::name_hash("ignore") != ig::name_hash("chalk"), "hash: distinct names differ");
    check(ig::name_hash("@typescript-eslint/eslint-plugin") != ig::name_hash("ignore"),
          "hash: scoped names differ from bare ones");
}

}  // namespace

int main() {
    test_per_edge_resolution_holds_two_versions_of_one_name();
    test_edge_windows_are_contiguous_and_open_once();
    test_placements_match_bun_itty_router_layout();
    test_scoped_names_split_into_two_path_components();
    test_name_hash_is_bun_wyhash_and_separates_names();

    std::println("test_install_graph: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
