// node:net SocketAddress + node:dgram JS layer partition.
//
// Augments the node:net module (real Socket/Server/isIP/BlockList live in
// js_net.cppm) with the SocketAddress value class, and installs node:dgram over
// the __mbunDgramNative UDP primitives (runtime/node_net.inc), driven by the same
// event-loop reactor (globalThis.__mbunNet) that js_net.cppm creates.
//
// NOTE: appended AFTER the master builtins IIFE (opened in bootstrap, closed by
// image_closure) AND before kNetJS runs, so this is a self-contained IIFE that
// binds G = globalThis and touches the reactor lazily (at bind/send time, once
// kNetJS has created globalThis.__mbunNet). SocketAddress set on M["net"] before
// kNetJS survives because kNetJS rebuilds net via Object.assign spreading M["net"].
//
// Blueprints: bun src/runtime/socket/SocketAddress.rs, src/js/node/dgram.ts.
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

  // ==================================================================== node:dgram
  const ND = G.__mbunDgramNative;
  const EE = (M["events"] && M["events"].EventEmitter) || (M["node:events"] && M["node:events"].EventEmitter);
  const B = G.Buffer;
  const te = new G.TextEncoder();
  const toBytes = (d) => (typeof d === "string" ? te.encode(d)
    : d instanceof Uint8Array ? d
    : ArrayBuffer.isView(d) ? new Uint8Array(d.buffer, d.byteOffset, d.byteLength)
    : d instanceof ArrayBuffer ? new Uint8Array(d) : te.encode(String(d)));
  const toB64 = (b) => { let s = ""; for (let i = 0; i < b.length; i += 4096) s += String.fromCharCode.apply(null, b.subarray(i, i + 4096)); return G.btoa(s); };
  const fromB64 = (s) => { const t = G.atob(s); const o = new Uint8Array(t.length); for (let i = 0; i < t.length; i++) o[i] = t.charCodeAt(i); return o; };

  if (ND && EE) {
    class DgramSocket extends EE {
      constructor(type) {
        super();
        const isObj = typeof type === "object" && type !== null;
        this.type = isObj ? (type.type || "udp4") : (type || "udp4");
        this._opts = isObj ? type : {};
        this._fd = -1;
        this._bound = false;
        this._closed = false;
        this._refed = true;
        this._counted = false;
      }
      _v6() { return this.type === "udp6"; }
      // reuseAddr is an opt-in (node dgram.createSocket({ reuseAddr })): without
      // it a duplicate bind must fail with EADDRINUSE (issue 24157).
      _ensureFd() { if (this._fd < 0) this._fd = ND.create(this.type, !!this._opts.reuseAddr); }
      _reactor() { return G.__mbunNet; }

      bind(a1, a2, a3) {
        let opts, port, addr, cb;
        if (typeof a1 === "function") { cb = a1; }
        else if (typeof a1 === "object" && a1 !== null) { opts = a1; if (typeof a2 === "function") cb = a2; }
        else { port = a1; if (typeof a2 === "function") { cb = a2; } else { addr = a2; if (typeof a3 === "function") cb = a3; } }
        const p = (opts ? opts.port : port) | 0;
        let ad = opts ? opts.address : addr;
        if (ad === undefined || ad === null) ad = "";
        const ipv6Only = !!((opts && opts.ipv6Only) || this._opts.ipv6Only);
        this._ensureFd();
        let info;
        try { info = ND.bind(this._fd, String(ad), p, this._v6(), ipv6Only); }
        catch (e) { const self = this; G.queueMicrotask(() => self.emit("error", e)); return this; }
        this._bound = true;
        this._addr = info;
        const R = this._reactor();
        if (R) { R.items.add(this); if (this._refed && !this._counted) { this._counted = true; R.pending++; } }
        if (cb) this.once("listening", cb);
        const self = this;
        G.queueMicrotask(() => { if (!self._closed) self.emit("listening"); });
        return this;
      }

      _poll() {
        if (this._closed || this._fd < 0) return 0;
        let n = 0;
        while (!this._closed) {
          let d;
          try { d = ND.recv(this._fd); }
          catch (e) { const self = this; G.queueMicrotask(() => self.emit("error", e)); break; }
          if (!d) break;
          n++;
          const buf = B ? B.from(d.data, "base64") : fromB64(d.data);
          this.emit("message", buf, { address: d.address, family: d.family, port: d.port, size: d.size });
        }
        return n;
      }

      send(msg, a2, a3, a4, a5, a6) {
        const bytes = toBytes(msg);
        let buf, p, addr, cb;
        if (typeof a3 === "number") { buf = bytes.subarray(a2, a2 + a3); p = a4; addr = a5; cb = a6; }
        else { buf = bytes; p = a2; addr = a3; cb = a4; }
        if (typeof addr === "function") { cb = addr; addr = undefined; }
        if (typeof p === "function") { cb = p; p = undefined; }
        // send() implicitly binds an unbound socket (address(), the `listening`
        // event, and reactor registration for replies). ref: bun dgram.ts:583.
        if (!this._bound) this.bind(0);
        this._ensureFd();
        const host = addr ? String(addr) : (this._v6() ? "::1" : "127.0.0.1");
        let sent = 0, err = null;
        try { sent = ND.send(this._fd, toB64(buf), p | 0, host, this._v6()); }
        catch (e) { err = e; }
        const self = this;
        if (cb) G.queueMicrotask(() => cb(err, err ? 0 : sent));
        else if (err) G.queueMicrotask(() => self.emit("error", err));
        return undefined;
      }

      address() {
        if (this._fd < 0 || !this._bound) throw mkE("Not running", "ERR_SOCKET_DGRAM_NOT_RUNNING");
        return ND.address(this._fd);
      }
      close(cb) {
        if (this._closed) { if (cb) G.queueMicrotask(cb); return this; }
        this._closed = true;
        const R = this._reactor();
        if (R) { R.items.delete(this); if (this._counted) { this._counted = false; R.pending = Math.max(0, R.pending - 1); } }
        if (this._fd >= 0) { try { ND.close(this._fd); } catch (e) {} this._fd = -1; }
        if (cb) this.once("close", cb);
        const self = this;
        G.queueMicrotask(() => self.emit("close"));
        return this;
      }
      setBroadcast(f) { this._ensureFd(); ND.setopt(this._fd, "broadcast", f ? 1 : 0, this._v6()); return this; }
      setTTL(n) { this._ensureFd(); ND.setopt(this._fd, "ttl", n | 0, this._v6()); return n | 0; }
      setMulticastTTL(n) { this._ensureFd(); ND.setopt(this._fd, "multicastTTL", n | 0, this._v6()); return n | 0; }
      setMulticastLoopback(f) { this._ensureFd(); ND.setopt(this._fd, "multicastLoopback", f ? 1 : 0, this._v6()); return !!f; }
      addMembership(group, iface) { this._ensureFd(); ND.membership(this._fd, true, String(group), iface ? String(iface) : "", this._v6()); }
      dropMembership(group, iface) { this._ensureFd(); ND.membership(this._fd, false, String(group), iface ? String(iface) : "", this._v6()); }
      ref() { if (!this._refed) { this._refed = true; const R = this._reactor(); if (R && this._bound && !this._counted) { this._counted = true; R.pending++; } } return this; }
      unref() { if (this._refed) { this._refed = false; const R = this._reactor(); if (R && this._counted) { this._counted = false; R.pending = Math.max(0, R.pending - 1); } } return this; }
    }

    const createSocket = (type, cb) => {
      const s = new DgramSocket(type);
      if (typeof cb === "function") s.on("message", cb);
      return s;
    };

    const dgram = { createSocket, Socket: DgramSocket };
    M["dgram"] = M["node:dgram"] = dgram;
  } else {
    // Native UDP unavailable (Windows): honest DEFERRED module.
    const deferred = () => { throw mkE("node:dgram is not supported on this platform", "ERR_DGRAM_UNSUPPORTED"); };
    M["dgram"] = M["node:dgram"] = { createSocket: deferred, Socket: deferred };
  }
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
