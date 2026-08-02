// CAP-NAPI binary/bigint/tagging surface: ArrayBuffer, TypedArray, DataView,
// node Buffer, BigInt, object type tags and napi_get_all_property_names.
// Included by prelude.hpp after napi_core.inc / napi_objects.inc, in the GLOBAL
// MODULE FRAGMENT (same external-linkage requirement: dlopen'd .node addons
// resolve these from the executable).
//
// Blueprint: bun-ref/src/jsc/bindings/napi.cpp (function-by-function) and, for
// the buffer family, bun's node:buffer bridge. Deviations are noted inline.

#include <JavaScriptCore/JSDataView.h>
#include <JavaScriptCore/JSTypedArrays.h>
#include <JavaScriptCore/TypedArrayType.h>
#include <wtf/SharedTask.h>
#include <limits>
#include <JavaScriptCore/IdentifierInlines.h>

namespace mbun::napi_rt {

// ── shared helpers ──────────────────────────────────────────────────────────

// Unwrap a napi_value that must be an ArrayBuffer (never a view). Returns null
// for anything else, including SharedArrayBuffer, matching node's
// napi_get_arraybuffer_info contract (`arraybuffer_expected`).
inline JSC::JSArrayBuffer* asArrayBufferObject(JSC::JSValue value) {
    if (!value || !value.isCell()) {
        return nullptr;
    }
    return WTF::dynamicDowncast<JSC::JSArrayBuffer>(value.asCell());
}

inline JSC::JSArrayBufferView* asArrayBufferView(JSC::JSValue value) {
    if (!value || !value.isCell()) {
        return nullptr;
    }
    return WTF::dynamicDowncast<JSC::JSArrayBufferView>(value.asCell());
}

// port: napi.cpp toJS(napi_typedarray_type). Keeps the two enums independent
// so a JSC reordering cannot silently remap a caller's element type.
inline bool napiTypeToJSCType(napi_typedarray_type type, JSC::TypedArrayType& out) {
    switch (type) {
    case napi_int8_array: out = JSC::TypeInt8; return true;
    case napi_uint8_array: out = JSC::TypeUint8; return true;
    case napi_uint8_clamped_array: out = JSC::TypeUint8Clamped; return true;
    case napi_int16_array: out = JSC::TypeInt16; return true;
    case napi_uint16_array: out = JSC::TypeUint16; return true;
    case napi_int32_array: out = JSC::TypeInt32; return true;
    case napi_uint32_array: out = JSC::TypeUint32; return true;
    case napi_float32_array: out = JSC::TypeFloat32; return true;
    case napi_float64_array: out = JSC::TypeFloat64; return true;
    case napi_bigint64_array: out = JSC::TypeBigInt64; return true;
    case napi_biguint64_array: out = JSC::TypeBigUint64; return true;
    default: return false;
    }
}

inline bool jscTypeToNapiType(JSC::TypedArrayType type, napi_typedarray_type& out) {
    switch (type) {
    case JSC::TypeInt8: out = napi_int8_array; return true;
    case JSC::TypeUint8: out = napi_uint8_array; return true;
    case JSC::TypeUint8Clamped: out = napi_uint8_clamped_array; return true;
    case JSC::TypeInt16: out = napi_int16_array; return true;
    case JSC::TypeUint16: out = napi_uint16_array; return true;
    case JSC::TypeInt32: out = napi_int32_array; return true;
    case JSC::TypeUint32: out = napi_uint32_array; return true;
    case JSC::TypeFloat32: out = napi_float32_array; return true;
    case JSC::TypeFloat64: out = napi_float64_array; return true;
    case JSC::TypeBigInt64: out = napi_bigint64_array; return true;
    case JSC::TypeBigUint64: out = napi_biguint64_array; return true;
    default: return false;  // TypeDataView / NotTypedArray
    }
}

inline size_t elementSizeOf(JSC::TypedArrayType type) {
    switch (type) {
    case JSC::TypeInt8:
    case JSC::TypeUint8:
    case JSC::TypeUint8Clamped: return 1;
    case JSC::TypeInt16:
    case JSC::TypeUint16: return 2;
    case JSC::TypeInt32:
    case JSC::TypeUint32:
    case JSC::TypeFloat32: return 4;
    default: return 8;  // Float64 / BigInt64 / BigUint64
    }
}

// Wrap a live JSC::ArrayBuffer in the global object's ArrayBuffer structure.
inline JSC::JSArrayBuffer* wrapArrayBuffer(napi_env env, RefPtr<JSC::ArrayBuffer>&& buffer) {
    JSC::JSGlobalObject* global = env->globalObject();
    return JSC::JSArrayBuffer::create(
        env->vm(), global->arrayBufferStructure(JSC::ArrayBufferSharingMode::Default),
        std::move(buffer));
}

// Build the typed-array cell for a JSC::TypedArrayType over an existing buffer.
// JSC's public C API only exposes the (type, buffer, offset, length) form for
// the eleven element types, and needs a JSObjectRef, so route through it: it is
// exactly what the rest of this runtime uses to mint typed arrays.
inline JSC::JSValue makeTypedArrayOverBuffer(napi_env env, napi_typedarray_type type,
                                             JSC::JSArrayBuffer* buffer, size_t byteOffset,
                                             size_t length, JSValueRef* exception) {
    static constexpr JSTypedArrayType kApiTypes[] = {
        kJSTypedArrayTypeInt8Array,    kJSTypedArrayTypeUint8Array,
        kJSTypedArrayTypeUint8ClampedArray, kJSTypedArrayTypeInt16Array,
        kJSTypedArrayTypeUint16Array,  kJSTypedArrayTypeInt32Array,
        kJSTypedArrayTypeUint32Array,  kJSTypedArrayTypeFloat32Array,
        kJSTypedArrayTypeFloat64Array, kJSTypedArrayTypeBigInt64Array,
        kJSTypedArrayTypeBigUint64Array,
    };
    if (static_cast<size_t>(type) >= std::size(kApiTypes)) {
        return {};
    }
    JSC::JSGlobalObject* global = env->globalObject();
    JSContextRef ctx = toRef(global);
    JSObjectRef bufferObj = toRef(static_cast<JSC::JSObject*>(buffer));
    JSObjectRef view = JSObjectMakeTypedArrayWithArrayBufferAndOffset(
        ctx, kApiTypes[static_cast<size_t>(type)], bufferObj, byteOffset, length, exception);
    if (view == nullptr) {
        return {};
    }
    return toJS(global, static_cast<JSValueRef>(view));
}

// ── type tags ───────────────────────────────────────────────────────────────
// port: napi.cpp napi_type_tag_object. bun stores the 128-bit tag in a hidden
// slot; here it rides the same private-name mechanism as napi_wrap, holding a
// heap napi_type_tag freed by a NapiExternal finalizer.
inline JSC::Identifier typeTagIdent() {
    static JSC::Identifier* ident = new JSC::Identifier(
        JSC::Identifier::fromUid(JSC::PrivateName(JSC::PrivateName::Description, "napiTypeTag"_s)));
    return *ident;
}

inline void freeTypeTag(napi_env, void* data, void*) {
    delete static_cast<napi_type_tag*>(data);
}

inline napi_type_tag* getTypeTagIfExists(JSC::VM& vm, JSC::JSObject* object) {
    JSC::JSValue slot = object->getDirect(vm, typeTagIdent());
    if (!slot) {
        return nullptr;
    }
    auto* external = dynamicDowncast<NapiExternal>(slot);
    return external ? static_cast<napi_type_tag*>(external->value()) : nullptr;
}

}  // namespace mbun::napi_rt

// ── ArrayBuffer ─────────────────────────────────────────────────────────────

extern "C" napi_status napi_is_arraybuffer(napi_env env, napi_value value, bool* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    *result = mbun::napi_rt::asArrayBufferObject(toJSValue(value)) != nullptr;
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_create_arraybuffer(napi_env env, size_t byte_length, void** data,
                                               napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    RefPtr<JSC::ArrayBuffer> buffer = JSC::ArrayBuffer::tryCreate(byte_length, 1);
    NAPI_RETURN_EARLY_IF_FALSE(env, buffer, napi_generic_failure);
    if (data != nullptr) {
        *data = buffer->data();
    }
    JSC::JSArrayBuffer* wrapper = mbun::napi_rt::wrapArrayBuffer(env, std::move(buffer));
    NAPI_RETURN_EARLY_IF_FALSE(env, wrapper, napi_generic_failure);
    *result = toNapiValue(JSC::JSValue(wrapper));
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_create_external_arraybuffer(napi_env env, void* external_data,
                                                        size_t byte_length,
                                                        napi_finalize finalize_cb,
                                                        void* finalize_hint, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    // The destructor runs on ArrayBuffer teardown (GC or detach), which is
    // exactly node's "the data is no longer referenced" point. It goes through
    // NapiFinalizer so it lands on the deferred queue, never inside GC.
    auto finalizer = mbun::napi_rt::NapiFinalizer{finalize_cb, finalize_hint};
    RefPtr<JSC::ArrayBuffer> buffer = JSC::ArrayBuffer::createFromBytes(
        std::span<const uint8_t>{static_cast<const uint8_t*>(external_data),
                                 external_data ? byte_length : 0},
        createSharedTask<void(void*)>([env, finalizer](void* ptr) mutable {
            finalizer.call(env, ptr);
        }));
    NAPI_RETURN_EARLY_IF_FALSE(env, buffer, napi_generic_failure);
    JSC::JSArrayBuffer* wrapper = mbun::napi_rt::wrapArrayBuffer(env, std::move(buffer));
    NAPI_RETURN_EARLY_IF_FALSE(env, wrapper, napi_generic_failure);
    *result = toNapiValue(JSC::JSValue(wrapper));
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_get_arraybuffer_info(napi_env env, napi_value arraybuffer, void** data,
                                                 size_t* byte_length) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, arraybuffer);
    JSC::JSArrayBuffer* wrapper = mbun::napi_rt::asArrayBufferObject(toJSValue(arraybuffer));
    NAPI_RETURN_EARLY_IF_FALSE(env, wrapper, napi_arraybuffer_expected);
    JSC::ArrayBuffer* impl = wrapper->impl();
    NAPI_RETURN_EARLY_IF_FALSE(env, impl, napi_arraybuffer_expected);
    if (data != nullptr) {
        *data = impl->isDetached() ? nullptr : impl->data();
    }
    if (byte_length != nullptr) {
        *byte_length = impl->isDetached() ? 0 : impl->byteLength();
    }
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_detach_arraybuffer(napi_env env, napi_value arraybuffer) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, arraybuffer);
    JSC::JSArrayBuffer* wrapper = mbun::napi_rt::asArrayBufferObject(toJSValue(arraybuffer));
    NAPI_RETURN_EARLY_IF_FALSE(env, wrapper, napi_arraybuffer_expected);
    JSC::ArrayBuffer* impl = wrapper->impl();
    NAPI_RETURN_EARLY_IF_FALSE(env, impl, napi_arraybuffer_expected);
    // node reports detachable_arraybuffer_expected for buffers that refuse to
    // detach (WASM memory, non-transferable); JSC records that as isShared().
    NAPI_RETURN_EARLY_IF_FALSE(env, !impl->isShared(), napi_detachable_arraybuffer_expected);
    if (!impl->isDetached()) {
        impl->detach(env->vm());
    }
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_is_detached_arraybuffer(napi_env env, napi_value value, bool* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    JSC::JSArrayBuffer* wrapper = mbun::napi_rt::asArrayBufferObject(toJSValue(value));
    // node: a non-ArrayBuffer is simply "not detached", not an error.
    *result = wrapper != nullptr && wrapper->impl() != nullptr && wrapper->impl()->isDetached();
    NAPI_RETURN_SUCCESS(env);
}

// ── TypedArray ──────────────────────────────────────────────────────────────

extern "C" napi_status napi_is_typedarray(napi_env env, napi_value value, bool* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    JSC::JSArrayBufferView* view = mbun::napi_rt::asArrayBufferView(toJSValue(value));
    napi_typedarray_type unused;
    // A DataView is a JSArrayBufferView too, so gate on the element type: only
    // the eleven element kinds map to a napi_typedarray_type.
    *result = view != nullptr
              && mbun::napi_rt::jscTypeToNapiType(JSC::typedArrayType(view->type()), unused);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_create_typedarray(napi_env env, napi_typedarray_type type,
                                              size_t length, napi_value arraybuffer,
                                              size_t byte_offset, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, arraybuffer);
    NAPI_CHECK_ARG(env, result);
    JSC::TypedArrayType jscType;
    NAPI_RETURN_EARLY_IF_FALSE(env, mbun::napi_rt::napiTypeToJSCType(type, jscType),
                               napi_invalid_arg);
    JSC::JSArrayBuffer* wrapper = mbun::napi_rt::asArrayBufferObject(toJSValue(arraybuffer));
    NAPI_RETURN_EARLY_IF_FALSE(env, wrapper, napi_arraybuffer_expected);
    JSC::ArrayBuffer* impl = wrapper->impl();
    NAPI_RETURN_EARLY_IF_FALSE(env, impl, napi_arraybuffer_expected);
    // port: node_api.cc napi_create_typedarray — a window past the end of the
    // buffer is ERR_NAPI_INVALID_TYPEDARRAY_LENGTH, a *thrown* RangeError, not
    // a status code. Same for a misaligned offset.
    const size_t elementSize = mbun::napi_rt::elementSizeOf(jscType);
    if (length > (std::numeric_limits<size_t>::max() - byte_offset) / elementSize
        || (length * elementSize) + byte_offset > impl->byteLength()) {
        auto scope = DECLARE_THROW_SCOPE(env->vm());
        JSC::throwException(env->globalObject(), scope,
                            JSC::createRangeError(env->globalObject(),
                                                  "Invalid typed array length"_s));
        return setLastError(env, napi_pending_exception);
    }
    if (byte_offset % elementSize != 0) {
        auto scope = DECLARE_THROW_SCOPE(env->vm());
        JSC::throwException(
            env->globalObject(), scope,
            JSC::createRangeError(
                env->globalObject(),
                makeString("start offset should be a multiple of "_s, elementSize)));
        return setLastError(env, napi_pending_exception);
    }
    JSValueRef exception = nullptr;
    JSC::JSValue view =
        mbun::napi_rt::makeTypedArrayOverBuffer(env, type, wrapper, byte_offset, length, &exception);
    NAPI_RETURN_IF_EXCEPTION(env);
    NAPI_RETURN_EARLY_IF_FALSE(env, view && exception == nullptr, napi_generic_failure);
    *result = toNapiValue(view);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_get_typedarray_info(napi_env env, napi_value typedarray,
                                                napi_typedarray_type* type, size_t* length,
                                                void** data, napi_value* arraybuffer,
                                                size_t* byte_offset) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, typedarray);
    JSC::JSArrayBufferView* view = mbun::napi_rt::asArrayBufferView(toJSValue(typedarray));
    NAPI_RETURN_EARLY_IF_FALSE(env, view, napi_invalid_arg);
    napi_typedarray_type napiType;
    NAPI_RETURN_EARLY_IF_FALSE(
        env, mbun::napi_rt::jscTypeToNapiType(JSC::typedArrayType(view->type()), napiType),
        napi_invalid_arg);
    const bool detached = view->isDetached();
    if (type != nullptr) {
        *type = napiType;
    }
    if (length != nullptr) {
        *length = detached ? 0 : view->length();
    }
    if (data != nullptr) {
        *data = detached ? nullptr : view->vector();
    }
    if (byte_offset != nullptr) {
        *byte_offset = detached ? 0 : view->byteOffset();
    }
    if (arraybuffer != nullptr) {
        // Materializing the buffer is what node does too (it hands back the
        // view's backing ArrayBuffer object, allocating the wrapper on demand).
        JSC::JSGlobalObject* global = env->globalObject();
        JSContextRef ctx = toRef(global);
        JSObjectRef viewObj = toRef(static_cast<JSC::JSObject*>(view));
        JSValueRef exception = nullptr;
        JSObjectRef buffer = JSObjectGetTypedArrayBuffer(ctx, viewObj, &exception);
        NAPI_RETURN_IF_EXCEPTION(env);
        *arraybuffer = toNapiValue(buffer ? toJS(global, static_cast<JSValueRef>(buffer))
                                          : JSC::jsUndefined());
    }
    NAPI_RETURN_SUCCESS(env);
}

// ── DataView ────────────────────────────────────────────────────────────────

extern "C" napi_status napi_is_dataview(napi_env env, napi_value value, bool* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue jsValue = toJSValue(value);
    *result = jsValue && jsValue.isCell()
              && WTF::dynamicDowncast<JSC::JSDataView>(jsValue.asCell()) != nullptr;
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_create_dataview(napi_env env, size_t byte_length,
                                            napi_value arraybuffer, size_t byte_offset,
                                            napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, arraybuffer);
    NAPI_CHECK_ARG(env, result);
    JSC::JSArrayBuffer* wrapper = mbun::napi_rt::asArrayBufferObject(toJSValue(arraybuffer));
    NAPI_RETURN_EARLY_IF_FALSE(env, wrapper, napi_arraybuffer_expected);
    JSC::ArrayBuffer* impl = wrapper->impl();
    NAPI_RETURN_EARLY_IF_FALSE(env, impl, napi_arraybuffer_expected);
    // port: napi.cpp napi_create_dataview — an out-of-range window is a thrown
    // RangeError with node's exact wording, not a status code.
    if (byte_length + byte_offset > impl->byteLength()) {
        auto scope = DECLARE_THROW_SCOPE(env->vm());
        JSC::throwException(
            env->globalObject(), scope,
            JSC::createRangeError(
                env->globalObject(),
                "byte_offset + byte_length should be less than or equal to the size in bytes of the array passed in"_s));
        return setLastError(env, napi_pending_exception);
    }
    JSC::JSGlobalObject* global = env->globalObject();
    JSC::JSDataView* view = JSC::JSDataView::create(
        global, global->typedArrayStructure(JSC::TypeDataView, /*isResizableOrGrowableShared=*/false),
        RefPtr<JSC::ArrayBuffer>{impl}, byte_offset, byte_length);
    NAPI_RETURN_IF_EXCEPTION(env);
    NAPI_RETURN_EARLY_IF_FALSE(env, view, napi_generic_failure);
    *result = toNapiValue(JSC::JSValue(view));
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_get_dataview_info(napi_env env, napi_value dataview,
                                              size_t* byte_length, void** data,
                                              napi_value* arraybuffer, size_t* byte_offset) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, dataview);
    JSC::JSValue jsValue = toJSValue(dataview);
    JSC::JSDataView* view =
        jsValue && jsValue.isCell() ? WTF::dynamicDowncast<JSC::JSDataView>(jsValue.asCell())
                                    : nullptr;
    NAPI_RETURN_EARLY_IF_FALSE(env, view, napi_invalid_arg);
    const bool detached = view->isDetached();
    if (byte_length != nullptr) {
        *byte_length = detached ? 0 : view->byteLength();
    }
    if (data != nullptr) {
        *data = detached ? nullptr : view->vector();
    }
    if (byte_offset != nullptr) {
        *byte_offset = detached ? 0 : view->byteOffset();
    }
    if (arraybuffer != nullptr) {
        JSC::ArrayBuffer* impl = view->possiblySharedBuffer();
        *arraybuffer =
            impl ? toNapiValue(JSC::JSValue(
                       mbun::napi_rt::wrapArrayBuffer(env, RefPtr<JSC::ArrayBuffer>{impl})))
                 : toNapiValue(JSC::jsUndefined());
    }
    NAPI_RETURN_SUCCESS(env);
}

// ── node Buffer ─────────────────────────────────────────────────────────────
// A node Buffer is a Uint8Array whose prototype is Buffer.prototype. mbun's
// Buffer lives in the JS layer (compat/bun/src/js/node), so the four creators
// below mint the backing ArrayBuffer in C++ and re-prototype the view rather
// than duplicating Buffer's construction logic here.

namespace mbun::napi_rt {

// `globalThis.Buffer.prototype` — the JS-layer constructor's prototype is what
// makes a Uint8Array a Buffer for this runtime, so it is the single authority
// both for minting Buffers and for answering napi_is_buffer. Read through the
// public C API (never throws out of a host frame, and swallows an absent
// Buffer global cleanly), which is the pattern the rest of the runtime uses.
inline JSC::JSObject* bufferPrototype(napi_env env) {
    JSC::JSGlobalObject* global = env->globalObject();
    JSContextRef ctx = toRef(global);
    JSObjectRef globalThis = JSContextGetGlobalObject(ctx);
    if (globalThis == nullptr) {
        return nullptr;
    }
    JSValueRef exception = nullptr;
    JSStringRef bufferName = JSStringCreateWithUTF8CString("Buffer");
    JSValueRef ctorValue = JSObjectGetProperty(ctx, globalThis, bufferName, &exception);
    JSStringRelease(bufferName);
    if (exception != nullptr || ctorValue == nullptr || !JSValueIsObject(ctx, ctorValue)) {
        return nullptr;
    }
    JSObjectRef ctorObject = JSValueToObject(ctx, ctorValue, &exception);
    if (exception != nullptr || ctorObject == nullptr) {
        return nullptr;
    }
    JSStringRef protoName = JSStringCreateWithUTF8CString("prototype");
    JSValueRef protoValue = JSObjectGetProperty(ctx, ctorObject, protoName, &exception);
    JSStringRelease(protoName);
    if (exception != nullptr || protoValue == nullptr || !JSValueIsObject(ctx, protoValue)) {
        return nullptr;
    }
    return toJS(global, protoValue).getObject();
}

// Wrap an ArrayBuffer as a Uint8Array and re-prototype it to Buffer.prototype,
// which is exactly what node:buffer's own fast path does.
inline JSC::JSValue makeBufferOverArrayBuffer(napi_env env, JSC::JSArrayBuffer* buffer,
                                              size_t byteOffset, size_t length) {
    JSValueRef exception = nullptr;
    JSC::JSValue view =
        makeTypedArrayOverBuffer(env, napi_uint8_array, buffer, byteOffset, length, &exception);
    if (!view || exception != nullptr) {
        return {};
    }
    if (JSC::JSObject* proto = bufferPrototype(env)) {
        JSC::JSGlobalObject* global = env->globalObject();
        view.getObject()->setPrototype(env->vm(), global, JSC::JSValue(proto));
    }
    return view;
}

}  // namespace mbun::napi_rt

extern "C" napi_status napi_create_buffer(napi_env env, size_t length, void** data,
                                          napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    RefPtr<JSC::ArrayBuffer> buffer = JSC::ArrayBuffer::tryCreate(length, 1);
    NAPI_RETURN_EARLY_IF_FALSE(env, buffer, napi_generic_failure);
    if (data != nullptr) {
        *data = buffer->data();
    }
    JSC::JSArrayBuffer* wrapper = mbun::napi_rt::wrapArrayBuffer(env, std::move(buffer));
    NAPI_RETURN_EARLY_IF_FALSE(env, wrapper, napi_generic_failure);
    JSC::JSValue view = mbun::napi_rt::makeBufferOverArrayBuffer(env, wrapper, 0, length);
    NAPI_RETURN_IF_EXCEPTION(env);
    NAPI_RETURN_EARLY_IF_FALSE(env, view, napi_generic_failure);
    *result = toNapiValue(view);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_create_buffer_copy(napi_env env, size_t length, const void* data,
                                               void** result_data, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    RefPtr<JSC::ArrayBuffer> buffer = JSC::ArrayBuffer::tryCreate(length, 1);
    NAPI_RETURN_EARLY_IF_FALSE(env, buffer, napi_generic_failure);
    if (data != nullptr && length > 0) {
        std::memcpy(buffer->data(), data, length);
    }
    if (result_data != nullptr) {
        *result_data = buffer->data();
    }
    JSC::JSArrayBuffer* wrapper = mbun::napi_rt::wrapArrayBuffer(env, std::move(buffer));
    NAPI_RETURN_EARLY_IF_FALSE(env, wrapper, napi_generic_failure);
    JSC::JSValue view = mbun::napi_rt::makeBufferOverArrayBuffer(env, wrapper, 0, length);
    NAPI_RETURN_IF_EXCEPTION(env);
    NAPI_RETURN_EARLY_IF_FALSE(env, view, napi_generic_failure);
    *result = toNapiValue(view);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_create_external_buffer(napi_env env, size_t length, void* data,
                                                   napi_finalize finalize_cb, void* finalize_hint,
                                                   napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    auto finalizer = mbun::napi_rt::NapiFinalizer{finalize_cb, finalize_hint};
    RefPtr<JSC::ArrayBuffer> buffer = JSC::ArrayBuffer::createFromBytes(
        std::span<const uint8_t>{static_cast<const uint8_t*>(data), data ? length : 0},
        createSharedTask<void(void*)>([env, finalizer](void* ptr) mutable {
            finalizer.call(env, ptr);
        }));
    NAPI_RETURN_EARLY_IF_FALSE(env, buffer, napi_generic_failure);
    JSC::JSArrayBuffer* wrapper = mbun::napi_rt::wrapArrayBuffer(env, std::move(buffer));
    NAPI_RETURN_EARLY_IF_FALSE(env, wrapper, napi_generic_failure);
    JSC::JSValue view = mbun::napi_rt::makeBufferOverArrayBuffer(env, wrapper, 0, length);
    NAPI_RETURN_IF_EXCEPTION(env);
    NAPI_RETURN_EARLY_IF_FALSE(env, view, napi_generic_failure);
    *result = toNapiValue(view);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_is_buffer(napi_env env, napi_value value, bool* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    *result = false;
    JSC::JSValue jsValue = toJSValue(value);
    JSC::JSArrayBufferView* view = mbun::napi_rt::asArrayBufferView(jsValue);
    if (view != nullptr) {
        // Authority is the JS layer's own Buffer.prototype, so a Buffer made
        // by node:buffer and one made by napi_create_buffer answer alike.
        if (JSC::JSObject* proto = mbun::napi_rt::bufferPrototype(env)) {
            JSC::JSValue chain = jsValue.getObject()->getPrototype(env->globalObject());
            NAPI_RETURN_IF_EXCEPTION(env);
            while (chain.isObject()) {
                if (chain.getObject() == proto) {
                    *result = true;
                    break;
                }
                chain = chain.getObject()->getPrototype(env->globalObject());
                NAPI_RETURN_IF_EXCEPTION(env);
            }
        }
    }
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_get_buffer_info(napi_env env, napi_value value, void** data,
                                            size_t* length) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    JSC::JSArrayBufferView* view = mbun::napi_rt::asArrayBufferView(toJSValue(value));
    NAPI_RETURN_EARLY_IF_FALSE(env, view, napi_invalid_arg);
    const bool detached = view->isDetached();
    if (data != nullptr) {
        *data = detached ? nullptr : view->vector();
    }
    if (length != nullptr) {
        *length = detached ? 0 : view->byteLength();
    }
    NAPI_RETURN_SUCCESS(env);
}

// ── BigInt ──────────────────────────────────────────────────────────────────

extern "C" napi_status napi_create_bigint_int64(napi_env env, int64_t value, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    JSC::JSBigInt* bigint = JSC::JSBigInt::createFrom(env->globalObject(), value);
    NAPI_RETURN_IF_EXCEPTION(env);
    NAPI_RETURN_EARLY_IF_FALSE(env, bigint, napi_generic_failure);
    *result = toNapiValue(JSC::JSValue(bigint));
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_create_bigint_uint64(napi_env env, uint64_t value, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    JSC::JSBigInt* bigint = JSC::JSBigInt::createFrom(env->globalObject(), value);
    NAPI_RETURN_IF_EXCEPTION(env);
    NAPI_RETURN_EARLY_IF_FALSE(env, bigint, napi_generic_failure);
    *result = toNapiValue(JSC::JSValue(bigint));
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_create_bigint_words(napi_env env, int sign_bit, size_t word_count,
                                                const uint64_t* words, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    NAPI_RETURN_EARLY_IF_FALSE(env, words != nullptr || word_count == 0, napi_invalid_arg);
    // port: node_api.cc napi_create_bigint_words. Past INT_MAX words the call
    // is simply malformed (napi_invalid_arg, nothing thrown); AT INT_MAX it is
    // well-formed and the engine's own RangeError/OOM is forwarded as
    // napi_pending_exception. test_bigint asserts both halves separately.
    NAPI_RETURN_EARLY_IF_FALSE(env, word_count <= static_cast<size_t>(INT_MAX), napi_invalid_arg);
    JSC::JSBigInt* bigint = JSC::JSBigInt::createFromWords(
        env->globalObject(), std::span<const uint64_t>{words, word_count}, sign_bit != 0);
    NAPI_RETURN_IF_EXCEPTION(env);
    NAPI_RETURN_EARLY_IF_FALSE(env, bigint, napi_generic_failure);
    *result = toNapiValue(JSC::JSValue(bigint));
    NAPI_RETURN_SUCCESS(env);
}

namespace mbun::napi_rt {

inline JSC::JSBigInt* asBigInt(JSC::JSValue value) {
    if (!value || !value.isCell()) {
        return nullptr;
    }
    return WTF::dynamicDowncast<JSC::JSBigInt>(value.asCell());
}

}  // namespace mbun::napi_rt

extern "C" napi_status napi_get_value_bigint_int64(napi_env env, napi_value value, int64_t* result,
                                                   bool* lossless) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    NAPI_CHECK_ARG(env, lossless);
    JSC::JSBigInt* bigint = mbun::napi_rt::asBigInt(toJSValue(value));
    NAPI_RETURN_EARLY_IF_FALSE(env, bigint, napi_bigint_expected);

    const unsigned length = bigint->length();
    const bool sign = bigint->sign();
    if (length == 0) {
        *result = 0;
        *lossless = true;
        NAPI_RETURN_SUCCESS(env);
    }
    const uint64_t magnitude = static_cast<uint64_t>(bigint->digit(0));
    // Two's-complement truncation, exactly node's `res = sign ? -mag : mag`
    // with wraparound; lossless iff one digit AND the magnitude round-trips.
    const uint64_t truncated = sign ? (~magnitude + 1) : magnitude;
    *result = static_cast<int64_t>(truncated);
    *lossless = length == 1
                && (sign ? magnitude <= (static_cast<uint64_t>(1) << 63)
                         : magnitude < (static_cast<uint64_t>(1) << 63));
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_get_value_bigint_uint64(napi_env env, napi_value value,
                                                    uint64_t* result, bool* lossless) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, result);
    NAPI_CHECK_ARG(env, lossless);
    JSC::JSBigInt* bigint = mbun::napi_rt::asBigInt(toJSValue(value));
    NAPI_RETURN_EARLY_IF_FALSE(env, bigint, napi_bigint_expected);

    const unsigned length = bigint->length();
    if (length == 0) {
        *result = 0;
        *lossless = true;
        NAPI_RETURN_SUCCESS(env);
    }
    const uint64_t magnitude = static_cast<uint64_t>(bigint->digit(0));
    const bool sign = bigint->sign();
    *result = sign ? (~magnitude + 1) : magnitude;
    *lossless = length == 1 && !sign;
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_get_value_bigint_words(napi_env env, napi_value value, int* sign_bit,
                                                   size_t* word_count, uint64_t* words) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, word_count);
    JSC::JSBigInt* bigint = mbun::napi_rt::asBigInt(toJSValue(value));
    NAPI_RETURN_EARLY_IF_FALSE(env, bigint, napi_bigint_expected);

    const size_t length = bigint->length();
    // node's two-call protocol: words == nullptr means "tell me the size".
    if (words == nullptr) {
        *word_count = length;
        NAPI_RETURN_SUCCESS(env);
    }
    NAPI_CHECK_ARG(env, sign_bit);
    *sign_bit = bigint->sign() ? 1 : 0;
    const size_t copied = std::min(length, *word_count);
    for (size_t i = 0; i < copied; i++) {
        words[i] = static_cast<uint64_t>(bigint->digit(static_cast<unsigned>(i)));
    }
    *word_count = copied;
    NAPI_RETURN_SUCCESS(env);
}

// ── object type tags ────────────────────────────────────────────────────────

extern "C" napi_status napi_type_tag_object(napi_env env, napi_value value,
                                            const napi_type_tag* type_tag) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, type_tag);
    JSC::JSObject* object = toJSValue(value).getObject();
    NAPI_RETURN_EARLY_IF_FALSE(env, object, napi_object_expected);
    JSC::VM& vm = env->vm();
    // node: re-tagging an already tagged object is napi_invalid_arg.
    NAPI_RETURN_EARLY_IF_FALSE(env, mbun::napi_rt::getTypeTagIfExists(vm, object) == nullptr,
                               napi_invalid_arg);
    auto* tag = new napi_type_tag{*type_tag};
    auto* external =
        mbun::napi_rt::NapiExternal::create(vm, mbun::napi_rt::externalStructure(env->globalObject()),
                                            tag, nullptr, mbun::napi_rt::freeTypeTag, env);
    object->putDirect(vm, mbun::napi_rt::typeTagIdent(), JSC::JSValue(external));
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_check_object_type_tag(napi_env env, napi_value value,
                                                  const napi_type_tag* type_tag, bool* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, value);
    NAPI_CHECK_ARG(env, type_tag);
    NAPI_CHECK_ARG(env, result);
    JSC::JSObject* object = toJSValue(value).getObject();
    NAPI_RETURN_EARLY_IF_FALSE(env, object, napi_object_expected);
    const napi_type_tag* tag = mbun::napi_rt::getTypeTagIfExists(env->vm(), object);
    *result = tag != nullptr && tag->lower == type_tag->lower && tag->upper == type_tag->upper;
    NAPI_RETURN_SUCCESS(env);
}

// ── napi_get_all_property_names ─────────────────────────────────────────────
// port: napi.cpp napi_get_all_property_names / v8's KeyCollector. Walks the
// prototype chain when asked, applies the attribute filters against each own
// property's descriptor, and de-duplicates by key (a shadowed prototype key is
// reported once, from the most-derived object).
extern "C" napi_status napi_get_all_property_names(napi_env env, napi_value object,
                                                   napi_key_collection_mode key_mode,
                                                   napi_key_filter key_filter,
                                                   napi_key_conversion key_conversion,
                                                   napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object);
    NAPI_CHECK_ARG(env, result);
    NAPI_RETURN_EARLY_IF_FALSE(
        env, key_mode == napi_key_include_prototypes || key_mode == napi_key_own_only,
        napi_invalid_arg);
    NAPI_RETURN_EARLY_IF_FALSE(
        env, key_conversion == napi_key_keep_numbers || key_conversion == napi_key_numbers_to_strings,
        napi_invalid_arg);

    JSC::JSGlobalObject* global = env->globalObject();
    JSC::VM& vm = env->vm();
    JSC::JSObject* start = toJSValue(object).getObject();
    NAPI_RETURN_EARLY_IF_FALSE(env, start, napi_object_expected);

    const bool skipStrings = (key_filter & napi_key_skip_strings) != 0;
    const bool skipSymbols = (key_filter & napi_key_skip_symbols) != 0;
    const bool needWritable = (key_filter & napi_key_writable) != 0;
    const bool needEnumerable = (key_filter & napi_key_enumerable) != 0;
    const bool needConfigurable = (key_filter & napi_key_configurable) != 0;

    JSC::JSArray* out = JSC::constructEmptyArray(global, nullptr);
    NAPI_RETURN_IF_EXCEPTION(env);

    JSC::MarkedArgumentBuffer seen;
    auto alreadySeen = [&](JSC::JSValue key) {
        for (unsigned i = 0; i < seen.size(); i++) {
            if (JSC::sameValue(global, seen.at(i), key)) {
                return true;
            }
        }
        return false;
    };

    unsigned index = 0;
    for (JSC::JSObject* current = start; current != nullptr;) {
        JSC::PropertyNameArrayBuilder names(vm, JSC::PropertyNameMode::StringsAndSymbols,
                                            JSC::PrivateSymbolMode::Exclude);
        current->methodTable()->getOwnPropertyNames(current, global, names,
                                                    JSC::DontEnumPropertiesMode::Include);
        NAPI_RETURN_IF_EXCEPTION(env);

        for (const JSC::Identifier& name : names) {
            const bool isSymbol = name.isSymbol();
            if ((isSymbol && skipSymbols) || (!isSymbol && skipStrings)) {
                continue;
            }
            if (needWritable || needEnumerable || needConfigurable) {
                JSC::PropertyDescriptor descriptor;
                const bool found =
                    current->getOwnPropertyDescriptor(global, name.impl(), descriptor);
                NAPI_RETURN_IF_EXCEPTION(env);
                if (!found) {
                    continue;
                }
                // Accessors have no [[Writable]]; v8 treats them as writable
                // when a setter exists, which is what node's tests observe.
                const bool writable =
                    descriptor.isAccessorDescriptor() ? descriptor.setter().isObject()
                                                      : descriptor.writable();
                if (needWritable && !writable) {
                    continue;
                }
                if (needEnumerable && !descriptor.enumerable()) {
                    continue;
                }
                if (needConfigurable && !descriptor.configurable()) {
                    continue;
                }
            }
            JSC::JSValue key = JSC::identifierToJSValue(vm, name);
            // napi_key_keep_numbers hands array indices back as numbers.
            if (!isSymbol && key_conversion == napi_key_keep_numbers) {
                if (auto maybeIndex = JSC::parseIndex(*name.impl())) {
                    key = JSC::jsNumber(*maybeIndex);
                }
            }
            if (alreadySeen(key)) {
                continue;
            }
            seen.append(key);
            out->putDirectIndex(global, index++, key);
            NAPI_RETURN_IF_EXCEPTION(env);
        }

        if (key_mode == napi_key_own_only) {
            break;
        }
        JSC::JSValue proto = current->getPrototype(global);
        NAPI_RETURN_IF_EXCEPTION(env);
        current = proto.isObject() ? proto.getObject() : nullptr;
    }

    *result = toNapiValue(JSC::JSValue(out));
    NAPI_RETURN_SUCCESS(env);
}
