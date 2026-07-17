// test_jsonc.cpp — T2.2 Bun.JSONC.parse pure-parser vectors (S0).
//
// source: test/js/bun/jsonc/jsonc.test.ts (all pure parse groups)
// source: test/js/bun/resolve/jsonc.test.ts (empty/config value semantics)
//
// DEFERRED(S1 / runtime integration):
// - `Bun.JSONC exists`: JSC global-object wiring.
// - resolve/jsonc.test.ts imports: module-loader `.jsonc` dispatch and process execution.
// - the pathological-input subprocess timeout assertion: this S0 test preserves the
//   exact 250k/240k inputs and result/error semantics; process timeout belongs to CLI.
import std;
import mbun.config.jsonc;
import mbun.config.value;

namespace {

using mbun::config::Value;
using mbun::config::canonical_hash;

struct OracleCase {
    std::string_view name;
    std::string_view source;
    int outcome; // 0 reject, 1 accept+hash, 2 implementation-defined smoke only
    std::uint64_t expectedHash;
};

struct FuzzCase {
    std::string_view source;
    bool required;
    std::uint64_t expectedHash;
};

#include "json_corpus_vectors.inc"
#include "json_fuzz_generated_vectors.inc"
#include "json_fuzz_mutant_vectors.inc"
using mbun::config::jsonc::parse;

int gChecks{};
int gFailures{};

void fail(std::string_view message) {
    ++gFailures;
    if (gFailures <= 40) {
        std::println("  FAIL {}", message);
    }
}

void expect(bool condition, std::string_view source) {
    ++gChecks;
    if (!condition) {
        fail(source);
    }
}

std::optional<Value> parse_ok(std::string_view source, std::string_view label) {
    auto result{parse(source)};
    ++gChecks;
    if (!result) {
        fail(std::format("{}: parse failed at {}: {}", label, result.error().offset,
                         result.error().message));
        return std::nullopt;
    }
    return std::move(*result);
}

void expect_error(std::string_view source, std::string_view label) {
    ++gChecks;
    if (parse(source)) {
        fail(std::format("{}: expected parse error", label));
    }
}

const Value* nav(const Value& root, std::initializer_list<std::variant<std::string_view, std::size_t>> path) {
    const Value* value{&root};
    for (const auto& part : path) {
        if (const auto* key{std::get_if<std::string_view>(&part)}) {
            value = value->get(*key);
        } else {
            const auto index{std::get<std::size_t>(part)};
            value = value->is_array() && index < value->size() ? &value->at(index) : nullptr;
        }
        if (value == nullptr) {
            return nullptr;
        }
    }
    return value;
}

void expect_string(const Value& root,
                   std::initializer_list<std::variant<std::string_view, std::size_t>> path,
                   std::string_view expected, std::string_view label) {
    const Value* value{nav(root, path)};
    expect(value != nullptr && value->is_string() && value->as_string() == expected, label);
}

void expect_number(const Value& root,
                   std::initializer_list<std::variant<std::string_view, std::size_t>> path,
                   double expected, std::string_view label) {
    const Value* value{nav(root, path)};
    expect(value != nullptr && value->is_number() && value->as_double() == expected, label);
}

void test_basics() {
    // source: jsonc.test.ts > basic/comments/trailing/complex/nested/boolean/null/empty
    auto basic{parse_ok(R"({"name":"test","value":42})", "basic")};
    if (basic) {
        expect_string(*basic, {"name"}, "test", "basic.name");
        expect_number(*basic, {"value"}, 42, "basic.value");
    }

    auto comments{parse_ok(R"JSON({
        // line
        "a": 1 /* one */,
        "b": true /* yes */,
        "c": null // nothing
        , "d": -2.5,
    })JSON", "comments")};
    if (comments) {
        expect_number(*comments, {"a"}, 1, "comments.a");
        const Value* b{comments->get("b")};
        const Value* c{comments->get("c")};
        expect(b && b->is_boolean() && b->as_bool(), "comments.b");
        expect(c && c->is_null(), "comments.c");
        expect_number(*comments, {"d"}, -2.5, "comments.d");
    }

    auto complex{parse_ok(R"JSON({
      "dependencies": { "react": "^18", "typescript": "^5", },
      "scripts": ["build", "test", "lint",],
      "outer": { "inner": { "value": 123, }, },
    })JSON", "complex")};
    if (complex) {
        expect_string(*complex, {"dependencies", "react"}, "^18", "complex.react");
        expect_string(*complex, {"scripts", std::size_t{2}}, "lint", "complex.script");
        expect_number(*complex, {"outer", "inner", "value"}, 123, "complex.nested");
    }

    auto emptyObject{parse_ok("", "empty package/tsconfig")};
    expect(emptyObject && emptyObject->is_object() && emptyObject->size() == 0,
           "empty file is empty object");
    auto emptyArray{parse_ok("[]", "empty array")};
    expect(emptyArray && emptyArray->is_array() && emptyArray->size() == 0, "empty array");
    auto single{parse_ok("{'a': 'b\"c'}", "single quotes")};
    if (single) {
        expect_string(*single, {"a"}, "b\"c", "single quoted strings");
    }
    auto negativeZero{parse_ok("-0", "negative zero")};
    expect(negativeZero && negativeZero->is_number() && negativeZero->as_double() == 0.0 &&
               std::signbit(negativeZero->as_double()),
           "JSON -0 preserves the JS Number sign bit");

    // source: json-test-suite.test.ts > N_VALID_JSONC number/string extensions
    struct NumberCase {
        std::string_view source;
        double expected;
    };
    constexpr std::array numberCases{
        NumberCase{"012", 10}, NumberCase{"-012", -10}, NumberCase{"1.", 1},
        NumberCase{".123", 0.123}, NumberCase{"-.123", -0.123}, NumberCase{"0x42", 66},
        NumberCase{"- 1", -1}, NumberCase{"1e400", std::numeric_limits<double>::infinity()},
    };
    for (const auto& item : numberCases) {
        auto value{parse_ok(item.source, "Bun JSONC numeric extension")};
        expect(value && value->is_number() && value->as_double() == item.expected,
               std::format("Bun JSONC numeric extension value: {}", item.source));
    }
    auto hexEscape{parse_ok(R"("a\x41\x00b")", "JSONC x escape")};
    expect(hexEscape && hexEscape->is_string() && hexEscape->as_string() == std::string{"aA\0b", 4},
           "JSONC x escape value");

    auto hashTree{parse_ok(R"({"a":[-0,"é","\ud800",true,null],"b":{"x":1.5}})",
                           "canonical deep hash")};
    expect(hashTree && canonical_hash(*hashTree) == 0x2fe44e2c748b39e8ULL,
           "canonical hash covers type/value/negative-zero/nesting/WTF-8");
}

void test_errors_and_depth() {
    // source: jsonc.test.ts > invalid/recovery/control/deep/pathological
    constexpr std::array invalid{
        std::string_view{"{ invalid json }"}, std::string_view{"{\"a\": \"line1\nline2\"}"},
        std::string_view{" "}, std::string_view{"// comment only"}, std::string_view{"/* comment only */"},
        std::string_view{"\"ab\tcd\""}, std::string_view{"\"abc"}, std::string_view{"{\"a\": 1"},
        std::string_view{"[1, 2"}, std::string_view{"{\"a\":1 \"b\":2}"},
        std::string_view{"{\"a\" \"b\"}"}, std::string_view{"[1 true]"},
        std::string_view{"[\"\": 1]"}, std::string_view{"{\"a\":{\"b\":1 \"c\":2}}"},
        std::string_view{"[{\"a\":1} {\"b\":2}]"},
        std::string_view{"truex"}, std::string_view{"null0"},
        std::string_view{"{\"a\":1}/*"}, std::string_view{"{\"a\":1}/"},
        std::string_view{"{\"a\":1}/**//"},
    };
    for (const auto input : invalid) {
        expect_error(input, "invalid/recovery");
    }

    constexpr std::size_t DEPTH{200'000};
    std::string arrays(DEPTH, '[');
    arrays.append(DEPTH, ']');
    expect_error(arrays, "deep arrays");
    std::string objects;
    objects.reserve(DEPTH * 6 + 1);
    for (std::size_t i{}; i < DEPTH; ++i) {
        objects += "{\"a\":";
    }
    objects += '1';
    objects.append(DEPTH, '}');
    expect_error(objects, "deep objects");

    std::string malformed{"[{\"-1"};
    for (int i{}; i < 50'000; ++i) {
        malformed += "\":[{";
    }
    expect_error(malformed, "malformed flood");
    std::string duplicate{"{"};
    for (int i{}; i < 40'000; ++i) {
        duplicate += "\"a\":1,";
    }
    duplicate += "\"a\":1}";
    auto dup{parse_ok(duplicate, "duplicate key flood")};
    if (dup) {
        expect_number(*dup, {"a"}, 1, "duplicate last value");
    }
}

class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_{seed | 1u} {}

    double next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        return static_cast<double>(state_ & 0xFFFFFFu) / 0x1000000u;
    }

private:
    std::uint64_t state_;
};

struct Generated {
    std::string document;
    Value expected;
};

Generated generate_json(Rng& rng, int depth) {
    const double choice{rng.next()};
    if (depth > 3 || choice < 0.12) {
        return {"null", Value::null()};
    }
    if (choice < 0.22) {
        const bool value{rng.next() < 0.5};
        return {value ? "true" : "false", Value::boolean(value)};
    }
    if (choice < 0.4) {
        const double kind{rng.next()};
        if (kind < 0.25) {
            const std::int64_t value{static_cast<std::int64_t>(rng.next() * 2147483648.0) *
                                     (rng.next() < 0.5 ? -1 : 1)};
            return {std::to_string(value), Value::integer(value)};
        }
        if (kind < 0.5) {
            const std::string text{std::format("{:.3f}", rng.next() * 1e9)};
            return {text, Value::floating(std::stod(text))};
        }
        if (kind < 0.75) {
            const int mantissa{static_cast<int>(rng.next() * 1e6)};
            const int exponent{static_cast<int>(rng.next() * 40) - 20};
            const std::string text{std::format("{}e{}", mantissa, exponent)};
            return {text, Value::floating(std::stod(text))};
        }
        const double value{rng.next() * 9007199254740991.0};
        std::array<char, 64> buffer{};
        const auto [end, error]{std::to_chars(buffer.data(), buffer.data() + buffer.size(), value)};
        expect(error == std::errc{}, "generated number formatting");
        return {std::string{buffer.data(), end}, Value::floating(value)};
    }
    if (choice < 0.62) {
        std::string encoded{"\""};
        std::string decoded;
        const int length{static_cast<int>(rng.next() * 60)};
        for (int i{}; i < length; ++i) {
            const double kind{rng.next()};
            if (kind < 0.06) {
                encoded += "\\\"";
                decoded += '"';
            } else if (kind < 0.12) {
                encoded += "\\\\";
                decoded += '\\';
            } else if (kind < 0.18) {
                encoded += "\\n";
                decoded += '\n';
            } else if (kind < 0.24) {
                encoded += "\\u00e9";
                decoded += "é";
            } else if (kind < 0.3) {
                encoded += "é";
                decoded += "é";
            } else if (kind < 0.34) {
                encoded += "🚀";
                decoded += "🚀";
            } else if (kind < 0.38) {
                encoded += "\\ud83d\\ude00";
                decoded += "😀";
            } else if (kind < 0.42) {
                encoded += "\\t";
                decoded += '\t';
            } else {
                const char c{static_cast<char>('a' + static_cast<int>(rng.next() * 26))};
                encoded += c;
                decoded += c;
            }
        }
        encoded += '"';
        return {std::move(encoded), Value::string(std::move(decoded))};
    }
    if (choice < 0.8) {
        Value expected{Value::make_array()};
        std::string document{"["};
        const int count{static_cast<int>(rng.next() * 6)};
        for (int i{}; i < count; ++i) {
            if (i) {
                document += ',';
            }
            auto child{generate_json(rng, depth + 1)};
            document += child.document;
            expected.array().push_back(std::move(child.expected));
        }
        document += ']';
        return {std::move(document), std::move(expected)};
    }
    Value expected{Value::make_object()};
    std::string document{"{"};
    const int count{static_cast<int>(rng.next() * 8)};
    for (int i{}; i < count; ++i) {
        if (i) {
            document += ',';
        }
        const std::string key{std::format("k{}_{}", i, static_cast<int>(rng.next() * 1000))};
        auto child{generate_json(rng, depth + 1)};
        document += std::format("\"{}\"{}:{}", key, rng.next() < 0.2 ? " " : "", child.document);
        expected.object().entries.emplace_back(key, std::move(child.expected));
    }
    document += '}';
    return {std::move(document), std::move(expected)};
}

void test_generated_differential() {
    // source: jsonc.test.ts > matches JSON.parse on generated documents
    Rng rng{0xC0FFEE};
    for (int i{}; i < 750; ++i) {
        auto generated{generate_json(rng, 0)};
        auto actual{parse_ok(generated.document, "generated differential")};
        if (actual) {
            expect(*actual == generated.expected, "generated document equals construction tree");
        }
    }
}

void test_unicode_and_escapes() {
    // source: jsonc.test.ts > escape-heavy/surrogates/BOM/exotic whitespace
    constexpr std::array escapeDocs{
        std::string_view{R"("\\\"")"}, std::string_view{R"(["\b\f\n\r\t\/\\"])"},
        std::string_view{R"({"😀":"😀"})"}, std::string_view{"\"\\ud83d\\ude00\""},
        std::string_view{"\"\\ud800\\udc00\""}, std::string_view{"{\"k\\ud800\":\"\\udc00v\"}"},
    };
    for (const auto doc : escapeDocs) {
        expect(static_cast<bool>(parse(doc)), "escape-heavy document");
    }

    // WTF-8 encodes unpaired UTF-16 surrogates as ED A0..BF 80..BF.
    auto lone{parse_ok("\"a\\ud800z\"", "lone surrogate")};
    if (lone) {
        expect(lone->is_string() && lone->as_string() == std::string{"a\xED\xA0\x80z", 5},
               "lone surrogate WTF-8");
    }
    auto pair{parse_ok("\"\\ud83d\\ude00\"", "surrogate pair")};
    if (pair) {
        expect(pair->is_string() && pair->as_string() == "\xF0\x9F\x98\x80", "surrogate pair UTF-8");
    }

    constexpr std::array bomDocs{
        std::string_view{"[1\uFEFF,2]"}, std::string_view{"[\uFEFF1]"},
        std::string_view{"[\uFEFFnull\uFEFF]"},
        std::string_view{"\uFEFF{\uFEFF\"a\"\uFEFF:\uFEFF1\uFEFF}"},
        std::string_view{"\uFEFF// comment\n{\"a\":1}"},
        std::string_view{"{\u00A0/*x*/\"a\":\u20281\u2029}"},
    };
    for (const auto doc : bomDocs) {
        expect(static_cast<bool>(parse(doc)), "BOM/exotic whitespace");
    }
    auto lineSep{parse_ok("// comment\xE2\x80\xA8" "42", "U+2028 ends line comment")};
    expect(lineSep && lineSep->is_number() && lineSep->as_double() == 42,
           "U+2028 terminates line comment");
    auto paragraphSep{parse_ok("// comment\xE2\x80\xA9" "42", "U+2029 ends line comment")};
    expect(paragraphSep && paragraphSep->is_number() && paragraphSep->as_double() == 42,
           "U+2029 terminates line comment");
}

std::string doc_at(std::size_t offset, std::string_view head, std::string_view lead,
                   std::string_view needle, std::string_view rest) {
    std::string doc{head};
    doc.append(offset - head.size() - lead.size(), ' ');
    doc += lead;
    doc += needle;
    doc += rest;
    expect(doc.find(needle) == offset, "doc_at offset");
    return doc;
}

void test_structural_seams() {
    // source: jsonc.test.ts > structural index window seams (all groups)
    constexpr std::size_t WINDOW{8192};
    constexpr std::array<std::size_t, 12> SEAMS{63, 64, 65, 191, 192, 193, 8191, 8192, 8193,
                                                16383, 16384, 16385};
    for (const auto offset : SEAMS) {
        auto stringDoc{doc_at(offset, "{\"k\":", "", "\"seam-value\"", "}")};
        auto parsed{parse_ok(stringDoc, "quote seam")};
        if (parsed) {
            expect_string(*parsed, {"k"}, "seam-value", "quote seam value");
        }
        expect(static_cast<bool>(parse(doc_at(offset, "{\"k\":", "\"ab", "\\n", "cd\"}"))),
               "escape seam");
        auto single{parse_ok(doc_at(offset, "{'k':", "", "'seam'", "}"), "single quote seam")};
        if (single) {
            expect_string(*single, {"k"}, "seam", "single quote seam value");
        }
    }

    constexpr std::array<std::size_t, 3> BOUNDARIES{128, WINDOW, 2 * WINDOW};
    constexpr std::array tokens{std::string_view{"true"}, std::string_view{"false"},
                                std::string_view{"null"}, std::string_view{"-1.25e+10"},
                                std::string_view{"98765.4321e-12"}, std::string_view{"1e3"}};
    for (const auto token : tokens) {
        for (const auto boundary : BOUNDARIES) {
            for (std::size_t start{boundary - token.size()}; start <= boundary + 1; ++start) {
                expect(static_cast<bool>(parse(doc_at(start, "{\"k\":", "", token, "}"))),
                       "token seam");
            }
        }
    }

    constexpr std::string_view escape{"\\u00e9"};
    constexpr std::string_view surrogate{"\\uD83D\\uDE00"};
    for (const auto boundary : BOUNDARIES) {
        for (int delta{-static_cast<int>(escape.size())}; delta <= 1; ++delta) {
            expect(static_cast<bool>(parse(doc_at(boundary + delta, "{\"k\":", "\"ab", escape,
                                                  "cd\"}"))),
                   "unicode escape seam");
        }
        for (int delta{-static_cast<int>(surrogate.size())}; delta <= 1; ++delta) {
            expect(static_cast<bool>(parse(doc_at(boundary + delta, "{\"k\":", "\"ab", surrogate,
                                                  "cd\"}"))),
                   "surrogate seam");
        }
    }

    for (const auto boundary : std::array<std::size_t, 2>{WINDOW, 2 * WINDOW}) {
        auto block{doc_at(boundary - 1, "{\"k\":", "", "/*xxxx*/", "42}")};
        expect(static_cast<bool>(parse(block)), "block-comment seam");
        auto line{doc_at(boundary - 1, "{\"k\":1", "", "//", "xx\n}")};
        expect(static_cast<bool>(parse(line)), "line-comment seam");
        std::string truncated{"{\""};
        truncated.append(boundary - truncated.size(), 'k');
        expect_error(truncated, "truncated key seam");
    }

    for (const auto offset : std::array<std::size_t, 3>{WINDOW - 1, WINDOW, 2 * WINDOW - 1}) {
        expect_error(doc_at(offset, "{\"k\":", "", "\"never-closed", ""), "unterminated seam");
        expect_error(doc_at(offset, "{\"k\":", "\"ab", std::string_view{"\x01", 1}, "cd\"}"),
                     "control seam");
    }

    std::string body(2 * WINDOW + 7, 'z');
    auto longString{parse_ok("{\"k\":\"" + body + body.substr(0, WINDOW) + "\"}", "3-window string")};
    expect(longString && longString->get("k") && longString->get("k")->as_string().size() ==
                                                 body.size() + WINDOW,
           "3-window string value");

    // source: jsonc.test.ts > matches JSON.parse across 64-byte block boundaries
    for (int padding{40}; padding <= 96; ++padding) {
        const std::string text(static_cast<std::size_t>(padding), 'a');
        auto object{parse_ok("{\"" + text + "\":\"x\",\"k\":\"" + text + "\\n\"}",
                             "64-byte object boundary")};
        if (object) {
            expect_string(*object, {"k"}, text + "\n", "64-byte escaped value");
        }
        auto slash{parse_ok("[\"" + text + "\\\\\"]", "64-byte slash boundary")};
        if (slash) {
            expect_string(*slash, {std::size_t{0}}, text + "\\", "64-byte slash value");
        }
        auto spaced{parse_ok("[" + std::string(static_cast<std::size_t>(padding), ' ') + "1]",
                             "64-byte whitespace boundary")};
        if (spaced) {
            expect_number(*spaced, {std::size_t{0}}, 1, "64-byte whitespace value");
        }
        expect(static_cast<bool>(parse("{\"k\":\"" + text + "\"}")), "64-byte plain string");
        auto unicode{parse_ok("\"" + text + "\\u00e9\"", "64-byte unicode boundary")};
        expect(unicode && unicode->is_string() && unicode->as_string() == text + "é",
               "64-byte unicode value");
    }

    // source: jsonc.test.ts > closing brace at boundary followed by trailing garbage
    for (const auto offset : std::array<std::size_t, 4>{WINDOW - 1, WINDOW, 2 * WINDOW - 1,
                                                        2 * WINDOW}) {
        auto trailing{parse_ok(doc_at(offset, "{\"k\":1", "", "}", "@@@@"), "trailing garbage")};
        if (trailing) {
            expect_number(*trailing, {"k"}, 1, "valid root wins over trailing garbage");
        }
    }
}

std::string minified_doc_of_length(std::size_t length) {
    std::vector<std::string> parts;
    std::size_t size{1};
    int index{};
    while (size + 40 < length) {
        std::string piece{std::format("\"k{:06}\":{},", index, index % 997)};
        size += piece.size();
        parts.push_back(std::move(piece));
        ++index;
    }
    const std::size_t fixed{std::string_view{"\"pad\":\"\"}"}.size()};
    std::string doc{"{"};
    for (const auto& part : parts) {
        doc += part;
    }
    doc += "\"pad\":\"";
    doc.append(length - size - fixed, 'p');
    doc += "\"}";
    expect(doc.size() == length, "exact window document length");
    return doc;
}

void test_exact_window_documents() {
    // source: jsonc.test.ts > documents sized around window multiples / BOM past first window
    constexpr std::size_t WINDOW{8192};
    for (const auto length : std::array<std::size_t, 7>{WINDOW - 1, WINDOW, WINDOW + 1, 3 * WINDOW,
                                                        8 * WINDOW - 1, 8 * WINDOW, 8 * WINDOW + 1}) {
        expect(static_cast<bool>(parse(minified_doc_of_length(length))), "exact window document parses");
    }
    std::string body;
    int index{};
    while (body.size() < 3 * WINDOW) {
        body += std::format("\"k{}\":{},\n", index++, index);
    }
    auto bom{parse_ok("\xEF\xBB\xBF{\n" + body + "\"z\":0 // trailing comment\n}\n",
                      "BOM and late comment")};
    expect(bom && bom->get("z") && bom->get("z")->as_double() == 0, "BOM late comment value");
    auto single{parse_ok("\xEF\xBB\xBF{\n" + body + "'z':'x'\n}\n", "BOM late single quote")};
    if (single) {
        expect_string(*single, {"z"}, "x", "BOM late single quote value");
    }
}

void test_large_document() {
    // source: jsonc.test.ts > huge documents with every value type
    std::string doc{"{"};
    for (int i{}; i < 5000; ++i) {
        if (i) {
            doc += ',';
        }
        doc += std::format("\"key_{}\":", i);
        if (i % 5 == 0) {
            doc += std::format("{{\"nested\":[{},\"{}\",null,{}],\"deeper\":{{\"x\":\"é🚀{}\"}}}}",
                               i, i, i % 2 == 0 ? "true" : "false", i);
        } else if (i % 3 == 0) {
            doc += std::format("\"value with \\\"escapes\\\" and \\\\ backslashes {}\"", i);
        } else {
            doc += std::format("{}", i * 1.5);
        }
    }
    doc += '}';
    expect(doc.size() > 16 * 8192, "huge source size");
    auto root{parse_ok(doc, "huge document")};
    if (root) {
        expect(root->size() == 5000, "huge object size");
        expect_string(*root, {"key_3"}, "value with \"escapes\" and \\ backslashes 3",
                      "huge escaped string");
        expect_string(*root, {"key_4995", "deeper", "x"}, "é🚀4995", "huge unicode tail");
    }
}

void test_upstream_json_corpus() {
    // source: json-test-suite.test.ts — all 318 vendored JSONTestSuite cases.
    expect(JSON_CORPUS_CASES.size() == 318, "JSONTestSuite corpus completeness");
    for (const auto& item : JSON_CORPUS_CASES) {
        const auto value{parse(item.source)};
        if (item.outcome == 0) {
            expect(!value, item.name);
        } else if (item.outcome == 1) {
            expect(value && canonical_hash(*value) == item.expectedHash, item.name);
        } else {
            ++gChecks; // upstream imposes no Bun assertion when JSON.parse rejects.
        }
    }
}

void test_upstream_differential_fuzz() {
    // source: json-differential-fuzz.test.ts, seed 0x6a736f6e.
    // Expected hashes were generated offline by JSC JSON.parse, not by mbun.
    expect(JSON_GENERATED_CASES.size() == 800, "400 plain+decorated differential documents");
    for (std::size_t i{}; i < JSON_GENERATED_CASES.size(); ++i) {
        const auto& item{JSON_GENERATED_CASES[i]};
        const auto value{parse(item.source)};
        expect(value && canonical_hash(*value) == item.expectedHash,
               std::format("generated/decorated JSON differential oracle {}", i));
    }
    expect(JSON_MUTANT_CASES.size() == 1500, "1500 differential mutants");
    for (std::size_t i{}; i < JSON_MUTANT_CASES.size(); ++i) {
        const auto& item{JSON_MUTANT_CASES[i]};
        const auto value{parse(item.source)};
        if (item.required) {
            expect(value && canonical_hash(*value) == item.expectedHash,
                   std::format("JSON.parse-accepted mutant differential oracle {}", i));
        } else {
            ++gChecks; // Bun may accept JSONC extensions or reject; Value is always serializable.
        }
    }
}

} // namespace

int main() {
    test_basics();
    test_errors_and_depth();
    test_generated_differential();
    test_unicode_and_escapes();
    test_structural_seams();
    test_exact_window_documents();
    test_large_document();
    test_upstream_json_corpus();
    test_upstream_differential_fuzz();
    std::println("test_jsonc: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
