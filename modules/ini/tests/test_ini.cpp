// test_ini.cpp — mbun.ini core parser smoke checks.
//
// Behavior mirrors bun's src/ini/lib.rs. These are hand-written sanity vectors
// for the pure-logic port (sections, key=value, comments, quotes/JSON, arrays,
// escapes, ${VAR} env expansion); the full npmrc/install suite is
// DEFERRED(S-install) with that layer.
import std;
import mbun.ini;

namespace {

using namespace mbun::ini;

int gChecks{0};
int gFailures{0};

void check(bool ok, std::string_view what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::println("FAIL: {}", what);
    }
}

// Render a scalar leaf via to_string; helper to fetch a path a.b.c.
const Value* dig(const Value& root, std::initializer_list<std::string_view> path) {
    const Value* cur = &root;
    for (auto k : path) {
        if (cur == nullptr) {
            return nullptr;
        }
        cur = cur->get(k);
    }
    return cur;
}

void expect_str(const Value& root, std::initializer_list<std::string_view> path,
                std::string_view expected, std::string_view what) {
    const Value* v = dig(root, path);
    check(v != nullptr && to_string(*v) == expected, what);
}

}  // namespace

int main() {
    // Basic key=value + comments + section.
    {
        Value r = parse("a = 1\n; comment\n# comment2\n[sec]\nb = hello\n");
        expect_str(r, {"a"}, "1", "basic scalar a=1");
        expect_str(r, {"sec", "b"}, "hello", "section value");
    }

    // Bare key -> true; true/false/null coercion.
    {
        Value r = parse("flag\nt = true\nf = false\nn = null\n");
        expect_str(r, {"flag"}, "true", "bare key true");
        expect_str(r, {"t"}, "true", "true literal");
        expect_str(r, {"f"}, "false", "false literal");
        expect_str(r, {"n"}, "null", "null literal");
    }

    // Dotted section header nests.
    {
        Value r = parse("[a.b.c]\nk = v\n");
        expect_str(r, {"a", "b", "c", "k"}, "v", "dotted section nests");
    }

    // key[] arrays.
    {
        Value r = parse("x[] = 1\nx[] = 2\nx[] = 3\n");
        const Value* x = dig(r, {"x"});
        check(x != nullptr && x->is_array() && x->size() == 3, "array length 3");
        check(x != nullptr && x->is_array() && to_string(x->array()[0]) == "1" &&
                  to_string(x->array()[2]) == "3",
              "array elements");
    }

    // Double-quoted -> JSON string (the quotes make it a JSON string literal).
    {
        Value r = parse("s = \"hello world\"\nport = \"8080\"\n");
        expect_str(r, {"s"}, "hello world", "quoted string");
        const Value* p = dig(r, {"port"});
        check(p != nullptr && p->is_string(), "double-quoted number stays string");
        expect_str(r, {"port"}, "8080", "double-quoted number to_string");
    }

    // Single-quoted content is JSON-parsed after stripping quotes: array/number.
    {
        Value r = parse("arr = '[1, 2, 3]'\n");
        const Value* a = dig(r, {"arr"});
        check(a != nullptr && a->is_array() && a->size() == 3, "single-quoted JSON array");
        check(a != nullptr && a->is_array() && a->array()[0].is_number() &&
                  to_string(a->array()[0]) == "1",
              "JSON array numbers");
    }

    // Single-quoted stays literal (JSON parse fails -> string path).
    {
        Value r = parse("s = 'raw ${X}'", [](std::string_view) -> std::optional<std::string> {
            return std::nullopt;  // X undefined -> left as-is
        });
        expect_str(r, {"s"}, "raw ${X}", "single-quoted literal, missing env kept");
    }

    // ${VAR} env expansion (defined), ${VAR?} optional (undefined -> empty).
    {
        EnvLookup env = [](std::string_view n) -> std::optional<std::string> {
            if (n == "USER") {
                return "alice";
            }
            return std::nullopt;
        };
        Value r = parse("u = ${USER}\nx = a${MISSING?}b\ny = ${MISSING}\n", env);
        expect_str(r, {"u"}, "alice", "env defined expands");
        expect_str(r, {"x"}, "ab", "optional missing -> empty");
        expect_str(r, {"y"}, "${MISSING}", "required missing -> kept");
    }

    // Escaped comment char is retained literally; a full-line comment is
    // skipped. (bun only strips an inline comment when the value also escapes.)
    {
        Value r = parse("; whole line\nb = foo\\;bar\n");
        expect_str(r, {"b"}, "foo;bar", "escaped semicolon retained");
        check(dig(r, {""}) == nullptr, "comment line produced no key");
    }

    // __proto__ key is skipped.
    {
        Value r = parse("__proto__ = evil\nok = 1\n");
        check(dig(r, {"__proto__"}) == nullptr, "__proto__ skipped");
        expect_str(r, {"ok"}, "1", "sibling after __proto__");
    }

    // ── npmrc semantic layer ────────────────────────────────────────────────

    // base64 decode: valid round-trip, empty, and invalid input.
    {
        check(base64_decode("aGVsbG8=") == std::optional<std::string>{"hello"},
              "base64 decodes padded value");
        check(base64_decode("Zm9vYmFy") == std::optional<std::string>{"foobar"},
              "base64 decodes unpadded-length value");
        check(base64_decode("") == std::optional<std::string>{""}, "base64 empty");
        check(!base64_decode("not base64!").has_value(), "base64 rejects invalid");
    }

    // ConfigIterator: `//host/:optname` auth keys, _authToken before _auth.
    {
        Value r = parse(
            "//registry.npmjs.org/:_authToken=TOKEN\n"
            "//registry.npmjs.org/:username=bob\n"
            "//other.com/:_auth=Zm9vOmJhcg==\n"
            "not-a-config = 1\n");
        std::vector<ConfigItem> items = config_items(r);
        check(items.size() == 3, "config_items count 3");
        check(items[0].optname == ConfigOpt::AuthToken &&
                  items[0].registryUrl == "registry.npmjs.org/" &&
                  items[0].value == "TOKEN",
              "authToken config item");
        check(items[1].optname == ConfigOpt::Username && items[1].value == "bob",
              "username config item");
        check(items[2].optname == ConfigOpt::Auth &&
                  items[2].registryUrl == "other.com/",
              "auth config item (not matched as _authToken)");
    }

    // ScopeIterator: `@scope:registry`.
    {
        Value r = parse(
            "@myorg:registry=https://somewhere-else.com/myorg\n"
            "@another:registry=https://npm.pkg.github.com\n"
            "notascope = x\n");
        std::vector<ScopeItem> scopes = scope_items(r);
        check(scopes.size() == 2, "scope_items count 2");
        check(scopes[0].scope == "myorg", "scope name myorg");
        check(scopes[0].registry.url.find("somewhere-else.com/myorg") !=
                  std::string::npos,
              "scope registry url");
    }

    // parse_registry_url_string: token from password-only userinfo.
    {
        NpmRegistry reg = parse_registry_url_string("https://user:pass@host.com/path");
        check(reg.username == "user" && reg.password == "pass",
              "registry url userinfo -> user/pass");
        NpmRegistry reg2 = parse_registry_url_string("https://:tok@host.com/");
        check(reg2.token == "tok", "registry url password-only -> token");
    }

    // load_npmrc: default registry + scoped registry auth application by host.
    {
        Value r = parse(
            "registry=https://registry.npmjs.org/\n"
            "@myorg:registry=https://somewhere-else.com/myorg\n"
            "//registry.npmjs.org/:_authToken=DEFAULTTOKEN\n"
            "//somewhere-else.com/myorg/:_authToken=MYORGTOKEN\n"
            "//somewhere-else.com/:username=shared\n"
            "ignore-scripts=true\n"
            "save-exact=true\n");
        NpmrcConfig cfg;
        load_npmrc(cfg, r);
        check(cfg.defaultRegistry.has_value() &&
                  cfg.defaultRegistry->token == "DEFAULTTOKEN",
              "default registry auth token applied");
        NpmRegistry* myorg = cfg.find_scope("myorg");
        check(myorg != nullptr && myorg->token == "MYORGTOKEN",
              "scoped registry auth token applied by host+path");
        // bun matches on host AND pathname: a host-only `//host/:username`
        // (pathname `/`) does NOT reach a `/myorg`-scoped registry.
        check(myorg != nullptr && myorg->username.empty(),
              "host-only username does not apply to a deeper-path scope");
        check(cfg.ignoreScripts == std::optional<bool>{true}, "ignore-scripts bool");
        check(cfg.saveExact == std::optional<bool>{true}, "save-exact bool");
    }

    // _auth base64 -> username/password split.
    {
        Value r = parse(
            "registry=https://reg.example.com/\n"
            "//reg.example.com/:_auth=Zm9vOmJhcg==\n");  // foo:bar
        NpmrcConfig cfg;
        load_npmrc(cfg, r);
        check(cfg.defaultRegistry.has_value() &&
                  cfg.defaultRegistry->username == "foo" &&
                  cfg.defaultRegistry->password == "bar",
              "_auth decodes to username:password");
    }

    // ca array + omit array.
    {
        Value r = parse("ca[]=cert1\nca[]=cert2\nomit=dev\n");
        NpmrcConfig cfg;
        load_npmrc(cfg, r);
        check(cfg.ca.size() == 2 && cfg.ca[0] == "cert1" && cfg.ca[1] == "cert2",
              "ca array collected");
        check(cfg.saveDev == std::optional<bool>{false}, "omit dev -> saveDev false");
    }

    // Multi-file precedence: later document wins for scalars; scopes/configs
    // accumulate and auth applies across files.
    {
        Value a = parse(
            "registry=https://registry.npmjs.org/\n"
            "@myorg:registry=https://somewhere-else.com/myorg\n"
            "ignore-scripts=false\n");
        Value b = parse(
            "//somewhere-else.com/myorg/:_authToken=LATERTOKEN\n"
            "ignore-scripts=true\n");
        std::array<const Value*, 2> docs{&a, &b};
        NpmrcConfig cfg = load_npmrc_all(docs);
        check(cfg.ignoreScripts == std::optional<bool>{true},
              "later file wins scalar (ignore-scripts)");
        NpmRegistry* myorg = cfg.find_scope("myorg");
        check(myorg != nullptr && myorg->token == "LATERTOKEN",
              "auth from later file applies to scope from earlier file");
    }

    std::println("ini: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
