// test_command_lockfile.cpp — install_project's lockfile write/update path,
// workspace linking and frozen semantics. Fixtures mirror
// compat/bun/test/cli/install/lockfile-version-2.test.ts and bad-workspace.test.ts;
// expected lock bytes match bun v1.4.0 output.
import std;
import mbun.install.command;

namespace command = mbun::install::command;

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

struct TempDir {
    std::filesystem::path path;

    explicit TempDir(std::string_view label) {
        const auto nonce{std::chrono::steady_clock::now().time_since_epoch().count()};
        path = std::filesystem::temp_directory_path() /
               std::format("mbun-cmdlock-{}-{}", label, nonce);
        std::filesystem::create_directories(path);
    }

    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// lockfile-version-2.test.ts "a freshly written text lockfile defaults to
// version 2".
void test_fresh_install_writes_v2_lock() {
    TempDir dir{"fresh-v2"};
    write_file(dir.path / "package.json",
               R"({"name":"root","dependencies":{"dep":"file:./dep"}})");
    write_file(dir.path / "dep/package.json", R"({"name":"dep","version":"1.0.0"})");

    command::InstallOptions options{};
    options.saveTextLockfile = true;
    auto result{command::install_project(dir.path, options)};
    check(result.has_value(), "fresh file-dep install succeeds");
    if (result) {
        check(result->savedLockfile, "fresh install saves the lockfile");
    }
    const std::string lock{read_file(dir.path / "bun.lock")};
    check(contains(lock, "\"lockfileVersion\": 2,"), "fresh lock stamps version 2");
    check(contains(lock, "\"dep\": [\"dep@file:dep\", {}],"), "file dep entry matches bun");

    // A second install with an up-to-date lockfile does not re-save.
    auto second{command::install_project(dir.path, {})};
    check(second.has_value() && !second->savedLockfile,
          "up-to-date lockfile is not re-saved");
}

// lockfile-version-2.test.ts "re-saving a v1 lockfile keeps it at version 1
// even after adding a dependency".
void test_resave_keeps_v1() {
    TempDir dir{"v1-no-bump"};
    write_file(dir.path / "package.json",
               R"({"name":"root","dependencies":{"a":"file:./a","b":"file:./b"}})");
    write_file(dir.path / "a/package.json", R"({"name":"a","version":"1.0.0"})");
    write_file(dir.path / "b/package.json", R"({"name":"b","version":"1.0.0"})");
    write_file(dir.path / "bun.lock",
               R"({"lockfileVersion":1,"configVersion":1,)"
               R"("workspaces":{"":{"name":"root","dependencies":{"a":"file:./a"}}},)"
               R"("packages":{"a":["a@file:a",{}]}})");

    auto result{command::install_project(dir.path, {})};
    check(result.has_value(), "v1 lock with a new dep installs");
    if (result) {
        check(result->savedLockfile, "out-of-date lock is re-saved");
    }
    const std::string lock{read_file(dir.path / "bun.lock")};
    check(contains(lock, "\"b\": [\"b@file:b\""), "new dependency lands in the lock");
    check(contains(lock, "\"lockfileVersion\": 1,"), "existing v1 is preserved");
    check(!contains(lock, "\"lockfileVersion\": 2,"), "v1 is never bumped to 2");
}

// lockfile-version-2.test.ts "re-saving a v0 lockfile floors it to version 1
// so it stays parseable" — plus the workspace symlink bun leaves behind.
void test_v0_floors_to_v1_and_workspace_links() {
    TempDir dir{"v0-floor"};
    write_file(dir.path / "package.json",
               R"({"name":"root","workspaces":["packages/*"],)"
               R"("dependencies":{"pkg1":"workspace:*"}})");
    write_file(dir.path / "packages/pkg1/package.json", R"({"name":"pkg1"})");
    write_file(dir.path / "bun.lock",
               R"({"lockfileVersion":0,)"
               R"("workspaces":{"":{"name":"root","dependencies":{"pkg1":"workspace:*"}},)"
               R"("packages/pkg1":{"name":"pkg1"}},)"
               R"("packages":{"pkg1":["pkg1@workspace:packages/pkg1",{}]}})");

    auto first{command::install_project(dir.path, {})};
    check(first.has_value(), "v0 workspace lock installs");
    const std::string lock{read_file(dir.path / "bun.lock")};
    check(contains(lock, "\"lockfileVersion\": 1,"), "v0 floors to v1");
    check(!contains(lock, "\"lockfileVersion\": 0,"), "v0 is not preserved");
    check(!contains(lock, "\"lockfileVersion\": 2,"), "v0 is not bumped past v1");
    check(contains(lock, "\"pkg1\": [\"pkg1@workspace:packages/pkg1\"],"),
          "workspace tuple uses the v1+ single-element form");
    check(contains(lock, "\"configVersion\": 0,"),
          "a lock without configVersion re-saves as 0 (bun observable)");

    const std::filesystem::path link{dir.path / "node_modules/pkg1"};
    check(std::filesystem::is_symlink(std::filesystem::symlink_status(link)),
          "workspace dep is a symlink");
    std::error_code ec;
    check(std::filesystem::read_symlink(link, ec) ==
              std::filesystem::path{"../packages/pkg1"},
          "workspace symlink is relative, matching bun");

    // The re-saved lockfile must still parse under --frozen-lockfile.
    command::InstallOptions frozen{};
    frozen.frozenLockfile = true;
    auto second{command::install_project(dir.path, frozen)};
    check(second.has_value(), "re-saved v1 lock passes a frozen install");
    if (second) {
        check(!second->savedLockfile, "frozen install never saves");
    }
}

// lockfile-version-2.test.ts "re-saving a v1 off-registry lockfile keeps it
// at version 1" (--lockfile-only round-trip, no fetch, no node_modules).
void test_lockfile_only_round_trips_v1_npm() {
    TempDir dir{"lockfile-only-npm"};
    write_file(dir.path / "package.json",
               R"({"name":"root","dependencies":{"no-deps":"1.0.0"}})");
    write_file(
        dir.path / "bun.lock",
        R"({"lockfileVersion":1,"configVersion":1,)"
        R"("workspaces":{"":{"name":"root","dependencies":{"no-deps":"1.0.0"}}},)"
        R"("packages":{"no-deps":["no-deps@1.0.0","http://127.0.0.1:1/no-deps/-/no-deps-1.0.0.tgz",{},""]}})");

    command::InstallOptions options{};
    options.lockfileOnly = true;
    auto result{command::install_project(dir.path, options)};
    check(result.has_value(), "--lockfile-only with a pinned npm dep succeeds offline");
    if (result) {
        check(result->savedLockfile, "--lockfile-only always saves");
        check(result->lockPackageCount == 2, "package count includes the root");
    }
    const std::string lock{read_file(dir.path / "bun.lock")};
    check(contains(lock, "\"lockfileVersion\": 1,"), "off-registry v1 stays v1");
    check(contains(lock,
                   "\"no-deps\": [\"no-deps@1.0.0\", "
                   "\"http://127.0.0.1:1/no-deps/-/no-deps-1.0.0.tgz\", {}, \"\"],"),
          "npm tuple round-trips byte-identically");
    check(!std::filesystem::exists(dir.path / "node_modules"),
          "--lockfile-only never touches node_modules");
}

// lockfile-version-2.test.ts "unsafe git .bun-tag is rejected only at version
// 2" — the v1 leg: a carried git edge round-trips under
// --frozen-lockfile --lockfile-only without a git client.
void test_lockfile_only_carries_v1_git_edge() {
    TempDir dir{"lockfile-only-git"};
    constexpr std::string_view GIT_URL{"git+ssh://git@127.0.0.1:1/example/repo.git#main"};
    write_file(dir.path / "package.json",
               std::format(R"({{"name":"root","dependencies":{{"dep":"{}"}}}})", GIT_URL));
    write_file(dir.path / "bun.lock",
               std::format(R"({{"lockfileVersion":1,"configVersion":1,)"
                           R"("workspaces":{{"":{{"name":"root","dependencies":{{"dep":"{}"}}}}}},)"
                           R"("packages":{{"dep":["dep@{}",{{}},"../escape"]}}}})",
                           GIT_URL, GIT_URL));

    command::InstallOptions options{};
    options.lockfileOnly = true;
    options.frozenLockfile = true;
    auto result{command::install_project(dir.path, options)};
    check(result.has_value(),
          "v1 git edge parses and --lockfile-only skips the install (exit 0)");
    check(!std::filesystem::exists(dir.path / "node_modules"),
          "no node_modules for the skipped git install");
}

// The v2 legs of the git/github/integrity checks surface bun's messages.
void test_v2_parse_errors_surface_bun_messages() {
    {
        TempDir dir{"v2-gittag"};
        constexpr std::string_view GIT_URL{"git+ssh://git@127.0.0.1:1/example/repo.git#main"};
        write_file(dir.path / "package.json",
                   std::format(R"({{"name":"root","dependencies":{{"dep":"{}"}}}})", GIT_URL));
        write_file(dir.path / "bun.lock",
                   std::format(R"({{"lockfileVersion":2,"configVersion":1,)"
                               R"("workspaces":{{"":{{"name":"root","dependencies":{{"dep":"{}"}}}}}},)"
                               R"("packages":{{"dep":["dep@{}",{{}},"../escape"]}}}})",
                               GIT_URL, GIT_URL));
        auto result{command::install_project(dir.path, {})};
        check(!result.has_value() &&
                  result.error().message == "Invalid git dependency tag",
              "v2 unsafe git tag fails while parsing with bun's message");
    }
    {
        TempDir dir{"v2-integrity"};
        write_file(dir.path / "package.json",
                   R"({"name":"root","dependencies":{"no-deps":"1.0.0"}})");
        write_file(
            dir.path / "bun.lock",
            R"({"lockfileVersion":2,"configVersion":1,)"
            R"("workspaces":{"":{"name":"root","dependencies":{"no-deps":"1.0.0"}}},)"
            R"("packages":{"no-deps":["no-deps@1.0.0","http://127.0.0.1:1/no-deps/-/no-deps-1.0.0.tgz",{},""]}})");
        command::InstallOptions frozen{};
        frozen.frozenLockfile = true;
        auto result{command::install_project(dir.path, frozen)};
        check(!result.has_value() &&
                  result.error().message ==
                      "Missing integrity hash for npm package resolved to a tarball URL "
                      "outside the configured registry",
              "v2 off-registry npm without integrity fails with bun's message");
        check(!std::filesystem::exists(dir.path / "node_modules/no-deps"),
              "the rejected package is never installed");
    }
}

// bad-workspace.test.ts messages, end to end through install_project.
void test_workspace_errors_surface_bun_messages() {
    {
        TempDir dir{"ws-missing"};
        write_file(dir.path / "package.json",
                   R"({"name":"hey","workspaces":["i-dont-exist"]})");
        auto result{command::install_project(dir.path, {})};
        check(!result.has_value() &&
                  result.error().message == "Workspace not found \"i-dont-exist\"",
              "missing workspace fails with bun's message");
    }
    {
        TempDir dir{"ws-non-string"};
        write_file(dir.path / "package.json", R"({"name":"hey","workspaces":[123]})");
        auto result{command::install_project(dir.path, {})};
        check(!result.has_value() &&
                  result.error().message ==
                      "Workspaces expects an array of strings, like:\n"
                      "  \"workspaces\": [\n"
                      "    \"path/to/package\"\n"
                      "  ]",
              "non-string workspaces entry fails with bun's message");
    }
    {
        TempDir dir{"ws-rootlike"};
        write_file(dir.path / "package.json",
                   R"({"name":"my-app","version":"1.0.0",)"
                   R"("workspaces":["./",".\\","some-workspace"]})");
        write_file(dir.path / "some-workspace/package.json",
                   R"({"name":"some-workspace","version":"1.0.0"})");
        auto result{command::install_project(dir.path, {})};
        check(result.has_value(), "'./' and '.\\' workspace entries do not fail");
        const std::string lock{read_file(dir.path / "bun.lock")};
        check(contains(lock, "\"some-workspace\": [\"some-workspace@workspace:some-workspace\"],"),
              "edge-less workspaces still register in the lock");
        check(!std::filesystem::exists(dir.path / "node_modules/some-workspace"),
              "edge-less workspaces are not linked");
    }
}

// Frozen drift uses bun's wording.
void test_frozen_drift_message() {
    TempDir dir{"frozen-drift"};
    write_file(dir.path / "package.json",
               R"({"name":"root","dependencies":{"a":"file:./a","b":"file:./b"}})");
    write_file(dir.path / "a/package.json", R"({"name":"a"})");
    write_file(dir.path / "b/package.json", R"({"name":"b"})");
    write_file(dir.path / "bun.lock",
               R"({"lockfileVersion":1,"configVersion":1,)"
               R"("workspaces":{"":{"name":"root","dependencies":{"a":"file:./a"}}},)"
               R"("packages":{"a":["a@file:a",{}]}})");

    command::InstallOptions frozen{};
    frozen.frozenLockfile = true;
    auto result{command::install_project(dir.path, frozen)};
    check(!result.has_value() &&
              result.error().message == "lockfile had changes, but lockfile is frozen",
          "frozen drift uses bun's message");
    check(!std::filesystem::exists(dir.path / "node_modules/b"),
          "frozen drift installs nothing");
}

}  // namespace

int main() {
    test_fresh_install_writes_v2_lock();
    test_resave_keeps_v1();
    test_v0_floors_to_v1_and_workspace_links();
    test_lockfile_only_round_trips_v1_npm();
    test_lockfile_only_carries_v1_git_edge();
    test_v2_parse_errors_surface_bun_messages();
    test_workspace_errors_surface_bun_messages();
    test_frozen_drift_message();

    std::println("test_command_lockfile: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
