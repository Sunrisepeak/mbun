import std;
import mbun.bun_core_macros;

namespace {
int failures { 0 };

void check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}

struct Handler {
    template<mbun::bun_core_macros::AttributeKind> constexpr auto schema() const { return "schema"; }
    template<mbun::bun_core_macros::AttributeKind> constexpr auto platform() const { return "platform"; }
    template<mbun::bun_core_macros::AttributeKind> constexpr auto feature() const { return "feature"; }
    template<mbun::bun_core_macros::AttributeKind> constexpr auto unknown() const { return "unknown"; }
};
}

int main() {
    using namespace mbun::bun_core_macros;
    constexpr auto field { make_attribute("field", "count") };
    constexpr auto platform { make_attribute("platform", "posix") };
    constexpr std::array entries { SchemaEntry { SchemaKind::field, "count", field },
                                   SchemaEntry { SchemaKind::type, "platform", platform } };
    constexpr auto diagnostic { validate_schema(Schema { entries }) };
    static_assert(diagnostic.error == SchemaError::none);
    static_assert(dispatch_target<AttributeKind::field>() == DispatchTarget::schema);
    static_assert(dispatch_target<AttributeKind::platform>() == DispatchTarget::platform);
    static_assert(std::string_view { dispatch<AttributeKind::feature>(Handler {}) } == "feature");

    check(field.kind == AttributeKind::field, "attribute kind is classified at compile time");
    check(platform.value == "posix", "attribute payload is retained without allocation");
    check(deferred_generator_plan().state == GeneratorState::deferred,
          "generator remains an explicit deferred seam");
    std::println("bun_core_macros checks: {} passed, {} failed", 3 - failures, failures);
    return failures == 0 ? 0 : 1;
}
