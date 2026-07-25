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

  // ----------------------------------------------------------------- icu ----
  factories["icu"] = () => ({
    icuErrName: (n) => "U_ERROR_" + n,
    transcode: (...args) => mod("buffer").transcode(...args),
    getStringWidth: (s) => String(s).length,
    toASCII: (s) => s,
    toUnicode: (s) => s,
    hasConverter: () => false,
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
