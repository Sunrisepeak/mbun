// ── __mbunNodeVMNative: real sandbox contexts for node:vm ───────────────────
// Powers node:vm's Script/createContext/runInContext/compileFunction (JS layer
// lives in builtins/node_vm.cppm). Each vm context is a genuine JSC global
// context created in the MAIN context's group (JSGlobalContextCreateInGroup):
// same VM/heap, so JSValueRefs cross freely, but a distinct global object — that
// separate global gives real isolation (no process/Bun/host globals leak in,
// and host globalThis is unreachable) without any custom scope machinery.
//
// Contextify semantics: before each run the sandbox's own-enumerable props are
// copied onto the child global; after the run the child global's own-enumerable
// props are copied back onto the sandbox. Engine builtins (Object, Array, …) are
// non-enumerable on the global, so they are naturally skipped both ways — only
// user data and script-created globals move. DEFERRED: real timeout interruption,
// microtask-queue isolation, cachedData/bytecode, DONT_CONTEXTIFY realm sharing,
// and node:vm.Module/SourceTextModule (handled elsewhere / left unimplemented).

struct NodeVMContext {
    JSGlobalContextRef ctx;
    JSObjectRef sandbox;  // own reference protected in the main context
};

std::unordered_map<int, NodeVMContext>& nodevm_contexts() {
    static std::unordered_map<int, NodeVMContext> gCtxs;
    return gCtxs;
}
int& nodevm_next_handle() {
    static int gNext{1};
    return gNext;
}

// Copy every own-enumerable property from src (in srcCtx) onto dst (in dstCtx).
// Same VM group, so a JSValueRef read in one context is valid in the other.
void nodevm_copy_enumerable(JSContextRef srcCtx, JSObjectRef src, JSContextRef dstCtx,
                            JSObjectRef dst) {
    JSPropertyNameArrayRef names{JSObjectCopyPropertyNames(srcCtx, src)};
    std::size_t n{JSPropertyNameArrayGetCount(names)};
    for (std::size_t i{0}; i < n; ++i) {
        JSStringRef name{JSPropertyNameArrayGetNameAtIndex(names, i)};
        JSValueRef v{JSObjectGetProperty(srcCtx, src, name, nullptr)};
        JSObjectSetProperty(dstCtx, dst, name, v, kJSPropertyAttributeNone, nullptr);
    }
    JSPropertyNameArrayRelease(names);
}

// createContext(sandbox?) → integer handle. sandbox is contextified in place; a
// non-object argument gets a fresh backing object (the JS layer normally passes
// an object already).
JSValueRef nodevm_create_context_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                    std::size_t argc, const JSValueRef args[], JSValueRef*) {
    JSObjectRef sandbox{(argc > 0 && JSValueIsObject(ctx, args[0]))
                            ? JSValueToObject(ctx, args[0], nullptr)
                            : JSObjectMake(ctx, nullptr, nullptr)};
    JSContextGroupRef group{JSContextGetGroup(ctx)};
    JSGlobalContextRef child{JSGlobalContextCreateInGroup(group, nullptr)};

    // Share a curated set of pure structured-value constructors from the host
    // realm so a value built inside the sandbox (new Set/Map/…) keeps a prototype
    // the host recognises (structural equality across the boundary). These carry
    // no host state; Object/Function/eval/globalThis are deliberately NOT shared,
    // so global-scope and host-object isolation is preserved.
    JSObjectRef mainGlobal{JSContextGetGlobalObject(ctx)};
    JSObjectRef childGlobal0{JSContextGetGlobalObject(child)};
    static constexpr const char* kSharedIntrinsics[]{
        "Set", "Map", "WeakSet", "WeakMap", "Date", "RegExp", "Promise"};
    for (const char* name : kSharedIntrinsics) {
        JSStringRef key{JSStringCreateWithUTF8CString(name)};
        JSValueRef v{JSObjectGetProperty(ctx, mainGlobal, key, nullptr)};
        if (!JSValueIsUndefined(ctx, v)) {
            // DontEnum so contextify-out never copies these back onto the sandbox.
            JSObjectSetProperty(child, childGlobal0, key, v, kJSPropertyAttributeDontEnum, nullptr);
        }
        JSStringRelease(key);
    }

    JSValueProtect(ctx, sandbox);
    int h{nodevm_next_handle()++};
    nodevm_contexts()[h] = NodeVMContext{child, sandbox};
    return JSValueMakeNumber(ctx, static_cast<double>(h));
}

// getGlobal(handle) — the child realm's global object, as a value usable from
// the main context (same VM group). The JS layer mirrors the sandbox onto it
// with full property descriptors, which JSObjectCopyPropertyNames (enumerable
// string keys only, values only) cannot express.
JSValueRef nodevm_get_global_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                std::size_t argc, const JSValueRef args[], JSValueRef* exc) {
    int h{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : 0};
    auto it{nodevm_contexts().find(h)};
    if (it == nodevm_contexts().end()) {
        if (exc != nullptr) *exc = make_string(ctx, "vm: invalid context handle");
        return JSValueMakeUndefined(ctx);
    }
    return JSContextGetGlobalObject(it->second.ctx);
}

// checkSyntax(code, filename?) — parse without running, so `new vm.Script(bad)`
// throws at construction time the way node does.
JSValueRef nodevm_check_syntax_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                  std::size_t argc, const JSValueRef args[], JSValueRef* exc) {
    JSValueRef ex{nullptr};
    JSStringRef src{JSValueToStringCopy(ctx, argc > 0 ? args[0] : JSValueMakeUndefined(ctx), &ex)};
    if (src == nullptr) {
        if (exc != nullptr) *exc = ex;
        return JSValueMakeUndefined(ctx);
    }
    JSStringRef url{(argc > 1 && !JSValueIsUndefined(ctx, args[1]))
                        ? JSValueToStringCopy(ctx, args[1], nullptr)
                        : nullptr};
    bool ok{JSCheckScriptSyntax(ctx, src, url, 0, &ex)};
    JSStringRelease(src);
    if (url != nullptr) JSStringRelease(url);
    if (!ok && ex != nullptr) {
        if (exc != nullptr) *exc = ex;
        return JSValueMakeUndefined(ctx);
    }
    return JSValueMakeBoolean(ctx, ok);
}

// runInContext(handle, code, filename?, raw?) — evaluate in the child global.
// `raw` (used by the current JS layer) skips the legacy value-level contextify
// copy entirely: the JS layer owns the mirroring and must not have an accessor
// silently flattened into a data property behind it. Syntax/runtime errors
// propagate as JS exceptions.
JSValueRef nodevm_run_in_context_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                    std::size_t argc, const JSValueRef args[], JSValueRef* exc) {
    int h{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : 0};
    auto it{nodevm_contexts().find(h)};
    if (it == nodevm_contexts().end()) {
        if (exc != nullptr) *exc = make_string(ctx, "vm: invalid context handle");
        return JSValueMakeUndefined(ctx);
    }
    bool raw{argc > 3 && JSValueToBoolean(ctx, args[3])};
    JSGlobalContextRef child{it->second.ctx};
    JSObjectRef sandbox{it->second.sandbox};
    JSObjectRef childGlobal{JSContextGetGlobalObject(child)};

    if (!raw) nodevm_copy_enumerable(ctx, sandbox, child, childGlobal);

    JSValueRef ex{nullptr};
    JSStringRef src{JSValueToStringCopy(ctx, argc > 1 ? args[1] : JSValueMakeUndefined(ctx), &ex)};
    if (src == nullptr) {
        if (exc != nullptr) *exc = ex;
        return JSValueMakeUndefined(ctx);
    }
    JSStringRef url{(argc > 2 && !JSValueIsUndefined(ctx, args[2]))
                        ? JSValueToStringCopy(ctx, args[2], nullptr)
                        : nullptr};
    JSValueRef result{JSEvaluateScript(child, src, nullptr, url, 0, &ex)};
    JSStringRelease(src);
    if (url != nullptr) JSStringRelease(url);

    // Contextify out even after a throw, so partial mutations are observable
    // (Node reflects sandbox writes made before the exception).
    if (!raw) nodevm_copy_enumerable(child, childGlobal, ctx, sandbox);

    if (ex != nullptr) {
        if (exc != nullptr) *exc = ex;
        return JSValueMakeUndefined(ctx);
    }
    return result;
}

// runInThisContext(code, filename?) / compileFunction default: evaluate in the
// MAIN context (host globals visible), returning the program's completion value.
JSValueRef nodevm_run_in_this_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                 std::size_t argc, const JSValueRef args[], JSValueRef* exc) {
    JSValueRef ex{nullptr};
    JSStringRef src{JSValueToStringCopy(ctx, argc > 0 ? args[0] : JSValueMakeUndefined(ctx), &ex)};
    if (src == nullptr) {
        if (exc != nullptr) *exc = ex;
        return JSValueMakeUndefined(ctx);
    }
    JSStringRef url{(argc > 1 && !JSValueIsUndefined(ctx, args[1]))
                        ? JSValueToStringCopy(ctx, args[1], nullptr)
                        : nullptr};
    JSValueRef result{JSEvaluateScript(ctx, src, nullptr, url, 0, &ex)};
    JSStringRelease(src);
    if (url != nullptr) JSStringRelease(url);
    if (ex != nullptr) {
        if (exc != nullptr) *exc = ex;
        return JSValueMakeUndefined(ctx);
    }
    return result;
}

void install_node_vm_native(JSContextRef ctx, JSObjectRef global) {
    JSObjectRef vmn{JSObjectMake(ctx, nullptr, nullptr)};
    set_fn(ctx, vmn, "createContext", nodevm_create_context_cb);
    set_fn(ctx, vmn, "runInContext", nodevm_run_in_context_cb);
    set_fn(ctx, vmn, "runInThis", nodevm_run_in_this_cb);
    set_fn(ctx, vmn, "getGlobal", nodevm_get_global_cb);
    set_fn(ctx, vmn, "checkSyntax", nodevm_check_syntax_cb);
    set_prop(ctx, global, "__mbunNodeVMNative", vmn);
}
