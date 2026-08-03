// Native parser-kernel driver for JSONC/INI. This is deliberately B-level:
// it does not include JS/JSC object materialization and cannot support an A-level claim.
import std;
import mbun.config.ini;
import mbun.config.jsonc;
import mbun.config.value;

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

template <typename Parse>
int run(std::string_view implementation, std::string_view source, int iterations, Parse parse) {
    std::optional<mbun::config::Value> last;
    for (int i{}; i < 3; ++i) {
        auto value{parse(source)};
        if (!value) {
            std::println("parse error at {}: {}", value.error().offset, value.error().message);
            return 1;
        }
        last = std::move(*value);
    }
    const auto begin{now_ns()};
    for (int i{}; i < iterations; ++i) {
        auto value{parse(source)};
        if (!value) {
            return 1;
        }
        last = std::move(*value);
    }
    const auto elapsed{now_ns() - begin};
    const std::uint64_t checksum{mbun::config::canonical_hash(*last)};
    std::println(R"({{"impl":"{}","parse_ops_per_s":{},"checksum":"{:016x}"}})", implementation,
                 elapsed > 0 ? static_cast<long>(iterations) * 1'000'000'000L / elapsed : 0L,
                 checksum);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::println("usage: bench-config <jsonc|ini> <fixture> [iterations]");
        return 2;
    }
    const std::string source{read_file(argv[2])};
    const int iterations{argc > 3 ? std::stoi(argv[3]) : 20000};
    if (std::string_view{argv[1]} == "jsonc") {
        return run("mbun-jsonc-native", source, iterations, mbun::config::jsonc::parse);
    }
    if (std::string_view{argv[1]} == "ini") {
        const mbun::config::ini::EnvLookup env{[](std::string_view key) -> std::optional<std::string_view> {
            return key == "TOKEN" ? std::optional<std::string_view>{"bench-token"} : std::nullopt;
        }};
        return run("mbun-ini-native", source, iterations,
                   [&env](std::string_view text) { return mbun::config::ini::parse(text, env); });
    }
    return 2;
}
