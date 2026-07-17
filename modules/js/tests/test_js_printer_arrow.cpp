// test_js_printer_arrow.cpp — the EArrow / EFunction / EClass arms, on the REAL
// Printer.
//
// Blueprint: .mbun/bun-ref/src/js_printer/lib.rs:3789 / :3835 / :3865
// Implementation: src/js_printer/arrow.cppm
//
// ── Why this is its own suite, and why it uses the real Printer ──────────────
// These three arms used to route to expr.cppm's `print_deferred_shard_`, which
// `requires`-probed for a `print_expr_deferred` that NOTHING ever defined, so
// the probe was always false and every arrow/function/class expression printed
// its RAW SOURCE SPAN with no erasure applied. `const f = (x: number): string
// => x` emitted its TS annotations verbatim — invalid JS, reported as success,
// because TranspileResult::ok only reflects the parse. A stubbed harness cannot
// catch that class of bug (test_js_printer_real.cpp's header explains why at
// length), so this suite instantiates the real assembled Printer, exactly as
// that file does.
//
// It is a separate file rather than more vectors in test_js_printer_real.cpp
// because that suite's stated job is INSTANTIATION and cross-shard seams, not
// per-arm byte fidelity ("⚠️ Read this before adding a vector", :30-35) — and
// its header explicitly tells vectors not to depend on the function/class arms,
// which is precisely what this file is about. Per-arm fidelity belongs in a
// per-arm suite.
//
// ── Where the expectations come from ─────────────────────────────────────────
// Every expectation below is the VERBATIM output of real bun 1.4.0 —
// `.mbun/bin/bun-rust`, whose `--version` matches the `.mbun/bun-ref` blueprint
// exactly — via `new Bun.Transpiler({loader:"ts"}).transformSync(src)`.
//
// ⚠️ transformSync, NOT `bun build`: `bun build` constant-folds and DCEs
// (`!(x => x)` becomes `false`, `export default function(){}` gets renamed to
// `input_default`), so it is the wrong oracle for a transpiler and would encode
// a bundler's output as this printer's target. transformSync is the API mbun's
// `transpile()` mirrors.
import std;
import mbun.ast;
import mbun.js_parser;
import mbun.js_printer;

namespace {

using mbun::js_parser::parse;
using mbun::js_parser::ParseResult;
using mbun::js_printer::IsTopLevel;
using mbun::js_printer::Options;
using mbun::js_printer::Printer;
using mbun::js_printer::PrinterFlags;
using mbun::js_printer::StringSink;
using mbun::js_printer::TopLevel;

int gChecks { 0 };
int gFailures { 0 };

std::string printable(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out += c;
        }
    }
    return out;
}

// The REAL, fully-assembled Printer — PrinterCore + Expr + Stmt + Binding +
// Property + Func + Arrow, no stubs.
using Real = Printer<PrinterFlags {}, StringSink>;

std::string print_src(std::string_view src) {
    ParseResult r { parse(src) };
    if (!r.ok) {
        return "<parse error: " + r.error + ">";
    }
    std::string out;
    Real p { StringSink { out }, Options {} };
    p.bind_arena(r.arena);
    p.source = src;
    p.print_stmt(r.program, TopLevel::init(IsTopLevel::Yes));
    return out;
}

void check(std::string_view src, std::string_view expected, std::string_view ref) {
    ++gChecks;
    const std::string got { print_src(src) };
    if (got != expected) {
        ++gFailures;
        std::println("  FAIL [{}]", ref);
        std::println("    src      \"{}\"", printable(src));
        std::println("    got      \"{}\"", printable(got));
        std::println("    expected \"{}\"", printable(expected));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// EArrow — ref :3789
// ─────────────────────────────────────────────────────────────────────────────
void test_arrow_args() {
    const char* R { "lib.rs:3803 (print_fn_args, is_arrow)" };
    // print_fn_args' `wrap` is a constant `true` (:2494), so a bare-identifier
    // head GAINS parens. This is also the regression guard for the Arg-invariant
    // bug: `x => x` built a bare Identifier where the list must hold `G::Arg`
    // (ast.cppm:82-85), so `arg.a` was NONE and the param printed as NOTHING —
    // `x => x` came out `() => x`, a silent arity change.
    check("const f = (x) => x;", "const f = (x) => x;\n", R);
    check("const f = x => x;", "const f = (x) => x;\n", R);
    check("const f = (a, b) => a;", "const f = (a, b) => a;\n", R);
    // ref :2517 — the default, via `print_whitespacer(ws!(b" = "))`.
    check("const f = (a = 1) => a;", "const f = (a = 1) => a;\n", R);
    // ref :2510 — the rest marker is FUNCTION-level (fnflags::HasRestArg) and
    // lands on the last arg. Dropping the flag printed `(r) => r`.
    check("const f = (...r) => r;", "const f = (...r) => r;\n", R);
    check("const f = (a, b = 1, ...r) => a;", "const f = (a, b = 1, ...r) => a;\n", R);
    // A destructured param — print_binding through the arrow seam.
    check("const f = ({a}) => a;", "const f = ({ a }) => a;\n", R);
}

void test_arrow_async() {
    const char* R { "lib.rs:3796 (EArrow is_async)" };
    check("const f = async () => 1;", "const f = async () => 1;\n", R);
    // The bare-identifier async head — `async` is not a reserved word, so the
    // parser must speculate. Losing the flag makes every `await` in the body a
    // SyntaxError, which is why this is a semantic vector, not a cosmetic one.
    check("const f = async x => await x;", "const f = async (x) => await x;\n", R);
    check("const f = async (x, y) => x;", "const f = async (x, y) => x;\n", R);
}

void test_arrow_ts_erasure() {
    const char* R { "lib.rs:3789 (EArrow) — TS erasure" };
    // ⚠️ THE vector this whole port exists for. Under the old raw-echo fallback
    // this printed `(x: number): string => x` — the TS annotations straight
    // through, invalid JS, and `TranspileResult::ok` still true.
    check("const f = (x: number): string => x;", "const f = (x) => x;\n", R);
    check("const f = <T>(x: T): T => x;", "const f = (x) => x;\n", R);
}

void test_arrow_body() {
    const char* R { "lib.rs:3811-3827 (prefer_expr vs print_block)" };
    // Expression body: mbun stores the expression node itself, so the kind test
    // (`!= Block`) stands in for bun's `stmts.len()==1 && prefer_expr && SReturn`.
    check("const f = (x) => x;", "const f = (x) => x;\n", R);
    // Block body — the same shape bun reaches via !prefer_expr.
    check("const f = (x) => { return x; };", "const f = (x) => {\n  return x;\n};\n", R);
    // An empty block stays a block: print_block emits `{}` for no stmts.
    check("const f = () => {};", "const f = () => {};\n", R);
    // ⚠️ arrow_expr_start (:3814): an object literal at the start of an arrow
    // body is a BLOCK, so EObject's wrap (expr.cppm:1215) must re-derive the
    // parens by OFFSET. mbun keeps a Paren node the parser did not drop, but the
    // Paren arm prints no bytes, so `arrowExprStart == written()` still holds.
    check("const f = () => ({a:1});", "const f = () => ({ a: 1 });\n", R);
    // The mirror: a paren that carries no meaning is dropped, because printing
    // is paren-transparent and Level::Comma forces nothing here.
    check("const f = () => (1);", "const f = () => 1;\n", R);
    // ⚠️ ForbidIn (:3815) — bun hands the body a FRESH `ExprFlag::ForbidIn`
    // rather than forwarding the incoming set, and it is OBSERVABLE: bun 1.4.0
    // prints these parens. Forwarding `flags` instead would silently drop them.
    check("const f = () => a in b;", "const f = () => (a in b);\n", R);
    // A sequence keeps its parens from Level::Comma, not from the Paren node.
    check("const f = () => (a, b);", "const f = () => (a, b);\n", R);
    // Nested arrows: the body is printed at Level::Comma, which is < Assign, so
    // the inner arrow does not wrap.
    check("const f = a => b => a + b;", "const f = (a) => (b) => a + b;\n", R);
}

void test_arrow_wrap() {
    const char* R { "lib.rs:3790 (wrap = level.gte(Level::Assign))" };
    // Callee position is Level::Postfix >= Assign, so the arrow wraps.
    check("(x => x)(1);", "((x) => x)(1);\n", R);
    // A call ARGUMENT is Level::Comma < Assign — no wrap.
    check("f(x => x);", "f((x) => x);\n", R);
    // A member target wraps.
    check("(() => x).call;", "(() => x).call;\n", R);
}

// ─────────────────────────────────────────────────────────────────────────────
// EFunction — ref :3835
// ─────────────────────────────────────────────────────────────────────────────
void test_function_expr() {
    const char* R { "lib.rs:3835 (EFunction)" };
    // ⚠️ No space before `(`: unlike the DECLARATION arm (:5326-5332, an
    // if/ELSE), the EFunction arm's print_space_before_identifier lives INSIDE
    // the name branch (:3855), so an anonymous function gets none.
    check("const f = function(){};", "const f = function() {};\n", R);
    check("const f = function g(){};", "const f = function g() {};\n", R);
    // ⚠️ `function*` keeps the space AFTER the star (:3850-3851) — reads wrong,
    // is bun's, and is confirmed against bun 1.4.0.
    check("const f = function*(){};", "const f = function* () {};\n", R);
    check("const f = async function(){};", "const f = async function() {};\n", R);
    check("const f = async function*(){};", "const f = async function* () {};\n", R);
    // TS erasure through the function-expression seam.
    check("const f = function(x: number): void {};", "const f = function(x) {};\n", R);
}

void test_function_expr_wrap() {
    const char* R { "lib.rs:3836-3837 (wrap = stmt_start || export_default_start)" };
    // ⚠️ EFunction/EClass wrap on stmt_start||export_default_start; EObject
    // wraps on stmt_start||arrow_expr_start. Three offsets, two rules.
    check("(function f(){})();", "(function f() {})();\n", R);
    check("(function(){})();", "(function() {})();\n", R);
    // Not at a statement start — no parens, even though the source had them.
    check("const f = (function(){});", "const f = function() {};\n", R);
}

// ─────────────────────────────────────────────────────────────────────────────
// EClass — ref :3865
// ─────────────────────────────────────────────────────────────────────────────
void test_class_expr() {
    const char* R { "lib.rs:3865 (EClass)" };
    // print_class always opens a body block with a newline (func.cppm:180-182),
    // so even an empty class expression is two lines. bun does the same.
    check("const C = class {};", "const C = class {\n};\n", R);
    check("const C = class X {};", "const C = class X {\n};\n", R);
    // ref :2544-2548 — `extends` at Level::New.sub(1).
    check("const C = (class X extends Y {});", "const C = class X extends Y {\n};\n", R);
    // A member, through the print_property seam.
    check("const C = class { m(){} };", "const C = class {\n  m() {}\n};\n", R);
    // A class FIELD has no value, so it needs its `;` (func.cppm:217).
    check("const C = class { static s = 1; };", "const C = class {\n  static s = 1;\n};\n", R);
}

void test_class_expr_wrap() {
    const char* R { "lib.rs:3866-3867 (wrap)" };
    check("(class{}).name;", "(class {\n}).name;\n", R);
}

// ─────────────────────────────────────────────────────────────────────────────
// The call-spread regression — ref :737-753
//
// Not an arrow arm, but it lived here: `parse_arguments_` consumed the `...` and
// dropped it, so `f(...args)` printed `f(args)`. It was unobservable while the
// enclosing function bodies were raw-echoed, and it is a silent arity change.
// The array-literal path already carried the fix and the comment recording the
// same bug ("Dropping it turned `[...a]` into `[a]`").
// ─────────────────────────────────────────────────────────────────────────────
void test_call_spread() {
    const char* R { "lib.rs:3224 (E::Spread) via parse_arguments_" };
    check("f(...args);", "f(...args);\n", R);
    check("g(a, ...b);", "g(a, ...b);\n", R);
    check("new J(...k);", "new J(...k);\n", R);
    // The two that always worked — kept adjacent so a diff localises a break.
    check("const h = [...c];", "const h = [...c];\n", R);
    check("const i = {...d};", "const i = { ...d };\n", R);
}

}  // namespace

int main() {
    std::println("test_js_printer_arrow — EArrow/EFunction/EClass (real Printer)");
    test_arrow_args();
    test_arrow_async();
    test_arrow_ts_erasure();
    test_arrow_body();
    test_arrow_wrap();
    test_function_expr();
    test_function_expr_wrap();
    test_class_expr();
    test_class_expr_wrap();
    test_call_spread();
    std::println("{} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
