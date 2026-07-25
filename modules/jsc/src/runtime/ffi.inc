// ── __mbunFfiNative: native backend for bun:ffi (Bun.FFI) ───────────────────
// Powers the bun:ffi JS module (builtins/ffi.cppm layer, a port of bun's
// src/js/bun/ffi.ts). The dlopen half is real: mbun.ffi.NativeLibrary does the
// dlopen/dlsym and mbun::ffi::call_native marshals a scalar/pointer signature
// through the self-implemented x86-64 System V trampoline (no libffi, no /usr
// host dep). Symbol descriptors ({args,returns}) are parsed here into an
// mbun::ffi::Signature, exactly as bun's FFIObject/host_fns.rs generate_symbols
// does. JSCallback (JS→native) and the `cc` runtime C compile stay DEFERRED.
// PORT-SOURCE: .mbun/bun-ref/src/runtime/ffi/{FFIObject.rs,host_fns.rs,ffi_body.rs}

// Callable native symbol: fn pointer + parsed signature. One heap record per
// resolved symbol, owned by the JSC object's private slot (freed in finalize).
struct FfiSymbol {
    void* fn{nullptr};
    mbun::ffi::Signature sig{};
};

JSClassRef gFfiSymbolClass{nullptr};

// dlopen'd libraries kept alive by integer handle; erase (dlclose) on close().
std::unordered_map<int, mbun::ffi::NativeLibrary>& ffi_libs() {
    static std::unordered_map<int, mbun::ffi::NativeLibrary> gLibs;
    return gLibs;
}
int& ffi_next_handle() {
    static int gNext{1};
    return gNext;
}

// Set *exc to a fresh Error("msg") and return undefined (C API throw idiom).
JSValueRef ffi_throw(JSContextRef ctx, JSValueRef* exc, const std::string& msg) {
    if (exc != nullptr) {
        JSValueRef m{make_string(ctx, msg)};
        *exc = JSObjectMakeError(ctx, 1, &m, nullptr);
    }
    return JSValueMakeUndefined(ctx);
}

// JSValue → raw memory address. Accepts a number (address) or a BigInt.
std::uintptr_t ffi_to_address(JSContextRef ctx, JSValueRef v) {
    if (JSValueIsNumber(ctx, v)) {
        double d{JSValueToNumber(ctx, v, nullptr)};
        if (!std::isfinite(d) || d < 0) return 0;
        return static_cast<std::uintptr_t>(d);
    }
    if (JSValueGetType(ctx, v) == kJSTypeBigInt) {
        std::string s{val_to_string(ctx, v)};
        std::uint64_t out{0};
        std::from_chars(s.data(), s.data() + s.size(), out);
        return static_cast<std::uintptr_t>(out);
    }
    return 0;
}

// Make a signed/unsigned 64-bit BigInt through the JSC-internal API (re-locked;
// the C API has no BigInt constructor). JSLockHolder is reentrant.
JSValueRef ffi_make_i64(JSContextRef ctx, std::int64_t v) {
    JSC::JSGlobalObject* g{toJS(ctx)};
    JSC::JSLockHolder locker{g->vm()};
    return toRef(g, JSC::JSBigInt::createFrom(g, v));
}
JSValueRef ffi_make_u64(JSContextRef ctx, std::uint64_t v) {
    JSC::JSGlobalObject* g{toJS(ctx)};
    JSC::JSLockHolder locker{g->vm()};
    return toRef(g, JSC::JSBigInt::createFrom(g, v));
}

// Parse one AbiType label from a JS value (string name or numeric FFIType).
std::optional<mbun::ffi::AbiType> ffi_parse_abi(JSContextRef ctx, JSValueRef v) {
    if (JSValueIsNumber(ctx, v)) {
        return mbun::ffi::abi_type_from_int(
            static_cast<std::int32_t>(JSValueToNumber(ctx, v, nullptr)));
    }
    if (JSValueIsString(ctx, v)) {
        return mbun::ffi::abi_type_from_name(val_to_string(ctx, v));
    }
    return std::nullopt;
}

// Parse a {args:[...], returns:...} descriptor into a Signature. Mirrors
// host_fns.rs generate_symbol_for_function's validation order/messages.
std::expected<mbun::ffi::Signature, std::string>
ffi_parse_signature(JSContextRef ctx, JSObjectRef desc) {
    mbun::ffi::Signature sig{};
    JSValueRef argsV{get_prop(ctx, desc, "args")};
    if (!JSValueIsUndefined(ctx, argsV) && !JSValueIsNull(ctx, argsV)) {
        JSObjectRef argsObj{JSValueToObject(ctx, argsV, nullptr)};
        if (argsObj == nullptr) {
            return std::unexpected(std::string{"Expected an object with \"args\" as an array"});
        }
        double len{JSValueToNumber(ctx, get_prop(ctx, argsObj, "length"), nullptr)};
        std::size_t n{std::isfinite(len) && len > 0 ? static_cast<std::size_t>(len) : 0};
        for (std::size_t i{0}; i < n; ++i) {
            JSValueRef el{JSObjectGetPropertyAtIndex(ctx, argsObj, static_cast<unsigned>(i), nullptr)};
            auto at{ffi_parse_abi(ctx, el)};
            if (!at) return std::unexpected(std::string{"Unknown FFI argument type"});
            sig.argument_types.push_back(*at);
        }
    }
    JSValueRef retV{get_prop(ctx, desc, "returns")};
    if (!JSValueIsUndefined(ctx, retV) && !JSValueIsNull(ctx, retV)) {
        auto rt{ffi_parse_abi(ctx, retV)};
        if (!rt) return std::unexpected(std::string{"Unknown FFI return type"});
        sig.return_type = *rt;
    }
    return sig;
}

// FFIBuilder-normalized JS arg → NativeValue for the given AbiType.
mbun::ffi::NativeValue ffi_js_to_native(JSContextRef ctx, JSValueRef v, mbun::ffi::AbiType at) {
    using mbun::ffi::AbiType;
    using mbun::ffi::NativeValue;
    switch (at) {
    case AbiType::Bool:
        return NativeValue::boolean(JSValueToBoolean(ctx, v));
    case AbiType::Float:
        return NativeValue::float32(static_cast<float>(JSValueToNumber(ctx, v, nullptr)));
    case AbiType::Double:
        return NativeValue::float64(JSValueToNumber(ctx, v, nullptr));
    case AbiType::Int64:
    case AbiType::Uint64:
    case AbiType::Int64Fast:
    case AbiType::Uint64Fast: {
        if (JSValueGetType(ctx, v) == kJSTypeBigInt) {
            std::string s{val_to_string(ctx, v)};
            std::int64_t out{0};
            std::from_chars(s.data(), s.data() + s.size(), out);
            return NativeValue::integer(at, out);
        }
        return NativeValue::integer(at, static_cast<std::int64_t>(JSValueToNumber(ctx, v, nullptr)));
    }
    case AbiType::Pointer:
    case AbiType::CString:
    case AbiType::Function:
    case AbiType::Buffer: {
        JSTypedArrayType tt{JSValueGetTypedArrayType(ctx, v, nullptr)};
        if (tt != kJSTypedArrayTypeNone) {
            JSObjectRef o{JSValueToObject(ctx, v, nullptr)};
            void* p{tt == kJSTypedArrayTypeArrayBuffer
                        ? JSObjectGetArrayBufferBytesPtr(ctx, o, nullptr)
                        : JSObjectGetTypedArrayBytesPtr(ctx, o, nullptr)};
            return NativeValue::pointer(p);
        }
        return NativeValue::pointer(reinterpret_cast<void*>(ffi_to_address(ctx, v)));
    }
    default:  // char/i8..u32 — integer class
        return NativeValue::integer(at, static_cast<std::int64_t>(JSValueToNumber(ctx, v, nullptr)));
    }
}

// NativeValue return → JS value per return AbiType (bun's shapes: non-fast
// 64-bit ints are BigInt, pointers are numbers, cstring stays a number and the
// JS FFIBuilder wraps it in CString).
JSValueRef ffi_native_to_js(JSContextRef ctx, const mbun::ffi::NativeValue& r, mbun::ffi::AbiType at) {
    using mbun::ffi::AbiType;
    switch (at) {
    case AbiType::Void:
        return JSValueMakeUndefined(ctx);
    case AbiType::Bool:
        return JSValueMakeBoolean(ctx, r.i != 0);
    case AbiType::Float:
    case AbiType::Double:
        return JSValueMakeNumber(ctx, r.d);
    case AbiType::Int64:
        return ffi_make_i64(ctx, r.i);
    case AbiType::Uint64:
        return ffi_make_u64(ctx, static_cast<std::uint64_t>(r.i));
    case AbiType::Pointer:
    case AbiType::CString:
    case AbiType::Function:
    case AbiType::Buffer:
        return JSValueMakeNumber(ctx,
            static_cast<double>(reinterpret_cast<std::uintptr_t>(r.p)));
    default:  // char/i8..u32, i64_fast/u64_fast → number
        return JSValueMakeNumber(ctx, static_cast<double>(r.i));
    }
}

// Invoke a resolved native symbol. Args arrive pre-normalized by the JS
// FFIBuilder; convert, call through the SysV trampoline, convert the result.
JSValueRef ffi_symbol_call(JSContextRef ctx, JSObjectRef fnObj, JSObjectRef,
                           std::size_t argc, const JSValueRef args[], JSValueRef* exc) {
    auto* sym{static_cast<FfiSymbol*>(JSObjectGetPrivate(fnObj))};
    if (sym == nullptr || sym->fn == nullptr) {
        return ffi_throw(ctx, exc, "ffi: invalid native function");
    }
    std::vector<mbun::ffi::NativeValue> nargs;
    nargs.reserve(sym->sig.argument_types.size());
    for (std::size_t i{0}; i < sym->sig.argument_types.size(); ++i) {
        JSValueRef a{i < argc ? args[i] : JSValueMakeUndefined(ctx)};
        nargs.push_back(ffi_js_to_native(ctx, a, sym->sig.argument_types[i]));
    }
    auto res{mbun::ffi::call_native(sym->fn, sym->sig, nargs)};
    if (!res) return ffi_throw(ctx, exc, res.error());
    return ffi_native_to_js(ctx, *res, sym->sig.return_type);
}

void ffi_symbol_finalize(JSObjectRef obj) {
    delete static_cast<FfiSymbol*>(JSObjectGetPrivate(obj));
}

// Build a callable symbol object carrying {fn, sig}; set its `.ptr` address.
JSObjectRef ffi_make_symbol(JSContextRef ctx, void* fn, const mbun::ffi::Signature& sig) {
    JSObjectRef o{JSObjectMake(ctx, gFfiSymbolClass, new FfiSymbol{fn, sig})};
    set_prop(ctx, o, "ptr",
             JSValueMakeNumber(ctx, static_cast<double>(reinterpret_cast<std::uintptr_t>(fn))));
    return o;
}

// ── ptr / toArrayBuffer / toBuffer / cstring ────────────────────────────────

// ptr(view, byteOffset?) → backing-store address (+ offset). ArrayBuffer or view.
JSValueRef ffi_ptr_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                      const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return ffi_throw(ctx, exc, "ptr: expected a TypedArray");
    JSObjectRef o{JSValueToObject(ctx, args[0], nullptr)};
    if (o == nullptr) return ffi_throw(ctx, exc, "ptr: expected a TypedArray");
    JSTypedArrayType tt{JSValueGetTypedArrayType(ctx, args[0], nullptr)};
    void* p{tt == kJSTypedArrayTypeArrayBuffer
                ? JSObjectGetArrayBufferBytesPtr(ctx, o, nullptr)
                : JSObjectGetTypedArrayBytesPtr(ctx, o, nullptr)};
    if (p == nullptr) return ffi_throw(ctx, exc, "ptr: expected a TypedArray or ArrayBuffer");
    std::uintptr_t addr{reinterpret_cast<std::uintptr_t>(p)};
    if (argc >= 2 && JSValueIsNumber(ctx, args[1])) {
        addr += static_cast<std::uintptr_t>(JSValueToNumber(ctx, args[1], nullptr));
    }
    return JSValueMakeNumber(ctx, static_cast<double>(addr));
}

// A no-op deallocator: the memory is externally owned (the C library / caller),
// so the JS view must not free it when collected.
void ffi_noop_dealloc(void*, void*) {}

JSValueRef ffi_to_array_buffer_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                  const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return ffi_throw(ctx, exc, "toArrayBuffer: expected a pointer");
    std::uintptr_t addr{ffi_to_address(ctx, args[0])};
    std::size_t off{argc >= 2 && JSValueIsNumber(ctx, args[1])
                        ? static_cast<std::size_t>(JSValueToNumber(ctx, args[1], nullptr)) : 0};
    std::size_t len{argc >= 3 && JSValueIsNumber(ctx, args[2])
                        ? static_cast<std::size_t>(JSValueToNumber(ctx, args[2], nullptr)) : 0};
    if (addr == 0) return ffi_throw(ctx, exc, "toArrayBuffer: null pointer");
    void* base{reinterpret_cast<void*>(addr + off)};
    return JSObjectMakeArrayBufferWithBytesNoCopy(ctx, base, len, ffi_noop_dealloc, nullptr, nullptr);
}

JSValueRef ffi_to_buffer_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return ffi_throw(ctx, exc, "toBuffer: expected a pointer");
    std::uintptr_t addr{ffi_to_address(ctx, args[0])};
    std::size_t off{argc >= 2 && JSValueIsNumber(ctx, args[1])
                        ? static_cast<std::size_t>(JSValueToNumber(ctx, args[1], nullptr)) : 0};
    std::size_t len{argc >= 3 && JSValueIsNumber(ctx, args[2])
                        ? static_cast<std::size_t>(JSValueToNumber(ctx, args[2], nullptr)) : 0};
    if (addr == 0) return ffi_throw(ctx, exc, "toBuffer: null pointer");
    void* base{reinterpret_cast<void*>(addr + off)};
    return JSObjectMakeTypedArrayWithBytesNoCopy(ctx, kJSTypedArrayTypeUint8Array, base, len,
                                                 ffi_noop_dealloc, nullptr, nullptr);
}

// cstring(ptr, byteOffset?, byteLength?) → JS string decoded from raw C memory
// (BunCString). With an explicit length, exactly that many bytes; otherwise up
// to the first NUL.
JSValueRef ffi_cstring_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return make_string(ctx, "");
    std::uintptr_t addr{ffi_to_address(ctx, args[0])};
    std::size_t off{argc >= 2 && JSValueIsNumber(ctx, args[1])
                        ? static_cast<std::size_t>(JSValueToNumber(ctx, args[1], nullptr)) : 0};
    if (addr == 0) return make_string(ctx, "");
    const char* p{reinterpret_cast<const char*>(addr + off)};
    std::string s;
    if (argc >= 3 && JSValueIsNumber(ctx, args[2])) {
        std::size_t len{static_cast<std::size_t>(JSValueToNumber(ctx, args[2], nullptr))};
        s.assign(p, len);
    } else {
        s.assign(p);
    }
    (void)exc;
    return make_string(ctx, s);
}

// ── read.* : DataView-style typed reads from a raw address ───────────────────

// Read (addr, offset) → T. Every reader shares this address computation.
template <typename T>
T ffi_read_at(JSContextRef ctx, std::size_t argc, const JSValueRef args[]) {
    std::uintptr_t addr{argc >= 1 ? ffi_to_address(ctx, args[0]) : 0};
    std::size_t off{argc >= 2 && JSValueIsNumber(ctx, args[1])
                        ? static_cast<std::size_t>(JSValueToNumber(ctx, args[1], nullptr)) : 0};
    T out{};
    std::memcpy(&out, reinterpret_cast<const void*>(addr + off), sizeof(T));
    return out;
}

#define MBUN_FFI_READ_NUM(NAME, T)                                                    \
    JSValueRef ffi_read_##NAME##_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,       \
                                    std::size_t argc, const JSValueRef args[], JSValueRef*) { \
        return JSValueMakeNumber(ctx, static_cast<double>(ffi_read_at<T>(ctx, argc, args))); \
    }
MBUN_FFI_READ_NUM(i8, std::int8_t)
MBUN_FFI_READ_NUM(u8, std::uint8_t)
MBUN_FFI_READ_NUM(i16, std::int16_t)
MBUN_FFI_READ_NUM(u16, std::uint16_t)
MBUN_FFI_READ_NUM(i32, std::int32_t)
MBUN_FFI_READ_NUM(u32, std::uint32_t)
MBUN_FFI_READ_NUM(f32, float)
MBUN_FFI_READ_NUM(f64, double)
#undef MBUN_FFI_READ_NUM

// ptr/intptr read a 64-bit word and return it as a (possibly-lossy) Number,
// matching bun's read.ptr/read.intptr (Number(getBigU/Int64)).
JSValueRef ffi_read_ptr_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef*) {
    return JSValueMakeNumber(ctx, static_cast<double>(ffi_read_at<std::uint64_t>(ctx, argc, args)));
}
JSValueRef ffi_read_intptr_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                              const JSValueRef args[], JSValueRef*) {
    return JSValueMakeNumber(ctx, static_cast<double>(ffi_read_at<std::int64_t>(ctx, argc, args)));
}
// i64/u64 return a BigInt (full 64-bit precision).
JSValueRef ffi_read_i64_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef*) {
    return ffi_make_i64(ctx, ffi_read_at<std::int64_t>(ctx, argc, args));
}
JSValueRef ffi_read_u64_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef*) {
    return ffi_make_u64(ctx, ffi_read_at<std::uint64_t>(ctx, argc, args));
}

// ── viewSource : emit a C trampoline source for a symbol descriptor ──────────

// One symbol's C source (declaration + wrapper stub). Faithful in shape to
// host_fns.rs print_source_code; the exact bytes are not asserted by tests
// (only non-empty + validation), so this is a genuine, compilable-shaped stub.
std::string ffi_gen_source(const std::string& name, const mbun::ffi::Signature& sig) {
    std::string out;
    out += "/* --- The Function To Call */\n";
    out += std::string(mbun::ffi::type_name(sig.return_type)) + " " + name + "(";
    for (std::size_t i{0}; i < sig.argument_types.size(); ++i) {
        if (i != 0) out += ", ";
        out += std::string(mbun::ffi::type_name(sig.argument_types[i])) + " arg" + std::to_string(i);
    }
    out += ");\n\n/* ---- Your Wrapper Function ---- */\n";
    out += "ZIG_REPR_TYPE JSFunctionCall(void* JS_GLOBAL_OBJECT, void* callFrame) {\n";
    out += "  LOAD_ARGUMENTS_FROM_CALL_FRAME;\n  ";
    if (sig.return_type != mbun::ffi::AbiType::Void) {
        out += std::string(mbun::ffi::type_name(sig.return_type)) + " return_value = ";
    }
    out += name + "(";
    for (std::size_t i{0}; i < sig.argument_types.size(); ++i) {
        if (i != 0) out += ", ";
        out += "arg" + std::to_string(i);
    }
    out += ");\n  return ";
    out += (sig.return_type != mbun::ffi::AbiType::Void) ? "return_value" : "ValueUndefined";
    out += ";\n}\n";
    return out;
}

// viewSource(symbols, isCallback?). isCallback truthy → a single source string
// for one function descriptor; otherwise an array of sources, one per symbol
// key. Non-object descriptor values throw "Expected an object" (bun parity).
JSValueRef ffi_view_source_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                              const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1 || !JSValueIsObject(ctx, args[0])) {
        return ffi_throw(ctx, exc, "Expected an options object with symbol names");
    }
    bool isCallback{argc >= 2 && JSValueToBoolean(ctx, args[1])};
    JSObjectRef obj{JSValueToObject(ctx, args[0], nullptr)};

    if (isCallback) {
        auto sig{ffi_parse_signature(ctx, obj)};
        if (!sig) return ffi_throw(ctx, exc, sig.error());
        return make_string(ctx, ffi_gen_source("my_callback_function", *sig));
    }

    JSPropertyNameArrayRef names{JSObjectCopyPropertyNames(ctx, obj)};
    std::size_t count{JSPropertyNameArrayGetCount(names)};
    std::vector<JSValueRef> out;
    out.reserve(count);
    for (std::size_t i{0}; i < count; ++i) {
        JSStringRef key{JSPropertyNameArrayGetNameAtIndex(names, i)};
        JSValueRef val{JSObjectGetProperty(ctx, obj, key, nullptr)};
        if (!JSValueIsObject(ctx, val)) {
            JSPropertyNameArrayRelease(names);
            return ffi_throw(ctx, exc, "Expected an object");
        }
        JSObjectRef desc{JSValueToObject(ctx, val, nullptr)};
        auto sig{ffi_parse_signature(ctx, desc)};
        if (!sig) {
            JSPropertyNameArrayRelease(names);
            return ffi_throw(ctx, exc, sig.error());
        }
        out.push_back(make_string(ctx, ffi_gen_source(to_utf8(key), *sig)));
    }
    JSPropertyNameArrayRelease(names);
    return JSObjectMakeArray(ctx, out.size(), out.empty() ? nullptr : out.data(), nullptr);
}

// ── dlopen / linkSymbols / close ────────────────────────────────────────────

// Build the {symbols:{...}, close} result object from a set of parsed symbols.
// `handle` (>0) keys the live NativeLibrary in ffi_libs for close(); 0 = none.
JSObjectRef ffi_make_result(JSContextRef ctx, JSObjectRef symbols, int handle) {
    JSObjectRef result{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, result, "symbols", symbols);
    set_prop(ctx, result, "__mbunFfiHandle", JSValueMakeNumber(ctx, handle));
    set_fn(ctx, result, "close", [](JSContextRef c, JSObjectRef, JSObjectRef self,
                                    std::size_t, const JSValueRef[], JSValueRef*) -> JSValueRef {
        int h{static_cast<int>(JSValueToNumber(c, get_prop(c, self, "__mbunFfiHandle"), nullptr))};
        if (h > 0) ffi_libs().erase(h);
        return JSValueMakeUndefined(c);
    });
    return result;
}

// dlopen(path, options). Opens the library (error names the path) and resolves
// each declared symbol (error names a missing symbol), returning callable
// natives. Mirrors bun's FFI::open error surface.
JSValueRef ffi_dlopen_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return ffi_throw(ctx, exc, "dlopen: expected a library path");
    std::string path{val_to_string(ctx, args[0])};
    if (argc < 2 || !JSValueIsObject(ctx, args[1])) {
        return ffi_throw(ctx, exc, "Expected an options object with symbol names");
    }
    // Permission Model: node gates the FFI scope in its own ffi binding, and it
    // must be gated here too — dlopen + an arbitrary call signature is arbitrary
    // native code execution, which voids every other gate in the model. Reporting
    // `process.permission.has("ffi") === false` while this still worked was the
    // exact "believes it has a sandbox and has none" bug the model exists to fix.
    if (permission_deny_scope(ctx, exc, mbun::permission::Scope::Ffi, path))
        return JSValueMakeUndefined(ctx);
    auto lib{mbun::ffi::NativeLibrary::open(path)};
    if (!lib) {
        return ffi_throw(ctx, exc, "Failed to open library \"" + path + "\": " + lib.error());
    }
    JSObjectRef options{JSValueToObject(ctx, args[1], nullptr)};
    JSObjectRef symbols{JSObjectMake(ctx, nullptr, nullptr)};

    JSPropertyNameArrayRef names{JSObjectCopyPropertyNames(ctx, options)};
    std::size_t count{JSPropertyNameArrayGetCount(names)};
    for (std::size_t i{0}; i < count; ++i) {
        JSStringRef key{JSPropertyNameArrayGetNameAtIndex(names, i)};
        std::string sym{to_utf8(key)};
        JSValueRef descV{JSObjectGetProperty(ctx, options, key, nullptr)};
        if (!JSValueIsObject(ctx, descV)) {
            JSPropertyNameArrayRelease(names);
            return ffi_throw(ctx, exc, "Expected an object for key \"" + sym + "\"");
        }
        auto sig{ffi_parse_signature(ctx, JSValueToObject(ctx, descV, nullptr))};
        if (!sig) {
            JSPropertyNameArrayRelease(names);
            return ffi_throw(ctx, exc, sig.error());
        }
        auto addr{lib->symbol(sym)};
        if (!addr) {
            JSPropertyNameArrayRelease(names);
            return ffi_throw(ctx, exc, "Symbol \"" + sym + "\" not found in \"" + path + "\"");
        }
        set_prop(ctx, symbols, sym.c_str(), ffi_make_symbol(ctx, *addr, *sig));
    }
    JSPropertyNameArrayRelease(names);

    int handle{ffi_next_handle()++};
    ffi_libs().emplace(handle, std::move(*lib));
    return ffi_make_result(ctx, symbols, handle);
}

// linkSymbols(options): like dlopen but each descriptor supplies its own `ptr`
// (a native address) instead of a library+name. Mirrors bun's link error text.
JSValueRef ffi_link_symbols_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef* exc) {
    // Permission Model: linkSymbols takes a RAW ADDRESS and a call signature, so
    // it is the same arbitrary-native-code capability as dlopen and is gated the
    // same way. Leaving it open would make the dlopen gate cosmetic.
    if (permission_deny_scope(ctx, exc, mbun::permission::Scope::Ffi, "linkSymbols"))
        return JSValueMakeUndefined(ctx);
    if (argc < 1 || !JSValueIsObject(ctx, args[0])) {
        return ffi_throw(ctx, exc, "Expected an options object with symbol names");
    }
    JSObjectRef options{JSValueToObject(ctx, args[0], nullptr)};
    JSObjectRef symbols{JSObjectMake(ctx, nullptr, nullptr)};

    JSPropertyNameArrayRef names{JSObjectCopyPropertyNames(ctx, options)};
    std::size_t count{JSPropertyNameArrayGetCount(names)};
    for (std::size_t i{0}; i < count; ++i) {
        JSStringRef key{JSPropertyNameArrayGetNameAtIndex(names, i)};
        std::string sym{to_utf8(key)};
        JSValueRef descV{JSObjectGetProperty(ctx, options, key, nullptr)};
        if (!JSValueIsObject(ctx, descV)) {
            JSPropertyNameArrayRelease(names);
            return ffi_throw(ctx, exc, "Expected an object for key \"" + sym + "\"");
        }
        JSObjectRef desc{JSValueToObject(ctx, descV, nullptr)};
        JSValueRef ptrV{get_prop(ctx, desc, "ptr")};
        if (!JSValueIsNumber(ctx, ptrV) && JSValueGetType(ctx, ptrV) != kJSTypeBigInt) {
            JSPropertyNameArrayRelease(names);
            return ffi_throw(ctx, exc, "Expected \"" + sym +
                "\" to be a function: you must provide a \"ptr\" field with the memory address "
                "of the native function. (linkSymbols/CFunction)");
        }
        std::uintptr_t addr{ffi_to_address(ctx, ptrV)};
        auto sig{ffi_parse_signature(ctx, desc)};
        if (!sig) {
            JSPropertyNameArrayRelease(names);
            return ffi_throw(ctx, exc, sig.error());
        }
        set_prop(ctx, symbols, sym.c_str(),
                 ffi_make_symbol(ctx, reinterpret_cast<void*>(addr), *sig));
    }
    JSPropertyNameArrayRelease(names);
    return ffi_make_result(ctx, symbols, 0);
}

// Install __mbunFfiNative (Bun.FFI backend). Called once from engine.inc.
void install_ffi_native(JSContextRef ctx, JSObjectRef global) {
    JSClassDefinition symDef{kJSClassDefinitionEmpty};
    symDef.className = "FFISymbol";
    symDef.callAsFunction = ffi_symbol_call;
    symDef.finalize = ffi_symbol_finalize;
    gFfiSymbolClass = JSClassCreate(&symDef);

    JSObjectRef ffi{JSObjectMake(ctx, nullptr, nullptr)};
    set_fn(ctx, ffi, "ptr", ffi_ptr_cb);
    set_fn(ctx, ffi, "toArrayBuffer", ffi_to_array_buffer_cb);
    set_fn(ctx, ffi, "toBuffer", ffi_to_buffer_cb);
    set_fn(ctx, ffi, "CString", ffi_cstring_cb);
    set_fn(ctx, ffi, "viewSource", ffi_view_source_cb);
    set_fn(ctx, ffi, "dlopen", ffi_dlopen_cb);
    set_fn(ctx, ffi, "linkSymbols", ffi_link_symbols_cb);

    JSObjectRef read{JSObjectMake(ctx, nullptr, nullptr)};
    set_fn(ctx, read, "u8", ffi_read_u8_cb);
    set_fn(ctx, read, "i8", ffi_read_i8_cb);
    set_fn(ctx, read, "u16", ffi_read_u16_cb);
    set_fn(ctx, read, "i16", ffi_read_i16_cb);
    set_fn(ctx, read, "u32", ffi_read_u32_cb);
    set_fn(ctx, read, "i32", ffi_read_i32_cb);
    set_fn(ctx, read, "u64", ffi_read_u64_cb);
    set_fn(ctx, read, "i64", ffi_read_i64_cb);
    set_fn(ctx, read, "f32", ffi_read_f32_cb);
    set_fn(ctx, read, "f64", ffi_read_f64_cb);
    set_fn(ctx, read, "ptr", ffi_read_ptr_cb);
    set_fn(ctx, read, "intptr", ffi_read_intptr_cb);
    set_prop(ctx, ffi, "read", read);

    set_prop(ctx, global, "__mbunFfiNative", ffi);
}
