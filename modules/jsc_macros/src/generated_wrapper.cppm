export module mbun.jsc_macros.generated_wrapper;

import std;
import mbun.jsc_macros.binding_descriptor;

export namespace mbun::jsc_macros {

enum class WrapperKind : std::uint8_t {
    HostFunction,
    CachedGetter,
    CachedSetter,
    ClassHook,
};

struct GeneratedWrapper {
    WrapperKind kind {WrapperKind::HostFunction};
    std::string_view wrapper_name {};
    std::string_view target_symbol {};
    bool uses_receiver {false};
    bool uses_global {false};
};

// This is the MC++ seam for Rust #[host_fn] expansion. The eventual JSC
// backend consumes the same result to emit an ABI-specific thunk.
[[nodiscard]] constexpr auto generate_host_wrapper(
    const BindingDescriptor& binding) -> GeneratedWrapper {
    const bool receiver {
        binding.kind != HostFunctionKind::Free || binding.pass_this
    };
    return GeneratedWrapper {
        .kind = WrapperKind::HostFunction,
        .wrapper_name = binding.rust_name,
        .target_symbol = binding.cpp_symbol,
        .uses_receiver = receiver,
        .uses_global = true,
    };
}

// Mirrors codegen_cached_accessors!: the generated wrapper names are stable
// and the C++ symbol names remain owned by class codegen.
[[nodiscard]] constexpr auto generate_cached_getter(
    const CachedPropertyDescriptor& property) -> GeneratedWrapper {
    return GeneratedWrapper {
        .kind = WrapperKind::CachedGetter,
        .wrapper_name = property.property_name,
        .target_symbol = property.getter_symbol,
        .uses_receiver = true,
        .uses_global = false,
    };
}

[[nodiscard]] constexpr auto generate_cached_setter(
    const CachedPropertyDescriptor& property) -> GeneratedWrapper {
    return GeneratedWrapper {
        .kind = WrapperKind::CachedSetter,
        .wrapper_name = property.property_name,
        .target_symbol = property.setter_symbol,
        .uses_receiver = true,
        .uses_global = true,
    };
}

// Mirrors the import-side hooks emitted by #[JsClass]. Hook availability is
// data, not a link-time side effect; native C++ emission remains DEFERRED.
[[nodiscard]] constexpr auto generate_class_hooks(
    const ClassBindingDescriptor& binding) -> std::array<GeneratedWrapper, 4> {
    return std::array {
        GeneratedWrapper {
            .kind = WrapperKind::ClassHook,
            .wrapper_name = "from_js",
            .target_symbol = binding.from_js_symbol,
            .uses_receiver = false,
            .uses_global = false,
        },
        GeneratedWrapper {
            .kind = WrapperKind::ClassHook,
            .wrapper_name = "create",
            .target_symbol = binding.create_symbol,
            .uses_receiver = false,
            .uses_global = true,
        },
        GeneratedWrapper {
            .kind = WrapperKind::ClassHook,
            .wrapper_name = "get_constructor",
            .target_symbol = binding.constructor_symbol,
            .uses_receiver = false,
            .uses_global = true,
        },
        GeneratedWrapper {
            .kind = WrapperKind::ClassHook,
            .wrapper_name = "finalize",
            .target_symbol = binding.finalize_symbol,
            .uses_receiver = true,
            .uses_global = false,
        },
    };
}

}
