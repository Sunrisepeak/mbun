// mbun 侧 glob 基准 driver — 与 benchmarks/suites/glob/bench.mjs 完全同口径
// （同 10 组 pattern/path、同热循环结构、同 checksum）。用法: bench-glob [iters]
import std;
import mbun.glob;

namespace {

// 与 compat/bun/bench/glob/match.mjs 一致的 10 组（UTF-8 字节；mbun.glob 按 WTF-8 字节匹配，
// 与 bun 交给其 matcher 的字节一致）。
constexpr std::array<std::pair<std::string_view, std::string_view>, 10> CASES{{
    {"1{2,3{4,5{6,7{8,9{a,b{c,d{e,f{g,h{i,j{k,l}}}}}}}}}}m", "13579bdfhjlm"},
    {"\U0001F60E/¢£.{ts,tsx,js,jsx}", "\U0001F60E/¢£.jsx"},
    {"フォルダ/**/*", "フォルダ/aaa.js"},
    {"1{2,3{4,5{6,7{8,\U0001F60E{a,b{c,d{e,f{g,h{i,j{k,l}}}}}}}}}}m", "1357\U0001F60Ebdfhjlm"},
    {"test/{foo/**,bar}/baz", "test/bar/baz"},
    {"a/**/c/*.md", "a/bb.bb/aa/b.b/aa/c/xyz.md"},
    {"a/b/**/c{d,e}/**/xyz.md", "a/b/cd/xyz.md"},
    {"foo/bar/**/one/**/*.*", "foo/bar/baz/one/two/three/image.png"},
    {"some/**/needle.{js,tsx,mdx,ts,jsx,txt}", "some/a/bigger/path/to/the/crazy/needle.txt"},
    {"f[^eiu][^eiu][^eiu][^eiu][^eiu]r", "foo-bar"},
}};

long now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

int main(int argc, char* argv[]) {
    const int iters = argc > 1 ? std::stoi(argv[1]) : 200;

    long checksum = 0;
    for (int k = 0; k < 3; ++k) {  // 预热
        for (auto& [g, p] : CASES) checksum += mbun::glob::match(g, p) ? 1 : 0;
    }

    auto t0 = now_ns();
    for (int k = 0; k < iters; ++k) {
        for (auto& [g, p] : CASES) checksum += mbun::glob::match(g, p) ? 1 : 0;
    }
    auto ns = now_ns() - t0;
    auto ops = static_cast<long>(CASES.size()) * iters;

    std::println(R"({{"impl":"mbun {}","match_ops_per_s":{},"checksum":{}}})", "0.1.0",
                 ns > 0 ? ops * 1'000'000'000 / ns : 0, checksum);
    return 0;
}
