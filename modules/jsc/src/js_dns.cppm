// modules/jsc/src/js_dns.cppm — module mbun.jsc.js_dns
//
// Pure-JS node:dns / node:dns/promises / Bun.dns layer over the native
// getaddrinfo/getnameinfo primitives in __mbunDnsNative (runtime.cppm).
// Evaluated once at runtime init, AFTER kNetJS (it reuses node:net's isIP for
// lookupService address validation, and re-registers the load-only dns stubs).
//
// Behaviour blueprint: bun's src/dns/lib.rs (family/order/backend option parsing,
// address_to_string) + src/runtime/dns_jsc/dns.rs (Resolver dispatch, RECORD_TYPE
// validation message, DNSException code/name). The pure result-shaping and the
// ares↔node error-code maps live in module mbun.dns; the native half drives the
// OS resolver and RFC 1035 record transport. DEFERRED(S-net): the process-global
// DNS cache, Windows Winsock record transport, and setServers reconfiguration.
export module mbun.jsc.js_dns;

import std;

namespace mbun::jsc::js_dns {

export constexpr std::string_view kDnsJS = R"JS(
(function () {
  "use strict";
  const G = globalThis;
  const DN = G.__mbunDnsNative;                 // native getaddrinfo/getnameinfo (POSIX)
  const M = G.__mbunNativeModules || (G.__mbunNativeModules = {});
  const def = (names, mod) => { for (const n of names) { M[n] = mod; M["node:" + n] = mod; } };
  const netMod = () => M["net"] || M["node:net"] || {};
  const isIP = (s) => { const f = netMod().isIP; return typeof f === "function" ? f(s) : 0; };

  const promisifyCustom = Symbol.for("nodejs.util.promisify.custom");
  // Ensure util.promisify.custom is the canonical symbol so the promisify-factory
  // wiring (dns[m][util.promisify.custom] === dns.promises[m]) resolves.
  { const P = (M["util"] || M["node:util"] || {}).promisify; if (P && !P.custom) P.custom = promisifyCustom; }

  // ── option / record-type parsing (mirrors mbun.dns.options) ────────────────
  const normFamily = (f) => {
    if (f === 4 || f === "IPv4" || f === "ipv4") return 4;
    if (f === 6 || f === "IPv6" || f === "ipv6") return 6;
    return 0; // 0 / "any" / undefined → unspecified
  };
  const RECORD_TYPES = ["A", "AAAA", "ANY", "CAA", "CNAME", "MX", "NAPTR", "NS", "PTR", "SOA", "SRV", "TXT"];
  // Bun.dns.resolve rejects NAPTR — bun's RECORD_TYPE_MAP has no NAPTR key
  // (dns.rs:3933-3942, literal at :4999). node:dns keeps NAPTR (resolveNaptr).
  const BUN_DNS_RECORD_TYPES = ["A", "AAAA", "ANY", "CAA", "CNAME", "MX", "NS", "PTR", "SOA", "SRV", "TXT"];
  const TYPE_CODES = { A: 1, AAAA: 28, ANY: 255, CAA: 257, CNAME: 5, MX: 15,
    NAPTR: 35, NS: 2, PTR: 12, SOA: 6, SRV: 33, TXT: 16 };

  // ── error shaping ──────────────────────────────────────────────────────────
  // Bun.dns rejects with a DNSException (DNS_-prefixed code); node:dns callbacks
  // receive the bare code (ENOTFOUND, …) with syscall set.
  const dnsException = (code, syscall, hostname) => {
    const e = new Error(code + (syscall ? ", " + syscall : "") + (hostname ? " " + hostname : ""));
    e.code = code; e.name = "DNSException"; e.errno = code;
    if (syscall) e.syscall = syscall;
    if (hostname) e.hostname = hostname;
    return e;
  };
  const nodeError = (code, syscall, hostname) => {
    const e = new Error(syscall + " " + code + (hostname ? " " + hostname : ""));
    e.code = code; e.errno = code;
    if (syscall) e.syscall = syscall;
    if (hostname) e.hostname = hostname;
    return e;
  };
  const soon = (fn) => { G.queueMicrotask ? G.queueMicrotask(fn) : Promise.resolve().then(fn); };

  // ── node argument validators (lib/internal/validators.js subset) ────────────
  // node:dns validates every public entry point synchronously and the corpus
  // asserts the exact ERR_INVALID_ARG_TYPE / ERR_OUT_OF_RANGE message, so the
  // shapes below are translated from lib/internal/errors.js
  // (determineSpecificType + the ERR_INVALID_ARG_TYPE determiner rules) rather
  // than approximated — test/common/index.js invalidArgTypeHelper builds the
  // expected string with the same algorithm.
  const specificType = (v) => {
    if (v === null) return "null";
    if (v === undefined) return "undefined";
    const t = typeof v;
    if (t === "bigint") return "type bigint (" + String(v) + "n)";
    if (t === "number") {
      if (v === 0) return 1 / v === -Infinity ? "type number (-0)" : "type number (0)";
      if (v !== v) return "type number (NaN)";
      return "type number (" + String(v) + ")";
    }
    if (t === "boolean") return v ? "type boolean (true)" : "type boolean (false)";
    if (t === "symbol") return "type symbol (" + String(v) + ")";
    if (t === "function") return "function " + v.name;
    if (t === "object") {
      if (v.constructor && "name" in v.constructor) return "an instance of " + v.constructor.name;
      return "[Object: null prototype] {}";
    }
    if (t === "string") {
      let s = v;
      if (s.length > 28) s = s.slice(0, 25) + "...";
      return s.indexOf("'") === -1 ? "type string ('" + s + "')" : "type string (" + JSON.stringify(s) + ")";
    }
    return "type " + t + " (" + String(v) + ")";
  };
  // `name` carrying a dot is a property (options.timeout), otherwise an argument.
  const argTypeError = (name, determinerText, actual) => {
    const kind = String(name).indexOf(".") !== -1 ? "property" : "argument";
    const e = new TypeError('The "' + name + '" ' + kind + " " + determinerText +
      ". Received " + specificType(actual));
    e.code = "ERR_INVALID_ARG_TYPE";
    return e;
  };
  const outOfRange = (name, range, actual) => {
    const e = new RangeError('The value of "' + name + '" is out of range. It must be ' +
      range + ". Received " + specificType(actual));
    e.code = "ERR_OUT_OF_RANGE";
    return e;
  };
  const validateString = (v, name) => {
    if (typeof v !== "string") throw argTypeError(name, "must be of type string", v);
  };
  const validateArray = (v, name) => {
    if (!Array.isArray(v)) throw argTypeError(name, "must be an instance of Array", v);
  };
  const validateFunction = (v, name) => {
    if (typeof v !== "function") throw argTypeError(name, "must be of type function", v);
  };
  const validateInt32 = (v, name, min, max) => {
    if (min === undefined) min = -2147483648;
    if (max === undefined) max = 2147483647;
    if (typeof v !== "number") throw argTypeError(name, "must be of type number", v);
    if (!Number.isInteger(v)) throw outOfRange(name, "an integer", v);
    if (v < min || v > max) throw outOfRange(name, ">= " + min + " && <= " + max, v);
  };
  const validateUint32 = (v, name, positive) => {
    if (typeof v !== "number") throw argTypeError(name, "must be of type number", v);
    if (!Number.isInteger(v)) throw outOfRange(name, "an integer", v);
    const min = positive ? 1 : 0;
    if (v < min || v > 4294967295) throw outOfRange(name, ">= " + min + " && <= 4294967295", v);
  };
  // ERR_INVALID_ARG_VALUE reports inspect(value), not determineSpecificType.
  const inspectValue = (v) => {
    if (typeof v === "string") return "'" + v + "'";
    if (typeof v === "bigint") return String(v) + "n";
    if (v === null || typeof v !== "object") return String(v);
    try { return JSON.stringify(v); } catch (e) { return String(v); }
  };
  const validateOneOf = (v, name, allowed) => {
    if (allowed.indexOf(v) !== -1) return;
    const list = allowed.map((x) => (typeof x === "string" ? "'" + x + "'" : String(x))).join(", ");
    const e = new TypeError("The " + (String(name).indexOf(".") !== -1 ? "property" : "argument") +
      " '" + name + "' must be one of: " + list + ". Received " + inspectValue(v));
    e.code = "ERR_INVALID_ARG_VALUE";
    throw e;
  };
  const VALID_DNS_ORDERS = ["verbatim", "ipv4first", "ipv6first"];

  // Native lookup completes on the JS thread through DN.drain(), which is
  // composed into the runtime's existing IO pump below.
  //
  // A resolver request in flight is a BOUNDED in-flight operation, so it belongs
  // in the reactor's `pending` channel: without it, a program whose only
  // outstanding work is a getaddrinfo has rem == 0 and NET.items empty-or-parked,
  // and __mbun_pump_idle_ms hands the pump a 60-SECOND park in poll() over the
  // net fds — a set the resolver's completion is not in. The callback then lands
  // a minute late or never (the pump's idle-grace bail fires first). This was
  // invisible while every resolver caller also owned a registered socket whose
  // readability woke the park; node:dgram resolves the bind/send address BEFORE
  // there is any socket to poll, which is what exposed it.
  const inflight = (callback) => {
    const NET = G.__mbunNet;
    if (!NET) return callback;
    NET.pending++;
    let done = false;
    return (r) => {
      if (!done) { done = true; NET.pending = Math.max(0, NET.pending - 1); }
      return callback(r);
    };
  };
  // ── internalBinding('cares_wrap') ──────────────────────────────────────────
  // node's dns layer never calls the OS resolver directly: every lookup goes
  // through `cares.getaddrinfo(req, hostname, family, hints, order)` on the
  // cares_wrap binding. Several corpus files replace that one function before
  // requiring node:dns — to observe the arguments dns.lookup computed, or to
  // force a failure — so the indirection is part of the observable contract and
  // not an implementation detail. Registered through the extension point in
  // builtins/node_internal_binding.cppm; the binding object is cached there, so
  // a test's monkey-patch is seen by every later lookup.
  // Blueprint: src/cares_wrap.{h,cc} (the DNS_ORDER_* constants, GetAddrInfo's
  // argument order, sort_addresses) and lib/dns.js (onlookup/onlookupall, which
  // receive an array of address STRINGS and derive the family with isIP).
  const DNS_ORDER_VERBATIM = 0, DNS_ORDER_IPV4_FIRST = 1, DNS_ORDER_IPV6_FIRST = 2;
  // node sorts in C++ (sort_addresses): a stable partition by family, leaving the
  // resolver's relative order intact inside each family.
  const sortByOrder = (rows, order) => {
    if (order !== DNS_ORDER_IPV4_FIRST && order !== DNS_ORDER_IPV6_FIRST) return rows;
    const first = order === DNS_ORDER_IPV4_FIRST ? 4 : 6;
    const head = [], tail = [];
    for (const row of rows) (row && row.family === first ? head : tail).push(row);
    return head.concat(tail);
  };
  // A stubbed cares.getaddrinfo hands back a NUMERIC uv errno
  // (internalBinding('uv').UV_ENOMEM), which node turns into the code string via
  // uv.errname before building the DNSException. Accept both forms.
  const uvName = (err) => {
    if (typeof err === "string") return err;
    if (typeof err === "number" && err !== 0) {
      const ib = G.__mbunInternalBinding;
      if (typeof ib === "function") {
        try { return ib("uv").errname(err); } catch (e) { /* fall through */ }
      }
    }
    return "ENOTFOUND";
  };
  const uvErrno = (code) => {
    // node hands the uv/ares error number to DNSException, which turns it back
    // into a string via uv.errname. Keep the code string itself: the surrounding
    // layer already maps codes, and inventing a wrong number would mislabel it.
    return code || "ENOTFOUND";
  };
  const caresWrap = {
    DNS_ORDER_VERBATIM, DNS_ORDER_IPV4_FIRST, DNS_ORDER_IPV6_FIRST,
    // Legacy aliases node kept for the pre-`order` API.
    AI_ADDRCONFIG: 1024, AI_ALL: 256, AI_V4MAPPED: 2048,
    getaddrinfo(req, hostname, family, hints, order) {
      if (!DN || !DN.lookup) { soon(() => req.oncomplete(uvErrno("ENOTFOUND"))); return 0; }
      DN.lookup(String(hostname), family | 0, hints | 0, inflight((r) => {
        if (!Array.isArray(r)) { req.oncomplete(uvErrno(r && r.error)); return; }
        // node's oncomplete takes address strings; the family is recovered with
        // isIP, exactly as lib/dns.js onlookup/onlookupall do.
        req.oncomplete(0, sortByOrder(r, order | 0).map((x) => x.address));
      }));
      return 0;
    },
    getnameinfo(req, ip, port) {
      if (!DN || !DN.lookupService) { soon(() => req.oncomplete(uvErrno("ENOTFOUND"))); return 0; }
      DN.lookupService(String(ip), port | 0, inflight((r) => {
        if (r && r.error) { req.oncomplete(uvErrno(r.error)); return; }
        req.oncomplete(0, r.hostname, r.service);
      }));
      return 0;
    },
    strerror: (code) => String(code),
    // The request/channel wrappers node's dns layer allocates. They carry no
    // native state here — the callback closure does — but they must exist and be
    // constructible, and a Resolver's _handle must answer getServers/setServers.
    GetAddrInfoReqWrap: function GetAddrInfoReqWrap() {},
    GetNameInfoReqWrap: function GetNameInfoReqWrap() {},
    QueryReqWrap: function QueryReqWrap() {},
    ChannelWrap: function ChannelWrap(timeout, tries, maxTimeout) {
      this._timeout = timeout; this._tries = tries; this._maxTimeout = maxTimeout;
      this._servers = [];
    },
  };
  caresWrap.ChannelWrap.prototype.getServers = function () { return this._servers.slice(); };
  caresWrap.ChannelWrap.prototype.setServers = function (list) { this._servers = list ? list.slice() : []; return 0; };
  caresWrap.ChannelWrap.prototype.setLocalAddress = function () {};
  caresWrap.ChannelWrap.prototype.cancel = function () {};
  // node src/cares_wrap.cc CanonicalizeIP: the inet_pton/inet_ntop round trip
  // that turns "fe80:0:0:0:0:0:0:1" into "fe80::1". node:tls' checkServerIdentity
  // compares IP SANs through it, and test-tls-canonical-ip reads it straight off
  // this binding. Backed natively by __mbunNodeTlsNative.canonicalizeIP
  // (runtime/node_tls.inc) — the same function node:tls already uses, so the two
  // callers can never disagree.
  caresWrap.canonicalizeIP = function canonicalizeIP(ip) {
    const N = G.__mbunNodeTlsNative;
    if (N && typeof N.canonicalizeIP === "function") return N.canonicalizeIP(ip);
    return undefined;
  };
  if (typeof G.__mbunInternalBindingDefine === "function") {
    G.__mbunInternalBindingDefine("cares_wrap", () => caresWrap);
  }
  // Resolved at CALL time, never captured: a corpus file replaces
  // cares.getaddrinfo after this partition has already run.
  const cares = () => {
    const ib = G.__mbunInternalBinding;
    if (typeof ib !== "function") return caresWrap;
    try { return ib("cares_wrap"); } catch (e) { return caresWrap; }
  };
  const rawLookup = (host, family, flags, callback, order) => {
    const CW = cares();
    if (typeof CW.getaddrinfo !== "function") { soon(() => callback({ error: "ENOTFOUND" })); return; }
    const req = new CW.GetAddrInfoReqWrap();
    req.hostname = String(host);
    req.family = family | 0;
    // Translate node's (err, addressStrings) back into this layer's
    // [{ address, family }] rows.
    req.oncomplete = (err, addresses) => {
      if (err || !Array.isArray(addresses)) { callback({ error: uvErrno(err) }); return; }
      callback(addresses.map((a) => ({ address: a, family: (family | 0) || isIP(a) })));
    };
    const err = CW.getaddrinfo(req, req.hostname, family | 0, flags | 0,
                               order === undefined ? DNS_ORDER_VERBATIM : order | 0);
    if (err) soon(() => callback({ error: uvErrno(err) }));
  };
  const rawReverse = (ip, callback) => {
    if (DN && DN.reverse) DN.reverse(String(ip), inflight(callback));
    else soon(() => callback({ error: "ENOTFOUND" }));
  };
  const rawLookupService = (address, port, callback) => {
    if (DN && DN.lookupService) DN.lookupService(String(address), port | 0, inflight(callback));
    else soon(() => callback({ error: "ENOTFOUND" }));
  };
  // `servers` (a dns.Resolver's own nameserver list, "IP[:PORT]"/"[IPv6]:PORT"
  // strings) plus its timeout/tries are threaded to the native record transport;
  // absent/empty means /etc/resolv.conf + the transport defaults, which is what
  // the module-level dns.resolve* uses. ref: runtime/dns.inc dnsn_resolve_cb.
  const rawResolve = (host, type, callback, servers, timeout, tries, channel) => {
    const CW = cares();
    const query = "query" + type[0] + type.slice(1).toLowerCase();
    const handle = channel || (typeof CW.ChannelWrap === "function"
      ? new CW.ChannelWrap(timeout, tries) : null);
    // resolve* goes through ChannelWrap in node, so a synchronous c-ares
    // failure remains observable to callback and promise callers alike.
    if (handle && typeof handle[query] === "function") {
      const req = typeof CW.QueryReqWrap === "function" ? new CW.QueryReqWrap() : {};
      let done = false;
      req.oncomplete = (err, rows) => {
        if (done) return;
        done = true;
        callback(err ? { error: uvName(err) } : rows);
      };
      const err = handle[query](req, String(host));
      if (err) soon(() => req.oncomplete(err));
      return;
    }
    if (DN && DN.resolve) DN.resolve(String(host), TYPE_CODES[type] | 0, callback,
                                     servers && servers.length ? servers : undefined,
                                     typeof timeout === "number" ? timeout : undefined,
                                     typeof tries === "number" ? tries : undefined);
    else soon(() => callback({ error: "ENOTIMP" }));
  };

  if (DN && typeof DN.drain === "function") {
    const previousIOTick = G.__mbun_io_tick;
    G.__mbun_io_tick = function () {
      const prior = typeof previousIOTick === "function" ? Number(previousIOTick()) || 0 : 0;
      return prior + (Number(DN.drain()) || 0);
    };
  }

  // ── Bun.dns ────────────────────────────────────────────────────────────────
  const BunDns = {
    lookup(hostname, options) {
      return new Promise((resolve, reject) => {
        let family = 0, flags = 0;
        if (options && typeof options === "object") {
          family = normFamily(options.family);
          if (typeof options.flags === "number") flags = options.flags;
        }
        rawLookup(hostname, family, flags, (r) => {
          if (Array.isArray(r)) resolve(r);
          else reject(dnsException("DNS_" + (r.error || "ENOTFOUND"), "getaddrinfo", String(hostname)));
        });
      });
    },
    lookupService(address, port) {
      // Synchronous argument validation (mirrors bun's UTF-16 test expectations).
      if (typeof address !== "string" || address.length === 0) {
        throw new TypeError("Expected address to be a non-empty string for 'lookupService'.");
      }
      if (isIP(address) === 0) {
        throw new TypeError('The "address" argument is invalid. Received type string (' + JSON.stringify(String(address)).replace(/"/g, "'") + ")");
      }
      return new Promise((resolve, reject) => {
        rawLookupService(address, port, (r) => {
          if (r && r.error) reject(dnsException("DNS_" + r.error, "getnameinfo", address));
          else resolve(r);
        });
      });
    },
    resolve(hostname, type) {
      const t = (type == null ? "A" : String(type));
      // Bun.dns.resolve validates against its own record map, which has NO NAPTR
      // key (unlike node:dns's RECORD_TYPES); the thrown list and the accepted
      // set both exclude it. ref: compat/bun/test/js/bun/dns/resolve-dns.test.ts.
      if (BUN_DNS_RECORD_TYPES.indexOf(t) === -1) {
        throw new TypeError('The property "record" is invalid. Expected one of: ' + BUN_DNS_RECORD_TYPES.join(", ") +
          ", received type " + typeof type + " (" + JSON.stringify(String(type)).replace(/"/g, "'") + ")");
      }
      if (t === "A" || t === "AAAA") {
        return new Promise((resolve, reject) => {
          rawLookup(hostname, t === "A" ? 4 : 6, 0, (r) => {
            if (Array.isArray(r)) resolve(r.map((x) => x.address));
            else reject(dnsException("DNS_" + (r.error || "ENOTFOUND"), "query" + t, String(hostname)));
          });
        });
      }
      return new Promise((resolve, reject) => {
        rawResolve(hostname == null ? "" : hostname, t, (r) => {
          const syscall = "query" + t[0] + t.slice(1).toLowerCase();
          if (r && r.error) reject(dnsException("DNS_" + r.error, syscall, String(hostname == null ? "" : hostname)));
          else resolve(r);
        });
      });
    },
    reverse(ip) {
      return new Promise((resolve, reject) => {
        rawReverse(ip, (r) => {
          if (Array.isArray(r)) resolve(r);
          else reject(dnsException("DNS_" + r.error, "getHostByAddr", String(ip)));
        });
      });
    },
    // DEFERRED(S-net): the process-global DNS cache. Stubs keep the API present.
    prefetch() { return undefined; },
    getCacheStats() {
      return { cacheHitsCompleted: 0, cacheHitsInflight: 0, cacheMisses: 0, size: 0, errors: 0, totalCount: 0 };
    },
    setServers(list) {
      if (!Array.isArray(list)) throw new TypeError("The 'servers' argument must be an array.");
      for (const entry of list) {
        const family = Array.isArray(entry) ? entry[0] : undefined;
        if (typeof family !== "number" || !Number.isInteger(family) || family < -2147483648 || family > 2147483647) {
          throw new TypeError("The 'family' property must be an int32.");
        }
      }
    },
    getServers() { return getServers_(); },
  };
  if (G.Bun) G.Bun.dns = BunDns;

  // ── the module-level default resolver's nameserver list ────────────────────
  // node binds dns.setServers/getServers/resolve* to a hidden default c-ares
  // channel (internal/dns/utils.js bindDefaultResolver), so dns.setServers is a
  // validating mutation the module-level dns.resolve* then observes — not a
  // no-op. An empty list means "whatever /etc/resolv.conf says".
  const defaultServers = [];
  let defaultServersConfigured = false;

  // ── /etc/resolv.conf nameservers for dns.getServers() ──────────────────────
  const getServers_ = () => {
    // A caller's explicit `setServers([])` means no servers, not a request to
    // re-read resolv.conf. Before the first mutation we expose the system list.
    if (defaultServersConfigured) return defaultServers.slice();
    try {
      const fs = M["fs"] || M["node:fs"];
      if (!fs || typeof fs.readFileSync !== "function") return [];
      const txt = fs.readFileSync("/etc/resolv.conf", "utf-8");
      const out = [];
      for (const line of txt.split("\n")) {
        const parts = line.trim().split(/\s+/);
        if (parts.length >= 2 && parts[0] === "nameserver") out.push(parts[1]);
      }
      return out;
    } catch { return []; }
  };

  // ── node:dns/promises ──────────────────────────────────────────────────────
  // NOTE: there is deliberately no result cache here. bun coalesces recent
  // resolver answers, but node:dns does not: every dns.lookup is one
  // cares.getaddrinfo call, and the corpus counts those calls to check the
  // arguments dns.lookup computed (test-dns-default-order-*, which replace
  // cares.getaddrinfo and assert one entry per lookup). A cache silently
  // swallowed every repeat lookup and made those files unfixable.
  // node lib/dns.js lookup() / internal/dns/promises.js lookup(): parse and
  // validate the arguments, in node's order, and return the numbers the
  // cares_wrap binding takes. Shared by the callback and promise entry points so
  // both reject identically.
  const AI_MASK = 1024 | 256 | 2048;  // AI_ADDRCONFIG | AI_ALL | AI_V4MAPPED
  const parseLookupOptions = (hostname, options) => {
    if (hostname) validateString(hostname, "hostname");
    let family = 0, all = false, hints = 0;
    let orderName = getResultOrder_();
    if (typeof options === "number") {
      validateOneOf(options, "family", [0, 4, 6]);
      family = options;
    } else if (options !== undefined && options !== null && typeof options !== "object") {
      throw argTypeError("options", "must be one of type integer or object", options);
    } else if (options) {
      if (options.hints !== undefined && options.hints !== null) {
        if (typeof options.hints !== "number") throw argTypeError("options.hints", "must be of type number", options.hints);
        hints = options.hints >>> 0;
        // node validateHints: anything outside the AI_* mask is refused.
        if ((hints & ~AI_MASK) !== 0) {
          const e = new TypeError("The argument 'hints' is invalid. Received " + inspectValue(options.hints));
          e.code = "ERR_INVALID_ARG_VALUE";
          throw e;
        }
      }
      if (options.family !== undefined && options.family !== null) {
        if (options.family === "IPv4") family = 4;
        else if (options.family === "IPv6") family = 6;
        else { validateOneOf(options.family, "options.family", [0, 4, 6]); family = options.family; }
      }
      if (options.all !== undefined && options.all !== null) {
        if (typeof options.all !== "boolean")
          throw argTypeError("options.all", "must be of type boolean", options.all);
        all = options.all;
      }
      if (options.verbatim !== undefined) {
        if (typeof options.verbatim !== "boolean")
          throw argTypeError("options.verbatim", "must be of type boolean", options.verbatim);
        orderName = options.verbatim ? "verbatim" : "ipv4first";
      }
      if (options.order !== undefined) {
        validateOneOf(options.order, "order", VALID_DNS_ORDERS);
        orderName = options.order;
      }
    }
    const order = orderName === "ipv4first" ? DNS_ORDER_IPV4_FIRST
      : orderName === "ipv6first" ? DNS_ORDER_IPV6_FIRST : DNS_ORDER_VERBATIM;
    return { family, all, hints, order };
  };
  const promiseLookup = (hostname, options) => {
    // Validation is synchronous: dns.lookup(1, {}) throws, it does not reject.
    const o = parseLookupOptions(hostname, options);
    return promiseLookup_(hostname, o);
  };
  // Mirrors internal/dns/promises.js createLookupPromise: the request object
  // carries `resolve`/`reject` and node's onlookup/onlookupall read them off
  // `this`. Corpus files stub cares.getaddrinfo and wrap exactly those two
  // properties, so the protocol — not just the result — is observable.
  const promiseLookup_ = (hostname, o) => new Promise((resolve, reject) => {
    if (!hostname) { resolve(o.all ? [] : { address: null, family: 4 }); return; }
    const host = String(hostname);
    if (host.indexOf("\0") !== -1) { reject(nodeError("ENOTFOUND", "getaddrinfo", host)); return; }
    const CW = cares();
    if (typeof CW.getaddrinfo !== "function") { reject(nodeError("ENOTFOUND", "getaddrinfo", host)); return; }
    const req = new CW.GetAddrInfoReqWrap();
    req.family = o.family;
    req.hostname = host;
    req.resolve = resolve;
    req.reject = reject;
    req.oncomplete = o.all
      ? function onlookupall(err, addresses) {
          if (err) { this.reject(nodeError(uvName(err), "getaddrinfo", this.hostname)); return; }
          const fam = this.family;
          this.resolve(addresses.map((a) => ({ address: a, family: fam || isIP(a) })));
        }
      : function onlookup(err, addresses) {
          if (err) { this.reject(nodeError(uvName(err), "getaddrinfo", this.hostname)); return; }
          if (!addresses || addresses.length === 0) { this.reject(nodeError("ENOTFOUND", "getaddrinfo", this.hostname)); return; }
          this.resolve({ address: addresses[0], family: this.family || isIP(addresses[0]) });
        };
    const err = CW.getaddrinfo(req, host, o.family, o.hints, o.order);
    if (err) reject(nodeError(uvName(err), "getaddrinfo", host));
  });

  // The record queries validate their hostname OUTSIDE the Promise executor:
  // node's resolve*/resolveNs throw ERR_INVALID_ARG_TYPE synchronously rather
  // than returning a rejected promise (test-dns-resolvens-typeerror asserts the
  // synchronous form for both dns.* and dns.promises.*).
  const promiseAddresses = (hostname, fam, rr, options) => {
    validateString(hostname, "name");
    return new Promise((resolve, reject) => {
      const host = hostname;
      const type = fam === 6 ? "AAAA" : "A";
      rawResolve(host, type, (r) => {
        if (Array.isArray(r)) resolve(options && options.ttl ? r : r.map((x) => x.address));
        else reject(nodeError(r.error || "ENOTFOUND", rr, host));
      });
    });
  };

  // `res` (optional) is the dns.Resolver whose servers/timeout/tries this query
  // must use; the module-level dns.resolve* rides the default server list that
  // dns.setServers maintains.
  const promiseRecord = (hostname, type, rr, res) => {
    validateString(hostname, "name");
    return promiseRecord_(hostname, type, rr, res);
  };
  const promiseRecord_ = (hostname, type, rr, res) => new Promise((resolve, reject) => {
    const host = String(hostname == null ? "" : hostname);
    const servers = res ? res._serverText : defaultServers;
    rawResolve(host, type, (r) => {
      if (r && r.error) reject(nodeError(r.error, rr, host));
      else resolve(r);
    }, servers, res && res._timeout, res && res._tries, res && res._handle);
  });

  const promiseReverse = (ip) => new Promise((resolve, reject) => {
    rawReverse(ip, (r) => {
      if (Array.isArray(r)) resolve(r);
      else reject(nodeError(r.error, "getHostByAddr", String(ip)));
    });
  });

  // Validation throws synchronously (node/bun contract: expect(()=>...).toThrow),
  // so it lives outside the Promise executor.
  const validateService = (address) => {
    if (typeof address !== "string" || address.length === 0)
      throw new TypeError("Expected address to be a non-empty string for 'lookupService'.");
    if (isIP(address) === 0)
      throw new TypeError('The "address" argument is invalid. Received type string (' + JSON.stringify(String(address)).replace(/"/g, "'") + ")");
  };
  // node lib/dns.js lookupService / internal/dns/promises.js
  // createLookupServicePromise both go through cares.getnameinfo and surface a
  // binding error IMMEDIATELY — the callback form throws, the promise form
  // rejects. test-dns-lookupService stubs getnameinfo to return UV_ENOENT and
  // asserts both shapes, so the split matters.
  const nodeValidateService = (address, port) => {
    // bun surfaces its own wording for node:dns.lookupService (see
    // compat/bun/test/js/node/dns/node-dns.test.js "test invalid arguments"):
    // an empty address gets the "non-empty string" TypeError, any other
    // non-IP address gets the ERR_INVALID_ARG_TYPE-shaped sentence. node's
    // own test-dns.js asserts a third wording, but that file already fails
    // earlier (dns.lookup('') arg-type) so nothing green is traded here.
    if (typeof address !== "string" || address.length === 0) {
      const e = new TypeError("Expected address to be a non-empty string for 'lookupService'.");
      e.code = "ERR_INVALID_ARG_VALUE";
      throw e;
    }
    if (isIP(address) === 0) {
      const e = new TypeError('The "address" argument is invalid. Received type string (' +
        JSON.stringify(String(address)).replace(/"/g, "'") + ")");
      e.code = "ERR_INVALID_ARG_VALUE";
      throw e;
    }
    if (typeof port !== "number" || !Number.isInteger(port) || port < 0 || port > 65535) {
      const e = new RangeError('The value of "port" is out of range. It must be >= 0 && <= 65535. Received ' + inspectValue(port));
      e.code = "ERR_SOCKET_BAD_PORT";
      throw e;
    }
  };
  // Starts the request; `onDone(err, result)` receives the async completion.
  // Returns a synchronous error from the binding (never throws it itself) so each
  // caller can throw or reject as node does.
  const startLookupService = (address, port, onDone) => {
    const CW = cares();
    if (typeof CW.getnameinfo !== "function") return "ENOTFOUND";
    const req = new CW.GetNameInfoReqWrap();
    req.hostname = String(address);
    req.port = port | 0;
    req.oncomplete = function onlookupservice(err, hostname, service) {
      if (err) { onDone(nodeError(uvName(err), "getnameinfo", this.hostname)); return; }
      onDone(null, { hostname, service });
    };
    const err = CW.getnameinfo(req, req.hostname, port | 0);
    return err ? uvName(err) : null;
  };
  const promiseLookupService = (address, port) => {
    nodeValidateService(address, port);
    return new Promise((resolve, reject) => {
      const err = startLookupService(address, port, (e, r) => (e ? reject(e) : resolve(r)));
      if (err) reject(nodeError(err, "getnameinfo", String(address)));
    });
  };
  // The callback form validates, then throws a binding error synchronously.
  const cbLookupService = function lookupService(address, port, callback) {
    nodeValidateService(address, port);
    validateFunction(callback, "callback");
    const err = startLookupService(address, port,
      (e, r) => (e ? callback(e) : callback(null, r.hostname, r.service)));
    if (err) throw nodeError(err, "getnameinfo", String(address));
    return undefined;
  };
  cbLookupService[promisifyCustom] = promiseLookupService;

  const promiseResolve = (hostname, rrtype) => {
    if (rrtype !== undefined) validateString(rrtype, "rrtype");
    const t = (rrtype === undefined ? "A" : rrtype);
    if (t === "A") return promiseAddresses(hostname, 4, "queryA");
    if (t === "AAAA") return promiseAddresses(hostname, 6, "queryAaaa");
    if (RECORD_TYPES.indexOf(t) === -1)
      return Promise.reject(nodeError("EBADQUERY", "query" + t, String(hostname == null ? "" : hostname)));
    return promiseRecord(hostname, t, "query" + t[0] + t.slice(1).toLowerCase());
  };

  // Shared default result order (node:dns, dns.promises and dns/promises all
  // observe the same setting — see issue #28948).
  // node internal/dns/utils.js initializeDns(): --dns-result-order seeds the
  // module default, and validateOneOf rejects a bad value at startup.
  let defaultResultOrder_ = "verbatim";
  {
    const argv = (G.process && G.process.execArgv) || [];
    for (const a of argv) {
      if (typeof a === "string" && a.startsWith("--dns-result-order=")) {
        const v = a.slice("--dns-result-order=".length);
        validateOneOf(v, "--dns-result-order", VALID_DNS_ORDERS);
        defaultResultOrder_ = v;
      }
    }
  }
  // node internal/dns/utils.js setDefaultResultOrder runs
  // validateOneOf(value, 'dnsOrder', validDnsOrders), i.e. ERR_INVALID_ARG_VALUE
  // — not a bare TypeError (test-dns-set-default-order asserts the code).
  const setResultOrder_ = (order) => {
    validateOneOf(order, "dnsOrder", VALID_DNS_ORDERS);
    defaultResultOrder_ = order;
  };
  const getResultOrder_ = () => defaultResultOrder_;

  // ── dns.Resolver / dns.promises.Resolver ──────────────────────────────────
  // An independent resolver: its own nameserver list plus timeout/tries, which
  // ride along to the native RFC 1035 transport (rawResolve's extra arguments).
  // Blueprint: node lib/internal/dns/utils.js (ResolverBase: setServers parsing
  // + ERR_INVALID_IP_ADDRESS, getServers echoing "IP" or "IP:PORT") and
  // lib/dns.js (the resolve* method table). bun keeps the same surface over
  // c-ares (src/runtime/dns_jsc/dns.rs Resolver).
  const invalidIPError = (value) => {
    const e = new TypeError("Invalid IP address: " + value);
    e.code = "ERR_INVALID_IP_ADDRESS";
    return e;
  };
  // "1.2.3.4" | "1.2.3.4:5353" | "[::1]:5353" | "::1" → validated, echoed back
  // verbatim (getServers returns what was set).
  const parseServerEntry = (entry, index) => {
    // node ResolverBase.setServers validates the element type BEFORE parsing it,
    // so a non-string reports ERR_INVALID_ARG_TYPE on "servers[i]" rather than
    // ERR_INVALID_IP_ADDRESS (test-dns-setservers-type-check asserts both).
    validateString(entry, "servers[" + (index === undefined ? 0 : index) + "]");
    let host = entry, port = 53;
    if (host.charCodeAt(0) === 91 /* [ */) {
      const close = host.indexOf("]");
      if (close === -1) throw invalidIPError(entry);
      const rest = host.slice(close + 1);
      host = host.slice(1, close);
      if (rest !== "") {
        if (rest.charCodeAt(0) !== 58 /* : */ || !/^[0-9]+$/.test(rest.slice(1))) throw invalidIPError(entry);
        port = Number(rest.slice(1));
      }
    } else {
      const colon = host.lastIndexOf(":");
      // One colon → IPv4:port; several → a bare IPv6 literal.
      if (colon !== -1 && host.indexOf(":") === colon) {
        const textPort = host.slice(colon + 1);
        if (!/^[0-9]+$/.test(textPort)) throw invalidIPError(entry);
        port = Number(textPort);
        host = host.slice(0, colon);
      }
    }
    const family = isIP(host);
    if (family === 0 || !Number.isInteger(port) || port < 1 || port > 65535) throw invalidIPError(entry);
    // c-ares normalizes an explicit :53 away and removes brackets from a
    // bracketed IPv6 server without a non-default port (test-dns.js).
    if (port === 53) return host;
    return family === 6 ? "[" + host + "]:" + port : host + ":" + port;
  };
  // dns.setServers / dns.promises.setServers / require('dns/promises').setServers
  // are all the default resolver's setServers, so they validate identically.
  // Every entry is parsed before anything is stored: node caches the old list and
  // restores it if c-ares rejects the new one, so a failed call must not leave a
  // half-applied server list behind.
  const setServers_ = (list) => {
    validateArray(list, "servers");
    // Array#map preserves holes, but c-ares ignores them. forEach also keeps
    // node's live-length behavior when an indexed getter shrinks the list.
    const parsed = [];
    list.forEach((entry, index) => { parsed.push(parseServerEntry(entry, index)); });
    defaultServers.length = 0;
    for (const entry of parsed) defaultServers.push(entry);
    defaultServersConfigured = true;
  };
  const RESOLVE_METHODS = {
    resolveAny: ["ANY", "queryAny"], resolveCname: ["CNAME", "queryCname"],
    resolveCaa: ["CAA", "queryCaa"], resolveMx: ["MX", "queryMx"],
    resolveNs: ["NS", "queryNs"], resolvePtr: ["PTR", "queryPtr"],
    resolveSoa: ["SOA", "querySoa"], resolveSrv: ["SRV", "querySrv"],
    resolveTxt: ["TXT", "queryTxt"], resolveNaptr: ["NAPTR", "queryNaptr"],
  };
  // in-addr.arpa / ip6.arpa name for a PTR query — a Resolver's reverse() must
  // go to ITS nameservers, so it cannot use the getnameinfo path.
  const arpaName = (ip) => {
    if (isIP(ip) === 4) return ip.split(".").reverse().join(".") + ".in-addr.arpa";
    // Expand the IPv6 literal to 32 nibbles, reversed (RFC 3596 2.5).
    const parts = ip.split("::");
    const head = parts[0] ? parts[0].split(":") : [];
    const tail = parts.length > 1 && parts[1] ? parts[1].split(":") : [];
    const groups = head.concat(new Array(8 - head.length - tail.length).fill("0"), tail);
    const nibbles = groups.map((g) => g.padStart(4, "0")).join("");
    return nibbles.split("").reverse().join(".") + ".ip6.arpa";
  };
  const makeResolverClass = (promiseStyle) => {
    // A promise-style method returns the promise; a callback-style one takes the
    // node (err, result) callback as its last argument and returns undefined.
    // `nameArg` names the leading positional argument so it can be validated
    // exactly where node does it — internal/dns/callback_resolver.js query()
    // runs validateString(name) BEFORE validateFunction(callback), so
    // `dns.resolveNs([])` must report the *name* error, not the callback one.
    const adapt = (fn, nameArg) => {
      const validateName = (a) => { if (nameArg) validateString(a[0], nameArg); };
      return promiseStyle
        ? function (...a) {
            validateName(a);
            const p = fn.apply(this, a);
            this._pendingQueries = (this._pendingQueries || 0) + 1;
            return p.then(
              (v) => { this._pendingQueries--; return v; },
              (e) => { this._pendingQueries--; throw e; },
            );
          }
        : function (...a) {
            validateName(a);
            const cb = a[a.length - 1];
            validateFunction(cb, "callback");
            const p = fn.apply(this, a.slice(0, -1));
            this._pendingQueries = (this._pendingQueries || 0) + 1;
            p.then(
              (v) => { this._pendingQueries--; cb(null, v); },
              (e) => { this._pendingQueries--; cb(e); },
            );
            return undefined;
          };
    };
    class Resolver {
      constructor(options) {
        const o = options && typeof options === "object" ? options : {};
        // node internal/dns/utils.js ResolverBase: validateTimeout /
        // validateTries / validateMaxTimeout run BEFORE the c-ares channel is
        // created, so an out-of-range option is a constructor throw.
        const timeout = o.timeout === undefined ? -1 : o.timeout;
        validateInt32(timeout, "options.timeout", -1);
        const tries = o.tries === undefined ? 4 : o.tries;
        validateInt32(tries, "options.tries", 1);
        const maxTimeout = o.maxTimeout === undefined ? 0 : o.maxTimeout;
        validateUint32(maxTimeout, "options.maxTimeout");
        const hide = (name, value) =>
          Object.defineProperty(this, name, { value, writable: true, enumerable: false, configurable: true });
        hide("_serverText", []);
        // node's defaults: timeout -1 ("use the c-ares default") and 4 tries.
        hide("_timeout", timeout);
        hide("_tries", tries);
        hide("_maxTimeout", maxTimeout);
        hide("_localAddress", null);
        hide("_pendingQueries", 0);
        const CW = cares().ChannelWrap || caresWrap.ChannelWrap;
        const handle = new CW(timeout, tries, maxTimeout);
        // ResolverBase exposes its c-ares handle. Seed it with the system
        // servers so its observable getServers() starts non-empty like node.
        handle.setServers(getServers_());
        hide("_handle", handle);
      }
      getServers() {
        const servers = this._handle.getServers();
        return Array.isArray(servers) ? servers : [];
      }
      setServers(list) {
        validateArray(list, "servers");
        if (this._pendingQueries > 0) {
          const e = new Error('c-ares failed to set servers: "There are pending queries." [' + list.join(", ") + "]");
          e.code = "ERR_DNS_SET_SERVERS_FAILED";
          throw e;
        }
        const parsed = [];
        list.forEach((entry, index) => { parsed.push(parseServerEntry(entry, index)); });
        this._serverText = parsed;
        this._handle.setServers(parsed);
      }
      // DEFERRED: in-flight native queries run on the resolver worker and are not
      // interruptible, so cancel() cannot abort them; it is a no-op rather than a
      // lie about having cancelled.
      cancel() {}
      // node ResolverBase.setLocalAddress: both arguments are validated as
      // strings, then handed to ares_set_local_ip4 / ares_set_local_ip6, which
      // are family-specific — so the ipv4 slot must not hold an IPv6 literal and
      // vice versa. A lone first argument may be either family (node passes it to
      // whichever setter matches). ref test-dns-setlocaladdress.
      setLocalAddress(ipv4, ipv6) {
        validateString(ipv4, "ipv4");
        if (ipv6 !== undefined) validateString(ipv6, "ipv6");
        const f4 = isIP(ipv4);
        if (f4 === 0) {
          const e = new TypeError("Invalid IP address: " + ipv4);
          e.code = "ERR_INVALID_IP_ADDRESS";
          throw e;
        }
        if (ipv6 !== undefined) {
          // With both slots supplied they must be one IPv4 and one IPv6.
          if (f4 !== 4 || isIP(ipv6) !== 6) {
            const e = new TypeError("Invalid IP address: " + (f4 !== 4 ? ipv4 : ipv6));
            e.code = "ERR_INVALID_IP_ADDRESS";
            throw e;
          }
        }
        this._localAddress = { ipv4, ipv6 };
      }
    }
    const proto = Resolver.prototype;
    for (const name of Object.keys(RESOLVE_METHODS)) {
      const type = RESOLVE_METHODS[name][0], rr = RESOLVE_METHODS[name][1];
      proto[name] = adapt(function (hostname) { return promiseRecord(hostname, type, rr, this); }, "name");
    }
    const addresses = (self, hostname, family, rr, options) =>
      promiseRecord(hostname, family === 6 ? "AAAA" : "A", rr, self).then(
        (rows) => (options && options.ttl ? rows : rows.map((row) => (row && row.address !== undefined ? row.address : row))));
    proto.resolve4 = adapt(function (hostname, options) { return addresses(this, hostname, 4, "queryA", options); }, "name");
    proto.resolve6 = adapt(function (hostname, options) { return addresses(this, hostname, 6, "queryAaaa", options); }, "name");
    proto.resolve = adapt(function (hostname, rrtype) {
      if (rrtype !== undefined) validateString(rrtype, "rrtype");
      const t = (rrtype === undefined ? "A" : rrtype);
      if (t === "A") return addresses(this, hostname, 4, "queryA");
      if (t === "AAAA") return addresses(this, hostname, 6, "queryAaaa");
      if (RECORD_TYPES.indexOf(t) === -1)
        return Promise.reject(nodeError("EBADQUERY", "query" + t, String(hostname == null ? "" : hostname)));
      return promiseRecord(hostname, t, "query" + t[0] + t.slice(1).toLowerCase(), this);
    });
    proto.reverse = adapt(function (ip) {
      const address = String(ip);
      if (isIP(address) === 0) return Promise.reject(nodeError("EINVAL", "getHostByAddr", address));
      // No custom servers → the OS reverse path (getnameinfo), same as dns.reverse.
      if (this._serverText.length === 0) return promiseReverse(address);
      return promiseRecord(arpaName(address), "PTR", "getHostByAddr", this);
    });
    return Resolver;
  };
  const Resolver = makeResolverClass(false);
  const PromiseResolver = makeResolverClass(true);

  const dnsPromises = {
    lookup: promiseLookup,
    lookupService: promiseLookupService,
    resolve: promiseResolve,
    resolve4: (h, o) => promiseAddresses(h, 4, "queryA", o),
    resolve6: (h, o) => promiseAddresses(h, 6, "queryAaaa", o),
    reverse: promiseReverse,
    resolveAny: (h) => promiseRecord(h, "ANY", "queryAny"),
    resolveCname: (h) => promiseRecord(h, "CNAME", "queryCname"),
    resolveCaa: (h) => promiseRecord(h, "CAA", "queryCaa"),
    resolveMx: (h) => promiseRecord(h, "MX", "queryMx"),
    resolveNs: (h) => promiseRecord(h, "NS", "queryNs"),
    resolvePtr: (h) => promiseRecord(h, "PTR", "queryPtr"),
    resolveSoa: (h) => promiseRecord(h, "SOA", "querySoa"),
    resolveSrv: (h) => promiseRecord(h, "SRV", "querySrv"),
    resolveTxt: (h) => promiseRecord(h, "TXT", "queryTxt"),
    resolveNaptr: (h) => promiseRecord(h, "NAPTR", "queryNaptr"),
    getServers: getServers_,
    setServers: setServers_,
    setDefaultResultOrder: setResultOrder_,
    getDefaultResultOrder: getResultOrder_,
    Resolver: PromiseResolver,
  };

  // ── node:dns (callback style) — wraps the promise layer, links promisify ────
  // A callback wrapper whose last arg is (err, ...results). node's lookup passes
  // (err, address, family); resolve*/reverse pass (err, result); we spread.
  // `nameArg`, when given, is validated BEFORE the callback — node's
  // internal/dns/callback_resolver.js query() runs validateString(name) first, so
  // `dns.resolveNs([])` (one argument, no callback) must report the name error.
  // `nameArg` may also be a function, for an entry point whose leading argument
  // is validated conditionally (lib/dns.js lookup only checks a *truthy*
  // hostname, so dns.lookup(undefined, cb) is legal).
  const cbify = (promiseFn, spread, nameArg) => {
    const fn = function (...a) {
      if (typeof nameArg === "function") nameArg(a);
      else if (nameArg) validateString(a[0], nameArg);
      const cb = a[a.length - 1];
      validateFunction(cb, "callback");
      const args = a.slice(0, -1);
      const p = promiseFn.apply(null, args);
      p.then((v) => cb(null, ...(spread ? spread(v) : [v])), (e) => cb(e));
      return undefined;
    };
    fn[promisifyCustom] = promiseFn;
    return fn;
  };

  const dns = {
    // node lib/dns.js lookup() validates the hostname and the options BEFORE the
    // callback, so `dns.lookup(false, 'options', 'cb')` reports the options
    // error rather than a callback one.
    lookup: cbify(promiseLookup, (v) => (v && v.address !== undefined && !Array.isArray(v) ? [v.address, v.family] : [v]),
      (a) => { parseLookupOptions(a[0], typeof a[1] === "function" ? undefined : a[1]); }),
    lookupService: cbLookupService,
    resolve: cbify(promiseResolve, undefined, "name"),
    resolve4: cbify(dnsPromises.resolve4, undefined, "name"),
    resolve6: cbify(dnsPromises.resolve6, undefined, "name"),
    reverse: cbify(promiseReverse),
    resolveAny: cbify(dnsPromises.resolveAny, undefined, "name"),
    resolveCname: cbify(dnsPromises.resolveCname, undefined, "name"),
    resolveCaa: cbify(dnsPromises.resolveCaa, undefined, "name"),
    resolveMx: cbify(dnsPromises.resolveMx, undefined, "name"),
    resolveNs: cbify(dnsPromises.resolveNs, undefined, "name"),
    resolvePtr: cbify(dnsPromises.resolvePtr, undefined, "name"),
    resolveSoa: cbify(dnsPromises.resolveSoa, undefined, "name"),
    resolveSrv: cbify(dnsPromises.resolveSrv, undefined, "name"),
    resolveTxt: cbify(dnsPromises.resolveTxt, undefined, "name"),
    resolveNaptr: cbify(dnsPromises.resolveNaptr, undefined, "name"),
    getServers: getServers_,
    setServers: setServers_,
    setDefaultResultOrder: setResultOrder_,
    getDefaultResultOrder: getResultOrder_,
    lookupService_: undefined,
    Resolver,
    promises: dnsPromises,
    ADDRCONFIG: 1024, V4MAPPED: 2048, ALL: 256,
    // node:dns error-code constants (subset used by tests / real callers).
    NODATA: "ENODATA", FORMERR: "EFORMERR", SERVFAIL: "ESERVFAIL", NOTFOUND: "ENOTFOUND",
    NOTIMP: "ENOTIMP", REFUSED: "EREFUSED", BADQUERY: "EBADQUERY", BADNAME: "EBADNAME",
    BADFAMILY: "EBADFAMILY", BADRESP: "EBADRESP", CONNREFUSED: "ECONNREFUSED", TIMEOUT: "ETIMEOUT",
    EOF: "EOF", FILE: "EFILE", NOMEM: "ENOMEM", DESTRUCTION: "EDESTRUCTION", BADSTR: "EBADSTR",
    BADFLAGS: "EBADFLAGS", NONAME: "ENONAME", BADHINTS: "EBADHINTS", NOTINITIALIZED: "ENOTINITIALIZED",
    LOADIPHLPAPI: "ELOADIPHLPAPI", ADDRGETNETWORKPARAMS: "EADDRGETNETWORKPARAMS", CANCELLED: "ECANCELLED",
  };
  Object.assign(dnsPromises, {
    NODATA: "ENODATA", FORMERR: "EFORMERR", SERVFAIL: "ESERVFAIL", NOTFOUND: "ENOTFOUND",
    NOTIMP: "ENOTIMP", REFUSED: "EREFUSED", BADQUERY: "EBADQUERY", BADNAME: "EBADNAME",
    BADFAMILY: "EBADFAMILY", BADRESP: "EBADRESP", CONNREFUSED: "ECONNREFUSED", TIMEOUT: "ETIMEOUT",
    EOF: "EOF", FILE: "EFILE", NOMEM: "ENOMEM", DESTRUCTION: "EDESTRUCTION", BADSTR: "EBADSTR",
    BADFLAGS: "EBADFLAGS", NONAME: "ENONAME", BADHINTS: "EBADHINTS", NOTINITIALIZED: "ENOTINITIALIZED",
    LOADIPHLPAPI: "ELOADIPHLPAPI", ADDRGETNETWORKPARAMS: "EADDRGETNETWORKPARAMS", CANCELLED: "ECANCELLED",
  });
  // Link each dns.<m> to dns.promises.<m> via the promisify custom symbol.
  for (const m of Object.keys(dnsPromises)) {
    if (typeof dns[m] === "function" && dns[m][promisifyCustom] === undefined) dns[m][promisifyCustom] = dnsPromises[m];
  }

  def(["dns"], dns);
  def(["dns/promises"], dnsPromises);
})();
)JS";

}  // namespace mbun::jsc::js_dns
