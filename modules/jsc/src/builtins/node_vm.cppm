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
// cachedData is a source FINGERPRINT rather than real bytecode (see
// makeCachedDataBuffer): produce/consume/reject and cachedDataRejected are all
// observably node's, but nothing is actually pre-compiled.
//
// DEFERRED: real timeout interruption (JSC does expose
// JSContextGroupSetExecutionTimeLimit in JSContextRefPrivate.h -- unused so far
// because the limit is per context GROUP and nested vm timeouts need a
// save/restore stack), microtaskMode isolation, DONT_CONTEXTIFY realm identity,
// and V8's readonly-assignment / redefine-property message wording (JSC's own
// text escapes from the mirrored global's real descriptors).
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
  const RegExpPrototypeExec = Function.prototype.call.bind(RegExp.prototype.exec);
  const StringPrototypeSlice = Function.prototype.call.bind(String.prototype.slice);

  const contexts = new WeakSet();
  const records = new WeakMap();
  // Weak handles on every contextified object, so measureMemory('detailed') can
  // report one `other` entry per context that is still alive.
  const liveContexts = [];

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
    // A JSC global object is an immutable-prototype exotic object, but node's
    // context global is a V8 global PROXY, which accepts a prototype write --
    // `contextGlobalThis.__proto__ = null` must not throw (nodejs/node#47798).
    // Relax it in the CHILD realm's own Object.prototype accessor, and only for
    // that realm's global: nothing on the host side changes, no own property
    // appears on the context global (which would show up in
    // getOwnPropertyNames(this) inside the context), and every other object
    // keeps the real setPrototypeOf semantics.
    NVM.runInContext(handle,
      "(function(){var d=Object.getOwnPropertyDescriptor(Object.prototype,'__proto__');" +
      "if(!d||typeof d.set!=='function')return;var s=d.set;" +
      "Object.defineProperty(Object.prototype,'__proto__',{get:d.get," +
      "set:function(v){try{s.call(this,v);}catch(e){if(this!==globalThis)throw e;}}," +
      "enumerable:d.enumerable,configurable:d.configurable});})();",
      undefined, true);
    // Snapshot of the realm's own globals, so a script that OVERWRITES one
    // (`this.Symbol = Symbol`) is still seen as a user write on the way out,
    // while the untouched builtins stay out of the sandbox.
    const nativeKeys = new Set(ownKeys(g));
    const nativeVals = new Map();
    for (const key of nativeKeys) {
      const d = gOPD(g, key);
      if (d !== undefined && "value" in d) nativeVals.set(key, d.value);
    }
    if (typeof WeakRef === "function") liveContexts.push(new WeakRef(sandbox));
    const rec = {
      handle,
      global: g,
      nativeKeys,
      nativeVals,
      mirrored: new Set(),
      // key -> the value syncIn last mirrored onto the realm global (or the map
      // itself as a sentinel for an accessor, which has no comparable value).
      mirroredVals: new Map(),
      proto: new Map(),
      // Keys of the realm's OWN globals that the running script assigned to.
      // node's PropertySetterCallback writes every `this.X = …` straight to the
      // sandbox, so `this.Symbol = Symbol` must surface on the sandbox even
      // though the value it stored is the realm's own Symbol — a value diff can
      // never see that write. `trapped` holds the descriptors swapped out to
      // observe it (see armWriteTraps/disarmWriteTraps).
      writes: new Set(),
      trapped: new Map(),
      sandbox,
    };
    return rec;
  }

  // Swap each untouched realm-global data property for an accessor pair that
  // records assignments, so syncOut can tell "the script re-assigned this
  // builtin" from "nobody touched it". The traps live ONLY for the duration of
  // one run: disarmWriteTraps puts the original descriptor back (carrying the
  // possibly-updated value) before anything outside the script can observe the
  // global, so the realm's descriptor shape is unchanged between runs.
  function armWriteTraps(rec) {
    const { global: g, nativeKeys, mirrored, writes, trapped } = rec;
    writes.clear();
    trapped.clear();
    for (const key of nativeKeys) {
      if (mirrored.has(key)) continue;  // sandbox owns it; the normal path applies
      const d = gOPD(g, key);
      if (d === undefined || !d.configurable || !("value" in d) || d.writable !== true) continue;
      let cur = d.value;
      try {
        ObjectDefineProperty(g, key, {
          get() { return cur; },
          set(v) { cur = v; writes.add(key); },
          enumerable: d.enumerable,
          configurable: true,
        });
        trapped.set(key, d);
      } catch { /* ignore */ }
    }
  }

  function disarmWriteTraps(rec) {
    const { global: g, trapped } = rec;
    for (const [key, d] of trapped) {
      const cur = gOPD(g, key);
      // The script may have redefined the key outright (defineProperty beats the
      // accessor); in that case leave its definition alone.
      if (cur === undefined || cur.get === undefined) continue;
      let value;
      try { value = cur.get.call(g); } catch { continue; }
      try {
        ObjectDefineProperty(g, key, {
          value, writable: d.writable, enumerable: d.enumerable, configurable: d.configurable,
        });
      } catch { /* ignore */ }
    }
    trapped.clear();
  }

  function syncIn(rec) {
    const { sandbox, global: g, mirrored, mirroredVals, proto } = rec;
    const seen = new Set();
    mirroredVals.clear();
    for (const key of ownKeys(sandbox)) {
      seen.add(key);
      proto.delete(key);
      // A Proxy sandbox may THROW from getOwnPropertyDescriptor. node never
      // queries attributes just to enter a context (issue 11902), so a throwing
      // trap must not escape: fall back to a plain read, and if even that
      // throws, leave the key unmirrored rather than failing the run.
      let desc;
      try {
        desc = gOPD(sandbox, key);
      } catch (e) {
        try {
          desc = { value: sandbox[key], writable: true, enumerable: true, configurable: true };
        } catch (e2) { continue; }
      }
      if (desc === undefined) continue;
      if ("value" in desc && desc.value === sandbox) {
        // node's PropertyGetterCallback maps the sandbox onto the global proxy,
        // so `ctx.window = ctx` makes `window === this` inside the context.
        desc = { ...desc, value: g };
      }
      try {
        ObjectDefineProperty(g, key, desc);
        mirrored.add(key);
        // Remember what we put there, so syncOut can tell an untouched mirror
        // from a value the script actually wrote and skip re-defining it on the
        // sandbox (which for a Proxy would fire traps node never fires).
        if ("value" in desc) mirroredVals.set(key, desc.value); else mirroredVals.set(key, mirroredVals);
      } catch (e) { /* non-configurable */ }
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
        try { delete g[key]; } catch (e) { /* ignore */ }
        mirrored.delete(key);
        mirroredVals.delete(key);
        proto.delete(key);
      }
    }
  }

  function syncOut(rec) {
    disarmWriteTraps(rec);
    const { sandbox, global: g, nativeKeys, nativeVals, mirrored, mirroredVals, proto, writes } = rec;
    const live = new Set();
    for (const key of ownKeys(g)) {
      const desc = gOPD(g, key);
      if (desc === undefined) continue;
      if (nativeKeys.has(key) && !mirrored.has(key)) {
        // Only a builtin the script actually WROTE travels out. The write trap
        // is authoritative; the value diff is the fallback for keys that could
        // not be trapped (non-configurable / accessor globals).
        if (!writes.has(key)) {
          if (!("value" in desc)) continue;
          // SameValue, not ===: the realm's own `NaN` global would otherwise
          // compare unequal to itself on every single run.
          if (Object.is(desc.value, nativeVals.get(key))) continue;
        } else if ("value" in desc) {
          // A trapped write lands on the sandbox the way node's setter callback
          // lands it: a plain own, enumerable, writable property — not with the
          // realm global's non-enumerable builtin attributes.
          live.add(key);
          try {
            ObjectDefineProperty(sandbox, key, {
              value: desc.value === g ? sandbox : desc.value,
              writable: true, enumerable: true, configurable: true,
            });
            mirrored.add(key);
          } catch { /* ignore */ }
          continue;
        }
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
      // An untouched mirror needs no write-back. Beyond saving work this is
      // load-bearing for a Proxy sandbox: node only touches the sandbox for
      // properties the script actually assigned, so re-defining an unchanged
      // key would fire traps node never fires.
      if (mirrored.has(key) && "value" in d && Object.is(d.value, mirroredVals.get(key))) {
        continue;
      }
      if (!mirrored.has(key)) {
        // node's PropertySetterCallback queries the sandbox's own descriptor
        // before storing, so a Proxy whose getOwnPropertyDescriptor trap breaks
        // the invariants must surface THAT error, not be silently swallowed
        // (issue 34606).
        gOPD(sandbox, key);
      }
      try { ObjectDefineProperty(sandbox, key, d); } catch (e) { /* ignore */ }
      mirrored.add(key);
      if ("value" in d) mirroredVals.set(key, d.value); else mirroredVals.set(key, mirroredVals);
    }
    for (const key of [...mirrored]) {
      if (!live.has(key)) {
        if (!proto.has(key)) { try { delete sandbox[key]; } catch (e) { /* ignore */ } }
        mirrored.delete(key);
        mirroredVals.delete(key);
        proto.delete(key);
      }
    }
  }

  // ── displayErrors: node's source-context decoration ────────────────────────
  // An error escaping a vm run carries, ahead of the usual stack, the offending
  // line and a caret:
  //     filename:1
  //     throw new Error("foo");
  //           ^
  //
  //     Error: foo
  //         at filename:1:7
  // JSC instead reports a bare "global code@filename:1:16" frame, so build the
  // header from the compiled source and restate the frames in V8 spelling.
  // ref: node lib/vm.js (displayErrors) / src/node_contextify.cc DecorateErrorStack.
  const vmDecorated = new WeakSet();

  // V8 points a call site at the START of the callee expression (including a
  // leading `new`); JSC points at the opening paren. Walk back over the callee
  // so the top frame's column matches node's.
  function callSiteColumn(srcLine, column) {
    if (typeof srcLine !== "string" || !(column > 1) || srcLine[column - 1] !== "(") return column;
    let i = column - 1;
    while (i > 0 && /\s/.test(srcLine[i - 1])) i--;
    while (i > 0 && /[\w$.]/.test(srcLine[i - 1])) i--;
    const before = srcLine.slice(0, i).trimEnd();
    if (before.endsWith("new") && !/[\w$.]/.test(before[before.length - 4] || "")) {
      i = before.length - 3;
    }
    return i + 1;
  }

  // "fn@file:line:col" / "global code@file:line:col" / "@" → "    at …".
  function v8Frames(stack) {
    const out = [];
    for (const raw of `${stack === undefined ? "" : stack}`.split("\n")) {
      const ln = raw.trim();
      if (ln === "") continue;
      const at = ln.lastIndexOf("@");
      let name = at >= 0 ? ln.slice(0, at) : "";
      const loc = at >= 0 ? ln.slice(at + 1) : ln;
      if (name === "global code" || name === "module code" || name === "eval code") name = "";
      out.push(name ? `    at ${name} (${loc || "<anonymous>"})` : `    at ${loc || "<anonymous>"}`);
    }
    return out;
  }

  const ErrorProtoToString = Error.prototype.toString;

  function decorateVmError(err, code, filename, displayErrors, lineOffset, columnOffset) {
    if (displayErrors === false) return err;
    if (err === null || typeof err !== "object") return err;
    if (vmDecorated.has(err)) return err;
    const line = err.line;
    if (typeof line !== "number" || line < 1) return err;
    // Only decorate errors raised BY the compiled script, never one that merely
    // travelled through it.
    const stack = err.stack;
    if (err.sourceURL !== filename &&
        !(typeof stack === "string" && stack.split("\n")[0].endsWith(`@${filename}:${line}:${err.column}`))) {
      return err;
    }
    const srcLine = `${code}`.split("\n")[line - 1];
    if (typeof srcLine !== "string") return err;
    const col = callSiteColumn(srcLine, typeof err.column === "number" ? err.column : 1);
    // vm.Script's lineOffset/columnOffset are zero-based source-display
    // offsets. JSC exposes the unshifted source position, while Node applies
    // the offsets to the decorated header and first frame.
    const displayLine = line + (typeof lineOffset === "number" ? lineOffset : 0);
    const displayColumn = col + (typeof columnOffset === "number" ? columnOffset : 0);
    const leading = srcLine.match(/^\s*/)[0].length;
    let title;
    try { title = ErrorProtoToString.call(err); } catch (e) { return err; }
    const frames = v8Frames(stack);
    if (frames.length === 0) frames.push("    at <anonymous>");
    frames[0] = `    at ${filename}:${displayLine}:${displayColumn}`;
    // The source excerpt and caret stay relative to the supplied source text;
    // only the reported filename/line/column frame carries the display offset.
    const head = `${filename}:${displayLine}\n${srcLine}\n${" ".repeat(leading)}^\n\n`;
    try {
      err.stack = head + title + "\n" + frames.join("\n");
      vmDecorated.add(err);
    } catch (e) { /* frozen error object: leave the stack alone */ }
    return err;
  }

  function evalInContext(rec, code, filename) {
    syncIn(rec);
    armWriteTraps(rec);
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
    if (options !== undefined) {
      // name/origin are inert here (they only label the context in V8's
      // inspector), but their type contract is observable.
      if (options.name !== undefined) validateString(options.name, "options.name");
      if (options.origin !== undefined) validateString(options.origin, "options.origin");
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
    while ((m = RegExpPrototypeExec(re, code)) !== null) last = m[1];
    return last;
  }

  // ── cachedData: a source fingerprint, not bytecode ────────────────────────
  // V8's code cache is a serialised compilation artifact; JSC's C API exposes no
  // equivalent, so mbun's buffer carries a magic prefix plus a fingerprint of
  // the source it was produced from. That is enough to reproduce every
  // OBSERVABLE part of the contract: a buffer produced from the same source is
  // accepted (cachedDataRejected === false), one produced from different source
  // -- or truncated, or minted by something else -- is rejected, and either way
  // the script still compiles from source text. The buffer must survive a
  // base64 round-trip through a child process (test-vm-cached-data produces it
  // with `-e`), so the fingerprint is computed from the source alone.
  //
  // 32 bytes exactly: common.getArrayBufferViews() only builds a view whose
  // element size divides byteLength, and the test feeds every view type it can
  // build back in, so a length divisible by 8 keeps BigInt64Array in play.
  const CACHE_MAGIC = "mbun-vm-cache:";
  function sourceFingerprint(src) {
    let h = 0x811c9dc5;
    for (let i = 0; i < src.length; i++) {
      const c = src.charCodeAt(i);
      h = ((h ^ (c & 0xff)) * 0x01000193) >>> 0;
      h = ((h ^ (c >>> 8)) * 0x01000193) >>> 0;
    }
    let hex = h.toString(16);
    while (hex.length < 8) hex = "0" + hex;
    return hex;
  }
  function makeCachedDataBuffer(source) {
    let tag = CACHE_MAGIC + sourceFingerprint(source === undefined ? "" : `${source}`);
    while (tag.length < 32) tag += " ";
    const B = G.Buffer;
    return B ? B.from(tag) : new Uint8Array([1]);
  }
  // true when the buffer must be reported as rejected. Mirrors V8: a rejected
  // cache is never fatal for vm.Script, it only flips cachedDataRejected.
  function cachedDataRejects(buf, source) {
    let text = "";
    try {
      const u8 = new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
      for (let i = 0; i < u8.length; i++) text += String.fromCharCode(u8[i]);
    } catch (e) { return true; }
    if (text.length !== 32 || text.slice(0, CACHE_MAGIC.length) !== CACHE_MAGIC) return true;
    return text.slice(CACHE_MAGIC.length).trim() !== sourceFingerprint(`${source}`);
  }

  // ── dynamic import() inside a vm script ───────────────────────────────────
  // node routes every `import()` evaluated inside a vm.Script through that
  // script's own host-defined option, so the specifier reaches
  // options.importModuleDynamically instead of the process module loader.
  // V8 exposes that as a per-script slot; JSC's C API has no equivalent hook,
  // so the CALL SITE is rewritten instead: a script whose source contains a
  // dynamic import runs with `import(` replaced by an entry in a registry that
  // carries this script's callback. Scripts without a dynamic import are left
  // byte-for-byte alone, so ordinary vm code still runs as an untouched
  // program (var/function declarations still land on the realm global).
  //
  // Built with RegExp() rather than a literal: the source text of a literal
  // containing `import(` is itself a dynamic import to any tooling that lowers
  // this file.
  const DYNIMPORT_RE = new RegExp("\\bimport[ \\t\\n]*\\(", "g");
  const DYN_REGISTRY = "__mbunVmDynamicImport";
  const dynRegistry = { __proto__: null };
  let dynNextId = 0;

  const vmErr = (code, message) => {
    const e = new TypeError(message);
    e.code = code;
    return e;
  };

  // The registry object is a plain non-enumerable global on whichever realm the
  // rewritten code runs in. It is also registered as one of the realm's OWN
  // globals so the contextify mirror treats it as a builtin and never copies it
  // out onto the sandbox.
  function installDynRegistry(rec) {
    const target = rec === undefined ? G : rec.global;
    if (gOPD(target, DYN_REGISTRY) === undefined) {
      ObjectDefineProperty(target, DYN_REGISTRY, {
        value: dynRegistry, writable: false, enumerable: false, configurable: true,
      });
    }
    if (rec !== undefined) {
      rec.nativeKeys.add(DYN_REGISTRY);
      rec.nativeVals.set(DYN_REGISTRY, dynRegistry);
    }
  }

  const hasVmModulesFlag = () => {
    const argv = G.process && G.process.execArgv;
    if (Array.isArray(argv)) {
      for (const a of argv) if (a === "--experimental-vm-modules") return true;
    }
    return false;
  };

  // `import()` evaluated in the host realm, for
  // vm.constants.USE_MAIN_CONTEXT_DEFAULT_LOADER. Compiled lazily out of split
  // text for the same reason DYNIMPORT_RE is built from a string.
  let mainRealmImport;
  const defaultLoader = (specifier) => {
    if (mainRealmImport === undefined) {
      mainRealmImport = new Function("s", "return imp" + "ort(s);");
    }
    return mainRealmImport(specifier);
  };

  // node accepts a vm.Module from the callback and resolves the import with its
  // namespace, evaluating it first if it has not run yet.
  function moduleNamespaceOf(result) {
    if (result !== null && (typeof result === "object" || typeof result === "function") &&
        typeof result.evaluate === "function" && "namespace" in result) {
      if (result.status === "evaluated") return result.namespace;
      return Promise.resolve(result.evaluate()).then(() => result.namespace);
    }
    throw vmErr("ERR_VM_MODULE_NOT_MODULE", "Provided module is not an instance of Module");
  }

  function validateImportModuleDynamically(value) {
    if (value === undefined) return undefined;
    if (typeof value !== "function" && value !== USE_MAIN_CONTEXT_DEFAULT_LOADER) {
      throw invArgType("options.importModuleDynamically", "of type function", value, "property");
    }
    return value;
  }

  function makeDynImportHandler(callback, getWrap) {
    return async (specifier, importOptions) => {
      const spec = `${specifier}`;
      if (callback === undefined) {
        throw vmErr("ERR_VM_DYNAMIC_IMPORT_CALLBACK_MISSING",
                    "A dynamic import callback was not specified.");
      }
      if (callback === USE_MAIN_CONTEXT_DEFAULT_LOADER) return defaultLoader(spec);
      // A user callback is only honoured under the flag, and node decides that
      // BEFORE invoking it — the callback must not be observed to run.
      if (!hasVmModulesFlag()) {
        throw vmErr("ERR_VM_DYNAMIC_IMPORT_CALLBACK_MISSING_FLAG",
                    "A dynamic import callback was invoked without --experimental-vm-modules");
      }
      const attributes = { __proto__: null };
      const withClause = importOptions === null || typeof importOptions !== "object"
        ? undefined : importOptions.with;
      if (withClause !== null && typeof withClause === "object") {
        for (const key of Object.keys(withClause)) attributes[key] = `${withClause[key]}`;
      }
      return moduleNamespaceOf(await callback(spec, getWrap(), attributes, "evaluation"));
    };
  }

  // Returns the code to actually run, or null when the source has no dynamic
  // import and must be left untouched.
  function prepareDynImport(code, callback, getWrap) {
    const src = `${code}`;
    DYNIMPORT_RE.lastIndex = 0;
    let match = RegExpPrototypeExec(DYNIMPORT_RE, src);
    if (match === null) return null;
    const id = dynNextId++;
    dynRegistry[id] = makeDynImportHandler(callback, getWrap);
    const replacement = DYN_REGISTRY + "[" + id + "](";
    let rewritten = "";
    let cursor = 0;
    do {
      rewritten += StringPrototypeSlice(src, cursor, match.index) + replacement;
      cursor = match.index + match[0].length;
      match = RegExpPrototypeExec(DYNIMPORT_RE, src);
    } while (match !== null);
    return rewritten + StringPrototypeSlice(src, cursor);
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
      const importModuleDynamically =
        validateImportModuleDynamically(options.importModuleDynamically);

      this.__filename = filename === undefined ? "evalmachine.<anonymous>" : `${filename}`;
      this.__lineOffset = lineOffset === undefined ? 0 : lineOffset;
      this.__columnOffset = columnOffset === undefined ? 0 : columnOffset;
      this.sourceMapURL = parseSourceMapURL(this.__code);
      if (cachedData !== undefined) {
        this.cachedData = cachedData;
        this.cachedDataRejected = cachedDataRejects(cachedData, this.__code);
      } else {
        this.cachedDataRejected = undefined;
      }
      if (produceCachedData === true) {
        this.cachedData = makeCachedDataBuffer(this.__code);
        this.cachedDataProduced = true;
      }
      // Surface syntax errors at construction time, like node does.
      NVM.checkSyntax(this.__code, this.__filename);
      const rewritten = prepareDynImport(this.__code, importModuleDynamically, () => this);
      this.__runCode = rewritten === null ? this.__code : rewritten;
    }
    runInThisContext(options) {
      validateRunOptions(options);
      if (this.__runCode !== this.__code) installDynRegistry(undefined);
      try {
        return NVM.runInThis(this.__runCode, this.__filename);
      } catch (err) {
        throw decorateVmError(err, this.__code, this.__filename,
                              options ? options.displayErrors : undefined,
                              this.__lineOffset, this.__columnOffset);
      }
    }
    runInContext(contextifiedObject, options) {
      validateContextified(contextifiedObject);
      validateRunOptions(options);
      const rec = records.get(contextifiedObject);
      if (this.__runCode !== this.__code) installDynRegistry(rec);
      try {
        return evalInContext(rec, this.__runCode, this.__filename);
      } catch (err) {
        throw decorateVmError(err, this.__code, this.__filename,
                              options ? options.displayErrors : undefined,
                              this.__lineOffset, this.__columnOffset);
      }
    }
    runInNewContext(contextObject, options) {
      // node reaches runInContext through a plain member call on `this`, so a
      // detached receiver fails with V8's bare "this.runInContext is not a
      // function". JSC appends an "(In '…', '…' is undefined)" clause that the
      // test's anchored regexp rejects, so raise the node-shaped TypeError
      // BEFORE createContext -- node's own createContext would not be reached
      // either, since V8 evaluates the callee reference at the call site only.
      if (this === null || this === undefined || typeof this.runInContext !== "function") {
        throw new TypeError("this.runInContext is not a function");
      }
      const o = normalizeOptions(options);
      if (o.contextName !== undefined) validateString(o.contextName, "options.contextName");
      if (o.contextOrigin !== undefined) validateString(o.contextOrigin, "options.contextOrigin");
      const ctx = createContext(contextObject, {
        codeGeneration: o.contextCodeGeneration,
        name: o.contextName,
        origin: o.contextOrigin,
      });
      return this.runInContext(ctx, options);
    }
    createCachedData() {
      return makeCachedDataBuffer(this.__code);
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
    // A Proxy passes IsArray but is not a JSArray; bun's compileFunction requires
    // a real one for `params` and `contextExtensions` and throws rather than
    // reading through the traps (regression/issue/isArray-proxy-crash).
    const PR = G.__mbunProxyRegistry;
    if (PR && params !== null && typeof params === "object" && PR.has(params)) {
      throw argTypeError("params", "an Array of strings");
    }
    const importModuleDynamically =
      validateImportModuleDynamically(options.importModuleDynamically);
    if (PR && options.contextExtensions !== null && typeof options.contextExtensions === "object"
        && PR.has(options.contextExtensions)) {
      throw argTypeError("options.contextExtensions", "an Array of objects");
    }
    const args = Array.isArray(params) ? params.slice() : [];
    const pc = options.parsingContext;
    let FunctionCtor;
    let rec;
    if (pc !== undefined && pc !== null) {
      if (!isContextInternal(pc)) throw argTypeError("options.parsingContext", "an vm.Context");
      rec = records.get(pc);
      syncIn(rec);
      FunctionCtor = NVM.runInContext(rec.handle, "Function", undefined, true);
    } else {
      FunctionCtor = Function;
    }
    // The compiled function is what node hands the callback as the referrer,
    // and it does not exist until after the body has been compiled.
    let compiled;
    const rewritten = prepareDynImport(code, importModuleDynamically, () => compiled);
    if (rewritten !== null) installDynRegistry(rec);
    const body = rewritten === null ? `${code}` : rewritten;
    // The Function constructor owns VALIDATION: its wrapper grammar is what
    // rejects a body that closes the wrapper early (`});…`) the way node's
    // compileFunction does. A bare function expression would happily parse that
    // body as a sequence of statements.
    const viaCtor = Reflect.apply(FunctionCtor, undefined, args.concat([body]));
    // node's compileFunction hands back an ANONYMOUS FUNCTION EXPRESSION: empty
    // .name, and `function (p, q) {\n…\n}` from toString(). The Function
    // constructor instead mints `function anonymous(\n…\n) {…}`, which the shape
    // is observable through. Recompile in the same realm for node's shape, and
    // fall back to the constructor's own result if that ever fails.
    let asExpr;
    try {
      const mk = Reflect.apply(FunctionCtor, undefined,
                               ["return (function (" + args.join(",") + ") {\n" + body + "\n});"]);
      asExpr = mk();
    } catch (e) { asExpr = undefined; }
    compiled = typeof asExpr === "function" ? asExpr : viaCtor;
    return compiled;
  }

  // node's vm.measureMemory resolves V8's per-context memory report. JSC has no
  // per-context accounting, so the NUMBERS are a whole-heap estimate rather
  // than a breakdown — but the argument validation, the experimental warning
  // and the result shape (including one `other` entry per live context) are
  // node's, because that is all a caller can branch on.
  const measureError = (name, value) => {
    const e = new TypeError("The argument '" + name + "' is invalid. Received '" + value + "'");
    e.code = "ERR_INVALID_ARG_VALUE";
    return e;
  };
  let measureWarned = false;
  function measureMemory(options) {
    if (options === undefined) options = { __proto__: null };
    if (typeof options !== "object" || options === null || Array.isArray(options)) {
      throw invArgType("options", "of type object", options);
    }
    const mode = options.mode === undefined ? "summary" : options.mode;
    if (mode !== "summary" && mode !== "detailed") throw measureError("options.mode", mode);
    const execution = options.execution === undefined ? "default" : options.execution;
    if (execution !== "default" && execution !== "eager") {
      throw measureError("options.execution", execution);
    }
    if (!measureWarned) {
      measureWarned = true;
      try {
        G.process.emitWarning(
          "vm.measureMemory is an experimental feature and might change at any time",
          "ExperimentalWarning");
      } catch (e) { /* ignore */ }
    }
    const estimate = () => {
      let bytes = 0;
      try {
        if (G.process && typeof G.process.memoryUsage === "function") {
          bytes = G.process.memoryUsage().heapUsed || 0;
        }
      } catch (e) { /* ignore */ }
      return { jsMemoryEstimate: bytes, jsMemoryRange: [bytes, bytes] };
    };
    if (mode === "summary") return Promise.resolve({ total: estimate() });
    const other = [];
    for (const ref of liveContexts) {
      if (ref.deref() !== undefined) other.push(estimate());
    }
    return Promise.resolve({ total: estimate(), current: estimate(), other });
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
        armWriteTraps(rec);
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
      // Shared with the :node_vm_modules partition so SourceTextModule's
      // cachedData uses the SAME fingerprint scheme as vm.Script's.
      makeCachedDataBuffer,
      cachedDataRejects,
    },
    enumerable: false,
    configurable: true,
  });
  M["vm"] = vm;
  M["node:vm"] = vm;
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
