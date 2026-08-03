// mbun 侧 stringWidth 基准 driver — 与 benchmarks/suites/string-width/bench.mjs
// 同口径（同 15 组字符串、同热循环、checksum=Σ宽度）。用法: bench-string-width [iters]
import std;
import mbun.core.strings;

namespace {
// 与 bench.mjs 完全一致的 15 组（UTF-8 字节）。
const std::array<std::string_view, 15> S{{
    "hello world",
    "the quick brown fox jumps over the lazy dog 0123456789",
    "你好世界",                              // 你好世界
    "こんにちは",                        // こんにちは
    "\U0001F60E中\U0001F600",                            // 😎中😀
    "\x1b[31mred\x1b[0m \x1b[1;32mgreen\x1b[0m",             // ANSI colored
    "café naïve résumé",                 // café naïve résumé
    "\U0001F469‍\U0001F4BB",                            // 👩‍💻 ZWJ family
    "áéó",                                 // áéó combining
    "\U0001F1FA\U0001F1F8\U0001F1E8\U0001F1F3",              // 🇺🇸🇨🇳 flags
    "ＡＢＣ",                                    // ＡＢＣ fullwidth
    "1️⃣ 2️⃣",                           // 1️⃣ 2️⃣ keycaps
    "mixed 中文 and English \U0001F680",             // mixed 中文 … 🚀
    "\t\ttabs and    spaces",
    "no-width​​zero",                              // zero-width spaces
}};

long now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

int main(int argc, char* argv[]) {
    const int iters = argc > 1 ? std::stoi(argv[1]) : 30000;

    long checksum = 0;
    for (int k = 0; k < 3; ++k) {  // 预热
        for (auto s : S) checksum += static_cast<long>(mbun::core::strings::string_width(s));
    }

    auto t0 = now_ns();
    for (int k = 0; k < iters; ++k) {
        for (auto s : S) checksum += static_cast<long>(mbun::core::strings::string_width(s));
    }
    auto ns = now_ns() - t0;
    auto ops = static_cast<long>(S.size()) * iters;

    std::println(R"({{"impl":"mbun {}","width_ops_per_s":{},"checksum":{}}})", "0.1.0",
                 ns > 0 ? ops * 1'000'000'000 / ns : 0, checksum);
    return 0;
}
