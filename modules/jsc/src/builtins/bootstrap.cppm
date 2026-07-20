// Bootstrap payload partition; keep raw bytes aligned with js_builtins.cppm lines 16-1572.
export module mbun.jsc.js_builtins:bootstrap;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kBootstrapJS = R"JS(
(function () {
  "use strict";
  const G = globalThis;
  const M = (globalThis.__mbunNativeModules = globalThis.__mbunNativeModules || {});
  const def = (names, mod) => { for (const n of names) { M[n] = mod; M["node:" + n] = mod; } };
  // Dynamic import(spec) shim: JSC's C-API context has no host module loader, so
  // `import(...)` in test/run sources is rewritten to this call. Resolves through
  // the CJS require chain (native + node builtins) and adds a non-enumerable
  // `default` self-reference so `(await import(x)).default` matches node's CJS
  // namespace interop (default = module.exports, named exports = its own keys).
  G.__mbun_dynimport = (spec) => {
    try {
      const m = (G.require || ((s) => G.__mbun_require_native(s, ".")))(spec);
      if (m && (typeof m === "object" || typeof m === "function") && !("default" in m)) {
        try { Object.defineProperty(m, "default", { value: m, enumerable: false, configurable: true }); } catch (e) {}
      }
      return Promise.resolve(m);
    } catch (e) { return Promise.reject(e); }
  };

  function deepEq(a, b, strict) {
    if (strict ? Object.is(a, b) : a == b) return true;
    if (typeof a !== "object" || typeof b !== "object" || !a || !b) return false;
    if (Array.isArray(a) !== Array.isArray(b)) return false;
    const ak = Object.keys(a), bk = Object.keys(b);
    if (ak.length !== bk.length) return false;
    for (const k of ak) { if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
      if (!deepEq(a[k], b[k], strict)) return false; }
    return true;
  }

  // ---- path (posix) ----
  // node validates every path arg as a string, throwing ERR_INVALID_ARG_TYPE.
  const pathArgType = (value) => {
    if (value === null) return "null";
    if (typeof value === "object") return "an instance of " + ((value.constructor && value.constructor.name) || "Object");
    return "type " + typeof value;
  };
  const validatePathStr = (p, name) => {
    if (typeof p !== "string" && !(p !== null && typeof p === "object" && Object.prototype.toString.call(p) === "[object String]")) {
      const e = new TypeError('The "' + (name || "path") + '" argument must be of type string. Received ' + pathArgType(p));
      e.code = "ERR_INVALID_ARG_TYPE";
      throw e;
    }
  };
  // path.format() validates its argument is a non-null object (node uses
  // validateObject → ERR_INVALID_ARG_TYPE). null has typeof "object" but is
  // still rejected; message reports typeof for the simplified template.
  const validatePathObject = (o) => {
    if (o === null || typeof o !== "object") {
      const e = new TypeError('The "pathObject" property must be of type object, got ' + typeof o);
      e.code = "ERR_INVALID_ARG_TYPE";
      throw e;
    }
  };
  // resolve() validates each arg inside its reverse loop (node uses the
  // "paths[i]" property name and stops once an absolute path is found).
  const validatePathsArg = (p, i) => {
    if (typeof p !== "string" && !(p !== null && typeof p === "object" && Object.prototype.toString.call(p) === "[object String]")) {
      const e = new TypeError('The "paths[' + i + ']" property must be of type string, got ' + (Array.isArray(p) ? "array" : typeof p));
      e.code = "ERR_INVALID_ARG_TYPE";
      throw e;
    }
  };
  // node's extname algorithm (preDotState) — handles ".."→"", "..."→".", drive prefix, etc.
  const extnameImpl = (p, isWin) => {
    validatePathStr(p);
    let start = 0, startDot = -1, startPart = 0, end = -1, matchedSlash = true, preDotState = 0;
    if (isWin && p.length >= 2 && p.charCodeAt(1) === 58) { const c = p.charCodeAt(0); if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122)) { start = startPart = 2; } }
    for (let i = p.length - 1; i >= start; --i) {
      const code = p.charCodeAt(i);
      if (code === 47 || (isWin && code === 92)) { if (!matchedSlash) { startPart = i + 1; break; } continue; }
      if (end === -1) { matchedSlash = false; end = i + 1; }
      if (code === 46) { if (startDot === -1) startDot = i; else if (preDotState !== 1) preDotState = 1; }
      else if (startDot !== -1) preDotState = -1;
    }
    if (startDot === -1 || end === -1 || preDotState === 0 || (preDotState === 1 && startDot === end - 1 && startDot === startPart + 1)) return "";
    return p.slice(startDot, end);
  };
  const path = {
    sep: "/", delimiter: ":",
    isAbsolute(p) { validatePathStr(p); return p.charCodeAt(0) === 47; },
    normalize(p) {
      validatePathStr(p);
      if (p.length === 0) return ".";
      const abs = p.charCodeAt(0) === 47, trail = p.charCodeAt(p.length - 1) === 47;
      const out = [];
      for (const s of p.split("/")) {
        if (s === "" || s === ".") continue;
        if (s === "..") { if (out.length && out[out.length - 1] !== "..") out.pop(); else if (!abs) out.push(".."); }
        else out.push(s);
      }
      let r = out.join("/");
      if (abs) r = "/" + r; else if (r === "") r = ".";
      if (trail && r[r.length - 1] !== "/") r += "/";
      return r;
    },
    join(...a) {
      const parts = a.filter((x) => { validatePathStr(x); return x.length; });
      return parts.length ? path.normalize(parts.join("/")) : ".";
    },
    resolve(...a) {
      let resolved = "", abs = false;
      for (let i = a.length - 1; i >= -1 && !abs; i--) {
        const p = i >= 0 ? a[i] : (globalThis.process && globalThis.process.cwd ? globalThis.process.cwd() : "/");
        validatePathsArg(p, i);
        if (!p.length) continue;
        resolved = p + "/" + resolved; abs = p.charCodeAt(0) === 47;
      }
      let r = path.normalize(resolved);
      if (r.length > 1 && r[r.length - 1] === "/") r = r.slice(0, -1);  // resolve drops trailing slash
      if (abs) return r[0] === "/" ? r : "/" + r;
      return r.length && r !== "." ? (r[0] === "/" ? r.slice(1) : r) : (r || ".");
    },
    dirname(p) {
      validatePathStr(p);
      if (p.length === 0) return ".";
      const hasRoot = p.charCodeAt(0) === 47;
      let end = -1, matched = true;
      for (let i = p.length - 1; i >= 1; i--) { if (p.charCodeAt(i) === 47) { if (!matched) { end = i; break; } } else matched = false; }
      if (end !== -1) return hasRoot && end === 1 ? "//" : p.slice(0, end);
      return hasRoot ? "/" : ".";
    },
    basename(p, ext) {
      validatePathStr(p);
      if (ext !== undefined) validatePathStr(ext, "ext");
      if (ext !== undefined && ext.length > 0 && ext === p) return "";
      let start = 0, end = -1, matched = true;
      for (let i = p.length - 1; i >= 0; i--) {
        if (p.charCodeAt(i) === 47) { if (!matched) { start = i + 1; break; } }
        else if (end === -1) { matched = false; end = i + 1; }
      }
      let b = end === -1 ? "" : p.slice(start, end);
      if (ext && b.endsWith(ext) && b !== ext) b = b.slice(0, -ext.length);
      return b;
    },
    extname(p) { return extnameImpl(p, false); },
    parse(p) {
      // node lib/path.js posix.parse — canonical preDotState scan (bun matches
      // byte-for-byte: "..".ext="" not ".", "./".dir="").
      validatePathStr(p);
      const ret = { root: "", dir: "", base: "", ext: "", name: "" };
      if (p.length === 0) return ret;
      const isAbsolute = p.charCodeAt(0) === 47;
      let start;
      if (isAbsolute) { ret.root = "/"; start = 1; } else start = 0;
      let startDot = -1, startPart = 0, end = -1, matchedSlash = true, preDotState = 0;
      for (let i = p.length - 1; i >= start; --i) {
        const code = p.charCodeAt(i);
        if (code === 47) { if (!matchedSlash) { startPart = i + 1; break; } continue; }
        if (end === -1) { matchedSlash = false; end = i + 1; }
        if (code === 46) { if (startDot === -1) startDot = i; else if (preDotState !== 1) preDotState = 1; }
        else if (startDot !== -1) preDotState = -1;
      }
      if (end !== -1) {
        const s = startPart === 0 && isAbsolute ? 1 : startPart;
        if (startDot === -1 || preDotState === 0 || (preDotState === 1 && startDot === end - 1 && startDot === startPart + 1)) {
          ret.base = ret.name = p.slice(s, end);
        } else { ret.name = p.slice(s, startDot); ret.base = p.slice(s, end); ret.ext = p.slice(startDot, end); }
      }
      if (startPart > 0) ret.dir = p.slice(0, startPart - 1);
      else if (isAbsolute) ret.dir = "/";
      return ret;
    },
    format(o) {
      validatePathObject(o);
      const dir = o.dir || o.root || "";
      const base = o.base || ((o.name || "") + (o.ext ? (o.ext[0] === "." ? "" : ".") + o.ext : ""));
      if (!dir) return base;
      return dir === o.root ? dir + base : dir + "/" + base;
    },
    relative(from, to) {
      validatePathStr(from, "from"); validatePathStr(to, "to");
      from = path.resolve(from); to = path.resolve(to);
      if (from === to) return "";
      const fp = from.split("/").filter(Boolean), tp = to.split("/").filter(Boolean);
      let i = 0; while (i < fp.length && i < tp.length && fp[i] === tp[i]) i++;
      return fp.slice(i).map(() => "..").concat(tp.slice(i)).join("/");
    },
  };
  // ---- win32 path (drive letters, \, UNC) — node's algorithm, simplified ----
  const isSepW = (c) => c === "/" || c === "\\";
  const isLetterW = (c) => c && ((c >= "A" && c <= "Z") || (c >= "a" && c <= "z"));
  // Parse a win32 path into { device, isAbs, tail }.
  const parseW = (p) => {
    let device = "", isAbs = false, rootEnd = 0;
    if (p.length >= 2 && isSepW(p[0]) && isSepW(p[1])) {  // UNC \\server\share
      let j = 2; while (j < p.length && !isSepW(p[j])) j++;
      if (j < p.length && j > 2) { const server = p.slice(2, j); let sepEnd = j; while (sepEnd < p.length && isSepW(p[sepEnd])) sepEnd++; if (sepEnd < p.length) { let k = sepEnd; while (k < p.length && !isSepW(p[k])) k++; device = "\\\\" + server + "\\" + p.slice(sepEnd, k); rootEnd = k; isAbs = true; } else { isAbs = true; rootEnd = 1; } }
      else { isAbs = true; rootEnd = 1; }
    } else if (isLetterW(p[0]) && p[1] === ":") {
      device = p.slice(0, 2);
      if (p.length > 2 && isSepW(p[2])) { isAbs = true; rootEnd = 3; } else rootEnd = 2;
    } else if (isSepW(p[0])) { isAbs = true; rootEnd = 1; }
    return { device, isAbs, tail: p.slice(rootEnd) };
  };
  const normSegsW = (tail, isAbs) => { const segs = []; for (const s of tail.split(/[\\/]+/)) { if (s === "" || s === ".") continue; if (s === "..") { if (segs.length && segs[segs.length - 1] !== "..") segs.pop(); else if (!isAbs) segs.push(".."); } else segs.push(s); } return segs; };
  const win32 = {
    sep: "\\", delimiter: ";",
    isAbsolute(p) { validatePathStr(p); if (!p.length) return false; if (isSepW(p[0])) return true; return !!(isLetterW(p[0]) && p[1] === ":" && p.length > 2 && isSepW(p[2])); },
    normalize(p) { validatePathStr(p); if (p.length === 0) return "."; const { device, isAbs, tail } = parseW(p); const segs = normSegsW(tail, isAbs); let body = segs.join("\\"); if (body === "" && !isAbs) body = "."; let r = device + (isAbs ? "\\" : "") + body; if (body !== "" && isSepW(p[p.length - 1]) && r[r.length - 1] !== "\\") r += "\\"; return r; },
    join(...a) {
      const parts = a.filter((x) => { validatePathStr(x); return x.length; });
      if (!parts.length) return ".";
      let joined = parts.join("\\");
      // node path.js win32 join: collapse a leading slash-run to one "\" unless
      // the first part is a real UNC prefix (exactly two seps + non-sep).
      const first = parts[0];
      const isSep = (c) => c === "/" || c === "\\";
      let needsReplace = true, slashCount = 0;
      if (isSep(first[0])) {
        ++slashCount;
        if (first.length > 1 && isSep(first[1])) {
          ++slashCount;
          if (first.length > 2) { if (isSep(first[2])) ++slashCount; else needsReplace = false; }
        }
      }
      if (needsReplace) {
        while (slashCount < joined.length && isSep(joined[slashCount])) ++slashCount;
        if (slashCount >= 2) joined = "\\" + joined.slice(slashCount);
      }
      return win32.normalize(joined);
    },
    resolve(...a) {
      let rDev = "", rTail = "", rAbs = false;
      for (let i = a.length - 1; i >= -1 && !(rDev && rAbs); i--) {
        const p = i >= 0 ? a[i] : (globalThis.process && process.cwd ? process.cwd().replace(/\//g, "\\") : "C:\\");
        validatePathsArg(p, i);
        if (!p.length) continue;
        const { device, isAbs, tail } = parseW(p);
        if (device && rDev && device.toLowerCase() !== rDev.toLowerCase()) continue;
        if (!rDev) rDev = device;
        if (!rAbs) { rTail = tail + (rTail ? "\\" + rTail : ""); rAbs = isAbs; }
      }
      if (!rDev && !rAbs) { const cwd = parseW(globalThis.process && process.cwd ? process.cwd().replace(/\//g, "\\") : "C:\\"); rDev = cwd.device; rAbs = cwd.isAbs; }
      const segs = normSegsW(rTail, rAbs); const body = segs.join("\\");
      return rDev + (rAbs ? "\\" : "") + body || ".";
    },
    dirname(p) {
      validatePathStr(p);
      const len = p.length;
      if (len === 0) return ".";
      if (len === 1) return isSepW(p[0]) ? p : ".";
      let rootEnd = -1, offset = 0;
      if (isSepW(p[0])) {
        rootEnd = offset = 1;
        if (isSepW(p[1])) {
          let j = 2, last = j;
          while (j < len && !isSepW(p[j])) j++;
          if (j < len && j !== last) {
            last = j;
            while (j < len && isSepW(p[j])) j++;
            if (j < len && j !== last) {
              last = j;
              while (j < len && !isSepW(p[j])) j++;
              if (j === len) return p;
              if (j !== last) { offset = j + 1; rootEnd = offset; }
            }
          }
        }
      } else if (isLetterW(p[0]) && p[1] === ":") {
        offset = (len > 2 && isSepW(p[2])) ? 3 : 2;
        rootEnd = offset;
      }
      let end = -1, matchedSlash = true;
      for (let i = len - 1; i >= offset; i--) {
        if (isSepW(p[i])) { if (!matchedSlash) { end = i; break; } }
        else matchedSlash = false;
      }
      if (end !== -1) return p.slice(0, end);
      if (rootEnd !== -1) return p.slice(0, rootEnd);
      return ".";
    },
    basename(p, ext) { validatePathStr(p); if (ext !== undefined) validatePathStr(ext, "ext"); if (ext !== undefined && ext.length > 0 && ext === p) return ""; const norm = p.replace(/[\\/]+$/, ""); const i = Math.max(norm.lastIndexOf("\\"), norm.lastIndexOf("/")); let b = norm.slice(i + 1); if (i < 0 && isLetterW(b[0]) && b[1] === ":") b = b.slice(2); if (ext && b.endsWith(ext) && b !== ext) b = b.slice(0, -ext.length); return b; },
    extname(p) { return extnameImpl(p, true); },
    parse(p) {
      // node lib/path.js win32.parse — verbatim (bun ships it). Handles UNC
      // roots, drive letters, preDotState name/ext scan, startPart dir rule.
      validatePathStr(p);
      const ret = { root: "", dir: "", base: "", ext: "", name: "" };
      const len = p.length;
      if (len === 0) return ret;
      let rootEnd = 0;
      if (len === 1) { if (isSepW(p[0])) { ret.root = ret.dir = p; return ret; } ret.base = ret.name = p; return ret; }
      if (isSepW(p[0])) {
        rootEnd = 1;
        if (isSepW(p[1])) {
          let j = 2, last = j;
          while (j < len && !isSepW(p[j])) j++;
          if (j < len && j !== last) {
            last = j;
            while (j < len && isSepW(p[j])) j++;
            if (j < len && j !== last) {
              last = j;
              while (j < len && !isSepW(p[j])) j++;
              if (j === len) rootEnd = j; else if (j !== last) rootEnd = j + 1;
            }
          }
        }
      } else if (isLetterW(p[0]) && p[1] === ":") {
        if (len <= 2) { ret.root = ret.dir = p; return ret; }
        rootEnd = 2;
        if (isSepW(p[2])) { if (len === 3) { ret.root = ret.dir = p; return ret; } rootEnd = 3; }
      }
      if (rootEnd > 0) ret.root = p.slice(0, rootEnd);
      let startDot = -1, startPart = rootEnd, end = -1, matchedSlash = true, preDotState = 0;
      for (let i = len - 1; i >= rootEnd; --i) {
        const code = p.charCodeAt(i);
        if (isSepW(p[i])) { if (!matchedSlash) { startPart = i + 1; break; } continue; }
        if (end === -1) { matchedSlash = false; end = i + 1; }
        if (code === 46) { if (startDot === -1) startDot = i; else if (preDotState !== 1) preDotState = 1; }
        else if (startDot !== -1) preDotState = -1;
      }
      if (end !== -1) {
        if (startDot === -1 || preDotState === 0 || (preDotState === 1 && startDot === end - 1 && startDot === startPart + 1)) {
          ret.base = ret.name = p.slice(startPart, end);
        } else {
          ret.name = p.slice(startPart, startDot);
          ret.base = p.slice(startPart, end);
          ret.ext = p.slice(startDot, end);
        }
      }
      if (startPart > 0 && startPart !== rootEnd) ret.dir = p.slice(0, startPart - 1);
      else ret.dir = ret.root;
      return ret;
    },
    format(o) { validatePathObject(o); const dir = o.dir || o.root || ""; const base = o.base || ((o.name || "") + (o.ext ? (o.ext[0] === "." ? "" : ".") + o.ext : "")); if (!dir) return base; return dir === o.root ? dir + base : dir + "\\" + base; },
    relative(from, to) {
      validatePathStr(from, "from"); validatePathStr(to, "to");
      if (from === to) return "";
      const fromOrig = win32.resolve(from); const toOrig = win32.resolve(to);
      if (fromOrig === toOrig) return "";
      from = fromOrig.toLowerCase(); to = toOrig.toLowerCase();
      if (from === to) return "";
      let fromStart = 0; while (fromStart < from.length && from.charCodeAt(fromStart) === 92) fromStart++;
      let fromEnd = from.length; while (fromEnd - 1 > fromStart && from.charCodeAt(fromEnd - 1) === 92) fromEnd--;
      const fromLen = fromEnd - fromStart;
      let toStart = 0; while (toStart < to.length && to.charCodeAt(toStart) === 92) toStart++;
      let toEnd = to.length; while (toEnd - 1 > toStart && to.charCodeAt(toEnd - 1) === 92) toEnd--;
      const toLen = toEnd - toStart;
      const length = Math.min(fromLen, toLen); let lastCommonSep = -1; let i = 0;
      for (; i < length; i++) { const fromCode = from.charCodeAt(fromStart + i); if (fromCode !== to.charCodeAt(toStart + i)) break; else if (fromCode === 92) lastCommonSep = i; }
      if (i !== length) { if (lastCommonSep === -1) return toOrig; }
      else {
        if (toLen > length) { if (to.charCodeAt(toStart + i) === 92) return toOrig.slice(toStart + i + 1); if (i === 2) return toOrig.slice(toStart + i); }
        if (fromLen > length) { if (from.charCodeAt(fromStart + i) === 92) lastCommonSep = i; else if (i === 2) lastCommonSep = 3; }
        if (lastCommonSep === -1) lastCommonSep = 0;
      }
      let out = "";
      for (i = fromStart + lastCommonSep + 1; i <= fromEnd; ++i) { if (i === fromEnd || from.charCodeAt(i) === 92) out += out.length === 0 ? ".." : "\\.."; }
      toStart += lastCommonSep;
      if (out.length > 0) return out + toOrig.slice(toStart, toEnd);
      if (toOrig.charCodeAt(toStart) === 92) ++toStart;
      return toOrig.slice(toStart, toEnd);
    },
    toNamespacedPath(p) { if (typeof p !== "string" || p.length === 0) return p; const resolved = win32.resolve(p); if (resolved.length <= 2) return p; if (resolved.charCodeAt(0) === 92) { if (resolved.charCodeAt(1) === 92) { const c = resolved.charCodeAt(2); if (c !== 63 && c !== 46) return "\\\\?\\UNC\\" + resolved.slice(2); } } else if (isLetterW(resolved[0]) && resolved.charCodeAt(1) === 58 && resolved.charCodeAt(2) === 92) { return "\\\\?\\" + resolved; } return resolved; },
  };
  path.toNamespacedPath = (p) => p;
  path._makeLong = path.toNamespacedPath;
  win32._makeLong = win32.toNamespacedPath;
  // matchesGlob — reuse Bun.Glob (mbun.glob core). win32 normalizes \ → / on both
  // path and pattern before matching (node/bun behavior). Non-string throws TypeError.
  const matchesGlobImpl = (isWindows, p, pattern) => {
    if (typeof p !== "string") throw new TypeError("The \"path\" argument must be of type string. Received " + typeof p);
    if (typeof pattern !== "string") throw new TypeError("The \"pattern\" argument must be of type string. Received " + typeof pattern);
    if (isWindows) { p = p.replaceAll("\\", "/"); pattern = pattern.replaceAll("\\", "/"); }
    return new (G.Bun.Glob)(pattern).match(p);
  };
  path.matchesGlob = (p, pattern) => matchesGlobImpl(false, p, pattern);
  win32.matchesGlob = (p, pattern) => matchesGlobImpl(true, p, pattern);
  path.posix = path; path.win32 = win32; win32.posix = path; win32.win32 = win32;
  def(["path"], path);
  def(["path/posix"], path);
  def(["path/win32"], win32);

  // ---- assert ----
  function AErr(msg) { const e = new Error(msg); e.name = "AssertionError"; e.code = "ERR_ASSERTION"; return e; }
  function assert(v, msg) { if (!v) throw AErr(msg || "The expression evaluated to a falsy value"); }
  assert.ok = assert;
  assert.equal = (a, b, m) => { if (a != b) throw AErr(m); };
  assert.notEqual = (a, b, m) => { if (a == b) throw AErr(m); };
  assert.strictEqual = (a, b, m) => { if (!Object.is(a, b)) throw AErr(m); };
  assert.notStrictEqual = (a, b, m) => { if (Object.is(a, b)) throw AErr(m); };
  assert.deepEqual = (a, b, m) => { if (!deepEq(a, b, false)) throw AErr(m); };
  assert.notDeepEqual = (a, b, m) => { if (deepEq(a, b, false)) throw AErr(m); };
  assert.deepStrictEqual = (a, b, m) => { if (!deepEq(a, b, true)) throw AErr(m); };
  assert.notDeepStrictEqual = (a, b, m) => { if (deepEq(a, b, true)) throw AErr(m); };
  assert.throws = (fn, e, m) => { try { fn(); } catch (_) { return; } throw AErr(m || "Missing expected exception"); };
  assert.doesNotThrow = (fn) => { fn(); };
  // rejects / doesNotReject: ported from bun src/js/node/assert.ts
  // (waitForActual/expectsError/expectsNoError/expectedException). Enriched
  // AssertionError path via makeAErr is opt-in; other assert.* keep using AErr.
  const NO_EXC = Symbol("assert.noException");
  const isRe = (v) => v instanceof RegExp;
  const isErrCtor = (fn) => { try { return Error.isPrototypeOf(fn); } catch (_) { return false; } };
  const isPromiseLike = (o) => (o instanceof Promise) || (o !== null && typeof o === "object" && typeof o.then === "function" && typeof o.catch === "function");
  function insp(v) {
    if (typeof v === "string") return "'" + v + "'";
    if (typeof v === "bigint") return String(v) + "n";
    if (typeof v === "function") return "[Function" + (v.name ? ": " + v.name : " (anonymous)") + "]";
    if (v === null) return "null";
    if (typeof v === "object") { try { return JSON.stringify(v); } catch (_) { return String(v); } }
    return String(v);
  }
  function makeAErr(f) {
    const AE = assert.AssertionError;
    const e = (typeof AE === "function")
      ? new AE({ message: f.message, actual: f.actual, expected: f.expected, operator: f.operator, generatedMessage: !!f.generatedMessage })
      : (() => { const x = new Error(f.message); x.name = "AssertionError"; x.actual = f.actual; x.expected = f.expected; x.operator = f.operator; x.generatedMessage = !!f.generatedMessage; return x; })();
    e.code = f.code || "ERR_ASSERTION";
    return e;
  }
  function argTypeErr(name, expectedStr, value) {
    let recv;
    if (value === null) recv = "null";
    else if (typeof value === "object") { const n = value.constructor && value.constructor.name; recv = n ? "an instance of " + n : "type object"; }
    else recv = "type " + typeof value;
    const e = new TypeError('The "' + name + '" argument must be ' + expectedStr + ". Received " + recv);
    e.code = "ERR_INVALID_ARG_TYPE";
    return e;
  }
  async function waitForActual(promiseFn) {
    let resultPromise;
    if (typeof promiseFn === "function") {
      resultPromise = promiseFn();
      if (!isPromiseLike(resultPromise)) { const e = new TypeError('Expected instance of Promise to be returned from the "promiseFn" function but got ' + insp(resultPromise) + "."); e.code = "ERR_INVALID_RETURN_VALUE"; throw e; }
    } else if (isPromiseLike(promiseFn)) {
      resultPromise = promiseFn;
    } else {
      throw argTypeErr("promiseFn", "of type function or an instance of Promise", promiseFn);
    }
    try { await resultPromise; } catch (e) { return e; }
    return NO_EXC;
  }
  function compareExceptionKey(actual, expected, key, message, opName) {
    if (!(key in actual) || !deepEq(actual[key], expected[key], true)) {
      throw makeAErr({ actual, expected, message, operator: opName, generatedMessage: !message });
    }
  }
  function expectedException(actual, expected, message, opName) {
    let generatedMessage = false, throwError = false;
    if (typeof expected !== "function") {
      if (isRe(expected)) {
        const str = String(actual);
        if (expected.test(str)) return;
        if (!message) { generatedMessage = true; message = "The input did not match the regular expression " + String(expected) + ". Input:\n\n'" + str + "'\n"; }
        throwError = true;
      } else if (typeof actual !== "object" || actual === null) {
        throw makeAErr({ actual, expected, message, operator: opName });
      } else {
        const keys = Object.keys(expected);
        if (expected instanceof Error) keys.push("name", "message");
        else if (keys.length === 0) { const er = new TypeError('The "error" argument must not be an empty object.'); er.code = "ERR_INVALID_ARG_VALUE"; throw er; }
        for (const key of keys) {
          if (typeof actual[key] === "string" && isRe(expected[key]) && expected[key].test(actual[key])) continue;
          compareExceptionKey(actual, expected, key, message, opName);
        }
        return;
      }
    } else if (expected.prototype !== undefined && actual instanceof expected) {
      return;
    } else if (isErrCtor(expected)) {
      if (!message) {
        generatedMessage = true;
        message = 'The error is expected to be an instance of "' + expected.name + '". Received ';
        if (actual instanceof Error) {
          const nm = (actual.constructor && actual.constructor.name) || actual.name;
          message += expected.name === nm ? "an error with identical name but a different prototype." : '"' + nm + '"';
          if (actual.message) message += "\n\nError message:\n\n" + actual.message;
        } else message += '"' + insp(actual) + '"';
      }
      throwError = true;
    } else {
      const res = expected(actual);
      if (res !== true) {
        if (!message) {
          generatedMessage = true;
          const name = expected.name ? '"' + expected.name + '" ' : "";
          message = "The " + name + 'validation function is expected to return "true". Received ' + insp(res);
          if (actual instanceof Error) message += "\n\nCaught error:\n\n" + actual;
        }
        throwError = true;
      }
    }
    if (throwError) throw makeAErr({ actual, expected, message, operator: opName, generatedMessage });
  }
  function hasMatchingError(actual, expected) {
    if (typeof expected !== "function") {
      if (isRe(expected)) return expected.test(String(actual));
      throw argTypeErr("expected", "one of type Function or RegExp", expected);
    }
    if (expected.prototype !== undefined && actual instanceof expected) return true;
    if (isErrCtor(expected)) return false;
    return expected(actual) === true;
  }
  function expectsError(opName, actual, error, message) {
    if (typeof error === "string") { message = error; error = undefined; }
    else if (error != null && typeof error !== "object" && typeof error !== "function") throw argTypeErr("error", "one of type Object, Error, Function, or RegExp", error);
    if (actual === NO_EXC) {
      let details = "";
      if (error && error.name) details += " (" + error.name + ")";
      details += message ? ": " + message : ".";
      const fnType = opName === "rejects" ? "rejection" : "exception";
      throw makeAErr({ actual: undefined, expected: error, operator: opName, message: "Missing expected " + fnType + details, generatedMessage: true });
    }
    if (!error) return;
    expectedException(actual, error, message, opName);
  }
  function expectsNoError(opName, actual, error, message) {
    if (actual === NO_EXC) return;
    if (typeof error === "string") { message = error; error = undefined; }
    if (!error || hasMatchingError(actual, error)) {
      const details = message ? ": " + message : ".";
      const fnType = opName === "doesNotReject" ? "rejection" : "exception";
      throw makeAErr({ actual, expected: error, operator: opName, message: "Got unwanted " + fnType + details + '\nActual message: "' + (actual && actual.message) + '"', generatedMessage: true });
    }
    throw actual;
  }
  assert.rejects = async function rejects(block, error, message) { return expectsError("rejects", await waitForActual(block), error, message); };
  assert.doesNotReject = async function doesNotReject(fn, error, message) { return expectsNoError("doesNotReject", await waitForActual(fn), error, message); };
  assert.fail = (m) => { throw AErr(m || "Failed"); };
  assert.ifError = (v) => { if (v) throw v; };
  function assertRegExpMatch(s, re, m, wantMatch) {
    if (!(re instanceof RegExp)) { const e = new TypeError(`The "regexp" argument must be of type RegExp. Received ${typeof re}`); e.code = "ERR_INVALID_ARG_TYPE"; throw e; }
    if (typeof s !== "string") { const e = new TypeError(`The "string" argument must be of type string. Received type ${typeof s}`); e.code = "ERR_INVALID_ARG_TYPE"; throw e; }
    if (re.test(s) !== wantMatch) throw AErr(m);
  }
  assert.match = (s, re, m) => assertRegExpMatch(s, re, m, true);
  assert.doesNotMatch = (s, re, m) => assertRegExpMatch(s, re, m, false);
  // partialDeepStrictEqual: `actual` must contain (deeply) every property/element
  // of `expected`; extra properties in actual are ignored.
  assert.partialDeepStrictEqual = (actual, expected, m) => {
    // `seen` memoizes (a -> Set<e>) pairs currently on the recursion stack so a
    // circular actual/expected (e.g. self-referential Maps) short-circuits to a
    // match instead of overflowing the stack. ref node assert partialDeepStrictEqual.
    const partial = (a, e, seen) => {
      if (e === a) return true;
      if (typeof e !== "object" || e === null) return Object.is(a, e);
      if (typeof a !== "object" || a === null) return false;
      let es = seen.get(a);
      if (es) { if (es.has(e)) return true; } else { es = new Set(); seen.set(a, es); }
      es.add(e);
      if (Array.isArray(e)) { if (!Array.isArray(a)) return false; const used = new Array(a.length).fill(false); for (const ev of e) { let ok = false; for (let j = 0; j < a.length; j++) { if (!used[j] && partial(a[j], ev, seen)) { used[j] = true; ok = true; break; } } if (!ok) return false; } return true; }
      if (e instanceof Map) { if (!(a instanceof Map)) return false; for (const [k, v] of e) { if (!a.has(k) || !partial(a.get(k), v, seen)) return false; } return true; }
      if (e instanceof Set) { if (!(a instanceof Set)) return false; for (const v of e) { let ok = false; for (const av of a) if (partial(av, v, seen)) { ok = true; break; } if (!ok) return false; } return true; }
      for (const k of Object.keys(e)) { if (!(k in a) || !partial(a[k], e[k], seen)) return false; }
      return true;
    };
    if (!partial(actual, expected, new WeakMap())) throw AErr(m || "Expected actual to partially match expected");
  };
  assert.strict = assert;
  def(["assert"], assert);

  // ---- util (subset) ----
  // node-like value inspection (unquoted keys, 'single-quoted' strings, class
  // names, Map/Set/TypedArray/Buffer/Error/Date/RegExp, circular refs, depth cap).
  const kInspectCustom = Symbol.for("nodejs.util.inspect.custom");
  function inspectValue(v, opts, seen, depth) {
    opts = opts || {}; seen = seen || new Set(); depth = depth || 0;
    const maxDepth = opts.depth === null ? Infinity : (typeof opts.depth === "number" ? opts.depth : 2);
    // bun:true selects Bun.inspect's layout (double-quoted strings, always
    // multi-line non-empty objects/maps/sets with trailing commas, bun function/
    // class tags). Unset → node util.inspect layout (byte-identical to before).
    const bun = opts.__bunStyle === true;
    // colors:true wraps primitives in node's util.inspect.styles palette (ANSI
    // SGR pairs). Off by default (colors undefined) → col() is a passthrough, so
    // non-color callers are byte-identical. ref node lib/internal/util/inspect.js
    // styles/colors.
    const col = opts.colors
      ? (o, c, s) => "\x1b[" + o + "m" + s + "\x1b[" + c + "m"
      : (o, c, s) => s;
    if (v === null) return col(1, 22, "null");
    const t = typeof v;
    if (t === "undefined") return col(90, 39, "undefined");
    if (t === "number") return col(33, 39, Object.is(v, -0) ? "-0" : String(v));
    if (t === "bigint") return col(33, 39, String(v) + "n");
    if (t === "boolean") return col(33, 39, String(v));
    if (t === "symbol") return col(32, 39, v.toString());
    if (t === "string") return col(32, 39, bun ? JSON.stringify(v) : "'" + v.replace(/\\/g, "\\\\").replace(/'/g, "\\'").replace(/\n/g, "\\n") + "'");
    if (t === "function") {
      const n = v.name;
      if (bun) {
        const s = Function.prototype.toString.call(v);
        if (s.startsWith("class") || /^class[\s{]/.test(s)) {
          const base = (s.match(/^class\s+(?:[A-Za-z0-9_$]+\s+)?extends\s+([A-Za-z0-9_$.]+)/) || [])[1];
          return "[class " + (n || "(anonymous)") + (base ? " extends " + base : "") + "]";
        }
        const cn = v.constructor && v.constructor.name;
        const kind = (cn === "AsyncFunction" || cn === "GeneratorFunction" || cn === "AsyncGeneratorFunction") ? cn : "Function";
        return n ? "[" + kind + ": " + n + "]" : "[" + kind + "]";
      }
      const tag = v.toString().startsWith("class") ? "class" : "Function"; return n ? "[" + tag + ": " + n + "]" : "[" + tag + " (anonymous)]";
    }
    if (seen.has(v)) return "[Circular *1]";
    // nodejs.util.inspect.custom dispatch: an object exposing a callable custom
    // symbol formats itself. Passed (depth, options{stylize,depth}, inspect).
    // ref node lib/internal/util/inspect.js formatValue custom-inspect branch.
    {
      const fn = v[kInspectCustom];
      if (typeof fn === "function") {
        const styleMap = { special: [36, 39], number: [33, 39], bigint: [33, 39], boolean: [33, 39], undefined: [90, 39], null: [1, 22], string: [32, 39], symbol: [32, 39], date: [35, 39], regexp: [31, 39], module: [4, 24] };
        const stylize = opts.colors
          ? (s, st) => { const c = styleMap[st]; return c ? "\x1b[" + c[0] + "m" + s + "\x1b[" + c[1] + "m" : s; }
          : (s) => s;
        const cOpts = Object.assign({}, opts, { stylize, depth: opts.depth });
        let r;
        try { r = fn.call(v, maxDepth - depth, cOpts, util.inspect); }
        catch (e) { const w = new Error("inspect-rethrow"); w.__inspectRethrow = true; w.__inspectOriginal = e; throw w; }
        if (typeof r === "string") return r;
        if (r === v) return "[" + ((v.constructor && v.constructor.name) || "Object") + "]";
        return inspectValue(r, opts, seen, depth);
      }
    }
    if (v instanceof Date) return isNaN(v.getTime()) ? "Invalid Date" : v.toISOString();
    if (v instanceof RegExp) return v.toString();
    if (v instanceof Error) {
      // bun/node error inspect starts with the "Name: message" header; mbun's
      // JSC-native stacks use `fn@source` frames without it, dropping the message
      // (which is where e.g. ENOENT/path live). Prepend it when absent.
      const name = v.name || "Error";
      // `stack` can be an own accessor that throws (userland Object.defineProperty).
      // Inspecting a value must never propagate that: node/bun both fall back to
      // the header instead of letting console.log blow the process up.
      // ref: regression circular-error-stack-edge-cases.
      let st;
      try { st = v.stack; } catch (e) { st = undefined; }
      let head;
      if (typeof st === "string" && st.length)
        head = st.startsWith(name) ? st : ((v.message ? name + ": " + v.message : name) + "\n" + st);
      else head = "[" + name + ": " + v.message + "]";
      // Own enumerable extras ride after the stack, as node/bun print them
      // (`Error: x\n  at …\n{\n  code: "E1",\n}`). A throwing getter is skipped,
      // not rethrown.
      const extraIndent = "  ".repeat(depth + 1);
      const extras = [];
      seen.add(v);
      try {
        for (const k of Object.keys(v)) {
          if (k === "message" || k === "stack") continue;
          let s;
          try { s = inspectValue(v[k], opts, seen, depth + 1); } catch (e) { continue; }
          extras.push(extraIndent +
                      (/^[A-Za-z_$][A-Za-z0-9_$]*$/.test(k) ? k : JSON.stringify(k)) + ": " + s + ",");
        }
      } finally { seen.delete(v); }
      if (extras.length) head += " {\n" + extras.join("\n") + "\n" + "  ".repeat(depth) + "}";
      return head;
    }
    if (G.Buffer && G.Buffer.isBuffer && G.Buffer.isBuffer(v)) return "<Buffer " + Array.from(v).map((x) => x.toString(16).padStart(2, "0")).join(" ") + ">";
    if (bun) {
      if (v instanceof Number) return "[Number: " + Number(v) + "]";
      if (v instanceof Boolean) return "[Boolean: " + Boolean(v) + "]";
      if (v instanceof String) return JSON.stringify(String(v));
    }
    if (depth > maxDepth) return Array.isArray(v) ? "[Array]" : (bun ? "[Object ...]" : "[Object]");
    seen.add(v);
    let result;
    // bun layout helpers: keys are bare identifiers else JSON-quoted; non-empty
    // objects/maps/sets always break across lines with 2-space/level indent and a
    // trailing comma per entry. ref bun ConsoleObject.zig / fmt writeObject.
    const bunKey = (k) => /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(k) ? k : JSON.stringify(k);
    const inner = "  ".repeat(depth + 1), outer = "  ".repeat(depth);
    const bunBlock = (label, items) => items.length ? label + "{\n" + items.map((it) => inner + it + ",").join("\n") + "\n" + outer + "}" : label + "{}";
    if (Array.isArray(v)) {
      const items = v.map((x) => inspectValue(x, opts, seen, depth + 1));
      if (!items.length) result = "[]";
      else if (bun) {
        const oneLine = "[ " + items.join(", ") + " ]";
        const complex = v.some((x) => x !== null && typeof x === "object" && !Array.isArray(x));
        const hasNL = items.some((s) => s.indexOf("\n") >= 0);
        result = (!complex && !hasNL && oneLine.length <= 72) ? oneLine : "[\n" + inner + items.join(", ") + "\n" + outer + "]";
      } else result = "[ " + items.join(", ") + " ]";
    }
    else if (v instanceof Map) {
      const items = []; for (const [k, val] of v) items.push(inspectValue(k, opts, seen, depth + 1) + (bun ? ": " : " => ") + inspectValue(val, opts, seen, depth + 1));
      if (bun) result = bunBlock(v.size ? "Map(" + v.size + ") " : "Map ", items);
      else result = "Map(" + v.size + ") {" + (items.length ? " " + items.join(", ") + " " : "") + "}";
    }
    else if (v instanceof Set) {
      const items = []; for (const x of v) items.push(inspectValue(x, opts, seen, depth + 1));
      if (bun) result = bunBlock(v.size ? "Set(" + v.size + ") " : "Set ", items);
      else result = "Set(" + v.size + ") {" + (items.length ? " " + items.join(", ") + " " : "") + "}";
    }
    else if (ArrayBuffer.isView(v) && !(v instanceof DataView)) { const nm = v.constructor ? v.constructor.name : "TypedArray"; const items = Array.from(v).map(String); result = nm + "(" + v.length + ") [" + (items.length ? " " + items.join(", ") + " " : "") + "]"; }
    else {
      const keys = Object.keys(v); const cn = v.constructor && v.constructor.name; const ctor = (cn && cn !== "Object") ? cn + " " : (Object.getPrototypeOf(v) === null ? "[Object: null prototype] " : "");
      // Enumerable symbol-keyed own props render after string keys: bun as
      // `[Symbol(desc)]: v`, node as `Symbol(desc): v`. ref util.inspect.
      const syms = Object.getOwnPropertySymbols(v).filter((s) => { const d = Object.getOwnPropertyDescriptor(v, s); return d && d.enumerable; });
      const descVal = (d, key) => (d && (d.get || d.set)) ? (d.get && d.set ? "[Getter/Setter]" : d.get ? "[Getter]" : "[Setter]") : inspectValue(v[key], opts, seen, depth + 1);
      if (bun) {
        const items = keys.map((k) => bunKey(k) + ": " + descVal(Object.getOwnPropertyDescriptor(v, k), k));
        for (const s of syms) items.push("[" + s.toString() + "]: " + descVal(Object.getOwnPropertyDescriptor(v, s), s));
        result = bunBlock(ctor, items);
      } else {
        const items = keys.map((k) => { const kk = /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(k) ? k : "'" + k + "'"; return kk + ": " + inspectValue(v[k], opts, seen, depth + 1); });
        for (const s of syms) items.push(s.toString() + ": " + inspectValue(v[s], opts, seen, depth + 1));
        result = ctor + (items.length ? "{ " + items.join(", ") + " }" : "{}");
      }
    }
    seen.delete(v);
    return result;
  }
  const util = {
    inspect(o, opts) { try { return inspectValue(o, opts, null, 0); } catch (e) { if (e && e.__inspectRethrow) throw e.__inspectOriginal; return String(o); } },
    format(f, ...a) {
      const fmtArg = (x) => typeof x === "string" ? x : util.inspect(x);
      if (typeof f !== "string") return [f, ...a].map(fmtArg).join(" ");
      let i = 0;
      let s = f.replace(/%[sdifjoOc%]/g, (m) => { if (m === "%%") return "%"; if (i >= a.length) return m;
        const v = a[i++];
        if (m === "%d" || m === "%i") return typeof v === "bigint" ? String(v) + "n" : String(Math.trunc(Number(v)));
        if (m === "%f") return String(parseFloat(v));
        if (m === "%j") { try { return JSON.stringify(v); } catch (e) { return "[Circular]"; } }
        if (m === "%c") return "";
        if (m === "%s") return typeof v === "string" ? v : (typeof v === "bigint" ? String(v) + "n" : (v !== null && typeof v === "object" ? util.inspect(v, { depth: 0 }) : String(v)));
        return util.inspect(v); });
      for (; i < a.length; i++) s += " " + fmtArg(a[i]);
      return s;
    },
    // formatWithOptions(inspectOptions, f, ...a): like format() but threads the
    // caller's inspect options (colors/depth/...) into every %o/%O/%s-object and
    // trailing-arg inspection. ref: node lib/internal/util/inspect.js formatWithOptions.
    formatWithOptions(inspectOptions, f, ...a) {
      const io = (inspectOptions && typeof inspectOptions === "object") ? inspectOptions : {};
      const insp = (x, extra) => util.inspect(x, extra ? Object.assign({}, io, extra) : io);
      const fmtArg = (x) => typeof x === "string" ? x : insp(x);
      if (typeof f !== "string") return [f, ...a].map(fmtArg).join(" ");
      let i = 0;
      let s = f.replace(/%[sdifjoOc%]/g, (m) => { if (m === "%%") return "%"; if (i >= a.length) return m;
        const v = a[i++];
        if (m === "%d" || m === "%i") return typeof v === "bigint" ? String(v) + "n" : String(Math.trunc(Number(v)));
        if (m === "%f") return String(parseFloat(v));
        if (m === "%j") { try { return JSON.stringify(v); } catch (e) { return "[Circular]"; } }
        if (m === "%c") return "";
        if (m === "%s") return typeof v === "string" ? v : (typeof v === "bigint" ? String(v) + "n" : (v !== null && typeof v === "object" ? insp(v, { depth: 0 }) : String(v)));
        if (m === "%o") return insp(v, { showHidden: true, showProxy: true, depth: 4 });
        if (m === "%O") return insp(v);
        return insp(v); });
      for (; i < a.length; i++) s += " " + fmtArg(a[i]);
      return s;
    },
    isDeepStrictEqual(a, b) { return deepEq(a, b, true); },
    promisify(fn) {
      // node ERR_INVALID_ARG_TYPE "Received …" suffix formatting.
      const recv = (v) => {
        if (v === null || v === undefined) return " Received " + v;
        const t = typeof v;
        if (t === "function") return v.name ? " Received function " + v.name : " Received an instance of Function";
        if (t === "object") return (v.constructor && v.constructor.name) ? " Received an instance of " + v.constructor.name : " Received " + util.inspect(v, { depth: -1 });
        let ins = util.inspect(v, { colors: false });
        if (ins.length > 28) ins = ins.slice(0, 25) + "...";
        return " Received type " + t + " (" + ins + ")";
      };
      if (typeof fn !== "function") { const e = new TypeError('The "original" argument must be of type function.' + recv(fn)); e.code = "ERR_INVALID_ARG_TYPE"; throw e; }
      const kCustom = Symbol.for("nodejs.util.promisify.custom");
      if (fn[kCustom] !== undefined && fn[kCustom] !== null) {
        const c = fn[kCustom];
        if (typeof c !== "function") { const e = new TypeError('The "util.promisify.custom" property must be of type function. Received ' + typeof c); e.code = "ERR_INVALID_ARG_TYPE"; throw e; }
        Object.defineProperty(c, kCustom, { value: c, enumerable: false, writable: false, configurable: true });
        return c;
      }
      const p = function (...a) { return new Promise((res, rej) => { fn.call(this, ...a, (e, v) => (e ? rej(e) : res(v))); }); };
      Object.defineProperty(p, kCustom, { value: p, enumerable: false, writable: false, configurable: true });
      return p;
    },
    callbackify(fn) { return function (...a) { const cb = a.pop(); const self = this; fn.apply(this, a).then((v) => cb.call(self, null, v), (e) => cb.call(self, e)); }; },
    inherits(ctor, sup) { ctor.super_ = sup; Object.setPrototypeOf(ctor.prototype, sup.prototype); },
    deprecate(fn) { return fn; },
    getSystemErrorName(errno) { const m = { "-1": "EPERM", "-2": "ENOENT", "-3": "ESRCH", "-4": "EINTR", "-5": "EIO", "-9": "EBADF", "-11": "EAGAIN", "-12": "ENOMEM", "-13": "EACCES", "-14": "EFAULT", "-16": "EBUSY", "-17": "EEXIST", "-20": "ENOTDIR", "-21": "EISDIR", "-22": "EINVAL", "-23": "ENFILE", "-24": "EMFILE", "-28": "ENOSPC", "-32": "EPIPE", "-36": "ENAMETOOLONG", "-39": "ENOTEMPTY", "-40": "ELOOP", "-95": "ENOTSUP", "-98": "EADDRINUSE", "-99": "EADDRNOTAVAIL", "-100": "ENETDOWN", "-101": "ENETUNREACH", "-103": "ECONNABORTED", "-104": "ECONNRESET", "-107": "ENOTCONN", "-110": "ETIMEDOUT", "-111": "ECONNREFUSED", "-113": "EHOSTUNREACH", "-125": "ECANCELED" }; return m[String(errno)] || ("Unknown system error " + errno); },
    getSystemErrorMap() { return new Map(); },
    toUSVString(s) { return String(s); },
    stripVTControlCharacters(s) { return String(s).replace(/\x1b\[[0-9;]*m/g, ""); },
    debuglog() { return () => {}; }, debug() { return () => {}; },
    _extend(a, b) { return Object.assign(a, b); },
    aborted: () => Promise.resolve(),
    parseArgs(config) {
      // Faithful port of node lib/internal/util/parse_args + bun src/runtime/node/util/parse_args.rs.
      const hasOwn = (o, k) => o != null && Object.prototype.hasOwnProperty.call(o, k);
      const objGetOwn = (o, k) => (hasOwn(o, k) ? o[k] : undefined);
      const received = (v) => {
        if (v === null) return "null";
        if (v === undefined) return "undefined";
        const t = typeof v;
        if (t === "function") return v.name ? "function " + v.name : "an instance of Function";
        if (t === "object") return (v.constructor && v.constructor.name) ? "an instance of " + v.constructor.name : "[object]";
        if (t === "string") { let s = JSON.stringify(v); if (s.length > 28) s = s.slice(0, 25) + "..."; return "type string (" + s + ")"; }
        if (t === "bigint") return "type bigint (" + String(v) + "n)";
        return "type " + t + " (" + String(v) + ")";
      };
      const kTypes = ["string", "function", "number", "object", "Function", "Object", "boolean", "bigint", "symbol", "undefined"];
      const errArgType = (name, expected, value) => {
        let head;
        if (name.endsWith(" argument")) head = "The " + name + " ";
        else head = 'The "' + name + '" ' + (name.includes(".") ? "property" : "argument") + " ";
        let kind;
        if (kTypes.includes(expected)) kind = "of type " + expected.toLowerCase();
        else if (/^[A-Z]/.test(expected)) kind = "an instance of " + expected;
        else kind = "of type " + expected;
        const e = new TypeError(head + "must be " + kind + ". Received " + received(value));
        e.code = "ERR_INVALID_ARG_TYPE";
        return e;
      };
      const errArgValue = (name, value, reason) => {
        const e = new TypeError("The property '" + name + "' " + reason + ". Received " + received(value));
        e.code = "ERR_INVALID_ARG_VALUE";
        return e;
      };
      const parseErr = (code, message) => { const e = new TypeError(message); e.code = code; return e; };
      const validateBoolean = (v, name) => { if (typeof v !== "boolean") throw errArgType(name, "boolean", v); return v; };
      const validateString = (v, name) => { if (typeof v !== "string") throw errArgType(name, "string", v); return v; };
      const validateArray = (v, name) => { if (!Array.isArray(v)) throw errArgType(name, "Array", v); return v; };
      const validateObject = (v, name) => { if (v === null || typeof v !== "object" || Array.isArray(v)) throw errArgType(name, "Object", v); return v; };
      const validateUnion = (v, name, union) => { if (!union.includes(v)) throw errArgType(name, union.map(u => "'" + u + "'").join("|"), v); return v; };
      const validateStringArray = (v, name) => { validateArray(v, name); for (let i = 0; i < v.length; i++) validateString(v[i], `${name}[${i}]`); return v; };
      const validateBooleanArray = (v, name) => { validateArray(v, name); for (let i = 0; i < v.length; i++) validateBoolean(v[i], `${name}[${i}]`); return v; };
      const isOptionLikeValue = (v) => typeof v === "string" && v.length > 1 && v[0] === "-";

      const getMainArgs = () => {
        const proc = globalThis.process;
        if (!proc) return [];
        const argv = proc.argv, execArgv = proc.execArgv;
        if (Array.isArray(argv) && Array.isArray(execArgv)) {
          for (const a of execArgv) {
            if (a === "-e" || a === "--eval" || a === "-p" || a === "--print") return argv.slice(1);
          }
          return argv.slice(2);
        }
        return [];
      };

      // Phase 0: resolve + validate config. Node coalesces each top-level flag with
      // `?? default` so an explicit `null`/`undefined` behaves like an absent key.
      const cfg = config == null ? {} : config;
      const args = objGetOwn(cfg, "args") ?? getMainArgs();
      const strict = objGetOwn(cfg, "strict") ?? true;
      const allowPositionals = objGetOwn(cfg, "allowPositionals") ?? !strict;
      const allowNegative = objGetOwn(cfg, "allowNegative") ?? false;
      const returnTokens = objGetOwn(cfg, "tokens") ?? false;
      const options = objGetOwn(cfg, "options") ?? { __proto__: null };

      validateArray(args, "args");
      validateBoolean(strict, "strict");
      validateObject(options, "options");
      validateBoolean(allowPositionals, "allowPositionals");
      validateBoolean(allowNegative, "allowNegative");
      validateBoolean(returnTokens, "tokens");

      const optKeys = Object.keys(options);
      for (const longOption of optKeys) {
        const optionConfig = options[longOption];
        validateObject(optionConfig, `options.${longOption}`);
        const optionType = objGetOwn(optionConfig, "type");
        validateUnion(optionType, `options.${longOption}.type`, ["string", "boolean"]);
        if (hasOwn(optionConfig, "short")) {
          const shortOption = optionConfig.short;
          validateString(shortOption, `options.${longOption}.short`);
          if (shortOption.length !== 1) throw errArgValue(`options.${longOption}.short`, shortOption, "must be a single character");
        }
        const multipleOption = objGetOwn(optionConfig, "multiple");
        if (hasOwn(optionConfig, "multiple")) validateBoolean(multipleOption, `options.${longOption}.multiple`);
        const defaultValue = objGetOwn(optionConfig, "default");
        if (defaultValue !== undefined) {
          let validator;
          if (optionType === "string") validator = multipleOption ? validateStringArray : validateString;
          else validator = multipleOption ? validateBooleanArray : validateBoolean;
          validator(defaultValue, `options.${longOption}.default`);
        }
      }

      // Option lookup helpers (over the `options` object).
      const findLongByShort = (shortName) => {
        let fallback;
        for (const long of optKeys) {
          const oc = options[long];
          if (oc && objGetOwn(oc, "short") === shortName) return long;
          if (long.length === 1 && long === shortName) fallback = long;
        }
        return fallback;
      };
      const optType = (long) => { const oc = objGetOwn(options, long); return oc == null ? undefined : objGetOwn(oc, "type"); };
      const classify = (arg) => {
        const len = arg.length;
        if (len === 2) {
          if (arg[0] === "-") return arg[1] === "-" ? "terminator" : "loneShort";
        } else if (len > 2) {
          if (arg.startsWith("--")) return arg.indexOf("=") >= 3 ? "longValue" : "loneLong";
          if (arg[0] === "-") {
            const long = findLongByShort(arg[1]);
            if (long !== undefined && optType(long) === "string") return "shortValue";
            return "shortGroup";
          }
        }
        return "positional";
      };

      const values = { __proto__: null };
      const positionals = [];
      const resultTokens = returnTokens ? [] : undefined;

      const checkOptionUsage = (token) => {
        const oc = objGetOwn(options, token.name);
        if (oc !== undefined) {
          const type = objGetOwn(oc, "type");
          const short = objGetOwn(oc, "short");
          const label = short ? `-${short}, --${token.name}` : `--${token.name}`;
          if (type === "string") {
            if (typeof token.value !== "string") {
              if (token.negative) throw parseErr("ERR_PARSE_ARGS_UNKNOWN_OPTION", `Unknown option '${token.rawName}'`);
              throw parseErr("ERR_PARSE_ARGS_INVALID_OPTION_VALUE", `Option '${label} <value>' argument missing`);
            }
          } else if (token.value !== undefined) {
            throw parseErr("ERR_PARSE_ARGS_INVALID_OPTION_VALUE", `Option '${label}' does not take an argument`);
          }
        } else if (allowPositionals) {
          throw parseErr("ERR_PARSE_ARGS_UNKNOWN_OPTION", `Unknown option '${token.rawName}'. To specify a positional argument starting with a '-', place it at the end of the command after '--', as in '-- "${token.rawName}"`);
        } else {
          throw parseErr("ERR_PARSE_ARGS_UNKNOWN_OPTION", `Unknown option '${token.rawName}'`);
        }
      };
      const checkOptionLikeValue = (token) => {
        if (!token.inlineValue && isOptionLikeValue(token.value)) {
          const rawName = token.rawName;
          if (token.rawArg.startsWith("--")) {
            throw parseErr("ERR_PARSE_ARGS_INVALID_OPTION_VALUE", `Option '${rawName}' argument is ambiguous.\nDid you forget to specify the option argument for '${rawName}'?\nTo specify an option argument starting with a dash use '${rawName}=-XYZ'.`);
          }
          throw parseErr("ERR_PARSE_ARGS_INVALID_OPTION_VALUE", `Option '${rawName}' argument is ambiguous.\nDid you forget to specify the option argument for '${rawName}'?\nTo specify an option argument starting with a dash use '--${token.name}=-XYZ' or '${rawName}-XYZ'.`);
        }
      };
      const storeOption = (token) => {
        const name = token.name;
        if (name === "__proto__") return;
        const oc = objGetOwn(options, name);
        const multiple = oc != null && objGetOwn(oc, "multiple") === true;
        const newValue = token.value === undefined ? !token.negative : token.value;
        if (multiple) {
          if (hasOwn(values, name)) values[name].push(newValue);
          else values[name] = [newValue];
        } else {
          values[name] = newValue;
        }
      };
      const emitOption = (token) => {
        if (strict) { checkOptionUsage(token); checkOptionLikeValue(token); }
        storeOption(token);
        if (returnTokens) resultTokens.push({
          kind: "option", index: token.index, name: token.name, rawName: token.rawName,
          value: token.value, inlineValue: token.value === undefined ? undefined : token.inlineValue,
        });
      };
      const emitPositional = (index, value) => {
        if (!allowPositionals) throw parseErr("ERR_PARSE_ARGS_UNEXPECTED_POSITIONAL", `Unexpected argument '${value}'. This command does not take positional arguments`);
        positionals.push(value);
        if (returnTokens) resultTokens.push({ kind: "positional", index, value });
      };

      // Phase 1+2: tokenize args and process each token.
      const numArgs = args.length;
      let index = 0;
      while (index < numArgs) {
        const arg = args[index];
        switch (classify(arg)) {
          case "terminator": {
            if (returnTokens) resultTokens.push({ kind: "option-terminator", index });
            index++;
            while (index < numArgs) { emitPositional(index, args[index]); index++; }
            break;
          }
          case "loneShort": {
            const short = arg.slice(1, 2);
            const long = findLongByShort(short);
            const type = long !== undefined ? optType(long) : "boolean";
            let value, inlineValue = true;
            if (type === "string" && index + 1 < numArgs) { value = args[index + 1]; inlineValue = false; }
            emitOption({ index, value, inlineValue, name: long !== undefined ? long : short, rawArg: arg, rawName: arg, negative: false });
            if (!inlineValue) index++;
            break;
          }
          case "shortGroup": {
            const originalIdx = index;
            const argLen = arg.length;
            for (let g = 1; g < argLen; g++) {
              const short = arg.slice(g, g + 1);
              const long = findLongByShort(short);
              const type = long !== undefined ? optType(long) : "boolean";
              if (type !== "string" || g === argLen - 1) {
                let value, inlineValue = true;
                if (type === "string" && index + 1 < numArgs) { value = args[index + 1]; inlineValue = false; }
                emitOption({ index: originalIdx, value, inlineValue, name: long !== undefined ? long : short, rawArg: arg, rawName: "-" + short, negative: false });
                if (!inlineValue) index++;
              } else {
                emitOption({ index: originalIdx, value: arg.slice(g + 1), inlineValue: true, name: long !== undefined ? long : short, rawArg: arg, rawName: "-" + short, negative: false });
                break;
              }
            }
            break;
          }
          case "shortValue": {
            const short = arg.slice(1, 2);
            const long = findLongByShort(short);
            emitOption({ index, value: arg.slice(2), inlineValue: true, name: long !== undefined ? long : short, rawArg: arg, rawName: arg.slice(0, 2), negative: false });
            break;
          }
          case "loneLong": {
            let longOption = arg.slice(2);
            let negative = false;
            if (allowNegative && longOption.startsWith("no-")) { longOption = longOption.slice(3); negative = true; }
            const known = objGetOwn(options, longOption) !== undefined;
            const type = known ? optType(longOption) : "boolean";
            let value, hasValue = false;
            if (type === "string" && index + 1 < numArgs && !negative) { value = args[index + 1]; hasValue = true; }
            emitOption({ index, value: hasValue ? value : undefined, inlineValue: !hasValue, name: longOption, rawArg: arg, rawName: arg, negative });
            if (hasValue) index++;
            break;
          }
          case "longValue": {
            const eq = arg.indexOf("=");
            emitOption({ index, value: arg.slice(eq + 1), inlineValue: true, name: arg.slice(2, eq), rawArg: arg, rawName: arg.slice(0, eq), negative: false });
            break;
          }
          default:
            emitPositional(index, arg);
            break;
        }
        index++;
      }

      // Phase 3: fill in default values for missing options.
      for (const longOption of optKeys) {
        if (longOption === "__proto__") continue;
        const def = objGetOwn(options[longOption], "default");
        if (def !== undefined && !hasOwn(values, longOption)) values[longOption] = def;
      }

      // Phase 4: build result.
      const result = { values, positionals };
      if (returnTokens) result.tokens = resultTokens;
      return result;
    },
    types: { isPromise: (v) => v instanceof Promise, isDate: (v) => v instanceof Date, isRegExp: (v) => v instanceof RegExp,
             isAsyncFunction: (v) => typeof v === "function" && v.constructor && v.constructor.name === "AsyncFunction" },
    TextEncoder: globalThis.TextEncoder, TextDecoder: globalThis.TextDecoder,
  };
  // util.MIMEType / MIMEParams (WHATWG): parse "type/subtype;p=v" strings.
  const MIME_TOKEN_RE = /^[!#$%&'*+\-.^_`|~A-Za-z0-9]+$/;
  const MIME_INVALID_VALUE_RE = /[^\t -~-ÿ]/;
  const mimeEncodeValue = (v) => { if (v.length === 0) return '""'; if (MIME_TOKEN_RE.test(v)) return v; return '"' + v.replace(/[\\"]/g, "\\$&") + '"'; };
  class MIMEParams {
    constructor() { this._m = new Map(); }
    get(k) { return this._m.has(k) ? this._m.get(k) : null; }
    set(k, v) { k = String(k); v = String(v); if (!MIME_TOKEN_RE.test(k)) throw new TypeError("The MIME syntax for a parameter name in " + k + " is invalid"); if (MIME_INVALID_VALUE_RE.test(v)) throw new TypeError("The MIME syntax for a parameter value in " + v + " is invalid"); this._m.set(k, v); }
    has(k) { return this._m.has(k); }
    delete(k) { this._m.delete(k); }
    entries() { return this._m.entries(); }
    keys() { return this._m.keys(); }
    values() { return this._m.values(); }
    [Symbol.iterator]() { return this._m.entries(); }
    toString() { return Array.from(this._m).map(([k, v]) => k + "=" + mimeEncodeValue(v)).join(";"); }
    toJSON() { return this.toString(); }
  }
  class MIMEType {
    constructor(input) {
      const s = String(input); const semi = s.indexOf(";");
      const essence = (semi < 0 ? s : s.slice(0, semi)).trim().toLowerCase();
      const slash = essence.indexOf("/");
      if (slash < 0) throw new TypeError("Invalid MIME type: " + input);
      this._type = essence.slice(0, slash); this._subtype = essence.slice(slash + 1);
      this.params = new MIMEParams();
      // WHATWG mimesniff §parse a MIME type (node internal/mime.js): quoted
      // values unescape \X, empty values are dropped, first name wins.
      if (semi >= 0) for (const part of s.slice(semi + 1).split(";")) {
        const eq = part.indexOf("="); if (eq < 0) continue;
        const name = part.slice(0, eq).trim().toLowerCase();
        let raw = part.slice(eq + 1).trim();
        let val;
        if (raw.startsWith('"')) {
          val = ""; let i = 1;
          for (; i < raw.length && raw[i] !== '"'; i++) { if (raw[i] === "\\" && i + 1 < raw.length) i++; val += raw[i]; }
        } else val = raw;
        if (!name || !MIME_TOKEN_RE.test(name) || val === "" || MIME_INVALID_VALUE_RE.test(val)) continue;
        if (!this.params._m.has(name)) this.params._m.set(name, val);
      }
    }
    get type() { return this._type; }
    set type(v) { v = String(v); if (!MIME_TOKEN_RE.test(v)) throw new TypeError("The MIME syntax for a type in " + v + " is invalid"); this._type = v.toLowerCase(); }
    get subtype() { return this._subtype; }
    set subtype(v) { v = String(v); if (!MIME_TOKEN_RE.test(v)) throw new TypeError("The MIME syntax for a subtype in " + v + " is invalid"); this._subtype = v.toLowerCase(); }
    get essence() { return this._type + "/" + this._subtype; }
    toString() { const p = this.params.toString(); return this.essence + (p ? ";" + p : ""); }
    toJSON() { return this.toString(); }
  }
  util.MIMEType = MIMEType; util.MIMEParams = MIMEParams;
  util.inspect.custom = kInspectCustom;
  G.MIMEType = MIMEType; G.MIMEParams = MIMEParams;
  // util.promisify.custom is the shared symbol Symbol.for("nodejs.util.promisify.custom").
  Object.defineProperty(util.promisify, "custom", { value: Symbol.for("nodejs.util.promisify.custom"), enumerable: false, writable: false, configurable: true });
  def(["util"], util);
  def(["util/types"], util.types);

  // ---- os (subset) ----
  const os = {
    EOL: "\n", platform: () => "linux", arch: () => "x64", type: () => "Linux", release: () => "",
    hostname: () => "localhost", tmpdir: () => "/tmp", homedir: () => "/root", endianness: () => "LE",
    cpus: () => [], totalmem: () => 0, freemem: () => 0, uptime: () => 0, loadavg: () => [0, 0, 0],
    networkInterfaces: () => ({}), userInfo: () => ({ username: "user", homedir: "/root", shell: "/bin/sh", uid: 1000, gid: 1000 }),
    devNull: "/dev/null",
    constants: {
      signals: { SIGHUP: 1, SIGINT: 2, SIGQUIT: 3, SIGILL: 4, SIGTRAP: 5, SIGABRT: 6, SIGIOT: 6, SIGBUS: 7, SIGFPE: 8, SIGKILL: 9, SIGUSR1: 10, SIGSEGV: 11, SIGUSR2: 12, SIGPIPE: 13, SIGALRM: 14, SIGTERM: 15, SIGCHLD: 17, SIGCONT: 18, SIGSTOP: 19, SIGTSTP: 20, SIGTTIN: 21, SIGTTOU: 22, SIGURG: 23, SIGXCPU: 24, SIGXFSZ: 25, SIGVTALRM: 26, SIGPROF: 27, SIGWINCH: 28, SIGIO: 29, SIGPOLL: 29, SIGPWR: 30, SIGSYS: 31 },
      errno: { EPERM: 1, ENOENT: 2, EINTR: 4, EIO: 5, EBADF: 9, EAGAIN: 11, ENOMEM: 12, EACCES: 13, EEXIST: 17, ENOTDIR: 20, EISDIR: 21, EINVAL: 22, ENFILE: 23, EMFILE: 24, EPIPE: 32, ENOTSUP: 95 },
      priority: { PRIORITY_LOW: 19, PRIORITY_BELOW_NORMAL: 10, PRIORITY_NORMAL: 0, PRIORITY_ABOVE_NORMAL: -7, PRIORITY_HIGH: -14, PRIORITY_HIGHEST: -20 },
    },
    getPriority: () => 0, setPriority: () => {}, availableParallelism: () => 1, machine: () => "x86_64",
    version: () => "", getuid: () => (G.process && G.process.getuid ? G.process.getuid() : 1000),
  };
  def(["os"], os);

  // ---- module (subset) ----
  const BUILTIN_LIST = ["_http_agent", "_http_client", "_http_common", "_http_incoming", "_http_outgoing", "_http_server", "_stream_duplex", "_stream_passthrough", "_stream_readable", "_stream_transform", "_stream_wrap", "_stream_writable", "_tls_common", "_tls_wrap", "assert", "assert/strict", "async_hooks", "buffer", "child_process", "cluster", "console", "constants", "crypto", "dgram", "diagnostics_channel", "dns", "dns/promises", "domain", "events", "fs", "fs/promises", "http", "http2", "https", "inspector", "inspector/promises", "module", "net", "os", "path", "path/posix", "path/win32", "perf_hooks", "process", "punycode", "querystring", "readline", "readline/promises", "repl", "stream", "stream/consumers", "stream/promises", "stream/web", "string_decoder", "sys", "timers", "timers/promises", "tls", "trace_events", "tty", "url", "util", "util/types", "v8", "vm", "wasi", "worker_threads", "zlib"];
  const nodeModule = {
    createRequire: () => globalThis.require,
    builtinModules: BUILTIN_LIST,
    isBuiltin: (m) => { const n = String(m).replace(/^node:/, ""); return BUILTIN_LIST.indexOf(n) !== -1; },
    _resolveFilename: (r) => r,
    _nodeModulePaths: (from) => { const parts = String(from).split("/"); const out = []; for (let i = parts.length; i > 0; i--) { if (parts[i - 1] === "node_modules") continue; out.push(parts.slice(0, i).join("/") + "/node_modules"); } return out; },
    _resolveLookupPaths: (r) => [],
    _cache: {},
    _extensions: { ".js": () => {}, ".json": () => {}, ".node": () => {} },
    globalPaths: [],
    wrap: (script) => "(function (exports, require, module, __filename, __dirname) { " + script + "\n});",
    wrapper: ["(function (exports, require, module, __filename, __dirname) { ", "\n});"],
    findSourceMap: () => undefined,
    SourceMap: class SourceMap { constructor(payload) { this.payload = payload; } findEntry() { return {}; } },
    syncBuiltinESMExports: () => {},
    runMain: () => {},
    Module: function Module(id) { this.id = id || ""; this.exports = {}; this.filename = null; this.loaded = false; this.children = []; this.paths = []; },
  };
  nodeModule.Module.createRequire = nodeModule.createRequire;
  nodeModule.Module.builtinModules = BUILTIN_LIST;
  nodeModule.Module.isBuiltin = nodeModule.isBuiltin;
  nodeModule.Module._nodeModulePaths = nodeModule._nodeModulePaths;
  nodeModule.Module.wrap = nodeModule.wrap;
  def(["module"], nodeModule);

  // ---- events (Node-compatible EventEmitter) ----
  // Ported from bun's src/js/node/events.ts + node-fallbacks/events.js, adapted
  // to plain JS (no JSC $-intrinsics). Prototype methods are enumerable so the
  // `Object.assign(obj, EventEmitter.prototype)` idiom copies them (Node compat).
  const EventEmitter = (function () {
    const kCapture = Symbol("kCapture");
    const kErrorMonitor = Symbol.for("events.errorMonitor");
    const kMaxEventTargetListeners = Symbol("events.maxEventTargetListeners");
    const kRejection = Symbol.for("nodejs.rejection");
    const captureRejectionSymbol = Symbol.for("nodejs.rejection");
    const kFirstEventParam = Symbol.for("nodejs.kFirstEventParam");
    let defaultMaxListeners = 10;

    const checkListener = (l) => { if (typeof l !== "function") throw new TypeError("The listener must be a function"); };
    // Node's NodeError bakes the code into toString(): "TypeError [ERR_x]: msg".
    // assert.throws(fn, /ERR_x/) matches on String(err), so it must appear there.
    const addCodeToName = (e, code) => { const base = e.name; Object.defineProperty(e, "toString", { value() { return `${base} [${code}]${this.message ? ": " + this.message : ""}`; }, configurable: true, writable: true }); return e; };
    const ERR_INVALID_ARG_TYPE = (name, type, value) => { const e = new TypeError(`The "${name}" argument must be of type ${type}. Received ${value}`); e.code = "ERR_INVALID_ARG_TYPE"; return addCodeToName(e, "ERR_INVALID_ARG_TYPE"); };
    const ERR_OUT_OF_RANGE = (name, range, value) => { const e = new RangeError(`The "${name}" argument is out of range. It must be ${range}. Received ${value}`); e.code = "ERR_OUT_OF_RANGE"; return addCodeToName(e, "ERR_OUT_OF_RANGE"); };
    const validateNumber = (value, name, min, max) => { if (typeof value !== "number") throw ERR_INVALID_ARG_TYPE(name, "number", value); if ((min != null && value < min) || (max != null && value > max) || ((min != null || max != null) && Number.isNaN(value))) throw ERR_OUT_OF_RANGE(name, `${min != null ? ">= " + min : ""}${min != null && max != null ? " && " : ""}${max != null ? "<= " + max : ""}`, value); };
    const validateInteger = (value, name, min) => { if (typeof value !== "number" || !Number.isInteger(value)) throw ERR_INVALID_ARG_TYPE(name, "integer", value); if (min != null && value < min) throw ERR_OUT_OF_RANGE(name, ">= " + min, value); };
    const validateObject = (value, name) => { if (value === null || typeof value !== "object") throw ERR_INVALID_ARG_TYPE(name, "Object", value); };
    const validateAbortSignal = (signal, name) => { if (signal !== undefined && (signal === null || typeof signal !== "object" || !("aborted" in signal))) throw ERR_INVALID_ARG_TYPE(name, "AbortSignal", signal); };
    const validateBoolean = (value, name) => { if (typeof value !== "boolean") throw ERR_INVALID_ARG_TYPE(name, "boolean", value); };
    class AbortError extends Error { constructor(message = "The operation was aborted.", options = undefined) { if (options !== undefined && typeof options !== "object") throw ERR_INVALID_ARG_TYPE("options", "Object", options); super(message, options); this.code = "ABORT_ERR"; this.name = "AbortError"; } }

    function EventEmitter(opts) {
      if (this._events === undefined || this._events === Object.getPrototypeOf(this)._events) {
        this._events = { __proto__: null };
        this._eventsCount = 0;
      }
      this._maxListeners ??= undefined;
      if ((this[kCapture] = opts?.captureRejections ? Boolean(opts.captureRejections) : EventEmitterPrototype[kCapture])) {
        this.emit = emitWithRejectionCapture;
      }
    }
    const EventEmitterPrototype = (EventEmitter.prototype = {});
    EventEmitterPrototype._events = undefined;
    EventEmitterPrototype._eventsCount = 0;
    EventEmitterPrototype._maxListeners = undefined;
    EventEmitterPrototype[kCapture] = false;
    EventEmitterPrototype.constructor = EventEmitter;

    EventEmitterPrototype.setMaxListeners = function setMaxListeners(n) { validateNumber(n, "setMaxListeners", 0); this._maxListeners = n; return this; };
    EventEmitterPrototype.getMaxListeners = function getMaxListeners() { return this._maxListeners ?? defaultMaxListeners; };

    function emitError(emitter, args) {
      const events = emitter._events;
      args[0] ??= new Error("Unhandled error.");
      if (!events) throw args[0];
      const errorMonitor = events[kErrorMonitor];
      if (errorMonitor) for (const handler of errorMonitor.slice()) handler.apply(emitter, args);
      const handlers = events.error;
      if (!handlers) throw args[0];
      for (const handler of handlers.slice()) handler.apply(emitter, args);
      return true;
    }
    function addCatch(emitter, promise, type, args) {
      promise.then(undefined, function (err) { queueMicrotask(() => emitUnhandledRejectionOrErr(emitter, err, type, args)); });
    }
    function emitUnhandledRejectionOrErr(emitter, err, type, args) {
      if (typeof emitter[kRejection] === "function") { emitter[kRejection](err, type, ...args); }
      else { try { emitter[kCapture] = false; emitter.emit("error", err); } finally { emitter[kCapture] = true; } }
    }
    const emitWithoutRejectionCapture = function emit(type, ...args) {
      if (type === "error") return emitError(this, args);
      const events = this._events;
      if (events === undefined) return false;
      const handlers = events[type];
      if (handlers === undefined) return false;
      const cloned = handlers.length > 1 ? handlers.slice() : handlers;
      for (let i = 0, { length } = cloned; i < length; i++) cloned[i].apply(this, args);
      return true;
    };
    const emitWithRejectionCapture = function emit(type, ...args) {
      if (type === "error") return emitError(this, args);
      const events = this._events;
      if (events === undefined) return false;
      const handlers = events[type];
      if (handlers === undefined) return false;
      const cloned = handlers.length > 1 ? handlers.slice() : handlers;
      for (let i = 0, { length } = cloned; i < length; i++) {
        const result = cloned[i].apply(this, args);
        if (result !== undefined && typeof result?.then === "function" && result.then === Promise.prototype.then) addCatch(this, result, type, args);
      }
      return true;
    };
    EventEmitterPrototype.emit = emitWithoutRejectionCapture;

    function overflowWarning(emitter, type, handlers) {
      handlers.warned = true;
      const warn = new Error(`Possible EventEmitter memory leak detected. ${handlers.length} ${String(type)} listeners added to [${emitter.constructor.name}]. Use emitter.setMaxListeners() to increase limit`);
      warn.name = "MaxListenersExceededWarning"; warn.emitter = emitter; warn.type = type; warn.count = handlers.length;
      (G.console && G.console.warn ? G.console.warn : (() => {}))(warn);
    }
    function insert(self, type, fn, prepend) {
      let events = self._events;
      if (!events) { events = self._events = { __proto__: null }; self._eventsCount = 0; }
      else if (events.newListener) self.emit("newListener", type, fn.listener ?? fn);
      const handlers = events[type];
      if (!handlers) { events[type] = [fn]; self._eventsCount++; }
      else {
        if (prepend) handlers.unshift(fn); else handlers.push(fn);
        const m = self._maxListeners ?? defaultMaxListeners;
        if (m > 0 && handlers.length > m && !handlers.warned) overflowWarning(self, type, handlers);
      }
      return self;
    }
    EventEmitterPrototype.addListener = function addListener(type, fn) { checkListener(fn); return insert(this, type, fn, false); };
    EventEmitterPrototype.on = EventEmitterPrototype.addListener;
    EventEmitterPrototype.prependListener = function prependListener(type, fn) { checkListener(fn); return insert(this, type, fn, true); };

    function onceWrapper(type, listener, ...args) { this.removeListener(type, listener); listener.apply(this, args); }
    EventEmitterPrototype.once = function once(type, fn) { checkListener(fn); const bound = onceWrapper.bind(this, type, fn); bound.listener = fn; this.addListener(type, bound); return this; };
    EventEmitterPrototype.prependOnceListener = function prependOnceListener(type, fn) { checkListener(fn); const bound = onceWrapper.bind(this, type, fn); bound.listener = fn; this.prependListener(type, bound); return this; };

    EventEmitterPrototype.removeListener = function removeListener(type, fn) {
      checkListener(fn);
      const events = this._events;
      if (!events) return this;
      const handlers = events[type];
      if (!handlers) return this;
      let position = -1, originalListener;
      for (let i = handlers.length - 1; i >= 0; i--) { if (handlers[i] === fn || handlers[i].listener === fn) { originalListener = handlers[i].listener; position = i; break; } }
      if (position < 0) return this;
      if (position === 0) handlers.shift(); else handlers.splice(position, 1);
      if (handlers.length === 0) { delete events[type]; this._eventsCount--; }
      if (events.removeListener !== undefined) this.emit("removeListener", type, originalListener || fn);
      return this;
    };
    EventEmitterPrototype.off = EventEmitterPrototype.removeListener;
    EventEmitterPrototype.removeAllListeners = function removeAllListeners(type) {
      const events = this._events;
      if (!events) return this;
      if (events.removeListener === undefined) {
        if (arguments.length === 0) { this._events = { __proto__: null }; this._eventsCount = 0; }
        else if (events[type] !== undefined) { delete events[type]; this._eventsCount--; }
        return this;
      }
      if (arguments.length === 0) {
        for (const key of Reflect.ownKeys(events)) { if (key === "removeListener") continue; this.removeAllListeners(key); }
        this.removeAllListeners("removeListener");
        this._events = { __proto__: null }; this._eventsCount = 0;
        return this;
      }
      const handlers = events[type];
      if (typeof handlers === "function") this.removeListener(type, handlers);
      else if (handlers !== undefined) { for (let i = handlers.length - 1; i >= 0; i--) this.removeListener(type, handlers[i]); }
      return this;
    };
    EventEmitterPrototype.listeners = function listeners(type) { const events = this._events; if (!events) return []; const handlers = events[type]; if (!handlers) return []; return handlers.map((x) => x.listener ?? x); };
    EventEmitterPrototype.rawListeners = function rawListeners(type) { const events = this._events; if (!events) return []; const handlers = events[type]; if (!handlers) return []; return handlers.slice(); };
    EventEmitterPrototype.listenerCount = function listenerCount(type, listener) {
      const events = this._events; if (!events) return 0;
      const evlistener = events[type]; if (!evlistener) return 0;
      if (listener != null) { let matching = 0; for (let i = 0; i < evlistener.length; i++) { if (evlistener[i] === listener || evlistener[i].listener === listener) matching++; } return matching; }
      return evlistener.length;
    };
    EventEmitterPrototype.eventNames = function eventNames() { return this._eventsCount > 0 ? Reflect.ownKeys(this._events) : []; };

    function eventTargetAgnosticRemoveListener(emitter, name, listener, flags) { if (typeof emitter.removeListener === "function") emitter.removeListener(name, listener); else emitter.removeEventListener(name, listener, flags); }
    function eventTargetAgnosticAddListener(emitter, name, listener, flags) { if (typeof emitter.on === "function") { if (flags?.once) emitter.once(name, listener); else emitter.on(name, listener); } else emitter.addEventListener(name, listener, flags); }

    function addAbortListener(signal, listener) {
      if (signal === undefined) throw ERR_INVALID_ARG_TYPE("signal", "AbortSignal", signal);
      validateAbortSignal(signal, "signal");
      if (typeof listener !== "function") throw ERR_INVALID_ARG_TYPE("listener", "function", listener);
      let removeEventListener;
      if (signal.aborted) queueMicrotask(() => listener());
      else { signal.addEventListener("abort", listener, { __proto__: null, once: true }); removeEventListener = () => signal.removeEventListener("abort", listener); }
      return { __proto__: null, [Symbol.dispose]() { removeEventListener?.(); } };
    }

    async function once(emitter, type, options) {
      options = options ?? {};
      validateObject(options, "options");
      const signal = options?.signal;
      validateAbortSignal(signal, "options.signal");
      if (signal?.aborted) throw new AbortError(undefined, { cause: signal.reason });
      return await new Promise((resolve, reject) => {
        const errorListener = (err) => { emitter.removeListener(type, resolver); if (signal != null) eventTargetAgnosticRemoveListener(signal, "abort", abortListener); reject(err); };
        const resolver = (...args) => { if (typeof emitter.removeListener === "function") emitter.removeListener("error", errorListener); if (signal != null) eventTargetAgnosticRemoveListener(signal, "abort", abortListener); resolve(args); };
        eventTargetAgnosticAddListener(emitter, type, resolver, { once: true });
        if (type !== "error" && typeof emitter.once === "function") emitter.once("error", errorListener);
        function abortListener() { eventTargetAgnosticRemoveListener(emitter, type, resolver); eventTargetAgnosticRemoveListener(emitter, "error", errorListener); reject(new AbortError(undefined, { cause: signal?.reason })); }
        if (signal != null) eventTargetAgnosticAddListener(signal, "abort", abortListener, { once: true });
      });
    }

    const AsyncIteratorPrototype = Object.getPrototypeOf(Object.getPrototypeOf(async function* () {}).prototype);
    const createIterResult = (value, done) => ({ value, done });
    function on(emitter, event, options) {
      options = options ?? {};
      validateObject(options, "options");
      const signal = options.signal;
      validateAbortSignal(signal, "options.signal");
      if (signal?.aborted) throw new AbortError(undefined, { cause: signal.reason });
      const highWatermark = options.highWaterMark ?? options.highWatermark ?? Number.MAX_SAFE_INTEGER;
      validateInteger(highWatermark, "options.highWaterMark", 1);
      const lowWatermark = options.lowWaterMark ?? options.lowWatermark ?? 1;
      validateInteger(lowWatermark, "options.lowWaterMark", 1);
      const unconsumedEvents = []; const unconsumedPromises = [];
      let paused = false, error = null, finished = false, size = 0;
      const iterator = Object.setPrototypeOf({
        next() {
          if (size) { const value = unconsumedEvents.shift(); size--; if (paused && size < lowWatermark) { emitter.resume?.(); paused = false; } return Promise.resolve(createIterResult(value, false)); }
          if (error) { const p = Promise.reject(error); error = null; return p; }
          if (finished) return closeHandler();
          return new Promise((resolve, reject) => { unconsumedPromises.push({ resolve, reject }); });
        },
        return() { return closeHandler(); },
        throw(err) { if (!err || !(err instanceof Error)) throw ERR_INVALID_ARG_TYPE("EventEmitter.AsyncIterator", "Error", err); errorHandler(err); },
        [Symbol.asyncIterator]() { return this; },
      }, AsyncIteratorPrototype);
      const { addEventListener, removeAll } = listenersController();
      addEventListener(emitter, event, options[kFirstEventParam] ? eventHandler : function (...args) { return eventHandler(args); });
      if (event !== "error" && typeof emitter.on === "function") addEventListener(emitter, "error", errorHandler);
      const closeEvents = options?.close;
      if (closeEvents?.length) for (let i = 0; i < closeEvents.length; i++) addEventListener(emitter, closeEvents[i], closeHandler);
      const abortListenerDisposable = signal ? addAbortListener(signal, abortListener) : null;
      return iterator;
      function abortListener() { errorHandler(new AbortError(undefined, { cause: signal?.reason })); }
      function eventHandler(value) {
        if (unconsumedPromises.length === 0) { size++; if (!paused && size > highWatermark) { paused = true; emitter.pause?.(); } unconsumedEvents.push(value); }
        else unconsumedPromises.shift().resolve(createIterResult(value, false));
      }
      function errorHandler(err) { if (unconsumedPromises.length === 0) error = err; else unconsumedPromises.shift().reject(err); closeHandler(); }
      function closeHandler() {
        abortListenerDisposable?.[Symbol.dispose](); removeAll(); finished = true;
        const doneResult = createIterResult(undefined, true);
        while (unconsumedPromises.length > 0) unconsumedPromises.shift().resolve(doneResult);
        return Promise.resolve(doneResult);
      }
    }
    Object.defineProperty(on, "name", { value: "on" });
    function listenersController() {
      const listeners = [];
      return {
        addEventListener(emitter, event, handler, flags) { eventTargetAgnosticAddListener(emitter, event, handler, flags); listeners.push([emitter, event, handler, flags]); },
        removeAll() { while (listeners.length > 0) { const [emitter, event, handler, flags] = listeners.pop(); eventTargetAgnosticRemoveListener(emitter, event, handler, flags); } },
      };
    }

    function getEventListeners(emitter, type) {
      if (typeof emitter?.listeners === "function") return emitter.listeners(type);
      throw ERR_INVALID_ARG_TYPE("emitter", "EventEmitter or EventTarget", emitter);
    }
    function getMaxListeners(emitterOrTarget) { return emitterOrTarget?._maxListeners ?? emitterOrTarget?.[kMaxEventTargetListeners] ?? defaultMaxListeners; }
    function setMaxListeners(n = defaultMaxListeners, ...eventTargets) {
      validateNumber(n, "setMaxListeners", 0);
      if (eventTargets.length === 0) { defaultMaxListeners = n; return; }
      for (let i = 0; i < eventTargets.length; i++) { const t = eventTargets[i]; if (typeof t.setMaxListeners === "function") t.setMaxListeners(n); else t[kMaxEventTargetListeners] = n; }
    }
    function listenerCount(emitter, type) {
      if (typeof emitter.listenerCount === "function") return emitter.listenerCount(type);
      return EventEmitterPrototype.listenerCount.call(emitter, type);
    }

    Object.defineProperties(EventEmitter, {
      captureRejections: { enumerable: true, get() { return EventEmitterPrototype[kCapture]; }, set(value) { validateBoolean(value, "EventEmitter.captureRejections"); EventEmitterPrototype[kCapture] = value; } },
      defaultMaxListeners: { enumerable: true, get: () => defaultMaxListeners, set: (arg) => { validateNumber(arg, "defaultMaxListeners", 0); defaultMaxListeners = arg; } },
    });
    class EventEmitterAsyncResource extends EventEmitter {
      constructor(options) {
        const ah = M["async_hooks"] || M["node:async_hooks"];
        const AsyncResource = ah && ah.AsyncResource;
        const opts = (options == null || typeof options !== "object") ? {} : options;
        const captureRejections = opts.captureRejections ?? false;
        const triggerAsyncId = opts.triggerAsyncId;
        const name = opts.name ?? new.target.name;
        const requireManualDestroy = opts.requireManualDestroy;
        super({ captureRejections });
        this.triggerAsyncId = triggerAsyncId ?? 0;
        this.asyncResource = new AsyncResource(name, { triggerAsyncId, requireManualDestroy });
      }
      emit(...args) { return this.asyncResource.runInAsyncScope(() => EventEmitterPrototype.emit.apply(this, args)); }
      emitDestroy() { this.asyncResource.emitDestroy(); }
    }
    Object.assign(EventEmitter, { once, on, getEventListeners, getMaxListeners, setMaxListeners, EventEmitter, EventEmitterAsyncResource, usingDomains: false, captureRejectionSymbol, errorMonitor: kErrorMonitor, addAbortListener, init: EventEmitter, listenerCount });
    return EventEmitter;
  })();
  def(["events"], EventEmitter);

  // ---- stream (EventEmitter-based Readable/Writable/Duplex/Transform) ----
  class Readable extends EventEmitter {
    constructor(opts) { super(); this._buf = []; this.readable = true; if (opts && typeof opts.read === "function") this._read = opts.read; }
    push(chunk) { if (chunk === null) { this.emit("end"); return false; } this._buf.push(chunk); this.emit("data", chunk); return true; }
    pipe(dest) { this.on("data", (c) => dest.write && dest.write(c)); this.on("end", () => dest.end && dest.end()); return dest; }
    read() { return this._buf.shift() || null; }
    setEncoding() { return this; } resume() { return this; } pause() { return this; } destroy() { this.emit("close"); return this; }
    [Symbol.asyncIterator]() { let i = 0; const s = this; return { next() { return i < s._buf.length ? Promise.resolve({ value: s._buf[i++], done: false }) : Promise.resolve({ value: undefined, done: true }); } }; }
  }
  Readable.from = (iter) => { const r = new Readable(); (async () => { for await (const x of iter) r.push(x); r.push(null); })(); return r; };
  Readable.fromWeb = (webStream) => { const r = new Readable(); (async () => { try { const reader = webStream.getReader(); for (;;) { const { value, done } = await reader.read(); if (done) break; r.push(value); } } catch (e) {} r.push(null); })(); return r; };
  Readable.toWeb = (nodeStream) => new G.ReadableStream({ start(c) { nodeStream.on("data", (d) => c.enqueue(d)); nodeStream.on("end", () => c.close()); nodeStream.on("error", (e) => c.error && c.error(e)); } });
  class Writable extends EventEmitter { constructor(opts) { super(); this.writable = true; if (opts && typeof opts.write === "function") this._write = opts.write; }
    write(chunk, enc, cb) { if (this._write) try { this._write(chunk, enc, cb || (() => {})); } catch (e) {} this.emit("data", chunk); if (typeof cb === "function") cb(); if (typeof enc === "function") enc(); return true; }
    end(chunk, enc, cb) { if (chunk != null) this.write(chunk); this.emit("finish"); this.emit("close"); const f = cb || (typeof enc === "function" ? enc : typeof chunk === "function" ? chunk : null); if (f) f(); return this; }
    destroy() { this.emit("close"); return this; } }
  class Duplex extends Readable {} Object.assign(Duplex.prototype, { write: Writable.prototype.write, end: Writable.prototype.end });
  class Transform extends Duplex { constructor(opts) { super(opts); if (opts && opts.transform) this._transform = opts.transform; } }
  class PassThrough extends Transform {}
  Writable.fromWeb = (webWritable) => { const w = new Writable(); w._write = (chunk, e, cb) => { try { const wr = webWritable.getWriter ? webWritable.getWriter() : webWritable; if (wr.write) wr.write(chunk); } catch (er) {} if (cb) cb(); }; return w; };
  Writable.toWeb = (nodeWritable) => new G.WritableStream({ write(chunk) { nodeWritable.write(chunk); }, close() { nodeWritable.end(); } });
  const streamMod = { Readable, Writable, Duplex, Transform, PassThrough, Stream: EventEmitter, pipeline: (...a) => { const cb = a[a.length - 1]; if (typeof cb === "function") cb(null); return a[a.length - 2] || a[0]; }, finished: (s, cb) => { if (typeof cb === "function") cb(null); }, addAbortSignal: (sig, s) => s, isReadable: (s) => !!(s && s.readable), isWritable: (s) => !!(s && s.writable), promises: { pipeline: (...a) => Promise.resolve(), finished: () => Promise.resolve() }, consumers: { text: (s) => G.Bun ? Bun.readableStreamToText(s) : Promise.resolve(""), json: (s) => G.Bun ? Bun.readableStreamToJSON(s) : Promise.resolve(null), arrayBuffer: (s) => G.Bun ? Bun.readableStreamToArrayBuffer(s) : Promise.resolve(new ArrayBuffer(0)) } };
  def(["stream"], streamMod);
  def(["stream/consumers"], streamMod.consumers);
  def(["stream/promises"], streamMod.promises);
  def(["stream/promises"], { pipeline: () => Promise.resolve(), finished: () => Promise.resolve() });

  // ---- process.stdin (EventEmitter-based readable) ----
  // Real pause/resume state machine matching Node's Readable: adding a "data"
  // listener schedules a resume (emits "resume" on nextTick), pause() emits
  // "pause" synchronously, and a paused stdin keeps no pending read alive so the
  // process can exit.
  //
  // Data delivery: while flowing, a pump object rides __mbunNet.items and its
  // _poll() drains fd 0 via the readFd native, emitting "data"/"end". Only the
  // *flowing* state attaches the pump, and only attach bumps __mbunNet.pending —
  // that pending count (plus refd timers) is what keeps the loop alive, so a
  // paused stdin still lets the process exit. Attach happens inside resume()
  // itself, never in the nextTick doResume: doResume deliberately still fires
  // "resume" after a synchronous pause() (Node does too), and attaching there
  // would resurrect the pump and hang a program that meant to exit.
  if (G.process) {
    const stdin = new EventEmitter();
    // When fd 0 is a real terminal (openpty child, interactive shell), stdin is a
    // tty.ReadStream: isTTY true and setRawMode drives the termios cbreak binding.
    const osn = () => G.__mbunOsNative;
    const stdinIsatty = () => { const O = osn(); return !!(O && typeof O.isatty === "function" && O.isatty(0)); };
    stdin.readable = true; stdin.isTTY = stdinIsatty() || undefined; stdin.isRaw = false; stdin.fd = 0; stdin.readableFlowing = null;
    let flowing = null, resumeScheduled = false, ended = false, attached = false;
    let rbuf = [], readableMode = false;  // paused/readable-mode buffer + flag
    // __mbunNet lives in mbun.jsc.js_net, which may load after this bootstrap
    // runs (and never, on natives-less builds) — resolve both lazily per use.
    const net = () => G.__mbunNet;
    const nn = () => G.__mbunNetNative;
    const emitEnd = () => { if (ended) return; ended = true; detach(); stdin.readable = false; stdin.emit("end"); stdin.emit("close"); };
    const pump = {
      _poll() {
        if (ended || (flowing !== true && !readableMode)) return 0;
        const N = nn(); if (!N || typeof N.readFd !== "function") return 0;
        const chunk = N.readFd(0);
        if (chunk === null) { emitEnd(); return 1; }   // EOF
        if (chunk === "") return 0;                    // EAGAIN — poll() will wake us
        const buf = G.Buffer.from(chunk, "base64");
        if (flowing === true) {
          stdin.emit("data", stdin._enc ? buf.toString(stdin._enc) : buf);
        } else {                                       // paused/readable mode: buffer + signal
          rbuf.push(buf);
          stdin.emit("readable");
        }
        return 1;
      },
    };
    const attach = () => { const N = net(); if (attached || ended || !N) return; attached = true; N.items.add(pump); N.pending++; };
    function detach() { const N = net(); if (!attached || !N) return; attached = false; N.items.delete(pump); N.pending--; }
    const doResume = () => { resumeScheduled = false; stdin.emit("resume"); };
    stdin.resume = () => { if (flowing !== true) { flowing = true; stdin.readableFlowing = true; attach(); if (!resumeScheduled) { resumeScheduled = true; G.process.nextTick(doResume); } } return stdin; };
    stdin.pause = () => { if (flowing !== false) { flowing = false; stdin.readableFlowing = false; detach(); stdin.emit("pause"); } return stdin; };
    stdin.setEncoding = (enc) => { stdin._enc = enc; return stdin; };
    // setRawMode is a tty.ReadStream method: node/bun only give process.stdin a
    // `setRawMode` when fd 0 IS a terminal (otherwise stdin is a pipe/file stream
    // that never had one). Defining it unconditionally made every piped-stdin
    // consumer that probes `typeof input.setRawMode === "function"` — readline
    // with `terminal: true`, for one — call it and take an ENOTTY 'error' event
    // that nothing listens for. ref: bun src/js/node/tty.ts (Prototype.setRawMode
    // lives on the tty ReadStream prototype); regression 26411.
    if (stdinIsatty()) {
      stdin.setRawMode = (flag) => {
        flag = !!flag;
        const O = osn();
        if (O && typeof O.setRawMode === "function") {
          const err = O.setRawMode(0, flag);
          if (err) { stdin.emit("error", new Error("setRawMode failed with errno: " + err)); return stdin; }
        }
        stdin.isRaw = flag;
        return stdin;
      };
    }
    stdin.ref = () => stdin; stdin.unref = () => stdin;
    stdin.read = () => {
      if (!rbuf.length) return null;
      const buf = rbuf.length === 1 ? rbuf[0] : G.Buffer.concat(rbuf);
      rbuf = [];
      return stdin._enc ? buf.toString(stdin._enc) : buf;
    };
    stdin.destroy = () => { detach(); ended = true; stdin.readable = false; stdin.emit("close"); return stdin; };
    // The pump's canonical consumer: `process.stdin.pipe(process.stdout)`. The
    // "data" subscription resumes stdin (see the on() override below), which is
    // what attaches the pump. Node never end()s stdout/stderr on the source's
    // "end" — they stay open until the process exits — so opts.end only governs
    // other destinations.
    stdin.pipe = (dest, opts) => {
      stdin.on("data", (c) => { if (dest && typeof dest.write === "function") dest.write(c); });
      stdin.on("end", () => {
        if (opts && opts.end === false) return;
        if (dest === G.process.stdout || dest === G.process.stderr) return;
        if (dest && typeof dest.end === "function") dest.end();
      });
      return dest;
    };
    stdin.unpipe = () => stdin;
    const origOn = stdin.on.bind(stdin);
    stdin.on = stdin.addListener = (ev, fn) => {
      origOn(ev, fn);
      // "data" enters flowing mode; "readable" starts paused-mode reads (attach
      // the pump without setting flowing=true, so .read() drives consumption).
      if (ev === "data") stdin.resume();
      else if (ev === "readable") { readableMode = true; if (!ended) attach(); }
      return stdin;
    };
    G.process.stdin = stdin;
    // process.stdout/.stderr are tty.WriteStream/Socket in node & bun, i.e. real
    // EventEmitters: consumers subscribe to "resize"/"error"/"close" on them
    // (readline's terminal mode does `output.on("resize", ...)` unconditionally).
    // The native objects installed by engine.inc only carry write/isTTY, so mix
    // an EventEmitter surface in. ref: regression 26411.
    for (const name of ["stdout", "stderr"]) {
      const strm = G.process[name];
      if (!strm || typeof strm.on === "function") continue;
      const ee = new EventEmitter();
      for (const k of ["on", "addListener", "prependListener", "once", "off", "removeListener",
                       "removeAllListeners", "emit", "listeners", "listenerCount",
                       "setMaxListeners", "eventNames"]) {
        if (typeof ee[k] === "function") strm[k] = ee[k].bind(ee);
      }
      // Writable tail that never actually closes the descriptor (node keeps
      // stdout/stderr open for the process lifetime).
      strm.end = strm.end || (() => strm);
      strm.destroy = strm.destroy || (() => strm);
      strm.cork = strm.cork || (() => {});
      strm.uncork = strm.uncork || (() => {});
    }
  }

  // ---- URLSearchParams + URL (WHATWG-ish; runs in every context) ----
  const inspectURLSearchParams = (params, nested) => {
    if (!params || !params._e || !params._e.length) return "URLSearchParams {}";
    const pad = nested ? "    " : "  ", close = nested ? "  " : "";
    const grouped = [], idx = {};
    for (const [k, v] of params._e) {
      if (Object.prototype.hasOwnProperty.call(idx, k)) { const g = grouped[idx[k]]; if (Array.isArray(g[1])) g[1].push(v); else g[1] = [g[1], v]; }
      else { idx[k] = grouped.length; grouped.push([k, v]); }
    }
    const fmt = (v) => Array.isArray(v) ? "[ " + v.map((x) => JSON.stringify(x)).join(", ") + " ]" : JSON.stringify(v);
    return "URLSearchParams {\n" + grouped.map(([k, v]) => pad + JSON.stringify(k) + ": " + fmt(v) + ",").join("\n") + "\n" + close + "}";
  };
  if (typeof G.URLSearchParams === "undefined") {
    // ref: bun src/jsc/bindings/URLSearchParams.cpp, backed by
    // WTF::URLParser::{parseURLEncodedForm,serialize}.  URLSearchParams uses
    // application/x-www-form-urlencoded, not encodeURIComponent's encode set.
    const toUSVString = (value) => {
      const input = String(value); let out = "";
      for (let i = 0; i < input.length; i++) {
        const code = input.charCodeAt(i);
        if (code >= 0xd800 && code <= 0xdbff) {
          if (i + 1 < input.length) {
            const low = input.charCodeAt(i + 1);
            if (low >= 0xdc00 && low <= 0xdfff) { out += input[i] + input[++i]; continue; }
          }
          out += "\ufffd";
        } else if (code >= 0xdc00 && code <= 0xdfff) out += "\ufffd";
        else out += input[i];
      }
      return out;
    };
    const decodeUTF8 = (bytes) => {
      let out = "";
      for (let i = 0; i < bytes.length;) {
        const first = bytes[i];
        if (first < 0x80) { out += String.fromCharCode(first); i++; continue; }
        let width, codePoint;
        if (first >= 0xc2 && first <= 0xdf) { width = 2; codePoint = first & 0x1f; }
        else if (first >= 0xe0 && first <= 0xef) { width = 3; codePoint = first & 0x0f; }
        else if (first >= 0xf0 && first <= 0xf4) { width = 4; codePoint = first & 0x07; }
        else { out += "\ufffd"; i++; continue; }
        if (i + 1 >= bytes.length) { out += "\ufffd"; i++; continue; }
        const second = bytes[i + 1];
        const validSecond = second >= 0x80 && second <= 0xbf
          && !(first === 0xe0 && second < 0xa0) && !(first === 0xed && second > 0x9f)
          && !(first === 0xf0 && second < 0x90) && !(first === 0xf4 && second > 0x8f);
        if (!validSecond) { out += "\ufffd"; i++; continue; }
        codePoint = (codePoint << 6) | (second & 0x3f);
        if (width >= 3) {
          if (i + 2 >= bytes.length) { out += "\ufffd"; i += 2; continue; }
          const third = bytes[i + 2];
          if (third < 0x80 || third > 0xbf) { out += "\ufffd"; i += 2; continue; }
          codePoint = (codePoint << 6) | (third & 0x3f);
        }
        if (width === 4) {
          if (i + 3 >= bytes.length) { out += "\ufffd"; i += 3; continue; }
          const fourth = bytes[i + 3];
          if (fourth < 0x80 || fourth > 0xbf) { out += "\ufffd"; i += 3; continue; }
          codePoint = (codePoint << 6) | (fourth & 0x3f);
        }
        out += String.fromCodePoint(codePoint); i += width;
      }
      return out;
    };
    const formDecode = (value) => String(value).replace(/\+/g, " ").replace(/(?:%[0-9a-f]{2})+/gi, (run) => {
      const bytes = [];
      for (let i = 1; i < run.length; i += 3) bytes.push(parseInt(run.slice(i, i + 2), 16));
      return decodeUTF8(bytes);
    });
    const formEncode = (value) => encodeURIComponent(toUSVString(value))
      .replace(/%20/g, "+")
      .replace(/[!'()~]/g, (c) => "%" + c.charCodeAt(0).toString(16).toUpperCase());
    const missingArgs = (name, required, actual) => {
      if (actual >= required) return;
      throw new TypeError(`${name} requires at least ${required} argument${required === 1 ? "" : "s"}`);
    };
    G.URLSearchParams = class URLSearchParams {
      constructor(init) {
        this._e = [];
        if (typeof init === "string") {
          let s = init[0] === "?" ? init.slice(1) : init;
          if (s) for (const p of s.split("&")) {
            if (!p) continue;
            const i = p.indexOf("=");
            this._e.push([formDecode(i < 0 ? p : p.slice(0, i)), formDecode(i < 0 ? "" : p.slice(i + 1))]);
          }
        } else if (init != null && typeof init[Symbol.iterator] === "function") {
          // Web IDL sequence conversion uses the object's actual iterator, even
          // for a URLSearchParams instance whose Symbol.iterator was replaced.
          for (const pair of init) {
            if (pair == null || typeof pair[Symbol.iterator] !== "function") throw new TypeError("Each query pair must be an iterable");
            const values = Array.from(pair);
            if (values.length !== 2) throw new TypeError("Each query pair must contain exactly two items");
            this._e.push([toUSVString(values[0]), toUSVString(values[1])]);
          }
        } else if (init && typeof init === "object") {
          // A live own-property walk preserves Web IDL's interleaved Get/value
          // conversion: deleting a later property while stringifying an earlier
          // value prevents the deleted property from becoming a pair.
          for (const key in init) {
            if (!Object.prototype.propertyIsEnumerable.call(init, key)) continue;
            this._e.push([toUSVString(key), toUSVString(init[key])]);
          }
        }
      }
      _updateURL() { if (this._url) { this._url._search = this._e.length ? "?" + this.toString() : ""; this._url._queryPresent = this._e.length !== 0; } }
      append(k, v) { missingArgs("URLSearchParams.append", 2, arguments.length); this._e.push([toUSVString(k), toUSVString(v)]); this._updateURL(); }
      set(k, v) {
        missingArgs("URLSearchParams.set", 2, arguments.length); k = toUSVString(k); v = toUSVString(v);
        const first = this._e.findIndex((x) => x[0] === k);
        if (first < 0) this._e.push([k, v]);
        else { this._e[first][1] = v; this._e = this._e.filter((x, i) => i === first || x[0] !== k); }
        this._updateURL();
      }
      get(k) { missingArgs("URLSearchParams.get", 1, arguments.length); const e = this._e.find((x) => x[0] === toUSVString(k)); return e ? e[1] : null; }
      getAll(k) { missingArgs("URLSearchParams.getAll", 1, arguments.length); k = toUSVString(k); return this._e.filter((x) => x[0] === k).map((x) => x[1]); }
      has(k, v) { missingArgs("URLSearchParams.has", 1, arguments.length); k = toUSVString(k); return (arguments.length < 2 || v === undefined) ? this._e.some((x) => x[0] === k) : this._e.some((x) => x[0] === k && x[1] === toUSVString(v)); }
      delete(k, v) { missingArgs("URLSearchParams.delete", 1, arguments.length); k = toUSVString(k); this._e = this._e.filter((x) => (arguments.length < 2 || v === undefined) ? x[0] !== k : !(x[0] === k && x[1] === toUSVString(v))); this._updateURL(); }
      forEach(cb, t) { missingArgs("URLSearchParams.forEach", 1, arguments.length); for (let i = 0; i < this._e.length; i++) { const [k, v] = this._e[i]; cb.call(t, v, k, this); } }
      keys() { return this._e.map((x) => x[0])[Symbol.iterator](); }
      values() { return this._e.map((x) => x[1])[Symbol.iterator](); }
      entries() { return this._e.map((x) => [x[0], x[1]])[Symbol.iterator](); }
      [Symbol.iterator]() { return this.entries(); }
      sort() { this._e.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0)); this._updateURL(); }
      get size() { return this._e.length; }
      get length() { return this._e.length; }
      toJSON() { const out = {}; for (const [k, v] of this._e) { if (Object.prototype.hasOwnProperty.call(out, k)) { if (Array.isArray(out[k])) out[k].push(v); else out[k] = [out[k], v]; } else out[k] = v; } return out; }
      toString() { return this._e.map(([k, v]) => formEncode(k) + "=" + formEncode(v)).join("&"); }
      [Symbol.for("nodejs.util.inspect.custom")]() { return inspectURLSearchParams(this, false); }
    };
    Object.defineProperty(G.URLSearchParams.prototype, "size", { get: Object.getOwnPropertyDescriptor(G.URLSearchParams.prototype, "size").get, enumerable: true, configurable: true });
  }
  if (typeof G.URL === "undefined" || typeof new G.URL("http://x/").hostname === "undefined") {
    // ref: bun src/jsc/bindings/{DOMURL,URLDecomposition}.cpp and the
    // WebKit WTF::URL state overrides they delegate to. These helpers retain
    // the state-machine boundaries (authority, path, query, fragment) while
    // avoiding per-test branches.
    const redactURL = (value) => value.indexOf("@") >= 0 ? "<redacted>" : JSON.stringify(value);
    const invalidURL = (input, base, hasBase) => {
      const message = hasBase
        ? redactURL(input) + " cannot be parsed as a URL against " + redactURL(base)
        : redactURL(input) + " cannot be parsed as a URL";
      const error = new TypeError(message); error.code = "ERR_INVALID_URL"; return error;
    };
    const specialProtocols = { "ftp:": true, "file:": true, "http:": true, "https:": true, "ws:": true, "wss:": true };
    const defaultPorts = { "ftp:": "21", "http:": "80", "https:": "443", "ws:": "80", "wss:": "443" };
    const pct = (code) => "%" + code.toString(16).toUpperCase().padStart(2, "0");
    const encodeBySet = (value, forbidden) => {
      let out = "";
      for (const c of String(value)) {
        const code = c.charCodeAt(0);
        if (code > 0x7f) out += encodeURIComponent(c);
        else out += forbidden(c, code) ? pct(code) : c;
      }
      return out;
    };
    const encodeUserInfo = (value) => encodeBySet(value, (c, n) => n <= 0x20 || n > 0x7e || '"#/:;<=>?@[\\]^`{|}'.indexOf(c) >= 0);
    const encodePath = (value, special) => encodeBySet(special ? String(value).replace(/\\/g, "/") : value,
      (c, n) => n <= 0x20 || n > 0x7e || c === '"' || c === "#" || c === "?" || c === "`" || c === "{" || c === "}");
    const encodeQuery = (value, special) => encodeBySet(value,
      (c, n) => n <= 0x20 || n > 0x7e || c === '"' || c === "#" || (special && c === "'"));
    const encodeFragment = (value) => encodeBySet(value,
      (c, n) => n <= 0x20 || n > 0x7e || c === '"' || c === "`" || c === "<" || c === ">");
    const normalizePath = (value, file) => {
      const parts = String(value).split("/"), out = [];
      for (let i = 0; i < parts.length; i++) {
        const part = parts[i], last = i === parts.length - 1;
        if (part === ".") { if (last) out.push(""); continue; }
        if (part === "..") {
          const driveRoot = file && out.length === 2 && out[0] === "" && /^[A-Za-z]:$/.test(out[1]);
          if (!driveRoot && out.length && !(out.length === 1 && out[0] === "")) out.pop();
          if (last) out.push("");
          continue;
        }
        out.push(part);
      }
      return out.join("/");
    };
    const canonicalIPv6 = (host) => {
      const input = host.slice(1, -1), halves = input.split("::");
      if (halves.length > 2) throw new TypeError("invalid IPv6 host");
      const read = (half, allowIPv4) => {
        if (!half) return [];
        const pieces = half.split(":"), words = [];
        for (let i = 0; i < pieces.length; i++) {
          const piece = pieces[i];
          if (piece.includes(".")) {
            if (!allowIPv4 || i !== pieces.length - 1) throw new TypeError("invalid IPv6 host");
            const octets = piece.split(".");
            if (octets.length !== 4 || octets.some((x) => !/^\d+$/.test(x) || Number(x) > 255)) throw new TypeError("invalid IPv6 host");
            words.push(Number(octets[0]) * 0x100 + Number(octets[1]), Number(octets[2]) * 0x100 + Number(octets[3]));
          } else {
            if (!/^[0-9a-f]{1,4}$/i.test(piece)) throw new TypeError("invalid IPv6 host");
            words.push(parseInt(piece, 16));
          }
        }
        return words;
      };
      const left = read(halves[0], halves.length === 1), right = read(halves[1] || "", true);
      if ((halves.length === 1 && left.length !== 8) || (halves.length === 2 && left.length + right.length > 7)) throw new TypeError("invalid IPv6 host");
      const words = halves.length === 2 ? left.concat(new Array(8 - left.length - right.length).fill(0), right) : left;
      let bestStart = -1, bestLength = 1;
      for (let i = 0; i < 8;) { if (words[i] !== 0) { i++; continue; } let j = i; while (j < 8 && words[j] === 0) j++; if (j - i > bestLength) { bestStart = i; bestLength = j - i; } i = j; }
      let result = "";
      for (let i = 0; i < 8; i++) {
        if (i === bestStart) { result += "::"; i += bestLength - 1; continue; }
        if (result && !result.endsWith(":")) result += ":";
        result += words[i].toString(16);
      }
      return "[" + result + "]";
    };
    const canonicalIPv4 = (host) => {
      if (!/^\d+(?:\.\d+){0,3}$/.test(host)) return host;
      const parts = host.split(".").map(Number);
      if (parts.length > 4 || parts.slice(0, -1).some((x) => x > 255)) throw new TypeError("invalid IPv4 host");
      const maxLast = 256 ** (5 - parts.length) - 1;
      if (parts[parts.length - 1] > maxLast) throw new TypeError("invalid IPv4 host");
      let value = parts[parts.length - 1];
      for (let i = 0; i < parts.length - 1; i++) value += parts[i] * 256 ** (3 - i);
      return [Math.floor(value / 16777216) % 256, Math.floor(value / 65536) % 256,
        Math.floor(value / 256) % 256, value % 256].join(".");
    };
    const canonicalHost = (value, special) => {
      let host = String(value);
      if (host.startsWith("[") || host.endsWith("]")) {
        if (!(host.startsWith("[") && host.endsWith("]"))) throw new TypeError("invalid host");
        return canonicalIPv6(host);
      }
      if (/\s|[\0/#?@\\]/.test(host)) throw new TypeError("invalid host");
      if (!special) return encodeBySet(host, (_c, n) => n > 0x7f);
      try { host = decodeURIComponent(host); } catch (_) { throw new TypeError("invalid host"); }
      if (host.includes("%")) throw new TypeError("invalid host");
      if (/[^\x00-\x7f]/.test(host)) host = puny.toASCII(host);
      return canonicalIPv4(host.toLowerCase());
    };
    const parsePort = (value, protocol, constructorMode) => {
      const input = String(value);
      if (input === "") return "";
      const match = constructorMode ? input.match(/^\d+$/) : input.match(/^\d+/);
      if (!match) return null;
      const number = Number(match[0]);
      if (number > 65535) return null;
      return String(number) === defaultPorts[protocol] ? "" : String(number);
    };
    const resolveRelative = (input, base) => {
      let rel = input, basePath = base.pathname;
      if (rel === "") return base.protocol + (base._hasAuthority || base.protocol === "file:" ? "//" + (base._authority || "") : "") + basePath + (base._queryPresent ? base._search : "");
      if (rel.startsWith("#")) return base.href.replace(/#.*$/, "") + rel;
      if (rel.startsWith("?")) return base.href.replace(/[?#].*$/, "") + rel;
      if (rel.startsWith("//") || (specialProtocols[base.protocol] && rel.startsWith("\\\\"))) return base.protocol + rel.replace(/\\/g, "/");
      if (specialProtocols[base.protocol]) rel = rel.replace(/\\/g, "/");
      if (rel.startsWith("/")) {
        if (base.protocol === "file:" && /^\/[A-Za-z]:/.test(basePath) && !/^\/[A-Za-z]:/.test(rel)) rel = basePath.slice(0, 3) + rel;
        return base.protocol + (base._hasAuthority || base.protocol === "file:" ? "//" + (base._authority || "") : "") + rel;
      }
      const directory = basePath.slice(0, basePath.lastIndexOf("/") + 1);
      return base.protocol + (base._hasAuthority || base.protocol === "file:" ? "//" + (base._authority || "") : "") + directory + rel;
    };
    // ref: bun src/runtime/webcore/ObjectURLRegistry.rs — a UUID→Blob map.
    // createObjectURL returns `blob:<uuid36>`; resolveObjectURL (node:buffer)
    // and revokeObjectURL look up / drop by that full specifier string.
    const __objectURLRegistry = new Map();
    const __blobUUID = () => {
      let s = "";
      for (let i = 0; i < 36; i++) {
        if (i === 8 || i === 13 || i === 18 || i === 23) s += "-";
        else if (i === 14) s += "4";
        else { const r = (Math.random() * 16) | 0; s += (i === 19 ? (r & 0x3) | 0x8 : r).toString(16); }
      }
      return s;
    };
    const __resolveObjectURL = function (id) {
      if (arguments.length < 1) return undefined;
      const s = String(id);
      if (s.length < 41 || s.slice(0, 5) !== "blob:") return undefined;
      return __objectURLRegistry.get(s);
    };
    try { Object.defineProperty(G, "__bunResolveObjectURL", { value: __resolveObjectURL, enumerable: false, configurable: true, writable: true }); } catch (e) {}
    G.URL = class URL {
      get [Symbol.toStringTag]() { return "URL"; }
      static canParse(input, ...rest) { try { new G.URL(input, ...rest); return true; } catch (e) { return false; } }
      static createObjectURL(blob) {
        if (arguments.length < 1) { const e = new TypeError("Not enough arguments"); e.code = "ERR_MISSING_ARGS"; throw e; }
        if (!(G.Blob && blob instanceof G.Blob)) throw new TypeError("createObjectURL expects a Blob object");
        const id = "blob:" + __blobUUID();
        __objectURLRegistry.set(id, blob);
        return id;
      }
      static revokeObjectURL(id) {
        const s = String(id);
        if (s.length < 41 || s.slice(0, 5) !== "blob:") return undefined;
        __objectURLRegistry.delete(s);
        return undefined;
      }
      constructor(input, base) {
        if (arguments.length === 0) { const e = new TypeError('The "url" argument must be specified'); e.code = "ERR_MISSING_ARGS"; throw e; }
        const original = String(input); let s = original.replace(/[\t\n\r]/g, "").trim(); const hasBase = arguments.length >= 2 && base !== undefined;
        let baseString = "";
        if (hasBase) {
          baseString = String(base);
          let parsedBase;
          try { parsedBase = new G.URL(baseString); } catch (_) { throw invalidURL(original, baseString, true); }
          const scheme = s.match(/^([a-z][a-z0-9+.-]*:)/i);
          if (scheme && scheme[1].toLowerCase() === parsedBase.protocol && specialProtocols[parsedBase.protocol]) {
            const tail = s.slice(scheme[1].length);
            if (tail === "") s = parsedBase.href;
            else if (!tail.startsWith("//")) s = resolveRelative(tail, parsedBase);
          } else if (!scheme) {
            if (!parsedBase.hostname && !parsedBase.pathname.startsWith("/")) throw invalidURL(original, baseString, true);
            s = resolveRelative(s, parsedBase);
          }
        }
        if (!/^[a-z][a-z0-9+.-]*:/i.test(s) && !s.startsWith("//")) throw invalidURL(original, baseString, hasBase);
        try { this._parse(s); } catch (_) { throw invalidURL(original, baseString, hasBase); }
      }
      _parse(s) {
        const oldParams = this.searchParams;
        const m = s.match(/^([a-z][a-z0-9+.-]*:)/i);
        this._protocol = m ? m[1].toLowerCase() : "";
        let rest = m ? s.slice(m[1].length) : s;
        const special = !!specialProtocols[this._protocol], file = this._protocol === "file:";
        const hi = rest.indexOf("#"), fragment = hi >= 0 ? rest.slice(hi + 1) : ""; if (hi >= 0) rest = rest.slice(0, hi);
        const qi = rest.indexOf("?"), query = qi >= 0 ? rest.slice(qi + 1) : ""; if (qi >= 0) rest = rest.slice(0, qi);
        if (special) rest = rest.replace(/\\/g, "/");
        if (special && !file && !rest.startsWith("//")) rest = "//" + rest.replace(/^\/+/, "");
        if (file && !rest.startsWith("//")) rest = "///" + rest.replace(/^\/+/, "");
        let path = rest; this._hasAuthority = rest.startsWith("//");
        this._username = ""; this._password = ""; this._hostname = ""; this._port = "";
        if (this._hasAuthority) {
          const after = rest.slice(2); const slash = after.indexOf("/");
          let auth = slash < 0 ? after : after.slice(0, slash); path = slash < 0 ? "" : after.slice(slash);
          const at = auth.lastIndexOf("@");
          if (at >= 0) { if (file) throw new TypeError("file credentials"); const ui = auth.slice(0, at); auth = auth.slice(at + 1); const ci = ui.indexOf(":"); this._username = encodeUserInfo(ci < 0 ? ui : ui.slice(0, ci)); this._password = ci < 0 ? "" : encodeUserInfo(ui.slice(ci + 1)); }
          let host = auth, port = "";
          if (auth.startsWith("[")) { const br = auth.indexOf("]"); if (br < 0) throw new TypeError("invalid host"); host = auth.slice(0, br + 1); if (auth.length > br + 1) { if (auth[br + 1] !== ":") throw new TypeError("invalid port"); port = auth.slice(br + 2); } }
          else { const ci = auth.lastIndexOf(":"); if (ci >= 0) { host = auth.slice(0, ci); port = auth.slice(ci + 1); } }
          if (file && port) throw new TypeError("file port");
          this._hostname = canonicalHost(host, special);
          const parsedPort = parsePort(port, this._protocol, true); if (parsedPort === null) throw new TypeError("invalid port"); this._port = parsedPort;
        }
        if (special && !file && !this._hostname) throw new TypeError("invalid host");
        this._opaque = !special && !this._hasAuthority && !path.startsWith("/");
        if (!this._opaque) path = normalizePath(encodePath(path, special), file);
        else path = encodePath(path, false);
        this._pathname = path || (this._hasAuthority && special ? "/" : "");
        this._queryPresent = qi >= 0; this._search = qi >= 0 ? "?" + encodeQuery(query, special) : "";
        this._hash = hi >= 0 ? "#" + encodeFragment(fragment) : "";
        const parsedParams = new G.URLSearchParams(query);
        if (oldParams) { oldParams._e = parsedParams._e; this.searchParams = oldParams; }
        else this.searchParams = parsedParams;
        Object.defineProperty(this.searchParams, "_url", { value: this, writable: true, configurable: true });
      }
      get protocol() { return this._protocol; }
      set protocol(v) { let p = String(v).toLowerCase(); if (!p.endsWith(":")) p += ":"; if (/^[a-z][a-z0-9+.-]*:$/.test(p)) { this._protocol = p; if (this._port === defaultPorts[p]) this._port = ""; } }
      get username() { return this._username; }
      set username(v) { if (this._hostname && this._protocol !== "file:") this._username = encodeUserInfo(v); }
      get password() { return this._password; }
      set password(v) { if (this._hostname && this._protocol !== "file:") this._password = encodeUserInfo(v); }
      get hostname() { return this._hostname; }
      set hostname(v) { if (!this._opaque) { try { this._hostname = canonicalHost(v, !!specialProtocols[this._protocol]); } catch (_) {} } }
      get port() { return this._port; }
      set port(v) { if (!this._hostname || this._protocol === "file:") return; const parsed = parsePort(v, this._protocol, false); if (parsed !== null) this._port = parsed; }
      get host() { return this._hostname + (this._port ? ":" + this._port : ""); }
      set host(v) { if (this._opaque) return; const value = String(v), close = value.startsWith("[") ? value.indexOf("]") : -1, ci = close >= 0 ? value.indexOf(":", close) : value.lastIndexOf(":"); const host = ci < 0 ? value : value.slice(0, ci), port = ci < 0 ? null : value.slice(ci + 1); try { const parsedHost = canonicalHost(host, !!specialProtocols[this._protocol]); const parsedPort = port === null ? this._port : parsePort(port, this._protocol, false); if (parsedPort !== null) { this._hostname = parsedHost; this._port = parsedPort; } } catch (_) {} }
      get pathname() { return this._pathname; }
      set pathname(v) { if (!this._opaque) { let path = encodePath(v, !!specialProtocols[this._protocol]); if (this._hasAuthority && !path.startsWith("/")) path = "/" + path; this._pathname = normalizePath(path, this._protocol === "file:"); } }
      get hash() { return this._hash; }
      set hash(v) { const value = String(v); this._hash = value === "" ? "" : "#" + encodeFragment(value.startsWith("#") ? value.slice(1) : value); }
      get search() { return this._search === "?" ? "" : this._search; }
      set search(v) { const s = String(v), query = s.startsWith("?") ? s.slice(1) : s; this._queryPresent = s !== ""; this._search = s === "" ? "" : "?" + encodeQuery(query, !!specialProtocols[this._protocol]); const next = new G.URLSearchParams(query); this.searchParams._e = next._e; }
      get _authority() { const credentials = this._username || this._password ? this._username + (this._password ? ":" + this._password : "") + "@" : ""; return credentials + this.host; }
      get href() { return this._protocol + (this._hasAuthority || this._protocol === "file:" ? "//" + this._authority : "") + this._pathname + (this._queryPresent ? this._search : "") + this._hash; }
      set href(v) { this._parse(String(v)); }
      get origin() {
        if (this._protocol === "blob:") {
          try {
            const inner = new G.URL(this._pathname);
            if (inner.protocol === "file:") return "file://" + inner.host;
            return inner.origin;
          } catch (_) { return "null"; }
        }
        return specialProtocols[this._protocol] && this._protocol !== "file:" && this._hostname ? this._protocol + "//" + this.host : "null";
      }
      toString() { return this.href; }
      toJSON() { return this.href; }
      [Symbol.for("nodejs.util.inspect.custom")]() {
        try { void this.href; } catch (_) { return "URL {}"; }
        return "URL {\n" +
          "  href: " + JSON.stringify(this.href) + ",\n" +
          "  origin: " + JSON.stringify(this.origin) + ",\n" +
          "  protocol: " + JSON.stringify(this.protocol) + ",\n" +
          "  username: " + JSON.stringify(this.username) + ",\n" +
          "  password: " + JSON.stringify(this.password) + ",\n" +
          "  host: " + JSON.stringify(this.host) + ",\n" +
          "  hostname: " + JSON.stringify(this.hostname) + ",\n" +
          "  port: " + JSON.stringify(this.port) + ",\n" +
          "  pathname: " + JSON.stringify(this.pathname) + ",\n" +
          "  hash: " + JSON.stringify(this.hash) + ",\n" +
          "  search: " + JSON.stringify(this.search) + ",\n" +
          "  searchParams: " + inspectURLSearchParams(this.searchParams, true) + ",\n" +
          "  toJSON: [Function: toJSON],\n" +
          "  toString: [Function: toString],\n" +
          "}";
      }
    };
    // Bun.inspect is installed later from util.inspect. Wrap that seam here so
    // URL formatting is ready regardless of whether the Bun global exists yet.
    const inspectBeforeURL = util.inspect;
    util.inspect = function inspect(value, options) {
      if (value instanceof G.URLSearchParams) return inspectURLSearchParams(value, false);
      if (value instanceof G.URL) return value[Symbol.for("nodejs.util.inspect.custom")]();
      return inspectBeforeURL.call(this, value, options);
    };
    for (const k of Object.keys(inspectBeforeURL)) { try { util.inspect[k] = inspectBeforeURL[k]; } catch (_) {} }
  }

  // ---- URLPattern (WHATWG subset: :named / * / {} groups / (regex) / ? optional) ----
  if (typeof G.URLPattern === "undefined") {
    const compilePart = (pattern, segSep) => {
      if (pattern == null || pattern === "*") return { re: /^(.*)$/s, names: ["0"], src: "*" };
      const seg = segSep || "/";
      let re = "^"; const names = []; let i = 0; const n = pattern.length;
      while (i < n) {
        const c = pattern[i];
        if (c === ":") { let j = i + 1, name = ""; while (j < n && /[A-Za-z0-9_$]/.test(pattern[j])) { name += pattern[j]; j++; } names.push(name); i = j; let grp = "([^" + seg + "]+)"; if (pattern[i] === "(") { let depth = 1, k = i + 1, rx = ""; while (k < n && depth > 0) { if (pattern[k] === "(") depth++; else if (pattern[k] === ")") { depth--; if (!depth) break; } rx += pattern[k]; k++; } grp = "(" + rx + ")"; i = k + 1; } if (pattern[i] === "?") { grp += "?"; i++; } else if (pattern[i] === "*") { grp = "(.*)"; i++; } re += grp; }
        else if (c === "*") { names.push(String(names.length)); re += "(.*)"; i++; }
        else if (c === "(") { let depth = 1, k = i + 1, rx = ""; while (k < n && depth > 0) { if (pattern[k] === "(") depth++; else if (pattern[k] === ")") { depth--; if (!depth) break; } rx += pattern[k]; k++; } names.push(String(names.length)); re += "(" + rx + ")"; i = k + 1; }
        else if (c === "{") { let k = i + 1, inner = ""; while (k < n && pattern[k] !== "}") { inner += pattern[k]; k++; } i = k + 1; let opt = ""; if (pattern[i] === "?") { opt = "?"; i++; } const sub = compilePart(inner, seg); re += "(?:" + sub.re.source.replace(/^\^|\$$/g, "") + ")" + opt; for (const nm of sub.names) names.push(nm); }
        else if ("\\^$.|?+[]".indexOf(c) >= 0) { re += "\\" + c; i++; }
        else { re += c; i++; }
      }
      re += "$";
      let compiled; try { compiled = new RegExp(re); } catch (e) { compiled = /^(.*)$/s; }
      return { re: compiled, names, src: pattern };
    };
    const PARTS = ["protocol", "username", "password", "hostname", "port", "pathname", "search", "hash"];
    const toComponents = (input, baseURL) => {
      if (typeof input === "string") { try { const u = new G.URL(input, baseURL); return { protocol: u.protocol.replace(/:$/, ""), username: u.username, password: u.password, hostname: u.hostname, port: u.port, pathname: u.pathname, search: u.search.replace(/^\?/, ""), hash: u.hash.replace(/^#/, "") }; } catch (e) { return { pathname: input }; } }
      return input || {};
    };
    G.URLPattern = class URLPattern {
      constructor(input, baseURL) {
        if (typeof input === "string") { try { const u = new G.URL(input, baseURL || "https://x"); input = { protocol: u.protocol.replace(/:$/, ""), hostname: u.hostname, port: u.port, pathname: u.pathname, search: u.search.replace(/^\?/, ""), hash: u.hash.replace(/^#/, "") }; } catch (e) { input = { pathname: input }; } }
        input = input || {}; this._c = {};
        for (const p of PARTS) { const pat = input[p] != null ? input[p] : "*"; this[p] = String(pat); this._c[p] = compilePart(String(pat), p === "pathname" ? "/" : (p === "hostname" ? "." : "/")); }
      }
      test(input, baseURL) { return this.exec(input, baseURL) !== null; }
      exec(input, baseURL) {
        const comp = toComponents(input, baseURL); const result = { inputs: [input] };
        for (const p of PARTS) { const { re, names } = this._c[p]; const v = comp[p] != null ? comp[p] : ""; const m = re.exec(v); if (!m) return null; const groups = {}; names.forEach((nm, idx) => { groups[nm] = m[idx + 1]; }); result[p] = { input: v, groups }; }
        return result;
      }
    };
  }

  // ---- punycode (RFC 3492 bootstring) — used by url.domainTo{ASCII,Unicode} ----
  const puny = (() => {
    const base = 36, tmin = 1, tmax = 26, skew = 38, damp = 700, initialBias = 72, initialN = 128, delim = "-", MAXINT = 0x7fffffff;
    const adapt = (delta, numPoints, firstTime) => { delta = firstTime ? Math.floor(delta / damp) : delta >> 1; delta += Math.floor(delta / numPoints); let k = 0; for (; delta > ((base - tmin) * tmax) >> 1; k += base) delta = Math.floor(delta / (base - tmin)); return Math.floor(k + (base - tmin + 1) * delta / (delta + skew)); };
    const ucs2 = (str) => { const out = []; let i = 0; while (i < str.length) { const c = str.charCodeAt(i++); if (c >= 0xd800 && c <= 0xdbff && i < str.length) { const c2 = str.charCodeAt(i++); if ((c2 & 0xfc00) === 0xdc00) out.push(((c & 0x3ff) << 10) + (c2 & 0x3ff) + 0x10000); else { out.push(c); i--; } } else out.push(c); } return out; };
    const digitToBasic = (d) => d + 22 + (d < 26 ? 75 : 0);
    const basicToDigit = (cp) => { if (cp - 48 < 10) return cp - 22; if (cp - 65 < 26) return cp - 65; if (cp - 97 < 26) return cp - 97; return base; };
    const encode = (input) => {
      const cps = ucs2(input), out = [];
      let n = initialN, delta = 0, bias = initialBias;
      for (const cp of cps) if (cp < 0x80) out.push(String.fromCharCode(cp));
      const basicLength = out.length; let handled = basicLength;
      if (basicLength) out.push(delim);
      while (handled < cps.length) {
        let m = MAXINT; for (const cp of cps) if (cp >= n && cp < m) m = cp;
        if (m - n > Math.floor((MAXINT - delta) / (handled + 1))) throw new RangeError("overflow");
        delta += (m - n) * (handled + 1); n = m;
        for (const cp of cps) {
          if (cp < n && ++delta > MAXINT) throw new RangeError("overflow");
          if (cp === n) {
            let q = delta;
            for (let k = base; ; k += base) { const t = k <= bias ? tmin : (k >= bias + tmax ? tmax : k - bias); if (q < t) break; const qt = q - t, bt = base - t; out.push(String.fromCharCode(digitToBasic(t + qt % bt))); q = Math.floor(qt / bt); }
            out.push(String.fromCharCode(digitToBasic(q))); bias = adapt(delta, handled + 1, handled === basicLength); delta = 0; ++handled;
          }
        }
        ++delta; ++n;
      }
      return out.join("");
    };
    const decode = (input) => {
      const output = []; let n = initialN, i = 0, bias = initialBias;
      let basic = input.lastIndexOf(delim); if (basic < 0) basic = 0;
      for (let j = 0; j < basic; j++) { if (input.charCodeAt(j) >= 0x80) throw new RangeError("not-basic"); output.push(input.charCodeAt(j)); }
      for (let idx = basic > 0 ? basic + 1 : 0; idx < input.length;) {
        const oldi = i; let w = 1;
        for (let k = base; ; k += base) { if (idx >= input.length) throw new RangeError("invalid"); const digit = basicToDigit(input.charCodeAt(idx++)); if (digit >= base || digit > Math.floor((MAXINT - i) / w)) throw new RangeError("overflow"); i += digit * w; const t = k <= bias ? tmin : (k >= bias + tmax ? tmax : k - bias); if (digit < t) break; const bt = base - t; if (w > Math.floor(MAXINT / bt)) throw new RangeError("overflow"); w *= bt; }
        const outLen = output.length + 1; bias = adapt(i - oldi, outLen, oldi === 0); if (Math.floor(i / outLen) > MAXINT - n) throw new RangeError("overflow"); n += Math.floor(i / outLen); i %= outLen; output.splice(i++, 0, n);
      }
      return String.fromCodePoint(...output);
    };
    const toASCII = (domain) => String(domain).split(".").map((l) => (/[^\x00-\x7F]/.test(l) ? "xn--" + encode(l) : l)).join(".");
    const toUnicode = (domain) => String(domain).split(".").map((l) => (/^xn--/i.test(l) ? (() => { try { return decode(l.slice(4)); } catch (e) { return l; } })() : l)).join(".");
    return { encode, decode, toASCII, toUnicode, ucs2: { decode: ucs2, encode: (a) => String.fromCodePoint(...a) }, version: "2.3.1" };
  })();
  def(["punycode"], puny);

  // ---- node:tty / node:perf_hooks / node:_http_common ----
  def(["tty"], { isatty: () => false, ReadStream: class ReadStream extends EventEmitter { constructor() { super(); this.isTTY = true; this.isRaw = false; } setRawMode() { return this; } ref() {} unref() {} }, WriteStream: class WriteStream extends EventEmitter { constructor() { super(); this.isTTY = true; this.columns = 80; this.rows = 24; } getColorDepth() { return 8; } hasColors() { return true; } clearLine() { return true; } cursorTo() { return true; } getWindowSize() { return [80, 24]; } } });
  const PerfObserver = class PerformanceObserver { constructor(cb) { this._cb = cb; } observe() {} disconnect() {} takeRecords() { return []; } };
  PerfObserver.supportedEntryTypes = ["mark", "measure", "function"];
  def(["perf_hooks"], { performance: G.performance || { now: () => 0, timeOrigin: 0, mark() {}, measure() {}, getEntries: () => [], getEntriesByName: () => [], getEntriesByType: () => [], clearMarks() {}, clearMeasures() {} }, PerformanceObserver: PerfObserver, PerformanceEntry: class PerformanceEntry {}, PerformanceMark: class PerformanceMark {}, PerformanceMeasure: class PerformanceMeasure {}, monitorEventLoopDelay: () => ({ enable() {}, disable() {}, reset() {}, percentile: () => 0, min: 0, max: 0, mean: 0, stddev: 0 }), createHistogram: () => ({ record() {}, reset() {}, percentile: () => 0 }), constants: { NODE_PERFORMANCE_GC_MAJOR: 4 } });
  def(["_http_common"], { HTTPParser: class HTTPParser {}, methods: ["GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS", "CONNECT", "TRACE"], continueExpression: () => false });
  // ---- more node builtins (stubs/aliases so importing files load) ----
  Object.defineProperty(M, "process", { get: () => G.process, configurable: true, enumerable: true });        // node:process → the global (lazy: process may install after this)
  Object.defineProperty(M, "node:process", { get: () => G.process, configurable: true, enumerable: true });
  def(["sys"], util);                                       // deprecated alias for util
  M["assert/strict"] = M["node:assert/strict"] = Object.assign(assert.strict || ((v, m) => assert(v, m)), assert);  // strict-mode assert
  assert.strict = M["assert/strict"];
  def(["diagnostics_channel"], { channel: (n) => ({ name: n, hasSubscribers: false, publish() {}, subscribe() {}, unsubscribe() {} }), hasSubscribers: () => false, subscribe() {}, unsubscribe() {}, tracingChannel: () => ({ start() {}, end() {}, asyncStart() {}, asyncEnd() {}, error() {}, traceSync: (fn, ...a) => fn(...a), tracePromise: (fn, ...a) => fn(...a), traceCallback: (fn, ...a) => fn(...a), subscribe() {}, unsubscribe() {} }) });
  def(["cluster"], Object.assign(new EventEmitter(), { isPrimary: true, isMaster: true, isWorker: false, workers: {}, settings: {}, schedulingPolicy: 2, fork: () => new EventEmitter(), setupPrimary() {}, setupMaster() {}, disconnect(cb) { if (cb) cb(); }, worker: null }));
  def(["inspector"], { open() { throw new Error("node:inspector is not yet implemented in Bun. Track the status & thumbs up the issue: https://github.com/oven-sh/bun/issues/2445"); }, close() { throw new Error("node:inspector is not yet implemented in Bun. Track the status & thumbs up the issue: https://github.com/oven-sh/bun/issues/2445"); }, url: () => undefined, waitForDebugger() { throw new Error("node:inspector is not yet implemented in Bun. Track the status & thumbs up the issue: https://github.com/oven-sh/bun/issues/2445"); }, console: G.console, Session: class Session extends EventEmitter { connect() {} disconnect() {} post(m, p, cb) { if (typeof p === "function") p(null, {}); else if (cb) cb(null, {}); } } });
  def(["trace_events"], { createTracing: () => ({ enable() {}, disable() {}, get enabled() { return false; }, categories: "" }), getEnabledCategories: () => undefined });
  def(["wasi"], { WASI: class WASI { constructor(o) { this.wasiImport = {}; this._opts = o || {}; } start() { return 0; } initialize() {} getImportObject() { return { wasi_snapshot_preview1: this.wasiImport }; } } });
  def(["repl"], { start: () => new EventEmitter(), REPLServer: class REPLServer extends EventEmitter {}, Recoverable: class Recoverable extends Error {}, writer: (v) => String(v), REPL_MODE_SLOPPY: 0, REPL_MODE_STRICT: 1 });
  def(["_stream_wrap"], { StreamWrap: class StreamWrap extends EventEmitter {} });
  def(["test/reporters"], { tap: function* () {}, spec: class Spec {}, dot: function* () {}, junit: function* () {}, lcov: class Lcov {} });

  // ---- node:vm — eval-backed (real context isolation DEFERRED; sandbox globals
  // are shallow-merged onto globalThis for the eval then restored) ----
  const vmRun = (code, sandbox) => {
    const keys = sandbox && typeof sandbox === "object" ? Object.keys(sandbox) : [];
    const saved = {}; for (const k of keys) { saved[k] = G[k]; G[k] = sandbox[k]; }
    try { return (0, eval)(String(code)); }
    finally { for (const k of keys) { if (sandbox) sandbox[k] = G[k]; G[k] = saved[k]; } }
  };
  class Script { constructor(code) { this.code = String(code); } runInThisContext() { return (0, eval)(this.code); } runInNewContext(sandbox) { return vmRun(this.code, sandbox); } runInContext(ctx) { return vmRun(this.code, ctx); } }
  def(["vm"], {
    Script,
    runInThisContext: (code) => (0, eval)(String(code)),
    runInNewContext: (code, sandbox) => vmRun(code, sandbox),
    runInContext: (code, ctx) => vmRun(code, ctx),
    createContext: (o) => o || {},
    isContext: () => true,
    compileFunction: (code, params) => new Function(...(params || []), String(code)),
    measureMemory: () => Promise.resolve({ total: { jsMemoryEstimate: 0, jsMemoryRange: [0, 0] } }),
  });

  // ---- node:url — legacy url.parse/format/resolve: faithful port of node's
  // lib/url.js (as shipped in bun's src/js/node/url.ts, Joyent/MIT). The
  // WHATWG-URL IDNA/host-validation step (`new URL("http://" + hostname)`)
  // is inlined as urlValidateHostname (validation only — IPv6 canonicalization
  // is intentionally skipped; the raw lowercased host is kept). ----
  // domainTo{ASCII,Unicode}: null/undefined pass through; a domain containing a
  // userinfo '@' / IPv6 ':' / whitespace is invalid → "" (node/bun behavior).
  const badDomain = (d) => d === "" || /[@\s]/.test(d) || d.indexOf(":") !== -1;
  const urlInvalid = (input) => { const e = new TypeError("Invalid URL: " + input); e.code = "ERR_INVALID_URL"; return e; };
  // querystring.parse semantics onto a null-prototype object ('+' → space,
  // lenient %-decoding, duplicate keys collect into arrays).
  const urlQsParse = (s) => {
    const o = Object.create(null);
    const dec = (x) => { x = String(x).replace(/\+/g, " "); try { return decodeURIComponent(x); } catch (e) { return x; } };
    if (s) for (const p of String(s).split("&")) { if (!p) continue; const i = p.indexOf("="); const k = dec(i < 0 ? p : p.slice(0, i)), v = i < 0 ? "" : dec(p.slice(i + 1)); if (k in o) { if (Array.isArray(o[k])) o[k].push(v); else o[k] = [o[k], v]; } else o[k] = v; }
    return o;
  };
  // WHATWG-style IPv6 literal check: one optional "::", 1-4 hex-digit groups,
  // optional trailing dotted IPv4; exactly 8 groups without "::", <8 with it.
  const urlIsIpv6 = (s) => {
    if (s === "" || /[^0-9a-fA-F:.]/.test(s)) return false;
    const halves = s.split("::");
    if (halves.length > 2) return false;
    let count = 0;
    const groupsOk = (str, tailOk) => {
      if (str === "") return true;
      const gs = str.split(":");
      for (let i = 0; i < gs.length; i++) {
        const g = gs[i];
        if (tailOk && i === gs.length - 1 && g.indexOf(".") !== -1) {
          const oct = g.split(".");
          if (oct.length !== 4) return false;
          for (const oc of oct) if (!/^\d{1,3}$/.test(oc) || +oc > 255) return false;
          count += 2;
        } else if (/^[0-9a-fA-F]{1,4}$/.test(g)) count++;
        else return false;
      }
      return true;
    };
    if (!groupsOk(halves[0], halves.length === 1)) return false;
    if (halves.length === 2 && !groupsOk(halves[1], true)) return false;
    return halves.length === 2 ? count < 8 : count === 8;
  };
  const urlValidateHostname = (hostname, input) => {
    if (hostname.indexOf("[") !== -1 || hostname.indexOf("]") !== -1) {
      if (!(hostname[0] === "[" && hostname[hostname.length - 1] === "]" && urlIsIpv6(hostname.slice(1, -1)))) throw urlInvalid(input);
      return hostname;
    }
    let h = hostname;
    if (/[^\x00-\x7F]/.test(h)) { try { h = puny.toASCII(h); } catch (e) { throw urlInvalid(input); } }
    if (h === "" || /[\x00-\x20#%/:?@\\]/.test(h)) throw urlInvalid(input);
    return h;
  };
  // ---- legacy Url class (node lib/url.js) ----
  const urlProtocolPattern = /^([a-z0-9.+-]+:)/i, urlPortPattern = /:[0-9]*$/,
    urlSimplePathPattern = /^(\/\/?(?!\/)[^?\s]*)(\?[^\s]*)?$/,
    urlAutoEscape = ["'", "{", "}", "|", "\\", "^", "`", "<", ">", '"', " ", "\r", "\n", "\t"],
    urlNonHostChars = ["%", "/", "?", ";", "#"].concat(urlAutoEscape),
    urlHostEndingChars = ["/", "?", "#"],
    urlHostnameMaxLen = 255,
    urlUnsafeProtocol = { __proto__: null, javascript: true, "javascript:": true },
    urlHostlessProtocol = { __proto__: null, javascript: true, "javascript:": true },
    urlSlashedProtocol = { __proto__: null, http: true, https: true, ftp: true, gopher: true, file: true, ws: true, wss: true, "http:": true, "https:": true, "ftp:": true, "gopher:": true, "file:": true, "ws:": true, "wss:": true };
  function Url() {
    this.protocol = null; this.slashes = null; this.auth = null; this.host = null;
    this.port = null; this.hostname = null; this.hash = null; this.search = null;
    this.query = null; this.pathname = null; this.path = null; this.href = null;
  }
  Url.prototype = {};
  const urlParse = (url, parseQueryString, slashesDenoteHost) => {
    if (url !== null && typeof url === "object" && url instanceof Url) return url;
    const u = new Url();
    try { u.parse(url, parseQueryString, slashesDenoteHost); }
    catch (e) { try { e.input = url; } catch (e2) {} throw e; }
    return u;
  };
  let urlWarnInvalidPort = true;
  const urlGetHostname = (self, rest, hostname, url) => {
    for (let i = 0; i < hostname.length; ++i) {
      const code = hostname.charCodeAt(i);
      const isValid = code !== 47 /* / */ && code !== 92 /* \ */ && code !== 35 /* # */ && code !== 63 /* ? */ && code !== 58 /* : */;
      if (!isValid) {
        // leftover starting with ":" is an invalid port; url.parse() stays
        // lenient about it (node DEP0170: warn once and keep going)
        if (urlWarnInvalidPort && code === 58) {
          if (G.process && typeof G.process.emitWarning === "function") G.process.emitWarning("The URL " + url + " is invalid. Future versions of Node.js will throw an error.", "DeprecationWarning", "DEP0170");
          urlWarnInvalidPort = false;
        }
        self.hostname = hostname.slice(0, i);
        return "/" + hostname.slice(i) + rest;
      }
    }
    return rest;
  };
  Url.prototype.parse = function parse(url, parseQueryString, slashesDenoteHost) {
    if (typeof url !== "string") { const e = new TypeError('The "url" argument must be of type string. Received ' + (url === null ? "null" : typeof url)); e.code = "ERR_INVALID_ARG_TYPE"; throw e; }
    // Copy chrome/IE/opera backslash-handling: backslashes before the query
    // string become forward slashes; also trim whitespace off both ends.
    let hasHash = false, hasAt = false, start = -1, end = -1, rest = "", lastPos = 0;
    for (let i = 0, inWs = false, split = false; i < url.length; ++i) {
      const code = url.charCodeAt(i);
      const isWs = code < 33 || code === 160 || code === 65279;
      if (start === -1) { if (isWs) continue; lastPos = start = i; }
      else if (inWs) { if (!isWs) { end = -1; inWs = false; } }
      else if (isWs) { end = i; inWs = true; }
      if (!split) {
        switch (code) {
          case 64 /* @ */: hasAt = true; break;
          case 35 /* # */: hasHash = true; // fall through
          case 63 /* ? */: split = true; break;
          case 92 /* \ */: if (i - lastPos > 0) rest += url.slice(lastPos, i); rest += "/"; lastPos = i + 1; break;
        }
      } else if (!hasHash && code === 35) hasHash = true;
    }
    if (start !== -1) {
      if (lastPos === start) { if (end === -1) { rest = start === 0 ? url : url.slice(start); } else rest = url.slice(start, end); }
      else if (end === -1 && lastPos < url.length) rest += url.slice(lastPos);
      else if (end !== -1 && lastPos < end) rest += url.slice(lastPos, end);
    }
    if (!slashesDenoteHost && !hasHash && !hasAt) {
      // fast path for simple path urls
      const simplePath = urlSimplePathPattern.exec(rest);
      if (simplePath) {
        this.path = rest; this.href = rest; this.pathname = simplePath[1];
        if (simplePath[2]) { this.search = simplePath[2]; this.query = parseQueryString ? urlQsParse(this.search.slice(1)) : this.search.slice(1); }
        else if (parseQueryString) { this.search = null; this.query = Object.create(null); }
        return this;
      }
    }
    let proto = urlProtocolPattern.exec(rest), lowerProto;
    if (proto) { proto = proto[0]; lowerProto = proto.toLowerCase(); this.protocol = lowerProto; rest = rest.slice(proto.length); }
    // user@server is *always* a hostname; //foo/bar resolves as host=foo,path=/bar
    let slashes;
    if (slashesDenoteHost || proto || /^\/\/[^@/]+@[^@/]+/.test(rest)) {
      slashes = rest.slice(0, 2) === "//";
      if (slashes && !(proto && urlHostlessProtocol[lowerProto])) { rest = rest.slice(2); this.slashes = true; }
    }
    if (!urlHostlessProtocol[lowerProto] && (slashes || (proto && !urlSlashedProtocol[proto]))) {
      // host ends at the first of / ? # ; auth may sit left of the last @
      // that appears before that point (http://a@b@c/ → user:a@b host:c)
      let hostEnd = -1;
      for (const hec0 of urlHostEndingChars) { const hec = rest.indexOf(hec0); if (hec !== -1 && (hostEnd === -1 || hec < hostEnd)) hostEnd = hec; }
      const atSign = hostEnd === -1 ? rest.lastIndexOf("@") : rest.lastIndexOf("@", hostEnd);
      if (atSign !== -1) { this.auth = decodeURIComponent(rest.slice(0, atSign)); rest = rest.slice(atSign + 1); }
      hostEnd = -1;
      for (const nhc of urlNonHostChars) { const hec = rest.indexOf(nhc); if (hec !== -1 && (hostEnd === -1 || hec < hostEnd)) hostEnd = hec; }
      if (hostEnd === -1) hostEnd = rest.length;
      this.host = rest.slice(0, hostEnd);
      rest = rest.slice(hostEnd);
      this.parseHost();
      if (typeof this.hostname !== "string") this.hostname = "";
      const hostname = this.hostname;
      // [xx] brackets → IPv6 literal
      const ipv6Hostname = hostname.charCodeAt(0) === 91 && hostname.charCodeAt(hostname.length - 1) === 93;
      if (!ipv6Hostname) rest = urlGetHostname(this, rest, hostname, url);
      if (this.hostname.length > urlHostnameMaxLen) this.hostname = "";
      else this.hostname = this.hostname.toLowerCase();
      if (this.hostname) this.hostname = urlValidateHostname(this.hostname, url);
      this.host = (this.hostname || "") + (this.port ? ":" + this.port : "");
      // hostname drops the [ ] brackets; host keeps them
      if (ipv6Hostname) { this.hostname = this.hostname.slice(1, -1); if (rest[0] !== "/") rest = "/" + rest; }
    }
    // escape delim/unwise chars that survived (except for unsafe protocols)
    if (!urlUnsafeProtocol[lowerProto]) {
      for (const ae of urlAutoEscape) {
        if (rest.indexOf(ae) === -1) continue;
        let esc = encodeURIComponent(ae);
        if (esc === ae) esc = escape(ae);
        rest = rest.split(ae).join(esc);
      }
    }
    // chop off from the tail first: everything from the first # is the hash
    // (so "#baz?a=1" keeps the ?-part inside the hash)
    const hashIdx = rest.indexOf("#");
    if (hashIdx !== -1) { this.hash = rest.slice(hashIdx); rest = rest.slice(0, hashIdx); }
    const qm = rest.indexOf("?");
    if (qm !== -1) {
      this.search = rest.slice(qm);
      this.query = parseQueryString ? urlQsParse(rest.slice(qm + 1)) : rest.slice(qm + 1);
      rest = rest.slice(0, qm);
    } else if (parseQueryString) { this.search = null; this.query = Object.create(null); }
    if (rest) this.pathname = rest;
    if (urlSlashedProtocol[lowerProto] && this.hostname && !this.pathname) this.pathname = "/";
    if (this.pathname || this.search) this.path = (this.pathname || "") + (this.search || "");
    this.href = this.format();
    return this;
  };
  Url.prototype.format = function format() {
    let auth = this.auth || "";
    if (auth) auth = encodeURIComponent(auth).replace(/%3A/i, ":") + "@";
    let protocol = this.protocol || "", pathname = this.pathname || "", hash = this.hash || "", host = "", query = "";
    if (this.host) host = auth + this.host;
    else if (this.hostname) {
      host = auth + (this.hostname.indexOf(":") === -1 ? this.hostname : "[" + this.hostname + "]");
      if (this.port) host += ":" + this.port;
    }
    if (this.query && typeof this.query === "object" && Object.keys(this.query).length) query = new G.URLSearchParams(this.query).toString();
    let search = this.search || (query && "?" + query) || "";
    if (protocol && protocol[protocol.length - 1] !== ":") protocol += ":";
    // only slashed protocols get the //; others only if slashes was set
    if (this.slashes || ((!protocol || urlSlashedProtocol[protocol]) && host.length > 0)) {
      host = "//" + host;
      if (pathname && pathname[0] !== "/") pathname = "/" + pathname;
    } else if (!host) host = "";
    if (hash && hash[0] !== "#") hash = "#" + hash;
    if (search && search[0] !== "?") search = "?" + search;
    pathname = pathname.replace(/[?#]/g, (m) => encodeURIComponent(m));
    search = search.replace(/#/g, "%23");
    return protocol + host + pathname + search + hash;
  };
  Url.prototype.parseHost = function parseHost() {
    let host = this.host;
    let port = urlPortPattern.exec(host);
    if (port) { port = port[0]; if (port !== ":") this.port = port.slice(1); host = host.slice(0, host.length - port.length); }
    if (host) this.hostname = host;
  };
  Url.prototype.resolve = function resolve(relative) { return this.resolveObject(urlParse(relative, false, true)).format(); };
  Url.prototype.resolveObject = function resolveObject(relative) {
    if (typeof relative === "string") { const rel = new Url(); rel.parse(relative, false, true); relative = rel; }
    const result = new Url();
    for (const tkey of Object.keys(this)) result[tkey] = this[tkey];
    // hash is always overridden, no matter what
    result.hash = relative.hash;
    if (relative.href === "") { result.href = result.format(); return result; }
    // hrefs like //foo/bar always cut to the protocol
    if (relative.slashes && !relative.protocol) {
      for (const rkey of Object.keys(relative)) if (rkey !== "protocol") result[rkey] = relative[rkey];
      if (urlSlashedProtocol[result.protocol] && result.hostname && !result.pathname) { result.pathname = "/"; result.path = result.pathname; }
      result.href = result.format();
      return result;
    }
    if (relative.protocol && relative.protocol !== result.protocol) {
      // changing to a non-slashed protocol takes relative wholesale;
      // file: drops the host; anything else must gain a host
      if (!urlSlashedProtocol[relative.protocol]) {
        for (const k of Object.keys(relative)) result[k] = relative[k];
        result.href = result.format();
        return result;
      }
      result.protocol = relative.protocol;
      if (!relative.host && !(relative.protocol === "file" || relative.protocol === "file:") && !urlHostlessProtocol[relative.protocol]) {
        const relPath = (relative.pathname || "").split("/");
        while (relPath.length && !(relative.host = relPath.shift()));
        if (!relative.host) relative.host = "";
        if (!relative.hostname) relative.hostname = "";
        if (relPath[0] !== "") relPath.unshift("");
        if (relPath.length < 2) relPath.unshift("");
        result.pathname = relPath.join("/");
      } else result.pathname = relative.pathname;
      result.search = relative.search;
      result.query = relative.query;
      result.host = relative.host || "";
      result.auth = relative.auth;
      result.hostname = relative.hostname || relative.host;
      result.port = relative.port;
      if (result.pathname || result.search) result.path = (result.pathname || "") + (result.search || "");
      result.slashes = result.slashes || relative.slashes;
      result.href = result.format();
      return result;
    }
    const isSourceAbs = result.pathname && result.pathname[0] === "/";
    const isRelAbs = relative.host || (relative.pathname && relative.pathname[0] === "/");
    let mustEndAbs = isRelAbs || isSourceAbs || (result.host && relative.pathname);
    const removeAllDots = mustEndAbs;
    let srcPath = (result.pathname && result.pathname.split("/")) || [];
    const relPath = (relative.pathname && relative.pathname.split("/")) || [];
    const psychotic = result.protocol && !urlSlashedProtocol[result.protocol];
    // for non-slashed urls, relative ../.. links can crawl up to the hostname
    if (psychotic) {
      result.hostname = ""; result.port = null;
      if (result.host) { if (srcPath[0] === "") srcPath[0] = result.host; else srcPath.unshift(result.host); }
      result.host = "";
      if (relative.protocol) {
        relative.hostname = null; relative.port = null; result.auth = null;
        if (relative.host) { if (relPath[0] === "") relPath[0] = relative.host; else relPath.unshift(relative.host); }
        relative.host = null;
      }
      mustEndAbs = mustEndAbs && (relPath[0] === "" || srcPath[0] === "");
    }
    if (isRelAbs) {
      if (relative.host || relative.host === "") { if (result.host !== relative.host) result.auth = null; result.host = relative.host; result.port = relative.port; }
      if (relative.hostname || relative.hostname === "") { if (result.hostname !== relative.hostname) result.auth = null; result.hostname = relative.hostname; }
      result.search = relative.search;
      result.query = relative.query;
      srcPath = relPath;
    } else if (relPath.length) {
      // it's relative: throw away the existing file and take the new path
      if (!srcPath) srcPath = [];
      srcPath.pop();
      srcPath = srcPath.concat(relPath);
      result.search = relative.search;
      result.query = relative.query;
    } else if (relative.search !== null && relative.search !== undefined) {
      // just pull out the search, like href='?foo'
      if (psychotic) {
        result.hostname = result.host = srcPath.shift();
        // auth can get stuck only in host (mailto:local1@domain1 style)
        const authInHost = result.host && result.host.indexOf("@") > 0 ? result.host.split("@") : false;
        if (authInHost) { result.auth = authInHost.shift(); result.hostname = result.host = authInHost.shift(); }
      }
      result.search = relative.search;
      result.query = relative.query;
      if (result.pathname !== null || result.search !== null) result.path = (result.pathname || "") + (result.search || "");
      result.href = result.format();
      return result;
    }
    if (!srcPath.length) {
      result.pathname = null;
      result.path = result.search ? "/" + result.search : null;
      result.href = result.format();
      return result;
    }
    // path ending in . / .. / "" keeps a trailing slash; strip single dots,
    // resolve double dots (`up` counts attempts to climb above the root)
    let last = srcPath.slice(-1)[0];
    const hasTrailingSlash = ((result.host || relative.host || srcPath.length > 1) && (last === "." || last === "..")) || last === "";
    let up = 0;
    for (let i = srcPath.length; i >= 0; i--) {
      last = srcPath[i];
      if (last === ".") srcPath.splice(i, 1);
      else if (last === "..") { srcPath.splice(i, 1); up++; }
      else if (up) { srcPath.splice(i, 1); up--; }
    }
    if (!mustEndAbs && !removeAllDots) for (; up--;) srcPath.unshift("..");
    if (mustEndAbs && srcPath[0] !== "" && (!srcPath[0] || srcPath[0][0] !== "/")) srcPath.unshift("");
    if (hasTrailingSlash && srcPath.join("/").slice(-1) !== "/") srcPath.push("");
    const isAbsolute = srcPath[0] === "" || (srcPath[0] && srcPath[0][0] === "/");
    // put the host back
    if (psychotic) {
      result.hostname = isAbsolute ? "" : srcPath.length ? srcPath.shift() : "";
      result.host = result.hostname;
      const authInHost = result.host && result.host.indexOf("@") > 0 ? result.host.split("@") : false;
      if (authInHost) { result.auth = authInHost.shift(); result.hostname = result.host = authInHost.shift(); }
    }
    mustEndAbs = mustEndAbs || (result.host && srcPath.length);
    if (mustEndAbs && !isAbsolute) srcPath.unshift("");
    if (srcPath.length > 0) result.pathname = srcPath.join("/");
    else { result.pathname = null; result.path = null; }
    if (result.pathname !== null || result.search !== null) result.path = (result.pathname || "") + (result.search || "");
    result.auth = relative.auth || result.auth;
    result.slashes = result.slashes || relative.slashes;
    result.href = result.format();
    return result;
  };
  const urlFormat = (urlObject, options) => {
    if (typeof urlObject === "string") urlObject = urlParse(urlObject);
    else if (typeof urlObject !== "object" || urlObject === null) { const e = new TypeError('The "urlObject" argument must be one of type object or string. Received ' + (urlObject === null ? "null" : typeof urlObject)); e.code = "ERR_INVALID_ARG_TYPE"; throw e; }
    if (urlObject instanceof G.URL) {
      // WHATWG URL + { auth, fragment, search, unicode } options
      let auth = true, fragment = true, search = true, unicode = false;
      if (options !== undefined) {
        if (options === null || typeof options !== "object") { const e = new TypeError('The "options" argument must be of type object. Received ' + (options === null ? "null" : typeof options)); e.code = "ERR_INVALID_ARG_TYPE"; throw e; }
        if (options.auth !== undefined) auth = !!options.auth;
        if (options.fragment !== undefined) fragment = !!options.fragment;
        if (options.search !== undefined) search = !!options.search;
        if (options.unicode !== undefined) unicode = !!options.unicode;
      }
      let ret = urlObject.protocol;
      if (urlObject.host !== "" || urlObject.protocol === "file:") {
        ret += "//";
        if (auth && (urlObject.username || urlObject.password)) { ret += urlObject.username; if (urlObject.password) ret += ":" + urlObject.password; ret += "@"; }
        ret += unicode ? urlMod.domainToUnicode(urlObject.hostname) : urlObject.hostname;
        if (urlObject.port) ret += ":" + urlObject.port;
      }
      ret += urlObject.pathname;
      if (search && urlObject.search) ret += urlObject.search;
      if (fragment && urlObject.hash) ret += urlObject.hash;
      return ret;
    }
    if (!(urlObject instanceof Url)) return Url.prototype.format.call(urlObject);
    return urlObject.format();
  };
  const urlToHttpOptions = (u) => {
    const o = {
      protocol: u.protocol,
      hostname: typeof u.hostname === "string" && u.hostname[0] === "[" ? u.hostname.slice(1, -1) : u.hostname,
      hash: u.hash, search: u.search, pathname: u.pathname,
      path: (u.pathname || "") + (u.search || ""), href: u.href,
    };
    if (u.port !== "" && u.port !== null && u.port !== undefined) o.port = Number(u.port);
    if (u.username || u.password) o.auth = decodeURIComponent(u.username || "") + ":" + decodeURIComponent(u.password || "");
    return o;
  };
  const urlMod = {
    URL: G.URL, URLSearchParams: G.URLSearchParams, Url,
    fileURLToPath: (u) => {
      u = String(u !== null && typeof u === "object" && "href" in u ? u.href : u);
      if (!/^file:/i.test(u)) { const e = new TypeError("The URL must be of scheme file"); e.code = "ERR_INVALID_URL_SCHEME"; throw e; }
      let p = u.replace(/^file:\/\//i, "").replace(/^file:/i, "");
      const cut = p.search(/[?#]/); if (cut >= 0) p = p.slice(0, cut);
      try { p = decodeURIComponent(p); } catch (e) {}
      if (p[0] !== "/") p = "/" + p;
      return p;
    },
    pathToFileURL: (p) => {
      p = String(p);
      // node: the path is resolved against cwd (normalizing . / ..); a trailing
      // slash on the input stays on the resolved path.
      let r = p;
      try { r = M.path.resolve(p); } catch (e) {}
      if (p[p.length - 1] === "/" && r[r.length - 1] !== "/") r += "/";
      const s = "file://" + (r[0] === "/" ? "" : "/") + encodeURI(r.replace(/%/g, "%25")).replace(/[?#~]/g, (m) => (m === "?" ? "%3F" : m === "#" ? "%23" : "%7E"));
      try { return new G.URL(s); } catch (e) { return { href: s, pathname: r, toString() { return s; } }; }
    },
    parse: urlParse,
    format: urlFormat,
    resolve: (source, relative) => urlParse(source, false, true).resolve(relative),
    resolveObject: (source, relative) => (source ? urlParse(source, false, true).resolveObject(relative) : relative),
    urlToHttpOptions,
    domainToASCII: (d) => { if (d == null) return d; d = String(d); if (badDomain(d)) return ""; if (d.toLowerCase().split(".").some((l) => /^xn--/i.test(l) && /[^\x00-\x7F]/.test(l))) return ""; try { return puny.toASCII(d.toLowerCase()); } catch (e) { return ""; } },
    domainToUnicode: (d) => { if (d == null) return d; d = String(d); if (badDomain(d)) return ""; if (d.toLowerCase().split(".").some((l) => /^xn--/i.test(l) && /[^\x00-\x7F]/.test(l))) return ""; try { return puny.toUnicode(d.toLowerCase()); } catch (e) { return ""; } },
  };
  def(["url"], urlMod);

  // ---- querystring ----
  def(["querystring"], {
    parse: (s) => { const o = {}; String(s || "").split("&").forEach((p) => { if (!p) return; const i = p.indexOf("="); const k = decodeURIComponent(i < 0 ? p : p.slice(0, i)); const v = i < 0 ? "" : decodeURIComponent(p.slice(i + 1)); if (k in o) { if (Array.isArray(o[k])) o[k].push(v); else o[k] = [o[k], v]; } else o[k] = v; }); return o; },
    stringify: (o) => Object.keys(o || {}).map((k) => { const v = o[k]; return Array.isArray(v) ? v.map((x) => encodeURIComponent(k) + "=" + encodeURIComponent(x)).join("&") : encodeURIComponent(k) + "=" + encodeURIComponent(v); }).join("&"),
    escape: encodeURIComponent, unescape: decodeURIComponent,
  });

  // ---- net / tls / dns / http / http2 / worker_threads / zlib / sqlite (module
  // shapes so files LOAD; real socket/compression/subprocess behaviour DEFERRED —
  // the exports throw or no-op honestly rather than fake results) ----
  const notImpl = (name) => () => { throw new Error(name + " is not implemented yet in mbun"); };
  class Socket extends EventEmitter { constructor(o) { super(); this.allowHalfOpen = !!(o && o.allowHalfOpen); this.connecting = false; this.destroyed = false; this.readable = true; this.writable = true; } connect() { return this; } write() { return true; } end() { return this; } destroy() { this.destroyed = true; return this; } setTimeout() { return this; } setNoDelay() { return this; } setKeepAlive() { return this; } ref() { return this; } unref() { return this; } }
  class Server extends EventEmitter { listen(...a) { const cb = a[a.length - 1]; if (typeof cb === "function") cb(); this.emit("listening"); return this; } close(cb) { if (cb) cb(); return this; } address() { return { port: 0, address: "127.0.0.1" }; } ref() { return this; } unref() { return this; } }
  def(["net"], { Socket, Server, createServer: () => new Server(), createConnection: () => new Socket(), connect: () => new Socket(), isIP: () => 0, isIPv4: () => false, isIPv6: () => false, getDefaultAutoSelectFamily: () => false, setDefaultAutoSelectFamily: () => {}, getDefaultAutoSelectFamilyAttemptTimeout: () => 250, setDefaultAutoSelectFamilyAttemptTimeout: () => {}, BlockList: class BlockList {
    // Rules live in a shared array so a structured clone of a BlockList shares
    // state with the original (matches bun/node native-handle clone semantics).
    constructor() { this._rules = []; }
    addAddress(a, family) { this._rules.push({ t: "addr", a: String(a && a.address ? a.address : a), f: family || "ipv4" }); }
    addRange(s, e, family) { this._rules.push({ t: "range", s: String(s && s.address ? s.address : s), e: String(e && e.address ? e.address : e), f: family || "ipv4" }); }
    addSubnet(net, prefix, family) { this._rules.push({ t: "subnet", n: String(net && net.address ? net.address : net), p: prefix >>> 0, f: family || "ipv4" }); }
    check(addr, family) {
      const a = String(addr && addr.address ? addr.address : addr);
      const v4 = (x) => { const p = x.split("."); if (p.length !== 4) return -1; let n = 0; for (const s of p) { const d = +s; if (!(d >= 0 && d <= 255)) return -1; n = n * 256 + d; } return n; };
      const an = v4(a);
      for (const r of this._rules) {
        if (r.t === "addr" && r.a === a) return true;
        if (an < 0) continue;
        if (r.t === "range") { const s = v4(r.s), e = v4(r.e); if (s >= 0 && e >= 0 && an >= s && an <= e) return true; }
        if (r.t === "subnet") { const n = v4(r.n); if (n >= 0 && r.p <= 32 && (r.p === 0 || (an >>> (32 - r.p)) === (n >>> (32 - r.p)))) return true; }
      }
      return false;
    }
    get rules() { return this._rules.map((r) => r.t === "addr" ? "Address: " + r.f.toUpperCase() + " " + r.a : r.t === "range" ? "Range: " + r.f.toUpperCase() + " " + r.s + "-" + r.e : "Subnet: " + r.f.toUpperCase() + " " + r.n + "/" + r.p); }
  } });
  class TLSServer extends Server { addContext() {} setSecureContext() {} getTicketKeys() { return Buffer.alloc(48); } setTicketKeys() {} }
  def(["tls"], { connect: () => new Socket(), createServer: (o, cb) => { const s = new TLSServer(); if (typeof (cb || o) === "function") s.on("secureConnection", cb || o); return s; }, TLSSocket: Socket, Server: TLSServer, createSecureContext: (o) => ({ context: {}, ...o }), getCiphers: () => ["aes128-gcm-sha256", "aes256-gcm-sha384"], rootCertificates: [], DEFAULT_MIN_VERSION: "TLSv1.2", DEFAULT_MAX_VERSION: "TLSv1.3", checkServerIdentity: () => undefined });
  const dnsPromises = { lookup: () => Promise.resolve({ address: "127.0.0.1", family: 4 }), resolve: () => Promise.resolve(["127.0.0.1"]), resolve4: () => Promise.resolve(["127.0.0.1"]), resolve6: () => Promise.resolve(["::1"]), reverse: () => Promise.resolve(["localhost"]), Resolver: class Resolver {} };
  def(["dns"], { lookup: (h, o, cb) => { const f = cb || o; if (typeof f === "function") f(null, "127.0.0.1", 4); }, resolve: (h, cb) => { if (typeof cb === "function") cb(null, ["127.0.0.1"]); }, resolve4: (h, cb) => { if (typeof cb === "function") cb(null, ["127.0.0.1"]); }, reverse: (h, cb) => { if (typeof cb === "function") cb(null, ["localhost"]); }, promises: dnsPromises, Resolver: class Resolver {}, ADDRCONFIG: 1024, V4MAPPED: 2048 });
  def(["dns/promises"], dnsPromises);
  const httpMod = { Server, ServerResponse: Writable, IncomingMessage: Readable, createServer: () => new Server(), request: notImpl("http.request"), get: notImpl("http.get"), Agent: class Agent {}, globalAgent: {}, STATUS_CODES: {}, METHODS: ["GET", "POST", "PUT", "DELETE", "PATCH", "HEAD", "OPTIONS"] };
  def(["http"], httpMod); def(["https"], httpMod);
  def(["http2"], { connect: notImpl("http2.connect"), createServer: () => new Server(), createSecureServer: () => new Server(), constants: {} });
  def(["worker_threads"], { Worker: class Worker extends EventEmitter { postMessage() {} terminate() { return Promise.resolve(); } ref() {} unref() {} }, isMainThread: true, parentPort: null, threadId: 0, workerData: null, get MessageChannel() { return G.MessageChannel; }, get MessagePort() { return G.MessagePort; }, BroadcastChannel: class BroadcastChannel extends EventEmitter { postMessage() {} close() {} }, receiveMessageOnPort: (port) => { if (port && port._queue && port._queue.length) return { message: port._queue.shift() }; return undefined; }, markAsUntransferable: () => {}, moveMessagePortToContext: (p) => p, setEnvironmentData: () => {}, getEnvironmentData: () => undefined, SHARE_ENV: Symbol("nodejs.worker_threads.SHARE_ENV") });
  // ---- zlib (real: DEFLATE/zlib/gzip via __mbunZlibNative → mbun.core.compress;
  // payloads cross the native boundary as base64, converted here to Buffer) ----
  const ZN = globalThis.__mbunZlibNative;
  const zToU8 = (d) => { if (typeof d === "string") return G.Buffer.from(d, "utf8"); if (d instanceof ArrayBuffer || (G.SharedArrayBuffer && d instanceof G.SharedArrayBuffer)) return new Uint8Array(d); if (ArrayBuffer.isView(d)) return new Uint8Array(d.buffer, d.byteOffset, d.byteLength); throw new TypeError("Received an instance of " + (d === null ? "null" : typeof d) + " where a buffer was expected"); };
  const zB64 = (u8) => { let s = ""; for (let i = 0; i < u8.length; i += 8192) s += String.fromCharCode.apply(null, u8.subarray(i, Math.min(i + 8192, u8.length))); return G.btoa(s); };
  const zErr = (e) => { const err = e instanceof Error ? e : new Error(String(e)); err.code = "Z_DATA_ERROR"; err.errno = -3; return err; };
  const zNum = (opts, k, d) => opts && typeof opts[k] === "number" ? opts[k] : d;
  // node enforces kMaxLength across ALL codecs (lib/zlib.js → ERR_BUFFER_TOO_LARGE).
  const zCap = (out, opts) => { const maxLen = opts && typeof opts.maxOutputLength === "number" ? opts.maxOutputLength : (M.buffer && M.buffer.kMaxLength); if (maxLen && out.length > maxLen) { const e = new RangeError("Cannot create a Buffer larger than " + maxLen + " bytes"); e.code = "ERR_BUFFER_TOO_LARGE"; throw e; } return out; };
  // Bytes cross as a Uint8Array, not base64: the base64 bridge cost ~6x the
  // payload in transient strings per crossing and a 150 MB deflateRawSync was
  // OOM-killed. ZN.compress/decompress answer a Uint8Array for a typed-array
  // input (base64 string in, base64 string out is still supported).
  const zSync = (op, fmt) => (data, opts) => { const level = zNum(opts, "level", -1), wbits = zNum(opts, "windowBits", 15), memLevel = zNum(opts, "memLevel", 8), strategy = zNum(opts, "strategy", 0); const inp = zToU8(data); let r; try { r = op === "c" ? ZN.compress(inp, fmt, level, wbits, memLevel, strategy) : ZN.decompress(inp, fmt, wbits); } catch (e) { throw zErr(e); } const out = typeof r === "string" ? G.Buffer.from(r, "base64") : G.Buffer.from(r.buffer, r.byteOffset, r.byteLength);const maxLen = opts && typeof opts.maxOutputLength === "number" ? opts.maxOutputLength : (M.buffer && M.buffer.kMaxLength); if (op === "d" && maxLen && out.length > maxLen) { const e = new RangeError("Cannot create a Buffer larger than " + maxLen + " bytes"); e.code = "ERR_BUFFER_TOO_LARGE"; throw e; } return out; };
  const zAsync = (sync) => (data, opts, cb) => { if (typeof opts === "function") { cb = opts; opts = undefined; } if (typeof cb !== "function") throw new TypeError("The callback argument must be of type function"); G.queueMicrotask(() => { try { cb(null, sync(data, opts)); } catch (e) { cb(e); } }); };
  const deflateSync = zSync("c", "zlib"), inflateSync = zSync("d", "zlib"), gzipSync = zSync("c", "gzip"), gunzipSync = zSync("d", "gzip"), deflateRawSync = zSync("c", "raw"), inflateRawSync = zSync("d", "raw"), unzipSync = zSync("d", "auto");
  // brotli (native BrotliEncoder/Decoder via __mbunZlibNative). node forwards
  // quality/lgwin/mode through opts.params keyed by BROTLI_PARAM_* (MODE=0,
  // QUALITY=1, LGWIN=2). All return Buffer.
  const brP = (opts) => { const p = (opts && opts.params) || {}; return [typeof p[1] === "number" ? p[1] : -1, typeof p[2] === "number" ? p[2] : 0, typeof p[0] === "number" ? p[0] : 0]; };
  const brotliCompressSync = (data, opts) => { const q = brP(opts); let r; try { r = ZN.brotliCompress(zB64(zToU8(data)), q[0], q[1], q[2]); } catch (e) { throw zErr(e); } return G.Buffer.from(r, "base64"); };
  const brotliDecompressSync = (data, opts) => { let r; try { r = ZN.brotliDecompress(zB64(zToU8(data))); } catch (e) { throw zErr(e); } return zCap(G.Buffer.from(r, "base64"), opts); };
  // zstd (native ZSTD_compress/decompress). level via opts.level or
  // opts.params[ZSTD_c_compressionLevel=100]; default 3.
  const zstdLvl = (opts) => { const l = zNum(opts, "level", -2); if (l !== -2) return l; const p = opts && opts.params; return p && typeof p[100] === "number" ? p[100] : 3; };
  const zstdCompressSync = (data, opts) => { let r; try { r = ZN.zstdCompress(zB64(zToU8(data)), zstdLvl(opts)); } catch (e) { throw zErr(e); } return G.Buffer.from(r, "base64"); };
  const zstdDecompressSync = (data, opts) => { let r; try { r = ZN.zstdDecompress(zB64(zToU8(data))); } catch (e) { throw zErr(e); } return zCap(G.Buffer.from(r, "base64"), opts); };
  // DEFERRED: streaming Transform classes (createGzip/createBrotliCompress/…,
  // and the zlib.Deflate/Brotli/Zstd class hierarchy). Real chunked streaming
  // needs the node threadpool _handle lifecycle AND a bounded native inflate
  // loop (the current mbun.compress zlib_inflate spins forever on truncated
  // input instead of erroring — see report). Until then createX stays notImpl
  // so callers fail fast rather than hang.
  const zConstants = { Z_NO_FLUSH: 0, Z_PARTIAL_FLUSH: 1, Z_SYNC_FLUSH: 2, Z_FULL_FLUSH: 3, Z_FINISH: 4, Z_BLOCK: 5, Z_TREES: 6, Z_OK: 0, Z_STREAM_END: 1, Z_NEED_DICT: 2, Z_ERRNO: -1, Z_STREAM_ERROR: -2, Z_DATA_ERROR: -3, Z_MEM_ERROR: -4, Z_BUF_ERROR: -5, Z_VERSION_ERROR: -6, Z_NO_COMPRESSION: 0, Z_BEST_SPEED: 1, Z_BEST_COMPRESSION: 9, Z_DEFAULT_COMPRESSION: -1, Z_FILTERED: 1, Z_HUFFMAN_ONLY: 2, Z_RLE: 3, Z_FIXED: 4, Z_DEFAULT_STRATEGY: 0, Z_MIN_WINDOWBITS: 8, Z_MAX_WINDOWBITS: 15, Z_DEFAULT_WINDOWBITS: 15, Z_MIN_CHUNK: 64, Z_MAX_CHUNK: Infinity, Z_DEFAULT_CHUNK: 16384, Z_MIN_MEMLEVEL: 1, Z_MAX_MEMLEVEL: 9, Z_DEFAULT_MEMLEVEL: 8, Z_MIN_LEVEL: -1, Z_MAX_LEVEL: 9, Z_DEFAULT_LEVEL: -1, ZLIB_VERNUM: 0x12b0, DEFLATE: 1, INFLATE: 2, GZIP: 3, GUNZIP: 4, DEFLATERAW: 5, INFLATERAW: 6, UNZIP: 7, BROTLI_DECODE: 8, BROTLI_ENCODE: 9, ZSTD_COMPRESS: 10, ZSTD_DECOMPRESS: 11,
    BROTLI_OPERATION_PROCESS: 0, BROTLI_OPERATION_FLUSH: 1, BROTLI_OPERATION_FINISH: 2, BROTLI_OPERATION_EMIT_METADATA: 3,
    BROTLI_PARAM_MODE: 0, BROTLI_PARAM_QUALITY: 1, BROTLI_PARAM_LGWIN: 2, BROTLI_PARAM_LGBLOCK: 3, BROTLI_PARAM_DISABLE_LITERAL_CONTEXT_MODELING: 4, BROTLI_PARAM_SIZE_HINT: 5, BROTLI_PARAM_LARGE_WINDOW: 6, BROTLI_PARAM_NPOSTFIX: 7, BROTLI_PARAM_NDIRECT: 8,
    BROTLI_MODE_GENERIC: 0, BROTLI_MODE_TEXT: 1, BROTLI_MODE_FONT: 2, BROTLI_DEFAULT_MODE: 0,
    BROTLI_MIN_QUALITY: 0, BROTLI_MAX_QUALITY: 11, BROTLI_DEFAULT_QUALITY: 11,
    BROTLI_MIN_WINDOW_BITS: 10, BROTLI_MAX_WINDOW_BITS: 24, BROTLI_LARGE_MAX_WINDOW_BITS: 30, BROTLI_DEFAULT_WINDOW: 22,
    BROTLI_MIN_INPUT_BLOCK_BITS: 16, BROTLI_MAX_INPUT_BLOCK_BITS: 24,
    BROTLI_DECODER_RESULT_ERROR: 0, BROTLI_DECODER_RESULT_SUCCESS: 1, BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT: 2, BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT: 3,
    ZSTD_e_continue: 0, ZSTD_e_flush: 1, ZSTD_e_end: 2, ZSTD_c_compressionLevel: 100, ZSTD_d_windowLogMax: 100, ZSTD_CLEVEL_DEFAULT: 3, ZSTD_MIN_CLEVEL: -131072, ZSTD_MAX_CLEVEL: 22 };
  def(["zlib"], { deflateSync, inflateSync, gzipSync, gunzipSync, deflateRawSync, inflateRawSync, unzipSync, deflate: zAsync(deflateSync), inflate: zAsync(inflateSync), gzip: zAsync(gzipSync), gunzip: zAsync(gunzipSync), deflateRaw: zAsync(deflateRawSync), inflateRaw: zAsync(inflateRawSync), unzip: zAsync(unzipSync), crc32: (data, v) => ZN.crc32(zB64(zToU8(data)), (v || 0) >>> 0) >>> 0, createGzip: () => new Transform(), createGunzip: () => new Transform(), createDeflate: () => new Transform(), createInflate: () => new Transform(), createDeflateRaw: () => new Transform(), createInflateRaw: () => new Transform(), createUnzip: () => new Transform(), brotliCompressSync, brotliDecompressSync, brotliCompress: zAsync(brotliCompressSync), brotliDecompress: zAsync(brotliDecompressSync), zstdCompressSync, zstdDecompressSync, zstdCompress: zAsync(zstdCompressSync), zstdDecompress: zAsync(zstdDecompressSync), createBrotliCompress: () => new Transform(), createBrotliDecompress: () => new Transform(), createZstdCompress: () => new Transform(), createZstdDecompress: () => new Transform(), constants: zConstants, ...zConstants });
  if (G.Bun && typeof G.Bun.deflateSync === "undefined") {
    // bun semantics: Bun.deflateSync/inflateSync are RAW deflate (zlib.test.js
    // "deflate_with_headers" reaches for node:zlib instead); gzipSync is the
    // gzip container; all return Uint8Array. inflateSync also accepts a
    // zlib-wrapped stream (lenient fallback).
    const asU8 = (b) => new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
    G.Bun.deflateSync = (data, opts) => asU8(deflateRawSync(data, opts));
    G.Bun.inflateSync = (data) => { try { return asU8(inflateRawSync(data)); } catch (e) { return asU8(inflateSync(data)); } };
    G.Bun.gzipSync = (data, opts) => asU8(gzipSync(data, opts));
    G.Bun.gunzipSync = (data) => asU8(gunzipSync(data));
    // zstd is the exception: bun's JSZstd::{compress,decompress}{,_sync} return a
    // node Buffer (JSValue::create_buffer in src/runtime/api/BunObject.rs), not a
    // plain Uint8Array, so `.toString()` decodes UTF-8 instead of joining bytes.
    G.Bun.zstdCompressSync = (data, opts) => zstdCompressSync(data, opts);
    G.Bun.zstdDecompressSync = (data, opts) => zstdDecompressSync(data, opts);
    G.Bun.zstdCompress = async (data, opts) => zstdCompressSync(data, opts);
    G.Bun.zstdDecompress = async (data, opts) => zstdDecompressSync(data, opts);
  }
  // ---- Bun.ArrayBufferSink (ref: src/runtime/webcore/ArrayBufferSink.rs) ----
  // A byte accumulator: write() appends string(UTF-8)/ArrayBuffer/TypedArray
  // chunks; end() returns the whole buffer (ArrayBuffer, or Uint8Array when
  // started with { asUint8Array: true }) and marks the sink done; in streaming
  // mode flush()/end() return the bytes accumulated since the last drain.
  if (G.Bun && typeof G.Bun.ArrayBufferSink === "undefined") {
    // TextEncoder is registered later in this bootstrap, so resolve it lazily
    // per call (`globalThis.TextEncoder`) rather than at block-eval time.
    let ABS_TE = null;
    const absToBytes = (data) => {
      if (typeof data === "string") { if (!ABS_TE) ABS_TE = new G.TextEncoder(); return ABS_TE.encode(data); }
      if (data instanceof ArrayBuffer) return new Uint8Array(data);
      if (typeof SharedArrayBuffer !== "undefined" && data instanceof SharedArrayBuffer) return new Uint8Array(data);
      if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
      throw new TypeError("Expected string, ArrayBuffer, or ArrayBufferView");
    };
    class ArrayBufferSink {
      #chunks = [];
      #len = 0;
      #asUint8Array = false;
      #streaming = false;
      #done = false;
      start(options) {
        this.#chunks = [];
        this.#len = 0;
        this.#done = false;
        if (options) {
          this.#asUint8Array = !!options.asUint8Array;
          this.#streaming = !!options.stream;
        }
      }
      write(data) {
        const b = absToBytes(data);
        this.#chunks.push(b);
        this.#len += b.byteLength;
        return b.byteLength;
      }
      #drain() {
        const out = new Uint8Array(this.#len);
        let off = 0;
        for (const c of this.#chunks) { out.set(c, off); off += c.byteLength; }
        this.#chunks = [];
        this.#len = 0;
        return this.#asUint8Array ? out : out.buffer;
      }
      flush() {
        if (!this.#streaming) return 0;
        return this.#drain();
      }
      end() {
        if (this.#done) return this.#asUint8Array ? new Uint8Array(0) : new ArrayBuffer(0);
        this.#done = true;
        return this.#drain();
      }
      ref() {}
      unref() {}
    }
    G.Bun.ArrayBufferSink = ArrayBufferSink;
  }
  // ---- bun:sqlite (real: __mbunSqliteNative → mbun.sqlite sqlite3 backend).
  // The Database/Statement classes live here; param parsing (positional ? /
  // named $x,:x,@x, strict/ugly key shapes) and row shaping are JS; the native
  // bridge does open/prepare/step/bind/exec. Values cross as native JS types. ----
  {
    const SQ = globalThis.__mbunSqliteNative;
    const isTA = ArrayBuffer.isView;
    const hasOwn = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
    const INT64_MAX = 9223372036854775807n, INT64_MIN = -9223372036854775808n;
    const constants = {
      SQLITE_OPEN_READONLY: 0x1, SQLITE_OPEN_READWRITE: 0x2, SQLITE_OPEN_CREATE: 0x4,
      SQLITE_OPEN_URI: 0x40, SQLITE_OPEN_MEMORY: 0x80, SQLITE_OPEN_NOMUTEX: 0x8000,
      SQLITE_OPEN_FULLMUTEX: 0x10000, SQLITE_OPEN_SHAREDCACHE: 0x20000,
      SQLITE_OPEN_PRIVATECACHE: 0x40000, SQLITE_OPEN_WAL: 0x80000,
      SQLITE_PREPARE_PERSISTENT: 0x1, SQLITE_PREPARE_NORMALIZE: 0x2, SQLITE_PREPARE_NO_VTAB: 0x4,
    };
    class SQLiteError extends Error {
      constructor(message, code, errno) { super(message); this.name = "SQLiteError"; if (code !== undefined) this.code = code; if (errno !== undefined) this.errno = errno; }
      static [Symbol.hasInstance](instance) { return instance != null && instance.name === "SQLiteError"; }
    }
    const NOT_FOUND = Symbol("nf");
    const isReadOnly = (sql) => /^\s*(?:select|pragma|with|explain|values)\b/i.test(sql) || /\breturning\b/i.test(sql);
    // Extract bind parameters (?, ?N, $x, :x, @x) in appearance order with the
    // sqlite 1-based position sqlite would assign (auto = max+1; ?N explicit;
    // named reuse first position). Skips string/quoted-ident/comment spans.
    const parseParams = (sql) => {
      const tokens = []; const nameIdx = new Map(); let maxIdx = 0; let i = 0; const n = sql.length;
      const isWord = (c) => c && (c >= "a" && c <= "z" || c >= "A" && c <= "Z" || c >= "0" && c <= "9" || c === "_");
      while (i < n) {
        const c = sql[i];
        if (c === "'" || c === '"' || c === "`") { const q = c; i++; while (i < n) { if (sql[i] === q) { if (sql[i + 1] === q) { i += 2; continue; } i++; break; } i++; } continue; }
        if (c === "[") { i++; while (i < n && sql[i] !== "]") i++; i++; continue; }
        if (c === "-" && sql[i + 1] === "-") { i += 2; while (i < n && sql[i] !== "\n") i++; continue; }
        if (c === "/" && sql[i + 1] === "*") { i += 2; while (i < n && !(sql[i] === "*" && sql[i + 1] === "/")) i++; i += 2; continue; }
        if (c === "?") {
          i++; let d = ""; while (i < n && sql[i] >= "0" && sql[i] <= "9") { d += sql[i]; i++; }
          let idx; if (d) { idx = parseInt(d, 10); if (idx > maxIdx) maxIdx = idx; } else { idx = ++maxIdx; }
          tokens.push({ index: idx, raw: d ? "?" + d : "?", bare: null }); continue;
        }
        if (c === ":" && sql[i + 1] === ":") { i += 2; continue; }
        if (c === ":" || c === "@" || c === "$") {
          let j = i + 1; while (j < n && isWord(sql[j])) j++;
          if (j > i + 1) { const raw = sql.slice(i, j); let idx = nameIdx.get(raw); if (idx === undefined) { idx = ++maxIdx; nameIdx.set(raw, idx); } tokens.push({ index: idx, raw, bare: sql.slice(i + 1, j) }); i = j; continue; }
        }
        i++;
      }
      return { count: maxIdx, tokens };
    };
    const lookup = (obj, t) => {
      if (hasOwn(obj, t.raw)) return obj[t.raw];
      if (t.bare != null && hasOwn(obj, t.bare)) return obj[t.bare];
      if (hasOwn(obj, t.index - 1)) return obj[t.index - 1];
      return NOT_FOUND;
    };
    // Build the positional values array native binds (position p → element p-1).
    const resolveBind = (parsed, rawArgs, strict, safe) => {
      const count = parsed.count; const pos = new Array(count);
      if (count > 0 && rawArgs.length > 0) {
        const a0 = rawArgs[0];
        if (a0 !== null && typeof a0 === "object" && !isTA(a0)) {
          if (Array.isArray(a0)) {
            if (a0.length < count) throw new SQLiteError(`SQLite query expected ${count} values, received ${a0.length}`, "SQLITE_ERROR");
            for (let p = 1; p <= count; p++) pos[p - 1] = a0[p - 1];
          } else {
            for (const t of parsed.tokens) {
              const v = lookup(a0, t);
              if (v === NOT_FOUND) { if (strict) throw new SQLiteError(`Missing parameter "${t.bare != null ? t.bare : t.raw}"`, "SQLITE_ERROR"); pos[t.index - 1] = null; }
              else pos[t.index - 1] = v;
            }
          }
        } else {
          if (rawArgs.length < count) throw new SQLiteError(`SQLite query expected ${count} values, received ${rawArgs.length}`, "SQLITE_ERROR");
          for (let p = 1; p <= count; p++) pos[p - 1] = rawArgs[p - 1];
        }
      }
      for (let i = 0; i < count; i++) {
        const v = pos[i]; const t = typeof v;
        if (v === undefined) { pos[i] = null; continue; }
        if (v === null || t === "boolean" || t === "number" || t === "string") continue;
        if (t === "bigint") { if (v > INT64_MAX || v < INT64_MIN) { if (safe) throw new RangeError(`BigInt value '${v}' is out of range`); pos[i] = Number(v); } continue; }
        if (isTA(v) || v instanceof ArrayBuffer) continue;
        throw new TypeError("Unsupported type used in query parameter. Expected boolean, number, bigint, string, or TypedArray.");
      }
      return pos;
    };
    class Statement {
      #db; #sql; #parsed; #strict; #safe; #finalized = false; #asClass = null; #meta = null; #lastParams = null; #hasExecuted = false; native;
      constructor(db, sql, strict, safe) {
        this.#db = db; this.#sql = sql; this.#parsed = parseParams(sql); this.#strict = !!strict; this.#safe = !!safe;
        const self = this;
        this.native = {
          get columns() { self.#ensureMeta(); return self.#meta ? self.#meta.columns.slice() : []; },
          get columnsCount() { self.#ensureMeta(); return self.#meta ? self.#meta.columns.length : 0; },
          get paramsCount() { return self.#parsed.count; },
          get columnTypes() { return self.columnTypes; },
          get declaredTypes() { return self.declaredTypes; },
          get safeIntegers() { return self.#safe; }, set safeIntegers(v) { self.#safe = !!v; },
          // bun renders the bound parameters in (sqlite3_expanded_sql), it does
          // not echo the raw SQL back.
          toString() { return SQ.expandedSQL(self.#db.handle, self.#sql, self.#lastParams || []); },
        };
      }
      #exec(rawArgs) {
        if (this.#finalized) throw new SQLiteError("Statement is finalized. To fix this, prepare a new statement.", "SQLITE_MISUSE");
        if (this.#sql.trim() === "") throw new SQLiteError("Query contained no valid SQL statement; likely empty query.", "SQLITE_ERROR");
        const pos = resolveBind(this.#parsed, rawArgs, this.#strict, this.#safe);
        this.#lastParams = pos;  // toString() renders these back in
        let res;
        try { res = SQ.run(this.#db.handle, this.#sql, pos); }
        catch (e) { this.#db.uncache(this.#sql); throw e; }
        // A pure read never modifies rows; sqlite3_changes is stale from the
        // prior write, so report 0 (bun uses a total_changes delta).
        if (/^\s*(?:select|pragma|explain|values)\b/i.test(this.#sql)) res.changes = 0;
        this.#meta = res; return res;
      }
      #ensureMeta() { if (!this.#meta && isReadOnly(this.#sql)) { try { this.#exec([]); } catch { /* metadata probe */ } } }
      #shape(cols, row) {
        if (this.#asClass) { const o = new this.#asClass(); for (let j = 0; j < cols.length; j++) o[cols[j]] = row[j]; return o; }
        const o = {}; for (let j = 0; j < cols.length; j++) o[cols[j]] = row[j]; return o;
      }
      get(...a) { const r = this.#exec(a); this.#hasExecuted = true; return r.values.length ? this.#shape(r.columns, r.values[0]) : null; }
      all(...a) { const r = this.#exec(a); this.#hasExecuted = true; return r.values.map((row) => this.#shape(r.columns, row)); }
      values(...a) { const r = this.#exec(a); this.#hasExecuted = true; return r.values; }
      raw(...a) { const r = this.#exec(a); this.#hasExecuted = true; return r.values; }
      run(...a) { const r = this.#exec(a); this.#hasExecuted = true; return { changes: r.changes, lastInsertRowid: r.lastInsertRowid }; }
      *iterate(...a) { const r = this.#exec(a); this.#hasExecuted = true; for (const row of r.values) yield this.#shape(r.columns, row); }
      [Symbol.iterator]() { return this.iterate(); }
      as(ClassType) {
        if (ClassType === undefined) { this.#asClass = null; return this; }
        if (ClassType === null || typeof ClassType !== "function") throw new Error("Expected class to be a constructor or undefined");
        if (ClassType.prototype === undefined) throw new Error("Expected a constructor");
        if (typeof ClassType.prototype !== "object") throw new Error("Expected a constructor prototype to be an object");
        this.#asClass = ClassType; return this;
      }
      safeIntegers(v) { if (v === undefined) return this.#safe; this.#safe = !!v; return this; }
      get columnNames() { this.#ensureMeta(); return this.#meta ? this.#meta.columns.slice() : []; }
      get columnTypes() {
        if (!isReadOnly(this.#sql)) throw new Error("columnTypes is not available for non-read-only statements. Use declaredTypes instead.");
        this.#ensureMeta(); return this.#meta ? this.#meta.types.slice() : [];
      }
      get declaredTypes() { if (!this.#hasExecuted) throw new Error("Statement must be executed before accessing declaredTypes"); return this.#meta ? this.#meta.declaredTypes.slice() : []; }
      get paramsCount() { return this.#parsed.count; }
      get isFinalized() { return this.#finalized; }
      finalize() { this.#finalized = true; }
      toString() { return SQ.expandedSQL(this.#db.handle, this.#sql, this.#lastParams || []); }
      [Symbol.dispose]() { if (!this.#finalized) this.finalize(); }
      toJSON() { return { sql: this.#sql, isFinalized: this.#finalized, paramsCount: this.#parsed.count, columnNames: this.columnNames }; }
    }
    const cacheCountSym = Symbol.for("Bun.Database.cache.count");
    class Database {
      #handle; #strict; #safe; #closed = false; #cache = new Map(); filename;
      get [cacheCountSym]() { return this.#cache.size; }
      constructor(filenameGiven, options) {
        if (typeof filenameGiven !== "undefined" && typeof filenameGiven !== "string") {
          if (isTA(filenameGiven)) throw new Error("Database deserialize is not implemented yet in mbun");
          throw new TypeError(`Expected 'filename' to be a string, got '${typeof filenameGiven}'`);
        }
        let filename = typeof filenameGiven === "string" ? filenameGiven.trim() : ":memory:";
        let readonly = false, create = true;
        if (options && typeof options === "object") {
          if ("readOnly" in options) throw new TypeError('Misspelled option "readOnly" should be "readonly"');
          readonly = !!options.readonly; create = !!options.create;
          if (options.readwrite) readonly = false;
          // bun: an options object with only strict/safeIntegers keeps the rw+create default.
          if (!options.readonly && !options.create && !options.readwrite) create = true;
          this.#strict = !!options.strict; this.#safe = !!options.safeIntegers;
        } else if (typeof options === "number") {
          readonly = (options & constants.SQLITE_OPEN_READONLY) !== 0;
          create = (options & constants.SQLITE_OPEN_CREATE) !== 0;
        }
        const anonymous = filename === "" || filename === ":memory:";
        if (anonymous && readonly) throw new Error("Cannot open an anonymous database in read-only mode.");
        this.#handle = SQ.open(anonymous ? ":memory:" : filename, readonly, create);
        this.filename = filename;
      }
      get handle() { return this.#handle; }
      get inTransaction() { return SQ.inTransaction(this.#handle); }
      static open(filename, options) { return new Database(filename, options); }
      prepare(query) { return new Statement(this, query, this.#strict, this.#safe); }
      query(query) {
        if (typeof query !== "string") throw new TypeError(`Expected 'query' to be a string, got '${typeof query}'`);
        if (query.length === 0) throw new Error("SQL query cannot be empty.");
        const cached = this.#cache.get(query); if (cached && !cached.isFinalized) return cached;
        const stmt = this.prepare(query); if (this.#cache.size < 20) this.#cache.set(query, stmt); return stmt;
      }
      run(query, ...params) { return this.prepare(query).run(...params); }
      close() {
        if (this.#closed) return; this.#closed = true;
        for (const s of this.#cache.values()) { try { s.finalize(); } catch { /* ignore */ } }
        this.#cache.clear(); SQ.close(this.#handle);
      }
      clearQueryCache() { for (const s of this.#cache.values()) { try { s.finalize(); } catch { /* ignore */ } } this.#cache.clear(); }
      uncache(sql) { this.#cache.delete(sql); }
      loadExtension() { throw new Error("Database.loadExtension is not implemented yet in mbun"); }
      serialize() { throw new Error("Database.serialize is not implemented yet in mbun"); }
      static deserialize() { throw new Error("Database.deserialize is not implemented yet in mbun"); }
      fileControl() { throw new Error("Database.fileControl is not implemented yet in mbun"); }
      static setCustomSQLite() { return true; }
      [Symbol.dispose]() { this.close(); }
      transaction(fn) {
        if (typeof fn !== "function") throw new TypeError("Expected first argument to be a function");
        const db = this;
        const wrap = (mode) => function (...args) {
          const nested = db.inTransaction;
          const begin = nested ? "SAVEPOINT __mbun_sp" : (mode ? "BEGIN " + mode : "BEGIN");
          const commit = nested ? "RELEASE __mbun_sp" : "COMMIT";
          const rollback = nested ? "ROLLBACK TO __mbun_sp" : "ROLLBACK";
          db.run(begin);
          try { const r = fn.apply(this, args); db.run(commit); return r; }
          catch (e) { if (db.inTransaction) { db.run(rollback); if (nested) db.run("RELEASE __mbun_sp"); } throw e; }
        };
        const dflt = wrap("");
        const props = { default: { value: dflt }, deferred: { value: wrap("DEFERRED") }, immediate: { value: wrap("IMMEDIATE") }, exclusive: { value: wrap("EXCLUSIVE") }, database: { value: this, enumerable: true } };
        Object.defineProperties(dflt, props); Object.defineProperties(props.deferred.value, props); Object.defineProperties(props.immediate.value, props); Object.defineProperties(props.exclusive.value, props);
        return dflt;
      }
    }
    Database.prototype.exec = Database.prototype.run;
    Database.MAX_QUERY_CACHE_SIZE = 20;
    const mod = { __esModule: true, Database, Statement, constants, SQLiteError, default: Database };
    M["bun:sqlite"] = { Database, Statement, constants, SQLiteError, default: mod };
  }
  const timersPromises = {
    setTimeout: (ms, v) => new Promise((r) => G.setTimeout(() => r(v), ms)),
    setInterval: (ms, v) => ({ [Symbol.asyncIterator]() { return { next: () => new Promise((r) => G.setTimeout(() => r({ value: v, done: false }), ms)) }; } }),
    setImmediate: (v) => new Promise((r) => G.setImmediate(() => r(v))),
  };
  def(["timers"], { setTimeout: G.setTimeout, clearTimeout: G.clearTimeout, setInterval: G.setInterval, clearInterval: G.clearInterval, setImmediate: G.setImmediate, clearImmediate: G.clearImmediate, promises: timersPromises });
  def(["timers/promises"], timersPromises); M["timers/promises"] = M["node:timers/promises"] = timersPromises;
  // ---- readline (minimal: createInterface with question/close over events) ----
  const readlineMod = {
    createInterface(opts) {
      const rl = new EventEmitter();
      rl.question = (q, cb) => { if (typeof cb === "function") cb(""); };
      rl.close = () => rl.emit("close");
      rl.write = () => {}; rl.pause = () => rl; rl.resume = () => rl; rl.setPrompt = () => {}; rl.prompt = () => {};
      rl[Symbol.asyncIterator] = function () { let done = false; return { next: () => Promise.resolve({ value: undefined, done: true }) }; };
      return rl;
    },
    clearLine: () => true, cursorTo: () => true, moveCursor: () => true, emitKeypressEvents: () => {},
    Interface: EventEmitter,
  };
  def(["readline"], readlineMod);
  def(["readline/promises"], { createInterface: readlineMod.createInterface });
  // node global alias + node:stream/web (WHATWG stream classes as a module)
  if (typeof G.global === "undefined") G.global = G;
  def(["stream/web"], { get ReadableStream() { return G.ReadableStream; }, get WritableStream() { return G.WritableStream; }, get TransformStream() { return G.TransformStream; }, get TextEncoderStream() { return G.TextEncoderStream; }, get TextDecoderStream() { return G.TextDecoderStream; }, get ByteLengthQueuingStrategy() { return G.ByteLengthQueuingStrategy || class {}; }, get CountQueuingStrategy() { return G.CountQueuingStrategy || class {}; } });
  // ---- async_hooks (synchronous AsyncLocalStorage — correct under a single call
  // stack; no continuation propagation across the virtual-timer loop) ----
  class AsyncLocalStorage {
    constructor() { this._s = undefined; }
    run(store, cb, ...a) { const prev = this._s; this._s = store; try { return cb(...a); } finally { this._s = prev; } }
    getStore() { return this._s; }
    enterWith(s) { this._s = s; }
    exit(cb, ...a) { const prev = this._s; this._s = undefined; try { return cb(...a); } finally { this._s = prev; } }
    disable() { this._s = undefined; }
  }
  class AsyncResource { constructor(type) { this.type = type; } runInAsyncScope(fn, thisArg, ...a) { return fn.apply(thisArg, a); } emitDestroy() { return this; } bind(fn) { return fn; } asyncId() { return 0; } triggerAsyncId() { return 0; } }
  def(["async_hooks"], { AsyncLocalStorage, AsyncResource, createHook: () => ({ enable() { return this; }, disable() { return this; } }), executionAsyncId: () => 1, triggerAsyncId: () => 0, executionAsyncResource: () => ({}) });
  // node:v8 — structured (de)serialize via a JSON round-trip; heap stats stubbed.
  def(["v8"], {
    serialize: (v) => Buffer.from(JSON.stringify(v) ?? "null", "utf8"),
    deserialize: (b) => JSON.parse(Buffer.from(b).toString("utf8")),
    getHeapStatistics: () => ({ total_heap_size: 4194304, total_heap_size_executable: 262144, total_physical_size: 4194304, total_available_size: 1073741824, used_heap_size: 2097152, heap_size_limit: 2147483648, malloced_memory: 8192, peak_malloced_memory: 1048576, does_zap_garbage: 0, number_of_native_contexts: 1, number_of_detached_contexts: 0 }),
    getHeapSpaceStatistics: () => [], setFlagsFromString: () => {}, writeHeapSnapshot: () => "", takeCoverage: () => {}, stopCoverage: () => {},
    Serializer: class Serializer { writeValue() {} releaseBuffer() { return Buffer.alloc(0); } writeHeader() {} }, Deserializer: class Deserializer { readHeader() {} readValue() {} },
    DefaultSerializer: class DefaultSerializer {}, DefaultDeserializer: class DefaultDeserializer {},
    promiseHooks: { createHook: () => () => {}, onInit: () => () => {}, onSettled: () => () => {}, onBefore: () => () => {}, onAfter: () => () => {} },
    startupSnapshot: { isBuildingSnapshot: () => false, addSerializeCallback: () => {}, addDeserializeCallback: () => {}, setDeserializeMainFunction: () => {} },
  });
  // process.getBuiltinModule (node ≥20.16) — resolves through the builtin table
  { const gbm = (n) => { n = String(n).replace(/^node:/, ""); return M[n]; };
    if (G.process && !G.process.getBuiltinModule) G.process.getBuiltinModule = gbm;
    else if (!G.process) { let done = false; Object.defineProperty(G, "process", { configurable: true, set(v) { delete G.process; G.process = v; if (v && !v.getBuiltinModule) v.getBuiltinModule = gbm; }, get() { return undefined; } }); } }
  // bun:ffi — shape only (native FFI DEFERRED); files that merely import it load.
  const ffiThrow = () => { throw new Error("bun:ffi is not implemented yet in mbun"); };
  M["bun:ffi"] = { dlopen: ffiThrow, CString: class CString extends String {}, ptr: (v) => globalThis.__mbunFfiPtr(v), toArrayBuffer: ffiThrow, toBuffer: ffiThrow, read: {}, JSCallback: class JSCallback { constructor() { this.ptr = 0; } close() {} }, FFIType: { void: 0, bool: 1, char: 2, int8_t: 3, i8: 3, uint8_t: 4, u8: 4, int16_t: 5, i16: 5, uint16_t: 6, u16: 6, int32_t: 7, i32: 7, uint32_t: 8, u32: 8, int64_t: 9, i64: 9, uint64_t: 10, u64: 10, float: 11, f32: 11, double: 12, f64: 12, pointer: 13, ptr: 13, cstring: 14 }, suffix: "so", linkSymbols: ffiThrow, viewSource: () => "", CFunction: ffiThrow };
  // ---- node:test → the bun:test harness (installed per-run as globalThis.__mbunBT;
  // resolved lazily since js_builtins runs before the harness). node:test's
  // callbacks receive a context `t`; we pass the same fn through so `test(name, fn)`
  // works — the `t.*` sub-API is not modeled. ----
  const BT = () => G.__mbunBT || {};
  const __nodeTestRun = (a) => {
    const bt = BT();
    const name = a[0], b = a[1], c = a[2];
    const opts = (b && typeof b === "object") ? b : (c && typeof c === "object") ? c : null;
    const fn = typeof b === "function" ? b : (typeof c === "function" ? c : undefined);
    if (opts) {
      if (opts.skip) return bt.test.skip(name, fn);
      if (opts.todo) return bt.test.todo(name, fn);
      if (opts.only) return (bt.test.only || bt.test)(name, fn);
    }
    return bt.test(name, fn);
  };
  const nodeTest = (...a) => __nodeTestRun(a);
  nodeTest.test = (...a) => __nodeTestRun(a);
  nodeTest.describe = (...a) => BT().describe(...a);
  nodeTest.suite = (...a) => BT().describe(...a);
  nodeTest.it = (...a) => __nodeTestRun(a);
  nodeTest.before = (...a) => BT().beforeAll(...a);
  nodeTest.after = (...a) => BT().afterAll(...a);
  nodeTest.beforeEach = (...a) => BT().beforeEach(...a);
  nodeTest.afterEach = (...a) => BT().afterEach(...a);
  nodeTest.mock = { fn: (...a) => BT().mock(...a), method: (...a) => BT().spyOn(...a), restoreAll() {}, reset() {} };
  nodeTest.skip = (...a) => BT().test.skip(...a);
  nodeTest.todo = (...a) => BT().test.todo(...a);
  nodeTest.only = (...a) => BT().test.only ? BT().test.only(...a) : BT().test(...a);
  def(["test"], nodeTest);
  { const um = M["util"] || M["node:util"]; if (um) def(["util/types"], um.types || {}); }
  def(["constants"], { E2BIG: 7, EACCES: 13, O_RDONLY: 0, O_WRONLY: 1, O_RDWR: 2, O_CREAT: 64, S_IFMT: 61440, S_IFREG: 32768, S_IFDIR: 16384 });

  // ---- bun-internal modules (bun:jsc / bun:internal-for-testing) ----
  // Diagnostic/internal surfaces bun's test harness imports at load time. Real
  // semantics where knowable (isASANEnabled=false for this build); otherwise
  // best-effort so the harness loads (tests asserting internals fail honestly).
  // Shared synthetic-allocation-limit guard (bun: jsc::virtual_machine::
  // synthetic_allocation_limit / bun_core STRING_ALLOCATION_LIMIT). Surfaces
  // that would materialize `byteLength` bytes as a JS string / typed array
  // fail with the same message real bun/JSC produces instead of allocating.
  G.__mbunCheckAllocLimit = (byteLength, kind) => {
    if (byteLength <= (G.__mbunSyntheticAllocationLimit || 0xFFFFFFFF)) return;
    if (kind === "text") throw new RangeError("Cannot create a string longer than 2^32-1 characters");
    if (kind === "json") throw new RangeError("Cannot parse a JSON string longer than 2^32-1 characters");
    throw new RangeError("Out of memory");
  };
  M["bun:jsc"] = {
    heapStats:() => ({ heapSize: 0, heapCapacity: 0, objectCount: 0, protectedObjectCount: 0,
                        globalObjectCount: 0, objectTypeCounts: {}, protectedObjectTypeCounts: {} }),
    memoryUsage: () => ({ current: 0, peak: 0 }),
    getRandomSeed: () => 0, setRandomSeed: () => {},
    // Real collect+sweep (JSGarbageCollect) — see runtime/engine.inc __mbunGcNative.
    gcAndSweep: () => (G.__mbunGcNative ? G.__mbunGcNative(true) : 0),
    fullGC: () => (G.__mbunGcNative ? G.__mbunGcNative(true) : 0),
    edenGC: () => (G.__mbunGcNative ? G.__mbunGcNative(false) : 0),
    isRope: () => false, describe: (v) => String(v), describeArray: () => "",
    serialize: (v) => v, deserialize: (v) => v, drainMicrotasks: () => {},
    getProtectedObjects: () => [], totalCompileTime: () => 0,
    // bun BunJSCModule.h:527 — the calling frame's source origin as a URL.
    // Bound to the native directly: any JS wrapper would itself become the
    // "caller" (its source origin is the builtins blob, i.e. empty).
    callerSourceOrigin: G.__mbunJscInternalsNative.callerSourceOriginNative,
    // bun BunJSCModule.h:931 — the cell's own estimated size. Objects whose
    // native-equivalent storage lives in a builtins closure (Performance's
    // entry buffer, AbortSignal's abort-algorithm list — WebCore members that
    // bun accounts for in memoryCost()) publish it through the
    // Symbol.for("mbun.memoryCost") hook, so the total stays comparable to
    // bun's estimatedSizeInBytes() for the same object.
    estimateShallowMemoryUsageOf: (value) => {
      let n = G.__mbunJscInternalsNative.estimateShallowMemoryUsageOfNative(value);
      if (value !== null && (typeof value === "object" || typeof value === "function")) {
        try {
          const cost = value[Symbol.for("mbun.memoryCost")];
          if (typeof cost === "function") n += cost.call(value) || 0;
        } catch (e) {}
      }
      return n;
    },
  };
  M["bun:internal-for-testing"] = {
    isASANEnabled: () => false,
    // canonicalizeIP (src/js/internal-for-testing.ts:16 → NodeTLS.cpp
    // Bun__canonicalizeIP): inet_pton/inet_ntop round trip; undefined for a
    // non-IP literal or a CIDR. Same native the node:tls IP-SAN check uses.
    canonicalizeIP: (...a) => {
      const N = G.__mbunNodeTlsNative;
      if (!N || typeof N.canonicalizeIP !== "function") return undefined;
      return N.canonicalizeIP(...a);  // spread so a 0-arg call still throws
    },
    // createStatsForIno(ino, bigint): builds a Stats whose .ino carries a u64
    // inode through the number path (static_cast<double>) or the bigint path
    // (static_cast<int64_t> == BigInt.asIntN(64, ino)). NFS inodes exceed
    // INT64_MAX and must not clamp. ref: src/js/internal-for-testing.ts.
    createStatsForIno: (ino, bigint) => {
      const b = BigInt(ino);
      return { ino: bigint ? BigInt.asIntN(64, b) : Number(b), mode: 0, size: 0,
        isFile: () => true, isDirectory: () => false, isSymbolicLink: () => false,
        isBlockDevice: () => false, isCharacterDevice: () => false, isFIFO: () => false, isSocket: () => false };
    },
    // Synthetic allocation limit (bun virtual_machine_exports.rs
    // Bun__setSyntheticAllocationLimitForTesting): clamped to >= 1 MiB, returns
    // the previous value. Read by the fs/Blob "would this allocation blow up?"
    // guards via globalThis.__mbunSyntheticAllocationLimit, so a test can force
    // a graceful ENOMEM/RangeError instead of a real multi-GB allocation.
    setSyntheticAllocationLimitForTesting: (limit) => {
      const prev = G.__mbunSyntheticAllocationLimit;
      const n = Number(limit);
      if (!Number.isFinite(n)) throw new TypeError("setSyntheticAllocationLimitForTesting expects a number");
      G.__mbunSyntheticAllocationLimit = Math.max(Math.trunc(n), 1024 * 1024);
      return prev;
    },
    // xxHash3ForTesting(bytes, seed?) — full-u64-seed XXH3_64bits (native).
    xxHash3ForTesting: G.__mbunXxHash3ForTesting,
    // bun internal-for-testing.ts:273 → socket_body.rs js_set_socket_options:
    // which 1=send(SO_SNDBUF)/2=recv(SO_RCVBUF), size in bytes.
    setSocketOptions: (socket, which, size) => {
      const fd = socket && socket._fd;
      if (typeof fd === "number" && fd >= 0 && G.__mbunNetNative && G.__mbunNetNative.setSockBuf)
        G.__mbunNetNative.setSockBuf(fd, which | 0, size | 0);
    },
    // Regex-escape helpers (bun install package-name matching). Every regex
    // metacharacter is backslash-escaped; '-' → '\x2d'. The package-name variant
    // additionally maps '*' → '.*' (glob-style wildcard) instead of '\*'.
    escapeRegExp: (s) => String(s).replace(/[\\^$*+?.()|{}[\]-]/g, (c) => (c === "-" ? "\\x2d" : "\\" + c)),
    escapeRegExpForPackageNameMatching: (s) => String(s).replace(/[\\^$*+?.()|{}[\]-]/g, (c) => (c === "-" ? "\\x2d" : c === "*" ? ".*" : "\\" + c)),
    jscInternals: {
      isLatin1String: () => false,
      isUTF16String: G.__mbunJscInternalsNative.isUTF16String,
      getStringWidth: (s) => (typeof s === "string" ? s.length : 0),
    },
    npm_manifest_test_helpers: {},
    // socketFaultInjection (blueprint src/js/internal-for-testing.ts:323): available()
    // is false unless built with --socket-fault-injection=on, so the six
    // *-syscall-fault suites self-skip exactly like a release bun.
    socketFaultInjection: { available: () => false, set: () => false, clear: () => {} },
    // translateUVErrorToE / translateNtStatusToE (ibid:382/388): Windows-only Rust
    // fns; off-Windows the tests assert they are functions returning undefined.
    translateUVErrorToE: () => undefined,
    translateNtStatusToE: () => undefined,
    // sysErrorNameFromLibuv (ibid:394): Windows-only mapping; POSIX → undefined.
    sysErrorNameFromLibuv: () => undefined,
    // stringsInternals.toUTF16AllocSentinel (ibid:408): lossy UTF-8→UTF-16 with
    // U+FFFD per maximal subpart — exactly WHATWG TextDecoder's algorithm.
    stringsInternals: { toUTF16AllocSentinel: (b) => new TextDecoder().decode(b) },
    Bun: globalThis.Bun,
    internalSourceMap: globalThis.__mbunSourceMapNative,
    // highlightJavaScript/Redacted attached later (in the highlighter's scope).
    // shellInternals.parse — tagged template over the native mbun.shell parser;
    // interpolations become __bun_<i> JSObjRef markers (bun's own encoding).
    shellInternals: (() => {
      const SHN = globalThis.__mbunShellNative;
      if (!SHN) return undefined;
      return {
        parse: (strings, ...vals) => {
          let src = strings[0];
          // \x08 (SPECIAL_JS_CHAR) sentinel marks a JS-object interpolation so
          // the native lexer emits a JSObjRef (looks_like_js_obj_ref) instead
          // of lexing "__bun_N" as plain text. ref: modules/shell parser.cppm.
          for (let i = 0; i < vals.length; i++) src += "\x08__bun_" + i + strings[i + 1];
          const err = SHN.parseErrors(src, vals.length);
          if (err) throw new Error(err);
          return SHN.parse(src, vals.length);
        },
        // POSIX build: no shell builtin is compiled out (internal-for-testing.ts).
        builtinDisabled: () => false,
      };
    })(),
  };

  // ---- bun:test outside the runner ----------------------------------------
  // Real bun always resolves bun:test; outside `bun test` registrations are
  // inert. Delegate to the runner's __mbunBT when present, else expose a
  // registration-only surface so harness.ts (and fixtures importing it) load.
  // expect() matchers throw honestly instead of silently passing.
  if (!M["bun:test"]) {
    const mkChain = (fn) => { fn.skip = fn; fn.todo = fn; fn.only = fn; fn.failing = fn;
      fn.skipIf = () => fn; fn.todoIf = () => fn; fn.if = () => fn; fn.each = () => fn;
      fn.concurrent = fn; return fn; };
    const noop = mkChain(function test() {});
    const desc = mkChain(function describe(n, f) { if (typeof n === "function") n(); else if (typeof f === "function") f(); });
    const expectStub = function expect() {
      return new Proxy({}, { get: (_t, m) => () => { throw new Error("expect()." + String(m) + " is not available outside `bun test` (mbun run-mode stub)"); } });
    };
    expectStub.extend = () => {}; expectStub.any = (c) => ({ __any: c }); expectStub.anything = () => ({});
    const hook = () => {};
    // setSystemTime IS live outside `bun test` (bun installs the native
    // JSMock__jsSetSystemTime on the module regardless of the runner) — issue
    // 32793 pins the clock from `bun -e`. The Date patch is installed lazily on
    // the first call so an ordinary run keeps the untouched native Date.
    let sysTime = null;
    const setSystemTime = (v) => {
      if (v === undefined || v === null) { sysTime = null; return; }
      sysTime = (typeof v === "number") ? v : Number(v.valueOf());
      if (G.__mbunRunDatePatched) return;
      G.__mbunRunDatePatched = true;
      const RD = G.Date;
      const MbunDate = function Date(...args) {
        if (!new.target) return RD();                       // Date() → string
        const a = (args.length === 0 && sysTime !== null) ? [sysTime] : args;
        return Reflect.construct(RD, a, new.target);        // keeps `class X extends Date`
      };
      MbunDate.prototype = RD.prototype;
      Object.setPrototypeOf(MbunDate, RD);                  // UTC/parse/… statics
      MbunDate.now = function () { return sysTime === null ? RD.now() : sysTime; };
      G.Date = MbunDate;
    };
    Object.defineProperty(M, "bun:test", { enumerable: true, configurable: true,
      get() { return G.__mbunBT || { test: noop, it: noop, xit: noop.skip, xtest: noop.skip,
        describe: desc, xdescribe: desc, expect: expectStub,
        jest: { fn: (i) => i || (() => {}), setSystemTime: (v) => { setSystemTime(v); } },
        mock: (i) => i || (() => {}), spyOn: () => ({ mockRestore() {} }),
        setSystemTime: setSystemTime,
        beforeAll: hook, afterAll: hook, beforeEach: hook, afterEach: hook, setDefaultTimeout: hook }; } });
  }

  // ---- fs (real, via __mbunFsNative) ----
  const F = globalThis.__mbunFsNative;
  const toStr = (x) => {
    if (typeof x === "string") return x;
    // node accepts file:// URL *instances* everywhere a path goes
    // (getValidatedPath → fileURLToPath); plain strings are never URL-parsed.
    // pglite et al. pass `new URL("./x.data", import.meta.url)` into fs.
    if (x && typeof x === "object" && x.href !== undefined && x.protocol === "file:" &&
        typeof x.pathname === "string") {
      try { return decodeURIComponent(x.pathname); } catch { return x.pathname; }
    }
    return x && x.toString ? x.toString() : String(x);
  };
  const recur = (o) => !!(o && (o === true || o.recursive));
  // node getValidatedPath: a path must be a string, Buffer, or file: URL.
  // Anything else throws TypeError ERR_INVALID_ARG_TYPE *synchronously* (even
  // for the async fs.mkdir form). ref: lib/internal/fs/utils.js.
  const validatePath = (p, name) => {
    if (typeof p === "string") return;
    if (p && typeof p === "object") {
      if (ArrayBuffer.isView(p) || p instanceof ArrayBuffer) return; // Buffer
      if (p.href !== undefined && p.protocol === "file:" && typeof p.pathname === "string") return; // URL
    }
    const e = new TypeError('The "' + (name || "path") + '" argument must be of type string or an instance of Buffer or URL. Received ' + (p === null ? "null" : typeof p));
    e.code = "ERR_INVALID_ARG_TYPE";
    throw e;
  };
  // node mkdir options: number → mode; object → { recursive, mode }. recursive,
  // when present, must be a boolean (validateBoolean → TypeError). Returns the
  // (recursive, mode) tuple the native mkdir consumes; mode defaults to 0o777.
  const mkdirOpts = (o) => {
    let recursive = false, mode = 0o777;
    if (typeof o === "number") mode = o;
    else if (o === true) recursive = true;
    else if (o && typeof o === "object") {
      if ("recursive" in o && o.recursive !== undefined) {
        if (typeof o.recursive !== "boolean") {
          const e = new TypeError('The "options.recursive" argument must be of type boolean. Received ' + (o.recursive === null ? "null" : typeof o.recursive));
          e.code = "ERR_INVALID_ARG_TYPE";
          throw e;
        }
        recursive = o.recursive;
      }
      if (o.mode != null) mode = typeof o.mode === "string" ? parseInt(o.mode, 8) : (Number(o.mode) & 0o7777);
    }
    return [recursive, mode];
  };
  // POSIX-normalize a path (resolve "." / "..") — used to resolve a symlink's
  // relative target against dirname(src) the way node's fs.cp does.
  const pNormalize = (p) => {
    const abs = p[0] === "/";
    const out = [];
    for (const seg of p.split("/")) {
      if (seg === "" || seg === ".") continue;
      if (seg === "..") { if (out.length && out[out.length - 1] !== "..") out.pop(); else if (!abs) out.push(".."); }
      else out.push(seg);
    }
    return (abs ? "/" : "") + out.join("/");
  };
  const pDirname = (p) => { const i = p.lastIndexOf("/"); return i <= 0 ? (i === 0 ? "/" : ".") : p.slice(0, i); };
  const cpError = (code, msg, path2) => { const e = new Error(code + ": " + msg); e.code = code; e.path = path2; return e; };
  // node fs.cp / fs.cpSync (blueprint src/js/internal/fs/cp): recursive copy with
  // force/errorOnExist/filter/dereference options. Symlinks are preserved (raw
  // target, relative targets resolved to absolute against dirname(src) so the
  // copy never links back into the source tree). Modes are carried to the copy;
  // directories require { recursive: true }; FIFOs/sockets are rejected.
  const cpRec = (src, dest, o) => {
    o = o || {};
    const recursive = !!o.recursive;
    const force = o.force !== false;
    const errorOnExist = !!o.errorOnExist;
    const dereference = !!o.dereference;
    const filter = typeof o.filter === "function" ? o.filter : null;
    const walk = (s, d) => {
      if (filter && !filter(s, d)) return;
      const lst = F.stat(s, true); // lstat
      if (!dereference && lst && lst.isSymbolicLink && lst.isSymbolicLink()) {
        let target = F.readlink(s);
        if (target[0] !== "/") target = pNormalize(pDirname(s) + "/" + target);
        try { F.unlink(d); } catch (e) {}
        F.symlink(target, d);
        return;
      }
      if (lst && lst.isDirectory()) {
        if (!recursive) throw cpError("ERR_FS_EISDIR", "recursive must be true to copy a directory", s);
        F.mkdir(d, true, (lst.mode & 0o777) || 0o777);
        try { F.chmod(d, lst.mode & 0o777); } catch (e) {}
        for (const e of F.readdir(s)) walk(s + "/" + e, d + "/" + e);
        return;
      }
      if (lst && (lst.isFIFO() || lst.isSocket() || lst.isBlockDevice() || lst.isCharacterDevice())) {
        throw cpError("ERR_FS_CP_FIFO_PIPE", "cannot copy a FIFO", s);
      }
      if (F.exists(d)) {
        const dst = F.stat(d, true);
        if (dst && dst.isDirectory()) throw cpError("ERR_FS_CP_NON_DIR_TO_DIR", "cannot overwrite directory with non-directory", s);
        if (!force) {
          if (errorOnExist) throw cpError("ERR_FS_CP_EEXIST", "file already exists", d);
          return;
        }
      }
      F.copyFile(s, d);
      try { if (lst) F.chmod(d, lst.mode & 0o777); } catch (e) {}
    };
    walk(src, dest);
  };
  // bun node_fs.rs should_throw_out_of_memory_early_for_javascript: a read whose
  // *decoded* length would exceed the synthetic allocation limit fails with
  // ENOMEM instead of really allocating. Without this, readFileSync("/dev/zero")
  // (st_size 0 ⇒ read-to-EOF) grows forever and the process is OOM-killed.
  // The divisor is the worst-case byte→code-unit expansion of each encoding.
  const fsOomDivisor = (enc) => {
    switch (enc) {
      case "utf8": case "utf-8": case "utf16le": case "utf-16le": case "ucs2": case "ucs-2": return 4;
      case "hex": return 2;
      case "base64": case "base64url": return 3;
      default: return 1;
    }
  };
  const fsSynthLimit = () => G.__mbunSyntheticAllocationLimit || 0xFFFFFFFF;
  // Largest byte count still under the limit once decoded; past it → ENOMEM.
  const fsOomCap = (enc) => fsOomDivisor(enc) * (fsSynthLimit() + 1);
  const fsOomError = (path2) =>
    Object.assign(new Error("ENOMEM: not enough memory, read '" + path2 + "'"),
                  { code: "ENOMEM", errno: -12, syscall: "read", path: path2 });
  const fsMod = {
    // node fs.readFileSync: no encoding → Buffer (was wrongly a String).
    // Reads real bytes via the native fd path (binary-correct; F.readFile
    // UTF-8-decodes). node/bun return Buffer, so the corpus + the bundler
    // harness (which patches Buffer.prototype.toUnixString) needs this.
    readFileSync: (p, opts) => {
      const enc = typeof opts === "string" ? opts : (opts && opts.encoding);
      const FD = globalThis.__mbunFdNative;
      // node/bun accept a raw fd; it stays open (the caller owns it).
      const isFd = typeof p === "number";
      const path2 = isFd ? String(p) : toStr(p);
      const fd = isFd ? p : FD.open(path2, "r", 0o666);
      const cap = fsOomCap(enc);
      try {
        let size = (!isFd && F.stat(path2) && F.stat(path2).size) | 0;
        if (size <= 0) size = 65536;  // procfs / char devices report st_size 0 — read to EOF
        if (size > cap) throw fsOomError(path2);
        let u = new Uint8Array(size);
        let off = 0, n;
        for (;;) {
          if (off >= u.length) {
            if (u.length > cap) throw fsOomError(path2);
            const g = new Uint8Array(Math.min(u.length * 2, cap + 8192)); g.set(u); u = g;
          }
          n = FD.read(fd, u, off, u.length - off, -1);
          if (n <= 0) break;
          off += n;
          if (off > cap) throw fsOomError(path2);
        }
        const buf = Buffer.from(u.buffer, 0, off);
        return enc ? buf.toString(enc) : buf;
      } finally { if (!isFd) FD.close(fd); }
    },
    // Binary data must NOT cross the C-API string boundary (NUL/UTF-8 mangling):
    // typed arrays / ArrayBuffers write through the fd native path byte-exact.
    writeFileSync: (p, d, o) => {
      // node: options may be an encoding string or { encoding, mode, flag };
      // `mode` is the creation mode (default 0o666) and must be applied even
      // when the file already exists is false — a fresh file created with
      // mode 0o777 has to come out executable (cli/run/run-extensionless).
      const mode = (o && typeof o === "object" && o.mode != null)
        ? (typeof o.mode === "string" ? parseInt(o.mode, 8) : (Number(o.mode) & 0o7777))
        : null;
      if (ArrayBuffer.isView(d) || d instanceof ArrayBuffer) {
        const FD = globalThis.__mbunFdNative;
        const u = d instanceof ArrayBuffer ? new Uint8Array(d) : new Uint8Array(d.buffer, d.byteOffset, d.byteLength);
        const fd = FD.open(toStr(p), "w", mode == null ? 0o666 : mode);
        try { FD.write(fd, u, 0, u.byteLength, -1); } finally { FD.close(fd); }
        if (mode != null) { try { F.chmod(toStr(p), mode); } catch (e) {} }
        return;
      }
      F.writeFile(toStr(p), toStr(d));
      if (mode != null) { try { F.chmod(toStr(p), mode); } catch (e) {} }
    },
    appendFileSync: (p, d) => F.appendFile(toStr(p), toStr(d)),
    existsSync: (p) => F.exists(toStr(p)),
    mkdirSync: (p, o) => { validatePath(p); const [rec, mode] = mkdirOpts(o); return F.mkdir(toStr(p), rec, mode); },
    rmSync: (p, o) => F.rm(toStr(p), recur(o), !!(o && o.force)),
    rmdirSync: (p, o) => F.rm(toStr(p), recur(o), true),
    readdirSync: (p, o) => {
      p = toStr(p);
      const wft = !!(o && typeof o === "object" && o.withFileTypes);
      if (o && typeof o === "object" && o.recursive) {
        const r = F.readdirRecursive(p);
        if (!wft) return r.names;
        return r.names.map((rel, i) => {
          const slash = rel.lastIndexOf("/");
          const name = slash < 0 ? rel : rel.slice(slash + 1);
          const parent = slash < 0 ? p : p + "/" + rel.slice(0, slash);
          return new fsMod.Dirent(name, r.types[i], parent);
        });
      }
      const names = F.readdir(p);
      if (!wft) return names;
      return names.map((n) => { let t = 1; try { t = F.stat(p + "/" + n)._isDir ? 2 : 1; } catch (e) { t = 3; } return new fsMod.Dirent(n, t, p); });
    },
    // native stat builds a plain object; link it to fs.Stats.prototype so
    // `statSync(x) instanceof Stats` holds (node/bun: statSync shares the
    // Stats prototype). Own isFile()/mtimeMs/… still shadow.
    statSync: (p, o) => { const s = F.stat(toStr(p)); return (o && o.bigint) ? mkBigIntStats(s) : Object.setPrototypeOf(s, Stats.prototype); },
    lstatSync: (p, o) => { const s = F.stat(toStr(p), true); return (o && o.bigint) ? mkBigIntStats(s) : Object.setPrototypeOf(s, Stats.prototype); },
    fstatSync: (fd, o) => { const s = F.fstat(fd); return (o && o.bigint) ? mkBigIntStats(s) : Object.setPrototypeOf(s, Stats.prototype); },
    fstat: (fd, o, cb) => { const fn = cb || o; if (typeof fn === "function") fn(null, fsMod.fstatSync(fd)); },
    statfsSync: () => ({ type: 0, bsize: 4096, blocks: 0, bfree: 0, bavail: 0, files: 0, ffree: 0 }),
    createStatsForIno: (ino, mode) => ({ ino: ino || 0, mode: mode || 0o644, size: 0, isFile: () => (mode == null ? true : (mode & 0o170000) === 0o100000), isDirectory: () => (mode != null && (mode & 0o170000) === 0o040000), isSymbolicLink: () => false, isBlockDevice: () => false, isCharacterDevice: () => false, isFIFO: () => false, isSocket: () => false, mtime: new Date(0), atime: new Date(0), ctime: new Date(0), birthtime: new Date(0), mtimeMs: 0, atimeMs: 0, ctimeMs: 0, birthtimeMs: 0, uid: 0, gid: 0, dev: 0, nlink: 1, rdev: 0, blksize: 4096, blocks: 0 }),
    unlinkSync: (p) => F.unlink(toStr(p)),
    realpathSync: (p) => F.realpath(toStr(p)),
    renameSync: (a, b) => F.rename(toStr(a), toStr(b)),
    copyFileSync: (a, b) => F.copyFile(toStr(a), toStr(b)),
    mkdtempSync: (pre) => F.mkdtemp(toStr(pre)),
    // fd-level I/O: real descriptors over mbun.core.io (native pread/pwrite),
    // zero-copy typed-array boundary via __mbunFdNative.
    openSync: (p, flags, mode) => globalThis.__mbunFdNative.open(toStr(p), flags == null ? "r" : toStr(flags), typeof mode === "number" ? mode : 0o666),
    closeSync: (fd) => { globalThis.__mbunFdNative.close(fd); },
    readSync: (fd, buf, off, len, pos) => {
      if (off !== null && typeof off === "object") { const o = off; off = o.offset || 0; len = o.length; pos = o.position; }
      return globalThis.__mbunFdNative.read(fd, buf, off || 0, len == null ? buf.byteLength : len, pos == null ? -1 : Number(pos));
    },
    writeSync: (fd, buf, off, len, pos) => {
      if (typeof buf === "string") { const enc = typeof len === "string" ? len : "utf8"; buf = Buffer.from(buf, enc); if (typeof off === "number") pos = off; else pos = null; off = 0; len = buf.byteLength; }
      return globalThis.__mbunFdNative.write(fd, buf, off || 0, len == null ? buf.byteLength : len, pos == null ? -1 : Number(pos));
    },
    fsyncSync: () => {}, fdatasyncSync: () => {},
    read: (fd, buf, off, len, pos, cb) => { let fn = typeof buf === "function" ? buf : (typeof cb === "function" ? cb : (typeof pos === "function" ? pos : undefined)); if (typeof buf === "function") { fn(null, 0, undefined); return; } try { const n = fsMod.readSync(fd, buf, off, len, typeof pos === "function" ? null : pos); if (fn) fn(null, n, buf); } catch (e) { if (fn) fn(e); } },
    write: (fd, buf, off, len, pos, cb) => { let fn; const a = [off, len, pos, cb]; for (const x of a) if (typeof x === "function") { fn = x; break; } try { const n = fsMod.writeSync(fd, buf, typeof off === "function" ? undefined : off, typeof len === "function" ? undefined : len, typeof pos === "function" ? undefined : pos); if (fn) fn(null, n, buf); } catch (e) { if (fn) fn(e); } },
    // permission/owner/time metadata: no-ops (our fs has no perm model); access
    // checks existence; readlink resolves via realpath (we have no real symlinks).
    chmodSync: (p, m) => F.chmod(toStr(p), typeof m === "string" ? parseInt(m, 8) : (Number(m) & 0o7777)), fchmodSync: () => {}, lchmodSync: () => {},
    chownSync: () => {}, fchownSync: () => {}, lchownSync: () => {},
    utimesSync: (p, a, m) => { const s = (v) => v instanceof Date ? v.getTime() / 1000 : Number(v); F.utimes(toStr(p), s(a), s(m)); }, futimesSync: () => {}, lutimesSync: () => {},
    truncateSync: () => {}, ftruncateSync: () => {},
    accessSync: (p, mode) => { if (!F.exists(toStr(p))) { const e = new Error("ENOENT: no such file or directory, access '" + toStr(p) + "'"); e.code = "ENOENT"; e.errno = -2; e.path = toStr(p); throw e; } },
    readlinkSync: (p) => F.readlink(toStr(p)),
    readlink: (p, o, cb) => { const fn = cb || o; try { fn(null, F.readlink(toStr(p))); } catch (e) { fn(e); } },
    // file streams: our fs I/O is synchronous, so read pushes the whole content on
    // a microtask and write accumulates then flushes on end/close.
    createReadStream: (p, opts) => { const rs = new Readable(); rs.path = toStr(p); rs.bytesRead = 0; const enc = typeof opts === "string" ? opts : (opts && opts.encoding); G.queueMicrotask(() => { try { const data = F.readFile(toStr(p)); rs.emit("open", 3); rs.emit("ready"); const buf = Buffer.from(data); rs.bytesRead = buf.length; rs.push(enc ? buf.toString(enc) : buf); rs.push(null); rs.emit("close"); } catch (e) { e.code = e.code || "ENOENT"; rs.emit("error", e); } }); rs.close = (cb) => { if (cb) cb(); return rs; }; return rs; },
    createWriteStream: (p, opts) => { const ws = new Writable(); ws.path = toStr(p); ws.bytesWritten = 0; const parts = []; const enc = (opts && opts.encoding) || "utf8"; ws._write = (chunk, e, cb) => { const s = typeof chunk === "string" ? chunk : Buffer.from(chunk).toString(enc); parts.push(s); ws.bytesWritten += s.length; if (typeof (cb || e) === "function") (cb || e)(); }; const flush = () => { try { F.writeFile(toStr(p), parts.join("")); } catch (er) { ws.emit("error", er); } }; const superEnd = ws.end.bind(ws); ws.end = (chunk, e, cb) => { if (chunk != null && typeof chunk !== "function") ws._write(chunk, enc, null); flush(); G.queueMicrotask(() => { ws.emit("finish"); ws.emit("close"); }); const f = cb || (typeof e === "function" ? e : typeof chunk === "function" ? chunk : null); if (f) f(); return ws; }; ws.close = (cb) => { if (cb) cb(); return ws; }; G.queueMicrotask(() => { ws.emit("open", 3); ws.emit("ready"); }); return ws; },
    chmod: (p, m, cb) => { const fn = typeof m === "function" ? m : cb; try { if (typeof m !== "function") F.chmod(toStr(p), typeof m === "string" ? parseInt(m, 8) : (Number(m) & 0o7777)); if (typeof fn === "function") fn(null); } catch (e) { if (typeof fn === "function") fn(e); } },
    chown: (p, u, g, cb) => { const fn = cb || g; if (typeof fn === "function") fn(null); },
    utimes: (p, a, m, cb) => { const fn = cb || m; try { const s = (v) => v instanceof Date ? v.getTime() / 1000 : Number(v); F.utimes(toStr(p), s(a), s(m)); if (typeof fn === "function") fn(null); } catch (e) { if (typeof fn === "function") fn(e); } },
    access: (p, m, cb) => { const fn = cb || m; try { if (!F.exists(toStr(p))) throw Object.assign(new Error("ENOENT"), { code: "ENOENT" }); fn(null); } catch (e) { fn(e); } },
    symlink: (t, p2, a, cb) => { const fn = cb || (typeof a === "function" ? a : undefined); try { F.symlink(toStr(t), toStr(p2)); fn && fn(null); } catch (e) { fn && fn(e); } },
    symlinkSync: (target, path2) => F.symlink(toStr(target), toStr(path2)),
    cpSync: (src, dest, o) => cpRec(toStr(src), toStr(dest), o),
    cp: (src, dest, o, cb) => { const fn = typeof o === "function" ? o : cb; if (typeof fn !== "function") { const e = new TypeError('The "cb" argument must be of type function. Received ' + (fn === undefined ? "undefined" : typeof fn)); e.code = "ERR_INVALID_ARG_TYPE"; throw e; } try { cpRec(toStr(src), toStr(dest), typeof o === "object" ? o : undefined); fn(null); } catch (e) { fn(e); } },
    // node fs.globSync (blueprint bun-ref/src/js/internal/fs/glob.ts): yields
    // matched files AND directories, defaults cwd to process.cwd() (so it
    // tracks process.chdir), and — with withFileTypes — hands Dirents to both
    // the results and the exclude callback. Backed by Bun.Glob.scanSync with
    // { onlyFiles: false } (node's glob is not files-only).
    globSync: (pat, o) => {
      const cwd = (o && o.cwd) || (G.process && G.process.cwd ? G.process.cwd() : ".");
      const wft = !!(o && typeof o === "object" && o.withFileTypes);
      const pats = Array.isArray(pat) ? pat : [pat];
      const excl = (o && o.exclude) || [];
      const exArr = Array.isArray(excl) ? excl : [excl];
      const strGlobs = [], fnExcl = [];
      for (const e of exArr) { if (typeof e === "function") fnExcl.push(e); else strGlobs.push(new (G.Bun.Glob)(toStr(e))); }
      const seen = new Set(), rels = [];
      const add = (m) => { if (!seen.has(m)) { seen.add(m); rels.push(m); } };
      for (const p of pats) {
        const ps = toStr(p);
        for (const m of new (G.Bun.Glob)(ps).scanSync({ cwd, onlyFiles: false })) add(m);
        // node/minimatch: a trailing "/**" also matches the prefix directory
        // itself ("a/**" matches "a"). Bun.Glob.scan intentionally omits this,
        // so fs.glob re-scans the prefix to recover the node semantics.
        if (ps.endsWith("/**")) { const pre = ps.slice(0, -3); if (pre) for (const m of new (G.Bun.Glob)(pre).scanSync({ cwd, onlyFiles: false })) add(m); }
      }
      const direntOf = (rel) => {
        const slash = rel.lastIndexOf("/");
        const name = slash < 0 ? rel : rel.slice(slash + 1);
        const parent = slash < 0 ? cwd : cwd + "/" + rel.slice(0, slash);
        // The Dirent must be stat'ed relative to options.cwd, not process.cwd().
        let t = 1; try { t = F.stat(cwd + "/" + rel)._isDir ? 2 : 1; } catch (e) { t = 1; }
        return new fsMod.Dirent(name, t, parent);
      };
      // Pass 1: which rels the exclude filter rejects (exclude sees a Dirent
      // when withFileTypes). A rejected directory prunes its whole subtree.
      const excluded = new Set(), cache = new Map();
      for (const rel of rels) {
        let d = null;
        if (wft) { d = direntOf(rel); cache.set(rel, d); }
        if (strGlobs.some((g) => g.match(rel)) || fnExcl.some((f) => f(wft ? d : rel))) excluded.add(rel);
      }
      // Pass 2: drop excluded entries and anything under an excluded directory.
      const out = [];
      for (const rel of rels) {
        if (excluded.has(rel)) continue;
        let pruned = false;
        for (const ex of excluded) { if (rel.startsWith(ex + "/")) { pruned = true; break; } }
        if (pruned) continue;
        out.push(wft ? cache.get(rel) : rel);
      }
      return out;
    },
    glob: (pat, o, cb) => { const fn = typeof o === "function" ? o : cb; if (typeof fn !== "function") throw new TypeError("The \"callback\" argument must be of type function."); try { fn(null, fsMod.globSync(pat, typeof o === "object" ? o : undefined)); } catch (e) { fn(e); } },
    readFile: (p, a, b) => { const cb = b || a; try { cb(null, F.readFile(toStr(p))); } catch (e) { cb(e); } },
    writeFile: (p, d, a, b) => { const cb = b || a; try { F.writeFile(toStr(p), toStr(d)); cb(null); } catch (e) { cb(e); } },
    mkdir: (p, a, b) => { validatePath(p); const cb = b || a; const [rec, mode] = mkdirOpts(typeof a === "object" || typeof a === "number" ? a : null); try { const __r = F.mkdir(toStr(p), rec, mode); cb(null, __r); } catch (e) { cb(e); } },
    // callback-style async (node passes (err, result); mirror the *Sync impls).
    stat: (p, a, b) => { const cb = typeof a === "function" ? a : b; try { cb(null, fsMod.statSync(toStr(p))); } catch (e) { cb(e); } },
    lstat: (p, a, b) => { const cb = typeof a === "function" ? a : b; try { cb(null, fsMod.lstatSync(toStr(p))); } catch (e) { cb(e); } },
    readdir: (p, a, b) => { const cb = typeof a === "function" ? a : b; try { cb(null, F.readdir(toStr(p))); } catch (e) { cb(e); } },
    unlink: (p, cb) => { try { F.unlink(toStr(p)); cb && cb(null); } catch (e) { cb && cb(e); } },
    realpath: (p, a, b) => { const cb = typeof a === "function" ? a : b; try { cb(null, F.realpath(toStr(p))); } catch (e) { cb(e); } },
    rename: (a2, b2, cb) => { try { F.rename(toStr(a2), toStr(b2)); cb && cb(null); } catch (e) { cb && cb(e); } },
    copyFile: (a2, b2, m, cb) => { const fn = typeof m === "function" ? m : cb; try { F.copyFile(toStr(a2), toStr(b2)); fn && fn(null); } catch (e) { fn && fn(e); } },
    appendFile: (p, d, a, b) => { const cb = typeof a === "function" ? a : b; try { F.appendFile(toStr(p), toStr(d)); cb && cb(null); } catch (e) { cb && cb(e); } },
    rm: (p, a, b) => { const cb = typeof a === "function" ? a : b; const o = typeof a === "object" ? a : undefined; try { F.rm(toStr(p), recur(o), !!(o && o.force)); cb && cb(null); } catch (e) { cb && cb(e); } },
    rmdir: (p, a, b) => { const cb = typeof a === "function" ? a : b; try { F.rm(toStr(p), recur(typeof a === "object" ? a : null), true); cb && cb(null); } catch (e) { cb && cb(e); } },
    exists: (p, cb) => { try { cb && cb(F.exists(toStr(p))); } catch (e) { cb && cb(false); } },
    truncate: (p, a, b) => { const cb = typeof a === "function" ? a : b; if (typeof cb === "function") cb(null); },
    open: (p, a, b, c) => { const args = [a, b, c]; const cb = args.reverse().find((x) => typeof x === "function"); try { const fd = globalThis.__mbunFdNative.open(toStr(p), typeof a === "string" ? a : "r", typeof b === "number" ? b : 0o666); cb && cb(null, fd); } catch (e) { cb && cb(e); } },
    close: (fd, cb) => { try { globalThis.__mbunFdNative.close(fd); cb && cb(null); } catch (e) { cb && cb(e); } },
    constants: { F_OK: 0, R_OK: 4, W_OK: 2, X_OK: 1, O_RDONLY: 0, O_WRONLY: 1, O_RDWR: 2, O_CREAT: 64, O_TRUNC: 512,
      UV_DIRENT_UNKNOWN: 0, UV_DIRENT_FILE: 1, UV_DIRENT_DIR: 2, UV_DIRENT_LINK: 3, UV_DIRENT_FIFO: 4, UV_DIRENT_SOCKET: 5, UV_DIRENT_CHAR: 6, UV_DIRENT_BLOCK: 7 },
  };
  fsMod.realpathSync.native = fsMod.realpathSync;
  fsMod.ReadStream = Readable; fsMod.WriteStream = Writable;
  // fs.Dirent — libuv DT_* dirent type checks (matches node's Dirent; type values
  // per UV_DIRENT_* above). isFIFO must be strictly type===4 so DT_UNKNOWN (0)
  // returns false for every predicate (regression issue #24129).
  // node DEP0180 Stats constructor (callable with or without `new`); field
  // order per lib/internal/fs/utils.js. statSync builds its own result objects
  // natively — this is the public `fs.Stats` class for `new Stats(...)` users.
  function Stats(dev, mode, nlink, uid, gid, rdev, blksize, ino, size, blocks,
                 atimeMs, mtimeMs, ctimeMs, birthtimeMs) {
    if (!(this instanceof Stats)) return new Stats(dev, mode, nlink, uid, gid, rdev, blksize, ino, size, blocks, atimeMs, mtimeMs, ctimeMs, birthtimeMs);
    this.dev = dev; this.mode = mode; this.nlink = nlink; this.uid = uid; this.gid = gid;
    this.rdev = rdev; this.blksize = blksize; this.ino = ino; this.size = size; this.blocks = blocks;
    this.atimeMs = atimeMs; this.mtimeMs = mtimeMs; this.ctimeMs = ctimeMs; this.birthtimeMs = birthtimeMs;
    this.atime = new Date(atimeMs); this.mtime = new Date(mtimeMs);
    this.ctime = new Date(ctimeMs); this.birthtime = new Date(birthtimeMs);
  }
  const S_IFMT = 0o170000;
  Stats.prototype.isFile = function () { return (this.mode & S_IFMT) === 0o100000; };
  Stats.prototype.isDirectory = function () { return (this.mode & S_IFMT) === 0o040000; };
  Stats.prototype.isSymbolicLink = function () { return (this.mode & S_IFMT) === 0o120000; };
  Stats.prototype.isBlockDevice = function () { return (this.mode & S_IFMT) === 0o060000; };
  Stats.prototype.isCharacterDevice = function () { return (this.mode & S_IFMT) === 0o020000; };
  Stats.prototype.isFIFO = function () { return (this.mode & S_IFMT) === 0o010000; };
  Stats.prototype.isSocket = function () { return (this.mode & S_IFMT) === 0o140000; };
  fsMod.Stats = Stats;
  // node fs BigInt Stats (statSync/lstatSync/fstatSync { bigint: true }): every
  // numeric field is a BigInt, plus nanosecond *Ns fields. Built from the plain
  // native stat object; birthtimeNs derived from birthtimeMs so it satisfies
  // node's birthtimeNs >= birthtimeMs*1e6 invariant. ref: lib/internal/fs/utils.js.
  function BigIntStats(dev, mode, nlink, uid, gid, rdev, blksize, ino, size, blocks,
                       atimeMs, mtimeMs, ctimeMs, birthtimeMs, atimeNs, mtimeNs, ctimeNs, birthtimeNs) {
    this.dev = dev; this.mode = mode; this.nlink = nlink; this.uid = uid; this.gid = gid;
    this.rdev = rdev; this.blksize = blksize; this.ino = ino; this.size = size; this.blocks = blocks;
    this.atimeMs = atimeMs; this.mtimeMs = mtimeMs; this.ctimeMs = ctimeMs; this.birthtimeMs = birthtimeMs;
    this.atimeNs = atimeNs; this.mtimeNs = mtimeNs; this.ctimeNs = ctimeNs; this.birthtimeNs = birthtimeNs;
    this.atime = new Date(Number(atimeMs)); this.mtime = new Date(Number(mtimeMs));
    this.ctime = new Date(Number(ctimeMs)); this.birthtime = new Date(Number(birthtimeMs));
  }
  BigIntStats.prototype.isFile = function () { return (this.mode & 0o170000n) === 0o100000n; };
  BigIntStats.prototype.isDirectory = function () { return (this.mode & 0o170000n) === 0o040000n; };
  BigIntStats.prototype.isSymbolicLink = function () { return (this.mode & 0o170000n) === 0o120000n; };
  BigIntStats.prototype.isBlockDevice = function () { return (this.mode & 0o170000n) === 0o060000n; };
  BigIntStats.prototype.isCharacterDevice = function () { return (this.mode & 0o170000n) === 0o020000n; };
  BigIntStats.prototype.isFIFO = function () { return (this.mode & 0o170000n) === 0o010000n; };
  BigIntStats.prototype.isSocket = function () { return (this.mode & 0o170000n) === 0o140000n; };
  const mkBigIntStats = (s) => {
    const B = (x) => BigInt(Math.floor(Number(x)));
    return new BigIntStats(B(s.dev), B(s.mode), B(s.nlink), B(s.uid), B(s.gid), B(s.rdev), B(s.blksize),
      B(s.ino), B(s.size), B(s.blocks), B(s.atimeMs), B(s.mtimeMs), B(s.ctimeMs), B(s.birthtimeMs),
      B(s.atimeMs) * 1000000n, B(s.mtimeMs) * 1000000n, B(s.ctimeMs) * 1000000n, B(s.birthtimeMs) * 1000000n);
  };
  fsMod.BigIntStats = BigIntStats;
  // node fs.Dir (blueprint lib/internal/fs/dir.js): a lazily-iterated handle over
  // a directory's entries. mbun reads the entries eagerly at open and hands them
  // out one Dirent at a time. read/close throw synchronously once closed
  // (ERR_DIR_CLOSED "Directory handle was closed"); a bad read callback is a
  // synchronous TypeError. Symbol.dispose / asyncDispose compose with `using`.
  const VALID_ENCODINGS = { utf8: 1, "utf-8": 1, ascii: 1, latin1: 1, binary: 1, ucs2: 1, "ucs-2": 1,
    utf16le: 1, "utf-16le": 1, base64: 1, base64url: 1, hex: 1, buffer: 1 };
  const cbTypeError = () => { const e = new TypeError('The "callback" argument must be of type function.'); e.code = "ERR_INVALID_ARG_TYPE"; throw e; };
  class Dir {
    constructor(path, entries, encoding) { this._path = path; this._entries = entries; this._i = 0; this._closed = false; this._encoding = encoding || "utf8"; }
    get path() { return this._path; }
    _guard() { if (this._closed) { const e = new Error("Directory handle was closed"); e.code = "ERR_DIR_CLOSED"; throw e; } }
    _decode(name) { const enc = this._encoding; if (enc === "utf8" || enc === "utf-8" || enc === "buffer") return name; return Buffer.from(name, "utf8").toString(enc); }
    _next() { if (this._i >= this._entries.length) return null; const e = this._entries[this._i++]; return new fsMod.Dirent(this._decode(e.name), e.type, this._path); }
    readSync() { this._guard(); return this._next(); }
    read(cb) { if (cb !== undefined && typeof cb !== "function") cbTypeError(); this._guard(); if (typeof cb === "function") { const ent = this._next(); G.queueMicrotask(() => cb(null, ent)); return; } return Promise.resolve(this._next()); }
    closeSync() { this._guard(); this._closed = true; }
    close(cb) { if (cb !== undefined && typeof cb !== "function") cbTypeError(); this._guard(); this._closed = true; if (typeof cb === "function") { G.queueMicrotask(() => cb(null)); return; } return Promise.resolve(); }
    [Symbol.dispose]() { this._closed = true; }
    [Symbol.asyncDispose]() { this._closed = true; return Promise.resolve(); }
  }
  fsMod.Dir = Dir;
  const openDirImpl = (p, opts) => {
    const path2 = toStr(p);
    let encoding = "utf8";
    if (typeof opts === "string") encoding = opts;
    else if (opts && typeof opts === "object" && opts.encoding != null) encoding = opts.encoding;
    if (!VALID_ENCODINGS[String(encoding).toLowerCase()]) {
      const e = new TypeError("The argument 'encoding' is invalid. Received " + JSON.stringify(encoding));
      e.code = "ERR_INVALID_ARG_VALUE";
      throw e;
    }
    const st = F.stat(path2, true); // throws ENOENT (Error) when missing
    if (!(st && st.isDirectory && st.isDirectory())) {
      const e = new Error("ENOTDIR: not a directory, opendir '" + path2 + "'");
      e.code = "ENOTDIR"; e.errno = process.platform === "win32" ? -4052 : -20; e.syscall = "opendir"; e.path = path2;
      throw e;
    }
    const entries = F.readdir(path2).map((n) => { let t = 1; try { t = F.stat(path2 + "/" + n)._isDir ? 2 : 1; } catch (e) { t = 3; } return { name: n, type: t }; });
    return new Dir(path2, entries, String(encoding).toLowerCase());
  };
  fsMod.opendirSync = (p, opts) => openDirImpl(p, opts);
  fsMod.opendir = (p, options, cb) => {
    const fn = typeof options === "function" ? options : cb;
    if (typeof fn !== "function") cbTypeError();
    const opts = typeof options === "object" || typeof options === "string" ? options : undefined;
    G.queueMicrotask(() => { try { fn(null, openDirImpl(p, opts)); } catch (e) { fn(e); } });
  };
  fsMod.Dirent = class Dirent {
    constructor(name, type, path) { this.name = name; this.parentPath = path; this.path = path; this._type = type; }
    isFile() { return this._type === 1; }
    isDirectory() { return this._type === 2; }
    isSymbolicLink() { return this._type === 3; }
    isFIFO() { return this._type === 4; }
    isSocket() { return this._type === 5; }
    isCharacterDevice() { return this._type === 6; }
    isBlockDevice() { return this._type === 7; }
  };
  def(["fs"], fsMod);

  const P = (fn) => (...a) => { try { return Promise.resolve(fn(...a)); } catch (e) { return Promise.reject(e); } };
  // node fs.promises FileHandle (blueprint bun-ref/src/js/node/fs.promises.ts):
  // a promise-returning wrapper over the fd-level sync ops.
  class FileHandle {
    constructor(fd) { this._fd = fd; this._closed = false; this._events = { __proto__: null }; }
    get fd() { return this._fd; }
    // node's FileHandle is an EventEmitter (emits "close"); createReadStream /
    // createWriteStream build fd-bound streams that autoClose the handle
    // (fs-leak: FileHandle stream must not leak the descriptor).
    on(ev, cb) { (this._events[ev] || (this._events[ev] = [])).push(cb); return this; }
    once(ev, cb) { const w = (...a) => { this.off(ev, w); cb(...a); }; return this.on(ev, w); }
    off(ev, cb) { const a = this._events[ev]; if (a) { const i = a.indexOf(cb); if (i >= 0) a.splice(i, 1); } return this; }
    removeListener(ev, cb) { return this.off(ev, cb); }
    emit(ev, ...a) { const l = this._events[ev]; if (l) for (const cb of l.slice()) cb(...a); return !!(l && l.length); }
    appendFile(data, o) {
      const enc = typeof o === "string" ? o : (o && o.encoding) || "utf8";
      const b = typeof data === "string" ? Buffer.from(data, enc) : (ArrayBuffer.isView(data) ? data : Buffer.from(data));
      return Promise.resolve().then(() => { fsMod.writeSync(this._fd, b, 0, b.byteLength || b.length, null); });
    }
    createWriteStream(opts) {
      opts = opts || {}; const self = this; const ws = new Writable();
      ws.autoClose = opts.autoClose !== false; ws.bytesWritten = 0; const parts = [];
      ws._write = (chunk, e, cb) => { const b = typeof chunk === "string" ? Buffer.from(chunk, "utf8") : Buffer.from(chunk); parts.push(b); ws.bytesWritten += b.length; const f = cb || e; if (typeof f === "function") f(); };
      ws.end = (chunk, e, cb) => {
        if (chunk != null && typeof chunk !== "function") ws._write(chunk, "utf8", null);
        try { const all = Buffer.concat(parts); fsMod.writeSync(self._fd, all, 0, all.length, null); } catch (er) { ws.emit("error", er); }
        if (ws.autoClose) self.close();
        G.queueMicrotask(() => { ws.emit("finish"); ws.emit("close"); });
        const f = cb || (typeof e === "function" ? e : typeof chunk === "function" ? chunk : null); if (f) f();
        return ws;
      };
      ws.close = (cb) => { if (cb) cb(); return ws; };
      return ws;
    }
    createReadStream(opts) {
      opts = opts || {}; const self = this; const rs = new Readable(); rs.bytesRead = 0;
      rs.autoClose = opts.autoClose === true; const enc = typeof opts === "string" ? opts : opts.encoding;
      G.queueMicrotask(() => {
        try {
          const chunks = []; const tmp = Buffer.alloc(65536); let n;
          while ((n = fsMod.readSync(self._fd, tmp, 0, tmp.length, null)) > 0) chunks.push(Buffer.from(tmp.subarray(0, n)));
          const buf = Buffer.concat(chunks); rs.bytesRead = buf.length;
          rs.push(enc ? buf.toString(enc) : buf); rs.push(null);
          if (rs.autoClose) self.close();
          rs.emit("close");
        } catch (e) { e.code = e.code || "ENOENT"; rs.emit("error", e); }
      });
      rs.close = (cb) => { if (cb) cb(); return rs; };
      return rs;
    }
    read(buf, off, len, pos) {
      if (buf && typeof buf === "object" && !ArrayBuffer.isView(buf)) {
        const o = buf; buf = o.buffer || Buffer.alloc(16384); off = o.offset || 0;
        len = o.length == null ? buf.byteLength - off : o.length; pos = o.position == null ? null : o.position;
      }
      return Promise.resolve().then(() => {
        const bytesRead = fsMod.readSync(this._fd, buf, off || 0, len == null ? buf.byteLength : len, pos == null ? null : pos);
        return { bytesRead, buffer: buf };
      });
    }
    write(buf, off, len, pos) {
      if (typeof buf === "string") {
        const enc2 = typeof off === "string" ? off : "utf8"; const b = Buffer.from(buf, enc2);
        return Promise.resolve().then(() => ({ bytesWritten: fsMod.writeSync(this._fd, b, 0, b.length, typeof off === "number" ? off : null), buffer: buf }));
      }
      return Promise.resolve().then(() => ({ bytesWritten: fsMod.writeSync(this._fd, buf, off || 0, len == null ? buf.byteLength : len, pos == null ? null : pos), buffer: buf }));
    }
    readFile(o) {
      const enc = typeof o === "string" ? o : (o && o.encoding);
      return Promise.resolve().then(() => {
        const chunks = []; const tmp = Buffer.alloc(65536); let n;
        while ((n = fsMod.readSync(this._fd, tmp, 0, tmp.length, null)) > 0) chunks.push(Buffer.from(tmp.subarray(0, n)));
        const all = Buffer.concat(chunks);
        return enc ? all.toString(enc) : all;
      });
    }
    writeFile(data, o) {
      const enc = typeof o === "string" ? o : (o && o.encoding) || "utf8";
      const b = typeof data === "string" ? Buffer.from(data, enc) : (ArrayBuffer.isView(data) ? data : Buffer.from(data));
      return Promise.resolve().then(() => { fsMod.writeSync(this._fd, b, 0, b.byteLength || b.length, null); });
    }
    stat() { return Promise.resolve().then(() => fsMod.fstatSync(this._fd)); }
    sync() { return Promise.resolve(); }
    datasync() { return Promise.resolve(); }
    truncate(len) { return Promise.resolve().then(() => { try { fsMod.ftruncateSync(this._fd, len); } catch (e) {} }); }
    chmod(m) { return Promise.resolve(); }
    chown() { return Promise.resolve(); }
    utimes() { return Promise.resolve(); }
    close() { if (this._closed) return Promise.resolve(); this._closed = true; return Promise.resolve().then(() => { fsMod.closeSync(this._fd); this.emit("close"); }); }
    [Symbol.asyncDispose]() { return this.close(); }
  }
  const fsPromises = {
    open: (p, flags, mode) => Promise.resolve().then(() => new FileHandle(globalThis.__mbunFdNative.open(toStr(p), flags == null ? "r" : (typeof flags === "number" ? flags : toStr(flags)), typeof mode === "number" ? mode : 0o666))),
    readFile: P((p) => F.readFile(toStr(p))),
    writeFile: (p, d, o) => (async () => {
      const path2 = toStr(p);
      if (d != null && typeof d !== "string" && !ArrayBuffer.isView(d) && !(d instanceof ArrayBuffer) &&
          (typeof d[Symbol.asyncIterator] === "function" || typeof d[Symbol.iterator] === "function")) {
        // node fs.promises.writeFile consumes (async) iterables: open the fd
        // first (so a directory path throws EISDIR before the iterator is
        // touched), then write each chunk. ref src/js/node/fs.promises.ts:1493.
        const FD = globalThis.__mbunFdNative;
        const fd = FD.open(path2, (o && o.flag) || "w", 0o666);
        try {
          for await (const chunk of d) {
            let u;
            if (typeof chunk === "string") u = Buffer.from(chunk, (o && o.encoding) || "utf8");
            else if (ArrayBuffer.isView(chunk)) u = new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
            else if (chunk instanceof ArrayBuffer) u = new Uint8Array(chunk);
            else throw Object.assign(new TypeError("The \"chunk\" argument must be of type string or an instance of Buffer, TypedArray, or DataView. Received " + typeof chunk), { code: "ERR_INVALID_ARG_TYPE" });
            FD.write(fd, u, 0, u.byteLength, -1);
          }
        } finally { FD.close(fd); }
        return;
      }
      return F.writeFile(path2, toStr(d));
    })(),
    appendFile: P((p, d) => F.appendFile(toStr(p), toStr(d))),
    mkdir: P((p, o) => { validatePath(p); const [rec, mode] = mkdirOpts(o); return F.mkdir(toStr(p), rec, mode); }),
    rm: P((p, o) => F.rm(toStr(p), recur(o), !!(o && o.force))),
    rmdir: P((p) => F.rm(toStr(p), true, true)),
    readdir: P((p) => F.readdir(toStr(p))),
    stat: P((p) => F.stat(toStr(p))),
    lstat: P((p) => F.stat(toStr(p), true)),
    unlink: P((p) => F.unlink(toStr(p))),
    realpath: P((p) => F.realpath(toStr(p))),
    rename: P((a, b) => F.rename(toStr(a), toStr(b))),
    copyFile: P((a, b) => F.copyFile(toStr(a), toStr(b))),
    mkdtemp: P((pre) => F.mkdtemp(toStr(pre))),
    access: P((p) => { if (!F.exists(toStr(p))) throw Object.assign(new Error("ENOENT: no such file or directory, access '" + toStr(p) + "'"), { code: "ENOENT" }); }),
    exists: P((p) => F.exists(toStr(p))),
    cp: P((src, dest, o) => cpRec(toStr(src), toStr(dest), o)),
    symlink: P((target, path2) => F.symlink(toStr(target), toStr(path2))),
    readlink: P((p) => F.readlink(toStr(p))),
    chmod: P((p, m) => F.chmod(toStr(p), typeof m === "string" ? parseInt(m, 8) : (Number(m) & 0o7777))), lchmod: P(() => {}), chown: P(() => {}), lchown: P(() => {}),
    utimes: P((p, a, m) => { const s = (v) => v instanceof Date ? v.getTime() / 1000 : Number(v); F.utimes(toStr(p), s(a), s(m)); }), lutimes: P(() => {}), truncate: P(() => {}),
    glob: (pat, o) => { const arr = fsMod.globSync(pat, o); let i = 0; return { [Symbol.asyncIterator]() { return { next: () => Promise.resolve(i < arr.length ? { value: arr[i++], done: false } : { value: undefined, done: true }) }; } }; },
    opendir: (p, opts) => Promise.resolve().then(() => fsMod.opendirSync(p, opts)),
  };
  fsMod.promises = fsPromises;
  M["fs/promises"] = fsPromises;
  M["node:fs/promises"] = fsPromises;

  // ---- string_decoder ----
  class StringDecoder {
    constructor(enc) { this.encoding = enc || "utf8"; }
    write(buf) { return typeof buf === "string" ? buf : String(buf); }
    end(buf) { return buf != null ? this.write(buf) : ""; }
  }
  def(["string_decoder"], { StringDecoder });

)JS";

}  // namespace mbun::jsc::builtins::detail
