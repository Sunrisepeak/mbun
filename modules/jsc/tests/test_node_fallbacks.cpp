import std;
import mbun.jsc.node_fallbacks;

namespace {

int checks { 0 };
int failures { 0 };

void expect(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}

} // namespace

int main() {
    using namespace mbun::jsc::node_fallbacks;

    expect(IMPORT_PATH.size() == 24, "VFS prefix preserves Bun's three-word fast path");
    expect(FALLBACKS.size() == 23, "all Bun browser fallback modules are represented");
    const auto* path { lookup_module("path") };
    expect(path != nullptr, "known module resolves");
    expect(path != nullptr && path->source_key == "node-fallbacks/path.js", "source key matches codegen");
    expect(path != nullptr && path->is_esm, "fallback package is ESM");
    expect(path != nullptr && !path->has_side_effects, "fallback package is side-effect free");
    expect(lookup_module("fs") == nullptr, "unsupported fallback stays absent");
    const auto* assertPath { lookup_path("/bun-vfs$$/node_modules/assert/index.js") };
    expect(assertPath != nullptr, "virtual index path returns a descriptor");
    expect(assertPath != nullptr && assertPath->name == "assert", "virtual index path resolves");
    const auto* zlibPath { lookup_path("/bun-vfs$$/node_modules/zlib/package.json") };
    expect(zlibPath != nullptr, "virtual package path returns a descriptor");
    expect(zlibPath != nullptr && zlibPath->name == "zlib", "virtual package path resolves");
    expect(lookup_path("node_modules/path/index.js") == nullptr, "non-VFS path is rejected");

    std::println("node fallback seam: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
