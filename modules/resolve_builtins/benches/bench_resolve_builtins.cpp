// bench_resolve_builtins — micro-benchmark for the builtin resolver hot path.
//
// resolve() is called once per import specifier during module resolution. The
// mix below is representative of a real dependency graph: bare Node names (the
// path that used to heap-allocate "node:"+specifier on every call), fully
// qualified node:/bun: names, and non-builtin misses (userland packages).
// Warmup + checksum guard mirror the semver bench so results are DCE-safe.
import std;
import mbun.resolve_builtins;

namespace {

long now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

int main(int argc, char* argv[]) {
    const int iters = argc > 1 ? std::stoi(argv[1]) : 200000;

    // ~40% bare Node names (the allocation-prone branch), ~30% qualified
    // builtins (early loop hits), ~30% userland misses (full scan + bare scan).
    const std::vector<std::string_view> specifiers = {
        "fs", "path", "events", "stream", "crypto", "util", "os", "buffer",
        "node:fs", "node:path", "bun:sqlite", "bun:test", "node:http",
        "react", "lodash", "express", "@scope/pkg", "my-app-module",
        "node:stream/promises", "assert", "url", "zlib",
    };

    using namespace mbun::resolve_builtins;

    long checksum = 0;
    for (int k = 0; k < 3; ++k) {  // warmup
        for (auto s : specifiers) {
            checksum += static_cast<long>(resolve(s).kind);
        }
    }

    const long t0 = now_ns();
    for (int k = 0; k < iters; ++k) {
        for (auto s : specifiers) {
            const auto r = resolve(s);
            checksum += static_cast<long>(r.kind) + (r.mapping ? 1 : 0);
        }
    }
    const long ns = now_ns() - t0;
    const long ops = static_cast<long>(specifiers.size()) * iters;

    std::println("resolve: {} ops in {} ns  ->  {:.2f} ns/op  (checksum={})",
                 ops, ns, static_cast<double>(ns) / static_cast<double>(ops), checksum);
    return 0;
}
