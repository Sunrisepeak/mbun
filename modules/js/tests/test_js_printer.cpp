// test_js_printer.cpp — T2.5 mbun.js_printer (AST → source printer) test suite.
//
// Vectors are extracted verbatim from bun's original transpiler test suite
// (assertion semantics preserved, see AGENTS.md TDD rules). Primary source file:
//   - .mbun/bun-ref/test/bundler/transpiler/transpiler.test.js
// Each group cites the bun `it()`/`describe()` block it derives from. bun asserts
// the printed output STRING exactly; the two helpers there are:
//   * expectPrinted(code, out)  / expectPrinted_(code, out):
//        expect(parsed(code, !out.endsWith(";\n"), …)).toBe(out)
//     i.e. the printed program is compared to `out`; when `out` does NOT end with
//     ";\n" the printed text is trimmed first (bun prints every statement with a
//     trailing ";\n"). We mirror that trimming rule exactly in `check` below.
//
// This suite consumes the DEFERRED(T2.5) round-trip checklist registered in the
// header of test_js_parser.cpp — the `expectPrinted*` vectors whose *printed*
// form that suite left to T2.5. Coverage realised here:
//   [T2.5 ✓] literal & array printing, object literal spacing, TS type-argument
//            /`as`/`satisfies` ERASURE round-trips (f<x> forms), operator
//            precedence with minimal parentheses (relational `f < x > +g` split),
//            single-param arrow parenthesization `(m) =>`, string quote selection
//            (double↔single), var/let/const declarations with erased annotations.
//
// DEFERRED(T2.5, re-deferred with reason):
//   - expectPrintedMin_ constant-folding vectors ("a"+"b" -> "ab", ["hey"][0] ->
//     "hey", (!1)**2 -> false**2, numeric folding): folding is an optimizer/parser
//     pass not yet implemented; the current parser produces no folded AST, so the
//     minified expectations cannot be met. The PrintOptions.minify flag is wired
//     but performs no syntax folding yet.
//   - array HOLE and SPREAD vectors ([,1] -> "[, 1]", [...b] -> "[...b]"): the
//     T2.4 array-literal parser collapses elisions and drops the spread marker, so
//     these node shapes are not represented in the AST.
//   - import/export statement round-trips: import/export STATEMENTS are not parsed
//     by the T2.4 subset (only import() / import.meta expressions), so their
//     printed forms land with the import/export parsing work.
//   - optional-call `f<number>?.()` -> "f?.()": the parser does not record call
//     optionality on the Call node yet.
//   - `f<x> as g<y>;` / `f<x> satisfies g<y>;` -> "f;\n": the parser's
//     instantiation-follow classifier treats a contextual `as`/`satisfies`
//     identifier after `<…>` as an expression start (relational reading), so the
//     type arguments are not erased; erasing needs a follow-classification tweak.
//   - string newline→backtick and lone-surrogate escaping (`"\n"`->`` `\n` ``,
//     `"\uD800"`): the template-fallback and surrogate-escape printer paths are
//     out of this subset.
import std;
import mbun.js_parser;
import mbun.ast;
import mbun.js_printer;

namespace {

using mbun::js_parser::parse;
using mbun::js_parser::ParseResult;
using mbun::js_printer::print;
using mbun::js_printer::PrintOptions;

int gChecks{0};
int gFailures{0};
constexpr int MAX_FAILURE_PRINTS{60};

std::string printable(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out += std::format("\\x{:02X}", c);
        }
    }
    return out;
}

std::string trim(std::string_view s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (a < b && ws(s[a])) {
        ++a;
    }
    while (b > a && ws(s[b - 1])) {
        --b;
    }
    return std::string{s.substr(a, b - a)};
}

// Mirror bun's `parsed(code, trim, …)`: when the expectation is trimmed (does not
// end with ";\n"), bun does out.trim(), strips ONE trailing ';', then trims again.
std::string trim_like_bun(std::string_view s) {
    std::string t = trim(s);
    if (!t.empty() && t.back() == ';') {
        t.pop_back();
    }
    return trim(t);
}

// Mirror bun's expectPrinted: parse, print, and compare — applying bun's trim rule
// when `expected` does not end with ";\n".
void check(std::string_view src, std::string_view expected, std::string_view ref,
           bool minify = false) {
    ++gChecks;
    ParseResult r = parse(src);
    if (!r.ok) {
        ++gFailures;
        if (gFailures <= MAX_FAILURE_PRINTS) {
            std::println("  FAIL [{}] parse(\"{}\") unexpected error \"{}\"", ref, printable(src),
                         printable(r.error));
        }
        return;
    }
    PrintOptions opts;
    opts.minify = minify;
    std::string got = print(r.arena, r.program, src, opts);
    std::string want{expected};
    std::string cmp = expected.ends_with(";\n") ? got : trim_like_bun(got);
    if (cmp != want) {
        ++gFailures;
        if (gFailures <= MAX_FAILURE_PRINTS) {
            std::println("  FAIL [{}] print(\"{}\") = \"{}\", expected \"{}\"", ref, printable(src),
                         printable(cmp), printable(want));
        }
    }
}

// ── vector groups ─────────────────────────────────────────────────────────────

void test_arrays() {
    // source: transpiler.test.js > describe("parser") > it("arrays").
    // (Hole/elision vectors re-deferred — see file header.)
    const char* SRC{"transpiler.test.js > parser > arrays"};
    check("[]", "[]", SRC);
    check("[1]", "[1]", SRC);
    check("[1,]", "[1]", SRC);
    check("[1,2]", "[1, 2]", SRC);
    check("[1,2,]", "[1, 2]", SRC);
}

void test_string_quote_selection() {
    // source: transpiler.test.js > describe("parser") > it("string quote selection").
    const char* SRC{"transpiler.test.js > parser > string quote selection"};
    check("console.log(\"\\\"\")", "console.log('\"')", SRC);
    check("console.log('\\'')", "console.log(\"'\")", SRC);
}

void test_type_argument_erasure() {
    // source: transpiler.test.js > describe("TypeScript") > it("types").
    // Type-argument lists / `as` / `satisfies` are erased; the JS shape prints.
    const char* SRC{"transpiler.test.js > TypeScript > types"};
    check("f<number>.g", "f.g;\n", SRC);
    check("f<x>, g<y>;", "f, g;\n", SRC);
    check("f<x>g<y>;", "f < x > g;\n", SRC);
    check("f<x>=g<y>;", "f < x >= g;\n", SRC);  // fused >= keeps relational (bun :547)
    check("f<x> = g<y>;", "f = g;\n", SRC);
    check("f<x> * g<y>;", "f * g;\n", SRC);
    check("f<x> == g<y>;", "f == g;\n", SRC);
    check("f<x> ?? g<y>;", "f ?? g;\n", SRC);
    check("f<x> in g<y>;", "f in g;\n", SRC);
    check("f<x> instanceof g<y>;", "f instanceof g;\n", SRC);
    check("f<x> ? g<y> : h<z>;", "f ? g : h;\n", SRC);
    check("a([f<x>]);", "a([f]);\n", SRC);
    check("f<x> + g<y>;", "f < x > +g;\n", SRC);
}

void test_satisfies() {
    // source: transpiler.test.js > describe("TypeScript") > it("satisfies").
    // (export-default variants re-deferred — export statements are unparsed.)
    const char* SRC{"transpiler.test.js > TypeScript > satisfies"};
    check("const t1 = { a: 1 } satisfies I1;", "const t1 = { a: 1 };\n", SRC);
    check("const t2 = { a: 1, b: 1 } satisfies I1;", "const t2 = { a: 1, b: 1 };\n", SRC);
    check("const t3 = { } satisfies I1;", "const t3 = {};\n", SRC);
    check("const t4: T1 = { a: 'a' } satisfies T1;", "const t4 = { a: \"a\" };\n", SRC);
    check("const t5 = (m => m.substring(0)) satisfies T2;", "const t5 = (m) => m.substring(0);\n",
          SRC);
    check("const t6 = [1, 2] satisfies [number, number];", "const t6 = [1, 2];\n", SRC);
    check("let t7 = { a: 'test' } satisfies A;", "let t7 = { a: \"test\" };\n", SRC);
    check("let t8 = { a: 'test', b: 'test' } satisfies A;",
          "let t8 = { a: \"test\", b: \"test\" };\n", SRC);
    check("var v = undefined satisfies 1;", "var v = undefined;\n", SRC);
    check("const a = { x: 10 } satisfies Partial<Point2d>;", "const a = { x: 10 };\n", SRC);
    check("const x2 = { m: true, s: \"false\" } satisfies Facts;",
          "const x2 = { m: true, s: \"false\" };\n", SRC);
}

}  // namespace

int main() {
    test_arrays();
    test_string_quote_selection();
    test_type_argument_erasure();
    test_satisfies();

    std::println("test_js_printer: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
