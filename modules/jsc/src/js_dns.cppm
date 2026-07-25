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
  const rawLookup = (host, family, flags, callback) => {
    if (DN && DN.lookup) DN.lookup(String(host), family | 0, flags | 0, inflight(callback));
    else soon(() => callback({ error: "ENOTFOUND" }));
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
  const rawResolve = (host, type, callback, servers, timeout, tries) => {
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

  // ── /etc/resolv.conf nameservers for dns.getServers() ──────────────────────
  const getServers_ = () => {
    if (defaultServers.length) return defaultServers.slice();
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
  // Bun coalesces/reuses recent resolver answers. A short raw-row cache keeps
  // callback, promise and util.promisify views consistent when an upstream DNS
  // server rotates equally-ranked addresses between consecutive requests.
  const lookupCache = new Map();
  const LOOKUP_CACHE_MS = 30_000;
  const promiseLookup = (hostname, options) => {
    // node lib/dns.js lookup(): a truthy hostname must be a string, and the
    // check is synchronous (dns.lookup(1, {}) throws, it does not reject).
    if (hostname) validateString(hostname, "hostname");
    return promiseLookup_(hostname, options);
  };
  const promiseLookup_ = (hostname, options) => new Promise((resolve, reject) => {
    let family = 0, all = false, flags = 0;
    if (typeof options === "number") family = normFamily(options);
    else if (options && typeof options === "object") {
      family = normFamily(options.family); all = !!options.all;
      if (typeof options.hints === "number") flags = options.hints;
    }
    if (!hostname) { resolve(all ? [] : { address: null, family: 4 }); return; }
    const host = String(hostname);
    if (host.indexOf("\0") !== -1) { reject(nodeError("ENOTFOUND", "getaddrinfo", host)); return; }
    const key = host + "\0" + family + "\0" + flags;
    const cacheable = host !== "localhost" && isIP(host) === 0;
    const finish = (rows) => {
      if (all) resolve(rows.map((x) => ({ address: x.address, family: x.family })));
      else resolve({ address: rows[0].address, family: rows[0].family });
    };
    const cached = cacheable ? lookupCache.get(key) : undefined;
    if (cached && Date.now() - cached.at < LOOKUP_CACHE_MS) { finish(cached.rows); return; }
    rawLookup(host, family, flags, (r) => {
      if (!Array.isArray(r)) { reject(nodeError(r.error || "ENOTFOUND", "getaddrinfo", host)); return; }
      if (cacheable) lookupCache.set(key, { at: Date.now(), rows: r });
      finish(r);
    });
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
    }, servers, res && res._timeout, res && res._tries);
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
  const promiseLookupService = (address, port) => {
    validateService(address);
    return new Promise((resolve, reject) => {
      rawLookupService(address, port, (r) => {
        if (r && r.error) reject(nodeError(r.error, "getnameinfo", address));
        else resolve(r);
      });
    });
  };

  const promiseResolve = (hostname, rrtype) => {
    const t = (rrtype == null ? "A" : String(rrtype));
    if (t === "A") return promiseAddresses(hostname, 4, "queryA");
    if (t === "AAAA") return promiseAddresses(hostname, 6, "queryAaaa");
    if (RECORD_TYPES.indexOf(t) === -1)
      return Promise.reject(nodeError("EBADQUERY", "query" + t, String(hostname == null ? "" : hostname)));
    return promiseRecord(hostname, t, "query" + t[0] + t.slice(1).toLowerCase());
  };

  // Shared default result order (node:dns, dns.promises and dns/promises all
  // observe the same setting — see issue #28948).
  let defaultResultOrder_ = "verbatim";
  const setResultOrder_ = (order) => {
    if (order !== "ipv4first" && order !== "ipv6first" && order !== "verbatim")
      throw new TypeError("The argument 'order' must be one of: 'verbatim', 'ipv4first', 'ipv6first'. Received " + JSON.stringify(order));
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
    let host = entry;
    if (host.charCodeAt(0) === 91 /* [ */) {
      const close = host.indexOf("]");
      if (close === -1) throw invalidIPError(entry);
      const rest = host.slice(close + 1);
      host = host.slice(1, close);
      if (rest !== "" && !(rest.charCodeAt(0) === 58 /* : */ && Number.isInteger(Number(rest.slice(1)))))
        throw invalidIPError(entry);
    } else {
      const colon = host.lastIndexOf(":");
      // One colon → IPv4:port; several → a bare IPv6 literal.
      if (colon !== -1 && host.indexOf(":") === colon) host = host.slice(0, colon);
    }
    if (isIP(host) === 0) throw invalidIPError(entry);
    return entry;
  };
  // dns.setServers / dns.promises.setServers / require('dns/promises').setServers
  // are all the default resolver's setServers, so they validate identically.
  // Every entry is parsed before anything is stored: node caches the old list and
  // restores it if c-ares rejects the new one, so a failed call must not leave a
  // half-applied server list behind.
  const setServers_ = (list) => {
    validateArray(list, "servers");
    const parsed = list.map(parseServerEntry);
    defaultServers.length = 0;
    for (const entry of parsed) defaultServers.push(entry);
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
        ? function (...a) { validateName(a); return fn.apply(this, a); }
        : function (...a) {
            validateName(a);
            const cb = a[a.length - 1];
            validateFunction(cb, "callback");
            fn.apply(this, a.slice(0, -1)).then((v) => cb(null, v), (e) => cb(e));
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
      }
      getServers() { return this._serverText.length ? this._serverText.slice() : getServers_(); }
      setServers(list) {
        validateArray(list, "servers");
        this._serverText = list.map(parseServerEntry);
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
      const t = (rrtype == null ? "A" : String(rrtype));
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
  const cbify = (promiseFn, spread, nameArg) => {
    const fn = function (...a) {
      if (nameArg) validateString(a[0], nameArg);
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
    lookup: cbify(promiseLookup, (v) => (v && v.address !== undefined && !Array.isArray(v) ? [v.address, v.family] : [v])),
    lookupService: cbify(promiseLookupService, (v) => [v.hostname, v.service]),
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
  // Link each dns.<m> to dns.promises.<m> via the promisify custom symbol.
  for (const m of Object.keys(dnsPromises)) {
    if (typeof dns[m] === "function" && dns[m][promisifyCustom] === undefined) dns[m][promisifyCustom] = dnsPromises[m];
  }

  def(["dns"], dns);
  def(["dns/promises"], dnsPromises);
})();
)JS";

}  // namespace mbun::jsc::js_dns
