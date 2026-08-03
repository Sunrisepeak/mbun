// mbun 侧 TOML 基准 driver — 与 benchmarks/suites/toml/bench.mjs 完全同口径
// （同 fixture、同热循环、同 checksum = iters × 顶层键数）。用法: bench-toml <fixture> [iters]
import std;
import mbun.toml;

namespace {
std::string read_file(const std::filesystem::path& p) {
    std::ifstream in{p, std::ios::binary};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
long now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::println("usage: bench-toml <fixture.toml> [iters]");
        return 2;
    }
    std::string src = read_file(argv[1]);
    const int iters = argc > 2 ? std::stoi(argv[2]) : 20000;

    long checksum = 0;
    for (int k = 0; k < 3; ++k) {  // 预热
        auto r = mbun::toml::parse(src);
        if (r) checksum += static_cast<long>(r->size());
    }

    auto t0 = now_ns();
    for (int k = 0; k < iters; ++k) {
        auto r = mbun::toml::parse(src);
        if (r) checksum += static_cast<long>(r->size());
    }
    auto ns = now_ns() - t0;

    std::println(R"({{"impl":"mbun {}","parse_ops_per_s":{},"checksum":{}}})", "0.1.0",
                 ns > 0 ? static_cast<long>(iters) * 1'000'000'000 / ns : 0, checksum);
    return 0;
}
