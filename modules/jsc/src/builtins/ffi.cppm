// bun:ffi JS module. Wraps the native __mbunFfiNative backend (Bun.FFI) with the
// user-facing surface: FFIType, dlopen/linkSymbols/CFunction, CString, ptr,
// read, toArrayBuffer/toBuffer, viewSource. Kept as a dedicated payload
// partition so it does not grow the bootstrap partition or runtime.cppm.
// Ref: bun src/js/bun/ffi.ts. JSCallback (JS→native) and cc (runtime C compile)
// are DEFERRED (the native backend implements the dlopen call path only).
export module mbun.jsc.js_builtins:ffi;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kFFIJS = R"JS(
  // ---- bun:ffi (over __mbunFfiNative; ffi.ts port) ----
  if (G.__mbunFfiNative) {
    const ffi = G.__mbunFfiNative;

    const FFIType = {
      "0":0,"1":1,"2":2,"3":3,"4":4,"5":5,"6":6,"7":7,"8":8,"9":9,"10":10,
      "11":11,"12":12,"13":13,"14":14,"15":15,"16":16,"17":17,
      bool:11, c_int:5, c_uint:6, char:0, "char*":12, double:9, f32:10, f64:9,
      float:10, i16:3, i32:5, i64:7, i8:1, int:5, int16_t:3, int32_t:5,
      int64_t:7, int8_t:1, isize:7, u16:4, u32:6, u64:8, u8:2, uint16_t:4,
      uint32_t:6, uint64_t:8, uint8_t:2, usize:8, "void*":12, ptr:12,
      pointer:12, void:13, cstring:14, i64_fast:15, u64_fast:16, function:17,
      callback:17, fn:17, napi_env:18, napi_value:19, buffer:20,
    };

    const suffix = G.process.platform === "win32" ? "dll" : G.process.platform === "darwin" ? "dylib" : "so";

    const ptr = (arg1, arg2) => (typeof arg2 === "undefined" ? ffi.ptr(arg1) : ffi.ptr(arg1, arg2));
    const toBuffer = ffi.toBuffer;
    const toArrayBuffer = ffi.toArrayBuffer;
    const viewSource = ffi.viewSource;
    const BunCString = ffi.CString;
    ffi.CString = function CString(ptrArg, byteOffset, byteLength) {
      const s = ptrArg
        ? (typeof byteLength === "number" && Number.isSafeInteger(byteLength)
            ? BunCString(ptrArg, byteOffset || 0, byteLength)
            : BunCString(ptrArg, byteOffset || 0))
        : "";
      if (new.target) {
        const o = new String(s);
        o.ptr = typeof ptrArg === "number" ? ptrArg : 0;
        if (typeof byteOffset !== "undefined") o.byteOffset = byteOffset;
        if (typeof byteLength !== "undefined") o.byteLength = byteLength;
        return o;
      }
      return s;
    };
    const read = ffi.read;

    class CString extends String {
      constructor(ptrArg, byteOffset, byteLength) {
        super(
          ptrArg
            ? typeof byteLength === "number" && Number.isSafeInteger(byteLength)
              ? BunCString(ptrArg, byteOffset || 0, byteLength)
              : BunCString(ptrArg, byteOffset || 0)
            : "",
        );
        this.ptr = typeof ptrArg === "number" ? ptrArg : 0;
        if (typeof byteOffset !== "undefined") this.byteOffset = byteOffset;
        if (typeof byteLength !== "undefined") this.byteLength = byteLength;
        this._ab = undefined;
      }
      get arrayBuffer() {
        if (this._ab) return this._ab;
        if (!this.ptr) return (this._ab = new ArrayBuffer(0));
        return (this._ab = toArrayBuffer(this.ptr, this.byteOffset, this.byteLength));
      }
    }
    Object.defineProperty(G, "__GlobalBunCString", { value: CString, enumerable: false, configurable: false });

    // Per-type argument coercion, matching ffi.ts ffiWrappers (as functions
    // rather than eval'd source — same observable behavior).
    const isView = (v) => ArrayBuffer.isView(v);
    const ptrWrap = (val) => {
      if (typeof val === "number") return val;
      if (!val) return null;
      if (isView(val)) return val;
      if (val instanceof ArrayBuffer) return ptr(val);
      if (typeof val === "string") throw new TypeError("To convert a string to a pointer, encode it as a buffer");
      throw new TypeError(`Unable to convert ${val} to a pointer`);
    };
    const WRAP = {
      0: (val) => val | 0,
      1: (val) => val | 0,
      2: (val) => (val < 0 ? 0 : val >= 255 ? 255 : val | 0),
      3: (val) => (val <= -32768 ? -32768 : val >= 32768 ? 32768 : val | 0),
      4: (val) => { const r = (typeof val === "bigint" ? Number(val) : val) | 0; return r <= 0 ? 0 : r > 0xffff ? 0xffff : r; },
      5: (val) => val | 0,
      6: (val) => (val < 0 ? 0 : val > 0xffffffff ? -1 : val | 0),
      7: (val) => (typeof val === "bigint" ? val : typeof val === "number" ? BigInt(val || 0) : BigInt(+val || 0)),
      8: (val) => (typeof val === "bigint" ? val : typeof val === "number" ? (val <= 0 ? 0n : BigInt(val || 0)) : BigInt(+val || 0)),
      9: (val) => (typeof val === "number" ? val : Number(val)),
      10: (val) => Math.fround(val),
      11: (val) => !!val,
      12: ptrWrap,
      14: ptrWrap,
      15: (val) => { if (typeof val === "bigint") { if (val <= BigInt(Number.MAX_SAFE_INTEGER) && val >= BigInt(-Number.MAX_SAFE_INTEGER)) return Number(val) || 0; return val; } return !val ? 0 : +val || 0; },
      16: (val) => { if (typeof val === "bigint") { if (val <= BigInt(Number.MAX_SAFE_INTEGER) && val >= 0n) return Number(val); return val; } return typeof val === "number" ? (val <= 0 ? 0 : +val || 0) : +val || 0; },
      17: (val) => { if (typeof val === "number") return val; if (typeof val === "bigint") return Number(val); const p = val && val.ptr; if (!p) throw new TypeError("Expected function to be a JSCallback or a number"); return p; },
      20: (val) => { if (!isView(val)) throw new TypeError("Expected a TypedArray"); return val; },
    };

    function FFIBuilder(params, returnType, functionToCall, name) {
      const wrappers = new Array(params.length);
      for (let i = 0; i < params.length; i++) {
        const w = WRAP[FFIType[params[i]]];
        if (!w) throw new TypeError(`Unsupported type ${params[i]}. Must be one of: ${Object.keys(FFIType).sort().join(", ")}`);
        wrappers[i] = w;
      }
      const retIsCString = FFIType[returnType] === FFIType.cstring;
      const wrap = function (...a) {
        for (let i = 0; i < wrappers.length; i++) a[i] = wrappers[i](a[i]);
        const r = functionToCall(...a);
        return retIsCString ? new CString(r) : r;
      };
      try { Object.defineProperty(wrap, "name", { value: name }); } catch (e) {}
      wrap.native = functionToCall;
      wrap.ptr = functionToCall.ptr;
      return wrap;
    }

    const buildSymbols = (result, options, useName) => {
      for (const key in result.symbols) {
        const symbol = result.symbols[key];
        if (options[key]?.args?.length || FFIType[options[key]?.returns] === FFIType.cstring) {
          result.symbols[key] = FFIBuilder(options[key].args ?? [], options[key].returns ?? FFIType.void, symbol, useName(key));
        } else {
          result.symbols[key].native = result.symbols[key];
        }
      }
    };

    const normalizePath = (path) => {
      if (typeof path === "string" && path?.startsWith?.("file:")) return G.Bun.fileURLToPath(path);
      if (typeof path === "object" && path) {
        if (path instanceof URL) return G.Bun.fileURLToPath(path);
        if (typeof path.name === "string") return path.name;
      }
      return path;
    };

    function dlopen(path, options) {
      path = normalizePath(path);
      const result = ffi.dlopen(path, options);
      if (result instanceof Error) throw result;
      buildSymbols(result, options, (key) =>
        (typeof path === "string" && path.includes("/")) ? `${key} (${path.split("/").pop()})` : `${key} (${path})`);
      result.close = result.close.bind(result);
      return result;
    }

    function linkSymbols(options) {
      const result = ffi.linkSymbols(options);
      if (result instanceof Error) throw result;
      buildSymbols(result, options, (key) => key);
      return result;
    }

    let cFunctionI = 0;
    function CFunction(options) {
      const identifier = `CFunction${cFunctionI++}`;
      const result = linkSymbols({ [identifier]: options });
      let hasClosed = false;
      let close = result.close.bind(result);
      result.symbols[identifier].close = () => {
        if (hasClosed || !close) return;
        hasClosed = true;
        close();
        close = undefined;
      };
      return result.symbols[identifier];
    }

    // JSCallback (JS→native trampoline) is DEFERRED: the native backend has no
    // closure generator. Constructing one throws so callers fail honestly
    // rather than silently getting a null pointer.
    class JSCallback {
      constructor() { throw new Error("bun:ffi JSCallback (JS→native callbacks) is not implemented yet in mbun"); }
    }

    // cc (runtime TinyCC compile) is DEFERRED.
    const cc = () => { throw new Error("bun:ffi cc (runtime C compilation) is not implemented yet in mbun"); };

    const FFIModule = { CFunction, CString, FFIType, JSCallback, dlopen, linkSymbols, ptr, read, suffix, toArrayBuffer, toBuffer, viewSource, cc };
    M["bun:ffi"] = FFIModule;
    if (G.Bun) {
      G.Bun.FFI = ffi;
      G.__mbunFfiPtr = ptr;
    }
  }
)JS";

}  // namespace mbun::jsc::builtins::detail
