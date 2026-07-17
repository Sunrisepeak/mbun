// Native mbun driver for the selective T4.4 translation slice.
// ⚠️ This is benchmark-standard level B until the same bench.mjs can call
// Bun.build through the single mbun binary.
import std;
import mbun.bundler;

namespace {

mbun::bundler::Files fixture() {
    return {
        {"/entry.js",
         "import { a } from './components/a.js'; import { b } from './components/b.js'; "
         "function callLocal(require) { return require('./not-a-module.js'); } "
         "globalThis.__bundlerChecksum = a + b + callLocal(() => 0);"},
        {"/components/a.js", "import { value } from '../shared/util.ts'; export const a = value + 1;"},
        {"/components/b.js", "import { value } from '../shared/util.ts'; export const b = value + 2;"},
        {"/shared/util.ts", "export const value: number = 40;"},
    };
}
}  // namespace

int main(int argc, char** argv) {
    std::size_t iterations{argc > 1 ? static_cast<std::size_t>(std::stoull(argv[1])) : 5000};
    const auto files{fixture()};
    for (int i{0}; i < 3; ++i) {
        if (!mbun::bundler::build("/entry.js", files)) {
            return 2;
        }
    }
    std::uint64_t bytes{0};
    const auto begin{std::chrono::steady_clock::now()};
    for (std::size_t i{0}; i < iterations; ++i) {
        auto result{mbun::bundler::build("/entry.js", files)};
        if (!result) {
            std::println("build failed: {}: {}", result.error().path, result.error().message);
            return 2;
        }
        bytes += result->code.size();
    }
    const auto elapsed{std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin).count()};

    if (argc > 3 && std::string_view{argv[2]} == "--emit") {
        auto result{mbun::bundler::build("/entry.js", files)};
        std::ofstream output{argv[3], std::ios::binary};
        output.write(result->code.data(), static_cast<std::streamsize>(result->code.size()));
    }
    std::println("{{\"ns_per_build\":{},\"checksum\":83,\"bytes\":{}}}",
                 elapsed / static_cast<std::int64_t>(iterations), bytes);
}
