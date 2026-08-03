// test_resolver.cpp — T2.7 mbun.resolver (Node module resolution + package.json
// exports/imports) test suite.
//
// The resolver is a PURE-LOGIC layer: all filesystem access is injected via a
// mbun::resolver::FileSystem (in-memory fixture below), so every scenario runs
// without touching a real disk (design/20260710-mbun-architecture.md: pure
// logic modules must be independently unit-testable). Vectors are extracted,
// assertion-semantics preserved (AGENTS.md TDD rules, test-vector-extraction
// SOP), from bun's original resolve suite:
//   - .mbun/bun-ref/test/js/bun/resolve/resolve-test.js
//       import.meta.resolveSync vectors: relative + extension completion,
//       TypeScript .js->.ts/.tsx rewrite, tsconfig "paths", package.json
//       "exports" subpath map + package.json passthrough + ".js" strip retry.
//   - .mbun/bun-ref/test/js/bun/resolve/resolve.test.ts
//       describe("wildcard exports with @ in matched subpath"): "./*" wildcard
//       targets, @scope handling, @version stripping bounded to the name span.
//   - .mbun/bun-ref/test/js/bun/resolve/import-custom-condition.test.ts
//       conditional "exports" (custom conditions first/browser/default, nested
//       import/require/default), require vs import selection.
//   - resolve-test.js it("#imports with wildcard") + resolve.test.ts imports
//       fixtures: package.json "imports" ("#" internal, wildcards).
//   - resolve.test.ts describe("NODE_PATH"): package.json "main" + node_modules
//       upward traversal.
// The tsconfig.json "paths"/baseUrl values are the real ones from
//   .mbun/bun-ref/test/tsconfig.json.
//
// The in-memory fixture reproduces the file tree those tests build at runtime
// (writePackageJSON*Fixture / tempDirWithFiles), so the expected resolved paths
// are exactly what bun's resolver returns for the same inputs.
//
// DEFERRED(S1) — require a real runtime / fs / JS engine, registered for the
// bun:test S1 runner (T3.4) and module loader (T3.3), not S0 pure-logic:
//   - resolve.test.ts: every Bun.spawn / spawnSync case (file:// URLs, NODE_PATH
//     env, --conditions CLI flag end-to-end, autoinstall, EACCES/symlink,
//     oversized-path MODULE_NOT_FOUND, dir-cache stress) — process + fs behavior.
//   - resolve-test.js it.todo("#imports") (bare-specifier imports target ->
//     re-resolve, e.g. "#internal-react" -> react; ResolveMessage error object).
//   - resolve-ts.test.ts / require.test.ts / esModule*.test.ts runtime loading.
//   - external URL passthrough non-ASCII cloning (Bun.resolveSync http://...): the
//     passthrough itself is asserted here; the heap-clone lifetime bug is runtime.
//   - chooses-ts.{js,ts} extension-priority tie: no static assertion upstream
//     pins which wins; left to the module loader's default extension order.

import std;
import mbun.resolver;

using mbun::resolver::FileSystem;
using mbun::resolver::Options;
using mbun::resolver::ResolveKind;
using mbun::resolver::Resolver;
using mbun::resolver::ResolveStatus;
using mbun::resolver::TsconfigPaths;

namespace {

int gChecks{0};
int gFailures{0};
constexpr int MAX_FAILURE_PRINTS{40};

void report_failure(std::string_view what) {
    ++gFailures;
    if (gFailures <= MAX_FAILURE_PRINTS) {
        std::println("  FAIL {}", what);
    }
}

void check(bool condition, std::string_view what) {
    ++gChecks;
    if (!condition) report_failure(what);
}

// ---------------------------------------------------------------------------
// In-memory filesystem fixture. Files are a set of absolute posix paths;
// directories are derived from the file paths (every prefix ending in '/').
// A subset of files carry contents (package.json / tsconfig text).
// ---------------------------------------------------------------------------
class MemoryFs {
public:
    void add_file(std::string path, std::string contents = {}) {
        files_[path] = std::move(contents);
        // register every ancestor directory
        std::string_view p{path};
        while (true) {
            const auto slash{p.rfind('/')};
            if (slash == std::string_view::npos || slash == 0) {
                dirs_.insert("/");
                break;
            }
            p = p.substr(0, slash);
            dirs_.insert(std::string{p});
        }
    }

    FileSystem make() const {
        return FileSystem{
            .file_exists = [this](std::string_view path) { return files_.contains(std::string{path}); },
            .dir_exists = [this](std::string_view path) { return dirs_.contains(std::string{path}); },
            .read_file =
                [this](std::string_view path) -> std::optional<std::string> {
                const auto it{files_.find(std::string{path})};
                if (it == files_.end()) {
                    return std::nullopt;
                }
                return it->second;
            },
        };
    }

private:
    std::map<std::string, std::string> files_;
    std::set<std::string> dirs_;
};

void check_resolve(Resolver& r, std::string_view specifier, std::string_view fromDir,
                   std::string_view expected, std::string_view src) {
    ++gChecks;
    const auto res{r.resolve(specifier, fromDir)};
    if (res.status != ResolveStatus::Success || res.path != expected) {
        report_failure(std::format("[{}] resolve(\"{}\", \"{}\") -> status={} path=\"{}\", expected \"{}\"",
                                    src, specifier, fromDir, static_cast<int>(res.status), res.path,
                                    expected));
    }
}

void check_external(Resolver& r, std::string_view specifier, std::string_view fromDir,
                    std::string_view src) {
    ++gChecks;
    const auto res{r.resolve(specifier, fromDir)};
    if (res.status != ResolveStatus::External || res.path != specifier) {
        report_failure(std::format("[{}] resolve(\"{}\") expected External passthrough, got status={} path=\"{}\"",
                                    src, specifier, static_cast<int>(res.status), res.path));
    }
}

void check_not_found(Resolver& r, std::string_view specifier, std::string_view fromDir,
                     std::string_view src) {
    ++gChecks;
    const auto res{r.resolve(specifier, fromDir)};
    if (res.status == ResolveStatus::Success) {
        report_failure(std::format("[{}] resolve(\"{}\") expected failure, but resolved to \"{}\"", src,
                                    specifier, res.path));
    }
}

// ---------------------------------------------------------------------------
// Fixture builder mirroring test/js/bun/resolve/ + the runtime-written
// node_modules/ trees. Root is /proj, importer dir is /proj/js/bun/resolve.
// ---------------------------------------------------------------------------
constexpr std::string_view DIR{"/proj/js/bun/resolve"};

MemoryFs build_fixture() {
    MemoryFs fs;

    // --- relative + extension completion / TS rewrite ---
    fs.add_file("/proj/js/bun/resolve/resolve-test.js");
    fs.add_file("/proj/js/bun/resolve/resolve-typescript-file.tsx");

    // --- tsconfig "paths" targets (test/tsconfig.json baseUrl=".") ---
    fs.add_file("/proj/js/bun/resolve/baz.js");                 // foo/bar, @faasjs/baz
    fs.add_file("/proj/js/bun/resolve/bar/src/index.js");       // @faasjs/bar
    fs.add_file("/proj/js/bun/resolve/bar/larger-index.js");    // @faasjs/larger/bar
    fs.add_file("/proj/src/utils/helpers.ts");                  // #/* tsconfig alias

    // --- package.json "exports" subpath map (package-json-exports) ---
    fs.add_file("/proj/node_modules/package-json-exports/foo/bar.js");
    fs.add_file("/proj/node_modules/package-json-exports/foo/references-baz.js");
    fs.add_file("/proj/node_modules/package-json-exports/package.json",
                R"({"name":"package-json-exports","exports":{"./baz":"./foo/bar.js","./references-baz":"./foo/references-baz.js"}})");

    // --- package.json "imports" (#internal) + wildcard (package-json-imports) ---
    fs.add_file("/proj/node_modules/package-json-imports/foo/bar.js");
    fs.add_file("/proj/node_modules/package-json-imports/foo/wildcard.js");
    fs.add_file("/proj/node_modules/package-json-imports/foo/private-foo.js");
    fs.add_file("/proj/node_modules/package-json-imports/package.json",
                R"({"name":"package-json-imports","exports":{"./baz":"./foo/bar.js"},"imports":{"#foo/bar":"./foo/private-foo.js","#foo/*.js":"./foo/*.js","#foo/extensionless/*":"./foo/*.js","#foo":"./foo/private-foo.js"}})");

    // --- conditional exports (custom / custom2), nested import/require/default ---
    fs.add_file("/proj/node_modules/custom/index.js");
    fs.add_file("/proj/node_modules/custom/browser.js");
    fs.add_file("/proj/node_modules/custom/not_allow.js");
    fs.add_file("/proj/node_modules/custom/package.json",
                R"({"name":"custom","exports":{"./test":{"first":"./index.js","browser":"./browser.js","default":"./not_allow.js"}}})");

    fs.add_file("/proj/node_modules/custom2/index.cjs");
    fs.add_file("/proj/node_modules/custom2/index.mjs");
    fs.add_file("/proj/node_modules/custom2/not_allow.js");
    fs.add_file("/proj/node_modules/custom2/package.json",
                R"({"name":"custom2","type":"module","exports":{"./test":{"first":{"import":"./index.mjs","require":"./index.cjs","default":"./index.mjs"},"default":"./not_allow.js"}}})");

    // --- wildcard "./*" exports + @scope / @version (resolve.test.ts) ---
    fs.add_file("/proj/node_modules/test-pkg/dist/packages/plain/index.js");
    fs.add_file("/proj/node_modules/test-pkg/dist/packages/@scope/sub/index.js");
    fs.add_file("/proj/node_modules/test-pkg/dist/packages/with@sign/sub/index.js");
    fs.add_file("/proj/node_modules/test-pkg/package.json",
                R"({"name":"test-pkg","version":"1.0.0","exports":{"./*":"./dist/packages/*"}})");
    fs.add_file("/proj/node_modules/@my/pkg/dist/@inner/bar/index.js");
    fs.add_file("/proj/node_modules/@my/pkg/dist/sub/index.js");
    fs.add_file("/proj/node_modules/@my/pkg/package.json",
                R"({"name":"@my/pkg","version":"1.0.0","exports":{"./*":"./dist/*"}})");

    // --- node_modules upward traversal + "main" field (NODE_PATH fixture) ---
    fs.add_file("/proj/node_modules/node-path-test/index.js");
    fs.add_file("/proj/node_modules/node-path-test/package.json",
                R"({"name":"node-path-test","version":"1.0.0","main":"index.js"})");

    // --- "browser" main-field override ---
    fs.add_file("/proj/node_modules/browserpkg/main.js");
    fs.add_file("/proj/node_modules/browserpkg/browser.js");
    fs.add_file("/proj/node_modules/browserpkg/package.json",
                R"({"name":"browserpkg","main":"./main.js","browser":"./browser.js"})");

    // --- package self-reference (import a package by its own "name") ---
    // Importer lives inside the package; the nearest package.json declares
    // "name":"selfpkg" + "exports", so "selfpkg" / "selfpkg/feature" resolve
    // through the package's own exports, no node_modules lookup.
    fs.add_file("/proj/selfpkg/src/main.js");
    fs.add_file("/proj/selfpkg/src/feature.js");
    fs.add_file("/proj/selfpkg/package.json",
                R"({"name":"selfpkg","exports":{".":"./src/main.js","./feature":"./src/feature.js"}})");
    // A package WITHOUT exports is not self-referenceable (falls through).
    fs.add_file("/proj/noexp/src/main.js");
    fs.add_file("/proj/noexp/package.json", R"({"name":"noexp","main":"./src/main.js"})");

    // --- exports conditions: node / types / default selection + priority ---
    fs.add_file("/proj/node_modules/condpkg/node.js");
    fs.add_file("/proj/node_modules/condpkg/types.d.ts");
    fs.add_file("/proj/node_modules/condpkg/default.js");
    fs.add_file("/proj/node_modules/condpkg/imp.js");
    fs.add_file("/proj/node_modules/condpkg/package.json",
                R"({"name":"condpkg","exports":{"./x":{"node":"./node.js","types":"./types.d.ts","default":"./default.js"},"./y":{"import":"./imp.js","node":"./node.js","default":"./default.js"}}})");

    return fs;
}

TsconfigPaths build_tsconfig() {
    // Real values from .mbun/bun-ref/test/tsconfig.json (baseUrl = ".",
    // resolved here to the absolute test root /proj).
    TsconfigPaths ts;
    ts.baseDir = "/proj";
    ts.entries = {
        {"foo/bar", {"./js/bun/resolve/baz.js"}},
        {"@faasjs/larger/*", {"./js/bun/resolve/*/larger-index.js"}},
        {"@faasjs/*", {"./js/bun/resolve/*.js", "./js/bun/resolve/*/src/index.js"}},
    };
    return ts;
}

// helper: make a Resolver for the given kind + conditions
Resolver make_resolver(const MemoryFs& fs, const TsconfigPaths* ts, ResolveKind kind,
                       std::vector<std::string> conditions = {}, bool browser = false) {
    Options opts;
    opts.kind = kind;
    opts.conditions = std::move(conditions);
    opts.browser = browser;
    opts.tsconfig = ts;
    return Resolver{fs.make(), std::move(opts)};
}

}  // namespace

int main() {
    const MemoryFs fs{build_fixture()};
    const TsconfigPaths ts{build_tsconfig()};

    // ============ relative paths + extension completion + TS rewrite ============
    {
        auto r{make_resolver(fs, &ts, ResolveKind::Import)};
        // source: resolve-test.js it("import.meta.resolveSync")
        check_resolve(r, "./resolve-test.js", DIR, "/proj/js/bun/resolve/resolve-test.js",
                      "relative exact");
        check_resolve(r, "./resolve-test", DIR, "/proj/js/bun/resolve/resolve-test.js",
                      "extensionless -> .js");
        // .js that doesn't exist retries .ts then .tsx (TypeScript compiler edgecase)
        check_resolve(r, "./resolve-typescript-file.js", DIR,
                      "/proj/js/bun/resolve/resolve-typescript-file.tsx", "js->tsx rewrite");
        check_resolve(r, "./resolve-typescript-file.tsx", DIR,
                      "/proj/js/bun/resolve/resolve-typescript-file.tsx", "tsx exact");
        // absolute specifier
        check_resolve(r, "/proj/js/bun/resolve/resolve-test.js", DIR,
                      "/proj/js/bun/resolve/resolve-test.js", "absolute exact");
        // parent-dir specifier normalizes
        check_resolve(r, "../resolve/resolve-test", DIR, "/proj/js/bun/resolve/resolve-test.js",
                      "parent-normalized");
        // failure
        check_not_found(r, "THIS FILE DOESNT EXIST", DIR, "missing");
    }

    // ============ tsconfig "paths" ============
    {
        auto r{make_resolver(fs, &ts, ResolveKind::Import)};
        // source: resolve-test.js "works with tsconfig.json paths"
        check_resolve(r, "foo/bar", DIR, "/proj/js/bun/resolve/baz.js", "tsconfig exact");
        check_resolve(r, "@faasjs/baz", DIR, "/proj/js/bun/resolve/baz.js", "tsconfig * first target");
        check_resolve(r, "@faasjs/bar", DIR, "/proj/js/bun/resolve/bar/src/index.js",
                      "tsconfig * second target");
        check_resolve(r, "@faasjs/larger/bar", DIR, "/proj/js/bun/resolve/bar/larger-index.js",
                      "tsconfig longest-prefix wins");

        TsconfigPaths hashAlias{ts};
        hashAlias.entries.emplace_back("#/*", std::vector<std::string>{"./src/*"});
        auto hashResolver{make_resolver(fs, &hashAlias, ResolveKind::Import)};
        check_resolve(hashResolver, "#/utils/helpers", DIR, "/proj/src/utils/helpers.ts",
                      "tsconfig hash alias precedes package imports");
    }

    // ============ package.json "exports" subpath map ============
    {
        auto r{make_resolver(fs, &ts, ResolveKind::Import)};
        // source: resolve-test.js "works with package.json exports"
        check_resolve(r, "package-json-exports/baz", DIR,
                      "/proj/node_modules/package-json-exports/foo/bar.js", "exports subpath");
        check_resolve(r, "package-json-exports/references-baz", DIR,
                      "/proj/node_modules/package-json-exports/foo/references-baz.js",
                      "exports subpath 2");
        // package.json passthrough even though not exported
        check_resolve(r, "package-json-exports/package.json", DIR,
                      "/proj/node_modules/package-json-exports/package.json", "package.json passthrough");
        // unnecessary ".js" extension stripped, retried against /baz
        check_resolve(r, "package-json-exports/baz.js", DIR,
                      "/proj/node_modules/package-json-exports/foo/bar.js", "exports .js strip retry");
    }

    // ============ wildcard "./*" exports with @scope / @version ============
    {
        auto r{make_resolver(fs, &ts, ResolveKind::Require)};
        // source: resolve.test.ts describe("wildcard exports with @ in matched subpath")
        check_resolve(r, "test-pkg/plain/index.js", DIR,
                      "/proj/node_modules/test-pkg/dist/packages/plain/index.js", "wildcard plain");
        check_resolve(r, "test-pkg/@scope/sub/index.js", DIR,
                      "/proj/node_modules/test-pkg/dist/packages/@scope/sub/index.js",
                      "wildcard @scope subpath");
        check_resolve(r, "test-pkg/with@sign/sub/index.js", DIR,
                      "/proj/node_modules/test-pkg/dist/packages/with@sign/sub/index.js",
                      "wildcard @ mid-segment");
        check_resolve(r, "@my/pkg/@inner/bar/index.js", DIR,
                      "/proj/node_modules/@my/pkg/dist/@inner/bar/index.js",
                      "scoped pkg @-prefixed subpath");
        // @version after the (scoped) package name is still stripped
        check_resolve(r, "test-pkg@1.0.0/plain/index.js", DIR,
                      "/proj/node_modules/test-pkg/dist/packages/plain/index.js",
                      "strip @version unscoped");
        check_resolve(r, "@my/pkg@1.0.0/sub/index.js", DIR,
                      "/proj/node_modules/@my/pkg/dist/sub/index.js", "strip @version scoped");
    }

    // ============ conditional exports: custom conditions ============
    {
        // source: import-custom-condition.test.ts
        // --conditions=first, import kind -> ./index.js (foo=1)
        auto rf{make_resolver(fs, &ts, ResolveKind::Import, {"first"})};
        check_resolve(rf, "custom/test", DIR, "/proj/node_modules/custom/index.js",
                      "condition first");
        // --conditions=browser -> ./browser.js (foo=2)
        auto rb{make_resolver(fs, &ts, ResolveKind::Import, {"browser"})};
        check_resolve(rb, "custom/test", DIR, "/proj/node_modules/custom/browser.js",
                      "condition browser");
        // no matching condition -> default -> ./not_allow.js
        auto rn{make_resolver(fs, &ts, ResolveKind::Import, {"first1"})};
        check_resolve(rn, "custom/test", DIR, "/proj/node_modules/custom/not_allow.js",
                      "condition default fallback");
        // nested: require kind under matched "first" -> require branch -> index.cjs (foo=5)
        auto rr{make_resolver(fs, &ts, ResolveKind::Require, {"first"})};
        check_resolve(rr, "custom2/test", DIR, "/proj/node_modules/custom2/index.cjs",
                      "nested require branch");
        // nested import kind -> index.mjs
        auto ri{make_resolver(fs, &ts, ResolveKind::Import, {"first"})};
        check_resolve(ri, "custom2/test", DIR, "/proj/node_modules/custom2/index.mjs",
                      "nested import branch");
    }

    // ============ package.json "imports" (#internal) wildcards ============
    {
        auto r{make_resolver(fs, &ts, ResolveKind::Import)};
        // source: resolve-test.js it("#imports with wildcard"). Importer is a file
        // inside the package, so the nearest package.json with "imports" is its own.
        constexpr std::string_view pkgDir{"/proj/node_modules/package-json-imports"};
        check_resolve(r, "#foo/wildcard.js", pkgDir,
                      "/proj/node_modules/package-json-imports/foo/wildcard.js", "#imports wildcard .js");
        check_resolve(r, "#foo/extensionless/wildcard", pkgDir,
                      "/proj/node_modules/package-json-imports/foo/wildcard.js",
                      "#imports extensionless wildcard");
        // exact "#foo/bar" -> ./foo/private-foo.js
        check_resolve(r, "#foo/bar", pkgDir,
                      "/proj/node_modules/package-json-imports/foo/private-foo.js", "#imports exact");
    }

    // ============ node_modules traversal + "main" field ============
    {
        auto r{make_resolver(fs, &ts, ResolveKind::Require)};
        // source: resolve.test.ts NODE_PATH fixture (main: "index.js"), resolved
        // from a deeply nested importer that must walk up to /proj/node_modules.
        check_resolve(r, "node-path-test", "/proj/js/bun/resolve",
                      "/proj/node_modules/node-path-test/index.js", "bare pkg main upward");
    }

    // ============ node-shim entry LOAD_AS_FILE_OR_DIRECTORY ============
    // RunAsNodeCommand boots its positional through Bun's resolver. The load
    // path uses the pinned entry extension order, while package.json main and
    // directory index fallback remain ordinary LOAD_AS_DIRECTORY behavior.
    {
        MemoryFs entryFs;
        entryFs.add_file("/entry/pkg/package.json", R"({"main":"./start"})");
        entryFs.add_file("/entry/pkg/start.jsx");
        entryFs.add_file("/entry/pkg/start.tsx");
        entryFs.add_file("/entry/index-only/index.mjs");

        Options opts{};
        opts.extension_order = {".tsx", ".jsx", ".mts", ".ts", ".mjs", ".js",
                                ".cts", ".cjs", ".json"};
        Resolver entryResolver{entryFs.make(), std::move(opts)};
        check_resolve(entryResolver, "/entry/pkg", "/", "/entry/pkg/start.tsx",
                      "node entry directory package main + pinned extension order");
        check_resolve(entryResolver, "/entry/index-only", "/", "/entry/index-only/index.mjs",
                      "node entry directory index fallback");
    }

    // An explicit override is not a nullable nearest-config probe: missing and
    // malformed files carry stable diagnostics to the CLI boundary.
    {
        MemoryFs configFs;
        configFs.add_file("/config/invalid.json", "{ invalid");
        configFs.add_file("/config/trailing.json", "{\"compilerOptions\":{}} invalid");
        auto missing{load_tsconfig_override(configFs.make(), "/config/missing.json")};
        check(!missing.config.has_value(), "missing explicit tsconfig does not parse as null success");
        check(missing.error == "Cannot find tsconfig file \"/config/missing.json\"",
              "missing explicit tsconfig diagnostic");
        auto invalid{load_tsconfig_override(configFs.make(), "/config/invalid.json")};
        check(!invalid.config.has_value(), "invalid explicit tsconfig does not parse as null success");
        check(invalid.error == "Expected string but found \"invalid\"\n    at /config/invalid.json:1:3",
              "invalid explicit tsconfig diagnostic");
        auto trailing{load_tsconfig_override(configFs.make(), "/config/trailing.json")};
        check(!trailing.config.has_value(), "trailing token does not parse as explicit tsconfig success");
        check(trailing.error == "Expected end of file but found \"invalid\"\n    at /config/trailing.json:1:24",
              "trailing token explicit tsconfig diagnostic");
    }

    // ============ "browser" main-field override ============
    {
        auto rNode{make_resolver(fs, &ts, ResolveKind::Require)};
        check_resolve(rNode, "browserpkg", DIR, "/proj/node_modules/browserpkg/main.js",
                      "browserpkg node main");
        auto rBrowser{make_resolver(fs, &ts, ResolveKind::Require, {}, /*browser=*/true)};
        check_resolve(rBrowser, "browserpkg", DIR, "/proj/node_modules/browserpkg/browser.js",
                      "browserpkg browser override");
    }

    // ============ package self-reference ============
    {
        // source: Node "self-referencing a package using its name" (bun resolver
        // resolver.rs is_self_reference branch). Importer is inside /proj/selfpkg.
        constexpr std::string_view selfDir{"/proj/selfpkg/src"};
        auto r{make_resolver(fs, &ts, ResolveKind::Import)};
        check_resolve(r, "selfpkg", selfDir, "/proj/selfpkg/src/main.js", "self-ref root '.'");
        check_resolve(r, "selfpkg/feature", selfDir, "/proj/selfpkg/src/feature.js",
                      "self-ref subpath");
        // A package without "exports" is not self-referenceable: "noexp" from
        // inside /proj/noexp is not the enclosing name+exports pair, so it falls
        // through to node_modules and (absent there) fails.
        check_not_found(r, "noexp", "/proj/noexp/src", "no-exports pkg not self-ref");
    }

    // ============ exports conditions: node / types / default + priority ============
    {
        // node condition active (supplied via conditions list, as the runtime
        // layer does) -> ./node.js.
        auto rNode{make_resolver(fs, &ts, ResolveKind::Import, {"node"})};
        check_resolve(rNode, "condpkg/x", DIR, "/proj/node_modules/condpkg/node.js",
                      "condition node");
        // types condition active, node absent -> ./types.d.ts.
        auto rTypes{make_resolver(fs, &ts, ResolveKind::Import, {"types"})};
        check_resolve(rTypes, "condpkg/x", DIR, "/proj/node_modules/condpkg/types.d.ts",
                      "condition types");
        // neither node nor types -> default fallback.
        auto rDef{make_resolver(fs, &ts, ResolveKind::Import)};
        check_resolve(rDef, "condpkg/x", DIR, "/proj/node_modules/condpkg/default.js",
                      "condition default fallback");
        // priority: with both "import" (kind-derived) and "node" active, the
        // first key in source order ("import") wins.
        auto rPrio{make_resolver(fs, &ts, ResolveKind::Import, {"node"})};
        check_resolve(rPrio, "condpkg/y", DIR, "/proj/node_modules/condpkg/imp.js",
                      "condition source-order priority");
    }

    // ============ external URL passthrough ============
    {
        auto r{make_resolver(fs, &ts, ResolveKind::Import)};
        // source: resolve.test.ts describe("resolving external URL specifiers ...")
        check_external(r, "http://localhost/path?query=a", DIR, "external http");
        check_external(r, "https://example/x", DIR, "external https");
        check_external(r, "//example/x?q", DIR, "external //");
    }

    std::println("resolver: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
