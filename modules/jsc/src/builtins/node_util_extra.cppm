// node:util completion partition: util.types (full brand-check set), legacy
// is* predicates, getSystemErrorName/getSystemErrorMap (libuv errno table),
// toUSVString, and TextEncoder/TextDecoder global identity.
//
// Mutates the existing M["util"] module object IN PLACE (bootstrap registers
// M["util"] === M["node:util"] and M["util/types"] === util.types, so in-place
// mutation propagates to every alias and to already-captured references).
//
// isProxy has no pure-JS brand check, so this shard transparently wraps the
// global Proxy constructor (construct + revocable) to record created proxies
// in a WeakSet — same observable behavior, node-aligned isProxy result.
//
// NOTE: appended AFTER the master builtins IIFE, so this is a self-contained
// IIFE that re-binds G = globalThis. Top level must never throw (a throw here
// would silently disable every later partition).
//
// Blueprint: node lib/util.js + lib/internal/util/types.js semantics; the
// errno table is libuv's uv_errno_map as exposed by node on linux (matches
// util.getSystemErrorMap()); bun aligns to the same libuv table.
export module mbun.jsc.js_builtins:node_util_extra;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeUtilExtraJS = R"JS(
(function () {
  const G = globalThis;
  try {
    const M = G.__mbunNativeModules;
    if (!M) return;
    const util = M["util"] || M["node:util"];
    if (!util) return;

    // ------------------------------------------------------------- helpers
    const ObjToStr = Object.prototype.toString;
    const toStr = (v) => ObjToStr.call(v);
    // %TypedArray%.prototype[@@toStringTag] getter: brand check that survives
    // Object.setPrototypeOf (reads the internal slot, returns the kind name).
    const taTagGet = Object.getOwnPropertyDescriptor(
      Object.getPrototypeOf(Uint8Array.prototype), Symbol.toStringTag).get;
    const taTag = (v) => (typeof v === "object" && v !== null) ? taTagGet.call(v) : undefined;
    const dvGet = Object.getOwnPropertyDescriptor(DataView.prototype, "byteLength").get;
    const abGet = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "byteLength").get;
    const sabGet = G.SharedArrayBuffer
      ? Object.getOwnPropertyDescriptor(G.SharedArrayBuffer.prototype, "byteLength").get
      : null;
    const mapSizeGet = Object.getOwnPropertyDescriptor(Map.prototype, "size").get;
    const setSizeGet = Object.getOwnPropertyDescriptor(Set.prototype, "size").get;
    const reSourceGet = Object.getOwnPropertyDescriptor(RegExp.prototype, "source").get;
    const brand = (fn, v) => { try { fn.call(v); return true; } catch (_) { return false; } };
    const brandArg = (fn, v, a) => { try { fn.call(v, a); return true; } catch (_) { return false; } };
    const isObj = (v) => typeof v === "object" && v !== null;
    const fnSrc = Function.prototype.toString;
    const isNativeCodeFn = (f) => { try { return /\[native code\]\s*\}\s*$/.test(fnSrc.call(f)); } catch (_) { return true; } };

    // -------------------------------------------- isProxy via Proxy wrapper
    // Proxies are transparent to JS, so track creation. The wrapper forwards
    // everything (name/length/prototype reads) to the native constructor.
    let proxySet = G.__mbunProxyRegistry;
    if (!proxySet) {
      proxySet = new WeakSet();
      const NativeProxy = G.Proxy;
      const revocable = function revocable(target, handler) {
        const out = NativeProxy.revocable(target, handler);
        try { proxySet.add(out.proxy); } catch (_) {}
        return out;
      };
      G.Proxy = new NativeProxy(NativeProxy, {
        construct(t, args) {
          const p = Reflect.construct(t, args);
          try { proxySet.add(p); } catch (_) {}
          return p;
        },
        get(t, k, r) {
          if (k === "revocable") return revocable;
          return Reflect.get(t, k, r);
        },
      });
      // Hidden, non-enumerable registry so a re-run reuses it.
      Object.defineProperty(G, "__mbunProxyRegistry", { value: proxySet, enumerable: false, writable: false, configurable: true });
    }

    // ----------------------------------------------------------- util.types
    // Mutate in place: M["util/types"] === util.types (bootstrap aliasing).
    const types = util.types || (util.types = {});
    const isBooleanObject = (v) => isObj(v) && brand(Boolean.prototype.valueOf, v);
    const isNumberObject = (v) => isObj(v) && brand(Number.prototype.valueOf, v);
    const isStringObject = (v) => isObj(v) && brand(String.prototype.valueOf, v);
    const isSymbolObject = (v) => isObj(v) && brand(Symbol.prototype.valueOf, v);
    const isBigIntObject = (v) => isObj(v) && brand(BigInt.prototype.valueOf, v);
    const T = {
      isExternal: (v) => false,
      isDate: (v) => isObj(v) && brand(Date.prototype.getTime, v),
      isArgumentsObject: (v) => isObj(v) && toStr(v) === "[object Arguments]",
      isBigIntObject,
      isBooleanObject,
      isNumberObject,
      isStringObject,
      isSymbolObject,
      isBoxedPrimitive: (v) =>
        isBooleanObject(v) || isNumberObject(v) || isStringObject(v) ||
        isSymbolObject(v) || isBigIntObject(v),
      isNativeError: (v) => (typeof Error.isError === "function"
        ? Error.isError(v)
        : (isObj(v) && toStr(v) === "[object Error]")),
      isRegExp: (v) => isObj(v) && brand(reSourceGet, v),
      // Bound functions inherit the target's prototype (and with it the
      // AsyncFunction/GeneratorFunction @@toStringTag), but node reports them
      // as plain functions — as it does host/builtin functions. Filter both
      // by their "[native code]" source.
      isAsyncFunction: (v) => {
        if (typeof v !== "function") return false;
        const t = toStr(v);
        if (t !== "[object AsyncFunction]" && t !== "[object AsyncGeneratorFunction]") return false;
        return !isNativeCodeFn(v);
      },
      isGeneratorFunction: (v) => {
        if (typeof v !== "function") return false;
        const t = toStr(v);
        if (t !== "[object GeneratorFunction]" && t !== "[object AsyncGeneratorFunction]") return false;
        return !isNativeCodeFn(v);
      },
      isGeneratorObject: (v) => {
        if (!isObj(v)) return false;
        const t = toStr(v);
        return t === "[object Generator]" || t === "[object AsyncGenerator]";
      },
      isPromise: (v) => isObj(v) && v instanceof Promise && toStr(v) === "[object Promise]",
      isMap: (v) => isObj(v) && brand(mapSizeGet, v),
      isSet: (v) => isObj(v) && brand(setSizeGet, v),
      isMapIterator: (v) => isObj(v) && toStr(v) === "[object Map Iterator]",
      isSetIterator: (v) => isObj(v) && toStr(v) === "[object Set Iterator]",
      isWeakMap: (v) => isObj(v) && brandArg(WeakMap.prototype.has, v, {}),
      isWeakSet: (v) => isObj(v) && brandArg(WeakSet.prototype.has, v, {}),
      isArrayBuffer: (v) => isObj(v) && brand(abGet, v),
      isDataView: (v) => isObj(v) && brand(dvGet, v),
      isSharedArrayBuffer: (v) => isObj(v) && sabGet !== null && brand(sabGet, v),
      isAnyArrayBuffer: (v) => isObj(v) && (brand(abGet, v) || (sabGet !== null && brand(sabGet, v))),
      isProxy: (v) => isObj(v) && proxySet.has(v),
      isModuleNamespaceObject: (v) => isObj(v) && toStr(v) === "[object Module]",
      isArrayBufferView: (v) => ArrayBuffer.isView(v),
      isTypedArray: (v) => taTag(v) !== undefined,
      isUint8Array: (v) => taTag(v) === "Uint8Array",
      isUint8ClampedArray: (v) => taTag(v) === "Uint8ClampedArray",
      isUint16Array: (v) => taTag(v) === "Uint16Array",
      isUint32Array: (v) => taTag(v) === "Uint32Array",
      isInt8Array: (v) => taTag(v) === "Int8Array",
      isInt16Array: (v) => taTag(v) === "Int16Array",
      isInt32Array: (v) => taTag(v) === "Int32Array",
      isFloat16Array: (v) => taTag(v) === "Float16Array",
      isFloat32Array: (v) => taTag(v) === "Float32Array",
      isFloat64Array: (v) => taTag(v) === "Float64Array",
      isBigInt64Array: (v) => taTag(v) === "BigInt64Array",
      isBigUint64Array: (v) => taTag(v) === "BigUint64Array",
      // Brand, never `instanceof`: a plain object given KeyObject.prototype, or
      // any object once Symbol.hasInstance is redefined, is not a key and must
      // not be reported as one (node test-crypto-keyobject-brand-check). The
      // predicate is published by builtins/crypto_asym.cppm, which owns the
      // slot table; the instanceof path remains only for a build without it.
      isKeyObject: (v) => {
        if (!isObj(v)) return false;
        if (typeof G.__mbunIsKeyObject === "function") return G.__mbunIsKeyObject(v);
        try {
          const c = M["crypto"] || M["node:crypto"];
          return !!(c && c.KeyObject) && v instanceof c.KeyObject;
        } catch (_) { return false; }
      },
      // A real CryptoKey is identified by its internal slots, never by
      // `instanceof`: prototype spoofing (or a forged Symbol.hasInstance) must
      // not fool it. ref: node test-webcrypto-cryptokey-brand-check.
      isCryptoKey: (v) => isObj(v) && typeof G.__mbunIsCryptoKey === "function"
        && G.__mbunIsCryptoKey(v),
      isEventTarget: (v) => isObj(v) && typeof G.EventTarget === "function" && v instanceof G.EventTarget,
    };
    for (const k of Object.keys(T)) {
      Object.defineProperty(T[k], "name", { value: k, configurable: true });
      types[k] = T[k];
    }
    M["util/types"] = M["node:util/types"] = types;

    // -------------------------------------- legacy deprecated is* predicates
    const legacy = {
      isArray: (v) => Array.isArray(v),
      isBoolean: (v) => typeof v === "boolean",
      isNull: (v) => v === null,
      isNullOrUndefined: (v) => v === null || v === undefined,
      isNumber: (v) => typeof v === "number",
      isString: (v) => typeof v === "string",
      isSymbol: (v) => typeof v === "symbol",
      isUndefined: (v) => v === undefined,
      isRegExp: (v) => types.isRegExp(v),
      isObject: (v) => typeof v === "object" && v !== null,
      isDate: (v) => types.isDate(v),
      isError: (v) => Object.prototype.toString.call(v) === "[object Error]" || v instanceof Error,
      isFunction: (v) => typeof v === "function",
      isPrimitive: (v) => v === null || (typeof v !== "object" && typeof v !== "function"),
      isBuffer: (v) => !!(G.Buffer && G.Buffer.isBuffer(v)),
    };
    for (const k of Object.keys(legacy)) {
      Object.defineProperty(legacy[k], "name", { value: k, configurable: true });
      util[k] = legacy[k];
    }

    // ------------------------------ toUSVString + TextEncoder/Decoder identity
    util.toUSVString = function toUSVString(input) {
      const s = "" + input;
      if (typeof s.toWellFormed === "function") return s.toWellFormed();
      return s.replace(
        /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?:^|[^\uD800-\uDBFF])[\uDC00-\uDFFF]/g,
        (m) => (m.length === 1 ? "�" : m[0] + "�"));
    };
    if (G.TextEncoder) util.TextEncoder = G.TextEncoder;
    if (G.TextDecoder) util.TextDecoder = G.TextDecoder;

    // ------------------------------------------------------------- parseEnv
    // node lib/util.js parseEnv(content) -> object, backed by the same .env
    // loader `mbun.dotenv` already drives for process.env (bun does the same:
    // src/runtime/node/node_util_binding.rs parse_env, OVERRIDE=true).
    // validate_string accepts a String object, which is exactly what the corpus
    // pins (util.test.js: util.parseEnv(new String("FOO=bar"))).
    if (G.__mbunDotenvNative) {
      util.parseEnv = function parseEnv(content) {
        if (typeof content !== "string" && !isStringObject(content)) {
          const err = new TypeError(
            'The "content" argument must be of type string. Received ' +
            (content === null ? "null" : typeof content));
          err.code = "ERR_INVALID_ARG_TYPE";
          throw err;
        }
        return G.__mbunDotenvNative.parse(String(content));
      };
    }

    // --------------------------- getSystemErrorName / getSystemErrorMap (uv)
    // libuv uv_errno_map for linux (identical to node's util.getSystemErrorMap()
    // output on linux; includes the UV__ fallback space entries EOF/UNKNOWN/
    // ECHARSET/EFTYPE that have no platform errno).
    const UV_LINUX = [
      [-1, "EPERM", "operation not permitted"],
      [-2, "ENOENT", "no such file or directory"],
      [-3, "ESRCH", "no such process"],
      [-4, "EINTR", "interrupted system call"],
      [-5, "EIO", "i/o error"],
      [-6, "ENXIO", "no such device or address"],
      [-7, "E2BIG", "argument list too long"],
      [-8, "ENOEXEC", "exec format error"],
      [-9, "EBADF", "bad file descriptor"],
      [-11, "EAGAIN", "resource temporarily unavailable"],
      [-12, "ENOMEM", "not enough memory"],
      [-13, "EACCES", "permission denied"],
      [-14, "EFAULT", "bad address in system call argument"],
      [-16, "EBUSY", "resource busy or locked"],
      [-17, "EEXIST", "file already exists"],
      [-18, "EXDEV", "cross-device link not permitted"],
      [-19, "ENODEV", "no such device"],
      [-20, "ENOTDIR", "not a directory"],
      [-21, "EISDIR", "illegal operation on a directory"],
      [-22, "EINVAL", "invalid argument"],
      [-23, "ENFILE", "file table overflow"],
      [-24, "EMFILE", "too many open files"],
      [-25, "ENOTTY", "inappropriate ioctl for device"],
      [-26, "ETXTBSY", "text file is busy"],
      [-27, "EFBIG", "file too large"],
      [-28, "ENOSPC", "no space left on device"],
      [-29, "ESPIPE", "invalid seek"],
      [-30, "EROFS", "read-only file system"],
      [-31, "EMLINK", "too many links"],
      [-32, "EPIPE", "broken pipe"],
      [-34, "ERANGE", "result too large"],
      [-36, "ENAMETOOLONG", "name too long"],
      [-38, "ENOSYS", "function not implemented"],
      [-39, "ENOTEMPTY", "directory not empty"],
      [-40, "ELOOP", "too many symbolic links encountered"],
      [-49, "EUNATCH", "protocol driver not attached"],
      [-61, "ENODATA", "no data available"],
      [-64, "ENONET", "machine is not on the network"],
      [-71, "EPROTO", "protocol error"],
      [-75, "EOVERFLOW", "value too large for defined data type"],
      [-84, "EILSEQ", "illegal byte sequence"],
      [-88, "ENOTSOCK", "socket operation on non-socket"],
      [-89, "EDESTADDRREQ", "destination address required"],
      [-90, "EMSGSIZE", "message too long"],
      [-91, "EPROTOTYPE", "protocol wrong type for socket"],
      [-92, "ENOPROTOOPT", "protocol not available"],
      [-93, "EPROTONOSUPPORT", "protocol not supported"],
      [-94, "ESOCKTNOSUPPORT", "socket type not supported"],
      [-95, "ENOTSUP", "operation not supported on socket"],
      [-97, "EAFNOSUPPORT", "address family not supported"],
      [-98, "EADDRINUSE", "address already in use"],
      [-99, "EADDRNOTAVAIL", "address not available"],
      [-100, "ENETDOWN", "network is down"],
      [-101, "ENETUNREACH", "network is unreachable"],
      [-103, "ECONNABORTED", "software caused connection abort"],
      [-104, "ECONNRESET", "connection reset by peer"],
      [-105, "ENOBUFS", "no buffer space available"],
      [-106, "EISCONN", "socket is already connected"],
      [-107, "ENOTCONN", "socket is not connected"],
      [-108, "ESHUTDOWN", "cannot send after transport endpoint shutdown"],
      [-110, "ETIMEDOUT", "connection timed out"],
      [-111, "ECONNREFUSED", "connection refused"],
      [-112, "EHOSTDOWN", "host is down"],
      [-113, "EHOSTUNREACH", "host is unreachable"],
      [-114, "EALREADY", "connection already in progress"],
      [-121, "EREMOTEIO", "remote I/O error"],
      [-125, "ECANCELED", "operation canceled"],
      [-3000, "EAI_ADDRFAMILY", "address family not supported"],
      [-3001, "EAI_AGAIN", "temporary failure"],
      [-3002, "EAI_BADFLAGS", "bad ai_flags value"],
      [-3003, "EAI_CANCELED", "request canceled"],
      [-3004, "EAI_FAIL", "permanent failure"],
      [-3005, "EAI_FAMILY", "ai_family not supported"],
      [-3006, "EAI_MEMORY", "out of memory"],
      [-3007, "EAI_NODATA", "no address"],
      [-3008, "EAI_NONAME", "unknown node or service"],
      [-3009, "EAI_OVERFLOW", "argument buffer overflow"],
      [-3010, "EAI_SERVICE", "service not available for socket type"],
      [-3011, "EAI_SOCKTYPE", "socket type not supported"],
      [-3013, "EAI_BADHINTS", "invalid value for hints"],
      [-3014, "EAI_PROTOCOL", "resolved protocol is unknown"],
      [-4028, "EFTYPE", "inappropriate file type or format"],
      [-4080, "ECHARSET", "invalid Unicode character"],
      [-4094, "UNKNOWN", "unknown error"],
      [-4095, "EOF", "end of file"],
    ];
    const uvMap = new Map();
    for (const [code, name, msg] of UV_LINUX) uvMap.set(code, [name, msg]);

    const received = (v) => {
      if (v === null) return "null";
      const t = typeof v;
      if (t === "string") {
        const s = v.length > 28 ? v.slice(0, 25) + "..." : v;
        return "type string ('" + s + "')";
      }
      if (t !== "object") return "type " + t;
      const c = v.constructor && v.constructor.name;
      return "an instance of " + (c || "Object");
    };
    const platformIsLinux = !!(G.process && G.process.platform === "linux");
    const prevGSEN = typeof util.getSystemErrorName === "function" ? util.getSystemErrorName : null;

    util.getSystemErrorName = function getSystemErrorName(err) {
      if (typeof err !== "number") {
        const e = new TypeError('The "err" argument must be of type number. Received ' + received(err));
        e.code = "ERR_INVALID_ARG_TYPE";
        throw e;
      }
      if (err >= 0 || !Number.isSafeInteger(err)) {
        const e = new RangeError('The value of "err" is out of range. It must be a negative integer. Received ' + err);
        e.code = "ERR_OUT_OF_RANGE";
        throw e;
      }
      if (platformIsLinux || !prevGSEN) {
        const entry = uvMap.get(err);
        return entry ? entry[0] : "Unknown system error " + err;
      }
      return prevGSEN(err);
    };
    util.getSystemErrorMap = function getSystemErrorMap() {
      return new Map(uvMap);
    };

    // ---- util.getCallSites (node >= 22.9) ---------------------------------
    // node lib/internal/util.js: returns the current call stack as objects
    // { functionName, scriptId, scriptName, lineNumber, column }, most recent
    // first, EXCLUDING getCallSites' own frame. Built from JSC's stack string
    // ("fn@file:line:col" / "global code@file:line:col" / "file:line:col"),
    // which is the only structured stack this engine exposes.
    if (typeof util.getCallSites !== "function") {
      const parseFrame = (raw) => {
        let text = String(raw).trim();
        let functionName = "";
        const at = text.lastIndexOf("@");
        if (at >= 0) { functionName = text.slice(0, at); text = text.slice(at + 1); }
        let scriptName = text;
        let lineNumber = 0;
        let column = 0;
        const m = /^(.*):(\d+):(\d+)$/.exec(text);
        if (m) { scriptName = m[1]; lineNumber = Number(m[2]); column = Number(m[3]); }
        else {
          const m2 = /^(.*):(\d+)$/.exec(text);
          if (m2) { scriptName = m2[1]; lineNumber = Number(m2[2]); }
        }
        // node reports the top-level frame's functionName as "" (anonymous);
        // JSC spells it "global code"/"module code".
        if (functionName === "global code" || functionName === "module code" ||
            functionName === "eval code") functionName = "";
        return { functionName, scriptId: "0", scriptName, lineNumber, column,
                 columnNumber: column };
      };
      util.getCallSites = function getCallSites(frames, options) {
        if (frames !== null && typeof frames === "object") { options = frames; frames = undefined; }
        if (frames === undefined) frames = 10;
        if (typeof frames !== "number" || !Number.isInteger(frames) || frames < 1) {
          const e = new RangeError('The value of "frameCount" is out of range. ' +
                                   "It must be an integer. Received " + String(frames));
          e.code = "ERR_OUT_OF_RANGE";
          throw e;
        }
        let stack = "";
        try { stack = String(new Error().stack || ""); } catch (e) { stack = ""; }
        const lines = stack.split("\n").filter((l) => l.trim().length > 0);
        // Drop this frame; tolerate a V8-style "Error" header line.
        let start = 0;
        if (lines.length && /^\s*(Error|[A-Za-z]*Error:)/.test(lines[0]) && lines[0].indexOf("@") < 0) start = 1;
        const out = [];
        for (let i = start + 1; i < lines.length && out.length < frames; i++) {
          out.push(parseFrame(lines[i]));
        }
        return out;
      };
    }
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
