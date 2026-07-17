// signature.cppm — immutable-shaped FFI call signatures.
// PORT-SOURCE: bun ABIType argument/return handling in runtime/ffi.
export module mbun.ffi.signature;

import std;
import mbun.ffi.type;

namespace mbun::ffi {

export struct Signature {
    AbiType return_type{AbiType::Void};
    std::vector<AbiType> argument_types{};

    bool needs_napi_env() const noexcept {
        if (return_type == AbiType::NapiValue) return true;
        return std::ranges::any_of(argument_types, [](AbiType type) {
            return type == AbiType::NapiEnv || type == AbiType::NapiValue;
        });
    }

    bool needs_handle_scope() const noexcept { return needs_napi_env(); }
};

export inline Signature make_signature(AbiType returnType, std::initializer_list<AbiType> arguments) {
    return Signature{returnType, std::vector<AbiType>(arguments)};
}

} // namespace mbun::ffi
