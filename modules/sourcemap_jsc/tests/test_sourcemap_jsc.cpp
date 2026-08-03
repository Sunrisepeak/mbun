import std;
import mbun.sourcemap_jsc;

namespace {
int failures { 0 };

void check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}
}  // namespace

int main() {
    using namespace mbun::sourcemap_jsc;

    BindingRegistry registry;
    check(registry.descriptors().size() == 5, "registry exposes JSSourceMap and internal seams");
    check(registry.descriptors()[2].name == "findEntry", "entry binding keeps Bun name");

    auto map { registry.construct(MapPayload {
        .mappings = "AAAA,KAAI",
        .sources = { "input.ts" },
        .names = {},
    }) };
    check(map.has_value(), "JSSourceMap payload converts through pure parser");
    if (map) {
        auto entry { to_entry(*map, SourcePosition { 0, 1 }) };
        check(entry.has_value(), "findEntry returns covering mapping");
        check(entry && entry->original == SourcePosition { 0, 0 },
              "findEntry preserves original source position");
        check(entry && entry->source == std::optional<std::string> { "input.ts" },
              "findEntry resolves source index");
        check(!to_entry(*map, SourcePosition { -1, 0 }).has_value(),
              "negative JSC position has no mapping");
    }

    check(!registry.construct(MapPayload { .mappings = "!" }).has_value(),
          "invalid mapping becomes a binding error");
    if (failures != 0) {
        std::println("{} checks failed", failures);
        return 1;
    }
    std::println("sourcemap_jsc checks passed");
}
