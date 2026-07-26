// node:net SocketAddress JS layer partition.
//
// Augments the node:net module (real Socket/Server/isIP/BlockList live in
// js_net.cppm) with the SocketAddress value class.
//
// NOTE: appended AFTER the master builtins IIFE (opened in bootstrap, closed by
// image_closure) AND before kNetJS runs, so this is a self-contained IIFE that
// binds G = globalThis and touches the reactor lazily (at bind/send time, once
// kNetJS has created globalThis.__mbunNet). SocketAddress set on M["net"] before
// kNetJS survives because kNetJS rebuilds net via Object.assign spreading M["net"].
//
// Blueprint: bun src/runtime/socket/SocketAddress.rs.
export module mbun.jsc.js_builtins:node_net;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeNetJS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  if (!M) return;

  // ---------------------------------------------------------------- helpers
  const mkE = (msg, code, Ctor) => { const e = new (Ctor || TypeError)(msg); e.code = code; return e; };
  const errArgType = (name, exp, val) =>
    mkE('The "' + name + '" argument must be of type ' + exp + '. Received ' + typeof val, "ERR_INVALID_ARG_TYPE");
  const errArgValue = (name, val) =>
    mkE("The argument '" + name + "' is invalid. Received " + String(val), "ERR_INVALID_ARG_VALUE");
  const errBadPort = (val) =>
    mkE('The "options.port" argument must be a valid IP port number. Received ' + String(val), "ERR_SOCKET_BAD_PORT", RangeError);
  const errBadIP = (val) =>
    mkE("Invalid socket address: " + String(val), "ERR_INVALID_IP_ADDRESS");

  // ================================================================ SocketAddress
  // Presentation validators. IPv6 canonicalization borrows the WHATWG URL parser
  // (mbun's URL compresses "1:0::" → "1::"); IPv4 numeric forms are parsed here
  // because mbun's URL does not normalize hex/octal/overflowing dotted-quads.
  const isV4Literal = (s) => {
    const p = s.split(".");
    if (p.length !== 4) return false;
    for (const o of p) { if (!/^\d{1,3}$/.test(o) || +o > 255) return false; }
    return true;
  };
  const isV6Literal = (s) => {
    try { return typeof G.URL === "function" && new G.URL("http://[" + s + "]").hostname[0] === "["; }
    catch (e) { return false; }
  };

  // WHATWG-style IPv4 parser: returns a canonical dotted-quad or null.
  const parseV4Number = (s) => {
    if (s === "") return 0;
    let R = 10;
    if (s.length >= 2 && s[0] === "0" && (s[1] === "x" || s[1] === "X")) { s = s.slice(2); R = 16; }
    else if (s.length >= 2 && s[0] === "0") { s = s.slice(1); R = 8; }
    if (s === "") return 0;
    const re = R === 10 ? /^[0-9]+$/ : R === 16 ? /^[0-9a-fA-F]+$/ : /^[0-7]+$/;
    if (!re.test(s)) return -1;
    const n = parseInt(s, R);
    return Number.isFinite(n) ? n : -1;
  };
  const parseV4 = (input) => {
    const parts = input.split(".");
    if (parts.length && parts[parts.length - 1] === "") parts.pop();  // trailing dot
    if (parts.length === 0 || parts.length > 4) return null;
    const nums = [];
    for (const p of parts) { const n = parseV4Number(p); if (n < 0 || n > 0xffffffff) return null; nums.push(n); }
    for (let i = 0; i < nums.length - 1; i++) if (nums[i] > 255) return null;
    const last = nums[nums.length - 1];
    if (last >= Math.pow(256, 5 - nums.length)) return null;
    let ipv4 = nums.pop();
    let c = 0;
    for (const n of nums) { ipv4 += n * Math.pow(256, 3 - c); c++; }
    return [(ipv4 >>> 24) & 255, (ipv4 >>> 16) & 255, (ipv4 >>> 8) & 255, ipv4 & 255].join(".");
  };
  const parsePortStr = (p) => {
    if (p === "") return 0;
    if (!/^[0-9]+$/.test(p)) return -1;
    const n = Number(p);
    return n > 65535 ? -1 : n;
  };

  const afOf = (fam) => {
    if (fam === undefined) return "ipv4";
    if (typeof fam === "string" && fam.length === 4) {
      const l = fam.toLowerCase();
      if (l === "ipv4") return "ipv4";
      if (l === "ipv6") return "ipv6";
    }
    throw errArgValue("options.family", fam);
  };
  const validatePort = (p) => {
    if (p === undefined) return 0;
    if (typeof p !== "number" || !Number.isFinite(p)) throw errBadPort(p);
    const n = p | 0;
    if (n !== p || n < 0 || n > 65535) throw errBadPort(p);
    return n;
  };

  const buildState = (options) => {
    if (options === undefined || options === null) return { family: "ipv4", address: "127.0.0.1", port: 0, flowlabel: 0 };
    if (typeof options !== "object") throw errArgType("options", "object", options);
    const family = afOf(options.family);
    const port = validatePort(options.port);
    let flowlabel = 0;
    if (family === "ipv6" && options.flowlabel !== undefined) {
      if (typeof options.flowlabel !== "number") throw errArgType("options.flowlabel", "number", options.flowlabel);
      flowlabel = options.flowlabel >>> 0;
    }
    let address;
    if (options.address !== undefined) {
      if (typeof options.address !== "string") throw errArgType("options.address", "string", options.address);
      address = options.address;
      if (family === "ipv4" ? !isV4Literal(address) : !isV6Literal(address)) throw errBadIP(address);
    } else {
      address = family === "ipv4" ? "127.0.0.1" : "::";
    }
    return { family, address, port, flowlabel };
  };

  const S = new WeakMap();          // instance → { address, family, port, flowlabel }
  const brand = new WeakSet();      // only true `new SocketAddress` instances
  const SENTINEL = { sa: true };
  let pendingState;                 // handoff for the internal factory below

  class SocketAddress {
    constructor(options) {
      S.set(this, options === SENTINEL ? pendingState : buildState(options));
      brand.add(this);
    }
    get address() { return S.get(this).address; }
    get port() { return S.get(this).port; }
    get family() { return S.get(this).family; }
    get flowlabel() { const st = S.get(this); return st.family === "ipv6" ? st.flowlabel : 0; }
    toJSON() {
      const st = S.get(this);
      return { address: this.address, family: st.family, port: st.port, flowlabel: this.flowlabel };
    }
    static isSocketAddress(value) {
      return typeof value === "object" && value !== null &&
        Object.getPrototypeOf(value) === SocketAddress.prototype && brand.has(value);
    }
    static parse(input) {
      if (typeof input !== "string") return undefined;
      let host, portStr, v6;
      if (input[0] === "[") {
        const end = input.indexOf("]");
        if (end < 0) return undefined;
        host = input.slice(1, end);
        const rest = input.slice(end + 1);
        if (rest === "") portStr = "";
        else { if (rest[0] !== ":") return undefined; portStr = rest.slice(1); }
        v6 = true;
      } else {
        const c = input.lastIndexOf(":");
        host = c >= 0 ? input.slice(0, c) : input;
        portStr = c >= 0 ? input.slice(c + 1) : "";
        v6 = false;
      }
      const port = parsePortStr(portStr);
      if (port < 0) return undefined;
      let state;
      if (v6) {
        let canon;
        try { canon = new G.URL("http://[" + host + "]").hostname; } catch (e) { return undefined; }
        if (!canon || canon[0] !== "[") return undefined;
        state = { family: "ipv6", address: canon.slice(1, -1), port, flowlabel: 0 };
      } else {
        const addr = parseV4(host);
        if (addr === null) return undefined;
        state = { family: "ipv4", address: addr, port, flowlabel: 0 };
      }
      pendingState = state;
      const sa = new SocketAddress(SENTINEL);
      pendingState = undefined;
      return sa;
    }
  }

  for (const k of ["net", "node:net"]) { const m = M[k] || (M[k] = {}); m.SocketAddress = SocketAddress; }

  // node:dgram now lives in modules/jsc/src/js_dgram.cppm (module
  // mbun.jsc.js_dgram, evaluated after kNetJS + kDnsJS): it is a translation of
  // node's lib/dgram.js over a real `UDP` handle, which needs the reactor and
  // dns.lookup in scope at install time. The stand-in that used to sit here
  // owned the fd on the Socket itself and could not express node's
  // handle/Socket split at all.
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
