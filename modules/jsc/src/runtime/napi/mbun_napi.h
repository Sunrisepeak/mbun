#pragma once
// CAP-NAPI runtime state: napi_env (struct NapiEnv, per the vendored ABI
// headers' `typedef struct NapiEnv* napi_env`) + value cells + handle scopes +
// references.
//
// Mechanical port of bun's Node-API layer onto the JSC internals this runtime
// already links (prelude.hpp). Blueprints, per function (bun-ref = bun source):
//   - src/jsc/bindings/napi.h        — NapiEnv / NapiRef / NapiWeakValue /
//                                      NAPICallFrame / handle-scope contract
//   - src/jsc/bindings/napi.cpp      — the extern "C" bodies + error macros
//   - src/jsc/bindings/napi_external.h / napi_finalizer.h — cells + finalizer
//   - src/jsc/bindings/NapiClass.cpp — class/function trampolines
//   - src/jsc/bindings/BunProcess.cpp Process_functionDlopen — module loading
//
// Everything here is INLINE: this header lives in the global module fragment
// (via prelude.hpp), and GCC rejects global-fragment declarations paired with
// module-purview definitions ("conflicting declaration ... in module").
//
// Deliberate deviations from bun (documented once here, not per line):
//   * bun's NapiClass/NapiPrototype/NapiExternal use WebCore-style custom iso
//     subspaces (BunClientData). This runtime has no client-data layer, so:
//       - NapiExternal lives in vm.destructibleObjectSpace() (same choice bun
//         makes for NapiPrototype), and
//       - napi functions/classes are plain host JSC::JSFunctions whose
//         {env, callback, data} ride a private-name property holding a
//         NapiExternal, instead of a JSFunction subclass. Trampolines find it
//         by walking jsCallee()'s prototype chain exactly like NapiClass.
//   * bun's handle scope is a custom JSCell (napi_handle_scope.cpp); here it
//     is a heap record owning a JSC::MarkedArgumentBuffer (GC-visible via the
//     VM mark-list set), which gives the same "values created in the scope
//     stay alive until it closes" guarantee.
//   * NapiEnv is not refcounted: envs are registered process-wide and torn
//     down (cleanup hooks + finalizers, LIFO) at exit, matching Node's
//     per-Environment teardown order for a single main env.

#include "node_api.h"

// Global-module-fragment header: `import std` is not visible here, so the std
// containers used below come in textually (WTF pulls most of these anyway).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mbun::napi_rt {

using namespace JSC;  // CREATE_METHOD_TABLE / JSCastingHelpers need it (same
                      // pattern as bun's `namespace Zig { using namespace JSC; }`)

// ── finalizer ───────────────────────────────────────────────────────────────
// port: napi_finalizer.h Bun::NapiFinalizer. `call(env, data, immediate)`
// defers to the pending queue when !immediate (bun routes through
// napi_internal_enqueue_finalizer to the event loop; here the queue drains at
// event-loop pump points and env cleanup).
class NapiFinalizer {
public:
    NapiFinalizer(napi_finalize callback, void* hint) : m_callback{callback}, m_hint{hint} {}
    NapiFinalizer() = default;

    inline void call(napi_env env, void* data, bool immediate = false);  // defined below
    void clear() {
        m_callback = nullptr;
        m_hint = nullptr;
    }
    napi_finalize callback() const { return m_callback; }
    void* hint() const { return m_hint; }

private:
    napi_finalize m_callback{nullptr};
    void* m_hint{nullptr};
};

// ── external cell ───────────────────────────────────────────────────────────
// port: napi_external.h Bun::NapiExternal, re-homed into the generic
// destructible-object subspace (no BunClientData iso subspaces here). Serves
// napi_create_external AND (via private-name properties) wrap contents and
// function/class callback payloads.
class NapiExternal final : public JSC::JSDestructibleObject {
public:
    using Base = JSC::JSDestructibleObject;
    static constexpr unsigned StructureFlags = Base::StructureFlags;
    static constexpr JSC::DestructionMode needsDestruction = JSC::NeedsDestruction;

    template<typename CellType, JSC::SubspaceAccess>
    static JSC::CompleteSubspace* subspaceFor(JSC::VM& vm) {
        return &vm.destructibleObjectSpace();
    }

    static const JSC::ClassInfo s_info;
    static constexpr const JSC::ClassInfo* info() { return &s_info; }

    static JSC::Structure* createStructure(JSC::VM& vm, JSC::JSGlobalObject* globalObject,
                                           JSC::JSValue prototype) {
        return JSC::Structure::create(vm, globalObject, prototype,
                                      JSC::TypeInfo(JSC::ObjectType, StructureFlags), info());
    }

    static NapiExternal* create(JSC::VM& vm, JSC::Structure* structure, void* value,
                                void* finalizer_hint, napi_finalize callback, napi_env env) {
        auto* cell = new (NotNull, JSC::allocateCell<NapiExternal>(vm)) NapiExternal(vm, structure);
        cell->finishCreation(vm, value, finalizer_hint, callback, env);
        return cell;
    }

    static inline void destroy(JSC::JSCell* cell);  // defined below (queues finalizer)

    void* value() const { return m_value; }

    void* m_value{nullptr};
    NapiFinalizer m_finalizer{};
    napi_env m_env{nullptr};

private:
    NapiExternal(JSC::VM& vm, JSC::Structure* structure) : Base(vm, structure) {}
    void finishCreation(JSC::VM& vm, void* value, void* finalizer_hint, napi_finalize callback,
                        napi_env env) {
        Base::finishCreation(vm);
        m_value = value;
        m_finalizer = NapiFinalizer{callback, finalizer_hint};
        m_env = env;
    }
};

inline const JSC::ClassInfo NapiExternal::s_info = {
    "External"_s, &NapiExternal::Base::s_info, nullptr, nullptr, CREATE_METHOD_TABLE(NapiExternal)};

// ── handle scopes ───────────────────────────────────────────────────────────
// port of the napi_handle_scope.cpp contract: every napi_value handed to the
// addon while a scope is open is appended to that scope, so heap-held handles
// survive GC until the scope closes. MarkedArgumentBuffer self-registers in
// the VM's mark-list set on first cell append (GC marks its slots), and the
// record never moves, so heap ownership is safe.
struct HandleScopeRec {
    HandleScopeRec* parent{nullptr};
    JSC::MarkedArgumentBuffer values;
    bool escapable{false};
    bool escapeCalled{false};
};

struct PendingFinalizer {
    napi_env env;
    napi_finalize cb;
    void* data;
    void* hint;
};

struct CleanupHook {
    void (*function)(void*){nullptr};
    void* data{nullptr};
    std::size_t insertionCounter{0};
};

// ── weak-reference plumbing ─────────────────────────────────────────────────
// bun implements weak-death callbacks with JSC::WeakHandleOwner subclasses
// (napi.h NapiRefWeakHandleOwner / ...SelfDeleting...). The prebuilt
// JavaScriptCore is -fno-rtti, so subclassing a library class with virtuals
// here (RTTI on) leaves `typeinfo for JSC::WeakHandleOwner` undefined at
// link. Equivalent mechanism without subclassing: an OWNERLESS JSC::Weak for
// the auto-clearing pointer + Heap::addFinalizer(cell, lambda) for the death
// callback (the same facility bun uses for napi_add_finalizer's no-ref path).
class NapiRef;

// Simplified NapiWeakValue: cells ride one JSC::Weak<JSCell>; primitives are
// stored inline (bun splits String/Cell only for typing convenience).
class NapiWeakValue {
public:
    NapiWeakValue() = default;

    void clear() {
        m_weak.clear();
        m_primitive = JSC::JSValue();
        m_set = false;
    }
    bool isSet() const { return m_set; }

    void set(JSC::JSValue value) {
        if (value.isCell()) {
            m_weak = JSC::Weak<JSC::JSCell>(value.asCell());
        } else {
            m_primitive = value;
        }
        m_set = true;
    }

    JSC::JSValue get() const {
        if (!m_set) {
            return {};
        }
        if (m_primitive) {
            return m_primitive;
        }
        return m_weak.get() ? JSC::JSValue(m_weak.get()) : JSC::JSValue();
    }

private:
    JSC::Weak<JSC::JSCell> m_weak;
    JSC::JSValue m_primitive{};
    bool m_set{false};
};

}  // namespace mbun::napi_rt

// ── napi_env ────────────────────────────────────────────────────────────────
// port: napi.h struct NapiEnv (the vendored js_native_api_types.h declares
// `typedef struct NapiEnv* napi_env` — bun's rename, kept verbatim).
struct NapiEnv {
    NapiEnv(JSC::JSGlobalObject* globalObject, int nmVersion, std::string modname)
        : m_globalObject{globalObject},
          m_vm{&JSC::getVM(globalObject)},
          m_nmVersion{nmVersion},
          m_modname{std::move(modname)} {}

    JSC::JSGlobalObject* globalObject() const { return m_globalObject; }
    JSC::VM& vm() const { return *m_vm; }
    int nmVersion() const { return m_nmVersion; }

    bool inGC() const { return m_vm->isCollectorBusyOnCurrentThread(); }
    void checkGC() const { /* only fatal for NAPI_VERSION_EXPERIMENTAL; see bun NapiEnv::checkGC */ }
    bool isVMTerminating() const { return m_vm->hasTerminationRequest(); }

    // Non-experimental modules must not run finalizers inside GC (napi.h
    // mustDeferFinalizers): they get queued and drained on the event loop.
    bool mustDeferFinalizers() const {
        return m_nmVersion != NAPI_VERSION_EXPERIMENTAL && !isVMTerminating();
    }

    // pending "env-scheduled" exception (napi_throw_error & friends park the
    // error here; the next JS re-entry point throws it for real).
    void scheduleException(JSC::JSValue exception) {
        if (exception.isEmpty()) {
            m_pendingException.clear();
            return;
        }
        m_pendingException.set(*m_vm, exception);
    }
    bool hasPendingException() const { return static_cast<bool>(m_pendingException); }
    JSC::JSValue pendingException() const {
        return m_pendingException ? m_pendingException.get() : JSC::JSValue();
    }
    void clearPendingException() { m_pendingException.clear(); }
    bool throwPendingException() {
        if (!m_pendingException) {
            return false;
        }
        auto scope = DECLARE_THROW_SCOPE(*m_vm);
        JSC::throwException(m_globalObject, scope, m_pendingException.get());
        m_pendingException.clear();
        return true;
    }

    void addCleanupHook(void (*function)(void*), void* data) {
        m_cleanupHooks.push_back({function, data, ++m_cleanupHookCounter});
    }
    void removeCleanupHook(void (*function)(void*), void* data) {
        for (auto it = m_cleanupHooks.begin(); it != m_cleanupHooks.end(); ++it) {
            if (it->function == function && it->data == data) {
                m_cleanupHooks.erase(it);
                return;
            }
        }  // absent → silently ignored (node cleanup_queue-inl.h)
    }

    struct BoundFinalizer {
        napi_finalize cb{nullptr};
        void* hint{nullptr};
        void* data{nullptr};
    };

    void addFinalizer(napi_finalize cb, void* hint, void* data) {
        m_finalizers.push_back({cb, hint, data});
    }
    void removeFinalizer(napi_finalize cb, void* hint, void* data) {
        for (auto it = m_finalizers.begin(); it != m_finalizers.end(); ++it) {
            if (it->cb == cb && it->hint == hint && it->data == data) {
                m_finalizers.erase(it);
                return;
            }
        }
    }
    // ~NapiRef uses this: a wrap's bound exit-finalizer holds the ref as data,
    // so it must be dropped when the ref dies first (else exit cleanup UAFs).
    void removeFinalizerByData(void* data) {
        std::erase_if(m_finalizers, [data](const BoundFinalizer& f) { return f.data == data; });
    }

    inline void cleanup();  // defined below: hooks LIFO + finalizers LIFO + instance data

    napi_extended_error_info m_lastNapiErrorInfo{
        .error_message = "",
        .engine_reserved = nullptr,
        .engine_error_code = 0,
        .error_code = napi_ok,
    };

    void* instanceData{nullptr};
    mbun::napi_rt::NapiFinalizer instanceDataFinalizer{};
    std::string filename;  // file:// URI of the addon (node_api_get_module_file_name)

private:
    JSC::JSGlobalObject* m_globalObject{nullptr};
    JSC::VM* m_vm{nullptr};
    int m_nmVersion{8};
    std::string m_modname;
    JSC::Strong<JSC::Unknown> m_pendingException;
    std::vector<mbun::napi_rt::CleanupHook> m_cleanupHooks;
    std::vector<BoundFinalizer> m_finalizers;
    std::size_t m_cleanupHookCounter{0};
};

namespace mbun::napi_rt {

// ── references (port: napi.h NapiRef) ───────────────────────────────────────
class NapiRef {
public:
    NapiRef(napi_env env, uint32_t count, NapiFinalizer finalizer)
        : env{env}, finalizer{finalizer}, refCount{count} {}

    JSC::JSValue value() const {
        if (refCount == 0 && !m_isEternal) {
            return weakValueRef.get();
        }
        return strongRef ? strongRef.get() : JSC::JSValue();
    }

    inline void setValueInitial(JSC::JSValue value, bool canBeWeak);  // defined below

    void ref() {
        if (++refCount == 1) {
            // resurrect the strong ref from whatever the weak ref still sees
            JSC::JSValue value = weakValueRef.get();
            if (value) {
                strongRef.set(env->vm(), value);
            }
        }
    }

    void unref() {
        if (refCount > 0 && --refCount == 0 && !m_isEternal) {
            strongRef.clear();
        }
    }

    inline void callFinalizer();  // defined below

    ~NapiRef() {
        *m_slot = nullptr;  // disarm the GC-death lambda
        // drop the wrap's bound exit-finalizer, if any (it stores `this`)
        if (env) {
            env->removeFinalizerByData(this);
        }
        if (!m_isEternal) {
            strongRef.clear();
        }
        weakValueRef.clear();
    }

    napi_env env{nullptr};
    NapiWeakValue weakValueRef{};
    JSC::Strong<JSC::Unknown> strongRef{};
    NapiFinalizer finalizer{};
    void* nativeObject{nullptr};
    uint32_t refCount{0};
    // wrap-without-result refs delete themselves when the wrapped object dies
    // (bun: NapiRefSelfDeletingWeakHandleOwner)
    bool selfDeleting{false};

private:
    // The GC-death lambda (Heap::addFinalizer) holds this slot, not the ref:
    // napi_delete_reference can run first, and the lambda must then no-op.
    std::shared_ptr<NapiRef*> m_slot{std::make_shared<NapiRef*>(this)};
    bool m_isEternal{false};
};

// payload behind napi_create_function / napi_define_class (bun keeps these
// fields inline in its NapiClass JSFunction subclass)
struct NapiCallbackData {
    napi_env env{nullptr};
    napi_callback cb{nullptr};
    void* data{nullptr};
};

// ── process-wide runtime state ──────────────────────────────────────────────
struct NapiState {
    // legacy NAPI_MODULE static-constructor registration (napi.cpp:808)
    std::vector<napi_module> pendingModules;
    std::size_t moduleRegisterCallCount{0};
    // dlopen handle → saved registrations, so a re-require replays them
    // (BunProcess.cpp DLHandleMap)
    std::unordered_map<void*, std::vector<napi_module>> dlHandleModules;

    std::vector<PendingFinalizer> pendingFinalizers;  // deferred (GC-time) finalizers
    std::vector<std::unique_ptr<NapiEnv>> envs;       // exit cleanup, LIFO

    HandleScopeRec* currentScope{nullptr};

    // lazily created per-context artifacts
    JSC::Strong<JSC::Structure> externalStructure;

    static NapiState& singleton() {
        static NapiState* state = new NapiState();  // leaked: alive through exit cleanup
        return *state;
    }
};

inline JSC::Structure* externalStructure(JSC::JSGlobalObject* globalObject) {
    auto& state = NapiState::singleton();
    if (!state.externalStructure) {
        state.externalStructure.set(
            JSC::getVM(globalObject),
            NapiExternal::createStructure(JSC::getVM(globalObject), globalObject, JSC::jsNull()));
    }
    return state.externalStructure.get();
}

// private-name identifiers for the hidden properties (never visible to JS —
// private symbols are excluded from every enumeration)
inline JSC::Identifier callbackDataIdent() {
    static JSC::Identifier* ident = new JSC::Identifier(JSC::Identifier::fromUid(
        JSC::PrivateName(JSC::PrivateName::Description, "napiCallbackData"_s)));
    return *ident;
}

inline JSC::Identifier wrapContentsIdent() {
    static JSC::Identifier* ident = new JSC::Identifier(JSC::Identifier::fromUid(
        JSC::PrivateName(JSC::PrivateName::Description, "napiWrappedContents"_s)));
    return *ident;
}

// ── inline definitions deferred until NapiState/NapiRef are complete ───────

inline void NapiFinalizer::call(napi_env env, void* data, bool immediate) {
    if (!m_callback) {
        return;
    }
    if (immediate) {
        m_callback(env, data, m_hint);
    } else {
        NapiState::singleton().pendingFinalizers.push_back({env, m_callback, data, m_hint});
    }
}

inline void NapiExternal::destroy(JSC::JSCell* cell) {
    auto* external = static_cast<NapiExternal*>(cell);
    // sweep-time: never re-enter JS from here — queue the user finalizer
    external->m_finalizer.call(external->m_env, external->m_value, /*immediate=*/false);
    external->~NapiExternal();
}

inline void NapiRef::callFinalizer() {
    NapiFinalizer saved = finalizer;
    finalizer.clear();
    saved.call(env, nativeObject, !env->mustDeferFinalizers() || !env->inGC());
}

inline void NapiRef::setValueInitial(JSC::JSValue value, bool canBeWeak) {
    if (refCount > 0) {
        strongRef.set(env->vm(), value);
    }
    if (canBeWeak) {
        weakValueRef.set(value);
        if (value.isCell()) {
            // GC-death callback (replaces bun's WeakHandleOwner::finalize —
            // see the NapiWeakValue note). The slot disarms if the ref died
            // first; selfDeleting mirrors bun's self-deleting owner.
            std::shared_ptr<NapiRef*> slot = m_slot;
            env->vm().heap.addFinalizer(value.asCell(), [slot](JSC::JSCell*) {
                if (NapiRef* ref = *slot) {
                    ref->callFinalizer();
                    if (ref->selfDeleting) {
                        delete ref;
                    }
                }
            });
        }
    }
    if (value.isSymbol()) {
        // registered (Symbol.for) symbols must survive gc even at rc 0
        if (auto* symbol = dynamicDowncast<JSC::Symbol>(value.asCell());
            symbol && symbol->uid().isRegistered()) {
            m_isEternal = true;
            if (refCount == 0) {
                strongRef.set(env->vm(), value);
            }
        }
    }
}

// ── value conversion (port: napi.h Zig::toJS / toNapi) ─────────────────────
inline JSC::JSValue toJSValue(napi_value val) {
    return JSC::JSValue::decode(reinterpret_cast<JSC::EncodedJSValue>(val));
}

inline napi_value toNapiValue(JSC::JSValue val) {
    if (val.isCell()) {
        if (auto* scope = NapiState::singleton().currentScope) {
            scope->values.append(val);
        }
    }
    return reinterpret_cast<napi_value>(JSC::JSValue::encode(val));
}

// RAII scope for trampolines / module init / finalizers (bun: NapiHandleScope)
class ScopedHandleScope {
public:
    ScopedHandleScope() {
        auto& state = NapiState::singleton();
        auto* rec = new HandleScopeRec();
        rec->parent = state.currentScope;
        state.currentScope = rec;
        m_rec = rec;
    }
    ~ScopedHandleScope() {
        auto& state = NapiState::singleton();
        // close any scopes the addon leaked below ours, then ours
        while (state.currentScope && state.currentScope != m_rec) {
            auto* leaked = state.currentScope;
            state.currentScope = leaked->parent;
            delete leaked;
        }
        if (state.currentScope == m_rec) {
            state.currentScope = m_rec->parent;
            delete m_rec;
        }
    }

private:
    HandleScopeRec* m_rec{nullptr};
};

// A finalizer is a new callback boundary. A VM exception that JS already
// caught can still be visible through the top exception scope when the native
// pump starts; letting NAPI_PREAMBLE observe it makes the finalizer's first API
// call fail with napi_pending_exception. Start clean, like bun's Finalizer::run
// entering from an event-loop task rather than from the previous addon call.
inline void prepareFinalizerCallback(napi_env env) {
    if (env == nullptr) return;
    auto catcher = DECLARE_TOP_EXCEPTION_SCOPE(env->vm());
    catcher.clearException();
    env->clearPendingException();
}

// The JS dispatcher is intentionally writable (workers and node:domain wrap
// it), so dispatch itself can fail. Bypass that user-configurable slot and arm
// the fatal channel's backing properties directly: the native pump reads them
// after every phase, and putDirect cannot run a hostile setter.
inline void armFinalizerFatal(napi_env env, JSC::JSValue error, int status) {
    if (env == nullptr || error.isEmpty()) return;
    JSC::VM& vm{env->vm()};
    JSC::Strong<JSC::Unknown> rooted{vm, error};
    auto catcher = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    catcher.clearException();
    env->clearPendingException();
    JSC::JSGlobalObject* globalObject{env->globalObject()};
    JSC::JSArray* fatal{JSC::constructEmptyArray(globalObject, nullptr, 1)};
    fatal->putDirectIndex(globalObject, 0, rooted.get());
    globalObject->putDirect(vm, JSC::Identifier::fromString(vm, "__mbun_fatal"_s), fatal);
    globalObject->putDirect(vm, JSC::Identifier::fromString(vm, "__mbun_fatal_status"_s),
                            JSC::jsNumber(status));
}

// Hand an error raised by a finalizer to the same process-level uncaught path
// used by timer/I/O callbacks. Return false after fail-closed fallback so the
// drain stops: a later finalizer must not clear the fatal error at its fresh
// callback boundary.
inline bool dispatchFinalizerException(napi_env env, JSC::JSValue error) {
    if (env == nullptr || error.isEmpty()) return true;
    JSC::JSGlobalObject* globalObject{env->globalObject()};
    JSContextRef ctx{toRef(globalObject)};
    JSObjectRef global{JSContextGetGlobalObject(ctx)};
    JSStringRef name{JSStringCreateWithUTF8CString("__mbun_uncaught")};
    JSValueRef lookupException{nullptr};
    JSValueRef candidate{JSObjectGetProperty(ctx, global, name, &lookupException)};
    JSStringRelease(name);
    if (lookupException != nullptr) {
        armFinalizerFatal(env, toJS(globalObject, lookupException), 7);
        return false;
    }
    if (candidate == nullptr || !JSValueIsObject(ctx, candidate)) {
        armFinalizerFatal(env, error, 1);
        return false;
    }
    JSObjectRef handler{JSValueToObject(ctx, candidate, nullptr)};
    if (handler == nullptr || !JSObjectIsFunction(ctx, handler)) {
        armFinalizerFatal(env, error, 1);
        return false;
    }
    JSValueRef argument{toRef(globalObject, error)};
    JSValueRef dispatchException{nullptr};
    JSValueRef dispatchResult{
        JSObjectCallAsFunction(ctx, handler, nullptr, 1, &argument, &dispatchException)};
    if (dispatchException != nullptr) {
        armFinalizerFatal(env, toJS(globalObject, dispatchException), 7);
        return false;
    }
    if (dispatchResult == nullptr) {
        armFinalizerFatal(env, error, 1);
        return false;
    }
    // The shared dispatcher returns false after it has armed fatal state. As
    // with tick/timer drains, stop this batch without overwriting that state.
    return JSValueToBoolean(ctx, dispatchResult);
}

inline bool dispatchFinalizerExceptions(napi_env env) {
    if (env == nullptr) return true;
    JSC::VM& vm{env->vm()};
    auto catcher = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSC::Strong<JSC::Unknown> exception;
    if (JSC::Exception* vmException{catcher.exception()}) {
        exception.set(vm, vmException->value());
        catcher.clearException();
    }
    if (env->hasPendingException()) {
        if (!exception) exception.set(vm, env->pendingException());
        env->clearPendingException();
    }
    if (!exception) return true;
    return dispatchFinalizerException(env, exception.get());
}

extern "C" __attribute__((visibility("hidden"))) void mbun_napi_test_dispatch_finalizer_error(
    void* opaqueContext, const char* message) {
    auto ctx = static_cast<JSContextRef>(opaqueContext);
    JSC::JSGlobalObject* globalObject{toJS(ctx)};
    JSC::JSLockHolder locker{globalObject->vm()};
    JSStringRef text{JSStringCreateWithUTF8CString(message)};
    JSValueRef argument{JSValueMakeString(ctx, text)};
    JSStringRelease(text);
    JSValueRef creationException{nullptr};
    JSObjectRef error{JSObjectMakeError(ctx, 1, &argument, &creationException)};
    if (creationException != nullptr || error == nullptr) return;
    NapiEnv env{globalObject, NAPI_VERSION, "[finalizer dispatch test]"};
    JSC::JSValue errorValue{toJS(globalObject, error)};
    // Model the defensive dual-store case: one callback error can be visible
    // through both the VM and napi_env. The drain must select and dispatch it
    // once, not offer the same exception to process twice.
    env.scheduleException(errorValue);
    (void)env.throwPendingException();
    env.scheduleException(errorValue);
    (void)dispatchFinalizerExceptions(&env);
}

// Drain the deferred (GC-time) finalizer task queue. Only called from
// event-loop pump points / env cleanup — never from inside GC.
inline void drainPendingFinalizers() {
    auto& state = NapiState::singleton();
    while (!state.pendingFinalizers.empty()) {
        std::vector<PendingFinalizer> batch = std::move(state.pendingFinalizers);
        state.pendingFinalizers.clear();
        for (const PendingFinalizer& fin : batch) {
            JSC::JSLockHolder locker{fin.env->vm()};
            ScopedHandleScope scope;
            prepareFinalizerCallback(fin.env);
            fin.cb(fin.env, fin.data, fin.hint);
            if (!dispatchFinalizerExceptions(fin.env)) return;
        }
    }
}

static void testFailingFinalizer(napi_env env, void*, void*) {
    (void)napi_throw_error(env, nullptr, "first finalizer failed");
}

static void testCountingFinalizer(napi_env, void* data, void*) {
    ++*static_cast<int*>(data);
}

extern "C" __attribute__((visibility("hidden"))) int mbun_napi_test_drain_two_finalizers(
    void* opaqueContext) {
    auto ctx = static_cast<JSContextRef>(opaqueContext);
    JSC::JSGlobalObject* globalObject{toJS(ctx)};
    NapiEnv env{globalObject, NAPI_VERSION, "[finalizer drain test]"};
    int secondCalls{0};
    auto& queue{NapiState::singleton().pendingFinalizers};
    queue.push_back({&env, testFailingFinalizer, nullptr, nullptr});
    queue.push_back({&env, testCountingFinalizer, &secondCalls, nullptr});
    drainPendingFinalizers();
    return secondCalls;
}

// ── callback info (port: napi.h NAPICallFrame) ─────────────────────────────
class NAPICallFrame {
public:
    NAPICallFrame(JSC::JSGlobalObject* globalObject, JSC::CallFrame* callFrame, void* dataPtr,
                  JSC::JSValue storedNewTarget = JSC::JSValue())
        : m_callFrame{callFrame}, m_dataPtr{dataPtr}, m_storedNewTarget{storedNewTarget} {
        m_isConstructorCall = !m_storedNewTarget.isEmpty();
        // Node-API callbacks are always "sloppy mode": null/undefined `this`
        // becomes globalThis, primitives are boxed (napi.h NAPICallFrame ctor).
        JSC::JSObject* jscThis = globalObject->globalThis();
        if (!m_callFrame->thisValue().isUndefinedOrNull()) {
            auto scope = DECLARE_THROW_SCOPE(JSC::getVM(globalObject));
            jscThis = m_callFrame->thisValue().toObject(globalObject);
            scope.assertNoException();  // toObject only throws for undefined/null
        }
        m_callFrame->setThisValue(jscThis);
    }

    JSC::JSValue thisValue() const { return m_callFrame->thisValue(); }
    napi_callback_info toNapi() { return reinterpret_cast<napi_callback_info>(this); }
    void* dataPtr() const { return m_dataPtr; }

    void extract(size_t* argc, napi_value* argv, napi_value* this_arg, void** data) {
        if (this_arg != nullptr) {
            *this_arg = toNapiValue(m_callFrame->thisValue());
        }
        if (data != nullptr) {
            *data = m_dataPtr;
        }
        size_t maxArgc = 0;
        if (argc != nullptr) {
            maxArgc = *argc;
            *argc = m_callFrame->argumentCount();
        }
        if (argv != nullptr) {
            for (size_t i = 0; i < maxArgc; i++) {
                // argument() yields js undefined out of bounds, matching napi
                argv[i] = toNapiValue(m_callFrame->argument(i));
            }
        }
    }

    JSC::JSValue newTarget() const {
        if (!m_isConstructorCall || m_storedNewTarget.isUndefined()) {
            return JSC::JSValue();
        }
        return m_storedNewTarget;
    }

private:
    JSC::CallFrame* m_callFrame;
    void* m_dataPtr;
    JSC::JSValue m_storedNewTarget;
    bool m_isConstructorCall{false};
};

// ── status plumbing (port: napi.cpp napi_set_last_error/napi_clear_last_error;
// kept as plain inline functions, not exported ABI) ─────────────────────────
inline napi_status setLastError(napi_env env, napi_status status) {
    if (env) {
        env->m_lastNapiErrorInfo.error_code = status;
    }
    return status;
}

inline napi_status clearLastError(napi_env env) {
    if (env) {
        env->m_lastNapiErrorInfo.error_code = napi_ok;
        env->m_lastNapiErrorInfo.engine_error_code = 0;
        env->m_lastNapiErrorInfo.engine_reserved = nullptr;
        env->m_lastNapiErrorInfo.error_message = nullptr;
    }
    return napi_ok;
}

}  // namespace mbun::napi_rt

// port: napi.h NapiEnv::cleanup — hooks LIFO, then finalizers LIFO, then
// instance data; exceptions cleared between callbacks.
inline void NapiEnv::cleanup() {
    auto clearExceptions = [&] {
        auto catcher = DECLARE_TOP_EXCEPTION_SCOPE(vm());
        catcher.clearException();
        clearPendingException();
    };
    clearExceptions();
    while (!m_cleanupHooks.empty()) {
        // highest insertionCounter first (LIFO), tolerating removals mid-drain
        auto newest = m_cleanupHooks.begin();
        for (auto it = m_cleanupHooks.begin(); it != m_cleanupHooks.end(); ++it) {
            if (it->insertionCounter > newest->insertionCounter) {
                newest = it;
            }
        }
        auto hook = *newest;
        m_cleanupHooks.erase(newest);
        if (hook.function != nullptr) {
            hook.function(hook.data);
        }
        clearExceptions();
    }
    mbun::napi_rt::drainPendingFinalizers();
    for (auto it = m_finalizers.rbegin(); it != m_finalizers.rend(); ++it) {
        mbun::napi_rt::ScopedHandleScope scope;
        if (it->cb != nullptr) {
            it->cb(this, it->data, it->hint);
        }
        clearExceptions();
    }
    m_finalizers.clear();
    instanceDataFinalizer.call(this, instanceData, /*immediate=*/true);
    instanceDataFinalizer.clear();
    clearExceptions();
}

// ── runtime hooks (defined in runtime/napi_objects.inc; called from
//    engine.inc's require path / process.dlopen trampoline) ─────────────────
JSValueRef mbun_napi_require_module(JSContextRef ctx, const std::string& path, JSValueRef* exc);
void mbun_napi_process_dlopen(JSContextRef ctx, JSObjectRef moduleObj, const std::string& filename,
                              JSValueRef* exc);
void mbun_napi_drain_deferred_finalizers();

// ── shared macros (port: napi.cpp preamble family, minus verbose logging) ──
#define NAPI_PREAMBLE(_env)                                                  \
    NAPI_CHECK_ARG(_env, _env);                                              \
    auto napi_preamble_throw_scope__ = DECLARE_THROW_SCOPE((_env)->vm());    \
    NAPI_RETURN_IF_VM_EXCEPTION(_env)

#define NAPI_PREAMBLE_NO_THROW_SCOPE(_env)  \
    do {                                    \
        NAPI_CHECK_ARG(_env, _env);         \
    } while (0)

#define NAPI_CHECK_ARG(_env, arg)                                            \
    do {                                                                     \
        if ((arg) == nullptr) [[unlikely]] {                                 \
            return mbun::napi_rt::setLastError(_env, napi_invalid_arg);      \
        }                                                                    \
    } while (0)

#define NAPI_RETURN_EARLY_IF_FALSE(_env, condition, code)       \
    do {                                                        \
        if (!(condition)) {                                     \
            return mbun::napi_rt::setLastError(_env, code);     \
        }                                                       \
    } while (0)

#define NAPI_RETURN_IF_VM_EXCEPTION(_env)  \
    RETURN_IF_EXCEPTION(napi_preamble_throw_scope__, mbun::napi_rt::setLastError((_env), napi_pending_exception))

#define NAPI_RETURN_IF_EXCEPTION_WITH_SCOPE(_env, _scope)                                          \
    do {                                                                                           \
        RETURN_IF_EXCEPTION((_scope), mbun::napi_rt::setLastError((_env), napi_pending_exception)); \
        if ((_env)->hasPendingException()) {                                                       \
            return mbun::napi_rt::setLastError((_env), napi_pending_exception);                    \
        }                                                                                          \
    } while (0)

#define NAPI_RETURN_IF_EXCEPTION(_env) \
    NAPI_RETURN_IF_EXCEPTION_WITH_SCOPE((_env), napi_preamble_throw_scope__)

#define NAPI_RETURN_SUCCESS(_env)                                 \
    do {                                                          \
        napi_preamble_throw_scope__.assertNoException();          \
        return mbun::napi_rt::setLastError(_env, napi_ok);        \
    } while (0)

#define NAPI_RETURN_SUCCESS_UNLESS_EXCEPTION(_env)                \
    do {                                                          \
        NAPI_RETURN_IF_EXCEPTION(_env);                           \
        return mbun::napi_rt::setLastError(_env, napi_ok);        \
    } while (0)
