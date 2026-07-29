// CAP-NAPI objects: property ops, native functions/classes, wraps, externals,
// calls, and the .node module loader (dlopen + registration). Included by
// runtime.cppm OUTSIDE the anonymous namespace (extern "C" ABI must stay
// externally visible). Blueprints: napi.cpp / NapiClass.cpp /
// BunProcess.cpp:Process_functionDlopen (see runtime/napi/mbun_napi.h).

using mbun::napi_rt::NapiCallbackData;
using mbun::napi_rt::NapiExternal;
using mbun::napi_rt::NapiRef;

// ── property machinery ──────────────────────────────────────────────────────

namespace mbun::napi_rt {

static JSC::Identifier identifierFromUtf8(JSC::VM& vm, const char* utf8) {
    return JSC::Identifier::fromString(
        vm, WTF::String::fromUTF8ReplacingInvalidSequences(
                std::span{reinterpret_cast<const char8_t*>(utf8), strlen(utf8)}));
}

// port: napi.cpp getPropertyAttributes
static uint32_t getPropertyAttributes(const napi_property_descriptor& prop) {
    uint32_t result = 0;
    const uint32_t attributes = static_cast<uint32_t>(prop.attributes);
    if (!(attributes & static_cast<uint32_t>(napi_configurable))) {
        result |= JSC::PropertyAttribute::DontDelete;
    }
    if (!(attributes & static_cast<uint32_t>(napi_enumerable))) {
        result |= JSC::PropertyAttribute::DontEnum;
    }
    if (!((attributes & static_cast<uint32_t>(napi_writable)) || prop.setter != nullptr)) {
        result |= JSC::PropertyAttribute::ReadOnly;
    }
    return result;
}

// Locate the NapiCallbackData attached to the callee (or a prototype-chain
// ancestor, for `class D extends NapiClass` construct calls) — the moral
// equivalent of NapiClass.cpp's dynamicDowncast walk.
static NapiCallbackData* callbackDataFor(JSC::VM& vm, JSC::JSObject* callee) {
    while (callee != nullptr) {
        JSC::JSValue dataValue = callee->getDirect(vm, callbackDataIdent());
        if (dataValue) {
            if (auto* external = dynamicDowncast<NapiExternal>(dataValue)) {
                return static_cast<NapiCallbackData*>(external->value());
            }
        }
        callee = callee->getPrototypeDirect().getObject();
    }
    return nullptr;
}

// port: NapiClass.cpp NapiClass_ConstructorFunction<ConstructCall>
template<bool ConstructCall>
static JSC::EncodedJSValue JSC_HOST_CALL_ATTRIBUTES napiFunctionTrampoline(
    JSC::JSGlobalObject* globalObject, JSC::CallFrame* callFrame) {
    JSC::VM& vm = JSC::getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);

    NapiCallbackData* data = callbackDataFor(vm, JSC::asObject(callFrame->jsCallee()));
    if (!data) [[unlikely]] {
        JSC::throwVMError(globalObject, scope,
                          JSC::createTypeError(globalObject,
                                               "NAPI function is missing its callback data"_s));
        return JSC::JSValue::encode(JSC::jsUndefined());
    }

    JSC::JSValue newTarget;
    if constexpr (ConstructCall) {
        newTarget = callFrame->newTarget();
        JSC::JSObject* newTargetObj =
            newTarget.isObject() ? JSC::asObject(newTarget) : JSC::asObject(callFrame->jsCallee());
        // instance prototype comes from newTarget.prototype (ObjectConstructor
        // subclass protocol; replaces bun's NapiPrototype::subclass)
        JSC::Structure* structure = JSC::InternalFunction::createSubclassStructure(
            globalObject, newTargetObj, globalObject->objectStructureForObjectConstructor());
        RETURN_IF_EXCEPTION(scope, {});
        JSC::JSObject* thisObject = JSC::constructEmptyObject(vm, structure);
        callFrame->setThisValue(thisObject);
    }

    NAPICallFrame frame(globalObject, callFrame, data->data, newTarget);
    ScopedHandleScope handleScope;

    JSC::JSValue ret = toJSValue(data->cb(data->env, frame.toNapi()));
    setLastError(data->env, napi_ok);
    if (data->env->throwPendingException()) {
        return {};
    }
    RETURN_IF_EXCEPTION(scope, {});
    if (ret.isEmpty()) {
        ret = JSC::jsUndefined();
    }
    if constexpr (ConstructCall) {
        RELEASE_AND_RETURN(scope, JSC::JSValue::encode(frame.thisValue()));
    } else {
        RELEASE_AND_RETURN(scope, JSC::JSValue::encode(ret));
    }
}

// port: NapiClass::create — a host function carrying {env, cb, data} behind a
// private-name external (constructible, like every bun NapiClass)
static JSC::JSFunction* makeNapiFunction(napi_env env, const WTF::String& name, napi_callback cb,
                                         void* data) {
    JSC::JSGlobalObject* globalObject = env->globalObject();
    JSC::VM& vm = env->vm();
    JSC::JSFunction* function = JSC::JSFunction::create(
        vm, globalObject, 0, name, napiFunctionTrampoline<false>,
        JSC::ImplementationVisibility::Public, JSC::NoIntrinsic, napiFunctionTrampoline<true>);
    auto* payload = new NapiCallbackData{env, cb, data};
    auto* external = NapiExternal::create(
        vm, externalStructure(globalObject), payload, nullptr,
        [](napi_env, void* d, void*) { delete static_cast<NapiCallbackData*>(d); }, env);
    function->putDirect(vm, callbackDataIdent(), external,
                        JSC::PropertyAttribute::DontEnum | JSC::PropertyAttribute::DontDelete |
                            JSC::PropertyAttribute::ReadOnly);
    return function;
}

// port: napi.cpp Napi::defineProperty
static void defineNapiProperty(napi_env env, JSC::JSObject* to,
                               const napi_property_descriptor& property, bool /*isInstance*/,
                               JSC::ThrowScope& scope) {
    JSC::JSGlobalObject* globalObject = env->globalObject();
    JSC::VM& vm = env->vm();
    void* dataPtr = property.data;

    auto getPropertyName = [&]() -> JSC::Identifier {
        if (property.utf8name != nullptr) {
            size_t len = strlen(property.utf8name);
            if (len > 0) {
                return JSC::Identifier::fromString(
                    vm, WTF::String::fromUTF8({property.utf8name, len}).isolatedCopy());
            }
        }
        if (!property.name) {
            JSC::throwVMError(globalObject, scope,
                              JSC::createTypeError(globalObject, "Property name is required"_s));
            return JSC::Identifier();
        }
        return toJSValue(property.name).toPropertyKey(globalObject);
    };

    JSC::Identifier propertyName = getPropertyName();
    if (!propertyName.isSymbol() && propertyName.isEmpty()) {
        return;
    }

    if (property.method) {
        WTF::String name;
        if (!propertyName.isSymbol()) {
            name = propertyName.string();
        }
        JSC::JSValue value = makeNapiFunction(env, name, property.method, dataPtr);
        to->putDirect(vm, propertyName, value, getPropertyAttributes(property));
        return;
    }

    if (property.getter != nullptr || property.setter != nullptr) {
        JSC::JSObject* getter = nullptr;
        JSC::JSObject* setter = nullptr;
        if (property.getter) {
            auto name = WTF::makeString("get "_s,
                                        propertyName.isSymbol() ? WTF::String() : propertyName.string());
            getter = makeNapiFunction(env, name, property.getter, dataPtr);
        } else {
            getter = JSC::JSNativeStdFunction::create(
                vm, globalObject, 0, WTF::String(),
                [](JSC::JSGlobalObject*, JSC::CallFrame*) -> JSC::EncodedJSValue {
                    return JSC::JSValue::encode(JSC::jsUndefined());
                });
        }
        if (property.setter) {
            auto name = WTF::makeString("set "_s,
                                        propertyName.isSymbol() ? WTF::String() : propertyName.string());
            setter = makeNapiFunction(env, name, property.setter, dataPtr);
        }
        auto* getterSetter = JSC::GetterSetter::create(vm, globalObject, getter, setter);
        to->putDirectAccessor(globalObject, propertyName, getterSetter,
                              JSC::PropertyAttribute::Accessor | getPropertyAttributes(property));
        return;
    }

    JSC::JSValue value = toJSValue(property.value);
    if (value.isEmpty()) {
        value = JSC::jsUndefined();
    }
    JSC::PropertyDescriptor descriptor(value, getPropertyAttributes(property));
    to->methodTable()->defineOwnProperty(to, globalObject, propertyName, descriptor, false);
}

}  // namespace mbun::napi_rt

// ── property access ─────────────────────────────────────────────────────────

extern "C" napi_status napi_set_property(napi_env env, napi_value target, napi_value key,
                                         napi_value value) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, target);
    NAPI_CHECK_ARG(env, key);
    NAPI_CHECK_ARG(env, value);
    JSC::JSGlobalObject* globalObject = env->globalObject();
    JSC::JSObject* object = toJSValue(target).toObject(globalObject);
    NAPI_RETURN_IF_EXCEPTION(env);
    JSC::Identifier identifier = toJSValue(key).toPropertyKey(globalObject);
    NAPI_RETURN_IF_EXCEPTION(env);
    JSC::PutPropertySlot slot(object, false);
    object->methodTable()->put(object, globalObject, identifier, toJSValue(value), slot);
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_get_property(napi_env env, napi_value object, napi_value key,
                                         napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object);
    NAPI_CHECK_ARG(env, key);
    NAPI_CHECK_ARG(env, result);
    JSC::JSGlobalObject* globalObject = env->globalObject();
    JSC::JSObject* target = toJSValue(object).toObject(globalObject);
    NAPI_RETURN_IF_EXCEPTION(env);
    JSC::Identifier identifier = toJSValue(key).toPropertyKey(globalObject);
    NAPI_RETURN_IF_EXCEPTION(env);
    *result = toNapiValue(target->get(globalObject, identifier));
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_has_property(napi_env env, napi_value object, napi_value key,
                                         bool* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object);
    NAPI_CHECK_ARG(env, key);
    NAPI_CHECK_ARG(env, result);
    JSC::JSGlobalObject* globalObject = env->globalObject();
    JSC::JSObject* target = toJSValue(object).toObject(globalObject);
    NAPI_RETURN_IF_EXCEPTION(env);
    JSC::Identifier identifier = toJSValue(key).toPropertyKey(globalObject);
    NAPI_RETURN_IF_EXCEPTION(env);
    *result = target->hasProperty(globalObject, identifier);
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_delete_property(napi_env env, napi_value object, napi_value key,
                                            bool* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object);
    NAPI_CHECK_ARG(env, key);
    JSC::JSGlobalObject* globalObject = env->globalObject();
    JSC::JSObject* target = toJSValue(object).toObject(globalObject);
    NAPI_RETURN_IF_EXCEPTION(env);
    JSC::Identifier identifier = toJSValue(key).toPropertyKey(globalObject);
    NAPI_RETURN_IF_EXCEPTION(env);
    bool deleted = JSC::JSCell::deleteProperty(target, globalObject, identifier);
    NAPI_RETURN_IF_EXCEPTION(env);
    if (result) {
        *result = deleted;
    }
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_has_own_property(napi_env env, napi_value object, napi_value key,
                                             bool* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object);
    NAPI_CHECK_ARG(env, key);
    NAPI_CHECK_ARG(env, result);
    JSC::JSGlobalObject* globalObject = env->globalObject();
    JSC::JSValue jsValue = toJSValue(object);
    NAPI_RETURN_EARLY_IF_FALSE(env, jsValue.isObject(), napi_object_expected);
    JSC::JSValue keyValue = toJSValue(key);
    NAPI_RETURN_EARLY_IF_FALSE(env, keyValue.isString() || keyValue.isSymbol(),
                               napi_name_expected);
    JSC::Identifier identifier = keyValue.toPropertyKey(globalObject);
    NAPI_RETURN_IF_EXCEPTION(env);
    *result = jsValue.getObject()->hasOwnProperty(globalObject, identifier);
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_set_named_property(napi_env env, napi_value object,
                                               const char* utf8Name, napi_value value) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object);
    NAPI_CHECK_ARG(env, utf8Name);
    NAPI_CHECK_ARG(env, value);
    JSC::JSGlobalObject* globalObject = env->globalObject();
    JSC::JSObject* target = toJSValue(object).toObject(globalObject);
    NAPI_RETURN_IF_EXCEPTION(env);
    JSC::Identifier identifier = mbun::napi_rt::identifierFromUtf8(env->vm(), utf8Name);
    JSC::PutPropertySlot slot(target, false);
    target->methodTable()->put(target, globalObject, identifier, toJSValue(value), slot);
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_get_named_property(napi_env env, napi_value object,
                                               const char* utf8Name, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object);
    NAPI_CHECK_ARG(env, utf8Name);
    NAPI_CHECK_ARG(env, result);
    JSC::JSGlobalObject* globalObject = env->globalObject();
    JSC::JSObject* target = toJSValue(object).toObject(globalObject);
    NAPI_RETURN_IF_EXCEPTION(env);
    JSC::Identifier identifier = mbun::napi_rt::identifierFromUtf8(env->vm(), utf8Name);
    *result = toNapiValue(target->get(globalObject, identifier));
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_has_named_property(napi_env env, napi_value object,
                                               const char* utf8Name, bool* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object);
    NAPI_CHECK_ARG(env, utf8Name);
    NAPI_CHECK_ARG(env, result);
    JSC::JSGlobalObject* globalObject = env->globalObject();
    JSC::JSObject* target = toJSValue(object).toObject(globalObject);
    NAPI_RETURN_IF_EXCEPTION(env);
    JSC::Identifier identifier = mbun::napi_rt::identifierFromUtf8(env->vm(), utf8Name);
    JSC::PropertySlot slot(target, JSC::PropertySlot::InternalMethodType::HasProperty);
    *result = target->getPropertySlot(globalObject, identifier, slot);
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_set_element(napi_env env, napi_value object, uint32_t index,
                                        napi_value value) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object);
    NAPI_CHECK_ARG(env, value);
    JSC::JSValue jsValue = toJSValue(object);
    NAPI_RETURN_EARLY_IF_FALSE(env, jsValue.isObject(), napi_object_expected);
    jsValue.getObject()->methodTable()->putByIndex(jsValue.getObject(), env->globalObject(), index,
                                                   toJSValue(value), false);
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_get_element(napi_env env, napi_value object, uint32_t index,
                                        napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue jsValue = toJSValue(object);
    NAPI_RETURN_EARLY_IF_FALSE(env, jsValue.isObject(), napi_object_expected);
    *result = toNapiValue(jsValue.getObject()->getIndex(env->globalObject(), index));
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_has_element(napi_env env, napi_value object, uint32_t index,
                                        bool* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue jsValue = toJSValue(object);
    NAPI_RETURN_EARLY_IF_FALSE(env, jsValue.isObject(), napi_object_expected);
    *result = jsValue.getObject()->hasProperty(env->globalObject(), index);
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_delete_element(napi_env env, napi_value object, uint32_t index,
                                           bool* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object);
    JSC::JSValue jsValue = toJSValue(object);
    NAPI_RETURN_EARLY_IF_FALSE(env, jsValue.isObject(), napi_object_expected);
    bool deleted = jsValue.getObject()->methodTable()->deletePropertyByIndex(
        jsValue.getObject(), env->globalObject(), index);
    NAPI_RETURN_IF_EXCEPTION(env);
    if (result) {
        *result = deleted;
    }
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_get_property_names(napi_env env, napi_value object,
                                               napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue jsValue = toJSValue(object);
    JSC::JSObject* jsObject = jsValue.getObject();
    NAPI_RETURN_EARLY_IF_FALSE(env, jsObject, napi_object_expected);
    JSC::JSValue keys =
        JSC::allPropertyKeys(env->globalObject(), jsObject, JSC::PropertyNameMode::Strings,
                             JSC::DontEnumPropertiesMode::Exclude);
    NAPI_RETURN_IF_EXCEPTION(env);
    *result = toNapiValue(keys);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_define_properties(napi_env env, napi_value object,
                                              size_t property_count,
                                              const napi_property_descriptor* properties) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    auto throwScope = DECLARE_THROW_SCOPE(env->vm());
    NAPI_RETURN_IF_EXCEPTION_WITH_SCOPE(env, throwScope);
    NAPI_CHECK_ARG(env, object);
    NAPI_RETURN_EARLY_IF_FALSE(env, properties || property_count == 0, napi_invalid_arg);
    JSC::JSObject* objectObject = toJSValue(object).getObject();
    NAPI_RETURN_EARLY_IF_FALSE(env, objectObject, napi_object_expected);
    for (size_t i = 0; i < property_count; i++) {
        mbun::napi_rt::defineNapiProperty(env, objectObject, properties[i], true, throwScope);
        RETURN_IF_EXCEPTION(throwScope, setLastError(env, napi_pending_exception));
    }
    throwScope.release();
    return setLastError(env, napi_ok);
}

extern "C" napi_status napi_object_freeze(napi_env env, napi_value object_value) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object_value);
    JSC::JSValue jsValue = toJSValue(object_value);
    NAPI_RETURN_EARLY_IF_FALSE(env, jsValue.isObject(), napi_object_expected);
    JSC::objectConstructorFreeze(env->globalObject(), jsValue.getObject());
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_object_seal(napi_env env, napi_value object_value) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, object_value);
    JSC::JSValue jsValue = toJSValue(object_value);
    NAPI_RETURN_EARLY_IF_FALSE(env, jsValue.isObject(), napi_object_expected);
    JSC::objectConstructorSeal(env->globalObject(), jsValue.getObject());
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

// ── functions / callback info / classes ─────────────────────────────────────

extern "C" napi_status napi_create_function(napi_env env, const char* utf8name, size_t length,
                                            napi_callback cb, void* data, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    NAPI_CHECK_ARG(env, cb);
    WTF::String name;
    if (utf8name != nullptr) {
        size_t len = length == NAPI_AUTO_LENGTH ? strlen(utf8name) : length;
        name = WTF::String::fromUTF8({utf8name, len});
    }
    JSC::JSFunction* function = mbun::napi_rt::makeNapiFunction(env, name, cb, data);
    *result = toNapiValue(function);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_get_cb_info(napi_env env, napi_callback_info cbinfo, size_t* argc,
                                        napi_value* argv, napi_value* this_arg, void** data) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, cbinfo);
    auto* frame = reinterpret_cast<mbun::napi_rt::NAPICallFrame*>(cbinfo);
    frame->extract(argc, argv, this_arg, data);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_get_new_target(napi_env env, napi_callback_info cbinfo,
                                           napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, cbinfo);
    NAPI_CHECK_ARG(env, result);
    auto* frame = reinterpret_cast<mbun::napi_rt::NAPICallFrame*>(cbinfo);
    JSC::JSValue newTarget = frame->newTarget();
    *result = newTarget.isEmpty() ? nullptr : toNapiValue(newTarget);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_define_class(napi_env env, const char* utf8name, size_t length,
                                         napi_callback constructor, void* data,
                                         size_t property_count,
                                         const napi_property_descriptor* properties,
                                         napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    NAPI_CHECK_ARG(env, utf8name);
    NAPI_CHECK_ARG(env, constructor);
    NAPI_RETURN_EARLY_IF_FALSE(env, properties || property_count == 0, napi_invalid_arg);

    JSC::JSGlobalObject* globalObject = env->globalObject();
    JSC::VM& vm = env->vm();
    size_t len = length == NAPI_AUTO_LENGTH ? strlen(utf8name) : length;
    WTF::String name = WTF::String::fromUTF8({utf8name, len}).isolatedCopy();

    JSC::JSFunction* napiClass = mbun::napi_rt::makeNapiFunction(env, name, constructor, data);
    // port: NapiClass::finishCreation — split static/instance descriptors
    JSC::JSObject* prototype = JSC::constructEmptyObject(globalObject);
    auto throwScope = DECLARE_THROW_SCOPE(vm);
    for (size_t i = 0; i < property_count; i++) {
        const napi_property_descriptor& property = properties[i];
        if (property.attributes & napi_static) {
            mbun::napi_rt::defineNapiProperty(env, napiClass, property, true, throwScope);
        } else {
            mbun::napi_rt::defineNapiProperty(env, prototype, property, false, throwScope);
        }
        RETURN_IF_EXCEPTION(throwScope, setLastError(env, napi_pending_exception));
    }
    napiClass->putDirect(vm, vm.propertyNames->prototype, prototype,
                         JSC::PropertyAttribute::DontEnum | 0);
    prototype->putDirect(vm, vm.propertyNames->constructor, napiClass,
                         JSC::PropertyAttribute::DontEnum | 0);

    *result = toNapiValue(napiClass);
    NAPI_RETURN_SUCCESS(env);
}

// ── externals / wrap ────────────────────────────────────────────────────────

extern "C" napi_status napi_create_external(napi_env env, void* data, napi_finalize finalize_cb,
                                            void* finalize_hint, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue value = NapiExternal::create(env->vm(),
                                              mbun::napi_rt::externalStructure(env->globalObject()),
                                              data, finalize_hint, finalize_cb, env);
    *result = toNapiValue(value);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_get_value_external(napi_env env, napi_value value, void** result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, result);
    NAPI_CHECK_ARG(env, value);
    auto* external = dynamicDowncast<NapiExternal>(toJSValue(value));
    NAPI_RETURN_EARLY_IF_FALSE(env, external, napi_invalid_arg);
    *result = external->value();
    NAPI_RETURN_SUCCESS(env);
}

namespace mbun::napi_rt {

// port: napi.cpp wrap_cleanup — env-exit fallback when the weak ref never fired
static void wrapCleanup(napi_env /*env*/, void* data, void* /*hint*/) {
    auto* ref = static_cast<NapiRef*>(data);
    ref->callFinalizer();  // no-op if the finalizer already ran (it self-clears)
}

// port: napi.cpp getWrapContentsIfExists (private-name branch only; this
// runtime has no NapiPrototype inline slot)
static NapiRef* getWrapContentsIfExists(JSC::VM& vm, JSC::JSObject* object) {
    JSC::JSValue contents = object->getDirect(vm, wrapContentsIdent());
    if (!contents) {
        return nullptr;
    }
    auto* external = dynamicDowncast<NapiExternal>(contents);
    return external ? static_cast<NapiRef*>(external->value()) : nullptr;
}

}  // namespace mbun::napi_rt

extern "C" napi_status napi_wrap(napi_env env, napi_value js_object, void* native_object,
                                 napi_finalize finalize_cb, void* finalize_hint,
                                 napi_ref* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, js_object);
    JSC::VM& vm = env->vm();
    JSC::JSValue jscValue = toJSValue(js_object);
    JSC::JSObject* jscObject = jscValue.getObject();
    NAPI_RETURN_EARLY_IF_FALSE(env, jscObject, napi_object_expected);

    NAPI_RETURN_EARLY_IF_FALSE(env,
                               mbun::napi_rt::getWrapContentsIfExists(vm, jscObject) == nullptr,
                               napi_invalid_arg);

    auto* ref = new NapiRef(env, 0, mbun::napi_rt::NapiFinalizer{finalize_cb, finalize_hint});
    ref->nativeObject = native_object;
    // env-exit fallback finalizer (removed again in ~NapiRef via wrapCleanup's
    // self-clearing call semantics)
    env->addFinalizer(mbun::napi_rt::wrapCleanup, finalize_hint, ref);

    auto* external = NapiExternal::create(vm, mbun::napi_rt::externalStructure(env->globalObject()),
                                          ref, nullptr, nullptr, env);
    jscObject->putDirect(vm, mbun::napi_rt::wrapContentsIdent(), JSC::JSValue(external));

    // no result → the ref cleans itself up when the object dies (bun:
    // NapiRefSelfDeletingWeakHandleOwner)
    ref->selfDeleting = (result == nullptr);
    ref->setValueInitial(jscValue, true);
    if (result) {
        *result = reinterpret_cast<napi_ref>(ref);
    }
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_unwrap(napi_env env, napi_value js_object, void** result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, js_object);
    NAPI_CHECK_ARG(env, result);
    JSC::JSObject* jscObject = toJSValue(js_object).getObject();
    NAPI_RETURN_EARLY_IF_FALSE(env, jscObject, napi_object_expected);
    NapiRef* ref = mbun::napi_rt::getWrapContentsIfExists(env->vm(), jscObject);
    NAPI_RETURN_EARLY_IF_FALSE(env, ref, napi_invalid_arg);
    *result = ref->nativeObject;
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_remove_wrap(napi_env env, napi_value js_object, void** result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, js_object);
    JSC::JSObject* jscObject = toJSValue(js_object).getObject();
    NAPI_RETURN_EARLY_IF_FALSE(env, jscObject, napi_object_expected);
    JSC::VM& vm = env->vm();
    NapiRef* ref = mbun::napi_rt::getWrapContentsIfExists(vm, jscObject);
    NAPI_RETURN_EARLY_IF_FALSE(env, ref, napi_invalid_arg);
    JSC::JSCell::deleteProperty(jscObject, env->globalObject(),
                                mbun::napi_rt::wrapContentsIdent());
    if (result) {
        *result = ref->nativeObject;
    }
    ref->finalizer.clear();
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_add_finalizer(napi_env env, napi_value js_object, void* native_object,
                                          napi_finalize finalize_cb, void* finalize_hint,
                                          napi_ref* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, js_object);
    NAPI_CHECK_ARG(env, finalize_cb);
    JSC::JSObject* object = toJSValue(js_object).getObject();
    NAPI_RETURN_EARLY_IF_FALSE(env, object, napi_object_expected);
    if (result) {
        auto* ref = new NapiRef(env, 0, mbun::napi_rt::NapiFinalizer{finalize_cb, finalize_hint});
        ref->setValueInitial(object, true);
        ref->nativeObject = native_object;
        *result = reinterpret_cast<napi_ref>(ref);
    } else {
        env->vm().heap.addFinalizer(object,
                                    [env, finalize_cb, native_object, finalize_hint](JSC::JSCell*) {
                                        mbun::napi_rt::NapiFinalizer{finalize_cb, finalize_hint}
                                            .call(env, native_object, /*immediate=*/false);
                                    });
    }
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status node_api_post_finalizer(napi_env env, napi_finalize finalize_cb,
                                               void* finalize_data, void* finalize_hint) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, finalize_cb);
    mbun::napi_rt::NapiState::singleton().pendingFinalizers.push_back(
        {env, finalize_cb, finalize_data, finalize_hint});
    NAPI_RETURN_SUCCESS(env);
}

// ── calls ───────────────────────────────────────────────────────────────────

extern "C" napi_status napi_call_function(napi_env env, napi_value recv, napi_value func,
                                          size_t argc, const napi_value* argv,
                                          napi_value* result) {
    if (env && env->throwPendingException()) {
        return setLastError(env, napi_pending_exception);
    }
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, recv);
    NAPI_RETURN_EARLY_IF_FALSE(env, argc == 0 || argv, napi_invalid_arg);
    NAPI_CHECK_ARG(env, func);
    JSC::JSValue funcValue = toJSValue(func);
    NAPI_RETURN_EARLY_IF_FALSE(env, funcValue.isCallable(), napi_invalid_arg);

    JSC::JSGlobalObject* globalObject = env->globalObject();
    JSC::MarkedArgumentBuffer args;
    for (size_t i = 0; i < argc; i++) {
        args.append(toJSValue(argv[i]));
    }
    JSC::JSValue thisValue = toJSValue(recv);
    if (thisValue.isEmpty()) {
        thisValue = JSC::jsUndefined();
    }
    auto callData = JSC::getCallData(funcValue);
    JSC::JSValue res = JSC::call(globalObject, funcValue, callData, thisValue, args);
    if (result) {
        *result = toNapiValue(res.isEmpty() ? JSC::jsUndefined() : res);
    }
    NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(env);
}

extern "C" napi_status napi_new_instance(napi_env env, napi_value constructor, size_t argc,
                                         const napi_value* argv, napi_value* result) {
    NAPI_PREAMBLE(env);
    NAPI_CHECK_ARG(env, constructor);
    NAPI_RETURN_EARLY_IF_FALSE(env, argc == 0 || argv, napi_invalid_arg);
    NAPI_CHECK_ARG(env, result);
    JSC::JSValue constructorValue = toJSValue(constructor);
    NAPI_RETURN_EARLY_IF_FALSE(env, constructorValue.isConstructor(), napi_function_expected);

    JSC::JSGlobalObject* globalObject = env->globalObject();
    JSC::MarkedArgumentBuffer args;
    for (size_t i = 0; i < argc; i++) {
        args.append(toJSValue(argv[i]));
    }
    JSC::JSObject* res = JSC::construct(globalObject, constructorValue, args, "constructor"_s);
    NAPI_RETURN_IF_EXCEPTION(env);
    *result = toNapiValue(res);
    NAPI_RETURN_SUCCESS(env);
}

extern "C" napi_status napi_instanceof(napi_env env, napi_value object, napi_value constructor,
                                       bool* result) {
    NAPI_PREAMBLE_NO_THROW_SCOPE(env);
    NAPI_CHECK_ARG(env, result);
    JSC::JSGlobalObject* globalObject = env->globalObject();
    JSC::JSValue objectValue = toJSValue(object);
    JSC::JSValue constructorValue = toJSValue(constructor);
    JSC::JSObject* constructorObject = constructorValue.getObject();
    auto scope = DECLARE_THROW_SCOPE(env->vm());
    if (!constructorObject || !constructorValue.isConstructor()) {
        JSC::throwVMError(globalObject, scope,
                          JSC::createTypeError(globalObject, "Constructor must be a function"_s));
        return setLastError(env, napi_pending_exception);
    }
    if (!constructorObject->structure()->typeInfo().implementsHasInstance()) [[unlikely]] {
        *result = false;
    } else {
        *result = constructorObject->hasInstance(globalObject, objectValue);
        RETURN_IF_EXCEPTION(scope, setLastError(env, napi_pending_exception));
    }
    return setLastError(env, napi_ok);
}

// ── module registration + loader ────────────────────────────────────────────

// port: napi.cpp:808 napi_module_register — legacy NAPI_MODULE static ctors
// fire during dlopen; registrations accumulate here.
extern "C" void napi_module_register(napi_module* mod) {
    auto& state = mbun::napi_rt::NapiState::singleton();
    state.moduleRegisterCallCount++;
    if (mod && mod->nm_register_func) {
        state.pendingModules.push_back(*mod);
    }
}

namespace mbun::napi_rt {

static napi_env makeNapiEnv(JSC::JSGlobalObject* globalObject, int nmVersion,
                            const std::string& modname, const std::string& path) {
    auto& state = NapiState::singleton();
    auto env = std::make_unique<NapiEnv>(globalObject, nmVersion, modname);
    env->filename = "file://" + path;
    napi_env raw = env.get();
    state.envs.push_back(std::move(env));
    return raw;
}

// run one legacy registration; returns the module's chosen exports.
// Node semantics (node_api.cc napi_module_register_by_symbol): init receives
// EXPORTS; a non-null return replaces it.
static JSC::JSValue executeRegisteredModule(JSC::JSGlobalObject* globalObject,
                                            const napi_module& mod, JSC::JSObject* exports,
                                            const std::string& path,
                                            JSC::ThrowScope& scope) {
    napi_env env = makeNapiEnv(globalObject, mod.nm_version,
                               mod.nm_modname ? mod.nm_modname : "[no modname]", path);
    ScopedHandleScope handleScope;
    JSC::JSValue result = toJSValue(mod.nm_register_func(env, toNapiValue(exports)));
    if (env->throwPendingException()) {
        return {};
    }
    RETURN_IF_EXCEPTION(scope, {});
    if (!result || result.isEmpty()) {
        return exports;
    }
    return result;
}

// port: BunProcess.cpp Process_functionDlopen (POSIX branch), minus the
// embedded-.node / plugin machinery. On failure a JS error is thrown on the
// VM and an empty JSValue returned.
static JSC::JSValue loadNapiModule(JSC::JSGlobalObject* globalObject, const std::string& path,
                                   JSC::JSObject* exports) {
    JSC::VM& vm = JSC::getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& state = NapiState::singleton();

    const std::size_t callCountAtStart = state.moduleRegisterCallCount;
    void* handle = dlopen(path.c_str(), RTLD_LAZY);
    if (!handle) {
        const char* err = dlerror();
        JSC::throwException(
            globalObject, scope,
            JSC::createError(globalObject,
                             WTF::makeString("ERR_DLOPEN_FAILED: "_s,
                                             WTF::String::fromUTF8(err ? err : "dlopen failed"))));
        return {};
    }

    // 1) legacy path: static constructors registered module(s) during dlopen
    if (callCountAtStart != state.moduleRegisterCallCount && !state.pendingModules.empty()) {
        auto pending = std::exchange(state.pendingModules, {});
        state.dlHandleModules[handle] = pending;  // replay cache for re-require
        JSC::JSValue result = exports;
        for (;;) {
            for (const napi_module& mod : pending) {
                result = executeRegisteredModule(globalObject, mod, exports, path, scope);
                RETURN_IF_EXCEPTION(scope, {});
                if (result.isEmpty()) {
                    return {};
                }
            }
            if (state.pendingModules.empty()) {
                break;  // no re-entrant registrations
            }
            pending = std::exchange(state.pendingModules, {});
        }
        return result;
    }

    // 2) re-required legacy module: dlopen returned a cached handle and no
    //    constructor re-ran — replay the saved registrations
    if (auto it = state.dlHandleModules.find(handle); it != state.dlHandleModules.end()) {
        JSC::JSValue result = exports;
        for (const napi_module& mod : it->second) {
            result = executeRegisteredModule(globalObject, mod, exports, path, scope);
            RETURN_IF_EXCEPTION(scope, {});
            if (result.isEmpty()) {
                return {};
            }
        }
        return result;
    }

    // 3) modern path: napi_register_module_v1
    auto register_v1 =
        reinterpret_cast<napi_value (*)(napi_env, napi_value)>(dlsym(handle, "napi_register_module_v1"));
    auto get_api_version =
        reinterpret_cast<int32_t (*)()>(dlsym(handle, "node_api_module_get_api_version_v1"));
    if (!register_v1) {
        dlclose(handle);
        JSC::throwTypeError(
            globalObject, scope,
            "symbol 'napi_register_module_v1' not found in native module. Is this a Node API (napi) module?"_s);
        return {};
    }
    int moduleVersion = 8;
    if (get_api_version) {
        moduleVersion = get_api_version();
    }

    napi_env env = makeNapiEnv(globalObject, moduleVersion, "[no modname]", path);
    ScopedHandleScope handleScope;
    JSC::JSValue result = toJSValue(register_v1(env, toNapiValue(exports)));
    if (env->throwPendingException()) {
        return {};
    }
    RETURN_IF_EXCEPTION(scope, {});
    if (!result || result.isEmpty()) {
        return exports;
    }
    return result;
}

static void cleanupAtExit() {
    auto& state = NapiState::singleton();
    if (state.envs.empty()) {
        return;
    }
    JSC::JSGlobalObject* globalObject = state.envs.front()->globalObject();
    JSC::JSLockHolder locker(JSC::getVM(globalObject));
    drainPendingFinalizers();
    for (auto it = state.envs.rbegin(); it != state.envs.rend(); ++it) {
        (*it)->cleanup();
    }
    drainPendingFinalizers();
}

static void registerExitCleanupOnce() {
    static bool registered = false;
    if (!registered) {
        registered = true;
        std::atexit(cleanupAtExit);
    }
}

}  // namespace mbun::napi_rt

// ── runtime hooks (declared in runtime.cppm ahead of the anonymous
//    namespace; called from engine.inc / process bindings) ──────────────────

// require("./addon.node") → dlopen + register → module exports value.
JSValueRef mbun_napi_require_module(JSContextRef ctx, const std::string& path, JSValueRef* exc) {
    JSC::JSGlobalObject* globalObject = toJS(ctx);
    JSC::VM& vm = JSC::getVM(globalObject);
    JSC::JSLockHolder locker(vm);
    mbun::napi_rt::registerExitCleanupOnce();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    JSC::JSObject* exports = JSC::constructEmptyObject(globalObject);
    JSC::JSValue result = mbun::napi_rt::loadNapiModule(globalObject, path, exports);
    if (scope.exception()) [[unlikely]] {
        if (exc) {
            *exc = toRef(globalObject, scope.exception()->value());
        }
        scope.clearException();
        return JSValueMakeUndefined(ctx);
    }
    if (result.isEmpty()) {
        result = JSC::jsUndefined();
    }
    return toRef(globalObject, result);
}

// process.dlopen(module, filename): loads into module.exports (node semantics).
void mbun_napi_process_dlopen(JSContextRef ctx, JSObjectRef moduleObj,
                              const std::string& filename, JSValueRef* exc) {
    JSC::JSGlobalObject* globalObject = toJS(ctx);
    JSC::VM& vm = JSC::getVM(globalObject);
    JSC::JSLockHolder locker(vm);
    mbun::napi_rt::registerExitCleanupOnce();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);

    JSC::JSObject* module = toJS(moduleObj);
    JSC::JSValue exportsValue =
        module->get(globalObject, JSC::Identifier::fromString(vm, "exports"_s));
    JSC::JSObject* exports =
        exportsValue.isObject() ? exportsValue.getObject() : JSC::constructEmptyObject(globalObject);
    if (scope.exception()) [[unlikely]] {
        if (exc) {
            *exc = toRef(globalObject, scope.exception()->value());
        }
        scope.clearException();
        return;
    }

    JSC::JSValue result = mbun::napi_rt::loadNapiModule(globalObject, filename, exports);
    if (scope.exception()) [[unlikely]] {
        if (exc) {
            *exc = toRef(globalObject, scope.exception()->value());
        }
        scope.clearException();
        return;
    }
    if (!result.isEmpty()) {
        module->putDirect(vm, JSC::Identifier::fromString(vm, "exports"_s), result);
    }
}

// Deferred (GC-time) napi finalizers drain at event-loop pump points.
void mbun_napi_drain_deferred_finalizers() { mbun::napi_rt::drainPendingFinalizers(); }
