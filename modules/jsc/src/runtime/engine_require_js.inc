// ── engine slice: the CommonJS require/module-environment JS prelude ────────
// Extracted from engine.inc verbatim (its 2000-line budget, enforced by
// test_runtime_structure). One raw-string constant; engine.inc evals it during
// install_bindings_ right after the native require hooks are registered.
constexpr std::string_view kRequireMachineryJs = R"JS(
// Explicit resource management symbols — the prebuilt JSC predates them, so
// polyfill as real distinct symbols (else `obj[Symbol.dispose]` keys under
// `undefined` and disposal helpers fail with "Object is not disposable").
if (typeof Symbol.dispose === "undefined") { Object.defineProperty(Symbol, "dispose", { value: Symbol("Symbol.dispose") }); }
if (typeof Symbol.asyncDispose === "undefined") { Object.defineProperty(Symbol, "asyncDispose", { value: Symbol("Symbol.asyncDispose") }); }
// `o` is the import-attributes bag the ESM → CJS lowering passes through:
// `import x from "m" with { type: "text" }` becomes `require("m", { type: "text" })`
// (js_parser/cjs_runtime.cppm require_call_), and the type selects the loader. It
// is normalized to a string here rather than in the trampoline because
// `arg_string` stringifies an absent argument to the literal "undefined", which
// would then be probed as a loader name. A plain user `require("m")` passes no
// second argument and is unaffected.
// Bridge for `require("blob:…")` / `import("blob:…")`: the object-URL registry is
// a JS-side Map (URL.createObjectURL in the web bootstrap), so the native module
// loader asks for the blob through this hook. Returns { type, source } — the
// blob's MIME type (which selects the loader, bun Blob.rs:2178 get_loader) and its
// bytes decoded as UTF-8 — or null when the id is not registered (revoked or
// never created), which makes require() fall through to the normal resolver and
// report MODULE_NOT_FOUND.
globalThis.__mbun_blob_module_source = function (spec) {
  const resolve = globalThis.__bunResolveObjectURL;
  if (typeof resolve !== "function") return null;
  const blob = resolve(spec);
  if (blob === undefined || blob === null) return null;
  const bytes = blob._u8;
  return { type: typeof blob.type === "string" ? blob.type : "",
           source: bytes ? new TextDecoder().decode(bytes) : "" };
};
globalThis.__mbun_make_require = function (dir) {
  const invalidRequireId = function (value) {
    if (typeof value !== "string") {
      let received;
      if (value === undefined) received = "undefined";
      else if (value === null) received = "null";
      else if (typeof value === "object") received = "an instance of " +
        (value && value.constructor ? value.constructor.name : "Object");
      else received = "type " + typeof value + " (" + String(value) + ")";
      const error = new TypeError('The "id" argument must be of type string. Received ' + received);
      error.code = "ERR_INVALID_ARG_TYPE";
      return error;
    }
    if (value.length === 0) {
      const error = new TypeError("The argument 'id' must be a non-empty string. Received ''");
      error.code = "ERR_INVALID_ARG_VALUE";
      return error;
    }
    return null;
  };
  // `k` (arg 3): truthy when this call is a lowered ESM import statement or a
  // dynamic import (ImportKind::Stmt/Dynamic) rather than a literal require() —
  // it only selects the Node error code on failure (ERR_MODULE_NOT_FOUND vs
  // MODULE_NOT_FOUND), not the resolution condition.
  const req = function (s, o, k) {
    const invalid = invalidRequireId(s);
    if (invalid) throw invalid;
    // The C++ bridge receives UTF-8 through a nul-terminated string. Reject
    // embedded NULs here so the full request stays observable in Node's error.
    if (s.indexOf("\0") !== -1) {
      const error = new Error("Cannot find module '" + s + "'");
      error.code = "MODULE_NOT_FOUND";
      throw error;
    }
    const load = function () {
      // Node consults require.cache before entering its CJS loader.  Keep the
      // native cache authoritative for ordinary modules, while honouring a
      // caller-installed cache record (including builtin keys) before crossing
      // the native boundary.
      const cache = req.cache;
      let cacheKey = s;
      if (s.indexOf("node:") !== 0) {
        try { cacheKey = req.resolve(s); } catch (_) {}
      }
      const cached = cache[cacheKey] || cache[s];
      if (cached && typeof cached === "object" && "exports" in cached) {
        return cached.exports;
      }
      return globalThis.__mbun_require_native(s, dir, false,
        (o && typeof o.type === "string") ? o.type : "", k ? 1 : 0);
    };
    const modules = globalThis.__mbunNativeModules;
    const dc = modules && (modules["diagnostics_channel"] || modules["node:diagnostics_channel"]);
    const trace = dc && typeof dc.tracingChannel === "function" ? dc.tracingChannel("module.require") : null;
    if (!trace || !trace.hasSubscribers) return load();
    return trace.traceSync(load, { parentFilename: req.__mbun_parent_filename, id: s });
  };
  req.resolve = function (s) { return globalThis.__mbun_resolve_native(String(s), dir); };
  // require.cache is a live view over the native CommonJS module cache: a plain
  // object can't reach the C++ cache, so `delete require.cache[key]` (a common
  // "force re-evaluation" idiom, and what test/js/bun/resolve/import-query uses to
  // re-import the same file) would be a no-op. The Proxy forwards property deletes
  // to the native evictor (key = resolved path + any ?query suffix). Reads/writes
  // stay on a backing object so ordinary require.cache access still works.
  req.cache = globalThis.__mbun_require_cache || (globalThis.__mbun_require_cache = new Proxy({}, {
    deleteProperty: function (target, key) {
      globalThis.__mbun_evict_module_cache(String(key));
      delete target[key];
      return true;
    },
  }));
  return req;
};
// The require handed to a module whose source was ESM: every require() in it is a
// lowered `import`, so resolution must activate the "import" condition instead of
// "require" (an ESM-only package exposes only {"types":…,"import":…} and is
// otherwise unresolvable — and a package with both branches would silently get the
// CJS build). See require_impl's `esm` parameter.
//
// This is a *separate* factory rather than a second parameter on
// __mbun_make_require because node:module (builtins/node_module.cppm:424-447)
// re-wraps __mbun_make_require with a `function (dir)` that does not forward extra
// arguments — an added parameter would be silently dropped the moment node:module
// is loaded. Delegating through the live global instead keeps that patch's
// require.resolve(options.paths) / require.extensions behavior on ESM modules too.
globalThis.__mbun_make_require_esm = function (dir) {
  const base = globalThis.__mbun_make_require(dir);
  // `k` (arg 3): the lowered-import / dynamic-import marker (see __mbun_make_require).
  // A literal require() in an ESM module passes no `k` → ImportKind::Require →
  // MODULE_NOT_FOUND, while a lowered `import` statement passes 1 → ERR_MODULE_NOT_FOUND.
  const req = function (s, o, k) {
    return globalThis.__mbun_require_native(s, dir, true,
      (o && typeof o.type === "string") ? o.type : "", k ? 1 : 0);
  };
  // Carry over everything the base (and any node:module patch) attached —
  // resolve/cache/extensions — so only the *load* path changes. require.resolve
  // keeps the "require" condition: it is a CJS API, and import.meta.resolve (the
  // ESM-facing one, built in __mbun_make_import_meta) is the esm-aware resolver.
  for (const k of Object.keys(base)) { req[k] = base[k]; }
  return req;
};
// Dynamic import(): the transpiler lowers `import(spec)` (cjs mode) to
// `__mbun_dyn_import(require, spec)` so it resolves + transpiles through the
// module-scoped require (the script-mode JSC loader can't fetch/transpile TS).
// Returns a Promise of an ESM namespace (exports + a `default`), so consumers
// that read `.default` (node builtins/stubs, interop) behave like real import().
globalThis.__mbun_esm_namespace = function (m, forceCommonJSDefault) {
  if (m === null || (typeof m !== "object" && typeof m !== "function")) return { default: m };
  // Own enumerable props become named exports (a transpiled ESM module also
  // copies its own `.default` here). A pure CJS module has no `default`, so the
  // whole exports object is the default binding (Node CJS→ESM interop).
  var ns = Object.assign({}, m);
  if (forceCommonJSDefault || !("default" in ns)) ns.default = m;
  return ns;
};
globalThis.__mbun_dyn_import = function (req, spec, options) {
  const load = function () {
  // A `data:` URL carries its own source — there is nothing to fetch, so bun
  // decodes and compiles it eagerly and a malformed payload (bad base64) throws
  // from the import() call site instead of rejecting later
  // (js/node/string-module.test.js asserts the synchronous throw).
    if (typeof spec === "string" && spec.startsWith("data:")) {
      return Promise.resolve(globalThis.__mbun_esm_namespace(req(spec, undefined, 1)));
    }
    return Promise.resolve().then(function () {
    // `import(spec, { with: { type } })` (and the legacy `{ assert: { type } }`)
    // — thread the type import attribute to require() as its 2nd arg so it
    // overrides the extension-derived loader, exactly like a static
    // `import ... with { type }`. bun ImportKind::Dynamic honors both keys.
    var attr;
    if (options != null && typeof options === "object") {
      var w = options.with != null ? options.with : options.assert;
      if (w != null && typeof w === "object" && typeof w.type === "string") attr = { type: w.type };
    }
    // Pass the import marker (3rd arg): a dynamic import() is ImportKind::Dynamic,
    // so a resolution failure reports ERR_MODULE_NOT_FOUND (bun ResolveMessage.rs).
      var requested = String(spec);
      // node:process is a CommonJS builtin. Its namespace default is the
      // process object even if user code assigns process.default.
      return globalThis.__mbun_esm_namespace(
        req(requested, attr, 1), requested === "process" || requested === "node:process");
    });
  };
  const modules = globalThis.__mbunNativeModules;
  const dc = modules && (modules["diagnostics_channel"] || modules["node:diagnostics_channel"]);
  const trace = dc && typeof dc.tracingChannel === "function" ? dc.tracingChannel("module.import") : null;
  if (!trace || !trace.hasSubscribers) return load();
  const parentFilename = req && req.__mbun_parent_filename;
  const parentURL = parentFilename ? "file://" + parentFilename : undefined;
  return trace.tracePromise(load, { parentURL: parentURL, url: String(spec) });
};
// Per-module import.meta: the CJS wrapper injects a local __mbunImportMeta built
// from the module's own file/dir, so `import.meta.url`/`.dir` are correct inside
// required modules (not the entry file's, which the global would give).
// import.meta only exists in an ESM module, so its resolve/require use the
// "import" condition (esm=true) — matching bun, where an `import` resolves with
// ESMConditions.import (bundler/options.rs:788-816).
globalThis.__mbun_make_import_meta = function (file, dir, urlQuery) {
  // `urlQuery` is the `?...` suffix of the import specifier (empty when none):
  // it rides on import.meta.url only — bun keeps the query in the module URL but
  // .path/.dir/.filename stay the bare on-disk path.
  return { url: "file://" + file + (urlQuery || ""), dir: dir, dirname: dir, path: file, filename: file,
           file: file, main: false, env: globalThis.process.env,
           // bun ImportMetaObject.cpp:422 — resolve() returns a file:// URL
           // (builtins pass through); resolveSync stays a bare path (:195).
           resolve: function (s) { s = String(s);
             // bun ImportMetaObject.cpp:486-500 — relative/absolute/file:// specifiers
             // take a fast path that is a pure WHATWG URL join against the module's
             // own file URL: no module resolution, no existence check, never throws.
             if (s.charCodeAt(0) === 47 /* / */ || s.startsWith("./") || s.startsWith("../") || s.startsWith("file://")) {
               return new URL(s, "file://" + file).href;
             }
             var r = globalThis.__mbun_resolve_native(s, dir, true);
             return (typeof r === "string" && r.charCodeAt(0) === 47 && globalThis.Bun && globalThis.Bun.pathToFileURL) ? globalThis.Bun.pathToFileURL(r).href : r; },
           resolveSync: function (s) { return globalThis.__mbun_resolve_native(String(s), dir, true); },
           require: globalThis.__mbun_make_require(dir) };
};
{
  const __mbun_qm = typeof globalThis.queueMicrotask === "function"
    ? globalThis.queueMicrotask.bind(globalThis)
    : function (fn) { Promise.resolve().then(fn); };
  globalThis.queueMicrotask = function queueMicrotask(callback) {
    if (typeof callback !== "function")
      throw new TypeError("queueMicrotask requires a function argument, but received a value of type " + typeof callback);
    return __mbun_qm(callback);
  };
}
// Explicit resource management (using / await using) disposal helpers — the
// transpiler lowers a `using x = e` to `const x = __mbun_using(env, e, false)`
// inside a try/finally that calls __mbun_dispose(env) (reverse order, honest
// error propagation), because the prebuilt JSC lacks native `using` support.
globalThis.__mbun_using = function (env, value, isAsync) {
  if (value !== null && value !== undefined) {
    var d = isAsync ? (value[Symbol.asyncDispose] || value[Symbol.dispose]) : value[Symbol.dispose];
    if (typeof d !== "function") throw new TypeError("@@dispose must not be undefined or null");
    env.stack.push({ v: value, d: d, async: isAsync });
  } else {
    env.stack.push({ v: value, d: null, async: isAsync });
  }
  return value;
};
// Sync `using`: disposal must run and re-throw synchronously — an async helper
// would turn a thrown disposer/initializer error into an unobserved rejected
// promise and swallow it (the finally has no `await`).
globalThis.__mbun_dispose = function (env) {
  var err = env.error, has = env.hasError;
  for (var i = env.stack.length - 1; i >= 0; i--) {
    var r = env.stack[i];
    try { if (r.d) { r.d.call(r.v); } }
    catch (e) { if (!has) { err = e; has = true; } }
  }
  if (has) throw err;
};
globalThis.__mbun_disposeAsync = async function (env) {
  var err = env.error, has = env.hasError;
  for (var i = env.stack.length - 1; i >= 0; i--) {
    var r = env.stack[i];
    try { if (r.d) { var p = r.d.call(r.v); if (r.async) await p; } }
    catch (e) { if (!has) { err = e; has = true; } }
  }
  if (has) throw err;
};
// ── ES stage-3 (TC39) decorators runtime ────────────────────────────────────
// The transpiler lowers a decorated class to calls against these helpers (see
// emit_decorated_class_ in js_parser.cppm). Per lowering site N:
//   __mbun_dcF(N,name,[classDecs])._h(heritage)._m(k,flags,name,[decs])._k(j,key)
// then inside the class body a first static block runs __mbun_dcP (private
// impl/access registration) + __mbun_dcA (application, spec order), field
// initializers run through __mbun_dcFld, instance extra initializers through a
// synthetic first field calling __mbun_dcII, and the class binding / expression
// value is finished by __mbun_dcFin / ._c.
(function () {
  if (!Symbol.metadata) { try { Symbol.metadata = Symbol.for("Symbol.metadata"); } catch (e) {} }
  const SLOT = new Map();  // site id -> context (valid during class creation)
  const SYMS = new Map();  // site id -> capture symbol (class -> context)
  const symOf = (n) => { let s = SYMS.get(n); if (!s) { s = Symbol("mbun.dec." + n); SYMS.set(n, s); } return s; };
  // Resolve the context from `this` at run time: the class itself (static
  // side, inherits along the constructor chain) or an instance (via its
  // constructor). Per-site unique symbols keep subclass sites separate.
  const ctxOf = (n, o) => {
    const s = symOf(n);
    if (o != null && o[s] !== undefined) return o[s];
    return o.constructor[s];
  };
  const toKey = (k) => typeof k === "symbol" ? k : "" + k;
  const KINDS = ["method", "getter", "setter", "field", "accessor"];
  const setName = (fn, name) => { try { Object.defineProperty(fn, "name", { value: name, configurable: true }); } catch (e) {} };
  // Per-decorator-invocation addInitializer: validates the argument and goes
  // dead (TypeError) once the decorator returns, per spec.
  const mkAdd = (push) => {
    let open = true;
    const f = (g) => {
      if (!open) throw new TypeError("addInitializer called after decoration finished");
      if (typeof g !== "function") throw new TypeError("addInitializer expects a function");
      push(g);
    };
    f._close = () => { open = false; };
    return f;
  };
  globalThis.__mbun_dcF = function (n, name, decs) {
    const c = { name: name, classDecs: decs, els: [], keys: [], heritage: void 0,
                classExtra: [], staticExtra: [], instExtra: [], final: null, metadata: null };
    SLOT.set(n, c);
    const h = {
      _h(v) { c.heritage = v; return h; },
      _m(k, flags, nm, ds) {
        c.els[k] = { kind: flags & 7, stat: !!(flags & 8), priv: !!(flags & 16),
                     keyIdx: (flags & 32) ? nm : -1, name: (flags & 32) ? void 0 : nm,
                     decs: ds, chain: [], extra: [], impl: void 0, access: void 0, out: void 0 };
        return h;
      },
      _k(j, v) { c.keys[j] = v; return h; },
      _c(cls) { return globalThis.__mbun_dcFin(n, cls); },
    };
    c._handle = h;
    return h;
  };
  globalThis.__mbun_dcH = (n) => SLOT.get(n).heritage;
  globalThis.__mbun_dcC = (n) => SLOT.get(n)._handle;
  // Throwaway key for the synthetic member that carries the member-decorator
  // evaluation into the class body; the property is deleted again in __mbun_dcA.
  globalThis.__mbun_dcT = (n) => { const c = SLOT.get(n); c.tmp = c.tmp || Symbol("mbun.dc.tmp"); return c.tmp; };
  globalThis.__mbun_dcK = (n, j) => SLOT.get(n).keys[j];
  globalThis.__mbun_dcP = (n, k, impl, access) => { const el = SLOT.get(n).els[k]; el.impl = impl; el.access = access; };
  globalThis.__mbun_dcA = function (n, cls) {
    const c = SLOT.get(n);
    if (c.tmp !== void 0) { try { delete cls[c.tmp]; } catch (e) {} }
    Object.defineProperty(cls, symOf(n), { value: c, configurable: true });
    if (c.name && cls.name === "") setName(cls, c.name);  // NamedEvaluation
    const par = Object.getPrototypeOf(cls);
    const parentMeta = par == null ? void 0 : par[Symbol.metadata];
    c.metadata = Object.create(parentMeta === void 0 ? null : parentMeta);
    for (const el of c.els) {
      if (el) el.key = el.priv ? el.name : (el.keyIdx >= 0 ? toKey(c.keys[el.keyIdx]) : toKey(el.name));
    }
    const applyEl = (el) => {
      const target = el.stat ? cls : cls.prototype;
      const kind = el.kind;
      let value, desc;
      if (el.priv) {
        value = el.impl;
        if (kind === 4) { setName(value.get, "get " + el.key); setName(value.set, "set " + el.key); }
        else if (kind !== 3) setName(value, (kind === 1 ? "get " : kind === 2 ? "set " : "") + el.key);
      } else if (kind === 3) {
        value = void 0;
      } else {
        desc = Object.getOwnPropertyDescriptor(target, el.key);
        value = kind === 0 ? desc.value : kind === 1 ? desc.get : kind === 2 ? desc.set
              : { get: desc.get, set: desc.set };
      }
      let access;
      if (el.priv) {
        access = el.access;
      } else {
        const key = el.key;
        access = { has: (o) => key in o };
        if (kind !== 2) access.get = (o) => o[key];
        if (kind === 2 || kind === 3 || kind === 4) access.set = (o, v) => { o[key] = v; };
      }
      for (let i = el.decs.length - 1; i >= 0; i--) {
        const add = mkAdd((f) => {
          if (kind === 3 || kind === 4) el.extra.push(f);
          else (el.stat ? c.staticExtra : c.instExtra).push(f);
        });
        const ctx = {
          kind: KINDS[kind], name: el.key, static: el.stat, private: el.priv,
          access: access, metadata: c.metadata, addInitializer: add,
        };
        const r = (0, el.decs[i])(kind === 4 ? { get: value.get, set: value.set } : value, ctx);
        add._close();
        if (r === void 0) continue;
        if (kind === 3) {
          if (typeof r !== "function") throw new TypeError("Field decorator must return a function or undefined");
          el.chain.push(r);
        } else if (kind === 4) {
          if (r === null || typeof r !== "object") throw new TypeError("Accessor decorator must return an object or undefined");
          if (r.get !== void 0) { if (typeof r.get !== "function") throw new TypeError("accessor .get must be a function"); value = { get: r.get, set: value.set }; }
          if (r.set !== void 0) { if (typeof r.set !== "function") throw new TypeError("accessor .set must be a function"); value = { get: value.get, set: r.set }; }
          if (r.init !== void 0) { if (typeof r.init !== "function") throw new TypeError("accessor .init must be a function"); el.chain.push(r.init); }
        } else {
          if (typeof r !== "function") throw new TypeError("Decorator must return a function or undefined");
          value = r;
        }
      }
      if (el.priv) el.out = value;
      else if (kind === 0) Object.defineProperty(target, el.key, { value: value, writable: true, configurable: true, enumerable: false });
      else if (kind === 1) Object.defineProperty(target, el.key, { get: value, set: desc.set, configurable: true, enumerable: false });
      else if (kind === 2) Object.defineProperty(target, el.key, { get: desc.get, set: value, configurable: true, enumerable: false });
      else if (kind === 4) Object.defineProperty(target, el.key, { get: value.get, set: value.set, configurable: true, enumerable: false });
    };
    // Spec application order: static non-fields, instance non-fields, static
    // fields, instance fields (declaration order within each batch).
    for (const [st, fld] of [[true, false], [false, false], [true, true], [false, true]]) {
      for (const el of c.els) {
        if (el && el.stat === st && (el.kind === 3) === fld && el.decs.length) applyEl(el);
      }
    }
    let cur = cls;
    for (let i = c.classDecs.length - 1; i >= 0; i--) {
      const add = mkAdd((f) => c.classExtra.push(f));
      const ctx = { kind: "class", name: c.name, metadata: c.metadata, addInitializer: add };
      const r = (0, c.classDecs[i])(cur, ctx);
      add._close();
      if (r === void 0) continue;
      if (typeof r !== "function") throw new TypeError("Class decorator must return a function or undefined");
      cur = r;
    }
    c.final = cur;
    if (c.classDecs.length || c.els.some((e) => e && e.decs.length)) {
      Object.defineProperty(cur, Symbol.metadata, { value: c.metadata, writable: true, configurable: true, enumerable: false });
    }
    for (const f of c.staticExtra) f.call(cur);
  };
  globalThis.__mbun_dcFin = function (n, cls) {
    const c = cls[symOf(n)] || SLOT.get(n);
    const f = (c && c.final) || cls;
    if (c) for (const g of c.classExtra) g.call(f);
    return f;
  };
  globalThis.__mbun_dcFld = function (n, k, self, v) {
    const el = ctxOf(n, self).els[k];
    for (let i = el.chain.length - 1; i >= 0; i--) v = el.chain[i].call(self, v);
    for (const f of el.extra) f.call(self);
    return v;
  };
  globalThis.__mbun_dcII = function (n, self) {
    for (const f of ctxOf(n, self).instExtra) f.call(self);
  };
  globalThis.__mbun_dcPV = (n, self, k) => ctxOf(n, self).els[k].out;
  // ESM named imports are live bindings. js_parser's CJS lowering binds each one
  // with `let` and subscribes it here (ref: js_parser.cppm build_cjs_import_).
  //
  // A settled exporter needs nothing: its exports are final, so the common
  // acyclic path just assigns and leaves the exports object a plain data object
  // — no accessor, no property-access deopt. Only while the exporter is still
  // evaluating (`__mbun_pending`, stamped across the module body by require()
  // in this file) does the slot become an accessor that fans later
  // `exports.x = v` writes out to every importer's binding, which is what lets
  // a cyclic import observe the finished value the way real ESM does.
  globalThis.__mbun_link = function (ns, name, set) {
    if (ns == null) { set(undefined); return; }
    if (!ns.__mbun_pending) { set(ns[name]); return; }
    const d = Object.getOwnPropertyDescriptor(ns, name);
    // Already linked by another importer of the same name: just join the fan-out.
    if (d && d.get && d.get.__mbun_subs) { d.get.__mbun_subs.push(set); set(d.get.call(ns)); return; }
    // A non-configurable slot can't be swapped; it can't change either, so the
    // snapshot is already correct.
    if (d && !d.configurable) { set(ns[name]); return; }
    const subs = [set];
    let cur = d ? (d.get ? d.get.call(ns) : d.value) : undefined;
    // Preserve enumerability: defining a not-yet-exported name must not make it
    // show up in Object.keys(ns) / spread before the exporter assigns it. The
    // first write upgrades the slot, matching what a plain assignment would do.
    let en = d ? d.enumerable : false;
    const get = function () { return cur; };
    get.__mbun_subs = subs;
    const set_ = function (v) {
      cur = v;
      if (!en) {
        en = true;
        Object.defineProperty(ns, name, { configurable: true, enumerable: true, get, set: set_ });
      }
      for (let i = 0; i < subs.length; i++) subs[i](v);
    };
    Object.defineProperty(ns, name, { configurable: true, enumerable: en, get, set: set_ });
    set(cur);
  };
  // TS legacy (experimentalDecorators) application, tsc's __decorate timing:
  // runs after the class evaluates. Per element: parameter decorators
  // (descending index) then member decorators bottom-up; the tail applies
  // constructor parameter decorators then class decorators bottom-up.
  globalThis.__mbun_ld = function (cls, els, tail) {
    for (const elFn of els) {
      const el = elFn();  // evaluates this element's decorator factories
      const kind = el[0] & 7, stat = !!(el[0] & 8), key = el[1], decs = el[2], params = el[3];
      const target = stat ? cls : cls.prototype;
      for (let i = params.length - 1; i >= 0; i--) {
        const idx = params[i][0], pds = params[i][1];
        for (let j = pds.length - 1; j >= 0; j--) pds[j](target, key, idx);
      }
      // tsc __decorate: the descriptor is `void 0` for a property, or the live
      // descriptor for a method/accessor. Decorators fold over it (r = dec() || r)
      // and the accumulated result is re-applied — which OVERWRITES any
      // `defineProperty` side effect a decorator performed, so an accessor keeps
      // its original get/set unless a decorator explicitly returns a descriptor.
      let r = kind === 3 ? void 0 : Object.getOwnPropertyDescriptor(target, key);
      for (let i = decs.length - 1; i >= 0; i--) {
        const rv = decs[i](target, key, r);
        if (rv !== null && rv !== void 0 && (typeof rv === "object" || typeof rv === "function")) {
          r = rv;
        }
      }
      if (r !== null && r !== void 0 && (typeof r === "object" || typeof r === "function")) {
        Object.defineProperty(target, key, r);
      }
    }
    const t = tail(), cds = t[0], cps = t[1];
    for (let i = cps.length - 1; i >= 0; i--) {
      const idx = cps[i][0], pds = cps[i][1];
      for (let j = pds.length - 1; j >= 0; j--) pds[j](cls, void 0, idx);
    }
    let cur = cls;
    for (let i = cds.length - 1; i >= 0; i--) {
      const r = cds[i](cur);
      if (typeof r === "function") cur = r;
    }
    return cur;
  };
})();
)JS";
