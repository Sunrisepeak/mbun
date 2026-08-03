// test_js_trim_imports.cpp — TS unused-import elision (js_parser/trim_imports.cppm).
//
// Every expectation below is the SEMANTIC content of what real bun 1.4.0
// (.mbun/bin/bun-rust) prints for the same input with the trim enabled, i.e.
//   new Bun.Transpiler({loader, trimUnusedImports:true}).transformSync(src)
// Byte equality is NOT the contract and must not be asserted here: this is the
// erasure path, which keeps the source's own quotes and whitespace, while bun
// re-prints from an AST. What has to match is WHICH imports survive — so each
// case asserts on the presence/absence of the import, not on layout.
//
// ⚠️ The oracle for this is Bun.Transpiler with the flag EXPLICITLY on, or
// `bun run` (which turns it on for TS itself). Bun.Transpiler's DEFAULT leaves
// unused imports alone — checked by the trim_off block at the bottom, which is
// the property the 9395-file corpus gate depends on.
import std;
import mbun.js_parser;

namespace {

int failures = 0;
int checks = 0;

std::string run(std::string_view src, bool trim, bool jsx = false) {
    mbun::js_parser::TranspileOptions o {};
    o.trim_unused_imports = trim;
    o.jsx = jsx;
    const mbun::js_parser::TranspileResult r { mbun::js_parser::transpile(src, o) };
    if (!r.ok) {
        return "<ERR: " + r.error + ">";
    }
    return r.code;
}

// `needle` must appear in the trimmed output.
void keeps(std::string_view name, std::string_view src, std::string_view needle,
           bool jsx = false) {
    ++checks;
    const std::string out { run(src, /*trim=*/true, jsx) };
    if (out.find(needle) == std::string::npos) {
        ++failures;
        std::println("  FAIL [{}] expected to KEEP {:?}", name, needle);
        std::println("       got: {:?}", out);
    }
}

// `needle` must NOT appear in the trimmed output.
void drops(std::string_view name, std::string_view src, std::string_view needle,
           bool jsx = false) {
    ++checks;
    const std::string out { run(src, /*trim=*/true, jsx) };
    if (out.find(needle) != std::string::npos) {
        ++failures;
        std::println("  FAIL [{}] expected to DROP {:?}", name, needle);
        std::println("       got: {:?}", out);
    }
}

}  // namespace

int main() {
    std::println("test_js_trim_imports — bun `trim_unused_imports` (oracle: bun 1.4.0)");

    // ── the import is dead: the whole statement goes ─────────────────────────
    drops("unused-default", "import d from 'y';\n", "'y'");
    drops("unused-named", "import {a} from 'y';\n", "'y'");
    drops("unused-namespace", "import * as ns from 'y';\n", "'y'");
    drops("unused-alias", "import {a as b} from 'y';\n", "'y'");
    // A name in a TYPE position is not a use. types.cppm builds no nodes for a
    // type, so `T` never reaches the identifier arm — which is exactly why these
    // work and why they are the whole point of the feature (`T` may not exist at
    // runtime at all).
    drops("type-annotation-only", "import {T} from 'y';\nlet x: T;\n", "'y'");
    drops("typeof-type-only", "import {T} from 'y';\nlet x: typeof T;\n", "'y'");
    drops("param-type-only", "import {T} from 'y';\nfunction f(x: T){}\n", "'y'");
    drops("implements-only", "import {I} from 'y';\nclass C implements I {}\n", "'y'");
    drops("type-argument-only", "import {T} from 'y';\nconst x = f<T>();\n", "'y'");
    drops("comment-is-not-a-use", "import {a} from 'y';\n// a\n", "'y'");

    // ── the import is live: it stays ─────────────────────────────────────────
    keeps("used-default", "import d from 'y';\nd();\n", "'y'");
    keeps("used-in-nested-fn", "import {a} from 'y';\nfunction f(){ return a; }\n", "'y'");
    keeps("namespace-member", "import * as ns from 'y';\nns.f();\n", "'y'");
    keeps("alias-used", "import {a as b} from 'y';\nb();\n", "'y'");
    keeps("extends-is-a-value", "import {B} from 'y';\nclass C extends B {}\n", "'y'");
    keeps("template-substitution", "import {a} from 'y';\nconst s = `${a}`;\n", "'y'");
    keeps("export-clause-is-a-use", "import {a} from 'y';\nexport {a};\n", "'y'");
    keeps("export-default-expr", "import d from 'y';\nexport default d;\n", "'y'");
    // The AST does not model the shorthand's value read (js_parser.cppm's
    // `DEFERRED` note on WasShorthand), so the trimmer marks the key itself.
    // Regression: bun's own fetch-h3.ts is `serve: { tls, http3: true }`.
    keeps("object-shorthand", "import {a} from 'y';\nconst o = {a};\n", "'y'");
    keeps("shorthand-in-default-export", "import {a} from 'y';\nexport default {a};\n", "'y'");
    // The decorator arm advances the identifier token by hand rather than going
    // through parse_primary_, so it needs its own mark.
    keeps("decorator-reference", "import {D} from 'y';\n@D class C {}\n", "'y'");
    // Order independence: harness.ts calls basename() at :61 and imports it at
    // :1208. Resolving marks eagerly (at reference time) trimmed this.
    keeps("use-precedes-import", "function f(){ return a; }\nimport {a} from 'y';\n", "'y'");

    // ── statements with no local binding are never trimmed ───────────────────
    keeps("bare-side-effect-import", "import 'y';\n", "'y'");
    keeps("re-export-from", "export {a} from 'y';\n", "'y'");
    keeps("re-export-star", "export * from 'y';\n", "'y'");

    // ── per-specifier trimming ───────────────────────────────────────────────
    // `import {a,b} from 'y'; a()` — bun prints `import { a } from "y";`. The
    // module stays, `b` goes.
    keeps("partial-clause-keeps-module", "import {a,b} from 'y';\na();\n", "'y'");
    keeps("partial-clause-keeps-live", "import {a,b} from 'y';\na();\n", "a");
    drops("partial-clause-drops-dead", "import {a,b} from 'y';\na();\n", "b");
    keeps("mixed-default-live-named-dead", "import d, {a} from 'y';\nd();\n", "'y'");
    keeps("type-and-value-keeps-value", "import {a,T} from 'y';\nlet x: T = a;\n", "'y'");

    // ── JSX. The lowering rewrites SOURCE, so the names it emits never reach
    // the identifier arm; jsx_lower collects them and the parser drains them.
    // With the automatic runtime an unused `React` IS dropped (verified).
    drops("jsx-automatic-drops-react", "import React from 'react';\nconst e = <div/>;\n",
          "'react'", /*jsx=*/true);
    keeps("jsx-component-name", "import {Foo} from 'y';\nconst e = <Foo/>;\n", "'y'", true);
    keeps("jsx-member-name", "import * as N from 'y';\nconst e = <N.X/>;\n", "'y'", true);
    keeps("jsx-attribute-value", "import {v} from 'y';\nconst e = <div a={v}/>;\n", "'y'", true);
    keeps("jsx-spread-attribute", "import {p} from 'y';\nconst e = <div {...p}/>;\n", "'y'", true);
    keeps("jsx-child-expression", "import {c} from 'y';\nconst e = <div>{c}</div>;\n", "'y'", true);

    // ── DEFAULT OFF. Bun.Transpiler keeps unused imports, and the corpus metric
    // is scored against it — this is the property the 0-diff gate rests on.
    {
        ++checks;
        const std::string out { run("import {T} from 'y';\nlet x: T;\n", /*trim=*/false) };
        if (out.find("'y'") == std::string::npos) {
            ++failures;
            std::println("  FAIL [trim-off-keeps] default path must NOT trim; got: {:?}", out);
        }
    }
    {
        // `import type` is erased whether or not trim is on — that is TS erasure,
        // not trimming, and conflating the two would hide a gap inside a
        // correct-looking result.
        ++checks;
        const std::string out { run("import type {T} from 'y';\nlet x: T;\n", /*trim=*/false) };
        if (out.find("'y'") != std::string::npos) {
            ++failures;
            std::println("  FAIL [import-type-erased] `import type` must erase; got: {:?}", out);
        }
    }

    // ── THE RUNTIME'S CONFIGURATION: cjs = true ──────────────────────────────
    // Everything above runs with cjs=false, which is Bun.Transpiler's shape — and
    // Bun.Transpiler is the one path where this feature is OFF. The path that
    // turns it ON (the module loader, module_loader.cppm transpile) always passes
    // cjs=true, and for a while that combination silently did nothing: the import
    // arm marked a CJS statement untrimmable because the require() lowering had
    // already claimed its span, so `bun run x.ts` still evaluated a module reached
    // only through a type. Every assertion below is therefore about the
    // configuration that actually ships; the cjs=false ones above cannot see it.
    //
    // Oracle: bun 1.4.0 `bun run` of the same shape (a module whose only use is a
    // type does not execute — its console.log never prints).
    {
        auto cjs = [](std::string_view src, bool trim, bool jsx = false) {
            mbun::js_parser::TranspileOptions o {};
            o.trim_unused_imports = trim;
            o.cjs = true;
            o.jsx = jsx;
            const mbun::js_parser::TranspileResult r { mbun::js_parser::transpile(src, o) };
            return r.ok ? r.code : ("<ERR: " + r.error + ">");
        };
        auto cjsCheck = [&](std::string_view name, std::string_view src, std::string_view needle,
                            bool want, bool jsx = false) {
            ++checks;
            const std::string out { cjs(src, /*trim=*/true, jsx) };
            if ((out.find(needle) != std::string::npos) != want) {
                ++failures;
                std::println("  FAIL [cjs/{}] expected to {} {:?}\n       got: {:?}", name,
                             want ? "KEEP" : "DROP", needle, out);
            }
        };

        // The payoff: no require('y') at all, so 'y' is never loaded.
        cjsCheck("type-only-drops-require", "import {T} from 'y';\nlet x: T;\n", "'y'", false);
        cjsCheck("unused-named-drops", "import {a} from 'y';\n", "'y'", false);
        cjsCheck("unused-default-drops", "import d from 'y';\n", "'y'", false);
        cjsCheck("unused-namespace-drops", "import * as ns from 'y';\n", "'y'", false);

        // …and the module must still load whenever anything actually uses it.
        // These are the five over-trims that a name-keyed tracker gets wrong if a
        // reference never reaches the identifier arm; each is a ReferenceError in
        // production, not a stray load.
        cjsCheck("value-use-keeps", "import {T} from 'y';\nconsole.log(T);\n", "'y'", true);
        cjsCheck("bare-import-keeps", "import 'y';\n", "'y'", true);
        cjsCheck("re-export-keeps", "export {a} from 'y';\n", "'y'", true);
        cjsCheck("partial-clause-keeps", "import {a,b} from 'y';\na();\n", "'y'", true);
        cjsCheck("decorator-keeps", "import {D} from 'y';\n@D class C {}\n", "'y'", true);
        cjsCheck("use-precedes-import-keeps", "function f(){ return a; }\nimport {a} from 'y';\n",
                 "'y'", true);
        cjsCheck("shorthand-keeps", "import {a} from 'y';\nconst o = {a};\n", "'y'", true);
        cjsCheck("jsx-component-keeps", "import {Foo} from 'y';\nconst e = <Foo/>;\n", "'y'", true,
                 /*jsx=*/true);
        cjsCheck("jsx-attribute-keeps", "import {v} from 'y';\nconst e = <div a={v}/>;\n", "'y'",
                 true, /*jsx=*/true);

        // Top-level `using`. The ESM path cannot trim here (the module-using
        // wrapper hoists and re-emits the import text, outrunning the erasure —
        // documented as known gap #1 in trim_imports.cppm). CJS has no import
        // statement left to hoist, so it trims and MATCHES bun, which drops it:
        //   Bun.Transpiler({loader:"ts",trimUnusedImports:true}).transformSync(
        //     "import {Server} from 'node:http';\nawait using h = f();\nconsole.log(1)")
        //   -> no node:http in the output (verified, bun 1.4.0).
        cjsCheck("using-toplevel-still-trims",
                 "import {S} from 'y';\nawait using h = f();\nconsole.log(1);\n", "'y'", false);

        // The default stays OFF in CJS too — the loader opts in, the shape does not.
        ++checks;
        const std::string off { cjs("import {T} from 'y';\nlet x: T;\n", /*trim=*/false) };
        if (off.find("'y'") == std::string::npos) {
            ++failures;
            std::println("  FAIL [cjs/trim-off-keeps] cjs default must NOT trim; got: {:?}", off);
        }
    }

    std::println("test_js_trim_imports: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
