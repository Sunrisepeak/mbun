import std;
import mbun.bundler;

namespace {
int checks {};
int failures {};

void check(bool value, std::string_view label) {
    ++checks;
    if (!value) {
        ++failures;
        std::println("FAIL: {}", label);
    }
}
}

int main() {
    using namespace mbun::bundler;
    using namespace mbun::bundler::ast_jsc;

    constexpr std::uint32_t ids[] { 0, 1, 0, 1, 0, 1, 0 };
    constexpr std::uint8_t records[] { 0, 2, 6 };
    constexpr std::uint32_t lengths[] { 1, 2 };
    constexpr RequestedModuleValue values[] { RequestedModuleValue::Javascript };
    constexpr std::uint32_t keys[] { 0 };
    constexpr std::uint8_t phases[] { 0 };
    const ModuleInfo valid { lengths, keys, values, phases, ids, records, 3 };
    check(validate(valid), "valid serialized AST metadata");

    // Real serialized ABI fixture: Bun writes one record-kind byte in this
    // exact order. Keep this independent of named tag constants.
    constexpr std::array<std::uint8_t, 10> abiTags { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    for (std::size_t i { 0 }; i < abiTags.size(); ++i) {
        check(record_width(abiTags[i]) != 0, "serialized tag matches Bun ABI");
    }
    check(record_width(6) == 3,
          "local export keeps Bun's conversion padding slot");
    constexpr std::array<std::uint32_t, 23> abiBuffer {};
    check(validate(ModuleInfo { lengths, {}, {}, {}, abiBuffer, abiTags, 3 }),
          "raw tags 0..9 consume the Bun ABI record widths");
    constexpr std::array<std::uint8_t, 1> unknownTag { 10 };
    constexpr std::array<std::uint32_t, 1> unknownBuffer {};
    check(!validate(ModuleInfo { lengths, {}, {}, {}, unknownBuffer, unknownTag, 3 }),
          "unknown serialized tag is rejected");

    auto mutable_phase { std::array<std::uint8_t, 1> { 2 } };
    const ModuleInfo invalid_phase { lengths, keys, values, mutable_phase, ids, records, 3 };
    check(!validate(invalid_phase), "invalid module phase rejected");
    check(!validate(ModuleInfo { lengths, keys, values, phases, std::span<const std::uint32_t> { ids, 6 },
                                 records, 3 }), "truncated record buffer rejected");

    using namespace mbun::bundler::bundler_jsc;
    check(source_map_from_string("inline") == SourceMapOption::Inline, "inline source map mode");
    check(!source_map_from_string("bad"), "unknown source map mode");
    check(target_from_string("bun") == Target::Bun, "bun target");
    check(loader_from_string("tsx") == Loader::Tsx, "tsx loader");

    using namespace mbun::bundler::standalone_graph;
    check(is_path("/$bunfs/root/index.js", OperatingSystem::Posix), "posix standalone path");
    check(!is_path("/tmp/index.js", OperatingSystem::Posix), "posix real path rejected");
    check(is_path("B:/~BUN/root/index.js", OperatingSystem::Windows), "windows public path");
    check(is_path("\\\\?\\B:/~BUN/root/index.js", OperatingSystem::Windows), "windows NT path");

    std::println("bundler ast-jsc: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
