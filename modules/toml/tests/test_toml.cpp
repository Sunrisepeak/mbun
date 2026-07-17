// test_toml.cpp — T2.2 mbun.toml (Bun.TOML.parse equivalent) test suite (S0).
//
// Test vectors are extracted verbatim from bun's original TOML test suite
// (assertion semantics preserved, see AGENTS.md / test-vector-extraction SOP):
//   - .mbun/bun-ref/test/js/bun/resolve/toml/toml.test.js
//   - .mbun/bun-ref/test/js/bun/resolve/toml/toml-parse.test.ts
//   - .mbun/bun-ref/test/js/bun/resolve/toml/toml-fixture.toml (checkToml fixture)
//   - .mbun/bun-ref/test/js/bun/resolve/toml/crash/toml-crash.test.ts (see note)
//
// DEFERRED(S1 / runtime) — need the Bun.TOML JS wrapper + JS value shape, not the
// pure C++ parser; re-checked once the bun:test runner bridges into mbun:
//   - toml.test.js: "via dynamic import" / "via import type toml" / "via dynamic
//     import with type attribute" / "empty via import statement" — module-loader
//     plumbing (the fixture's *values* are asserted here via parse()).
//   - toml-parse.test.ts: "Bun.TOML.parse with non-string input throws"
//     (SharedArrayBuffer / undefined / null) — JS argument coercion, not parsing.
//   - crash/toml-crash.test.ts: "toml import error has correct lineText" — that is
//     a Bun.build() bundler-diagnostic test (source .lineText), not TOML parsing.
//
// Recalibration note (source-driven, per test-vector-extraction red line): the
// toml.test.js deep-nesting test picks depth=200_000 to exhaust bun's 18 MB
// main-thread stack at a ~100 B zig frame. mbun replaces unbounded recursion with
// an explicit MAX_DEPTH guard in the parser, so the "must not crash, must error"
// semantics are preserved for the SAME 200_000-deep input (the threshold moved
// into the parser, the test input is unchanged).
import std;
import mbun.toml;

namespace {

using mbun::toml::parse;
using mbun::toml::Value;

int gChecks{0};
int gFailures{0};
constexpr int MAX_FAILURE_PRINTS{40};

void report_failure(std::string_view what) {
    ++gFailures;
    if (gFailures <= MAX_FAILURE_PRINTS) {
        std::println("  FAIL {}", what);
    }
}

// --- navigation over a parsed Value tree -----------------------------------

struct Step {
    bool isKey;
    std::string key;
    std::size_t idx;
};
Step K(std::string k) {
    return {true, std::move(k), 0};
}
Step I(std::size_t i) {
    return {false, {}, i};
}

const Value* nav(const Value& root, std::initializer_list<Step> steps) {
    const Value* cur{&root};
    for (const auto& s : steps) {
        if (cur == nullptr) {
            return nullptr;
        }
        if (s.isKey) {
            cur = cur->get(s.key);
        } else {
            if (!cur->is_array() || s.idx >= cur->size()) {
                return nullptr;
            }
            cur = &cur->at(s.idx);
        }
    }
    return cur;
}

// --- checks against a successfully parsed document -------------------------

void expect_string(const Value& root, std::initializer_list<Step> steps, std::string_view expected,
                   std::string_view src) {
    ++gChecks;
    const Value* v{nav(root, steps)};
    if (v == nullptr || !v->is_string() || v->as_string() != expected) {
        report_failure(std::format("[{}] expected string \"{}\", got {}", src, expected,
                                   v == nullptr ? "<absent>"
                                                : (v->is_string() ? v->as_string() : "<non-string>")));
    }
}

void expect_int(const Value& root, std::initializer_list<Step> steps, std::int64_t expected,
                std::string_view src) {
    ++gChecks;
    const Value* v{nav(root, steps)};
    if (v == nullptr || !v->is_integer() || v->as_integer() != expected) {
        report_failure(std::format("[{}] expected int {}, got {}", src, expected,
                                   v == nullptr ? std::string{"<absent>"}
                                   : v->is_integer() ? std::to_string(v->as_integer())
                                                     : std::string{"<non-int>"}));
    }
}

void expect_bool(const Value& root, std::initializer_list<Step> steps, bool expected,
                 std::string_view src) {
    ++gChecks;
    const Value* v{nav(root, steps)};
    if (v == nullptr || !v->is_boolean() || v->as_bool() != expected) {
        report_failure(std::format("[{}] expected bool {}", src, expected));
    }
}

void expect_absent(const Value& root, std::initializer_list<Step> steps, std::string_view src) {
    ++gChecks;
    const Value* v{nav(root, steps)};
    if (v != nullptr) {
        report_failure(std::format("[{}] expected absent key", src));
    }
}

// Parse and return the root, or record a failure and return nullopt.
std::optional<Value> parse_ok(std::string_view toml, std::string_view src) {
    auto r{parse(toml)};
    if (!r.has_value()) {
        ++gChecks;
        report_failure(std::format("[{}] expected parse OK, got error: {}", src, r.error().message));
        return std::nullopt;
    }
    return std::move(*r);
}

// Expect parse failure; if substr is non-empty the error message must contain it.
void expect_error(std::string_view toml, std::string_view substr, std::string_view src) {
    ++gChecks;
    auto r{parse(toml)};
    if (r.has_value()) {
        report_failure(std::format("[{}] expected parse error, but parse succeeded", src));
        return;
    }
    if (!substr.empty() && r.error().message.find(substr) == std::string::npos) {
        report_failure(std::format("[{}] error message \"{}\" missing \"{}\"", src,
                                   r.error().message, substr));
    }
}

// Expect a single top-level string key to decode to `expected`.
void expect_key_string(std::string_view toml, std::string key, std::string_view expected,
                       std::string_view src) {
    auto root{parse_ok(toml, src)};
    if (root) {
        expect_string(*root, {K(std::move(key))}, expected, src);
    }
}

// ---------------------------------------------------------------------------
// toml.test.js — checkToml(fixture)
// ---------------------------------------------------------------------------

// The fixture is copied verbatim from toml-fixture.toml (the .txt twin is byte
// identical; a single C++ vector covers both, semantics unchanged).
constexpr std::string_view kFixture{R"TOML(
framework = "next"
origin = "http://localhost:5000"
inline.array = [1234, 4, 5, 6]


[macros]
react-relay = { "graphql" = "node_modules/bun-macro-relay/bun-macro-relay.tsx" }

[install.scopes]
"@mybigcompany2" = { "token" = "123456", "url" = "https://registry.mybigcompany.com" }
"@mybigcompany3" = { "token" = "123456", "url" = "https://registry.mybigcompany.com", "three" = 4 }


[install.scopes."@mybigcompany"]
token = "123456"
url = "https://registry.mybigcompany.com"

[bundle.packages]
"@emotion/react" = true

[install.cache]
dir = "C:\\Windows\\System32"
dir2 = "C:\\Windows\\System32\\🏳️‍🌈"

[dev]
foo = 123
"foo.bar" = "baz"
"abba.baba" = "baba"
dabba = -123
doo = 123.456
one.two.three = 4

[[array]]
entry_one = "one"
entry_two = "two"

[[array]]
entry_one = "three"

[[array.nested]]
entry_one = "four"
)TOML"};

void test_fixture() {
    constexpr std::string_view src{"toml.test.js > checkToml"};
    auto root{parse_ok(kFixture, src)};
    if (!root) {
        return;
    }
    const Value& t{*root};
    expect_string(t, {K("framework")}, "next", src);
    expect_bool(t, {K("bundle"), K("packages"), K("@emotion/react")}, true, src);
    expect_string(t, {K("array"), I(0), K("entry_one")}, "one", src);
    expect_string(t, {K("array"), I(0), K("entry_two")}, "two", src);
    expect_string(t, {K("array"), I(1), K("entry_one")}, "three", src);
    expect_absent(t, {K("array"), I(1), K("entry_two")}, src);
    expect_string(t, {K("array"), I(1), K("nested"), I(0), K("entry_one")}, "four", src);
    expect_int(t, {K("dev"), K("one"), K("two"), K("three")}, 4, src);
    expect_int(t, {K("dev"), K("foo")}, 123, src);
    expect_int(t, {K("inline"), K("array"), I(0)}, 1234, src);
    expect_int(t, {K("inline"), K("array"), I(1)}, 4, src);
    expect_string(t, {K("dev"), K("foo.bar")}, "baz", src);
    expect_string(t, {K("install"), K("scopes"), K("@mybigcompany"), K("url")},
                  "https://registry.mybigcompany.com", src);
    expect_string(t, {K("install"), K("scopes"), K("@mybigcompany2"), K("url")},
                  "https://registry.mybigcompany.com", src);
    expect_int(t, {K("install"), K("scopes"), K("@mybigcompany3"), K("three")}, 4, src);
    expect_string(t, {K("install"), K("cache"), K("dir")}, "C:\\Windows\\System32", src);
    expect_string(t, {K("install"), K("cache"), K("dir2")}, "C:\\Windows\\System32\\🏳️‍🌈", src);
    // extra: dotted quoted keys, negative int, float
    expect_string(t, {K("dev"), K("abba.baba")}, "baba", src);
    expect_int(t, {K("dev"), K("dabba")}, -123, src);
    ++gChecks;
    {
        const Value* doo{nav(t, {K("dev"), K("doo")})};
        if (doo == nullptr || !doo->is_float() || std::abs(doo->as_float() - 123.456) > 1e-9) {
            report_failure(std::format("[{}] dev.doo expected float 123.456", src));
        }
    }
}

void test_empty() {
    constexpr std::string_view src{"toml.test.js > empty"};
    auto root{parse_ok("", src)};
    if (root) {
        ++gChecks;
        if (!root->is_table() || root->size() != 0) {
            report_failure(std::format("[{}] expected empty table", src));
        }
    }
}

// ---------------------------------------------------------------------------
// toml.test.js — programmatic Bun.TOML.parse cases
// ---------------------------------------------------------------------------

void test_inline_then_table_array() {
    constexpr std::string_view src{"toml.test.js > inline table followed by table array"};
    constexpr std::string_view toml{R"TOML(
[global]
inline_table = { q1 = 1 }

[[items]]
q1 = 1
q2 = 2

[[items]]
q1 = 3
q2 = 4
)TOML"};
    auto root{parse_ok(toml, src)};
    if (!root) {
        return;
    }
    expect_int(*root, {K("global"), K("inline_table"), K("q1")}, 1, src);
    expect_int(*root, {K("items"), I(0), K("q1")}, 1, src);
    expect_int(*root, {K("items"), I(0), K("q2")}, 2, src);
    expect_int(*root, {K("items"), I(1), K("q1")}, 3, src);
    expect_int(*root, {K("items"), I(1), K("q2")}, 4, src);
    ++gChecks;
    if (const Value* items{nav(*root, {K("items")})}; items == nullptr || items->size() != 2) {
        report_failure(std::format("[{}] items length != 2", src));
    }
}

void test_array_then_table_array() {
    constexpr std::string_view src{"toml.test.js > array followed by table array"};
    constexpr std::string_view toml{R"TOML(
[global]
array = [1, 2, 3]

[[items]]
q1 = 1
)TOML"};
    auto root{parse_ok(toml, src)};
    if (!root) {
        return;
    }
    expect_int(*root, {K("global"), K("array"), I(0)}, 1, src);
    expect_int(*root, {K("global"), K("array"), I(1)}, 2, src);
    expect_int(*root, {K("global"), K("array"), I(2)}, 3, src);
    expect_int(*root, {K("items"), I(0), K("q1")}, 1, src);
    ++gChecks;
    if (const Value* items{nav(*root, {K("items")})}; items == nullptr || items->size() != 1) {
        report_failure(std::format("[{}] items length != 1", src));
    }
}

void test_nested_inline_tables() {
    constexpr std::string_view src{"toml.test.js > nested inline tables"};
    constexpr std::string_view toml{R"TOML(
[global]
nested = { outer = { inner = 1 } }

[[items]]
q1 = 1
)TOML"};
    auto root{parse_ok(toml, src)};
    if (!root) {
        return;
    }
    expect_int(*root, {K("global"), K("nested"), K("outer"), K("inner")}, 1, src);
    expect_int(*root, {K("items"), I(0), K("q1")}, 1, src);
}

void test_deep_nesting_errors() {
    // toml.test.js > "throws on deeply nested inline tables instead of crashing".
    // Same 200_000-deep input as bun; must return an error (never overflow the
    // stack). See recalibration note at the top of this file.
    constexpr std::string_view src{"toml.test.js > deeply nested inline tables"};
    constexpr std::size_t depth{200000};
    std::string deep{"a = "};
    deep.reserve(depth * 8 + 8);
    for (std::size_t i{0}; i < depth; ++i) {
        deep += "{ b = ";
    }
    deep += "1";
    for (std::size_t i{0}; i < depth; ++i) {
        deep += " }";
    }
    expect_error(deep, "", src);
}

// ---------------------------------------------------------------------------
// toml-parse.test.ts
// ---------------------------------------------------------------------------

void test_parse_ts_vectors() {
    // \u{XX} variable-length escape at start of a basic string (#30893).
    expect_key_string("key = \"\\u{41}\"", "key", "A", "toml-parse.ts > #30893 \\u{41}");

    // Quoted KEY at file offset 0 with a bad escape must throw, never panic
    // (#30893 underflow). The codepoint after \x / \u is U+3945C (F0 B9 91 9C),
    // which is neither a hex digit nor '{', so the escape is rejected.
    expect_error("\"\\x"
                 "\xF0\xB9\x91\x9C"
                 "\" = 1",
                 "", "toml-parse.ts > #30893 \\x in quoted key");
    expect_error("\"\\u"
                 "\xF0\xB9\x91\x9C"
                 "\" = 1",
                 "", "toml-parse.ts > #30893 \\u in quoted key");

    // Trailing backslash-CR line-continuation ending a multiline basic string
    // (#30893). Bytes: key = """\<CR>"""  ->  "".
    expect_key_string("key = \"\"\"\\\r\"\"\"", "key", "", "toml-parse.ts > #30893 backslash-CR");

    // \t = U+0009, \f = U+000C (the zig lexer had these swapped).
    expect_key_string("k = \"a\\tb\"", "k", "a\tb", "toml-parse.ts > \\t escape");
    expect_key_string("k = \"a\\fb\"", "k", "a\fb", "toml-parse.ts > \\f escape");

    // Literal CRLF normalizes to LF in a multiline basic string; \t forces the
    // slow decode path. Bytes: k = """a<CRLF>b\tc"""  ->  "a\nb\tc".
    expect_key_string("k = \"\"\"a\r\nb\\tc\"\"\"", "k", "a\nb\tc",
                      "toml-parse.ts > CRLF normalization");

    // Out-of-range \u{...} escapes throw with the specific message (#30825).
    constexpr std::string_view oor{"Unicode escape sequence is out of range"};
    expect_error("a = \"\\u{3333333316aaaaaaa}\"", oor, "toml-parse.ts > #30825 oor big");
    std::string f64(64, 'f');
    expect_error("a = \"\\u{" + f64 + "}\"", oor, "toml-parse.ts > #30825 oor 64f");
    expect_error("a = \"\\u{0000" + f64 + "}\"", oor, "toml-parse.ts > #30825 oor 0000+64f");
    expect_error("a = \"\\u{110000}\"", oor, "toml-parse.ts > #30825 oor 110000");

    // \u{...} with no closing brace throws Syntax Error (#30825).
    constexpr std::string_view syn{"Syntax Error"};
    expect_error("a = \"\\u{41\"", syn, "toml-parse.ts > #30825 no-brace 41");
    expect_error("a = \"\\u{\"", syn, "toml-parse.ts > #30825 no-brace empty");
    expect_error("a = \"\\u{110000\"", syn, "toml-parse.ts > #30825 no-brace 110000");
    expect_error("a = \"\\u{" + f64 + "\"", syn, "toml-parse.ts > #30825 no-brace 64f");

    // In-range \u{...} escapes still decode (#30825).
    {
        constexpr std::string_view src{"toml-parse.ts > #30825 in-range 41"};
        auto root{parse_ok("a = \"\\u{41}\"", src)};
        if (root) {
            expect_string(*root, {K("a")}, "A", src);
        }
    }
    {
        constexpr std::string_view src{"toml-parse.ts > #30825 in-range leading zeros"};
        std::string z64(64, '0');
        auto root{parse_ok("a = \"\\u{" + z64 + "41}\"", src)};
        if (root) {
            expect_string(*root, {K("a")}, "A", src);
        }
    }
    {
        constexpr std::string_view src{"toml-parse.ts > #30825 in-range 10FFFF"};
        auto root{parse_ok("a = \"\\u{10FFFF}\"", src)};
        if (root) {
            // U+10FFFF encodes to F4 8F BF BF.
            expect_string(*root, {K("a")}, "\xF4\x8F\xBF\xBF", src);
        }
    }

    // Arrays require comma separators (#31252).
    expect_error("a = [1 2]", "", "toml-parse.ts > #31252 [1 2]");
    expect_error("a = [1 2 3]", "", "toml-parse.ts > #31252 [1 2 3]");
    expect_error("a = [1, 2 3]", "", "toml-parse.ts > #31252 [1, 2 3]");
    expect_error("a = [\"x\" \"y\"]", "", "toml-parse.ts > #31252 [\"x\" \"y\"]");
    {
        constexpr std::string_view src{"toml-parse.ts > #31252 [1, 2]"};
        auto root{parse_ok("a = [1, 2]", src)};
        if (root) {
            expect_int(*root, {K("a"), I(0)}, 1, src);
            expect_int(*root, {K("a"), I(1)}, 2, src);
            ++gChecks;
            if (const Value* a{nav(*root, {K("a")})}; a == nullptr || a->size() != 2) {
                report_failure(std::format("[{}] length != 2", src));
            }
        }
    }
    {
        constexpr std::string_view src{"toml-parse.ts > #31252 trailing comma"};
        auto root{parse_ok("a = [1, 2,]", src)};
        if (root) {
            ++gChecks;
            if (const Value* a{nav(*root, {K("a")})}; a == nullptr || a->size() != 2) {
                report_failure(std::format("[{}] trailing-comma length != 2", src));
            }
        }
    }
}

}  // namespace

int main() {
    test_fixture();
    test_empty();
    test_inline_then_table_array();
    test_array_then_table_array();
    test_nested_inline_tables();
    test_deep_nesting_errors();
    test_parse_ts_vectors();

    if (gFailures > MAX_FAILURE_PRINTS) {
        std::println("  ... {} more failures not shown", gFailures - MAX_FAILURE_PRINTS);
    }
    std::println("test_toml: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
