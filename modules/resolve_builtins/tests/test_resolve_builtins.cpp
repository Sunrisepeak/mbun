import std;
import mbun.resolve_builtins;

namespace {

using mbun::resolve_builtins::BuiltinNamespace;
using mbun::resolve_builtins::ResolutionKind;
using mbun::resolve_builtins::resolve;

void expect_found(std::string_view specifier, BuiltinNamespace namespaceKind,
                  std::string_view canonical) {
    auto result{resolve(specifier)};
    if (result.kind != ResolutionKind::Found || !result.mapping.has_value()
        || result.mapping->namespace_kind != namespaceKind
        || result.mapping->canonical != canonical) {
        std::println("expected builtin '{}' -> '{}'", specifier, canonical);
        std::exit(1);
    }
}

void expect_missing(std::string_view specifier) {
    auto result{resolve(specifier)};
    if (result.kind != ResolutionKind::NotFound || result.mapping.has_value()) {
        std::println("expected '{}' to be unresolved", specifier);
        std::exit(1);
    }
}

void expect_gated(std::string_view specifier) {
    auto result{resolve(specifier)};
    if (result.kind != ResolutionKind::Gated) {
        std::println("expected '{}' to be gated", specifier);
        std::exit(1);
    }
}

} // namespace

int main() {
    expect_found("bun:test", BuiltinNamespace::Bun, "bun:test");
    expect_found("node:fs", BuiltinNamespace::Node, "node:fs");
    expect_found("fs", BuiltinNamespace::Node, "node:fs");
    expect_found("sys", BuiltinNamespace::Node, "node:util");
    expect_found("web:streams", BuiltinNamespace::Web, "web:streams");
    expect_gated("bun:internal-for-testing");
    expect_gated("node:stream/iter");
    expect_missing("left-pad");
    expect_missing("node:");
    return 0;
}
