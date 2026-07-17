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
inline std::string require_call_(std::string_view specRaw, std::string_view typeAttr) {
    std::string s{"require(" + std::string{specRaw}};
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
                              bool sideEffect, std::string_view typeAttr = {}) {
    if (sideEffect && def.empty() && ns.empty() && named.empty()) {
        return require_call_(specRaw, typeAttr) + ";";
    }
    const std::string g{"__mbun_i" + std::to_string(uniq)};
    std::string s{"const " + g + " = " + require_call_(specRaw, typeAttr) + ";"};
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
        s += " const " + std::string{ns} + " = " + g + ";";
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
