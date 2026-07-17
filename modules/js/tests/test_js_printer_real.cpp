// test_js_printer_real.cpp — the REAL `Printer`, not a stub.
//
// ── Why this file exists ─────────────────────────────────────────────────────
// Every other printer test (test_js_printer_{stmt,binding,expr,property}.cpp)
// assembles its OWN Printer-shaped type out of a subset of the CRTP mixins plus
// stubs for whatever had not landed yet. That was the right call while the
// shards were being translated in parallel — each shard could test its own arms
// without waiting on the others (printer_core.cppm:53-62).
//
// It also had a hole big enough to hide a whole broken seam in, and it did:
//
//   binding.cppm:366 calls `self_().print_string_literal_utf8(name, false)`.
//   expr.cppm defined that method PRIVATE and `_`-suffixed
//   (`print_string_literal_utf8_`). So on the real assembled `Printer` the name
//   binding.cppm calls DID NOT EXIST, and `Printer::print_binding` could not be
//   instantiated at all. Nothing caught it, because
//   test_js_printer_binding.cpp:111 defines a `print_string_literal_utf8` stub
//   on its own harness type — the 21 green binding vectors were green against
//   the stub. A CRTP mixin's member bodies are not instantiated until used, so
//   "it compiles" proves nothing about a method nobody calls.
//
// This file closes that hole by instantiating the REAL
// `mbun::js_printer::Printer<F, StringSink>` — PrinterCore + ExprPrinter +
// StmtPrinter + BindingPrinter + PropertyPrinter, no stubs — and CALLING the
// cross-shard entry points. Every `self_()` seam between the mixins has to
// resolve for this translation unit to compile. If someone re-privatises a seam
// or renames one out from under a caller, this file stops building; the other
// suites would stay green.
//
// ⚠️ Read this before adding a vector: this suite's job is INSTANTIATION and the
// cross-shard seams, not print fidelity. Per-arm byte fidelity is the individual
// shard suites' job and they have far denser coverage. Vectors here are the ones
// that (a) force a seam to resolve and (b) are stable across the arms that are
// still DEFERRED — an object/class/function body still echoes its source span
// (expr.cppm's print_deferred_shard_), so vectors must not depend on those.
//
// ── Where the expectations come from ─────────────────────────────────────────
// Real bun 1.3.14 (`new Bun.Transpiler({loader:"js"}).transformSync(src)`),
// cross-read against the 1.4.0 blueprint in .mbun/bun-ref. Where the two
// disagree the blueprint wins; the vectors below are ones where they agree.
import std;
import mbun.ast;
import mbun.js_parser;
import mbun.js_parser.module_scope;  // filter_export_clauses — see print_src_filtered
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

// The REAL Printer — this alias is the whole point of the file.
using Real = Printer<PrinterFlags {}, StringSink>;

// Print `src` through the real, fully-assembled Printer.
std::string print_src(std::string_view src, Options opts = {}) {
    ParseResult r { parse(src) };
    if (!r.ok) {
        return "<parse error: " + r.error + ">";
    }
    std::string out;
    Real p { StringSink { out }, std::move(opts) };
    p.bind_arena(r.arena);
    p.source = src;  // ExprPrinter's (expr.cppm:209)
    p.print_stmt(r.program, TopLevel::init(IsTopLevel::Yes));
    return out;
}

// Print `src` the way `transpile()`'s printer branch does: run the post-parse AST
// passes FIRST, then print. Today that means `filter_export_clauses`
// (js_parser/module_scope.cppm, ref visit/visit_stmt.rs:168), which compacts the
// dead `export {…}` specifiers out of the arena.
//
// `print_src` above cannot be used for these vectors and must not be "fixed" to:
// it exists to test the PRINTER in isolation, and the printer deliberately does
// not know what is in scope — bun's does not either (lib.rs:5583 prints whatever
// survived the visit pass). Printing `export {a}` through the raw printer emits
// `export { a };` for an undeclared `a`, which is correct-for-the-printer and
// wrong-for-the-file. The filter is a parser pass, so the test has to run it.
std::string print_src_filtered(std::string_view src) {
    ParseResult r { parse(src) };
    if (!r.ok) {
        return "<parse error: " + r.error + ">";
    }
    mbun::js_parser::detail::filter_export_clauses(r.arena, r.program);
    std::string out;
    Real p { StringSink { out } };
    p.bind_arena(r.arena);
    p.source = src;
    p.print_stmt(r.program, TopLevel::init(IsTopLevel::Yes));
    return out;
}

// Print `src` and report what the printer RECORDED rather than what it emitted:
// "<kind>" for an unported kind, "" for a clean print. The reason a separate
// helper is needed at all is the point of the change it tests — `print_src`
// above cannot distinguish "printed nothing because nothing is correct" from
// "printed nothing because there is no port", and for this printer's whole life
// neither could anything else.
std::string unsupported_of(std::string_view src) {
    ParseResult r { parse(src) };
    if (!r.ok) {
        return "<parse error: " + r.error + ">";
    }
    std::string out;
    Real p { StringSink { out } };
    p.bind_arena(r.arena);
    p.source = src;
    p.print_stmt(r.program, TopLevel::init(IsTopLevel::Yes));
    if (!p.has_unsupported()) {
        return "";
    }
    return std::string { mbun::ast::node_kind_name(p.first_unsupported().kind) };
}

void check_unsupported(std::string_view src, std::string_view expected, std::string_view ref) {
    ++gChecks;
    const std::string got { unsupported_of(src) };
    if (got != expected) {
        ++gFailures;
        std::println("  FAIL [{}]", ref);
        std::println("    src         \"{}\"", printable(src));
        std::println("    recorded    \"{}\"", got);
        std::println("    expected    \"{}\"", expected);
    }
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
// The regression this file was written for.
//
// `const {"a-b": c} = o;` is the ONLY input that reaches binding.cppm:366, the
// call that did not resolve: a BindingObject property whose key is not a valid
// identifier takes the `else` at :365 and asks for a quoted key. Every other
// binding vector routes around it. If `print_string_literal_utf8` goes private
// or grows an underscore again, THIS is what stops compiling.
// ─────────────────────────────────────────────────────────────────────────────
void test_binding_string_key_seam() {
    const char* R { "lib.rs:5180 (binding.cppm:366 seam)" };
    check("const {\"a-b\": c} = o;", "const { \"a-b\": c } = o;\n", R);
    // The identifier-key sibling at :5170 — the arm that was ALWAYS reachable.
    // Kept adjacent so a diff between the two localises a break to the seam.
    check("const {a: b} = o;", "const { a: b } = o;\n", R);
    // `{a}` folds to shorthand (:5175) — the third arm of the same match.
    check("const {a} = o;", "const { a } = o;\n", R);
}

// ─────────────────────────────────────────────────────────────────────────────
// Cross-shard seams, one vector each. These are here to force INSTANTIATION of
// the real mixins against each other — the byte-level rules they exercise are
// pinned in depth by the per-shard suites.
// ─────────────────────────────────────────────────────────────────────────────

// StmtPrinter -> BindingPrinter -> ExprPrinter: print_decls (stmt.cppm:264)
// calls print_binding, which calls print_expr for a default.
void test_stmt_binding_expr_chain() {
    const char* R { "lib.rs:5052 print_binding <- print_decls" };
    check("const [x = 1] = a;", "const [x = 1] = a;\n", R);
    check("const {a, ...rest} = o;", "const { a, ...rest } = o;\n", R);
    check("const {[k]: v} = o;", "const { [k]: v } = o;\n", R);
}

// StmtPrinter -> ExprPrinter. `print_expr`'s EIndex arm (:3707) is the OTHER
// caller of print_string_literal_utf8 — the one internal to ExprPrinter, which
// is why it kept compiling while the binding seam was broken.
void test_stmt_expr_chain() {
    const char* R { "lib.rs:3195 print_expr <- print_stmt" };
    check("a[\"a-b\"];", "a[\"a-b\"];\n", R);  // :3704-3708 — the EIndex quoted arm
    check("a.b;", "a.b;\n", R);                // :3691 — the identifier-name arm
    check("a + b * c;", "a + b * c;\n", R);    // precedence, expr_op.cppm
    check("x = 1;", "x = 1;\n", R);
}

// ─────────────────────────────────────────────────────────────────────────────
// ref :3926 EObject. Only reachable on the real Printer: the arm calls
// print_property, which calls print_func, which calls print_binding — four
// mixins deep. Every expectation is real bun 1.3.14 output.
// ─────────────────────────────────────────────────────────────────────────────
void test_object_literal() {
    const char* R { "lib.rs:3926 EObject" };
    check("const o = {a: 1};", "const o = { a: 1 };\n", R);
    check("const o = {a: 1, b: 2};", "const o = { a: 1, b: 2 };\n", R);
    // ref :3941 — an empty object skips the padding block entirely.
    check("const o = {};", "const o = {};\n", R);
    check("const o = {a};", "const o = { a };\n", R);      // :4921 shorthand fold
    check("const o = {...x};", "const o = { ...x };\n", R);  // :4759 spread
    check("const o = {[k]: 1};", "const o = { [k]: 1 };\n", R);
    check("const o = {\"a-b\": 1};", "const o = { \"a-b\": 1 };\n", R);
    check("const o = {m(){}};", "const o = { m() {} };\n", R);
    check("const o = {a: 1, m(){}, get g(){}};", "const o = { a: 1, m() {}, get g() {} };\n", R);
    // ref e.rs:1233 is_single_line — a newline anywhere inside flips the WHOLE
    // layout to one property per indented line. Same tree, different flag.
    check("const o = {\n a: 1,\n b: 2\n};", "const o = {\n  a: 1,\n  b: 2\n};\n", R);
    check("const o = {\n a: 1\n};", "const o = {\n  a: 1\n};\n", R);
}

// ref :3927-3934 — `wrap`. The reason stmtStart/arrowExprStart exist: `{` at a
// statement start is a BLOCK, so an object there must parenthesise itself. bun
// tests it by comparing written() against the offset, not with a flag.
void test_object_literal_stmt_start_wrap() {
    const char* R { "lib.rs:3927 EObject wrap" };
    check("({a: 1}).b;", "({ a: 1 }).b;\n", R);   // bun 1.3.14: "({ a: 1 }).b;"
    // The control: same literal, NOT at a statement start -> no parens.
    check("x = {a: 1};", "x = { a: 1 };\n", R);   // bun 1.3.14: "x = { a: 1 };"
    check("const o = {a: 1}[\"a\"];", "const o = { a: 1 }[\"a\"];\n", R);
}

// ref :2527 print_func / :5307 the S::Function arm. Neither existed before
// func.cppm — `Printer::print_stmt` could not be instantiated at all.
void test_function_decl() {
    const char* R { "lib.rs:5307 S::Function + :2527 print_func" };
    check("function f(){}", "function f() {}\n", R);
    check("function* g(){}", "function* g() {}\n", R);        // :5326 `*` + space
    check("function r(...a){}", "function r(...a) {}\n", R);  // :2510 has_rest_arg
    check("function d(a = 1){}", "function d(a = 1) {}\n", R);  // :2517 default
    check("function m(a, b){ return a; }", "function m(a, b) {\n  return a;\n}\n", R);
}

// ⚠️ KNOWN GAP, pinned so it cannot rot silently — a PARSER gap, not a printer
// one, and NOT this change's to fix.
//
// `async function h(){}` in STATEMENT position is a declaration; bun 1.3.14
// prints `async function h() {}`. mbun parses it as an EXPRESSION statement: the
// statement dispatcher (js_parser.cppm parse_statement_) has no `async function`
// arm, so `async` falls through to parse_primary_, which builds a FunctionExpr.
// print_expr's EFunction arm is still DEFERRED, so it echoes the source span —
// and the span starts at `function`, so even the `async` is lost.
//
// The parser already HAS the right lookahead, in the right shape, in
// parse_statement_or_async_function_ (`ident_is_("async") && peek_kind_(1) ==
// Token::Function && !newlineBefore`) — but only the two `export` paths call it.
// Routing the statement dispatcher through it is a one-word change and looks
// obviously right, which is exactly why it is NOT bundled in here: it flips the
// node's KIND from ExpressionStmt to FunctionDecl, and js_parser.cppm's CJS
// export lowering branches on that kind (`ik == NodeKind::FunctionDecl`), so it
// changes hoisting for `async function` at top level. That is a transpile-path
// change with its own corpus verification to do, not a printer test's business.
//
// print_function_decl's IsAsync arm (ref :5322) is therefore correct but
// unreachable from this dispatcher today; it lights up the moment the parser
// builds the declaration.
void test_async_function_decl_known_gap() {
    const char* R { "KNOWN GAP: parse_statement_ has no `async function` arm" };
    // bun 1.3.14: "async function h() {}\n"
    check("async function h(){}", "function h(){};\n", R);
}

// ref :2543 print_class / :5361 the S::Class arm.
void test_class_decl() {
    const char* R { "lib.rs:5361 S::Class + :2543 print_class" };
    check("class C {}", "class C {\n}\n", R);
    check("class C extends B {}", "class C extends B {\n}\n", R);  // :2544 heritage
    check("class C { m(){} }", "class C {\n  m() {}\n}\n", R);
    // ref :2577 — a field has no VALUE, so it is a statement and takes a `;`. A
    // method has one and is terminated by its own `}`. The absence IS the signal.
    check("class C { x = 1 }", "class C {\n  x = 1;\n}\n", R);
    check("class C { x }", "class C {\n  x;\n}\n", R);
    check("class C { static m(){} }", "class C {\n  static m() {}\n}\n", R);
    check("class C { get a(){} }", "class C {\n  get a() {}\n}\n", R);
    check("class C { #p = 1 }", "class C {\n  #p = 1;\n}\n", R);
    // ref :2561 — a `static {}` block. mbun's parser returns a plain Block node
    // here, never a Property, so print_class selects the arm on the node's KIND
    // where bun selects on the property's kind (see func.cppm's note).
    check("class C { static { x = 1 } }", "class C {\n  static {\n    x = 1;\n  }\n}\n", R);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// `export <declaration>` — ref lib.rs:6748 / :5318 / :5372.
//
// The one module form printable from mbun's arena: it is the only one whose
// parser arm stores anything (js_parser.cppm:1561 `a = inner`). Everything else
// — all of `import`, `export {…}`, `export *`, `export default` — is a payloadless
// span marker and prints "" by design; see the MODULE-SYNTAX AST-GAP note in
// stmt.cppm's header. Those are NOT pinned here as `""` expectations: "" is the
// gap, not the contract, and pinning it would cement the bug.
//
// Expectations are real bun 1.4.0 — `.mbun/bin/bun-rust`, which IS the blueprint
// version (.mbun/bun-ref/package.json), so unlike the 1.3.14 vectors above there
// is no cross-read and no version disagreement to adjudicate.
// ─────────────────────────────────────────────────────────────────────────────
void test_export_decl() {
    const char* R { "lib.rs:6748 S::Local is_export" };
    check("export const x = 1;", "export const x = 1;\n", R);
    check("export let y = 2, z = 3;", "export let y = 2, z = 3;\n", R);
    check("export var v = 4;", "export var v = 4;\n", R);
    // The binding printer under an export — `export ` must not disturb it.
    check("export const {a, b: c} = o;", "export const { a, b: c } = o;\n", R);

    const char* RF { "lib.rs:5318 S::Function is_export" };
    check("export function f(){}", "export function f() {}\n", RF);
    check("export function* g(){}", "export function* g() {}\n", RF);
    // `export async function` reaches a FunctionDecl even though a BARE
    // `async function` does not: the export arm parses its declaration with
    // js_parser.cppm's parse_statement_or_async_function_ (:1506), which has the
    // `async` arm the plain statement dispatcher lacks (see the KNOWN GAP note
    // above). So this prints where its unexported sibling still cannot.
    check("export async function af(){}", "export async function af() {}\n", RF);

    const char* RC { "lib.rs:5372 S::Class is_export" };
    check("export class C {}", "export class C {\n}\n", RC);
    check("export class D extends B {}", "export class D extends B {\n}\n", RC);

    // TS-only declarations: bun erases them whole (verified — both transformSync
    // to "" on bun 1.4.0), and so does this arm. It drops them because the node
    // they wrap (InterfaceDecl / TypeAliasDecl) is unported, where bun drops them
    // by rule; different route, same bytes, and the bytes are the contract.
    const char* RT { "lib.rs — TS erasure (bun 1.4.0 => \"\")" };
    check("export interface I { x: number }", "", RT);
    check("export type T = number;", "", RT);
}

// ✅ CLOSED — was "KNOWN GAP: `declare` is an edit, invisible to the AST".
//
// The gap: `declare X` is type-only and bun 1.4.0 erases it to "", but
// js_parser.cppm's declare arm parsed the inner statement, recorded ONE outer
// deletion over the span, and then RETURNED `inner` UNCHANGED. The `declare`
// therefore lived only in the edit list, and the node that landed in the arena
// was an ordinary VarDecl — byte-identical in shape to the one `const dc;`
// produces. Nothing in the AST could tell them apart, so the AST-rebuild printer
// printed the declaration back out.
//
// The fix is the one this comment used to ASK for, verbatim: "a type-only
// declaration must either not reach the arena or must carry a flag saying so".
// It does not reach the arena any more. The declare arm returns a
// `TypeScriptStmt` (ast.cppm) and discards `inner`, which is a 1:1 port of what
// bun already did — `return Ok(Some(p.s(S::TypeScript {}, loc)))`
// (.mbun/bun-ref/src/js_parser/parse/parse_stmt.rs:1882). bun then drops
// S::TypeScript in its visit pass (visit/visit_stmt.rs:76-79); mbun has no visit
// pass, so print_stmt erases it instead. Same answer, same route.
//
// Note what the OLD expectation was: `const dc;\n` — invalid TS erasure, pinned
// as correct-for-now. It was honest about being a gap, but a passing test whose
// expectation is the bug is still a test that goes green when the code is wrong.
// It is now bun's actual bytes.
//
// How this surfaced is worth keeping: closing print_stmt's `default:` made
// `declare namespace N{}` / `declare enum E{}` start REPORTING as unported —
// they reach the arena as NamespaceDecl/EnumDecl by the same mechanism. Two
// corpus files (compat/bun/test/js/compat/bun/test/jest.d.ts and expect-extend.types.d.ts) had
// been AGREEING with bun byte-for-byte purely because the printer dropped their
// `declare module` silently and "" happened to be bun's answer too — two bugs
// cancelling. The sink turned that accident into a visible failure, which is
// what a failure channel is for.
// ─────────────────────────────────────────────────────────────────────────────
// The failure channel — print_stmt's `default:` swallow, closed.
//
// These vectors are about the DIFFERENCE between "printed nothing because that
// is bun's answer" and "printed nothing because there is no port". Until
// js_printer/unsupported.cppm existed those two were the same observable, which
// is why `enum E{A} export const v=E.A` could print `export const v = E.A;` — an
// `E` that is never declared — and still report ok.
//
// ⚠️ NOT a port of anything: bun has no failure channel because bun has no
// unported kinds. SEnum/SNamespace never reach its printer at all; they are
// lowered in the visit pass (visit/visit_stmt.rs:89/:120 -> p.rs:6415
// `generate_closure_for_type_script_namespace_or_enum`). mbun has no visit pass,
// so these kinds arrive at print_stmt and it must say so.
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// `export {…}` (no `from`) specifier filtering — js_parser/module_scope.cppm.
//
// A specifier survives iff its local name is a MODULE-SCOPE VALUE binding
// (ref visit/visit_stmt.rs:227 `symbol.kind == Unbound -> continue`).
//
// EVERY expectation below is the CLAUSE bun 1.4.0 actually emits, taken from
// `Bun.Transpiler({loader:"ts"}).transformSync` on .mbun/bin/bun-rust — not from
// reasoning about what it ought to do. Only the `export {…}` statement is
// asserted here; the surrounding statements are printed by arms this file already
// covers, and several of them (enum/namespace lowering, `var` hoisting) are known
// unported gaps that are NOT this pass's business.
// ─────────────────────────────────────────────────────────────────────────────
void test_export_clause_filtering() {
    const char* R { "module_scope.cppm — export{} scope filter (vs bun 1.4.0)" };
    // The `export {…}` STATEMENT only. It is not always last — `export {a}; let
    // a=1` (the order-independence vector) prints it first — so this takes the one
    // line that starts the clause rather than the tail of the output. Every vector
    // below keeps its clause on a single line, which is bun's layout whenever the
    // source braces held no newline (ast.cppm mflags::IsSingleLine).
    const auto clause_of = [](std::string_view src) {
        const std::string all { print_src_filtered(src) };
        for (std::size_t b = 0; b < all.size();) {
            const std::size_t e { all.find('\n', b) };
            const std::string line { all.substr(b, e == std::string::npos ? e : e - b) };
            if (line.starts_with("export {")) {
                return line;
            }
            if (e == std::string::npos) {
                break;
            }
            b = e + 1;
        }
        return std::string { "<no export clause>" };
    };
    const auto ck = [&](std::string_view src, std::string_view want) {
        ++gChecks;
        const std::string got { clause_of(src) };
        if (got != want) {
            ++gFailures;
            std::println("  FAIL [{}]", R);
            std::println("    src      \"{}\"", printable(src));
            std::println("    got      \"{}\"", printable(got));
            std::println("    expected \"{}\"", printable(want));
        }
    };

    // ── KEPT: every module-scope value binding form ──────────────────────────
    ck("let a=1; export {a}", "export { a };");
    ck("const a=1; export {a}", "export { a };");
    ck("var a=1; export {a}", "export { a };");
    ck("function a(){} export {a}", "export { a };");
    ck("class a{} export {a}", "export { a };");
    ck("enum E{X} export {E}", "export { E };");            // enum lowers to a value
    ck("const enum E{X} export {E}", "export { E };");
    ck("namespace N{export const x=1} export {N}", "export { N };");
    ck("import {a} from 'y'; export {a}", "export { a };");
    ck("import a from 'y'; export {a}", "export { a };");
    ck("import * as a from 'y'; export {a}", "export { a };");
    // Destructuring binds every LEAF (keys and computed keys bind nothing).
    ck("let {a}=o; export {a}", "export { a };");
    ck("let {x:{a}}=o; export {a}", "export { a };");
    ck("let {...a}=o; export {a}", "export { a };");
    ck("let [a]=o; export {a}", "export { a };");
    // Order independence — the whole reason this is a pre-pass and not a print test.
    ck("export {a}; let a=1", "export { a };");
    // `var` hoists out of every non-function scope; `let`/`const` do not.
    ck("{ var a=1; } export {a}", "export { a };");
    ck("for(var a of x){} export {a}", "export { a };");
    ck("if(x) var a=1; export {a}", "export { a };");
    ck("switch(x){case 1: var a=1;} export {a}", "export { a };");
    ck("l: { var a=1; } export {a}", "export { a };");
    ck("while(x){ var a=1; } export {a}", "export { a };");
    ck("try{ var a=1; }catch(e){} export {a}", "export { a };");
    // Aliases print `L as R`; the LOCAL side (L) is what must resolve.
    ck("let a=1; export {a as b}", "export { a as b };");
    ck("let a=1; export {a as default}", "export { a as default };");
    // Partial survival — the compaction keeps order.
    ck("let a=1; export {a, b as c}", "export { a };");
    ck("let a=1,b=2; export {b, a}", "export { b, a };");
    // Merged/shadowed declarations still resolve to the value.
    ck("interface a{} const a=1; export {a}", "export { a };");
    ck("let a=1; { let a=2; } export {a}", "export { a };");
    ck("function a(): void; function a(){} export {a}", "export { a };");

    // ── DROPPED: an emptied clause still prints `export {};` (ref lib.rs:5653,
    //    the empty-clause arm). It is what marks the file an ES module.
    ck("export {a}", "export {};");
    ck("export {a, b as c}", "export {};");
    ck("export {}", "export {};");            // empty in SOURCE — same bytes
    ck("{ let a=1; } export {a}", "export {};");           // block-scoped
    ck("for(let a of x){} export {a}", "export {};");
    ck("function f(){ var a=1; } export {a}", "export {};");  // function scope stops it
    ck("const f=()=>{ var a=1; }; export {a}", "export {};");
    ck("{ function a(){} } export {a}", "export {};");     // bun: block fn -> `let a = fn`
    ck("try{}catch(a){} export {a}", "export {};");        // catch param
    ck("function f(a){} export {a}", "export {};");        // param
    ck("export {globalThis}", "export {};");               // an unbound GLOBAL is not a binding
    // TS type-only decls bind no value. `declare` reaches the printer as a
    // TypeScriptStmt (parser, ref parse/parse_stmt.rs:1882).
    ck("type a = number; export {a}", "export {};");
    ck("interface a {} export {a}", "export {};");
    ck("declare const a: number; export {a}", "export {};");
    ck("declare enum E{X} export {E}", "export {};");
    ck("declare function a(): void; export {a}", "export {};");
    ck("import type {a} from 'y'; export {a}", "export {};");
    ck("import {type a} from 'y'; export {a}", "export {};");

    // ── `export {…} from 'y'` is EXEMPT: re-export names are the OTHER module's,
    //    never local refs, so no resolution happens and nothing is dropped.
    ck("export {a} from 'y'", "export { a } from \"y\";");
    ck("export {zzz as q} from 'y'", "export { zzz as q } from \"y\";");
    // ⚠️ bun really does emit TWO spaces here: S::ExportFrom (lib.rs:5758) has no
    // empty-clause arm, so its two print_space()s land either side of nothing.
    ck("export {} from 'y'", "export {  } from \"y\";");

    // ── The whole statement VANISHES when every specifier was inline-`type` and
    //    the clause is left empty — S::TypeScript, not `export {};`
    //    (ref parse/parse_stmt.rs:1322 bare, :1261 with `from`). Distinct from a
    //    clause that merely filters to nothing, which still prints `export {};`.
    ck("let a=1; export {type a}", "<no export clause>");
    ck("export {type a}", "<no export clause>");
    ck("export {type a} from 'y'", "<no export clause>");
    // …but ONE surviving value specifier keeps the statement.
    ck("let a=1,b=2; export {type a, b}", "export { b };");
}

void test_unsupported_is_reported() {
    const char* R { "unsupported.cppm — no silent drop" };

    // TS enum / namespace: unported, because the port is a VISIT PASS.
    check_unsupported("enum E{A}", "EnumDecl", R);
    check_unsupported("const enum E{A=1}", "EnumDecl", R);
    check_unsupported("export enum E{A,B}", "EnumDecl", R);
    check_unsupported("namespace N{export const x=1}", "NamespaceDecl", R);
    check_unsupported("export namespace N{export const x=1}", "NamespaceDecl", R);
    check_unsupported("namespace A{export namespace B{export const x=1}}", "NamespaceDecl", R);

    // The vector from the handoff. The enum is unported, so the file FAILS —
    // rather than emitting `export const v = E.A;` against an undeclared `E` and
    // reporting success, which is what HEAD did (verified on 03fbd610).
    check_unsupported("enum E{} export const v=E.A", "EnumDecl", R);

    // `export {…}` with no `from` is PORTED — the module-scope name resolution it
    // needs lives in js_parser/module_scope.cppm (ref visit/visit_stmt.rs:168), so
    // it must NOT be recorded. Was `"ExportDecl"` while the arm was a stub; the
    // expectation moved with the implementation, it was not relaxed — the positive
    // side is asserted byte-for-byte in test_export_clause_filtering() below.
    check_unsupported("let a=1; export {a}", "", R);
    check_unsupported("export {a}", "", R);

    // ⚠️ The other half of the contract, and the easier one to get wrong:
    // ERASURE MUST NOT BE RECORDED. Each of these prints nothing because nothing
    // is bun's actual answer, so recording them would turn correct output into a
    // spurious failure — and would put back the very conflation this change
    // removed, just with the sign flipped.
    check_unsupported("type X = number", "", R);
    check_unsupported("interface I{a:string}", "", R);
    check_unsupported("declare const x: number", "", R);
    check_unsupported("declare enum E{A}", "", R);
    check_unsupported("declare namespace N{const x:number}", "", R);
    // And ordinary JS is obviously never recorded.
    check_unsupported("const x = 1;", "", R);
    check_unsupported("function f(a){ return a; }", "", R);
}

void test_declare_erasure() {
    const char* R { "parse_stmt.rs:1882 declare -> S::TypeScript" };
    // Every expectation here is real bun 1.4.0 via
    //   new Bun.Transpiler({loader:"ts"}).transformSync(src)
    check("declare const dc: number;", "", R);
    check("export declare const dc: number;", "", R);
    // The two spellings that made the gap visible. Both erase: `declare` means
    // no runtime emit, so there is no namespace/enum closure to generate — and
    // that is why these do NOT reach print_stmt's EnumDecl/NamespaceDecl arm,
    // which reports unported.
    check("declare enum E{A}", "", R);
    check("declare namespace N{const x:number}", "", R);
    check("declare module \"m\" { const x: number; }", "", R);
}

// ─────────────────────────────────────────────────────────────────────────────
// ESM — module_syntax.cppm, through the real assembled Printer.
//
// EVERY expectation below is real bun 1.4.0 (.mbun/bin/bun-rust, the blueprint's
// own version) via `new Bun.Transpiler({loader:"ts"}).transformSync(src)`, copied
// verbatim from its output.
//
// ⚠️ If you are re-deriving these, use Bun.Transpiler and NOT `bun build
// --no-bundle`. The bundler tree-shakes unused imports (`import d from 'y'` =>
// ""), so it would "prove" that half this file should print nothing. See the
// ORACLE note in js_printer/stmt.cppm's header — two agents lost days to it.
// ─────────────────────────────────────────────────────────────────────────────
void test_import_decl() {
    const char* R { "lib.rs:6160 S::Import" };
    check("import d from 'y'", "import d from \"y\";\n", R);
    check("import {a} from 'y'", "import { a } from \"y\";\n", R);
    check("import {a as b} from 'y'", "import { a as b } from \"y\";\n", R);
    check("import * as ns from 'y'", "import * as ns from \"y\";\n", R);
    check("import d, {a} from 'y'", "import d, { a } from \"y\";\n", R);
    check("import d, * as ns from 'y'", "import d, * as ns from \"y\";\n", R);
    // `default` is a reserved word but a legal specifier name.
    check("import {default as d} from 'y'", "import { default as d } from \"y\";\n", R);
    // A trailing comma is not a specifier.
    check("import {a,} from 'y'", "import { a } from \"y\";\n", R);

    // ⚠️ NO space after the keyword. This is not a special case in the printer —
    // it falls out of `item_count == 0` skipping the whole `from ` block
    // (lib.rs:6235), leaving the path's quote hard against `import`.
    const char* RB { "lib.rs:6235 — bare import, item_count==0 skips `from `" };
    check("import 'y'", "import\"y\";\n", RB);
    // An EMPTY clause collapses to the bare form: it binds nothing, so
    // `item_count` never counts it.
    check("import {} from 'y'", "import\"y\";\n", RB);
    check("import d, {} from 'y'", "import d from \"y\";\n", RB);

    // is_single_line (s.rs:207) — ONLY newlines INSIDE the braces count.
    const char* RS { "s.rs:207 is_single_line" };
    check("import {a,\nb} from 'y'", "import {\n  a,\n  b\n} from \"y\";\n", RS);
    check("import\n{a} from 'y'", "import { a } from \"y\";\n", RS);
    check("import d\nfrom 'y'", "import d from \"y\";\n", RS);

    // A string module-export name prints via print_clause_alias (lib.rs:2478):
    // bare when it is a valid identifier, re-quoted when it is not.
    const char* RA { "lib.rs:2478 print_clause_alias" };
    check("import {'a-b' as c} from 'y'", "import { \"a-b\" as c } from \"y\";\n", RA);
    // …and it folds when the two names are equal BY CONTENT, quotes and all.
    check("import {'a' as a} from 'y'", "import { a } from \"y\";\n", RA);
    check("import {a as a} from 'y'", "import { a } from \"y\";\n", RA);

    // TS erasure — bun deletes these outright.
    const char* RT { "TS type-only import — erased whole" };
    check("import type {T} from 'y'", "", RT);
    check("import type d from 'y'", "", RT);
    check("import type * as ns from 'y'", "", RT);
    // An INLINE `type` specifier is dropped, the clause survives.
    check("import {type T, a} from 'y'", "import { a } from \"y\";\n", RT);

    // Import attributes are dropped by the transpiler (verified).
    check("import d from 'y' with { type: 'json' }", "import d from \"y\";\n",
          "lib.rs:6248 — attributes not re-emitted");
}

void test_export_module_forms() {
    const char* RS { "lib.rs:5536 S::ExportStar" };
    check("export * from 'y'", "export * from \"y\";\n", RS);
    check("export * as ns from 'y'", "export * as ns from \"y\";\n", RS);

    // A re-export is IMMUNE to the `export {…}` resolution rule: the names are
    // the other module's, never local refs. So it prints with no scope analysis.
    const char* RF { "lib.rs:5759 S::ExportFrom" };
    check("export {a} from 'y'", "export { a } from \"y\";\n", RF);
    check("export {a as b} from 'y'", "export { a as b } from \"y\";\n", RF);
    check("export {default as x} from 'y'", "export { default as x } from \"y\";\n", RF);
    check("export {default} from 'y'", "export { default } from \"y\";\n", RF);
    // The empty re-export keeps BOTH single-line spaces — note this does NOT
    // collapse the way an empty IMPORT clause does.
    check("export {} from 'y'", "export {  } from \"y\";\n", RF);
    check("export {\na} from 'y'", "export {\n  a\n} from \"y\";\n", RF);
    check("export {type T, a} from 'y'", "export { a } from \"y\";\n", RF);
    check("export type {T} from 'y'", "", RF);

    const char* RD { "lib.rs:6100 S::ExportDefault" };
    check("export default 1+2", "export default 1 + 2;\n", RD);
    check("export default function foo(){}", "export default function foo() {}\n", RD);
    check("export default class Foo {}", "export default class Foo {\n}\n", RD);
    check("export default () => {}", "export default () => {};\n", RD);

    // TS `export =` — bun emits this unconditionally, not only for a CJS target.
    check("export = foo", "module.exports = foo;\n", "lib.rs:6160 S::ExportEquals");
}

int main() {
    std::println("test_js_printer_real — the assembled Printer (no stubs)");
    test_binding_string_key_seam();
    test_stmt_binding_expr_chain();
    test_stmt_expr_chain();
    test_object_literal();
    test_object_literal_stmt_start_wrap();
    test_function_decl();
    test_class_decl();
    test_export_decl();
    test_import_decl();
    test_export_module_forms();
    test_declare_erasure();
    test_export_clause_filtering();
    test_unsupported_is_reported();
    std::println("{} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
