// src/js_parser/module_scope.cppm — module mbun.js_parser.module_scope
//
// `export {a}` (no `from`) specifier filtering: a specifier survives iff its
// LOCAL name resolves to a module-scope VALUE binding. Everything else is
// silently dropped, so `export {a}` alone prints `export {};`.
//
// Blueprint (bun-ref, 1:1 coordinates):
//   filter    src/js_parser/visit/visit_stmt.rs:168 `s_export_clause` — the whole
//             pass. Its survivor test is ONE line (:227):
//                 if p.symbols[ref_.inner_index()].kind == symbol::Kind::Unbound { continue; }
//             i.e. "did find_symbol() resolve this name to a declared symbol?"
//             Everything below is that question re-asked against mbun's arena.
//   compact   visit_stmt.rs:214-216 `items.swap(end, i); end += 1;` then truncate
//             to `end` — the kept items are compacted into a PREFIX, in place.
//             This module does the same, for the same reason: no reallocation, so
//             no span can dangle.
//   JS error  visit_stmt.rs:197-207 — in a NON-TypeScript file an unresolved
//             specifier is a hard error ("X is not declared in this file"), not a
//             silent drop. See the DEVIATION note at the bottom.
//   re-export s.rs:79 `S::ExportFrom` is a DIFFERENT statement kind and never
//             reaches this pass: its names are the other module's, not local refs.
//             mbun's ExportForm::ClauseFrom is exempt here for the same reason.
//
// WHY A PARSER PASS AND NOT A PRINTER TEST. bun filters in the visit pass and
// leaves its printer dumb (lib.rs:5583 prints whatever items survived). Three
// reasons to keep that split:
//   1. `export {a}; let a=1` — bun keeps `a`. The answer depends on statements
//      the printer has not reached, so a print-time test cannot be order
//      independent without a pre-pass anyway.
//   2. mbun's printer must not import mbun.js_parser.* — js_parser.cppm already
//      imports mbun.js_printer, so the dependency would be a CYCLE.
//   3. It reuses the seam subst.cppm already cut: transpile()'s printer branch
//      runs post-parse AST passes there and nowhere else.
//
// WHY THE ERASURE PATH CANNOT SEE THIS (the hard constraint). This pass is called
// from exactly one place — transpile()'s `use_printer` branch — and it writes
// only `Node::listCount` on ExportDecl nodes. The erasure path reads `Arena::edits_`
// and never a node (ast.cppm:143), and the CJS clause lowering was already built
// from the parser's own `specs` vector at PARSE time (js_parser.cppm:1584-1600),
// not from the ClauseItem list this touches. So the default path is untouched by
// construction, and the corpus 0-diff gate proves it.
//
// SCOPE vs bun. bun answers "is this name declared?" with `p.symbols` + a real
// scope chain, which mbun has neither of. This module builds the ONE set that the
// question needs — module-scope value names — directly off the arena and throws it
// away. Nothing is written back onto the AST beyond the compaction. That is the
// same shape as subst.cppm, and for the same reason.
export module mbun.js_parser.module_scope;

import std;
import mbun.ast;

export namespace mbun::js_parser::detail {

using mbun::ast::Arena;
using mbun::ast::ExportForm;
using mbun::ast::Node;
using mbun::ast::NodeIndex;
using mbun::ast::NodeKind;
using mbun::ast::NONE;
using mbun::ast::VarKind;

// ─────────────────────────────────────────────────────────────────────────────
// ModuleScope — the module's declared VALUE names.
//
// bun's equivalent is `p.symbols` filtered by `find_symbol` from the module
// scope; the survivor test is `kind != Unbound` (visit_stmt.rs:227). A name is in
// this set iff bun would have resolved it to a non-Unbound symbol.
// ─────────────────────────────────────────────────────────────────────────────
class ModuleScope {
public:
    explicit ModuleScope(const Arena& arena) : arena_{&arena} {}

    void build(NodeIndex program) {
        if (program == NONE) {
            return;
        }
        for (NodeIndex s : arena_->list_of(arena_->at(program))) {
            collect_stmt_(s, /*topLevel=*/true);
        }
    }

    [[nodiscard]] bool has(std::string_view name) const { return names_.contains(name); }

private:
    const Arena* arena_;
    std::unordered_set<std::string_view> names_;

    // ── on `declare`, and why there is no erasure test here ──────────────────
    // A `declare const a: number` binds NOTHING, and an earlier draft of this
    // module answered that itself, by asking whether the declaration's source span
    // was covered by a pure-deletion Edit. That is no longer needed and would now
    // be dead weight: e15d048a made the parser return a `TypeScriptStmt` for the
    // whole `declare …` form (ref parse/parse_stmt.rs:1882 `S::TypeScript`), so a
    // declared binding never reaches this walk as a value-shaped node at all — it
    // lands on collect_stmt_'s `default:` and binds nothing, for free.
    //
    // Verified rather than assumed: with the erasure test stubbed to `false`, all
    // 9395 corpus files and all 56 vectors produce byte-identical output. Removed
    // on that evidence.
    //
    // ⚠️ ONE KNOWN HOLE, and it is the parser's, not this module's:
    // `declare global { var g: number }` mis-parses (the `global` block is not
    // recognised, so its `var g` survives as an ordinary block and hoists here),
    // giving `export { g };` where bun gives `export {};`. e15d048a records the
    // same gap at parse/parse_stmt.rs:1816. It predates this pass and is not
    // reachable through it in the corpus; fixing it belongs with `declare global`.

    void add_(std::string_view t) {
        if (!t.empty()) {
            names_.insert(t);
        }
    }

    // Every name a binding TREE binds, in source order — the leaves of the
    // pattern parse_binding_name_ builds. Mirrors js_parser.cppm's
    // collect_export_names_ (:1360): keys are not names (`{a: b}` binds only `b`),
    // computed keys are expressions, array holes bind nothing.
    //
    // Verified against bun: destructure-obj / -nest / -rest / -arr all KEEP.
    void collect_binding_names_(NodeIndex nameNode) {
        if (nameNode == NONE) {
            return;
        }
        const Node& n{arena_->at(nameNode)};
        if (n.kind == NodeKind::Identifier || n.kind == NodeKind::BindingIdentifier) {
            add_(n.text);
            return;
        }
        if (n.kind == NodeKind::BindingArray) {
            for (NodeIndex el : arena_->list_of(n)) {
                // BindingElement.a is the target; a `[, x]` hole is a bare
                // BindingMissing whose `.a` is NONE and binds nothing.
                collect_binding_names_(arena_->at(el).a);
            }
            return;
        }
        if (n.kind == NodeKind::BindingObject) {
            for (NodeIndex p : arena_->list_of(n)) {
                collect_binding_names_(arena_->at(p).b);  // .b = target, not key
            }
        }
    }

    // The local name a clause specifier BINDS on the import side.
    //   import { L as R }  → L is the other module's export, R is the local.
    // ast.cppm ClauseItem: a=L, b=R (NONE when the source wrote no `as`).
    void collect_import_item_(NodeIndex item) {
        const Node& ci{arena_->at(item)};
        const NodeIndex local{ci.b != NONE ? ci.b : ci.a};
        if (local == NONE) {
            return;
        }
        const Node& ln{arena_->at(local)};
        // A StringLiteral local is `import { "a-b" as c }`'s LEFT side only; the
        // local side is always an Identifier. Guard anyway.
        if (ln.kind == NodeKind::Identifier) {
            add_(ln.text);
        }
    }

    // ── the walk ─────────────────────────────────────────────────────────────
    // `topLevel` distinguishes "this statement's own bindings land in module
    // scope" from "we are only here to hoist `var` out of a nested block".
    //
    // The nesting rule, verified against bun 1.4.0 (probe set in the report):
    //   { let a=1; } export {a}          => export {};      (block-scoped)
    //   { var a=1; } export {a}          => export { a };   (var hoists)
    //   { function a(){} } export {a}    => export {};      (bun rewrites the
    //                                      block fn to a block-scoped
    //                                      `let a = function(){}`)
    //   function f(){ var a=1; } export {a} => export {};   (function scope stops
    //                                      the hoist)
    //   try{}catch(a){} export {a}       => export {};      (catch param)
    void collect_stmt_(NodeIndex s, bool topLevel) {
        if (s == NONE) {
            return;
        }
        const Node& n{arena_->at(s)};
        switch (n.kind) {
        case NodeKind::VarDecl: {
            // A nested `var` hoists to module scope; a nested let/const does not.
            const auto kind = static_cast<VarKind>(n.aux);
            if (!topLevel && kind != VarKind::Var) {
                return;
            }
            for (NodeIndex d : arena_->list_of(n)) {
                collect_binding_names_(arena_->at(d).a);
            }
            return;
        }
        case NodeKind::FunctionDecl:
        case NodeKind::ClassDecl:
        case NodeKind::EnumDecl:
        case NodeKind::NamespaceDecl:
            // Only a TOP-LEVEL declaration of these binds in module scope. A
            // function declaration in a block does NOT hoist out of it — bun
            // rewrites it to `let a = function(){}` (probe: fn-decl-in-block).
            if (topLevel) {
                collect_binding_names_(n.a);
            }
            return;

        case NodeKind::ImportDecl:
            // An import binding is a module-scope value (probe: import-named /
            // -default / -star all KEEP). A type-only import binds nothing, and
            // mflags::IsTypeOnly is what says so (ast.cppm).
            if (!topLevel || (n.flags & mbun::ast::mflags::IsTypeOnly) != 0) {
                return;
            }
            collect_binding_names_(n.a);  // default binding
            collect_binding_names_(n.b);  // `* as ns`
            for (NodeIndex it : arena_->list_of(n)) {
                collect_import_item_(it);
            }
            return;

        case NodeKind::ExportDecl: {
            // `export const a = 1` / `export function f(){}` declare in module
            // scope exactly as the bare form does — bun models it as the decl
            // carrying `is_export`, with no wrapper at all (ast.cppm's note).
            const auto form = static_cast<ExportForm>(n.aux);
            if (form == ExportForm::Decl || form == ExportForm::DefaultDecl) {
                collect_stmt_(n.a, topLevel);
            }
            return;
        }

        // ── containers: descend, but ONLY to hoist `var` ─────────────────────
        // Everything below is a non-function scope, so `var` inside it belongs to
        // the module. `topLevel` goes false so let/const/class/function inside
        // are correctly NOT collected.
        case NodeKind::Program:
        case NodeKind::Block:
        case NodeKind::FinallyClause:
            for (NodeIndex c : arena_->list_of(n)) {
                collect_stmt_(c, false);
            }
            return;
        case NodeKind::IfStmt:
            collect_stmt_(n.b, false);
            collect_stmt_(n.c, false);
            return;
        case NodeKind::ForStmt:
            collect_stmt_(n.a, false);  // the init may be a `var`
            for (NodeIndex c : arena_->list_of(n)) {
                collect_stmt_(c, false);
            }
            return;
        case NodeKind::ForInStmt:
            collect_stmt_(n.a, false);  // `for (var a of x)` (probe: for-var KEEPS)
            for (NodeIndex c : arena_->list_of(n)) {
                collect_stmt_(c, false);
            }
            return;
        case NodeKind::WhileStmt:
            collect_stmt_(n.b, false);
            return;
        case NodeKind::DoWhileStmt:
            collect_stmt_(n.a, false);
            return;
        case NodeKind::WithStmt:
            collect_stmt_(n.b, false);
            return;
        case NodeKind::LabeledStmt:
            collect_stmt_(n.a, false);  // probe: label-var KEEPS
            return;
        case NodeKind::SwitchStmt:
            for (NodeIndex c : arena_->list_of(n)) {
                collect_stmt_(c, false);
            }
            return;
        case NodeKind::SwitchCase:
            for (NodeIndex c : arena_->list_of(n)) {
                collect_stmt_(c, false);
            }
            return;
        case NodeKind::TryStmt:
            // list = the try body; b = CatchClause; c = FinallyClause.
            for (NodeIndex c : arena_->list_of(n)) {
                collect_stmt_(c, false);
            }
            collect_stmt_(n.b, false);
            collect_stmt_(n.c, false);
            return;
        case NodeKind::CatchClause:
            // `.a` is the catch PARAMETER — a catch-scoped binding that never
            // reaches module scope (probe: catch-param => export {};). Only the
            // body is walked, so a `var` inside it still hoists.
            for (NodeIndex c : arena_->list_of(n)) {
                collect_stmt_(c, false);
            }
            return;

        // FunctionDecl/ClassDecl bodies and every expression are NOT walked: a
        // `var` inside a function belongs to that function (probe: in-function /
        // arrow-var both => export {};). Expressions can only contain function
        // bodies, so they cannot contribute a module-scope `var` either.
        default:
            return;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// filter_export_clauses — ref visit_stmt.rs:168 `s_export_clause`.
//
// Drops every `export {…}` (no `from`) specifier whose local name is not a
// module-scope value binding, compacting the survivors into a prefix of the
// existing child span (visit_stmt.rs:214). In place: no commit_list, so no
// reallocation, so no live span can dangle.
//
// An emptied clause still PRINTS as `export {};` — that is bun (probe:
// `export {a}` => `export {};`), and it is load-bearing: the statement is what
// marks the file an ES module. The one form that vanishes entirely is
// `export {type a}` (every specifier type-only), which bun turns into
// `S::TypeScript` at parse time (parse_stmt.rs:1320) — mbun tags that
// ExportForm::None in the parser, so it never arrives here.
// ─────────────────────────────────────────────────────────────────────────────
void filter_export_clauses(Arena& arena, NodeIndex program) {
    if (program == NONE) {
        return;
    }
    ModuleScope scope{arena};
    scope.build(program);

    // Top-level only: an `export` is only legal at module scope, and mbun's parser
    // cannot produce a nested one.
    //
    // ⚠️ SNAPSHOT, not a live span. `list_of()` hands back a view into
    // `Arena::children_`; a span held across anything that appends to the arena
    // dangles (the exact shape that bit the rewrite_/commit_list path). Nothing in
    // this loop appends today — the copy costs one small vector and retires the
    // hazard by construction rather than by audit.
    const std::vector<NodeIndex> top{[&] {
        const std::span<const NodeIndex> live{arena.list_of(arena.at(program))};
        return std::vector<NodeIndex>{live.begin(), live.end()};
    }()};

    for (const NodeIndex s : top) {
        if (s == NONE || arena.at(s).kind != NodeKind::ExportDecl) {
            continue;
        }
        if (static_cast<ExportForm>(arena.at(s).aux) != ExportForm::Clause) {
            continue;  // ClauseFrom is exempt; every other form has no clause.
        }
        const std::span<NodeIndex> items{arena.list_of_mut(arena.at(s))};
        std::uint32_t end{0};
        for (std::size_t k = 0; k < items.size(); ++k) {
            const Node& ci{arena.at(items[k])};
            // ast.cppm ClauseItem: a=L, b=R. For `export { L as R }` the LOCAL
            // binding is L and the exported name is R — the opposite of the import
            // side. So the name to resolve is always `a`.
            const NodeIndex localNode{ci.a};
            if (localNode == NONE) {
                continue;
            }
            const Node& ln{arena.at(localNode)};
            // `export { "a-b" as c }` with no `from` is a syntax error in bun (a
            // string local is only legal in a re-export), so a StringLiteral here
            // cannot name a binding.
            if (ln.kind != NodeKind::Identifier || !scope.has(ln.text)) {
                continue;
            }
            items[end] = items[k];  // ref visit_stmt.rs:214 `items.swap(end, i)`
            ++end;
        }
        arena.at(s).listCount = end;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DEVIATIONS — recorded, not silently absorbed.
//
// 1. NON-TYPESCRIPT input drops instead of erroring. visit_stmt.rs:197-207 makes
//    an unresolved specifier a hard error when `!TYPESCRIPT`:
//        export {a}     .js  => SyntaxError: "a" is not declared in this file
//                       .ts  => export {};
//    (verified, 1.4.0, all four loaders.) mbun's transpile has no loader
//    discriminator to branch on — TS erasure is applied unconditionally — so
//    there is nothing here to test. Implementing it means threading a loader
//    through TranspileOptions; it is a ~1h change and its own slice. Dropping is
//    the TS answer, and every file this pass currently runs on is parsed as TS.
//
// 2. TYPE-ONLY NAMESPACES are kept as values. bun erases a namespace with no
//    value members and never declares its symbol, so it is Unbound:
//        namespace N{ type T=1 }  export {N}   => export {};   (bun)
//    (parse_typescript.rs:352 `stmts.len() == import_equal_count` => S::TypeScript.)
//    mbun cannot answer this: parse_namespace_ (js_parser.cppm:1074) lowers EVERY
//    namespace to an IIFE unconditionally and stores NO body on the node — the
//    `list=body` in ast.cppm's NodeKind comment is aspirational, `commit_list` is
//    never called for it. So the emptiness test has nothing to read. Keeping the
//    name matches mbun's OWN erasure output (which does emit `var N`), which is
//    the self-consistent choice while that gap stands. Closing it properly means
//    giving NamespaceDecl a body list and porting the :352 test — ~3h, and it
//    belongs with the namespace PORT, not here.
//
//    ⚠️ That port is now elysia's gate, and it is worth knowing why this
//    deviation is currently UNREACHABLE in practice: stmt.cppm has no arm for
//    NamespaceDecl at all, so a file containing one records `no port for
//    NamespaceDecl` and fails as a whole — the export clause is never reached.
//    Measured: elysia's `src/sucrose.ts:8 export namespace Sucrose` is the ONLY
//    remaining unported kind in its 39 src files (the other two failures,
//    src/index.ts and src/type-system/index.ts, were this pass's `export {…}` and
//    are now ok). So the namespace port subsumes this deviation: whoever gives
//    NamespaceDecl a body list to print it will hand this pass the emptiness test
//    for free, and both should land together.
//
// 3. `import a = require("y")` binds nothing here. mbun rewrites the statement in
//    place and leaves an ImportDecl with empty text/a/b/list (mflags::IsTsImportEquals,
//    ast.cppm) — the name is not on the node to collect. bun keeps it
//    (probe: import-eq => `const a = require("y"); export { a };`). The fix is the
//    AST change mflags::IsTsImportEquals already documents as DEFERRED (give the
//    node a VarDecl); ~1h, and it fixes the printer's own IsTsImportEquals hole at
//    the same time. Recorded there rather than worked around here.
// ─────────────────────────────────────────────────────────────────────────────

}  // namespace mbun::js_parser::detail
