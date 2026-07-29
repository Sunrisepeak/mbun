// node:vm.Module / SourceTextModule / SyntheticModule payload partition.
//
// node builds these on V8's own module records: V8 parses the source, owns the
// import/export bindings and runs the linking algorithm. JSC's C API exposes
// none of that, so the module graph is reconstructed in JS:
//
//   * the import / export clauses are lifted out of the source and the residual
//     body is evaluated as ordinary code in the target realm (the contextified
//     one via vm.__internal.runRaw, or the main one via runInThis);
//   * imported names resolve through a `with` scope backed by a Proxy whose
//     `has` trap only claims the imported identifiers, so every read re-reads
//     the exporting module. That is what keeps bindings LIVE, which circular
//     graphs and SyntheticModule.setExport-after-evaluation depend on;
//   * the namespace object is a Proxy over the export getters, so `export *`
//     re-exports and `in` checks resolve transitively.
//
// evaluate() is deliberately not an async method: per
// https://tc39.es/ecma262/#sec-moduleevaluation a graph with no async
// dependency settles its evaluation promise synchronously, and the tests check
// exactly that (`inspect(mod.evaluate())` must already read `Promise {
// undefined }`).
//
// The classes are only reachable under --experimental-vm-modules, matched
// through a lazy accessor because the builtins image is evaluated before
// process.execArgv exists.
//
// DEFERRED: evaluate({ timeout }) interruption, cachedData/bytecode, node 26's
// linkRequests()/instantiate() split, and dynamic import() inside a plain
// vm.Script (JSC gives the script no module-loader hook).
//
// NOTE: appended AFTER the master builtins IIFE has closed (see image_closure)
// and after :node_vm, whose vm.__internal seam it uses.
export module mbun.jsc.js_builtins:node_vm_modules;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeVmModulesJS = R"JS(
(function () {
  const G = globalThis;
  const NVM = G.__mbunNodeVMNative;
  const M = G.__mbunNativeModules;
  if (!NVM || !M) return;
  const vm = M["vm"];
  if (!vm || !vm.__internal) return;
  const internal = vm.__internal;

  // node only defines vm.Module & friends under --experimental-vm-modules.
  const flagged = () => {
    const argv = (G.process && G.process.execArgv) || [];
    for (const a of argv) if (a === "--experimental-vm-modules") return true;
    return false;
  };

  const ERR = (code, Ctor, msg) => { const e = new Ctor(msg); e.code = code; return e; };
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
  const invArgType = (name, expected, actual, kind) =>
    ERR("ERR_INVALID_ARG_TYPE", TypeError,
        'The "' + name + '" ' + (kind || "argument") + " must be " + expected + "." +
        invalidArgTypeHelper(actual));
  const invalidThis = () => ERR("ERR_INVALID_THIS", TypeError, 'Value of "this" must be of type Module');

  const kWrap = Symbol("kWrap");
  const kNamespace = Symbol("kNamespace");
  const kExports = Symbol("kExports");
  const kStarExports = Symbol("kStarExports");
  const kDeps = Symbol("kDeps");
  const kResolved = Symbol("kResolved");
  const kValues = Symbol("kValues");
  const kRequests = Symbol("kRequests");

  const kMainContextKey = { __proto__: null };
  const identifierCounters = new WeakMap();
  function nextIdentifier(contextKey) {
    let n = identifierCounters.get(contextKey);
    if (n === undefined) n = 0;
    identifierCounters.set(contextKey, n + 1);
    return "vm:module(" + n + ")";
  }

  // ── module source analysis ────────────────────────────────────────────────
  // node hands the source to V8, which owns the parse, the module record and
  // the linker. JSC's C API exposes none of that, so the import/export clauses
  // are lifted out here and the remaining body is evaluated as ordinary code in
  // the target realm. Live bindings survive because imported names resolve
  // through a `with` scope backed by a Proxy: every read re-reads the exporting
  // module, which is what circular graphs and a SyntheticModule.setExport after
  // evaluation depend on.
  const IMPORT_RE = /(^|[\n;])[ \t]*import[ \t\n]+(?:([^'"]*?)[ \t\n]+from[ \t\n]*)?(['"])((?:\\.|(?!\3)[^\\])*)\3([ \t\n]*(?:with|assert)[ \t\n]*\{[^}]*\})?[ \t]*;?/g;
  const EXPORT_FROM_RE = /(^|[\n;])[ \t]*export[ \t\n]+(\*[ \t\n]*(?:as[ \t\n]+([\w$]+)[ \t\n]*)?|\{([^}]*)\})[ \t\n]*from[ \t\n]*(['"])((?:\\.|(?!\5)[^\\])*)\5([ \t\n]*(?:with|assert)[ \t\n]*\{[^}]*\})?[ \t]*;?/g;
  const EXPORT_NAMED_RE = /(^|[\n;])[ \t]*export[ \t\n]*\{([^}]*)\}[ \t]*;?/g;
  const EXPORT_DEFAULT_RE = /(^|[\n;])([ \t]*)export[ \t\n]+default[ \t\n]+/g;
  const EXPORT_DECL_RE = /(^|[\n;])([ \t]*)export[ \t\n]+(?=(?:const|let|var|function|class|async)\b)/g;
  const EXPORT_DECL_NAME_RE =
    /(^|[\n;])[ \t]*export[ \t\n]+(?:async[ \t\n]+)?(?:const|let|var|function|class)[ \t\n]*\*?[ \t\n]*([\w$]+)/g;

  function parseAttributes(text) {
    const attributes = { __proto__: null };
    if (!text) return attributes;
    const body = text.slice(text.indexOf("{") + 1, text.lastIndexOf("}"));
    const re = /(?:([\w$]+)|(['"])((?:\\.|(?!\2)[^\\])*)\2)[ \t\n]*:[ \t\n]*(['"])((?:\\.|(?!\4)[^\\])*)\4/g;
    let m;
    while ((m = re.exec(body)) !== null) {
      attributes[m[1] !== undefined ? m[1] : m[3]] = m[5];
    }
    return attributes;
  }

  function splitClause(clause) {
    const out = [];
    for (const raw of clause.split(",")) {
      const part = raw.trim();
      if (part === "") continue;
      const m = /^(.+?)[ \t\n]+as[ \t\n]+(.+)$/.exec(part);
      if (m) out.push([m[1].trim(), m[2].trim()]);
      else out.push([part, part]);
    }
    return out;
  }

  function analyze(source) {
    const imports = [];
    const exports = [];
    const reexports = [];
    let body = source;

    const declNames = [];
    let dm;
    EXPORT_DECL_NAME_RE.lastIndex = 0;
    while ((dm = EXPORT_DECL_NAME_RE.exec(source)) !== null) declNames.push(dm[2]);

    body = body.replace(EXPORT_FROM_RE, (all, lead, what, starAs, clause, q, spec, attrText) => {
      const attributes = parseAttributes(attrText);
      if (what.charAt(0) === "*") {
        reexports.push({ specifier: spec, attributes, phase: "evaluation", star: !starAs, starAs: starAs || null, names: [], dep: undefined });
      } else {
        reexports.push({ specifier: spec, attributes, phase: "evaluation", star: false, starAs: null, names: splitClause(clause), dep: undefined });
      }
      return lead;
    });

    body = body.replace(IMPORT_RE, (all, lead, clause, q, spec, attrText) => {
      const attributes = parseAttributes(attrText);
      let phase = "evaluation";
      if (clause && /^[ \t\n]*source\b/.test(clause)) {
        phase = "source";
        clause = clause.replace(/^[ \t\n]*source\b/, "");
      }
      const entry = { specifier: spec, attributes, phase, bindings: [], star: null, dep: undefined };
      if (clause) {
        let rest = clause.trim();
        const starMatch = /\*[ \t\n]*as[ \t\n]+([\w$]+)/.exec(rest);
        if (starMatch) {
          entry.star = starMatch[1];
          rest = rest.replace(starMatch[0], "").trim();
        }
        const braceMatch = /\{([^}]*)\}/.exec(rest);
        if (braceMatch) {
          for (const pair of splitClause(braceMatch[1])) entry.bindings.push(pair);
          rest = rest.replace(braceMatch[0], "").trim();
        }
        const def = rest.replace(/^,+|,+$/g, "").trim();
        if (def) entry.bindings.push(["default", def]);
      }
      imports.push(entry);
      return lead;
    });

    body = body.replace(EXPORT_NAMED_RE, (all, lead, clause) => {
      for (const pair of splitClause(clause)) exports.push({ local: pair[0], exported: pair[1] });
      return lead;
    });

    body = body.replace(EXPORT_DEFAULT_RE, (all, lead, indent) => {
      exports.push({ local: "__vmDefault", exported: "default" });
      return lead + indent + "let __vmDefault = ";
    });

    body = body.replace(EXPORT_DECL_RE, (all, lead, indent) => lead + indent);

    for (const name of declNames) exports.push({ local: name, exported: name });
    return { imports, exports, reexports, body };
  }

  const META_RE = new RegExp("\\bimport[ \\t\\n]*\\.[ \\t\\n]*meta\\b", "g");
  const DYNIMPORT_RE = new RegExp("\\bimport[ \\t\\n]*\\(", "g");

  function buildWrapper(analysis) {
    const registrations = analysis.exports
      .map((n) => "__e(" + JSON.stringify(n.exported) + ", () => " + n.local + ");")
      .join("\n");
    let body = analysis.body;
    // Built with RegExp() rather than literals: the source text of a literal
    // containing `import` + `.meta` / `import(` is itself rewritten by the
    // ESM->CJS lowering this file may be loaded through.
    body = body.replace(META_RE, "__vm.meta");
    body = body.replace(DYNIMPORT_RE, "__vm.dynamicImport(");
    // The JSC C API has no module-record evaluator. The reconstructed graph
    // still records TLA for the Node 26 introspection APIs, but its ordinary
    // function wrapper cannot parse a bare await expression. Lower statement
    // position awaits so linking/instantiation and namespace inspection remain
    // available; real async evaluation stays outside this JS fallback.
    body = body.replace(/(^|[;\n])(\s*)await\s+/g, "$1$2");
    return "(function (__vm) {\n" +
           "const __e = __vm.registerExport;\n" +
           "with (__vm.scope) {\n" +
           registrations + "\n" +
           body + "\n" +
           "}\n})";
  }

  // ── the module classes ────────────────────────────────────────────────────
  const isModule = (value) =>
    value !== null && typeof value === "object" && kExports in value;
  const brand = (mod) => { if (!isModule(mod)) throw invalidThis(); return mod[kWrap]; };

  class Module {
    constructor() {
      if (new.target === Module) throw new TypeError("Module is not a constructor");
    }
    get status() { return brand(this).status; }
    get identifier() { return brand(this).identifier; }
    get context() { return brand(this).contextObject; }
    get error() {
      const w = brand(this);
      if (w.status !== "errored") {
        throw ERR("ERR_VM_MODULE_STATUS", Error, "Module status must be errored");
      }
      return w.error;
    }
    get namespace() {
      const w = brand(this);
      if (w.status === "unlinked") {
        throw ERR("ERR_VM_MODULE_STATUS", Error, "Module status must not be unlinked");
      }
      return this[kNamespace];
    }
    linkRequests(modules) {
      const w = brand(this);
      if (!Array.isArray(modules)) throw invArgType("modules", "an Array", modules);
      if (w.status !== "unlinked") {
        throw ERR("ERR_VM_MODULE_STATUS", Error, "Module status must be unlinked");
      }
      if (modules.length !== this[kDeps].length) {
        throw ERR("ERR_MODULE_LINK_MISMATCH", Error, "The requested modules do not match the module requests");
      }
      const bySpecifier = new Map();
      for (let i = 0; i < modules.length; ++i) {
        const resolved = modules[i];
        if (!isModule(resolved)) {
          throw ERR("ERR_VM_MODULE_NOT_MODULE", TypeError,
                    "Provided module is not an instance of Module");
        }
        const dep = this[kDeps][i];
        const prior = bySpecifier.get(dep.specifier);
        if (prior !== undefined && prior !== resolved) {
          throw ERR("ERR_MODULE_LINK_MISMATCH", Error, "The requested modules do not match the module requests");
        }
        bySpecifier.set(dep.specifier, resolved);
        this[kResolved].set(dep, resolved);
      }
      w.requestsLinked = true;
      return undefined;
    }
    instantiate() {
      const w = brand(this);
      if (!w.requestsLinked) {
        throw ERR("ERR_VM_MODULE_LINK_FAILURE", Error,
                  "Module " + JSON.stringify(w.identifier) + " has not been linked");
      }
      instantiateModule(this, new Set());
      return undefined;
    }
    hasTopLevelAwait() { return brand(this).hasTopLevelAwait; }
    hasAsyncGraph() {
      const w = brand(this);
      if (w.status !== "linked" && w.status !== "evaluated" && w.status !== "evaluating") {
        throw ERR("ERR_VM_MODULE_STATUS", Error, "Module status must be instantiated");
      }
      return hasAsyncGraph(this, new Set());
    }
    link(linker) {
      let w;
      try {
        w = brand(this);
        if (typeof linker !== "function") throw invArgType("linker", "of type function", linker);
        if (w.status !== "unlinked") {
          throw ERR("ERR_VM_MODULE_STATUS", Error, "Module status must be unlinked");
        }
      } catch (e) {
        return Promise.reject(e);
      }
      w.status = "linking";
      if (this[kDeps].length === 0) {
        // Nothing to resolve: settle synchronously rather than through an
        // extra async hop, so `await mod.link(...)` observes 'linked'.
        w.status = "linked";
        return Promise.resolve(undefined);
      }
      return linkModule(this, linker, new Set()).then(
        () => { w.status = "linked"; return undefined; },
        (e) => { w.status = "errored"; w.error = e; throw e; });
    }
    // Deliberately NOT an async function. Per
    // https://tc39.es/ecma262/#sec-moduleevaluation a module graph with no
    // async dependency settles its evaluation promise *synchronously*, and the
    // tests observe exactly that (`inspect(mod.evaluate())` must already read
    // `Promise { undefined }`). An async method would hand back a pending
    // promise instead.
    evaluate(options) {
      let w;
      try {
        w = brand(this);
        if (options !== undefined && (typeof options !== "object" || options === null)) {
          throw invArgType("options", "of type object", options);
        }
        if (w.status === "unlinked" || w.status === "linking") {
          throw ERR("ERR_VM_MODULE_STATUS", Error,
                    "Module status must be one of linked, evaluated, or errored");
        }
      } catch (e) {
        return Promise.reject(e);
      }
      if (w.evaluatePromise !== undefined) return w.evaluatePromise;
      if (w.status === "errored") {
        w.evaluatePromise = Promise.reject(w.error);
        return w.evaluatePromise;
      }
      try {
        evaluateModule(this, new Set());
        w.status = "evaluated";
        w.evaluatePromise = Promise.resolve(undefined);
      } catch (e) {
        w.status = "errored";
        w.error = e;
        w.evaluatePromise = Promise.reject(e);
      }
      return w.evaluatePromise;
    }
  }

  async function linkModule(mod, linker, seen) {
    if (seen.has(mod)) return;
    seen.add(mod);
    for (const dep of mod[kDeps]) {
      const extra = { attributes: dep.attributes, assert: dep.attributes };
      const result = await linker(dep.specifier, mod, extra);
      if (!isModule(result)) {
        throw ERR("ERR_VM_MODULE_NOT_MODULE", TypeError,
                  "Provided module is not an instance of Module");
      }
      mod[kResolved].set(dep, result);
      const rw = result[kWrap];
      if (rw.status === "unlinked") {
        rw.status = "linking";
        await linkModule(result, linker, seen);
        rw.status = "linked";
      }
    }
  }

  function instantiateModule(mod, seen) {
    if (seen.has(mod)) return;
    seen.add(mod);
    const w = mod[kWrap];
    for (const dep of mod[kDeps]) {
      const resolved = mod[kResolved].get(dep);
      if (resolved === undefined) {
        throw ERR("ERR_VM_MODULE_LINK_FAILURE", Error,
                  "request for '" + dep.specifier + "' can not be resolved on module '" +
                  w.identifier + "' that is not linked");
      }
      if (!resolved[kWrap].requestsLinked && resolved[kDeps].length !== 0) {
        throw ERR("ERR_VM_MODULE_LINK_FAILURE", Error,
                  "request for '" + dep.specifier + "' can not be resolved on module '" +
                  resolved[kWrap].identifier + "' that is not linked");
      }
      instantiateModule(resolved, seen);
    }
    w.status = "linked";
  }

  function hasAsyncGraph(mod, seen) {
    if (seen.has(mod)) return false;
    seen.add(mod);
    if (mod[kWrap].hasTopLevelAwait) return true;
    for (const dep of mod[kDeps]) {
      const resolved = mod[kResolved].get(dep);
      if (resolved && hasAsyncGraph(resolved, seen)) return true;
    }
    return false;
  }

  function evaluateModule(mod, seen) {
    if (seen.has(mod)) return;
    seen.add(mod);
    const w = mod[kWrap];
    if (w.evaluated) return;
    w.evaluated = true;
    w.status = "evaluating";
    for (const dep of mod[kResolved].values()) {
      evaluateModule(dep, seen);
      const dw = dep[kWrap];
      if (dw.status === "errored") throw dw.error;
      if (dw.status === "evaluating") dw.status = "evaluated";
    }
    // The result is intentionally not awaited: an async evaluation step of a
    // SyntheticModule that rejects is unobservable from the outside and has to
    // reach the isolate-level unhandledRejection handler (SMR Evaluate).
    w.run();
  }

  function lookupExport(mod, key, seen) {
    if (typeof key !== "string") return undefined;
    if (seen.has(mod)) return undefined;
    seen.add(mod);
    const own = mod[kExports].get(key);
    if (own !== undefined) return own;
    for (const star of mod[kStarExports]) {
      const dep = mod[kResolved].get(star);
      if (dep === undefined) continue;
      const found = lookupExport(dep, key, seen);
      if (found !== undefined) return found;
    }
    return undefined;
  }

  function exportNamesOf(mod, seen) {
    if (seen.has(mod)) return [];
    seen.add(mod);
    const names = [...mod[kExports].keys()];
    for (const star of mod[kStarExports]) {
      const dep = mod[kResolved].get(star);
      if (dep === undefined) continue;
      for (const n of exportNamesOf(dep, seen)) {
        if (n !== "default" && !names.includes(n)) names.push(n);
      }
    }
    return names;
  }

  function makeNamespace(mod) {
    return new Proxy({ __proto__: null }, {
      get(t, key) {
        if (key === Symbol.toStringTag) return "Module";
        const getter = lookupExport(mod, key, new Set());
        return getter === undefined ? undefined : getter();
      },
      has(t, key) {
        if (key === Symbol.toStringTag) return true;
        return lookupExport(mod, key, new Set()) !== undefined;
      },
      ownKeys() { return exportNamesOf(mod, new Set()); },
      getOwnPropertyDescriptor(t, key) {
        const getter = lookupExport(mod, key, new Set());
        if (getter === undefined) return undefined;
        return { value: getter(), writable: true, enumerable: true, configurable: true };
      },
      set() { return false; },
      defineProperty() { return false; },
    });
  }

  function hasSourceTopLevelAwait(source) {
    // Ignore strings and comments, then track function bodies. This keeps the
    // query tied to the source module rather than to promise behavior at run
    // time: an await inside an exported async function is not TLA.
    const text = source.replace(/(?:\/\*[\s\S]*?\*\/|\/\/[^\n]*|'(?:\\.|[^'\\])*'|"(?:\\.|[^"\\])*"|`(?:\\.|[^`\\])*`)/g,
      (match) => match.replace(/[^\n]/g, " "));
    let braceDepth = 0;
    let functionPending = false;
    const functionDepths = [];
    for (let i = 0; i < text.length;) {
      const word = /^[A-Za-z_$][\w$]*/.exec(text.slice(i));
      if (word) {
        if (word[0] === "function") functionPending = true;
        if (word[0] === "await" && functionDepths.length === 0) return true;
        i += word[0].length;
        continue;
      }
      if (text[i] === "{") {
        ++braceDepth;
        if (functionPending) { functionDepths.push(braceDepth); functionPending = false; }
      } else if (text[i] === "}") {
        if (functionDepths[functionDepths.length - 1] === braceDepth) functionDepths.pop();
        --braceDepth;
      }
      ++i;
    }
    return false;
  }

  function initBase(mod, contextObject, identifier) {
    const contextKey = contextObject === undefined ? kMainContextKey : contextObject;
    mod[kExports] = new Map();
    mod[kStarExports] = [];
    mod[kDeps] = [];
    mod[kResolved] = new Map();
    mod[kRequests] = [];
    mod[kWrap] = {
      status: "unlinked",
      identifier: identifier === undefined ? nextIdentifier(contextKey) : `${identifier}`,
      contextObject,
      error: undefined,
      evaluated: false,
      evaluatePromise: undefined,
      dependencyList: undefined,
      requestsLinked: false,
      hasTopLevelAwait: false,
      run: () => undefined,
    };
    mod[kNamespace] = makeNamespace(mod);
  }

  const inspectCustom = Symbol.for("nodejs.util.inspect.custom");
  function inspectModule(mod, name, depth, opts, inspect) {
    const w = brand(mod);
    if (typeof depth === "number" && depth < 0) return "[" + name + "]";
    const shown = { status: w.status, identifier: w.identifier, context: w.contextObject };
    const fn = typeof inspect === "function"
      ? inspect
      : (M["util"] && M["util"].inspect);
    return name + " " + fn(shown, opts || {});
  }

  class SourceTextModule extends Module {
    constructor(sourceText, options) {
      super();
      if (typeof sourceText !== "string") {
        throw invArgType("sourceText", "of type string", sourceText);
      }
      options = options === undefined ? {} : options;
      if (typeof options !== "object" || options === null) {
        throw invArgType("options", "of type object", options);
      }
      const contextObject = options.context;
      if (contextObject !== undefined && !vm.isContext(contextObject)) {
        throw invArgType("options.context", "a vm.Context", contextObject, "property");
      }
      initBase(this, contextObject, options.identifier);

      const analysis = analyze(sourceText);
      this[kWrap].hasTopLevelAwait = hasSourceTopLevelAwait(sourceText);
      const self = this;
      const seenRequests = new Map();
      const addDep = (entry) => {
        const key = entry.phase + "\u0000" + entry.specifier + "\u0000" + JSON.stringify(entry.attributes);
        let dep = seenRequests.get(key);
        if (dep === undefined) {
          dep = { specifier: entry.specifier, attributes: entry.attributes, phase: entry.phase };
          seenRequests.set(key, dep);
          this[kDeps].push(dep);
          this[kRequests].push(Object.freeze({ __proto__: null, specifier: dep.specifier,
            attributes: Object.freeze({ __proto__: null, ...dep.attributes }), phase: dep.phase }));
        }
        entry.dep = dep;
        return dep;
      };
      for (const imp of analysis.imports) addDep(imp);
      for (const rex of analysis.reexports) addDep(rex);
      this[kRequests] = Object.freeze(this[kRequests]);
      // Instantiation exposes the complete namespace shape before evaluation.
      // The wrapper replaces these placeholders with live local bindings when
      // it runs, while re-export getters below are live from the beginning.
      for (const exp of analysis.exports) {
        this[kExports].set(exp.exported, () => undefined);
      }

      const bindings = new Map();
      for (const imp of analysis.imports) {
        if (imp.star) {
          bindings.set(imp.star, () => self[kResolved].get(imp.dep)[kNamespace]);
        }
        for (const pair of imp.bindings) {
          const imported = pair[0];
          bindings.set(pair[1], () => self[kResolved].get(imp.dep)[kNamespace][imported]);
        }
      }
      for (const rex of analysis.reexports) {
        if (rex.star && !rex.starAs) {
          this[kStarExports].push(rex.dep);
        } else if (rex.starAs) {
          this[kExports].set(rex.starAs, () => self[kResolved].get(rex.dep)[kNamespace]);
        }
        for (const pair of rex.names) {
          const imported = pair[0];
          this[kExports].set(pair[1],
            () => self[kResolved].get(rex.dep)[kNamespace][imported]);
        }
      }

      const scope = new Proxy({ __proto__: null }, {
        has: (t, k) => bindings.has(k),
        get: (t, k) => { const f = bindings.get(k); return f === undefined ? undefined : f(); },
        set: () => { throw new TypeError("Assignment to constant variable."); },
      });

      const metaObject = { __proto__: null };
      const initMeta = options.initializeImportMeta;
      if (initMeta !== undefined && typeof initMeta !== "function") {
        throw invArgType("options.initializeImportMeta", "of type function", initMeta, "property");
      }
      let metaReady = initMeta === undefined;
      const importCb = options.importModuleDynamically;
      const bridge = {
        scope,
        // node runs initializeImportMeta when the module record is
        // instantiated, never inside the constructor — the callback commonly
        // closes over the very binding `new SourceTextModule(...)` is being
        // assigned to, which is still in TDZ at construction time.
        get meta() {
          if (!metaReady) { metaReady = true; initMeta(metaObject, self); }
          return metaObject;
        },
        registerExport: (name, getter) => { self[kExports].set(name, getter); },
        dynamicImport: async (specifier, attrs) => {
          if (typeof importCb !== "function") {
            throw ERR("ERR_VM_DYNAMIC_IMPORT_CALLBACK_MISSING", TypeError,
                      "A dynamic import callback was not specified.");
          }
          const result = await importCb(specifier, self, attrs);
          if (!isModule(result)) {
            throw ERR("ERR_VM_MODULE_NOT_MODULE", TypeError,
                      "Provided module is not an instance of Module");
          }
          if (result[kWrap].status !== "evaluated") await result.evaluate();
          return result[kNamespace];
        },
      };

      const wrapperSource = buildWrapper(analysis);
      const wrapper = contextObject === undefined
        ? NVM.runInThis(wrapperSource, this[kWrap].identifier)
        : internal.runRaw(contextObject, wrapperSource, this[kWrap].identifier);
      this[kWrap].run = () => wrapper(bridge);
    }
    get dependencySpecifiers() {
      const w = brand(this);
      if (w.dependencyList === undefined) {
        w.dependencyList = Object.freeze(this[kRequests].map((d) => d.specifier));
      }
      return w.dependencyList;
    }
    get moduleRequests() { brand(this); return this[kRequests]; }
    createCachedData() {
      brand(this);
      const B = G.Buffer;
      return B ? B.from("mbun-vm-module-cache ") : new Uint8Array([1]);
    }
    [inspectCustom](depth, opts, inspect) {
      return inspectModule(this, "SourceTextModule", depth, opts, inspect);
    }
  }

  class SyntheticModule extends Module {
    constructor(exportNames, evaluateCallback, options) {
      super();
      if (!Array.isArray(exportNames) || exportNames.some((n) => typeof n !== "string")) {
        throw ERR("ERR_INVALID_ARG_TYPE", TypeError,
                  'The "exportNames" argument must be an Array of unique strings.' +
                  invalidArgTypeHelper(exportNames));
      }
      const seen = new Set();
      for (const name of exportNames) {
        if (seen.has(name)) {
          throw ERR("ERR_INVALID_ARG_VALUE", TypeError,
                    "The property 'exportNames." + name + "' is duplicated. Received '" +
                    name + "'");
        }
        seen.add(name);
      }
      if (typeof evaluateCallback !== "function") {
        throw invArgType("evaluateCallback", "of type function", evaluateCallback);
      }
      options = options === undefined ? {} : options;
      if (typeof options !== "object" || options === null) {
        throw invArgType("options", "of type object", options);
      }
      const contextObject = options.context;
      if (contextObject !== undefined && !vm.isContext(contextObject)) {
        throw invArgType("options.context", "a vm.Context", contextObject, "property");
      }
      initBase(this, contextObject, options.identifier);
      const values = new Map();
      for (const name of exportNames) {
        values.set(name, undefined);
        this[kExports].set(name, () => values.get(name));
      }
      Object.defineProperty(this, kValues, { value: values, enumerable: false });
      this[kWrap].status = "linked";
      const self = this;
      this[kWrap].run = () => evaluateCallback.call(undefined, self);
    }
    // A SyntheticModule has no dependencies and reports itself linked from
    // construction, yet node still accepts (and the tests still call) link().
    async link(linker) {
      brand(this);
      if (typeof linker !== "function") throw invArgType("linker", "of type function", linker);
      return undefined;
    }
    setExport(name, value) {
      brand(this);
      const values = this[kValues];
      if (values === undefined) throw invalidThis();
      if (typeof name !== "string") throw invArgType("name", "of type string", name);
      if (!values.has(name)) {
        throw ERR("ERR_VM_MODULE_STATUS", ReferenceError,
                  "Export '" + name + "' is not defined in module");
      }
      values.set(name, value);
      return undefined;
    }
    [inspectCustom](depth, opts, inspect) {
      return inspectModule(this, "SyntheticModule", depth, opts, inspect);
    }
  }

  // Lazily gated: the builtins image is evaluated before process.execArgv
  // exists, so the flag can only be consulted on first access.
  for (const [name, value] of [["Module", Module],
                               ["SourceTextModule", SourceTextModule],
                               ["SyntheticModule", SyntheticModule]]) {
    Object.defineProperty(vm, name, {
      get() { return flagged() ? value : undefined; },
      set(v) {
        Object.defineProperty(vm, name, {
          value: v, writable: true, enumerable: true, configurable: true,
        });
      },
      enumerable: true,
      configurable: true,
    });
  }
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
