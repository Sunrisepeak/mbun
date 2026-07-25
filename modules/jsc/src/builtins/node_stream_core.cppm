// node:stream payload partition — registry + bun-intrinsic shims, then legacy/utils/state/destroy/end-of-stream/add-abort-signal/from.
//
// Mechanical 1:1 translation of bun-ref src/js/internal/streams/*.ts (三段法
// stage 1): TS types stripped and bun's $-intrinsics lowered onto the shims in
// node_stream_core.cppm; algorithm, branches and error text are the blueprint's,
// not reinvented. The blueprint's deliberately-lazy cycle-breaking requires
// (readable->compose, duplex->duplexify, pipeline->readable) are preserved and
// resolved by the CJS registry, so module init order matches bun's.
//
// NOTE: appended AFTER the master builtins IIFE has closed (see image_closure),
// so this is a self-contained IIFE that re-binds G = globalThis and pulls shared
// helpers off G.__mbunStreamReg.
export module mbun.jsc.js_builtins:node_stream_core;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeStreamCoreJS = R"JS(
// ---- node:stream registry + bun-intrinsic shims (partition 1 preamble) ----
// Mirrors bun's builtin environment for src/js/internal/streams/*: a CJS
// registry (lazy require => the blueprint's deliberate cycle-breaking lazy
// requires behave exactly as in bun), the $ERR_* factories, and the handful of
// native seams. Message text/arities verified against bun 1.3.14 verbatim.
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  if (!M) return;
  const process = G.process;

  const __mods = { __proto__: null };
  const __cache = { __proto__: null };
  function __req(id) {
    id = id.startsWith("node:") ? id.slice(5) : id;
    if (id in __cache) return __cache[id];
    const f = __mods[id];
    if (f) {
      // Seed the cache before running the body: CJS cycle semantics.
      const module = { exports: {} };
      __cache[id] = module.exports;
      f(__req, module, module.exports);
      __cache[id] = module.exports;
      return module.exports;
    }
    const m = M["node:" + id] || M[id];
    if (m) return (__cache[id] = m);
    throw new Error("Cannot find module '" + id + "'");
  }

  // ---- $ERR_* factories (bun ErrorCode.ts ctor types; text from bun 1.3.14) ----
  const mk = (Ctor, code, msg) => {
    const e = new Ctor(msg); e.code = code;
    // Node's NodeError bakes the code into toString(): "TypeError [ERR_x]: msg"
    // (stack/name stay the base name). assert.throws(fn, /ERR_x/) matches on
    // String(err), so the code must appear there.
    const base = e.name;
    Object.defineProperty(e, "toString", {
      value() { return `${base} [${code}]${this.message ? ": " + this.message : ""}`; },
      configurable: true, writable: true,
    });
    return e;
  };
  // determineSpecificType(): "type number (42)" / "an instance of Foo" / "null".
  const specific = (v) => {
    if (v === null) return "null";
    if (v === undefined) return "undefined";
    const t = typeof v;
    if (t === "object") {
      const n = v.constructor && v.constructor.name;
      return "an instance of " + (n || "Object");
    }
    if (t === "string") return "type string ('" + v + "')";
    if (t === "function") return "function " + (v.name || "anonymous");
    if (t === "bigint") return "type bigint (" + String(v) + "n)";
    if (t === "symbol") return "type symbol (" + String(v) + ")";
    return "type " + t + " (" + String(v) + ")";
  };
  // ["A","B","C"] -> "A, B, or C"; ["A","B"] -> "A or B"; ["A"] -> "A"
  const joinTypes = (t) => {
    if (!Array.isArray(t)) return String(t);
    if (t.length === 1) return String(t[0]);
    if (t.length === 2) return t[0] + " or " + t[1];
    return t.slice(0, -1).join(", ") + ", or " + t[t.length - 1];
  };
  const $ERR_INVALID_ARG_TYPE = (name, type, val) =>
    mk(TypeError, "ERR_INVALID_ARG_TYPE",
      `The ${name.endsWith(" argument") ? name : `"${name}" ${name.includes(".") ? "property" : "argument"}`} must be of type ${joinTypes(type)}. Received ${specific(val)}`);
  // ERR_INVALID_ARG_VALUE inspects the value (numbers/booleans print bare).
  const inspectVal = (v) =>
    typeof v === "string" ? `'${v}'`
      : typeof v === "number" || typeof v === "boolean" ? String(v)
      : v === null ? "null" : v === undefined ? "undefined" : specific(v);
  const $ERR_INVALID_ARG_VALUE = (name, value, reason = "is invalid") =>
    mk(TypeError, "ERR_INVALID_ARG_VALUE",
      `The ${name.includes(".") ? "property" : "argument"} '${name}' ${reason}. Received ${inspectVal(value)}`);
  // node errors.js:1471 declares ERR_INVALID_ARG_VALUE with a RangeError variant
  // (`E(..., TypeError, RangeError)`); stream/iter's consumers use it for an
  // out-of-domain options.encoding.
  const $ERR_INVALID_ARG_VALUE_RangeError = (name, value, reason = "is invalid") =>
    mk(RangeError, "ERR_INVALID_ARG_VALUE",
      `The ${name.includes(".") ? "property" : "argument"} '${name}' ${reason}. Received ${inspectVal(value)}`);
  // node errors.js:1554 `E('ERR_INVALID_STATE', 'Invalid state: %s', Error,
  // TypeError, RangeError)` — one code, three constructors, which stream/iter
  // uses to distinguish a protocol misuse (TypeError) from an out-of-range
  // backpressure decision (RangeError).
  const $ERR_INVALID_STATE = (msg) => mk(Error, "ERR_INVALID_STATE", `Invalid state: ${msg}`);
  const $ERR_INVALID_STATE_TypeError = (msg) => mk(TypeError, "ERR_INVALID_STATE", `Invalid state: ${msg}`);
  const $ERR_INVALID_STATE_RangeError = (msg) => mk(RangeError, "ERR_INVALID_STATE", `Invalid state: ${msg}`);
  // node errors.js:1646 `E('ERR_OPERATION_FAILED', 'Operation failed: %s', Error,
  // TypeError)`; stream/iter wraps a thrown non-Error in the TypeError variant.
  const $ERR_OPERATION_FAILED = (msg) => mk(TypeError, "ERR_OPERATION_FAILED", `Operation failed: ${msg}`);
  const $ERR_INVALID_RETURN_VALUE = (input, name, value) =>
    mk(TypeError, "ERR_INVALID_RETURN_VALUE",
      `Expected ${input} to be returned from the "${name}" function but got ${specific(value)}.`);
  const $ERR_OUT_OF_RANGE = (name, range, value) =>
    mk(RangeError, "ERR_OUT_OF_RANGE", `The value of "${name}" is out of range. It must be ${range}. Received ${value}`);
  const $ERR_MISSING_ARGS = (...a) =>
    mk(TypeError, "ERR_MISSING_ARGS", `The ${a.map(x => `"${x}"`).join(", ")} argument must be specified`);
  const $ERR_METHOD_NOT_IMPLEMENTED = (name) => mk(Error, "ERR_METHOD_NOT_IMPLEMENTED", `The ${name} method is not implemented`);
  const $ERR_ILLEGAL_CONSTRUCTOR = () => mk(TypeError, "ERR_ILLEGAL_CONSTRUCTOR", "Illegal constructor");
  const $ERR_MULTIPLE_CALLBACK = () => mk(Error, "ERR_MULTIPLE_CALLBACK", "Callback called multiple times");
  const $ERR_UNKNOWN_ENCODING = (enc) => mk(TypeError, "ERR_UNKNOWN_ENCODING", `Unknown encoding: ${enc}`);
  const $ERR_STREAM_DESTROYED = (name) => mk(Error, "ERR_STREAM_DESTROYED", `Cannot call ${name} after a stream was destroyed`);
  const $ERR_STREAM_ALREADY_FINISHED = (name) => mk(Error, "ERR_STREAM_ALREADY_FINISHED", `Cannot call ${name} after a stream was finished`);
  const $ERR_STREAM_WRITE_AFTER_END = () => mk(Error, "ERR_STREAM_WRITE_AFTER_END", "write after end");
  const $ERR_STREAM_NULL_VALUES = () => mk(TypeError, "ERR_STREAM_NULL_VALUES", "May not write null values to stream");
  const $ERR_STREAM_PREMATURE_CLOSE = () => mk(Error, "ERR_STREAM_PREMATURE_CLOSE", "Premature close");
  const $ERR_STREAM_PUSH_AFTER_EOF = () => mk(Error, "ERR_STREAM_PUSH_AFTER_EOF", "stream.push() after EOF");
  const $ERR_STREAM_UNSHIFT_AFTER_END_EVENT = () => mk(Error, "ERR_STREAM_UNSHIFT_AFTER_END_EVENT", "stream.unshift() after end event");
  const $ERR_STREAM_CANNOT_PIPE = () => mk(Error, "ERR_STREAM_CANNOT_PIPE", "Cannot pipe, not readable");
  const $ERR_STREAM_UNABLE_TO_PIPE = () => mk(Error, "ERR_STREAM_UNABLE_TO_PIPE", "Cannot pipe to a closed or destroyed stream");
  const $ERR_STREAM_ITER_MISSING_FLAG = () => mk(TypeError, "ERR_STREAM_ITER_MISSING_FLAG", "Missing flag for stream iteration");

  const $makeAbortError = (msg, opts) => {
    const e = new Error(msg || "The operation was aborted");
    e.name = "AbortError"; e.code = "ABORT_ERR";
    if (opts && "cause" in opts) e.cause = opts.cause;
    return e;
  };
  const $toClass = (fn, name, sup) => {
    if (sup) { Object.setPrototypeOf(fn.prototype, sup.prototype); Object.setPrototypeOf(fn, sup); }
    Object.defineProperty(fn, "name", { value: name, configurable: true });
    return fn;
  };
  const __isCallable = (f) => typeof f === "function";
  const __debug = () => {};
  const __assert = (cond, msg) => { if (!cond) throw new Error("Assertion failed" + (msg ? ": " + msg : "")); };

  // ---- native seams ----
  // bun resolves these against its own JSC-backed web stream classes; the
  // observable contract is an instanceof test against the global web classes.
  const $inheritsReadableStream = (o) => typeof G.ReadableStream === "function" && o instanceof G.ReadableStream;
  const $inheritsWritableStream = (o) => typeof G.WritableStream === "function" && o instanceof G.WritableStream;
  const $inheritsTransformStream = (o) => typeof G.TransformStream === "function" && o instanceof G.TransformStream;
  const $inheritsBlob = (o) => typeof G.Blob === "function" && o instanceof G.Blob;
  // eos()'s AsyncResource binding is an optimization keyed on there being an
  // active async context; mbun has no $asyncContext internal field, so report
  // "no context" (callback simply is not AsyncResource-wrapped).
  const __hasAsyncContext = () => false;
  // Only reached for a web stream lacking kIsClosedPromise. bun reads the
  // stream's internal closed promise; approximate with the reader's.
  // node's `stream[kIsClosedPromise].promise` — a per-stream promise that
  // settles on close/error and NEVER locks the stream. The old fallback here
  // acquired a reader/writer just to read `.closed`, so `finished(webStream)`
  // locked it and any later getReader()/getWriter()/`for await` threw
  // "ReadableStream is locked" (test-webstreams-finished, -compose,
  // -duplex-fromweb-*).
  const $webStreamClosedPromise = (stream) => {
    try {
      const S = G.__mbunStreams;
      if (S && typeof S.closedPromise === "function") return S.closedPromise(stream);
    } catch {}
    try {
      if (typeof stream.getReader === "function") return stream.getReader().closed;
      if (typeof stream.getWriter === "function") return stream.getWriter().closed;
    } catch {}
    return new Promise(() => {});
  };
  // $cpp("NodeModuleModule.cpp", "createStreamIterEnabledFlag") is bun's
  // write-once CLI bit for --experimental-stream-iter. It gates both the
  // Symbol.for("Stream.toAsyncStreamable") interop path on Readable and the
  // node:stream/iter + node:zlib/iter entry points (node_stream_iter_entry).
  // It cannot be resolved at image-evaluation time — process.execArgv does not
  // exist yet — which is exactly why node defers the check too
  // (internal/streams/readable.js:1819), so read execArgv on each call and let
  // the callers cache. Not a mutable seam: mbun derives execArgv from the raw
  // command line at startup (src/cli.cppm derive_exec_argv).
  const $cpp = (file, name) => {
    if (name !== "createStreamIterEnabledFlag") return false;
    const argv = (G.process && G.process.execArgv) || [];
    for (let i = 0; i < argv.length; i++) {
      if (argv[i] === "--experimental-stream-iter") return true;
    }
    return false;
  };

  const H = {
    $ERR_INVALID_ARG_TYPE, $ERR_INVALID_ARG_VALUE, $ERR_INVALID_RETURN_VALUE, $ERR_OUT_OF_RANGE,
    $ERR_MISSING_ARGS, $ERR_METHOD_NOT_IMPLEMENTED, $ERR_ILLEGAL_CONSTRUCTOR, $ERR_MULTIPLE_CALLBACK,
    $ERR_UNKNOWN_ENCODING, $ERR_STREAM_DESTROYED, $ERR_STREAM_ALREADY_FINISHED, $ERR_STREAM_WRITE_AFTER_END,
    $ERR_STREAM_NULL_VALUES, $ERR_STREAM_PREMATURE_CLOSE, $ERR_STREAM_PUSH_AFTER_EOF,
    $ERR_STREAM_UNSHIFT_AFTER_END_EVENT, $ERR_STREAM_CANNOT_PIPE, $ERR_STREAM_UNABLE_TO_PIPE,
    $ERR_STREAM_ITER_MISSING_FLAG, $makeAbortError, $toClass, __isCallable, __debug, __assert,
    $inheritsReadableStream, $inheritsWritableStream, $inheritsTransformStream, $inheritsBlob,
    __hasAsyncContext, $webStreamClosedPromise, $cpp,
    // stream/iter (internal/streams/iter/*)
    $ERR_INVALID_ARG_VALUE_RangeError, $ERR_INVALID_STATE, $ERR_INVALID_STATE_TypeError,
    $ERR_INVALID_STATE_RangeError, $ERR_OPERATION_FAILED,
  };
  G.__mbunStreamReg = { def: (id, fn) => { __mods[id] = fn; }, require: __req, H };

  // ---- internal/* shims (1:1 with bun-ref internal/{validators,shared,errors,primordials,abort_listener}) ----
  const validateFunction = (v, name) => { if (typeof v !== "function") throw $ERR_INVALID_ARG_TYPE(name, "function", v); };
  const validateAbortSignal = (signal, name) => {
    if (signal !== undefined && (signal === null || typeof signal !== "object" || !("aborted" in signal)))
      throw $ERR_INVALID_ARG_TYPE(name, "AbortSignal", signal);
  };
  const validateBoolean = (v, name) => { if (typeof v !== "boolean") throw $ERR_INVALID_ARG_TYPE(name, "boolean", v); };
  const validateObject = (v, name) => { if (v === null || Array.isArray(v) || typeof v !== "object") throw $ERR_INVALID_ARG_TYPE(name, "Object", v); };
  const validateInteger = (v, name, min = Number.MIN_SAFE_INTEGER, max = Number.MAX_SAFE_INTEGER) => {
    if (typeof v !== "number") throw $ERR_INVALID_ARG_TYPE(name, "number", v);
    if (!Number.isInteger(v)) throw $ERR_OUT_OF_RANGE(name, "an integer", v);
    if (v < min || v > max) throw $ERR_OUT_OF_RANGE(name, `>= ${min} && <= ${max}`, v);
  };
  // node's validateOneOf: ERR_INVALID_ARG_VALUE with "must be one of: ..." as the
  // reason (webstreams_adapters.ts:540 validates options.type through it).
  const validateOneOf = (value, name, oneOf) => {
    if (!oneOf.includes(value)) {
      const allowed = oneOf.map(v => (typeof v === "string" ? `'${v}'` : String(v))).join(", ");
      throw $ERR_INVALID_ARG_VALUE(name, value, "must be one of: " + allowed);
    }
  };
  __mods["internal/validators"] = (req, module) => {
    module.exports = { validateFunction, validateAbortSignal, validateBoolean, validateObject, validateInteger, validateOneOf };
  };
  __mods["internal/shared"] = (req, module) => {
    const kEmptyObject = Object.freeze(Object.create(null));
    function once(callback, { preserveReturnValue = false } = kEmptyObject) {
      let called = false, returnValue;
      return function (...args) {
        if (called) return returnValue;
        called = true;
        const result = callback.apply(this, args);
        returnValue = preserveReturnValue ? result : undefined;
        return result;
      };
    }
    module.exports = {
      once, kEmptyObject,
      kAutoDestroyed: Symbol("kAutoDestroyed"),
      kResistStopPropagation: Symbol("kResistStopPropagation"),
      kWeakHandler: Symbol("kWeak"),
    };
  };
  __mods["internal/errors"] = (req, module) => {
    function aggregateTwoErrors(innerError, outerError) {
      if (innerError && outerError && innerError !== outerError) {
        const outerErrors = outerError.errors;
        if (Array.isArray(outerErrors)) { outerErrors.push(innerError); return outerError; }
        const err = new AggregateError([outerError, innerError], outerError.message);
        err.code = outerError.code;
        return err;
      }
      return innerError || outerError;
    }
    module.exports = { aggregateTwoErrors };
  };
  __mods["internal/primordials"] = (req, module) => {
    // SafePromiseAllReturnVoid(promises[, mapFn]): Promise.all that resolves with
    // undefined; rejects on the first rejection (webstreams_adapters.ts:418,866).
    const SafePromiseAllReturnVoid = (promises, mapFn) =>
      Promise.all(mapFn ? Array.prototype.map.call(promises, mapFn) : promises).then(() => {});
    module.exports = {
      SafeSet: Set,
      SafePromiseAllReturnVoid,
      TypedArrayPrototypeGetBuffer: (ta) => ta.buffer,
      TypedArrayPrototypeGetByteOffset: (ta) => ta.byteOffset,
      TypedArrayPrototypeGetByteLength: (ta) => ta.byteLength,
    };
  };
  __mods["internal/abort_listener"] = (req, module) => {
    const { kResistStopPropagation } = req("internal/shared");
    function addAbortListener(signal, listener) {
      if (signal === undefined) throw $ERR_INVALID_ARG_TYPE("signal", "AbortSignal", signal);
      validateAbortSignal(signal, "signal");
      validateFunction(listener, "listener");
      let removeEventListener;
      if (signal.aborted) {
        queueMicrotask(() => listener());
      } else {
        signal.addEventListener("abort", listener, { once: true, [kResistStopPropagation]: true });
        removeEventListener = () => { signal.removeEventListener("abort", listener); };
      }
      return { [Symbol.dispose]() { removeEventListener?.(); } };
    }
    module.exports = { addAbortListener };
  };
  // internal/webstreams_adapters is defined by the node_stream_webadapters
  // partition (R.def), which is appended after this one.
})();

(function () {
  const G = globalThis;
  const R = G.__mbunStreamReg;
  if (!R) return;
  const process = G.process;
  const Buffer = G.Buffer;
  const { $ERR_INVALID_ARG_TYPE, $ERR_INVALID_ARG_VALUE, $ERR_INVALID_RETURN_VALUE, $ERR_OUT_OF_RANGE, $ERR_MISSING_ARGS, $ERR_METHOD_NOT_IMPLEMENTED, $ERR_ILLEGAL_CONSTRUCTOR, $ERR_MULTIPLE_CALLBACK, $ERR_UNKNOWN_ENCODING, $ERR_STREAM_DESTROYED, $ERR_STREAM_ALREADY_FINISHED, $ERR_STREAM_WRITE_AFTER_END, $ERR_STREAM_NULL_VALUES, $ERR_STREAM_PREMATURE_CLOSE, $ERR_STREAM_PUSH_AFTER_EOF, $ERR_STREAM_UNSHIFT_AFTER_END_EVENT, $ERR_STREAM_CANNOT_PIPE, $ERR_STREAM_UNABLE_TO_PIPE, $ERR_STREAM_ITER_MISSING_FLAG, $makeAbortError, $toClass, __isCallable, __debug, __assert, $inheritsReadableStream, $inheritsWritableStream, $inheritsTransformStream, $inheritsBlob, __hasAsyncContext, $webStreamClosedPromise, $cpp } = R.H;

  R.def("internal/streams/legacy", function (require, module, exports) {
    const EE = require("node:events");
    const { isArrayBufferView, isUint8Array } = require("node:util/types");
    const ReflectOwnKeys = Reflect.ownKeys;
    const ArrayIsArray = Array.isArray;
    function Stream(opts) {
      EE.call(this, opts);
    }
    $toClass(Stream, "Stream", EE);
    Stream.prototype.pipe = function(dest, options) {
      const source = this;
      function ondata(chunk) {
        if (dest.writable && dest.write(chunk) === false && source.pause) {
          source.pause();
        }
      }
      source.on("data", ondata);
      function ondrain() {
        if (source.readable && source.resume) {
          source.resume();
        }
      }
      dest.on("drain", ondrain);
      if (!dest._isStdio && (!options || options.end !== false)) {
        source.on("end", onend);
        source.on("close", onclose);
      }
      let didOnEnd = false;
      function onend() {
        if (didOnEnd)
          return;
        didOnEnd = true;
        dest.end();
      }
      function onclose() {
        if (didOnEnd)
          return;
        didOnEnd = true;
        if (typeof dest.destroy === "function")
          dest.destroy();
      }
      function onerror(er) {
        cleanup();
        if (EE.listenerCount(this, "error") === 0) {
          this.emit("error", er);
        }
      }
      prependListener(source, "error", onerror);
      prependListener(dest, "error", onerror);
      function cleanup() {
        source.removeListener("data", ondata);
        dest.removeListener("drain", ondrain);
        source.removeListener("end", onend);
        source.removeListener("close", onclose);
        source.removeListener("error", onerror);
        dest.removeListener("error", onerror);
        source.removeListener("end", cleanup);
        source.removeListener("close", cleanup);
        dest.removeListener("close", cleanup);
      }
      source.on("end", cleanup);
      source.on("close", cleanup);
      dest.on("close", cleanup);
      dest.emit("pipe", source);
      return dest;
    };
    Stream.prototype.eventNames = function eventNames() {
      const names = [];
      for (const key of ReflectOwnKeys(this._events)) {
        if (typeof this._events[key] === "function" || ArrayIsArray(this._events[key]) && this._events[key].length > 0) {
          names.push(key);
        }
      }
      return names;
    };
    function prependListener(emitter, event, fn) {
      if (typeof emitter.prependListener === "function")
        return emitter.prependListener(event, fn);
      let events, existing;
      if (!(events = emitter._events) || !(existing = events[event]))
        emitter.on(event, fn);
      else if (ArrayIsArray(existing))
        existing.unshift(fn);
      else
        events[event] = [fn, existing];
    }
    Stream._isArrayBufferView = isArrayBufferView;
    Stream._isUint8Array = isUint8Array;
    Stream._uint8ArrayToBuffer = function _uint8ArrayToBuffer(chunk) {
      return new Buffer(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    };
    module.exports = { Stream, prependListener };

  });

  R.def("internal/streams/utils", function (require, module, exports) {
    const SymbolFor = Symbol.for;
    const SymbolIterator = Symbol.iterator;
    const SymbolAsyncIterator = Symbol.asyncIterator;
    const kIsDestroyed = SymbolFor("nodejs.stream.destroyed");
    const kIsErrored = SymbolFor("nodejs.stream.errored");
    const kIsReadable = SymbolFor("nodejs.stream.readable");
    const kIsWritable = SymbolFor("nodejs.stream.writable");
    const kIsDisturbed = SymbolFor("nodejs.stream.disturbed");
    const kOnConstructed = Symbol("kOnConstructed");
    const kIsClosedPromise = SymbolFor("nodejs.webstream.isClosedPromise");
    const kControllerErrorFunction = SymbolFor("nodejs.webstream.controllerErrorFunction");
    const kState = Symbol("kState");
    const kObjectMode = 1 << 0;
    const kErrorEmitted = 1 << 1;
    const kAutoDestroy = 1 << 2;
    const kEmitClose = 1 << 3;
    const kDestroyed = 1 << 4;
    const kClosed = 1 << 5;
    const kCloseEmitted = 1 << 6;
    const kErrored = 1 << 7;
    const kConstructed = 1 << 8;
    function isReadableNodeStream(obj, strict = false) {
      return !!(obj && typeof obj.pipe === "function" && typeof obj.on === "function" && (!strict || typeof obj.pause === "function" && typeof obj.resume === "function") && (!obj._writableState || obj._readableState?.readable !== false) && (!obj._writableState || obj._readableState));
    }
    function isWritableNodeStream(obj) {
      return !!(obj && typeof obj.write === "function" && typeof obj.on === "function" && (!obj._readableState || obj._writableState?.writable !== false));
    }
    function isDuplexNodeStream(obj) {
      return !!(obj && typeof obj.pipe === "function" && obj._readableState && typeof obj.on === "function" && typeof obj.write === "function");
    }
    function isNodeStream(obj) {
      return obj && (obj._readableState || obj._writableState || typeof obj.write === "function" && typeof obj.on === "function" || typeof obj.pipe === "function" && typeof obj.on === "function");
    }
    function isReadableStream(obj) {
      return $inheritsReadableStream(obj);
    }
    function isWritableStream(obj) {
      return $inheritsWritableStream(obj);
    }
    function isTransformStream(obj) {
      return $inheritsTransformStream(obj);
    }
    function isWebStream(obj) {
      return isReadableStream(obj) || isWritableStream(obj) || isTransformStream(obj);
    }
    function isIterable(obj, isAsync) {
      if (obj == null)
        return false;
      if (isAsync === true)
        return typeof obj[SymbolAsyncIterator] === "function";
      if (isAsync === false)
        return typeof obj[SymbolIterator] === "function";
      return typeof obj[SymbolAsyncIterator] === "function" || typeof obj[SymbolIterator] === "function";
    }
    function isDestroyed(stream) {
      if (!isNodeStream(stream))
        return null;
      const wState = stream._writableState;
      const rState = stream._readableState;
      const state = wState || rState;
      return !!(stream.destroyed || stream[kIsDestroyed] || state?.destroyed);
    }
    function isWritableEnded(stream) {
      if (!isWritableNodeStream(stream))
        return null;
      if (stream.writableEnded === true)
        return true;
      const wState = stream._writableState;
      if (wState?.errored)
        return false;
      if (typeof wState?.ended !== "boolean")
        return null;
      return wState.ended;
    }
    function isWritableFinished(stream, strict) {
      if (!isWritableNodeStream(stream))
        return null;
      if (stream.writableFinished === true)
        return true;
      const wState = stream._writableState;
      if (wState?.errored)
        return false;
      if (typeof wState?.finished !== "boolean")
        return null;
      return !!(wState.finished || strict === false && wState.ended === true && wState.length === 0);
    }
    function isReadableEnded(stream) {
      if (!isReadableNodeStream(stream))
        return null;
      if (stream.readableEnded === true)
        return true;
      const rState = stream._readableState;
      if (!rState || rState.errored)
        return false;
      if (typeof rState?.ended !== "boolean")
        return null;
      return rState.ended;
    }
    function isReadableFinished(stream, strict) {
      if (!isReadableNodeStream(stream))
        return null;
      const rState = stream._readableState;
      if (rState?.errored)
        return false;
      if (typeof rState?.endEmitted !== "boolean")
        return null;
      return !!(rState.endEmitted || strict === false && rState.ended === true && rState.length === 0);
    }
    function isReadable(stream) {
      if (stream && stream[kIsReadable] != null)
        return stream[kIsReadable];
      if (typeof stream?.readable !== "boolean")
        return null;
      if (isDestroyed(stream))
        return false;
      return isReadableNodeStream(stream) && stream.readable && !isReadableFinished(stream);
    }
    function isWritable(stream) {
      if (stream && stream[kIsWritable] != null)
        return stream[kIsWritable];
      if (typeof stream?.writable !== "boolean")
        return null;
      if (isDestroyed(stream))
        return false;
      return isWritableNodeStream(stream) && stream.writable && !isWritableEnded(stream);
    }
    function isFinished(stream, opts) {
      if (!isNodeStream(stream)) {
        return null;
      }
      if (isDestroyed(stream)) {
        return true;
      }
      if (opts?.readable !== false && isReadable(stream)) {
        return false;
      }
      if (opts?.writable !== false && isWritable(stream)) {
        return false;
      }
      return true;
    }
    function isWritableErrored(stream) {
      if (!isNodeStream(stream)) {
        return null;
      }
      const writableErrored = stream.writableErrored;
      if (writableErrored) {
        return writableErrored;
      }
      return stream._writableState?.errored ?? null;
    }
    function isReadableErrored(stream) {
      if (!isNodeStream(stream)) {
        return null;
      }
      const readableErrored = stream.readableErrored;
      if (readableErrored) {
        return readableErrored;
      }
      return stream._readableState?.errored ?? null;
    }
    function isClosed(stream) {
      if (!isNodeStream(stream)) {
        return null;
      }
      const closed = stream.closed;
      if (typeof closed === "boolean") {
        return closed;
      }
      const wState = stream._writableState;
      const rState = stream._readableState;
      if (typeof wState?.closed === "boolean" || typeof rState?.closed === "boolean") {
        return wState?.closed || rState?.closed;
      }
      const _closed = stream._closed;
      if (typeof _closed === "boolean" && isOutgoingMessage(stream)) {
        return _closed;
      }
      return null;
    }
    function isOutgoingMessage(stream) {
      return typeof stream._closed === "boolean" && typeof stream._defaultKeepAlive === "boolean" && typeof stream._removedConnection === "boolean" && typeof stream._removedContLen === "boolean";
    }
    function isServerResponse(stream) {
      return typeof stream._sent100 === "boolean" && isOutgoingMessage(stream);
    }
    function isServerRequest(stream) {
      return typeof stream._consuming === "boolean" && typeof stream._dumped === "boolean" && stream.req?.upgradeOrConnect === undefined;
    }
    function willEmitClose(stream) {
      if (!isNodeStream(stream))
        return null;
      const wState = stream._writableState;
      const rState = stream._readableState;
      const state = wState || rState;
      return !state && isServerResponse(stream) || !!(state?.autoDestroy && state.emitClose && state.closed === false);
    }
    function isDisturbed(stream) {
      return !!(stream && (stream[kIsDisturbed] ?? (stream.readableDidRead || stream.readableAborted)));
    }
    function isErrored(stream) {
      return !!(stream && (stream[kIsErrored] ?? stream.readableErrored ?? stream.writableErrored ?? stream._readableState?.errorEmitted ?? stream._writableState?.errorEmitted ?? stream._readableState?.errored ?? stream._writableState?.errored));
    }
    module.exports = {
      kOnConstructed,
      isDestroyed,
      kIsDestroyed,
      isDisturbed,
      kIsDisturbed,
      isErrored,
      kIsErrored,
      isReadable,
      kIsReadable,
      kIsClosedPromise,
      kControllerErrorFunction,
      kIsWritable,
      isClosed,
      isDuplexNodeStream,
      isFinished,
      isIterable,
      isReadableNodeStream,
      isReadableStream,
      isReadableEnded,
      isReadableFinished,
      isReadableErrored,
      isNodeStream,
      isWebStream,
      isWritable,
      isWritableNodeStream,
      isWritableStream,
      isWritableEnded,
      isWritableFinished,
      isWritableErrored,
      isServerRequest,
      isServerResponse,
      willEmitClose,
      isTransformStream,
      kState,
      kObjectMode,
      kErrorEmitted,
      kAutoDestroy,
      kEmitClose,
      kDestroyed,
      kClosed,
      kCloseEmitted,
      kErrored,
      kConstructed
    };

  });

  R.def("internal/streams/state", function (require, module, exports) {
    const { validateInteger } = require("internal/validators");
    const NumberIsInteger = Number.isInteger;
    const MathFloor = Math.floor;
    let defaultHighWaterMarkBytes = process.platform === "win32" ? 16 * 1024 : 64 * 1024;
    let defaultHighWaterMarkObjectMode = 16;
    function highWaterMarkFrom(options, isDuplex, duplexKey) {
      return options.highWaterMark != null ? options.highWaterMark : isDuplex ? options[duplexKey] : null;
    }
    function getDefaultHighWaterMark(objectMode = false) {
      return objectMode ? defaultHighWaterMarkObjectMode : defaultHighWaterMarkBytes;
    }
    function setDefaultHighWaterMark(objectMode, value) {
      validateInteger(value, "value", 0);
      if (objectMode) {
        defaultHighWaterMarkObjectMode = value;
      } else {
        defaultHighWaterMarkBytes = value;
      }
    }
    function getHighWaterMark(state, options, duplexKey, isDuplex) {
      const hwm = highWaterMarkFrom(options, isDuplex, duplexKey);
      if (hwm != null) {
        if (!NumberIsInteger(hwm) || hwm < 0) {
          const name = isDuplex ? `options.${duplexKey}` : "options.highWaterMark";
          throw $ERR_INVALID_ARG_VALUE(name, hwm);
        }
        return MathFloor(hwm);
      }
      return getDefaultHighWaterMark(state.objectMode);
    }
    module.exports = {
      getHighWaterMark,
      getDefaultHighWaterMark,
      setDefaultHighWaterMark
    };

  });

  R.def("internal/streams/destroy", function (require, module, exports) {
    const { aggregateTwoErrors } = require("internal/errors");
    const {
      kIsDestroyed,
      isDestroyed,
      isFinished,
      isServerRequest,
      kState,
      kErrorEmitted,
      kEmitClose,
      kClosed,
      kCloseEmitted,
      kConstructed,
      kDestroyed,
      kAutoDestroy,
      kErrored
    } = require("internal/streams/utils");
    const ProcessNextTick = process.nextTick;
    const kDestroy = Symbol("kDestroy");
    const kConstruct = Symbol("kConstruct");
    function checkError(err, w, r) {
      if (err) {
        err.stack;
        if (w && !w.errored) {
          w.errored = err;
        }
        if (r && !r.errored) {
          r.errored = err;
        }
      }
    }
    function destroy(err, cb) {
      const r = this._readableState;
      const w = this._writableState;
      const s = w || r;
      if (w && (w[kState] & kDestroyed) !== 0 || r && (r[kState] & kDestroyed) !== 0) {
        if (typeof cb === "function") {
          cb();
        }
        return this;
      }
      checkError(err, w, r);
      if (w) {
        w[kState] |= kDestroyed;
      }
      if (r) {
        r[kState] |= kDestroyed;
      }
      if ((s[kState] & kConstructed) === 0) {
        this.once(kDestroy, function(er) {
          _destroy(this, aggregateTwoErrors(er, err), cb);
        });
      } else {
        _destroy(this, err, cb);
      }
      return this;
    }
    function _destroy(self, err, cb) {
      let called = false;
      function onDestroy(err) {
        if (called) {
          return;
        }
        called = true;
        const r = self._readableState;
        const w = self._writableState;
        checkError(err, w, r);
        if (w) {
          w[kState] |= kClosed;
        }
        if (r) {
          r[kState] |= kClosed;
        }
        if (typeof cb === "function") {
          cb(err);
        }
        if (err) {
          ProcessNextTick(emitErrorCloseNT, self, err);
        } else {
          ProcessNextTick(emitCloseNT, self);
        }
      }
      try {
        self._destroy(err || null, onDestroy);
      } catch (err) {
        onDestroy(err);
      }
    }
    function emitErrorCloseNT(self, err) {
      emitErrorNT(self, err);
      emitCloseNT(self);
    }
    function emitCloseNT(self) {
      const r = self._readableState;
      const w = self._writableState;
      if (w) {
        w[kState] |= kCloseEmitted;
      }
      if (r) {
        r[kState] |= kCloseEmitted;
      }
      if (w && (w[kState] & kEmitClose) !== 0 || r && (r[kState] & kEmitClose) !== 0) {
        self.emit("close");
      }
    }
    function emitErrorNT(self, err) {
      const r = self._readableState;
      const w = self._writableState;
      if (w && (w[kState] & kErrorEmitted) !== 0 || r && (r[kState] & kErrorEmitted) !== 0) {
        return;
      }
      if (w) {
        w[kState] |= kErrorEmitted;
      }
      if (r) {
        r[kState] |= kErrorEmitted;
      }
      self.emit("error", err);
    }
    function undestroy() {
      const r = this._readableState;
      const w = this._writableState;
      if (r) {
        r.constructed = true;
        r.closed = false;
        r.closeEmitted = false;
        r.destroyed = false;
        r.errored = null;
        r.errorEmitted = false;
        r.reading = false;
        r.ended = r.readable === false;
        r.endEmitted = r.readable === false;
      }
      if (w) {
        w.constructed = true;
        w.destroyed = false;
        w.closed = false;
        w.closeEmitted = false;
        w.errored = null;
        w.errorEmitted = false;
        w.finalCalled = false;
        w.prefinished = false;
        w.ended = w.writable === false;
        w.ending = w.writable === false;
        w.finished = w.writable === false;
      }
    }
    function errorOrDestroy(stream, err, sync) {
      const r = stream._readableState;
      const w = stream._writableState;
      if (w && (w[kState] ? (w[kState] & kDestroyed) !== 0 : w.destroyed) || r && (r[kState] ? (r[kState] & kDestroyed) !== 0 : r.destroyed)) {
        return this;
      }
      if (r && (r[kState] & kAutoDestroy) !== 0 || w && (w[kState] & kAutoDestroy) !== 0) {
        stream.destroy(err);
      } else if (err) {
        err.stack;
        if (w && (w[kState] & kErrored) === 0) {
          w.errored = err;
        }
        if (r && (r[kState] & kErrored) === 0) {
          r.errored = err;
        }
        if (sync) {
          ProcessNextTick(emitErrorNT, stream, err);
        } else {
          emitErrorNT(stream, err);
        }
      }
    }
    function construct(stream, cb) {
      if (typeof stream._construct !== "function") {
        return;
      }
      const r = stream._readableState;
      const w = stream._writableState;
      if (r) {
        r[kState] &= ~kConstructed;
      }
      if (w) {
        w[kState] &= ~kConstructed;
      }
      stream.once(kConstruct, cb);
      if (stream.listenerCount(kConstruct) > 1) {
        return;
      }
      ProcessNextTick(constructNT, stream);
    }
    function constructNT(stream) {
      let called = false;
      function onConstruct(err) {
        if (called) {
          errorOrDestroy(stream, err ?? $ERR_MULTIPLE_CALLBACK());
          return;
        }
        called = true;
        const r = stream._readableState;
        const w = stream._writableState;
        const s = w || r;
        if (r) {
          r[kState] |= kConstructed;
        }
        if (w) {
          w[kState] |= kConstructed;
        }
        if (s.destroyed) {
          stream.emit(kDestroy, err);
        } else if (err) {
          errorOrDestroy(stream, err, true);
        } else {
          stream.emit(kConstruct);
        }
      }
      try {
        stream._construct((err) => {
          ProcessNextTick(onConstruct, err);
        });
      } catch (err) {
        ProcessNextTick(onConstruct, err);
      }
    }
    function isRequest(stream) {
      return stream?.setHeader && typeof stream.abort === "function";
    }
    function emitCloseLegacy(stream) {
      stream.emit("close");
    }
    function emitErrorCloseLegacy(stream, err) {
      stream.emit("error", err);
      ProcessNextTick(emitCloseLegacy, stream);
    }
    function destroyer(stream, err) {
      if (!stream || isDestroyed(stream)) {
        return;
      }
      if (!err && !isFinished(stream)) {
        err = $makeAbortError();
      }
      if (isServerRequest(stream)) {
        stream.socket = null;
        stream.destroy(err);
      } else if (isRequest(stream)) {
        stream.abort();
      } else {
        const req = stream.req;
        if (isRequest(req)) {
          req.abort();
        } else if (typeof stream.destroy === "function") {
          stream.destroy(err);
        } else if (typeof stream.close === "function") {
          stream.close();
        } else if (err) {
          ProcessNextTick(emitErrorCloseLegacy, stream, err);
        } else {
          ProcessNextTick(emitCloseLegacy, stream);
        }
      }
      if (!stream.destroyed) {
        stream[kIsDestroyed] = true;
      }
    }
    module.exports = {
      construct,
      destroyer,
      destroy,
      undestroy,
      errorOrDestroy
    };

  });

  R.def("internal/streams/end-of-stream", function (require, module, exports) {
    const { kEmptyObject, once } = require("internal/shared");
    const { validateAbortSignal, validateFunction, validateObject, validateBoolean } = require("internal/validators");
    const {
      isClosed,
      isReadable,
      isReadableNodeStream,
      isReadableStream,
      isReadableFinished,
      isReadableErrored,
      isWritable,
      isWritableNodeStream,
      isWritableStream,
      isWritableFinished,
      isWritableErrored,
      isNodeStream,
      willEmitClose: _willEmitClose,
      kIsClosedPromise
    } = require("internal/streams/utils");
    const SymbolDispose = Symbol.dispose;
    const PromisePrototypeThen = Promise.prototype.then;
    let addAbortListener;
    let AsyncResource;
    function isRequest(stream) {
      return stream.setHeader && typeof stream.abort === "function";
    }
    const nop = () => {};
    function bindAsyncResource(fn, type) {
      AsyncResource ??= require("node:async_hooks").AsyncResource;
      const resource = new AsyncResource(type);
      return function(...args) {
        return resource.runInAsyncScope(fn, this, ...args);
      };
    }
    function hasAsyncContext() {
      return __hasAsyncContext();
    }
    function getEosErrored(stream) {
      const errored = isWritableErrored(stream) || isReadableErrored(stream);
      return typeof errored !== "boolean" && errored || null;
    }
    function getEosOnCloseError(stream, readable, readableFinished, writable, writableFinished) {
      const errored = getEosErrored(stream);
      if (errored) {
        return errored;
      }
      if (readable && !readableFinished && isReadableNodeStream(stream, true)) {
        if (!isReadableFinished(stream, false)) {
          return $ERR_STREAM_PREMATURE_CLOSE();
        }
      }
      if (writable && !writableFinished) {
        if (!isWritableFinished(stream, false)) {
          return $ERR_STREAM_PREMATURE_CLOSE();
        }
      }
      return null;
    }
    const kEosNodeSynchronousCallback = Symbol("kEosNodeSynchronousCallback");
    function eos(stream, options, callback) {
      if (arguments.length === 2) {
        callback = options;
        options = kEmptyObject;
      } else if (options == null) {
        options = kEmptyObject;
      } else {
        validateObject(options, "options");
      }
      validateFunction(callback, "callback");
      validateAbortSignal(options.signal, "options.signal");
      if (isReadableStream(stream) || isWritableStream(stream)) {
        return eosWeb(stream, options, callback);
      }
      if (!isNodeStream(stream)) {
        throw $ERR_INVALID_ARG_TYPE("stream", ["ReadableStream", "WritableStream", "Stream"], stream);
      }
      const readable = options.readable ?? isReadableNodeStream(stream);
      const writable = options.writable ?? isWritableNodeStream(stream);
      let willEmitClose = _willEmitClose(stream) && isReadableNodeStream(stream) === readable && isWritableNodeStream(stream) === writable;
      let writableFinished = isWritableFinished(stream, false);
      let readableFinished = isReadableFinished(stream, false);
      const wState = stream._writableState;
      const rState = stream._readableState;
      let immediateResult;
      if (isClosed(stream)) {
        immediateResult = getEosOnCloseError(stream, readable, readableFinished, writable, writableFinished);
      } else if (wState?.errorEmitted || rState?.errorEmitted) {
        if (!willEmitClose) {
          immediateResult = getEosErrored(stream);
        }
      } else if (!readable && (!willEmitClose || isReadable(stream)) && (writableFinished || isWritable(stream) === false) && (wState == null || wState.pendingcb === undefined || wState.pendingcb === 0)) {
        immediateResult = getEosErrored(stream);
      } else if (!writable && (!willEmitClose || isWritable(stream)) && (readableFinished || isReadable(stream) === false)) {
        immediateResult = getEosErrored(stream);
      } else if (rState && stream.req && stream.aborted) {
        immediateResult = getEosErrored(stream);
      }
      let cleanup = () => {
        callback = nop;
      };
      if (immediateResult !== undefined) {
        if (options.error !== false) {
          stream.on("error", nop);
          cleanup = () => {
            callback = nop;
            stream.removeListener("error", nop);
          };
        }
      } else {
        const signal = options.signal;
        if (signal?.aborted) {
          immediateResult = $makeAbortError(undefined, { cause: signal.reason });
        }
      }
      const invokeImmediate = () => {
        if (immediateResult === null) {
          callback.call(stream);
        } else {
          callback.call(stream, immediateResult);
        }
      };
      if (immediateResult !== undefined && options[kEosNodeSynchronousCallback]) {
        invokeImmediate();
        return cleanup;
      }
      if (hasAsyncContext()) {
        callback = bindAsyncResource(callback, "STREAM_END_OF_STREAM");
      }
      if (immediateResult !== undefined) {
        process.nextTick(invokeImmediate);
        return cleanup;
      }
      callback = once(callback);
      const onlegacyfinish = () => {
        if (!stream.writable) {
          onfinish();
        }
      };
      const onfinish = () => {
        writableFinished = true;
        if (stream.destroyed) {
          willEmitClose = false;
        }
        if (willEmitClose && (!stream.readable || readable)) {
          return;
        }
        if (!readable || readableFinished) {
          callback.call(stream);
        }
      };
      const onend = () => {
        readableFinished = true;
        if (stream.destroyed) {
          willEmitClose = false;
        }
        if (willEmitClose && (!stream.writable || writable)) {
          return;
        }
        if (!writable || writableFinished) {
          callback.call(stream);
        }
      };
      const onerror = (err) => {
        callback.call(stream, err);
      };
      const onclose = () => {
        const error = getEosOnCloseError(stream, readable, readableFinished, writable, writableFinished);
        if (error === null) {
          callback.call(stream);
        } else {
          callback.call(stream, error);
        }
      };
      const onrequest = () => {
        stream.req.on("finish", onfinish);
      };
      if (isRequest(stream)) {
        stream.on("complete", onfinish);
        if (!willEmitClose) {
          stream.on("abort", onclose);
        }
        if (stream.req) {
          onrequest();
        } else {
          stream.on("request", onrequest);
        }
      } else if (writable && !wState) {
        stream.on("end", onlegacyfinish);
        stream.on("close", onlegacyfinish);
      }
      if (!willEmitClose && typeof stream.aborted === "boolean") {
        stream.on("aborted", onclose);
      }
      stream.on("end", onend);
      stream.on("finish", onfinish);
      if (options.error !== false) {
        stream.on("error", onerror);
      }
      stream.on("close", onclose);
      cleanup = () => {
        callback = nop;
        stream.removeListener("aborted", onclose);
        stream.removeListener("complete", onfinish);
        stream.removeListener("abort", onclose);
        stream.removeListener("request", onrequest);
        const streamReq = stream.req;
        if (streamReq)
          streamReq.removeListener("finish", onfinish);
        stream.removeListener("end", onlegacyfinish);
        stream.removeListener("close", onlegacyfinish);
        stream.removeListener("finish", onfinish);
        stream.removeListener("end", onend);
        stream.removeListener("error", onerror);
        stream.removeListener("close", onclose);
      };
      const signal = options.signal;
      if (signal) {
        const abort = () => {
          const endCallback = callback;
          cleanup();
          endCallback.call(stream, $makeAbortError(undefined, { cause: signal.reason }));
        };
        addAbortListener ??= require("internal/abort_listener").addAbortListener;
        const disposable = addAbortListener(signal, abort);
        const originalCallback = callback;
        callback = once((...args) => {
          disposable[SymbolDispose]();
          originalCallback.apply(stream, args);
        });
      }
      return cleanup;
    }
    function eosWeb(stream, options, callback) {
      if (hasAsyncContext()) {
        callback = once(bindAsyncResource(callback, "STREAM_END_OF_STREAM"));
      } else {
        callback = once(callback);
      }
      let isAborted = false;
      let abort = nop;
      const signal = options.signal;
      if (signal) {
        abort = () => {
          isAborted = true;
          callback.call(stream, $makeAbortError(undefined, { cause: signal.reason }));
        };
        if (signal.aborted) {
          process.nextTick(abort);
        } else {
          addAbortListener ??= require("internal/abort_listener").addAbortListener;
          const disposable = addAbortListener(signal, abort);
          const originalCallback = callback;
          callback = once((...args) => {
            disposable[SymbolDispose]();
            originalCallback.apply(stream, args);
          });
        }
      }
      const resolverFn = (...args) => {
        if (!isAborted) {
          process.nextTick(() => callback.apply(stream, args));
        }
      };
      const closedPromise = stream[kIsClosedPromise]?.promise ?? $webStreamClosedPromise(stream);
      PromisePrototypeThen.call(closedPromise, resolverFn, resolverFn);
      return nop;
    }
    function finished(stream, opts) {
      let autoCleanup = false;
      if (opts === null) {
        opts = kEmptyObject;
      }
      if (opts?.cleanup) {
        validateBoolean(opts.cleanup, "cleanup");
        autoCleanup = opts.cleanup;
      }
      return new Promise((resolve, reject) => {
        const cleanup = eos(stream, opts, (err) => {
          if (autoCleanup) {
            cleanup();
          }
          if (err) {
            reject(err);
          } else {
            resolve();
          }
        });
      });
    }
    eos.finished = finished;
    eos.kEosNodeSynchronousCallback = kEosNodeSynchronousCallback;
    module.exports = eos;

  });

  R.def("internal/streams/add-abort-signal", function (require, module, exports) {
    const { isNodeStream, isWebStream, kControllerErrorFunction } = require("internal/streams/utils");
    const eos = require("internal/streams/end-of-stream");
    const SymbolDispose = Symbol.dispose;
    let addAbortListener;
    const validateAbortSignal = (signal, name) => {
      if (typeof signal !== "object" || !("aborted" in signal)) {
        throw $ERR_INVALID_ARG_TYPE(name, "AbortSignal", signal);
      }
    };
    function addAbortSignal(signal, stream) {
      validateAbortSignal(signal, "signal");
      if (!isNodeStream(stream) && !isWebStream(stream)) {
        throw $ERR_INVALID_ARG_TYPE("stream", ["ReadableStream", "WritableStream", "Stream"], stream);
      }
      return addAbortSignalNoValidate(signal, stream);
    }
    function addAbortSignalNoValidate(signal, stream) {
      if (typeof signal !== "object" || !("aborted" in signal)) {
        return stream;
      }
      const onAbort = isNodeStream(stream) ? () => {
        stream.destroy($makeAbortError(undefined, { cause: signal.reason }));
      } : () => {
        stream[kControllerErrorFunction]($makeAbortError(undefined, { cause: signal.reason }));
      };
      if (signal.aborted) {
        onAbort();
      } else {
        addAbortListener ??= require("internal/abort_listener").addAbortListener;
        const disposable = addAbortListener(signal, onAbort);
        eos(stream, disposable[SymbolDispose]);
      }
      return stream;
    }
    module.exports = {
      addAbortSignal,
      addAbortSignalNoValidate
    };

  });

  R.def("internal/streams/from", function (require, module, exports) {
    const SymbolIterator = Symbol.iterator;
    const SymbolAsyncIterator = Symbol.asyncIterator;
    const PromisePrototypeThen = Promise.prototype.then;
    const { aggregateTwoErrors } = require("internal/errors");
    function from(Readable, iterable, opts) {
      let iterator;
      if (typeof iterable === "string" || iterable instanceof Buffer) {
        return new Readable({
          objectMode: true,
          ...opts,
          read() {
            this.push(iterable);
            this.push(null);
          }
        });
      }
      let isAsync;
      if (iterable?.[SymbolAsyncIterator]) {
        isAsync = true;
        iterator = iterable[SymbolAsyncIterator]();
      } else if (iterable?.[SymbolIterator]) {
        isAsync = false;
        iterator = iterable[SymbolIterator]();
      } else {
        throw $ERR_INVALID_ARG_TYPE("iterable", ["Iterable"], iterable);
      }
      const readable = new Readable({
        objectMode: true,
        highWaterMark: 1,
        ...opts
      });
      let reading = false;
      let isAsyncValues = false;
      readable._read = function() {
        if (!reading) {
          reading = true;
          if (isAsync) {
            nextAsync();
          } else if (isAsyncValues) {
            nextSyncWithAsyncValues();
          } else {
            nextSyncWithSyncValues();
          }
        }
      };
      const originalDestroy = readable._destroy;
      readable._destroy = function(error, cb) {
        originalDestroy.call(this, error, (destroyError) => {
          const combinedError = destroyError || error;
          PromisePrototypeThen.call(close(combinedError), __isCallable(cb) ? () => process.nextTick(cb, combinedError) : () => {}, __isCallable(cb) ? (closeError) => process.nextTick(cb, aggregateTwoErrors(combinedError, closeError)) : () => {});
        });
      };
      async function close(error) {
        const hadError = error !== undefined && error !== null;
        const hasThrow = typeof iterator.throw === "function";
        if (hadError && hasThrow) {
          const { value, done } = await iterator.throw(error);
          await value;
          if (done) {
            return;
          }
        }
        if (typeof iterator.return === "function") {
          const { value } = await iterator.return();
          await value;
        }
      }
      function nextSyncWithSyncValues() {
        for (;; ) {
          try {
            const { value, done } = iterator.next();
            if (done) {
              readable.push(null);
              return;
            }
            if (value && typeof value.then === "function") {
              return changeToAsyncValues(value);
            }
            if (value === null) {
              reading = false;
              throw $ERR_STREAM_NULL_VALUES();
            }
            if (readable.push(value)) {
              continue;
            }
            reading = false;
          } catch (err) {
            readable.destroy(err);
          }
          break;
        }
      }
      async function changeToAsyncValues(value) {
        isAsyncValues = true;
        try {
          const res = await value;
          if (res === null) {
            reading = false;
            throw $ERR_STREAM_NULL_VALUES();
          }
          if (readable.push(res)) {
            nextSyncWithAsyncValues();
            return;
          }
          reading = false;
        } catch (err) {
          readable.destroy(err);
        }
      }
      async function nextSyncWithAsyncValues() {
        for (;; ) {
          try {
            const { value, done } = iterator.next();
            if (done) {
              readable.push(null);
              return;
            }
            const res = value && typeof value.then === "function" ? await value : value;
            if (res === null) {
              reading = false;
              throw $ERR_STREAM_NULL_VALUES();
            }
            if (readable.push(res)) {
              continue;
            }
            reading = false;
          } catch (err) {
            readable.destroy(err);
          }
          break;
        }
      }
      async function nextAsync() {
        for (;; ) {
          try {
            const { value, done } = await iterator.next();
            if (done) {
              readable.push(null);
              return;
            }
            if (value === null) {
              reading = false;
              throw $ERR_STREAM_NULL_VALUES();
            }
            if (readable.push(value)) {
              continue;
            }
            reading = false;
          } catch (err) {
            readable.destroy(err);
          }
          break;
        }
      }
      return readable;
    }
    module.exports = from;

  });
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
