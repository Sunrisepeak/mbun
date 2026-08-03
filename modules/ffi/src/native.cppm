// native.cppm — real native FFI backend for bun:ffi's dlopen() API.
//
// Route: dlopen()/dlsym() (system libdl, folded into glibc) to resolve symbols
// from an already-compiled shared library, plus a self-implemented x86-64
// System V call trampoline (`mbun_ffi_call_sysv`, below) to marshal arguments
// and perform the call for an arbitrary scalar/pointer AbiType signature. This
// is the `dlopen` half of bun:ffi (Bun.dlopen). Runtime C compilation (bun's
// `cc` / TinyCC path) stays DEFERRED — see call.cppm.
//
// PORT-SOURCE: .mbun/bun-ref/src/bun.js/api/ffi.zig (Bun.dlopen / FFIFunction)
//              .mbun/bun-zig-src/src/runtime/ffi/ffi.zig (ABIType marshalling)
// bun uses libffi for the non-JIT call path; mbun replaces libffi's generic
// ffi_prep_cif/ffi_call with a hand-rolled SysV AMD64 trampoline. The set of
// signatures bun:ffi's dlopen path needs is scalar-and-pointer only (no C
// struct-by-value), which the classify() below covers exactly — so the full
// libffi CIF machinery is unnecessary, and dropping it removes the build's only
// third-party host-library dependency (reproducible across CI/platforms). The
// trampoline is x86-64 only; other arches return an error (DEFERRED).
module;

#include <dlfcn.h>

// System V AMD64 generic call trampoline. Loads up to 6 integer/pointer args
// (rdi,rsi,rdx,rcx,r8,r9), up to 8 SSE args (xmm0-7), and any overflow onto the
// 16-byte-aligned stack, sets al to the vector-register count (the varargs ABI
// contract), calls `fn`, and writes the raw integer (rax) and SSE (xmm0) return
// slots back out. Defined in raw assembly so register allocation is exact.
//
// extern "C" void mbun_ffi_call_sysv(
//     void* fn,                    // rdi
//     const uint64_t* gpregs,      // rsi   6 integer-class eightbytes
//     const uint64_t* sseregs,     // rdx   8 SSE-class eightbytes
//     const uint64_t* stack,       // rcx   nstack overflow eightbytes
//     uint64_t nstack,             // r8
//     uint64_t nsse,               // r9    -> al (vector reg count)
//     uint64_t* out_rax,           // [rbp+16]
//     double*   out_xmm0);         // [rbp+24]
#if defined(__x86_64__)
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl mbun_ffi_call_sysv\n"
    ".type mbun_ffi_call_sysv, @function\n"
    "mbun_ffi_call_sysv:\n"
    "    pushq %rbp\n"
    "    movq  %rsp, %rbp\n"
    "    pushq %rbx\n"
    "    pushq %r12\n"
    "    pushq %r13\n"
    "    pushq %r14\n"
    "    pushq %r15\n"
    "    movq  %rdi, %r11\n"        // fn
    "    movq  %rsi, %rbx\n"        // gpregs
    "    movq  %rdx, %r12\n"        // sseregs
    "    movq  %rcx, %r13\n"        // stack
    "    movq  %r8,  %r14\n"        // nstack
    "    movq  %r9,  %r15\n"        // nsse
    "    movsd   0(%r12), %xmm0\n"
    "    movsd   8(%r12), %xmm1\n"
    "    movsd  16(%r12), %xmm2\n"
    "    movsd  24(%r12), %xmm3\n"
    "    movsd  32(%r12), %xmm4\n"
    "    movsd  40(%r12), %xmm5\n"
    "    movsd  48(%r12), %xmm6\n"
    "    movsd  56(%r12), %xmm7\n"
    "    movq  %r14, %rax\n"        // reserve nstack*8 bytes, 16-align the base
    "    shlq  $3, %rax\n"
    "    subq  %rax, %rsp\n"
    "    andq  $-16, %rsp\n"
    "    xorq  %rcx, %rcx\n"
    "1:\n"                          // copy overflow args to [rsp + i*8]
    "    cmpq  %r14, %rcx\n"
    "    jae   2f\n"
    "    movq  (%r13,%rcx,8), %rax\n"
    "    movq  %rax, (%rsp,%rcx,8)\n"
    "    incq  %rcx\n"
    "    jmp   1b\n"
    "2:\n"
    "    movq   0(%rbx), %rdi\n"
    "    movq   8(%rbx), %rsi\n"
    "    movq  16(%rbx), %rdx\n"
    "    movq  24(%rbx), %rcx\n"
    "    movq  32(%rbx), %r8\n"
    "    movq  40(%rbx), %r9\n"
    "    movq  %r15, %rax\n"        // al = number of vector registers used
    "    call  *%r11\n"
    "    movq  16(%rbp), %r10\n"    // *out_rax  = rax
    "    movq  %rax, (%r10)\n"
    "    movq  24(%rbp), %r10\n"    // *out_xmm0 = xmm0
    "    movsd %xmm0, (%r10)\n"
    "    leaq  -40(%rbp), %rsp\n"
    "    popq  %r15\n"
    "    popq  %r14\n"
    "    popq  %r13\n"
    "    popq  %r12\n"
    "    popq  %rbx\n"
    "    popq  %rbp\n"
    "    ret\n"
    ".size mbun_ffi_call_sysv, .-mbun_ffi_call_sysv\n"
);
#endif

export module mbun.ffi.native;

import std;
import mbun.ffi.type;
import mbun.ffi.signature;

namespace mbun::ffi {

#if defined(__x86_64__)
extern "C" void mbun_ffi_call_sysv(
    void* fn, const std::uint64_t* gpregs, const std::uint64_t* sseregs,
    const std::uint64_t* stack, std::uint64_t nstack, std::uint64_t nsse,
    std::uint64_t* out_rax, double* out_xmm0);
#endif

// The three SysV argument classes mbun's scalar/pointer FFI needs. Integers,
// booleans, and every pointer-shaped type take the INTEGER class (GP registers
// then stack); float/double take the SSE class (xmm registers then stack); Void
// is only valid as a return type. Napi handles have no C-ABI representation.
enum class AbiClass { Integer, Sse, Void, Unsupported };

AbiClass classify(AbiType type) noexcept {
    switch (type) {
    case AbiType::Char:
    case AbiType::Int8:       case AbiType::Uint8:
    case AbiType::Int16:      case AbiType::Uint16:
    case AbiType::Int32:      case AbiType::Uint32:
    case AbiType::Int64:      case AbiType::Uint64:
    case AbiType::Int64Fast:  case AbiType::Uint64Fast:
    case AbiType::Bool:
    case AbiType::Pointer:    case AbiType::CString:
    case AbiType::Function:   case AbiType::Buffer:
        return AbiClass::Integer;
    case AbiType::Double:     case AbiType::Float:
        return AbiClass::Sse;
    case AbiType::Void:
        return AbiClass::Void;
    case AbiType::NapiEnv:     case AbiType::NapiValue:
        return AbiClass::Unsupported;
    }
    return AbiClass::Unsupported;
}

// Non-null iff `type` has a direct scalar/pointer C-ABI representation here
// (napi handles do not). Returns an opaque per-class descriptor tag; callers
// only test it against nullptr. (Kept for API parity with bun's ABIType→ffi
// mapping; the actual marshalling routes through classify() above.)
export const void* to_ffi_type(AbiType type) noexcept {
    static constexpr std::array<char, 3> tags{};  // {Integer, Sse, Void} slots
    switch (classify(type)) {
    case AbiClass::Integer: return &tags[0];
    case AbiClass::Sse:     return &tags[1];
    case AbiClass::Void:    return &tags[2];
    case AbiClass::Unsupported: return nullptr;
    }
    return nullptr;
}

// A tagged native scalar. Integers/bool live in `i`, float/double in `d`,
// pointer/cstring/function/buffer in `p`. Mirrors the small closed set of
// shapes an AbiType can carry across the C ABI boundary.
export struct NativeValue {
    AbiType type { AbiType::Void };
    std::int64_t i { 0 };
    double d { 0.0 };
    void* p { nullptr };

    static NativeValue integer(AbiType t, std::int64_t v) noexcept { return NativeValue { t, v, 0.0, nullptr }; }
    static NativeValue boolean(bool v) noexcept { return NativeValue { AbiType::Bool, v ? 1 : 0, 0.0, nullptr }; }
    static NativeValue float64(double v) noexcept { return NativeValue { AbiType::Double, 0, v, nullptr }; }
    static NativeValue float32(float v) noexcept { return NativeValue { AbiType::Float, 0, static_cast<double>(v), nullptr }; }
    static NativeValue pointer(void* v) noexcept { return NativeValue { AbiType::Pointer, 0, 0.0, v }; }
    static NativeValue cstring(const char* v) noexcept {
        return NativeValue { AbiType::CString, 0, 0.0, const_cast<char*>(v) };
    }

    bool as_bool() const noexcept { return i != 0; }
    double as_double() const noexcept { return d; }
    float as_float() const noexcept { return static_cast<float>(d); }
    void* as_pointer() const noexcept { return p; }
};

// A dlopen'd shared library. RAII: dlclose on destruction. Move-only.
export class NativeLibrary {
private:
    void* handle_ { nullptr };
    std::string path_;

public:
    NativeLibrary() = default;
    NativeLibrary(const NativeLibrary&) = delete;
    NativeLibrary& operator=(const NativeLibrary&) = delete;

    NativeLibrary(NativeLibrary&& other) noexcept
        : handle_ { other.handle_ }, path_ { std::move(other.path_) } {
        other.handle_ = nullptr;
    }

    NativeLibrary& operator=(NativeLibrary&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            path_ = std::move(other.path_);
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~NativeLibrary() { close(); }

    // Open `path` (or, when empty, the global/main-program symbol space).
    // On failure returns the dlerror() text.
    static std::expected<NativeLibrary, std::string> open(std::string_view path) {
        ::dlerror();  // clear any stale error
        void* handle = path.empty()
            ? ::dlopen(nullptr, RTLD_NOW | RTLD_GLOBAL)
            : ::dlopen(std::string(path).c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            const char* err = ::dlerror();
            return std::unexpected(err != nullptr ? std::string(err) : std::string("dlopen failed"));
        }
        NativeLibrary lib;
        lib.handle_ = handle;
        lib.path_ = std::string(path);
        return lib;
    }

    bool is_open() const noexcept { return handle_ != nullptr; }
    std::string_view path() const noexcept { return path_; }

    // Resolve a symbol to its address. nullopt if unresolved.
    std::optional<void*> symbol(std::string_view name) const noexcept {
        if (handle_ == nullptr) return std::nullopt;
        ::dlerror();
        void* sym = ::dlsym(handle_, std::string(name).c_str());
        // A symbol can legitimately be null; use dlerror to disambiguate.
        if (sym == nullptr && ::dlerror() != nullptr) return std::nullopt;
        return sym;
    }

    void close() noexcept {
        if (handle_ != nullptr) {
            ::dlclose(handle_);
            handle_ = nullptr;
        }
    }
};

namespace detail {

// Stable 8-byte, 8-aligned scratch slots. Every scalar AbiType fits in 8 bytes,
// so libffi reads the low `ffi_type->size` bytes correctly on a little-endian
// target. Kept in a deque-free fixed vector so pointers stay valid for the call.
using Slot = std::array<std::byte, 8>;

inline void store_argument(const NativeValue& value, AbiType type, Slot& slot) noexcept {
    std::byte* p = slot.data();
    switch (type) {
    case AbiType::Char:   { auto v = static_cast<signed char>(value.i);   std::memcpy(p, &v, sizeof v); break; }
    case AbiType::Int8:   { auto v = static_cast<std::int8_t>(value.i);   std::memcpy(p, &v, sizeof v); break; }
    case AbiType::Uint8:  { auto v = static_cast<std::uint8_t>(value.i);  std::memcpy(p, &v, sizeof v); break; }
    case AbiType::Bool:   { auto v = static_cast<std::uint8_t>(value.i != 0); std::memcpy(p, &v, sizeof v); break; }
    case AbiType::Int16:  { auto v = static_cast<std::int16_t>(value.i);  std::memcpy(p, &v, sizeof v); break; }
    case AbiType::Uint16: { auto v = static_cast<std::uint16_t>(value.i); std::memcpy(p, &v, sizeof v); break; }
    case AbiType::Int32:  { auto v = static_cast<std::int32_t>(value.i);  std::memcpy(p, &v, sizeof v); break; }
    case AbiType::Uint32: { auto v = static_cast<std::uint32_t>(value.i); std::memcpy(p, &v, sizeof v); break; }
    case AbiType::Int64:
    case AbiType::Int64Fast:  { auto v = static_cast<std::int64_t>(value.i);  std::memcpy(p, &v, sizeof v); break; }
    case AbiType::Uint64:
    case AbiType::Uint64Fast: { auto v = static_cast<std::uint64_t>(value.i); std::memcpy(p, &v, sizeof v); break; }
    case AbiType::Float:  { auto v = static_cast<float>(value.d);  std::memcpy(p, &v, sizeof v); break; }
    case AbiType::Double: { auto v = value.d;                      std::memcpy(p, &v, sizeof v); break; }
    case AbiType::Pointer:
    case AbiType::CString:
    case AbiType::Function:
    case AbiType::Buffer: { void* v = value.p; std::memcpy(p, &v, sizeof v); break; }
    default: break;
    }
}

inline NativeValue load_return(AbiType type, const Slot& slot) noexcept {
    const std::byte* p = slot.data();
    switch (type) {
    // The integer return lands in rax, whose bits above the declared width are
    // undefined per the SysV ABI. Read only the type's own bytes and sign- or
    // zero-extend to the full NativeValue integer, so a narrow negative result
    // (e.g. strcmp) round-trips correctly instead of picking up rax's garbage.
    case AbiType::Char:
    case AbiType::Int8: {
        std::int8_t v; std::memcpy(&v, p, sizeof v);
        return NativeValue::integer(type, v);
    }
    case AbiType::Int16: {
        std::int16_t v; std::memcpy(&v, p, sizeof v);
        return NativeValue::integer(type, v);
    }
    case AbiType::Int32: {
        std::int32_t v; std::memcpy(&v, p, sizeof v);
        return NativeValue::integer(type, v);
    }
    case AbiType::Int64:
    case AbiType::Int64Fast: {
        std::int64_t v; std::memcpy(&v, p, sizeof v);
        return NativeValue::integer(type, v);
    }
    case AbiType::Uint8: {
        std::uint8_t v; std::memcpy(&v, p, sizeof v);
        return NativeValue::integer(type, static_cast<std::int64_t>(v));
    }
    case AbiType::Uint16: {
        std::uint16_t v; std::memcpy(&v, p, sizeof v);
        return NativeValue::integer(type, static_cast<std::int64_t>(v));
    }
    case AbiType::Uint32: {
        std::uint32_t v; std::memcpy(&v, p, sizeof v);
        return NativeValue::integer(type, static_cast<std::int64_t>(v));
    }
    case AbiType::Uint64:
    case AbiType::Uint64Fast: {
        std::uint64_t v; std::memcpy(&v, p, sizeof v);
        return NativeValue::integer(type, static_cast<std::int64_t>(v));
    }
    case AbiType::Bool: {
        std::uint64_t v; std::memcpy(&v, p, sizeof v);
        return NativeValue::boolean((v & 0xFF) != 0);
    }
    case AbiType::Float: {
        float v; std::memcpy(&v, p, sizeof v);
        return NativeValue::float32(v);
    }
    case AbiType::Double: {
        double v; std::memcpy(&v, p, sizeof v);
        return NativeValue::float64(v);
    }
    case AbiType::Pointer:
    case AbiType::CString:
    case AbiType::Function:
    case AbiType::Buffer: {
        void* v; std::memcpy(&v, p, sizeof v);
        return NativeValue { type, 0, 0.0, v };
    }
    case AbiType::Void:
    default:
        return NativeValue { AbiType::Void, 0, 0.0, nullptr };
    }
}

} // namespace detail

// Invoke `fn` with `args` according to `signature`, via the self-implemented
// SysV trampoline. Returns the typed return value, or an error string if the
// signature/args are rejected (unsupported types, arg-count mismatch, or a
// non-x86-64 host).
export std::expected<NativeValue, std::string>
call_native(void* fn, const Signature& signature, std::span<const NativeValue> args) {
    if (fn == nullptr) return std::unexpected(std::string("null function pointer"));
    if (args.size() != signature.argument_types.size()) {
        return std::unexpected(std::string("argument count mismatch"));
    }

    const std::size_t nargs = signature.argument_types.size();

    if (classify(signature.return_type) == AbiClass::Unsupported) {
        return std::unexpected(std::string("unsupported return type: ") + std::string(type_name(signature.return_type)));
    }
    for (std::size_t idx = 0; idx < nargs; ++idx) {
        AbiType at = signature.argument_types[idx];
        if (at == AbiType::Void) {
            return std::unexpected(std::string("void is not a valid argument type"));
        }
        if (classify(at) == AbiClass::Unsupported) {
            return std::unexpected(std::string("unsupported argument type: ") + std::string(type_name(at)));
        }
    }

#if !defined(__x86_64__)
    return std::unexpected(std::string("native ffi call unsupported on this architecture (x86-64 System V only)"));
#else
    // Marshal each argument into its SysV class: INTEGER -> gp[] (6 GP regs then
    // stack), SSE -> sse[] (8 xmm regs then stack). Every scalar/pointer value
    // fits one eightbyte, produced by store_argument.
    std::array<std::uint64_t, 6> gp{};
    std::array<std::uint64_t, 8> sse{};
    std::vector<std::uint64_t> stack;
    std::size_t gpn = 0;
    std::size_t ssen = 0;

    for (std::size_t idx = 0; idx < nargs; ++idx) {
        AbiType at = signature.argument_types[idx];
        detail::Slot slot{};
        detail::store_argument(args[idx], at, slot);
        std::uint64_t eightbyte;
        std::memcpy(&eightbyte, slot.data(), sizeof eightbyte);

        if (classify(at) == AbiClass::Sse) {
            if (ssen < sse.size()) sse[ssen++] = eightbyte;
            else stack.push_back(eightbyte);
        } else {  // Integer class
            if (gpn < gp.size()) gp[gpn++] = eightbyte;
            else stack.push_back(eightbyte);
        }
    }

    std::uint64_t out_rax = 0;
    double out_xmm0 = 0.0;
    mbun_ffi_call_sysv(fn, gp.data(), sse.data(), stack.data(), stack.size(),
                       ssen, &out_rax, &out_xmm0);

    // Reconstruct the typed return: SSE-class returns come back in xmm0, every
    // other class in rax. load_return narrows the eightbyte to the exact type.
    detail::Slot rslot{};
    if (classify(signature.return_type) == AbiClass::Sse) {
        std::memcpy(rslot.data(), &out_xmm0, sizeof out_xmm0);
    } else {
        std::memcpy(rslot.data(), &out_rax, sizeof out_rax);
    }
    return detail::load_return(signature.return_type, rslot);
#endif
}

} // namespace mbun::ffi
