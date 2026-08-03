// src/js_printer/printer_core.cppm — module mbun.js_printer.printer_core
//
// CAP-BUILD-PRINTER shard 1/4 — the Printer skeleton every other shard hangs off.
//
// Blueprint (.mbun/bun-ref/src/js_printer/lib.rs):
//   :1607  `struct Printer<'a, W, ASCII_ONLY, REWRITE_ESM_TO_CJS, IS_BUN_PLATFORM,
//                          IS_JSON, GENERATE_SOURCE_MAP>`
//   :1908  `print_buffer`     :1913  `print`
//   :1918  `unindent`         :1922  `indent`        :1926  `print_indent`
//   :1957  `print_space`      :1963  `print_newline`
//   :1969  `print_semicolon_after_statement`  :1976  `print_semicolon_if_needed`
//   :1985  `print_equals`
//   :2604  `print_whitespacer`
//   :2638  `print_string_characters_utf8`   :2650  `print_string_characters_utf16`
//
// ═════════════════════════════════════════════════════════════════════════════
// THE SHARD CONTRACT — how shards 2/3/4 attach (read this before writing code)
// ═════════════════════════════════════════════════════════════════════════════
//
// bun's `Printer` is one 6000-line `impl` block. Ported literally that is a
// single C++ class, which means one file every shard has to edit — the exact
// merge-conflict hazard AGENTS.md 规则 10 exists to prevent, and the reason the
// 2000-line cap is a hard constraint. Module partitions do not help: a partition
// can hold out-of-line *definitions*, but the class *declaration* still has to
// list every method, so one file would still own all four shards' signatures.
//
// So the split is by CRTP mixin. Each shard owns one module and one class
// template, all of them mixed into `Printer` at the bottom of the chain:
//
//     // modules/js/src/js_printer/expr.cppm  — shard 2
//     export module mbun.js_printer.expr;
//     import mbun.js_printer.printer_core;
//     export namespace mbun::js_printer {
//     template <typename D>
//     class ExprPrinter {
//     private:
//         D& self_() { return static_cast<D&>(*this); }
//     public:
//         void print_expr(mbun::ast::NodeIndex e, Level level, ExprFlagSet flags) {
//             self_().print("(");             // ← PrinterCore method
//             self_().print_stmt(body, ...);  // ← shard 3's method, resolves fine
//         }
//     };
//     }  // namespace mbun::js_printer
//
// and then in printer.cppm (mine — I add the base as each shard lands):
//
//     class Printer : public PrinterCore<Printer, F, W>,
//                     public ExprPrinter<Printer>,     // shard 2
//                     public StmtPrinter<Printer>,     // shard 3
//                     public BindingPrinter<Printer> { ... };
//
// Why this and not the obvious alternatives:
//   * Mutual recursion works. print_stmt→print_expr→print_stmt crosses module
//     boundaries with no seam, because every cross-shard call goes through
//     `self_()` whose type is only resolved when `Printer` is instantiated. A
//     shard can call a method of a shard that does not exist yet and it compiles
//     the moment that shard lands.
//   * Zero cost. Static dispatch, no vtable, empty bases. This is what bun's one
//     big impl block gets for free and what a `PrinterCore&`-holding-classes
//     design would have thrown away.
//   * No shared file. Nobody edits anybody else's module to add a method.
//
// Rules for shards 2/3/4:
//   1. One module per shard, `export module mbun.js_printer.<part>;`, file at
//      `modules/js/src/js_printer/<part>.cppm`. Split further if you approach
//      2000 lines (`expr_binary.cppm`, …) — the mixin composes as many bases as
//      you like.
//   2. ⚠️ mcpp flattens object names to the file's BASENAME: `src/js_printer/
//      options.cppm` builds to `obj/options.m.o`. A basename collision with
//      another file anywhere under `modules/js/src/` silently clobbers. Taken so
//      far: ast, js_lexer, js_parser, js_printer, neutral_literal_seam,
//      encoding, quote, whitespacer, flags, options, printer_core, printer.
//   3. Reach PrinterCore state through `self_()`, never through a member — your
//      mixin has no state of its own unless it genuinely owns some.
//   4. `F` (the PrinterFlags NTTP) carries bun's five const generics. Read them
//      as `F.asciiOnly` etc. — they are compile-time, so `if constexpr` them.
//   5. New `.cppm` files need `mcpp build --no-cache` (mcpp does not rescan the
//      source list). `Finished in 0.0Xs` means it did not compile — do not trust
//      exit 0.
// ═════════════════════════════════════════════════════════════════════════════
export module mbun.js_printer.printer_core;

import std;
import mbun.js_printer.encoding;
import mbun.js_printer.quote;
import mbun.js_printer.whitespacer;
import mbun.js_printer.options;
import mbun.js_printer.flags;

export namespace mbun::js_printer {

// ─────────────────────────────────────────────────────────────────────────────
// PrinterFlags — ref lib.rs:1607-1615
//
// bun spells these as five separate `const` generic parameters. One structural
// NTTP aggregate is the same thing with one template argument instead of five,
// and it means a new print-time compile-time flag does not re-thread every
// signature in four shards' worth of files.
// ─────────────────────────────────────────────────────────────────────────────
struct PrinterFlags {
    bool asciiOnly { false };          // ref :1610 — escape every non-ASCII on the way out
    bool rewriteEsmToCjs { false };    // ref :1611
    bool isBunPlatform { false };      // ref :1612
    bool isJson { false };             // ref :1613
    bool generateSourceMap { false };  // ref :1614
};

// ref lib.rs:784 — `const ASCII_ONLY_ALWAYS_ON_UNLESS_MINIFYING: bool = true`
// "For support JavaScriptCore". Consumed by `can_print_identifier_utf16`
// (:3131), NOT by the string escaper — which is why real bun prints `const s =
// "héllo"` (raw) but `const héllo = 1` unchanged only because the *identifier*
// path has its own latin1 check. VERIFIED on bun 1.3.14: default target keeps
// `héllo` as an identifier and as string content; only target:"bun" escapes
// either (`const h\u{e9}llo` / `"h\xE9llo"`).
inline constexpr bool ASCII_ONLY_ALWAYS_ON_UNLESS_MINIFYING { true };

// ─────────────────────────────────────────────────────────────────────────────
// PrinterCore — the AST-independent half of bun's `Printer`.
//
// CRTP: `Derived` is the assembled `Printer`. `F` is the compile-time flag set.
// `W` is the byte sink (bun's `W` type parameter at :1608).
// ─────────────────────────────────────────────────────────────────────────────
template <typename Derived, PrinterFlags F, ByteSink W>
class PrinterCore {
private:
    W writer_;  // ref :1630 — `pub writer: W`

    [[nodiscard]] Derived& self_() { return static_cast<Derived&>(*this); }

public:
    // ref :1618-1620 — printer state the whole walk shares.
    bool needsSemicolon { false };  // ref :1618
    Options options {};             // ref :1620

    // ref :1621-1623 — source offsets used to decide whether an expression at a
    // statement/`export default`/arrow position needs wrapping in parens.
    std::int32_t stmtStart { -1 };
    std::int32_t exportDefaultStart { -1 };
    std::int32_t arrowExprStart { -1 };
    std::int32_t forOfInitStart { -1 };

    // ref :1626-1628 — operator/number/regex adjacency, so `a - -b` does not
    // print as `a--b` and `1 .toString()` keeps its space.
    //
    // ⚠️ -1, not 0 — ref the construction site at :7028-7030 (`prev_op_end: -1,
    // prev_num_end: -1, prev_reg_exp_end: -1`). These are compared against
    // `writer.written()`, which is `len - 1` and therefore starts at -1 (:7367).
    // Initialising them to 0 makes `written() == prev_reg_exp_end` fire at
    // offset 0 — i.e. after exactly one byte has been printed — so
    // `print_space_before_identifier` (:2136) injects a bogus leading space and
    // `{ a(); b(); }` minifies to `{ a();b()}` instead of `{a();b()}`.
    // Caught by tests/test_js_printer_stmt.cpp's minify vectors.
    std::int32_t prevOpEnd { -1 };
    std::int32_t prevNumEnd { -1 };
    std::int32_t prevRegExpEnd { -1 };

    bool hasPrintedBundledImportStatement { false };  // ref :1632
    std::uint32_t symbolCounter { 0 };                // ref :1639
    bool wasLazyExport { false };                     // ref :1647

public:
    explicit PrinterCore(W writer, Options opts = {})
        : writer_ { std::move(writer) }
        , options { std::move(opts) } { }

    [[nodiscard]] W& writer() { return writer_; }

    [[nodiscard]] static constexpr PrinterFlags flags() { return F; }

    // ── output ───────────────────────────────────────────────────────────────

    // ref :1908 / :1913. bun's `print` is generic over a `PrintArg` trait so it
    // takes bytes or a single char; two overloads are the MC++ spelling.
    void print(std::string_view str) { writer_.write_all(str); }
    void print(char c) { writer_.write_all(std::string_view { &c, 1 }); }
    void print_buffer(std::string_view str) { writer_.write_all(str); }

    // ── indentation ──────────────────────────────────────────────────────────

    // ref :1918 — note `saturating_sub`: unindenting at depth 0 stays at 0
    // rather than wrapping. Load-bearing on malformed input.
    void unindent() {
        if (options.indent.count > 0) {
            options.indent.count -= 1;
        }
    }

    // ref :1922
    void indent() { options.indent.count += 1; }

    // ref :1926
    void print_indent() {
        if (options.indent.count == 0 || options.minifyWhitespace) {
            return;
        }

        const std::array<char, 128>& buf { options.indent.character == IndentationCharacter::Space
                ? INDENTATION_SPACE_BUF
                : INDENTATION_TAB_BUF };

        std::size_t i { options.indent.count * options.indent.scalar };
        while (i > 0) {
            const std::size_t amt { std::min(i, buf.size()) };
            print(std::string_view { buf.data(), amt });
            i -= amt;
        }
    }

    // ── whitespace / punctuation ─────────────────────────────────────────────

    void print_space() {  // ref :1957
        if (!options.minifyWhitespace) {
            print(" ");
        }
    }

    void print_newline() {  // ref :1963
        if (!options.minifyWhitespace) {
            print("\n");
        }
    }

    // ref :1969 — when minifying, the semicolon is deferred: it is only emitted
    // if the *next* statement actually needs a separator
    // (`print_semicolon_if_needed`), which is how ASI-safe output drops them.
    void print_semicolon_after_statement() {
        if (!options.minifyWhitespace) {
            print(";\n");
        } else {
            needsSemicolon = true;
        }
    }

    void print_semicolon_if_needed() {  // ref :1976
        if (needsSemicolon) {
            print(";");
            needsSemicolon = false;
        }
    }

    void print_equals() {  // ref :1985
        print(options.minifyWhitespace ? "=" : " = ");
    }

    // ref :2604
    void print_whitespacer(Whitespacer spacer) {
        print(options.minifyWhitespace ? spacer.minify : spacer.normal);
    }

    // ── strings — the shard-1 escape layer's entry points ────────────────────

    // ref :2638. `quote` must be one of ' " ` (bun debug-asserts it at :2639).
    void print_string_characters_utf8(std::string_view text, std::uint8_t quote) {
        write_pre_quoted_string_inner<Encoding::Utf8>(text, writer_, quote, F.asciiOnly, false);
    }

    // ref :2650. `text` is the UTF-16 code units as raw little-endian bytes —
    // bun passes `bytemuck::cast_slice(&[u16])` at :2652. Callers holding a
    // `std::u16string_view` go through the overload below.
    void print_string_characters_utf16(std::string_view textAsBytes, std::uint8_t quote) {
        write_pre_quoted_string_inner<Encoding::Utf16>(
            textAsBytes, writer_, quote, F.asciiOnly, false);
    }

    void print_string_characters_utf16(std::u16string_view text, std::uint8_t quote) {
        print_string_characters_utf16(
            std::string_view { reinterpret_cast<const char*>(text.data()), text.size() * 2 }, quote);
    }

    // ref :3131 — `can_print_identifier_utf16`'s gate. Exposed here because it
    // is pure flag logic; the lexer-backed identifier classifiers it selects
    // between belong to the shard that ports `print_identifier`.
    [[nodiscard]] static constexpr bool identifier_must_be_latin1() {
        return F.asciiOnly || ASCII_ONLY_ALWAYS_ON_UNLESS_MINIFYING;
    }

    // ── DEFERRED — bun Printer fields with no mbun counterpart yet ────────────
    // Named so the shard that needs one knows what to port, not guessed at.
    //
    //   import_records: &[ImportRecord]        (:1617)  — shard 3 (print_stmt)
    //   prev_op: Op::Code                      (:1625)  — shard 2 (needs Op::Code)
    //   call_target: Option<ExprData>          (:1629)  — shard 2
    //   renamer: rename::Renamer               (:1634)  — shard 4
    //   prev_stmt_tag: StmtTag                 (:1635)  — shard 3
    //   source_map_builder: SourceMap::chunk::Builder (:1636) — sourcemap port
    //   temporary_bindings: Vec<B::Property>   (:1641)  — shard 4
    //   binary_expression_stack: Vec<BinaryExpressionVisitor> (:1643) — shard 2.
    //     ⚠️ This one is not incidental: bun iterates binary expressions on the
    //     heap instead of recursing, specifically to survive deeply-nested ASTs
    //     (:1653-1656). Do not "simplify" it back into recursion.
    //   stack_check / stack_overflowed         (:1645-1646) — same concern
    //   module_info: Option<&mut ModuleInfo>   (:1649)  — ModuleInfo port
    //   bump: &bun_alloc::Arena                (:1651)  — transient print-time
    //     allocations (rope flattening, UTF-16→UTF-8). mbun uses std::string's
    //     own growth today; revisit if profiling shows churn.
};

}  // namespace mbun::js_printer
