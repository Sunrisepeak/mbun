// node's `internalBinding(name)` — the C++-binding accessor node's own
// `lib/internal/**` modules are evaluated with.
//
// mbun resolves a bare `internal/x` specifier to node's real
// `compat/node/lib/internal/x.js` (node's own tsconfig.json `paths` maps it),
// so those module bodies load — but node evaluates them in a scope holding two
// free variables the CommonJS wrapper does not provide, `primordials` and
// `internalBinding`, so every one of them died on
// `ReferenceError: primordials is not defined`.
//
// This partition supplies the second of the two. It is NOT installed as an
// ambient global: the loader hands it to a module as a wrapper parameter, and
// only for modules that were required as `internal/...` AND live under a
// `lib/internal/` directory (see module_loading.inc). Ordinary user code —
// including a package that does `require("internal/foo")` into its own
// `node_modules` — never sees either name. The one global installed here is
// the bridge the loader reads, and it is defineProperty'd non-enumerable so
// node's `common` leaked-globals check (`for (const val in globalThis)`) does
// not see it.
//
// Blueprint: node lib/internal/bootstrap/realm.js (`internalBinding`, the
// `internalBindingAllowlist`/`canBeRequiredByUsers` split) and the individual
// `src/node_*.cc` binding initializers whose exports are re-expressed here on
// top of what mbun's runtime already exposes.
//
// An unimplemented binding throws with its name in the message rather than
// returning an empty object: a silent `{}` turns into a downstream
// `undefined is not a function` far from the cause, which is exactly the
// un-triageable failure the corpus tooling exists to avoid.
export module mbun.jsc.js_builtins:node_internal_binding;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeInternalBindingJS = R"JS(
(function () {
  const G = globalThis;

  // Stands in for libuv's loop-start epoch, so getLibuvNow() can return a small
  // monotonic-looking millisecond count the way uv_now(loop) does. Captured at
  // bootstrap rather than on first use: a lazily-captured baseline would make
  // the first reading ~0 no matter how long the process had already run.
  const LOOP_START_MS = Date.now();

  // Lazily-built binding namespaces, keyed by node's binding name. Each entry
  // is a factory so a binding nobody asks for costs nothing at startup.
  const factories = { __proto__: null };
  const cache = { __proto__: null };

  function internalBinding(name) {
    if (typeof name !== "string") {
      const e = new TypeError("internalBinding name must be a string");
      e.code = "ERR_INVALID_ARG_TYPE";
      throw e;
    }
    if (cache[name] !== undefined) return cache[name];
    const make = factories[name];
    if (make === undefined) {
      // Same shape node uses for an unknown binding (realm.js), with the name
      // kept in the message so a corpus log says which binding is missing.
      const e = new Error("No such binding: " + name);
      e.code = "ERR_INVALID_MODULE";
      throw e;
    }
    const value = make();
    cache[name] = value;
    return value;
  }

  const M = G.__mbunNativeModules || {};
  const mod = (name) => M[name] || M["node:" + name] || {};
  // This IIFE runs before any entry script, so there is no `require` in scope;
  // the entry's own require (rooted at the entry's directory, which is what
  // resolves `internal/...`) is read at call time.
  const req = (id) => (M[id] || (typeof G.require === "function" ? G.require(id) : {}));

  // ---------------------------------------------------------------- util ----
  // node src/node_util.cc. Everything here is expressible on top of what this
  // runtime already has; the V8-introspection entries (getProxyDetails,
  // getExternalValue) have no JSC equivalent and report "not a proxy" / 0,
  // which is what node itself returns for the non-exotic case.
  factories["util"] = () => {
    const kPending = 0, kFulfilled = 1, kRejected = 2;
    // Property-filter bit flags — node src/util.h PropertyFilter, mirrored from
    // v8::PropertyFilter.
    const ALL_PROPERTIES = 0, ONLY_WRITABLE = 1, ONLY_ENUMERABLE = 2,
          ONLY_CONFIGURABLE = 4, SKIP_STRINGS = 8, SKIP_SYMBOLS = 16;

    // node's private symbols live in a C++ side table; a JS Symbol per name is
    // observationally the same for lib/internal (they only ever use them as
    // hidden property keys). Minted on demand so the list never goes stale.
    const symbolCache = { __proto__: null };
    const privateSymbols = new Proxy({ __proto__: null }, {
      get(_t, key) {
        if (typeof key !== "string") return undefined;
        return symbolCache[key] || (symbolCache[key] = Symbol("node:" + key));
      },
      has() { return true; },
    });

    const peek = G.Bun && typeof G.Bun.peek === "function" ? G.Bun.peek : null;

    const isIndexKey = (k) => {
      const n = +k;
      return Number.isInteger(n) && n >= 0 && String(n) === k;
    };

    return {
      constants: { kPending, kFulfilled, kRejected, kExiting: 0, kHasExitCode: 1 },
      kPending, kFulfilled, kRejected,
      ALL_PROPERTIES, ONLY_WRITABLE, ONLY_ENUMERABLE, ONLY_CONFIGURABLE,
      SKIP_STRINGS, SKIP_SYMBOLS,
      privateSymbols,

      // [state] | [state, value] — node returns the settled value alongside the
      // state. Bun.peek reads the already-settled slot without scheduling.
      getPromiseDetails(p) {
        if (peek === null) return [kPending];
        try {
          const status = peek.status(p);
          if (status === "pending") return [kPending];
          return [status === "fulfilled" ? kFulfilled : kRejected, peek(p)];
        } catch { return [kPending]; }
      },
      // JSC exposes no proxy introspection; node's callers all treat a falsy
      // result as "not a proxy", which is the honest answer here.
      getProxyDetails() { return undefined; },
      getExternalValue() { return 0n; },
      getConstructorName(obj) {
        let o = obj;
        while (o !== null && o !== undefined) {
          const d = Object.getOwnPropertyDescriptor(o, "constructor");
          if (d !== undefined && typeof d.value === "function" && d.value.name) {
            return d.value.name;
          }
          o = Object.getPrototypeOf(o);
        }
        return "Object";
      },
      // Own string keys that are not array indices, plus symbols, filtered the
      // way v8::Object::GetPropertyNames does for the flags above.
      getOwnNonIndexProperties(obj, filter) {
        const out = [];
        const wantEnumOnly = (filter & ONLY_ENUMERABLE) !== 0;
        if ((filter & SKIP_STRINGS) === 0) {
          for (const k of Object.getOwnPropertyNames(obj)) {
            if (isIndexKey(k)) continue;
            if (wantEnumOnly) {
              const d = Object.getOwnPropertyDescriptor(obj, k);
              if (d === undefined || !d.enumerable) continue;
            }
            out.push(k);
          }
        }
        if ((filter & SKIP_SYMBOLS) === 0) {
          for (const s of Object.getOwnPropertySymbols(obj)) {
            if (wantEnumOnly) {
              const d = Object.getOwnPropertyDescriptor(obj, s);
              if (d === undefined || !d.enumerable) continue;
            }
            out.push(s);
          }
        }
        return out;
      },
      // [entries, isKeyValue] — Map/Set (and their iterators) preview.
      previewEntries(value) {
        try {
          if (value instanceof Map) {
            const flat = [];
            for (const [k, v] of value) { flat.push(k); flat.push(v); }
            return [flat, true];
          }
          if (value instanceof Set) return [[...value], false];
          if (value instanceof WeakMap || value instanceof WeakSet) return [[], false];
        } catch { /* fall through */ }
        return [[], false];
      },
      constructSharedArrayBuffer(len) {
        return typeof SharedArrayBuffer === "function" ? new SharedArrayBuffer(len)
                                                       : new ArrayBuffer(len);
      },
      arrayBufferViewHasBuffer() { return true; },
      // node blocks the thread; Atomics.wait on a throwaway SAB is the same
      // observable behaviour without a spin loop when it is available.
      sleep(ms) {
        const end = Date.now() + ms;
        try {
          const sab = new SharedArrayBuffer(4);
          Atomics.wait(new Int32Array(sab), 0, 0, ms);
        } catch { while (Date.now() < end) { /* spin */ } }
      },
      guessHandleType(fd) {
        try {
          const tty = mod("tty");
          if (typeof tty.isatty === "function" && tty.isatty(fd)) return "TTY";
        } catch { /* not a tty */ }
        try {
          const st = mod("fs").fstatSync(fd);
          if (st.isFile()) return "FILE";
          if (st.isFIFO()) return "PIPE";
          if (st.isSocket()) return "TCP";
        } catch { /* fall through */ }
        return "UNKNOWN";
      },
      // node lib helper: install accessors that require(id) on first read.
      defineLazyProperties(target, id, keys, writable = true) {
        for (const key of keys) {
          let value;
          let started = false;
          Object.defineProperty(target, key, {
            __proto__: null, configurable: true, enumerable: false,
            get() {
              if (!started) { started = true; value = req(id)[key]; }
              return value;
            },
            set: writable ? (v) => { started = true; value = v; } : undefined,
          });
        }
      },
      isInsideNodeModules() { return false; },
      markPromiseAsHandled(p) { try { p.then(undefined, () => {}); } catch { /* not a promise */ } },
      shouldAbortOnUncaughtToggle: [0],
      getCallerLocation() { return []; },
      WeakReference: class WeakReference extends WeakRef {
        #refs = 0;
        get() { return this.deref(); }
        incRef() { return ++this.#refs; }
        decRef() { return --this.#refs; }
      },
    };
  };

  // ----------------------------------------------------------- constants ----
  // node src/node_constants.cc groups every constant under os/fs/crypto/zlib/
  // trace; each group is already published by the corresponding public module,
  // so this re-shapes what the runtime has rather than duplicating a table.
  factories["constants"] = () => {
    const os = mod("os").constants || {};
    return {
      os,
      fs: mod("fs").constants || {},
      crypto: mod("crypto").constants || {},
      zlib: mod("zlib").constants || {},
      trace: {},
      internal: {},
      signals: os.signals || {},
    };
  };

  // ------------------------------------------------------------------ uv ----
  // node src/uv.cc: the UV_E* codes plus errname/getErrorMap. The table is the
  // one node:util.getSystemErrorMap() already publishes (libuv's linux errno
  // map), so the two can never disagree.
  factories["uv"] = () => {
    const util = mod("util");
    const map = typeof util.getSystemErrorMap === "function" ? util.getSystemErrorMap() : new Map();
    const out = { __proto__: null };
    for (const [code, entry] of map) out["UV_" + entry[0]] = code;
    out.errname = (n) => {
      const e = map.get(n);
      if (e !== undefined) return e[0];
      const err = new TypeError("The value of \"err\" is out of range. Received " + n);
      err.code = "ERR_OUT_OF_RANGE";
      throw err;
    };
    out.getErrorMap = () => map;
    out.getErrorMessage = (n) => { const e = map.get(n); return e === undefined ? "Unknown system error" : e[1]; };
    return out;
  };

  // -------------------------------------------------------------- errors ----
  // node src/node_errors.cc + src/node_exit_code.h (EXIT_CODE_LIST).
  factories["errors"] = () => ({
    exitCodes: {
      kNoFailure: 0, kGenericUserError: 1, kInternalJSParseError: 3,
      kInternalJSEvaluationFailure: 4, kV8FatalError: 5,
      kInvalidFatalExceptionMonkeyPatching: 6, kExceptionInFatalExceptionHandler: 7,
      kInvalidCommandLineArgument: 9, kBootstrapFailure: 10,
      kInvalidCommandLineArgument2: 12, kUnsettledTopLevelAwait: 13,
      kStartupSnapshotFailure: 14, kAbort: 134,
    },
    triggerUncaughtException(err) { throw err; },
    noSideEffectsToString(v) {
      try { return typeof v === "symbol" ? v.toString() : String(v); } catch { return "[object]"; }
    },
    getErrorSourcePositions() { return undefined; },
    setEnhanceStackForFatalException() {},
    setPrepareStackTraceCallback() {},
    setSourceMapsEnabled() {},
    setGetSourceMapErrorSource() {},
    setMaybeCacheGeneratedSourceMap() {},
  });

  // --------------------------------------------------------- mksnapshot ----
  // node src/node_snapshotable.cc. mbun builds no startup snapshot, so every
  // answer here is "not building one" — which is the truthful answer, not a
  // stub of convenience.
  //
  // Why this tiny binding matters far beyond snapshots: node's
  // internal/errors.js:249 calls isErrorStackTraceLimitWritable(), which does
  // `require('internal/v8/startup_snapshot').namespace.isBuildingSnapshot()`,
  // and that module top-levels internalBinding('mksnapshot'). Without this,
  // EVERY ERR_* constructed through node's own internal/errors throws
  // "No such binding: mksnapshot" instead of the error it was building — i.e.
  // it fails exactly when a test is already failing, replacing a readable
  // diagnostic with a confusing one, repo-wide.
  //
  // isBuildingSnapshotBuffer is a Uint8Array because internal/v8/startup_snapshot
  // reads element 0 as a flag rather than calling a function.
  factories["mksnapshot"] = () => ({
    isBuildingSnapshotBuffer: new Uint8Array(1),
    setSerializeCallback() {},
    setDeserializeCallback() {},
    setDeserializeMainFunction() {},
    runDeserializeCallbacks() {},
    compileSerializeMain() { return undefined; },
  });

  // ------------------------------------------------------------- options ----
  // node src/node_options.cc. This runtime does not carry node's CLI option
  // table, so the dictionary is empty: internal/options.js getOptionValue then
  // reports every option as unset, which is the truthful answer for a runtime
  // that accepted no such flag.
  factories["options"] = () => ({
    // The Permission Model's flags ARE carried, because node's own
    // lib/internal/process/permission.js decides isEnabled() from
    // getOptionValue('--permission'); an empty dictionary would report the
    // sandbox as off to every node lib/** module that asks.
    getCLIOptionsValues: () => {
      const PN = G.__mbunPermissionNative;
      const out = { __proto__: null };
      // node src/node_options.cc default (16 KiB) — this runtime's real HTTP
      // header limit, which node's own lib reads to size its parser.
      out["--max-http-header-size"] = 16384;
      if (!PN) return out;
      out["--permission"] = !!PN.enabled && !PN.audit;
      out["--permission-audit"] = !!PN.audit;
      out["--allow-fs-read"] = PN.allowFsRead || [];
      out["--allow-fs-write"] = PN.allowFsWrite || [];
      out["--allow-addons"] = !!PN.allowAddons;
      out["--allow-child-process"] = !!PN.allowChildProcess;
      out["--allow-worker"] = !!PN.allowWorker;
      out["--allow-inspector"] = !!PN.allowInspector;
      out["--allow-wasi"] = !!PN.allowWasi;
      out["--allow-net"] = !!PN.allowNet;
      out["--allow-ffi"] = !!PN.allowFfi;
      return out;
    },
    getCLIOptionsInfo: () => ({ options: new Map(), aliases: new Map() }),
    getOptionsAsFlags: () => [],
    getEmbedderOptions: () => ({
      shouldNotRegisterESMLoader: false, noGlobalSearchPaths: false,
      hasEmbedderPreload: false,
    }),
    getEnvOptionsInputType: () => new Map(),
    getNamespaceOptionsInputType: () => new Map(),
    envSettings: { kAllowedInEnvironment: 0, kDisallowedInEnvironment: 1 },
    types: {
      kNoOp: 0, kV8Option: 1, kBoolean: 2, kInteger: 3, kUInteger: 4,
      kString: 5, kHostPort: 6, kStringList: 7,
    },
  });

  // -------------------------------------------------------------- config ----
  // node src/node_config.cc — build-time feature flags of the running binary.
  factories["config"] = () => ({
    hasInspector: false,
    hasOpenSSL: true,
    hasIntl: typeof Intl === "object",
    hasSmallICU: false,
    hasTracing: false,
    hasNodeOptions: true,
    hasDtrace: false,
    isDebugBuild: false,
    noBrowserGlobals: false,
    bits: 64,
    getDefaultLocale() {
      try { return Intl.DateTimeFormat().resolvedOptions().locale; } catch { return "en-US"; }
    },
  });

  // ------------------------------------------------------------- symbols ----
  // node src/node_symbols.cc — per-isolate well-known private symbols. Same
  // reasoning as util.privateSymbols: minted on demand, stable per process.
  factories["symbols"] = () => {
    const cached = { __proto__: null };
    return new Proxy({ __proto__: null }, {
      get(_t, key) {
        if (typeof key !== "string") return undefined;
        return cached[key] || (cached[key] = Symbol("node:" + key));
      },
      has() { return true; },
    });
  };

  // --------------------------------------------------------- credentials ----
  factories["credentials"] = () => ({
    safeGetenv: (k) => (G.process && G.process.env[k]) || "",
    getTempDir: () => { try { return mod("os").tmpdir(); } catch { return "/tmp"; } },
  });

  // --------------------------------------------------------------- types ----
  factories["types"] = () => Object.assign({ __proto__: null }, mod("util/types"));

  // ----------------------------------------------------------- task_queue ----
  factories["task_queue"] = () => ({
    enqueueMicrotask: (fn) => queueMicrotask(fn),
    runMicrotasks() {},
    setTickCallback() {},
    setPromiseRejectCallback() {},
    tickInfo: new Uint32Array(2),
    promiseRejectEvents: {
      kPromiseRejectWithNoHandler: 0, kPromiseHandlerAddedAfterReject: 1,
      kPromiseResolveAfterResolved: 2, kPromiseRejectAfterResolved: 3,
    },
  });

  // -------------------------------------------------------- trace_events ----
  // This runtime emits no trace events; the category set is always disabled,
  // which is what node reports when built without tracing.
  factories["trace_events"] = () => ({
    CategorySet: class CategorySet { constructor() {} enable() {} disable() {} },
    getCategoryEnabledBuffer: () => new Uint8Array(1),
    isTraceCategoryEnabled: () => false,
    setTraceCategoryStateUpdateHandler() {},
    trace() {},
  });

  // ------------------------------------------------ async_context_frame ----
  factories["async_context_frame"] = () => {
    let data;
    return {
      getContinuationPreservedEmbedderData: () => data,
      setContinuationPreservedEmbedderData: (v) => { data = v; },
    };
  };

  // ------------------------------------------------------ string_decoder ----
  // node src/string_decoder.cc. `encodings` is indexed by node's `enum encoding`
  // (src/node.h: ASCII=0, UTF8, BASE64, UCS2, BINARY/LATIN1, HEX, BUFFER,
  // BASE64URL), and internal/util.js reads it at load to build normalizeEncoding
  // — which is why this one binding gated the whole internal/util graph.
  // decode/flush drive node's lib/string_decoder.js; this runtime serves
  // `require("string_decoder")` from its own builtin, so they are expressed on
  // top of that StringDecoder, keyed on the caller's state array.
  factories["string_decoder"] = () => {
    const encodings = ["ascii", "utf8", "base64", "utf16le", "latin1", "hex", "buffer", "base64url"];
    const kEncodingField = 6;
    const decoders = new WeakMap();
    const decoderFor = (handle) => {
      let d = decoders.get(handle);
      if (d === undefined) {
        const Ctor = mod("string_decoder").StringDecoder;
        d = new Ctor(encodings[handle[kEncodingField]] || "utf8");
        decoders.set(handle, d);
      }
      return d;
    };
    return {
      encodings,
      kIncompleteCharactersStart: 0, kIncompleteCharactersEnd: 4,
      kMissingBytes: 4, kBufferedBytes: 5, kEncodingField, kNumFields: 7,
      decode: (handle, buf) => decoderFor(handle).write(buf),
      flush: (handle) => decoderFor(handle).end(),
    };
  };

  // ------------------------------------------------------------- buffer ----
  // node src/node_buffer.cc — the slice/write pair per encoding plus the
  // allocation helpers internal/buffer.js builds its read*/write* accessors on.
  // Every one of them is a method this runtime's Buffer already implements, so
  // this is a re-shaping of the public API into node's binding signature.
  factories["buffer"] = () => {
    const B = mod("buffer");
    const Buf = B.Buffer;
    const slice = (enc) => function (start, end) {
      return Buf.prototype.toString.call(this, enc, start, end);
    };
    const write = (enc) => function (string, offset, length) {
      return Buf.prototype.write.call(this, string, offset, length, enc);
    };
    return {
      kMaxLength: B.kMaxLength, kStringMaxLength: B.kStringMaxLength,
      asciiSlice: slice("ascii"), base64Slice: slice("base64"),
      base64urlSlice: slice("base64url"), latin1Slice: slice("latin1"),
      hexSlice: slice("hex"), ucs2Slice: slice("ucs2"), utf8Slice: slice("utf8"),
      asciiWriteStatic: write("ascii"), base64Write: write("base64"),
      base64urlWrite: write("base64url"), latin1WriteStatic: write("latin1"),
      hexWrite: write("hex"), ucs2Write: write("ucs2"), utf8WriteStatic: write("utf8"),
      asciiWrite: write("ascii"), latin1Write: write("latin1"), utf8Write: write("utf8"),
      byteLengthUtf8: (s) => Buf.byteLength(s, "utf8"),
      compare: (a, b) => Buf.compare(a, b),
      compareOffset: (a, b, ts, ss, te, se) =>
        Buf.compare(a.subarray(ss, se), b.subarray(ts, te)),
      copy: (source, target, targetStart, sourceStart, sourceEnd) =>
        Buf.prototype.copy.call(source, target, targetStart, sourceStart, sourceEnd),
      fill: (buf, value, start, end, encoding) =>
        Buf.prototype.fill.call(buf, value, start, end, encoding),
      indexOfBuffer: (buf, val, byteOffset, encoding, isForward) =>
        (isForward ? Buf.prototype.indexOf : Buf.prototype.lastIndexOf)
          .call(buf, val, byteOffset, encoding),
      indexOfNumber: (buf, val, byteOffset, isForward) =>
        (isForward ? Buf.prototype.indexOf : Buf.prototype.lastIndexOf).call(buf, val, byteOffset),
      indexOfString: (buf, val, byteOffset, encoding, isForward) =>
        (isForward ? Buf.prototype.indexOf : Buf.prototype.lastIndexOf)
          .call(buf, val, byteOffset, encoding),
      swap16: (buf) => Buf.prototype.swap16.call(buf),
      swap32: (buf) => Buf.prototype.swap32.call(buf),
      swap64: (buf) => Buf.prototype.swap64.call(buf),
      isAscii: (buf) => (typeof B.isAscii === "function" ? B.isAscii(buf) : true),
      isUtf8: (buf) => (typeof B.isUtf8 === "function" ? B.isUtf8(buf) : true),
      atob: (s) => G.atob(s), btoa: (s) => G.btoa(s),
      createUnsafeArrayBuffer: (len) => new ArrayBuffer(len),
      copyArrayBuffer: (dst, dstOff, src, srcOff, len) => {
        new Uint8Array(dst, dstOff, len).set(new Uint8Array(src, srcOff, len));
      },
      // node hands the ArrayBuffer a detach key so only it may detach the
      // buffer; JSC has no such hook and no caller inspects the result.
      setDetachKey() {},
      zeroFill: new Uint32Array(1),
    };
  };

  // ----------------------------------------------------------------- url ----
  // node src/node_url.cc (BindingData), expressed over the WHATWG URL this
  // runtime already owns.
  //
  // node's lib/internal/url.js does not keep a parsed URL record: it keeps the
  // serialized href plus the nine `ada::url_components` offsets into it, and
  // re-reads the `urlComponents` accessor after every parse/update. So the
  // binding has to (a) parse/mutate and (b) publish those offsets. (a) is
  // `new URL()` and its setters; (b) is recovered arithmetically from the href,
  // which is exact because a *serialized* URL only ever contains ':' '@' '?'
  // '#' at component boundaries.
  //
  // Without this binding `internal/url` throws at load, and with it every
  // module that requires it — `internal/fs/utils`, `internal/modules/helpers`,
  // and the whole `internal/test_runner/*` tree — so node's own test runner
  // could not be loaded at all.
  factories["url"] = () => {
    // ada spells "omitted" as uint32_t(-1); URLContext compares against it.
    const OMITTED = 4294967295;
    // ada::scheme::type — the order is load-bearing (URLContext.scheme_type).
    const SCHEME_TYPE = {
      __proto__: null,
      "http:": 0, "https:": 2, "ws:": 3, "ftp:": 4, "wss:": 5, "file:": 6,
    };
    const urlMod = () => mod("url");

    // The shared component buffer node reads back through `urlComponents`.
    const buffer = [0, 0, 0, 0, OMITTED, 0, OMITTED, OMITTED, 1];

    const invalidURL = (input, base) => {
      const e = new TypeError("Invalid URL");
      e.code = "ERR_INVALID_URL";
      e.input = input;
      if (base !== undefined) e.base = base;
      return e;
    };

    // ada::url_components for `u`, as offsets into u.href.
    const componentsOf = (u) => {
      const href = u.href;
      const protocolEnd = u.protocol.length;
      const scheme = SCHEME_TYPE[u.protocol];
      let usernameEnd, hostStart, hostEnd, pathnameStart;
      // "//" after the scheme means an authority; an opaque path (mailto:,
      // data:, javascript:) has none and ada collapses every host offset onto
      // protocol_end.
      if (href.charCodeAt(protocolEnd) === 47 && href.charCodeAt(protocolEnd + 1) === 47) {
        const authStart = protocolEnd + 2;
        const user = u.username;
        const pass = u.password;
        usernameEnd = authStart + user.length;
        // With credentials host_start indexes the '@' (node's getters test
        // href[host_start] for it); without, the first host character.
        hostStart = usernameEnd + (pass.length !== 0 ? pass.length + 1 : 0);
        const hostAt = (user.length !== 0 || pass.length !== 0) ? hostStart + 1 : hostStart;
        hostEnd = hostAt + u.hostname.length;
        pathnameStart = hostEnd + (u.port.length !== 0 ? u.port.length + 1 : 0);
      } else {
        usernameEnd = hostStart = hostEnd = pathnameStart = protocolEnd;
      }
      const hashStart = href.indexOf("#", pathnameStart);
      const queryEnd = hashStart === -1 ? href.length : hashStart;
      let searchStart = href.indexOf("?", pathnameStart);
      if (searchStart === -1 || searchStart > queryEnd) searchStart = OMITTED;
      return [
        protocolEnd,
        usernameEnd,
        hostStart,
        hostEnd,
        u.port.length !== 0 ? +u.port : OMITTED,
        pathnameStart,
        searchStart,
        hashStart === -1 ? OMITTED : hashStart,
        scheme === undefined ? 1 : scheme,
      ];
    };

    const publish = (u) => {
      const c = componentsOf(u);
      for (let i = 0; i < 9; i++) buffer[i] = c[i];
      return u.href;
    };

    const construct = (input, base) => (
      base === undefined || base === null ? new G.URL(input) : new G.URL(input, base)
    );

    // ---- RFC1738-unsafe chars node percent-encodes before parsing a path ----
    // node src/node_url.cc EncodePathChars / lookup_table.
    const PATH_ENCODE = { __proto__: null };
    PATH_ENCODE["\0"] = "%00"; PATH_ENCODE["\t"] = "%09";
    PATH_ENCODE["\n"] = "%0A"; PATH_ENCODE["\r"] = "%0D";
    PATH_ENCODE[" "] = "%20"; PATH_ENCODE['"'] = "%22";
    PATH_ENCODE["#"] = "%23"; PATH_ENCODE["%"] = "%25";
    PATH_ENCODE["?"] = "%3F"; PATH_ENCODE["["] = "%5B";
    PATH_ENCODE["\\"] = "%5C"; PATH_ENCODE["]"] = "%5D";
    PATH_ENCODE["^"] = "%5E"; PATH_ENCODE["|"] = "%7C";
    PATH_ENCODE["~"] = "%7E";

    const encodePathChars = (input, windows) => {
      let out = "file://";
      for (let i = 0; i < input.length; i++) {
        const ch = input[i];
        if (input.charCodeAt(i) > 126) { out += ch; continue; }
        if (windows && ch === "\\") { out += "/"; continue; }
        out += PATH_ENCODE[ch] !== undefined ? PATH_ENCODE[ch] : ch;
      }
      return out;
    };

    // Special-scheme set per the URL Standard. A protocol setter may not cross
    // the special/non-special boundary; JSC's URL applies the change anyway, so
    // the guard lives here (ada reports it as a failed setter → `false`).
    const isSpecial = (protocol) => SCHEME_TYPE[protocol] !== undefined;

    // kProtocol..kHref, in lib/internal/url.js's `updateActions` order.
    const applyUpdate = (u, action, value) => {
      switch (action) {
        case 0:
          if (isSpecial(u.protocol) !== isSpecial(`${value}`.replace(/:*$/, "") + ":")) return false;
          u.protocol = value; return true;
        case 1: u.host = value; return true;
        case 2: u.hostname = value; return true;
        case 3: u.port = value; return true;
        case 4: u.username = value; return true;
        case 5: u.password = value; return true;
        case 6: u.pathname = value; return true;
        case 7: u.search = value; return true;
        case 8: u.hash = value; return true;
        case 9:
          try { u.href = value; } catch { return false; }
          // JSC ignores an unparseable href assignment instead of throwing.
          return G.URL.canParse ? G.URL.canParse(value) : (() => {
            try { new G.URL(value); return true; } catch { return false; }
          })();
        default: return false;
      }
    };

    return {
      // args: (input, base, raiseException)
      parse(input, base, raiseException) {
        const inputStr = `${input}`;
        const baseStr = base === undefined || base === null ? undefined : `${base}`;
        let u;
        try { u = construct(inputStr, baseStr); } catch {
          if (raiseException) throw invalidURL(inputStr, baseStr);
          return undefined;
        }
        return publish(u);
      },
      // args: (href, action, value) -> new href, or false when the setter failed
      update(href, action, value) {
        let u;
        try { u = new G.URL(`${href}`); } catch { return false; }
        if (!applyUpdate(u, action, `${value}`)) return false;
        return publish(u);
      },
      canParse(input, base) {
        const inputStr = `${input}`;
        if (typeof base === "string") {
          if (G.URL.canParse) return G.URL.canParse(inputStr, base);
          try { new G.URL(inputStr, base); return true; } catch { return false; }
        }
        if (G.URL.canParse) return G.URL.canParse(inputStr);
        try { new G.URL(inputStr); return true; } catch { return false; }
      },
      getOrigin(input) {
        let u;
        try { u = new G.URL(`${input}`); } catch { throw invalidURL(`${input}`); }
        return u.origin;
      },
      // node throws ERR_INVALID_URL when the encoded path will not parse.
      pathToFileURL(input, windows, hostname) {
        const inputStr = `${input}`;
        let u;
        try { u = new G.URL(encodePathChars(inputStr, windows === true)); } catch {
          throw invalidURL(inputStr);
        }
        if (windows === true && hostname !== undefined) {
          try { u.hostname = `${hostname}`; } catch {
            throw invalidURL(inputStr, `${hostname}`);
          }
        }
        return publish(u);
      },
      // NoSideEffect in node: must NOT touch the component buffer.
      format(href, hash, unicode, search, auth) {
        const hrefStr = `${href}`;
        let u;
        try { u = new G.URL(hrefStr); } catch { return hrefStr; }
        if (!hash) u.hash = "";
        if (!search) u.search = "";
        if (!auth) { u.username = ""; u.password = ""; }
        let out = u.href;
        if (unicode && u.hostname.length !== 0) {
          const toUnicode = urlMod().domainToUnicode;
          const uni = typeof toUnicode === "function" ? toUnicode(u.hostname) : "";
          if (uni.length !== 0) {
            const c = componentsOf(u);
            const hostAt = (u.username.length !== 0 || u.password.length !== 0) ? c[2] + 1 : c[2];
            out = out.slice(0, hostAt) + uni + out.slice(c[3]);
          }
        }
        return out;
      },
      domainToASCII(domain) {
        const s = `${domain}`;
        if (s.length === 0) return "";
        const fn = urlMod().domainToASCII;
        return typeof fn === "function" ? fn(s) : s;
      },
      domainToUnicode(domain) {
        const s = `${domain}`;
        if (s.length === 0) return "";
        const fn = urlMod().domainToUnicode;
        return typeof fn === "function" ? fn(s) : s;
      },
      get urlComponents() { return buffer; },
    };
  };

  // ---------------------------------------------------------- url_pattern ----
  factories["url_pattern"] = () => ({ URLPattern: G.URLPattern });

  // ------------------------------------------------------------------ os ----
  // node src/node_os.cc, expressed over the public node:os this runtime ships.
  factories["os"] = () => {
    const os = mod("os");
    return {
      getHostname: () => os.hostname(),
      getLoadAvg: (arr) => { const l = os.loadavg(); if (arr) { arr[0] = l[0]; arr[1] = l[1]; arr[2] = l[2]; } return l; },
      getUptime: () => os.uptime(),
      getTotalMem: () => os.totalmem(),
      getFreeMem: () => os.freemem(),
      getCPUs: () => os.cpus(),
      getInterfaceAddresses: () => os.networkInterfaces(),
      getHomeDirectory: () => os.homedir(),
      getUserInfo: (opts) => os.userInfo(opts),
      setPriority: (pid, prio) => { os.setPriority(pid, prio); return 0; },
      getPriority: (pid) => os.getPriority(pid),
      getOSInformation: () => [os.type(), os.release(), os.version(), os.machine()],
      getAvailableParallelism: () => os.availableParallelism(),
      isBigEndian: os.endianness() === "BE",
    };
  };

  // ------------------------------------------------------------ messaging ----
  // node src/node_messaging.cc. MessagePort/MessageChannel and the structured
  // clone entry points come from node:worker_threads / the web globals; the
  // port-lifecycle hooks node drives from C++ have no standalone equivalent
  // and are inert.
  factories["messaging"] = () => {
    const wt = mod("worker_threads");
    return {
      MessagePort: wt.MessagePort || G.MessagePort,
      MessageChannel: wt.MessageChannel || G.MessageChannel,
      DOMException: G.DOMException,
      broadcastChannel: G.BroadcastChannel,
      receiveMessageOnPort: wt.receiveMessageOnPort,
      structuredClone: G.structuredClone,
      drainMessagePort() {},
      stopMessagePort() {},
      moveMessagePortToContext() { throw new Error("moveMessagePortToContext is not supported"); },
      setDeserializerCreateObjectFunction() {},
      exposeLazyDOMExceptionProperty() {},
    };
  };

  // ---------------------------------------------------------- performance ----
  // node src/node_perf.cc. The milestone table is what perf_hooks already
  // reports; the observer plumbing node drives from C++ is inert here, which
  // internal/perf/observe.js tolerates (it only ever counts and installs).
  factories["performance"] = () => {
    const perf = mod("perf_hooks");
    const constants = {
      NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN: 0,
      NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN_TIMESTAMP: 1,
      NODE_PERFORMANCE_MILESTONE_ENVIRONMENT: 2,
      NODE_PERFORMANCE_MILESTONE_NODE_START: 3,
      NODE_PERFORMANCE_MILESTONE_V8_START: 4,
      NODE_PERFORMANCE_MILESTONE_LOOP_START: 5,
      NODE_PERFORMANCE_MILESTONE_LOOP_EXIT: 6,
      NODE_PERFORMANCE_MILESTONE_BOOTSTRAP_COMPLETE: 7,
      NODE_PERFORMANCE_ENTRY_TYPE_GC: 0,
      NODE_PERFORMANCE_ENTRY_TYPE_HTTP2: 1,
      NODE_PERFORMANCE_ENTRY_TYPE_HTTP: 2,
      NODE_PERFORMANCE_ENTRY_TYPE_NET: 3,
      NODE_PERFORMANCE_ENTRY_TYPE_DNS: 4,
      NODE_PERFORMANCE_ENTRY_TYPE_QUIC: 5,
    };
    const milestones = new Float64Array(8);
    const origin = (perf.performance && perf.performance.timeOrigin) || Date.now();
    milestones[constants.NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN] = origin * 1e6;
    milestones[constants.NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN_TIMESTAMP] = origin;
    return {
      constants,
      milestones,
      observerCounts: new Uint32Array(8),
      now: () => (perf.performance ? perf.performance.now() : Date.now() - origin),
      loopIdleTime: () => 0,
      uvMetricsInfo: () => [0, 0, 0],
      markBootstrapComplete() {},
      setupObservers() {},
      installGarbageCollectionTracking() {},
      removeGarbageCollectionTracking() {},
      createELDHistogram: () => (perf.monitorEventLoopDelay ? perf.monitorEventLoopDelay() : {}),
      Histogram: class Histogram {},
    };
  };

  // --------------------------------------------------- encoding_binding ----
  // node src/encoding_binding.cc — the UTF-8 primitives internal/encoding.js
  // builds TextEncoder/TextDecoder on. `encodeIntoResults` is node's shared
  // out-parameter buffer, read straight after each encodeInto call.
  factories["encoding_binding"] = () => {
    const encoder = new TextEncoder();
    const encodeIntoResults = new Uint32Array(2);
    return {
      encodeIntoResults,
      encodeInto(input, dest) {
        const r = encoder.encodeInto(String(input), dest);
        encodeIntoResults[0] = r.read;
        encodeIntoResults[1] = r.written;
      },
      encodeUtf8String: (input) => encoder.encode(String(input)),
      decodeUTF8: (buf, ignoreBOM, hasFatal) =>
        new TextDecoder("utf-8", { ignoreBOM: !!ignoreBOM, fatal: !!hasFatal }).decode(buf),
      toASCII: (s) => s,
      toUnicode: (s) => s,
    };
  };

  // ------------------------------------------------------------ builtins ----
  // node src/node_builtins.cc: the loader that compiles lib/**.js out of the
  // binary's embedded sources. This runtime has no embedded copy — the very
  // reason `internal/x` resolves to a file — so `compileFunction` hands back a
  // wrapper that re-enters the normal module loader for the same id. The id
  // list is read from the lib/ tree those files were resolved from, so it
  // always matches the tree actually in use.
  factories["builtins"] = () => {
    let ids = null;
    const builtinIds = () => {
      if (ids !== null) return ids;
      ids = [];
      try {
        const path = mod("path");
        const fs = mod("fs");
        // internal/util always resolves for a tree that has lib/internal.
        const libDir = path.dirname(path.dirname(G.require.resolve("internal/util")));
        for (const rel of fs.readdirSync(libDir, { recursive: true })) {
          const p = String(rel).split(path.sep).join("/");
          if (p.endsWith(".js")) ids.push(p.slice(0, -3));
        }
      } catch { /* no lib tree: only the public builtins exist */ }
      for (const id of Object.keys(M)) {
        if (!id.includes(":") && !ids.includes(id)) ids.push(id);
      }
      return ids;
    };
    return {
      get builtinIds() { return builtinIds(); },
      natives: {},
      // Arguments must match BuiltinLoader::LookupAndCompile()'s parameter list
      // (realm.js compileForInternalLoader calls it with those six).
      compileFunction: (id) => function (exports, require_, module_) {
        module_.exports = req(id);
        return module_.exports;
      },
      setInternalLoaders() {},
      importBuiltinSourceTextModule() { throw new Error("No such builtin source text module"); },
      getCacheUsage: () => ({ compiledWithCache: [], compiledWithoutCache: [], cacheRejected: [] }),
    };
  };

  // --------------------------------------------------------- module_wrap ----
  // node src/module_wrap.cc. Only the class identity and the status constants
  // are observed by the modules that reach this from a CommonJS entry.
  factories["module_wrap"] = () => ({
    ModuleWrap: class ModuleWrap {
      constructor(url) { this.url = url; }
      instantiate() {}
      evaluate() {}
      setExport() {}
      getNamespace() { return {}; }
      getStatus() { return 0; }
    },
    kUninstantiated: 0, kInstantiating: 1, kInstantiated: 2, kEvaluating: 3,
    kEvaluated: 4, kErrored: 5, kEvaluationPhase: 1, kSourcePhase: 0,
    setImportModuleDynamicallyCallback() {},
    setInitializeImportMetaObjectCallback() {},
    setImportMetaResolveInitializer() {},
    throwIfPromiseRejected() {},
  });

  // ----------------------------------------------------- process_methods ----
  // node src/node_process_methods.cc. internal/process/per_thread.js reads the
  // clock through the shared `hrtimeBuffer` (upper/lower 32 bits of seconds +
  // nanoseconds), so the layout matters, not just the value.
  factories["process_methods"] = () => {
    const p = G.process;
    const hrtimeBuffer = new Uint32Array(4);
    const hrBig = new BigUint64Array(hrtimeBuffer.buffer, 0, 1);
    const nanos = () => {
      const t = p.hrtime.bigint ? p.hrtime.bigint() : BigInt(Math.round(performance.now() * 1e6));
      return t;
    };
    return {
      hrtimeBuffer,
      hrtime() {
        const t = nanos();
        const sec = t / 1000000000n;
        hrtimeBuffer[0] = Number(sec / 0x100000000n);
        hrtimeBuffer[1] = Number(sec % 0x100000000n);
        hrtimeBuffer[2] = Number(t % 1000000000n);
      },
      hrtimeBigInt() { hrBig[0] = nanos(); },
      cpuUsage(array) {
        const u = p.cpuUsage();
        if (array) { array[0] = u.user; array[1] = u.system; }
        return u;
      },
      memoryUsage(array) {
        const m = p.memoryUsage();
        if (array) {
          array[0] = m.rss; array[1] = m.heapTotal; array[2] = m.heapUsed;
          array[3] = m.external; array[4] = m.arrayBuffers;
        }
        return m;
      },
      rss: () => p.memoryUsage().rss,
      resourceUsage(array) { if (array) array.fill(0); return {}; },
      availableMemory: () => { try { return BigInt(mod("os").freemem()); } catch { return 0n; } },
      constrainedMemory: () => 0n,
      uptime: () => p.uptime(),
      umask: (mask) => p.umask(mask),
      reallyExit: (code) => p.reallyExit ? p.reallyExit(code) : p.exit(code),
      _kill: (pid, sig) => { try { p.kill(pid, sig); return 0; } catch (e) { return e.errno || -1; } },
      _rawDebug: (...args) => { p._rawDebug ? p._rawDebug(...args) : console.error(...args); },
      _getActiveHandles: () => (p._getActiveHandles ? p._getActiveHandles() : []),
      _getActiveRequests: () => (p._getActiveRequests ? p._getActiveRequests() : []),
      _getActiveResourcesInfo: () => [],
      _debugProcess() {}, _debugEnd() {},
      patchProcessObject() {},
      setEmitWarningSync() {},
      loadEnvFile() {},
      ref() {}, unref() {},
    };
  };

  // ---------------------------------------------------------------- worker ----
  // node src/node_worker.cc — thread identity plus the resource-limit index
  // layout internal/worker.js reads its options array against.
  factories["worker"] = () => {
    const wt = mod("worker_threads");
    return {
      Worker: wt.Worker,
      threadId: wt.threadId === undefined ? 0 : wt.threadId,
      threadName: "",
      isMainThread: wt.isMainThread !== false,
      isInternalThread: false,
      ownsProcessState: wt.isMainThread !== false,
      resourceLimits: new Float64Array(5),
      getEnvMessagePort: () => wt.parentPort,
      kMaxYoungGenerationSizeMb: 0, kMaxOldGenerationSizeMb: 1,
      kCodeRangeSizeMb: 2, kStackSizeMb: 3, kTotalResourceLimitCount: 4,
    };
  };

  // ------------------------------------------------------------ permission ----
  // node src/permission/permission.cc. Backed by the REAL model, so a node
  // lib/** module that reaches for internalBinding('permission').has gets the
  // same answers process.permission.has gives. Returning `true` unconditionally
  // (as this did) told every caller the sandbox was open.
  factories["permission"] = () => {
    const PN = G.__mbunPermissionNative;
    if (!PN) return { has: () => true, drop() {} };
    return { has: (scope, ref) => PN.has(scope, ref), drop: (scope, ref) => PN.drop(scope, ref) };
  };

  // ------------------------------------------------------------ async_wrap ----
  // node src/async_wrap.cc + src/env.h (Environment::AsyncHooks). The field
  // indices are the enum order in env.h; the arrays ARE the state
  // internal/async_hooks.js reads and writes, so making them plain typed arrays
  // gives node's JS side a working id counter and execution/trigger pair. What
  // is missing is the C++ side that would *emit* init/before/after — this
  // runtime has no such instrumentation, so hooks stay silent rather than
  // pretending to fire.
  factories["async_wrap"] = () => {
    const constants = {
      kInit: 0, kBefore: 1, kAfter: 2, kDestroy: 3, kPromiseResolve: 4,
      kTotals: 5, kCheck: 6, kStackLength: 7, kUsesExecutionAsyncResource: 8,
      kExecutionAsyncId: 0, kTriggerAsyncId: 1, kAsyncIdCounter: 2,
      kDefaultTriggerAsyncId: 3,
    };
    const async_id_fields = new Float64Array(4);
    async_id_fields[constants.kAsyncIdCounter] = 1;
    async_id_fields[constants.kDefaultTriggerAsyncId] = -1;
    return {
      constants,
      async_hook_fields: new Uint32Array(9),
      async_id_fields,
      async_ids_stack: new Float64Array(2 * 16384),
      execution_async_resources: [],
      Providers: { NONE: 0 },
      setCallbackTrampoline() {},
      setupHooks() {},
      setPromiseHooks() {},
      enablePromiseHook() {}, disablePromiseHook() {},
      pushAsyncContext() {}, popAsyncContext() { return false; },
      executionAsyncResource: () => undefined,
      clearAsyncIdStack() {},
      queueDestroyAsyncId() {},
      registerDestroyHook() {},
      getPromiseHooks: () => [],
    };
  };

  // -------------------------------------------------------------- timers ----
  // node src/timers.cc. `lib/internal/timers.js` reads `immediateInfo` /
  // `timeoutInfo` as shared counter arrays and calls the four scheduling
  // methods; this runtime owns its own timer wheel, so the methods are no-ops
  // and the arrays are real (so node's ref-counting arithmetic still balances).
  // What this buys is that the module *loads* — the corpus reaches into it for
  // the `kTimeout` symbol.
  factories["timers"] = () => ({
    immediateInfo: new Uint32Array(3),
    timeoutInfo: new Int32Array(1),
    setupTimers() {},
    // node returns uv_now(loop): milliseconds since loop start, on the SAME
    // clock its timer deadlines are computed against (see node
    // lib/internal/timers.js:387, which uses this very call as the `start` a
    // deadline is measured from). That shared clock is the whole contract:
    // test-timers-ordering asserts a setTimeout(f, 1) chain advances this value
    // by >=1 each hop, which holds by construction only if deadlines and this
    // reading come from one clock.
    //
    // mbun's timer queue computes `at: Date.now() + d` (process_web.cppm), so
    // this must read Date.now() too. Reading performance.now() here — a
    // different origin AND a different resolution — let a 1ms timer satisfy its
    // Date.now()-truncated deadline after as little as 0.1ms of real time, so
    // the trunc'd performance.now() had not advanced and the assert failed
    // ~5 runs in 6. It looked like a merge regression in a corpus diff; it was
    // a long-standing 1-in-6 flake whose baseline run had simply rolled well.
    //
    // Subtracting a start baseline keeps the magnitude SMI-small, which
    // test-timers-now requires (`< 0x3ffffff`, ~18.6h of uptime) and which a
    // raw Date.now() would blow by six orders of magnitude.
    getLibuvNow: () => Date.now() - LOOP_START_MS,
    scheduleTimer() {},
    toggleTimerRef() {},
    toggleImmediateRef() {},
  });

  // ----------------------------------------------------------------- icu ----
  factories["icu"] = () => ({
    icuErrName: (n) => "U_ERROR_" + n,
    transcode: (...args) => mod("buffer").transcode(...args),
    getStringWidth: (s) => String(s).length,
    toASCII: (s) => s,
    toUnicode: (s) => s,
    hasConverter: () => false,
  });

  // A binding namespace whose member set is enumerated from node's C++
  // initializer: reading a name that is NOT implemented throws NAMING ITSELF,
  // so a corpus log reads `No such binding member: fs.lchown` instead of dying
  // three frames later on `undefined is not a function`. This is the same
  // reason internalBinding() itself throws for an unknown binding — the cost of
  // a silent `undefined` is an un-triageable failure far from its cause.
  //
  // Writes are allowed and shadow the implementation, because node's own tests
  // monkey-patch binding members (test-fs-sync-fd-leak assigns writeFileUtf8,
  // test-tls-keyengine-unsupported assigns SecureContext).
  //
  // NOT used for `crypto`: node's own lib/internal/crypto/* feature-DETECTS
  // there (`if (Argon2Job === undefined)`), so throwing on a member OpenSSL did
  // not compile in would break the very code path that handles its absence.
  const strictNs = (ns, obj) => new Proxy(obj, {
    get(t, k, r) {
      if (Reflect.has(t, k)) return Reflect.get(t, k, r);
      if (typeof k === "symbol") return undefined;
      const e = new Error("No such binding member: " + ns + "." + String(k));
      e.code = "ERR_INVALID_MODULE";
      throw e;
    },
  });

  // ------------------------------------------------------------------- fs ----
  // node src/node_file.cc. Three call conventions share every entry point, and
  // getting the dispatch right is the whole of it — the last argument decides:
  //   * `undefined`             → synchronous; throw on error, return the value
  //   * an `FSReqCallback`      → async; `req.oncomplete(err)` / `(null, value)`
  //   * `kUsePromises`          → return a Promise
  // A trailing `ctx` object may follow `undefined`; node's older entries filled
  // `ctx.errno` instead of throwing and `handleErrorFromBinding(ctx)` re-threw.
  // Throwing directly is what node's current entries do, and it leaves `ctx`
  // untouched — which is what test-fs-filehandle asserts (`ctx.errno`
  // undefined).
  //
  // THE PERMISSION MODEL: every syscall below is reached through
  // `__mbunFsNative` / `__mbunFdNative`. Those ARE the C++ boundary the
  // --permission fs gate lives at (io_bindings.inc `permission_deny_fs`, called
  // from all 21 fsn_* entries and fdn_open). This binding therefore adds NO
  // second route to the filesystem: `internalBinding('fs').open` is gated by
  // exactly the same --allow-fs-read/--allow-fs-write check as fs.openSync, and
  // there is deliberately no path here that calls a syscall any other way.
  factories["fs"] = () => {
    const kUsePromises = Symbol("kUsePromises");
    // node src/node_file.h kFsStatsFieldsNumber. StatWatcher hands node's JS
    // side TWO stats in one array (current at 0, previous at 18), so the buffer
    // is twice the field count — internal/fs/watchers.js reads offset 18.
    const kFsStatsFieldsNumber = 18;
    const statValues = new Float64Array(kFsStatsFieldsNumber * 2);
    const bigintStatValues = new BigInt64Array(kFsStatsFieldsNumber * 2);
    const statFsValues = new Float64Array(8);
    const bigintStatFsValues = new BigInt64Array(8);
    const FSN = () => G.__mbunFsNative;
    const FDN = () => G.__mbunFdNative;
    const fsjs = () => mod("fs");

    class FSReqCallback {
      constructor(bigint = false) {
        this.oncomplete = undefined;
        this.bigint = bigint;
        this.context = undefined;
      }
    }

    const dispatch = (req, run) => {
      if (req === undefined || req === null) return run();
      if (req === kUsePromises) {
        return new Promise((resolve, reject) => {
          queueMicrotask(() => { try { resolve(run()); } catch (e) { reject(e); } });
        });
      }
      queueMicrotask(() => {
        let v; let err = null;
        try { v = run(); } catch (e) { err = e; }
        const oc = req.oncomplete;
        if (typeof oc !== "function") return;
        if (err !== null) oc.call(req, err); else oc.call(req, null, v);
      });
      return undefined;
    };

    // sec/nsec pair per node's FillStatsArray. The seconds come from the
    // fractional-ms field (a double holds ~1.7e12 ms exactly); the bigint form
    // uses the integer-nanosecond field, which is what internal/fs/utils
    // nsFromTimeSpecBigInt recombines.
    const fillTime = (A, i, ms, ns, big) => {
      if (big) {
        const total = BigInt(Math.round(ns));
        A[i] = total / 1000000000n;
        A[i + 1] = total % 1000000000n;
      } else {
        const sec = Math.floor(ms / 1000);
        A[i] = sec;
        A[i + 1] = Math.round((ms - sec * 1000) * 1e6);
      }
    };
    // node src/node_file.cc FillStatsArray. The INDEX LAYOUT IS THE CONTRACT
    // internal/fs/utils.js getStatsFromBinding decodes positionally: a
    // reordering here does not error, it silently produces garbage Stats.
    const fillStats = (s, big, offset = 0) => {
      const A = big ? bigintStatValues : statValues;
      const n = big ? ((v) => BigInt(Math.trunc(v))) : ((v) => v);
      A[offset + 0] = n(s.dev);
      A[offset + 1] = n(s.mode);
      A[offset + 2] = n(s.nlink);
      A[offset + 3] = n(s.uid);
      A[offset + 4] = n(s.gid);
      A[offset + 5] = n(s.rdev);
      A[offset + 6] = n(s.blksize);
      A[offset + 7] = n(s.ino);
      A[offset + 8] = n(s.size);
      A[offset + 9] = n(s.blocks);
      fillTime(A, offset + 10, s.atimeMs, s.atimeNs, big);
      fillTime(A, offset + 12, s.mtimeMs, s.mtimeNs, big);
      fillTime(A, offset + 14, s.ctimeMs, s.ctimeNs, big);
      fillTime(A, offset + 16, s.birthtimeMs, s.birthtimeNs, big);
      return A;
    };
    const fillStatFs = (s, big) => {
      const A = big ? bigintStatFsValues : statFsValues;
      const n = big ? ((v) => BigInt(Math.trunc(v))) : ((v) => v);
      A[0] = n(s.type); A[1] = n(s.bsize); A[2] = n(s.frsize); A[3] = n(s.blocks);
      A[4] = n(s.bfree); A[5] = n(s.bavail); A[6] = n(s.files); A[7] = n(s.ffree);
      return A;
    };

    // ENOENT with throwIfNoEntry=false is the one error node swallows.
    const statLike = (get, big, req, throwIfNoEntry) => dispatch(req, () => {
      let s;
      try { s = get(); } catch (e) {
        // stat's `throwIfNoEntry: false` swallows ENOENT; fstat's
        // `shouldNotThrow` reaches here with the same flag and a bad fd.
        if (throwIfNoEntry === false && e && (e.code === "ENOENT" || e.code === "EBADF")) {
          return undefined;
        }
        throw e;
      }
      return fillStats(s, big);
    });

    const toBuf = (v) => (typeof v === "string" ? mod("buffer").Buffer.from(v) : v);
    // node's `encoding` argument on readlink/realpath/mkdtemp: "buffer" hands
    // back a Buffer, anything else a string in that encoding.
    const encode = (str, encoding) => {
      if (encoding === "buffer") return mod("buffer").Buffer.from(str, "utf8");
      if (encoding === undefined || encoding === null || encoding === "utf8" ||
          encoding === "utf-8") return str;
      return mod("buffer").Buffer.from(str, "utf8").toString(encoding);
    };

    // libuv dirent type codes (uv.h uv_dirent_type_t) — the values node's
    // fs.constants.UV_DIRENT_* publish and internal/fs/utils Dirent switches on.
    const UV_DIRENT_UNKNOWN = 0, UV_DIRENT_FILE = 1, UV_DIRENT_DIR = 2,
          UV_DIRENT_LINK = 3, UV_DIRENT_FIFO = 4, UV_DIRENT_SOCKET = 5,
          UV_DIRENT_CHAR = 6, UV_DIRENT_BLOCK = 7;
    const direntType = (full) => {
      let st;
      try { st = FSN().stat(full, true); } catch { return UV_DIRENT_UNKNOWN; }
      const t = st.mode & 61440;
      if (t === 32768) return UV_DIRENT_FILE;
      if (t === 16384) return UV_DIRENT_DIR;
      if (t === 40960) return UV_DIRENT_LINK;
      if (t === 4096) return UV_DIRENT_FIFO;
      if (t === 49152) return UV_DIRENT_SOCKET;
      if (t === 8192) return UV_DIRENT_CHAR;
      if (t === 24576) return UV_DIRENT_BLOCK;
      return UV_DIRENT_UNKNOWN;
    };

    // node's C++ FileHandle: an fd plus a close that is safe to call twice. The
    // lib-level `FileHandle` in internal/fs/promises.js wraps this one.
    class FileHandle {
      constructor(fd) { this.fd = fd; this[Symbol.for("closed")] = false; }
      close() {
        if (this.fd < 0) return Promise.resolve();
        const fd = this.fd;
        this.fd = -1;
        return new Promise((resolve, reject) => {
          try { FDN().close(fd); resolve(); } catch (e) { reject(e); }
        });
      }
      release() { this.fd = -1; }
      // node emits this warning from C++ when a FileHandle is GC'd unclosed.
      onclose() {}
    }

    const impl = {
      FSReqCallback,
      FileHandle,
      kUsePromises,
      kFsStatsFieldsNumber,
      statValues,
      bigintStatValues,
      statFsValues,
      bigintStatFsValues,

      access: (path, mode, req) => dispatch(req, () => { fsjs().accessSync(path, mode); }),
      close: (fd, req) => dispatch(req, () => { FDN().close(fd); }),
      open: (path, flags, mode, req) =>
        dispatch(req, () => FDN().open(String(path), flags, mode)),
      openFileHandle: (path, flags, mode, req) =>
        dispatch(req, () => new FileHandle(FDN().open(String(path), flags, mode))),
      read: (fd, buffer, offset, length, position, req) =>
        dispatch(req, () => FDN().read(fd, buffer, offset || 0,
                                      length == null ? buffer.byteLength : length,
                                      position == null ? -1 : Number(position))),
      readBuffers: (fd, buffers, position, req) => dispatch(req, () => {
        let total = 0;
        let pos = position == null ? -1 : Number(position);
        for (const b of buffers) {
          const n = FDN().read(fd, b, 0, b.byteLength, pos);
          total += n;
          if (pos >= 0) pos += n;
          if (n < b.byteLength) break;
        }
        return total;
      }),
      writeBuffer: (fd, buffer, offset, length, position, req) =>
        dispatch(req, () => FDN().write(fd, buffer, offset || 0,
                                       length == null ? buffer.byteLength - (offset || 0) : length,
                                       position == null ? -1 : Number(position))),
      writeBuffers: (fd, buffers, position, req) => dispatch(req, () => {
        let total = 0;
        let pos = position == null ? -1 : Number(position);
        for (const b of buffers) {
          const n = FDN().write(fd, b, 0, b.byteLength, pos);
          total += n;
          if (pos >= 0) pos += n;
        }
        return total;
      }),
      // node: writeString(fd, string, position, encoding, req)
      writeString: (fd, string, position, encoding, req) => dispatch(req, () => {
        const b = mod("buffer").Buffer.from(String(string), encoding || "utf8");
        return FDN().write(fd, b, 0, b.byteLength, position == null ? -1 : Number(position));
      }),
      // The C++ fast paths. Node's own tests replace these to prove the JS layer
      // uses them (test-fs-sync-fd-leak), so they must be real, not aliases.
      writeFileUtf8: (pathOrFd, data, flags, mode) => {
        if (typeof pathOrFd === "number") {
          const b = mod("buffer").Buffer.from(String(data), "utf8");
          return FDN().write(pathOrFd, b, 0, b.byteLength, -1);
        }
        FSN().writeFile(String(pathOrFd), String(data));
        return undefined;
      },
      readFileUtf8: (pathOrFd, flags) => {
        if (typeof pathOrFd === "number") return fsjs().readFileSync(pathOrFd, "utf8");
        return FSN().readFile(String(pathOrFd));
      },
      existsSync: (path) => {
        try { return FSN().exists(String(path)); } catch { return false; }
      },
      // node src/node_file.cc InternalModuleStat: -errno, 0 for a file, 1 for a
      // directory. The module loader's hot path; it never throws.
      internalModuleStat: (path) => {
        let s;
        try { s = FSN().stat(String(path)); } catch (e) {
          return typeof e.errno === "number" ? e.errno : -2;
        }
        return (s.mode & 61440) === 16384 ? 1 : 0;
      },
      stat: (path, big, req, throwIfNoEntry) =>
        statLike(() => FSN().stat(String(path)), big, req, throwIfNoEntry),
      lstat: (path, big, req, throwIfNoEntry) =>
        statLike(() => FSN().stat(String(path), true), big, req, throwIfNoEntry),
      // node's 4th arg here is `shouldNotThrow`, the inverse of stat's
      // `throwIfNoEntry`: true means swallow the error and return undefined.
      fstat: (fd, big, req, shouldNotThrow) =>
        statLike(() => FSN().fstat(fd), big, req, shouldNotThrow === true ? false : undefined),
      statfs: (path, big, req) =>
        dispatch(req, () => fillStatFs(FSN().statfs(String(path)), big)),
      // node: readdir(path, encoding, withFileTypes, req) → names, or
      // [names, types] when withFileTypes.
      readdir: (path, encoding, withFileTypes, req) => dispatch(req, () => {
        const p = String(path);
        const names = FSN().readdir(p);
        const out = encoding === "buffer"
          ? names.map((n) => mod("buffer").Buffer.from(n, "utf8"))
          : names;
        if (!withFileTypes) return out;
        const sep = p.endsWith("/") ? "" : "/";
        return [out, names.map((n) => direntType(p + sep + n))];
      }),
      readlink: (path, encoding, req) =>
        dispatch(req, () => encode(FSN().readlink(String(path)), encoding)),
      realpath: (path, encoding, req) =>
        dispatch(req, () => encode(FSN().realpath(String(path)), encoding)),
      mkdtemp: (prefix, encoding, req) =>
        dispatch(req, () => encode(FSN().mkdtemp(String(prefix)), encoding)),
      unlink: (path, req) => dispatch(req, () => { FSN().unlink(String(path)); }),
      rename: (from, to, req) => dispatch(req, () => { FSN().rename(String(from), String(to)); }),
      copyFile: (src, dest, mode, req) =>
        dispatch(req, () => { FSN().copyFile(String(src), String(dest), mode); }),
      link: (from, to, req) => dispatch(req, () => { fsjs().linkSync(from, to); }),
      symlink: (target, path, type, req) =>
        dispatch(req, () => { FSN().symlink(String(target), String(path)); }),
      mkdir: (path, mode, recursive, req) => dispatch(req, () => {
        const r = FSN().mkdir(String(path), !!recursive, mode);
        // node returns the first directory created when recursive.
        return recursive ? r : undefined;
      }),
      rmdir: (path, req) => dispatch(req, () => { FSN().rmdir(String(path)); }),
      rmSync: (path, maxRetries, recursive, retryDelay) => {
        FSN().rm(String(path), !!recursive, false);
      },
      chmod: (path, mode, req) => dispatch(req, () => { FSN().chmod(String(path), mode); }),
      fchmod: (fd, mode, req) => dispatch(req, () => { fsjs().fchmodSync(fd, mode); }),
      chown: (path, uid, gid, req) => dispatch(req, () => { fsjs().chownSync(path, uid, gid); }),
      fchown: (fd, uid, gid, req) => dispatch(req, () => { fsjs().fchownSync(fd, uid, gid); }),
      lchown: (path, uid, gid, req) => dispatch(req, () => { fsjs().lchownSync(path, uid, gid); }),
      utimes: (path, atime, mtime, req) =>
        dispatch(req, () => { FSN().utimes(String(path), atime, mtime); }),
      futimes: (fd, atime, mtime, req) => dispatch(req, () => { fsjs().futimesSync(fd, atime, mtime); }),
      lutimes: (path, atime, mtime, req) => dispatch(req, () => { fsjs().lutimesSync(path, atime, mtime); }),
      ftruncate: (fd, len, req) => dispatch(req, () => { FDN().ftruncate(fd, len || 0); }),
      fdatasync: (fd, req) => dispatch(req, () => { fsjs().fdatasyncSync(fd); }),
      fsync: (fd, req) => dispatch(req, () => { fsjs().fsyncSync(fd); }),
      // node src/node_file.cc: 0 none, 1 module, 2 commonjs, based on the
      // nearest package.json "type". mbun's loader owns that decision; reporting
      // "no extension gives no format" is the honest answer here.
      getFormatOfExtensionlessFile: () => 0,
      cpSyncCheckPaths: () => undefined,
      // node's StatWatcher (uv_fs_poll). Backed by the same poll node's
      // lib/internal/fs/watchers.js expects: start(path, interval) then
      // onchange(current, previous) reading the shared stat array.
      StatWatcher: class StatWatcher {
        constructor(useBigint) { this.bigint = !!useBigint; this._timer = null; this._prev = null; }
        start(path, interval) {
          const A = this.bigint ? bigintStatValues : statValues;
          const read = () => {
            let s = null;
            try { s = FSN().stat(String(path)); } catch { s = null; }
            return s;
          };
          const zero = () => {
            for (let i = 0; i < kFsStatsFieldsNumber; i++) A[i] = this.bigint ? 0n : 0;
          };
          this._prev = read();
          const tick = () => {
            const cur = read();
            if (cur === null) zero(); else fillStats(cur, this.bigint, 0);
            if (this._prev === null) {
              for (let i = 0; i < kFsStatsFieldsNumber; i++) {
                A[kFsStatsFieldsNumber + i] = this.bigint ? 0n : 0;
              }
            } else {
              fillStats(this._prev, this.bigint, kFsStatsFieldsNumber);
            }
            const changed = cur === null || this._prev === null ||
                            cur.mtimeMs !== this._prev.mtimeMs || cur.size !== this._prev.size ||
                            cur.ino !== this._prev.ino;
            this._prev = cur;
            if (changed && typeof this.onchange === "function") {
              this.onchange(A, A.subarray(kFsStatsFieldsNumber));
            }
          };
          this._timer = setInterval(tick, interval > 0 ? interval : 5007);
          if (this._timer && typeof this._timer.unref === "function") this._timer.unref();
          return 0;
        }
        close() { if (this._timer !== null) { clearInterval(this._timer); this._timer = null; } }
        ref() { if (this._timer && this._timer.ref) this._timer.ref(); }
        unref() { if (this._timer && this._timer.unref) this._timer.unref(); }
      },
      UV_DIRENT_UNKNOWN, UV_DIRENT_FILE, UV_DIRENT_DIR, UV_DIRENT_LINK,
      UV_DIRENT_FIFO, UV_DIRENT_SOCKET, UV_DIRENT_CHAR, UV_DIRENT_BLOCK,
    };
    return strictNs("fs", impl);
  };

  // ---------------------------------------------------------------- fs_dir ----
  // node src/node_dir.cc. opendir over the readdir this runtime has; the
  // DirHandle keeps the caller's position in the name list.
  factories["fs_dir"] = () => {
    const opendir = (path) => {
      const names = G.__mbunFsNative.readdir(String(path));
      let i = 0;
      return {
        read(bufferSize) {
          if (i >= names.length) return null;
          const n = names[i++];
          return [n, 1];
        },
        close() { i = names.length; },
      };
    };
    return strictNs("fs_dir", {
      opendir: (path, encoding, req) => {
        const h = opendir(path);
        if (req === undefined || req === null) return h;
        queueMicrotask(() => { if (typeof req.oncomplete === "function") req.oncomplete(null, h); });
        return undefined;
      },
      opendirSync: (path) => opendir(path),
    });
  };

  // --------------------------------------------------------- fs_event_wrap ----
  // node src/fs_event_wrap.cc. Backed by node:fs.watch so a JS-side FSEvent
  // observes real changes rather than never firing.
  factories["fs_event_wrap"] = () => ({
    FSEvent: class FSEvent {
      constructor() { this._w = null; this.onchange = undefined; }
      start(path, persistent, recursive) {
        try {
          this._w = mod("fs").watch(String(path), { persistent: !!persistent, recursive: !!recursive },
            (event, filename) => {
              if (typeof this.onchange === "function") {
                // node's FSEvent flags: 1 = rename, 2 = change.
                this.onchange(0, event === "rename" ? 1 : 2, filename);
              }
            });
        } catch (e) { return typeof e.errno === "number" ? e.errno : -2; }
        return 0;
      }
      close() { if (this._w !== null) { this._w.close(); this._w = null; } }
      ref() {} unref() {}
    },
  });

  // --------------------------------------------------------------- crypto ----
  // node src/crypto/*. NOT wrapped in strictNs: node's own
  // lib/internal/crypto/util.js feature-detects the optional algorithms by
  // destructuring them and testing for `undefined` (Argon2Job, the ML_DSA/ML_KEM
  // key types), so a throwing member would break the code that handles absence.
  //
  // What this buys is that `internal/crypto/util` and `internal/tls/common`
  // LOAD — the whole node lib crypto/tls JS graph top-levels this binding.
  factories["crypto"] = () => {
    const c = mod("crypto");
    const list = (fn) => (typeof fn === "function" ? fn() : []);
    return {
      getCiphers: () => list(c.getCiphers),
      getCurves: () => list(c.getCurves),
      getHashes: () => list(c.getHashes),
      // node's cached OBJ_NAME alias table (getCachedAliases) — an empty map is
      // the "nothing cached yet" state its callers already handle.
      getCachedAliases: () => ({ __proto__: null }),
      setEngine: () => {
        const e = new Error("Custom engines not supported by this OpenSSL");
        e.code = "ERR_CRYPTO_CUSTOM_ENGINE_NOT_SUPPORTED";
        throw e;
      },
      secureHeapUsed: () => ({ total: 0, min: 0, used: 0, utilization: 0 }),
      // OpenSSL's @SECLEVEL. 1 is the OpenSSL 3.x default and the level node's
      // own default build reports; the corpus reads it to pick key sizes.
      getOpenSSLSecLevelCrypto: () => 1,
      getOpenSSLSecLevel: () => 1,
      // Optional algorithms this build does not carry. `undefined` IS the
      // signal node's lib checks for — see the note above.
      EVP_PKEY_ML_DSA_44: undefined, EVP_PKEY_ML_DSA_65: undefined,
      EVP_PKEY_ML_DSA_87: undefined, EVP_PKEY_ML_KEM_512: undefined,
      EVP_PKEY_ML_KEM_768: undefined, EVP_PKEY_ML_KEM_1024: undefined,
      kKeyVariantAES_OCB_128: undefined,
      Argon2Job: undefined,
      KmacJob: undefined,
      // node's C++ SecureContext. mbun's TLS owns its own context, so this is
      // the shape node's internal/tls/common.js drives — enough for that module
      // to load and for a test to observe which setters exist.
      SecureContext: class SecureContext {
        init() {} setKey() {} setCert() {} addCACert() {} addCRL() {}
        addRootCerts() {} setCipherSuites() {} setCiphers() {} setSigalgs() {}
        setECDHCurve() {} setDHParam() {} setMaxProto() {} setMinProto() {}
        getMaxProto() { return 0; } getMinProto() { return 0; }
        setOptions() {} setSessionIdContext() {} setSessionTimeout() {}
        close() {} loadPKCS12() {} setTicketKeys() {} getTicketKeys() {}
        setFreeListLength() {} enableTicketKeyCallback() {}
        setClientCertEngine() {} setEngineKey() {}
        setAlpnProtocols() {} setKeylogCallback() {} setOCSPResponse() {}
        setVerifyMode() {} setPskIdentityHint() {} enableTrace() {}
      },
      // node throws this for an unknown OpenSSL error name.
      getRootCertificates: () => (typeof c.getRootCertificates === "function"
        ? c.getRootCertificates() : []),
      getFipsCrypto: () => 0,
      setFipsCrypto: () => {},
      testFipsCrypto: () => 0,
      timingSafeEqual: c.timingSafeEqual,
      randomBytes: (size, buf) => {
        const out = buf === undefined ? mod("buffer").Buffer.allocUnsafe(size) : buf;
        c.randomFillSync(out, 0, size);
        return out;
      },
    };
  };

  // -------------------------------------------------------------- tls_wrap ----
  // node src/crypto/crypto_tls.cc. `HAVE_SSL_TRACE` is the build flag node's
  // own tests gate on: this build compiles no SSL_trace(), and reporting false
  // makes those tests skip themselves rather than fail on a lie.
  factories["tls_wrap"] = () => ({
    HAVE_SSL_TRACE: false,
    TLSWrap: class TLSWrap {
      constructor() { this._parent = null; this._secureContext = null; }
      start() {} setVerifyMode() {} enableSessionCallbacks() {}
      enableKeylogCallback() {} enableTrace() {} enableCertCb() {}
      destroySSL() {} close() {} receive() {} setServername() {}
      setSession() {} getSession() { return undefined; }
      getPeerCertificate() { return undefined; }
      getCertificate() { return undefined; }
      getCipher() { return undefined; }
      getProtocol() { return "TLSv1.3"; }
      getEphemeralKeyInfo() { return {}; }
      getFinished() { return undefined; }
      getPeerFinished() { return undefined; }
      getSharedSigalgs() { return []; }
      getTLSTicket() { return undefined; }
      verifyError() { return undefined; }
      endParser() {} shutdownSSL() {} requestOCSP() {}
      exportKeyingMaterial() { return undefined; }
      ref() {} unref() {}
    },
    wrap: () => { throw new Error("tls_wrap.wrap is not supported"); },
  });

  // ----------------------------------------------------------- stream_pipe ----
  // node src/stream_pipe.cc. internal/http2/core.js:192 destructures StreamPipe
  // from this binding at MODULE LOAD time, so an unregistered binding made
  // `require('node:http2')` itself throw "No such binding: stream_pipe" — three
  // corpus files failed there without ever touching a pipe.
  //
  // StreamPipe here THROWS when constructed rather than resolving to a no-op.
  // That is deliberate: mbun has no stream-to-stream pipe, and the only caller
  // is http2's respondWithFile/respondWithFD. A silently-succeeding stub would
  // make respondWithFile "work" while sending nothing, and this project has
  // already found four defects of exactly that shape (a dead-code resolver gate,
  // a validated-then-dropped `ciphers` option, an accepted-then-unenforced
  // --permission flag, and a process._rawDebug that wrote nowhere). Failing at
  // the point of use keeps the unimplemented path honest and visible.
  factories["stream_pipe"] = () => strictNs("stream_pipe", {
    StreamPipe: class StreamPipe {
      constructor() {
        const err = new Error(
          "StreamPipe is not implemented in mbun (internalBinding('stream_pipe')); " +
          "http2 respondWithFile/respondWithFD needs a native stream-to-stream pipe");
        err.code = "ERR_METHOD_NOT_IMPLEMENTED";
        throw err;
      }
    },
  });

  // ----------------------------------------------------------- stream_wrap ----
  // node src/stream_base.cc / src/stream_wrap.cc. `streamBaseState` is the
  // shared out-parameter array internal/stream_base_commons.js reads after
  // every writev/read, so the index constants below are load-bearing.
  factories["stream_wrap"] = () => {
    const kReadBytesOrError = 0, kArrayBufferOffset = 1, kBytesWritten = 2,
          kLastWriteWasAsync = 3, kNumStreamBaseStateFields = 4;
    return strictNs("stream_wrap", {
      kReadBytesOrError, kArrayBufferOffset, kBytesWritten, kLastWriteWasAsync,
      kNumStreamBaseStateFields,
      streamBaseState: new Int8Array(kNumStreamBaseStateFields),
      ShutdownWrap: class ShutdownWrap {
        constructor() { this.handle = null; this.oncomplete = undefined; this.callback = undefined; }
        ref() {} unref() {}
      },
      WriteWrap: class WriteWrap {
        constructor() { this.handle = null; this.oncomplete = undefined; this.async = false; }
        ref() {} unref() {}
      },
      LibuvStreamWrap: class LibuvStreamWrap {
        readStart() { return 0; } readStop() { return 0; }
        shutdown() { return 0; } writev() { return 0; }
        writeBuffer() { return 0; } writeAsciiString() { return 0; }
        writeUtf8String() { return 0; } writeUcs2String() { return 0; }
        writeLatin1String() { return 0; }
        useUserBuffer() {} setBlocking() { return 0; }
        close() {} ref() {} unref() {}
      },
    });
  };

  // ------------------------------------------------------------- js_stream ----
  // node src/js_stream.cc: the C++ side of internal/js_stream_socket.js, which
  // adapts an arbitrary JS duplex to a libuv stream handle. Everything it does
  // is dispatch back into the JS object's own callbacks, so it is expressible
  // here in full.
  factories["js_stream"] = () => {
    const sw = internalBinding("stream_wrap");
    return strictNs("js_stream", {
      JSStream: class JSStream {
        constructor() {
          this.onread = undefined;
          this.onreadstart = undefined;
          this.onreadstop = undefined;
          this.onshutdown = undefined;
          this.onwrite = undefined;
          this._closed = false;
        }
        // node's readBuffer pushes bytes up: it records the length in
        // streamBaseState[kReadBytesOrError] and calls the stream's onStreamRead.
        readBuffer(buf) {
          sw.streamBaseState[sw.kReadBytesOrError] = buf.length;
          if (typeof this.onStreamRead === "function") this.onStreamRead(buf);
          return 0;
        }
        emitEOF() {
          sw.streamBaseState[sw.kReadBytesOrError] = 0;
          if (typeof this.onStreamRead === "function") this.onStreamRead(undefined);
        }
        readStart() { if (typeof this.onreadstart === "function") this.onreadstart(); return 0; }
        readStop() { if (typeof this.onreadstop === "function") this.onreadstop(); return 0; }
        shutdown(req) { if (typeof this.onshutdown === "function") this.onshutdown(req); return 0; }
        writev(req, chunks) { if (typeof this.onwrite === "function") this.onwrite(req, chunks); return 0; }
        writeBuffer(req, buf) { if (typeof this.onwrite === "function") this.onwrite(req, [buf]); return 0; }
        doClose() { this._closed = true; }
        close(cb) { this._closed = true; if (typeof cb === "function") cb(); }
        isAlive() { return !this._closed; }
        isClosing() { return this._closed; }
        ref() {} unref() {}
      },
    });
  };

  // -------------------------------------------------------------- tcp_wrap ----
  // node src/tcp_wrap.cc. This runtime's net is not built on a libuv handle
  // reachable from JS, so `bind` reports EADDRNOTAVAIL rather than pretending
  // an address is bindable — `common/net.js hasMultiLocalhost()` then reports
  // "no second localhost", which makes the tests that need one skip themselves
  // instead of failing on a false claim.
  factories["tcp_wrap"] = () => ({
    // node src/tcp_wrap.h SocketType
    constants: { SOCKET: 0, SERVER: 1, UV_TCP_IPV6ONLY: 1, UV_TCP_REUSEPORT: 4 },
    TCP: class TCP {
      constructor(type) { this.type = type; this.reading = false; }
      bind() { return -99; }
      bind6() { return -99; }
      listen() { return -99; }
      connect() { return -99; }
      connect6() { return -99; }
      open() { return -9; }
      getsockname() { return -9; }
      getpeername() { return -9; }
      setNoDelay() { return 0; }
      setKeepAlive() { return 0; }
      setSimultaneousAccepts() { return 0; }
      readStart() { return 0; } readStop() { return 0; }
      close(cb) { if (typeof cb === "function") cb(); }
      ref() {} unref() {}
    },
    TCPConnectWrap: class TCPConnectWrap {
      constructor() { this.oncomplete = undefined; }
    },
  });

  // ------------------------------------------------------------- pipe_wrap ----
  factories["pipe_wrap"] = () => ({
    constants: { SOCKET: 0, SERVER: 1, IPC: 2, UV_READABLE: 1, UV_WRITABLE: 2 },
    Pipe: class Pipe {
      constructor(type) { this.type = type; }
      bind() { return -99; } listen() { return -99; }
      connect() { return -99; } open() { return -9; }
      fchmod() { return -9; }
      close(cb) { if (typeof cb === "function") cb(); }
      ref() {} unref() {}
    },
    PipeConnectWrap: class PipeConnectWrap { constructor() { this.oncomplete = undefined; } },
  });

  // ----------------------------------------------------------- http_parser ----
  // node src/node_http_parser.cc. The callback-slot indices and the type
  // constants ARE the protocol between node's lib/_http_common.js and llhttp;
  // this runtime parses HTTP in its own layer rather than through a JS-visible
  // parser, so `execute`/`consume` report "not supported" instead of silently
  // consuming nothing — a parser that swallows bytes is worse than one that
  // says it cannot.
  factories["http_parser"] = () => {
    const P = class HTTPParser {
      constructor() { this[HTTPParser.kOnMessageBegin] = undefined; }
      initialize() {}
      close() {}
      free() {}
      remove() {}
      execute() { throw new Error("http_parser.execute is not supported"); }
      finish() { return undefined; }
      pause() {} resume() {}
      consume() { throw new Error("http_parser.consume is not supported"); }
      unconsume() {}
      getCurrentBuffer() { return mod("buffer").Buffer.alloc(0); }
      duration() { return 0; }
      headersCompleted() { return false; }
    };
    // node src/node_http_parser.cc `enum parser_types` + the kOn* callback slots.
    P.REQUEST = 1;
    P.RESPONSE = 2;
    P.kOnMessageBegin = 0;
    P.kOnHeaders = 1;
    P.kOnHeadersComplete = 2;
    P.kOnBody = 3;
    P.kOnMessageComplete = 4;
    P.kOnExecute = 5;
    P.kOnTimeout = 6;
    P.kLenientNone = 0;
    P.kLenientHeaders = 1 << 0;
    P.kLenientChunkedLength = 1 << 1;
    P.kLenientKeepAlive = 1 << 2;
    P.kLenientTransferEncoding = 1 << 3;
    P.kLenientVersion = 1 << 4;
    P.kLenientDataAfterClose = 1 << 5;
    P.kLenientOptionalLFAfterCR = 1 << 6;
    P.kLenientOptionalCRLFAfterChunk = 1 << 7;
    P.kLenientOptionalCRBeforeLF = 1 << 8;
    P.kLenientSpacesAfterChunkSize = 1 << 9;
    P.kLenientAll = (1 << 10) - 1;
    return strictNs("http_parser", {
      HTTPParser: P,
      methods: [
        "DELETE", "GET", "HEAD", "POST", "PUT", "CONNECT", "OPTIONS", "TRACE",
        "COPY", "LOCK", "MKCOL", "MOVE", "PROPFIND", "PROPPATCH", "SEARCH",
        "UNLOCK", "BIND", "REBIND", "UNBIND", "ACL", "REPORT", "MKACTIVITY",
        "CHECKOUT", "MERGE", "M-SEARCH", "NOTIFY", "SUBSCRIBE", "UNSUBSCRIBE",
        "PATCH", "PURGE", "MKCALENDAR", "LINK", "UNLINK", "SOURCE", "QUERY",
      ],
      allMethods: [],
      ConnectionsList: class ConnectionsList {
        constructor() { this._all = []; }
        all() { return this._all; }
        idle() { return []; }
        active() { return []; }
        expired() { return []; }
      },
    });
  };

  // ------------------------------------------------------------ signal_wrap ----
  // node src/signal_wrap.cc. The handle is a real one: start(signum) arms the
  // POSIX handler through the same seam process.on("SIG…") uses (engine.inc
  // signalWatch + __mbunSignalTick), so a Signal that reports success actually
  // delivers. Every method is brand-checked — node's C++ methods unwrap `this`
  // and throw "Illegal invocation" for a foreign receiver, which is the whole
  // point of test-signal-safety.
  factories["signal_wrap"] = () => {
    const BRAND = new WeakSet();
    const P = G.process;
    const nameOf = (signum) => {
      try {
        const sigs = (mod("os").constants || {}).signals || {};
        for (const k of Object.keys(sigs)) if (sigs[k] === signum) return k;
      } catch (e) {}
      return null;
    };
    const self = (o) => { if (!BRAND.has(o)) throw new TypeError("Illegal invocation"); return o; };
    return {
      Signal: class Signal {
        constructor() {
          BRAND.add(this);
          this.onsignal = undefined;
          this._name = null;
          this._listener = null;
        }
        start(signum) {
          self(this);
          const name = nameOf(signum);
          if (name === null || !P || typeof P.on !== "function") return -22;  // UV_EINVAL
          if (this._listener) this.stop();
          const handle = this;
          this._name = name;
          this._listener = function () {
            if (typeof handle.onsignal === "function") handle.onsignal(signum);
          };
          P.on(name, this._listener);
          return 0;
        }
        stop() {
          self(this);
          if (this._listener && P && typeof P.removeListener === "function") {
            P.removeListener(this._name, this._listener);
          }
          this._listener = null;
          this._name = null;
          return 0;
        }
        close(cb) { self(this); this.stop(); if (typeof cb === "function") cb(); }
        ref() { self(this); }
        unref() { self(this); }
        hasRef() { self(this); return this._listener !== null; }
      },
    };
  };

  // ------------------------------------------------------------- cares_wrap ----
  // node src/cares_wrap.cc, as far as node:dns needs it here. mbun resolves
  // through its own DNS layer rather than c-ares, so this exposes the constants
  // and the GetAddrInfoReqWrap/ChannelWrap shells the corpus reaches for; the
  // query methods report UV_ENOSYS rather than silently succeeding.
  factories["cares_wrap"] = () => ({
    // node dns.constants (ADDRCONFIG/V4MAPPED/ALL) + the ai_family codes.
    AI_ADDRCONFIG: 1024, AI_ALL: 256, AI_V4MAPPED: 8,
    GetAddrInfoReqWrap: class GetAddrInfoReqWrap { constructor() { this.oncomplete = undefined; } },
    GetNameInfoReqWrap: class GetNameInfoReqWrap { constructor() { this.oncomplete = undefined; } },
    QueryReqWrap: class QueryReqWrap { constructor() { this.oncomplete = undefined; } },
    ChannelWrap: class ChannelWrap {
      constructor() { this._servers = []; }
      getServers() { return this._servers.slice(); }
      setServers(list) { this._servers = Array.isArray(list) ? list.slice() : []; return 0; }
      setLocalAddress() { return 0; }
      cancel() {}
      // Every resolve* is a c-ares query mbun does not implement; UV_ENOSYS is
      // the honest answer (a 0 here would report success with no result).
      queryAny() { return -38; } queryA() { return -38; } queryAaaa() { return -38; }
      queryCaa() { return -38; } queryCname() { return -38; } queryMx() { return -38; }
      queryNs() { return -38; } queryTxt() { return -38; } querySrv() { return -38; }
      queryPtr() { return -38; } queryNaptr() { return -38; } querySoa() { return -38; }
      getHostByAddr() { return -38; }
    },
    isIP: (s) => { try { return mod("net").isIP(s) | 0; } catch (e) { return 0; } },
    isIPv4: (s) => { try { return !!mod("net").isIPv4(s); } catch (e) { return false; } },
    isIPv6: (s) => { try { return !!mod("net").isIPv6(s); } catch (e) { return false; } },
    strerror: (code) => "Unknown system error " + code,
  });

  Object.defineProperty(G, "__mbunInternalBinding", {
    value: internalBinding, writable: true, configurable: true, enumerable: false,
  });
  Object.defineProperty(G, "__mbunInternalBindingDefine", {
    value: (name, make) => { factories[name] = make; },
    writable: true, configurable: true, enumerable: false,
  });
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
