// test_js_printer_expr.cpp — CAP-BUILD-PRINTER shard 2/4 test suite.
//
// Covers the expression printer ported from .mbun/bun-ref/src/js_printer/lib.rs
// :3195 (`print_expr`) and .mbun/bun-ref/src/ast/op.rs (the Level/Code/TABLE
// precedence model): modules mbun.js_printer.{expr_op,expr}.
//
// ── Where the expectations come from ─────────────────────────────────────────
// NOT from reading the Rust. Every `bun says` vector below is transcribed from
// the actual output of real bun, captured with:
//
//   bun -e 'const t = new Bun.Transpiler({loader:"ts"});
//           console.log(JSON.stringify(t.transformSync(<src>)))'
//
// ⚠️ VERSION SKEW. The bun on this box is 1.3.14; the blueprint in .mbun/bun-ref
// is 1.4.0. Where they could disagree the BLUEPRINT WINS. Every vector here was
// additionally checked against the 1.4.0 source it is supposed to be testing —
// each `ref` comment cites the lib.rs/op.rs line that produces the bytes, so a
// 1.3.14-only quirk would show up as a vector with no blueprint line behind it.
// None of the behaviours below changed between the two: they are all produced by
// code paths (`print_space_before_operator`, the `Op::TABLE` precedence
// comparisons, `print_number`) that are byte-identical in the 1.4.0 tree.
//
// ⚠️ transformSync DEAD-CODE-ELIMINATES side-effect-free statements, so a bare
// `0x10;` prints "" and cannot be probed directly. Number/precedence vectors are
// therefore captured in a side-effecting position (`x = <expr>`) — noted per
// group where it matters.
import std;
import mbun.ast;
import mbun.js_parser;
import mbun.js_printer;

namespace {

int gChecks { 0 };
int gFailures { 0 };

std::string printable(std::string_view s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\\': out += "\\\\"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\x{:02X}", static_cast<unsigned char>(c));
            } else {
                out += c;
            }
        }
    }
    return out;
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view what) {
    gChecks += 1;
    if (actual != expected) {
        gFailures += 1;
        std::println("FAIL {}\n  expected: \"{}\"\n  actual:   \"{}\"", what,
            printable(expected), printable(actual));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// The assembled printer under test — the REAL one.
//
// This suite used to hand-assemble `PrinterCore + ExprPrinter` and re-implement
// the six members ExprPrinter reaches through `self_()` that StmtPrinter owns
// (`arena()`/`node()`/`written()`/`prev_char()`/
// `print_space_before_identifier()`/`add_source_mapping()`, stmt.cppm:23-47).
// Its own comment carried the warning that made the case for deleting it:
// "Keep these bodies IDENTICAL to stmt.cppm's. If they drift, this suite stops
// testing what the real Printer does."
//
// They can no longer drift, because there is no copy: every shard has landed, so
// `Printer` itself is now assemblable and instantiable (it was NOT until
// func.cppm — `print_stmt` called `print_function_decl`/`print_class_decl`, which
// nothing defined; see tests/test_js_printer_real.cpp's header). The original
// reason for the subset — "mixing in shard 3's StmtPrinter would let a
// statement-printer change move these numbers" — does not apply to the six
// members in question: they are the plumbing ExprPrinter was ALREADY calling
// through `self_()`, so using the real ones is what makes these vectors describe
// the real Printer rather than a lookalike.
//
// This suite still pins SHARD 2's bytes: every vector below drives `print_expr`
// directly, never `print_stmt`.
// ─────────────────────────────────────────────────────────────────────────────
using mbun::js_printer::Code;
using mbun::js_printer::ExprPrinter;
using mbun::js_printer::Level;
using mbun::js_printer::Printer;
using mbun::js_printer::PrinterCore;
using mbun::js_printer::PrinterFlags;
using mbun::js_printer::StringSink;
using mbun::js_printer::expr_flag::none;

template <PrinterFlags F>
using TestPrinter = Printer<F, StringSink>;

constexpr PrinterFlags PLAIN {};

// Parse `<src>` (a single expression statement) and print its expression back.
// Returns the printed bytes. `src` must be a complete statement (`a + b;`).
std::string print_expr_of(std::string_view src, Level level = Level::Lowest) {
    mbun::js_parser::ParseResult r { mbun::js_parser::parse(src) };
    if (r.program == mbun::ast::NONE) {
        return "<<parse failed>>";
    }
    const mbun::ast::Node& prog { r.arena.at(r.program) };
    const std::span<const mbun::ast::NodeIndex> stmts { r.arena.list_of(prog) };
    if (stmts.empty()) {
        return "<<no statements>>";
    }
    const mbun::ast::Node& st { r.arena.at(stmts[0]) };
    if (st.kind != mbun::ast::NodeKind::ExpressionStmt) {
        return "<<not an expression statement>>";
    }

    std::string out;
    TestPrinter<PLAIN> p { StringSink { out } };
    p.bind_arena(r.arena);
    p.source = src;
    p.print_expr(st.a, level, none());
    return out;
}

// ═════════════════════════════════════════════════════════════════════════════
// Precedence / paren minimisation — ref op.rs:301 (TABLE) + lib.rs:1716
// (`binary_check_and_prepare`). This is the heart of the shard: bun DROPS the
// source's parens (its AST has no paren node) and re-derives them from `Level`.
// ═════════════════════════════════════════════════════════════════════════════
void test_binary_precedence() {
    // bun says: "a + b * c;"  — `*` binds tighter, no parens needed.
    check_eq(print_expr_of("a + b * c;"), "a + b * c", "prec: a + b * c");
    // bun says: "(a + b) * c;" — parens REQUIRED, and re-derived not copied.
    check_eq(print_expr_of("(a + b) * c;"), "(a + b) * c", "prec: (a + b) * c");
    // bun says: "a - (b - c);" — right operand of a left-assoc op at same level.
    check_eq(print_expr_of("a - (b - c);"), "a - (b - c)", "prec: a - (b - c)");
    // bun says: "a * (b + c);"
    check_eq(print_expr_of("a * (b + c);"), "a * (b + c)", "prec: a * (b + c)");
    // Left-assoc chain must NOT re-parenthesise — ref op.rs:119 + lib.rs:1748.
    check_eq(print_expr_of("a - b - c;"), "a - b - c", "prec: a - b - c (left-assoc)");
    // bun says: "a, b, c;" — ref op.rs:350, comma has no leading space.
    check_eq(print_expr_of("a, b, c;"), "a, b, c", "prec: comma chain");
    // bun says: "(a, b) + c;"
    check_eq(print_expr_of("(a, b) + c;"), "(a, b) + c", "prec: (a, b) + c");
}

void test_exponentiation() {
    // `**` is RIGHT-associative — ref op.rs:122/:126. bun says: "a ** b ** c;"
    check_eq(print_expr_of("a ** b ** c;"), "a ** b ** c", "pow: right-assoc chain");
    // ...so the LEFT-nested form must keep its parens. bun says: "(a ** b) ** c;"
    check_eq(print_expr_of("(a ** b) ** c;"), "(a ** b) ** c", "pow: (a ** b) ** c");
    // ref :1765-1772 — `-a ** b` is a SyntaxError; the unary operand forces
    // Level::Call on the left. bun says: "(-a) ** b;"
    check_eq(print_expr_of("(-a) ** b;"), "(-a) ** b", "pow: (-a) ** b keeps parens");
    // bun says: "(a + b) ** c;"
    check_eq(print_expr_of("(a + b) ** c;"), "(a + b) ** c", "pow: (a + b) ** c");
}

void test_nullish_and_logical() {
    // ref :1752-1763 — `??` cannot directly contain `||`/`&&`; a real grammar
    // restriction, so the parens are mandatory. bun says: "a ?? (b || c);"
    check_eq(print_expr_of("a ?? (b || c);"), "a ?? (b || c)", "nullish: a ?? (b || c)");
    // bun says: "(a && b) ?? c;"
    check_eq(print_expr_of("(a && b) ?? c;"), "(a && b) ?? c", "nullish: (a && b) ?? c");
    // `&&` binds tighter than `||` — ref op.rs:344/:345. bun says: "a || b && c;"
    check_eq(print_expr_of("a || b && c;"), "a || b && c", "logical: a || b && c");
    // bun says: "(a || b) && c;"
    check_eq(print_expr_of("(a || b) && c;"), "(a || b) && c", "logical: (a || b) && c");
}

void test_conditional() {
    // bun says: "a ? b : c;"
    check_eq(print_expr_of("a ? b : c;"), "a ? b : c", "cond: a ? b : c");
    // ref :3770 — `level.gte(Level::Conditional)` wraps a test that is itself a
    // conditional. bun says: "(a ? b : c) ? d : e;"
    check_eq(print_expr_of("(a ? b : c) ? d : e;"), "(a ? b : c) ? d : e", "cond: nested in test");
    // ...but a conditional in the CONSEQUENT needs no parens: bun DROPS them.
    // bun says: "a ? b ? c : d : e;"  (input was "a ? (b ? c : d) : e;")
    check_eq(print_expr_of("a ? (b ? c : d) : e;"), "a ? b ? c : d : e", "cond: parens dropped in yes");
}

void test_assignment() {
    // `=` is right-associative — ref op.rs:126. bun says: "a = b = c;"
    check_eq(print_expr_of("a = b = c;"), "a = b = c", "assign: right-assoc chain");
    // bun says: "a += b;"
    check_eq(print_expr_of("a += b;"), "a += b", "assign: +=");
}

// ═════════════════════════════════════════════════════════════════════════════
// Operator adjacency — ref lib.rs:4524 `print_space_before_operator`.
// These vectors are the reason `prev_op` / `prev_op_end` exist at all.
// ═════════════════════════════════════════════════════════════════════════════
void test_operator_adjacency() {
    // bun says: "- -a;"  — `--a` would lex as a decrement. ref :4536-4540.
    check_eq(print_expr_of("-(-a);"), "- -a", "adjacency: -(-a) => - -a");
    check_eq(print_expr_of("- -a;"), "- -a", "adjacency: - -a stays");
    // bun says: "a - -b;"
    check_eq(print_expr_of("a - -b;"), "a - -b", "adjacency: a - -b");
    // bun says: "a + +b;"
    check_eq(print_expr_of("a + +b;"), "a + +b", "adjacency: a + +b");
    // bun says: "a-- > b;" — `-->` would open an HTML comment. ref :4543.
    check_eq(print_expr_of("a-- > b;"), "a-- > b", "adjacency: a-- > b");

    // ⚠️ THE LEADING-SPACE QUIRK — ref :7027 (`prev_op: Op::Code::BinAdd`) +
    // :7367 (`written: -1`). At position 0, `prev_op_end == written()` (both -1)
    // so `print_space_before_operator` FIRES, and the seeded `BinAdd` matches the
    // `+`/`++` arm. Real bun 1.3.14 genuinely emits the leading space:
    //     "+(+a);"  =>  " + +a;\n"
    //     "++a;"    =>  " ++a;\n"
    //     "-(-a);"  =>  "- -a;\n"     <- the control: BinAdd does NOT match `-`
    // The third vector is what proves the seed is BinAdd rather than a neutral
    // sentinel. Reproducing this is required for byte-identical output.
    check_eq(print_expr_of("+(+a);"), " + +a", "adjacency: leading space (prev_op seeds BinAdd)");
    check_eq(print_expr_of("++a;"), " ++a", "adjacency: ++a leading space");
}

// ═════════════════════════════════════════════════════════════════════════════
// Numbers — ref lib.rs:6926 `print_number` / :2612 `print_non_negative_float`.
// Probed through `x = <n>` because transformSync DCEs a bare literal statement.
// ═════════════════════════════════════════════════════════════════════════════
std::string print_rhs_of(std::string_view src) {
    // `x = <expr>;` — return just the RHS by printing the Assignment's `b`.
    mbun::js_parser::ParseResult r { mbun::js_parser::parse(src) };
    if (r.program == mbun::ast::NONE) {
        return "<<parse failed>>";
    }
    const std::span<const mbun::ast::NodeIndex> stmts { r.arena.list_of(r.arena.at(r.program)) };
    if (stmts.empty()) {
        return "<<no statements>>";
    }
    const mbun::ast::Node& st { r.arena.at(stmts[0]) };
    const mbun::ast::Node& assign { r.arena.at(st.a) };
    std::string out;
    TestPrinter<PLAIN> p { StringSink { out } };
    p.bind_arena(r.arena);
    p.source = src;
    p.print_expr(assign.b, Level::Lowest, none());
    return out;
}

void test_number_normalisation() {
    // bun says (via `x = 0x10;`  =>  "x = 16;\n"): radix prefixes are FOLDED.
    check_eq(print_rhs_of("x = 0x10;"), "16", "number: 0x10 => 16");
    check_eq(print_rhs_of("x = 0b101;"), "5", "number: 0b101 => 5");
    check_eq(print_rhs_of("x = 0o17;"), "15", "number: 0o17 => 15");
    // bun says (`x = 1_000_000;` => "x = 1e6;\n"): separators stripped AND the
    // pow10 shortening fires — ref :2624 `pow10_exp_1e4_to_1e9`.
    check_eq(print_rhs_of("x = 1_000_000;"), "1e6", "number: 1_000_000 => 1e6");
    check_eq(print_rhs_of("x = 1000000;"), "1e6", "number: 1000000 => 1e6");
    check_eq(print_rhs_of("x = 1e6;"), "1e6", "number: 1e6 stays");
    // Below the 1e4 floor there is nothing to win — ref the pow10 range.
    check_eq(print_rhs_of("x = 1000;"), "1000", "number: 1000 stays (below 1e4)");
    check_eq(print_rhs_of("x = 10000;"), "1e4", "number: 10000 => 1e4");
    // Non-integers take the shortest-round-trip float path — ref :2635.
    check_eq(print_rhs_of("x = 1.5;"), "1.5", "number: 1.5");
    check_eq(print_rhs_of("x = 0.5;"), "0.5", "number: 0.5");
}

void test_number_adjacency() {
    // ⚠️ THE `prev_num_end` VECTOR — ref :3692. bun 1.3.14:
    //     "(1).toString();"  =>  "1 .toString();\n"
    // bun drops the parens (its AST has none) and then MUST insert a space,
    // because `1.toString()` is a syntax error. This is the single best probe
    // that `prev_num_end` and the -1-based `written()` are both right: with a
    // len-based counter the comparison fails and the space vanishes.
    check_eq(print_expr_of("(1).toString();"), "1 .toString()", "number: (1).toString() => 1 .toString()");
    check_eq(print_expr_of("1 .toString();"), "1 .toString()", "number: 1 .toString() stays");
    // ref :6966-6973 — a NEGATIVE number at Level::Prefix wraps instead.
    // bun says: "(-1).toString();"
    check_eq(print_expr_of("(-1).toString();"), "(-1).toString()", "number: (-1).toString() wraps");
}

// ═════════════════════════════════════════════════════════════════════════════
// Members / calls / new — ref :3663 / :3478 / :3435
// ═════════════════════════════════════════════════════════════════════════════
void test_member_and_call() {
    check_eq(print_expr_of("a.b.c;"), "a.b.c", "member: a.b.c");
    check_eq(print_expr_of("a[b][c];"), "a[b][c]", "index: a[b][c]");
    check_eq(print_expr_of("f(a, b);"), "f(a, b)", "call: f(a, b)");
    check_eq(print_expr_of("f();"), "f()", "call: f()");
    // bun says: "(a, b).c;" — the comma expression must keep its parens.
    check_eq(print_expr_of("(a, b).c;"), "(a, b).c", "member: (a, b).c");
    // ref :3455 — `new X()` DROPS its empty parens. bun says: "new X;"
    check_eq(print_expr_of("new X();"), "new X", "new: new X() => new X");
    check_eq(print_expr_of("new X;"), "new X", "new: new X stays");
    // ...but not when a member access follows. bun says: "new X().y;"
    check_eq(print_expr_of("new X().y;"), "new X().y", "new: new X().y keeps parens");
    // ref :3448 — `forbid_call` on the target. bun says: "new (a());"
    check_eq(print_expr_of("new (a())();"), "new (a())", "new: new (a())");
    // bun says: "new a.b;"
    check_eq(print_expr_of("new (a.b)();"), "new a.b", "new: new (a.b)() => new a.b");
}

// ═════════════════════════════════════════════════════════════════════════════
// Unary — ref :4386
// ═════════════════════════════════════════════════════════════════════════════
void test_unary() {
    // Keyword operators need the space — ref :4398-4402.
    check_eq(print_expr_of("typeof a;"), "typeof a", "unary: typeof a");
    check_eq(print_expr_of("void a;"), "void a", "unary: void a");
    check_eq(print_expr_of("delete a.b;"), "delete a.b", "unary: delete a.b");
    check_eq(print_expr_of("!a;"), "!a", "unary: !a");
    check_eq(print_expr_of("~a;"), "~a", "unary: ~a");
    // `await` — mbun encodes it as Unary with aux==0; see expr_op.cppm's header.
    check_eq(print_expr_of("await a;"), "await a", "unary: await a (aux==0 seam)");
    // Postfix vs prefix — ref op.rs:21-26 + the Node::flags bit0 seam.
    check_eq(print_expr_of("a++;"), "a++", "update: a++ (postfix)");
    check_eq(print_expr_of("a--;"), "a--", "update: a-- (postfix)");
    // ref :4388 — a unary inside a tighter context wraps.
    check_eq(print_expr_of("(typeof a).x;"), "(typeof a).x", "unary: (typeof a).x wraps");
}

// ═════════════════════════════════════════════════════════════════════════════
// Strings — ref :4016 + quote.cppm's `best_quote_char_for_string`.
// ⚠️ See `print_string_literal_raw_`: quote CHOICE is reproduced, escape-sequence
// DECODING is not (mbun's AST has no decoded value). Vectors are limited to what
// this shard actually claims.
// ═════════════════════════════════════════════════════════════════════════════
void test_strings() {
    // bun says: "\"a\";" — single quotes normalise to double.
    check_eq(print_expr_of("'a';"), "\"a\"", "string: 'a' => \"a\"");
    check_eq(print_expr_of("\"a\";"), "\"a\"", "string: \"a\" stays");
    // bun says: "'a\"b';" — a double quote inside flips the delimiter.
    check_eq(print_expr_of("'a\"b';"), "'a\"b'", "string: 'a\"b' keeps single");
}

// ═════════════════════════════════════════════════════════════════════════════
// Literals / identifiers / misc
// ═════════════════════════════════════════════════════════════════════════════
void test_literals() {
    check_eq(print_expr_of("null;"), "null", "literal: null");
    check_eq(print_expr_of("this;"), "this", "literal: this");
    check_eq(print_expr_of("true;"), "true", "literal: true");
    check_eq(print_expr_of("false;"), "false", "literal: false");
    check_eq(print_expr_of("x;"), "x", "literal: identifier");
    check_eq(print_expr_of("[a, b];"), "[a, b]", "literal: array");
    check_eq(print_expr_of("[];"), "[]", "literal: empty array");
    check_eq(print_expr_of("/ab+c/g;"), "/ab+c/g", "literal: regexp verbatim");
    check_eq(print_expr_of("123n;"), "123n", "literal: bigint verbatim");
    // mbun-only Paren node must be TRANSPARENT — otherwise `((a))` survives and
    // paren minimisation is defeated. bun has no paren node at all.
    check_eq(print_expr_of("((a));"), "a", "paren: mbun Paren node is transparent");
    check_eq(print_expr_of("(a + b);"), "a + b", "paren: redundant parens dropped");
}

// ═════════════════════════════════════════════════════════════════════════════
// ✅ FORMERLY KNOWN GAPS — now fixed, and pinned as positive assertions.
//
// These three were characterization tests asserting mbun's WRONG output, kept as
// tripwires that would break once the parser recorded what it was throwing away.
// They have now tripped and are updated to bun's real output. The parser fixes:
//   * `[...a]` — parse_array_literal_ builds a `SpreadElement` instead of
//     `advance_()`-ing past the `...` (ref parse_prefix.rs:737-753).
//   * `[, x]`  — an elision builds a `Missing` element, and the separating comma
//     is consumed by the ONE shared tail, not by the elision arm
//     (ref parse_prefix.rs:731-736 + :761-769).
//   * `'\x41'` — the parser records the lexer's DECODED value on the node
//     (ast.cppm `Arena::add_string`), so the printer re-encodes the value rather
//     than re-escaping raw source bytes (ref lib.rs:4029).
//
// Every expectation below is verified against real bun via `transformSync`
// (loader:"js"), and is IDENTICAL on bun 1.3.14 and the 1.4.0 blueprint.
// ═════════════════════════════════════════════════════════════════════════════
void test_known_gaps() {
    // bun says: "[...a];" — the spread marker survives as an `E::Spread` node.
    check_eq(print_expr_of("[...a];"), "[...a]",
        "array spread marker recorded by parser (SpreadElement)");

    // bun says: "[, x];" — `[, x]` has length 2, `[x]` has length 1.
    check_eq(print_expr_of("[, x];"), "[, x]",
        "array elision recorded by parser (Missing element)");

    // bun says: "A" — the ONE-character string A, not the four bytes `\x41`.
    check_eq(print_expr_of("'\\x41';"), "\"A\"",
        "string escape decoded, not doubled (decoded value on the AST node)");

    // ── the boundaries the three fixes must not break ────────────────────────
    // A trailing comma is NOT a hole: it exits via the loop condition without
    // pushing, so this stays length 1. bun: "[a];"
    check_eq(print_expr_of("[a,];"), "[a]", "trailing comma is not an elision");
    // ...but a second one IS a hole: length 2. bun: "[a, ,];"
    check_eq(print_expr_of("[a,,];"), "[a, ,]", "second trailing comma is an elision");
    // A lone hole: length 1. bun: "[,];"
    check_eq(print_expr_of("[,];"), "[,]", "lone elision");
    // Holes and spreads mixing, with a hole in the middle. bun: "[a, ...b, , c];"
    check_eq(print_expr_of("[a, ...b, , c];"), "[a, ...b, , c]", "elision + spread mixed");

    // ── decoded string values ────────────────────────────────────────────────
    // An unescaped string decodes to itself. bun: "A"
    check_eq(print_expr_of("'A';"), "\"A\"", "plain string round-trips");
    // ⚠️ bun prints NUL as `\x00`, NOT as `\0` (verified on 1.3.14). A `\0`
    // followed by a digit would change meaning, so bun never emits the short form.
    check_eq(print_expr_of("'\\0';"), "\"\\x00\"", "NUL prints as \\x00, matching bun");
    check_eq(print_expr_of("'\\x00A';"), "\"\\x00A\"", "NUL followed by a digit-safe char");
    // ⚠️ A surrogate PAIR stays escaped per code unit — bun's utf16 path does not
    // re-fold it into a UTF-8 astral character. bun: "😀" (😀).
    check_eq(print_expr_of("'\\uD83D\\uDE00';"), "\"\\uD83D\\uDE00\"",
        "surrogate pair escaped per code unit");
    // A LONE surrogate is why the decoded value is UTF-16 and not UTF-8: it has
    // no UTF-8 encoding at all, so a utf8 round-trip would corrupt it. bun: "\uD800"
    check_eq(print_expr_of("'\\uD800';"), "\"\\uD800\"", "lone surrogate survives");
    // The quote choice is made from the DECODED units, so a decoded newline makes
    // a backtick cheapest — this is skeleton-agent 差分 finding #3, now correct.
    // bun: `<LF>` (a template literal holding a literal newline).
    check_eq(print_expr_of("'\\n';"), "`\n`", "decoded newline prefers a backtick");
    // Decoded quote characters drive the choice too. bun: "a'b"
    check_eq(print_expr_of("'a\\'b';"), "\"a'b\"", "decoded quote picks the other delimiter");
}

// ═════════════════════════════════════════════════════════════════════════════
// The `Level` argument — the caller-supplied context that drives wrapping.
// ═════════════════════════════════════════════════════════════════════════════
void test_level_argument() {
    // At Lowest nothing wraps; at a level above the operator's own, it must.
    check_eq(print_expr_of("a + b;", Level::Lowest), "a + b", "level: Lowest => bare");
    // ref op.rs:323 — `+` is Level::Add; asking for Multiply forces parens.
    check_eq(print_expr_of("a + b;", Level::Multiply), "(a + b)", "level: Multiply => wrapped");
    check_eq(print_expr_of("a + b;", Level::Add), "(a + b)", "level: Add => wrapped (gte)");
    // ref :1740 — a left-assoc op's right operand gets `e_level`, so an equal
    // level still wraps: that is what makes `a - (b - c)` keep its parens.
    check_eq(print_expr_of("a * b;", Level::Add), "a * b", "level: Add < Multiply => bare");
}

// ═════════════════════════════════════════════════════════════════════════════
// Deep left-nesting — ref :4431 and printer_core.cppm:279-282.
//
// The binary printer iterates the LEFT spine on the heap instead of recursing,
// specifically so a long `a+a+a+...` chain cannot blow the stack. This builds a
// chain far deeper than the C++ stack would survive under recursion, which is
// what makes it a real test of the machinery rather than a smoke test.
// ═════════════════════════════════════════════════════════════════════════════
void test_deep_left_nesting() {
    constexpr int N { 20000 };
    std::string src { "a" };
    src.reserve(N * 4 + 8);
    for (int i { 0 }; i < N; ++i) {
        src += " + a";
    }
    src += ";";

    std::string expected { "a" };
    for (int i { 0 }; i < N; ++i) {
        expected += " + a";
    }

    const std::string got { print_expr_of(src) };
    check_eq(got, expected, std::format("binary: {}-deep left spine is heap-iterated", N));
}

}  // namespace

int main() {
    test_binary_precedence();
    test_exponentiation();
    test_nullish_and_logical();
    test_conditional();
    test_assignment();
    test_operator_adjacency();
    test_number_normalisation();
    test_number_adjacency();
    test_member_and_call();
    test_unary();
    test_strings();
    test_literals();
    test_known_gaps();
    test_level_argument();
    test_deep_left_nesting();

    std::println("test_js_printer_expr: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
