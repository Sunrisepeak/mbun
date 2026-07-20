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

  // ── Argument validation (mirrors node lib/internal/validators + vm.js) ──────
  // node throws ERR_INVALID_ARG_TYPE / ERR_OUT_OF_RANGE with these exact codes;
  // the tests probe err.code / err.name, so match those precisely.
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

  // isContext() as callers use it internally (never throws); the public vm.isContext
  // validates its argument first (see below).
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

  // codeGeneration / contextCodeGeneration: validate shape, return normalized flags.
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
  function applyCodegen(h, cg) {
    if (!cg) return;
    if (cg.strings === false) {
      // Disallow code generation from strings for this context by replacing the
      // realm's own `eval` with one that throws the context's EvalError (matching
      // node's --disallow-code-generation-from-strings observable behaviour).
      // Non-enumerable, so contextify-out never copies it back onto the sandbox.
      NVM.runInContext(h,
        "Object.defineProperty(globalThis,'eval',{value:function(){throw new EvalError('Code generation from strings disallowed for this context');},writable:true,enumerable:false,configurable:true});",
        undefined);
    }
    if (cg.wasm === false) {
      NVM.runInContext(h,
        "(function(){var CE=WebAssembly.CompileError;Object.defineProperty(WebAssembly,'Module',{value:function(){throw new CE('Wasm code generation disallowed in this context');},writable:true,enumerable:false,configurable:true});})();",
        undefined);
    }
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
    if (options !== undefined && (typeof options !== "object" || options === null)) {
      throw invArgType("options", "of type object", options);
    }
    const codegen = validateCodegen(options ? options.codeGeneration : undefined, "options.codeGeneration");
    if (isContextInternal(contextObject)) return contextObject;
    const h = NVM.createContext(contextObject);
    handles.set(contextObject, h);
    contexts.add(contextObject);
    applyCodegen(h, codegen);
    return contextObject;
  }

  const normalizeOptions = (options) =>
    typeof options === "string" ? { filename: options } : (options || {});
  // Top-level vm.runIn*() accept a bare filename string in the options slot;
  // normalize it to { filename } before it reaches the Script/method layer, which
  // (like node) requires an options object there.
  const toOptionsObject = (options) =>
    typeof options === "string" ? { filename: options } : options;

  // Run-time options for the runIn* methods: node validates the whole object and
  // the timeout / displayErrors / breakOnSigint members.
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

  // //# sourceMappingURL=  /  //@ sourceMappingURL= magic comment (last wins).
  function parseSourceMapURL(code) {
    const re = /(?:^|\n)[ \t]*\/\/[#@][ \t]+sourceMappingURL=([^\s'"]+)[ \t]*(?=\n|$)/g;
    let m, last;
    while ((m = re.exec(code)) !== null) last = m[1];
    return last;
  }

  function makeCachedDataBuffer() {
    // DEFERRED: no genuine V8 bytecode cache; return an opaque, non-empty Buffer
    // so `createCachedData() instanceof Buffer` and round-trips hold.
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
      // cachedData/bytecode is DEFERRED (no real V8 bytecode). We expose the shape
      // node scripts carry: a consumed Buffer is "accepted" (not rejected), and
      // produceCachedData yields a Buffer so round-trip callers observe the contract.
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
    }
    runInThisContext(options) {
      validateRunOptions(options);
      return NVM.runInThis(this.__code, this.__filename);
    }
    runInContext(contextifiedObject, options) {
      validateContextified(contextifiedObject);
      validateRunOptions(options);
      return NVM.runInContext(handles.get(contextifiedObject), this.__code, this.__filename);
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
      if (!isContextInternal(pc)) throw argTypeError("options.parsingContext", "an vm.Context");
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
    createScript,
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
