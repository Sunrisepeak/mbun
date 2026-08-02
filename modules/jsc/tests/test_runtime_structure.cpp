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

std::string_view function_body(std::string_view source, std::string_view signature) {
    const auto start{source.find(signature)};
    if (start == std::string_view::npos) return {};
    const auto open{source.find('{', start + signature.size())};
    if (open == std::string_view::npos) return {};
    std::size_t depth{};
    for (std::size_t i{open}; i < source.size(); ++i) {
        if (source[i] == '{') {
            ++depth;
        } else if (source[i] == '}' && --depth == 0) {
            return source.substr(open + 1, i - open - 1);
        }
    }
    return {};
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

    // A deferred N-API finalizer is a fresh callback boundary. A previously
    // caught addon exception must not poison its NAPI_PREAMBLE, while an error
    // raised by the finalizer itself must reach the shared uncaught channel.
    // Keep these two sides together: clearing after the callback made both
    // ordinary finalizers and throwing finalizers silently disappear (#86).
    const auto napi{read_source(runtimeDir / "napi" / "mbun_napi.h")};
    const auto drain{function_body(napi, "inline void drainPendingFinalizers()")};
    check(!drain.empty(), "deferred N-API finalizer drain exists");
    const auto lock{drain.find("JSC::JSLockHolder locker{fin.env->vm()}")};
    const auto prepare{drain.find("prepareFinalizerCallback(fin.env)")};
    const auto invoke{drain.find("fin.cb(fin.env, fin.data, fin.hint)")};
    const auto dispatch{drain.find("dispatchFinalizerExceptions(fin.env)")};
    check(lock != std::string_view::npos && lock < invoke,
          "N-API finalizer holds the JSC API lock while calling addon code");
    check(prepare != std::string_view::npos && prepare < invoke,
          "N-API finalizer starts from a clean callback exception state");
    check(invoke != std::string_view::npos && dispatch != std::string_view::npos &&
              dispatch > invoke,
          "N-API finalizer transfers its own exception to shared uncaught handling");

    if (gFailed != 0) {
        std::println(std::cerr, "test_runtime_structure: {} failed", gFailed);
        return 1;
    }
    std::println("test_runtime_structure: ok");
    return 0;
}
