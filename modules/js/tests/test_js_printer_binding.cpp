// test_js_printer_binding.cpp — CAP-BUILD-PRINTER shard 4/4 test suite.
//
// Covers the binding (destructuring pattern) printer ported from
// .mbun/bun-ref/src/js_printer/lib.rs (:5052 `print_binding`, :5280
// `maybe_print_default_binding_value`) into mbun.js_printer.binding.
//
// ── Where the expectations come from ─────────────────────────────────────────
// Transcribed from the actual output of real bun 1.3.14, captured with:
//
//   bun -e 'const t = new Bun.Transpiler({loader:"js"});
//           console.log(JSON.stringify(t.transformSync(<src>)))'
//
// ⚠️ VERSION SKEW: the installed bun is 1.3.14, the blueprint in .mbun/bun-ref is
// 1.4.0. Where they disagree the blueprint wins. Every vector below was diffed
// against the 1.4.0 blueprint by hand and the two agree: the `print_binding`
// match arms cited above account for each byte of each expectation (the padding
// asymmetry, the trailing-hole comma, and the single-line rule are all read
// straight off :5079-5275), so 1.3.14's output IS the spec here.
//
// ── Why this file assembles its own printer ──────────────────────────────────
// Same reason as test_js_printer_stmt.cpp: `Printer` mixes in every shard and
// shard 2 (`print_expr`) has not landed. BindingPrinter is a CRTP mixin, so this
// file assembles a Printer-shaped type whose shard-2 methods are stubs echoing
// the original source slice — see printer_core.cppm:53-62. It mixes in the REAL
// StmtPrinter (shard 3) rather than stubbing it, because shard 3 is what calls
// print_binding (`print_decls`, stmt.cppm:264) and owns `arena()` / `node()` /
// `print_space_before_identifier` / `add_source_mapping` (stmt.cppm:24-49). So
// these vectors also pin that the two mixins compose.
//
// Vector selection: every default/key expression here prints back byte-identical
// to its source text (`1`, `o`, `k`, `[]`, `"a-b"`, `0`), which is what makes the
// echo stub sound. Anything shard 2 would re-print (requoting, folding) is not a
// vector here — that is shard 2's test's job.
import std;
import mbun.ast;
import mbun.js_parser;
import mbun.js_printer;

namespace {

using mbun::ast::NodeIndex;
using mbun::js_parser::parse;
using mbun::js_parser::ParseResult;
using mbun::js_printer::BindingPrinter;
using mbun::js_printer::ExprFlagSet;
using mbun::js_printer::IsTopLevel;
using mbun::js_printer::Level;
using mbun::js_printer::ModulePrinter;
using mbun::js_printer::Options;
using mbun::js_printer::UnsupportedSink;
using mbun::js_printer::PrinterCore;
using mbun::js_printer::PrinterFlags;
using mbun::js_printer::StmtPrinter;
using mbun::js_printer::StringSink;
using mbun::js_printer::TopLevel;
using mbun::js_printer::TopLevelAndIsExport;

int gChecks { 0 };
int gFailures { 0 };

// Render control characters so a diff is readable.
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

// ─────────────────────────────────────────────────────────────────────────────
// The shard-4 assembly: real PrinterCore + real StmtPrinter + real
// BindingPrinter, with shard 2's methods stubbed.
// ─────────────────────────────────────────────────────────────────────────────
template <PrinterFlags F>
class StubPrinter
    : public PrinterCore<StubPrinter<F>, F, StringSink>
    , public StmtPrinter<StubPrinter<F>>
    , public BindingPrinter<StubPrinter<F>>
    // print_stmt's ESM arms delegate to module_syntax.cppm, so any stub that
    // mixes in StmtPrinter needs it too — it is real code, not a shard stub.
    , public ModulePrinter<StubPrinter<F>>
    // print_stmt records unported kinds (js_printer/unsupported.cppm) rather
    // than dropping them, so the sink is part of the code under test.
    , public UnsupportedSink {
private:
    using Core = PrinterCore<StubPrinter<F>, F, StringSink>;

    std::string_view src_;

    // Echo a node's original source bytes — stands in for shard 2's print_expr.
    // The print_space_before_identifier call mirrors bun's EIdentifier arm
    // (lib.rs:4238); see test_js_printer_stmt.cpp's echo_ for why it matters.
    void echo_(NodeIndex i) {
        const mbun::ast::Node& n { this->node(i) };
        const std::string_view text { src_.substr(n.start, n.end - n.start) };
        if (!text.empty()
            && mbun::js_printer::is_identifier_part_byte(static_cast<std::uint8_t>(text.front()))) {
            this->print_space_before_identifier();
        }
        this->print(text);
    }

public:
    StubPrinter(StringSink writer, std::string_view src, Options opts = {})
        : Core { writer, std::move(opts) }
        , src_ { src } { }

    // ── shard 2 stubs ────────────────────────────────────────────────────────
    void print_expr(NodeIndex e, Level, ExprFlagSet) { echo_(e); }
    void print_function_decl(NodeIndex, TopLevel) { this->print("<function-decl:shard-2>"); }
    void print_class_decl(NodeIndex, TopLevel) { this->print("<class-decl:shard-2>"); }
    void print_identifier(std::string_view name) { this->print(name); }
    void print_string_literal_utf8(std::string_view text, bool) {
        this->print('"');
        this->print(text);
        this->print('"');
    }
};

using Plain = StubPrinter<PrinterFlags {}>;

// Parse `src` and print it back through the real StmtPrinter + BindingPrinter.
std::string print_src(std::string_view src, Options opts = {}) {
    ParseResult r { parse(src) };
    if (!r.ok) {
        return "<parse error: " + r.error + ">";
    }
    std::string out;
    Plain p { StringSink { out }, src, std::move(opts) };
    p.bind_arena(r.arena);
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

// ── vector groups ────────────────────────────────────────────────────────────

// ref :5125-5275 — the BObject arm.
void test_object_bindings() {
    const char* R { "lib.rs:5125 BObject" };
    // ⚠️ The inner padding is not cosmetic drift: :5136-5141 prints a space
    // BEFORE every property on a single line (including the first) and :5270
    // prints one before the `}`. An array does neither — see below. Getting this
    // backwards is the single most visible way to be wrong here.
    check("const {a} = o;", "const { a } = o;\n", R);
    check("const {a, b} = o;", "const { a, b } = o;\n", R);
    // `{a: b}` RENAMES — b is the target. Never a type annotation.
    check("const {a: b} = o;", "const { a: b } = o;\n", R);
    check("const {a = 1} = o;", "const { a = 1 } = o;\n", R);
    // ref :5142 — object rest is per-property (binding.rs:258).
    check("const {a, ...rest} = o;", "const { a, ...rest } = o;\n", R);
    // ref :5131 — an empty pattern skips the whole padding block: `{}`, not `{ }`.
    check("const {} = o;", "const {} = o;\n", R);
}

// ref :5079-5122 — the BArray arm.
void test_array_bindings() {
    const char* R { "lib.rs:5079 BArray" };
    // No inner padding — contrast the object arm above. :5090 prints a space
    // only BETWEEN elements (`i != 0`), and there is no closing pad at :5117.
    check("const [x] = a;", "const [x] = a;\n", R);
    check("const [x, y] = a;", "const [x, y] = a;\n", R);
    // ref :5104 / binding.rs:230 — has_spread is ARRAY-level, applied to the
    // last element. There is no per-element spread flag.
    check("const [x, ...ys] = a;", "const [x, ...ys] = a;\n", R);
    check("const [x = 1] = a;", "const [x = 1] = a;\n", R);
    check("const [] = a;", "const [] = a;\n", R);
}

// ref :5060 BMissing + :5111 the trailing-comma rule.
void test_array_holes() {
    const char* R { "lib.rs:5060 BMissing" };
    // A hole prints as nothing; the comma that delimits it comes from :5085.
    check("const [, x] = a;", "const [, x] = a;\n", R);
    // ref :5111 — "Make sure there's a comma after trailing missing items".
    // Without it this prints `[x, ]`, which is a ONE-element array: the trailing
    // comma is load-bearing, not style.
    check("const [x, ,] = a;", "const [x, ,] = a;\n", R);
}

// ref :5147-5167 (computed) and :5169-5258 (EString / other keys).
void test_object_keys() {
    const char* R { "lib.rs:5147 keys" };
    // A computed key is an EXPRESSION: `k` is read, only `v` binds.
    check("const {[k]: v} = o;", "const { [k]: v } = o;\n", R);
    // ref :5172/:5180 — not a valid identifier, so it stays quoted.
    check("const {\"a-b\": c} = o;", "const { \"a-b\": c } = o;\n", R);
    // ref :5248 — a numeric key takes the `_ =>` arm and prints as an expression.
    check("const {0: d} = o;", "const { 0: d } = o;\n", R);
    // A reserved word is a perfectly good key.
    check("const {default: e} = o;", "const { default: e } = o;\n", R);
}

// ref :5052 — print_binding recurses; the nested arms compose.
void test_nested_bindings() {
    const char* R { "lib.rs:5052 nesting" };
    check("const {a: {b}, c: [d]} = o;", "const { a: { b }, c: [d] } = o;\n", R);
    check("const {a: [b] = []} = o;", "const { a: [b] = [] } = o;\n", R);
}

// ref :5083/:5129 `is_single_line` (b.rs:79/:87), which the parser samples at
// mod.rs:1002-1071 / :1088-1126. It affects whitespace only (b.rs:121).
void test_single_line_flag() {
    const char* R { "lib.rs:5083 is_single_line" };
    // A newline anywhere in the pattern clears the flag, and the printer then
    // indents one property per line. This is the ONLY thing that distinguishes
    // these from the single-line vectors above — same tree, different flag.
    check("const {\n  a,\n  b\n} = o;", "const {\n  a,\n  b\n} = o;\n", R);
    check("const [\n  x,\n  y\n] = a;", "const [\n  x,\n  y\n] = a;\n", R);
}

}  // namespace

int main() {
    std::println("test_js_printer_binding — CAP-BUILD-PRINTER shard 4 (print_binding)");
    test_object_bindings();
    test_array_bindings();
    test_array_holes();
    test_object_keys();
    test_nested_bindings();
    test_single_line_flag();
    std::println("{} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
