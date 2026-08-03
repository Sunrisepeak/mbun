// type.cppm — ABI tags and source-level type spelling for bun:ffi.
// PORT-SOURCE: .mbun/bun-ref/src/runtime/ffi/abi_type.rs (ABIType)
//              .mbun/bun-zig-src/src/runtime/ffi/ffi.zig (ABIType)
export module mbun.ffi.type;

import std;

namespace mbun::ffi {

export enum class AbiType : std::int32_t {
    Char = 0,
    Int8 = 1,
    Uint8 = 2,
    Int16 = 3,
    Uint16 = 4,
    Int32 = 5,
    Uint32 = 6,
    Int64 = 7,
    Uint64 = 8,
    Double = 9,
    Float = 10,
    Bool = 11,
    Pointer = 12,
    Void = 13,
    CString = 14,
    Int64Fast = 15,
    Uint64Fast = 16,
    Function = 17,
    NapiEnv = 18,
    NapiValue = 19,
    Buffer = 20,
};

export constexpr std::optional<AbiType> abi_type_from_int(std::int32_t value) noexcept {
    if (value < 0 || value > 20) return std::nullopt;
    return static_cast<AbiType>(value);
}

export constexpr bool is_floating_point(AbiType type) noexcept {
    return type == AbiType::Double || type == AbiType::Float;
}

export constexpr std::string_view type_name(AbiType type) noexcept {
    switch (type) {
    case AbiType::Char: return "char";
    case AbiType::Int8: return "int8_t";
    case AbiType::Uint8: return "uint8_t";
    case AbiType::Int16: return "int16_t";
    case AbiType::Uint16: return "uint16_t";
    case AbiType::Int32: return "int32_t";
    case AbiType::Uint32: return "uint32_t";
    case AbiType::Int64: return "int64_t";
    case AbiType::Uint64: return "uint64_t";
    case AbiType::Double: return "double";
    case AbiType::Float: return "float";
    case AbiType::Bool: return "bool";
    case AbiType::Pointer: return "void*";
    case AbiType::Void: return "void";
    case AbiType::CString: return "void*";
    case AbiType::Int64Fast: return "int64_t";
    case AbiType::Uint64Fast: return "uint64_t";
    case AbiType::Function: return "void*";
    case AbiType::NapiEnv: return "napi_env";
    case AbiType::NapiValue: return "napi_value";
    case AbiType::Buffer: return "void*";
    }
    return "void";
}

export constexpr std::optional<AbiType> abi_type_from_name(std::string_view name) noexcept {
    if (name == "char") return AbiType::Char;
    if (name == "i8" || name == "int8_t") return AbiType::Int8;
    if (name == "u8" || name == "uint8_t") return AbiType::Uint8;
    if (name == "i16" || name == "int16_t") return AbiType::Int16;
    if (name == "u16" || name == "uint16_t") return AbiType::Uint16;
    if (name == "i32" || name == "int" || name == "int32_t" || name == "c_int") return AbiType::Int32;
    if (name == "u32" || name == "uint32_t" || name == "c_uint") return AbiType::Uint32;
    if (name == "i64" || name == "isize" || name == "int64_t") return AbiType::Int64;
    if (name == "u64" || name == "usize" || name == "uint64_t" || name == "size_t") return AbiType::Uint64;
    if (name == "f32" || name == "float") return AbiType::Float;
    if (name == "f64" || name == "double") return AbiType::Double;
    if (name == "bool") return AbiType::Bool;
    if (name == "ptr" || name == "pointer" || name == "void*") return AbiType::Pointer;
    if (name == "void") return AbiType::Void;
    if (name == "cstring" || name == "char*") return AbiType::CString;
    if (name == "i64_fast") return AbiType::Int64Fast;
    if (name == "u64_fast") return AbiType::Uint64Fast;
    if (name == "function" || name == "callback" || name == "fn") return AbiType::Function;
    if (name == "napi_env") return AbiType::NapiEnv;
    if (name == "napi_value") return AbiType::NapiValue;
    if (name == "buffer") return AbiType::Buffer;
    return std::nullopt;
}

} // namespace mbun::ffi
