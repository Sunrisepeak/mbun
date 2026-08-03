// ABI metadata seam corresponding to bindgenv2's C++/Zig boundary planning.
// The Bun generator computes richer conversion plans; this first MC++ port
// records stable scalar layout/pass facts and leaves backend lowering deferred.
export module mbun.bindgenv2.abi_metadata;

import std;
import mbun.bindgenv2.function;

export namespace mbun::bindgenv2 {

enum class AbiPassKind : std::uint8_t {
    by_value,
    pointer,
    out_parameter,
    opaque,
};

struct AbiTypeMetadata {
    std::string_view typeName {};
    std::size_t size { 0 };
    std::size_t alignment { 0 };
    AbiPassKind pass { AbiPassKind::opaque };
    bool nullable { false };

    [[nodiscard]] constexpr bool is_layout_known() const noexcept {
        return size != 0 && alignment != 0;
    }
};

struct AbiFunctionMetadata {
    std::string_view functionName {};
    AbiTypeMetadata returnType {};
    std::span<const AbiTypeMetadata> parameters {};
    bool exceptionFlag { false };
};

[[nodiscard]] constexpr AbiTypeMetadata scalar_abi(
    std::string_view typeName,
    std::size_t size,
    std::size_t alignment,
    bool nullable = false) noexcept {
    return AbiTypeMetadata { typeName, size, alignment, AbiPassKind::by_value, nullable };
}

[[nodiscard]] constexpr AbiTypeMetadata pointer_abi(
    std::string_view typeName,
    bool nullable = false) noexcept {
    return AbiTypeMetadata { typeName, sizeof(void*), alignof(void*), AbiPassKind::pointer, nullable };
}

[[nodiscard]] inline bool matches_signature(
    FunctionMetadata function,
    AbiFunctionMetadata abi) {
    return function.name == abi.functionName && function.parameters.size() == abi.parameters.size();
}

} // namespace mbun::bindgenv2
