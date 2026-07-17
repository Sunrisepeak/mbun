// test_ini.cpp — T2.2 bun INI pure-parser vectors (S0).
//
// source: test/js/bun/ini/ini.test.ts > describe("parse ini")
// source: test/js/bun/ini/foo.ini > bigboi fixture
//
// DEFERRED(S1 / consumers): `bun:internal-for-testing` export wiring, subprocess
// environment isolation, npmrc discovery/precedence, registry/install behavior.
// All pure parser assertions, including env substitution and malformed bytes, are here.
import std;
import mbun.config.ini;
import mbun.config.value;

namespace {

using mbun::config::Value;
using mbun::config::ini::parse;

int gChecks{};
int gFailures{};

void expect(bool condition, std::string_view label) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        if (gFailures <= 40) {
            std::println("  FAIL {}", label);
        }
    }
}

using Env = std::unordered_map<std::string, std::string>;

std::optional<Value> parse_ok(std::string_view source, const Env& env, std::string_view label) {
    auto result{parse(source, [&env](std::string_view key) -> std::optional<std::string_view> {
        const auto it{env.find(std::string{key})};
        return it == env.end() ? std::nullopt : std::optional<std::string_view>{it->second};
    })};
    ++gChecks;
    if (!result) {
        ++gFailures;
        if (gFailures <= 40) {
            std::println("  FAIL {}: {} at {}", label, result.error().message, result.error().offset);
        }
        return std::nullopt;
    }
    return std::move(*result);
}

std::optional<Value> parse_ok(std::string_view source, std::string_view label) {
    return parse_ok(source, {}, label);
}

const Value* nav(const Value& root, std::initializer_list<std::string_view> path) {
    const Value* value{&root};
    for (const auto key : path) {
        value = value->get(key);
        if (!value) {
            return nullptr;
        }
    }
    return value;
}

void expect_string(const Value& root, std::initializer_list<std::string_view> path,
                   std::string_view expected, std::string_view label) {
    const Value* value{nav(root, path)};
    expect(value && value->is_string() && value->as_string() == expected, label);
}

void test_sections_and_values() {
    // source: ini.test.ts > weird/really long/unicode/basic/basic sections/key-section
    auto weird{parse_ok("[foo\\]]\nlol = true\n", "weird section")};
    if (weird) {
        const Value* key{weird->get("[foo\\]]")};
        expect(key && key->is_boolean() && key->as_bool(), "weird section becomes true key");
        const Value* lol{weird->get("lol")};
        expect(lol && lol->is_boolean() && lol->as_bool(), "weird section lol");
    }

    std::string longName(1024, 'a');
    auto longDoc{parse_ok("[" + longName + ".lol.this.be.long]\n wow='hi'\n", "long section")};
    if (longDoc) {
        expect_string(*longDoc, {longName, "lol", "this", "be", "long", "wow"}, "hi",
                      "long nested section");
    }

    auto unicode{parse_ok("hi👋lol='lol hi 👋'\n[😎.🫒.🤦‍♀️]\nlol='wtf'\n", "unicode")};
    if (unicode) {
        expect_string(*unicode, {"hi👋lol"}, "lol hi 👋", "unicode key/value");
        expect_string(*unicode, {"😎", "🫒", "🤦‍♀️", "lol"}, "wtf", "unicode section");
    }

    auto basic{parse_ok("hello='friends'\n[foo]\nbar='baz'\n", "basic sections")};
    if (basic) {
        expect_string(*basic, {"hello"}, "friends", "basic root");
        expect_string(*basic, {"foo", "bar"}, "baz", "basic section");
    }
    auto collision{parse_ok("foo='hihihi'\n[foo]\nisbar='lol'\n", "key then section")};
    if (collision) {
        expect_string(*collision, {"foo"}, "hihihi", "scalar blocks section mutation");
        expect(collision->size() == 1, "scalar collision keeps one key");
    }
}

void test_env() {
    // source: ini.test.ts > env vars (all table entries)
    struct Case {
        std::string_view name;
        std::string_view ini;
        Env env;
        std::string_view expected;
    };
    const std::vector<Case> cases{
        {"multiple", "hi=${FOO}${BAR}", {{"FOO", "bar"}, {"BAR", "baz"}}, "barbaz"},
        {"mixed optional", "hi=${FOO?}${BAZ?}", {{"FOO", "bar"}}, "bar"},
        {"escaped", R"(hi=\${FOO})", {{"FOO", "bar"}}, "${FOO}"},
        {"double escape", R"(hi=\\${FOO})", {{"FOO", "bar"}}, R"(\bar)"},
        {"triple escape", R"(hi=\\\${FOO})", {{"FOO", "bar"}}, R"(\${FOO})"},
        {"missing", "hi=${BAZ}", {{"FOO", "bar"}}, "${BAZ}"},
        {"escaped missing", R"(hi=\${BAZ})", {}, "${BAZ}"},
        {"double missing", R"(hi=\\${BAZ})", {}, R"(\${BAZ})"},
        {"escaped optional", R"(hi=\${FOO?})", {{"FOO", "bar"}}, "${FOO?}"},
        {"double optional", R"(hi=\\${FOO?})", {{"FOO", "bar"}}, R"(\bar)"},
        {"missing optional", "hi=${BAZ?}", {}, ""},
        {"escaped missing optional", R"(hi=\${BAZ?})", {}, "${BAZ?}"},
        {"double missing optional", R"(hi=\\${BAZ?})", {}, R"(\)"},
        {"windows paths", R"(hi=C:\Home\user\Documents)", {}, R"(C:\Home\user\Documents)"},
        {"prefix", "hi=greeting: ${LOL}", {{"LOL", "hi"}}, "greeting: hi"},
        {"nested", "hi=greeting: ${what${LOL}lol}", {{"LOL", "hi"}}, "greeting: ${whathilol}"},
        {"nested2", "hi=greeting: ${what${omg${LOL}why}lol}", {{"LOL", "hi"}},
         "greeting: ${what${omghiwhy}lol}"},
        // source: bun Rust src/ini/lib.rs expand_env_vars (quoted path uses balanced outer name)
        {"quoted nested preserves missing outer", R"(hi="${what${LOL}lol}")", {{"LOL", "hi"}},
         "${what${LOL}lol}"},
        {"quoted nested optional outer", R"(hi="${what${LOL}lol?}")", {{"LOL", "hi"}}, ""},
        {"quoted nested exact outer name", R"(hi="${what${LOL}lol}")",
         {{"LOL", "inner"}, {"what${LOL}lol", "outer"}}, "outer"},
        {"unclosed", "hi=greeting: ${LOL", {{"LOL", "hi"}}, "greeting: ${LOL"},
        {"double quoted", R"(hi="${LOL}")", {{"LOL", "hi"}}, "hi"},
        {"single quoted", R"(hi='${LOL}')", {{"LOL", "hi"}}, "hi"},
        {"quoted prefix", R"(hi="Bearer ${TOKEN}")", {{"TOKEN", "secret123"}}, "Bearer secret123"},
        {"quoted missing", R"(hi="${NOTFOUND}")", {}, "${NOTFOUND}"},
        {"quoted optional missing", R"(hi="${NOTFOUND?}")", {}, ""},
        {"quoted optional value", R"(hi="${TOKEN?}")", {{"TOKEN", "secret"}}, "secret"},
        {"single optional missing", R"(hi='${NOTFOUND?}')", {}, ""},
        {"unquoted optional prefix", "hi=Bearer ${TOKEN?}", {}, "Bearer "},
        {"quoted optional prefix", R"(hi="Bearer ${TOKEN?}")", {}, "Bearer "},
        {"quoted backslash", R"(hi="\\${LOL}")", {{"LOL", "hi"}}, R"(\hi)"},
    };
    for (const auto& item : cases) {
        auto root{parse_ok(item.ini, item.env, item.name)};
        if (root) {
            expect_string(*root, {"hi"}, item.expected, item.name);
        }
    }
}

void test_empty_and_duplicates() {
    // source: ini.test.ts > empty single-quoted value / duplicate properties
    const std::vector<std::pair<std::string_view, std::string_view>> emptyCases{
        {"a='", "a"}, {"a=''", "a"}, {"'=x", ""}, {"''=x", ""},
    };
    for (const auto& [source, key] : emptyCases) {
        auto root{parse_ok(source, "empty quote")};
        if (root) {
            expect_string(*root, {key}, source.starts_with("a") ? "" : "x", "empty quote value");
        }
    }
    auto sectionEmpty{parse_ok("[']\nx=1", "empty section")};
    if (sectionEmpty) {
        expect_string(*sectionEmpty, {"", "x"}, "1", "empty section name");
    }
    auto noCycle{parse_ok("='\n[]\n='", "self reference fuzz")};
    if (noCycle) {
        expect_string(*noCycle, {""}, "", "self-reference fuzz value");
    }
    auto collision{parse_ok("a=''\n[a]\nhello=world", "empty scalar section collision")};
    if (collision) {
        expect_string(*collision, {"a"}, "", "empty scalar preserved");
    }

    auto dup{parse_ok("zr[]=deedee\nzr=123\nar[]=one\nar[]=three\nstr=3\nbrr=1\nbrr=2\nbrr=3\n",
                      "duplicates")};
    if (dup) {
        const Value* zr{dup->get("zr")};
        const Value* ar{dup->get("ar")};
        expect(zr && zr->is_array() && zr->size() == 2, "array plus scalar appends");
        if (zr && zr->is_array()) {
            expect(zr->at(0).as_string() == "deedee" && zr->at(1).as_string() == "123",
                   "zr values");
        }
        expect(ar && ar->is_array() && ar->size() == 2, "repeated array");
        expect_string(*dup, {"brr"}, "3", "duplicate scalar last wins");
    }
}

constexpr std::string_view FOO_INI{R"INI(o = p

   a with spaces   =     b  c
" xa  n          p " = "\"\r\nyoyoyo\r\r\n"
"[disturbing]" = hey you never know
s = 'something'
s1 = "something'
s2 = "something else"
s3 =
s4 =
s5 = '   '
s6 = ' a '
s7
true = true
false = false
null = null
undefined = undefined
zr[] = deedee
ar[] = one
ar[] = three
ar = this is included
br = cold
br = warm
eq = "eq=eq"
[a]
av = a val
e = { o: p, a: { av: a val, b: { c: { e: "this [value]" } } } }
j = "{ o: "p", a: { av: "a val", b: { c: { e: "this [value]" } } } }"
"[]" = a square?
cr[] = four
cr[] = eight
[b]
[a.b.c]
e = 1
j = 2
[x\.y\.z]
x.y.z = xyz
[x\.y\.z.a\.b\.c]
a.b.c = abc
nocomment = this\; this is not a comment
noHashComment = this\# this is not a comment
)INI"};

void test_fixture_and_weird_keys() {
    // source: ini.test.ts > bigboi / matches stupid npm/ini behavior
    auto root{parse_ok(FOO_INI, "foo.ini")};
    if (root) {
        expect_string(*root, {"o"}, "p", "fixture.o");
        expect_string(*root, {"a with spaces"}, "b  c", "fixture spaced key");
        expect_string(*root, {" xa  n          p "}, "\"\r\nyoyoyo\r\r\n", "fixture escaped value");
        expect_string(*root, {"a", "b", "c", "e"}, "1", "fixture nested");
        expect_string(*root, {"x.y.z", "a.b.c", "nocomment"}, "this; this is not a comment",
                      "fixture escaped semicolon");
        expect_string(*root, {"x.y.z", "a.b.c", "noHashComment"}, "this# this is not a comment",
                      "fixture escaped hash");
        const Value* ar{root->get("ar")};
        expect(ar && ar->is_array() && ar->size() == 3, "fixture mixed array");
        const Value* null{root->get("null")};
        expect(null && null->is_null(), "fixture null");
    }

    auto objectKey{parse_ok("'{ \"what\": \"is this\" }'=seriously?", "npm object key")};
    if (objectKey) {
        expect_string(*objectKey, {"[Object object]"}, "seriously?", "npm object coercion");
    }
    auto arrayKey{parse_ok("'[1, 2, 3]'=cmon man", "npm array key")};
    if (arrayKey) {
        expect_string(*arrayKey, {"1,2,3"}, "cmon man", "npm array coercion");
    }

    // source: bun src/ini/lib.rs prepare_str > parse_utf8_impl::<true> fallback
    auto trailingObject{parse_ok(R"(x='{ "a": 1, }')", "strict quoted JSON value")};
    if (trailingObject) {
        expect_string(*trailingObject, {"x"}, R"({ "a": 1, })",
                      "invalid strict JSON remains a string");
    }
    auto trailingKey{parse_ok(R"('{ "a": 1, }'=value)", "strict quoted JSON key")};
    if (trailingKey) {
        expect_string(*trailingKey, {R"({ "a": 1, })"}, "value",
                      "invalid strict JSON key is not object-coerced");
    }
}

void test_invalid_utf8_is_safe() {
    // source: ini.test.ts > truncated/invalid utf-8 (all nine cases)
    const std::vector<std::string> suffixes{
        std::string{"\x80", 1}, std::string{"\xC0", 1}, std::string{"\xE0", 1},
        std::string{"\xE0\x80", 2}, std::string{"\xF0", 1}, std::string{"\xF0\x80", 2},
        std::string{"\xF0\x80\x80", 3}, std::string{"\\\xC0", 2}, std::string{"\\\x80", 2},
    };
    for (const auto& suffix : suffixes) {
        expect(static_cast<bool>(parse("key = " + suffix)), "invalid/truncated UTF-8 does not crash");
    }
}

} // namespace

int main() {
    test_sections_and_values();
    test_env();
    test_empty_and_duplicates();
    test_fixture_and_weird_keys();
    test_invalid_utf8_is_safe();
    std::println("test_ini: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
