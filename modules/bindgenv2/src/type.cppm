// Initial metadata port of Bun's codegen/bindgenv2 Type/NamedType model.
// Reference: .mbun/bun-ref/src/codegen/bindgenv2/internal/base.ts and the
// matching .mbun/bun-zig-src source. Code generation and JSC conversion stay
// outside this pure metadata seam.
export module mbun.bindgenv2.type;

import std;

export namespace mbun::bindgenv2 {

enum class TypeKind : std::uint8_t {
    primitive,
    any,
    string,
    optional,
    nullable,
    union_type,
    dictionary,
    enumeration,
    array,
    interface_type,
};

enum class PrimitiveKind : std::uint8_t {
    boolean,
    unsigned_integer,
    signed_integer,
    floating_point,
};

enum TypeFlags : std::uint16_t {
    none = 0,
    named = 1 << 0,
    has_cpp_header = 1 << 1,
    has_cpp_source = 1 << 2,
    has_zig_source = 1 << 3,
};

struct TypeMetadata {
    std::string_view name {};
    std::string_view idlType {};
    std::string_view bindgenType {};
    std::string_view zigType {};
    TypeKind kind { TypeKind::primitive };
    std::uint16_t flags { TypeFlags::none };
    PrimitiveKind primitive { PrimitiveKind::boolean };
    std::uint16_t bitWidth { 0 };

    [[nodiscard]] constexpr bool is_named() const noexcept { return flags & TypeFlags::named; }
    [[nodiscard]] constexpr bool has_cpp_header() const noexcept {
        return flags & TypeFlags::has_cpp_header;
    }
    [[nodiscard]] constexpr bool has_cpp_source() const noexcept {
        return flags & TypeFlags::has_cpp_source;
    }
    [[nodiscard]] constexpr bool has_zig_source() const noexcept {
        return flags & TypeFlags::has_zig_source;
    }
};

struct TypeDependency {
    std::string_view owner {};
    std::string_view dependency {};
};

[[nodiscard]] constexpr bool is_any(TypeKind kind) noexcept {
    return kind == TypeKind::any;
}

[[nodiscard]] constexpr bool is_container(TypeKind kind) noexcept {
    return kind == TypeKind::array || kind == TypeKind::dictionary
        || kind == TypeKind::union_type;
}

} // namespace mbun::bindgenv2
