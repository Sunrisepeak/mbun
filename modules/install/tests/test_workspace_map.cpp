// test_workspace_map.cpp — root `workspaces` field processing against real
// temp directories. Error text matches bun's (WorkspaceMap.rs), markup
// stripped; fixtures mirror compat/bun/test/cli/install/bad-workspace.test.ts.
import std;
import mbun.install.npm.json;
import mbun.install.workspace_map;

namespace json = mbun::install::npm::json;
namespace wsmap = mbun::install::workspace_map;

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
               std::format("mbun-wsmap-{}-{}", label, nonce);
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

// Keeps the parsed document alive alongside the collect() result.
wsmap::Result collect_for(const std::filesystem::path& root, std::string_view packageJson) {
    auto document{json::parse(packageJson)};
    if (!document || !document->root) {
        return std::unexpected(wsmap::Error{"fixture package.json failed to parse"});
    }
    return wsmap::collect(root, document->root->get("workspaces"));
}

void test_missing_workspace_reports_bun_error() {
    TempDir dir{"missing"};
    auto result{collect_for(dir.path, R"({"name":"hey","workspaces":["i-dont-exist"]})")};
    check(!result.has_value(), "missing workspace directory fails");
    if (!result) {
        check(result.error().message == "Workspace not found \"i-dont-exist\"",
              "missing workspace uses bun's message");
    }
}

void test_non_string_entry_reports_bun_error() {
    TempDir dir{"non-string"};
    auto result{collect_for(dir.path, R"({"name":"hey","workspaces":[123]})")};
    check(!result.has_value(), "non-string workspaces entry fails");
    if (!result) {
        check(result.error().message ==
                  "Workspaces expects an array of strings, like:\n"
                  "  \"workspaces\": [\n"
                  "    \"path/to/package\"\n"
                  "  ]",
              "non-string entry uses bun's message (markup stripped)");
    }
}

void test_root_like_entries_are_skipped() {
    TempDir dir{"root-like"};
    write_file(dir.path / "some-workspace/package.json",
               R"({"name":"some-workspace","version":"1.0.0"})");
    auto result{collect_for(
        dir.path, R"({"name":"my-app","workspaces":["./",".\\",".","","some-workspace"]})")};
    check(result.has_value(), R"("./" / ".\" / "." / "" entries never fail)");
    if (result) {
        check(result->size() == 1, "only the real workspace registers");
        if (!result->empty()) {
            check(result->front().relPath == "some-workspace" &&
                      result->front().name == "some-workspace" &&
                      result->front().version == "1.0.0",
                  "workspace path, name and version are read from its package.json");
        }
    }
}

void test_glob_expansion_matches_packages() {
    TempDir dir{"glob"};
    write_file(dir.path / "packages/pkg1/package.json", R"({"name":"pkg1"})");
    write_file(dir.path / "packages/pkg2/package.json",
               R"({"name":"@scope/pkg2","version":"2.0.0"})");
    write_file(dir.path / "packages/not-a-pkg/README.md", "no manifest here");
    write_file(dir.path / "node_modules/hidden/package.json", R"({"name":"hidden"})");
    auto result{collect_for(dir.path, R"({"name":"root","workspaces":["packages/*"]})")};
    check(result.has_value(), "glob workspaces expand");
    if (result) {
        check(result->size() == 2, "glob matches exactly the manifest-bearing dirs");
        const bool hasPkg1{std::ranges::any_of(*result, [](const wsmap::Entry& e) {
            return e.relPath == "packages/pkg1" && e.name == "pkg1" && e.version.empty();
        })};
        const bool hasPkg2{std::ranges::any_of(*result, [](const wsmap::Entry& e) {
            return e.relPath == "packages/pkg2" && e.name == "@scope/pkg2" &&
                   e.version == "2.0.0";
        })};
        check(hasPkg1 && hasPkg2, "glob entries carry names and versions");
    }
}

void test_missing_name_reports_bun_error() {
    TempDir dir{"noname"};
    write_file(dir.path / "ws/package.json", R"({"version":"1.0.0"})");
    auto result{collect_for(dir.path, R"({"name":"root","workspaces":["ws"]})")};
    check(!result.has_value(), "workspace without a name fails");
    if (!result) {
        check(result.error().message == "Missing \"name\" from package.json in ws",
              "missing name uses bun's message");
    }
}

void test_detect_glob_syntax() {
    check(wsmap::detect_glob_syntax("packages/*"), "star is glob syntax");
    check(wsmap::detect_glob_syntax("p?kg"), "question mark is glob syntax");
    check(wsmap::detect_glob_syntax("{a,b}"), "braces are glob syntax");
    check(wsmap::detect_glob_syntax("[ab]"), "brackets are glob syntax");
    check(wsmap::detect_glob_syntax("!negated"), "leading negation is glob syntax");
    check(!wsmap::detect_glob_syntax("plain/path"), "plain path is not glob syntax");
    check(!wsmap::detect_glob_syntax("esc\\*aped"), "escaped star is not glob syntax");
}

}  // namespace

int main() {
    test_missing_workspace_reports_bun_error();
    test_non_string_entry_reports_bun_error();
    test_root_like_entries_are_skipped();
    test_glob_expansion_matches_packages();
    test_missing_name_reports_bun_error();
    test_detect_glob_syntax();

    std::println("test_workspace_map: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
