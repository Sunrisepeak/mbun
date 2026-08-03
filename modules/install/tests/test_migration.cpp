// test_migration.cpp — unit tests for the install migration/integrity/
// hosted_git_info/repository ports (T2.10). Pure-logic assertions only (no
// network / git CLI).
//
// Covers:
//   - integrity: SRI parse (sha512/sha1), Tag::parse, hex shasum, round-trip
//     to_string, strongest-wins, `?`/`=` trimming.
//   - hosted_git_info: is_github_shorthand, from_url (shortcut + full git URL),
//     per-provider extract.
//   - repository: is_safe_resolved_tag, parse_append_git/github, try_ssh/https.
//   - migration: parse a package-lock.json v3 sample and assert the resolved
//     dependency graph (ids, resolutions, behaviors, node_modules tree-walk,
//     bundled skip, constructed registry URL).
import std;
import mbun.install.integrity;
import mbun.install.hosted_git_info;
import mbun.install.repository;
import mbun.install.migration;

namespace {

using namespace mbun::install;
namespace hgi = mbun::install::hgi;
namespace mig = mbun::install::migration;

int gChecks{0};
int gFailures{0};

void check_true(bool cond, std::string_view what) {
    ++gChecks;
    if (!cond) {
        ++gFailures;
        std::println("  FAIL {}", what);
    }
}

template <class A, class B>
void check_eq(const A& a, const B& b, std::string_view what) {
    ++gChecks;
    if (!(a == b)) {
        ++gFailures;
        if constexpr (std::is_convertible_v<A, std::string_view> &&
                      std::is_convertible_v<B, std::string_view>) {
            std::println("  FAIL {}: got \"{}\", expected \"{}\"", what,
                         std::string_view{a}, std::string_view{b});
        } else {
            std::println("  FAIL {}", what);
        }
    }
}

// ── integrity ───────────────────────────────────────────────────────────────
void test_integrity() {
    // Tag::parse
    {
        auto [tag, off] = Tag::parse("sha512-AAAA");
        check_true(tag == Tag::SHA512(), "tag: sha512 parsed");
        check_eq(off, std::size_t{7}, "tag: sha512 offset");
        check_eq(tag.digest_len(), SHA512_DIGEST_LEN, "tag: sha512 digest len");
    }
    {
        auto [tag, off] = Tag::parse("sha1-abc");
        check_true(tag == Tag::SHA1(), "tag: sha1 parsed");
        check_eq(off, std::size_t{5}, "tag: sha1 offset");
    }
    {
        auto [tag, off] = Tag::parse("md5-xxx");
        check_true(tag == Tag::UNKNOWN(), "tag: md5 unknown");
        check_eq(off, std::size_t{0}, "tag: unknown offset 0");
    }

    // hex shasum → sha1
    {
        auto integ = Integrity::parse_sha_sum("3cd0599b099384b815c10f7fa7df0092b62d534f");
        check_true(integ.has_value(), "shasum: parsed");
        check_true(integ->tag == Tag::SHA1(), "shasum: tag sha1");
        check_eq(integ->slice().size(), SHA1_DIGEST_LEN, "shasum: 20 bytes");
        check_eq(integ->value[0], std::uint8_t{0x3c}, "shasum: first byte 0x3c");
        check_eq(integ->value[1], std::uint8_t{0xd0}, "shasum: second byte 0xd0");
    }
    // odd-length / invalid hex rejected
    check_true(!Integrity::parse_sha_sum("abc").has_value(), "shasum: odd length rejected");
    check_true(!Integrity::parse_sha_sum("3g").has_value(), "shasum: non-hex rejected");
    // empty → UNKNOWN (not error)
    {
        auto e = Integrity::parse_sha_sum("");
        check_true(e.has_value() && e->tag == Tag::UNKNOWN(), "shasum: empty unknown");
    }

    // Round-trip: build a SHA512 integrity, format, re-parse.
    {
        Integrity src{};
        src.tag = Tag::SHA512();
        for (std::size_t i = 0; i < SHA512_DIGEST_LEN; ++i)
            src.value[i] = static_cast<std::uint8_t>(i * 7 + 1);
        std::string formatted = src.to_string();
        check_true(formatted.starts_with("sha512-"), "sri: format prefix");
        check_true(formatted.ends_with("=="), "sri: sha512 padding ==");

        Integrity parsed = Integrity::parse(formatted);
        check_true(parsed.tag == Tag::SHA512(), "sri: round-trip tag");
        bool same = true;
        for (std::size_t i = 0; i < SHA512_DIGEST_LEN; ++i)
            if (parsed.value[i] != src.value[i]) same = false;
        check_true(same, "sri: round-trip bytes");
    }

    // sha1 formats with single '='.
    {
        Integrity src{};
        src.tag = Tag::SHA1();
        std::string formatted = src.to_string();
        check_true(formatted.starts_with("sha1-"), "sri: sha1 prefix");
        check_true(formatted.ends_with("=") && !formatted.ends_with("=="),
                   "sri: sha1 single = padding");
    }

    // strongest-wins across whitespace-delimited entries.
    {
        // sha1 (20 bytes) encodes to shorter; sha512 should win.
        Integrity s1{}; s1.tag = Tag::SHA1();
        Integrity s5{}; s5.tag = Tag::SHA512();
        std::string combined = s1.to_string() + " " + s5.to_string();
        Integrity parsed = Integrity::parse(combined);
        check_true(parsed.tag == Tag::SHA512(), "sri: strongest sha512 wins");
    }

    // '?' suffix stripped.
    {
        Integrity src{}; src.tag = Tag::SHA256();
        std::string base = src.to_string();  // sha256-...==
        // insert a query option before padding-free tail: append ?foo
        // to_string has no '?'; craft: strip '==' add "?foo"
        std::string_view b64 = std::string_view{base}.substr(std::string_view{"sha256-"}.size());
        std::string with_q = "sha256-" + std::string{b64} + "?foo=bar";
        Integrity parsed = Integrity::parse(with_q);
        check_true(parsed.tag == Tag::SHA256(), "sri: ? option ignored");
    }
}

// ── integrity hashing (for_bytes / verify wired to mbun.crypto) ─────────────
void test_integrity_hash() {
    auto hex_digest = [](const Integrity& in) {
        static constexpr char D[]{"0123456789abcdef"};
        std::string s;
        for (std::uint8_t b : in.slice()) {
            s.push_back(D[b >> 4]);
            s.push_back(D[b & 0xF]);
        }
        return s;
    };

    // FIPS 180-2 sha512 test vectors.
    constexpr std::string_view SHA512_EMPTY{
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"};
    constexpr std::string_view SHA512_ABC{
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"};

    // for_bytes: sha512 over empty input.
    {
        Integrity empty = Integrity::for_bytes({});
        check_true(empty.tag == Tag::SHA512(), "hash: for_bytes tag sha512");
        check_eq(hex_digest(empty), SHA512_EMPTY, "hash: sha512(\"\") vector");
        check_true(empty.verify({}), "hash: verify empty against itself");
    }

    // for_bytes: sha512("abc") + verify success/failure.
    {
        const std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
        Integrity in = Integrity::for_bytes(abc);
        check_eq(hex_digest(in), SHA512_ABC, "hash: sha512(\"abc\") vector");
        check_true(in.verify(abc), "hash: verify abc matches");
        const std::array<std::uint8_t, 3> abd{'a', 'b', 'd'};
        check_true(!in.verify(abd), "hash: verify mismatch fails");
        check_true(!in.verify({}), "hash: verify empty vs abc fails");
    }

    // verify goes through the entry's OWN algorithm (sha1 here), matching
    // integrity.rs verify_by_tag's per-tag dispatch.
    {
        // sha1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d
        auto s1 = Integrity::parse("sha1-qZk+NkcGgWq6PiVxeFDCbJzQ2J0=");
        check_true(s1.tag == Tag::SHA1(), "hash: sha1 SRI parsed");
        const std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
        check_true(s1.verify(abc), "hash: sha1 verify matches");
        const std::array<std::uint8_t, 3> abd{'a', 'b', 'd'};
        check_true(!s1.verify(abd), "hash: sha1 verify mismatch fails");
    }

    // Multi-entry SRI: parse keeps the strongest entry (sha512) and verify
    // checks against THAT entry only — bun's strongest-wins semantics.
    {
        const std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
        std::string sha1_abc{"sha1-qZk+NkcGgWq6PiVxeFDCbJzQ2J0="};
        std::string sha512_abc{Integrity::for_bytes(abc).to_string()};
        Integrity multi = Integrity::parse(sha1_abc + " " + sha512_abc);
        check_true(multi.tag == Tag::SHA512(), "hash: multi-entry keeps sha512");
        check_true(multi.verify(abc), "hash: multi-entry verify via strongest");
        // Strongest entry wrong -> verify fails even if a weaker entry matched.
        Integrity multiBad = Integrity::parse(
            sha1_abc + " sha512-" + std::string(86, 'A') + "==");
        check_true(multiBad.tag == Tag::SHA512(), "hash: bad multi keeps sha512");
        check_true(!multiBad.verify(abc), "hash: weaker match cannot rescue");
    }

    // UNKNOWN tag never verifies (integrity.rs verify_by_tag `_ => false`).
    {
        Integrity unknown{};
        check_true(!unknown.verify({}), "hash: unknown tag -> false");
    }
}

// ── hosted_git_info ─────────────────────────────────────────────────────────
void test_hosted_git_info() {
    // is_github_shorthand
    check_true(hgi::is_github_shorthand("user/repo"), "hgi: user/repo shorthand");
    check_true(hgi::is_github_shorthand("user/repo#branch"), "hgi: with committish");
    check_true(!hgi::is_github_shorthand("./local"), "hgi: leading dot not shorthand");
    check_true(!hgi::is_github_shorthand("/abs"), "hgi: leading slash not shorthand");
    check_true(!hgi::is_github_shorthand("noslash"), "hgi: no slash not shorthand");
    check_true(!hgi::is_github_shorthand("user/repo/extra"),
               "hgi: second slash without hash rejected");
    check_true(!hgi::is_github_shorthand("user@host/repo"),
               "hgi: @ before hash rejected");
    check_true(!hgi::is_github_shorthand("user/repo/"), "hgi: trailing slash rejected");

    // from_url shortcut path
    {
        auto info = hgi::from_url("user/repo");
        check_true(info.has_value(), "hgi: from_url shorthand parsed");
        check_true(info->host_provider == hgi::HostProvider::Github, "hgi: provider github");
        check_eq(info->project_view(), std::string_view{"repo"}, "hgi: project repo");
        check_true(info->user_view() == std::optional<std::string_view>{"user"},
                   "hgi: user user");
        check_true(info->default_representation == hgi::Representation::Shortcut,
                   "hgi: shortcut representation");
    }
    {
        auto info = hgi::from_url("github:owner/proj#v1.2.3");
        check_true(info.has_value(), "hgi: github: shortcut parsed");
        check_eq(info->project_view(), std::string_view{"proj"}, "hgi: project proj");
        check_true(info->committish_view() == std::optional<std::string_view>{"v1.2.3"},
                   "hgi: committish v1.2.3");
    }
    {
        auto info = hgi::from_url("gitlab:group/sub");
        check_true(info.has_value() && info->host_provider == hgi::HostProvider::Gitlab,
                   "hgi: gitlab shortcut");
    }
    // full git+ssh URL → not a shortcut, extracts via domain
    {
        auto info = hgi::from_url("git+ssh://git@github.com/user/repo.git#abc");
        check_true(info.has_value(), "hgi: git+ssh parsed");
        check_true(info->host_provider == hgi::HostProvider::Github, "hgi: git+ssh github");
        check_eq(info->project_view(), std::string_view{"repo"}, "hgi: git+ssh project");
        check_true(info->user_view() == std::optional<std::string_view>{"user"},
                   "hgi: git+ssh user");
    }
    // non-git URL → nullopt
    check_true(!hgi::from_url("https://example.com/foo/bar").has_value(),
               "hgi: unknown host rejected");
}

// ── repository ──────────────────────────────────────────────────────────────
void test_repository() {
    // is_safe_resolved_tag
    check_true(is_safe_resolved_tag("abc123def"), "repo: alnum safe");
    check_true(is_safe_resolved_tag("v1.2.3_beta-1"), "repo: dots dashes underscores safe");
    check_true(!is_safe_resolved_tag(""), "repo: empty unsafe");
    check_true(!is_safe_resolved_tag("-leadingdash"), "repo: leading dash unsafe");
    check_true(!is_safe_resolved_tag(".."), "repo: dotdot unsafe");
    check_true(!is_safe_resolved_tag("a/b"), "repo: slash unsafe");
    check_true(!is_safe_resolved_tag(std::string(300, 'a')), "repo: too long unsafe");

    // host_tld
    check_true(host_tld("github") == std::optional<std::string_view>{".com"}, "repo: github tld");
    check_true(host_tld("bitbucket") == std::optional<std::string_view>{".org"},
               "repo: bitbucket tld");
    check_true(!host_tld("example").has_value(), "repo: unknown tld none");

    // parse_append_github
    {
        Repository r = parse_append_github("github:owner/repo#deadbeef");
        check_eq(r.owner, std::string{"owner"}, "repo: github owner");
        check_eq(r.repo, std::string{"repo"}, "repo: github repo");
        check_eq(r.committish, std::string{"deadbeef"}, "repo: github committish");
    }
    {
        Repository r = parse_append_github("owner/repo");
        check_eq(r.owner, std::string{"owner"}, "repo: github owner no-prefix");
        check_eq(r.repo, std::string{"repo"}, "repo: github repo no-hash");
        check_true(r.committish.empty(), "repo: github no committish");
    }
    // parse_append_git
    {
        Repository r = parse_append_git("git+https://github.com/u/r.git#tag");
        check_eq(r.repo, std::string{"https://github.com/u/r.git"}, "repo: git repo stripped git+");
        check_eq(r.committish, std::string{"tag"}, "repo: git committish");
    }

    // try_ssh / try_https
    {
        auto s = try_ssh("git@github.com:user/repo.git");
        check_true(s == std::optional<std::string>{"git@github.com:user/repo.git"},
                   "repo: try_ssh git@ passthrough");
    }
    check_true(!try_ssh("https://github.com/u/r").has_value(),
               "repo: try_ssh rejects http");
    {
        auto h = try_https("https://github.com/u/r");
        check_true(h == std::optional<std::string>{"https://github.com/u/r"},
                   "repo: try_https http passthrough");
    }
    {
        auto h = try_https("ssh://git@github.com/u/r.git");
        check_true(h == std::optional<std::string>{"https://git@github.com/u/r.git"},
                   "repo: try_https ssh->https");
    }

    // format
    {
        Repository r; r.owner = "o"; r.repo = "rp"; r.committish = "c";
        check_eq(format_repository(r, "github:"), std::string{"github:o/rp#c"},
                 "repo: format owner/repo#committish");
    }
}

// ── migration ───────────────────────────────────────────────────────────────
void test_migration_basic() {
    // Build a valid sha512 SRI string (correct base64 length) at runtime.
    Integrity si{};
    si.tag = Tag::SHA512();
    for (std::size_t i = 0; i < SHA512_DIGEST_LEN; ++i)
        si.value[i] = static_cast<std::uint8_t>(i);
    std::string integ = si.to_string();

    std::string lock = std::format(R"JSON(
{{
  "name": "root-pkg",
  "lockfileVersion": 3,
  "packages": {{
    "": {{
      "name": "root-pkg",
      "version": "1.0.0",
      "dependencies": {{ "a": "^1.0.0" }},
      "devDependencies": {{ "d": "^3.0.0" }}
    }},
    "node_modules/a": {{
      "version": "1.0.0",
      "resolved": "https://registry.npmjs.org/a/-/a-1.0.0.tgz",
      "integrity": "{}",
      "dependencies": {{ "b": "^2.0.0" }}
    }},
    "node_modules/b": {{
      "version": "2.0.0",
      "resolved": "https://registry.npmjs.org/b/-/b-2.0.0.tgz"
    }},
    "node_modules/d": {{
      "version": "3.0.0",
      "resolved": "https://registry.npmjs.org/d/-/d-3.0.0.tgz"
    }}
  }}
}}
)JSON",
                                   integ);

    auto res = mig::migrate_npm_lockfile(lock);
    check_true(res.has_value(), "mig: lockfile parsed");
    if (!res) return;

    check_eq(res->packages.size(), std::size_t{4}, "mig: 4 packages (root,a,b,d)");

    const auto& root = res->packages[0];
    check_eq(root.name, std::string{"root-pkg"}, "mig: root name");
    check_true(root.resolution.tag == mig::ResolutionTag::Root, "mig: root resolution Root");
    check_eq(root.dependencies.size(), std::size_t{2}, "mig: root has 2 deps (a,d)");

    // root -> a (dependencies), root -> d (dev)
    {
        const auto& da = root.dependencies[0];
        check_eq(da.name, std::string{"a"}, "mig: root dep0 a");
        check_true(da.behavior == mig::DepKind::Dependencies, "mig: a is prod dep");
        check_eq(da.resolved_id, std::uint32_t{1}, "mig: a resolves to id 1");
        const auto& dd = root.dependencies[1];
        check_eq(dd.name, std::string{"d"}, "mig: root dep1 d");
        check_true(dd.behavior == mig::DepKind::Dev, "mig: d is dev dep");
        check_eq(dd.resolved_id, std::uint32_t{3}, "mig: d resolves to id 3");
    }

    // package a
    const auto& a = res->packages[1];
    check_eq(a.name, std::string{"a"}, "mig: pkg1 name a");
    check_true(a.resolution.tag == mig::ResolutionTag::Npm, "mig: a resolution Npm");
    check_eq(a.resolution.value, std::string{"https://registry.npmjs.org/a/-/a-1.0.0.tgz"},
             "mig: a resolved url");
    check_true(a.integrity.tag == Tag::SHA512(), "mig: a integrity sha512");
    check_eq(a.dependencies.size(), std::size_t{1}, "mig: a has 1 dep (b)");
    check_eq(a.dependencies[0].resolved_id, std::uint32_t{2}, "mig: a->b id 2");

    // package b, d
    check_eq(res->packages[2].name, std::string{"b"}, "mig: pkg2 name b");
    check_eq(res->packages[3].name, std::string{"d"}, "mig: pkg3 name d");
}

// version gate + malformed
void test_migration_errors() {
    // lockfileVersion 1 → version mismatch
    constexpr std::string_view v1 = R"({"lockfileVersion":1,"packages":{"":{}}})";
    auto r1 = mig::migrate_npm_lockfile(v1);
    check_true(!r1.has_value() &&
                   r1.error() == mig::MigrationError::NPMLockfileVersionMismatch,
               "mig: v1 version mismatch");

    // missing packages
    constexpr std::string_view no_pkgs = R"({"lockfileVersion":3})";
    auto r2 = mig::migrate_npm_lockfile(no_pkgs);
    check_true(!r2.has_value() && r2.error() == mig::MigrationError::InvalidNPMLockfile,
               "mig: missing packages invalid");

    // first key not "" (self reference)
    constexpr std::string_view no_root = R"({"lockfileVersion":3,"packages":{"node_modules/a":{}}})";
    auto r3 = mig::migrate_npm_lockfile(no_root);
    check_true(!r3.has_value() && r3.error() == mig::MigrationError::InvalidNPMLockfile,
               "mig: missing root self-ref invalid");

    // garbage
    auto r4 = mig::migrate_npm_lockfile("not json");
    check_true(!r4.has_value(), "mig: garbage rejected");
}

// bundled dep skip + nested node_modules tree-walk + constructed registry url
constexpr std::string_view kLockNested = R"JSON(
{
  "name": "root",
  "lockfileVersion": 3,
  "packages": {
    "": { "name": "root", "dependencies": { "a": "^1.0.0" } },
    "node_modules/a": {
      "version": "1.0.0",
      "dependencies": { "shared": "^1.0.0", "bundled-x": "^1.0.0" }
    },
    "node_modules/a/node_modules/shared": {
      "version": "1.5.0",
      "resolved": "https://registry.npmjs.org/shared/-/shared-1.5.0.tgz"
    },
    "node_modules/a/node_modules/bundled-x": {
      "version": "1.0.0",
      "inBundle": true
    }
  }
}
)JSON";

void test_migration_nested() {
    auto res = mig::migrate_npm_lockfile(kLockNested);
    check_true(res.has_value(), "mig-nested: parsed");
    if (!res) return;

    // packages: root(0), a(1), shared(2). bundled-x is skipped from package
    // building but recorded in id_map as bundled → its dep is skipped.
    check_eq(res->packages.size(), std::size_t{3}, "mig-nested: 3 packages");

    // 'a' has no `resolved`; a constructed registry URL must back its resolution.
    const auto& a = res->packages[1];
    check_eq(a.name, std::string{"a"}, "mig-nested: pkg1 a");
    check_true(a.resolution.tag == mig::ResolutionTag::Npm, "mig-nested: a Npm via constructed url");
    check_eq(a.resolution.value, std::string{"https://registry.npmjs.org/a/-/a-1.0.0.tgz"},
             "mig-nested: constructed registry url");

    // a -> shared resolves to the NESTED shared (id 2), bundled-x is skipped.
    check_eq(a.dependencies.size(), std::size_t{1}, "mig-nested: a has 1 linked dep (shared)");
    check_eq(a.dependencies[0].name, std::string{"shared"}, "mig-nested: a dep shared");
    check_eq(a.dependencies[0].resolved_id, std::uint32_t{2}, "mig-nested: shared id 2");
    check_eq(res->packages[2].name, std::string{"shared"}, "mig-nested: pkg2 shared");
}

}  // namespace

int main() {
    test_integrity();
    test_integrity_hash();
    test_hosted_git_info();
    test_repository();
    test_migration_basic();
    test_migration_errors();
    test_migration_nested();

    std::println("test_migration: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
