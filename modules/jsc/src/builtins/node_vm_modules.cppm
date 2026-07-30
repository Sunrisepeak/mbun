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
// undefined }`). A module with top-level await is compiled to an async wrapper
// and its returned promise remains observable to evaluate().
//
// The classes are NOT gated on --experimental-vm-modules: bun exports them
// unconditionally, and the corpus reaches them directly under `bun test`, where
// no execArgv flag can be supplied. See the note at the export site.
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
  const kNamedImports = Symbol("kNamedImports");
  const kDeps = Symbol("kDeps");
  const kResolved = Symbol("kResolved");
  const kValues = Symbol("kValues");
  const kRequests = Symbol("kRequests");
  // The source text a SourceTextModule was built from, kept so createCachedData
  // can fingerprint it (see :node_vm's cachedData scheme).
  const kSource = Symbol("kSource");

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
  const IMPORT_RE = /(^|[\n;])[ \t]*import[ \t\n]+(?:([^'"]*?)[ \t\n]+from[ \t\n]*)?(['"])((?:\\.|(?!\3)[^\\])*)\3([ \t\n]*(?:with|assert)[ \t\n]*\{[^}]*\})?[ \t]*/g;
  const EXPORT_FROM_RE = /(^|[\n;])[ \t]*export[ \t\n]+(\*[ \t\n]*(?:as[ \t\n]+([\w$]+)[ \t\n]*)?|\{([^}]*)\})[ \t\n]*from[ \t\n]*(['"])((?:\\.|(?!\5)[^\\])*)\5([ \t\n]*(?:with|assert)[ \t\n]*\{[^}]*\})?[ \t]*/g;
  const EXPORT_NAMED_RE = /(^|[\n;])[ \t]*export[ \t\n]*\{([^}]*)\}[ \t]*/g;
  const EXPORT_DEFAULT_RE = /(^|[\n;])([ \t]*)export[ \t\n]+default[ \t\n]+/g;
  const EXPORT_DECL_RE = /(^|[\n;])([ \t]*)export[ \t\n]+(?=(?:const|let|var|function|class|async)\b)/g;
  const EXPORT_DECL_NAME_RE =
    /(^|[\n;])[ \t]*export[ \t\n]+(?:async[ \t\n]+)?(const|let|var|function|class)[ \t\n]*\*?[ \t\n]*([\w$]+)/g;

  // `export const a = 1, b = 2;` declares two exports. The regex above only sees
  // the first declarator, so walk the rest of the declaration list by hand:
  // top-level commas separate declarators, and the statement ends at a `;` or at
  // a newline that is not a continuation of a trailing comma.
  function trailingDeclaratorNames(source, from) {
    const names = [];
    let depth = 0;
    let last = "";
    let i = from;
    while (i < source.length) {
      const c = source.charAt(i);
      if (c === "'" || c === '"' || c === "`") {
        ++i;
        while (i < source.length) {
          if (source.charAt(i) === "\\") { i += 2; continue; }
          if (source.charAt(i) === c) { ++i; break; }
          ++i;
        }
        last = "s";
        continue;
      }
      if (c === "(" || c === "[" || c === "{") { ++depth; last = c; ++i; continue; }
      if (c === ")" || c === "]" || c === "}") { --depth; last = c; ++i; continue; }
      if (depth === 0 && c === ";") break;
      if (c === "\n") { if (last !== ",") break; ++i; continue; }
      if (c === " " || c === "\t" || c === "\r") { ++i; continue; }
      if (depth === 0 && c === ",") {
        ++i;
        const m = /^[ \t\n]*([\w$]+)/.exec(source.slice(i));
        if (m) { names.push(m[1]); i += m[0].length; last = "n"; } else { last = ","; }
        continue;
      }
      last = c;
      ++i;
    }
    return names;
  }

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
    while ((dm = EXPORT_DECL_NAME_RE.exec(source)) !== null) {
      declNames.push(dm[3]);
      if (dm[2] === "const" || dm[2] === "let" || dm[2] === "var") {
        for (const extra of trailingDeclaratorNames(source, EXPORT_DECL_NAME_RE.lastIndex)) {
          declNames.push(extra);
        }
      }
    }

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

  function buildWrapper(analysis, hasTopLevelAwait) {
    const registrations = analysis.exports
      .map((n) => "__e(" + JSON.stringify(n.exported) + ", () => " + n.local + ");")
      .join("\n");
    let body = analysis.body;
    // Built with RegExp() rather than literals: the source text of a literal
    // containing `import` + `.meta` / `import(` is itself rewritten by the
    // ESM->CJS lowering this file may be loaded through.
    body = body.replace(META_RE, "__vm.meta");
    body = body.replace(DYNIMPORT_RE, "__vm.dynamicImport(");
    return "(" + (hasTopLevelAwait ? "async " : "") + "function (__vm) {\n" +
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
      // node rejects both pre-link states here, and its message names both.
      if (w.status === "unlinked" || w.status === "linking") {
        throw ERR("ERR_VM_MODULE_STATUS", Error,
                  "Module status must not be unlinked or linking");
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
          // node distinguishes the two: a link already in flight is a status
          // error, while a module that has finished linking is ALREADY_LINKED.
          if (w.status === "linking") {
            throw ERR("ERR_VM_MODULE_STATUS", Error, "Module status must be unlinked");
          }
          throw ERR("ERR_VM_MODULE_ALREADY_LINKED", Error, "Module has already been linked");
        }
      } catch (e) {
        return Promise.reject(e);
      }
      w.status = "linking";
      if (this[kDeps].length === 0) {
        // Nothing to resolve, but do NOT settle synchronously: node's contract is
        // that an un-awaited `link()` leaves status 'linking'. Flipping to
        // 'linked' inside the microtask still lets `await mod.link(...)` observe
        // 'linked', because the awaiting continuation resumes after this callback.
        return Promise.resolve().then(() => { w.status = "linked"; return undefined; });
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
        // node validates both evaluate() options as TypeErrors before touching
        // module state. `timeout` interruption itself is still DEFERRED, but the
        // argument contract is observable and cheap to honour.
        if (options !== undefined) {
          if (options.breakOnSigint !== undefined && typeof options.breakOnSigint !== "boolean") {
            throw invArgType("options.breakOnSigint", "of type boolean",
                             options.breakOnSigint, "property");
          }
          if (options.timeout !== undefined &&
              (typeof options.timeout !== "number" || !(options.timeout > 0) ||
               !Number.isInteger(options.timeout))) {
            throw ERR("ERR_OUT_OF_RANGE", RangeError,
                      'The value of "options.timeout" is out of range. ' +
                      "It must be a positive integer. Received " + String(options.timeout));
          }
        }
        if (w.status === "unlinked" || w.status === "linking") {
          throw ERR("ERR_VM_MODULE_STATUS", Error,
                    "Module status must be one of linked, evaluated, or errored");
        }
        // Re-entrant evaluate(): the module is running its own body right now,
        // so no evaluation promise exists yet to hand back. (An already-started
        // top-level-await module has one and returns it below.)
        if (w.status === "evaluating" && w.evaluatePromise === undefined) {
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
      return evaluateModule(this, new Set());
    }
  }

  // node resolves every request of one module before descending into any of
  // them: the linker is called for the whole request list of the referrer first,
  // and only then recursively for each resolved dependency. A depth-first walk
  // interleaves the two and reports the requests in the wrong order (and can ask
  // for the same shared dependency twice).
  async function linkModule(mod, linker, seen) {
    if (seen.has(mod)) return;
    seen.add(mod);
    const resolvedDeps = [];
    for (const dep of mod[kDeps]) {
      const extra = { attributes: dep.attributes, assert: dep.attributes };
      const result = await linker(dep.specifier, mod, extra);
      if (!isModule(result)) {
        throw ERR("ERR_VM_MODULE_NOT_MODULE", TypeError,
                  "Provided module is not an instance of Module");
      }
      // A linker may only return modules from the SAME context as the importer;
      // node rejects a cross-context resolution rather than linking realms
      // together. `undefined` is the main context, so compare identity directly.
      if (result[kWrap].contextObject !== mod[kWrap].contextObject) {
        throw ERR("ERR_VM_MODULE_DIFFERENT_CONTEXT", Error,
                  "Linked modules must use the same context");
      }
      // Linking onto an already-errored module fails the whole link, carrying the
      // dependency's own error as `cause` (node sets it so the caller can tell
      // WHICH dependency broke, not just that linking failed).
      if (result[kWrap].status === "errored") {
        const le = ERR("ERR_VM_MODULE_LINK_FAILURE", Error,
                       "Provided module could not be linked");
        le.cause = result[kWrap].error;
        throw le;
      }
      mod[kResolved].set(dep, result);
      resolvedDeps.push(result);
    }
    for (const result of resolvedDeps) {
      const rw = result[kWrap];
      if (rw.status === "unlinked") {
        rw.status = "linking";
        await linkModule(result, linker, seen);
        rw.status = "linked";
      }
    }
    // Only now are the children linked, so their star re-exports are resolvable
    // and exportNamesOf() is complete. Importing a name a dependency does not
    // export is a SYNTAX error in the spec (resolution happens at link time, not
    // at run time), and node surfaces the engine's SyntaxError with no code.
    for (const need of mod[kNamedImports] || []) {
      const target = mod[kResolved].get(need.dep);
      if (target === undefined) continue;
      const available = exportNamesOf(target, new Set());
      for (const name of need.names) {
        if (!available.includes(name)) {
          throw new SyntaxError(
            "The requested module '" + need.dep.specifier +
            "' does not provide an export named '" + name + "'");
        }
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
        // node names the request that cannot be resolved ON the unlinked module,
        // not the request that led us to it.
        const missing = resolved[kDeps][0];
        throw ERR("ERR_VM_MODULE_LINK_FAILURE", Error,
                  "request for '" + missing.specifier + "' can not be resolved on module '" +
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
    if (seen.has(mod)) return mod[kWrap].evaluatePromise || Promise.resolve(undefined);
    seen.add(mod);
    const w = mod[kWrap];
    if (w.evaluatePromise !== undefined) return w.evaluatePromise;
    if (w.evaluated) return Promise.resolve(undefined);
    w.evaluated = true;
    w.status = "evaluating";
    let dependencies;
    try {
      dependencies = [...mod[kResolved].values()].map((dep) => evaluateModule(dep, seen));
    } catch (e) {
      w.status = "errored";
      w.error = e;
      w.evaluatePromise = Promise.reject(e);
      return w.evaluatePromise;
    }
    const run = () => w.run();
    const finish = () => { w.status = "evaluated"; return undefined; };
    const fail = (e) => { w.status = "errored"; w.error = e; throw e; };
    // Promise.all([]) settles asynchronously, while Node exposes a settled
    // promise for a wholly synchronous graph. Keep the fast path synchronous
    // and only await the graph when a dependency or this wrapper is async.
    try {
      const pending = dependencies.some((p) => p && p.__mbunVmAsync === true);
      if (!pending) {
        const result = run();
        // https://tc39.es/ecma262/#sec-smr-Evaluate: a synthetic module's
        // evaluation steps settle its promise *immediately* — either resolved
        // with undefined or rejected with a synchronous throw. Anything the
        // callback returns, including a promise that later rejects, is not
        // observable through evaluate(); a rejection surfaces as an unhandled
        // rejection instead.
        if (w.synthetic === true) {
          finish();
          w.evaluatePromise = Promise.resolve(undefined);
        } else if (result && typeof result.then === "function") {
          w.evaluatePromise = result.then(finish, fail);
          w.evaluatePromise.__mbunVmAsync = true;
        } else {
          finish();
          w.evaluatePromise = Promise.resolve(undefined);
        }
      } else {
        w.evaluatePromise = Promise.all(dependencies).then(run).then(finish, fail);
        w.evaluatePromise.__mbunVmAsync = true;
      }
    } catch (e) {
      w.evaluatePromise = Promise.reject(e);
      w.evaluatePromise.__mbunVmAsync = true;
      w.status = "errored";
      w.error = e;
    }
    return w.evaluatePromise;
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
    // A real module namespace carries `Symbol.toStringTag: 'Module'` as a
    // non-configurable OWN property, and `Reflect.ownKeys` reports it after the
    // exported names. Define it on the proxy target so the ownKeys /
    // getOwnPropertyDescriptor traps can report it without breaking the
    // non-configurability invariant.
    const target = { __proto__: null };
    Object.defineProperty(target, Symbol.toStringTag, {
      value: "Module", writable: false, enumerable: false, configurable: false,
    });
    return new Proxy(target, {
      get(t, key) {
        if (key === Symbol.toStringTag) return "Module";
        const getter = lookupExport(mod, key, new Set());
        return getter === undefined ? undefined : getter();
      },
      has(t, key) {
        if (key === Symbol.toStringTag) return true;
        return lookupExport(mod, key, new Set()) !== undefined;
      },
      ownKeys() {
        const keys = exportNamesOf(mod, new Set());
        keys.push(Symbol.toStringTag);
        return keys;
      },
      getOwnPropertyDescriptor(t, key) {
        if (key === Symbol.toStringTag) {
          return { value: "Module", writable: false, enumerable: false, configurable: false };
        }
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
    mod[kNamedImports] = [];
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
      synthetic: false,
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
      // node validates identifier before use; initBase would otherwise coerce a
      // number through `${identifier}` and silently accept it.
      if (options.identifier !== undefined && typeof options.identifier !== "string") {
        throw invArgType("options.identifier", "of type string", options.identifier, "property");
      }
      // Real bytecode cachedData is DEFERRED (JSC exposes no equivalent), but the
      // argument contract is observable, so reject a non-BufferSource up front.
      if (options.cachedData !== undefined) {
        const cd = options.cachedData;
        const ok = cd instanceof ArrayBuffer ||
                   (typeof SharedArrayBuffer === "function" && cd instanceof SharedArrayBuffer) ||
                   (cd !== null && typeof cd === "object" && ArrayBuffer.isView(cd));
        if (!ok) {
          throw invArgType("options.cachedData",
                           "an instance of Buffer, TypedArray, or DataView", cd, "property");
        }
      }
      if (options.cachedData !== undefined) {
        // Unlike vm.Script -- which only reports cachedDataRejected -- a
        // SourceTextModule handed a cache that does not belong to its source
        // THROWS. The buffer carries a fingerprint of the source it was produced
        // from (see :node_vm), so the mismatch is detectable with no bytecode.
        const cd = options.cachedData;
        const view = ArrayBuffer.isView(cd) ? cd : new Uint8Array(cd);
        if (internal.cachedDataRejects(view, sourceText)) {
          throw ERR("ERR_VM_MODULE_CACHED_DATA_REJECTED", Error, "cachedData buffer was rejected");
        }
      }
      initBase(this, contextObject, options.identifier);
      Object.defineProperty(this, kSource, { value: sourceText, enumerable: false });

      const analysis = analyze(sourceText);
      const hasTopLevelAwait = hasSourceTopLevelAwait(sourceText);
      this[kWrap].hasTopLevelAwait = hasTopLevelAwait;
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
        // Record the named bindings so linkModule can verify at link time that
        // each dependency actually exports them.
        if (imp.bindings && imp.bindings.length) {
          this[kNamedImports].push({ dep: imp.dep, names: imp.bindings.map((b) => b[0]) });
        }
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
      if (importCb !== undefined && typeof importCb !== "function") {
        throw invArgType("options.importModuleDynamically", "of type function", importCb, "property");
      }
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
          if (isModule(result)) {
            if (result[kWrap].status !== "evaluated") await result.evaluate();
            return result[kNamespace];
          }
          // node also accepts an already-evaluated module namespace object.
          if (result !== null && typeof result === "object" &&
              result[Symbol.toStringTag] === "Module") {
            return result;
          }
          throw ERR("ERR_VM_MODULE_NOT_MODULE", TypeError,
                    "Provided module is not an instance of Module");
        },
      };

      const wrapperSource = buildWrapper(analysis, hasTopLevelAwait);
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
      // V8 can only serialise a module that has not run yet; node surfaces the
      // rest as ERR_VM_MODULE_CANNOT_CREATE_CACHED_DATA.
      const st = brand(this).status;
      if (st === "evaluating" || st === "evaluated" || st === "errored") {
        throw ERR("ERR_VM_MODULE_CANNOT_CREATE_CACHED_DATA", Error,
                  "Cached data cannot be created for a module which has been evaluated");
      }
      return internal.makeCachedDataBuffer(this[kSource]);
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
      // node validates identifier before use; initBase would otherwise coerce a
      // number through `${identifier}` and silently accept it.
      if (options.identifier !== undefined && typeof options.identifier !== "string") {
        throw invArgType("options.identifier", "of type string", options.identifier, "property");
      }
      // Real bytecode cachedData is DEFERRED (JSC exposes no equivalent), but the
      // argument contract is observable, so reject a non-BufferSource up front.
      if (options.cachedData !== undefined) {
        const cd = options.cachedData;
        const ok = cd instanceof ArrayBuffer ||
                   (typeof SharedArrayBuffer === "function" && cd instanceof SharedArrayBuffer) ||
                   (cd !== null && typeof cd === "object" && ArrayBuffer.isView(cd));
        if (!ok) {
          throw invArgType("options.cachedData",
                           "an instance of Buffer, TypedArray, or DataView", cd, "property");
        }
      }
      initBase(this, contextObject, options.identifier);
      const values = new Map();
      for (const name of exportNames) {
        values.set(name, undefined);
        this[kExports].set(name, () => values.get(name));
      }
      Object.defineProperty(this, kValues, { value: values, enumerable: false });
      this[kWrap].status = "linked";
      this[kWrap].synthetic = true;
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

  // NOT gated on --experimental-vm-modules. node hides these behind the flag,
  // but bun exports them unconditionally (src/js/node/vm.ts's default export
  // lists Module / SourceTextModule / SyntheticModule with no flag check), and
  // bun is the reference implementation here. The gate cost
  // vm/vm-script-fetcher-leak.test.ts, which constructs SourceTextModule
  // directly under `bun test`, where no execArgv flag can be supplied.
  //
  // node's own suite is unaffected: test/common re-spawns any file carrying a
  // `// Flags:` comment as a child process WITH those flags, so the vm-module
  // tests already arrive flagged, and none of them assert the classes are
  // ABSENT without it (test-vm-dynamic-import-callback-missing-flag checks the
  // ERR_VM_DYNAMIC_IMPORT_CALLBACK_MISSING_FLAG path, not class visibility).
  for (const [name, value] of [["Module", Module],
                               ["SourceTextModule", SourceTextModule],
                               ["SyntheticModule", SyntheticModule]]) {
    Object.defineProperty(vm, name, {
      value, writable: true, enumerable: true, configurable: true,
    });
  }
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
