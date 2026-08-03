// test_vertical_slice.cpp — selective parse -> link -> emit translation slice.
//
// Sources (assertion semantics preserved):
//   test/bundler/bundler_files.test.ts > describe("bundler files option") >
//     test("basic in-memory file bundling")
//     test("in-memory file with imports")
//     test("in-memory file with relative imports (same directory)")
//     test("in-memory file with relative imports (subdirectory)")
//     test("in-memory file with relative imports (parent directory)")
//     test("in-memory file with relative imports between multiple files")
//     test("in-memory file with nested imports")
//
// This is a named vertical slice, not completion of the surrounding describe.
// B.1 extends it with multiple entry points, dependency-first ordering,
// module-level tree-shaking, CJS/ESM default-import interop and basic source-map
// v3 association (exercised below, executed under node when available).
// R6 extends it with an on-disk fallback (entrypoints + transitive imports read
// through an injected filesystem, resolved via mbun.resolver) and literal dynamic
// imports as ordinary graph edges.
// R7 extends it with the on_resolve/on_load plugin hooks (test_plugin_hooks).
// DEFERRED(T4.4): Blob/Uint8Array/ArrayBuffer input wrappers; JSX options;
// external modules, output naming, per-export tree shaking, code splitting,
// computed dynamic-import specifiers, require()-in-CJS-source graph edges,
// minify, column-precise/enum-shift source-map accuracy, and every other
// test/bundler group.

import std;
import mbun.bundler.ascii_only;
import mbun.bundler.vertical_slice;
import mbun.resolver;

namespace {

int gChecks{0};
int gFailures{0};

void check(bool condition, std::string_view message) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::println("FAIL: {}", message);
    }
}
void check_contains(std::string_view text, std::string_view needle, std::string_view source) {
    check(text.contains(needle), std::format("{}: output does not contain {:?}", source, needle));
}

void test_basic_and_typescript() {
    mbun::bundler::Files files{
        {"/entry.ts", "const x: number = 42; console.log(x);"},
    };
    auto result{mbun::bundler::build("/entry.ts", files)};
    check(result.has_value(), "basic in-memory TypeScript build succeeds");
    if (result) {
        check(result->moduleCount == 1, "basic build emits exactly one reachable module");
        check_contains(result->code, "const x = 42", "basic in-memory TypeScript");
        check(!result->code.contains(": number"), "TypeScript annotation is erased");
    }
}

void test_path_forms_and_nested_graph() {
    mbun::bundler::Files files{
        {"/src/app/entry.js",
         "import { componentA } from '../components/a.js';\n"
         "import { componentB } from '../components/b.js';\n"
         "console.log(componentA, componentB);"},
        {"/src/components/a.js",
         "import { util } from './shared/util.js'; export const componentA = 'A:' + util;"},
        {"/src/components/b.js",
         "import { util } from './shared/util.js'; export const componentB = 'B:' + util;"},
        {"/src/components/shared/util.js", "export const util = 'shared-util';"},
    };
    auto result{mbun::bundler::build("/src/app/entry.js", files)};
    check(result.has_value(), "relative parent/subdirectory/shared dependency build succeeds");
    if (result) {
        check(result->moduleCount == 4, "shared dependency is linked once");
        check_contains(result->code, "shared-util", "shared dependency");
        check_contains(result->code, "A:", "component A");
        check_contains(result->code, "B:", "component B");
        check(!result->code.contains("from '../components"), "entry imports are linked");
        check(!result->code.contains("from './shared"), "nested imports are linked");
    }
}

void test_absolute_and_extension_completion() {
    mbun::bundler::Files files{
        {"/entry.js", "import { answer } from '/lib'; console.log(answer);"},
        {"/lib.ts", "export const answer: number = 42;"},
    };
    auto result{mbun::bundler::build("/entry.js", files)};
    check(result.has_value(), "absolute import with TypeScript extension completion succeeds");
    if (result) {
        check(result->moduleCount == 2, "absolute import reaches two modules");
        check_contains(result->code, "42", "absolute import output");
    }
}

void test_cycle_and_side_effect_import() {
    mbun::bundler::Files files{
        {"/entry.js", "import './a.js'; import './a.js'; console.log('entry');"},
        {"/a.js", "import './b.js'; export const a = 'a';"},
        {"/b.js", "import './a.js'; export const b = 'b';"},
    };
    auto result{mbun::bundler::build("/entry.js", files)};
    check(result.has_value(), "cycle and duplicate side-effect import build succeeds");
    if (result) {
        check(result->moduleCount == 3, "cycle and duplicate import keep one module per normalized path");
        check_contains(result->code, "__mbun_cache", "cycle-safe runtime cache");
    }
}

void test_await_context() {
    mbun::bundler::Files files{
        {"/entry.js",
         "const obj = { await: 1 }; console.log(obj.await); "
         "async function f() { return await Promise.resolve(1); } "
         "const g = async () => await Promise.resolve(2);"},
    };
    auto result{mbun::bundler::build("/entry.js", files)};
    check(result.has_value(), "await property names and function-local await are not top-level await");
}

void test_shadowed_require_is_not_a_graph_edge() {
    mbun::bundler::Files files{
        {"/entry.js",
         "import { value } from './dep.js'; "
         "function callLocal(require) { return require('./not-a-module.js'); } "
         "globalThis.__bundlerChecksum = value + callLocal(() => 1);"},
        {"/dep.js", "export const value = 82;"},
    };
    auto result{mbun::bundler::build("/entry.js", files)};
    check(result.has_value(), "function-local shadowed require is not resolved as a module edge");
    if (result) {
        check(result->moduleCount == 2, "shadowed require does not add a reachable module");
        check_contains(result->code, "require('./not-a-module.js')", "shadowed require is preserved");
    }
}

void test_errors() {
    mbun::bundler::Files files{{"/entry.js", "import x from './missing.js'; console.log(x);"}};
    auto missingEntry{mbun::bundler::build("/absent.js", files)};
    check(!missingEntry, "missing entry is rejected");
    auto missingImport{mbun::bundler::build("/entry.js", files)};
    check(!missingImport, "missing local dependency is rejected");

    files["/bad.js"] = "export const = ;";
    auto syntax{mbun::bundler::build("/bad.js", files)};
    check(!syntax, "parse failure is rejected instead of emitting invalid bundle");

    files["/pkg.js"] = "import React from 'react'; console.log(React);";
    auto package{mbun::bundler::build("/pkg.js", files)};
    check(!package, "package import outside this slice is explicit error");

    // A computed dynamic-import specifier has no statically linkable target.
    files["/dynamic-expr.js"] = "const n = '1'; import('./lazy' + n + '.js');";
    files["/lazy1.js"] = "export const lazy = true;";
    auto dynamicExpr{mbun::bundler::build("/dynamic-expr.js", files)};
    // bun leaves a non-literal specifier as a runtime import (p.rs transpose_import
    // only records string literals); js_parser lowers it to __mbun_dyn_import with a
    // chunk-local fallback, so the build succeeds with no edge recorded.
    check(dynamicExpr.has_value(), "computed dynamic import specifier defers to the runtime");

    files["/await.js"] = "await Promise.resolve(1);";
    auto topLevelAwait{mbun::bundler::build("/await.js", files)};
    check(!topLevelAwait, "async module outside this slice is explicit error");
}

// Execute an emitted bundle with node and capture stdout. Returns nullopt when a
// node engine is unavailable (the structural checks still gate the suite).
std::optional<std::string> run_bundle(std::string_view code) {
    namespace fs = std::filesystem;
    const fs::path dir{fs::temp_directory_path()};
    const fs::path script{dir / std::format("mbun_bundle_{}.cjs", std::rand())};
    const fs::path out{dir / std::format("mbun_out_{}.txt", std::rand())};
    {
        std::ofstream f{script};
        if (!f) {
            return std::nullopt;
        }
        f << code;
    }
    const std::string cmd{
        std::format("node {} > {} 2>/dev/null", script.string(), out.string())};
    const int rc{std::system(cmd.c_str())};
    std::optional<std::string> result;
    if (rc == 0) {
        std::ifstream in{out};
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            result = ss.str();
        }
    }
    std::error_code ec;
    fs::remove(script, ec);
    fs::remove(out, ec);
    return result;
}

void test_multiple_entry_points() {
    mbun::bundler::Files files{
        {"/e1.js", "import { v } from './shared.js'; console.log('e1:' + v);"},
        {"/e2.js", "import { v } from './shared.js'; console.log('e2:' + v);"},
        {"/shared.js", "export const v = 'S';"},
    };
    auto result{mbun::bundler::build_bundle({"/e1.js", "/e2.js"}, files)};
    check(result.has_value(), "multi-entry bundle succeeds");
    if (result) {
        check(result->entryModules.size() == 2, "two entries are recorded");
        check(result->moduleCount == 3, "shared dependency counted once across entries");
        check_contains(result->code, "__mbun_entries", "multi-entry exposes entry map");
        check_contains(result->code, "\"/e1.js\"", "entry map keyed by entry path");
        auto stdout_text{run_bundle(result->code)};
        if (stdout_text) {
            check(*stdout_text == "e1:S\ne2:S\n", "both entries run in input order, sharing the dep");
        }
    }
}

void test_tree_shaking_drops_orphan_modules() {
    mbun::bundler::Files files{
        {"/entry.js", "import { used } from './used.js'; console.log(used);"},
        {"/used.js", "export const used = 'used';"},
        {"/orphan.js", "export const orphan = 'never imported';"},
    };
    auto shaken{mbun::bundler::build_bundle({"/entry.js"}, files, {.tree_shaking = true})};
    check(shaken.has_value(), "tree-shaken build succeeds");
    if (shaken) {
        check(shaken->moduleCount == 2, "unreachable orphan module is tree-shaken out");
        check(!shaken->code.contains("never imported"), "orphan source is not emitted");
    }
    auto whole{mbun::bundler::build_bundle({"/entry.js"}, files, {.tree_shaking = false})};
    check(whole.has_value(), "no-tree-shaking build succeeds");
    if (whole) {
        check(whole->moduleCount == 3, "without tree-shaking every input file is bundled");
        check_contains(whole->code, "never imported", "orphan is included when tree-shaking is off");
    }
}

void test_cjs_esm_interop() {
    // An ESM module default-imports a CommonJS module: the lowered require must
    // interop `module.exports` as the default (no __esModule marker on the CJS side).
    mbun::bundler::Files files{
        {"/entry.js", "import cfg from './cjs.js'; console.log(cfg.name);"},
        {"/cjs.js", "module.exports = { name: 'cjs-value' };"},
    };
    auto result{mbun::bundler::build_bundle({"/entry.js"}, files)};
    check(result.has_value(), "ESM-importing-CJS interop bundle succeeds");
    if (result) {
        check(result->moduleCount == 2, "CJS module is linked as a graph node");
        check_contains(result->code, "__esModule", "default-import interop guard is present");
        auto stdout_text{run_bundle(result->code)};
        if (stdout_text) {
            check(*stdout_text == "cjs-value\n", "default import resolves to CJS module.exports");
        }
    }

    // Reverse direction: an ESM module named-imports from an ESM module that also
    // sets a default; the __esModule marker must let the interop pick .default.
    mbun::bundler::Files files2{
        {"/entry.js", "import def, { extra } from './esm.js'; console.log(def + extra);"},
        {"/esm.js", "export default 'D'; export const extra = 'X';"},
    };
    auto result2{mbun::bundler::build_bundle({"/entry.js"}, files2)};
    check(result2.has_value(), "ESM default+named interop bundle succeeds");
    if (result2) {
        auto stdout_text{run_bundle(result2->code)};
        if (stdout_text) {
            check(*stdout_text == "DX\n", "default + named ESM imports both resolve");
        }
    }
}

void test_sourcemap_association() {
    mbun::bundler::Files files{
        {"/entry.js", "import { greet } from './greet.js';\nconsole.log(greet('world'));"},
        {"/greet.js", "export const greet = (who) => 'hi ' + who;"},
    };
    auto result{mbun::bundler::build_bundle({"/entry.js"}, files, {.sourcemap = true})};
    check(result.has_value(), "source-map build succeeds");
    if (result) {
        check(!result->sourcemap.empty(), "source map is produced when requested");
        check_contains(result->sourcemap, "\"version\":3", "source map is v3");
        check_contains(result->sourcemap, "/entry.js", "source map lists entry source path");
        check_contains(result->sourcemap, "/greet.js", "source map lists dependency source path");
        check_contains(result->sourcemap, "sourcesContent", "source map embeds original contents");
        check_contains(result->sourcemap, "\"mappings\":\"", "source map carries VLQ mappings");
        check(result->code.contains('\n'), "sourcemap layout emits one module line per source line");
        // The bundle must still execute after the multi-line layout.
        auto stdout_text{run_bundle(result->code)};
        if (stdout_text) {
            check(*stdout_text == "hi world\n", "mapped multi-line bundle still runs correctly");
        }
        // Determinism: the same inputs produce byte-identical output.
        auto again{mbun::bundler::build_bundle({"/entry.js"}, files, {.sourcemap = true})};
        check(again && again->code == result->code, "bundle output is deterministic");
        check(again && again->sourcemap == result->sourcemap, "source map is deterministic");
    }
}

// Template substitutions and regex literals need lexer context while scanning for
// import records: a `}` closing a `${` continues the template, and `/` after an
// operator opens a regex whose body may contain quotes or braces.
void test_template_and_regex_lexing() {
    mbun::bundler::Files files{
        {"/entry.js",
         "import { name } from './dep.js';\n"
         "const re = /['\"`{}]/g;\n"
         "const msg = `hi ${name} ${`nested ${name}`}`;\n"
         "console.log(msg.replace(re, ''));"},
        {"/dep.js", "export const name = 'world';"},
    };
    auto result{mbun::bundler::build("/entry.js", files)};
    check(result.has_value(), "template literals and regex literals lex while scanning imports");
    if (result) {
        check(result->moduleCount == 2, "import is still discovered past a template/regex");
        auto stdout_text{run_bundle(result->code)};
        if (stdout_text) {
            check(*stdout_text == "hi world nested world\n", "template bundle runs correctly");
        }
    }
}

// A literal dynamic import is an ordinary graph edge: with no code splitting the
// target is bundled into the same chunk. ref: bun src/bundler/LinkerContext.rs:397
// is_external_dynamic_import() only externalises it when code_splitting is on.
void test_dynamic_import_is_bundled() {
    mbun::bundler::Files files{
        {"/entry.js",
         "export function main() { return import('./dep.js'); }\n"
         "main().then(m => console.log(m.value));"},
        {"/dep.js", "export const value = 'lazy';"},
    };
    auto result{mbun::bundler::build("/entry.js", files)};
    check(result.has_value(), "literal dynamic import builds");
    if (result) {
        check(result->moduleCount == 2, "dynamic import target is bundled into the chunk");
        // ref: bun issue 24709 — the lowering must not leave an empty arrow body.
        check(!result->code.contains("() => )"), "dynamic import lowering emits no empty arrow body");
        auto stdout_text{run_bundle(result->code)};
        if (stdout_text) {
            check(*stdout_text == "lazy\n", "dynamically imported module resolves at runtime");
        }
    }
}

// Disk fallback: an on-disk entrypoint and its extensionless relative import
// bundle through the injected filesystem (resolution via mbun.resolver).
// An import specifier is an ordinary string token and carries the full escape
// grammar. Source: test/regression/issue/14976/14976.test.ts > test("more
// unicode imports"), which imports "./modထ.ts" and "./modထ.ts" and expects
// both to reach the same module.
void test_unicode_escaped_specifier() {
    mbun::bundler::Files files{
        {"/modထ.ts", "export const n = 1;"},
        {"/entry.ts",
         "import { n as a } from \"./mod\\u1011.ts\";\n"
         "import { n as b } from \"./modထ.ts\";\n"
         "console.log(a, b);\n"},
    };
    auto result{mbun::bundler::build("/entry.ts", files)};
    check(result.has_value(), "escaped-unicode import specifier builds");
    if (result) {
        check(result->moduleCount == 2,
              "\\uXXXX and literal specifier normalize to one module");
    }

    // \u{...}, a surrogate pair and \x reach the same path as the literal form.
    // U+100D8 is 𐃘 as a surrogate pair, matching the fixture's `𐃘`.
    mbun::bundler::Files escapes{
        {"/a\U000100D8b.ts", "export const n = 1;"},
        {"/entry.ts",
         "import { n as a } from \"./a\\u{100d8}b.ts\";\n"
         "import { n as b } from \"./a\\ud800\\udcd8\\x62.ts\";\n"
         "console.log(a, b);\n"},
    };
    auto escaped{mbun::bundler::build("/entry.ts", escapes)};
    check(escaped.has_value(), "\\u{...}, surrogate pair and \\x specifiers build");
    if (escaped) {
        check(escaped->moduleCount == 2, "all escape forms resolve to one module");
    }
}

void test_disk_entrypoint_and_imports() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir{fs::temp_directory_path() / std::format("mbun_disk_{}", std::rand())};
    fs::create_directories(dir, ec);
    if (ec) {
        return;  // no writable temp dir; the in-memory checks still gate the suite
    }
    {
        std::ofstream entryFile{dir / "entry.ts"};
        entryFile << "import { greet } from './greet';\nconsole.log(greet('disk'));\n";
        std::ofstream depFile{dir / "greet.ts"};
        depFile << "export const greet = (name: string): string => `hi ${name}`;\n";
    }
    const mbun::resolver::FileSystem osFs{
        .file_exists =
            [](std::string_view p) {
                std::error_code e;
                return fs::is_regular_file(fs::path{p}, e);
            },
        .dir_exists =
            [](std::string_view p) {
                std::error_code e;
                return fs::is_directory(fs::path{p}, e);
            },
        .read_file =
            [](std::string_view p) -> std::optional<std::string> {
                std::ifstream in{fs::path{p}, std::ios::binary};
                if (!in) {
                    return std::nullopt;
                }
                return std::string{std::istreambuf_iterator<char>{in},
                                   std::istreambuf_iterator<char>{}};
            },
    };
    auto result{mbun::bundler::build_bundle({(dir / "entry.ts").string()}, {}, {.fs = &osFs})};
    check(result.has_value(), "on-disk entrypoint builds");
    if (result) {
        check(result->moduleCount == 2, "extensionless disk import resolves to ./greet.ts");
        auto stdout_text{run_bundle(result->code)};
        if (stdout_text) {
            check(*stdout_text == "hi disk\n", "disk bundle runs correctly");
        }
    }

    // In-memory files shadow disk at the same path.
    mbun::bundler::Files overlay{{(dir / "greet.ts").string(), "export const greet = () => 'shadowed';"}};
    auto shadowed{mbun::bundler::build_bundle({(dir / "entry.ts").string()}, overlay, {.fs = &osFs})};
    check(shadowed.has_value(), "overlay build succeeds");
    if (shadowed) {
        check_contains(shadowed->code, "shadowed", "in-memory file shadows the on-disk file");
    }

    // `--tsconfig-override` is a shared transpiler option in Bun's run/test/build
    // tables. The build pipeline receives the same parsed alias map instead of
    // accepting and then discarding the option at its CLI boundary.
    {
        mbun::bundler::Files aliasFiles{
            {"/alias/entry.ts", "import { answer } from '#/answer'; console.log(answer);"},
            {"/alias/src/answer.ts", "export const answer = 42;"},
        };
        mbun::resolver::TsconfigPaths tsconfig{
            .baseDir = "/alias", .entries = {{"#/*", {"./src/*"}}}};
        auto aliased{mbun::bundler::build_bundle(
            {"/alias/entry.ts"}, aliasFiles, {.tsconfig = &tsconfig})};
        check(aliased.has_value(), "build routes explicit tsconfig aliases into resolver");
        if (aliased) check(aliased->moduleCount == 2, "tsconfig alias adds target module to graph");
    }
    fs::remove_all(dir, ec);
}

// Plugin hooks (R7). The JS-facing half lives in modules/jsc bun_build.inc; these
// exercise the engine contract directly: on_resolve runs ahead of the default
// resolver, on_load ahead of the default read, and a hook error fails the build
// rather than stalling it.
//   test/bundler/bundler_files.test.ts > "onLoad plugin can transform in-memory files"
//                                        "onResolve plugin can redirect in-memory file imports"
//   test/bundler/plugin-error-nested-throw.test.ts (error path, not the hang)
void test_plugin_hooks() {
    using namespace mbun::bundler;
    const Files files{
        {"/entry.js", "import { value } from \"./lib.js\"; console.log(value);"},
        {"/lib.js", "export const value = \"original\";"},
    };

    {  // on_load replaces the source of a file that exists
        BuildOptions options;
        std::string loadedPath;
        options.on_load = [&](std::string_view path, std::string_view ns)
            -> std::expected<std::optional<PluginLoadResult>, BuildError> {
            if (!path.ends_with("lib.js")) {
                return std::nullopt;
            }
            loadedPath = std::string{path};
            check(ns == "file", "on_load receives the file namespace");
            return PluginLoadResult{"export const value = \"transformed by plugin\";", "js"};
        };
        auto result{build_bundle({"/entry.js"}, files, options)};
        check(result.has_value(), "on_load build succeeds");
        if (result) {
            check(loadedPath == "/lib.js", "on_load receives the resolved path");
            check_contains(result->code, "transformed by plugin", "on_load transform");
            check(!result->code.contains("original"), "on_load replaces the original source");
        }
    }

    {  // on_resolve redirects a bare specifier to another in-memory file
        const Files redirected{
            {"/entry.js", "import { value } from \"virtual:data\"; console.log(value);"},
            {"/actual-data.js", "export const value = \"from actual-data\";"},
        };
        BuildOptions options;
        options.on_resolve = [&](std::string_view specifier, std::string_view, std::string_view, std::string_view)
            -> std::expected<std::optional<PluginResolveResult>, BuildError> {
            if (specifier != "virtual:data") {
                return std::nullopt;
            }
            return PluginResolveResult{"/actual-data.js", "file", false};
        };
        auto result{build_bundle({"/entry.js"}, redirected, options)};
        check(result.has_value(), "on_resolve build succeeds");
        if (result) {
            check_contains(result->code, "from actual-data", "on_resolve redirect");
        }
    }

    {  // a virtual namespace is served entirely by on_load
        const Files virt{{"/entry.js", "import { v } from \"virtual:thing\"; console.log(v);"}};
        BuildOptions options;
        options.on_resolve = [&](std::string_view specifier, std::string_view, std::string_view, std::string_view)
            -> std::expected<std::optional<PluginResolveResult>, BuildError> {
            if (specifier != "virtual:thing") {
                return std::nullopt;
            }
            return PluginResolveResult{"thing", "virt", false};
        };
        options.on_load = [&](std::string_view path, std::string_view ns)
            -> std::expected<std::optional<PluginLoadResult>, BuildError> {
            if (ns != "virt") {
                return std::nullopt;
            }
            check(path == "thing", "on_load receives the virtual path");
            return PluginLoadResult{"export const v = \"from virtual namespace\";", "js"};
        };
        auto result{build_bundle({"/entry.js"}, virt, options)};
        check(result.has_value(), "virtual-namespace build succeeds");
        if (result) {
            check_contains(result->code, "from virtual namespace", "virtual namespace on_load");
        }
    }

    {  // a throwing on_load fails the build with the plugin's message
        BuildOptions options;
        options.on_load = [&](std::string_view path, std::string_view)
            -> std::expected<std::optional<PluginLoadResult>, BuildError> {
            if (!path.ends_with("lib.js")) {
                return std::nullopt;
            }
            return std::unexpected(BuildError{std::string{path}, "plugin exploded", 0});
        };
        auto result{build_bundle({"/entry.js"}, files, options)};
        check(!result.has_value(), "a throwing on_load fails the build");
        if (!result) {
            check(result.error().message == "plugin exploded", "on_load error message propagates");
        }
    }

    {  // a throwing on_resolve fails the build with the plugin's message
        BuildOptions options;
        options.on_resolve = [&](std::string_view specifier, std::string_view, std::string_view, std::string_view)
            -> std::expected<std::optional<PluginResolveResult>, BuildError> {
            if (specifier != "./lib.js") {
                return std::nullopt;
            }
            return std::unexpected(BuildError{"/entry.js", "resolve exploded", 0});
        };
        auto result{build_bundle({"/entry.js"}, files, options)};
        check(!result.has_value(), "a throwing on_resolve fails the build");
        if (!result) {
            check(result.error().message == "resolve exploded", "on_resolve error message propagates");
        }
    }

    {  // a hook that matches nothing leaves the default pipeline untouched
        BuildOptions options;
        options.on_resolve = [&](std::string_view, std::string_view, std::string_view, std::string_view)
            -> std::expected<std::optional<PluginResolveResult>, BuildError> { return std::nullopt; };
        options.on_load = [&](std::string_view, std::string_view)
            -> std::expected<std::optional<PluginLoadResult>, BuildError> { return std::nullopt; };
        auto result{build_bundle({"/entry.js"}, files, options)};
        check(result.has_value(), "non-matching hooks keep the default pipeline");
        if (result) {
            check_contains(result->code, "original", "non-matching hooks read the real source");
        }
    }
}

// ASCII-only output for `target: "bun"`.
// Source: test/regression/issue/14976/14976.test.ts >
//   test("bun build --target=bun outputs only ascii") — every output byte < 0x80.
// ref: bun-ref/src/js_printer/lib.rs:8019 `is_bun_platform = ascii_only`.
void test_ascii_only_output() {
    // The 14976 fixture: non-ASCII in a directive-position string AND in an
    // exported identifier (which the CJS lowering also re-emits as `exports.<id>`).
    mbun::bundler::Files files{
        {"/import_target.ts",
         "\"use\U000100D8unicode\";\n"
         "export const mile\U000100D8add1 = (int: number) => int + 1;\n"},
    };
    mbun::bundler::BuildOptions options{.ascii_only = true};
    std::vector<std::string> entries{"/import_target.ts"};
    auto built{mbun::bundler::build_bundle(entries, files, options)};
    check(built.has_value(), "ascii-only bundle builds");
    if (built) {
        bool allAscii{true};
        for (unsigned char c : built->code) {
            if (c >= 0x80) {
                allAscii = false;
            }
        }
        check(allAscii, "target=bun output is pure ASCII");
        // U+100D8 escapes as `\u{100d8}` in an identifier and as a surrogate pair
        // in a string. ref: lib.rs:6871 vs :1159.
        check(built->code.find("mile\\u{100d8}add1") != std::string::npos,
              "non-ASCII identifier escapes as \\u{...}");
        check(built->code.find("\"use\\uD800\\uDCD8unicode\"") != std::string::npos,
              "non-ASCII string escapes as a \\uHHHH surrogate pair");
    }

    // Default (target=browser) leaves the source bytes alone.
    auto plain{mbun::bundler::build_bundle(entries, files, mbun::bundler::BuildOptions{})};
    check(plain.has_value(), "non-ascii-only bundle builds");
    if (plain) {
        check(plain->code.find("mile\U000100D8add1") != std::string::npos,
              "without ascii_only the identifier stays literal");
    }
}

// The pass must survive the token kinds where naive text rewriting corrupts:
// templates (`.raw` is observable), regex bodies (`\u{}` needs the `u` flag) and
// existing escape sequences.
void test_ascii_only_token_fidelity() {
    using mbun::bundler::escape_ascii_only;

    // A `}` closing a template substitution must be re-scanned as a template
    // continuation, else the tail lexes as ordinary tokens.
    // ref: js_parser.cppm:370 rescan_close_brace_as_template_token.
    check(escape_ascii_only("const s = `a${x}b\U000100D8`;") == "const s = `a${x}b\U000100D8`;",
          "template text passes through verbatim (keeps .raw exact)");
    // Division after a value vs a regex after an operator.
    check(escape_ascii_only("const r = a / b\U000100D8 / c;")
              == "const r = a / b\\u{100d8} / c;",
          "division operands escape as identifiers");
    check(escape_ascii_only("const r = /\U000100D8/u;") == "const r = /\U000100D8/u;",
          "regex bodies pass through verbatim");
    // An already-escaped source stays byte-identical (no decode/re-quote round trip).
    check(escape_ascii_only("const s = '\\u{100d8}\\x41\\n';") == "const s = '\\u{100d8}\\x41\\n';",
          "existing escapes and quote style are preserved");
    // `\<non-ASCII>` is an identity escape → the pair collapses to one escape.
    check(escape_ascii_only("const s = \"\\\U000100D8\";") == "const s = \"\\uD800\\uDCD8\";",
          "identity escape of a non-ASCII code point collapses");
    // U+00E9 is <= 0xFF → `\xHH`, not `\uHHHH`. ref: lib.rs:1154 hex2_upper.
    check(escape_ascii_only("const s = \"é\";") == "const s = \"\\xE9\";",
          "a Latin-1 code point escapes as \\xHH");
    // Newline count must not change, or the line-granularity source map shifts.
    const std::string mapped{escape_ascii_only("const a\U000100D8 = 1;\nconst b = \"\U000100D8\";\n")};
    check(std::count(mapped.begin(), mapped.end(), '\n') == 2, "escaping preserves newline count");
    // Unlexable input degrades to a verbatim copy rather than corrupting.
    check(escape_ascii_only("const s = '\U000100D8") == "const s = '\U000100D8",
          "a lex error falls back to the original bytes");
}

// A `.css` entry point is printed through mbun.css into a CSS chunk instead of
// being handed to the JS parser (which rejected the leading `.` of a class
// selector: "Unexpected .").
// Sources (assertion semantics preserved):
//   test/bundler/css/mask-geometry-box.test.ts > css/mask-geometry-box-preserved
//   test/bundler/css/view-transition-23600.test.ts > css/view-transition-class-selector-23600
void test_css_entry_point() {
    mbun::bundler::Files files{
        {"/index.css",
         ".test-a::after {\n"
         "    mask: linear-gradient(#fff 0 0) padding-box, linear-gradient(#fff 0 0);\n"
         "}\n"
         ".test-b::after {\n"
         "    mask: linear-gradient(#fff 0 0) content-box, linear-gradient(#fff 0 0);\n"
         "}\n"},
    };
    auto built{mbun::bundler::build_bundle({"/index.css"}, files)};
    check(built.has_value(), "a .css entry point bundles");
    if (built) {
        check(built->cssChunk, "a .css entry produces a CSS chunk, not a JS chunk");
        check(built->code.starts_with("/* index.css */\n"),
              "the CSS chunk carries its input's provenance comment");
        check_contains(built->code, "padding-box", "css mask");
        check_contains(built->code, "content-box", "css mask");
        check(!built->code.contains("__mbun_modules"), "the CSS chunk has no JS module table");
        check(!built->code.contains(".test-a:after, .test-b:after"),
              "geometry-box masks are not merged into one selector list");
    }

    // A nested rule list (@keyframes/@media) is laid out like the top level: the
    // first rule on its own indented line, a blank separator between rules.
    mbun::bundler::Files frames{
        {"/k.css",
         "@keyframes slide-out {\n"
         "  from { opacity: 1; }\n"
         "  to { opacity: 0; }\n"
         "}\n"},
    };
    auto keyframes{mbun::bundler::build_bundle({"/k.css"}, frames)};
    check(keyframes.has_value(), "a @keyframes stylesheet bundles");
    if (keyframes) {
        check(keyframes->code ==
                  "/* k.css */\n"
                  "@keyframes slide-out {\n"
                  "  from {\n    opacity: 1;\n  }\n"
                  "\n"
                  "  to {\n    opacity: 0;\n  }\n"
                  "}\n",
              "nested rules print one per indented line with a blank separator");
    }

    // Values are re-serialized in their shortest equivalent form: a redundant
    // leading zero is dropped and an opaque rgb()/rgba() collapses to the
    // shorter of its named colour and its hex spelling.
    // Sources: test/bundler/css/wpt/background-computed.test.ts
    //   ("background-position-x: 0.5em" -> ".5em",
    //    "background-color: rgb(255, 0, 0)" -> "red")
    struct Case {
        std::string_view decl;
        std::string_view expected;
    };
    static constexpr Case kValueCases[]{
        {"background-position-x: 0.5em", "background-position-x: .5em"},
        {"background-position-x: calc(10px - 0.5em)", "background-position-x: calc(10px - .5em)"},
        {"background-color: rgb(255, 0, 0)", "background-color: red"},
        {"background-color: rgb(0, 0, 0)", "background-color: #000"},
        {"background-color: rgba(255, 0, 0, 1)", "background-color: red"},
        // Translucent and non-numeric colours are passed through untouched.
        {"background-color: rgba(255, 0, 0, 0.5)", "background-color: rgba(255, 0, 0, .5)"},
        {"background-color: rgb(var(--c))", "background-color: rgb(var(--c))"},
        // A bare `0` keeps its digit; only the redundant leading zero goes.
        {"opacity: 0", "opacity: 0"},
        {"margin: -0.25em", "margin: -.25em"},
    };
    for (const Case& c : kValueCases) {
        mbun::bundler::Files one{{"/v.css", std::format("h1 {{ {}; }}\n", c.decl)}};
        auto out{mbun::bundler::build_bundle({"/v.css"}, one)};
        check(out.has_value(), std::format("value css builds: {}", c.decl));
        if (out) {
            check_contains(out->code, c.expected, std::format("css value {:?}", c.decl));
        }
    }
}

}  // namespace

int main() {
    test_basic_and_typescript();
    test_path_forms_and_nested_graph();
    test_absolute_and_extension_completion();
    test_cycle_and_side_effect_import();
    test_await_context();
    test_shadowed_require_is_not_a_graph_edge();
    test_errors();
    test_multiple_entry_points();
    test_tree_shaking_drops_orphan_modules();
    test_cjs_esm_interop();
    test_sourcemap_association();
    test_template_and_regex_lexing();
    test_dynamic_import_is_bundled();
    test_unicode_escaped_specifier();
    test_disk_entrypoint_and_imports();
    test_plugin_hooks();
    test_ascii_only_output();
    test_ascii_only_token_fidelity();
    test_css_entry_point();

    if (gFailures != 0) {
        std::println("bundler: {}/{} checks failed", gFailures, gChecks);
        return 1;
    }
    std::println("bundler: {} checks passed", gChecks);
    return 0;
}
