// test_text_writer.cpp — text bun.lock writer + v1+/v2 parse strictness.
// Expected bytes are bun v1.4.0 output captured for the same fixtures
// (test/cli/install/lockfile-version-2.test.ts shapes).
import std;
import mbun.install;
import mbun.install.lockfile.text_writer;

namespace install = mbun::install;
namespace writer = mbun::install::lockfile::text_writer;

namespace {

int gChecks{0};
int gFailures{0};

void check(bool condition, std::string_view description) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::println(std::cerr, "FAIL: {}", description);
    }
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view what) {
    ++gChecks;
    if (actual != expected) {
        ++gFailures;
        std::println(std::cerr, "FAIL: {}\n--- expected:\n{}\n--- actual:\n{}", what, expected,
                     actual);
    }
}

// bun's byte output for a fresh single-file-dep install (GT: bun install
// --save-text-lockfile with dep: file:./dep).
constexpr std::string_view FRESH_FILE_DEP_LOCK{
    "{\n"
    "  \"lockfileVersion\": 2,\n"
    "  \"configVersion\": 1,\n"
    "  \"workspaces\": {\n"
    "    \"\": {\n"
    "      \"name\": \"root\",\n"
    "      \"dependencies\": {\n"
    "        \"dep\": \"file:./dep\",\n"
    "      },\n"
    "    },\n"
    "  },\n"
    "  \"packages\": {\n"
    "    \"dep\": [\"dep@file:dep\", {}],\n"
    "  }\n"
    "}\n"};

// bun's byte output after adding `b` to a v1 lockfile (GT2): blank line
// between package entries, version preserved at 1.
constexpr std::string_view V1_TWO_DEPS_LOCK{
    "{\n"
    "  \"lockfileVersion\": 1,\n"
    "  \"configVersion\": 1,\n"
    "  \"workspaces\": {\n"
    "    \"\": {\n"
    "      \"name\": \"root\",\n"
    "      \"dependencies\": {\n"
    "        \"a\": \"file:./a\",\n"
    "        \"b\": \"file:./b\",\n"
    "      },\n"
    "    },\n"
    "  },\n"
    "  \"packages\": {\n"
    "    \"a\": [\"a@file:a\", {}],\n"
    "\n"
    "    \"b\": [\"b@file:b\", {}],\n"
    "  }\n"
    "}\n"};

// bun's byte output for the re-saved v0 workspace lockfile (GT3): floored to
// v1, configVersion 0 (the v0 lock had none), single-element workspace tuple.
constexpr std::string_view V0_FLOORED_WORKSPACE_LOCK{
    "{\n"
    "  \"lockfileVersion\": 1,\n"
    "  \"configVersion\": 0,\n"
    "  \"workspaces\": {\n"
    "    \"\": {\n"
    "      \"name\": \"root\",\n"
    "      \"dependencies\": {\n"
    "        \"pkg1\": \"workspace:*\",\n"
    "      },\n"
    "    },\n"
    "    \"packages/pkg1\": {\n"
    "      \"name\": \"pkg1\",\n"
    "    },\n"
    "  },\n"
    "  \"packages\": {\n"
    "    \"pkg1\": [\"pkg1@workspace:packages/pkg1\"],\n"
    "  }\n"
    "}\n"};

// bun's byte output for the --lockfile-only v1 off-registry round-trip (GT9):
// 4-element npm tuple with the off-registry URL and empty integrity kept.
constexpr std::string_view V1_OFF_REGISTRY_LOCK{
    "{\n"
    "  \"lockfileVersion\": 1,\n"
    "  \"configVersion\": 1,\n"
    "  \"workspaces\": {\n"
    "    \"\": {\n"
    "      \"name\": \"root\",\n"
    "      \"dependencies\": {\n"
    "        \"no-deps\": \"1.0.0\",\n"
    "      },\n"
    "    },\n"
    "  },\n"
    "  \"packages\": {\n"
    "    \"no-deps\": [\"no-deps@1.0.0\", \"http://127.0.0.1:1/no-deps/-/no-deps-1.0.0.tgz\", "
    "{}, \"\"],\n"
    "  }\n"
    "}\n"};

void test_round_trips_match_bun_bytes() {
    for (const auto& [source, what] :
         {std::pair{FRESH_FILE_DEP_LOCK, std::string_view{"fresh file-dep lock"}},
          std::pair{V1_TWO_DEPS_LOCK, std::string_view{"v1 two-file-deps lock"}},
          std::pair{V0_FLOORED_WORKSPACE_LOCK, std::string_view{"v0-floored workspace lock"}},
          std::pair{V1_OFF_REGISTRY_LOCK, std::string_view{"v1 off-registry npm lock"}}}) {
        auto parsed{install::parse_text(source)};
        check(parsed.has_value(), std::format("{} parses", what));
        if (!parsed) {
            continue;
        }
        const std::string rewritten{writer::write_text(
            *parsed, parsed->lockfileVersion, parsed->configVersion.value_or(0))};
        check_eq(rewritten, source, std::format("{} round-trips byte-identically", what));
    }
}

void test_version_to_write() {
    install::Lockfile empty{};
    check(writer::version_to_write(empty, std::nullopt) == 2,
          "fresh clean lockfile stamps the current version");
    check(writer::version_to_write(empty, 0) == 1, "v0 floors to v1");
    check(writer::version_to_write(empty, 1) == 1, "v1 is preserved, never bumped");
    check(writer::version_to_write(empty, 2) == 2, "v2 is preserved");

    // A fresh lockfile carrying an off-registry npm tarball without integrity
    // cannot be stamped v2 (the next parse would reject it).
    install::Lockfile offRegistry{};
    install::Package npmPkg{};
    npmPkg.key = "no-deps";
    npmPkg.name = "no-deps";
    npmPkg.version = "1.0.0";
    npmPkg.tag = install::Resolution::Npm;
    npmPkg.registry = "http://127.0.0.1:1/no-deps/-/no-deps-1.0.0.tgz";
    offRegistry.packages.push_back(npmPkg);
    check(writer::version_to_write(offRegistry, std::nullopt) == 1,
          "fresh off-registry npm entry without integrity stays v1");

    npmPkg.integrity = "sha512-abc";
    install::Lockfile withIntegrity{};
    withIntegrity.packages.push_back(npmPkg);
    check(writer::version_to_write(withIntegrity, std::nullopt) == 2,
          "supported integrity makes the off-registry entry v2-clean");

    install::Lockfile unsafeGit{};
    install::Package gitPkg{};
    gitPkg.key = "dep";
    gitPkg.name = "dep";
    gitPkg.tag = install::Resolution::Git;
    gitPkg.resolution = "git+ssh://git@127.0.0.1:1/example/repo.git#main";
    gitPkg.bunTag = "../escape";
    unsafeGit.packages.push_back(gitPkg);
    check(writer::version_to_write(unsafeGit, std::nullopt) == 1,
          "unsafe git .bun-tag cannot be stamped v2");
}

std::string git_lock(std::uint32_t version, std::string_view tag) {
    return std::format(
        R"({{"lockfileVersion":{},"configVersion":1,)"
        R"("workspaces":{{"":{{"name":"root","dependencies":{{"dep":"git+ssh://git@127.0.0.1:1/example/repo.git#main"}}}}}},)"
        R"("packages":{{"dep":["dep@git+ssh://git@127.0.0.1:1/example/repo.git#main",{{}},"{}"]}}}})",
        version, tag);
}

std::string github_lock(std::uint32_t version, std::string_view tag) {
    return std::format(
        R"({{"lockfileVersion":{},"configVersion":1,)"
        R"("workspaces":{{"":{{"name":"root","dependencies":{{"dep":"github:example/repo#main"}}}}}},)"
        R"("packages":{{"dep":["dep@github:example/repo#main",{{}},"{}"]}}}})",
        version, tag);
}

std::string off_registry_lock(std::uint32_t version, std::string_view integrity) {
    return std::format(
        R"({{"lockfileVersion":{},"configVersion":1,)"
        R"("workspaces":{{"":{{"name":"root","dependencies":{{"no-deps":"1.0.0"}}}}}},)"
        R"("packages":{{"no-deps":["no-deps@1.0.0","http://127.0.0.1:9/no-deps/-/no-deps-1.0.0.tgz",{{}},"{}"]}}}})",
        version, integrity);
}

void test_parse_time_strictness() {
    {
        auto v2{install::parse_text(git_lock(2, "../escape"))};
        check(!v2 && v2.error() == install::ParseError::InvalidGitDependencyTag,
              "unsafe git tag is rejected while parsing at v2");
        auto v1{install::parse_text(git_lock(1, "../escape"))};
        check(v1.has_value(), "unsafe git tag is tolerated at v1 (pre-check lockfiles)");
        auto safe{install::parse_text(git_lock(2, "abc123"))};
        check(safe.has_value(), "safe git tag parses at v2");
    }
    {
        auto v1{install::parse_text(github_lock(1, "../escape"))};
        check(!v1 && v1.error() == install::ParseError::InvalidGitDependencyTag,
              "unsafe github tag is rejected at every version");
        auto v2{install::parse_text(github_lock(2, "../escape"))};
        check(!v2 && v2.error() == install::ParseError::InvalidGitDependencyTag,
              "unsafe github tag is rejected at v2 too");
    }
    {
        const std::string missingTag{
            R"({"lockfileVersion":1,"configVersion":1,)"
            R"("workspaces":{"":{"name":"root","dependencies":{"dep":"github:example/repo#main"}}},)"
            R"("packages":{"dep":["dep@github:example/repo#main",{}]}})"};
        auto parsed{install::parse_text(missingTag)};
        check(!parsed && parsed.error() == install::ParseError::MissingGitDependencyTag,
              "git/github tuple without .bun-tag is rejected at v1+");
    }
    {
        auto v2{install::parse_text(off_registry_lock(2, ""))};
        check(!v2 && v2.error() == install::ParseError::MissingIntegrityHash,
              "v2 rejects off-registry npm tarball without integrity");
        auto v1{install::parse_text(off_registry_lock(1, ""))};
        check(v1.has_value(), "v1 predates the integrity check");
        auto withHash{install::parse_text(off_registry_lock(2, "sha512-abc"))};
        check(withHash.has_value(), "v2 accepts off-registry tarball with integrity");
    }
}

void test_shared_predicates() {
    check(install::url_is_under_registry("https://registry.npmjs.org/x/-/x-1.tgz",
                                         "https://registry.npmjs.org/"),
          "default-registry tarball is under the default registry");
    check(!install::url_is_under_registry("http://127.0.0.1:1/x/-/x-1.tgz",
                                          "https://registry.npmjs.org/"),
          "loopback tarball is not under the default registry");
    check(!install::url_is_under_registry("https://registry.npmjs.org.evil.com/x.tgz",
                                          "https://registry.npmjs.org/"),
          "prefix match requires a path boundary");
    check(install::is_safe_resolved_tag("v1.2.3"), "plain tag is safe");
    check(!install::is_safe_resolved_tag("../escape"), "path separators are unsafe");
    check(!install::is_safe_resolved_tag(""), "empty tag is unsafe");
    check(!install::is_safe_resolved_tag("-leading-dash"), "leading dash is unsafe");
    check(install::integrity_is_supported("sha512-abc"), "sha512 SRI is supported");
    check(!install::integrity_is_supported(""), "empty integrity is unsupported");
    check(!install::integrity_is_supported("md5-abc"), "unknown algorithm is unsupported");
}

}  // namespace

int main() {
    test_round_trips_match_bun_bytes();
    test_version_to_write();
    test_parse_time_strictness();
    test_shared_predicates();

    std::println("test_text_writer: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
