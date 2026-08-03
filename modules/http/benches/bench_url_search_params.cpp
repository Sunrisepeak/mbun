// Internal native profiler for the URLSearchParams kernel.
// ⚠️ Not comparable with bun's JS API; never use this for a cross-runtime claim.
import std;
import mbun.http.url_search_params;

using mbun::http::UrlSearchParams;

template <typename Function>
std::uint64_t measure(Function&& function) {
    const auto start{std::chrono::steady_clock::now()};
    function();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

int main(int argc, char** argv) {
    const int iterations{argc > 1 ? std::max(1, std::atoi(argv[1])) : 500'000};
    constexpr std::string_view INPUT{
        "name=John+Doe&tag=runtime&tag=performance&emoji=%F0%9F%98%80&"
        "redirect=https%3A%2F%2Fexample.com%2Fa%3Fx%3D1%26y%3D2"};
    std::uint64_t checksum{0};

    for (int i{0}; i < 10'000; ++i) {
        UrlSearchParams params{INPUT};
        checksum += params.to_string().size();
    }

    const auto parseSerializeNs{measure([&] {
        for (int i{0}; i < iterations; ++i) {
            UrlSearchParams params{INPUT};
            checksum += params.to_string().size();
        }
    })};

    UrlSearchParams query{INPUT};
    const auto queryNs{measure([&] {
        for (int i{0}; i < iterations * 10; ++i) {
            checksum += query.has("tag", "performance") ? 1U : 0U;
            checksum += query.get("redirect")->size();
        }
    })};

    std::println("{{\"parse_serialize_ops_per_s\":{},\"query_pairs_per_s\":{},\"checksum\":{}}}",
                 static_cast<std::uint64_t>(iterations / (parseSerializeNs / 1e9)),
                 static_cast<std::uint64_t>((iterations * 10) / (queryNs / 1e9)), checksum);
}
