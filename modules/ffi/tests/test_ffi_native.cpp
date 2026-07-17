// test_ffi_native.cpp — exercises the real dlopen()/libffi native backend end
// to end: open system libraries (libm, libc via the global handle), resolve
// symbols, and call them through libffi across every supported AbiType, then
// verify the marshalled arguments and typed return values.
import std;
import mbun.ffi;

using namespace mbun::ffi;

namespace {

int failed{0};

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failed;
    }
}

// Resolve `name` from `lib`, call it via libffi, and return the typed result.
NativeValue call(const NativeLibrary& lib, std::string_view name, const Signature& sig,
                 std::vector<NativeValue> args, std::string_view label) {
    auto sym = lib.symbol(name);
    if (!sym) {
        expect(false, label);
        return NativeValue{};
    }
    auto result = call_native(*sym, sig, args);
    if (!result) {
        std::cerr << "  (" << name << " error: " << result.error() << ")\n";
        expect(false, label);
        return NativeValue{};
    }
    return *result;
}

bool close_to(double a, double b) { return std::fabs(a - b) < 1e-9; }

} // namespace

int main() {
    // --- libm: double-valued math -----------------------------------------
    auto libm = NativeLibrary::open("libm.so.6");
    expect(libm.has_value(), "dlopen libm.so.6");
    if (libm) {
        // double sqrt(double)
        auto r = call(*libm, "sqrt", Signature{AbiType::Double, {AbiType::Double}},
                      {NativeValue::float64(144.0)}, "sqrt call resolves");
        expect(close_to(r.as_double(), 12.0), "sqrt(144) == 12");

        // double pow(double, double)
        auto p = call(*libm, "pow", Signature{AbiType::Double, {AbiType::Double, AbiType::Double}},
                      {NativeValue::float64(2.0), NativeValue::float64(10.0)}, "pow call resolves");
        expect(close_to(p.as_double(), 1024.0), "pow(2,10) == 1024");

        // float type marshalling: float powf(float, float)
        auto pf = call(*libm, "powf", Signature{AbiType::Float, {AbiType::Float, AbiType::Float}},
                       {NativeValue::float32(3.0f), NativeValue::float32(4.0f)}, "powf call resolves");
        expect(std::fabs(pf.as_float() - 81.0f) < 1e-4f, "powf(3,4) == 81");
    }

    // --- libc via the global (null-path) handle ---------------------------
    auto libc = NativeLibrary::open("");  // dlopen(nullptr): main + loaded libs
    expect(libc.has_value(), "dlopen(null) global handle");
    if (libc) {
        const std::string hello{"hello"};

        // size_t strlen(const char*) -> Uint64, cstring argument
        auto len = call(*libc, "strlen", Signature{AbiType::Uint64, {AbiType::CString}},
                        {NativeValue::cstring(hello.c_str())}, "strlen call resolves");
        expect(len.i == 5, "strlen(\"hello\") == 5");

        // int abs(int) -> Int32, negative argument round-trips as signed
        auto a = call(*libc, "abs", Signature{AbiType::Int32, {AbiType::Int32}},
                      {NativeValue::integer(AbiType::Int32, -7)}, "abs call resolves");
        expect(a.i == 7, "abs(-7) == 7");

        // int strcmp(const char*, const char*) -> Int32
        auto eq = call(*libc, "strcmp", Signature{AbiType::Int32, {AbiType::CString, AbiType::CString}},
                       {NativeValue::cstring("abc"), NativeValue::cstring("abc")}, "strcmp eq resolves");
        expect(eq.i == 0, "strcmp(abc,abc) == 0");
        auto lt = call(*libc, "strcmp", Signature{AbiType::Int32, {AbiType::CString, AbiType::CString}},
                       {NativeValue::cstring("abc"), NativeValue::cstring("abd")}, "strcmp lt resolves");
        expect(lt.i < 0, "strcmp(abc,abd) < 0");

        // char* strchr(const char*, int) -> Pointer return, verify it points
        // into the input buffer at the expected offset.
        const std::string haystack{"hello"};
        auto found = call(*libc, "strchr", Signature{AbiType::Pointer, {AbiType::CString, AbiType::Int32}},
                          {NativeValue::cstring(haystack.c_str()),
                           NativeValue::integer(AbiType::Int32, static_cast<int>('l'))},
                          "strchr call resolves");
        expect(found.p != nullptr, "strchr found 'l'");
        if (found.p != nullptr) {
            auto* cp = static_cast<const char*>(found.p);
            expect(*cp == 'l', "strchr return points at 'l'");
            expect(cp - haystack.c_str() == 2, "strchr offset == 2");
        }

        // Symbol that does not exist -> nullopt.
        expect(!libc->symbol("mbun_no_such_symbol_xyz").has_value(),
               "missing symbol resolves to nullopt");
    }

    // --- error paths -------------------------------------------------------
    // dlopen of a nonexistent path fails with an error string.
    auto bad = NativeLibrary::open("/nonexistent/mbun-not-a-real.so");
    expect(!bad.has_value(), "dlopen bad path returns error");

    // null function pointer is rejected.
    expect(!call_native(nullptr, Signature{AbiType::Void, {}}, {}).has_value(),
           "null fn pointer rejected");

    // argument count mismatch is rejected.
    {
        std::vector<NativeValue> none{};
        int dummy{0};
        auto e = call_native(reinterpret_cast<void*>(&dummy),
                             Signature{AbiType::Int32, {AbiType::Int32}}, none);
        expect(!e.has_value(), "arg count mismatch rejected");
    }

    // unsupported (napi) return type is rejected before any call.
    {
        int dummy{0};
        auto e = call_native(reinterpret_cast<void*>(&dummy),
                             Signature{AbiType::NapiValue, {}}, {});
        expect(!e.has_value(), "napi return type rejected");
    }

    // to_ffi_type mapping sanity: scalar types map, napi handles do not.
    expect(to_ffi_type(AbiType::Int32) != nullptr, "to_ffi_type(int32) mapped");
    expect(to_ffi_type(AbiType::Pointer) != nullptr, "to_ffi_type(pointer) mapped");
    expect(to_ffi_type(AbiType::NapiEnv) == nullptr, "to_ffi_type(napi_env) unmapped");

    if (failed == 0) {
        std::cout << "all ffi native (dlopen+libffi) tests passed\n";
    }
    return failed == 0 ? 0 : 1;
}
