// node:module payload partition. Replaces the earlier bootstrap `module` stub
// with a Node-shaped implementation: `require("module")` returns the `Module`
// constructor itself (with all statics), matching Node/bun where the module's
// default export IS the CommonJS Module function. Blueprint: bun-ref
// src/jsc/modules/NodeModuleModule.cpp (the LUT table + accessors),
// src/jsc/bindings/isBuiltinModule.cpp (the isBuiltin sorted list), and Node's
// lib/internal/modules/cjs/loader.js (_nodeModulePaths / _resolveLookupPaths).
//
// Covers: createRequire, builtinModules, isBuiltin, Module (constructor +
// _resolveFilename/_resolveLookupPaths/_nodeModulePaths/_cache/_extensions/
// wrap/wrapper/prototype._compile), register, syncBuiltinESMExports, SourceMap
// (real VLQ decode + findEntry/findOrigin), findSourceMap, constants,
// getCompileCacheDir/enableCompileCache, globalPaths. require.resolve is taught
// options.paths + builtin passthrough by wrapping __mbun_make_require.
//
// DEFERRED (need native CJS-loader integration, which is in engine.inc): the
// loader honouring overridden Module._resolveFilename / Module.prototype.require
// / require.extensions custom loaders, require.cache interception for builtins,
// Module.runMain / CLI --require, findSourceMap returning live maps for loaded
// modules, and enableCompileCache actually caching bytecode.
export module mbun.jsc.js_builtins:node_module;

import std;

export namespace mbun::jsc::builtins::detail {

// NOTE: appended AFTER the master builtins IIFE has closed (see image_closure),
// so this is a self-contained IIFE that re-binds G = globalThis and overrides
// the module registry's earlier `module` stub with the real implementation.
inline constexpr std::string_view kNodeModuleJS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  if (!M) return;

  const getPath = () => M["node:path"] || M["path"];
  const getUrl = () => M["node:url"] || M["url"];

  // bun overstuffs builtinModules with its own + thirdparty names so bundler
  // `external` lists exclude them; this is the exact 76-entry list bun exposes.
  const BUILTIN_MODULES = [
    "_http_agent", "_http_client", "_http_common", "_http_incoming",
    "_http_outgoing", "_http_server", "_stream_duplex", "_stream_passthrough",
    "_stream_readable", "_stream_transform", "_stream_wrap", "_stream_writable",
    "_tls_common", "_tls_wrap", "assert", "assert/strict", "async_hooks",
    "buffer", "bun:ffi", "bun:jsc", "bun:sqlite", "bun:test", "bun:wrap", "bun",
    "child_process", "cluster", "console", "constants", "crypto", "dgram",
    "diagnostics_channel", "dns", "dns/promises", "domain", "events", "fs",
    "fs/promises", "http", "http2", "https", "inspector", "inspector/promises",
    "module", "net", "os", "path", "path/posix", "path/win32", "perf_hooks",
    "process", "punycode", "querystring", "readline", "readline/promises",
    "repl", "stream", "stream/consumers", "stream/promises", "stream/web",
    "string_decoder", "sys", "timers", "timers/promises", "tls", "trace_events",
    "tty", "undici", "url", "util", "util/types", "v8", "vm", "wasi",
    "worker_threads", "ws", "zlib",
  ];

  // isBuiltin() uses a distinct list: it includes node-only-prefix names as
  // "node:xxx" (so isBuiltin("node:test") is true but isBuiltin("test") false).
  const ISBUILTIN_SET = new Set([
    "fs", "os", "v8", "vm", "ws", "bun", "dns", "net", "sys", "tls", "tty",
    "url", "http", "path", "repl", "util", "wasi", "zlib", "dgram", "http2",
    "https", "assert", "buffer", "crypto", "domain", "events", "module",
    "stream", "timers", "undici", "bun:ffi", "bun:jsc", "cluster", "console",
    "process", "bun:wrap", "punycode", "bun:test", "bun:main", "readline",
    "_tls_wrap", "constants", "inspector", "node:test", "bun:sqlite",
    "path/posix", "path/win32", "perf_hooks", "stream/web", "util/types",
    "_http_agent", "_tls_common", "async_hooks", "fs/promises", "querystring",
    "_http_client", "_http_common", "_http_server", "_stream_wrap",
    "dns/promises", "trace_events", "assert/strict", "child_process",
    "_http_incoming", "_http_outgoing", "_stream_duplex", "string_decoder",
    "worker_threads", "stream/promises", "timers/promises", "_stream_readable",
    "_stream_writable", "stream/consumers", "_stream_transform",
    "readline/promises", "inspector/promises", "_stream_passthrough",
    "diagnostics_channel",
  ]);

  function isBuiltin(name) {
    name = String(name);
    if (ISBUILTIN_SET.has(name)) return true;
    if (name.startsWith("node:")) return ISBUILTIN_SET.has(name.slice(5));
    return false;
  }

  // ---- Node ERR_INVALID_ARG_TYPE message (word-for-word for the cases tested).
  function invalidArgType(name, expected, actual) {
    let received;
    if (actual === undefined) received = "undefined";
    else if (actual === null) received = "null";
    else {
      const t = typeof actual;
      if (t === "string") {
        const s = actual.length > 25 ? actual.slice(0, 25) + "..." : actual;
        received = "type string ('" + s + "')";
      } else if (t === "number" || t === "bigint" || t === "boolean") {
        received = "type " + t + " (" + actual + ")";
      } else if (t === "function") {
        received = "function " + (actual.name || "(anonymous)");
      } else {
        received = "an instance of " + (actual && actual.constructor ? actual.constructor.name : "Object");
      }
    }
    const e = new TypeError('The "' + name + '" argument must be of type ' + expected + ". Received " + received);
    e.code = "ERR_INVALID_ARG_TYPE";
    return e;
  }

  // ---- SourceMap (node:module) — real base64-VLQ mappings decode.
  const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const B64MAP = (() => { const m = {}; for (let i = 0; i < B64.length; i++) m[B64[i]] = i; return m; })();

  function decodeVLQ(str, state) {
    let result = 0, shift = 0, cont, digit;
    do {
      if (state.i >= str.length) { const e = new SyntaxError("Unexpected end of mappings"); throw e; }
      const c = str[state.i++];
      digit = B64MAP[c];
      if (digit === undefined) { const e = new SyntaxError("Invalid base64 VLQ digit"); throw e; }
      cont = digit & 32;
      digit &= 31;
      result += digit << shift;
      shift += 5;
    } while (cont);
    const negate = result & 1;
    result >>>= 1;
    return negate ? -result : result;
  }

  function decodeMappings(mappings, sources, names) {
    const entries = [];
    let genLine = 0, srcIdx = 0, origLine = 0, origCol = 0, nameIdx = 0;
    const lines = mappings.split(";");
    for (let li = 0; li < lines.length; li++, genLine++) {
      const line = lines[li];
      if (line.length === 0) continue;
      let genCol = 0;
      const segs = line.split(",");
      for (let si = 0; si < segs.length; si++) {
        const seg = segs[si];
        if (seg.length === 0) continue;
        const state = { i: 0 };
        genCol += decodeVLQ(seg, state);
        const entry = {
          generatedLine: genLine,
          generatedColumn: genCol,
          originalSource: undefined,
          originalLine: undefined,
          originalColumn: undefined,
          name: undefined,
        };
        if (state.i < seg.length) {
          srcIdx += decodeVLQ(seg, state);
          origLine += decodeVLQ(seg, state);
          origCol += decodeVLQ(seg, state);
          entry.originalSource = sources ? sources[srcIdx] : undefined;
          entry.originalLine = origLine;
          entry.originalColumn = origCol;
          if (state.i < seg.length) {
            nameIdx += decodeVLQ(seg, state);
            entry.name = names ? names[nameIdx] : undefined;
          }
        }
        entries.push(entry);
      }
    }
    return entries;
  }

  class SourceMap {
    constructor(payload, options) {
      if (typeof payload !== "object" || payload === null) {
        throw invalidArgType("payload", "object", payload);
      }
      this.__payload = payload;
      this.__lineLengths = options && options.lineLengths !== undefined ? options.lineLengths : undefined;
      this.__entries = decodeMappings(
        String(payload.mappings || ""),
        payload.sources,
        payload.names,
      );
    }
    get payload() { return this.__payload; }
    get lineLengths() { return this.__lineLengths; }

    findEntry(lineOffset, columnOffset) {
      let found;
      for (let k = 0; k < this.__entries.length; k++) {
        const e = this.__entries[k];
        if (e.generatedLine !== lineOffset) continue;
        if (e.generatedColumn <= columnOffset) {
          if (!found || e.generatedColumn > found.generatedColumn) found = e;
        }
      }
      if (!found) return {};
      return {
        generatedLine: found.generatedLine,
        generatedColumn: found.generatedColumn,
        originalSource: found.originalSource,
        originalLine: found.originalLine,
        originalColumn: found.originalColumn,
        name: found.name,
      };
    }

    findOrigin(lineNumber, columnNumber) {
      const e = this.findEntry(lineNumber, columnNumber);
      if (e.originalSource === undefined) return {};
      return {
        name: e.name,
        fileName: e.originalSource,
        line: e.originalLine,
        column: e.originalColumn,
      };
    }
  }

  // ---- Module constructor (must be `new`-able; require("module") returns it).
  function Module(id, parent) {
    this.id = id || "";
    this.path = typeof id === "string" ? getPath().dirname(id) : ".";
    this.exports = {};
    this.parent = parent;
    this.filename = null;
    this.loaded = false;
    this.children = [];
    this.paths = [];
  }

  // Prototype methods are concise (non-constructable, dynamic `this`).
  Object.assign(Module.prototype, {
    _compile(content, filename) {
      const path = getPath();
      const dirname = path.dirname(filename);
      const wrapped = Module.wrap(content);
      const compiled = (0, eval)(wrapped);
      const req = this.require;
      compiled.call(this.exports, this.exports, req, this, filename, dirname);
      return undefined;
    },
    require(id) {
      const dir = this.path || (this.filename ? getPath().dirname(this.filename) : G.process.cwd());
      const req = G.__mbun_make_require(dir);
      return req(id);
    },
    load(filename) {
      this.filename = filename;
      this.paths = Module._nodeModulePaths(getPath().dirname(filename));
      this.loaded = true;
    },
  });

  // ---- createRequire(pathOrFileURL) → a require function anchored at its dir.
  function createRequire(filename) {
    if (filename === undefined || filename === null) {
      const e = new TypeError("createRequire() requires at least one argument");
      e.code = "ERR_MISSING_ARGS";
      throw e;
    }
    const path = getPath();
    let val = filename !== null && typeof filename === "object" && "href" in filename
      ? String(filename.href)
      : String(filename);
    if (!path.isAbsolute(val)) {
      // file URL string / object → filesystem path.
      val = getUrl().fileURLToPath(val);
    }
    // A trailing slash means the argument names a directory; joining a dummy
    // basename makes dirname(val) resolve back to that directory (Node parity).
    if (val.endsWith("/") || val.endsWith("\\")) val = path.join(val, "noop.js");
    const baseDir = path.dirname(val);
    return G.__mbun_make_require(baseDir);
  }

  // ---- _nodeModulePaths(from) — Node's exact walk (skips node_modules segs).
  // Node scans the path BACKWARD, so it matches each segment against the char
  // codes of "node_modules" in REVERSE order (lib/internal/modules/cjs/loader.js
  // uses the pre-reversed nmChars array). Comparing against the forward string
  // never matches, which leaves the redundant .../node_modules/node_modules
  // entries in the list — hence the reversed lookup table here.
  const NM = "node_modules";
  const NM_LEN = NM.length;
  const NM_CHARS_REV = (() => {
    const a = [];
    for (let k = NM_LEN - 1; k >= 0; --k) a.push(NM.charCodeAt(k));
    return a;
  })();
  function nodeModulePaths(from) {
    if (from === undefined) {
      throw invalidArgType("path", "string", from);
    }
    const path = getPath();
    from = path.resolve(String(from));
    const sep = 47; // '/'
    if (from === "/") return ["/" + NM];
    const paths = [];
    let p = 0, last = from.length;
    for (let i = from.length - 1; i >= 0; --i) {
      const code = from.charCodeAt(i);
      if (code === sep) {
        if (p !== NM_LEN) paths.push(from.slice(0, last) + "/" + NM);
        last = i;
        p = 0;
      } else if (p !== -1) {
        if (NM_CHARS_REV[p] === code) ++p;
        else p = -1;
      }
    }
    paths.push("/" + NM);
    return paths;
  }

  // ---- _resolveLookupPaths(request, parent)
  function resolveLookupPaths(request, parent) {
    request = String(request);
    if (isBuiltin(request)) return null;
    const path = getPath();
    let parentPaths = null, parentFilename = null;
    if (parent && typeof parent === "object") {
      if (Array.isArray(parent.paths)) parentPaths = parent.paths;
      if (typeof parent.filename === "string") parentFilename = parent.filename;
    }
    const c0 = request.charCodeAt(0), c1 = request.charCodeAt(1);
    // Not a relative specifier → node_modules lookup (parent.paths or []).
    if (c0 !== 46 || (request.length > 1 && c1 !== 46 && c1 !== 47)) {
      if (parentPaths) return parentPaths.slice();
      return [];
    }
    if (parentFilename) return [path.dirname(parentFilename)];
    return ["."];
  }

  // ---- _resolveFilename(request, parent, isMain, options)
  function resolveFilename(request, parent, isMain, options) {
    if (request === undefined || request === null) {
      throw new TypeError("Module._resolveFilename expects a string");
    }
    request = String(request);
    // Validate options.paths TYPE before the builtin short-circuit, per node
    // jsFunctionResolveFileName (NodeModuleModule.cpp): a non-array `paths`
    // must throw even when the request is a builtin.
    if (options && options.paths !== undefined && options.paths !== null && !Array.isArray(options.paths)) {
      throw invalidArgType("options.paths", "array", options.paths);
    }
    if (isBuiltin(request)) return request;
    const path = getPath();
    let fromDir;
    if (typeof parent === "string") fromDir = path.dirname(parent);
    else if (parent && typeof parent === "object") {
      const f = parent.filename || parent.id;
      fromDir = typeof f === "string" && f.length ? path.dirname(f) : G.process.cwd();
    } else fromDir = G.process.cwd();

    if (options && options.paths !== undefined && options.paths !== null) {
      if (!Array.isArray(options.paths)) {
        throw invalidArgType("options.paths", "array", options.paths);
      }
      let lastErr;
      for (let k = 0; k < options.paths.length; k++) {
        try { return G.__mbun_resolve_native(request, String(options.paths[k])); }
        catch (e) { lastErr = e; }
      }
      throw lastErr || new Error("Cannot find module '" + request + "'");
    }
    return G.__mbun_resolve_native(request, fromDir);
  }

  // Non-constructable statics (arrow fns): `new Module._resolveFilename()` etc.
  // must throw, per the "native module functions are not constructors" test.
  Module.Module = Module;
  Module.createRequire = (f) => createRequire(f);
  Module.builtinModules = BUILTIN_MODULES;
  Module.isBuiltin = (m) => isBuiltin(m);
  Module._resolveFilename = (request, parent, isMain, options) => resolveFilename(request, parent, isMain, options);
  Module._resolveLookupPaths = (request, parent) => resolveLookupPaths(request, parent);
  Module._nodeModulePaths = (from) => nodeModulePaths(from);
  Module._cache = {};
  Module._pathCache = {};

  // _stat(path): node's internalModuleStat — 1 for a directory, 0 for a file,
  // and a negative errno for anything else (missing path, etc.).
  Module._stat = (filename) => {
    const fs = M["node:fs"] || M["fs"];
    try {
      const s = fs.statSync(String(filename));
      return s.isDirectory() ? 1 : 0;
    } catch (e) {
      return -2; // -ENOENT
    }
  };

  // require.extensions default loaders (stubs; the loader is native — custom
  // loaders are DEFERRED, but the shape/identity are correct).
  const noopLoader = (module, filename) => {};
  Module._extensions = {
    ".js": noopLoader, ".json": noopLoader, ".node": noopLoader,
    ".cts": noopLoader, ".ts": noopLoader, ".mjs": noopLoader, ".mts": noopLoader,
  };
  Module.globalPaths = [];

  Module.wrapper = [
    "(function (exports, require, module, __filename, __dirname) { ",
    "\n});",
  ];
  Module.wrap = (script) => Module.wrapper[0] + script + Module.wrapper[1];

  Module.SourceMap = SourceMap;
  Module.findSourceMap = (path) => undefined;
  Module.syncBuiltinESMExports = () => {};

  Module.constants = Object.freeze({
    compileCacheStatus: Object.freeze({
      FAILED: 0, ENABLED: 1, ALREADY_ENABLED: 2, DISABLED: 3,
    }),
  });

  // The native module loader owns serialized bytecode.  Keep the public
  // compile-cache configuration here, however, so CommonJS and ESM callers
  // agree on one directory and on Node's enable/disable result contract.
  // Loader-side cache production/consumption is intentionally separate from
  // this API layer (see the DEFERRED note at the top of this payload).
  let compileCacheDirectory;
  const compileCacheStatus = Module.constants.compileCacheStatus;

  function compileCacheFs() {
    return M["node:fs"] || M["fs"];
  }

  function compileCacheDefaultDirectory() {
    const env = G.process && G.process.env;
    if (env && env.NODE_COMPILE_CACHE) return env.NODE_COMPILE_CACHE;
    const os = M["node:os"] || M["os"];
    const path = getPath();
    const tmp = os && typeof os.tmpdir === "function" ? os.tmpdir() : "/tmp";
    return path && typeof path.join === "function" ? path.join(tmp, "node-compile-cache") : tmp + "/node-compile-cache";
  }

  function invalidCompileCacheOptions(options) {
    return invalidArgType("options", "string or Object or undefined", options);
  }

  function enableCompileCache(options) {
    const env = G.process && G.process.env;
    if (env && env.NODE_DISABLE_COMPILE_CACHE === "1") {
      return { status: compileCacheStatus.DISABLED };
    }
    if (compileCacheDirectory !== undefined) {
      return { status: compileCacheStatus.ALREADY_ENABLED, directory: compileCacheDirectory };
    }

    let directory;
    let portable;
    if (options === undefined || typeof options === "string") {
      directory = options;
    } else if (options !== null && typeof options === "object") {
      ({ directory, portable } = options);
      if (portable !== undefined && typeof portable !== "boolean") {
        throw invalidArgType("options.portable", "boolean", portable);
      }
    } else {
      throw invalidCompileCacheOptions(options);
    }
    if (directory === undefined) directory = compileCacheDefaultDirectory();
    if (typeof directory !== "string") {
      throw invalidArgType("options.directory", "string", directory);
    }

    try {
      const fs = compileCacheFs();
      if (!fs || typeof fs.mkdirSync !== "function") {
        return { status: compileCacheStatus.FAILED, message: "The file system module is unavailable" };
      }
      fs.mkdirSync(directory, { recursive: true });
      compileCacheDirectory = directory;
      return { status: compileCacheStatus.ENABLED, directory };
    } catch (error) {
      return {
        status: compileCacheStatus.FAILED,
        message: error && error.message ? String(error.message) : String(error),
      };
    }
  }

  Module.getCompileCacheDir = () => compileCacheDirectory;
  Module.enableCompileCache = enableCompileCache;
  Module.flushCompileCache = () => {};

  // NODE_COMPILE_CACHE enables the cache during process initialization, before
  // user preloads can call getCompileCacheDir().
  if (G.process && G.process.env && G.process.env.NODE_COMPILE_CACHE) {
    enableCompileCache(G.process.env.NODE_COMPILE_CACHE);
  }

  // ESM loader hooks — accepted but a no-op (native loader integration DEFERRED).
  Module.register = (specifier, parentURL, options) => {};

  Module.runMain = (main) => {
    // DEFERRED: real CLI entrypoint execution happens natively.
    if (main !== undefined) G.__mbun_make_require(G.process.cwd())(main);
  };
  Module._load = (request, parent, isMain) => {
    const dir = parent && parent.path ? parent.path : G.process.cwd();
    return G.__mbun_make_require(dir)(request);
  };
  Module._initPaths = () => {};
  Module._preloadModules = () => {};

  M["module"] = Module;
  M["node:module"] = Module;

  // nationalized "abort-controller" (mysticatea/abort-controller): ref bun
  // src/jsc/modules/AbortControllerModuleModule.h — putDirect AbortSignal /
  // AbortController / default onto the global AbortController constructor and
  // return it as the module (default export).
  if (typeof G.AbortController === "function" && typeof G.AbortSignal === "function" && !M["abort-controller"]) {
    const AC = G.AbortController, AS = G.AbortSignal;
    try {
      if (AC.AbortSignal !== AS) AC.AbortSignal = AS;
      if (AC.AbortController !== AC) AC.AbortController = AC;
      if (AC.default !== AC) AC.default = AC;
    } catch (e) {}
    M["abort-controller"] = AC;
  }

  // Teach every module-scoped require the pieces the native make_require lacks:
  // options.paths + builtin passthrough on require.resolve, and identity of
  // require.extensions with Module._extensions. Additive wrap — the original
  // require/resolve behaviour is preserved for the common path.
  const origMakeRequire = G.__mbun_make_require;
  if (origMakeRequire && !origMakeRequire.__mbunModulePatched) {
    const patched = function (dir) {
      const req = origMakeRequire(dir);
      const origResolve = req.resolve;
      req.resolve = function (s, options) {
        s = String(s);
        if (isBuiltin(s)) return s;
        if (options && Array.isArray(options.paths)) {
          let lastErr;
          for (let k = 0; k < options.paths.length; k++) {
            try { return G.__mbun_resolve_native(s, String(options.paths[k])); }
            catch (e) { lastErr = e; }
          }
          throw lastErr || new Error("Cannot find module '" + s + "'");
        }
        return origResolve ? origResolve.call(this, s) : G.__mbun_resolve_native(s, dir);
      };
      req.extensions = Module._extensions;
      return req;
    };
    patched.__mbunModulePatched = true;
    G.__mbun_make_require = patched;
  }
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
