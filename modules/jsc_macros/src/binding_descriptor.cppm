export module mbun.jsc_macros.binding_descriptor;

import std;

export namespace mbun::jsc_macros {

// Mirrors the Rust jsc_macros host_fn modes. The descriptor is deliberately
// independent of JSC types so generated glue can be inspected and tested
// before the native binding backend is available.
enum class HostFunctionKind : std::uint8_t {
    Free,
    Method,
    Getter,
    Setter,
};

struct BindingDescriptor {
    std::string_view rust_name {};
    std::string_view js_name {};
    std::string_view cpp_symbol {};
    HostFunctionKind kind {HostFunctionKind::Free};
    bool pass_this {false};
};

struct CachedPropertyDescriptor {
    std::string_view type_name {};
    std::string_view property_name {};
    std::string_view getter_symbol {};
    std::string_view setter_symbol {};
};

struct ClassBindingDescriptor {
    std::string_view rust_type {};
    std::string_view cpp_type {};
    std::string_view from_js_symbol {};
    std::string_view create_symbol {};
    std::string_view constructor_symbol {};
    std::string_view finalize_symbol {};
    bool has_constructor {true};
    bool has_construct_hook {true};
    bool has_finalize_hook {true};
    bool has_estimated_size {false};
};

[[nodiscard]] constexpr auto host_fn_descriptor(
    std::string_view rustName,
    std::string_view jsName,
    std::string_view cppSymbol,
    HostFunctionKind kind,
    bool passThis = false) -> BindingDescriptor {
    return BindingDescriptor {
        .rust_name = rustName,
        .js_name = jsName,
        .cpp_symbol = cppSymbol,
        .kind = kind,
        .pass_this = passThis,
    };
}

[[nodiscard]] constexpr auto cached_property_descriptor(
    std::string_view typeName,
    std::string_view propertyName,
    std::string_view getterSymbol,
    std::string_view setterSymbol) -> CachedPropertyDescriptor {
    return CachedPropertyDescriptor {
        .type_name = typeName,
        .property_name = propertyName,
        .getter_symbol = getterSymbol,
        .setter_symbol = setterSymbol,
    };
}

[[nodiscard]] constexpr auto class_binding_descriptor(
    std::string_view rustType,
    std::string_view cppType,
    std::string_view fromJsSymbol,
    std::string_view createSymbol,
    std::string_view constructorSymbol,
    std::string_view finalizeSymbol,
    bool hasConstructor = true,
    bool hasConstructHook = true,
    bool hasFinalizeHook = true,
    bool hasEstimatedSize = false) -> ClassBindingDescriptor {
    return ClassBindingDescriptor {
        .rust_type = rustType,
        .cpp_type = cppType,
        .from_js_symbol = fromJsSymbol,
        .create_symbol = createSymbol,
        .constructor_symbol = constructorSymbol,
        .finalize_symbol = finalizeSymbol,
        .has_constructor = hasConstructor,
        .has_construct_hook = hasConstructHook,
        .has_finalize_hook = hasFinalizeHook,
        .has_estimated_size = hasEstimatedSize,
    };
}

}
