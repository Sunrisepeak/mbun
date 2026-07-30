// src/js_parser/cjs_runtime.cppm — module mbun.js_parser.cjs_runtime
//
// The vocabulary of the ESM → CommonJS lowering: the injected `__mbun_*` helper
// names, the prelude that defines them, and the pure string builders that shape
// a lowered `import` / live export.
//
// Why this is a module of its own (AGENTS.md 规则 10 — split by 职责): the CJS
// lowering *decisions* are inseparable from parsing (parse_import_/parse_export_
// must know what they are looking at), but the *emitted text* is not — every
// function here is a pure function of its arguments, touching no token, no AST
// node and no parser state. That makes this the half of the subsystem that can
// be read, reviewed and changed without the parser in your head, which is why it
// is also the half that has been churning (see the __mbun_X/__mbun_XP/
// __mbun_link commits). It imports only `std`.
//
// The parser side keeps the decisions; this module owns the strings.
export module mbun.js_parser.cjs_runtime;

import std;

export namespace mbun::js_parser::detail {

// Internal alias holding the real `Object` for every CJS-lowering helper this
// transpiler injects. Bound once by the __esModule prelude (see transpile()) to
// `({}).constructor`, which cannot be intercepted by a user `export function
// Object(){}` hoisted into the same module scope. Every injected reference to
// `Object` must go through this name; a bare `Object` in emitted code is a
// shadowing bug. The `__mbun_` prefix is this project's reserved namespace, so
// the alias itself is not a realistic collision (bun reserves `__` the same way
// — bun-ref src/runtime.js:10).
inline constexpr std::string_view kObjectAlias{"__mbun_O"};

// The live-binding export helper, `__mbun_X(key, () => local)`. An ESM export is
// a *live binding*: `exports.x = x` at the declaration publishes one snapshot,
// so a later write to `x` never reaches an importer. bun gives every export a
// getter instead — `__export(target, { name: () => local })` (bun-ref
// src/runtime.js:129-137; the bundler's call site is
// src/bundler/linker_context/doStep5.rs:568) — and a getter closure captures the
// local lexically, so unlike rewriting references it needs no scope table this
// erasure parser lacks.
//
// One helper per module, emitted in the prelude beside kObjectAlias rather than
// inline at each export site: the declaration form fires once per exported
// declaration, so an inline copy would repeat ~300 bytes across every one of
// them. This mirrors bun, which keeps a single `__export` in its runtime module.
inline constexpr std::string_view kLiveExportAlias{"__mbun_X"};

// The end-of-module re-push, `__mbun_XP(key, () => local)`. mbun lowers a named
// import to a `let` + an `__mbun_link` subscription that *pushes* later
// `exports.x = v` writes out to the binding (build_cjs_import_), because
// rewriting every reference to `ns.x` — the esbuild/bun answer — needs a scope
// table this parser lacks. A getter is a *pull*, so the two models meet only
// where __mbun_X finds a link accessor already installed (the __mbun_subs
// branch) and pushes one value through it. At the declaration that value is not
// final yet: `export var VET;` in a cycle pushes `undefined`, and the enum IIFE
// that fills VET on the next statement never reaches the importer — typebox's
// errors.mjs <-> function.mjs cycle is exactly this, and dies with
// "undefined is not an object (evaluating 'VET.Bad')".
//
// So push once more when the module body is done and every local has its final
// value. Only through an existing link accessor: with no cyclic importer there
// is nothing to push to, and the getter __mbun_X installed is already live.
inline constexpr std::string_view kLiveRepushAlias{"__mbun_XP"};

// The post-settle mutation notify, `__mbun_XN(__mbun_g<N>, <assignment>)`.
//
// THE HOLE THIS CLOSES. __mbun_X makes `exports.x` a live *pull*: the getter
// closes over the module-local, so `exports.x` is correct forever. The importer
// side is a *push*: `let x = g.x` plus an __mbun_link subscription. Those two
// meet only while the exporter is still evaluating — once it settles,
// __mbun_link takes its snapshot fast path and no subscription exists at all, so
// an assignment the exporter performs LATER (from an exported function called
// back into, which is what a test harness's `beforeAll` does) updates
// `exports.x` and never reaches the importer's binding. Measured minimal repro:
// mod.ts `export let x; export function init(){ x = 1 }` + main.ts
// `import {x, init}` printed `after: undefined` where bun prints `1`, while
// `import * as ns` already read `1` — proof that the exporter half was already
// right and only the importer's binding was stale.
//
// WHY THIS RATHER THAN REFERENCE REWRITING. esbuild and bun's bundler answer by
// rewriting every importer-side reference `x` -> `ns.x`, which needs the scope
// table this erasure parser deliberately does not have (a rewritten reference
// that was actually a shadowed local, an object-literal shorthand or a binding
// position is a silent miscompile). Notifying from the *writer* needs no scope
// analysis at all, because a notify aimed at the wrong binding is HARMLESS: the
// helper re-reads the value through the MODULE-SCOPE export getter rather than
// trusting the assigned one, so a wrap that fired for a shadowing local
// (`export let x; function f(){ let x; x = 1 }`) simply re-publishes the
// module-level x's unchanged value. Wrong-scope costs a redundant push; it
// cannot publish a wrong value. That asymmetry is what makes the transform safe
// without a symbol table, and it is the whole reason this shape was chosen.
//
// The value passed through is the ASSIGNMENT EXPRESSION's own value, returned
// unchanged, so the wrap is transparent in expression position — and because the
// helper re-reads via `g()` instead of using it, postfix `x++` (whose value is
// the OLD one) still publishes the new value.
//
// COST. Only assignments to a name this module exports live are wrapped, and the
// helper's fan-out list is only ever allocated when some importer actually
// subscribed — __mbun_link gates that on the `__mbun_mut` marker the tail emits
// (kLiveMutMarker), so a module that never reassigns an export costs one
// property load per import binding and nothing per read.
inline constexpr std::string_view kLiveNotifyAlias{"__mbun_XN"};

// The non-enumerable `exports.__mbun_mut` marker: the array of export KEYS this
// module reassigns after their declaration, emitted in the module tail so it is
// in place before any acyclic importer links. __mbun_link reads it to decide
// whether a settled export needs a fan-out subscription; absent (the
// overwhelming majority of modules) it keeps the plain snapshot fast path, so
// the marker is what stops this feature from taxing every import in the corpus.
// Non-enumerable because `exports` IS the namespace object — an enumerable
// property would show up in `Object.keys(ns)`, spread and `for…in`.
inline constexpr std::string_view kLiveMutMarker{"__mbun_mut"};

// A captured `{ a as b }` specifier: `name` is the in-braces identifier, `alias`
// the name after `as` (== `name` when there is no `as`). Import reads it as
// name→binding (`{ name: alias } = g`); export as binding→export (`exports.alias = name`).
struct NamedSpec {
    std::string name;
    std::string alias;
};

inline bool is_string_spec_name_(std::string_view name) {
    return !name.empty() && (name.front() == '"' || name.front() == '\'');
}
// `g.x` for an identifier name, `g["a-b"]` for a string one.
inline std::string member_ref_(std::string_view obj, std::string_view name) {
    return is_string_spec_name_(name) ? (std::string{obj} + "[" + std::string{name} + "]")
                                      : (std::string{obj} + "." + std::string{name});
}
// The name as a JS string literal, for passing as a property key.
inline std::string key_literal_(std::string_view name) {
    return is_string_spec_name_(name) ? std::string{name} : ("\"" + std::string{name} + "\"");
}

// The `type` import attribute, as require()'s second argument.
//
// An import attribute survives the ESM → CJS lowering only if it is carried into
// the emitted call: `import css from "./ui.css" with { type: "text" }` must still
// select the text loader once it has become `require("./ui.css")`, or the
// extension decides alone and the CSS is handed to the JS lexer ("Invalid
// character: '@'"). bun threads the same value into the same decision through
// JSC's ScriptFetchParameters (.mbun/bun-ref/src/runtime/jsc_hooks.rs:4062-4079
// reads `type_attribute` off the module request and passes it to
// get_loader_and_virtual_source, which applies it at bundler/options.rs:600-604);
// with no ESM linker in the JSC C API, this transpiler's require() call *is* the
// module request, so the attribute rides there.
//
// Shape mirrors the ESM spelling (`{ type: "…" }`) rather than inventing one, and
// the argument is simply absent when there is no attribute — so every require()
// this transpiler has ever emitted is byte-identical to before, and a user
// require() (which never sees this argument) is unaffected. The value is a loader
// name captured from a string literal (js_parser.cppm parse_import_attributes_),
// so it needs no escaping beyond the quotes.
// Does the module declare a top-level binding named `require`? Line-anchored
// scan (a nested declaration is indented, and a false positive only means the
// lowering uses the alias the runtime binds anyway) over `const|let|var|
// function|class require` followed by a non-identifier character.
inline bool declares_top_level_require_(std::string_view src) {
    static constexpr std::string_view kKeywords[]{"const ", "let ", "var ", "function ", "class "};
    std::size_t pos{0};
    while (pos < src.size()) {
        const std::size_t eol{src.find('\n', pos)};
        std::string_view line{src.substr(pos, eol == std::string_view::npos ? eol : eol - pos)};
        for (std::string_view keyword : kKeywords) {
            if (!line.starts_with(keyword)) continue;
            std::string_view rest{line.substr(keyword.size())};
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
                rest.remove_prefix(1);
            }
            if (rest.starts_with("require")) {
                const std::string_view after{rest.substr(7)};
                if (after.empty() || (std::isalnum(static_cast<unsigned char>(after.front())) == 0 &&
                                      after.front() != '_' && after.front() != '$')) {
                    return true;
                }
            }
        }
        if (eol == std::string_view::npos) break;
        pos = eol + 1;
    }
    return false;
}

// The identifier the ESM->CJS lowering calls for its OWN import statements.
// Normally the plain `require` the CommonJS wrapper injects; a module that
// itself declares a top-level `require` (node's test/common/index.mjs does:
// `const require = createRequire(import.meta.url)`) collides with that wrapper
// PARAMETER — "Cannot declare a const variable twice: 'require'", a hard
// SyntaxError before a single statement runs — so those modules get lowered
// against this reserved alias, which the runtime wrappers bind alongside
// `require`.
inline constexpr std::string_view kEsmRequireAlias{"__mbun_esm_require"};

inline std::string require_call_(std::string_view specRaw, std::string_view typeAttr,
                                 std::string_view requireName = "require") {
    std::string s{std::string{requireName} + "(" + std::string{specRaw}};
    // The third argument (1) marks this require() as a lowered ESM *import
    // statement* (ImportKind::Stmt). It lets the runtime report the correct
    // Node error code on a resolution failure — ERR_MODULE_NOT_FOUND for an
    // import statement vs MODULE_NOT_FOUND for a literal user require()
    // (bun jsc/ResolveMessage.rs get_code, :76-98). The second slot is the
    // import-attributes bag (undefined when absent); a user require() passes
    // neither and is unaffected.
    if (!typeAttr.empty()) {
        s += ", { type: \"" + std::string{typeAttr} + "\" }, 1";
    } else {
        s += ", undefined, 1";
    }
    s += ")";
    return s;
}

// Build the CJS lowering of an import: a single require() bound to a generated
// name, then default / namespace / named bindings off it. `uniq` seeds the
// generated identifier so multiple imports don't collide. `typeAttr` is the
// import's `type` attribute (empty when absent) — see require_call_.
inline std::string build_cjs_import_(std::uint32_t uniq, std::string_view def, std::string_view ns,
                              const std::vector<NamedSpec>& named, std::string_view specRaw,
                              bool sideEffect, std::string_view typeAttr = {},
                              std::string_view requireName = "require") {
    if (sideEffect && def.empty() && ns.empty() && named.empty()) {
        return require_call_(specRaw, typeAttr, requireName) + ";";
    }
    const std::string g{"__mbun_i" + std::to_string(uniq)};
    std::string s{"const " + g + " = " + require_call_(specRaw, typeAttr, requireName) + ";"};
    if (!def.empty()) {
        // interop: an ESM module we transpiled sets __esModule → take .default;
        // a real CJS module has no __esModule → the module.exports itself is the
        // default binding. The marker alone is NOT enough: TS-compiled CJS libs
        // (e.g. @grpc/grpc-js) set __esModule:true without ever assigning
        // exports.default — bun binds module.exports there, not undefined, so
        // require the property to actually exist before taking .default.
        s += " const " + std::string{def} + " = " + g + " && " + g + ".__esModule && \"default\" in " +
             g + " ? " + g + ".default : " + g + ";";
    }
    if (!ns.empty()) {
        // `import * as ns from "<cjs>"` — Node's (and bun's) CJS→ESM namespace
        // always carries a `default` binding whose value is module.exports
        // itself, on top of the detected named exports. A real CJS module never
        // assigns one, so attach it here; `js/bun/stream/direct-readable-stream`
        // reads `React.default.createContext` off a namespace import of the
        // CommonJS `react` package.
        //
        // It is defined **non-enumerably on module.exports** rather than by
        // copying into a fresh object: a copy would freeze the named bindings at
        // import time (killing the live-binding behaviour a cycle depends on) and
        // a Proxy would tax every namespace property read. Non-enumerable keeps
        // require()'s own view identical — Object.keys / JSON.stringify /
        // `for…in` over module.exports are unchanged — and the interop tests
        // (`g.__esModule && "default" in g`) still gate on __esModule, which this
        // does not add. Skipped when the module already exposes `default` (every
        // module we transpiled from ESM does) or is non-extensible.
        const std::string n{ns};
        s += " const " + n + " = " + g + " && (typeof " + g + " === \"object\" || typeof " + g +
             " === \"function\") && Object.isExtensible(" + g + ") && !(\"default\" in " + g +
             ") ? (Object.defineProperty(" + g + ", \"default\", { value: " + g +
             ", writable: true, configurable: true }), " + g + ") : " + g + ";";
    }
    if (!named.empty()) {
        // ESM named imports are *live bindings*, and a `const {x} = g`
        // destructure snapshots instead. That is invisible for an acyclic
        // import (the exporter has finished, so its value is already final)
        // but wrong for a cycle: the importer runs while the exporter is
        // mid-body, snapshots `undefined`, and never sees the assignment —
        // real ESM resolves it by the time the reference is evaluated.
        //
        // So bind `let` and subscribe each name to later `exports.x = …`
        // writes through __mbun_link (runtime/engine.inc; the bundler emits
        // a chunk-local fallback). Rewriting every reference to `g.x` — the
        // esbuild/bun-bundler answer — would need a scope table this erasure
        // parser does not have; pushing the write out to the binding needs
        // none. `let` costs the readonly-import TypeError on assignment to
        // an import, which TS rejects at compile time anyway.
        s += " let ";
        for (std::size_t i{0}; i < named.size(); ++i) {
            if (i != 0) {
                s += ", ";
            }
            s += named[i].alias + " = " + member_ref_(g, named[i].name);
        }
        s += ";";
        for (const NamedSpec& spec : named) {
            // The parameter must not be able to shadow the binding it assigns:
            // `import { v }` built `(v) => v = v`, whose parameter captures the
            // name, so the assignment hit the parameter and the pushed value was
            // dropped on the floor — a live binding that silently never updated.
            // `__mbun_` is this project's reserved prefix, so it cannot collide
            // with a user's imported name.
            s += " __mbun_link(" + g + ", " + key_literal_(spec.name) +
                 ", (__mbun_v) => " + spec.alias + " = __mbun_v);";
        }
    }
    return s;
}

}  // namespace mbun::js_parser::detail
