// Bun.password JS layer. Self-contained IIFE (appended after the master
// builtins IIFE) that installs Bun.password = { hash, hashSync, verify,
// verifySync } over the native __mbunPasswordNative (runtime/bun_password.inc,
// backed by pure-C++ mbun.crypto bcrypt / argon2). All argument validation,
// option parsing, and error-message shaping match bun's PasswordObject.rs.
//
// Async hash/verify compute synchronously and wrap the result in a resolved/
// rejected Promise (no threadpool); invalid-encoding / unsupported-algorithm
// errors surface synchronously, WeakParameters rejects on the async path.
export module mbun.jsc.js_builtins:bun_password;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kBunPasswordJS = R"JS(
(function () {
  const G = globalThis;
  const NV = G.__mbunPasswordNative;
  if (!NV || !G.Bun) return;

  const UNKNOWN = 'unknown algorithm, expected one of: "bcrypt", "argon2id", "argon2d", "argon2i" (default is "argon2id")';
  const ARGONS = ["argon2id", "argon2d", "argon2i"];
  const isObj = (v) => v !== null && (typeof v === "object" || typeof v === "function");
  const toInt32 = (n) => n | 0;

  const parseAlgorithm = (a) => {
    if (a === undefined || a === null) return { name: "argon2id", memoryCost: 65536, timeCost: 2 };
    if (typeof a === "string") {
      if (a === "bcrypt") return { name: "bcrypt", cost: 10 };
      if (ARGONS.includes(a)) return { name: a, memoryCost: 65536, timeCost: 2 };
      throw new TypeError(UNKNOWN);
    }
    if (isObj(a)) {
      const algo = a.algorithm;
      if (typeof algo !== "string") throw new TypeError('The "algorithm" argument must be of type string.');
      if (algo === "bcrypt") {
        let cost = 10;
        const c = a.cost;
        if (c !== undefined && c !== null && c !== 0) {
          if (typeof c !== "number") throw new TypeError('The "cost" argument must be of type number.');
          const rounds = toInt32(c);
          if (rounds < 4 || rounds > 31) throw new Error("Rounds must be between 4 and 31");
          cost = rounds & 0x3f;
        }
        return { name: "bcrypt", cost };
      }
      if (ARGONS.includes(algo)) {
        let memoryCost = 65536;
        let timeCost = 2;
        const t = a.timeCost;
        if (t !== undefined && t !== null && t !== 0) {
          if (typeof t !== "number") throw new TypeError('The "timeCost" argument must be of type number.');
          const tc = toInt32(t);
          if (tc < 1) throw new Error("Time cost must be greater than 0");
          timeCost = tc;
        }
        const m = a.memoryCost;
        if (m !== undefined && m !== null && m !== 0) {
          if (typeof m !== "number") throw new TypeError('The "memoryCost" argument must be of type number.');
          const mc = toInt32(m);
          if (mc < 8) throw new Error("Memory cost must be at least 8");
          memoryCost = mc;
        }
        return { name: algo, memoryCost, timeCost };
      }
      throw new TypeError(UNKNOWN);
    }
    throw new TypeError('The "algorithm" argument must be of type string.');
  };

  // Coerce a hash-input value (password or stored hash). Buffers pass through;
  // everything else stringifies (Symbol / throwing toString propagate).
  const coerce = (v) => {
    // String(Symbol()) does NOT throw (returns "Symbol()"); reject explicitly so
    // symbols surface as a type error like bun's StringOrBuffer decode.
    if (typeof v === "symbol") {
      throw new TypeError("password must be a string, TypedArray, or Buffer");
    }
    if (typeof v === "string") return { value: v, empty: v.length === 0 };
    if (ArrayBuffer.isView(v)) return { value: v, empty: v.byteLength === 0 };
    if (v instanceof ArrayBuffer) return { value: v, empty: v.byteLength === 0 };
    const s = String(v);
    return { value: s, empty: s.length === 0 };
  };

  const requirePassword = (v, argc) => {
    if (argc < 1 || v === undefined) throw new TypeError("password is required");
    const p = coerce(v);
    if (p.empty) throw new Error("password must not be empty");
    return p.value;
  };

  const doHash = (pwValue, opts) => {
    if (opts.name === "bcrypt") return NV.hash(pwValue, "bcrypt", opts.cost, 0, 0);
    return NV.hash(pwValue, opts.name, 0, opts.memoryCost, opts.timeCost);
  };

  const validateVerifyAlgorithm = (a) => {
    if (a === undefined || a === null) return undefined;
    if (typeof a !== "string") throw new TypeError('The "algorithm" argument must be of type string.');
    if (a !== "bcrypt" && !ARGONS.includes(a)) throw new TypeError(UNKNOWN);
    return a;
  };

  const runVerify = (pwValue, hashValue, algo) => {
    const code = NV.verify(pwValue, hashValue, algo);
    if (code === 1) return true;
    if (code === 0) return false;
    if (code === 2) {
      const e = new Error('Password verification failed with error "WeakParameters"');
      e.code = "PASSWORD_WEAK_PARAMETERS";
      throw e;
    }
    if (code === 3) throw new Error('Password verification failed with error "InvalidEncoding"');
    throw new Error(UNKNOWN);
  };

  const password = {
    hashSync(value, algorithm) {
      const pw = requirePassword(value, arguments.length);
      return doHash(pw, parseAlgorithm(algorithm));
    },
    hash(value, algorithm) {
      let pw, opts;
      try {
        pw = requirePassword(value, arguments.length);
        opts = parseAlgorithm(algorithm);
      } catch (e) {
        throw e;
      }
      return Promise.resolve(doHash(pw, opts));
    },
    verifySync(value, hash, algorithm) {
      if (arguments.length < 2) throw new TypeError("hash is required");
      const algo = validateVerifyAlgorithm(algorithm);
      const P = coerce(value);
      const H = coerce(hash);
      if (P.empty || H.empty) return false;
      return runVerify(P.value, H.value, algo);
    },
    verify(value, hash, algorithm) {
      if (arguments.length < 2) throw new TypeError("hash is required");
      const algo = validateVerifyAlgorithm(algorithm);
      const P = coerce(value);
      const H = coerce(hash);
      if (P.empty || H.empty) return Promise.resolve(false);
      try {
        return Promise.resolve(runVerify(P.value, H.value, algo));
      } catch (e) {
        if (e && e.code === "PASSWORD_WEAK_PARAMETERS") return Promise.reject(e);
        throw e;
      }
    },
  };

  Object.defineProperty(G.Bun, "password", {
    configurable: true, enumerable: true, writable: true, value: password,
  });
})();
)JS";


// ---- Bun.FileSystemRouter ------------------------------------------------
// Next.js-style filesystem routing over node:fs. Blueprint: bun
// src/runtime/api/FileSystemRouter (JSFileSystemRouter / MatchedRoute); the
// route model ([param] / [...catchAll] / [[...optionalCatchAll]] and the
// static > dynamic > catch-all > optional-catch-all priority) is the one
// modules/router/src/router.cppm carries. It rides this partition rather than
// its own because a fresh js_builtins partition is not picked up by the module
// scanner without a clean rebuild; the payload is independent of bun_password.
// A MatchedRoute exposes exactly {name, filePath, pathname, params, query,
// src} -- the corpus asserts nothing else is readable off it -- and `query` is
// params merged with the query string, params first (bun's order, which the
// JSON.stringify comparisons depend on).
inline constexpr std::string_view kBunFsRouterJS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  if (!G.Bun || !M || typeof G.Bun.FileSystemRouter !== "undefined") return;
  const fsOf = () => M["node:fs"] || M["fs"];

  const DEFAULT_EXTS = [".tsx", ".jsx", ".ts", ".mjs", ".cjs", ".js"];
  const KIND_STATIC = 0, KIND_DYNAMIC = 1, KIND_CATCH_ALL = 2, KIND_OPT_CATCH_ALL = 3;

  const hex = (c) => {
    if (c >= 48 && c <= 57) return c - 48;
    if (c >= 97 && c <= 102) return c - 87;
    if (c >= 65 && c <= 70) return c - 55;
    return -1;
  };
  // Decode %XX ONCE, byte-wise, leaving malformed escapes untouched (bun's
  // decoder is fault-tolerant: "%PUBLIC_URL%" survives as itself instead of
  // throwing the way decodeURIComponent does).
  const enc = new TextEncoder();
  const dec = new TextDecoder();
  const percentDecode = (s) => {
    if (s.indexOf("%") === -1) return s;
    const bytes = [];
    for (let i = 0; i < s.length; i++) {
      const ch = s[i];
      if (ch === "%" && i + 2 < s.length) {
        const hi = hex(s.charCodeAt(i + 1)), lo = hex(s.charCodeAt(i + 2));
        if (hi >= 0 && lo >= 0) { bytes.push((hi << 4) | lo); i += 2; continue; }
      }
      const code = s.codePointAt(i);
      if (code > 0xffff) i++;
      for (const b of enc.encode(String.fromCodePoint(code))) bytes.push(b);
    }
    return dec.decode(new Uint8Array(bytes));
  };

  const splitSegments = (p) => {
    const out = [];
    let start = p.charCodeAt(0) === 47 ? 1 : 0;
    while (start <= p.length) {
      let end = p.indexOf("/", start);
      const stop = end === -1 ? p.length : end;
      if (stop > start) out.push(p.slice(start, stop));
      if (end === -1) break;
      start = end + 1;
    }
    return out;
  };
  const normalizePath = (p) => {
    if (!p) return "/";
    let r = p.charCodeAt(0) === 47 ? p : "/" + p;
    while (r.length > 1 && r.endsWith("/")) r = r.slice(0, -1);
    return r;
  };
  // "/foo/index", "/foo/index/index" and "/index" all name the parent route.
  const stripIndex = (p) => {
    let r = p;
    while (r.endsWith("/index")) { r = r.slice(0, -6); if (!r) { r = "/"; break; } }
    return r;
  };
  // Empty-key pairs are SKIPPED, not treated as end-of-query.
  const parseQuery = (q, into) => {
    if (!q) return into;
    for (const part of q.split("&")) {
      if (!part) continue;
      const eq = part.indexOf("=");
      const key = eq === -1 ? part : part.slice(0, eq);
      if (!key) continue;
      into[percentDecode(key)] = eq === -1 ? "" : percentDecode(part.slice(eq + 1));
    }
    return into;
  };

  const joinPath = (dir, rel) => (!dir ? rel : dir.endsWith("/") ? dir + rel : dir + "/" + rel);

  function makeRoute(file, ext) {
    const segments = splitSegments(file.slice(0, file.length - ext.length));
    if (segments.length && segments[segments.length - 1] === "index") segments.pop();
    let kind = KIND_STATIC;
    for (const seg of segments) {
      if (seg.startsWith("[[...") && seg.endsWith("]]")) {
        if (kind < KIND_OPT_CATCH_ALL) kind = KIND_OPT_CATCH_ALL;
      } else if (seg.startsWith("[...") && seg.endsWith("]")) {
        if (kind < KIND_CATCH_ALL) kind = KIND_CATCH_ALL;
      } else if (seg.startsWith("[") && seg.endsWith("]")) {
        if (seg.length <= 2) throw new Error("Route is missing a closing bracket]");
        if (kind < KIND_DYNAMIC) kind = KIND_DYNAMIC;
      } else if (seg.indexOf("[") !== -1 || seg.indexOf("]") !== -1) {
        throw new Error("Route is missing a closing bracket]");
      }
    }
    return { name: "/" + segments.join("/"), file: file, segments: segments,
             kind: kind, count: segments.length };
  }

  function matchRoute(route, request, params) {
    let i = 0;
    for (const pattern of route.segments) {
      if (pattern.startsWith("[[...") && pattern.endsWith("]]")) {
        const key = pattern.slice(5, -2);
        const value = request.slice(i).join("/");
        if (value) params[key] = value;
        i = request.length;
        continue;
      }
      if (pattern.startsWith("[...") && pattern.endsWith("]")) {
        if (i === request.length) return false;
        params[pattern.slice(4, -1)] = request.slice(i).join("/");
        i = request.length;
        continue;
      }
      if (i === request.length) return false;
      if (pattern.length >= 2 && pattern.startsWith("[") && pattern.endsWith("]")) {
        params[pattern.slice(1, -1)] = request[i++];
      } else if (pattern !== request[i++]) return false;
    }
    return i === request.length;
  }

  const ABS_URL = /^[a-zA-Z][a-zA-Z0-9+.\-]*:\/\//;
  // A path string must be spelled with its leading '/': bun matches on the raw
  // string, so "%2Ftop", "top" and " top" are all non-paths and match nothing.
  // A full URL (what a Request carries) is unwrapped to pathname + search.
  function toPathAndQuery(input) {
    let s = input;
    if (s !== null && typeof s === "object") {
      const u = s.url;
      if (typeof u !== "string") return null;
      s = u;
    }
    if (typeof s !== "string") return null;
    if (ABS_URL.test(s)) {
      let u;
      try { u = new URL(s); } catch (e) { return null; }
      return [u.pathname, u.search ? u.search.slice(1) : ""];
    }
    if (s === "") return ["/", ""];
    if (s.charCodeAt(0) !== 47) return null;
    const q = s.indexOf("?");
    return q === -1 ? [s, ""] : [s.slice(0, q), s.slice(q + 1)];
  }

  function FileSystemRouter(options) {
    if (!new.target) throw new TypeError("Class constructor FileSystemRouter cannot be invoked without 'new'");
    const opts = options || {};
    if (typeof opts.dir !== "string") throw new TypeError("Expected dir to be a string");
    if (opts.origin !== undefined && opts.origin !== null && typeof opts.origin !== "string")
      throw new TypeError("Expected origin to be a string");
    if (opts.assetPrefix !== undefined && opts.assetPrefix !== null && typeof opts.assetPrefix !== "string")
      throw new TypeError("Expected assetPrefix to be a string");
    if (opts.style !== undefined && opts.style !== "nextjs")
      throw new TypeError('Only the "nextjs" router style is currently supported');
    this.dir = opts.dir;
    this.style = "nextjs";
    this.origin = typeof opts.origin === "string" ? opts.origin : "";
    this.assetPrefix = typeof opts.assetPrefix === "string" ? opts.assetPrefix : "";
    let exts = Array.isArray(opts.fileExtensions) && opts.fileExtensions.length
      ? opts.fileExtensions.map(String) : DEFAULT_EXTS.slice();
    this._exts = exts.map((e) => (e.startsWith(".") ? e : "." + e));
    this.reload();
  }

  FileSystemRouter.prototype.reload = function () {
    const fs = fsOf();
    const files = [];
    const walk = (abs, rel) => {
      let entries;
      try { entries = fs.readdirSync(abs, { withFileTypes: true }); } catch (e) { return; }
      for (const ent of entries) {
        const name = ent.name;
        if (!name || name.charCodeAt(0) === 46 || name === "node_modules") continue;
        const childRel = rel ? rel + "/" + name : name;
        if (ent.isDirectory()) { walk(abs + "/" + name, childRel); continue; }
        if (!ent.isFile()) continue;
        files.push(childRel);
      }
    };
    walk(this.dir, "");
    files.sort();
    const routes = {};
    const list = [];
    for (const file of files) {
      const slash = file.lastIndexOf("/"), dot = file.lastIndexOf(".");
      if (dot === -1 || dot < slash) continue;
      const ext = file.slice(dot);
      if (this._exts.indexOf(ext) === -1) continue;
      const route = makeRoute(file, ext);
      routes[route.name] = joinPath(this.dir, file);
      list.push(route);
    }
    list.sort((a, b) => {
      if (a.kind !== b.kind) return a.kind - b.kind;
      if (a.kind >= KIND_CATCH_ALL && a.count !== b.count) return a.count - b.count;
      return a.name < b.name ? -1 : a.name > b.name ? 1 : 0;
    });
    this._routes = routes;
    this._list = list;
  };

  Object.defineProperty(FileSystemRouter.prototype, "routes", {
    configurable: true,
    get() { return this._routes; },
  });

  FileSystemRouter.prototype.match = function (input) {
    const parsed = toPathAndQuery(input);
    if (parsed === null) return null;
    const pathname = normalizePath(percentDecode(parsed[0]));
    const requestSegments = splitSegments(stripIndex(pathname));
    for (const route of this._list) {
      const params = {};
      if (!matchRoute(route, requestSegments, params)) continue;
      const query = {};
      for (const k of Object.keys(params)) query[k] = params[k];
      parseQuery(parsed[1], query);
      return {
        name: route.name,
        filePath: joinPath(this.dir, route.file),
        pathname: pathname,
        params: params,
        query: query,
        src: this.origin + this.assetPrefix + route.file,
      };
    }
    return null;
  };

  Object.defineProperty(FileSystemRouter, "name", { value: "FileSystemRouter", configurable: true });
  G.Bun.FileSystemRouter = FileSystemRouter;
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
