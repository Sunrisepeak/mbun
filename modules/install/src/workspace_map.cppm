// workspace_map.cppm — process the root package.json `workspaces` field into a
// path→(name, version) map.
//
// Mechanical port of bun's WorkspaceMap::process_names_array:
//   ref: bun-ref/src/install/lockfile/Package/WorkspaceMap.rs:169-330
//     - non-string entry → "Workspaces expects an array of strings, ..."
//     - ""/"."/"./"/".\\" entries are skipped
//     - glob entries expand against `<pattern>/package.json` on disk
//     - non-glob entries must have a package.json → "Workspace not found ..."
//     - package.json without a "name" → "Missing \"name\" from package.json ..."
//   ref: bun-ref/src/glob/lib.rs:21-55 detect_glob_syntax
//
// Matching reuses mbun.glob (the Bun.Glob matcher port); the walk skips
// node_modules/.git like bun's ignored_workspace_paths.
export module mbun.install.workspace_map;

import std;
import mbun.glob;
import mbun.install.npm.json;

namespace mbun::install::workspace_map {

export struct Entry {
    std::string relPath;  // posix-separated path relative to the root
    std::string name;
    std::string version;  // empty when absent
};

export struct Error {
    std::string message;
};

export using Result = std::expected<std::vector<Entry>, Error>;

// ref: glob/lib.rs detect_glob_syntax — special tokens "*{[?" not escaped by
// an odd number of backslashes; a leading '!' (negation) is glob syntax too.
export bool detect_glob_syntax(std::string_view pattern) {
    if (!pattern.empty() && pattern.front() == '!') {
        return true;
    }
    constexpr std::string_view SPECIAL{"*{[?"};
    for (char token : SPECIAL) {
        std::size_t from{0};
        while (from < pattern.size()) {
            const std::size_t idx{pattern.find(token, from)};
            if (idx == std::string_view::npos) {
                break;
            }
            std::size_t backslashes{0};
            for (std::size_t i{idx}; i > 0 && pattern[i - 1] == '\\'; --i) {
                ++backslashes;
            }
            if (backslashes % 2 == 0) {
                return true;
            }
            from = idx + 1;
        }
    }
    return false;
}

namespace detail {

// bun matches `<pattern>/package.json` against every walked package.json path
// (WorkspaceMap.rs:335-341); appending the fixed suffix to both sides keeps
// the component-boundary semantics identical.
bool glob_match(std::string_view pattern, std::string_view relPath) {
    std::string patternWithManifest{pattern};
    patternWithManifest += "/package.json";
    std::string pathWithManifest{relPath};
    pathWithManifest += "/package.json";
    return mbun::glob::match(patternWithManifest, pathWithManifest);
}

// Normalize a workspaces entry to a posix relative path: backslashes become
// slashes (bun path-normalizes input entries), "./" prefixes drop, trailing
// slash drops.
std::string normalize_entry(std::string_view input) {
    std::string path{input};
    std::ranges::replace(path, '\\', '/');
    while (path.starts_with("./")) {
        path.erase(0, 2);
    }
    while (path.ends_with('/')) {
        path.pop_back();
    }
    return path;
}

struct PackageIdentity {
    std::string name;
    std::string version;
};

enum class ReadError { NotFound, Invalid, MissingName };

std::expected<PackageIdentity, ReadError>
read_package_identity(const std::filesystem::path& packageJsonPath) {
    std::ifstream stream{packageJsonPath, std::ios::binary};
    if (!stream) {
        return std::unexpected(ReadError::NotFound);
    }
    std::string contents{std::istreambuf_iterator<char>{stream},
                         std::istreambuf_iterator<char>{}};
    auto document{mbun::install::npm::json::parse(contents)};
    if (!document || !document->root || !document->root->is_object()) {
        return std::unexpected(ReadError::Invalid);
    }
    PackageIdentity identity{};
    if (const auto* name{document->root->get("name")}) {
        if (auto text{name->as_str()}) {
            identity.name.assign(*text);
        }
    }
    if (identity.name.empty()) {
        return std::unexpected(ReadError::MissingName);
    }
    if (const auto* version{document->root->get("version")}) {
        if (auto text{version->as_str()}) {
            identity.version.assign(*text);
        }
    }
    return identity;
}

// Walk `root` collecting directories that contain a package.json and match the
// pattern. node_modules/.git subtrees are never entered (bun's
// ignored_workspace_paths), and neither is the root itself.
std::vector<std::string> expand_glob(const std::filesystem::path& root,
                                     std::string_view pattern) {
    std::vector<std::string> matches;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it{
        root, std::filesystem::directory_options::skip_permission_denied, ec};
    const std::filesystem::recursive_directory_iterator end{};
    if (ec) {
        return matches;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_directory(ec)) {
            continue;
        }
        const std::string dirName{it->path().filename().string()};
        if (dirName == "node_modules" || dirName == ".git") {
            it.disable_recursion_pending();
            continue;
        }
        const std::filesystem::path rel{it->path().lexically_relative(root)};
        const std::string relPosix{rel.generic_string()};
        if (glob_match(pattern, relPosix)) {
            if (std::filesystem::is_regular_file(it->path() / "package.json", ec)) {
                matches.push_back(relPosix);
            }
        }
    }
    std::ranges::sort(matches);
    return matches;
}

}  // namespace detail

// Process the `workspaces` field (array, or object carrying a `packages`
// array). A null field yields an empty map. Error messages are bun's, with
// pretty-markup tags stripped.
export Result collect(const std::filesystem::path& root,
                      const mbun::install::npm::json::Value* workspacesField) {
    std::vector<Entry> entries;
    if (workspacesField == nullptr) {
        return entries;
    }
    const mbun::install::npm::json::Value* array{workspacesField};
    if (workspacesField->is_object()) {
        array = workspacesField->get("packages");
        if (array == nullptr) {
            return entries;
        }
    }
    constexpr std::string_view NOT_STRINGS_MESSAGE{
        "Workspaces expects an array of strings, like:\n"
        "  \"workspaces\": [\n"
        "    \"path/to/package\"\n"
        "  ]"};
    if (!array->is_array()) {
        return std::unexpected(Error{std::string{NOT_STRINGS_MESSAGE}});
    }

    std::vector<std::string> globs;
    auto add_entry{[&entries](std::string relPath, detail::PackageIdentity identity) {
        const bool duplicate{std::ranges::any_of(
            entries, [&relPath](const Entry& e) { return e.relPath == relPath; })};
        if (!duplicate) {
            entries.push_back(Entry{std::move(relPath), std::move(identity.name),
                                    std::move(identity.version)});
        }
    }};

    for (const auto& item : array->items) {
        if (!item) {
            return std::unexpected(Error{std::string{NOT_STRINGS_MESSAGE}});
        }
        const auto text{item->as_str()};
        if (!text) {
            return std::unexpected(Error{std::string{NOT_STRINGS_MESSAGE}});
        }
        const std::string_view input{*text};
        // ref: WorkspaceMap.rs:206-211 — the root itself is never a workspace.
        if (input.empty() || input == "." || input == "./" || input == ".\\") {
            continue;
        }
        if (detect_glob_syntax(input)) {
            globs.emplace_back(detail::normalize_entry(input));
            continue;
        }
        const std::string relPath{detail::normalize_entry(input)};
        if (relPath.empty()) {
            continue;
        }
        std::error_code ec;
        const std::filesystem::path dir{
            std::filesystem::weakly_canonical(root / relPath, ec)};
        if (!ec && dir == std::filesystem::weakly_canonical(root, ec)) {
            continue;  // resolves back to the root package.json — skip
        }
        auto identity{detail::read_package_identity(root / relPath / "package.json")};
        if (!identity) {
            switch (identity.error()) {
                case detail::ReadError::NotFound:
                case detail::ReadError::Invalid:
                    return std::unexpected(Error{
                        std::format("Workspace not found \"{}\"", input)});
                case detail::ReadError::MissingName:
                    return std::unexpected(Error{std::format(
                        "Missing \"name\" from package.json in {}", input)});
            }
        }
        add_entry(relPath, std::move(*identity));
    }

    for (const std::string& pattern : globs) {
        for (std::string& relPath : detail::expand_glob(root, pattern)) {
            auto identity{detail::read_package_identity(root / relPath / "package.json")};
            if (!identity) {
                if (identity.error() == detail::ReadError::MissingName) {
                    return std::unexpected(Error{std::format(
                        "Missing \"name\" from package.json in {}", relPath)});
                }
                continue;  // raced away / unreadable: a glob match is best-effort
            }
            add_entry(std::move(relPath), std::move(*identity));
        }
    }

    return entries;
}

}  // namespace mbun::install::workspace_map
