// src/js_printer/stmt.cppm — module mbun.js_printer.stmt
//
// CAP-BUILD-PRINTER shard 3/4 — the statement printer.
//
// A 1:1 port of the `S::*` half of bun's `Printer` impl block, per AGENTS.md
// 「移植三段法」. Blueprint: .mbun/bun-ref/src/js_printer/lib.rs
//
//   :5289  `print_stmt`              — the match this file is built around
//   :2173  `print_body`              :2194  `print_block_body`
//   :2201  `print_block`             :2224  `print_two_blocks_in_one`
//   :2242  `print_decls`             :6748  `print_decl_stmt`
//   :6535  `print_for_loop_init`     :6585  `print_if`
//   :6669  `wrap_to_avoid_ambiguous_else`
//   :2130  `print_space_before_identifier`   (see PROVISIONAL below)
//   :2450  `add_source_mapping`             (see PROVISIONAL below)
//
// The shard contract is documented at the top of printer_core.cppm — read it
// first. In short: this is a CRTP mixin, cross-shard calls go through `self_()`,
// and `print_expr` / `print_binding` / `print_symbol` / `print_func` /
// `print_class` resolve at instantiation against whatever shard 2/4 land.
// (`print_func` / `print_class` / `print_function_decl` / `print_class_decl` now
// land in func.cppm.)
//
// ═════════════════════════════════════════════════════════════════════════════
// PROVISIONAL — three things this file owns that it should not
// ═════════════════════════════════════════════════════════════════════════════
// The shard contract did not home these, and shard 1 did not port them. They are
// parked here because shard 3 cannot print a statement without them and the
// files they belong in were being edited in parallel. Each is a small move:
//
//  1. `arena_` + `bind_arena` / `arena` / `node`.
//     bun's AST is by-value (`Stmt` carries its own data), so bun's Printer has
//     no arena field to port. mbun's is a flat arena, so EVERY shard needs a
//     `const ast::Arena&`. It belongs in PrinterCore next to `options`. Shards
//     2/4: reach it as `self_().arena()` / `self_().node(i)` — do NOT add a
//     second arena pointer to your own mixin, or the two will drift out of sync.
//
//  2. `print_space_before_identifier` (:2130) + `written` / `prev_char`.
//     lib.rs:2130 is squarely in the region printer_core.cppm ported (:1908
//     -:2650); it was skipped because `StringSink` exposes no way to look at the
//     bytes already written. Shard 2 needs it as much as shard 3 does (it is
//     what keeps `x instanceof y` from minifying to `xinstanceof y`).
//     Shards 2/4: call `self_().print_space_before_identifier()`. Do NOT
//     redefine it — two definitions in two mixins is an ambiguous base at
//     assembly, and this one is the ported-from-blueprint copy.
//
//  3. `add_source_mapping` (:2450) — a deliberate no-op. `source_map_builder` is
//     listed DEFERRED at printer_core.cppm:281. The calls are kept at every site
//     bun has one so the sourcemap port is a body-fill, not an archaeology dig.
//
// ⚠️ None of this is wired into `mbun::js_parser::transpile`. transpile is still
// erasure-based; this printer prints nothing in production yet. See printer.cppm.
// ═════════════════════════════════════════════════════════════════════════════
//
// ═════════════════════════════════════════════════════════════════════════════
// MODULE-SYNTAX — CLOSED
// ═════════════════════════════════════════════════════════════════════════════
// This block used to say `import` could not be printed from this arena at all,
// and that no amount of work in this file would change it. That was true and is
// no longer: the gap was on the PARSER/AST side, and it has been closed there.
// ast.cppm now has an `── ESM ──` section (ImportDecl/ExportDecl/ClauseItem +
// ExportForm + mflags), parse_import_/parse_export_ populate it, and the printer
// arms live in module_syntax.cppm. print_stmt dispatches to them below.
//
// What that bought, measured on the 9395-file corpus with the AST-rebuild printer
// on: agreement with real bun 1.4.0 went 1552 -> 3005 files (16.5% -> 32.0%),
// and on the 4640 files containing module syntax 524 -> 1977 (11.3% -> 42.6%),
// with 0 files regressed and 0 new parse errors. All 28 module-syntax vectors are
// byte-identical to Bun.Transpiler.
//
// ── ⚠️ PICK THE RIGHT ORACLE — the two bun modes DISAGREE ────────────────────
// (Kept, because it is the single most expensive mistake available here.)
// `bun build --no-bundle` is NOT the reference for this path, `Bun.Transpiler`
// is. They differ on unused imports, because `bun build` tree-shakes:
//
//      import d from 'y'      --no-bundle  =>  ""     (dropped: `d` unused)
//                             transformSync =>  import d from "y";
//
// mbun's `transpile` is a transpiler, not a bundler, so transformSync is the
// target. Measured against `bun build`, the unused-import drop makes `import`
// printing look like it needs bun's `ImportRecord` table (import_record.rs:15),
// `Symbol::use_count_estimate` (ast/symbol.rs:49) and the post-visit
// `ImportScanner` (p.rs:8313-8315). mbun has NONE of those — no symbol table, no
// scopes, no visit pass — so that reading turns an AST-modelling job into an
// engine rewrite and gets it wrongly abandoned. Two agents reached exactly that
// wrong conclusion from exactly that oracle. Against the correct one, imports
// need NO symbol table: bun's `print_symbol` (lib.rs:2472) is just
// `print_identifier(name_for_symbol(ref))`, and with no bundling/renaming that is
// the node's own text — which mbun already prints (binding.cppm:203).
//
// ── `export {…}` (no `from`) — CLOSED, and NOT in this file ──────────────────
// A specifier survives iff its local name is a MODULE-SCOPE VALUE binding;
// otherwise bun silently drops it. Verified, transformSync:
//
//      export {a, b as c}              =>  export {};       // neither declared
//      let a=1; export {a, b as c}     =>  export { a };    // only `a` resolves
//
// KEPT:    let/const/var, function/class decl, TS enum + namespace (they lower
//          to values), any import binding (named/default/star), and every name
//          a destructuring pattern binds (`let {x:{a}}=o`, `let {...a}=o`, …).
//          Order-independent: `export {a}; let a=1` still prints `export { a }`.
//          `var` hoists OUT of blocks (`{ var a=1; } export {a}` keeps `a`).
// DROPPED: undeclared; declared only in a nested block (`{ let a=1; }`) or
//          inside a function (`function f(){ var a=1; }`); a function decl in a
//          block (bun rewrites it to a block-scoped `let a = function(){}`); a
//          catch param; and TS type-only decls (`type`/`interface`/`declare`).
// `export {…} FROM 'y'` is exempt and IS printed: re-export names are not local
// refs, so no resolution is needed.
//
// That filter lives in js_parser/module_scope.cppm, not here, because bun puts it
// in the PARSER too: `s_export_clause` (visit/visit_stmt.rs:168) compacts the dead
// specifiers out during the visit pass, and the printer (lib.rs:5583) prints
// whatever is left without ever asking a scope question. It has to work that way —
// `export {a}; let a=1` keeps `a`, so the answer depends on statements the printer
// has not reached. By the time the arm below runs, the node's ClauseItem list holds
// exactly the survivors, and printing it verbatim is correct.
//
// ── THE `default:` SWALLOW IS CLOSED ─────────────────────────────────────────
// print_stmt's dispatch used to end in a blanket `default: break;`. That arm was
// how EVERY gap above stayed invisible: an unported kind produced no bytes and
// no complaint, and `TranspileResult::ok` only ever reflected the PARSE
// (js_parser.cppm:5126), so a file could lose its enums, its namespaces or its
// whole `export {…}` and still be reported a success.
//
// It is gone. The switch now lists every NodeKind explicitly, so a kind added to
// ast.cppm is a -Wswitch diagnostic instead of a silent drop, and the kinds that
// are reachable-but-unported call `record_unsupported_` (js_printer/
// unsupported.cppm) which surfaces as `ok == false` with a reason. This is the
// statement-side of what b9c11ca6 did to print_expr when it deleted the
// `requires` probe: make the absence a compile error where possible, and a loud
// runtime one where not.
//
// ⚠️ Erasure is NOT a gap and must not be recorded: TypeAliasDecl /
// InterfaceDecl / TypeScriptStmt print nothing because nothing IS bun's answer
// (each transforms to "" on real bun 1.4.0). Conflating those two was itself a
// bug — `export enum` used to share the erased forms' `default:`, so a real gap
// hid inside a correct one.
// ═════════════════════════════════════════════════════════════════════════════
export module mbun.js_printer.stmt;

import std;
import mbun.ast;
import mbun.js_printer.flags;
import mbun.js_printer.op_level;
import mbun.js_printer.whitespacer;

export namespace mbun::js_printer {

// ─────────────────────────────────────────────────────────────────────────────
// is_identifier_part_byte — ref lexer_tables.rs:817 `is_identifier_continue`,
// which forwards to `bun_core::identifier::is_identifier_part(codepoint: i32)`.
//
// ⚠️ bun feeds this a SINGLE BYTE: `print_space_before_identifier` (:2133) calls
// `is_identifier_continue(self.writer.prev_char() as i32)`, and `prev_char` is a
// `u8` (:7401). So for any byte >= 0x80 — i.e. the tail byte of a multi-byte
// UTF-8 sequence — bun asks "is codepoint U+0080..U+00FF an identifier part?",
// not "is the character that byte belongs to one?". That is latin1-vs-UTF-8
// confusion, but it is the blueprint's behavior and it is observable, so it is
// reproduced exactly rather than "fixed": mbun's job is to match bun's bytes.
//
// The Latin-1 supplement's ID_Continue set (what the real Unicode table returns
// for U+0080-U+00FF) is small enough to inline, so this needs no table import
// and stays constexpr.
[[nodiscard]] constexpr bool is_identifier_part_byte(std::uint8_t c) {
    if (c < 0x80) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '_' || c == '$';
    }
    // U+00AA FEMININE ORDINAL / U+00BA MASCULINE ORDINAL — ID_Start.
    // U+00B5 MICRO SIGN — ID_Start.  U+00B7 MIDDLE DOT — ID_Continue only.
    // U+00C0-U+00D6, U+00D8-U+00F6, U+00F8-U+00FF — letters, minus the two
    // mathematical operators U+00D7 (×) and U+00F7 (÷) that sit inside the run.
    return c == 0xAA || c == 0xB5 || c == 0xB7 || c == 0xBA
        || (c >= 0xC0 && c <= 0xFF && c != 0xD7 && c != 0xF7);
}

// A sink this mixin can look backwards into. `print_space_before_identifier`
// needs the last byte written and the byte count; `ByteSink` (quote.cppm:68)
// only promises `write_all`. StringSink satisfies this today.
template <typename W>
concept ReadableSink = requires(W& w) {
    { w.buffer() } -> std::convertible_to<const std::string&>;
};

// ─────────────────────────────────────────────────────────────────────────────
// StmtPrinter — the `S::*` arms of ref lib.rs:5289.
// ─────────────────────────────────────────────────────────────────────────────
template <typename Derived>
class StmtPrinter {
private:
    [[nodiscard]] Derived& self_() { return static_cast<Derived&>(*this); }
    [[nodiscard]] const Derived& self_() const { return static_cast<const Derived&>(*this); }

    // PROVISIONAL(1) — belongs in PrinterCore. See the header.
    const mbun::ast::Arena* arena_ { nullptr };

    // ref :1635 `prev_stmt_tag`, listed DEFERRED to shard 3 at
    // printer_core.cppm:277. Only `SEmpty` reads it (:5411) — consecutive empty
    // statements at indent 0 collapse to one `;`.
    //
    // Seeded to EmptyStmt, not Missing — ref the construction site at :7034
    // (`prev_stmt_tag: StmtTag::SEmpty`). It means a program whose FIRST
    // statement is `;` prints nothing for it, exactly as if an empty statement
    // preceded it. Observable: `;;;` prints "" rather than ";".
    mbun::ast::NodeKind prevStmtTag_ { mbun::ast::NodeKind::EmptyStmt };

public:
    // ── PROVISIONAL(1): arena access ─────────────────────────────────────────
    void bind_arena(const mbun::ast::Arena& arena) { arena_ = &arena; }

    [[nodiscard]] const mbun::ast::Arena& arena() const { return *arena_; }

    [[nodiscard]] const mbun::ast::Node& node(mbun::ast::NodeIndex i) const {
        return arena_->at(i);
    }

    [[nodiscard]] mbun::ast::NodeKind kind_of(mbun::ast::NodeIndex i) const {
        return i == mbun::ast::NONE ? mbun::ast::NodeKind::Missing : arena_->at(i).kind;
    }

    // ── PROVISIONAL(2): writer introspection ─────────────────────────────────

    // ref :7355 `pub written: i32` + :7367 `written: -1` + :7419
    // `self.written = self.written.wrapping_add(count)`.
    // ⚠️ It inits to -1 and accumulates byte counts, so it is `len - 1`, NOT
    // `len`. Every `>= 0` / `== prev_reg_exp_end` test downstream depends on
    // that offset — `stmt_start = writer.written()` (:6479) is an index, not a
    // length. Getting this off by one silently mis-parenthesises statements.
    [[nodiscard]] std::int32_t written() {
        static_assert(ReadableSink<std::remove_cvref_t<decltype(self_().writer())>>,
            "StmtPrinter needs a sink it can read back (see ReadableSink). "
            "print_space_before_identifier cannot work on a write-only sink.");
        return static_cast<std::int32_t>(self_().writer().buffer().size()) - 1;
    }

    // ref :7401 — `self.ctx.get_last_byte()`, 0 when nothing has been written.
    [[nodiscard]] std::uint8_t prev_char() {
        const std::string& buf { self_().writer().buffer() };
        return buf.empty() ? std::uint8_t { 0 } : static_cast<std::uint8_t>(buf.back());
    }

    // ref :2130. The `>= 0` (not `> 0`) is load-bearing and bun says so at
    // :2131-2133: with `written()` being `len - 1`, `>= 0` means "at least one
    // byte written". `> 0` would skip the space when exactly one byte precedes a
    // keyword, giving `xinstanceof y`.
    void print_space_before_identifier() {
        if (written() >= 0
            && (is_identifier_part_byte(prev_char()) || written() == self_().prevRegExpEnd)) {
            self_().print(' ');
        }
    }

    // ── PROVISIONAL(3): sourcemaps ───────────────────────────────────────────

    // ref :2450. No-op until the sourcemap port lands (printer_core.cppm:281).
    // Takes the node whose `start` bun would map, so filling this in later needs
    // no new call sites.
    void add_source_mapping(mbun::ast::NodeIndex) { }

    // ═════════════════════════════════════════════════════════════════════════
    // Blocks and bodies
    // ═════════════════════════════════════════════════════════════════════════

    // ref :2201. bun threads `loc` + `close_brace_loc` purely for sourcemaps
    // (:2202/:2213-2217); with `add_source_mapping` a no-op there is nothing to
    // thread, so the signature is the stmt list and the level.
    void print_block(std::span<const mbun::ast::NodeIndex> stmts, TopLevel tlmtlo) {
        self_().print('{');
        if (!stmts.empty()) {
            self_().print_newline();
            self_().indent();
            print_block_body(stmts, tlmtlo);
            self_().unindent();
            self_().print_indent();
        }
        self_().print('}');
        // ref :2219 — a `}` is its own terminator; a deferred `;` must not leak
        // past it when minifying.
        self_().needsSemicolon = false;
    }

    // ref :2194
    void print_block_body(std::span<const mbun::ast::NodeIndex> stmts, TopLevel tlmtlo) {
        for (const mbun::ast::NodeIndex s : stmts) {
            self_().print_semicolon_if_needed();
            print_stmt(s, tlmtlo);
        }
    }

    // ref :2224 — used by the CJS/ESM lowering to splice a prelude into an
    // existing block. Ported for completeness; no caller in mbun yet.
    void print_two_blocks_in_one(std::span<const mbun::ast::NodeIndex> stmts,
        std::span<const mbun::ast::NodeIndex> prepend) {
        self_().print('{');
        self_().print_newline();
        self_().indent();
        print_block_body(prepend, TopLevel::init(IsTopLevel::No));
        print_block_body(stmts, TopLevel::init(IsTopLevel::No));
        self_().unindent();
        self_().needsSemicolon = false;
        self_().print_indent();
        self_().print('}');
    }

    // ref :2173 — a block body prints inline after a space; anything else drops
    // to the next line one indent deeper.
    void print_body(mbun::ast::NodeIndex stmt, TopLevel tlmtlo) {
        if (kind_of(stmt) == mbun::ast::NodeKind::Block) {
            self_().print_space();
            print_block(arena_->list_of(node(stmt)), tlmtlo);
            self_().print_newline();
        } else {
            self_().print_newline();
            self_().indent();
            print_stmt(stmt, tlmtlo);
            self_().unindent();
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // Declarations
    // ═════════════════════════════════════════════════════════════════════════

    // ref :2242.
    //
    // DEFERRED: bun guards a large minify pass here behind
    // `FeatureFlags::SAME_TARGET_BECOMES_DESTRUCTURING` (:2258-2424) that
    // rewrites `var a = obj.foo, b = obj.bar` into `var {a, b} = obj`. It needs
    // `temporary_bindings` (printer_core.cppm:278, DEFERRED) and only fires
    // under minify-syntax, which mbun does not implement yet. The emit loop
    // below is bun's :2427-2447 — the path every non-minified print takes.
    void print_decls(std::string_view keyword, std::span<const mbun::ast::NodeIndex> decls,
        ExprFlagSet flags, TopLevelAndIsExport tlm) {
        self_().print(keyword);
        self_().print_space();

        // ref :2251-2256 — `var ;` is invalid syntax; bun asserts unreachable.
        if (decls.empty()) {
            return;
        }

        bool first { true };
        for (const mbun::ast::NodeIndex d : decls) {
            if (!first) {
                self_().print(',');
                self_().print_space();
            }
            first = false;

            const mbun::ast::Node& decl { node(d) };
            self_().print_binding(decl.a, tlm);
            if (decl.b != mbun::ast::NONE) {
                self_().print_whitespacer(ws<" = ">);
                self_().print_expr(decl.b, Level::Comma, flags);
            }
        }
    }

    // ref :6748.
    //
    // DEFERRED: bun's `MAY_HAVE_MODULE_INFO` branch (:6758-6777) records
    // top-level var/lexical kinds into `module_info` for the CJS analyzer, and
    // the `REWRITE_ESM_TO_CJS` tail (:6782+) emits `__export(module.exports, …)`
    // per declarator. Both need ports listed DEFERRED in printer_core.cppm
    // (`module_info`, `runtime_imports`). `tlmtlo` is threaded to the exact
    // point bun consumes it so filling that in is local.
    void print_decl_stmt(bool isExport, std::string_view keyword,
        std::span<const mbun::ast::NodeIndex> decls, TopLevel tlmtlo) {
        if constexpr (!Derived::flags().rewriteEsmToCjs) {
            if (isExport) {
                self_().print("export ");
            }
        }
        (void)tlmtlo;
        print_decls(keyword, decls, expr_flag::none(), TopLevelAndIsExport {});
        self_().print_semicolon_after_statement();
    }

    // ref :2196 (`S::Kind` → keyword). mbun's VarKind has no Using/AwaitUsing
    // arm; see the AST-GAP note on the VarDecl case in print_stmt.
    [[nodiscard]] static constexpr std::string_view keyword_for(mbun::ast::VarKind k) {
        switch (k) {
        case mbun::ast::VarKind::Var: return "var";
        case mbun::ast::VarKind::Let: return "let";
        case mbun::ast::VarKind::Const: return "const";
        }
        return "var";
    }

    // ref :6535 — the `for(...)` head. An init is a statement, but only three
    // shapes are legal there and bun panics on anything else.
    void print_for_loop_init(mbun::ast::NodeIndex init, ExprFlagSet extraFlags) {
        switch (kind_of(init)) {
        case mbun::ast::NodeKind::ExpressionStmt:
            self_().print_expr(node(init).a, Level::Lowest,
                ExprFlag::ForbidIn | ExprFlag::ExprResultIsUnused | extraFlags);
            break;
        case mbun::ast::NodeKind::VarDecl: {
            const mbun::ast::Node& n { node(init) };
            print_decls(keyword_for(static_cast<mbun::ast::VarKind>(n.aux)),
                arena_->list_of(n), ExprFlag::ForbidIn, TopLevelAndIsExport {});
            break;
        }
        // ref :6578 — `for(;;)`: an empty init prints nothing.
        case mbun::ast::NodeKind::EmptyStmt:
        case mbun::ast::NodeKind::Missing:
            break;
        default:
            // ref :6581 — bun panics here. mbun cannot: the printer is a library
            // and a malformed arena must not abort the process. Printing the
            // init as an expression statement is the closest well-formed output.
            self_().print_expr(init, Level::Lowest,
                ExprFlag::ForbidIn | ExprFlag::ExprResultIsUnused | extraFlags);
            break;
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // if / else
    // ═════════════════════════════════════════════════════════════════════════

    // ref :6669. `if (a) if (b) x(); else y();` — the `else` binds to the INNER
    // `if`, so an outer `if` whose consequent ends in a danglable `if` must brace
    // it. The walk descends through every construct whose body is a tail
    // position (:6674-6684) rather than testing the top node only.
    [[nodiscard]] bool wrap_to_avoid_ambiguous_else(mbun::ast::NodeIndex s) const {
        while (s != mbun::ast::NONE) {
            const mbun::ast::Node& n { node(s) };
            switch (n.kind) {
            case mbun::ast::NodeKind::IfStmt:
                // ref :6672-6677 — no `else` of its own ⇒ it would swallow ours.
                if (n.c == mbun::ast::NONE) {
                    return true;
                }
                s = n.c;
                break;
            // ref :6679-6681. AST-GAP: For/ForIn keep their body in `list[0]`,
            // not `b` (ast.cppm:89-90) — `a`/`b`/`c` are already spent on
            // init/test/update. Same node, different accessor.
            case mbun::ast::NodeKind::ForStmt:
            case mbun::ast::NodeKind::ForInStmt: {
                const std::span<const mbun::ast::NodeIndex> body { arena_->list_of(n) };
                if (body.empty()) {
                    return false;
                }
                s = body[0];
                break;
            }
            case mbun::ast::NodeKind::WhileStmt:  // ref :6682
            case mbun::ast::NodeKind::WithStmt:   // ref :6683
                s = n.b;
                break;
            case mbun::ast::NodeKind::LabeledStmt:  // ref :6684
                s = n.a;
                break;
            default:
                return false;
            }
        }
        return false;
    }

    // ref :6585. `else if` chains recurse straight back in here without passing
    // through `print_stmt` (:6660), which is why bun gives this its own stack
    // guard (:6588-6591) — see printer_core.cppm:283 (`stack_check`, DEFERRED).
    void print_if(mbun::ast::NodeIndex s, TopLevel tlmtlo) {
        const mbun::ast::Node& n { node(s) };
        const mbun::ast::NodeIndex yes { n.b };
        const mbun::ast::NodeIndex no { n.c };

        print_space_before_identifier();
        add_source_mapping(s);
        self_().print("if");
        self_().print_space();
        self_().print('(');
        self_().print_expr(n.a, Level::Lowest, expr_flag::none());
        self_().print(')');

        if (kind_of(yes) == mbun::ast::NodeKind::Block) {
            self_().print_space();
            print_block(arena_->list_of(node(yes)), tlmtlo);
            if (no != mbun::ast::NONE) {
                self_().print_space();
            } else {
                self_().print_newline();
            }
        } else if (wrap_to_avoid_ambiguous_else(yes)) {
            // ref :6617-6634 — brace the consequent so our `else` cannot be
            // captured by a dangling inner `if`.
            self_().print_space();
            self_().print('{');
            self_().print_newline();
            self_().indent();
            print_stmt(yes, tlmtlo);
            self_().unindent();
            self_().needsSemicolon = false;
            self_().print_indent();
            self_().print('}');
            if (no != mbun::ast::NONE) {
                self_().print_space();
            } else {
                self_().print_newline();
            }
        } else {
            // ref :6636-6644
            self_().print_newline();
            self_().indent();
            print_stmt(yes, tlmtlo);
            self_().unindent();
            if (no != mbun::ast::NONE) {
                self_().print_indent();
            }
        }

        if (no == mbun::ast::NONE) {
            return;
        }

        // ref :6648-6667
        self_().print_semicolon_if_needed();
        print_space_before_identifier();
        add_source_mapping(no);
        self_().print("else");

        if (kind_of(no) == mbun::ast::NodeKind::Block) {
            self_().print_space();
            print_block(arena_->list_of(node(no)), tlmtlo);
            self_().print_newline();
        } else if (kind_of(no) == mbun::ast::NodeKind::IfStmt) {
            // ref :6659 — `else if`, printed flat, NOT as a nested block.
            print_if(no, tlmtlo);
        } else {
            self_().print_newline();
            self_().indent();
            print_stmt(no, tlmtlo);
            self_().unindent();
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    // print_stmt — ref :5289
    // ═════════════════════════════════════════════════════════════════════════
    void print_stmt(mbun::ast::NodeIndex s, TopLevel tlmtlo) {
        if (s == mbun::ast::NONE) {
            return;
        }

        const mbun::ast::Node& n { node(s) };
        // ref :5294-5300 — bun snapshots the tag and assigns the new one at
        // every return point rather than holding a scopeguard across the match
        // (borrowck). C++ has no such constraint: one scope-exit assignment.
        const mbun::ast::NodeKind prevStmtTag { prevStmtTag_ };
        struct TagOnExit {
            mbun::ast::NodeKind* slot;
            mbun::ast::NodeKind value;
            ~TagOnExit() { *slot = value; }
        } tagOnExit { &prevStmtTag_, n.kind };

        switch (n.kind) {
        // ── the driver ───────────────────────────────────────────────────────
        // bun has no `S::Program`: its driver loops the top-level stmts itself
        // at :8082-8087. mbun's parser produces a Program node, so that loop
        // lives here. Top-level, so no `sub_var()`.
        //
        // ⚠️ This is NOT `print_block_body`, and the difference is not cosmetic:
        // the driver calls `print_semicolon_if_needed()` AFTER each statement
        // (:8086), while print_block_body calls it BEFORE (:2196). Only the
        // after-form flushes the LAST statement's deferred semicolon, which is
        // why bun minifies `while (a) b();` to `while(a)b();` (with the `;`) but
        // `{ a(); b(); }` to `{a();b()}` (without) — a block's closing `}`
        // clears the flag, top level has no `}` to clear it.
        case mbun::ast::NodeKind::Program:
            for (const mbun::ast::NodeIndex st : arena_->list_of(n)) {
                print_stmt(st, tlmtlo);
                self_().print_semicolon_if_needed();
            }
            break;

        // ── ref :6475 ────────────────────────────────────────────────────────
        case mbun::ast::NodeKind::ExpressionStmt:
            // ref :6476 — NOT `print_indent()`. An expression statement at
            // indent 0 must not be indented even when the option says so.
            if (!self_().options.minifyWhitespace && self_().options.indent.count > 0) {
                self_().print_indent();
            }
            // ref :6479 — where `(function(){})()` decides it needs parens.
            self_().stmtStart = written();
            self_().print_expr(n.a, Level::Lowest, expr_flag::expr_result_is_unused());
            self_().print_semicolon_after_statement();
            break;

        // ── ref :6406 ────────────────────────────────────────────────────────
        case mbun::ast::NodeKind::Block:
            self_().print_indent();
            print_block(arena_->list_of(n), tlmtlo.sub_var());
            self_().print_newline();
            break;

        // ── ref :5411 ────────────────────────────────────────────────────────
        case mbun::ast::NodeKind::EmptyStmt:
            // ref :5412-5415 — `;;;` at top level collapses to `;`. Only at
            // indent 0: inside a block the extra `;` are kept.
            if (prevStmtTag == mbun::ast::NodeKind::EmptyStmt
                && self_().options.indent.count == 0) {
                break;
            }
            self_().print_indent();
            add_source_mapping(s);
            self_().print(';');
            self_().print_newline();
            break;

        // ── ref :5820 ────────────────────────────────────────────────────────
        case mbun::ast::NodeKind::VarDecl: {
            self_().print_indent();
            print_space_before_identifier();
            add_source_mapping(s);
            // AST-GAP: mbun's VarKind (ast.cppm:196) has Var/Let/Const only.
            // bun also carries KUsing / KAwaitUsing (:5836-5844) for the
            // explicit-resource-management proposal, and mbun's parser DOES
            // parse `using` heads (js_parser.cppm:2904 parse_for_using_head_) —
            // so a `using x = …` currently reaches here as some other kind and
            // loses its keyword. Printing it needs a VarKind arm, which is
            // ast.cppm's to add, not this shard's.
            print_decl_stmt(/*isExport=*/false, keyword_for(static_cast<mbun::ast::VarKind>(n.aux)),
                arena_->list_of(n), tlmtlo);
            break;
        }

        // ── ref :5845 ────────────────────────────────────────────────────────
        case mbun::ast::NodeKind::IfStmt:
            self_().print_indent();
            print_if(s, tlmtlo.sub_var());
            break;

        // ── ref :5919 ────────────────────────────────────────────────────────
        case mbun::ast::NodeKind::WhileStmt:
            self_().print_indent();
            print_space_before_identifier();
            add_source_mapping(s);
            self_().print("while");
            self_().print_space();
            self_().print('(');
            self_().print_expr(n.a, Level::Lowest, expr_flag::none());
            self_().print(')');
            print_body(n.b, tlmtlo.sub_var());
            break;

        // ── ref :5849 ────────────────────────────────────────────────────────
        case mbun::ast::NodeKind::DoWhileStmt: {
            self_().print_indent();
            print_space_before_identifier();
            add_source_mapping(s);
            self_().print("do");
            const TopLevel subVar { tlmtlo.sub_var() };
            if (kind_of(n.a) == mbun::ast::NodeKind::Block) {
                self_().print_space();
                print_block(arena_->list_of(node(n.a)), subVar);
                self_().print_space();
            } else {
                // ref :5864-5871. Note the `print_semicolon_if_needed` BEFORE
                // unindent: `do x(); while(y)` must not become `do x() while(y)`
                // when minifying — unlike print_body, the deferred semicolon
                // cannot be dropped here.
                self_().print_newline();
                self_().indent();
                print_stmt(n.a, subVar);
                self_().print_semicolon_if_needed();
                self_().unindent();
                self_().print_indent();
            }
            self_().print("while");
            self_().print_space();
            self_().print('(');
            self_().print_expr(n.b, Level::Lowest, expr_flag::none());
            self_().print(')');
            self_().print_semicolon_after_statement();
            break;
        }

        // ── ref :5983 ────────────────────────────────────────────────────────
        case mbun::ast::NodeKind::ForStmt: {
            self_().print_indent();
            print_space_before_identifier();
            add_source_mapping(s);
            self_().print("for");
            self_().print_space();
            self_().print('(');
            if (n.a != mbun::ast::NONE) {
                print_for_loop_init(n.a, expr_flag::none());
            }
            self_().print(';');
            if (n.b != mbun::ast::NONE) {
                self_().print_expr(n.b, Level::Lowest, expr_flag::none());
            }
            self_().print(';');
            self_().print_space();
            if (n.c != mbun::ast::NONE) {
                self_().print_expr(n.c, Level::Lowest, expr_flag::none());
            }
            self_().print(')');
            // AST-GAP: body lives in list[0] (ast.cppm:89) because a/b/c are
            // spent on init/test/update. bun reads `s.body` (:6006).
            print_body(body_of_for_(n), tlmtlo.sub_var());
            break;
        }

        // ── ref :5883 (SForIn) / :5899 (SForOf) ──────────────────────────────
        case mbun::ast::NodeKind::ForInStmt: {
            // bun splits these into two node types; mbun tags one node with
            // `flags` bit0 = `of` (ast.cppm:90). The two arms are identical
            // apart from the keyword and the RHS level, so they share a body.
            const bool isOf { (n.flags & 1u) != 0u };
            self_().print_indent();
            print_space_before_identifier();
            add_source_mapping(s);
            self_().print("for");
            // AST-GAP: `for await (… of …)` is unrepresentable — there is no
            // await bit (ast.cppm:90 defines bit0 = of and nothing else), and
            // js_parser.cppm:2918-2941 never sets one. bun prints `" await"`
            // here from `s.is_await` (:5904). A `for await` that reaches this
            // printer silently loses the `await`. Needs an AST flag bit first.
            self_().print_space();
            self_().print('(');
            if (isOf) {
                // ref :5906 — lets print_expr disambiguate a leading `of`/`let`.
                self_().forOfInitStart = written();
            }
            print_for_loop_init(n.a, isOf ? expr_flag::is_followed_by_of() : expr_flag::none());
            self_().print_space();
            print_space_before_identifier();
            self_().print(isOf ? "of" : "in");
            self_().print_space();
            // ref :5915 vs :5897 — `of` binds its RHS at Comma (a bare comma
            // would end the head), `in` at Lowest.
            self_().print_expr(n.b, isOf ? Level::Comma : Level::Lowest, expr_flag::none());
            self_().print(')');
            print_body(body_of_for_(n), tlmtlo.sub_var());
            break;
        }

        // ── ref :5930 ────────────────────────────────────────────────────────
        case mbun::ast::NodeKind::WithStmt:
            self_().print_indent();
            print_space_before_identifier();
            add_source_mapping(s);
            self_().print("with");
            self_().print_space();
            self_().print('(');
            self_().print_expr(n.a, Level::Lowest, expr_flag::none());
            self_().print(')');
            print_body(n.b, tlmtlo.sub_var());
            break;

        // ── ref :5941 ────────────────────────────────────────────────────────
        case mbun::ast::NodeKind::LabeledStmt:
            // ref :5942 — same indent-0 exception as SExpr, not print_indent().
            if (!self_().options.minifyWhitespace && self_().options.indent.count > 0) {
                self_().print_indent();
            }
            print_space_before_identifier();
            add_source_mapping(s);
            // ref :5946 `print_symbol(s.name.ref_)` — a label is a symbol, so it
            // goes through the renamer (shard 4). AST-GAP: mbun stores the label
            // as `text` (ast.cppm:101), not as a symbol ref, so it cannot be
            // renamed. Printing the raw name is correct until a renamer exists.
            self_().print_identifier(n.text);
            self_().print(':');
            print_body(n.a, tlmtlo.sub_var());
            break;

        // ── ref :5951 ────────────────────────────────────────────────────────
        case mbun::ast::NodeKind::TryStmt: {
            self_().print_indent();
            print_space_before_identifier();
            add_source_mapping(s);
            self_().print("try");
            self_().print_space();
            const TopLevel subVarTry { tlmtlo.sub_var() };
            print_block(arena_->list_of(n), subVarTry);

            if (n.b != mbun::ast::NONE) {
                // ref :5960-5972
                const mbun::ast::Node& c { node(n.b) };
                self_().print_space();
                add_source_mapping(n.b);
                self_().print("catch");
                if (c.a != mbun::ast::NONE) {
                    // ref :5964 — optional catch binding: `catch {}` is legal.
                    self_().print_space();
                    self_().print('(');
                    self_().print_binding(c.a, TopLevelAndIsExport {});
                    self_().print(')');
                }
                self_().print_space();
                print_block(arena_->list_of(c), subVarTry);
            }

            if (n.c != mbun::ast::NONE) {
                // ref :5974-5979
                self_().print_space();
                self_().print("finally");
                self_().print_space();
                print_block(arena_->list_of(node(n.c)), subVarTry);
            }
            self_().print_newline();
            break;
        }

        // ── ref :6011 ────────────────────────────────────────────────────────
        case mbun::ast::NodeKind::SwitchStmt: {
            self_().print_indent();
            print_space_before_identifier();
            add_source_mapping(s);
            self_().print("switch");
            self_().print_space();
            self_().print('(');
            self_().print_expr(n.a, Level::Lowest, expr_flag::none());
            self_().print(')');
            self_().print_space();
            self_().print('{');
            self_().print_newline();
            self_().indent();

            const TopLevel subVarCase { tlmtlo.sub_var() };
            for (const mbun::ast::NodeIndex ci : arena_->list_of(n)) {
                const mbun::ast::Node& c { node(ci) };
                self_().print_semicolon_if_needed();
                self_().print_indent();

                if (c.a != mbun::ast::NONE) {
                    self_().print("case");
                    self_().print_space();
                    // ref :6033 — LogicalAnd, not Lowest: `case a, b:` would
                    // reparse as a sequence, so a comma expression needs parens.
                    self_().print_expr(c.a, Level::LogicalAnd, expr_flag::none());
                } else {
                    self_().print("default");
                }
                self_().print(':');

                const std::span<const mbun::ast::NodeIndex> body { arena_->list_of(c) };
                // ref :6041-6055 — a case whose whole body is one block prints
                // it inline (`case 1: { … }`) instead of indenting a lone block.
                if (body.size() == 1
                    && kind_of(body[0]) == mbun::ast::NodeKind::Block) {
                    self_().print_space();
                    print_block(arena_->list_of(node(body[0])), subVarCase);
                    self_().print_newline();
                    continue;
                }

                self_().print_newline();
                self_().indent();
                for (const mbun::ast::NodeIndex st : body) {
                    self_().print_semicolon_if_needed();
                    print_stmt(st, subVarCase);
                }
                self_().unindent();
            }

            self_().unindent();
            self_().print_indent();
            self_().print('}');
            self_().print_newline();
            self_().needsSemicolon = false;
            break;
        }

        // ── ref :6433 / :6444 ────────────────────────────────────────────────
        case mbun::ast::NodeKind::BreakStmt:
        case mbun::ast::NodeKind::ContinueStmt:
            self_().print_indent();
            print_space_before_identifier();
            add_source_mapping(s);
            self_().print(n.kind == mbun::ast::NodeKind::BreakStmt ? "break" : "continue");
            if (!n.text.empty()) {
                // ref :6438 — a plain space, never a newline: ASI would turn
                // `break\nfoo` into `break; foo;`.
                self_().print(' ');
                self_().print_identifier(n.text);
            }
            self_().print_semicolon_after_statement();
            break;

        // ── ref :6455 ────────────────────────────────────────────────────────
        case mbun::ast::NodeKind::ReturnStmt:
            self_().print_indent();
            print_space_before_identifier();
            add_source_mapping(s);
            self_().print("return");
            if (n.a != mbun::ast::NONE) {
                // ref :6461 — print_space(), so minified output is `return x`
                // (the space survives). Dropping it would give `returnx`.
                self_().print_space();
                self_().print_expr(n.a, Level::Lowest, expr_flag::none());
            }
            self_().print_semicolon_after_statement();
            break;

        // ── ref :6466 ────────────────────────────────────────────────────────
        case mbun::ast::NodeKind::ThrowStmt:
            self_().print_indent();
            print_space_before_identifier();
            add_source_mapping(s);
            self_().print("throw");
            self_().print_space();
            self_().print_expr(n.a, Level::Lowest, expr_flag::none());
            self_().print_semicolon_after_statement();
            break;

        // ── ref :5307 / :5361 ────────────────────────────────────────────────
        // Function and class DECLARATIONS. The shape after the name — params,
        // body, heritage, members — is `print_func` (:2527) / `print_class`
        // (:2543), which now live in func.cppm (they landed nowhere until then,
        // so these two calls made `Printer::print_stmt` un-INSTANTIABLE; see that
        // file's header). Routed through self_() per the CRTP contract.
        case mbun::ast::NodeKind::FunctionDecl:
            self_().print_indent();
            print_space_before_identifier();
            add_source_mapping(s);
            self_().print_function_decl(s, tlmtlo);
            break;

        case mbun::ast::NodeKind::ClassDecl:
            self_().print_indent();
            print_space_before_identifier();
            add_source_mapping(s);
            self_().print_class_decl(s, tlmtlo);
            break;

        // ── ref :6748 (`export const`) / :5318 (`export function`) / :5372
        //    (`export class`) ────────────────────────────────────────────────
        // `export <declaration>` — the ONLY module form this arena can print.
        //
        // bun has no export-wrapper node at all: `export function f(){}` is an
        // `S::Function` carrying `is_export` (:5318), `export const x = 1` an
        // `S::Local` carrying it (:6748). mbun's parser instead builds an
        // ExportDecl WRAPPER and hangs the declaration on `a`
        // (js_parser.cppm:1560-1562). That single field is the only payload any
        // import/export node in this arena carries — see the MODULE-SYNTAX
        // AST-GAP note in this file's header for why the other forms cannot be
        // printed at all and fall to `default:`.
        //
        // `export ` is printed HERE rather than read off `fnflags::IsExport`
        // (which func.cppm:247/:285 already honours) because mbun's parser never
        // sets that flag — func.cppm:37-40 says so, and the arena is const to
        // this shard, so it cannot be set on the way past either. Order matches
        // bun exactly: indent, space-before-identifier, source mapping, THEN
        // `export `.
        // ── ref :2000 `print_import_statement` ────────────────────────────────
        // The AST-GAP that used to make this unprintable is closed; the arm lives
        // in module_syntax.cppm.
        case mbun::ast::NodeKind::ImportDecl:
            self_().print_import_decl(s);
            break;

        case mbun::ast::NodeKind::ExportDecl: {
            // The FORM is the discriminant, not the kind — ast.cppm ExportForm.
            // Only `Decl` (and the erased `None`) is handled below; every other
            // form is module_syntax.cppm's.
            switch (static_cast<mbun::ast::ExportForm>(n.aux)) {
            case mbun::ast::ExportForm::Star:
                self_().print_export_star(s);
                return;
            case mbun::ast::ExportForm::Clause:
                // `export {a}` with no `from` re-exports LOCAL bindings, and bun
                // prints only the names actually declared as module-scope VALUES —
                // `export {a}` with no `a` in scope prints `export {};`.
                //
                // That filter is NOT applied here, and deliberately so: it is a
                // whole-module question (`export {a}; let a=1` keeps `a`, so the
                // answer depends on statements this arm has not reached), and bun
                // answers it the same way — in the visit pass at
                // visit_stmt.rs:168 `s_export_clause`, leaving its printer to print
                // whatever specifiers survived (lib.rs:5583). mbun mirrors that
                // split: js_parser/module_scope.cppm's `filter_export_clauses` runs
                // over the arena before printing and compacts the dead specifiers
                // out of the list, so by the time this arm runs the node holds
                // exactly the survivors and printing it verbatim is correct.
                //
                // An emptied clause still prints `export {};` — that IS bun, and it
                // is load-bearing: the statement is what marks the file an ES
                // module. The form that vanishes entirely (`export {type a}`) is
                // tagged ExportForm::None by the parser and never reaches here.
                self_().print_export_clause(s, /*hasFrom=*/false);
                return;
            case mbun::ast::ExportForm::ClauseFrom:
                // A re-export IS immune to the above: the names are the other
                // module's, never local references, so no scope analysis is needed.
                self_().print_export_clause(s, /*hasFrom=*/true);
                return;
            case mbun::ast::ExportForm::DefaultExpr:
                self_().print_export_default(s, /*isDecl=*/false, tlmtlo);
                return;
            case mbun::ast::ExportForm::DefaultDecl:
                self_().print_export_default(s, /*isDecl=*/true, tlmtlo);
                return;
            case mbun::ast::ExportForm::Assign:
                self_().print_export_equals(s);
                return;
            case mbun::ast::ExportForm::None:
                // A type-only export — bun erases it whole. Printing nothing is
                // that same answer.
                return;
            case mbun::ast::ExportForm::Decl:
                break;  // ↓
            }
            const mbun::ast::NodeIndex inner { n.a };
            switch (kind_of(inner)) {
            case mbun::ast::NodeKind::VarDecl: {
                // ref :6748 — `print_decl_stmt` already takes `is_export` and
                // prints the keyword itself; it has been dead since it landed
                // because nothing set the flag. This is its first caller.
                const mbun::ast::Node& d { node(inner) };
                self_().print_indent();
                print_space_before_identifier();
                add_source_mapping(s);
                print_decl_stmt(/*isExport=*/true,
                    keyword_for(static_cast<mbun::ast::VarKind>(d.aux)),
                    arena_->list_of(d), tlmtlo);
                break;
            }
            case mbun::ast::NodeKind::FunctionDecl:
            case mbun::ast::NodeKind::ClassDecl:
                self_().print_indent();
                print_space_before_identifier();
                add_source_mapping(s);
                // ref :5318 / :5372 — both arms gate the keyword on the CJS
                // rewrite, which re-homes the binding onto `exports` instead.
                if constexpr (!Derived::flags().rewriteEsmToCjs) {
                    self_().print("export ");
                }
                if (kind_of(inner) == mbun::ast::NodeKind::FunctionDecl) {
                    self_().print_function_decl(inner, tlmtlo);
                } else {
                    self_().print_class_decl(inner, tlmtlo);
                }
                break;
            // A TS-only declaration — `export interface` / `export type` /
            // `export declare` — which bun ERASES whole (verified against real
            // bun 1.4.0: all three transform to ""). mbun's parser has already
            // recorded that erasure as an edit (js_parser.cppm:1510-1512), and
            // printing nothing here is the same answer by a different route.
            case mbun::ast::NodeKind::TypeAliasDecl:
            case mbun::ast::NodeKind::InterfaceDecl:
                break;

            // ⚠️ `export enum` / `export namespace` used to land on the same
            // `default:` as the erased forms above — which is how a REAL gap hid
            // inside a CORRECT one. The old comment even said so ("unported
            // exactly like the bare EnumDecl/NamespaceDecl they wrap") while
            // producing output indistinguishable from a deliberate erasure: no
            // bytes, no complaint. They are not erased by bun, they are lowered
            // (visit_stmt.rs:89/:120) — see the EnumDecl/NamespaceDecl arm in
            // the outer switch. `export enum E{A,B}` is
            //     export var E;
            //     ((E) => { E[E["A"] = 0] = "A"; E[E["B"] = 1] = "B"; })(E ||= {});
            // on real bun 1.4.0, not "".
            case mbun::ast::NodeKind::EnumDecl:
            case mbun::ast::NodeKind::NamespaceDecl:
                record_unsupported_(node(inner));
                break;

            default:
                // Any other kind under `export <decl>` is an arena invariant
                // break: the parser only ever hangs a declaration here.
                record_unsupported_(n);
                break;
            }
            break;
        }

        // ═════════════════════════════════════════════════════════════════════
        // TS type-only declarations — bun ERASES these entirely.
        // ═════════════════════════════════════════════════════════════════════
        //
        // Printing nothing is not a gap here, it is the CORRECT output, so these
        // must NOT reach the sink below. Verified against real bun 1.4.0
        // (Bun.Transpiler.transformSync, loader "ts"): each transforms to the
        // empty string.
        //
        //     type X = number          =>  ""
        //     interface I{a:string}    =>  ""
        //     declare const x: number  =>  ""
        //     declare enum E{A}        =>  ""
        //     declare namespace N{…}   =>  ""
        //
        // TypeScriptStmt is every `declare …` (ast.cppm — image of bun
        // `S::TypeScript`, s.rs:72). `declare enum` / `declare namespace` erase
        // through THIS arm rather than the EnumDecl/NamespaceDecl one below:
        // `declare` means "no runtime emit", so there is no closure to generate
        // and nothing is genuinely the answer. bun reaches the same place by
        // dropping S::TypeScript in the visit pass (visit/visit_stmt.rs:76-79) —
        // which is exactly why its printer has no arm for it.
        case mbun::ast::NodeKind::TypeAliasDecl:
        case mbun::ast::NodeKind::InterfaceDecl:
        case mbun::ast::NodeKind::TypeScriptStmt:
            break;

        // ═════════════════════════════════════════════════════════════════════
        // ⚠️ TS enum / namespace — NOT a printer gap. There is nothing to port.
        // ═════════════════════════════════════════════════════════════════════
        //
        // These are recorded, not printed, and the reason is structural rather
        // than "nobody got to it yet". bun's printer has NO `SEnum`/`SNamespace`
        // arm — grep lib.rs and there is not one — because by the time the
        // printer runs, neither kind exists any more. They are LOWERED in the
        // parser's visit pass into ordinary JS:
        //
        //   visit/visit_stmt.rs:89   `StmtData::SEnum(sr)      => …s_enum(…)`
        //   visit/visit_stmt.rs:120  `StmtData::SNamespace(sr) => …s_namespace(…)`
        //   visit/visit_stmt.rs:2137 `fn s_enum`       (209 lines)
        //   visit/visit_stmt.rs:2346 `fn s_namespace`  (49 lines)
        //   p.rs:6415 `generate_closure_for_type_script_namespace_or_enum`
        //                                              (~180 lines)
        //
        // which emit the `var E; ((E) => { … })(E ||= {})` closure as plain
        // S::Local + S::Expr statements. The printer then just prints those.
        //
        // So the port is a VISIT PASS, which mbun does not have — the parser
        // hands its arena straight to the printer, and that arena is const to
        // this shard. It also needs infrastructure that does not exist anywhere
        // in mbun: a symbol table with link-following for `namespace` merging
        // (p.rs:6427-6432), `emitted_namespace_vars` so two blocks of the same
        // namespace emit `var N` ONCE (p.rs:6443), `enclosing_namespace_arg_ref`
        // for the nested `B = A.B ||= {}` form (p.rs:6480), and — for
        // `enum E{A}; const v=E.A` => `const v = 0 /* A */` — the `ts_enums` map
        // plus constant folding (lib.rs:6697 `try_to_get_imported_enum_value`,
        // :6721 `print_inlined_enum`). Full spec + oracle vectors handed back to
        // the coordinator; see this file's header.
        //
        // Recording (rather than dropping) is what makes that gap COUNTABLE:
        // `enum E{} export const v=E.A` now fails loudly instead of printing
        // `export const v = E.A` against an `E` that was never declared.
        case mbun::ast::NodeKind::EnumDecl:
        case mbun::ast::NodeKind::NamespaceDecl:
            record_unsupported_(n);
            break;

        // ═════════════════════════════════════════════════════════════════════
        // Not statements — reaching print_stmt with one of these is an arena
        // invariant break, i.e. a parser bug, not a missing port.
        // ═════════════════════════════════════════════════════════════════════
        //
        // bun debug-panics on exactly this (:6484 `Output::panic`), and it is
        // right to: there is no correct output. mbun cannot abort — the printer
        // is a library — so it records instead, which is the same information
        // delivered without taking the process down.
        //
        // The error-recovery placeholder (ast.cppm:27) — and, because it is
        // enumerator 0, what a default-constructed Node holds (ast.cppm:531).
        // At statement position it means the parser handed over a zeroed node.
        case mbun::ast::NodeKind::Missing:
        // Expressions:
        case mbun::ast::NodeKind::Identifier:
        case mbun::ast::NodeKind::PrivateName:
        case mbun::ast::NodeKind::NumberLiteral:
        case mbun::ast::NodeKind::StringLiteral:
        case mbun::ast::NodeKind::BigIntLiteral:
        case mbun::ast::NodeKind::BooleanLiteral:
        case mbun::ast::NodeKind::NullLiteral:
        case mbun::ast::NodeKind::RegExpLiteral:
        case mbun::ast::NodeKind::TemplateLiteral:
        case mbun::ast::NodeKind::TaggedTemplate:
        case mbun::ast::NodeKind::ThisExpr:
        case mbun::ast::NodeKind::SuperExpr:
        case mbun::ast::NodeKind::ImportMeta:
        case mbun::ast::NodeKind::ImportCall:
        case mbun::ast::NodeKind::ArrayLiteral:
        case mbun::ast::NodeKind::ObjectLiteral:
        case mbun::ast::NodeKind::SpreadElement:
        case mbun::ast::NodeKind::Unary:
        case mbun::ast::NodeKind::Yield:
        case mbun::ast::NodeKind::Update:
        case mbun::ast::NodeKind::Binary:
        case mbun::ast::NodeKind::Logical:
        case mbun::ast::NodeKind::Assignment:
        case mbun::ast::NodeKind::Conditional:
        case mbun::ast::NodeKind::Sequence:
        case mbun::ast::NodeKind::Call:
        case mbun::ast::NodeKind::New:
        case mbun::ast::NodeKind::Member:
        case mbun::ast::NodeKind::PrivateMember:
        case mbun::ast::NodeKind::Index:
        case mbun::ast::NodeKind::Paren:
        case mbun::ast::NodeKind::Arrow:
        case mbun::ast::NodeKind::FunctionExpr:
        case mbun::ast::NodeKind::ClassExpr:
        // Sub-nodes — owned and printed by their parent's arm:
        case mbun::ast::NodeKind::Property:
        case mbun::ast::NodeKind::BindingIdentifier:
        case mbun::ast::NodeKind::BindingArray:
        case mbun::ast::NodeKind::BindingObject:
        case mbun::ast::NodeKind::BindingElement:
        case mbun::ast::NodeKind::BindingProperty:
        case mbun::ast::NodeKind::BindingMissing:
        case mbun::ast::NodeKind::Arg:
        case mbun::ast::NodeKind::VarDeclarator:
        case mbun::ast::NodeKind::SwitchCase:
        case mbun::ast::NodeKind::CatchClause:
        case mbun::ast::NodeKind::FinallyClause:
        case mbun::ast::NodeKind::EnumMember:
        case mbun::ast::NodeKind::TypeParam:
        case mbun::ast::NodeKind::ClauseItem:
            record_unsupported_(n);
            break;
        }
        // ⚠️ NO `default:` — deliberately, and it is the point of this switch.
        //
        // With every enumerator listed, adding a kind to ast.cppm makes this
        // switch non-exhaustive and the compiler says so (-Wswitch, on by
        // default in both toolchains). A `default:` would swallow the new kind
        // exactly the way it swallowed EnumDecl/NamespaceDecl for this printer's
        // whole life — silently, and reported as success. This is the same move
        // b9c11ca6 made on the expr side when it deleted the `requires` probe:
        // an unported kind should be a COMPILE error, and where it cannot be
        // (the kinds above are reachable from a valid arena), a loud runtime one.
    }

private:
    // The single call site shape for the failure channel (unsupported.cppm).
    // Takes the Node so the offset is always the one the caller already has —
    // an unported statement is reported at its own source position, not at
    // whatever the walk happened to reach last.
    void record_unsupported_(const mbun::ast::Node& n) {
        self_().record_unsupported(n.kind, n.start);
    }

    // AST-GAP helper: For/ForIn store the body in `list[0]` (ast.cppm:89-90,
    // js_parser.cppm:2941/2973) because `a`/`b`/`c` are spent. bun reads
    // `s.body` directly. One accessor so the hack is described once, and so the
    // day the AST grows a real body slot there is exactly one line to change.
    [[nodiscard]] mbun::ast::NodeIndex body_of_for_(const mbun::ast::Node& n) const {
        const std::span<const mbun::ast::NodeIndex> body { arena_->list_of(n) };
        return body.empty() ? mbun::ast::NONE : body[0];
    }
};

}  // namespace mbun::js_printer
