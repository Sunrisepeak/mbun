// Function metadata seam for bindgenv2's generated boundary.
// It models the source-level contract without depending on JSC or a native
// code generator, so consumers can validate names and ABI plans independently.
export module mbun.bindgenv2.function;

import std;
import mbun.bindgenv2.type;

export namespace mbun::bindgenv2 {

enum class FunctionKind : std::uint8_t {
    constructor,
    method,
    getter,
    setter,
    free_function,
};

struct ParameterMetadata {
    std::string_view name {};
    std::string_view typeName {};
    bool optional { false };
    bool nullable { false };
};

struct FunctionMetadata {
    std::string_view name {};
    std::string_view returnType {};
    std::span<const ParameterMetadata> parameters {};
    FunctionKind kind { FunctionKind::free_function };
    bool throws { false };
};

[[nodiscard]] inline bool has_parameter(FunctionMetadata function, std::string_view name) {
    return std::ranges::any_of(function.parameters, [name](const auto& parameter) {
        return parameter.name == name;
    });
}

[[nodiscard]] inline bool references_known_type(
    FunctionMetadata function,
    std::span<const TypeMetadata> types) {
    const auto known = [types](std::string_view name) {
        return std::ranges::any_of(types, [name](const auto& type) { return type.name == name; });
    };
    if (!function.returnType.empty() && !known(function.returnType)) {
        return false;
    }
    return std::ranges::all_of(function.parameters, [known](const auto& parameter) {
        return known(parameter.typeName);
    });
}

} // namespace mbun::bindgenv2
