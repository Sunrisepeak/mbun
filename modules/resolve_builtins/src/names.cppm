// Builtin names are represented as borrowed specifiers so resolution stays
// allocation-free. The tables mirror Bun's resolve_builtins HardcodedModule
// and Alias layers; runtime module bodies are deliberately out of scope.
export module mbun.resolve_builtins.names;

import std;

namespace mbun::resolve_builtins {

export enum class BuiltinNamespace : std::uint8_t { Bun, Node, Web };

export struct BuiltinName {
    std::string_view canonical{};
    BuiltinNamespace namespace_kind{BuiltinNamespace::Node};
    bool node_builtin{false};
    bool node_only_prefix{false};
};

export constexpr bool has_prefix(std::string_view value, std::string_view prefix) noexcept {
    return value.starts_with(prefix);
}

} // namespace mbun::resolve_builtins
