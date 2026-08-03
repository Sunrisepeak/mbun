import std;
import mbun.css.lightningcss;
import mbun.css.property_tables;

namespace {

int gChecks{};
int gFailures{};

void check(bool condition, std::string_view label) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::println("FAIL: {}", label);
    }
}

constexpr std::uint32_t version(unsigned major, unsigned minor = 0, unsigned patch = 0) {
    return (major << 16U) | (minor << 8U) | patch;
}

void test_existing_property_tables_remain_available() {
    using namespace mbun::css;

    check(property_id("COLOR") == PropertyIdTag::Color, "canonical property table remains exported");
    check(property_kind("all") == PropertyKind::Shorthand, "canonical all metadata remains intact");
}

void test_property_bridge() {
    namespace lightning = mbun::css::lightningcss;

    const auto color = lightning::lookup_property("CoLoR");
    check(color.id == mbun::css::PropertyIdTag::Color, "property lookup uses canonical IDs");
    check(color.kind == mbun::css::PropertyKind::Longhand, "property kind uses canonical metadata");

    const auto all = lightning::lookup_property("ALL");
    check(all.id == mbun::css::PropertyIdTag::All, "all property is shared with canonical table");
    check(all.kind == mbun::css::PropertyKind::Shorthand, "all remains shorthand");

    const auto custom = lightning::lookup_property("--theme");
    check(custom.id == mbun::css::PropertyIdTag::Custom, "custom property is preserved");
}

void test_value_metadata() {
    namespace lightning = mbun::css::lightningcss;

    check(lightning::lookup_value_kind("CoLoR") == lightning::ValueKind::color,
          "syntax component names are ASCII-case-insensitive");
    check(lightning::lookup_value_kind("length-percentage") ==
              lightning::ValueKind::length_percentage,
          "length-percentage syntax component");
    check(lightning::lookup_value_kind("transform-list") == lightning::ValueKind::transform_list,
          "transform-list syntax component");
    check(lightning::is_css_wide_keyword("INITIAL"), "CSS-wide keywords ignore ASCII case");
    check(lightning::lookup_value_kind("not-a-type") == lightning::ValueKind::unknown,
          "unknown syntax component stays unknown");
}

void test_exact_compatibility_edges() {
    namespace lightning = mbun::css::lightningcss;

    lightning::BrowserTargets targets{};
    targets[lightning::Browser::chrome] = version(25);
    check(!lightning::is_compatible(lightning::Feature::calc_function, targets),
          "calc is unavailable before Chrome 26");
    targets[lightning::Browser::chrome] = version(26);
    check(lightning::is_compatible(lightning::Feature::calc_function, targets),
          "calc is available in Chrome 26");

    targets = {};
    targets[lightning::Browser::ie] = version(11);
    check(!lightning::is_compatible(lightning::Feature::calc_function, targets),
          "calc is unavailable for IE in Bun table");

    targets = {};
    targets[lightning::Browser::chrome] = version(119);
    check(!lightning::is_compatible(lightning::Feature::nesting, targets),
          "nesting is unavailable before Chrome 120");
    targets[lightning::Browser::chrome] = version(120);
    check(lightning::is_compatible(lightning::Feature::nesting, targets),
          "nesting is available in Chrome 120");

    targets = {};
    targets[lightning::Browser::samsung] = version(30);
    check(!lightning::is_compatible(lightning::Feature::nesting, targets),
          "nesting is unavailable for Samsung in Bun table");

    targets = {};
    targets[lightning::Browser::safari] = version(12, 0);
    check(!lightning::is_compatible(lightning::Feature::conic_gradient, targets),
          "conic gradient is unavailable before Safari 12.1");
    targets[lightning::Browser::safari] = version(12, 1);
    check(lightning::is_compatible(lightning::Feature::conic_gradient, targets),
          "conic gradient is available in Safari 12.1");
}

void test_target_overrides() {
    namespace lightning = mbun::css::lightningcss;

    lightning::Targets targets{};
    check(!targets.should_compile(lightning::Feature::nesting),
          "runtime target without browsers does not compile transforms");

    targets.browsers[lightning::Browser::chrome] = version(119);
    check(targets.should_compile(lightning::Feature::nesting),
          "unsupported browser requests transform");

    targets.exclude[0] = lightning::Feature::nesting;
    targets.exclude_count = 1;
    check(!targets.should_compile(lightning::Feature::nesting), "exclude overrides browser support");

    targets.include[0] = lightning::Feature::nesting;
    targets.include_count = 1;
    check(targets.should_compile(lightning::Feature::nesting), "include has highest priority");
}

}  // namespace

int main() {
    test_existing_property_tables_remain_available();
    test_property_bridge();
    test_value_metadata();
    test_exact_compatibility_edges();
    test_target_overrides();
    std::println("test_css_lightning: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
