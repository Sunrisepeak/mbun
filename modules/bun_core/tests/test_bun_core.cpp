import std;
import mbun.bun_core;

namespace {

int checks { 0 };
int failures { 0 };

void check(bool condition, std::string_view message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL {}", message);
    }
}

void test_version() {
    using mbun::bun_core::Version;
    const auto parsed { Version::parse("1.3.14") };
    check(parsed.has_value(), "version parses major.minor.patch");
    check(parsed->major == 1 && parsed->minor == 3 && parsed->patch == 14,
          "version components are preserved");
    check(parsed->to_string() == "1.3.14", "version formats without allocation in the API contract");
    check(!Version::parse("1.3"), "incomplete version is rejected");
}

void test_platform() {
    using namespace mbun::bun_core;
    check(platform::current().os == platform::current().os &&
              platform::current().architecture == platform::current().architecture,
          "current platform is stable");
    check(!platform::current().npm_name().empty(), "platform has an archive name");
    check(!platform::architecture_npm_name(platform::current_architecture()).empty(),
          "architecture has an archive name");
}

void test_features_and_context() {
    using namespace mbun::bun_core;
    constexpr FeatureSet defaults { FeatureSet::defaults() };
    check(defaults.enabled(Feature::tracing), "bun tracing default is enabled");
    check(defaults.enabled(Feature::entry_cache), "bun entry cache default is enabled");
    check(!defaults.enabled(Feature::verbose_fs), "bun verbose fs default is disabled");

    RuntimeContext context {};
    check(context.runtime_data.version == runtime_version(), "context shares the runtime version seam");
    check(context.runtime_data.platform == platform::current(), "context shares the platform seam");
    check(context.runtime_data.features.enabled(Feature::tracing), "context carries default feature data");
}

} // namespace

int main() {
    test_version();
    test_platform();
    test_features_and_context();
    std::println("bun_core checks: {} passed, {} failed", checks - failures, failures);
    return failures == 0 ? 0 : 1;
}
