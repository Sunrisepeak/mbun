// test_css_tokenizer.cpp — public tokenizer + parse/serialize round-trip tests.
//
// Complements test_css.cpp (which drives bun's upstream minify/css vectors)
// with token-level coverage of the shared scanner `mbun::css::tokenize` and
// with structural round-trip / idempotency properties of the transform engine.
//
// Discipline (AGENTS.md TDD): the concrete `minify(...)` expectations below are
// only asserted where the output is deterministic and semantically unambiguous
// (whitespace collapse, trailing-';' drop, comment stripping, comma/`!important`
// spacing) — these mirror the engine behavior already validated by the upstream
// vectors, not invented normalizations. Typed-property transforms stay DEFERRED.
import std;
import mbun.css;
import mbun.css.tokenizer;

namespace {

using namespace mbun::css;

int gChecks{};
int gFailures{};

void check(bool condition, std::string_view label) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::println("FAIL: {}", label);
    }
}

void eq(std::string_view got, std::string_view want, std::string_view label) {
    ++gChecks;
    if (got != want) {
        ++gFailures;
        std::println("FAIL: {}\n  got:  {:?}\n  want: {:?}", label, got, want);
    }
}

// ── tokenizer: kind classification of each syntactic category ──
void test_token_kinds() {
    auto kinds = [](std::string_view src) {
        std::vector<CssTokenKind> k;
        for (const auto& t : tokenize(src)) k.push_back(t.kind);
        return k;
    };
    using K = CssTokenKind;

    check(kinds("color") == std::vector{K::Ident}, "bare ident");
    check(kinds("calc(") == std::vector{K::Function}, "function keeps trailing (");
    check(kinds("@media") == std::vector{K::AtKeyword}, "at-keyword");
    check(kinds("#id") == std::vector{K::Hash}, "hash");
    check(kinds("\"hi\"") == std::vector{K::String}, "string");
    check(kinds("42") == std::vector{K::Number}, "number");
    check(kinds("10%") == std::vector{K::Percentage}, "percentage");
    check(kinds(".5em") == std::vector{K::Dimension}, "dimension with leading dot");
    check(kinds("1.5e3px") == std::vector{K::Dimension}, "dimension with exponent");
    check(kinds("   ") == std::vector{K::Whitespace}, "whitespace run collapses to one token");
    check(kinds("/* x */") == std::vector{K::Comment}, "comment");
    check(kinds("<!--") == std::vector{K::Cdo}, "CDO");
    check(kinds("-->") == std::vector{K::Cdc}, "CDC");
    check(kinds("url(a.png)") == std::vector{K::Url}, "unquoted url is one Url token");

    // quoted url() is a normal function call, not a Url token
    check(kinds("url('a')") == std::vector{K::Function, K::String, K::CloseParen},
          "quoted url is Function + String + CloseParen");

    // attribute selector with substring-match operators
    check(kinds("[a~=b]") ==
              std::vector{K::OpenSquare, K::Ident, K::IncludeMatch, K::Ident, K::CloseSquare},
          "~= include-match operator");
    check(kinds("[a|=b]") ==
              std::vector{K::OpenSquare, K::Ident, K::DashMatch, K::Ident, K::CloseSquare},
          "|= dash-match operator");
    check(kinds("[a^=b]") ==
              std::vector{K::OpenSquare, K::Ident, K::PrefixMatch, K::Ident, K::CloseSquare},
          "^= prefix-match operator");
    check(kinds("[a$=b]") ==
              std::vector{K::OpenSquare, K::Ident, K::SuffixMatch, K::Ident, K::CloseSquare},
          "$= suffix-match operator");
    check(kinds("[a*=b]") ==
              std::vector{K::OpenSquare, K::Ident, K::SubstringMatch, K::Ident, K::CloseSquare},
          "*= substring-match operator");

    // punctuation
    check(kinds("a:b;") == std::vector{K::Ident, K::Colon, K::Ident, K::Semicolon}, "colon/semicolon");
    check(kinds("a,b") == std::vector{K::Ident, K::Comma, K::Ident}, "comma");
    check(kinds("{}") == std::vector{K::OpenCurly, K::CloseCurly}, "curly braces");

    // negative number vs ident starting with '-'
    check(kinds("-5") == std::vector{K::Number}, "negative number");
    check(kinds("-webkit-box") == std::vector{K::Ident}, "vendor-prefixed ident");
    check(kinds("--custom") == std::vector{K::Ident}, "custom-property ident");
}

// ── tokenizer: zero-copy spans point back into the source ──
void test_token_spans() {
    std::string_view src = "a > .b";
    auto toks = tokenize(src);
    // Ident(a) WS Delim(>) WS Ident(.b→ '.' delim + ident 'b')  — '.' not a
    // number here, so it is a Delim; 'b' is an Ident.
    check(toks.size() >= 3, "prelude produces several tokens");
    eq(toks.front().text(src), "a", "first span is 'a'");

    auto combinator = std::ranges::find_if(
        toks, [](const CssToken& t) { return t.kind == CssTokenKind::Delim && t.delim == '>'; });
    check(combinator != toks.end(), "child combinator present as Delim '>'");

    // Dimension span carries the whole unit
    auto dim = tokenize("12.5px");
    check(dim.size() == 1 && dim[0].kind == CssTokenKind::Dimension, "single dimension token");
    eq(dim[0].text("12.5px"), "12.5px", "dimension span covers number+unit");
}

// ── tokenizer: escapes and helpers ──
void test_escapes_and_helpers() {
    // a hex escape inside an ident does not terminate the ident
    auto t = tokenize("\\2b");  // escaped '+'
    check(t.size() == 1 && t[0].kind == CssTokenKind::Ident, "leading escape yields ident");

    // escape inside a string is consumed, not treated as a close
    auto s = tokenize("\"a\\\"b\"");
    check(s.size() == 1 && s[0].kind == CssTokenKind::String, "escaped quote stays inside string");

    // bare backslash before newline is a Delim (invalid escape)
    auto bad = tokenize("\\\n");
    check(!bad.empty() && bad[0].kind == CssTokenKind::Delim, "invalid escape is a delim");

    check(css_is_whitespace(' ') && css_is_whitespace('\t') && css_is_whitespace('\n'),
          "css_is_whitespace covers ascii ws");
    check(css_is_digit('0') && css_is_digit('9') && !css_is_digit('a'), "css_is_digit");
}

// ── round-trip: deterministic minify outputs ──
void test_minify_roundtrip() {
    eq(minify("a { color: red }"), "a{color:red}", "basic rule minify");
    eq(minify("a{color:red;}"), "a{color:red}", "trailing semicolon dropped");
    eq(minify("/* c */ a { color: red }"), "a{color:red}", "top-level comment stripped");
    eq(minify("a { color/* c */: red }"), "a{color:red}", "inline comment stripped");
    eq(minify("a,  b {color:red}"), "a,b{color:red}", "selector-list comma has no space");
    eq(minify("a{color:red !important}"), "a{color:red!important}", "important has no space");
    eq(minify("a   b {color:red}"), "a b{color:red}", "descendant combinator kept");
    eq(minify("a>b{color:red}"), "a>b{color:red}", "child combinator tight");
    eq(minify("@media screen{a{color:red}}"), "@media screen{a{color:red}}", "nested at-rule body");
    eq(minify("a{&:hover{color:red}}"), "a{&:hover{color:red}}", "CSS nesting round-trip");
}

// ── round-trip: structural invariants that hold regardless of exact spacing ──
void test_invariants() {
    for (std::string_view src : {
             "a{color:red}",
             "a { color : red ; background : blue }",
             "@media screen and (min-width: 100px) { a { color: red } }",
             ".x > .y + .z ~ .w { top: 0 }",
             "a{&:hover{color:red}}",
             "@font-face{font-family:'x';src:url(a.woff)}",
             "a[href~=\"x\"]{content:\"y\"}",
             "@import \"a.css\";",
             ":root{--c:calc(1px + 2px)}",
         }) {
        std::string m1 = minify(src);
        // idempotency: minifying a minified sheet is a fixed point.
        eq(minify(m1), m1, "minify is idempotent");
        // pretty output is valid CSS: re-minifying it reproduces the minify.
        eq(minify(print(src)), m1, "minify(print(src)) == minify(src)");
    }
}

}  // namespace

int main() {
    test_token_kinds();
    test_token_spans();
    test_escapes_and_helpers();
    test_minify_roundtrip();
    test_invariants();
    std::println("test_css_tokenizer: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
