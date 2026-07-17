import std;
import mbun.logger;

int main() {
    using namespace mbun::logger;
    int failures{0};
    auto check = [&](bool ok, std::string_view name) { if (!ok) { ++failures; std::println("FAIL {}", name); } };
    check(should_print(Kind::note, Level::warning), "note threshold");
    check(!should_print(Kind::debug, Level::warning), "debug filtered");
    check(!should_print(Kind::verbose, Level::info), "verbose filtered");
    check(parse_level("warn") == Level::warning, "parse warn");
    check(!parse_level("bad"), "invalid level");
    VectorSink sink{};
    Provider provider{Level::info, &sink};
    Location location{"entry.ts", "file", std::string{"const x"}, 1, 4, 1, 7};
    check(provider.add(Kind::error, "bad input", location), "add event");
    check(!provider.add(Kind::verbose, "trace"), "filter event");
    check(provider.error_count() == 1 && provider.events().size() == 1 && sink.events().size() == 1, "provider seam");
    std::println("logger checks: {}, failures: {}", 8, failures);
    return failures == 0 ? 0 : 1;
}
