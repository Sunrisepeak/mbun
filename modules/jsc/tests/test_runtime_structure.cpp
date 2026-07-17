// Structural contract for the runtime hotspot split. Behavior tests protect
// the public API; this test prevents the physical implementation slices from
// collapsing back into a single >2000-line source file.
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

void check_source(const std::filesystem::path& root, std::string_view relative) {
    const auto path{root / relative};
    check(std::filesystem::is_regular_file(path), std::string{relative} + " exists");
    const auto source{read_source(path)};
    check(!source.empty(), std::string{relative} + " is non-empty");
    check(line_count(source) <= 2000, std::string{relative} + " respects line budget");
}

}  // namespace

int main() {
    const auto root{source_root()};
    const auto runtime{read_source(root / "runtime.cppm")};
    check(runtime.contains("export module mbun.jsc.runtime;"), "public runtime module is preserved");
    check(!runtime.contains("export import :"), "broken partition graph is not used");
    check(line_count(runtime) <= 2000, "runtime.cppm respects line budget");

    const std::array<std::string_view, 15> slices{
        "runtime/prelude.hpp",
        "runtime/common.inc",
        "runtime/jsc_internal.hpp",
        "runtime/core_bindings.inc",
        "runtime/webcrypto.inc",
        "runtime/sourcemap.inc",
        "runtime/io_bindings.inc",
        "runtime/shell.inc",
        "runtime/process_base.inc",
        "runtime/process_extended.inc",
        "runtime/net.inc",
        "runtime/serve_native.inc",
        "runtime/dns.inc",
        "runtime/engine.inc",
        "runtime/api_impl.inc",
    };
    for (const auto slice : slices) check_source(root, slice);

    std::size_t previous{};
    for (const auto slice : slices | std::views::drop(1)) {
        const std::string include{"#include \"" + std::string{slice} + "\""};
        const auto position{runtime.find(include)};
        check(position != std::string::npos, include + " is present");
        check(position >= previous, include + " keeps source order");
        previous = position;
    }

    const auto runtimeDir{root / "runtime"};
    for (const auto& entry : std::filesystem::directory_iterator{runtimeDir}) {
        if (!entry.is_regular_file()) continue;
        const std::string relative{"runtime/" + entry.path().filename().string()};
        check(std::ranges::find(slices, relative) != slices.end(),
              relative + " is a registered physical slice");
        check(line_count(read_source(entry.path())) <= 2000,
              relative + " respects line budget even when unregistered");
    }

    if (gFailed != 0) {
        std::println(std::cerr, "test_runtime_structure: {} failed", gFailed);
        return 1;
    }
    std::println("test_runtime_structure: ok");
    return 0;
}
