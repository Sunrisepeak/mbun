// WHATWG Headers conformance partition.
//
// The base Headers class (process_web) is a plain multi-map with no fetch-spec
// validation. This shard upgrades it IN PLACE (same prototype, so existing
// instances and instanceof stay valid) and re-binds G.Headers to a Proxy whose
// construct trap performs spec-correct HeadersInit conversion:
//   - null / primitive init throws TypeError
//   - sequence init: every entry converted via iteration, exact length 2
//   - record init: own-key snapshot, own-ness re-checked per key, Get and
//     ToString interleaved per key (Web IDL record semantics)
//   - name/value validation with bun-exact messages (canonical well-known
//     header names: "Header 'Content-Type' has invalid value: ...")
//   - value normalization (strip leading/trailing HTTP whitespace)
// plus arity/validation on append/set/get/has/delete, spec getAll (set-cookie
// only), sorted+combined iteration (entries/keys/values/forEach/iterator),
// a Bun.inspect/util.inspect Headers formatter, and the
// bun:internal-for-testing lowercaseHeaderNameSIMD reference kernel.
//
// NOTE: appended AFTER the master builtins IIFE; self-contained IIFE that
// re-binds G = globalThis. Top level must never throw.
//
// Blueprint: fetch spec §2.2 (headers class) + Web IDL record/sequence
// conversion, message shapes pinned by compat/bun/test/js/web/fetch/*.test.
export module mbun.jsc.js_builtins:web_headers;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kWebHeadersJS = R"JS(
(function () {
  const G = globalThis;
  try {
    const OrigHeaders = G.Headers;
    if (typeof OrigHeaders !== "function" || !OrigHeaders.prototype) return;
    const proto = OrigHeaders.prototype;
    const M = G.__mbunNativeModules;

    // -------------------------------------------------- name canonicalization
    const WELL_KNOWN = [
      "Accept", "Accept-Charset", "Accept-Encoding", "Accept-Language", "Accept-Ranges",
      "Access-Control-Allow-Credentials", "Access-Control-Allow-Headers", "Access-Control-Allow-Methods",
      "Access-Control-Allow-Origin", "Access-Control-Expose-Headers", "Access-Control-Max-Age",
      "Access-Control-Request-Headers", "Access-Control-Request-Method", "Age", "Authorization",
      "Cache-Control", "Connection", "Content-Disposition", "Content-Encoding", "Content-Language",
      "Content-Length", "Content-Location", "Content-Range", "Content-Security-Policy", "Content-Type",
      "Cookie", "Cookie2", "Date", "DNT", "ETag", "Expect", "Expires", "From", "Host", "If-Match",
      "If-Modified-Since", "If-None-Match", "If-Range", "If-Unmodified-Since", "Keep-Alive",
      "Last-Modified", "Link", "Location", "Origin", "Pragma", "Proxy-Authorization", "Range",
      "Referer", "Referrer-Policy", "Refresh", "Retry-After", "Sec-Fetch-Dest", "Sec-Fetch-Mode",
      "Sec-Fetch-Site", "Sec-WebSocket-Accept", "Sec-WebSocket-Extensions", "Sec-WebSocket-Key",
      "Sec-WebSocket-Protocol", "Sec-WebSocket-Version", "Server", "Set-Cookie", "Set-Cookie2",
      "TE", "Trailer", "Transfer-Encoding", "Upgrade", "Upgrade-Insecure-Requests", "User-Agent",
      "Vary", "Via", "WWW-Authenticate", "X-Content-Type-Options", "X-Frame-Options", "X-XSS-Protection",
    ];
    const CANONICAL = new Map();
    for (const n of WELL_KNOWN) CANONICAL.set(n.toLowerCase(), n);

    // ------------------------------------------------------------ validation
    // RFC 7230 token, checked by code point rather than /^[...]+$/.test().
    // RegExp.prototype.test goes through RegExpExec, which reads
    // `RegExp.prototype.exec` at call time — the deno corpus
    // (headerInitWithPrototypePollution) replaces that with a thrower and then
    // requires `new Headers([...])` to still work, so the validator must not
    // touch a regexp at all.
    const isTokenChar = (c) =>
      (c >= 0x30 && c <= 0x39) ||            // 0-9
      (c >= 0x41 && c <= 0x5a) ||            // A-Z
      (c >= 0x61 && c <= 0x7a) ||            // a-z
      c === 0x21 || (c >= 0x23 && c <= 0x27) ||  // ! # $ % & '
      c === 0x2a || c === 0x2b || c === 0x2d || c === 0x2e ||  // * + - .
      c === 0x5e || c === 0x5f || c === 0x60 || c === 0x7c || c === 0x7e;  // ^ _ ` | ~
    const validateName = (name) => {
      const s = "" + name;
      let ok = s.length > 0;
      for (let i = 0; ok && i < s.length; i++) ok = isTokenChar(s.charCodeAt(i));
      if (!ok) throw new TypeError("Invalid header name: '" + s + "'");
      return s;
    };
    // Strip leading/trailing HTTP whitespace, then require ISO-8859-1 without
    // NUL/CR/LF.
    // Hand-rolled rather than /^[\t\n\r ]+|[\t\n\r ]+$/g: the trailing-run
    // alternative has no anchor JSC can use, so it retries at every index and
    // the trim goes quadratic. The undici corpus feeds a 500k-tab value
    // ("headers that might cause a ReDoS") that took ~60s through the regex.
    const isHttpWs = (c) => c === 9 || c === 10 || c === 13 || c === 32;
    const normalizeValue = (value) => {
      const s = "" + value;
      let a = 0;
      let b = s.length;
      while (a < b && isHttpWs(s.charCodeAt(a))) a++;
      while (b > a && isHttpWs(s.charCodeAt(b - 1))) b--;
      return a === 0 && b === s.length ? s : s.slice(a, b);
    };
    const validateValue = (value, givenName) => {
      for (let i = 0; i < value.length; i++) {
        const c = value.charCodeAt(i);
        if (c === 0 || c === 10 || c === 13 || c > 255) {
          const lk = ("" + givenName).toLowerCase();
          const display = CANONICAL.get(lk) || ("" + givenName);
          throw new TypeError("Header '" + display + "' has invalid value: '" + value + "'");
        }
      }
      return value;
    };

    // ----------------------------------------------------- prototype upgrade
    // Raw storage ops on the existing internals (_m combined map + _sc list).
    const rawAppend = (h, name, v) => {
      const lk = ("" + name).toLowerCase();
      if (lk === "set-cookie") h._sc.push(v);
      // Track the first-seen original-case name for the wire (bun HTTPHeaderMap
      // preserves the given case for output; the JS API stays lowercased).
      if (h._names instanceof Map && !h._names.has(lk)) h._names.set(lk, "" + name);
      const e = h._m.get(lk);
      // WebKit HTTPHeaderMap::add — Cookie combines with "; ", others ", ".
      h._m.set(lk, e == null ? v : e + (lk === "cookie" ? "; " : ", ") + v);
    };
    const requireArgs = (n, got, method) => {
      if (got < n)
        throw new TypeError(`Headers.${method} requires ${n} argument${n > 1 ? "s" : ""}, but only ${got} ${got === 1 ? "was" : "were"} provided`);
    };

    proto.append = function append(name, value) {
      requireArgs(2, arguments.length, "append");
      const n = validateName(name);
      const v = validateValue(normalizeValue(value), n);
      rawAppend(this, n, v);
    };
    proto.set = function set(name, value) {
      requireArgs(2, arguments.length, "set");
      const n = validateName(name);
      const v = validateValue(normalizeValue(value), n);
      const lk = n.toLowerCase();
      if (lk === "set-cookie") this._sc = [v];
      if (this._names instanceof Map) this._names.set(lk, n);  // set replaces the wire-name case
      this._m.set(lk, v);
    };
    proto.get = function get(name) {
      requireArgs(1, arguments.length, "get");
      const lk = validateName(name).toLowerCase();
      const v = this._m.get(lk);
      return v == null ? null : v;
    };
    proto.has = function has(name) {
      requireArgs(1, arguments.length, "has");
      return this._m.has(validateName(name).toLowerCase());
    };
    proto.delete = function del(name) {
      requireArgs(1, arguments.length, "delete");
      const lk = validateName(name).toLowerCase();
      if (lk === "set-cookie") this._sc = [];
      this._m.delete(lk);
      if (this._names instanceof Map) this._names.delete(lk);
    };
    proto.getAll = function getAll(name) {
      requireArgs(1, arguments.length, "getAll");
      if (validateName(name).toLowerCase() !== "set-cookie")
        throw new TypeError("Headers.getAll() is only supported for 'Set-Cookie' headers");
      return this._sc.slice();
    };
    proto.getSetCookie = function getSetCookie() { return this._sc.slice(); };

    // Sorted + combined iteration; set-cookie values enumerate individually and
    // come LAST, after the sorted ordinary headers, rather than at set-cookie's
    // own sorted position. Upstream keeps set-cookie in a separate vector that
    // the iterator drains after the sorted HTTPHeaderMap, so
    // `[...new Headers([["Set-Cookie","a"],["X-Deno","b"]])]` is
    // [["x-deno","b"],["set-cookie","a"]] even though "s" < "x" — the deno
    // corpus (headersInitMultiple / headersAppendMultiple) pins that order.
    const sortedEntries = (h) => {
      const keys = Array.from(h._m.keys()).sort();
      const out = [];
      for (const k of keys) {
        if (k === "set-cookie") continue;
        out.push([k, h._m.get(k)]);
      }
      if (h._m.has("set-cookie")) {
        if (h._sc.length) { for (const v of h._sc) out.push(["set-cookie", v]); }
        else out.push(["set-cookie", h._m.get("set-cookie")]);
      }
      return out;
    };
    // WHATWG maplike iterator: LIVE, not a snapshot. Each next() re-derives the
    // sorted header list and returns the element at the running index, so a
    // mutation inside a `for..of headers` loop is observed on the next step
    // (headers.undici "should freeze values while iterating"). WebKit's
    // JSFetchHeaders iterator has the same index-into-current-list behavior.
    // A single shared iterator prototype so every Headers iterator's `next` is
    // the same function object (headers.undici "always use the same prototype
    // Iterator"): it reads its (headers, index, kind) state off `this`, never a
    // per-instance closure.
    const HeadersIteratorProto = {
      next() {
        const s = this && this.__hi;
        if (!s) throw new TypeError("Headers Iterator.prototype.next called on an incompatible receiver");
        const entries = sortedEntries(s.h);
        if (s.i >= entries.length) return { value: undefined, done: true };
        const e = entries[s.i++];
        return { value: s.kind === "key" ? e[0] : s.kind === "value" ? e[1] : [e[0], e[1]], done: false };
      },
    };
    HeadersIteratorProto[Symbol.iterator] = function () { return this; };
    const makeHeadersIterator = (h, kind) => {
      const it = Object.create(HeadersIteratorProto);
      it.__hi = { h, i: 0, kind };
      return it;
    };
    proto.entries = function entries() { return makeHeadersIterator(this, "entry"); };
    proto.keys = function keys() { return makeHeadersIterator(this, "key"); };
    proto.values = function values() { return makeHeadersIterator(this, "value"); };
    proto[Symbol.iterator] = proto.entries;
    proto.forEach = function forEach(callback, thisArg) {
      // Callback type is validated BEFORE iterating, so an empty Headers still
      // rejects a non-callable callback (WHATWG/undici arg validation).
      if (typeof callback !== "function") throw new TypeError("Headers.forEach callback must be a function");
      for (const [k, v] of sortedEntries(this)) callback.call(thisArg, v, k, this);
    };
    try { Object.defineProperty(proto, Symbol.toStringTag, { value: "Headers", writable: false, enumerable: false, configurable: true }); } catch (_) {}
    // toJSON: bun/WebKit HTTPHeaderMap order — well-known headers first (in
    // insertion order), then set-cookie, then custom headers (in insertion order).
    //
    // set-cookie is the one header that does NOT collapse to a joined string: it
    // is emitted as an ARRAY of the individual values. Every other repeated header
    // stays joined with ", ".
    //
    // ref: bun-ref/src/jsc/bindings/webcore/JSFetchHeaders.cpp:656
    //   getInternalProperties (what toJSON delegates to, :410-412) writes exactly
    //   three groups in this order:
    //     :675-682 internal.commonHeaders()      -> jsString(value)   (joined)
    //     :684-699 internal.getSetCookieHeaders() -> constructEmptyArray + putDirectIndex
    //              per value, and only when `count > 0` (:687)
    //     :701-708 internal.uncommonHeaders()    -> jsString(value)   (joined)
    //   set-cookie lives in its own vector upstream, so it is never part of
    //   commonHeaders and never joined.
    proto.toJSON = function toJSON() {
      const common = [], uncommon = [];
      for (const [k, v] of this._m) {
        if (k === "set-cookie") continue;  // emitted from _sc below, as an array
        (CANONICAL.has(k) ? common : uncommon).push([k, v]);
      }
      const out = Object.fromEntries(common);
      // blueprint :687 — the key is present only when there is at least one value
      if (this._sc.length) out["set-cookie"] = this._sc.slice();
      for (const [k, v] of uncommon) out[k] = v;
      return out;
    };

    // ----------------------------------------------------- constructor trap
    const fillFromInit = (h, init) => {
      if (init === undefined) return;
      if (init === null || (typeof init !== "object" && typeof init !== "function"))
        throw new TypeError("Headers init must be an object, array, or Headers instance");
      // webidl HeadersInit union (sequence<sequence<ByteString>> or record):
      // read @@iterator EXACTLY ONCE via GetMethod, then decide. `instanceof`
      // uses [[GetPrototypeOf]] (no property Get), so the Headers-copy fast path
      // does not count against "Symbol.iterator is only accessed once".
      const iterFn = init[Symbol.iterator];
      if (init instanceof OrigHeaders && init._m instanceof Map && Array.isArray(init._sc)) {
        // another Headers: exact copy incl. individual set-cookie values
        for (const [k, v] of init._m) h._m.set(k, v);
        h._sc = init._sc.slice();
        if (init._names instanceof Map && h._names instanceof Map)
          for (const [lk, n] of init._names) h._names.set(lk, n);  // preserve wire-name case
        return;
      }
      if (typeof iterFn === "function") {
        // sequence<sequence<ByteString>> — drive the iterator obtained from the
        // SINGLE @@iterator read (no re-Get via a fresh for..of).
        const iterator = iterFn.call(init);
        if (iterator == null || typeof iterator.next !== "function")
          throw new TypeError("Headers init is not iterable");
        const pairs = [];
        for (;;) {
          const step = iterator.next();
          if (step == null || typeof step !== "object") throw new TypeError("Iterator result is not an object");
          if (step.done) break;
          const entry = step.value;
          if (entry === null || (typeof entry !== "object" && typeof entry !== "string") ||
              typeof entry === "string" || typeof entry[Symbol.iterator] !== "function")
            throw new TypeError("Headers init entry must be a two-element sequence");
          const items = Array.from(entry);
          if (items.length !== 2) throw new TypeError("Headers init entry must have exactly two items");
          pairs.push(items);
        }
        for (const [k, v] of pairs) {
          const n = validateName(k);
          rawAppend(h, n, validateValue(normalizeValue(v), n));
        }
        return;
      }
      // A present-but-non-callable @@iterator is not iterable → TypeError
      // (undici/bun: `new Headers({ [Symbol.iterator]: null })` throws).
      if (Symbol.iterator in init) throw new TypeError("Headers init is not iterable");
      // record<ByteString, ByteString>: snapshot keys, then per key re-check
      // own enumerability, Get, ToString — in that interleaved order.
      const keys = Object.keys(init);
      for (const key of keys) {
        const desc = Object.getOwnPropertyDescriptor(init, key);
        if (!desc || !desc.enumerable) continue;
        const value = "" + init[key];
        const n = validateName(key);
        rawAppend(h, n, validateValue(normalizeValue(value), n));
      }
    };

    const HeadersProxy = new Proxy(OrigHeaders, {
      construct(target, args, newTarget) {
        const h = Reflect.construct(target, [], newTarget);
        // (re)initialize internals in case the base ctor shape changes
        if (!(h._m instanceof Map)) h._m = new Map();
        if (!Array.isArray(h._sc)) h._sc = [];
        if (!(h._names instanceof Map)) h._names = new Map();
        fillFromInit(h, args[0]);
        return h;
      },
    });
    G.Headers = HeadersProxy;

    // ------------------------------------------------ Bun.inspect formatting
    const PReflectApply = Reflect.apply;
    const headersInspect = (h) => {
      const o = h.toJSON();
      const keys = Object.keys(o);
      if (keys.length === 0) return "Headers {}";
      return "Headers " + JSON.stringify(o, null, 2).replace(/(\s+})$/, ",$1");
    };
    const util = M && (M["util"] || M["node:util"]);
    if (util && typeof util.inspect === "function") {
      const prev = util.inspect;
      // Forward EVERY argument: Bun.inspect also takes the positional
      // (value, colors, depth) form, which a 2-parameter wrapper would drop.
      // Forwarding goes through a primordial Reflect.apply rather than a
      // `...rest` spread — a spread call re-reads Array.prototype[Symbol.iterator]
      // at call time, so deleting it (which the corpus does deliberately) would
      // make this outermost wrapper throw before the tampering-hardened
      // inspectValue underneath ever runs.
      const wrapped = function inspect(value) {
        if (value !== null && typeof value === "object" && value instanceof OrigHeaders && value._m instanceof Map) {
          try { return headersInspect(value); } catch (_) {}
        }
        return PReflectApply(prev, this, arguments);
      };
      for (const k of Object.keys(prev)) { try { wrapped[k] = prev[k]; } catch (_) {} }
      util.inspect = wrapped;
      if (G.Bun && G.Bun.inspect === prev) {
        try {
          const bunPrev = G.Bun.inspect;
          for (const k of Object.keys(bunPrev)) { try { wrapped[k] = bunPrev[k]; } catch (_) {} }
          G.Bun.inspect = wrapped;
        } catch (_) {}
      } else if (G.Bun && typeof G.Bun.inspect === "function") {
        try {
          const bunPrev = G.Bun.inspect;
          const bunWrapped = function inspect(value) {
            if (value !== null && typeof value === "object" && value instanceof OrigHeaders && value._m instanceof Map) {
              try { return headersInspect(value); } catch (_) {}
            }
            return PReflectApply(bunPrev, this, arguments);
          };
          for (const k of Object.keys(bunPrev)) { try { bunWrapped[k] = bunPrev[k]; } catch (_) {} }
          G.Bun.inspect = bunWrapped;
        } catch (_) {}
      }
    }

    // ------------------------------------------- fetch synchronous validation
    // fetch(url, { headers }) must throw synchronously on invalid names/values
    // (bun validates while building the request). Convert the init eagerly.
    if (typeof G.fetch === "function") {
      const origFetch = G.fetch;
      const wrappedFetch = function fetch(input, init) {
        if (init && init.headers !== undefined && init.headers !== null && !(init.headers instanceof OrigHeaders))
          void new G.Headers(init.headers);
        return origFetch.apply(this, arguments);
      };
      for (const k of Object.keys(origFetch)) { try { wrappedFetch[k] = origFetch[k]; } catch (_) {} }
      G.fetch = wrappedFetch;
    }

    // -------------------------------- bun:internal-for-testing lowercase kernel
    if (M && M["bun:internal-for-testing"] && !M["bun:internal-for-testing"].lowercaseHeaderNameSIMD) {
      M["bun:internal-for-testing"].lowercaseHeaderNameSIMD = function lowercaseHeaderNameSIMD(s) {
        s = "" + s;
        let out = "";
        for (let i = 0; i < s.length; i++) {
          const c = s.charCodeAt(i);
          out += c >= 65 && c <= 90 ? String.fromCharCode(c + 32) : s[i];
        }
        return out;
      };
    }
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
