// src/js_parser/subst.cppm — module mbun.js_parser.subst
//
// Single-use symbol substitution ("const alias inlining"): the `minify_syntax`
// pass that rewrites
//
//     const c = context;  return handle(c);   ->   return handle(context);
//
// Blueprint (bun-ref, 1:1 coordinates):
//   gate      src/bundler/options.rs:1970  `tree_shaking = target.is_bun() || production;
//                                           inlining = tree_shaking;
//                                           if inlining { minify_syntax = true }`
//   gate test src/js_parser/visit/mod.rs:1514 `!minify_syntax || !dead_code_elimination -> return`
//   driver    src/js_parser/visit/mod.rs:1640-1686 (kind!=KVar, !is_export, LAST
//             declarator only, BIdentifier binding, use_count_estimate == 1)
//   stmt half src/js_parser/p.rs:2380  `substitute_single_use_symbol_in_stmt`
//   expr half src/js_parser/p.rs:2473  `substitute_single_use_symbol_in_expr`
//
// WHY THIS IS OBSERVABLE (and therefore not a cosmetic minification): it changes
// what `Function.prototype.toString()` reports. Elysia's Sucrose reads handler
// source text and asks, by regex, whether the context parameter is handed to an
// unknown function (sucrose.ts:546 `isContextPassToFunction`, called at :734
// with the MAIN parameter only — the alias list it computes at :723 is passed
// solely to `inferBodyReference`). So `handle(c)` never matches and `handle(
// context)` does: without this pass Sucrose leaves `context.body` unpopulated
// and the handler observes an empty body. Verified against bun 1.4.0 by running
// the real sucrose() over both texts.
//
// WHY A MODULE OF ITS OWN (AGENTS.md 规则 10): js_parser.cppm is already ~5.1k
// lines, far past the 2000-line cap, so new subsystems land as submodules. The
// seam is deliberately narrow — this pass speaks only `mbun.ast` (arena + node
// indices) and is called from exactly one place, transpile()'s printer branch.
//
// SCOPE OF THIS PASS vs bun. mbun's production transpiler is an ERASURE pass
// with no symbol table, no scope chain and no renamer (js_parser.cppm's note at
// the `__mbun_O` prelude). bun resolves `use_count_estimate` and unbound-ness
// off `p.symbols`, which does not exist here, so this module builds its own
// lexical scope tree and use counts over the arena. That analysis is private to
// the pass: nothing is written back onto the AST, so the erasure path is
// untouched by construction.
//
// CONSERVATISM RULE: every "don't know" answers NO-SUBSTITUTION. A missed inline
// prints the source as it stands (today's behaviour); a wrong inline silently
// changes what another program's `toString()` observes. The asymmetry is the
// whole reason the guards below are written the strict way round.
export module mbun.js_parser.subst;

import std;
import mbun.ast;

export namespace mbun::js_parser::detail {

using mbun::ast::Arena;
using mbun::ast::Node;
using mbun::ast::NodeIndex;
using mbun::ast::NodeKind;
using mbun::ast::NONE;

// p.rs:2473's three-state result. `Continue` means "the use is not in here and
// this subexpression is safe to reorder the replacement PAST — keep looking";
// `Failure` means "stop, something here may observe or change the value".
enum class Subst : std::uint8_t { Continue, Success, Failure };

class SingleUseSubstituter {
private:
    struct Scope {
        std::uint32_t parent{0xFFFF'FFFFu};
        bool isFunction{false};
        std::vector<std::pair<std::string_view, NodeIndex>> decls;
    };

    Arena& arena_;
    std::vector<Scope> scopes_;
    // scope-introducing node -> scope id (built in pass 1, replayed in pass 2)
    std::unordered_map<NodeIndex, std::uint32_t> scopeAt_;
    // Identifier REFERENCE node -> the declaration node it resolves to. Absent
    // => unbound (a read that may throw ReferenceError, hence not removable).
    std::unordered_map<NodeIndex, NodeIndex> refDecl_;
    std::unordered_map<NodeIndex, std::uint32_t> useCount_;
    // Declarations that are ever an assignment / ++ / -- target. bun gets this
    // for free from use_count_estimate + symbol flags; here it is explicit.
    std::unordered_set<NodeIndex> assigned_;
    // `eval` / `with` anywhere: either can reach a binding by name at runtime,
    // so no use count in the file can be trusted. Whole-file bail (bun tracks
    // this per-scope as `contains_direct_eval`, visit/mod.rs:1519).
    bool bail_{false};

public:
    explicit SingleUseSubstituter(Arena& arena) : arena_{arena} {}

    void run(NodeIndex program) {
        if (program == NONE) return;
        scopes_.push_back(Scope{0xFFFF'FFFFu, true, {}});
        scopeAt_[program] = 0;
        collect_(program, 0);
        if (bail_) return;
        resolve_(program, 0);
        if (bail_) return;
        rewrite_(program);
    }

private:
    static bool introduces_scope_(NodeKind k) {
        switch (k) {
            case NodeKind::Program:
            case NodeKind::Block:
            case NodeKind::Arrow:
            case NodeKind::FunctionExpr:
            case NodeKind::FunctionDecl:
            case NodeKind::ForStmt:
            case NodeKind::ForInStmt:
            case NodeKind::CatchClause:
            case NodeKind::SwitchStmt:
            case NodeKind::ClassExpr:
            case NodeKind::ClassDecl:
            case NodeKind::NamespaceDecl:
                return true;
            default:
                return false;
        }
    }

    static bool is_function_scope_(NodeKind k) {
        switch (k) {
            case NodeKind::Program:
            case NodeKind::Arrow:
            case NodeKind::FunctionExpr:
            case NodeKind::FunctionDecl:
                return true;
            default:
                return false;
        }
    }

    // A declaration's NAME child is a binding, never a reference — pass 2 must
    // not count it as a use. Same for a non-computed property key: `{a: 1}`
    // reads nothing called `a`.
    static bool child_is_decl_name_(NodeKind parent, int slot) {
        switch (parent) {
            case NodeKind::FunctionDecl:
            case NodeKind::FunctionExpr:
            case NodeKind::ClassDecl:
            case NodeKind::ClassExpr:
                return slot == 0;  // `a` = name
            default:
                return false;
        }
    }

    std::uint32_t enclosing_function_(std::uint32_t s) const {
        while (s != 0xFFFF'FFFFu && !scopes_[s].isFunction) s = scopes_[s].parent;
        return s == 0xFFFF'FFFFu ? 0u : s;
    }

    void declare_(std::uint32_t scope, std::string_view name, NodeIndex declNode) {
        if (name.empty()) return;
        scopes_[scope].decls.emplace_back(name, declNode);
    }

    NodeIndex lookup_(std::string_view name, std::uint32_t scope) const {
        for (std::uint32_t s = scope; s != 0xFFFF'FFFFu; s = scopes_[s].parent) {
            for (const auto& d : scopes_[s].decls)
                if (d.first == name) return d.second;
        }
        return NONE;
    }

    // Declare every name a binding pattern introduces. `BindingIdentifier` is
    // the only shape the substituter itself will ever accept as a candidate
    // (p.rs's `BData::BIdentifier` guard at visit/mod.rs:1659), but the others
    // must still be DECLARED so that pass 2 resolves references to them and
    // shadowing is honoured.
    void declare_binding_(NodeIndex b, std::uint32_t scope) {
        if (b == NONE) return;
        const Node& n = arena_.at(b);
        switch (n.kind) {
            case NodeKind::BindingIdentifier:
                declare_(scope, n.text, b);
                return;
            case NodeKind::BindingArray:
            case NodeKind::BindingObject:
                for (NodeIndex c : arena_.list_of(n)) declare_binding_(c, scope);
                return;
            case NodeKind::BindingElement:
                declare_binding_(n.a, scope);
                return;
            case NodeKind::BindingProperty:
                declare_binding_(n.b, scope);  // b = value binding; a = key
                return;
            case NodeKind::Arg:
                declare_binding_(n.a, scope);
                return;
            default:
                return;
        }
    }

    // ── pass 1: scopes + declarations (runs to completion before any reference
    // is resolved, which is what gives hoisting the right answer) ────────────
    void collect_(NodeIndex n, std::uint32_t scope) {
        if (n == NONE) return;
        const NodeKind kind = arena_.at(n).kind;

        std::uint32_t inner = scope;
        if (introduces_scope_(kind) && n != NONE) {
            auto it = scopeAt_.find(n);
            if (it == scopeAt_.end()) {
                inner = static_cast<std::uint32_t>(scopes_.size());
                scopes_.push_back(Scope{scope, is_function_scope_(kind), {}});
                scopeAt_[n] = inner;
            } else {
                inner = it->second;
            }
        }

        switch (kind) {
            case NodeKind::Identifier:
                // Direct `eval(...)` can resolve any binding by name at runtime.
                if (arena_.at(n).text == "eval") bail_ = true;
                break;
            case NodeKind::WithStmt:
                bail_ = true;
                break;
            case NodeKind::VarDecl: {
                const Node& d = arena_.at(n);
                const auto kindv = static_cast<mbun::ast::VarKind>(d.aux);
                // `var` hoists to the nearest function scope; let/const bind here.
                const std::uint32_t target = (kindv == mbun::ast::VarKind::Var)
                                                 ? enclosing_function_(inner)
                                                 : inner;
                for (NodeIndex dec : arena_.list_of(d))
                    if (dec != NONE) declare_binding_(arena_.at(dec).a, target);
                break;
            }
            case NodeKind::FunctionDecl:
            case NodeKind::ClassDecl: {
                const Node& d = arena_.at(n);
                if (d.a != NONE) {
                    const Node& nm = arena_.at(d.a);
                    // The name binds in the ENCLOSING scope, not the body's.
                    if (!nm.text.empty()) declare_(scope, nm.text, d.a);
                }
                break;
            }
            case NodeKind::Arg:
                declare_binding_(arena_.at(n).a, inner);
                break;
            case NodeKind::CatchClause:
                declare_binding_(arena_.at(n).a, inner);
                break;
            default:
                break;
        }

        walk_children_(n, [&](NodeIndex c) { collect_(c, inner); });
    }

    // ── pass 2: resolve references, count uses ───────────────────────────────
    void resolve_(NodeIndex n, std::uint32_t scope) {
        if (n == NONE) return;
        const NodeKind kind = arena_.at(n).kind;

        std::uint32_t inner = scope;
        if (introduces_scope_(kind)) {
            auto it = scopeAt_.find(n);
            if (it != scopeAt_.end()) inner = it->second;
        }

        if (kind == NodeKind::Identifier) {
            const NodeIndex decl = lookup_(arena_.at(n).text, inner);
            if (decl != NONE) {
                refDecl_[n] = decl;
                ++useCount_[decl];
            }
            return;
        }

        // An assignment / update TARGET is a write, not a substitutable read.
        if (kind == NodeKind::Assignment || kind == NodeKind::Update) {
            const NodeIndex t = arena_.at(n).a;
            if (t != NONE && arena_.at(t).kind == NodeKind::Identifier) {
                const NodeIndex decl = lookup_(arena_.at(t).text, inner);
                if (decl != NONE) assigned_.insert(decl);
            }
        }

        walk_children_(n, [&](NodeIndex c) { resolve_(c, inner); });
    }

    // Generic child enumeration over the flat record (a, b, c + list), with the
    // two slots that are NOT references filtered out.
    template <class F>
    void walk_children_(NodeIndex n, F&& f) {
        const Node& node = arena_.at(n);
        const NodeKind k = node.kind;

        // `{a: 1}` / `{[k]: 1}` — the key is only a reference when computed.
        const bool skipKey = (k == NodeKind::Property) && !(node.flags & mbun::ast::pflags::IsComputed);
        const bool skipBindKey = (k == NodeKind::BindingProperty) &&
                                 !(node.flags & mbun::ast::bflags::IsComputed);

        if (node.a != NONE && !child_is_decl_name_(k, 0) && !skipKey && !skipBindKey) f(node.a);
        if (node.b != NONE) f(node.b);
        if (node.c != NONE) f(node.c);
        for (NodeIndex c : arena_.list_of(node))
            if (c != NONE) f(c);
    }

    // ── p.rs:3006 tail — "can we reorder the replacement past this?" ──────────
    static bool is_primitive_literal_(NodeKind k) {
        switch (k) {
            case NodeKind::NumberLiteral:
            case NodeKind::StringLiteral:
            case NodeKind::BooleanLiteral:
            case NodeKind::NullLiteral:
            case NodeKind::BigIntLiteral:
                return true;
            default:
                return false;
        }
    }

    // bun `expr_can_be_removed_if_unused`. Deliberately a strict subset: only
    // shapes whose evaluation provably cannot throw, call, or observe anything.
    bool can_be_removed_if_unused_(NodeIndex e) const {
        if (e == NONE) return false;
        const Node& n = arena_.at(e);
        switch (n.kind) {
            case NodeKind::Identifier:
                // Bound read = pure. UNBOUND read may throw ReferenceError, so
                // it is NOT removable — this single line is what makes
                // `unboundFn(c)` refuse to inline while `handle(c)` accepts it
                // (verified against bun 1.4.0; see the header note).
                return refDecl_.find(e) != refDecl_.end();
            case NodeKind::NumberLiteral:
            case NodeKind::StringLiteral:
            case NodeKind::BooleanLiteral:
            case NodeKind::NullLiteral:
            case NodeKind::BigIntLiteral:
            case NodeKind::RegExpLiteral:
            case NodeKind::ThisExpr:
            case NodeKind::Arrow:
            case NodeKind::FunctionExpr:
                return true;
            case NodeKind::Paren:
                return can_be_removed_if_unused_(n.a);
            case NodeKind::ArrayLiteral: {
                for (NodeIndex c : arena_.list_of(n)) {
                    if (c == NONE) continue;
                    if (arena_.at(c).kind == NodeKind::SpreadElement) return false;
                    if (!can_be_removed_if_unused_(c)) return false;
                }
                return true;
            }
            case NodeKind::ObjectLiteral: {
                for (NodeIndex c : arena_.list_of(n)) {
                    if (c == NONE) continue;
                    const Node& p = arena_.at(c);
                    if (p.kind != NodeKind::Property) return false;
                    // A computed key or a spread can run arbitrary code.
                    if (p.flags & mbun::ast::pflags::IsComputed) return false;
                    if (p.flags & mbun::ast::pflags::IsSpread) return false;
                    if (p.b != NONE && !can_be_removed_if_unused_(p.b)) return false;
                }
                return true;
            }
            default:
                // Member/Index (getters may run), Call/New, Binary (valueOf),
                // Template, Await, Yield, ... — all assumed side-effecting.
                return false;
        }
    }

    void replace_with_(NodeIndex site, NodeIndex replacement) {
        // The arena stores nodes as flat records that reference children by
        // index, so copying the replacement's record over the use site's slot
        // rewrites the expression in place — no parent link has to be re-pointed
        // and no list has to be rebuilt.
        arena_.at(site) = arena_.at(replacement);
        // Keep the reference map coherent for chained aliases (`const c = ctx;
        // const d = c; f(d)`), which re-enter this pass at the same index.
        auto it = refDecl_.find(replacement);
        if (it != refDecl_.end())
            refDecl_[site] = it->second;
        else
            refDecl_.erase(site);
    }

    // ── p.rs:2473 `substitute_single_use_symbol_in_expr` ─────────────────────
    Subst subst_expr_(NodeIndex e, NodeIndex declNode, NodeIndex replacement, bool replCanBeRemoved) {
        if (e == NONE) return Subst::Continue;
        Node& n = arena_.at(e);

        switch (n.kind) {
            case NodeKind::Identifier: {
                auto it = refDecl_.find(e);
                if (it != refDecl_.end() && it->second == declNode) {
                    replace_with_(e, replacement);
                    return Subst::Success;
                }
                break;
            }
            case NodeKind::Paren: {
                const Subst r = subst_expr_(n.a, declNode, replacement, replCanBeRemoved);
                if (r != Subst::Continue) return r;
                break;
            }
            case NodeKind::Call:
            case NodeKind::New: {
                // Callee first (it is evaluated first), then the arguments — and
                // the arguments only if the replacement is itself removable,
                // because otherwise moving it past the callee would reorder two
                // side effects (p.rs:2518).
                const Subst r = subst_expr_(n.a, declNode, replacement, replCanBeRemoved);
                if (r != Subst::Continue) return r;
                if (replCanBeRemoved) {
                    for (NodeIndex arg : arena_.list_of(n)) {
                        const Subst ra = subst_expr_(arg, declNode, replacement, replCanBeRemoved);
                        if (ra != Subst::Continue) return ra;
                    }
                }
                break;
            }
            case NodeKind::SpreadElement: {
                const Subst r = subst_expr_(n.a, declNode, replacement, replCanBeRemoved);
                if (r != Subst::Continue) return r;
                break;
            }
            case NodeKind::Member: {
                const Subst r = subst_expr_(n.a, declNode, replacement, replCanBeRemoved);
                if (r != Subst::Continue) return r;
                break;
            }
            case NodeKind::Index: {
                Subst r = subst_expr_(n.a, declNode, replacement, replCanBeRemoved);
                if (r != Subst::Continue) return r;
                if (replCanBeRemoved) {
                    r = subst_expr_(n.b, declNode, replacement, replCanBeRemoved);
                    if (r != Subst::Continue) return r;
                }
                break;
            }
            case NodeKind::Binary: {
                Subst r = subst_expr_(n.a, declNode, replacement, replCanBeRemoved);
                if (r != Subst::Continue) return r;
                if (replCanBeRemoved) {
                    r = subst_expr_(n.b, declNode, replacement, replCanBeRemoved);
                    if (r != Subst::Continue) return r;
                }
                break;
            }
            case NodeKind::Sequence: {
                for (NodeIndex c : arena_.list_of(n)) {
                    const Subst r = subst_expr_(c, declNode, replacement, replCanBeRemoved);
                    if (r != Subst::Continue) return r;
                    // Only the FIRST element is unconditionally evaluated before
                    // a later one can observe the replacement; anything after it
                    // needs the removable guard.
                    if (!replCanBeRemoved) break;
                }
                break;
            }
            case NodeKind::Conditional: {
                // The test is unconditionally evaluated (p.rs:2740).
                const Subst r = subst_expr_(n.a, declNode, replacement, replCanBeRemoved);
                if (r != Subst::Continue) return r;
                // Both arms are conditional code paths; only a removable
                // replacement may be pushed into one of them.
                if (replCanBeRemoved) {
                    const Subst y = subst_expr_(n.b, declNode, replacement, replCanBeRemoved);
                    if (y == Subst::Success) return Subst::Success;
                    const Subst no = subst_expr_(n.c, declNode, replacement, replCanBeRemoved);
                    if (no == Subst::Success) return Subst::Success;
                    if (y != Subst::Continue || no != Subst::Continue) return Subst::Failure;
                }
                break;
            }
            case NodeKind::Logical: {
                // `a && b` — only `a` is unconditionally evaluated.
                const Subst r = subst_expr_(n.a, declNode, replacement, replCanBeRemoved);
                if (r != Subst::Continue) return r;
                break;
            }
            case NodeKind::Unary: {
                const Subst r = subst_expr_(n.a, declNode, replacement, replCanBeRemoved);
                if (r != Subst::Continue) return r;
                break;
            }
            case NodeKind::ArrayLiteral: {
                for (NodeIndex c : arena_.list_of(n)) {
                    const Subst r = subst_expr_(c, declNode, replacement, replCanBeRemoved);
                    if (r != Subst::Continue) return r;
                    if (!replCanBeRemoved) break;
                }
                break;
            }
            default:
                break;
        }

        // p.rs:3006 tail.
        if (replCanBeRemoved && can_be_removed_if_unused_(e)) return Subst::Continue;
        if (is_primitive_literal_(arena_.at(e).kind)) return Subst::Continue;
        return Subst::Failure;
    }

    // ── p.rs:2380 `substitute_single_use_symbol_in_stmt` ─────────────────────
    bool subst_stmt_(NodeIndex stmt, NodeIndex declNode, NodeIndex replacement, bool replCanBeRemoved) {
        if (stmt == NONE) return false;
        const Node& s = arena_.at(stmt);
        NodeIndex slot = NONE;
        switch (s.kind) {
            case NodeKind::ExpressionStmt:
            case NodeKind::ThrowStmt:
                slot = s.a;
                break;
            case NodeKind::ReturnStmt:
                slot = s.a;  // NONE for a bare `return;` — handled below
                break;
            case NodeKind::IfStmt:
            case NodeKind::SwitchStmt:
                slot = s.a;  // the test only; never the branches
                break;
            case NodeKind::VarDecl: {
                // Only the FIRST declarator's initializer, and only when its
                // binding is a plain identifier (p.rs:2407).
                const auto decls = arena_.list_of(s);
                if (decls.empty()) return false;
                const Node& d0 = arena_.at(decls[0]);
                if (d0.a == NONE || arena_.at(d0.a).kind != NodeKind::BindingIdentifier) return false;
                slot = d0.b;
                break;
            }
            default:
                return false;
        }
        if (slot == NONE) return false;
        return subst_expr_(slot, declNode, replacement, replCanBeRemoved) == Subst::Success;
    }

    // ── visit/mod.rs:1640 driver, over every statement list in the file ──────
    void rewrite_stmt_list_(Node& owner) {
        const auto span = arena_.list_of(owner);
        if (span.size() < 2) return;
        // Materialise immediately: `span` points into the arena's child pool,
        // which commit_list() below may reallocate.
        std::vector<NodeIndex> out{span.begin(), span.end()};
        const std::size_t originalCount = out.size();

        for (std::size_t i = 0; i + 1 < out.size();) {
            const NodeIndex stmt = out[i];
            if (stmt == NONE || arena_.at(stmt).kind != NodeKind::VarDecl) {
                ++i;
                continue;
            }
            Node& local = arena_.at(stmt);
            const auto kindv = static_cast<mbun::ast::VarKind>(local.aux);
            // `var` is function-scoped and may be referenced from anywhere in
            // the function, so its use count is not a local fact (visit/mod.rs:1642).
            if (kindv == mbun::ast::VarKind::Var || local.listCount == 0) {
                ++i;
                continue;
            }

            const auto decls = arena_.list_of(local);
            const NodeIndex lastDecl = decls[decls.size() - 1];
            if (lastDecl == NONE) {
                ++i;
                continue;
            }
            const Node& last = arena_.at(lastDecl);
            if (last.a == NONE || arena_.at(last.a).kind != NodeKind::BindingIdentifier ||
                last.b == NONE) {
                ++i;
                continue;
            }

            const NodeIndex declNode = last.a;
            auto uc = useCount_.find(declNode);
            if (uc == useCount_.end() || uc->second != 1 || assigned_.count(declNode) != 0) {
                ++i;
                continue;
            }

            const NodeIndex replacement = last.b;
            const bool replCanBeRemoved = can_be_removed_if_unused_(replacement);
            if (!subst_stmt_(out[i + 1], declNode, replacement, replCanBeRemoved)) {
                ++i;
                continue;
            }

            if (local.listCount == 1) {
                out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                // Drop just the last declarator; the rest of the statement stays
                // (visit/mod.rs:1681) — `const _ = ctx, a = ctx;` keeps `_`.
                --local.listCount;
            }
            // Re-test the SAME index so alias chains collapse in one sweep.
        }

        if (out.size() != originalCount) {
            owner.listStart = arena_.commit_list(out);
            owner.listCount = static_cast<std::uint32_t>(out.size());
        }
    }

    void rewrite_(NodeIndex n) {
        if (n == NONE) return;
        const NodeKind k = arena_.at(n).kind;
        switch (k) {
            case NodeKind::Program:
            case NodeKind::Block:
            case NodeKind::SwitchCase:
            case NodeKind::CatchClause:
            case NodeKind::FinallyClause:
                rewrite_stmt_list_(arena_.at(n));
                break;
            default:
                break;
        }
        // Re-read the node: rewrite_stmt_list_ may have re-pointed the list.
        const Node snapshot = arena_.at(n);
        if (snapshot.a != NONE) rewrite_(snapshot.a);
        if (snapshot.b != NONE) rewrite_(snapshot.b);
        if (snapshot.c != NONE) rewrite_(snapshot.c);
        // COPY the child list before recursing. `list_of` hands back a span into
        // the arena's flat child pool, and a nested rewrite_ may append to that
        // pool via commit_list() — which reallocates it and leaves the span
        // dangling. Iterating the span directly reads freed memory and walks off
        // into arbitrary node indices.
        const auto kids = arena_.list_of(arena_.at(n));
        const std::vector<NodeIndex> children{kids.begin(), kids.end()};
        for (NodeIndex c : children)
            if (c != NONE) rewrite_(c);
    }
};

// Entry point. Runs bun's `minify_syntax` single-use substitution over `program`
// in place. Safe to call on any tree: with nothing to inline it is a read-only
// traversal.
inline void substitute_single_use_symbols(Arena& arena, NodeIndex program) {
    SingleUseSubstituter{arena}.run(program);
}

}  // namespace mbun::js_parser::detail
