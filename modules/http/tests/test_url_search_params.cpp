// test_url_search_params.cpp — T3.6 URLSearchParams pure-logic core.
//
// Sources (assertion semantics preserved):
//   - test/js/node/test/fixtures/url-searchparams.js: complete 47-case
//     application/x-www-form-urlencoded fixture (no selective sampling).
//   - test/js/node/test/parallel/test-whatwg-url-custom-searchparams.js:
//     append/get/getAll/has/set/delete/iteration and serialization behavior.
//   - test/js/node/test/parallel/test-whatwg-url-custom-searchparams-sort.js:
//     stable sorting, including the >100-pair path.
//   - test/js/web/html/URLSearchParams.test.ts: value-selective delete/has and
//     USVString/lone-surrogate outcomes represented at the UTF-8 core boundary.
//
// DEFERRED(S1/JSC binding): JS value coercion and Get interleaving, DOM URL
// association/update, prototype descriptors, iterator branding, Bun.inspect,
// toJSON object materialization, and direct JS execution of the source files.

import std;
import mbun.http.url_search_params;

namespace {

using mbun::http::UrlSearchParams;

int gChecks{0};
int gFailures{0};

void fail(std::string_view what) {
    ++gFailures;
    if (gFailures <= 30) {
        std::println("  FAIL {}", what);
    }
}

template <typename A, typename B>
void eq(const A& actual, const B& expected, std::string_view what) {
    ++gChecks;
    if (!(actual == expected)) {
        fail(what);
    }
}

void yes(bool value, std::string_view what) {
    ++gChecks;
    if (!value) {
        fail(what);
    }
}

void eq_string(std::string_view actual, std::string_view expected, std::string_view what) {
    ++gChecks;
    if (actual != expected) {
        fail(std::format("{}: got [{}], expected [{}]", what, actual, expected));
    }
}

struct Fixture {
    std::string_view input;
    std::string_view serialized;
    // Own the table container. An initializer_list member would retain a view
    // into a temporary backing array and becomes dangling under LLVM/libc++.
    std::vector<std::pair<std::string_view, std::string_view>> pairs;
};

// Complete data table from node's fixtures/url-searchparams.js.
const std::array FIXTURES{
    Fixture{"", "", {}},
    Fixture{"foo=918854443121279438895193", "foo=918854443121279438895193", {{"foo", "918854443121279438895193"}}},
    Fixture{"foo=bar", "foo=bar", {{"foo", "bar"}}},
    Fixture{"foo=bar&foo=quux", "foo=bar&foo=quux", {{"foo", "bar"}, {"foo", "quux"}}},
    Fixture{"foo=1&bar=2", "foo=1&bar=2", {{"foo", "1"}, {"bar", "2"}}},
    Fixture{"my%20weird%20field=q1!2%22'w%245%267%2Fz8)%3F", "my+weird+field=q1%212%22%27w%245%267%2Fz8%29%3F", {{"my weird field", "q1!2\"'w$5&7/z8)?"}}},
    Fixture{"foo%3Dbaz=bar", "foo%3Dbaz=bar", {{"foo=baz", "bar"}}},
    Fixture{"foo=baz=bar", "foo=baz%3Dbar", {{"foo", "baz=bar"}}},
    Fixture{"str=foo&arr=1&somenull&arr=2&undef=&arr=3", "str=foo&arr=1&somenull=&arr=2&undef=&arr=3", {{"str", "foo"}, {"arr", "1"}, {"somenull", ""}, {"arr", "2"}, {"undef", ""}, {"arr", "3"}}},
    Fixture{" foo = bar ", "+foo+=+bar+", {{" foo ", " bar "}}},
    Fixture{"foo=%zx", "foo=%25zx", {{"foo", "%zx"}}},
    Fixture{"foo=%EF%BF%BD", "foo=%EF%BF%BD", {{"foo", "�"}}},
    Fixture{"foo&bar=baz", "foo=&bar=baz", {{"foo", ""}, {"bar", "baz"}}},
    Fixture{"a=b&c&d=e", "a=b&c=&d=e", {{"a", "b"}, {"c", ""}, {"d", "e"}}},
    Fixture{"a=b&c=&d=e", "a=b&c=&d=e", {{"a", "b"}, {"c", ""}, {"d", "e"}}},
    Fixture{"a=b&=c&d=e", "a=b&=c&d=e", {{"a", "b"}, {"", "c"}, {"d", "e"}}},
    Fixture{"a=b&=&d=e", "a=b&=&d=e", {{"a", "b"}, {"", ""}, {"d", "e"}}},
    Fixture{"&&foo=bar&&", "foo=bar", {{"foo", "bar"}}},
    Fixture{"&", "", {}},
    Fixture{"&&&&", "", {}},
    Fixture{"&=&", "=", {{"", ""}}},
    Fixture{"&=&=", "=&=", {{"", ""}, {"", ""}}},
    Fixture{"=", "=", {{"", ""}}},
    Fixture{"+", "+=", {{" ", ""}}},
    Fixture{"+=", "+=", {{" ", ""}}},
    Fixture{"+&", "+=", {{" ", ""}}},
    Fixture{"=+", "=+", {{"", " "}}},
    Fixture{"+=&", "+=", {{" ", ""}}},
    Fixture{"a&&b", "a=&b=", {{"a", ""}, {"b", ""}}},
    Fixture{"a=a&&b=b", "a=a&b=b", {{"a", "a"}, {"b", "b"}}},
    Fixture{"&a", "a=", {{"a", ""}}},
    Fixture{"&=", "=", {{"", ""}}},
    Fixture{"a&a&", "a=&a=", {{"a", ""}, {"a", ""}}},
    Fixture{"a&a&a&", "a=&a=&a=", {{"a", ""}, {"a", ""}, {"a", ""}}},
    Fixture{"a&a&a&a&", "a=&a=&a=&a=", {{"a", ""}, {"a", ""}, {"a", ""}, {"a", ""}}},
    Fixture{"a=&a=value&a=", "a=&a=value&a=", {{"a", ""}, {"a", "value"}, {"a", ""}}},
    Fixture{"foo%20bar=baz%20quux", "foo+bar=baz+quux", {{"foo bar", "baz quux"}}},
    Fixture{"+foo=+bar", "+foo=+bar", {{" foo", " bar"}}},
    Fixture{"a+", "a+=", {{"a ", ""}}},
    Fixture{"=a+", "=a+", {{"", "a "}}},
    Fixture{"a+&", "a+=", {{"a ", ""}}},
    Fixture{"=a+&", "=a+", {{"", "a "}}},
    Fixture{"%20+", "++=", {{"  ", ""}}},
    Fixture{"=%20+", "=++", {{"", "  "}}},
    Fixture{"%20+&", "++=", {{"  ", ""}}},
    Fixture{"=%20+&", "=++", {{"", "  "}}},
    Fixture{"foo=%©ar&baz=%A©uux&xyzzy=%©ud", "foo=%25%C2%A9ar&baz=%25A%C2%A9uux&xyzzy=%25%C2%A9ud", {{"foo", "%©ar"}, {"baz", "%A©uux"}, {"xyzzy", "%©ud"}}},
    Fixture{"a=1&b=2&a=3", "a=1&b=2&a=3", {{"a", "1"}, {"b", "2"}, {"a", "3"}}},
    Fixture{"?a", "%3Fa=", {{"?a", ""}}},
};

void test_fixtures() {
    for (std::size_t i{0}; i < FIXTURES.size(); ++i) {
        const auto& fixture{FIXTURES[i]};
        const std::array prefixes{false, true};
        for (bool leadingQuestion : prefixes) {
            // Exact branch from test-whatwg-url-custom-searchparams.js: the
            // bare constructor case is omitted when fixture input starts '?'.
            if (!leadingQuestion && fixture.input.starts_with('?')) {
                continue;
            }
            std::string input{leadingQuestion ? "?" : ""};
            input.append(fixture.input);
            UrlSearchParams params{input};
            eq_string(params.to_string(), fixture.serialized, std::format("fixture {} serialize", i));
            eq(params.size(), fixture.pairs.size(), std::format("fixture {} size", i));
            std::size_t pairIndex{0};
            for (const auto& expected : fixture.pairs) {
                eq(params.pairs()[pairIndex].name, expected.first, std::format("fixture {} name {}", i, pairIndex));
                eq(params.pairs()[pairIndex].value, expected.second, std::format("fixture {} value {}", i, pairIndex));
                ++pairIndex;
            }
        }
    }
}

void test_mutation_and_queries() {
    UrlSearchParams params{"a=1&a=2&b=3"};
    yes(params.has("a"), "has name");
    yes(params.has("a", "2"), "has name/value");
    yes(!params.has("a", "3"), "has rejects value");
    eq(params.get("a").value_or(""), "1", "get first");
    eq(params.get_all("a"), std::vector<std::string>({"1", "2"}), "get all");

    params.set("a", "updated");
    eq(params.to_string(), "a=updated&b=3", "set replaces first and removes rest");
    params.append("a", "last");
    eq(params.to_string(), "a=updated&b=3&a=last", "append preserves order");
    params.remove("a", "updated");
    eq(params.to_string(), "b=3&a=last", "value-selective delete");
    params.remove("a");
    eq(params.to_string(), "b=3", "name delete");
    params.remove("missing");
    eq(params.to_string(), "b=3", "missing delete no-op");
}

void test_encoding_and_usv_boundary() {
    UrlSearchParams params{};
    params.append("emoji", "😀");
    params.append("replacement", "a�b");
    eq_string(params.to_string(), "emoji=%F0%9F%98%80&replacement=a%EF%BF%BDb", "unicode encode");
    UrlSearchParams roundTrip{params.to_string()};
    eq(roundTrip.get("emoji").value_or(""), "😀", "emoji round trip");
    eq(roundTrip.get("replacement").value_or(""), "a�b", "replacement round trip");

    // WebIDL converts JS lone surrogates before reaching this UTF-8 core. Raw
    // percent-decoded bytes instead follow the WHATWG UTF-8 decoder's maximal
    // subpart rules (verified against bun-zig and bun-rust).
    const std::array invalidPercent{
        std::pair{"a=%E2%82", "a=%EF%BF%BD"},
        std::pair{"a=%E2%28%A1", "a=%EF%BF%BD%28%EF%BF%BD"},
        std::pair{"a=%ED%A0%80", "a=%EF%BF%BD%EF%BF%BD%EF%BF%BD"},
        std::pair{"a=%F0%9F%98", "a=%EF%BF%BD"},
        std::pair{"a=%C0%AF", "a=%EF%BF%BD%EF%BF%BD"},
        std::pair{"a=%80%80", "a=%EF%BF%BD%EF%BF%BD"},
    };
    for (const auto& [input, expected] : invalidPercent) {
        eq_string(UrlSearchParams{input}.to_string(), expected, "invalid percent UTF-8");
    }
}

void test_stable_sort() {
    UrlSearchParams params{"z=a&=b&c=d&a=first&a=second"};
    params.sort();
    eq(params.to_string(), "=b&a=first&a=second&c=d&z=a", "stable sort");

    // URLSearchParams sorting is UTF-16 code-unit order. Bun Zig 1.3.14 and
    // bun-rust both order astral U+10000 (lead surrogate D800) before U+E000,
    // which differs from UTF-8 byte/code-point order.
    UrlSearchParams unicode{};
    unicode.append("", "bmp");       // U+E000
    unicode.append("𐀀", "astral");  // U+10000
    unicode.sort();
    eq(unicode.pairs()[0].name, "𐀀", "sort uses UTF-16 code units");

    UrlSearchParams large{};
    for (int i{109}; i >= 0; --i) {
        large.append(std::format("a{:03}", i), "b");
    }
    large.sort();
    eq(large.size(), std::size_t{110}, "large sort size");
    for (int i{0}; i < 110; ++i) {
        eq(large.pairs()[static_cast<std::size_t>(i)].name, std::format("a{:03}", i), "large sort order");
    }
}

void test_self_alias_safety() {
    const std::string longQuery{
        "first=abcdefghijklmnopqrstuvwxyz0123456789&second=ABCDEFGHIJKLMNOPQRSTUVWXYZ9876543210"};
    {
        UrlSearchParams params{};
        params.append("source", longQuery);
        const std::string_view aliasedInit{params.pairs()[0].value};
        params.parse(aliasedInit);
        eq(params.get("first").value_or(""), "abcdefghijklmnopqrstuvwxyz0123456789",
           "parse accepts pairs value alias");
        eq(params.get("second").value_or(""), "ABCDEFGHIJKLMNOPQRSTUVWXYZ9876543210",
           "parse keeps full aliased input");
    }
    {
        UrlSearchParams params{};
        params.append("source", longQuery);
        const std::string_view aliasedInit{*params.get("source")};
        params.parse(aliasedInit);
        eq(params.size(), std::size_t{2}, "parse accepts get view alias");
    }

    const std::string longName(96, 'n');
    const std::string longValue(96, 'v');
    {
        UrlSearchParams params{};
        params.append(longName, "first");
        params.append("keep", "middle");
        params.append(longName, "last");
        const std::string_view aliasedName{params.pairs()[0].name};
        params.remove(aliasedName);
        eq(params.to_string(), "keep=middle", "remove name accepts self alias");
    }
    {
        UrlSearchParams params{};
        params.append(longName, longValue);
        params.append("keep", "middle");
        params.append(longName, longValue);
        const std::string_view aliasedName{params.pairs()[0].name};
        const std::string_view aliasedValue{params.pairs()[0].value};
        params.remove(aliasedName, aliasedValue);
        eq(params.to_string(), "keep=middle", "remove name/value accepts self aliases");
    }
}

}  // namespace

int main() {
    test_fixtures();
    test_mutation_and_queries();
    test_encoding_and_usv_boundary();
    test_stable_sort();
    test_self_alias_safety();
    std::println("URLSearchParams: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
