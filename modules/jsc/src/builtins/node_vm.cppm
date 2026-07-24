// node:vm payload partition. Real sandbox contexts via __mbunNodeVMNative
// (runtime/node_vm.inc): each createContext() builds a genuine JSC global
// context in the main VM group, so context isolation (no host process/Bun/
// globalThis leak) and cross-context value passing are real, not emulated.
//
// Covers Script (runInThisContext/runInContext/runInNewContext), createContext/
// isContext, runIn{This,New,}Context, compileFunction, and the string/options
// forms (filename/lineOffset/timeout/displayErrors/contextObject). Blueprint:
// bun-ref src/js/node/vm.ts (the JS shape; the native NodeVM.cpp is replaced by
// the C-API bridge above).
//
// Contextify lives HERE rather than in the native layer: node installs V8
// named/indexed property interceptors on the context's global proxy, and JSC's
// C API has no equivalent, so the sandbox is mirrored onto the child realm's
// global around every run. Doing that in JS (over the global object handed back
// by __mbunNodeVMNative.getGlobal) is what makes the mirror carry FULL property
// descriptors — accessors stay accessors, non-enumerable stays non-enumerable,
// symbol keys travel, deletions propagate, and the sandbox's prototype chain is
// reachable — none of which JSObjectCopyPropertyNames + JSObjectSetProperty
// (enumerable string keys, values only) can express.
//
// DEFERRED: vm.Module/SourceTextModule/SyntheticModule, cachedData/bytecode
// (createCachedData/cachedDataRejected), real timeout interruption,
// microtaskMode isolation, and DONT_CONTEXTIFY realm identity.
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

  const ObjectDefineProperty = Object.defineProperty;
  const gOPD = Object.getOwnPropertyDescriptor;
  const ownKeys = Reflect.ownKeys;

  const contexts = new WeakSet();
  const records = new WeakMap();

  const DONT_CONTEXTIFY = Symbol("vm_dont_contextify");
  const USE_MAIN_CONTEXT_DEFAULT_LOADER = Symbol("vm_use_main_context_default_loader");

  const invalidArgTypeHelper = (input) => {
    if (input === undefined || input === null) return " Received " + String(input);
    if (typeof input === "function") return " Received function " + input.name;
    if (typeof input === "object") {
      if (input.constructor && input.constructor.name) {
        return " Received an instance of " + input.constructor.name;
      }
      return " Received " + String(input);
    }
    let inspected = typeof input === "string" ? "'" + input + "'" : String(input);
    if (inspected.length > 28) inspected = inspected.slice(0, 25) + "...";
    return " Received type " + (typeof input) + " (" + inspected + ")";
  };
  const invArgType = (name, expected, actual, kind) => {
    const e = new TypeError('The "' + name + '" ' + (kind || "argument") + " must be " +
                            expected + "." + invalidArgTypeHelper(actual));
    e.code = "ERR_INVALID_ARG_TYPE";
    return e;
  };
  const outOfRange = (name, msg, actual) => {
    const e = new RangeError('The value of "' + name + '" is out of range.' +
                             (msg ? " " + msg : "") + " Received " + String(actual));
    e.code = "ERR_OUT_OF_RANGE";
    return e;
  };
  const argTypeError = (name, expected) => invArgType(name, expected, undefined);

  const validateString = (value, name) => {
    if (typeof value !== "string") throw invArgType(name, "of type string", value, "property");
  };
  const validateBoolean = (value, name) => {
    if (typeof value !== "boolean") throw invArgType(name, "of type boolean", value, "property");
  };
  const validateInt32 = (value, name, min, max) => {
    if (min === undefined) min = -2147483648;
    if (max === undefined) max = 2147483647;
    if (typeof value !== "number") throw invArgType(name, "of type number", value, "property");
    if (!Number.isInteger(value)) throw outOfRange(name, "It must be an integer.", value);
    if (value < min || value > max) {
      throw outOfRange(name, "It must be >= " + min + " && <= " + max + ".", value);
    }
  };
  const isArrayBufferView = (v) => typeof ArrayBuffer.isView === "function" && ArrayBuffer.isView(v);

  function isContextInternal(object) {
    if ((typeof object !== "object" && typeof object !== "function") || object === null) return false;
    return contexts.has(object);
  }
  function isContext(object) {
    if (typeof object !== "object" || object === null) {
      throw invArgType("contextifiedObject", "of type object", object);
    }
    return contexts.has(object);
  }

  // ── contextify ────────────────────────────────────────────────────────────
  // node contextifies through V8 named/indexed property interceptors installed
  // on the context's global proxy, so the sandbox object stays the single
  // source of truth. JSC's C API exposes no such interceptor, so the sandbox is
  // mirrored onto the child realm's global object around every run — but with
  // FULL property descriptors (accessors, non-enumerable data, symbol keys) in
  // both directions, which is what makes the observable surface match:
  // `Object.getOwnPropertyNames(this)` inside the context sees the sandbox's
  // non-enumerable keys, an accessor defined on the sandbox keeps firing on the
  // sandbox, and a property deleted on either side disappears on the other.
  //
  // A dedicated `raw` evaluation path keeps the native layer out of the copy so
  // it cannot downgrade an accessor into a data property behind our back.
  function newRecord(sandbox) {
    const handle = NVM.createContext(NVM.getGlobal ? undefined : {});
    const g = NVM.getGlobal ? NVM.getGlobal(handle)
                            : NVM.runInContext(handle, "globalThis", undefined);
    // Snapshot of the realm's own globals, so a script that OVERWRITES one
    // (`this.Symbol = Symbol`) is still seen as a user write on the way out,
    // while the untouched builtins stay out of the sandbox.
    const nativeKeys = new Set(ownKeys(g));
    const nativeVals = new Map();
    for (const key of nativeKeys) {
      const d = gOPD(g, key);
      if (d !== undefined && "value" in d) nativeVals.set(key, d.value);
    }
    const rec = {
      handle,
      global: g,
      nativeKeys,
      nativeVals,
      mirrored: new Set(),
      proto: new Map(),
      sandbox,
    };
    return rec;
  }

  function syncIn(rec) {
    const { sandbox, global: g, mirrored, proto } = rec;
    const seen = new Set();
    for (const key of ownKeys(sandbox)) {
      seen.add(key);
      proto.delete(key);
      let desc = gOPD(sandbox, key);
      if (desc === undefined) continue;
      if ("value" in desc && desc.value === sandbox) {
        // node's PropertyGetterCallback maps the sandbox onto the global proxy,
        // so `ctx.window = ctx` makes `window === this` inside the context.
        desc = { ...desc, value: g };
      }
      try { ObjectDefineProperty(g, key, desc); mirrored.add(key); } catch { /* non-configurable */ }
    }
    // node resolves an in-context global lookup with GetRealNamedProperty on
    // the sandbox, which walks its prototype chain; mirror inherited members
    // too, but non-enumerably and marked so they never travel back as OWN
    // properties of the sandbox. Object.prototype is skipped: the context realm
    // already has its own, and copying it would add own keys to the global.
    let src = sandbox;
    try { src = Object.getPrototypeOf(sandbox); } catch { src = null; }
    while (src !== null && src !== undefined && src !== Object.prototype) {
      for (const key of ownKeys(src)) {
        if (seen.has(key)) continue;
        seen.add(key);
        const desc = gOPD(src, key);
        if (desc === undefined) continue;
        try {
          ObjectDefineProperty(g, key, { ...desc, enumerable: false });
          mirrored.add(key);
          proto.set(key, "value" in desc ? desc.value : proto);
        } catch { /* non-configurable */ }
      }
      try { src = Object.getPrototypeOf(src); } catch { break; }
    }
    // Properties dropped from the sandbox since the last run must disappear
    // from the context global too.
    for (const key of [...mirrored]) {
      if (!seen.has(key)) {
        try { delete g[key]; } catch { /* ignore */ }
        mirrored.delete(key);
        proto.delete(key);
      }
    }
  }

  function syncOut(rec) {
    const { sandbox, global: g, nativeKeys, nativeVals, mirrored, proto } = rec;
    const live = new Set();
    for (const key of ownKeys(g)) {
      const desc = gOPD(g, key);
      if (desc === undefined) continue;
      if (nativeKeys.has(key) && !mirrored.has(key)) {
        // Only a builtin the script actually replaced travels out. An accessor
        // among the realm's own globals is never one of those, so it stays put
        // (defining it on the sandbox would be an extra, observable write).
        if (!("value" in desc)) continue;
        // SameValue, not ===: the realm's own `NaN` global would otherwise
        // compare unequal to itself on every single run.
        if (Object.is(desc.value, nativeVals.get(key))) continue;
      } else if (proto.has(key)) {
        // node's PropertySetterCallback always writes to the sandbox itself,
        // so assigning to a name the sandbox merely INHERITS creates an own,
        // enumerable property on it. A member left untouched must not.
        if (!("value" in desc) || Object.is(desc.value, proto.get(key))) { live.add(key); continue; }
        live.add(key);
        proto.delete(key);
        try {
          ObjectDefineProperty(sandbox, key, {
            value: desc.value === g ? sandbox : desc.value,
            writable: true, enumerable: true, configurable: true,
          });
        } catch { /* ignore */ }
        continue;
      } else if ((desc.get !== undefined || desc.set !== undefined) && mirrored.has(key)) {
        // An accessor mirrored in from the sandbox stays owned by the sandbox.
        live.add(key);
        continue;
      }
      live.add(key);
      let d = desc;
      if ("value" in d && d.value === g) d = { ...d, value: sandbox };
      try { ObjectDefineProperty(sandbox, key, d); } catch { /* ignore */ }
      mirrored.add(key);
    }
    for (const key of [...mirrored]) {
      if (!live.has(key)) {
        if (!proto.has(key)) { try { delete sandbox[key]; } catch { /* ignore */ } }
        mirrored.delete(key);
        proto.delete(key);
      }
    }
  }

  function evalInContext(rec, code, filename) {
    syncIn(rec);
    try {
      return NVM.runInContext(rec.handle, code, filename, true);
    } finally {
      syncOut(rec);
    }
  }

  function validateCodegen(cg, name) {
    if (cg === undefined) return undefined;
    if (typeof cg !== "object" || cg === null) {
      throw invArgType(name, "of type object", cg, "property");
    }
    const strings = cg.strings;
    const wasm = cg.wasm;
    if (strings !== undefined) validateBoolean(strings, name + ".strings");
    if (wasm !== undefined) validateBoolean(wasm, name + ".wasm");
    return { strings: strings, wasm: wasm };
  }
  function applyCodegen(rec, cg) {
    if (!cg) return;
    if (cg.strings === false) {
      NVM.runInContext(rec.handle,
        "Object.defineProperty(globalThis,'eval',{value:function(){throw new EvalError('Code generation from strings disallowed for this context');},writable:true,enumerable:false,configurable:true});",
        undefined, true);
    }
    if (cg.wasm === false) {
      NVM.runInContext(rec.handle,
        "(function(){var CE=WebAssembly.CompileError;Object.defineProperty(WebAssembly,'Module',{value:function(){throw new CE('Wasm code generation disallowed in this context');},writable:true,enumerable:false,configurable:true});})();",
        undefined, true);
    }
  }

  function createContext(contextObject, options) {
    if (contextObject === DONT_CONTEXTIFY) {
      const rec0 = newRecord({});
      const o = rec0.global;
      rec0.sandbox = o;
      records.set(o, rec0);
      contexts.add(o);
      return o;
    }
    if (contextObject === undefined || contextObject === null) contextObject = {};
    if (typeof contextObject !== "object" && typeof contextObject !== "function") {
      throw argTypeError("contextObject", "of type object");
    }
    if (options !== undefined && (typeof options !== "object" || options === null)) {
      throw invArgType("options", "of type object", options);
    }
    const codegen = validateCodegen(options ? options.codeGeneration : undefined, "options.codeGeneration");
    if (isContextInternal(contextObject)) return contextObject;
    const rec = newRecord(contextObject);
    records.set(contextObject, rec);
    contexts.add(contextObject);
    applyCodegen(rec, codegen);
    return contextObject;
  }

  const normalizeOptions = (options) =>
    typeof options === "string" ? { filename: options } : (options || {});
  const toOptionsObject = (options) =>
    typeof options === "string" ? { filename: options } : options;

  function validateRunOptions(options) {
    if (options === undefined) return;
    if (typeof options !== "object" || options === null) {
      throw invArgType("options", "of type object", options);
    }
    if (options.timeout !== undefined) validateInt32(options.timeout, "options.timeout", 1);
    if (options.displayErrors !== undefined) validateBoolean(options.displayErrors, "options.displayErrors");
    if (options.breakOnSigint !== undefined) validateBoolean(options.breakOnSigint, "options.breakOnSigint");
  }

  function validateContextified(contextifiedObject) {
    if ((typeof contextifiedObject !== "object" && typeof contextifiedObject !== "function") ||
        contextifiedObject === null) {
      throw invArgType("contextifiedObject", "of type object", contextifiedObject);
    }
    if (!isContextInternal(contextifiedObject)) {
      const e = new TypeError('The "contextifiedObject" argument must be an vm.Context.');
      e.code = "ERR_INVALID_ARG_TYPE";
      throw e;
    }
  }

  function parseSourceMapURL(code) {
    const re = /(?:^|\n)[ \t]*\/\/[#@][ \t]+sourceMappingURL=([^\s'"]+)[ \t]*(?=\n|$)/g;
    let m, last;
    while ((m = re.exec(code)) !== null) last = m[1];
    return last;
  }

  function makeCachedDataBuffer() {
    const B = G.Buffer;
    return B ? B.from("mbun-vm-cache ") : new Uint8Array([1]);
  }

  class Script {
    constructor(code, options) {
      this.__code = `${code}`;
      if (options === undefined) options = {};
      else if (typeof options === "string") options = { filename: options };
      else if (typeof options !== "object" || options === null) {
        throw invArgType("options", "of type object", options);
      }
      const filename = options.filename;
      const lineOffset = options.lineOffset;
      const columnOffset = options.columnOffset;
      const cachedData = options.cachedData;
      const produceCachedData = options.produceCachedData;
      if (filename !== undefined) validateString(filename, "options.filename");
      if (lineOffset !== undefined) validateInt32(lineOffset, "options.lineOffset");
      if (columnOffset !== undefined) validateInt32(columnOffset, "options.columnOffset");
      if (cachedData !== undefined && !isArrayBufferView(cachedData)) {
        throw invArgType("options.cachedData", "an instance of Buffer, TypedArray, or DataView",
                         cachedData, "property");
      }
      if (produceCachedData !== undefined) validateBoolean(produceCachedData, "options.produceCachedData");

      this.__filename = filename === undefined ? "evalmachine.<anonymous>" : `${filename}`;
      this.sourceMapURL = parseSourceMapURL(this.__code);
      if (cachedData !== undefined) {
        this.cachedData = cachedData;
        this.cachedDataRejected = false;
      } else {
        this.cachedDataRejected = undefined;
      }
      if (produceCachedData === true) {
        this.cachedData = makeCachedDataBuffer();
        this.cachedDataProduced = true;
      }
      // Surface syntax errors at construction time, like node does.
      NVM.checkSyntax(this.__code, this.__filename);
    }
    runInThisContext(options) {
      validateRunOptions(options);
      return NVM.runInThis(this.__code, this.__filename);
    }
    runInContext(contextifiedObject, options) {
      validateContextified(contextifiedObject);
      validateRunOptions(options);
      return evalInContext(records.get(contextifiedObject), this.__code, this.__filename);
    }
    runInNewContext(contextObject, options) {
      const o = normalizeOptions(options);
      const ctx = createContext(contextObject, { codeGeneration: o.contextCodeGeneration });
      return this.runInContext(ctx, options);
    }
    createCachedData() {
      return makeCachedDataBuffer();
    }
  }

  function runInThisContext(code, options) {
    options = toOptionsObject(options);
    return new Script(code, options).runInThisContext(options);
  }
  function runInContext(code, contextifiedObject, options) {
    validateContextified(contextifiedObject);
    options = toOptionsObject(options);
    return new Script(code, options).runInContext(contextifiedObject, options);
  }
  function runInNewContext(code, contextObject, options) {
    if (contextObject !== undefined && contextObject !== DONT_CONTEXTIFY &&
        (typeof contextObject !== "object" || contextObject === null)) {
      throw argTypeError("contextObject", "of type object");
    }
    options = toOptionsObject(options);
    return new Script(code, options).runInNewContext(contextObject, options);
  }
  function createScript(code, options) {
    return new Script(code, options);
  }

  function compileFunction(code, params, options) {
    options = options || {};
    const args = Array.isArray(params) ? params.slice() : [];
    args.push(`${code}`);
    const pc = options.parsingContext;
    let FunctionCtor;
    if (pc !== undefined && pc !== null) {
      if (!isContextInternal(pc)) throw argTypeError("options.parsingContext", "an vm.Context");
      const rec = records.get(pc);
      syncIn(rec);
      FunctionCtor = NVM.runInContext(rec.handle, "Function", undefined, true);
    } else {
      FunctionCtor = Function;
    }
    return Reflect.apply(FunctionCtor, undefined, args);
  }

  function measureMemory() {
    return Promise.resolve({ total: { jsMemoryEstimate: 0, jsMemoryRange: [0, 0] } });
  }

  const constants = Object.freeze({ __proto__: null, DONT_CONTEXTIFY, USE_MAIN_CONTEXT_DEFAULT_LOADER });

  const vm = {
    Script,
    createContext,
    createScript,
    isContext,
    runInThisContext,
    runInContext,
    runInNewContext,
    compileFunction,
    measureMemory,
    constants,
  };
  // Internal seam for the node:vm.Module partition (node_vm_modules): it needs
  // to evaluate a wrapper *in* a contextified realm and to reach that realm's
  // global, both of which only this module knows how to do. Non-enumerable, so
  // it never shows up in a `vm` namespace snapshot.
  Object.defineProperty(vm, "__internal", {
    value: {
      runRaw(contextifiedObject, code, filename) {
        const rec = records.get(contextifiedObject);
        syncIn(rec);
        try {
          return NVM.runInContext(rec.handle, code, filename, true);
        } finally {
          syncOut(rec);
        }
      },
      globalOf(contextifiedObject) {
        const rec = records.get(contextifiedObject);
        return rec === undefined ? undefined : rec.global;
      },
    },
    enumerable: false,
    configurable: true,
  });
  M["vm"] = vm;
  M["node:vm"] = vm;
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
