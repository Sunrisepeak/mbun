// modules/jsc/src/js_dgram.cppm — module mbun.jsc.js_dgram
//
// node:dgram, translated from node's own lib/dgram.js + lib/internal/dgram.js
// rather than shaped by hand.
//
// The previous implementation was a ~120-line stand-in whose Socket owned the fd
// directly. That shape cannot express node's contract, because node splits the
// module in two: a `UDP` *handle* (src/udp_wrap.cc — every method reports failure
// by returning a negative libuv errno, never by throwing) and a JS `Socket` that
// turns those return values into ErrnoException / ExceptionWithHostPort and owns
// all the argument validation. The corpus tests that split directly: they reach
// for `socket[kStateSymbol].handle`, replace `handle.lookup` / `handle.send`, and
// construct `new UDP()` out of `internalBinding("udp_wrap")`.
//
// So this partition reproduces both halves:
//   * `UDP` / `SendWrap` over the __mbunDgramNative fd primitives
//     (runtime/node_net.inc), published as internalBinding("udp_wrap");
//   * `internal/dgram`'s kStateSymbol / newHandle / _createSocketHandle;
//   * `dgram.Socket` / `dgram.createSocket`, a translation of lib/dgram.js.
//
// Evaluated after kNetJS (it borrows the reactor `__mbunNet` for the recv poll
// and net.BlockList for receiveBlockList/sendBlockList) and after kDnsJS (the
// handle's default lookup is dns.lookup).
export module mbun.jsc.js_dgram;

import std;

namespace mbun::jsc::js_dgram {

export constexpr std::string_view kDgramJS = R"JS(
(function () {
  "use strict";
  const G = globalThis;
  const M = G.__mbunNativeModules || {};
  const ND = G.__mbunDgramNative;
  const NET = G.__mbunNet;
  const EE = (M["events"] && M["events"].EventEmitter) || (M["node:events"] && M["node:events"].EventEmitter);
  const B = G.Buffer;
  const net = M["net"] || M["node:net"] || {};
  const util = M["util"] || M["node:util"] || {};
  const nextTick = (G.process && G.process.nextTick) ? G.process.nextTick.bind(G.process)
    : (fn, ...a) => G.queueMicrotask(() => fn(...a));
  // node lib/dgram.js resolves the 'udp.socket' diagnostics channel once at
  // module load; hasSubscribers gates the publish on the hot path.
  const dc = M["diagnostics_channel"] || M["node:diagnostics_channel"];
  const udpSocketChannel = (dc && typeof dc.channel === "function")
    ? dc.channel("udp.socket") : { hasSubscribers: false, publish() {} };

  // ---- errors ---------------------------------------------------------------
  // internal/errors.js message templates, reproduced so assert.throws shapes
  // ({ code, name, message }) match upstream exactly.
  const inspect = typeof util.inspect === "function" ? util.inspect : (v) => String(v);
  const iath = (input) => {
    if (input === undefined || input === null) return " Received " + String(input);
    if (typeof input === "function") return " Received function " + input.name;
    if (typeof input === "object") {
      if (input.constructor && input.constructor.name) return " Received an instance of " + input.constructor.name;
      return " Received " + inspect(input, { depth: -1 });
    }
    let ins;
    try { ins = inspect(input, { colors: false }); }
    catch (e) { ins = typeof input === "string" ? "'" + input + "'" : String(input); }
    if (ins.length > 28) ins = ins.slice(0, 25) + "...";
    return " Received type " + (typeof input) + " (" + ins + ")";
  };
  const PRIMS = ["string", "number", "bigint", "boolean", "symbol", "function", "object"];
  const orList = (a) => a.length <= 1 ? (a[0] || "")
    : a.length === 2 ? a[0] + " or " + a[1]
    : a.slice(0, -1).join(", ") + " or " + a[a.length - 1];
  const mk = (Ctor, code, msg) => { const e = new Ctor(msg); e.code = code; return e; };
  const ERR_INVALID_ARG_TYPE = (name, expected, actual) => {
    // Shared node-exact factory (bootstrap __mbunNodeErrors): node's formatList
    // puts an Oxford comma before the last class ("Buffer, TypedArray, or
    // DataView") and always emits primitive types before class instances,
    // independent of the order they were passed in.
    const NE = globalThis.__mbunNodeErrors;
    if (NE) return NE.ERR_INVALID_ARG_TYPE(name, expected, actual);
    if (!Array.isArray(expected)) expected = [expected];
    const types = [], instances = [], other = [];
    for (const v of expected) {
      if (PRIMS.indexOf(v) !== -1) types.push(v);
      else if (/^[A-Z]/.test(v)) instances.push(v);
      else other.push(v);
    }
    const parts = [];
    if (types.length) parts.push("of type " + orList(types));
    if (instances.length) parts.push("an instance of " + orList(instances));
    if (other.length) parts.push("one of " + orList(other));
    return mk(TypeError, "ERR_INVALID_ARG_TYPE",
      'The "' + name + '" argument must be ' + parts.join(" or ") + "." + iath(actual));
  };
  const ERR_OUT_OF_RANGE = (name, range, value) => mk(RangeError, "ERR_OUT_OF_RANGE",
    'The value of "' + name + '" is out of range. It must be ' + range + ". Received " + inspect(value));
  const ERR_SOCKET_BAD_TYPE = () => mk(TypeError, "ERR_SOCKET_BAD_TYPE",
    "Bad socket type specified. Valid types are: udp4, udp6");
  const ERR_SOCKET_ALREADY_BOUND = () => mk(Error, "ERR_SOCKET_ALREADY_BOUND", "Socket is already bound");
  const ERR_SOCKET_DGRAM_NOT_RUNNING = () => mk(Error, "ERR_SOCKET_DGRAM_NOT_RUNNING", "Not running");
  const ERR_SOCKET_DGRAM_IS_CONNECTED = () => mk(Error, "ERR_SOCKET_DGRAM_IS_CONNECTED", "Already connected");
  const ERR_SOCKET_DGRAM_NOT_CONNECTED = () => mk(Error, "ERR_SOCKET_DGRAM_NOT_CONNECTED", "Not connected");
  const ERR_SOCKET_BAD_BUFFER_SIZE = () => mk(TypeError, "ERR_SOCKET_BAD_BUFFER_SIZE",
    "Buffer size must be a positive integer");
  const ERR_SOCKET_BUFFER_SIZE = (ctx) => {
    const e = mk(Error, "ERR_SOCKET_BUFFER_SIZE", "Could not get or set buffer size: " + String(ctx && ctx.message || ctx));
    return e;
  };
  const ERR_BUFFER_OUT_OF_BOUNDS = (name) => mk(RangeError, "ERR_BUFFER_OUT_OF_BOUNDS",
    name ? '"' + name + '" is outside of buffer bounds' : "Attempt to access memory outside buffer bounds");
  const ERR_MISSING_ARGS = (name) => mk(TypeError, "ERR_MISSING_ARGS",
    'The "' + name + '" argument must be specified');
  const ERR_INVALID_FD_TYPE = (type) => mk(TypeError, "ERR_INVALID_FD_TYPE",
    "Unsupported fd type: " + type);
  const ERR_IP_BLOCKED = (ip) => mk(Error, "ERR_IP_BLOCKED", "IP(" + ip + ") is blocked by net.BlockList");

  // ---- validators (internal/validators.js) ----------------------------------
  const isInt32 = (v) => v === (v | 0);
  const validateString = (v, n) => { if (typeof v !== "string") throw ERR_INVALID_ARG_TYPE(n, "string", v); };
  const validateNumber = (v, n) => { if (typeof v !== "number") throw ERR_INVALID_ARG_TYPE(n, "number", v); };
  const validateFunction = (v, n) => { if (typeof v !== "function") throw ERR_INVALID_ARG_TYPE(n, "function", v); };
  const validateUint32 = (v, n, positive) => {
    if (typeof v !== "number") throw ERR_INVALID_ARG_TYPE(n, "number", v);
    if (!Number.isInteger(v)) throw ERR_OUT_OF_RANGE(n, "an integer", v);
    const min = positive ? 1 : 0;
    if (v < min || v > 4294967295) throw ERR_OUT_OF_RANGE(n, ">= " + min + " && < 4294967296", v);
  };
  const validateAbortSignal = (v, n) => {
    if (v !== undefined && (v === null || typeof v !== "object" || !("aborted" in v)))
      throw ERR_INVALID_ARG_TYPE(n, "AbortSignal", v);
  };
  // internal/validators.js validatePort: node accepts anything that coerces to a
  // 0..65535 integer, and rejects everything else with ERR_SOCKET_BAD_PORT.
  const validatePort = (port, name, allowZero) => {
    name = name || "Port";
    if ((typeof port !== "number" && typeof port !== "string") ||
        (typeof port === "string" && String(port).trim().length === 0) ||
        +port !== (+port >>> 0) || port > 0xFFFF || (port === 0 && allowZero === false)) {
      const e = new RangeError(name + " should be " + (allowZero === false ? "> 0 and " : ">= 0 and ") +
        "< 65536. Received " + inspect(port) + ".");
      e.code = "ERR_SOCKET_BAD_PORT";
      throw e;
    }
    return port | 0;
  };

  // ---- libuv errno bridge ---------------------------------------------------
  // The natives report failure as the POSIX errno NAME; node's handle methods
  // return the negative UV_E* number. util.getSystemErrorMap() is the same table
  // internalBinding("uv") publishes, so the two can never disagree.
  let uvByName = null;
  const uvOf = (name) => {
    if (uvByName === null) {
      uvByName = new Map();
      try {
        const m = util.getSystemErrorMap ? util.getSystemErrorMap() : new Map();
        for (const [code, entry] of m) uvByName.set(entry[0], code);
      } catch (e) {}
    }
    const v = uvByName.get(name);
    return v === undefined ? -4094 : v;  // UV_UNKNOWN
  };
  const sysName = (errno) => {
    if (typeof util.getSystemErrorName === "function") {
      try { return util.getSystemErrorName(errno); } catch (e) {}
    }
    return "UNKNOWN";
  };
  // Every native returns a string on failure and a number/object/null otherwise.
  const rc = (r) => (typeof r === "string" ? uvOf(r) : 0);
  const ErrnoException = (err, syscall, original) => {
    const code = sysName(err);
    const e = new Error(original ? syscall + " " + code + " " + original : syscall + " " + code);
    e.errno = err; e.code = code; e.syscall = syscall;
    return e;
  };
  const ExceptionWithHostPort = (err, syscall, address, port) => {
    const code = sysName(err);
    let details = "";
    if (port && port > 0) details = " " + address + ":" + port;
    else if (address) details = " " + address;
    const e = new Error(syscall + " " + code + details);
    e.errno = err; e.code = code; e.syscall = syscall; e.address = address;
    if (port) e.port = port;
    return e;
  };

  // ---- base64 bridge --------------------------------------------------------
  const toB64 = (b) => {
    if (B) return B.from(b.buffer ? b.buffer : b, b.byteOffset || 0, b.byteLength !== undefined ? b.byteLength : b.length).toString("base64");
    let s = "";
    for (let i = 0; i < b.length; i += 4096) s += String.fromCharCode.apply(null, b.subarray(i, i + 4096));
    return G.btoa(s);
  };
  const fromB64 = (s) => {
    if (B) return B.from(s, "base64");
    const t = G.atob(s); const o = new Uint8Array(t.length);
    for (let i = 0; i < t.length; i++) o[i] = t.charCodeAt(i);
    return o;
  };

  if (!ND || !EE) {
    // Native UDP unavailable (Windows): honest DEFERRED module.
    const deferred = () => { throw mk(Error, "ERR_DGRAM_UNSUPPORTED", "node:dgram is not supported on this platform"); };
    M["dgram"] = M["node:dgram"] = { createSocket: deferred, Socket: deferred };
    return;
  }

  // ======================================================================= UDP
  // node src/udp_wrap.cc. Failure is a negative libuv errno RETURN VALUE, never
  // a throw — lib/dgram.js relies on that everywhere.
  let nextAsyncId = 1000;
  // Descriptors currently in the reactor's watch set (libuv's loop->watchers),
  // which is what uv_udp_open checks before adopting one. See UDP#open.
  const udpWatchedFds = new Set();
  class UDP {
    constructor() {
      this.fd = -1;
      this._family6 = false;
      this._reuseAddr = false;
      this._reusePort = false;
      this._ipv6Only = false;
      this._closed = false;
      this._receiving = false;
      this._asyncId = ++nextAsyncId;
      this.onmessage = null;
      this.onerror = null;
      // reactor loop-reference state (js_net.cppm NET.hold / NET.release):
      // `_refd` is the sticky user intent, `_held` whether the handle count
      // currently carries this handle, `_loopOpen` whether it is open at all.
      this._refd = true;
      this._held = false;
      this._loopOpen = false;
      this._sendQueueSize = 0;
      this._sendQueueCount = 0;
    }
    getAsyncId() { return this._asyncId; }
    // UV_UDP_REUSEADDR = 4, UV_UDP_IPV6ONLY = 1, UV_UDP_REUSEPORT = 8 (uv.h).
    _ensureFd_(v6) {
      if (this.fd >= 0) return 0;
      if (this._closed) return uvOf("EBADF");
      this._family6 = !!v6;
      const r = ND.create(v6 ? "udp6" : "udp4", this._reuseAddr, this._reusePort);
      if (typeof r === "string") return uvOf(r);
      this.fd = r | 0;
      return 0;
    }
    _bind_(address, port, flags, v6) {
      this._reuseAddr = !!(flags & 4);
      this._reusePort = !!(flags & 8);
      this._ipv6Only = !!(flags & 1);
      const e = this._ensureFd_(v6);
      if (e !== 0) return e;
      const r = ND.bind(this.fd, address == null ? "" : String(address), port | 0, !!v6, this._ipv6Only);
      return typeof r === "string" ? uvOf(r) : 0;
    }
    bind(address, port, flags) { return this._bind_(address, port, flags | 0, false); }
    bind6(address, port, flags) { return this._bind_(address, port, flags | 0, true); }
    _connect_(address, port, v6) {
      const e = this._ensureFd_(v6);
      if (e !== 0) return e;
      const r = ND.connect(this.fd, address == null ? "" : String(address), port | 0, !!v6);
      return typeof r === "string" ? uvOf(r) : 0;
    }
    connect(address, port) { return this._connect_(address, port, false); }
    connect6(address, port) { return this._connect_(address, port, true); }
    disconnect() {
      if (this.fd < 0) return uvOf("EBADF");
      return rc(ND.disconnect(this.fd));
    }
    open(fd) {
      if (this.fd >= 0) return uvOf("EEXIST");
      // libuv uv_udp_open: UV_EBUSY when THIS handle already has a descriptor,
      // UV_EEXIST when the descriptor is already in the loop's watcher list —
      // i.e. some other live handle is actively receiving on it (libuv#1851).
      // A handle that is merely bound is NOT watched, which is why
      // _createSocketHandle may re-open a bound-but-idle UDP fd
      // (test-dgram-create-socket-handle-fd) while binding onto a *receiving*
      // socket's fd is EEXIST (test-dgram-bind-fd-error).
      if (udpWatchedFds.has(fd | 0)) return uvOf("EEXIST");
      const r = ND.open(fd | 0);
      if (typeof r === "string") return uvOf(r);
      this.fd = fd | 0;
      return 0;
    }
    // node UDPWrap::DoSend: >= 1 means "finished synchronously, and the value is
    // length + 1"; 0 means "queued, oncomplete will fire"; < 0 is an errno. The
    // POSIX sendto() behind this always completes, so only the first two happen.
    _send_(req, list, count, port, address, hasCallback, v6) {
      const e = this._ensureFd_(v6);
      if (e !== 0) return e;
      let total = 0;
      for (let i = 0; i < count; i++) total += list[i].length;
      let joined;
      if (count === 1) joined = list[0];
      else {
        joined = B ? B.allocUnsafe(total) : new Uint8Array(total);
        let off = 0;
        for (let i = 0; i < count; i++) { joined.set(list[i], off); off += list[i].length; }
      }
      const connected = port === undefined || port === null;
      const r = ND.send(this.fd, toB64(joined), connected ? 0 : (port | 0),
                        address == null ? "" : String(address), !!v6, connected);
      if (typeof r === "string") return uvOf(r);
      // Node's --test-udp-no-try-send disables the synchronous fast path so
      // callers can observe pending sends. The reactor still completes the
      // datagram immediately, but retain the libuv queue counters until the
      // next microtask, matching that observable contract.
      const argv = (G.process && Array.isArray(G.process.execArgv)) ? G.process.execArgv : [];
      if (argv.indexOf("--test-udp-no-try-send") !== -1) {
        this._sendQueueSize += total;
        this._sendQueueCount++;
        G.queueMicrotask(() => {
          this._sendQueueSize -= total;
          this._sendQueueCount--;
        });
      }
      return (r | 0) + 1;
    }
    send(req, list, count, port, address, hasCallback) {
      if (arguments.length === 4) return this._send_(req, list, count, null, null, port, false);
      return this._send_(req, list, count, port, address, hasCallback, false);
    }
    send6(req, list, count, port, address, hasCallback) {
      if (arguments.length === 4) return this._send_(req, list, count, null, null, port, true);
      return this._send_(req, list, count, port, address, hasCallback, true);
    }
    recvStart() {
      if (this.fd < 0 || this._receiving) return 0;
      this._receiving = true;
      this._loopOpen = true;
      udpWatchedFds.add(this.fd);       // uv__io_start: the fd joins the loop
      if (NET) { NET.items.add(this); NET.hold(this); }
      return 0;
    }
    recvStop() {
      if (this._receiving && this.fd >= 0) udpWatchedFds.delete(this.fd);
      this._receiving = false;
      this._loopOpen = false;
      if (NET) { NET.items.delete(this); NET.release(this); }
      return 0;
    }
    // Reactor entry point: drain every readable datagram, then hand each to
    // node's onmessage(nread, handle, buf, rinfo) contract.
    _poll() {
      if (this._closed || this.fd < 0 || !this._receiving) return 0;
      let n = 0;
      while (!this._closed && this._receiving && this.fd >= 0) {
        const d = ND.recv(this.fd);
        if (d === null || d === undefined) break;
        if (typeof d === "string") {
          if (typeof this.onmessage === "function") this.onmessage(uvOf(d), this, null, null);
          break;
        }
        n++;
        const buf = fromB64(d.data);
        if (typeof this.onmessage === "function") {
          this.onmessage(buf.length, this, buf, { address: d.address, family: d.family, port: d.port, size: d.size });
        }
      }
      return n;
    }
    close(cb) {
      this.recvStop();
      if (this.fd >= 0) { ND.close(this.fd); this.fd = -1; }
      this._closed = true;
      if (typeof cb === "function") nextTick(cb);
      return 0;
    }
    getsockname(out) {
      if (this.fd < 0) return uvOf("EBADF");
      const r = ND.address(this.fd);
      if (typeof r === "string") return uvOf(r);
      out.address = r.address; out.family = r.family; out.port = r.port;
      return 0;
    }
    getpeername(out) {
      if (this.fd < 0) return uvOf("EBADF");
      const r = ND.peername(this.fd);
      if (typeof r === "string") return uvOf(r);
      out.address = r.address; out.family = r.family; out.port = r.port;
      return 0;
    }
    // node UDPWrap::BufferSize: returns undefined on failure with ctx.code /
    // ctx.message filled in; lib/dgram.js turns that into ERR_SOCKET_BUFFER_SIZE.
    bufferSize(size, isRecv, ctx) {
      const syscall = isRecv ? "uv_recv_buffer_size" : "uv_send_buffer_size";
      if (this.fd < 0) {
        if (ctx) { ctx.errno = uvOf("EBADF"); ctx.code = "EBADF"; ctx.message = "bad file descriptor"; ctx.syscall = syscall; }
        return undefined;
      }
      const r = ND.bufsize(this.fd, size | 0, !!isRecv);
      if (typeof r === "string") {
        if (ctx) { ctx.errno = uvOf(r); ctx.code = r; ctx.message = r; ctx.syscall = syscall; }
        return undefined;
      }
      return r | 0;
    }
    // An unbound handle has NO socket yet — node's UDP handle reports EBADF for
    // every option call in that state (test-dgram-setBroadcast,
    // test-dgram-multicast-loopback, test-dgram-socket-buffer-size all assert
    // it). Creating the fd lazily here would silently succeed instead.
    _setopt_(name, value) {
      if (this.fd < 0) return uvOf("EBADF");
      return rc(ND.setopt(this.fd, name, value | 0, this._family6));
    }
    setBroadcast(on) { return this._setopt_("broadcast", on); }
    setTTL(ttl) { return this._setopt_("ttl", ttl); }
    setMulticastTTL(ttl) { return this._setopt_("multicastTTL", ttl); }
    setMulticastLoopback(on) { return this._setopt_("multicastLoopback", on); }
    setMulticastInterface(iface) {
      if (this.fd < 0) return uvOf("EBADF");
      return rc(ND.mcastiface(this.fd, iface == null ? "" : String(iface), this._family6));
    }
    // uv_udp_set_membership runs uv__udp_maybe_deferred_bind FIRST, so an
    // unbound socket gets its descriptor here rather than reporting EBADF —
    // node's addMembership() on a fresh dgram.Socket has to reach setsockopt(2)
    // and answer EINVAL for a bad group (test-dgram-membership). That is the
    // opposite of uv_udp_set_broadcast/_ttl above, which really do return
    // UV_EBADF while unbound.
    _membership_(add, group, iface) {
      const e = this._ensureFd_(this._family6);
      if (e !== 0) return e;
      return rc(ND.membership(this.fd, add, String(group), iface == null ? "" : String(iface), this._family6));
    }
    addMembership(group, iface) { return this._membership_(true, group, iface); }
    dropMembership(group, iface) { return this._membership_(false, group, iface); }
    _srcMembership_(add, source, group, iface) {
      const e = this._ensureFd_(this._family6);  // uv__udp_maybe_deferred_bind
      if (e !== 0) return e;
      return rc(ND.srcmembership(this.fd, add, String(source), String(group), iface == null ? "" : String(iface), this._family6));
    }
    addSourceSpecificMembership(source, group, iface) { return this._srcMembership_(true, source, group, iface); }
    dropSourceSpecificMembership(source, group, iface) { return this._srcMembership_(false, source, group, iface); }
    // The POSIX send path is synchronous, so nothing is ever queued.
    getSendQueueSize() { return this._sendQueueSize; }
    getSendQueueCount() { return this._sendQueueCount; }
    ref() { this._refd = true; if (NET) NET.hold(this); }
    unref() { this._refd = false; if (NET) NET.release(this); }
    hasRef() { return this._refd !== false; }
  }

  // node src/udp_wrap.cc SendWrap: a ReqWrap carrying the send's list/address/
  // port and its oncomplete. Only used for the async completion path.
  class SendWrap {
    constructor() { this.list = null; this.address = undefined; this.port = undefined; this.callback = undefined; this.oncomplete = undefined; }
  }

  const udpWrap = {
    UDP, SendWrap,
    // uv.h UV_UDP_* flags, exactly as internalBinding("udp_wrap").constants.
    constants: { UV_UDP_IPV6ONLY: 1, UV_UDP_REUSEADDR: 4, UV_UDP_REUSEPORT: 8 },
  };
  // node src/udp_wrap.cc is reachable as internalBinding("udp_wrap"): the corpus
  // builds handles out of it directly (test-dgram-bind-fd,
  // test-dgram-create-socket-handle).
  if (typeof G.__mbunInternalBindingDefine === "function") {
    G.__mbunInternalBindingDefine("udp_wrap", () => udpWrap);
  }

  // ============================================================ internal/dgram
  const kStateSymbol = Symbol("state symbol");

  const isIP = typeof net.isIP === "function" ? net.isIP : () => 0;
  // internal/dgram.js lookup4/lookup6: the handle's lookup is dns.lookup bound to
  // the family, with the loopback default. It is read at CALL time, because
  // test-dgram-custom-lookup replaces dns.lookup after the socket exists and
  // requires the default path to go through the replacement.
  const makeLookup = (lookup, family) => function (address, callback) {
    const host = address || (family === 6 ? "::1" : "127.0.0.1");
    const fn = lookup !== undefined ? lookup
      : ((M["dns"] || M["node:dns"] || {}).lookup);
    if (typeof fn !== "function") { nextTick(callback, ErrnoException(uvOf("ENOENT"), "getaddrinfo")); return; }
    return fn(host, family, callback);
  };

  function newHandle(type, lookup) {
    if (lookup !== undefined) validateFunction(lookup, "lookup");
    if (type === "udp4") {
      const handle = new UDP();
      handle.lookup = makeLookup(lookup, 4);
      return handle;
    }
    if (type === "udp6") {
      const handle = new UDP();
      handle._family6 = true;
      handle.lookup = makeLookup(lookup, 6);
      handle.bind = handle.bind6;
      handle.connect = handle.connect6;
      handle.send = handle.send6;
      return handle;
    }
    throw ERR_SOCKET_BAD_TYPE();
  }

  function _createSocketHandle(address, port, addressType, fd, flags) {
    const handle = newHandle(addressType);
    let err;
    if (isInt32(fd) && fd > 0) {
      const type = ND.handletype(fd);
      if (type !== "UDP") err = uvOf("EINVAL");
      else err = handle.open(fd);
    } else if (port || address) {
      err = handle.bind(address, port || 0, flags);
    }
    if (err) { handle.close(); return err; }
    return handle;
  }

  M["internal/dgram"] = { kStateSymbol, _createSocketHandle, newHandle };

  // ==================================================================== Socket
  const BIND_STATE_UNBOUND = 0, BIND_STATE_BINDING = 1, BIND_STATE_BOUND = 2;
  const CONNECT_STATE_DISCONNECTED = 0, CONNECT_STATE_CONNECTING = 1, CONNECT_STATE_CONNECTED = 2;
  const RECV_BUFFER = true, SEND_BUFFER = false;

  const isBlockList = (v) => !!v && typeof v === "object" &&
    (typeof net.BlockList === "function" ? v instanceof net.BlockList : typeof v.check === "function");

  // internal/async_hooks owner_symbol: the handle → Socket back-pointer node's
  // onmessage/onerror read (`handle[owner_symbol]`).
  const kOwner = Symbol("owner_symbol");

  class Socket extends EE {
    constructor(type, listener) {
      super();
      let lookup, recvBufferSize, sendBufferSize, receiveBlockList, sendBlockList, options;
      if (type !== null && typeof type === "object") {
        options = type;
        type = options.type;
        lookup = options.lookup;
        if (options.recvBufferSize) validateUint32(options.recvBufferSize, "options.recvBufferSize");
        if (options.sendBufferSize) validateUint32(options.sendBufferSize, "options.sendBufferSize");
        recvBufferSize = options.recvBufferSize;
        sendBufferSize = options.sendBufferSize;
        if (options.receiveBlockList) {
          if (!isBlockList(options.receiveBlockList))
            throw ERR_INVALID_ARG_TYPE("options.receiveBlockList", "net.BlockList", options.receiveBlockList);
          receiveBlockList = options.receiveBlockList;
        }
        if (options.sendBlockList) {
          if (!isBlockList(options.sendBlockList))
            throw ERR_INVALID_ARG_TYPE("options.sendBlockList", "net.BlockList", options.sendBlockList);
          sendBlockList = options.sendBlockList;
        }
      }

      const handle = newHandle(type, lookup);
      handle[kOwner] = this;
      this.type = type;

      if (typeof listener === "function") this.on("message", listener);

      this[kStateSymbol] = {
        handle,
        receiving: false,
        bindState: BIND_STATE_UNBOUND,
        connectState: CONNECT_STATE_DISCONNECTED,
        queue: undefined,
        reuseAddr: options && options.reuseAddr,
        reusePort: options && options.reusePort,
        ipv6Only: options && options.ipv6Only,
        recvBufferSize, sendBufferSize, receiveBlockList, sendBlockList,
      };

      if (options && options.signal !== undefined) {
        const signal = options.signal;
        validateAbortSignal(signal, "options.signal");
        const onAborted = () => { if (this[kStateSymbol].handle) this.close(); };
        if (signal.aborted) onAborted();
        else {
          signal.addEventListener("abort", onAborted, { once: true });
          this.once("close", () => { try { signal.removeEventListener("abort", onAborted); } catch (e) {} });
        }
      }
      // node lib/dgram.js: the last thing the constructor does is announce the
      // socket on the 'udp.socket' diagnostics channel.
      if (udpSocketChannel.hasSubscribers) udpSocketChannel.publish({ socket: this });
    }
  }

  function createSocket(type, listener) { return new Socket(type, listener); }

  function startListening(socket) {
    const state = socket[kStateSymbol];
    state.handle.onmessage = onMessage;
    state.handle.onerror = onError;
    state.handle.recvStart();
    state.receiving = true;
    state.bindState = BIND_STATE_BOUND;
    if (state.recvBufferSize) bufferSize(socket, state.recvBufferSize, RECV_BUFFER);
    if (state.sendBufferSize) bufferSize(socket, state.sendBufferSize, SEND_BUFFER);
    socket.emit("listening");
  }

  function replaceHandle(self, newHandleObj) {
    const state = self[kStateSymbol];
    const oldHandle = state.handle;
    if (!oldHandle.hasRef() && typeof newHandleObj.unref === "function") newHandleObj.unref();
    newHandleObj.lookup = oldHandle.lookup;
    newHandleObj.bind = oldHandle.bind;
    newHandleObj.send = oldHandle.send;
    // node's handle arrives already built for the right family (its primary calls
    // dgram._createSocketHandle with the addressType). mbun rebuilds the handle
    // from a bare descriptor, so the family has to ride across from the socket's
    // own handle or every setopt/membership call would use the v4 socket level.
    if (newHandleObj._family6 !== undefined) newHandleObj._family6 = !!oldHandle._family6;
    newHandleObj[kOwner] = self;
    oldHandle.close();
    state.handle = newHandleObj;
  }

  // lib/dgram.js lazyLoadCluster(): read at CALL time, never at module scope.
  // node:cluster freezes its primary/worker role at first require and a worker's
  // role decides whether bind() may touch bind(2) at all, so capturing it here
  // would also capture the role of whichever module happened to load first.
  const lazyLoadCluster = () => M["cluster"] || M["node:cluster"] || G.__mbunCluster || {};

  // lib/dgram.js bindServerHandle(): in a cluster worker a non-exclusive bind is
  // not a bind — it is a request to the primary for the already-bound shared UDP
  // handle (internal/cluster/shared_handle.js), which then replaces this
  // socket's own. Both failure shapes matter to the corpus: an error reply after
  // the socket was closed must stay silent (test-dgram-bind-socket-close-before-
  // cluster-reply asserts mustNotCall on 'error'), and a handle that arrives
  // after the close must be closed rather than adopted (test-dgram-cluster-
  // close-during-bind terminates on exactly that handle.close()).
  function bindServerHandle(self, options, errCb) {
    const cluster = lazyLoadCluster();
    const state = self[kStateSymbol];
    cluster._getServer(self, options, (err, handle) => {
      if (err) {
        // Do not call the callback if the socket is closed.
        if (state.handle) errCb(err);
        return;
      }
      if (!state.handle) {
        // Handle has been closed in the mean time.
        return handle.close();
      }
      replaceHandle(self, handle);
      startListening(self);
    });
  }

  function bufferSize(self, size, buffer) {
    if (size >>> 0 !== size) throw ERR_SOCKET_BAD_BUFFER_SIZE();
    const ctx = {};
    const ret = self[kStateSymbol].handle.bufferSize(size, buffer, ctx);
    if (ret === undefined) throw ERR_SOCKET_BUFFER_SIZE(ctx);
    return ret;
  }

  Socket.prototype.bind = function (port_, address_ /* , callback */) {
    let port = port_;
    healthCheck(this);
    const state = this[kStateSymbol];
    if (state.bindState !== BIND_STATE_UNBOUND) throw ERR_SOCKET_ALREADY_BOUND();
    state.bindState = BIND_STATE_BINDING;

    const cb = arguments.length && arguments[arguments.length - 1];
    if (typeof cb === "function") {
      const removeListeners = function () {
        this.removeListener("error", removeListeners);
        this.removeListener("listening", onListening);
      };
      const onListening = function () {
        removeListeners.call(this);
        cb.call(this);
      };
      this.on("error", removeListeners);
      this.on("listening", onListening);
    }

    // bind(handle): adopt a handle handed over by the cluster primary.
    if (port !== null && typeof port === "object" && typeof port.recvStart === "function") {
      replaceHandle(this, port);
      startListening(this);
      return this;
    }

    // bind({ fd }): open an existing descriptor instead of creating one.
    if (port !== null && typeof port === "object" && isInt32(port.fd) && port.fd > 0) {
      const fd = port.fd;
      const fdExclusive = !!port.exclusive;
      const fdCluster = lazyLoadCluster();
      if (fdCluster.isWorker && !fdExclusive) {
        bindServerHandle(this, {
          address: null, port: null, addressType: this.type, fd, flags: null,
        }, (err) => {
          const ex = ErrnoException(err, "open");
          state.bindState = BIND_STATE_UNBOUND;
          this.emit("error", ex);
        });
        return this;
      }
      const type = ND.handletype(fd);
      if (type !== "UDP") throw ERR_INVALID_FD_TYPE(type);
      const err = state.handle.open(fd);
      if (err) throw ErrnoException(err, "open");
      startListening(this);
      return this;
    }

    let address;
    let exclusive;
    if (port !== null && typeof port === "object") {
      address = port.address || "";
      exclusive = !!port.exclusive;
      port = port.port;
    } else {
      address = typeof address_ === "function" ? "" : address_;
      exclusive = false;
    }

    // Defaulting address for bind to all interfaces.
    if (!address) address = this.type === "udp4" ? "0.0.0.0" : "::";

    // Resolve address first (node does; a hostname bind must not reach bind(2)).
    state.handle.lookup(address, (err, ip) => {
      if (!state.handle) return;  // closed in the mean time
      if (err) {
        state.bindState = BIND_STATE_UNBOUND;
        this.emit("error", err);
        return;
      }
      let flags = 0;
      if (state.reuseAddr) flags |= 4;      // UV_UDP_REUSEADDR
      if (state.ipv6Only) flags |= 1;       // UV_UDP_IPV6ONLY
      // SO_REUSEPORT means "every socket binds for itself", so it also means the
      // worker must NOT ask the primary for a shared handle (lib/dgram.js).
      if (state.reusePort) { exclusive = true; flags |= 8; }  // UV_UDP_REUSEPORT
      const cluster = lazyLoadCluster();
      if (cluster.isWorker && !exclusive) {
        bindServerHandle(this, {
          address: ip, port: port, addressType: this.type, fd: -1, flags: flags,
        }, (err) => {
          const ex = ExceptionWithHostPort(err, "bind", ip, port);
          state.bindState = BIND_STATE_UNBOUND;
          this.emit("error", ex);
        });
        return;
      }
      const berr = state.handle.bind(ip, port || 0, flags);
      if (berr) {
        const ex = ExceptionWithHostPort(berr, "bind", ip, port);
        state.bindState = BIND_STATE_UNBOUND;
        this.emit("error", ex);
        return;
      }
      startListening(this);
    });

    return this;
  };

  Socket.prototype.connect = function (port, address, callback) {
    port = validatePort(port, "Port", false);
    if (typeof address === "function") { callback = address; address = ""; }
    else if (address === undefined) address = "";
    validateString(address, "address");

    const state = this[kStateSymbol];
    if (state.connectState !== CONNECT_STATE_DISCONNECTED) throw ERR_SOCKET_DGRAM_IS_CONNECTED();
    state.connectState = CONNECT_STATE_CONNECTING;
    if (state.bindState === BIND_STATE_UNBOUND) this.bind({ port: 0, exclusive: true }, null);

    if (state.bindState !== BIND_STATE_BOUND) {
      enqueue(this, _connect.bind(this, port, address, callback));
      return;
    }
    _connect.call(this, port, address, callback);
  };

  function _connect(port, address, callback) {
    const state = this[kStateSymbol];
    if (callback) this.once("connect", callback);
    const self = this;
    state.handle.lookup(address, (ex, ip) => doConnect(ex, self, ip, address, port, callback));
  }

  function doConnect(ex, self, ip, address, port, callback) {
    const state = self[kStateSymbol];
    if (!state.handle) return;
    if (!ex && state.sendBlockList && state.sendBlockList.check(ip, "ipv" + isIP(ip))) {
      ex = ERR_IP_BLOCKED(ip);
    }
    if (!ex) {
      const err = state.handle.connect(ip, port);
      if (err) ex = ExceptionWithHostPort(err, "connect", address, port);
    }
    if (ex) {
      state.connectState = CONNECT_STATE_DISCONNECTED;
      nextTick(() => {
        if (callback) { self.removeListener("connect", callback); callback(ex); }
        else self.emit("error", ex);
      });
      return;
    }
    state.connectState = CONNECT_STATE_CONNECTED;
    nextTick(() => self.emit("connect"));
  }

  Socket.prototype.disconnect = function () {
    const state = this[kStateSymbol];
    if (state.connectState !== CONNECT_STATE_CONNECTED) throw ERR_SOCKET_DGRAM_NOT_CONNECTED();
    const err = state.handle.disconnect();
    if (err) throw ErrnoException(err, "connect");
    state.connectState = CONNECT_STATE_DISCONNECTED;
  };

  Socket.prototype.sendto = function (buffer, offset, length, port, address, callback) {
    validateNumber(offset, "offset");
    validateNumber(length, "length");
    validateNumber(port, "port");
    validateString(address, "address");
    this.send(buffer, offset, length, port, address, callback);
  };

  const isArrayBufferView = (v) => ArrayBuffer.isView(v);

  function sliceBuffer(buffer, offset, length) {
    if (typeof buffer === "string") buffer = B.from(buffer);
    else if (!isArrayBufferView(buffer)) {
      throw ERR_INVALID_ARG_TYPE("buffer", ["Buffer", "TypedArray", "DataView", "string"], buffer);
    }
    offset = offset >>> 0;
    length = length >>> 0;
    if (offset > buffer.byteLength) throw ERR_BUFFER_OUT_OF_BOUNDS("offset");
    if (offset + length > buffer.byteLength) throw ERR_BUFFER_OUT_OF_BOUNDS("length");
    return B.from(buffer.buffer, buffer.byteOffset + offset, length);
  }

  function fixBufferList(list) {
    const newlist = new Array(list.length);
    for (let i = 0, l = list.length; i < l; i++) {
      const buf = list[i];
      if (typeof buf === "string") newlist[i] = B.from(buf);
      else if (B.isBuffer(buf)) newlist[i] = buf;
      else if (!isArrayBufferView(buf)) return null;
      else newlist[i] = B.from(buf.buffer, buf.byteOffset, buf.byteLength);
    }
    return newlist;
  }

  function enqueue(self, toEnqueue) {
    const state = self[kStateSymbol];
    if (state.queue === undefined) {
      state.queue = [];
      self.once(EE.errorMonitor !== undefined ? EE.errorMonitor : "error", onListenError);
      self.once("listening", onListenSuccess);
    }
    state.queue.push(toEnqueue);
  }

  function onListenSuccess() {
    this.removeListener(EE.errorMonitor !== undefined ? EE.errorMonitor : "error", onListenError);
    clearQueue.call(this);
  }

  function onListenError(err) {
    this.removeListener("listening", onListenSuccess);
    this[kStateSymbol].queue = undefined;
  }

  function clearQueue() {
    const state = this[kStateSymbol];
    const queue = state.queue;
    state.queue = undefined;
    for (const queueEntry of queue) queueEntry();
  }

  Socket.prototype.send = function (buffer, offset, length, port, address, callback) {
    let list;
    const state = this[kStateSymbol];
    const connected = state.connectState === CONNECT_STATE_CONNECTED;
    if (!connected) {
      if (address || (port && typeof port !== "function")) {
        buffer = sliceBuffer(buffer, offset, length);
      } else {
        callback = port;
        port = offset;
        address = length;
      }
    } else {
      if (typeof length === "number") {
        buffer = sliceBuffer(buffer, offset, length);
        if (typeof port === "function") { callback = port; port = null; }
      } else {
        callback = offset;
      }
      if (port || address) throw ERR_SOCKET_DGRAM_IS_CONNECTED();
    }

    if (!Array.isArray(buffer)) {
      if (typeof buffer === "string") list = [B.from(buffer)];
      else if (!isArrayBufferView(buffer)) {
        throw ERR_INVALID_ARG_TYPE("buffer", ["Buffer", "TypedArray", "DataView", "string"], buffer);
      } else list = [buffer];
    } else if (!(list = fixBufferList(buffer))) {
      throw ERR_INVALID_ARG_TYPE("buffer list arguments", ["Buffer", "TypedArray", "DataView", "string"], buffer);
    }

    if (!connected) port = validatePort(port, "Port", false);

    if (typeof callback !== "function") callback = undefined;

    if (typeof address === "function") { callback = address; address = undefined; }
    else if (address != null) validateString(address, "address");

    healthCheck(this);

    if (state.bindState === BIND_STATE_UNBOUND) this.bind({ port: 0, exclusive: true }, null);

    if (list.length === 0) list.push(B ? B.alloc(0) : new Uint8Array(0));

    if (state.bindState !== BIND_STATE_BOUND) {
      enqueue(this, Socket.prototype.send.bind(this, list, port, address, callback));
      return;
    }

    const self = this;
    const afterDns = (ex, ip) => doSend(ex, self, ip, list, address, port, callback);
    if (!connected) state.handle.lookup(address, afterDns);
    else afterDns(null, null);
  };

  function doSend(ex, self, ip, list, address, port, callback) {
    const state = self[kStateSymbol];
    if (ex) {
      if (typeof callback === "function") { nextTick(callback, ex); return; }
      nextTick(() => self.emit("error", ex));
      return;
    } else if (!state.handle) return;

    if (ip && state.sendBlockList && state.sendBlockList.check(ip, "ipv" + isIP(ip))) {
      if (callback) nextTick(callback, ERR_IP_BLOCKED(ip));
      return;
    }

    const req = new SendWrap();
    req.list = list;
    req.address = address;
    req.port = port;
    if (callback) { req.callback = callback; req.oncomplete = afterSend; }

    let err;
    if (port) err = state.handle.send(req, list, list.length, port, ip, !!callback);
    else err = state.handle.send(req, list, list.length, !!callback);

    if (err >= 1) {
      // Synchronous finish; the return code is msg_length + 1.
      if (callback) nextTick(callback, null, err - 1);
      return;
    }
    if (err && callback) {
      const ex2 = ExceptionWithHostPort(err, "send", address, port);
      nextTick(callback, ex2);
    }
  }

  function afterSend(err, sent) {
    if (err) err = ExceptionWithHostPort(err, "send", this.address, this.port);
    else err = null;
    this.callback(err, sent);
  }

  Socket.prototype.close = function (callback) {
    const state = this[kStateSymbol];
    const queue = state.queue;
    if (typeof callback === "function") this.on("close", callback);
    if (queue !== undefined) { queue.push(Socket.prototype.close.bind(this)); return this; }
    healthCheck(this);
    stopReceiving(this);
    state.handle.close();
    state.handle = null;
    nextTick(socketCloseNT, this);
    return this;
  };

  if (typeof Symbol.asyncDispose === "symbol") {
    Socket.prototype[Symbol.asyncDispose] = async function () {
      if (!this[kStateSymbol].handle) return;
      await new Promise((resolve) => { this.close(resolve); });
    };
  }

  function socketCloseNT(self) { self.emit("close"); }

  Socket.prototype.address = function () {
    healthCheck(this);
    const out = {};
    const err = this[kStateSymbol].handle.getsockname(out);
    if (err) throw ErrnoException(err, "getsockname");
    return out;
  };

  Socket.prototype.remoteAddress = function () {
    healthCheck(this);
    const state = this[kStateSymbol];
    if (state.connectState !== CONNECT_STATE_CONNECTED) throw ERR_SOCKET_DGRAM_NOT_CONNECTED();
    const out = {};
    const err = state.handle.getpeername(out);
    if (err) throw ErrnoException(err, "getpeername");
    return out;
  };

  Socket.prototype.setBroadcast = function (arg) {
    const err = this[kStateSymbol].handle.setBroadcast(arg ? 1 : 0);
    if (err) throw ErrnoException(err, "setBroadcast");
  };

  Socket.prototype.setTTL = function (ttl) {
    validateNumber(ttl, "ttl");
    const err = this[kStateSymbol].handle.setTTL(ttl);
    if (err) throw ErrnoException(err, "setTTL");
    return ttl;
  };

  Socket.prototype.setMulticastTTL = function (ttl) {
    validateNumber(ttl, "ttl");
    const err = this[kStateSymbol].handle.setMulticastTTL(ttl);
    if (err) throw ErrnoException(err, "setMulticastTTL");
    return ttl;
  };

  Socket.prototype.setMulticastLoopback = function (arg) {
    const err = this[kStateSymbol].handle.setMulticastLoopback(arg ? 1 : 0);
    if (err) throw ErrnoException(err, "setMulticastLoopback");
    return arg;  // 0.4 compatibility
  };

  Socket.prototype.setMulticastInterface = function (interfaceAddress) {
    healthCheck(this);
    validateString(interfaceAddress, "interfaceAddress");
    const err = this[kStateSymbol].handle.setMulticastInterface(interfaceAddress);
    if (err) throw ErrnoException(err, "setMulticastInterface");
  };

  Socket.prototype.addMembership = function (multicastAddress, interfaceAddress) {
    healthCheck(this);
    if (!multicastAddress) throw ERR_MISSING_ARGS("multicastAddress");
    const err = this[kStateSymbol].handle.addMembership(multicastAddress, interfaceAddress);
    if (err) throw ErrnoException(err, "addMembership");
  };

  Socket.prototype.dropMembership = function (multicastAddress, interfaceAddress) {
    healthCheck(this);
    if (!multicastAddress) throw ERR_MISSING_ARGS("multicastAddress");
    const err = this[kStateSymbol].handle.dropMembership(multicastAddress, interfaceAddress);
    if (err) throw ErrnoException(err, "dropMembership");
  };

  Socket.prototype.addSourceSpecificMembership = function (sourceAddress, groupAddress, interfaceAddress) {
    healthCheck(this);
    validateString(sourceAddress, "sourceAddress");
    validateString(groupAddress, "groupAddress");
    const err = this[kStateSymbol].handle.addSourceSpecificMembership(sourceAddress, groupAddress, interfaceAddress);
    if (err) throw ErrnoException(err, "addSourceSpecificMembership");
  };

  Socket.prototype.dropSourceSpecificMembership = function (sourceAddress, groupAddress, interfaceAddress) {
    healthCheck(this);
    validateString(sourceAddress, "sourceAddress");
    validateString(groupAddress, "groupAddress");
    const err = this[kStateSymbol].handle.dropSourceSpecificMembership(sourceAddress, groupAddress, interfaceAddress);
    if (err) throw ErrnoException(err, "dropSourceSpecificMembership");
  };

  function healthCheck(socket) {
    if (!socket[kStateSymbol].handle) throw ERR_SOCKET_DGRAM_NOT_RUNNING();
  }

  function stopReceiving(socket) {
    const state = socket[kStateSymbol];
    if (!state.receiving) return;
    state.handle.recvStop();
    state.receiving = false;
  }

  function onMessage(nread, handle, buf, rinfo) {
    const self = handle[kOwner];
    if (!self) return;
    if (nread < 0) return self.emit("error", ErrnoException(nread, "recvmsg"));
    const state = self[kStateSymbol];
    if (state && state.receiveBlockList &&
        state.receiveBlockList.check(rinfo.address, rinfo.family && String(rinfo.family).toLowerCase())) {
      return;
    }
    rinfo.size = buf.length;  // compatibility
    self.emit("message", buf, rinfo);
  }

  function onError(nread, handle, error) {
    const self = handle[kOwner];
    if (self) self.emit("error", error);
  }

  Socket.prototype.ref = function () {
    const handle = this[kStateSymbol].handle;
    if (handle) handle.ref();
    return this;
  };

  Socket.prototype.unref = function () {
    const handle = this[kStateSymbol].handle;
    if (handle) handle.unref();
    return this;
  };

  Socket.prototype.hasRef = function () {
    const handle = this[kStateSymbol].handle;
    return handle ? handle.hasRef() : false;
  };

  Socket.prototype.setRecvBufferSize = function (size) { bufferSize(this, size, RECV_BUFFER); };
  Socket.prototype.setSendBufferSize = function (size) { bufferSize(this, size, SEND_BUFFER); };
  Socket.prototype.getRecvBufferSize = function () { return bufferSize(this, 0, RECV_BUFFER); };
  Socket.prototype.getSendBufferSize = function () { return bufferSize(this, 0, SEND_BUFFER); };
  Socket.prototype.getSendQueueSize = function () { return this[kStateSymbol].handle.getSendQueueSize(); };
  Socket.prototype.getSendQueueCount = function () { return this[kStateSymbol].handle.getSendQueueCount(); };

  // node's dgram.Socket predates ES classes, so `dgram.Socket('udp4')` with no
  // `new` is legal. Re-export through a plain-function stand-in that keeps
  // construct behaviour, prototype identity and instanceof intact (same shape
  // js_net.cppm / js_tls_live.cppm use).
  const callable = (Cls) => {
    const wrapper = function (...args) {
      if (new.target !== undefined) return Reflect.construct(Cls, args, new.target);
      return Reflect.construct(Cls, args);
    };
    try {
      Object.setPrototypeOf(wrapper, Cls);
      wrapper.prototype = Cls.prototype;
      Object.defineProperty(wrapper, "name", { value: Cls.name, configurable: true });
      Object.defineProperty(wrapper, "length", { value: Cls.length, configurable: true });
      Object.defineProperty(Cls.prototype, "constructor", { value: wrapper, writable: true, configurable: true });
    } catch (e) { return Cls; }
    return wrapper;
  };
  const SocketW = callable(Socket);

  M["dgram"] = M["node:dgram"] = { createSocket, Socket: SocketW };
})();
)JS";

}  // namespace mbun::jsc::js_dgram
