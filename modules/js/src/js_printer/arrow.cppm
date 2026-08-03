// src/js_printer/arrow.cppm — module mbun.js_printer.arrow
//
// The three FUNCTION-VALUED expression arms of bun's `print_expr`.
//
// Blueprint: .mbun/bun-ref/src/js_printer/lib.rs
//   :3789  the `ExprData::EArrow` arm
//   :3835  the `ExprData::EFunction` arm
//   :3865  the `ExprData::EClass` arm
//
// ── Why this is its own module ───────────────────────────────────────────────
// expr.cppm is at 1688 lines and AGENTS.md 规则 10 caps a file at 2000, so these
// three arms get a file, exactly as print_func/print_class got func.cppm. The
// split is along bun's own seam: everything here is `print_expr` dispatch and
// the WRAP decision; everything after the name is func.cppm's.
//
// ── ⚠️ What this file replaces: a fallback that could not fail loudly ────────
// expr.cppm routed all three kinds to `print_deferred_shard_`, which probed for
// `print_expr_deferred` with `requires` and echoed the RAW SOURCE SPAN when the
// probe missed. Nothing ever defined `print_expr_deferred`, so the probe was
// ALWAYS false and all three kinds echoed source bytes verbatim, with none of
// the erasure edits applied. `const f = (x: number): string => x` printed its TS
// annotations straight through — invalid JS, emitted silently, with
// `TranspileResult::ok` still true. The old fallback's own comment claimed the
// echo "is always VALID output (the bytes parsed once already)"; that is false
// for exactly the TS input this printer exists to erase.
//
// So the probe is gone rather than satisfied. expr.cppm now calls these three
// methods DIRECTLY: a missing arm is a compile error, not a silent wrong answer.
// That is the same reason func.cppm exists — see its false-green note.
//
// ── The one structural difference from the blueprint: no `prefer_expr` ───────
// bun's `FnBody` is always a `stmts` list (g.rs:282), so an expression-bodied
// arrow is stored as `[SReturn(val)]` and a separate `prefer_expr` flag records
// that it was WRITTEN as an expression and must print back as one. bun's test is
// therefore three-part (:3812) — `stmts.len() == 1 && prefer_expr && stmts[0] is
// SReturn(Some(val))`.
//
// mbun's parser does not need any of that: `finish_arrow_` (js_parser.cppm)
// stores a braced body as a `Block` NODE and an expression body as the
// expression node ITSELF. The shape IS the signal, so the test is one kind
// comparison. This is not a liberty — it is the same trade func.cppm documents
// for `static {}` blocks ("the node's KIND, where bun's is the property's kind.
// Same members, same output, one indirection fewer") and for the class
// field/method split ("the absence IS the signal"). It is also why fnflags has
// no `PreferExpr` bit to spare (ast.cppm:252-271 spends all eight).
//
// Equivalence, case by case:
//   `x => x`            mbun: b=Identifier (not Block) → expr    bun: prefer_expr → expr
//   `x => { return x }` mbun: b=Block                   → block  bun: !prefer_expr → block
//   `x => {}`           mbun: b=Block (empty)           → block  bun: len 0 → block
// A `Paren` can never wrap a `Block` (a block is not an expression), so the kind
// test needs no `skip_parens_` — see print_arrow's body note.
export module mbun.js_printer.arrow;

import std;
import mbun.ast;
import mbun.js_printer.flags;
import mbun.js_printer.op_level;
import mbun.js_printer.whitespacer;  // ws<" => "> — ref :3809 `ws!(b" => ")`

export namespace mbun::js_printer {

// ─────────────────────────────────────────────────────────────────────────────
// ArrowPrinter — CRTP mixin. `D` is the assembled `Printer`.
//
// Reached through `self_()`, i.e. owned by other shards: `print_expr` /
// `print_identifier` (expr.cppm), `print_fn_args` / `print_func` / `print_class`
// (func.cppm), `print_block` / `arena()` / `node()` / `written()` /
// `print_space_before_identifier` / `add_source_mapping` (stmt.cppm), and the
// `stmtStart` / `exportDefaultStart` / `arrowExprStart` offsets
// (printer_core.cppm:138-140). This mixin holds no state of its own.
// ─────────────────────────────────────────────────────────────────────────────
template <typename D>
class ArrowPrinter {
private:
    [[nodiscard]] D& self_() { return static_cast<D&>(*this); }

    [[nodiscard]] const mbun::ast::Node& node_(mbun::ast::NodeIndex i) { return self_().node(i); }

    // ref :3837 / :3867 — EFunction and EClass share one wrap rule, and it is NOT
    // EObject's. Both test `stmt_start` and `export_default_start`; EObject
    // (expr.cppm:1215) tests `stmt_start` and `arrow_expr_start`. The reason is
    // grammatical: a `function`/`class` at the start of a statement is a
    // DECLARATION, and after `export default` it is also a declaration — but an
    // arrow body has no such ambiguity (`() => function(){}` is already an
    // expression). An object literal has the mirror-image problem: `{` is a block
    // at a statement start AND at an arrow-body start, but `export default {a:1}`
    // is unambiguous. Three offsets, two rules; using the wrong pair is silent.
    [[nodiscard]] bool wrap_at_decl_position_() {
        const std::int32_t written { self_().written() };
        return self_().stmtStart == written || self_().exportDefaultStart == written;
    }

public:
    // ═════════════════════════════════════════════════════════════════════════
    // print_arrow — ref :3789, the `EArrow` arm.
    //
    // `flags` is deliberately unused: bun's arm reads `level` only (for `wrap`)
    // and hands the BODY a freshly built `ExprFlag::ForbidIn.into()` (:3815)
    // rather than passing the incoming set down. Verified against bun 1.4.0
    // transformSync: `const f = () => a in b;` → `const f = () => (a in b);` —
    // the parens come from that unconditional ForbidIn, so dropping it (or
    // forwarding `flags` instead) silently changes output.
    // ═════════════════════════════════════════════════════════════════════════
    void print_arrow(mbun::ast::NodeIndex e, Level level, ExprFlagSet flags) {
        (void)flags;  // ref :3789-3833 — bun's arm never reads the incoming set
        const mbun::ast::Node& n { node_(e) };

        const bool wrap { level >= Level::Assign };  // ref :3790 `level.gte(Level::Assign)`

        if (wrap) {
            self_().print('(');  // ref :3792-3794
        }

        // ref :3796-3801
        if ((n.flags & mbun::ast::fnflags::IsAsync) != 0) {
            self_().add_source_mapping(e);
            self_().print_space_before_identifier();
            self_().print("async");
            self_().print_space();
        }

        // ref :3803-3808. bun's first argument is the sourcemap loc
        // (`if e.is_async { None } else { Some(expr.loc) }`); add_source_mapping
        // is a no-op (stmt.cppm:173) so there is nothing to thread, exactly as
        // print_fn_args' own signature note explains.
        self_().print_fn_args(self_().arena().list_of(n),
                              (n.flags & mbun::ast::fnflags::HasRestArg) != 0,
                              /*isArrow=*/true);
        self_().print_whitespacer(ws<" => ">);  // ref :3809

        // ref :3811-3827 — the `prefer_expr` test, as a kind comparison. See the
        // header for why the two are equivalent.
        //
        // ⚠️ No `skip_parens_` here, and that is deliberate. It would be needed
        // if this INSPECTED through the paren (expr.cppm:1398 — "EVERY structural
        // test in bun's print_expr must go through this"), but the only question
        // asked is "is this a Block?", and a `Paren` cannot wrap a Block. The
        // PRINTING is already paren-transparent (expr.cppm:883 recurses with the
        // same level), and — load-bearing — the Paren arm emits no bytes, so
        // `arrowExprStart == written()` still holds when the inner node's arm
        // runs. That is what makes both of these come out right through a Paren:
        //   `() => (1)`      → `() => 1`         (Level::Comma drops the paren)
        //   `() => ({a:1})`  → `() => ({ a: 1 })` (arrowExprStart re-derives it)
        const mbun::ast::NodeIndex body { n.b };
        if (body != mbun::ast::NONE && node_(body).kind != mbun::ast::NodeKind::Block) {
            self_().arrowExprStart = self_().written();  // ref :3814
            self_().print_expr(body, Level::Comma, ExprFlag::ForbidIn);  // ref :3815
        } else {
            // ref :3820-3826. bun's body is a struct that always exists; mbun's
            // is a node reference that can be NONE only on a parse error, which
            // never reaches the printer — but print_arrow is a public seam, so it
            // must not read `list_of` off a NONE node. Same guard, same reason, as
            // func.cppm's print_body_block_.
            self_().print_block(
                body == mbun::ast::NONE ? std::span<const mbun::ast::NodeIndex> {}
                                        : self_().arena().list_of(node_(body)),
                TopLevel::init(IsTopLevel::No));
        }

        if (wrap) {
            self_().print(')');  // ref :3829-3831
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // print_function_expr — ref :3835, the `EFunction` arm.
    //
    // The declaration twin is func.cppm's print_function_decl (:5307). They are
    // NOT the same code and the differences are bun's, not drift:
    //   • wrap        — expression only; a decl cannot need parens.
    //   • the space   — the decl arm's `*`/name split is an if/ELSE (:5326-5332),
    //                   so an unnamed `function` still gets
    //                   print_space_before_identifier. Here the call lives INSIDE
    //                   the name branch (:3855), so an anonymous function gets
    //                   nothing: `function() {}`, not `function () {}`. Confirmed
    //                   against bun 1.4.0 transformSync.
    //   • `export `   — decl only (:5318); an expression cannot be exported.
    // ═════════════════════════════════════════════════════════════════════════
    void print_function_expr(mbun::ast::NodeIndex e) {
        const mbun::ast::Node& n { node_(e) };
        const bool wrap { wrap_at_decl_position_() };  // ref :3836-3837

        if (wrap) {
            self_().print('(');  // ref :3839-3841
        }

        self_().print_space_before_identifier();  // ref :3843
        self_().add_source_mapping(e);            // ref :3844

        if ((n.flags & mbun::ast::fnflags::IsAsync) != 0) {
            self_().print("async ");  // ref :3845-3847 — the trailing space is bun's
        }
        self_().print("function");  // ref :3848

        // ref :3849-3852. `function*` keeps the space AFTER the star, giving
        // `function* () {}` / `async function* () {}` — reproduced from bun and
        // confirmed against it. It reads wrong and it is right; the same
        // counter-intuitive spacing shows up in property.cppm's `async* ag`.
        if ((n.flags & mbun::ast::fnflags::IsGenerator) != 0) {
            self_().print('*');
            self_().print_space();
        }

        // ref :3854-3858. bun prints the RENAMED symbol (`print_symbol`); mbun has
        // no renamer (printer_core.cppm:274 lists it DEFERRED) and bun's
        // transformSync does not rename either, so the name node's own text is
        // printed — exactly as print_function_decl (func.cppm:269) does.
        if (n.a != mbun::ast::NONE) {
            self_().print_space_before_identifier();
            self_().add_source_mapping(n.a);
            self_().print_identifier(node_(n.a).text);
        }

        self_().print_func(e);  // ref :3860 — FunctionExpr and FunctionDecl share
                                // a field layout (ast.cppm:64 / :107), so the same
                                // print_func serves both.
        if (wrap) {
            self_().print(')');  // ref :3861-3863
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // print_class_expr — ref :3865, the `EClass` arm.
    //
    // The declaration twin is func.cppm's print_class_decl (:5361). Note the
    // space: the decl arm prints `"class "` with a trailing space unconditionally
    // (:5376) because a declaration must be named; here the space is printed only
    // WITH the name (:3876), so an anonymous `class {}` gets none from this side —
    // print_class's own leading print_space (func.cppm:178) supplies the one
    // before the brace.
    // ═════════════════════════════════════════════════════════════════════════
    void print_class_expr(mbun::ast::NodeIndex e) {
        const mbun::ast::Node& n { node_(e) };
        const bool wrap { wrap_at_decl_position_() };  // ref :3866-3867

        if (wrap) {
            self_().print('(');  // ref :3868-3870
        }

        self_().print_space_before_identifier();  // ref :3872
        self_().add_source_mapping(e);            // ref :3873
        self_().print("class");                   // ref :3874

        // ref :3875-3879 — see print_function_expr's note on print_symbol.
        if (n.a != mbun::ast::NONE) {
            self_().print(' ');
            self_().add_source_mapping(n.a);
            self_().print_identifier(node_(n.a).text);
        }

        self_().print_class(e);  // ref :3880 — ClassExpr and ClassDecl share a
                                 // field layout (ast.cppm:65 / :108).
        if (wrap) {
            self_().print(')');  // ref :3881-3883
        }
    }
};

}  // namespace mbun::js_printer
