// src/js_printer/func.cppm — module mbun.js_printer.func
//
// The function/class SHAPE printer: everything after the name.
//
// Blueprint: .mbun/bun-ref/src/js_printer/lib.rs
//   :2487  `print_fn_args`   :2527  `print_func`   :2543  `print_class`
//   :5307  the `S::Function` arm   :5361  the `S::Class` arm
//
// ── Why this is its own module ───────────────────────────────────────────────
// bun has one ~6000-line `impl Printer` block; mbun splits it into CRTP mixins
// (the contract is documented in full at the top of printer_core.cppm). These
// five functions had no home: expr.cppm is at 1543 lines and AGENTS.md 规则 10
// caps a file at 2000, and stmt.cppm only ever NAMED `print_func`/`print_class`
// (its :19-20 header and the :827 comment) without defining them. So both shards
// called through `self_()` into methods that did not exist on the assembled
// `Printer` — see the false-green note below.
//
// ── ⚠️ What this file fixed, and what it means for the other test suites ─────
// stmt.cppm's `S::Function`/`S::Class` arms call `self_().print_function_decl` /
// `self_().print_class_decl`. NOTHING defined them. A CRTP mixin's member bodies
// are not instantiated until used, so `Printer::print_stmt` compiled fine and
// could not be INSTANTIATED at all — the real, assembled `Printer` could not
// print a single statement. Every printer suite was green because each assembles
// its own Printer-shaped harness type that stubs these two
// (test_js_printer_stmt.cpp:118-119, test_js_printer_binding.cpp:108-109,
// test_js_printer_property.cpp:101-102). tests/test_js_printer_real.cpp now
// instantiates the REAL Printer so that class of false green cannot regrow.
//
// `print_function_decl` / `print_class_decl` are mbun seam names, not bun ones:
// bun inlines both arms directly into its `print_stmt` match (:5307, :5361).
// They are functions here because stmt.cppm was written against that seam and
// because the arms need `print_func`/`print_class`, which live here.
//
// ── DEFERRED, and why (do not "fill these in" without the AST first) ─────────
//   :5311-5314  the name-is-mandatory panic — mbun's parser can produce a
//               FunctionDecl with no name node; printing nothing is not a crash.
//   :5318/:5372 `IsExport` — mbun models `export <decl>` as an ExportDecl WRAPPER
//               (ast.cppm's `── ESM ──` section) rather than as a flag on the
//               declaration, so the `export ` keyword is printed by stmt.cppm's
//               ExportDecl arm and no node reaches THIS file carrying IsExport.
//               The flag exists (ast.cppm fnflags::IsExport) and this file still
//               reads it, so it lights up if the parser ever sets it — but note
//               that would then DOUBLE the keyword, since the wrapper prints it.
//               (Was: "mbun ERASES ESM syntax rather than modelling it, so no node
//               reaches this printer carrying it." The erasure claim is stale —
//               ESM *is* modelled now; the reason this flag stays unset is the
//               wrapper, not the absence of a model.)
//   :5338-5351 / :5381-5391  module_info, and REWRITE_ESM_TO_CJS's
//               `print_bundled_export` — both are ports of their own.
//   :5320      `name_for_symbol` — bun prints the RENAMED symbol. mbun has no
//               renamer (printer_core.cppm:274 lists it DEFERRED), so the name
//               node's own text is printed, exactly as binding.cppm:203 does.
//   :5362-5365 SClass's "extra newline for readability" needs `prev_stmt_tag`
//               (printer_core.cppm:277 DEFERRED), so it is not emitted.
export module mbun.js_printer.func;

import std;
import mbun.ast;
import mbun.js_printer.flags;
import mbun.js_printer.op_level;
import mbun.js_printer.expr_op;
import mbun.js_printer.whitespacer;  // ws<" = "> — ref :2518 `ws!(b" = ")`

export namespace mbun::js_printer {

// ─────────────────────────────────────────────────────────────────────────────
// FuncPrinter — CRTP mixin. `D` is the assembled `Printer`.
//
// Reached through `self_()`, i.e. owned by other shards: `print_expr` /
// `print_identifier` (expr.cppm), `print_binding` (binding.cppm),
// `print_property` (property.cppm), `print_block` / `arena()` / `node()` /
// `written()` / `print_space_before_identifier` / `add_source_mapping`
// (stmt.cppm). This mixin holds no state of its own.
// ─────────────────────────────────────────────────────────────────────────────
template <typename D>
class FuncPrinter {
private:
    [[nodiscard]] D& self_() { return static_cast<D&>(*this); }

    [[nodiscard]] const mbun::ast::Node& node_(mbun::ast::NodeIndex i) { return self_().node(i); }

public:
    // ═════════════════════════════════════════════════════════════════════════
    // print_fn_args — ref :2487
    //
    // bun's `wrap` is `let wrap = true;` (:2494) — a constant, kept as a named
    // `if` around the parens because a later minifier arm is meant to make it
    // conditional (the `_is_arrow` parameter is there for exactly that, :2492
    // "is_arrow can be used for minifying later"). Not folded away: doing so
    // would silently drop the extension point bun is holding open.
    //
    // `open_paren_loc` is sourcemap-only (:2496-2497) and mbun's
    // add_source_mapping is a no-op (stmt.cppm:44-46), so it is not threaded —
    // the same call bun makes has nothing to record. Same reasoning as
    // print_block's signature (stmt.cppm:177-179).
    // ═════════════════════════════════════════════════════════════════════════
    void print_fn_args(std::span<const mbun::ast::NodeIndex> args, bool hasRestArg, bool isArrow) {
        (void)isArrow;  // ref :2492 — bun's `_is_arrow`, unused there too
        constexpr bool wrap { true };  // ref :2494

        if constexpr (wrap) {
            self_().print('(');
        }

        for (std::size_t i { 0 }; i < args.size(); ++i) {
            if (i != 0) {
                self_().print(',');
                self_().print_space();
            }

            // ref :2510 — the rest marker is FUNCTION-level and lands on the last
            // arg. See the parse_params_ note in js_parser.cppm for why the flag
            // lives on the function rather than on the arg.
            if (hasRestArg && i + 1 == args.size()) {
                self_().print("...");
            }

            // Each element is an `Arg` (ast.cppm, ref g.rs:332): a=binding,
            // b=default.
            const mbun::ast::Node& arg { node_(args[i]) };
            self_().print_binding(arg.a, TopLevelAndIsExport {});  // ref :2515

            // ref :2517-2520 — `print_whitespacer(ws!(b" = "))`, NOT print_equals:
            // the two differ when minifying (` = ` collapses to `=` either way,
            // but the whitespacer is what bun uses here) .
            if (arg.b != mbun::ast::NONE) {
                self_().print_whitespacer(ws<" = ">);
                self_().print_expr(arg.b, Level::Comma, expr_flag::none());
            }
        }

        if constexpr (wrap) {
            self_().print(')');
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // print_func — ref :2527. Everything after a function's NAME: `(a, b) {…}`.
    //
    // The caller prints `async`/`function`/`*`/the name; this prints the args and
    // the body. That split is bun's, and it is what lets one function serve the
    // declaration arm (:5336), the expression arm, and a method shorthand
    // (property.cppm's try_print_method_, ref :4880/:5017) — a method has no
    // `function` keyword, and this never prints one.
    // ═════════════════════════════════════════════════════════════════════════
    void print_func(mbun::ast::NodeIndex fn) {
        if (fn == mbun::ast::NONE) {
            return;
        }
        const mbun::ast::Node& n { node_(fn) };
        print_fn_args(self_().arena().list_of(n),
                      (n.flags & mbun::ast::fnflags::HasRestArg) != 0,
                      /*isArrow=*/false);  // ref :2528-2533
        self_().print_space();             // ref :2534
        // ref :2535-2540. bun's `func.body` is a `FnBody` STRUCT (g.rs:282), so
        // its `.stmts` is always a list; mbun's body is a Block NODE, and it can
        // be NONE — a TS overload signature / `abstract` method has no body at
        // all (`function f(): void;`). bun cannot reach that state here because
        // its parser drops such members before printing (parse_property.rs:123-
        // 127); mbun's parser drops them too, but print_func is a public seam and
        // must not read `list_of` off a NONE node.
        print_body_block_(n.b);
    }

    // ═════════════════════════════════════════════════════════════════════════
    // print_class — ref :2543. Everything after a class's NAME: the heritage
    // clause and the body.
    // ═════════════════════════════════════════════════════════════════════════
    void print_class(mbun::ast::NodeIndex cls) {
        if (cls == mbun::ast::NONE) {
            return;
        }
        const mbun::ast::Node& n { node_(cls) };

        // ref :2544-2548 — `Level::New.sub(1)`. The precedence is load-bearing:
        // `class A extends (B, C) {}` and `class A extends (() => B) {}` both
        // need the parens the level forces, and `extends` binds tighter than a
        // comma or an arrow.
        if (n.b != mbun::ast::NONE) {
            self_().print(" extends");
            self_().print_space();
            self_().print_expr(n.b, sub(Level::New, 1), expr_flag::none());
        }

        self_().print_space();       // ref :2550
        self_().add_source_mapping(cls);  // ref :2552 (class.body_loc)
        self_().print('{');          // ref :2553
        self_().print_newline();     // ref :2554
        self_().indent();            // ref :2555

        // The span is read once: print_property recurses into the arena, but
        // nothing in this printer mutates it, so the list stays valid.
        const std::span<const mbun::ast::NodeIndex> members { self_().arena().list_of(n) };
        for (const mbun::ast::NodeIndex m : members) {
            if (m == mbun::ast::NONE) {
                continue;
            }
            self_().print_semicolon_if_needed();  // ref :2558
            self_().print_indent();               // ref :2559

            // ref :2561-2574 — bun's `item.kind == G::PropertyKind::ClassStaticBlock`.
            // ⚠️ mbun's shape differs and the difference is the PARSER's, not a
            // choice made here: a `static {}` block is returned as a plain Block
            // node, never as a Property (js_parser.cppm's parse_class_member_;
            // ast.cppm's Property note spells out why it gets no
            // `class_static_block` slot). So the test that selects this arm is
            // the node's KIND, where bun's is the property's kind. Same members,
            // same output, one indirection fewer.
            if (node_(m).kind == mbun::ast::NodeKind::Block) {
                self_().print("static");                              // ref :2562
                self_().print_space();                                // ref :2563
                print_body_block_(m);                                 // ref :2565-2570
                self_().print_newline();                              // ref :2571
                continue;                                             // ref :2572
            }

            self_().print_property(m);  // ref :2575

            // ref :2577-2581 — a class FIELD (`x = 1`, `x`) has no value and is a
            // statement: it needs its `;`. A method HAS a value (the function) and
            // is terminated by its own `}`, so it only needs a newline. This is
            // why ast.cppm's Property maps `value -> b` and notes "omitted for
            // class fields" (g.rs:161) — the absence IS the signal.
            if (node_(m).b == mbun::ast::NONE) {
                self_().print_semicolon_after_statement();
            } else {
                self_().print_newline();
            }
        }

        self_().needsSemicolon = false;  // ref :2584
        self_().unindent();              // ref :2585
        self_().print_indent();          // ref :2586
        // ref :2587-2589 — the close-brace sourcemap entry is conditional; with
        // add_source_mapping a no-op there is nothing to guard.
        self_().print('}');              // ref :2590
    }

    // ═════════════════════════════════════════════════════════════════════════
    // print_function_decl — ref :5307, the `S::Function` arm of bun's print_stmt.
    //
    // stmt.cppm:830-835 has already done the arm's first three lines
    // (print_indent / print_space_before_identifier / add_source_mapping), which
    // is where mbun's seam is cut; this is the rest.
    // ═════════════════════════════════════════════════════════════════════════
    void print_function_decl(mbun::ast::NodeIndex s, TopLevel tlmtlo) {
        (void)tlmtlo;  // ref :5338-5351 — module_info only. DEFERRED.
        const mbun::ast::Node& n { node_(s) };

        // ref :5318-5321. See the DEFERRED note in this file's header: mbun's
        // parser never sets IsExport, so this is unreachable today and is here so
        // that it lights up if it ever does.
        if constexpr (!D::flags().rewriteEsmToCjs) {
            if ((n.flags & mbun::ast::fnflags::IsExport) != 0) {
                self_().print("export ");
            }
        }
        if ((n.flags & mbun::ast::fnflags::IsAsync) != 0) {
            self_().print("async ");  // ref :5322-5324
        }
        self_().print("function");  // ref :5325
        // ref :5326-5332 — `function*` needs no space before the name (`*` is a
        // separator), a plain `function` does. The else branch is
        // print_space_before_identifier, NOT an unconditional space: `function f`
        // needs one, but the writer may already have emitted one.
        if ((n.flags & mbun::ast::fnflags::IsGenerator) != 0) {
            self_().print('*');
            self_().print_space();
        } else {
            self_().print_space_before_identifier();
        }

        // ref :5334-5336. bun panics on a missing name (:5311); mbun prints none.
        if (n.a != mbun::ast::NONE) {
            self_().add_source_mapping(n.a);
            self_().print_identifier(node_(n.a).text);
        }
        self_().print_func(s);     // ref :5337
        self_().print_newline();   // ref :5353
    }

    // ═════════════════════════════════════════════════════════════════════════
    // print_class_decl — ref :5361, the `S::Class` arm of bun's print_stmt.
    // ═════════════════════════════════════════════════════════════════════════
    void print_class_decl(mbun::ast::NodeIndex s, TopLevel tlmtlo) {
        (void)tlmtlo;  // ref :5381-5390 — module_info only. DEFERRED.
        const mbun::ast::Node& n { node_(s) };

        // ref :5370-5374 — bun reads `s.is_export` (an S::Class field) where the
        // function arm reads a FnFlag; mbun carries both on the node's flag byte.
        if constexpr (!D::flags().rewriteEsmToCjs) {
            if ((n.flags & mbun::ast::fnflags::IsExport) != 0) {
                self_().print("export ");
            }
        }
        self_().print("class ");  // ref :5376 — the trailing space is bun's
        if (n.a != mbun::ast::NONE) {
            self_().add_source_mapping(n.a);      // ref :5377
            self_().print_identifier(node_(n.a).text);  // ref :5379
        }
        self_().print_class(s);   // ref :5380
        self_().print_newline();  // ref :5395 (the non-REWRITE_ESM_TO_CJS branch)
    }

private:
    // `print_block` over a Block NODE, tolerating NONE. bun's equivalent is
    // inline at both call sites because its body is a struct that always exists;
    // mbun's is a node reference that may not (see print_func's note).
    void print_body_block_(mbun::ast::NodeIndex body) {
        if (body == mbun::ast::NONE) {
            // No body to print. bun's `print_block` would still emit `{}`, but
            // bun never reaches it body-less; emitting `{}` here would INVENT a
            // body for a construct the parser deliberately erased.
            return;
        }
        self_().print_block(self_().arena().list_of(node_(body)),
                            TopLevel::init(IsTopLevel::No));
    }
};

}  // namespace mbun::js_printer
