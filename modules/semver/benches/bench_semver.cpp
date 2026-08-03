// mbun 侧 semver 基准 driver — 与 bench.mjs 完全同口径（同数据、同循环结构、
// 同 checksum 防 DCE）。用法: bench-semver <suite目录> [iters]
import std;
import mbun.semver;

namespace {

std::vector<std::string> read_lines(const std::filesystem::path& file) {
    std::ifstream in { file };
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

long now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::println("usage: bench-semver <suite-dir> [iters]");
        return 2;
    }
    std::filesystem::path dir { argv[1] };
    const int iters = argc > 2 ? std::stoi(argv[2]) : 50;

    auto versions = read_lines(dir / "versions.txt");
    auto ranges = read_lines(dir / "ranges.txt");
    if (versions.empty() || ranges.empty()) {
        std::println("bench-semver: missing versions.txt/ranges.txt in {}", dir.string());
        return 2;
    }

    long checksum = 0;
    for (int k = 0; k < 3; ++k) {  // 预热
        for (std::size_t i = 0; i + 1 < versions.size(); ++i) {
            checksum += mbun::semver::order(versions[i], versions[i + 1]);
        }
    }

    auto t0 = now_ns();
    for (int k = 0; k < iters; ++k) {
        for (std::size_t i = 0; i + 1 < versions.size(); ++i) {
            checksum += mbun::semver::order(versions[i], versions[i + 1]);
        }
    }
    auto orderNs = now_ns() - t0;
    auto orderOps = static_cast<long>(versions.size() - 1) * iters;

    t0 = now_ns();
    for (int k = 0; k < iters; ++k) {
        for (std::size_t i = 0; i < versions.size(); ++i) {
            checksum += mbun::semver::satisfies(versions[i], ranges[i % ranges.size()]) ? 1 : 0;
        }
    }
    auto satNs = now_ns() - t0;
    auto satOps = static_cast<long>(versions.size()) * iters;

    std::println(R"({{"impl":"mbun {}","order_ops_per_s":{},"satisfies_ops_per_s":{},"checksum":{}}})",
                 "0.1.0",
                 orderNs > 0 ? orderOps * 1'000'000'000 / orderNs : 0,
                 satNs > 0 ? satOps * 1'000'000'000 / satNs : 0, checksum);
    return 0;
}
