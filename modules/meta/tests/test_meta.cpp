import mbun.meta;

import std;

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::println("FAIL: {}", message);
        std::exit(1);
    }
}

void test_version() {
    using mbun::meta::Version;
    check(Version::parse("1.2.3") == Version { 1, 2, 3 }, "parse semantic version");
    check(Version::parse("1.2") == std::nullopt, "reject incomplete version");
    check(Version { 1, 2, 3 }.to_string() == "1.2.3", "format version");
}

void test_build_metadata() {
    using mbun::meta::BuildMetadata;
    const BuildMetadata build { "1.2.3", "abc123", true };
    check(build.version == "1.2.3", "retain build version");
    check(build.revision == "abc123", "retain revision");
    check(build.dirty, "retain dirty flag");
}

void test_target_metadata() {
    using mbun::meta::TargetMetadata;
    const TargetMetadata target { "linux", "x86_64", "gnu", "x86_64-unknown-linux-gnu" };
    check(target.os == "linux", "retain target os");
    check(target.arch == "x86_64", "retain target architecture");
    check(target.environment == "gnu", "retain target environment");
    check(target.triple() == "x86_64-unknown-linux-gnu", "retain target triple");
}

void test_features() {
    using mbun::meta::Feature;
    using mbun::meta::FeatureSet;
    FeatureSet features {};
    check(!features.enabled(Feature::jit), "features start disabled");
    features.enable(Feature::jit);
    check(features.enabled(Feature::jit), "enable feature");
    features.disable(Feature::jit);
    check(!features.enabled(Feature::jit), "disable feature");
}

} // namespace

int main() {
    test_version();
    test_build_metadata();
    test_target_metadata();
    test_features();
    std::println("meta: all checks passed");
}
