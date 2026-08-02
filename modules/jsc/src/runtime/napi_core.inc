// CAP-NAPI core: env plumbing, primitive values, strings, errors, handle
// scopes, references. Included by runtime.cppm OUTSIDE the anonymous
// namespace: every `extern "C" napi_*` here must be a real external symbol so
// dlopen'd .node addons resolve it from the executable (see app/cli ldflags
// `--export-dynamic`).
//
// Blueprint: bun-ref/src/jsc/bindings/napi.cpp + src/runtime/napi/napi_body.rs
// (function-by-function; see runtime/napi/mbun_napi.h for the deviations).

using mbun::napi_rt::clearLastError;
using mbun::napi_rt::setLastError;
using mbun::napi_rt::toJSValue;
using mbun::napi_rt::toNapiValue;

// ── error info ──────────────────────────────────────────────────────────────

extern "C" napi_status napi_get_last_error_info(napi_env env,
                                                const napi_extended_error_info** result) {
    if (!env) {
        return napi_invalid_arg;
    }
    NAPI_CHECK_ARG(env, result);

    // port: napi.cpp:196-236 (message table indexed by status)
    constexpr napi_status last_status = napi_would_deadlock;
    constexpr const char* error_messages[] = {
        nullptr,  // napi_ok
        "Invalid argument",
        "An object was expected",
        "A string was expected",
        "A string or symbol was expected",
        "A function was expected",
        "A number was expected",
        "A boolean was expected",
        "An array was expected",
        "Unknown failure",
        "An exception is pending",
        "The async work item was cancelled",
        "napi_escape_handle already called on scope",
        "Invalid handle scope usage",
        "Invalid callback scope usage",
        "Thread-safe function queue is full",
        "Thread-safe function handle is closing",
        "A bigint was expected",
        "A date was expected",
        "An arraybuffer was expected",
        "A detachable arraybuffer was expected",
        "Main thread would deadlock",
    };
    static_assert(std::size(error_messages) == last_status + 1);

    napi_status status = env->m_lastNapiErrorInfo.error_code;
    if (status >= 0 && status <= last_status) {
        env->m_lastNapiErrorInfo.error_message = error_messages[status];
    } else {
        env->m_lastNapiErrorInfo.error_message = nullptr;
    }
    *result = &env->m_lastNapiErrorInfo;
    return napi_ok;  // never overwrites the recorded status
}

// ── singletons ──────────────────────────────────────────────────────────────

extern "C" napi_status napi_get_undefined(napi_env env, napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    *result = toNapiValue(JSC::jsUndefined());
    return clearLastError(env);
}

extern "C" napi_status napi_get_null(napi_env env, napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    *result = toNapiValue(JSC::jsNull());
    return clearLastError(env);
}

extern "C" napi_status napi_get_global(napi_env env, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    *result = toNapiValue(env->globalObject()->globalThis());
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_get_boolean(napi_env env, bool value, napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    *result = toNapiValue(JSC::jsBoolean(value));
    return clearLastError(env);
}

// ── numbers ─────────────────────────────────────────────────────────────────

extern "C" napi_status napi_create_double(napi_env env, double value, napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    *result = toNapiValue(JSC::jsDoubleNumber(value));
    return clearLastError(env);
}

extern "C" napi_status napi_create_int32(napi_env env, int32_t value, napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    *result = toNapiValue(JSC::jsNumber(value));
    return clearLastError(env);
}

extern "C" napi_status napi_create_uint32(napi_env env, uint32_t value, napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    *result = toNapiValue(JSC::jsNumber(value));
    return clearLastError(env);
}

extern "C" napi_status napi_create_int64(napi_env env, int64_t value, napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    *result = toNapiValue(JSC::jsNumber(static_cast<double>(value)));
    return clearLastError(env);
}

extern "C" napi_status napi_get_value_double(napi_env env, napi_value value, double* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue jsValue = toJSValue(value);
    NAPI_RETURN_EARLY_IF_FALSE(env, jsValue.isNumber(), napi_number_expected);
    *result = jsValue.asNumber();
    return clearLastError(env);
}

extern "C" napi_status napi_get_value_int32(napi_env env, napi_value value, int32_t* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue jsValue = toJSValue(value);
    NAPI_RETURN_EARLY_IF_FALSE(env, jsValue.isNumber(), napi_number_expected);
    // ECMA ToInt32 semantics for out-of-range doubles (matches node/bun)
    *result = JSC::toInt32(jsValue.asNumber());
    return clearLastError(env);
}

extern "C" napi_status napi_get_value_uint32(napi_env env, napi_value value, uint32_t* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue jsValue = toJSValue(value);
    NAPI_RETURN_EARLY_IF_FALSE(env, jsValue.isNumber(), napi_number_expected);
    *result = static_cast<uint32_t>(JSC::toInt32(jsValue.asNumber()));
    return clearLastError(env);
}

extern "C" napi_status napi_get_value_int64(napi_env env, napi_value value, int64_t* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue jsValue = toJSValue(value);
    NAPI_RETURN_EARLY_IF_FALSE(env, jsValue.isNumber(), napi_number_expected);
    // port: napi.cpp:2221-2266 — 0 for every non-finite input (node gates on
    // isfinite, not just isnan: ±Infinity is documented to yield 0, NOT the
    // clamped range endpoint a finite overflow gives), then clamp finite
    // out-of-range values to the int64 endpoints.
    double d = jsValue.asNumber();
    constexpr double kMin = static_cast<double>(std::numeric_limits<int64_t>::min());
    constexpr double kMax = static_cast<double>(std::numeric_limits<int64_t>::max());
    if (!std::isfinite(d)) {
        *result = 0;
    } else if (d <= kMin) {
        *result = std::numeric_limits<int64_t>::min();
    } else if (d >= kMax) {
        *result = std::numeric_limits<int64_t>::max();
    } else {
        *result = static_cast<int64_t>(d);
    }
    return clearLastError(env);
}

extern "C" napi_status napi_get_value_bool(napi_env env, napi_value value, bool* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue jsValue = toJSValue(value);
    NAPI_RETURN_EARLY_IF_FALSE(env, jsValue.isBoolean(), napi_boolean_expected);
    *result = jsValue.asBoolean();
    return clearLastError(env);
}

// ── strings ─────────────────────────────────────────────────────────────────

extern "C" napi_status napi_create_string_utf8(napi_env env, const char* str, size_t length,
                                               napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    if (length > 0) {
        NAPI_CHECK_ARG(env, str);
    }
    size_t len = length == NAPI_AUTO_LENGTH ? (str ? strlen(str) : 0) : length;
    // invalid sequences become U+FFFD, matching node/V8 (napi_body.rs:663)
    WTF::String string = WTF::String::fromUTF8ReplacingInvalidSequences(
        std::span{reinterpret_cast<const char8_t*>(str), len});
    *result = toNapiValue(JSC::jsString(env->vm(), string));
    return clearLastError(env);
}

extern "C" napi_status napi_create_string_latin1(napi_env env, const char* str, size_t length,
                                                 napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    if (length > 0) {
        NAPI_CHECK_ARG(env, str);
    }
    size_t len = length == NAPI_AUTO_LENGTH ? (str ? strlen(str) : 0) : length;
    WTF::String string{std::span{str, len}};  // span<const char> ctor: Latin-1 bytes
    *result = toNapiValue(JSC::jsString(env->vm(), string));
    return clearLastError(env);
}

extern "C" napi_status napi_create_string_utf16(napi_env env, const char16_t* str, size_t length,
                                                napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    if (length > 0) {
        NAPI_CHECK_ARG(env, str);
    }
    size_t len = length;
    if (len == NAPI_AUTO_LENGTH) {
        len = 0;
        if (str) {
            while (str[len] != 0) {
                len++;
            }
        }
    }
    WTF::String string{std::span{str, len}};
    *result = toNapiValue(JSC::jsString(env->vm(), string));
    return clearLastError(env);
}

// port: napi.cpp napi_get_value_string_any_encoding. buf==nullptr → length
// query; truncation writes complete code points and always NUL-terminates
// when room remains.
namespace mbun::napi_rt {

enum class StringEncoding : uint8_t { utf8, latin1, utf16 };

template<StringEncoding EncodeTo, typename Element>
static napi_status getValueStringImpl(napi_env env, napi_value napiValue, Element* buf,
                                      size_t bufsize, size_t* writtenPtr) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, napiValue);
    JSC::JSValue jsValue = toJSValue(napiValue);
    NAPI_RETURN_EARLY_IF_FALSE(env, jsValue.isString(), napi_string_expected);

    JSC::JSGlobalObject* globalObject = env->globalObject();
    JSC::JSString* jsString = jsValue.toString(globalObject);
    NAPI_RETURN_IF_VM_EXCEPTION(env);
    WTF::String string = jsString->value(globalObject);
    NAPI_RETURN_IF_VM_EXCEPTION(env);

    if (buf == nullptr) {  // length query
        NAPI_CHECK_ARG(env, writtenPtr);
        if constexpr (EncodeTo == StringEncoding::utf8) {
            *writtenPtr = string.utf8().length();
        } else {
            // latin1: one byte per code unit (truncating); utf16: code units
            *writtenPtr = string.length();
        }
        return setLastError(env, napi_ok);
    }

    if (bufsize == 0) [[unlikely]] {
        if (writtenPtr) {
            *writtenPtr = 0;
        }
        return setLastError(env, napi_ok);
    }

    size_t written = 0;
    if constexpr (EncodeTo == StringEncoding::utf8) {
        WTF::CString utf8 = string.utf8();
        written = std::min(utf8.length(), bufsize - 1);
        // don't split a multi-byte sequence at the truncation point
        const char* data = utf8.data();
        while (written > 0 && written < utf8.length() &&
               (static_cast<unsigned char>(data[written]) & 0xC0) == 0x80) {
            written--;
        }
        memcpy(buf, data, written);
    } else if constexpr (EncodeTo == StringEncoding::latin1) {
        written = std::min(static_cast<size_t>(string.length()), bufsize - 1);
        if (string.is8Bit()) {
            memcpy(buf, string.span8().data(), written);
        } else {
            auto span = string.span16();
            for (size_t i = 0; i < written; i++) {
                buf[i] = static_cast<Element>(span[i] & 0xFF);  // truncate, like bun writeLatin1
            }
        }
    } else {  // utf16
        written = std::min(static_cast<size_t>(string.length()), bufsize - 1);
        if (string.is8Bit()) {
            auto span = string.span8();
            for (size_t i = 0; i < written; i++) {
                buf[i] = static_cast<Element>(span[i]);
            }
        } else {
            memcpy(buf, string.span16().data(), written * sizeof(char16_t));
        }
    }

    if (writtenPtr != nullptr) {
        *writtenPtr = written;
    }
    if (written < bufsize) {
        buf[written] = 0;
    }
    return setLastError(env, napi_ok);
}

}  // namespace mbun::napi_rt

extern "C" napi_status napi_get_value_string_utf8(napi_env env, napi_value value, char* buf,
                                                  size_t bufsize, size_t* written) {
    return mbun::napi_rt::getValueStringImpl<mbun::napi_rt::StringEncoding::utf8>(env, value, buf,
                                                                                  bufsize, written);
}

extern "C" napi_status napi_get_value_string_latin1(napi_env env, napi_value value, char* buf,
                                                    size_t bufsize, size_t* written) {
    return mbun::napi_rt::getValueStringImpl<mbun::napi_rt::StringEncoding::latin1>(
        env, value, buf, bufsize, written);
}

extern "C" napi_status napi_get_value_string_utf16(napi_env env, napi_value value, char16_t* buf,
                                                   size_t bufsize, size_t* written) {
    return mbun::napi_rt::getValueStringImpl<mbun::napi_rt::StringEncoding::utf16>(
        env, value, buf, bufsize, written);
}

// ── typeof / checks / coercions ─────────────────────────────────────────────

extern "C" napi_status napi_typeof(napi_env env, napi_value val, napi_valuetype* result) {
    // port: napi.cpp:2473 (no preamble: works with a pending exception)
    NAPI_CHECK_ARG(env, val);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue value = toJSValue(val);
    if (value.isEmpty()) {
        *result = napi_object;
        return clearLastError(env);
    }
    if (value.isCell()) {
        JSC::JSCell* cell = value.asCell();
        switch (cell->type()) {
        case JSC::JSFunctionType:
        case JSC::InternalFunctionType:
            *result = napi_function;
            return clearLastError(env);
        case JSC::ObjectType:
            if (cell->inherits(mbun::napi_rt::NapiExternal::info())) {
                *result = napi_external;
                return clearLastError(env);
            }
            *result = napi_object;
            return clearLastError(env);
        case JSC::HeapBigIntType:
            *result = napi_bigint;
            return clearLastError(env);
        case JSC::DerivedStringObjectType:
        case JSC::StringObjectType:
            *result = napi_object;
            return clearLastError(env);
        case JSC::StringType:
            *result = napi_string;
            return clearLastError(env);
        case JSC::SymbolType:
            *result = napi_symbol;
            return clearLastError(env);
        case JSC::FinalObjectType:
        case JSC::ArrayType:
        case JSC::DerivedArrayType:
            *result = napi_object;
            return clearLastError(env);
        default:
            if (cell->isCallable() || cell->isConstructor()) {
                *result = napi_function;
                return clearLastError(env);
            }
            if (cell->isObject()) {
                *result = napi_object;
                return clearLastError(env);
            }
            return setLastError(env, napi_generic_failure);
        }
    }
    if (value.isNumber()) {
        *result = napi_number;
    } else if (value.isUndefined()) {
        *result = napi_undefined;
    } else if (value.isNull()) {
        *result = napi_null;
    } else if (value.isBoolean()) {
        *result = napi_boolean;
    } else {
        return setLastError(env, napi_generic_failure);
    }
    return clearLastError(env);
}

extern "C" napi_status napi_coerce_to_bool(napi_env env, napi_value value, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    bool b = toJSValue(value).toBoolean(env->globalObject());
    NAPI_RETURN_IF_EXCEPTION(env);
    *result = toNapiValue(JSC::jsBoolean(b));
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_coerce_to_number(napi_env env, napi_value value, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    double d = toJSValue(value).toNumber(env->globalObject());
    NAPI_RETURN_IF_EXCEPTION(env);
    *result = toNapiValue(JSC::jsNumber(d));
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_coerce_to_object(napi_env env, napi_value value, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    JSC::JSObject* obj = toJSValue(value).toObject(env->globalObject());
    NAPI_RETURN_IF_EXCEPTION(env);
    *result = toNapiValue(obj);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_coerce_to_string(napi_env env, napi_value value, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue str = JSC::JSValue(toJSValue(value).toString(env->globalObject()));
    NAPI_RETURN_IF_EXCEPTION(env);
    *result = toNapiValue(str);
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_strict_equals(napi_env env, napi_value lhs, napi_value rhs,
                                          bool* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, lhs);
    NAPI_CHECK_ARG(env, rhs);
    NAPI_CHECK_ARG(env, result);
    // isStrictEqual can only throw on rope resolution OOM
    *result = JSC::JSValue::strictEqual(env->globalObject(), toJSValue(lhs), toJSValue(rhs));
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_is_array(napi_env env, napi_value value, bool* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue jsValue = toJSValue(value);
    // port: napi_body.rs:928 — bare array check, no Proxy piercing
    *result = jsValue.isCell() && jsValue.asCell()->type() == JSC::ArrayType;
    if (!*result && jsValue.isObject()) {
        *result = JSC::isArray(env->globalObject(), jsValue);
    }
    return clearLastError(env);
}

extern "C" napi_status napi_get_array_length(napi_env env, napi_value value, uint32_t* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue jsValue = toJSValue(value);
    NAPI_RETURN_EARLY_IF_FALSE(env, jsValue.isObject() && JSC::isArray(env->globalObject(), jsValue),
                               napi_array_expected);
    *result = static_cast<uint32_t>(
        toJSValue(value).get(env->globalObject(), env->vm().propertyNames->length)
            .toUInt32(env->globalObject()));
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_get_prototype(napi_env env, napi_value object, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue jsValue = toJSValue(object);
    NAPI_RETURN_EARLY_IF_FALSE(env, jsValue.isObject(), napi_object_expected);
    JSC::JSValue proto = jsValue.getObject()->getPrototype(env->globalObject());
    NAPI_RETURN_IF_EXCEPTION(env);
    *result = toNapiValue(proto);
    NAPI_RETURN_SUCCESS(env);
}

// ── containers ──────────────────────────────────────────────────────────────

extern "C" napi_status napi_create_object(napi_env env, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    *result = toNapiValue(JSC::constructEmptyObject(env->globalObject()));
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_create_array(napi_env env, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    JSC::JSArray* array = JSC::constructEmptyArray(env->globalObject(), nullptr);
    NAPI_RETURN_IF_EXCEPTION(env);
    *result = toNapiValue(array);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_create_array_with_length(napi_env env, size_t length,
                                                     napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    // port: napi_body.rs:529 — clamped to uint32
    unsigned len = static_cast<unsigned>(std::min<size_t>(length, UINT32_MAX));
    JSC::JSArray* array = JSC::constructEmptyArray(env->globalObject(), nullptr, len);
    NAPI_RETURN_IF_EXCEPTION(env);
    *result = toNapiValue(array);
    NAPI_RETURN_SUCCESS(env);
}

// ── symbols ─────────────────────────────────────────────────────────────────

extern "C" napi_status napi_create_symbol(napi_env env, napi_value description,
                                          napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    JSC::VM& vm = env->vm();
    JSC::JSValue descValue = toJSValue(description);
    if (description != nullptr && !descValue.isUndefined()) {
        NAPI_RETURN_EARLY_IF_FALSE(env, descValue.isString(), napi_string_expected);
        WTF::String desc = descValue.getString(env->globalObject());
        NAPI_RETURN_IF_EXCEPTION(env);
        *result = toNapiValue(JSC::Symbol::createWithDescription(vm, desc));
    } else {
        *result = toNapiValue(JSC::Symbol::create(vm));
    }
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status node_api_symbol_for(napi_env env, const char* utf8description,
                                           size_t length, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    NAPI_CHECK_ARG(env, utf8description);
    size_t len = length == NAPI_AUTO_LENGTH ? strlen(utf8description) : length;
    WTF::String desc = WTF::String::fromUTF8ReplacingInvalidSequences(
        std::span{reinterpret_cast<const char8_t*>(utf8description), len});
    // Symbol.for(): the VM-wide symbol registry
    *result = toNapiValue(
        JSC::Symbol::create(env->vm(), env->vm().symbolRegistry().symbolForKey(desc)));
    NAPI_RETURN_SUCCESS(env);
}

// ── errors ──────────────────────────────────────────────────────────────────

namespace mbun::napi_rt {

// port: napi.cpp createErrorWithCode
static JSC::ErrorInstance* createErrorWithCode(JSC::VM& vm, JSC::JSGlobalObject* globalObject,
                                               const WTF::String& code, const WTF::String& message,
                                               JSC::ErrorType type) {
    auto* error = JSC::ErrorInstance::create(vm, globalObject->errorStructure(type), message,
                                             JSC::JSValue(), nullptr, JSC::RuntimeType::TypeNothing,
                                             type);
    if (!code.isNull()) {
        error->putDirect(vm, JSC::Identifier::fromString(vm, "code"_s), JSC::jsString(vm, code), 0);
    }
    return error;
}

// port: napi.cpp throwErrorWithCStrings — parks the error on the env; the
// trampoline rethrows it into the VM at the JS boundary
static napi_status throwErrorWithCStrings(napi_env env, const char* code_utf8,
                                          const char* msg_utf8, JSC::ErrorType type) {
    if (!msg_utf8) {
        return setLastError(env, napi_invalid_arg);
    }
    WTF::String code = code_utf8 ? WTF::String::fromUTF8(code_utf8) : WTF::String();
    WTF::String message = WTF::String::fromUTF8(msg_utf8);
    env->scheduleException(
        createErrorWithCode(env->vm(), env->globalObject(), code, message, type));
    return setLastError(env, napi_ok);
}

// port: napi.cpp createErrorWithNapiValues (TopExceptionScope nuance reduced
// to a catch scope: a pre-existing exception is preserved, one raised by our
// own string resolution is cleared)
static napi_status createErrorWithNapiValues(napi_env env, napi_value code, napi_value message,
                                             JSC::ErrorType type, napi_value* result) {
    NAPI_CHECK_ARG(env, result);
    NAPI_CHECK_ARG(env, message);
    JSC::JSValue jsCode = toJSValue(code);
    JSC::JSValue jsMessage = toJSValue(message);
    NAPI_RETURN_EARLY_IF_FALSE(
        env, jsMessage.isString() && (jsCode.isEmpty() || jsCode.isString()),
        napi_string_expected);

    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(env->vm());
    JSC::Exception* preExisting = scope.exception();
    WTF::String code_str =
        jsCode.isEmpty() ? WTF::String() : jsCode.getString(env->globalObject());
    WTF::String message_str = jsMessage.getString(env->globalObject());
    *result = toNapiValue(
        createErrorWithCode(env->vm(), env->globalObject(), code_str, message_str, type));
    if (scope.exception() && scope.exception() != preExisting) {
        scope.clearException();
    }
    return setLastError(env, napi_ok);
}

}  // namespace mbun::napi_rt

extern "C" napi_status napi_create_error(napi_env env, napi_value code, napi_value msg,
                                         napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    return mbun::napi_rt::createErrorWithNapiValues(env, code, msg, JSC::ErrorType::Error, result);
}

extern "C" napi_status napi_create_type_error(napi_env env, napi_value code, napi_value msg,
                                              napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    return mbun::napi_rt::createErrorWithNapiValues(env, code, msg, JSC::ErrorType::TypeError,
                                                    result);
}

extern "C" napi_status napi_create_range_error(napi_env env, napi_value code, napi_value msg,
                                               napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    return mbun::napi_rt::createErrorWithNapiValues(env, code, msg, JSC::ErrorType::RangeError,
                                                    result);
}

extern "C" napi_status node_api_create_syntax_error(napi_env env, napi_value code, napi_value msg,
                                                    napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    return mbun::napi_rt::createErrorWithNapiValues(env, code, msg, JSC::ErrorType::SyntaxError,
                                                    result);
}

extern "C" napi_status napi_throw_error(napi_env env, const char* code, const char* msg) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    return mbun::napi_rt::throwErrorWithCStrings(env, code, msg, JSC::ErrorType::Error);
}

extern "C" napi_status napi_throw_type_error(napi_env env, const char* code, const char* msg) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    return mbun::napi_rt::throwErrorWithCStrings(env, code, msg, JSC::ErrorType::TypeError);
}

extern "C" napi_status napi_throw_range_error(napi_env env, const char* code, const char* msg) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    return mbun::napi_rt::throwErrorWithCStrings(env, code, msg, JSC::ErrorType::RangeError);
}

extern "C" napi_status node_api_throw_syntax_error(napi_env env, const char* code,
                                                   const char* msg) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    return mbun::napi_rt::throwErrorWithCStrings(env, code, msg, JSC::ErrorType::SyntaxError);
}

extern "C" napi_status napi_throw(napi_env env, napi_value error) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, error);
    auto scope = DECLARE_THROW_SCOPE(env->vm());
    JSC::throwException(env->globalObject(), scope, toJSValue(error));
    return setLastError(env, napi_ok);
}

extern "C" napi_status napi_is_error(napi_env env, napi_value value, bool* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue jsValue = toJSValue(value);
    *result = jsValue.isCell() && jsValue.asCell()->inherits(JSC::ErrorInstance::info());
    return clearLastError(env);
}

extern "C" napi_status napi_is_exception_pending(napi_env env, bool* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(env->vm());
    *result = env->hasPendingException() || scope.exception() != nullptr;
    return clearLastError(env);
}

extern "C" napi_status napi_get_and_clear_last_exception(napi_env env, napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(env->vm());
    if (env->hasPendingException()) {
        *result = toNapiValue(env->pendingException());
        env->clearPendingException();
    } else if (auto* exception = scope.exception()) {
        *result = toNapiValue(exception->value());
        scope.clearException();
    } else {
        *result = toNapiValue(JSC::jsUndefined());
    }
    return clearLastError(env);
}

extern "C" napi_status napi_fatal_exception(napi_env env, napi_value err) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, err);
    // node reports through 'uncaughtException'; minimal port: print + keep going
    JSC::JSValue value = toJSValue(err);
    WTF::String message = value.toWTFStringForConsole(env->globalObject());
    fprintf(stderr, "napi_fatal_exception: %s\n", message.utf8().data());
    return clearLastError(env);
}

extern "C" void napi_fatal_error(const char* location, size_t location_len, const char* message,
                                 size_t message_len) {
    const char* loc = location ? location : "";
    size_t loc_len = location_len == NAPI_AUTO_LENGTH ? strlen(loc) : location_len;
    const char* msg = message ? message : "";
    size_t msg_len = message_len == NAPI_AUTO_LENGTH ? strlen(msg) : message_len;
    if (loc_len > 0) {
        fprintf(stderr, "FATAL ERROR: %.*s %.*s\n", static_cast<int>(loc_len), loc,
                static_cast<int>(msg_len), msg);
    } else {
        fprintf(stderr, "FATAL ERROR: %.*s\n", static_cast<int>(msg_len), msg);
    }
    abort();
}

// ── handle scopes ───────────────────────────────────────────────────────────

extern "C" napi_status napi_open_handle_scope(napi_env env, napi_handle_scope* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    auto& state = mbun::napi_rt::NapiState::singleton();
    auto* rec = new mbun::napi_rt::HandleScopeRec();
    rec->parent = state.currentScope;
    state.currentScope = rec;
    *result = reinterpret_cast<napi_handle_scope>(rec);
    return clearLastError(env);
}

extern "C" napi_status napi_close_handle_scope(napi_env env, napi_handle_scope scope) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, scope);
    auto& state = mbun::napi_rt::NapiState::singleton();
    auto* rec = reinterpret_cast<mbun::napi_rt::HandleScopeRec*>(scope);
    if (state.currentScope != rec) [[unlikely]] {
        return setLastError(env, napi_handle_scope_mismatch);
    }
    state.currentScope = rec->parent;
    delete rec;
    return clearLastError(env);
}

extern "C" napi_status napi_open_escapable_handle_scope(napi_env env,
                                                        napi_escapable_handle_scope* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    auto& state = mbun::napi_rt::NapiState::singleton();
    auto* rec = new mbun::napi_rt::HandleScopeRec();
    rec->parent = state.currentScope;
    rec->escapable = true;
    state.currentScope = rec;
    *result = reinterpret_cast<napi_escapable_handle_scope>(rec);
    return clearLastError(env);
}

extern "C" napi_status napi_close_escapable_handle_scope(napi_env env,
                                                         napi_escapable_handle_scope scope) {
    return napi_close_handle_scope(env, reinterpret_cast<napi_handle_scope>(scope));
}

extern "C" napi_status napi_escape_handle(napi_env env, napi_escapable_handle_scope scope,
                                          napi_value escapee, napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, scope);
    NAPI_CHECK_ARG(env, escapee);
    NAPI_CHECK_ARG(env, result);
    auto* rec = reinterpret_cast<mbun::napi_rt::HandleScopeRec*>(scope);
    if (rec->escapeCalled) {
        return setLastError(env, napi_escape_called_twice);
    }
    rec->escapeCalled = true;
    JSC::JSValue value = toJSValue(escapee);
    if (value.isCell() && rec->parent != nullptr) {
        rec->parent->values.append(value);
    }
    *result = escapee;
    return clearLastError(env);
}

// ── references ──────────────────────────────────────────────────────────────

extern "C" napi_status napi_create_reference(napi_env env, napi_value value,
                                             uint32_t initial_refcount, napi_ref* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    NAPI_CHECK_ARG(env, value);
    JSC::JSValue val = toJSValue(value);
    bool canBeWeak = true;
    if (!(val.isObject() || val.isCallable() || val.isSymbol())) {
        // pre-experimental NAPI only allows object/function/symbol refs
        NAPI_RETURN_EARLY_IF_FALSE(env, env->nmVersion() == NAPI_VERSION_EXPERIMENTAL,
                                   napi_invalid_arg);
        canBeWeak = false;
    }
    auto* ref = new mbun::napi_rt::NapiRef(env, initial_refcount, mbun::napi_rt::NapiFinalizer{});
    ref->setValueInitial(val, canBeWeak);
    *result = reinterpret_cast<napi_ref>(ref);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_delete_reference(napi_env env, napi_ref ref) {
    // callable from finalizers mid-sweep: no throw scope, no GC check (napi.cpp:1255)
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, ref);
    delete reinterpret_cast<mbun::napi_rt::NapiRef*>(ref);
    return clearLastError(env);
}

extern "C" napi_status napi_reference_ref(napi_env env, napi_ref ref, uint32_t* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, ref);
    auto* napiRef = reinterpret_cast<mbun::napi_rt::NapiRef*>(ref);
    napiRef->ref();
    if (result) [[likely]] {
        *result = napiRef->refCount;
    }
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_reference_unref(napi_env env, napi_ref ref, uint32_t* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, ref);
    auto* napiRef = reinterpret_cast<mbun::napi_rt::NapiRef*>(ref);
    if (napiRef->refCount == 0) {
        return setLastError(env, napi_generic_failure);
    }
    napiRef->unref();
    if (result) [[likely]] {
        *result = napiRef->refCount;
    }
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_get_reference_value(napi_env env, napi_ref ref, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, ref);
    NAPI_CHECK_ARG(env, result);
    auto* napiRef = reinterpret_cast<mbun::napi_rt::NapiRef*>(ref);
    *result = toNapiValue(napiRef->value());
    NAPI_RETURN_SUCCESS(env);
}

// ── cleanup hooks / instance data / misc env ───────────────────────────────

extern "C" napi_status napi_add_env_cleanup_hook(napi_env env, void (*function)(void*),
                                                 void* data) {
    NAPI_PREAMBLE(env);
    if (function) {
        env->addCleanupHook(function, data);
    }
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_remove_env_cleanup_hook(napi_env env, void (*function)(void*),
                                                    void* data) {
    NAPI_PREAMBLE(env);
    if (function != nullptr) [[likely]] {
        env->removeCleanupHook(function, data);
    }
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_set_instance_data(napi_env env, void* data, napi_finalize finalize_cb,
                                              void* finalize_hint) {
    NAPI_PREAMBLE(env);
    env->instanceData = data;
    env->instanceDataFinalizer = mbun::napi_rt::NapiFinalizer{finalize_cb, finalize_hint};
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_get_instance_data(napi_env env, void** data) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, data);
    *data = env->instanceData;
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_get_version(napi_env env, uint32_t* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    *result = NAPI_VERSION;  // 10, matching bun DEFAULT_NAPI_VERSION
    return clearLastError(env);
}

extern "C" napi_status napi_get_node_version(napi_env env, const napi_node_version** version) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, version);
    // keep in sync with the runtime's claimed node compat version (engine.inc
    // process.versions.node)
    static constexpr napi_node_version node_version = {24, 3, 0, "node"};
    *version = &node_version;
    return clearLastError(env);
}

extern "C" napi_status napi_adjust_external_memory(napi_env env, int64_t change_in_bytes,
                                                   int64_t* adjusted_value) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, adjusted_value);
    // port: napi.cpp:1308 — reported to the collector, no bookkeeping back
    if (change_in_bytes > 0) {
        env->vm().heap.deprecatedReportExtraMemory(static_cast<size_t>(change_in_bytes));
    }
    *adjusted_value = 0;
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status node_api_get_module_file_name(napi_env env, const char** result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    *result = env->filename.c_str();
    return clearLastError(env);
}

// ── dates / promises / script ───────────────────────────────────────────────

extern "C" napi_status napi_create_date(napi_env env, double time, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    *result = toNapiValue(JSC::DateInstance::create(
        env->vm(), env->globalObject()->dateStructure(), time));
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_is_date(napi_env env, napi_value value, bool* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue jsValue = toJSValue(value);
    *result = jsValue.isCell() && jsValue.asCell()->inherits(JSC::DateInstance::info());
    return clearLastError(env);
}

extern "C" napi_status napi_get_date_value(napi_env env, napi_value value, double* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    auto* date = dynamicDowncast<JSC::DateInstance>(toJSValue(value));
    NAPI_RETURN_EARLY_IF_FALSE(env, date, napi_date_expected);
    *result = date->internalNumber();
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_create_promise(napi_env env, napi_deferred* deferred,
                                           napi_value* promise) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, deferred);
    NAPI_CHECK_ARG(env, promise);
    JSC::JSGlobalObject* globalObject = env->globalObject();
    auto* jsPromise = JSC::JSPromise::create(env->vm(), globalObject->promiseStructure());
    NAPI_RETURN_IF_EXCEPTION(env);
    // the deferred is a strong handle on the promise (napi_body.rs:1502)
    auto* strong = new JSC::Strong<JSC::JSPromise>(env->vm(), jsPromise);
    *deferred = reinterpret_cast<napi_deferred>(strong);
    *promise = toNapiValue(jsPromise);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_resolve_deferred(napi_env env, napi_deferred deferred,
                                             napi_value resolution) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, deferred);
    NAPI_CHECK_ARG(env, resolution);
    auto* strong = reinterpret_cast<JSC::Strong<JSC::JSPromise>*>(deferred);
    strong->get()->resolve(env->globalObject(), env->vm(), toJSValue(resolution));
    delete strong;
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_reject_deferred(napi_env env, napi_deferred deferred,
                                            napi_value rejection) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, deferred);
    NAPI_CHECK_ARG(env, rejection);
    auto* strong = reinterpret_cast<JSC::Strong<JSC::JSPromise>*>(deferred);
    strong->get()->reject(env->vm(), toJSValue(rejection));
    delete strong;
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_is_promise(napi_env env, napi_value value, bool* is_promise) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, is_promise);
    JSC::JSValue jsValue = toJSValue(value);
    *is_promise = jsValue.isCell() && jsValue.asCell()->inherits(JSC::JSPromise::info());
    return clearLastError(env);
}

extern "C" napi_status napi_run_script(napi_env env, napi_value script, napi_value* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, script);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue scriptValue = toJSValue(script);
    NAPI_RETURN_EARLY_IF_FALSE(env, scriptValue.isString(), napi_string_expected);

    JSC::JSGlobalObject* globalObject = env->globalObject();
    auto throwScope = DECLARE_THROW_SCOPE(env->vm());
    WTF::String code = scriptValue.getString(globalObject);
    RETURN_IF_EXCEPTION(throwScope, setLastError(env, napi_pending_exception));

    // Evaluate through the C API (JSEvaluateScript) rather than
    // JSC::makeSource + evaluate: the inline StringSourceProvider
    // instantiation needs `typeinfo for JSC::SourceProvider`, which the
    // -fno-rtti prebuilt does not export.
    JSGlobalContextRef ctx = toGlobalRef(globalObject);
    JSStringRef src = JSStringCreateWithUTF8CString(code.utf8().data());
    JSValueRef excRef = nullptr;
    JSValueRef ret = JSEvaluateScript(ctx, src, nullptr, nullptr, 1, &excRef);
    JSStringRelease(src);
    if (excRef != nullptr) {
        env->scheduleException(toJS(globalObject, excRef));
        return setLastError(env, napi_pending_exception);
    }
    *result = toNapiValue(toJS(globalObject, ret));
    return setLastError(env, napi_ok);
}
