// test_override_map.cpp — mbun.install.override_map tests.
//
// Locks the semantics ported from .mbun/bun-ref/src/install/lockfile/OverrideMap.rs:
// the `overrides` XOR `resolutions` fallback (:139-159), the `$name` root-dep
// reference (:413-437), the `{".": "…"}` nested form (:200-250), the
// `resolutions` key normalization (:306-355), and the single hard error
// (:175-182) against a pile of warning-and-skip paths.

import std;
import mbun.install.npm.json;
import mbun.install.override_map;

namespace om = mbun::install::override_map;
namespace json = mbun::install::npm::json;

namespace {

int gChecks{0};
int gFailures{0};

void check(bool ok, std::string_view what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::println("  FAIL: {}", what);
    }
}

// Parse `text` as a package.json root and run the override parser over it.
// `rootDeps` stands in for the root's own dependency map (the `$name` form).
// Values are string_views into the source and into the dep map, so both outlive
// the call in function-local statics.
std::expected<om::ParseResult, std::string> run(
    std::string_view text,
    std::map<std::string, std::string, std::less<>> rootDeps = {}) {
    static std::vector<std::unique_ptr<std::string>> gKeepAlive;
    static std::vector<std::map<std::string, std::string, std::less<>>> gDeps;
    gKeepAlive.push_back(std::make_unique<std::string>(text));
    gDeps.push_back(std::move(rootDeps));
    const auto& deps{gDeps.back()};
    static std::vector<json::Document> gDocs;
    auto doc{json::parse(*gKeepAlive.back())};
    if (!doc) {
        return std::unexpected(std::string{"json parse failed"});
    }
    gDocs.push_back(std::move(*doc));
    return om::parse_from_package_json(
        *gDocs.back().root, [&deps](std::string_view name) -> std::optional<std::string_view> {
            const auto it{deps.find(name)};
            if (it == deps.end()) {
                return std::nullopt;
            }
            return std::string_view{it->second};
        });
}

void test_basic_overrides() {
    // The elysia case: a flat pin that must beat every transitive range.
    auto r{run(R"({"overrides": {"esbuild": "0.25.4"}})")};
    check(r.has_value(), "flat override parses");
    if (r) {
        const std::string* v{r->map.get("esbuild")};
        check(v != nullptr && *v == "0.25.4", "esbuild -> 0.25.4");
        check(r->map.get("nope") == nullptr, "absent name -> nullptr");
        check(r->map.contains_name("esbuild"), "contains_name hit");
        check(!r->map.empty(), "map not empty");
        check(r->warnings.empty(), "no warnings for a clean override");
    }
    check(run(R"({})")->map.empty(), "no overrides field -> empty map");
}

void test_overrides_xor_resolutions() {
    // OverrideMap.rs:139-159 — `else if`, not a merge.
    auto both{run(R"({"overrides": {"a": "1"}, "resolutions": {"b": "2"}})")};
    check(both.has_value(), "both fields parse");
    if (both) {
        check(both->map.get("a") != nullptr, "overrides wins: a present");
        check(both->map.get("b") == nullptr, "resolutions ignored when overrides exists");
    }
    // An EMPTY overrides object still hides resolutions entirely.
    auto empty{run(R"({"overrides": {}, "resolutions": {"b": "2"}})")};
    check(empty.has_value() && empty->map.empty(),
          "empty overrides still shadows resolutions");
    // resolutions alone is honoured.
    auto only{run(R"({"resolutions": {"b": "2"}})")};
    check(only.has_value() && only->map.get("b") != nullptr, "resolutions alone applies");
}

void test_hard_error_and_warning_asymmetry() {
    // OverrideMap.rs:175-182 — the only hard error.
    check(!run(R"({"overrides": "nope"})").has_value(), "non-object overrides -> error");
    // OverrideMap.rs:295-302 — a non-object resolutions only warns.
    auto r{run(R"({"resolutions": "nope"})")};
    check(r.has_value(), "non-object resolutions -> ok");
    check(r && r->map.empty() && !r->warnings.empty(), "non-object resolutions warns");
}

void test_dollar_ref() {
    // OverrideMap.rs:413-437 — `$name` reuses the root's own declared specifier.
    auto hit{run(R"({"overrides": {"lodash": "$underscore"}})",
                 {{"underscore", "^1.2.3"}})};
    check(hit.has_value(), "$ref parses");
    if (hit) {
        const std::string* v{hit->map.get("lodash")};
        check(v != nullptr && *v == "^1.2.3", "$underscore -> root's ^1.2.3");
    }
    // Miss -> warning + drop, not an error.
    auto miss{run(R"({"overrides": {"lodash": "$absent"}})")};
    check(miss.has_value(), "$ref miss is not an error");
    check(miss && miss->map.empty(), "$ref miss drops the entry");
    check(miss && !miss->warnings.empty(), "$ref miss warns");
}

void test_nested_object_form() {
    // OverrideMap.rs:200-250.
    auto dot{run(R"({"overrides": {"foo": {".": "1.2.3"}}})")};
    check(dot.has_value() && dot->map.get("foo") != nullptr, "{\".\": v} applies");
    if (dot && dot->map.get("foo")) {
        check(*dot->map.get("foo") == "1.2.3", "{\".\": v} value is v");
    }
    check(dot && dot->warnings.empty(), "lone {\".\"} does not warn");

    // A sibling key warns but the "." value STILL applies (:207-223).
    auto sibling{run(R"({"overrides": {"foo": {".": "1.2.3", "bar": "2"}}})")};
    check(sibling.has_value() && sibling->map.get("foo") != nullptr,
          "sibling key: \".\" still applies");
    check(sibling && !sibling->warnings.empty(), "sibling key warns");

    // No "." key -> drop (:235-242).
    auto noDot{run(R"({"overrides": {"foo": {"bar": "2"}}})")};
    check(noDot.has_value() && noDot->map.empty(), "no \".\" key -> dropped");
    check(noDot && !noDot->warnings.empty(), "no \".\" key warns");

    // "." present but not a string -> drop (:224-234).
    auto badDot{run(R"({"overrides": {"foo": {".": 5}}})")};
    check(badDot.has_value() && badDot->map.empty(), "non-string \".\" -> dropped");
}

void test_resolution_key_normalization() {
    // OverrideMap.rs:306-308 — a leading `**/` is stripped.
    auto star{run(R"({"resolutions": {"**/foo": "1"}})")};
    check(star.has_value() && star->map.get("foo") != nullptr, "**/foo -> foo");
    // :331-355 — nested paths are rejected outright.
    auto nested{run(R"({"resolutions": {"foo/bar": "1"}})")};
    check(nested.has_value() && nested->map.empty(), "foo/bar rejected");
    check(nested && !nested->warnings.empty(), "foo/bar warns");
    // A scoped name's single slash is structural, not nesting.
    auto scoped{run(R"({"resolutions": {"@a/b": "1"}})")};
    check(scoped.has_value() && scoped->map.get("@a/b") != nullptr, "@a/b kept");
    auto scopedNested{run(R"({"resolutions": {"@a/b/c": "1"}})")};
    check(scopedNested.has_value() && scopedNested->map.empty(), "@a/b/c rejected");
    // `**/@a/b` strips then keeps.
    auto starScoped{run(R"({"resolutions": {"**/@a/b": "1"}})")};
    check(starScoped.has_value() && starScoped->map.get("@a/b") != nullptr, "**/@a/b -> @a/b");
    // An `overrides` key is used VERBATIM — no `**/` stripping (:196).
    auto verbatim{run(R"({"overrides": {"**/foo": "1"}})")};
    check(verbatim.has_value() && verbatim->map.get("**/foo") != nullptr,
          "overrides key is verbatim");
}

void test_skips() {
    // patch: values are dropped with a warning (:255-263, :357-365).
    auto patch{run(R"({"overrides": {"foo": "patch:foo@1.0.0#p.patch"}})")};
    check(patch.has_value() && patch->map.empty(), "patch: value dropped");
    check(patch && !patch->warnings.empty(), "patch: value warns");
    // Empty value (:404-407).
    auto emptyVal{run(R"({"overrides": {"foo": ""}})")};
    check(emptyVal.has_value() && emptyVal->map.empty(), "empty value dropped");
    // Empty key (:187-194).
    auto emptyKey{run(R"({"overrides": {"": "1"}})")};
    check(emptyKey.has_value() && emptyKey->map.empty(), "empty key dropped");
    check(emptyKey && !emptyKey->warnings.empty(), "empty key warns");
}

void test_alias_valued_override() {
    // `update_name_and_name_hash_from_version_replacement`
    // (PackageManagerEnqueue.rs:1945-1968): an override value may itself be an
    // alias, redirecting package identity. The map stores the literal; the
    // consumer re-parses it.
    auto r{run(R"({"overrides": {"foo": "npm:bar@1.2.3"}})")};
    check(r.has_value() && r->map.get("foo") != nullptr, "alias-valued override stored");
    if (r && r->map.get("foo")) {
        check(*r->map.get("foo") == "npm:bar@1.2.3", "alias literal preserved verbatim");
    }
}

}  // namespace

int main() {
    test_basic_overrides();
    test_overrides_xor_resolutions();
    test_hard_error_and_warning_asymmetry();
    test_dollar_ref();
    test_nested_object_form();
    test_resolution_key_normalization();
    test_skips();
    test_alias_valued_override();

    std::println("test_override_map: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
