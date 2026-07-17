// test_command.cpp — real filesystem coverage for the offline `mbun install`
// vertical slice. The v1 lock fixture is adapted without weakening assertions
// from compat/bun/test/cli/install/lockfile-version-2.test.ts "existing v1 lockfile".
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
               std::format("mbun-install-{}-{}", label, nonce);
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

constexpr std::string_view V1_LOCK{R"LOCK({
  "lockfileVersion": 1,
  "configVersion": 1,
  "workspaces": {
    "": {
      "name": "root",
      "dependencies": {
        "dep": "file:./dep"
      }
    }
  },
  "packages": {
    "dep": ["dep@file:dep", {}]
  }
})LOCK"};

void test_existing_text_lock_installs_real_folder() {
    TempDir fixture{"existing-v1"};
    write_file(fixture.path / "package.json",
               R"JSON({"name":"root","dependencies":{"dep":"file:./dep"}})JSON");
    write_file(fixture.path / "dep/package.json",
               R"JSON({"name":"dep","version":"1.0.0"})JSON");
    write_file(fixture.path / "dep/index.js", "module.exports = 42;\n");
    write_file(fixture.path / "dep/assets/value.txt", "linked directory payload\n");
    std::filesystem::create_symlink("index.js", fixture.path / "dep/file-link.js");
    std::filesystem::create_directory_symlink("assets", fixture.path / "dep/directory-link");
    write_file(fixture.path / "bun.lock", V1_LOCK);

    auto result{command::install_project(fixture.path)};
    check(result.has_value(), "existing v1 text lock installs");
    if (result) {
        check(result->installed == 1, "summary reports one installed package");
        check(result->loaded_text_lockfile, "summary reports loaded text lockfile");
    }
    check(read_file(fixture.path / "node_modules/dep/package.json") ==
              R"JSON({"name":"dep","version":"1.0.0"})JSON",
          "dependency package.json is physically installed");
    check(read_file(fixture.path / "node_modules/dep/index.js") == "module.exports = 42;\n",
          "dependency payload is physically installed");
    const auto installedFileLink{fixture.path / "node_modules/dep/file-link.js"};
    const auto installedDirectoryLink{fixture.path / "node_modules/dep/directory-link"};
    const bool fileLinkPreserved{
        std::filesystem::is_symlink(std::filesystem::symlink_status(installedFileLink))};
    check(fileLinkPreserved, "file symlink remains a symlink");
    if (fileLinkPreserved) {
        check(std::filesystem::read_symlink(installedFileLink) == "index.js",
              "file symlink target is preserved");
    }
    const bool directoryLinkPreserved{
        std::filesystem::is_symlink(std::filesystem::symlink_status(installedDirectoryLink))};
    check(directoryLinkPreserved, "directory symlink remains a symlink");
    if (directoryLinkPreserved) {
        check(std::filesystem::read_symlink(installedDirectoryLink) == "assets",
              "directory symlink target is preserved");
    }

    write_file(fixture.path / "dep/second.js", "export default 7;\n");
    auto second{command::install_project(fixture.path)};
    check(second.has_value() && second->installed == 1, "reinstall succeeds and replaces target");
    check(read_file(fixture.path / "node_modules/dep/second.js") == "export default 7;\n",
          "reinstall materializes newly added source file");
}

void test_nested_cwd_finds_package_root() {
    TempDir fixture{"nested-root"};
    write_file(fixture.path / "package.json",
               R"JSON({"name":"root","dependencies":{"dep":"file:./dep"}})JSON");
    write_file(fixture.path / "dep/package.json", R"JSON({"name":"dep"})JSON");
    write_file(fixture.path / "bun.lock", V1_LOCK);
    std::filesystem::create_directories(fixture.path / "src/deep");

    auto result{command::install_project(fixture.path / "src/deep")};
    check(result.has_value(), "nested cwd walks up to package root");
    check(std::filesystem::exists(fixture.path / "node_modules/dep/package.json"),
          "nested cwd installs into root node_modules");
}

void test_missing_dependency_manifest_fails_without_target() {
    TempDir fixture{"missing-manifest"};
    write_file(fixture.path / "package.json",
               R"JSON({"name":"root","dependencies":{"dep":"file:./dep"}})JSON");
    write_file(fixture.path / "bun.lock", V1_LOCK);

    auto result{command::install_project(fixture.path)};
    check(!result.has_value(), "missing dependency package.json fails");
    if (!result) {
        check(result.error().code == command::ErrorCode::MissingDependencyPackageJson,
              "missing dependency has stable error category");
    }
    check(!std::filesystem::exists(fixture.path / "node_modules/dep"),
          "failed install does not leave target package");
}

void test_unsafe_aliases_are_rejected() {
    constexpr std::array UNSAFE_ALIASES{std::string_view{"a/b/c"},
                                        std::string_view{".bin/tool"}};
    for (std::string_view alias : UNSAFE_ALIASES) {
        TempDir fixture{"unsafe-alias"};
        write_file(fixture.path / "package.json",
                   std::format(R"JSON({{"name":"root","dependencies":{{"{}":"file:./dep"}}}})JSON",
                               alias));

        auto result{command::install_project(fixture.path)};
        check(!result && result.error().code == command::ErrorCode::UnsafePackageName,
              std::format("unsafe alias '{}' is rejected", alias));
        check(!std::filesystem::exists(fixture.path / "node_modules"),
              std::format("unsafe alias '{}' creates no install tree", alias));
    }
}

void test_missing_optional_file_dependency_is_skipped() {
    TempDir fixture{"missing-optional"};
    write_file(fixture.path / "package.json",
               R"JSON({"name":"root","optionalDependencies":{"dep":"file:./missing"}})JSON");

    auto result{command::install_project(fixture.path)};
    check(result.has_value() && result->installed == 0,
          "missing optional file dependency is skipped");
    check(!std::filesystem::exists(fixture.path / "node_modules/dep"),
          "missing optional dependency creates no target");
}

void test_non_string_dependency_is_invalid_package_json() {
    TempDir fixture{"non-string-dependency"};
    write_file(fixture.path / "package.json",
               R"JSON({"name":"root","dependencies":{"dep":42}})JSON");

    auto result{command::install_project(fixture.path)};
    check(!result && result.error().code == command::ErrorCode::InvalidPackageJson,
          "non-string dependency value rejects package.json");
    check(!std::filesystem::exists(fixture.path / "node_modules"),
          "invalid dependency value creates no install tree");
}

void test_frozen_install_requires_text_lockfile() {
    TempDir fixture{"frozen-no-lock"};
    write_file(fixture.path / "package.json",
               R"JSON({"name":"root","dependencies":{"dep":"file:./dep"}})JSON");
    write_file(fixture.path / "dep/package.json", R"JSON({"name":"dep"})JSON");

    command::InstallOptions options{};
    options.frozenLockfile = true;
    auto result{command::install_project(fixture.path, options)};
    check(!result && result.error().code == command::ErrorCode::LockfileOutOfDate,
          "frozen install without text lockfile remains RED");
    check(!std::filesystem::exists(fixture.path / "node_modules/dep"),
          "frozen lockfile failure creates no package");
}

void test_lifecycle_scripts_require_ignore_scripts() {
    TempDir fixture{"lifecycle-red"};
    write_file(fixture.path / "package.json",
               R"JSON({"name":"root","dependencies":{"dep":"file:./dep"}})JSON");
    write_file(fixture.path / "dep/package.json",
               R"JSON({"name":"dep","scripts":{"install":"node install.js"}})JSON");
    write_file(fixture.path / "bun.lock", V1_LOCK);

    auto result{command::install_project(fixture.path)};
    check(!result && result.error().code == command::ErrorCode::UnsupportedLifecycleScripts,
          "lifecycle execution remains explicit RED");
    check(!std::filesystem::exists(fixture.path / "node_modules/dep"),
          "lifecycle RED creates no package");

    command::InstallOptions options{};
    options.ignoreScripts = true;
    auto ignored{command::install_project(fixture.path, options)};
    check(ignored.has_value() && ignored->installed == 1,
          "--ignore-scripts permits the supported filesystem slice");
}

void test_deferred_paths_fail_explicitly() {
    {
        // The registry path is wired now (see test_install_e2e.cpp for the
        // green pipeline); an unreachable registry must surface an honest
        // ManifestFetchFailed and never fake an install.
        TempDir fixture{"registry-unreachable"};
        write_file(fixture.path / "package.json",
                   R"JSON({"name":"root","dependencies":{"left-pad":"1.3.0"}})JSON");
        command::InstallOptions options{};
        options.registry = "http://127.0.0.1:1/";  // reserved port: refused
        auto result{command::install_project(fixture.path, options)};
        check(!result && result.error().code == command::ErrorCode::ManifestFetchFailed,
              "unreachable registry fails with ManifestFetchFailed");
        check(!std::filesystem::exists(fixture.path / "node_modules/left-pad"),
              "unreachable registry creates no fake package");
    }
    {
        TempDir fixture{"tarball-red"};
        write_file(fixture.path / "package.json",
                   R"JSON({"name":"root","dependencies":{"dep":"file:./dep.tgz"}})JSON");
        auto result{command::install_project(fixture.path)};
        check(!result && result.error().code == command::ErrorCode::UnsupportedLocalTarball,
              "local tarball path remains explicit RED");
        check(!std::filesystem::exists(fixture.path / "node_modules/dep"),
              "tarball RED creates no fake package");
    }
    {
        // A bun.lockb is no longer rejected outright (the reader is wired up),
        // but a truncated one -- header present, format word absent -- must
        // still fail loudly rather than be treated as "no lockfile".
        TempDir fixture{"lockb-truncated"};
        write_file(fixture.path / "package.json", R"JSON({"name":"root"})JSON");
        write_file(fixture.path / "bun.lockb",
                   "#!/usr/bin/env bun\nbun-lockfile-format-v0\n");
        auto result{command::install_project(fixture.path)};
        check(!result && result.error().code == command::ErrorCode::InvalidBinaryLockfile,
              "truncated bun.lockb is a parse error, not a silent skip");
    }
}

}  // namespace

int main() {
    test_existing_text_lock_installs_real_folder();
    test_nested_cwd_finds_package_root();
    test_missing_dependency_manifest_fails_without_target();
    test_unsafe_aliases_are_rejected();
    test_missing_optional_file_dependency_is_skipped();
    test_non_string_dependency_is_invalid_package_json();
    test_frozen_install_requires_text_lockfile();
    test_lifecycle_scripts_require_ignore_scripts();
    test_deferred_paths_fail_explicitly();

    std::println("test_command: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
