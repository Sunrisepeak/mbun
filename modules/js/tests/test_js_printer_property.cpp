// test_js_printer_property.cpp — print_property (lib.rs:4743) test suite.
//
// Covers mbun.js_printer.property, ported from .mbun/bun-ref/src/js_printer/lib.rs
// (:4743 print_property, :5045 print_initializer).
//
// ── Where the expectations come from ─────────────────────────────────────────
// Transcribed from the actual output of real bun 1.3.14, captured with:
//
//   bun -e 'const t = new Bun.Transpiler({loader:"js"});
//           console.log(JSON.stringify(t.transformSync(<src>)))'
//
// The captures (the property substring of each, which is what this file prints):
//   x = {a: 1}      -> "x = { a: 1 };"        x = {a}        -> "x = { a };"
//   x = {...r}      -> "x = { ...r };"        x = {[k]: v}   -> "x = { [k]: v };"
//   x = {"a-b": 1}  -> "x = { \"a-b\": 1 };"  x = {1: y}     -> "x = { 1: y };"
//   x = {a: a}      -> "x = { a };"           ({a = 1} = o)  -> "({ a = 1 } = o);"
//   x = {"a": 1}    -> "x = { a: 1 };"        <- see KNOWN DIVERGENCE below
//
// ⚠️ VERSION SKEW: the installed bun is 1.3.14, the blueprint in .mbun/bun-ref is
// 1.4.0; where they disagree the blueprint wins. Every vector below was diffed
// against the 1.4.0 blueprint by hand and the two AGREE — each byte is accounted
// for by the match arms cited on the vector (:4759 spread, :4869 computed, :4906
// the EString key, :4921 the shorthand fold, :5045 the initializer), so 1.3.14's
// output IS the spec here.
//
// ── Why this file assembles its own printer ──────────────────────────────────
// Same reason as test_js_printer_binding.cpp / test_js_printer_stmt.cpp: shard 2
// re-prints expressions, and this suite is about print_property's own bytes, not
// about print_expr's. So it assembles a Printer-shaped type whose shard-2 methods
// echo the original source slice (every key/value vector here — `1`, `v`, `k`,
// `r`, `y`, `"a-b"` — echoes back byte-identical, which is what makes the stub
// sound), and mixes in the REAL StmtPrinter, which homes arena()/node()/
// print_space_before_identifier()/add_source_mapping() (stmt.cppm:24-49).
import std;
import mbun.ast;
import mbun.js_parser;
import mbun.js_printer;

namespace {

using mbun::ast::NodeIndex;
using mbun::js_parser::parse;
using mbun::js_parser::ParseResult;
using mbun::js_printer::ExprFlagSet;
using mbun::js_printer::Level;
using mbun::js_printer::ModulePrinter;
using mbun::js_printer::Options;
using mbun::js_printer::UnsupportedSink;
using mbun::js_printer::PrinterCore;
using mbun::js_printer::PrinterFlags;
using mbun::js_printer::BindingPrinter;
using mbun::js_printer::FuncPrinter;
using mbun::js_printer::PropertyPrinter;
using mbun::js_printer::StmtPrinter;
using mbun::js_printer::StringSink;

int gChecks { 0 };
int gFailures { 0 };

std::string printable(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Real PrinterCore + real StmtPrinter + real PropertyPrinter + real FuncPrinter
// + real BindingPrinter, shard 2 (print_expr / print_identifier) stubbed.
//
// FuncPrinter and BindingPrinter are mixed in because print_property REACHES
// them: a method shorthand is `print_func`'d (lib.rs:4880/:5017), and print_func
// print_binding's each of its args (:2515). Stubbing print_func would make these
// vectors green against the stub rather than against the port — the exact false
// green that hid the print_string_literal_utf8 seam (see
// tests/test_js_printer_real.cpp's header). print_expr stays a source-echo stub
// so the vectors stay independent of shard 2's requoting/folding.
// ─────────────────────────────────────────────────────────────────────────────
template <PrinterFlags F>
class StubPrinter
    : public PrinterCore<StubPrinter<F>, F, StringSink>
    , public StmtPrinter<StubPrinter<F>>
    , public BindingPrinter<StubPrinter<F>>
    , public FuncPrinter<StubPrinter<F>>
    , public PropertyPrinter<StubPrinter<F>>
    // print_stmt's ESM arms delegate to module_syntax.cppm, so any stub that
    // mixes in StmtPrinter needs it too — it is real code, not a shard stub.
    , public ModulePrinter<StubPrinter<F>>
    // print_stmt records unported kinds (js_printer/unsupported.cppm) rather
    // than dropping them, so the sink is part of the code under test.
    , public UnsupportedSink {
private:
    using Core = PrinterCore<StubPrinter<F>, F, StringSink>;

    void echo_(NodeIndex i) {
        const mbun::ast::Node& n { this->node(i) };
        const std::string_view text { source.substr(n.start, n.end - n.start) };
        if (!text.empty()
            && mbun::js_printer::is_identifier_part_byte(static_cast<std::uint8_t>(text.front()))) {
            this->print_space_before_identifier();
        }
        this->print(text);
    }

public:
    // PropertyPrinter reads `self_().source` — shard 2 owns it on the real
    // Printer (expr.cppm:209, a public member); the stub provides the same name.
    std::string_view source {};

    StubPrinter(StringSink writer, std::string_view src, Options opts = {})
        : Core { writer, std::move(opts) }
        , source { src } { }

    // ── shard 2 stubs ────────────────────────────────────────────────────────
    void print_expr(NodeIndex e, Level, ExprFlagSet) { echo_(e); }
    void print_identifier(std::string_view name) { this->print(name); }
    // BindingPrinter's seam into shard 2 (binding.cppm:366) — the real one lives
    // on ExprPrinter (expr.cppm), which this harness does not mix in.
    void print_string_literal_utf8(std::string_view text, bool) {
        this->print('"');
        this->print(text);
        this->print('"');
    }
};

using Plain = StubPrinter<PrinterFlags {}>;

// Parse `({...})` and print property #i back through the REAL PropertyPrinter.
std::string print_prop(std::string_view src, std::size_t i = 0, Options opts = {}) {
    ParseResult r { parse(src) };
    if (!r.ok) {
        return "<parse error: " + r.error + ">";
    }
    // ExpressionStmt -> Paren -> ObjectLiteral (a `{` in statement position is a
    // Block, so the object literal is reached through a Paren).
    auto stmts = r.arena.list_of(r.arena.at(r.program));
    if (stmts.size() != 1) {
        return "<not one stmt>";
    }
    NodeIndex e = r.arena.at(stmts[0]).a;
    if (e == mbun::ast::NONE || r.arena.at(e).kind != mbun::ast::NodeKind::Paren) {
        return "<no paren>";
    }
    NodeIndex obj = r.arena.at(e).a;
    // `({a = 1} = o)`: the parens wrap the ASSIGNMENT, so unwrap it to the LHS.
    if (obj != mbun::ast::NONE && r.arena.at(obj).kind == mbun::ast::NodeKind::Assignment) {
        obj = r.arena.at(obj).a;
    }
    if (obj == mbun::ast::NONE || r.arena.at(obj).kind != mbun::ast::NodeKind::ObjectLiteral) {
        return "<no object literal>";
    }
    auto props = r.arena.list_of(r.arena.at(obj));
    if (i >= props.size()) {
        return "<no such property>";
    }
    std::string out;
    Plain p { StringSink { out }, src, std::move(opts) };
    p.bind_arena(r.arena);
    p.print_property(props[i]);
    return out;
}

void check(std::string_view src, std::string_view expected, std::string_view ref,
           std::size_t i = 0) {
    ++gChecks;
    const std::string got { print_prop(src, i) };
    if (got != expected) {
        ++gFailures;
        std::println("  FAIL [{}]", ref);
        std::println("    src      \"{}\"", printable(src));
        std::println("    got      \"{}\"", printable(got));
        std::println("    expected \"{}\"", printable(expected));
    }
}

// ── vector groups ────────────────────────────────────────────────────────────

// The plain `key: value` tail — ref :5021-5033.
void test_plain_properties() {
    const char* R { "lib.rs:5021 key: value" };
    check("({a: 1})", "a: 1", R);          // bun: "x = { a: 1 };"
    check("({a: b})", "a: b", R);
    // A key that is not a valid identifier is quoted — ref :4917.
    check("({\"a-b\": 1})", "\"a-b\": 1", R);  // bun: "x = { \"a-b\": 1 };"
    // A numeric key takes the `_ =>` arm (:5000) and prints as an expression.
    check("({1: y})", "1: y", R);          // bun: "x = { 1: y };"
}

// ref :4921 — "Use a shorthand property if the names are the same".
void test_shorthand() {
    const char* R { "lib.rs:4921 shorthand fold" };
    check("({a})", "a", R);      // bun: "x = { a };"
    // ⚠️ The FOLD: bun re-prints `{a: a}` as `{a}` by comparing the key text to
    // the value's name — NOT by trusting the parser's WasShorthand flag (which is
    // false here). Getting this backwards is invisible in `{a}` and only shows up
    // in this vector.
    check("({a: a})", "a", R);   // bun: "x = { a };"
    // The control: names differ, so no fold.
    check("({a: b})", "a: b", R);
}

// ref :4759 — the Spread arm.
void test_spread() {
    const char* R { "lib.rs:4759 Spread" };
    check("({...r})", "...r", R);            // bun: "x = { ...r };"
    check("({...a.b})", "...a.b", R);
    // Spread is a KIND, not a flag, and it is keyless (g.rs:158) — so it returns
    // before any of the key machinery runs.
    check("({a: 1, ...r})", "...r", R, 1);
}

// ref :4869 — the computed-key arm.
void test_computed() {
    const char* R { "lib.rs:4869 IsComputed" };
    check("({[k]: v})", "[k]: v", R);        // bun: "x = { [k]: v };"
    check("({[a.b]: v})", "[a.b]: v", R);
}

// ref :5045 — print_initializer, reached from the shorthand-with-default form.
void test_initializer() {
    const char* R { "lib.rs:5045 print_initializer" };
    check("({a = 1} = o)", "a = 1", R);      // bun: "({ a = 1 } = o);"
    check("({a = b} = o)", "a = b", R);
}

// ref :4816-4838 — the Get/Set/AutoAccessor prefixes.
//
// ⚠️ These two were a pinned KNOWN GAP (`get a` with no body) for as long as the
// parser built no value for a method shorthand and no shard defined print_func.
// Both have landed, so they now pin bun's real bytes. The gap is closed, not
// weakened — the expectations went UP, to what bun 1.3.14 actually prints.
void test_accessor_prefixes() {
    const char* R { "lib.rs:4816 Get/Set prefix" };
    check("({get a(){}})", "get a() {}", R);   // bun 1.3.14: "{ get a() {} }"
    check("({set a(v){}})", "set a(v) {}", R);  // bun 1.3.14: "{ set a(v) {} }"
    // The control that proves the prefix is driven by `kind` and not by the
    // spelling of the key: a property KEYED `get` must NOT gain a prefix.
    check("({get: 1})", "get: 1", R);        // bun: "x = { get: 1 };"
}

// ref :4838-4853 — a method's `async`/`*`, read off the VALUE function's FnFlags
// (g.rs:285), never off the Property: bun has no `flags::Property::IsAsync`.
//
// ⚠️ The space rule is counter-intuitive and is bun's: `async` and `*` each print
// with no trailing space, and a space is printed only when BOTH are present
// (:4847-4851) — so `{async *ag(){}}` prints `async* ag`, not `async *ag`. Every
// expectation below is real bun 1.3.14 output, and each is accounted for
// line-by-line by the 1.4.0 blueprint arm cited; the two agree.
void test_method_shorthand() {
    const char* R { "lib.rs:4838 method async/generator + :4880 print_func" };
    check("({m(){}})", "m() {}", R);                  // bun: "{ m() {} }"
    check("({async b(){}})", "async b() {}", R);      // bun: "{ async b() {} }"
    check("({*g(){}})", "*g() {}", R);                // bun: "{ *g() {} }"
    check("({async *ag(){}})", "async* ag() {}", R);  // bun: "{ async* ag() {} }"
    // ref :2487 print_fn_args — separators, rest, defaults.
    check("({m(a,b){return a}})", "m(a, b) {\n  return a;\n}", R);
    check("({m(...r){}})", "m(...r) {}", R);          // :2510 has_rest_arg
    check("({m(a=1){}})", "m(a = 1) {}", R);          // :2517 the ws!(" = ")
    check("({m(a,...r){}})", "m(a, ...r) {}", R);
    // A computed-key method — ref :4880-4886, the arm that print_func's and
    // RETURNS rather than printing `:` + value.
    check("({[k](){}})", "[k]() {}", R);              // bun: "{ [k]() {} }"
    // The control: a non-method value keyed the same way still prints `key: value`.
    check("({m: f})", "m: f", R);
}

// ⚠️ KNOWN DIVERGENCE from bun, recorded as a test so it cannot rot silently.
//
// bun prints `x = {"a": 1}` as `x = { a: 1 };` — it UNQUOTES a string key that is
// a valid identifier, because in bun BOTH `{a: 1}` and `{"a": 1}` have an EString
// key and :4911 asks `lexer::is_identifier(key_str)` of it.
//
// mbun splits bun's EString across two node kinds: an identifier-like key is a
// bare Identifier, a quoted key is a StringLiteral whose only text is its RAW
// source slice — quotes and undecoded escapes included (expr.cppm:47 records the
// same gap for string values). Asking is_identifier of `"a"` is meaningless, and
// stripping the quotes by hand would be wrong the moment the key holds an escape
// (`{"\x61": 1}`), so the StringLiteral key takes the `_ =>` expression arm
// (:5000) and stays quoted. That is a LOSSLESS answer, just not bun's bytes.
//
// The unblock is a DECODED key value, which is exactly what the string pool
// landing in ast.cppm (add_string/string_value) provides: once a StringLiteral
// key carries its decoded text, this arm merges into the Identifier arm above and
// the vector below flips to bun's `a: 1`.
void test_known_divergence_quoted_identifier_key() {
    const char* R { "lib.rs:4911 is_identifier(EString) — needs decoded key text" };
    ++gChecks;
    const std::string got { print_prop("({\"a\": 1})") };
    const std::string_view mbunToday { "\"a\": 1" };
    const std::string_view bunSpec { "a: 1" };
    if (got != mbunToday) {
        ++gFailures;
        std::println("  FAIL [{}] known-divergence vector moved", R);
        std::println("    got      \"{}\"", printable(got));
        std::println("    expected \"{}\" (mbun today; bun says \"{}\")", printable(mbunToday),
                     printable(bunSpec));
    }
}

}  // namespace

int main() {
    test_plain_properties();
    test_shorthand();
    test_spread();
    test_computed();
    test_initializer();
    test_accessor_prefixes();
    test_method_shorthand();
    test_known_divergence_quoted_identifier_key();

    std::println("test_js_printer_property: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
