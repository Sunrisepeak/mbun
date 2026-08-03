// test_api.cpp — contract tests for the runtime-independent API seam.
// Source anchors: Bun Rust `src/api/lib.rs`, `src/runtime/api/*.rs`, and Zig `src/api/schema.peechy`, `src/runtime/api/*.zig`.
import std;
import mbun.api;
using namespace mbun::api;
namespace { int checks{0}; int failures{0}; void check(bool condition, std::string_view what) { ++checks; if (!condition) { ++failures; std::println("FAIL: {}", what); } } }
int main() {
    static constexpr ArgumentSpec specs[]{{"input", ArgumentKind::String, false}, {"options", ArgumentKind::Object, true}};
    constexpr ApiDescriptor descriptor{"Bun.example", ApiKind::Function, specs, CapabilitySet{}.with(Capability::Values), 1};
    check(descriptor.accepts(1), "required argument accepted"); check(descriptor.accepts(2), "optional argument accepted"); check(!descriptor.accepts(0), "missing required argument rejected");
    check(descriptor.available_in(CapabilitySet{}.with(Capability::Values)), "provided capability satisfies descriptor"); check(!descriptor.available_in(CapabilitySet{}), "missing capability rejected");
    constexpr Argument good[]{{1, ArgumentKind::String}}; constexpr Argument bad[]{{1, ArgumentKind::Number}};
    check(!first_argument_error({good}, specs).has_value(), "matching argument kind accepted"); auto badIndex{first_argument_error({bad}, specs)}; check(badIndex.has_value() && *badIndex == 0, "mismatched argument kind reported");
    check(first_argument_error({good}, specs).value_or(99) == 99, "optional trailing argument does not fail validation");
    auto error{ApiError::invalid_arguments("expected string", 0)}; check(error.code == ErrorCode::InvalidArguments && error.argumentIndex == 0, "structured argument error retains index");
    ApiResult<int> result{42}; check(result.has_value() && *result == 42, "successful result carries value"); check(ok().has_value(), "void success result is representable");
    std::println("api: {} checks, {} failures", checks, failures); return failures == 0 ? 0 : 1;
}
