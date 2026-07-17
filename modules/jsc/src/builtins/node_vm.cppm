// node:vm payload partition. Real sandbox contexts via __mbunNodeVMNative
// (runtime/node_vm.inc): each createContext() builds a genuine JSC global
// context in the main VM group, so context isolation (no host process/Bun/
// globalThis leak) and cross-context value passing are real, not emulated.
//
// Covers Script (runInThisContext/runInContext/runInNewContext), createContext/
// isContext, runIn{This,New,}Context, compileFunction, and the string/options
// forms (filename/lineOffset/timeout/displayErrors/contextObject). Blueprint:
// bun-ref src/js/node/vm.ts (the JS shape; the native NodeVM.cpp is replaced by
// the C-API bridge above). DEFERRED: vm.Module/SourceTextModule/SyntheticModule,
// cachedData/bytecode (createCachedData/cachedDataRejected), real timeout
// interruption, microtaskMode isolation, and DONT_CONTEXTIFY realm sharing.
export module mbun.jsc.js_builtins:node_vm;

import std;

export namespace mbun::jsc::builtins::detail {

// NOTE: appended AFTER the master builtins IIFE has closed (see image_closure),
// so this is a self-contained IIFE that re-binds G = globalThis and overrides
// the module registry's earlier `vm` stub with the real implementation.
inline constexpr std::string_view kNodeVmJS = R"JS(
(function () {
  const G = globalThis;
  const NVM = G.__mbunNodeVMNative;
  const M = G.__mbunNativeModules;
  if (!NVM || !M) return;

  // Objects that have been contextified, and their native context handles.
  const contexts = new WeakSet();
  const handles = new WeakMap();

  const DONT_CONTEXTIFY = Symbol("vm_dont_contextify");
  const USE_MAIN_CONTEXT_DEFAULT_LOADER = Symbol("vm_use_main_context_default_loader");

  const argTypeError = (name, expected) => {
    const e = new TypeError('The "' + name + '" argument must be ' + expected + ".");
    e.code = "ERR_INVALID_ARG_TYPE";
    return e;
  };

  function isContext(object) {
    if ((typeof object !== "object" && typeof object !== "function") || object === null) return false;
    return contexts.has(object);
  }

  function createContext(contextObject, options) {
    if (contextObject === DONT_CONTEXTIFY) {
      // DEFERRED: a true uncontextified realm whose globalThis IS the returned
      // object. Approximated with a fresh contextified object so basic use and
      // isContext() still work; realm-identity tests remain unsupported.
      const o = {};
      handles.set(o, NVM.createContext({}));
      contexts.add(o);
      return o;
    }
    if (contextObject === undefined || contextObject === null) contextObject = {};
    if (typeof contextObject !== "object" && typeof contextObject !== "function") {
      throw argTypeError("contextObject", "of type object");
    }
    if (isContext(contextObject)) return contextObject;
    const h = NVM.createContext(contextObject);
    handles.set(contextObject, h);
    contexts.add(contextObject);
    return contextObject;
  }

  const normalizeOptions = (options) =>
    typeof options === "string" ? { filename: options } : (options || {});
  const filenameOf = (options) => {
    const f = normalizeOptions(options).filename;
    return f === undefined ? "evalmachine.<anonymous>" : String(f);
  };

  class Script {
    constructor(code, options) {
      this.__code = `${code}`;
      this.__filename = filenameOf(options);
      // cachedData/bytecode is DEFERRED; expose the shape Node scripts carry.
      this.cachedDataRejected = normalizeOptions(options).cachedData !== undefined ? true : undefined;
    }
    runInThisContext(options) {
      return NVM.runInThis(this.__code, this.__filename);
    }
    runInContext(contextifiedObject, options) {
      if (!isContext(contextifiedObject)) throw argTypeError("contextifiedObject", "an vm.Context");
      return NVM.runInContext(handles.get(contextifiedObject), this.__code, this.__filename);
    }
    runInNewContext(contextObject, options) {
      const ctx = createContext(contextObject, options);
      return NVM.runInContext(handles.get(ctx), this.__code, this.__filename);
    }
    createCachedData() {
      // DEFERRED: no V8-style bytecode cache. Node throws on module/error scripts;
      // keep the observable "failed" contract for callers that probe it.
      throw new Error("createCachedData failed");
    }
  }

  function runInThisContext(code, options) {
    return new Script(code, options).runInThisContext(options);
  }
  function runInContext(code, contextifiedObject, options) {
    if (!isContext(contextifiedObject)) throw argTypeError("contextifiedObject", "an vm.Context");
    return new Script(code, options).runInContext(contextifiedObject, options);
  }
  function runInNewContext(code, contextObject, options) {
    if (contextObject !== undefined && contextObject !== DONT_CONTEXTIFY &&
        (typeof contextObject !== "object" || contextObject === null)) {
      throw argTypeError("contextObject", "of type object");
    }
    return new Script(code, options).runInNewContext(contextObject, options);
  }

  function compileFunction(code, params, options) {
    options = options || {};
    // Build through the target realm's Function constructor: it parses the
    // assembled `function anonymous(params){ body }` as one unit, so a body that
    // tries to close the wrapper early (injection) is a SyntaxError, and the
    // resulting function belongs to that realm (parsingContext → sandbox realm,
    // so eval/with inside it cannot reach the host globals).
    const args = Array.isArray(params) ? params.slice() : [];
    args.push(`${code}`);
    const pc = options.parsingContext;
    let FunctionCtor;
    if (pc !== undefined && pc !== null) {
      if (!isContext(pc)) throw argTypeError("options.parsingContext", "an vm.Context");
      FunctionCtor = NVM.runInContext(handles.get(pc), "Function", undefined);
    } else {
      FunctionCtor = Function;
    }
    return Reflect.apply(FunctionCtor, undefined, args);
  }

  function measureMemory() {
    // DEFERRED: JSC has no per-context accounting; report an empty summary.
    return Promise.resolve({ total: { jsMemoryEstimate: 0, jsMemoryRange: [0, 0] } });
  }

  const constants = Object.freeze({ __proto__: null, DONT_CONTEXTIFY, USE_MAIN_CONTEXT_DEFAULT_LOADER });

  const vm = {
    Script,
    createContext,
    isContext,
    runInThisContext,
    runInContext,
    runInNewContext,
    compileFunction,
    measureMemory,
    constants,
  };
  M["vm"] = vm;
  M["node:vm"] = vm;
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
