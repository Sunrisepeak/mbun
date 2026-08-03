import std;
import mbun.bindgenv2;

namespace {
int failures { 0 };

void check(bool condition, std::string_view message) {
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", message);
    }
}
}

int main() {
    using namespace mbun::bindgenv2;

    constexpr TypeMetadata stringType {
        "String", "::Bun::IDLString", "bindgen.BindgenString", "bun.String",
        TypeKind::string, TypeFlags::named | TypeFlags::has_cpp_header,
    };
    constexpr TypeMetadata countType {
        "u32", "::Bun::IDLStrictInteger<::std::uint32_t>", "bindgen.BindgenU32", "u32",
        TypeKind::primitive, TypeFlags::none, PrimitiveKind::unsigned_integer, 32,
    };
    constexpr std::array types { stringType, countType };
    const std::array parameters { ParameterMetadata { "value", "String", false, false } };
    const FunctionMetadata function {
        "setValue", "String", parameters, FunctionKind::setter, true,
    };
    const std::array abiParameters { scalar_abi("String", 16, 8) };
    const AbiFunctionMetadata abi {
        "setValue", pointer_abi("String"), abiParameters, true,
    };

    check(stringType.is_named() && stringType.has_cpp_header(), "named type flags are retained");
    check(countType.primitive == PrimitiveKind::unsigned_integer && countType.bitWidth == 32,
        "primitive metadata retains integer width");
    check(has_parameter(function, "value"), "function metadata exposes parameter lookup");
    check(references_known_type(function, types), "function references exported types");
    check(matches_signature(function, abi), "ABI metadata matches function arity and name");
    check(abi.returnType.pass == AbiPassKind::pointer && abi.returnType.is_layout_known(),
        "ABI pointer metadata records portable layout facts");

    if (failures != 0) {
        std::println("{} checks failed", failures);
        return 1;
    }
    std::println("bindgenv2 checks passed");
}
