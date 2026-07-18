// Structural contract for the runtime hotspot split. Behavior tests protect
// the public API; this test prevents the physical implementation slices from
// collapsing back into a single >2000-line source file.
//
// The registered-slice set is DERIVED from the sources (runtime.cppm's
// #include lines plus nested includes from the slices themselves, e.g.
// prelude.hpp -> icu_decompress.inc) instead of a hand-maintained snapshot —
// the old hardcoded list silently drifted five slices behind reality.
import std;

namespace {

int gFailed{};

void check(bool condition, std::string_view message) {
    if (condition) return;
    std::println(std::cerr, "FAIL: {}", message);
    ++gFailed;
}

std::filesystem::path source_root() {
    const auto testFile{std::filesystem::path{__FILE__}};
    return testFile.parent_path().parent_path() / "src";
}

std::string read_source(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::size_t line_count(std::string_view source) {
    return static_cast<std::size_t>(std::ranges::count(source, '\n'))
        + (!source.empty() && source.back() != '\n' ? 1U : 0U);
}

// Collect the filenames (relative to runtime/) referenced by #include lines in
// `source`. Accepts both `#include "runtime/x.inc"` (from runtime.cppm) and
// `#include "x.inc"` (from a slice inside runtime/).
std::vector<std::string> collect_includes(std::string_view source) {
    std::vector<std::string> found;
    constexpr std::string_view NEEDLE{"#include \""};
    std::size_t pos{0};
    while ((pos = source.find(NEEDLE, pos)) != std::string_view::npos) {
        pos += NEEDLE.size();
        const auto end{source.find('"', pos)};
        if (end == std::string_view::npos) break;
        std::string path{source.substr(pos, end - pos)};
        pos = end + 1;
        if (path.starts_with("runtime/")) path.erase(0, std::string_view{"runtime/"}.size());
        if (path.find('/') != std::string::npos) continue;  // outside runtime/
        if (path.starts_with("<")) continue;
        found.push_back(std::move(path));
    }
    return found;
}

}  // namespace

int main() {
    const auto root{source_root()};
    const auto runtime{read_source(root / "runtime.cppm")};
    check(runtime.contains("export module mbun.jsc.runtime;"), "public runtime module is preserved");
    check(!runtime.contains("export import :"), "broken partition graph is not used");
    check(line_count(runtime) <= 2000, "runtime.cppm respects line budget");

    // Registered = reachable from the module unit through #include, to a fixpoint.
    std::vector<std::string> queue{collect_includes(runtime)};
    check(!queue.empty(), "runtime.cppm registers physical slices");
    std::set<std::string> registered;
    while (!queue.empty()) {
        const std::string name{std::move(queue.back())};
        queue.pop_back();
        if (!registered.insert(name).second) continue;
        const auto path{root / "runtime" / name};
        check(std::filesystem::is_regular_file(path), "runtime/" + name + " exists");
        if (!std::filesystem::is_regular_file(path)) continue;
        const auto source{read_source(path)};
        check(!source.empty(), "runtime/" + name + " is non-empty");
        check(line_count(source) <= 2000, "runtime/" + name + " respects line budget");
        for (auto& nested : collect_includes(source)) queue.push_back(std::move(nested));
    }

    // Every top-level file in runtime/ must be registered (no dead slices) and
    // budget-bound either way.
    const auto runtimeDir{root / "runtime"};
    for (const auto& entry : std::filesystem::directory_iterator{runtimeDir}) {
        if (!entry.is_regular_file()) continue;
        const std::string name{entry.path().filename().string()};
        check(registered.contains(name), "runtime/" + name + " is a registered physical slice");
        check(line_count(read_source(entry.path())) <= 2000,
              "runtime/" + name + " respects line budget even when unregistered");
    }

    if (gFailed != 0) {
        std::println(std::cerr, "test_runtime_structure: {} failed", gFailed);
        return 1;
    }
    std::println("test_runtime_structure: ok");
    return 0;
}
