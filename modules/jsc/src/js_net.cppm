// modules/jsc/src/js_net.cppm — module mbun.jsc.js_net
//
// Pure-JS networking layer: real node:net Socket/Server, node:http server,
// Bun.serve, and a real network fetch() — all built on the non-blocking POSIX
// TCP primitives in __mbunNetNative (runtime.cppm). Evaluated once at runtime
// init, AFTER kNodeBuiltinsJS (it re-registers the net/http modules over the
// earlier load-only stubs and replaces the fetch stub).
//
// Architecture (see runtime.cppm __mbunNetNative for the native half): a
// single-threaded reactor. Every socket is O_NONBLOCK; all progress is made on
// the JS thread by the event-loop pump calling globalThis.__mbunNetDrain(),
// which polls each registered pollable (listeners accept, sockets flush their
// write queues and read available bytes into the incremental HTTP parsers).
// While a fetch() is awaiting its response, __mbunNet.pending > 0 keeps the
// pump alive, so an in-process Bun.serve/net server gets its handler dispatched
// between drains — no threads, no re-entrancy, JSC stays single-threaded.
//
// Behaviour blueprint: bun's fetch error taxonomy (ECONNRESET on a connection
// closed mid-body, InvalidHTTPResponse on malformed chunked framing — see
// compat/bun/test/js/web/fetch/chunked-trailing.test.js) and Bun.serve's Request/
// Response dispatch shape. Bun.serve streams `type: "direct"` ReadableStream
// bodies chunked (write/flush map to the socket via an HTTPResponseSink; see
// writeDirectStreamResponse). DEFERRED (honest throws): TLS (https/tls), unix
// sockets, WebSocket upgrade, HTTP/2+, proxy CONNECT, non-direct ReadableStream
// response bodies (still drained then sent with Content-Length), and DNS for
// non-local hostnames.
export module mbun.jsc.js_net;

import std;

namespace mbun::jsc::js_net {

export constexpr std::string_view kNetJS = R"JS(
(function () {
  "use strict";
  const G = globalThis;
  const NN = G.__mbunNetNative;
  const SN = G.__mbunServeNative || null;  // native epoll Bun.serve (T-LOOP, Linux)
  const M = G.__mbunNativeModules || (G.__mbunNativeModules = {});
  const def = (names, mod) => { for (const n of names) { M[n] = mod; M["node:" + n] = mod; } };
  const EE = (M["events"] && M["events"].EventEmitter) || class { on() { return this; } once() { return this; } off() { return this; } emit() { return false; } };
  const te = new G.TextEncoder(), td = new G.TextDecoder();

  if (typeof Promise.withResolvers !== "function") {
    Promise.withResolvers = function () { let resolve, reject; const promise = new Promise((res, rej) => { resolve = res; reject = rej; }); return { promise, resolve, reject }; };
  }

  // ---- built-in net diagnostics channels (node lib/net.js) ----
  // Resolved once at module load; every publish is gated on hasSubscribers so an
  // unsubscribed channel costs one property read per connection.
  const __dc = M["diagnostics_channel"] || M["node:diagnostics_channel"];
  const kNoDC = { hasSubscribers: false, publish() {} };
  const netClientSocketChannel = (__dc && typeof __dc.channel === "function") ? __dc.channel("net.client.socket") : kNoDC;
  const netServerSocketChannel = (__dc && typeof __dc.channel === "function") ? __dc.channel("net.server.socket") : kNoDC;
  const netServerListen = (__dc && typeof __dc.tracingChannel === "function")
    ? __dc.tracingChannel("net.server.listen")
    : { hasSubscribers: false, asyncStart: kNoDC, asyncEnd: kNoDC, error: kNoDC };
  // node lib/_http_server.js: the server half of the http diagnostics channels
  // (the client half lives with ClientRequest in the node_http partition).
  const onRequestStartChannel = (__dc && typeof __dc.channel === "function") ? __dc.channel("http.server.request.start") : kNoDC;
  const onResponseFinishChannel = (__dc && typeof __dc.channel === "function") ? __dc.channel("http.server.response.finish") : kNoDC;

  // ---- byte helpers (bytes cross the native boundary as base64) ----
  const u8 = (d) => (d == null ? new Uint8Array(0) : typeof d === "string" ? te.encode(d) : d instanceof Uint8Array ? d : ArrayBuffer.isView(d) ? new Uint8Array(d.buffer, d.byteOffset, d.byteLength) : d instanceof ArrayBuffer ? new Uint8Array(d) : d._u8 instanceof Uint8Array ? d._u8 : te.encode(String(d)));
  const toB64 = (b) => { let s = ""; for (let i = 0; i < b.length; i += 4096) s += String.fromCharCode.apply(null, b.subarray(i, i + 4096)); return G.btoa(s); };
  const fromB64 = (s) => { const t = G.atob(s); const o = new Uint8Array(t.length); for (let i = 0; i < t.length; i++) o[i] = t.charCodeAt(i); return o; };
  // Max bytes handed to a single native write(); the kernel socket buffer is
  // ~64 KB anyway, so encoding more than this per call is pure waste.
  const WCHUNK = 65536;
  // ref bun src/http/Decompressor.rs: decode response content-encoding
  // (gzip/deflate/br/zstd; identity/unknown = passthrough). zstd multi-frame is
  // handled inside the native decoder (modules/compress/src/zstd.cppm loops).
  const decodeCE = (bytes, enc) => {
    const ZN = G.__mbunZlibNative;
    if (!ZN || !enc) return bytes;
    const b64 = toB64(bytes);
    let out;
    if (enc === "gzip" || enc === "x-gzip") out = ZN.decompress(b64, "gzip", 15);
    else if (enc === "deflate") { try { out = ZN.decompress(b64, "zlib", 15); } catch (e) { out = ZN.decompress(b64, "raw", 15); } }
    else if (enc === "br") out = ZN.brotliDecompress(b64);
    else if (enc === "zstd") out = ZN.zstdDecompress(b64);
    else return bytes;
    return fromB64(out);
  };
  const latin1 = (b, from, to) => { let s = ""; for (let i = from; i < to; i += 4096) s += String.fromCharCode.apply(null, b.subarray(i, Math.min(i + 4096, to))); return s; };
  const concatU8 = (list) => { let total = 0; for (const c of list) total += c.length; const out = new Uint8Array(total); let o = 0; for (const c of list) { out.set(c, o); o += c.length; } return out; };
  const mkErr = (msg, code) => { const e = new Error(msg); e.code = code; return e; };
  // The reactor's natives report failures as messages, not errno numbers. Any
  // errno name the message carries is the socket error node would surface; the
  // ECONNRESET fallback keeps the previous behaviour for unrecognised strings.
  const ERRNO_RE = /\b(ECONNREFUSED|ECONNRESET|ECONNABORTED|EPIPE|ENOENT|EACCES|EPERM|EADDRINUSE|EADDRNOTAVAIL|EAFNOSUPPORT|EHOSTUNREACH|ENETUNREACH|ENETDOWN|ETIMEDOUT|EINVAL|ENAMETOOLONG|EISDIR|ENOTDIR|ELOOP|EMFILE|ENFILE|ENOTSOCK|EAI_AGAIN|EAI_FAIL|ENOTFOUND)\b/;
  const codeOf = (e, fallback) => { const m = String((e && e.message) || e); const hit = ERRNO_RE.exec(m); return hit ? hit[1] : (fallback || "ECONNRESET"); };
  // A Permission Model refusal is NOT a connect/listen failure to be re-spelled
  // as `connect ECONNRESET host:port`: node raises ERR_ACCESS_DENIED from
  // tcp_wrap/pipe_wrap and it reaches the caller verbatim, carrying .permission
  // and .resource. Re-wrapping it would erase both and report a network error
  // for something the sandbox refused.
  const isAccessDenied = (e) => !!(e && e.code === "ERR_ACCESS_DENIED");
  const connectError = (nativeError, host, port) => {
    if (isAccessDenied(nativeError)) {
      // node internal/errors.js ExceptionWithHostPort, permission branch:
      // `connect ERR_ACCESS_DENIED <the denial message>`, with syscall/address/
      // port attached and the code kept. `.permission`/`.resource` are carried
      // over too — node drops them here, and keeping them is strictly more
      // diagnostic information on an error nothing asserts the absence of.
      const err = mkErr("connect ERR_ACCESS_DENIED " + String(nativeError.message),
                        "ERR_ACCESS_DENIED");
      err.syscall = "connect"; err.address = host;
      if (port !== undefined) err.port = port;
      err.permission = nativeError.permission; err.resource = nativeError.resource;
      return err;
    }
    const code = codeOf(nativeError);
    // node formats a pipe/unix connect as `connect <code> <path>` — no port.
    const error = mkErr("connect " + code + " " + host + (port === undefined ? "" : ":" + port), code);
    error.syscall = "connect"; error.address = host; if (port !== undefined) error.port = port;
    return error;
  };
  // A unix socket path has to fit in sockaddr_un.sun_path (108 bytes incl. NUL);
  // libuv reports a longer one as EINVAL rather than truncating it, and the
  // natives here would otherwise fail with an unrecognised message.
  const PIPE_PATH_MAX = 107;
  const pipePathTooLong = (p) => (G.Buffer ? G.Buffer.byteLength(p) : p.length) > PIPE_PATH_MAX;
  // node lib/net.js Server: a bind failure carries the requested address/port and
  // syscall so `err.address`/`err.port` are usable (test-net-better-error-messages-*).
  const listenError = (nativeError, address, port) => {
    if (isAccessDenied(nativeError)) return nativeError;
    // A bind failure the natives describe without an errno ("Is port N in use?")
    // is the address-in-use case, which is what the previous hard-coded value
    // covered — keep it as the fallback so EADDRINUSE detection is unchanged.
    const code = codeOf(nativeError, "EADDRINUSE");
    const error = mkErr("listen " + code + " " + address + (port === undefined ? "" : ":" + port), code);
    error.syscall = "listen"; error.address = address; if (port !== undefined) error.port = port;
    return error;
  };

  // ---- reactor: pollables driven by the C++ event-loop pump ------------------
  // pending counts bounded in-flight operations that MUST make progress for the
  // program to advance (currently fetch). Long-lived listeners are deliberately
  // not counted here: test_runner uses pending to decide whether a single test
  // can time out. wait() blocks ≤2ms per idle drain so we never busy-spin.
  //
  // handles is the separate loop-ref channel that ref/unref act on: the count of
  // ref'd, open node handles (net.Server, net.Socket, dgram.Socket, and the
  // http/http2/tls layers built on them). The C++ pump keeps running while it is
  // non-zero, so `server.unref()` really does let the process leave — it used to
  // be a no-op that still pinned the loop, which hung 580 corpus files.
  const NET = (G.__mbunNet = { pending: 0, items: new Set(), stall: 0, serveActive: 0, gen: 0, handles: 0 });
  // A handle's loop reference. `refd` is the user's intent (sticky across
  // open/close, as node's uv_ref/uv_unref flag is), `held` whether the count
  // currently carries this handle.
  NET.hold = (h) => { if (!h._held && h._refd !== false && h._loopOpen) { h._held = true; NET.handles++; } };
  NET.release = (h) => { if (h._held) { h._held = false; NET.handles = Math.max(0, NET.handles - 1); } };
  G.__mbunNetDrain = function () {
    if (!NN) return 0;
    let total = 0;
    // A fresh generation per drain invocation. A socket whose TLS handshake just
    // completed records this generation and skips reading application data for
    // the rest of this drain, so 'secureConnect' lands on its own turn: the
    // user's secureConnect continuation (its write()/end()) runs when microtasks
    // drain after this call returns, BEFORE the peer's first record/FIN is read
    // next generation. Mirrors node, where handshake completion and the first
    // 'data'/'end' are distinct event-loop ticks (a TLS1.2 server that FINs one
    // flight early otherwise trips ERR_STREAM_WRITE_AFTER_END on that write).
    NET.gen = (NET.gen + 1) | 0;
    for (let pass = 0; pass < 16; pass++) {
      let progress = 0;
      for (const it of Array.from(NET.items)) {
        try { progress += it._poll() | 0; }
        catch (e) {
          // A poll is a node callback boundary: the native reads below already
          // catch their own errors and route them to _fail, so what escapes here
          // is a user listener throwing — node's 'uncaughtException', not a
          // socket error. Only a bounded in-flight op (fetch) keeps the legacy
          // rejection path, where the throw belongs to the operation.
          if (it._pendingOp && it._fail) { NET.items.delete(it); try { it._fail(e); } catch (e2) {} }
          else if (!(G.__mbun_uncaught && G.__mbun_uncaught(e))) { NET.items.delete(it); NET.release(it); }
        }
      }
      total += progress;
      if (!progress) break;
    }
    if (total > 0) { NET.stall = 0; }
    else if (NET.pending > 0) {
      // Wedged ops are declared on a wall clock, not on a pass count. A pass
      // only costs ~2ms when NN.wait(2) is the idle path; with Bun.serve active
      // the idle path is SN.tick(2), which returns as soon as its own throwaway
      // timer fires, so an iteration budget meant to span 5s could burn through
      // in ~30ms and time out a request that was merely waiting on a slow peer.
      // NET.stall is the timestamp the stall began (0 = making progress).
      if (!NET.stall) NET.stall = Date.now();
      if (Date.now() - NET.stall > 5000) {  // 5s with zero progress → wedged
        NET.stall = 0;
        for (const it of Array.from(NET.items)) {
          if (it._pendingOp && it._fail) { try { it._fail(mkErr("The socket connection timed out", "ETIMEDOUT")); } catch (e) {} NET.items.delete(it); }
        }
      } else if (SN && NET.serveActive > 0) { try { SN.tick(2); } catch (e) {} }
      else { NN.wait(2); }
    }
    return total;
  };

  if (!NN) return;  // natives unavailable (Windows): keep the honest stubs

  // ---- node:net — real Socket / Server over the reactor ----------------------
  const HWM = 64 * 1024;
  // node net.js Happy-Eyeballs default (getDefault/setDefaultAutoSelectFamily*).
  // mbun's connect is single-stack so the value is advisory, but the getter/
  // setter contract (default 2500, floor 10, positive-int validation) is tested.
  let autoSelectFamilyDefault = false;
  // node's initial default is 500ms; the test harness (common/index.js) reads it,
  // multiplies by 5 and re-sets it (→ 2500), which is what tests assert against.
  let autoSelectFamilyAttemptTimeoutDefault = 500;
  class Socket extends EE {
    constructor(opts) {
      super();
      opts = opts || {};
      this._fd = -1; this._wq = []; this._wqLen = 0; this._needDrain = false;
      this._shutW = false; this._shutSent = false; this._eof = false; this._closeEmitted = false;
      this._paused = false; this._enc = null;
      this._onread = opts.onread && typeof opts.onread.callback === "function" ? opts.onread.callback : null;
      this.destroyed = false; this.connecting = false; this.readable = true; this.writable = true; this.pending = true;
      // Loop-reference state (see NET.hold): sticky intent + current hold.
      this._refd = true; this._held = false; this._loopOpen = false;
      this.allowHalfOpen = !!opts.allowHalfOpen;
      this.remoteAddress = "127.0.0.1"; this.remoteFamily = "IPv4"; this.remotePort = 0;
      this.localAddress = "127.0.0.1"; this.localPort = 0;
      this.bytesRead = 0; this.bytesWritten = 0;
      // Minimal node stream.Readable state. This transport pushes straight to
      // 'data' rather than running the Readable machinery, but consumers of a
      // *socket* legitimately read it: npm `ws` socketOnClose gates its final
      // drain on `socket._readableState.endEmitted` and then `socket.read()`.
      this._hwm = typeof opts.highWaterMark === "number" ? opts.highWaterMark : HWM;
      this._readableState = { endEmitted: false, ended: false, destroyed: false, length: 0, flowing: true, readable: true, objectMode: false, highWaterMark: this._hwm };
      // node:http's OutgoingMessage.end() sets _writableState.corked before its
      // final uncork(); keep the cork counter in one place.
      this._corked = 0;
      this._writableState = { corked: 0, ended: false, finished: false, destroyed: false, length: 0, objectMode: false, highWaterMark: this._hwm };
    }
    _adopt(fd) {
      // A descriptor received over IPC (SCM_RIGHTS) never went through
      // accept()/connect(), so the reactor's poll set does not know it and the
      // pump would park without watching it. Idempotent for our own fds.
      if (NN && NN.track) { try { NN.track(fd); } catch (e) {} }
      this._fd = fd; this.pending = false; this.destroyed = false; this.connecting = false;
      this.readable = true; this.writable = true;
      this._shutW = false; this._shutSent = false; this._eof = false; this._closeEmitted = false;
      NET.items.add(this);
      // An open socket holds the event loop open (node: uv_tcp_t is ref'd until
      // it closes), unless the user unref'd it. Without this the pump could
      // decide the loop was idle while a request was still in flight.
      this._loopOpen = true; NET.hold(this);
      return this;
    }
    connect(...a) {
      // node Socket.prototype.connect announces the socket on
      // 'net.client.socket' before it does anything else with the arguments, so
      // every creation path (net.connect / net.createConnection / new
      // net.Socket().connect / tls.connect) reports. node's TLSSocket *is* the
      // connecting socket; mbun's wraps a hidden transport Socket, so tls.connect
      // points `_dcClientSocket` at the TLSSocket the caller actually holds
      // (test-diagnostics-channel-net-client-socket-tls asserts
      // `socket instanceof tls.TLSSocket`).
      if (netClientSocketChannel.hasSubscribers) netClientSocketChannel.publish({ socket: this._dcClientSocket || this });
      // ---- node net.js argument validation (Socket.prototype.connect) ----
      const nErr = (Ctor, code, msg) => { const e = new Ctor(msg); e.code = code; return e; };
      const optArg = (typeof a[0] === "object" && a[0] !== null && !Array.isArray(a[0])) ? a[0] : null;
      if (optArg) {
        if (optArg.objectMode)
          throw nErr(TypeError, "ERR_INVALID_ARG_VALUE", "The property 'options.objectMode' is not supported. Received " + (typeof optArg.objectMode === "string" ? "'" + optArg.objectMode + "'" : String(optArg.objectMode)));
        if (optArg.readableObjectMode || optArg.writableObjectMode) {
          const k = optArg.readableObjectMode ? "readableObjectMode" : "writableObjectMode";
          throw nErr(TypeError, "ERR_INVALID_ARG_VALUE", "The property 'options." + k + "' is not supported. Received " + (typeof optArg[k] === "string" ? "'" + optArg[k] + "'" : String(optArg[k])));
        }
        if (optArg.host !== undefined && typeof optArg.host !== "string")
          throw nErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "options.host" argument must be of type string. Received ' + (optArg.host === null ? "null" : Array.isArray(optArg.host) ? "an instance of Array" : "type " + typeof optArg.host));
        if (optArg.path == null && optArg.port === undefined && optArg.fd === undefined)
          throw nErr(TypeError, "ERR_MISSING_ARGS", 'The "options" or "port" or "path" argument must be specified');
        // node net.js: a pipe connect runs validateString(path, 'options.path').
        if (optArg.path != null && typeof optArg.path !== "string")
          throw nErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "options.path" argument must be of type string. Received ' +
            (Array.isArray(optArg.path) ? "an instance of Array" : typeof optArg.path === "object" ? "an instance of " + ((optArg.path.constructor && optArg.path.constructor.name) || "Object") : "type " + typeof optArg.path));
        // node net.js lookupAndConnect validation (options form). Types match
        // validateBoolean / validateInt32(min 1) / isIP / validateNumber /
        // ERR_INVALID_ARG_TYPE + validatePort, in node's order.
        if (optArg.autoSelectFamily !== undefined && typeof optArg.autoSelectFamily !== "boolean")
          throw nErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "options.autoSelectFamily" argument must be of type boolean. Received ' + (typeof optArg.autoSelectFamily === "string" ? "type string ('" + optArg.autoSelectFamily + "')" : "type " + typeof optArg.autoSelectFamily));
        if (optArg.autoSelectFamilyAttemptTimeout !== undefined) {
          const t = optArg.autoSelectFamilyAttemptTimeout;
          if (typeof t !== "number" || !Number.isInteger(t) || t < 1)
            throw nErr(RangeError, "ERR_OUT_OF_RANGE", 'The value of "options.autoSelectFamilyAttemptTimeout" is out of range. It must be a positive integer greater than 0. Received ' + String(t));
        }
        if (optArg.localAddress != null && !isIP(optArg.localAddress))
          throw nErr(TypeError, "ERR_INVALID_IP_ADDRESS", "Invalid IP address: " + String(optArg.localAddress));
        if (optArg.localPort != null && typeof optArg.localPort !== "number")
          throw nErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "options.localPort" argument must be of type number. Received ' + (typeof optArg.localPort === "string" ? "type string ('" + optArg.localPort + "')" : "type " + typeof optArg.localPort));
        if (optArg.lookup != null && typeof optArg.lookup !== "function")
          throw nErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "options.lookup" argument must be of type function. Received ' + (typeof optArg.lookup === "object" ? "an instance of " + ((optArg.lookup && optArg.lookup.constructor && optArg.lookup.constructor.name) || "Object") : "type " + typeof optArg.lookup + " (" + String(optArg.lookup) + ")"));
        if (optArg.hints !== undefined) {
          const dnsMod = M["dns"] || M["node:dns"] || {};
          const validMask = (dnsMod.ADDRCONFIG || 0) | (dnsMod.V4MAPPED || 0) | (dnsMod.ALL || 0);
          const hints = optArg.hints;
          if (typeof hints !== "number" || (validMask !== 0 && (hints & ~validMask) !== 0))
            throw nErr(TypeError, "ERR_INVALID_ARG_VALUE", "The argument 'hints' is invalid. Received " + String(hints));
        }
        if (optArg.port !== undefined) {
          const p = optArg.port;
          if (typeof p !== "number" && typeof p !== "string")
            throw nErr(TypeError, "ERR_INVALID_ARG_TYPE", 'The "options.port" argument must be one of type number or string. Received ' + (p === null ? "null" : Array.isArray(p) ? "an instance of Array" : typeof p === "object" ? "an instance of " + ((p.constructor && p.constructor.name) || "Object") : "type " + typeof p + " (" + String(p) + ")"));
          if ((typeof p === "string" && p.trim().length === 0) || +p !== (+p >>> 0) || +p > 0xFFFF) {
            const e = new RangeError('options.port should be >= 0 and < 65536. Received ' + String(p));
            e.code = "ERR_SOCKET_BAD_PORT"; throw e;
          }
        }
      } else if (a.length === 0 || a[0] === undefined || a[0] === null) {
        throw nErr(TypeError, "ERR_MISSING_ARGS", 'The "options" or "port" or "path" argument must be specified');
      }
      let port = 0, host = "localhost", cb = null, unixPath = null;
      if (typeof a[0] === "object" && a[0] !== null) { unixPath = a[0].path ? String(a[0].path) : null; port = a[0].port | 0; host = a[0].host || "localhost"; cb = typeof a[1] === "function" ? a[1] : null; }
      // node normalizeArgs/isPipeName: a non-numeric string first argument is a
      // pipe/unix path, not a port.
      else if (typeof a[0] === "string" && !/^\d+$/.test(a[0].trim())) { unixPath = a[0]; cb = typeof a[1] === "function" ? a[1] : null; }
      else { port = +a[0] | 0; if (typeof a[1] === "string") { host = a[1]; cb = typeof a[2] === "function" ? a[2] : null; } else if (typeof a[1] === "function") cb = a[1]; }
      if (cb) this.once("connect", cb);
      this.connecting = true;
      const self = this;
      // node net.js blockList / custom-lookup pre-connect resolution. Isolated to
      // the case where the caller actually passes options.blockList or
      // options.lookup, so the normal (http/tls/numeric-host) path below is
      // byte-for-byte unchanged. A resolved address that the BlockList rejects is
      // ERR_IP_BLOCKED; a lookup that yields a non-4/6 family is
      // ERR_INVALID_ADDRESS_FAMILY (both surfaced on 'error', as in node).
      const _blockList = optArg && optArg.blockList;
      const _lookup = optArg && optArg.lookup;
      const _famOf = (v) => isIPv6(v) ? 6 : isIPv4(v) ? 4 : 0;
      if (!unixPath && ((_blockList && _famOf(host)) || _lookup)) {
        const failWith = (err) => { self.connecting = false; G.queueMicrotask(() => { self.emit("error", err); self.destroy(); }); return self; };
        const dialResolved = (addr, fam) => {
          if (fam !== 4 && fam !== 6) { const e = mkErr("Invalid address family: " + fam + " " + host + ":" + port, "ERR_INVALID_ADDRESS_FAMILY"); e.host = host; e.port = port; return failWith(e); }
          if (_blockList && _blockList.check(addr, fam === 6 ? "ipv6" : "ipv4")) return failWith(mkErr("IP is blocked by net.BlockList", "ERR_IP_BLOCKED"));
          const dh = (addr === "::1" || addr === "::" || addr === "::0") ? "127.0.0.1" : addr;
          let fd2;
          try { fd2 = NN.connect(dh, port); }
          catch (e) { self.connecting = false; const err = connectError(e, addr, port); G.queueMicrotask(() => { if (self.destroyed) return; self.emit("error", err); self.destroy(); }); return self; }
          self._adopt(fd2); self.remotePort = port;
          self.connecting = true;
          G.queueMicrotask(() => { if (self.destroyed) { self.connecting = false; return; } self.connecting = false; self.emit("connect"); self.emit("ready"); });
          return self;
        };
        const hf = _famOf(host);
        if (hf) return dialResolved(host, hf);
        // host needs resolution — drive the caller-supplied lookup (node passes
        // { family, hints, all }; all is set under autoSelectFamily).
        const lopts = { family: optArg.family || 0, hints: optArg.hints || 0, all: !!optArg.autoSelectFamily };
        try {
          _lookup(host, lopts, (err, address, family) => {
            if (err) { if (!err.code) err.code = "ENOTFOUND"; return failWith(err); }
            // node lookupAndConnect: `all` selects the array form; otherwise the
            // callback must hand back a plain IP string, and anything else is
            // ERR_INVALID_IP_ADDRESS.
            if (lopts.all && Array.isArray(address)) {
              if (!address.length) { const e = mkErr("getaddrinfo ENOTFOUND " + host, "ENOTFOUND"); e.host = host; e.port = port; return failWith(e); }
              return dialResolved(address[0].address, address[0].family);
            }
            if (typeof address !== "string" || !_famOf(address)) {
              const e = mkErr("Invalid IP address: " + String(address), "ERR_INVALID_IP_ADDRESS");
              return failWith(e);
            }
            return dialResolved(address, family);
          });
        } catch (e) { return failWith(e instanceof Error ? e : mkErr(String(e), "ERR_INVALID_ARG_TYPE")); }
        return this;
      }
      // The reactor's native connect is IPv4 (net.inc net_parse_addr); an IPv6
      // loopback/wildcard (as reported by an IPv6-defaulted server.address())
      // dials the v4 loopback, which the v4-mapped INADDR_ANY listener accepts.
      const dialHost = (host === "::1" || host === "::" || host === "::0") ? "127.0.0.1" : host;
      let fd;
      try { if (unixPath && pipePathTooLong(unixPath)) throw new Error("EINVAL"); fd = unixPath ? NN.connectUnix(unixPath) : NN.connect(dialHost, port); }
      catch (e) { this.connecting = false; const err = connectError(e, unixPath || host, unixPath ? undefined : port); G.queueMicrotask(() => { if (this.destroyed) return; this.emit("error", err); this.destroy(); }); return this; }
      this._adopt(fd);
      this.remotePort = port;
      // node reports `connecting === true` from the moment connect() returns
      // until the 'connect' event fires; _adopt cleared it because the reactor's
      // connect() already completed synchronously. A destroy() in between must
      // cancel the pending 'connect' rather than resurrect the socket.
      this.connecting = true;
      G.queueMicrotask(() => { if (this.destroyed) { this.connecting = false; return; } this.connecting = false; this.emit("connect"); this.emit("ready"); });
      return this;
    }
    setEncoding(enc) { this._enc = enc || "utf8"; return this; }
    // node net.Socket.setTimeout: an idle timer that emits 'timeout' after
    // `ms` with no read/write activity (0 clears it). Node does NOT destroy the
    // socket on timeout — the listener decides. Re-armed on every I/O below.
    setTimeout(ms, cb) {
      ms = ms | 0;
      if (typeof cb === "function") this.once("timeout", cb);
      this._timeoutMs = ms;
      // node net.Socket#setTimeout publishes the interval as `socket.timeout`
      // (0/undefined once cleared), which tls.connect({ timeout }) then reports.
      this.timeout = ms === 0 ? undefined : ms;
      this._armTimeout();
      return this;
    }
    _armTimeout() {
      if (this._timeoutTimer) { G.clearTimeout(this._timeoutTimer); this._timeoutTimer = null; }
      if (this._timeoutMs > 0 && !this.destroyed) {
        this._timeoutTimer = G.setTimeout(() => { this._timeoutTimer = null; if (!this.destroyed) this.emit("timeout"); }, this._timeoutMs);
        // node arms socket timeouts on the handle's own timer list, so an idle
        // (agent-pooled, unref'd) socket's timeout never keeps the process
        // alive by itself -- the socket handle does. A ref'd G.setTimeout here
        // kept every pooled connection's 5s timer holding the loop open.
        if (this._timeoutTimer && typeof this._timeoutTimer.unref === "function") this._timeoutTimer.unref();
      }
    }
    setNoDelay() { return this; }
    // node net.Socket#setKeepAlive(enable, initialDelayMs): SO_KEEPALIVE plus
    // TCP_KEEPIDLE (seconds). The agent arms this on every pooled socket.
    setKeepAlive(enable, initialDelay) {
      const on = enable === undefined ? true : !!enable;
      if (this._fd >= 0 && NN && NN.setSockBuf) {
        const secs = on ? Math.max(1, Math.round((+initialDelay || 0) / 1000) || 60) : 0;
        try { NN.setSockBuf(this._fd, 3, secs); } catch (e) {}
      }
      return this;
    }
    // This transport writes through immediately, so corking is bookkeeping
    // only -- but node:http's OutgoingMessage reads writableCorked and pokes
    // _writableState.corked on end(), so both must exist and stay consistent.
    cork() { this._corked = (this._corked | 0) + 1; if (this._writableState) this._writableState.corked = this._corked; return this; }
    uncork() { if (this._corked > 0) this._corked--; if (this._writableState) this._writableState.corked = this._corked; return this; }
    get writableCorked() { return this._corked | 0; }
    // node honours the `highWaterMark` construction option on both sides of the
    // duplex (net.Socket passes it straight to stream.Duplex), so tls.connect
    // ({ highWaterMark }) is observable on the socket it creates.
    get writableHighWaterMark() { return this._hwm; }
    get readableHighWaterMark() { return this._hwm; }
    // node net.Socket#bufferSize: how much this socket still has queued to write.
    get bufferSize() { return this.writableLength; }
    // node net.Socket#ref/unref: sticky user intent over the handle's loop
    // reference. These were no-ops, so an unref'd keep-alive/agent socket still
    // pinned the process.
    ref() { this._refd = true; NET.hold(this); return this; }
    unref() { this._refd = false; NET.release(this); return this; }
    hasRef() { return this._refd !== false; }
    // stream.Readable#read: nothing is buffered by this transport (chunks go
    // straight out as 'data'), except bytes handed back through unshift().
    read(n) {
      const q = this._unshiftQ;
      if (q && q.length) { this._unshiftQ = null; return q.length === 1 ? q[0] : (G.Buffer ? G.Buffer.concat(q) : q[0]); }
      return null;
    }
    pause() { this._paused = true; return this; }
    resume() { this._paused = false; return this; }
    // stream.Readable#pipe / #unpipe. node's net.Socket is a Duplex, so every
    // consumer that forwards a socket somewhere else uses pipe() — including
    // the corpus' own echo fixture (test/fixtures/tls-connect.js does
    // `conn.pipe(conn)`), which a missing pipe turned into a TypeError before
    // the test's real assertions ever ran. This transport is already in flowing
    // mode ('data' is emitted as bytes arrive), so pipe is the flowing-mode
    // forwarder: write each chunk, honour the destination's backpressure by
    // pausing until it drains, and end the destination on 'end' unless
    // { end: false }. Errors are NOT forwarded (node parity).
    pipe(dest, options) {
      if (!dest || typeof dest.write !== "function") throw new TypeError("The \"destination\" argument must be a writable stream");
      const src = this;
      const endDest = !(options && options.end === false);
      // Self-pipe (an echo server) must not apply backpressure to itself:
      // pausing the read side is what would let the write queue drain.
      const selfPipe = dest === src;
      const onData = (chunk) => {
        const ok = dest.write(chunk);
        if (ok === false && !selfPipe) {
          src.pause();
          dest.once("drain", () => { if (!src.destroyed) src.resume(); });
        }
      };
      const onEnd = () => { if (endDest && typeof dest.end === "function") { try { dest.end(); } catch (e) {} } };
      const entry = { dest, onData, onEnd };
      if (!this._pipes) this._pipes = [];
      this._pipes.push(entry);
      src.on("data", onData);
      src.on("end", onEnd);
      try { if (typeof dest.emit === "function") dest.emit("pipe", src); } catch (e) {}
      if (this._paused) this.resume();
      return dest;
    }
    unpipe(dest) {
      const pipes = this._pipes;
      if (!pipes || !pipes.length) return this;
      const keep = [];
      for (const p of pipes) {
        if (dest !== undefined && p.dest !== dest) { keep.push(p); continue; }
        this.removeListener("data", p.onData);
        this.removeListener("end", p.onEnd);
        try { if (p.dest && typeof p.dest.emit === "function") p.dest.emit("unpipe", this); } catch (e) {}
      }
      this._pipes = keep;
      return this;
    }
    // stream.Readable#unshift: push bytes back to the front of the read queue.
    // node's HTTP client hands the bytes that followed a 101 header to the
    // 'upgrade' listener, and every upgrade consumer (npm `ws`
    // websocket.js setSocket) unshifts them so its own 'data' handler sees
    // them ahead of anything still on the wire. Ordering is preserved by
    // draining this queue before any freshly-read chunk (see _drain).
    unshift(chunk) {
      if (chunk == null) return true;
      const b = typeof chunk === "string"
        ? (G.Buffer ? G.Buffer.from(chunk, this._enc || "utf8") : chunk)
        : (G.Buffer ? G.Buffer.from(chunk) : chunk);
      if (!b.length) return true;
      if (!this._unshiftQ) this._unshiftQ = [];
      this._unshiftQ.unshift(b);
      if (!this._unshiftPending) {
        this._unshiftPending = true;
        G.queueMicrotask(() => { this._unshiftPending = false; this._flushUnshift(); });
      }
      return true;
    }
    // stream.Readable#pipe over this transport: node's net.Socket is a Duplex,
    // and upgrade handlers routinely `socket.pipe(socket)`.
    pipe(dest, opts) {
      const onData = (c) => { const ok = dest.write(c); if (ok === false && this.pause) this.pause(); };
      const onDrain = () => { if (this.resume) this.resume(); };
      const onEnd = () => { if (!opts || opts.end !== false) { try { dest.end(); } catch (e) {} } };
      this.on("data", onData);
      if (dest.on) dest.on("drain", onDrain);
      this.on("end", onEnd);
      this.resume();
      if (dest.emit) dest.emit("pipe", this);
      return dest;
    }
    // stream.Duplex#push: feed bytes to this socket's readable side. node's
    // net.Socket is a Duplex, and http tests drive fake/broken responses by
    // pushing straight into it (test-http-client-read-in-error,
    // test-http-header-overflow).
    push(chunk, enc) {
      if (chunk === null || chunk === undefined) {
        if (!this._eof) { this._eof = true; this.readable = false; this._readableState.endEmitted = true; this.emit("end"); }
        return false;
      }
      const b = typeof chunk === "string"
        ? (G.Buffer ? G.Buffer.from(chunk, enc || this._enc || "utf8") : chunk)
        : (G.Buffer ? G.Buffer.from(chunk) : chunk);
      if (!b.length) return true;
      this.bytesRead += b.length;
      if (this._onread) this._onread(b.length, b);
      else this.emit("data", this._enc && G.Buffer ? b.toString(this._enc) : b);
      return true;
    }
    _flushUnshift() {
      const q = this._unshiftQ;
      if (!q || !q.length) return;
      this._unshiftQ = null;
      for (const c of q) {
        if (this.destroyed) return;
        if (this._onread) this._onread(c.length, c);
        else this.emit("data", this._enc && G.Buffer ? c.toString(this._enc) : c);
      }
    }
    address() { return { port: this.localPort, address: this.localAddress, family: "IPv4" }; }
    get writableLength() { return this._wqLen; }
    get writableEnded() { return this._shutW; }
    get writableFinished() { return this._shutSent && this._wq.length === 0; }
    write(data, enc, cb) {
      if (typeof enc === "function") { cb = enc; enc = null; }
      // node stream.Writable chunk validation (test-net-write-arguments): only a
      // string / Buffer / TypedArray / DataView is a legal chunk; null is a
      // distinct ERR_STREAM_NULL_VALUES, everything else ERR_INVALID_ARG_TYPE.
      if (data === null) {
        const e = new TypeError("May not write null values to stream"); e.code = "ERR_STREAM_NULL_VALUES"; throw e;
      }
      if (typeof data !== "string" && !ArrayBuffer.isView(data) && !(data instanceof ArrayBuffer)) {
        const recv = data === undefined ? ". Received undefined"
          : (typeof data === "object")
            ? ". Received an instance of " + ((data && data.constructor && data.constructor.name) || "Object")
            : ". Received type " + typeof data + " (" + String(data) + ")";
        const e = new TypeError('The "chunk" argument must be of type string or an instance of Buffer, TypedArray, or DataView.' + recv);
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      if (this.destroyed || this._shutW) { const err = mkErr("write after end", "ERR_STREAM_WRITE_AFTER_END"); if (typeof cb === "function") G.queueMicrotask(() => cb(err)); else this.emit("error", err); return false; }
      const b = typeof data === "string" && enc && enc !== "utf8" && enc !== "utf-8" && G.Buffer ? u8(G.Buffer.from(data, enc)) : u8(data);
      // A zero-length chunk must never enter the queue: _flush() stops on the
      // first write() that reports 0 bytes, so an empty head parks every byte
      // behind it forever. node:http's end() flushes with `_send("")`, so this
      // wedged the *second* response on any keep-alive connection.
      if (b.length === 0) {
        if (typeof cb === "function") G.queueMicrotask(cb);
        return this._wqLen < HWM;
      }
      this._wq.push(b); this._wqLen += b.length; this.bytesWritten += b.length;
      if (this._timeoutMs) this._armTimeout();
      this._flush();
      if (typeof cb === "function") G.queueMicrotask(cb);
      if (this._wqLen >= HWM) { this._needDrain = true; return false; }
      return true;
    }
    end(data, enc, cb) {
      if (typeof data === "function") { cb = data; data = null; }
      if (typeof enc === "function") { cb = enc; enc = null; }
      if (data != null) this.write(data, enc);
      this._shutW = true; this.writable = false;
      if (typeof cb === "function") this.once("close", cb);
      this._flush();
      return this;
    }
    destroy(err) {
      if (this.destroyed) return this;
      this.destroyed = true; this.connecting = false; this.readable = false; this.writable = false;
      if (this._readableState) { this._readableState.destroyed = true; this._readableState.readable = false; }
      if (this._timeoutTimer) { G.clearTimeout(this._timeoutTimer); this._timeoutTimer = null; }
      if (this._fd >= 0) { try { NN.close(this._fd); } catch (e) {} this._fd = -1; }
      NET.items.delete(this);
      this._loopOpen = false; NET.release(this);
      if (err) this.emit("error", err);
      if (!this._closeEmitted) { this._closeEmitted = true; G.queueMicrotask(() => this.emit("close", !!err)); }
      return this;
    }
    destroySoon() { return this.destroy(); }
    resetAndDestroy() { return this.destroy(); }
    _fail(e) { this.destroy(e instanceof Error && e.code ? e : mkErr(String((e && e.message) || e), codeOf(e))); }
    // TLS mode (T-TLS.3): after _startTls, all IO rides the per-fd TLS channel
    // (_tls: 1 = handshaking, 2 = established). Reads/writes stay non-blocking.
    _startTls(o) {
      o = o || {};
      try {
        // o.verify may be a tri-state number (0 disabled / 1 required / 2
        // optional) or a bool (legacy fetch/https callers); coerce either.
        const vmode = typeof o.verify === "number" ? o.verify : (o.verify ? 1 : 0);
        NN.tlsWrap(this._fd, !!o.isServer, o.cert || "", o.key || "", o.servername || "",
                   vmode, o.ca || "", o.alpn || "", o.minVersion || "", o.maxVersion || "",
                   o.ciphers || "");
        this._tls = 1;
      } catch (e) { this._fail(e); }
      return this;
    }
    _flush() {
      if (this._fd < 0) return 0;
      if (this._tls === 1) return 0;  // handshake still in flight (see _poll)
      // A TLS upgrade is scheduled but _startTls has not run yet (js_tls_live
      // arms it on the transport's 'connect'). Anything queued in that window
      // belongs INSIDE the TLS record layer: flushing it as cleartext would put
      // the request bytes on the wire in front of the ClientHello. node:https
      // hits this window on every request, because ClientRequest writes its
      // header the moment tls.connect() returns the socket.
      if (this._tlsPending) return 0;
      let progress = 0;
      // Encode at most WCHUNK bytes per write(): base64-ing the WHOLE pending
      // buffer each pass is quadratic (the socket accepts ~64 KB, so a large
      // queued payload was re-encoded once per 64 KB — a multi-MB write blew up
      // in time and memory).
      while (this._wq.length) {
        const head = this._wq[0];
        if (head.length === 0) { this._wq.shift(); continue; }  // never stall on an empty chunk
        const piece = head.length > WCHUNK ? head.subarray(0, WCHUNK) : head;
        let n;
        try { n = this._tls ? NN.tlsWrite(this._fd, toB64(piece)) : NN.write(this._fd, toB64(piece)); }
        catch (e) { this._fail(e); return progress; }
        if (n <= 0) break;
        progress++;
        this._wqLen -= n;
        if (n < head.length) { this._wq[0] = head.subarray(n); if (n < piece.length) break; continue; }
        this._wq.shift();
      }
      if (!this._wq.length && this._shutW && !this._shutSent && this._fd >= 0) {
        this._shutSent = true;
        if (this._tls) { try { NN.tlsClose(this._fd); } catch (e) {} }
        try { NN.shutdown(this._fd); } catch (e) {}
        if (this._eof) this.destroy();
      }
      if (this._needDrain && this._wqLen === 0 && !this.destroyed) { this._needDrain = false; progress++; this.emit("drain"); }
      return progress;
    }
    _poll() {
      if (this.destroyed || this._fd < 0) { NET.items.delete(this); return 0; }
      if (this._tls === 1) {  // drive the TLS handshake before any app IO
        let st;
        try { st = NN.tlsStep(this._fd); } catch (e) { st = -1; }
        if (st < 0) {
          // Surface the handshake's own reason (SSL error code, or ECONNRESET on
          // a peer FIN before the handshake) so error.code matches node.
          let info = null;
          try { info = NN.tlsError(this._fd); } catch (e) {}
          this._fail(mkErr((info && info.message) || "TLS handshake failed",
                           (info && info.code) || "ERR_TLS_HANDSHAKE"));
          return 1;
        }
        if (st !== 1) return 0;
        this._tls = 2;
        this._hsGen = NET.gen;  // suppress appdata reads for the rest of this drain
        this.emit("secureConnect");
        return 1;               // secureConnect gets its own turn; read next drain
      }
      let progress = this._flush();
      // A socket that just finished its handshake this drain defers its first
      // appdata read to the next generation (see __mbunNetDrain): the user's
      // secureConnect continuation must be able to write() before the peer's
      // first record/FIN closes the write side.
      if (this._hsGen === NET.gen) return progress;
      if (!this._paused && !this._eof && !this.destroyed) {
        for (let i = 0; i < 64; i++) {
          let r;
          try { r = this._tls ? NN.tlsRead(this._fd) : NN.read(this._fd); }
          catch (e) { this._fail(e); return progress; }
          if (r === "") break;
          progress++;
          if (r === null) {
            this._eof = true; this.readable = false;
            this._readableState.endEmitted = true;
            this.emit("end");
            // node net.js onReadableStreamEnd auto-ends the write side on the
            // NEXT tick, not inline, so data written synchronously right after
            // 'end'/'secureConnect' (e.g. a TLS1.2 peer that FINs one flight
            // early) still flushes instead of hitting ERR_STREAM_WRITE_AFTER_END.
            if (!this.allowHalfOpen && !this._shutW) {
              G.queueMicrotask(() => {
                if (this.destroyed || this._shutW) return;
                this._shutW = true; this.writable = false; this._flush();
                if (this._eof && this._shutSent && this._wq.length === 0) this.destroy();
              });
            }
            if (this._shutSent && this._wq.length === 0) this.destroy();
            break;
          }
          const bytes = fromB64(r);
          this.bytesRead += bytes.length;
          if (this._timeoutMs) this._armTimeout();
          const chunk = G.Buffer ? G.Buffer.from(bytes) : bytes;
          if (this._unshiftQ) this._flushUnshift();   // unshifted bytes come first
          if (this._onread) this._onread(bytes.length, chunk);
          else this.emit("data", this._enc ? (G.Buffer ? chunk.toString(this._enc) : latin1(bytes, 0, bytes.length)) : chunk);
          if (this.destroyed || this._paused) break;
        }
      }
      if (this._eof && this._shutSent && this._wq.length === 0) this.destroy();
      return progress;
    }
  }
  // node keeps the pre-io.js `_connecting` name as an alias of `connecting`.
  Object.defineProperty(Socket.prototype, "_connecting", {
    get() { return this.connecting; },
    set(v) { this.connecting = v; },
    configurable: true,
  });
  Socket.prototype[Symbol.asyncDispose] = function () { this.destroy(); return Promise.resolve(); };
  Socket.prototype[Symbol.dispose] = function () { this.destroy(); };

  class Server extends EE {
    constructor(opts, cb) {
      super();
      if (typeof opts === "function") { cb = opts; opts = {}; }
      // node net.js Server: options must be an object (or a connectionListener
      // function, handled above). A string/number/etc. is ERR_INVALID_ARG_TYPE.
      else if (opts != null && typeof opts !== "object") {
        const e = new TypeError('The "options" argument must be of type object. Received ' +
          (typeof opts === "string" ? "type string (" + JSON.stringify(opts) + ")" : "type " + typeof opts + " (" + String(opts) + ")"));
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      this._opts = opts || {};
      // node net.Server publishes both construction options as own properties;
      // tls.Server inherits them through net.Server.call(this, options, …), and
      // test-tls-server-parent-constructor-options reads them directly.
      this.allowHalfOpen = !!this._opts.allowHalfOpen;
      this.pauseOnConnect = !!this._opts.pauseOnConnect;
      if (typeof cb === "function") this.on("connection", cb);
      this._fd = -1; this._addr = null; this.listening = false; this._conns = new Set();
      // node semantics: `_refd` is the sticky user intent (a handle unref'd
      // BEFORE listen() must not hold the loop once it starts listening —
      // `net.createServer().unref().listen()` used to hang forever because
      // unref() was a no-op on a not-yet-listening server and listen() then
      // took an unconditional hold); `_loopOpen` is whether the handle is live.
      this._refd = true; this._held = false; this._loopOpen = false;
    }
    _hold() { this._loopOpen = true; NET.hold(this); }
    _release() { this._loopOpen = false; NET.release(this); }
    listen(...a) {
      let port = 0, host = null, cb = null, unixPath = null;
      // node lib/internal/validators validatePort (allowZero): every listen form
      // routes its port through this, so an out-of-range value (e.g. -1>>>0) is a
      // RangeError ERR_SOCKET_BAD_PORT rather than a silent `| 0` truncation.
      const validateListenPort = (p) => {
        if (p == null) return;
        if ((typeof p !== "number" && typeof p !== "string") ||
            (typeof p === "string" && p.trim().length === 0) ||
            +p !== (+p >>> 0) || +p > 0xFFFF) {
          const e = new RangeError('options.port should be >= 0 and < 65536. Received ' + String(p));
          e.code = "ERR_SOCKET_BAD_PORT"; throw e;
        }
      };
      if (typeof a[0] === "object" && a[0] !== null && typeof a[0] !== "function") {
        const o = a[0];
        // node lib/net.js Server.listen: the options form must carry port/path/fd.
        if (o.port == null && o.path == null && o.fd == null && o.handle == null) {
          let recv; try { recv = JSON.stringify(o); } catch (e) { recv = String(o); }
          throw new Error('The argument \'options\' must have the property "port" or "path". Received ' + recv);
        }
        if (o.path != null) unixPath = String(o.path);
        if (o.port != null) { validateListenPort(o.port); port = o.port | 0; }
        if (o.host != null) host = String(o.host);
        if (o.exclusive != null) this._exclusive = !!o.exclusive;
        // node lib/net.js Server.listen: `reusePort` implies `exclusive`, so the
        // worker binds its own SO_REUSEPORT socket instead of asking the primary
        // (test-cluster-net-reuseport asserts cluster._getServer is NOT called).
        if (o.reusePort === true) { this._exclusive = true; this._reusePort = true; }
        if (o.backlog != null) this._backlog = o.backlog | 0;
        if (o.ipv6Only) this._ipv6Only = true;
        if (typeof a[1] === "function") cb = a[1];
        // node lib/net.js Server.listen: options.signal is validated up front and
        // closes the server when it aborts (an already-aborted signal closes on
        // the next tick, so the caller still sees a 'close').
        if (o.signal !== undefined && o.signal !== null) {
          const sig = o.signal;
          if (typeof sig !== "object" || typeof sig.addEventListener !== "function" || !("aborted" in sig)) {
            const e = new TypeError('The "options.signal" argument must be an instance of AbortSignal. Received ' +
              (typeof sig === "string" ? "type string ('" + sig + "')" : "type " + typeof sig));
            e.code = "ERR_INVALID_ARG_TYPE"; throw e;
          }
          if (sig.aborted) G.queueMicrotask(() => this.close());
          else {
            const onAborted = () => this.close();
            sig.addEventListener("abort", onAborted, { once: true });
            this.once("close", () => { try { sig.removeEventListener("abort", onAborted); } catch (e) {} });
          }
        }
      } else {
        // node Server.listen(path[, backlog][, cb]): a non-numeric string first
        // arg is a unix/pipe path; a numeric one is a TCP port.
        if (typeof a[0] === "number" || (typeof a[0] === "string" && /^\d+$/.test(a[0]))) { validateListenPort(a[0]); port = +a[0]; }
        else if (typeof a[0] === "string") unixPath = a[0];
        else if (typeof a[0] === "function") cb = a[0];
        for (let i = 1; i < a.length; i++) { if (typeof a[i] === "string") host = a[i]; else if (typeof a[i] === "function") cb = a[i]; }
      }
      // node Server.prototype.listen publishes the 'net.server.listen' tracing
      // channel's asyncStart with the NORMALIZED options object — for the
      // options form that is the caller's own object, so a subscriber sees any
      // extra property it carried (test-diagnostics-channel-net reads
      // `options.customOption`). asyncEnd follows a successful bind+listen and
      // error a failed one (both below, per bind path).
      if (netServerListen.hasSubscribers) {
        const dcOptions = (typeof a[0] === "object" && a[0] !== null && typeof a[0] !== "function")
          ? a[0]
          : (unixPath != null ? { path: unixPath } : (host != null ? { port, host } : { port }));
        netServerListen.asyncStart.publish({ server: this, options: dcOptions });
      }
      // node lib/net.js listenInCluster: inside a cluster worker the bind is
      // delegated to the primary (which owns the listening socket) unless the
      // caller asked for an exclusive one. `cluster._getServer` answers with
      // either a shared listening descriptor (SCHED_NONE / dgram) or a faux
      // round-robin handle that receives accepted descriptors over IPC.
      const clusterMod = G.__mbunCluster;
      if (clusterMod && clusterMod.isWorker && !this._exclusive &&
          typeof clusterMod._getServer === "function") {
        return this._listenInCluster(clusterMod, unixPath, host, port, cb);
      }
      if (unixPath) {
        this._unixPath = unixPath;
        if (cb) this.once("listening", cb);
        let ulh;
        try { if (pipePathTooLong(unixPath)) throw new Error("EINVAL"); ulh = NN.listenUnix(unixPath); }
        // node's pipe_wrap Bind raises ERR_ACCESS_DENIED synchronously, so
        // `assert.throws(() => server.listen(path))` sees it — an async 'error'
        // event would not be catchable there.
        catch (e) {
          if (isAccessDenied(e)) throw e;
          const err = listenError(e, unixPath);
          if (netServerListen.hasSubscribers) netServerListen.error.publish({ server: this, error: err });
          G.queueMicrotask(() => this.emit("error", err));
          return this;
        }
        this._fd = ulh.fd;
        if (netServerListen.hasSubscribers) netServerListen.asyncEnd.publish({ server: this });
        this._addr = { address: unixPath, family: "unix", port: 0 };
        this.listening = true;
        NET.items.add(this);
        this._hold();
        // node's bind/listen completes on a later loop turn, so a close() issued
        // before then cancels the pending 'listening' (and its callback) instead
        // of firing it against an already-closed server.
        G.queueMicrotask(() => { if (this.listening) this.emit("listening"); });
        return this;
      }
      // node default with no host is the IPv6 wildcard "::" (dual-stack), NOT
      // 0.0.0.0. mbun binds v4-compatible under the hood (net.inc is IPv4;
      // INADDR_ANY accepts v4-mapped v6 peers, v6 loopback maps to v4 loopback),
      // but the reported address/family stay the node-visible values — matching
      // the Bun.serve ":: default" convention already used above.
      if (host == null) host = "::";
      const isV6 = host.indexOf(":") !== -1;
      const bindHost = host === "::1" ? "127.0.0.1" : host;  // v6 loopback → v4 bind
      if (cb) this.once("listening", cb);
      let lh;
      try { lh = NN.listen(bindHost, port, !!this._reusePort); }
      catch (e) {
        if (isAccessDenied(e)) throw e;
        const err = listenError(e, host, port);
        if (netServerListen.hasSubscribers) netServerListen.error.publish({ server: this, error: err });
        G.queueMicrotask(() => this.emit("error", err));
        return this;
      }
      this._fd = lh.fd;
      if (netServerListen.hasSubscribers) netServerListen.asyncEnd.publish({ server: this });
      const reportAddr = host === "localhost" ? (isV6 ? "::1" : "127.0.0.1") : host;
      this._addr = { port: lh.port, address: reportAddr, family: isV6 ? "IPv6" : "IPv4" };
      // node net.js Server: `${addressType}:${address}:${requestedPort}` — the
      // port is the value passed to listen() (0 for an ephemeral bind), not the
      // kernel-assigned one (test-net-listen-invalid-port).
      this._connectionKey = (isV6 ? "6" : "4") + ":" + reportAddr + ":" + port;
      this.listening = true;
      NET.items.add(this);
      this._hold();
      G.queueMicrotask(() => { if (this.listening) this.emit("listening"); });
      return this;
    }
    // node lib/net.js Server._listen2 for a cluster worker: ask the primary for
    // the server, then adopt whichever kind of handle it answers with.
    //   * a handle carrying a real descriptor (SCHED_NONE / a shared listening
    //     socket) is bound straight onto this server's own accept loop;
    //   * the faux round-robin handle has no descriptor — connections arrive as
    //     `newconn` IPC frames and land in onconnection().
    _listenInCluster(clusterMod, unixPath, host, port, cb) {
      if (cb) this.once("listening", cb);
      const addressType = unixPath ? -1 : ((host || "").indexOf(":") !== -1 ? 6 : 4);
      const address = unixPath ? unixPath : (host == null ? (addressType === 6 ? "::" : "0.0.0.0") : host);
      const options = {
        address,
        port: unixPath ? -1 : port,
        addressType,
        // node passes `undefined` (not -1) when not listening on a descriptor;
        // the primary echoes it back in the 'listening' info object, which
        // test-cluster-basic reads as `hasOwn(info,'fd') && info.fd === undefined`.
        fd: undefined,
        flags: this._ipv6Only ? 1 : 0,
        backlog: this._backlog === undefined ? 511 : this._backlog,
      };
      clusterMod._getServer(this, options, (err, handle) => {
        if (err) {
          // node lib/net.js listenOnPrimaryHandle: `new ExceptionWithHostPort(
          // err, 'bind', address, port)` — syscall 'bind', not 'listen'.
          const code = typeof err === "string" ? err : "EADDRINUSE";
          const e = mkErr("bind " + code + " " + address + (unixPath ? "" : ":" + port), code);
          e.syscall = "bind"; e.address = address;
          if (!unixPath) e.port = port;
          this.emit("error", e);
          return;
        }
        this._clusterHandle = handle;
        handle.owner = this;
        if (typeof handle.fd === "number" && handle.fd >= 0) {
          // Shared descriptor: accept locally, exactly like a normal listen().
          if (NN && NN.track) { try { NN.track(handle.fd); } catch (e) {} }
          this._fd = handle.fd;
          const sn = handle.sockname;
          if (unixPath) this._addr = { address: unixPath, family: "unix", port: 0 };
          else this._addr = {
            port: sn && sn.port !== undefined ? sn.port : port,
            address: sn && sn.address !== undefined ? sn.address : address,
            family: sn && sn.family !== undefined ? sn.family : (addressType === 6 ? "IPv6" : "IPv4"),
          };
          this.listening = true;
          NET.items.add(this);
          this._hold();
        } else {
          handle.onconnection = (er, clientHandle) => {
            if (er || !clientHandle || typeof clientHandle.fd !== "number" || clientHandle.fd < 0) return;
            const sock = new Socket({ allowHalfOpen: !!this._opts.allowHalfOpen })._adopt(clientHandle.fd);
            sock.localPort = this._addr ? this._addr.port : 0;
            // node net.js onconnection: `socket.server` is the listener that
            // accepted it (and `_server` its internal alias).
            sock.server = this; sock._server = this;
            // node net.js onconnection: with pauseOnConnect the accepted socket
            // is handed to the listener already paused, so the consumer decides
            // when the first byte is read (it may pass the fd elsewhere first).
            if (this._opts.pauseOnConnect) sock.pause();
            this._conns.add(sock);
            sock.once("close", () => this._conns.delete(sock));
            this.emit("connection", sock);
            if (netServerSocketChannel.hasSubscribers) netServerSocketChannel.publish({ socket: sock });
          };
          const out = {};
          if (typeof handle.getsockname === "function") handle.getsockname(out);
          if (unixPath) this._addr = { address: unixPath, family: "unix", port: 0 };
          else this._addr = {
            port: out.port === undefined ? port : out.port,
            address: out.address === undefined ? address : out.address,
            family: out.family === undefined ? (addressType === 6 ? "IPv6" : "IPv4") : out.family,
          };
          this.listening = true;
        }
        G.queueMicrotask(() => { if (this.listening) this.emit("listening"); });
      });
      return this;
    }
    address() { return this._addr; }
    close(cb) {
      if (this._clusterHandle) {
        // The handle owns the descriptor (shared case) and the primary-side
        // bookkeeping (round-robin case); closing this._fd here too would be a
        // double close of a number the runtime may have already re-used.
        const h = this._clusterHandle; this._clusterHandle = null;
        this.listening = false;
        if (this._fd >= 0) { this._fd = -1; NET.items.delete(this); this._release(); }
        try { h.close(); } catch (e) {}
        if (typeof cb === "function") G.queueMicrotask(() => cb(null));
        G.queueMicrotask(() => this.emit("close"));
        return this;
      }
      if (this._fd >= 0) {
        try { NN.close(this._fd); } catch (e) {} this._fd = -1; NET.items.delete(this); this.listening = false;
        this._release();
        // node/libuv unlinks the unix socket file it created on close (skip
        // abstract sockets, leading NUL). Already-gone is fine.
        if (this._unixPath && this._unixPath[0] !== "\0") { try { (M["fs"] || M["node:fs"]).unlinkSync(this._unixPath); } catch (e) {} }
      }
      if (typeof cb === "function") G.queueMicrotask(() => cb(null));
      G.queueMicrotask(() => this.emit("close"));
      return this;
    }
    ref() {
      this._refd = true;
      // A round-robin server has no descriptor of its own: the faux handle's
      // ref()/unref() (a keep-alive interval) is what holds the worker's loop.
      if (this._clusterHandle && typeof this._clusterHandle.ref === "function") this._clusterHandle.ref();
      NET.hold(this);
      return this;
    }
    unref() {
      this._refd = false;
      if (this._clusterHandle && typeof this._clusterHandle.unref === "function") this._clusterHandle.unref();
      NET.release(this);
      return this;
    }
    hasRef() { return this._refd !== false; }
    setTimeout() { return this; }
    getConnections(cb) { if (typeof cb === "function") cb(null, this._conns.size); return this; }
    _poll() {
      if (this._fd < 0) { NET.items.delete(this); return 0; }
      let progress = 0;
      for (let i = 0; i < 64; i++) {
        let cfd;
        try { cfd = NN.accept(this._fd); } catch (e) { break; }
        if (cfd < 0) break;
        // cluster's round-robin primary owns the listening socket but never
        // builds a Socket for the connection: the raw descriptor is handed to a
        // worker over IPC instead (internal/cluster/round_robin_handle.js).
        if (this._rawAccept) { this._rawAccept(cfd); progress++; continue; }
        const sock = new Socket({ allowHalfOpen: !!this._opts.allowHalfOpen })._adopt(cfd);
        sock.localPort = this._addr ? this._addr.port : 0;
        sock.server = this; sock._server = this;
        if (this._opts.pauseOnConnect) sock.pause();
        this._conns.add(sock);
        sock.once("close", () => this._conns.delete(sock));
        progress++;
        this.emit("connection", sock);
        // node onconnection() publishes 'net.server.socket' right after the
        // 'connection' event.
        if (netServerSocketChannel.hasSubscribers) netServerSocketChannel.publish({ socket: sock });
      }
      return progress;
    }
  }
  Server.prototype[Symbol.asyncDispose] = function () { const s = this; return new Promise((res) => s.close(res)); };
  Server.prototype[Symbol.dispose] = function () { this.close(); };

  // ref: bun src/runtime/node/net/BlockList.rs and src/js/node/net.ts. Keep
  // addresses in network-order bytes so subnet checks do not depend on host
  // endianness and mapped-v4 normalization is allocation-free after parsing.
  function parseIPv4(value) {
    if (typeof value !== "string") return null;
    const parts = value.split(".");
    if (parts.length !== 4) return null;
    const out = new Uint8Array(4);
    for (let i = 0; i < 4; i++) {
      const part = parts[i];
      if (!/^(?:0|[1-9]\d{0,2})$/.test(part)) return null;
      const octet = +part;
      if (octet > 255) return null;
      out[i] = octet;
    }
    return out;
  }
  function parseIPv6(value) {
    if (typeof value !== "string" || value.length === 0 || value.indexOf("%") !== -1) return null;
    let source = value;
    const lastColon = source.lastIndexOf(":");
    if (source.indexOf(".") !== -1) {
      if (lastColon < 0) return null;
      const tail = parseIPv4(source.slice(lastColon + 1));
      if (!tail) return null;
      const high = ((tail[0] << 8) | tail[1]).toString(16);
      const low = ((tail[2] << 8) | tail[3]).toString(16);
      source = source.slice(0, lastColon + 1) + high + ":" + low;
    }
    const halves = source.split("::");
    if (halves.length > 2) return null;
    const splitHalf = (half) => half === "" ? [] : half.split(":");
    const left = splitHalf(halves[0]);
    const right = halves.length === 2 ? splitHalf(halves[1]) : [];
    if (halves.length === 1 && left.length !== 8) return null;
    const zeros = 8 - left.length - right.length;
    if (zeros < (halves.length === 2 ? 1 : 0)) return null;
    const groups = left.concat(new Array(zeros).fill("0"), right);
    if (groups.length !== 8) return null;
    const out = new Uint8Array(16);
    for (let i = 0; i < groups.length; i++) {
      if (!/^[0-9a-fA-F]{1,4}$/.test(groups[i])) return null;
      const group = parseInt(groups[i], 16);
      out[i * 2] = group >>> 8;
      out[i * 2 + 1] = group & 255;
    }
    return out;
  }
  // node lib/internal/net.js isIPv4/isIPv6 are regex tests: RegExp.test coerces
  // its argument with String(), so isIP(123) / isIP({toString}) match node's
  // stringifying behaviour, and the IPv6 form accepts a trailing %zone id
  // ([0-9a-zA-Z-.:], so "%eth0.0" is valid but "%eth0@1" is not).
  const v4Seg = "(?:25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])";
  const v4Str = "(?:" + v4Seg + "[.]){3}" + v4Seg;
  const IPv4Reg = new RegExp("^" + v4Str + "$");
  const v6Seg = "(?:[0-9a-fA-F]{1,4})";
  const IPv6Reg = new RegExp("^(?:" +
    "(?:" + v6Seg + ":){7}(?:" + v6Seg + "|:)|" +
    "(?:" + v6Seg + ":){6}(?:" + v4Str + "|:" + v6Seg + "|:)|" +
    "(?:" + v6Seg + ":){5}(?::" + v4Str + "|(?::" + v6Seg + "){1,2}|:)|" +
    "(?:" + v6Seg + ":){4}(?:(?::" + v6Seg + "){0,1}:" + v4Str + "|(?::" + v6Seg + "){1,3}|:)|" +
    "(?:" + v6Seg + ":){3}(?:(?::" + v6Seg + "){0,2}:" + v4Str + "|(?::" + v6Seg + "){1,4}|:)|" +
    "(?:" + v6Seg + ":){2}(?:(?::" + v6Seg + "){0,3}:" + v4Str + "|(?::" + v6Seg + "){1,5}|:)|" +
    "(?:" + v6Seg + ":){1}(?:(?::" + v6Seg + "){0,4}:" + v4Str + "|(?::" + v6Seg + "){1,6}|:)|" +
    "(?::(?:(?::" + v6Seg + "){0,5}:" + v4Str + "|(?::" + v6Seg + "){1,7}|:))" +
    ")(?:%[0-9a-zA-Z-.:]{1,})?$");
  const isIPv4 = (value) => IPv4Reg.test(value);
  const isIPv6 = (value) => IPv6Reg.test(value);
  const isIP = (value) => isIPv4(value) ? 4 : isIPv6(value) ? 6 : 0;
  const mappedV4 = (bytes) => {
    if (!bytes || bytes.length !== 16 || bytes[10] !== 255 || bytes[11] !== 255) return null;
    for (let i = 0; i < 10; i++) if (bytes[i] !== 0) return null;
    return bytes.subarray(12);
  };
  const mapV4 = (bytes) => {
    const out = new Uint8Array(16);
    out[10] = 255; out[11] = 255; out.set(bytes, 12);
    return out;
  };
  const parseAddress = (value, family) => {
    const normalized = family == null ? "ipv4" : String(family).toLowerCase();
    if (normalized === "ipv4" || normalized === "4") {
      const bytes = parseIPv4(value);
      return bytes ? { family: 4, bytes } : null;
    }
    if (normalized === "ipv6" || normalized === "6") {
      const bytes = parseIPv6(value);
      return bytes ? { family: 6, bytes } : null;
    }
    return null;
  };
  const bytesEqual = (a, b) => {
    if (!a || !b || a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
  };
  const bytesCompare = (a, b) => {
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
  };
  const addressForRule = (address, ruleFamily) => {
    if (address.family === ruleFamily) return address.bytes;
    if (ruleFamily === 6 && address.family === 4) return mapV4(address.bytes);
    if (ruleFamily === 4 && address.family === 6) return mappedV4(address.bytes);
    return null;
  };
  const prefixMatches = (address, network, prefix) => {
    const fullBytes = prefix >>> 3;
    const tailBits = prefix & 7;
    for (let i = 0; i < fullBytes; i++) if (address[i] !== network[i]) return false;
    if (tailBits === 0) return true;
    const mask = (255 << (8 - tailBits)) & 255;
    return (address[fullBytes] & mask) === (network[fullBytes] & mask);
  };

  class BlockList {
    constructor() { this._rules = []; }
    addAddress(value, family) {
      const address = parseAddress(value, family);
      if (!address) throw new TypeError("Invalid IP address");
      this._rules.unshift({ kind: "address", family: address.family, address: address.bytes });
    }
    addRange(startValue, endValue, family) {
      const start = parseAddress(startValue, family), end = parseAddress(endValue, family);
      if (!start || !end || start.family !== end.family) throw new TypeError("Invalid IP address range");
      if (bytesCompare(start.bytes, end.bytes) > 0) throw new RangeError("start must come before end");
      this._rules.unshift({ kind: "range", family: start.family, start: start.bytes, end: end.bytes });
    }
    addSubnet(value, prefix, family) {
      const network = parseAddress(value, family);
      if (!network) throw new TypeError("Invalid IP subnet");
      const max = network.family === 4 ? 32 : 128;
      if (!Number.isInteger(prefix) || prefix < 0 || prefix > max) throw new RangeError("Invalid subnet prefix");
      this._rules.unshift({ kind: "subnet", family: network.family, network: network.bytes, prefix });
    }
    // node blocklist.js `rules`: human-readable strings (rules are unshifted).
    get rules() {
      const fam = (f) => (f === 4 ? "IPv4" : "IPv6");
      const fmtAddr = (family, bytes) => family === 4
        ? Array.from(bytes).join(".")
        : Array.from({ length: 8 }, (_, i) => ((bytes[2 * i] << 8) | bytes[2 * i + 1]).toString(16)).join(":");
      return this._rules.map((r) => r.kind === "address"
        ? "Address: " + fam(r.family) + " " + fmtAddr(r.family, r.address)
        : r.kind === "range"
          ? "Range: " + fam(r.family) + " " + fmtAddr(r.family, r.start) + "-" + fmtAddr(r.family, r.end)
          : "Subnet: " + fam(r.family) + " " + fmtAddr(r.family, r.network) + "/" + r.prefix);
    }
    check(value, family) {
      const address = parseAddress(value, family);
      if (!address) return false;
      for (const rule of this._rules) {
        const bytes = addressForRule(address, rule.family);
        if (!bytes) continue;
        if (rule.kind === "address" && bytesEqual(bytes, rule.address)) return true;
        if (rule.kind === "range" && bytesCompare(bytes, rule.start) >= 0 && bytesCompare(bytes, rule.end) <= 0) return true;
        if (rule.kind === "subnet" && prefixMatches(bytes, rule.network, rule.prefix)) return true;
      }
      return false;
    }
  }

  // TCP-only Bun.listen compatibility. It intentionally shares the exact
  // Socket/Server reactor used by node:net; this is a real kernel socket path,
  // not an in-memory test transport. Unix/fd/TLS options remain unsupported.
  class BunTcpSocket {
    constructor(socket, listener) {
      this._socket = socket; this.listener = listener; this.data = listener.data;
      this._lastError = null;
      socket.on("data", (chunk) => this._call("data", chunk));
      socket.on("drain", () => this._call("drain"));
      socket.on("end", () => this._call("end"));
      socket.on("error", (error) => { this._lastError = error; this._call("error", error); });
      socket.on("close", () => this._call("close", this._lastError));
    }
    _call(name, value) {
      const handlers = this.listener._handlers;
      const callback = handlers && handlers[name];
      if (typeof callback !== "function") return;
      try { value === undefined ? callback(this) : callback(this, value); }
      catch (error) {
        if (name !== "error" && typeof handlers.error === "function") handlers.error(this, error);
        else G.queueMicrotask(() => { throw error; });
      }
    }
    // bun's writeOrEnd (src/runtime/socket/socket_body.rs) returns -1 when the
    // socket is already shut down or closed; it never surfaces node's
    // ERR_STREAM_WRITE_AFTER_END. The echo tests write from a `data` callback
    // that can land after the peer's FIN, so the node-level error must not leak
    // into the Bun.listen/Bun.connect handlers.
    get _writeShutdown() { return this._socket.destroyed || this._socket.writableEnded; }
    write(data, encoding) {
      const length = u8(typeof data === "string" && encoding && G.Buffer ? G.Buffer.from(data, encoding) : data).length;
      if (this._writeShutdown) return -1;
      this._socket.write(data, encoding);
      return length;
    }
    end(data, encoding) {
      if (this._writeShutdown) return this;
      this._socket.end(data, encoding);
      return this;
    }
    close() { this._socket.destroy(); return this; }
    terminate() { this._socket.destroy(); return this; }
    pause() { this._socket.pause(); return this; }
    resume() { this._socket.resume(); return this; }
    ref() { this._socket.ref(); return this; }
    unref() { this._socket.unref(); return this; }
    setTimeout(ms) { this._socket.setTimeout(ms); return this; }
    get bytesWritten() { return this._socket.bytesWritten; }
    get localPort() { return this._socket.localPort; }
    get remotePort() { return this._socket.remotePort; }
    get remoteAddress() { return this._socket.remoteAddress; }
  }

  function bunListen(options) {
    if (!options || typeof options !== "object") throw new TypeError("Bun.listen expects an options object");
    if (options.unix != null || options.fd != null || options.tls) {
      throw new Error("Bun.listen: this mbun slice supports TCP listeners only");
    }
    const handlers = options.socket;
    if (!handlers || typeof handlers !== "object" ||
        (typeof handlers.data !== "function" && typeof handlers.drain !== "function")) {
      throw new TypeError('Bun.listen expects at least a "data" or "drain" socket callback');
    }
    const hostname = options.hostname || "localhost";
    const server = new Server({ allowHalfOpen: !!options.allowHalfOpen });
    const listener = {
      data: options.data,
      hostname,
      unix: undefined,
      port: 0,
      get connectionsCount() { return server._conns.size; },
      ref() { server.ref(); return this; },
      unref() { server.unref(); return this; },
      reload(next) {
        if (!next || !next.socket) throw new TypeError('Expected "socket" object');
        this._handlers = next.socket;
        return this;
      },
      stop(force) {
        server.close();
        if (force) for (const socket of Array.from(server._conns)) socket.destroy();
      },
      _handlers: handlers,
    };
    server.on("connection", (socket) => {
      const wrapped = new BunTcpSocket(socket, listener);
      wrapped._call("open");
    });
    server.on("error", (error) => {
      if (typeof listener._handlers.error === "function") listener._handlers.error(listener, error);
      else G.queueMicrotask(() => { throw error; });
    });
    server.listen(options.port == null ? 0 : options.port, hostname);
    listener.port = server.address().port;
    listener[Symbol.dispose] = () => listener.stop(true);
    listener[Symbol.asyncDispose] = () => { listener.stop(true); return Promise.resolve(); };
    return listener;
  }

  const BunObject = G.Bun || (G.Bun = {});
  BunObject.listen = bunListen;

  // node's net.Server / net.Socket are pre-class constructors, so both
  // `new net.Server()` and the bare factory call `net.Server()` are legal and
  // its own corpus uses both (test-net-pipe-unref: `net.Server()`; the
  // connect-buffer/binary/bytes-stats family: `net.Server(onconn)`). mbun
  // implements them as ES classes, where a call without `new` is a hard
  // "Cannot call a class constructor Server without |new|". Re-export each class
  // through a plain-function stand-in that restores the factory form while
  // keeping construct behaviour, prototype identity, statics and instanceof
  // intact — the same proven shape used for node:http in node_legacy_ctors.
  // The prototype's own `constructor` is re-pointed at the stand-in so instances
  // (including those the reactor builds internally with `new Socket`) report the
  // exported value as their constructor. Mirrors node lib/net.js.
  const callable = (Cls) => {
    const wrapper = function (...args) {
      if (new.target !== undefined) return Reflect.construct(Cls, args, new.target);
      // `Ctor.call(this, opts)` (util.inherits subclassing): the receiver's
      // chain already reaches the class — initialise it in place.
      if (this !== null && this !== undefined && typeof this === "object" && this instanceof Cls) {
        Object.defineProperties(this, Object.getOwnPropertyDescriptors(Reflect.construct(Cls, args)));
        return undefined;
      }
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
  const ServerW = callable(Server);
  const SocketW = callable(Socket);

  // node net.js Happy-Eyeballs default accessors. setDefault* runs
  // validateInt32(value, 'value', 1) (a value < 1 is ERR_OUT_OF_RANGE) and then
  // floors the attempt timeout at 10ms.
  const setDefaultAutoSelectFamilyAttemptTimeout = (value) => {
    if (typeof value !== "number" || !Number.isInteger(value) || value < 1) {
      const e = new RangeError('The value of "value" is out of range. It must be a positive integer greater than 0. Received ' + String(value));
      e.code = "ERR_OUT_OF_RANGE"; throw e;
    }
    autoSelectFamilyAttemptTimeoutDefault = value < 10 ? 10 : value;
  };
  const getDefaultAutoSelectFamilyAttemptTimeout = () => autoSelectFamilyAttemptTimeoutDefault;
  const setDefaultAutoSelectFamily = (value) => {
    if (typeof value !== "boolean") {
      const e = new TypeError('The "value" argument must be of type boolean. Received ' + typeof value);
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    autoSelectFamilyDefault = value;
  };
  const getDefaultAutoSelectFamily = () => autoSelectFamilyDefault;

  def(["net"], Object.assign({}, M["net"] || {}, {
    Socket: SocketW, Stream: SocketW, Server: ServerW, BlockList,
    createServer: (o, cb) => new Server(o, cb),
    createConnection: (...a) => new Socket(typeof a[0] === "object" ? a[0] : undefined).connect(...a),
    connect: (...a) => new Socket(typeof a[0] === "object" ? a[0] : undefined).connect(...a),
    isIP, isIPv4, isIPv6,
    setDefaultAutoSelectFamilyAttemptTimeout, getDefaultAutoSelectFamilyAttemptTimeout,
    setDefaultAutoSelectFamily, getDefaultAutoSelectFamily,
  }));

  // ---- incremental HTTP/1.1 parser (requests and responses) ------------------
  // Content-Length AND chunked transfer-encoding bodies, 100-continue skip,
  // chunk extensions tolerated, trailers read tolerantly (EOF/missing final
  // CRLF accepted, matching bun — chunked-trailing.test.js). Error taxonomy:
  // bytes that contradict the framing → InvalidHTTPResponse; connection closed
  // while the framing still expects bytes → ECONNRESET (bun's fetch codes).
  const CRLF2 = [13, 10, 13, 10];
  function findSeq(buf, off, seq) {
    const n = buf.length - seq.length;
    outer: for (let i = off; i <= n; i++) {
      for (let j = 0; j < seq.length; j++) if (buf[i + j] !== seq[j]) continue outer;
      return i;
    }
    return -1;
  }
  // RFC 7230 field-name (tchar+); anything else in a header name is a parse
  // error, not something to normalise away.
  const HEADER_TOKEN_RE = /^[\^_`a-zA-Z\-0-9!#$%&'*+.|~]+$/;
  function findCRLF(buf, off) {
    for (let i = off; i + 1 < buf.length; i++) if (buf[i] === 13 && buf[i + 1] === 10) return i;
    return -1;
  }
  class HttpParser {
    constructor(isResponse) {
      this.isResponse = !!isResponse;
      this.buf = new Uint8Array(0); this.off = 0;
      this.state = "head"; this.done = false; this.headDone = false;
      this.method = ""; this.target = ""; this.status = 0; this.statusText = ""; this.httpVersion = "1.1";
      this.headers = {}; this.rawHeaders = []; this.trailers = {};
      this.remaining = 0; this.chunked = false; this.toEof = false;
      this.reqMethod = "GET";
      // node: parser.maxHeaderPairs = server.maxHeadersCount << 1 (0 = no cap);
      // maxHeaderSize caps the whole head block (--max-http-header-size).
      this.maxHeaderPairs = 0;
      this.maxHeaderSize = 0;
      this.onHead = null; this.onBody = null; this.onDone = null; this.onError = null;
      // 1xx interim heads are not the final response: node re-arms the parser
      // and raises 'continue'/'information' on the ClientRequest instead
      // (_http_client.js parserOnIncomingClient -> `return 1`).
      this.onInterim = null;
    }
    leftover() { return this.buf.subarray(this.off); }
    push(bytes) {
      if (this.done || this.state === "error") return 0;
      if (bytes && bytes.length) {
        this.buf = this.off === this.buf.length ? bytes : concatU8([this.buf.subarray(this.off), bytes]);
        this.off = 0;
      }
      return this._process(false);
    }
    eof() { if (this.done || this.state === "error") return 0; return this._process(true); }
    _err(msg, code) { this.state = "error"; if (this.onError) this.onError(mkErr(msg, code)); }
    _finish() { this.done = true; if (this.onDone) this.onDone(); }
    _emitBody(b) { if (b.length && this.onBody) this.onBody(b); }
    _process(eofSeen) {
      let events = 0;
      for (;;) {
        const avail = this.buf.length - this.off;
        if (this.state === "head") {
          const at = findSeq(this.buf, this.off, CRLF2);
          if (at < 0) {
            if (!eofSeen) return events;
            if (avail === 0 && !this.isResponse) { this.done = true; return events; }  // idle conn closed
            this._err("The socket connection was closed unexpectedly", "ECONNRESET");
            return events + 1;
          }
          const headLen = at + 4 - this.off;
          const hardLimit = this.maxHeaderSize > 0 ? this.maxHeaderSize
            : (G.__mbunHttpNative && G.__mbunHttpNative.getMaxHeaderSize ? G.__mbunHttpNative.getMaxHeaderSize() | 0 : 0);
          if (hardLimit > 0 && headLen > hardLimit) {
            this.off = at + 4;
            this._err("Parse Error: Header overflow", "HPE_HEADER_OVERFLOW");
            return events + 1;
          }
          const head = latin1(this.buf, this.off, at);
          this.off = at + 4;
          // picohttpparser refuses any control byte inside the head: every byte
          // below 0x20 except HTAB (and the CR/LF that end a line), plus DEL.
          // bun surfaces that as Malformed_HTTP_Response / BadRequest
          // (compat/bun/src/picohttp/lib.rs). Accepting them let a redirect
          // Location carrying a raw \x0b / \x01 / \x7f be followed as a normal
          // target instead of failing the exchange.
          for (let i = 0; i < head.length; i++) {
            const cc = head.charCodeAt(i);
            if (cc === 9 || cc === 10 || cc === 13) continue;
            if (cc < 32 || cc === 127) {
              if (this.isResponse) this._err("Malformed_HTTP_Response", "Malformed_HTTP_Response");
              else this._err("Invalid HTTP request", "InvalidHTTPRequest");
              return events + 1;
            }
          }
          const lines = head.split("\r\n");
          const first = lines.shift() || "";
          if (this.isResponse) {
            const m = /^HTTP\/(\d\.\d)\s+(\d{3})\s*(.*)$/.exec(first);
            if (!m) { this._err("Invalid HTTP response", "InvalidHTTPResponse"); return events + 1; }
            this.httpVersion = m[1]; this.status = +m[2]; this.statusText = m[3] || "";
          } else {
            const m = /^(\S+)\s+(\S+)\s+HTTP\/(\d\.\d)$/.exec(first);
            if (!m) { this._err("Invalid HTTP request", "InvalidHTTPRequest"); return events + 1; }
            this.method = m[1].toUpperCase(); this.target = m[2]; this.httpVersion = m[3];
          }
          // ---- header-block validation (llhttp strictness) ----------------
          // Everything below is a request-smuggling vector when it is merely
          // tolerated, so each one is a hard parse error with llhttp's own
          // code (node surfaces them through 'clientError' / the request's
          // 'error', and the default server answer is 400 Bad Request).
          this.headers = {}; this.rawHeaders = [];
          let sawCL = false, sawTE = false, teChunked = false, clValue = null;
          for (const line of lines) {
            if (!line) continue;
            // A CR or LF that did not terminate a line: the header block was
            // framed with a bare CR/LF, which two parsers can disagree about.
            if (line.indexOf("\r") !== -1 || line.indexOf("\n") !== -1) {
              this._err("Parse Error: Expected LF after CR", "HPE_LF_EXPECTED");
              return events + 1;
            }
            const c = line.indexOf(":");
            // No separator at all, an empty name, or whitespace between the
            // name and the colon (obs-fold / "Name : value") is invalid.
            if (c <= 0 || /[ \t]$/.test(line.slice(0, c))) {
              this._err("Parse Error: Invalid header token", "HPE_INVALID_HEADER_TOKEN");
              return events + 1;
            }
            const k = line.slice(0, c), v = line.slice(c + 1).trim();
            if (!HEADER_TOKEN_RE.test(k)) {
              this._err("Parse Error: Invalid header token", "HPE_INVALID_HEADER_TOKEN");
              return events + 1;
            }
            const lk = k.toLowerCase();
            if (lk === "content-length") {
              if (!/^\d+$/.test(v)) {
                this._err("Parse Error: Invalid content length", "HPE_UNEXPECTED_CONTENT_LENGTH");
                return events + 1;
              }
              // Two Content-Lengths (or one folded "1, 2") let a proxy and an
              // origin frame the same bytes differently.
              if (sawCL && clValue !== v) {
                this._err("Parse Error: Duplicate Content-Length", "HPE_UNEXPECTED_CONTENT_LENGTH");
                return events + 1;
              }
              sawCL = true; clValue = v;
            } else if (lk === "transfer-encoding") {
              const codings = v.toLowerCase().split(",").map((t) => t.trim()).filter((t) => t.length);
              const chunkedAt = codings.indexOf("chunked");
              // chunked must be the final coding and appear exactly once; any
              // other shape leaves the body length undefined.
              teChunked = !sawTE && codings.length > 0 && chunkedAt === codings.length - 1 &&
                          codings.lastIndexOf("chunked") === chunkedAt;
              sawTE = true;
            }
            if (this.maxHeaderPairs > 0 && this.rawHeaders.length >= this.maxHeaderPairs) continue;
            this.rawHeaders.push(k, v);
            this.headers[lk] = lk in this.headers ? this.headers[lk] + ", " + v : v;
          }
          // Transfer-Encoding together with Content-Length is the classic
          // desync: llhttp refuses the combination outright, before the message
          // is ever handed to the application.
          if (sawTE && sawCL) {
            this._err("Parse Error: Content-Length can't be present with Transfer-Encoding",
                      "HPE_INVALID_TRANSFER_ENCODING");
            return events + 1;
          }
          if (this.isResponse && this.status >= 100 && this.status < 200 && this.status !== 101) {
            if (this.onInterim) {
              this.onInterim({ status: this.status, statusText: this.statusText, httpVersion: this.httpVersion,
                               headers: this.headers, rawHeaders: this.rawHeaders });
            }
            events++;
            continue;  // 1xx interim: the final response follows on this connection
          }
          this.headDone = true;
          const noBody = this.isResponse
            ? (this.reqMethod === "HEAD" || this.status === 204 || this.status === 304)
            : false;
          if (noBody) { this.state = "done"; }
          else if (sawTE && teChunked) { this.chunked = true; this.state = "chunk-size"; }
          else if (sawTE) { this.state = "te-invalid"; }
          else if (sawCL) { this.remaining = parseInt(clValue, 10) || 0; this.state = this.remaining > 0 ? "body-cl" : "done"; }
          else if (this.isResponse) { this.toEof = true; this.state = "body-eof"; }
          else { this.state = "done"; }
          events++;
          if (this.onHead) this.onHead();
          // llhttp raises on_headers_complete first and only then rejects a
          // Transfer-Encoding it cannot frame, so the request is observed once
          // and its body never is (test-http-transfer-encoding-repeated-chunked).
          if (this.state === "te-invalid") {
            this._err("Parse Error: Invalid transfer encoding", "HPE_INVALID_TRANSFER_ENCODING");
            return events + 1;
          }
          if (this.state === "done") { this._finish(); return events + 1; }
          continue;
        }
        if (this.state === "body-cl") {
          if (avail > 0) {
            const n = Math.min(avail, this.remaining);
            this._emitBody(this.buf.subarray(this.off, this.off + n));
            this.off += n; this.remaining -= n; events++;
            if (this.remaining === 0) { this._finish(); return events + 1; }
            continue;
          }
          if (eofSeen) { this._err("The socket connection was closed unexpectedly", "ECONNRESET"); return events + 1; }
          return events;
        }
        if (this.state === "body-eof") {
          if (avail > 0) { this._emitBody(this.buf.subarray(this.off)); this.off = this.buf.length; events++; continue; }
          if (eofSeen) { this._finish(); return events + 1; }
          return events;
        }
        if (this.state === "chunk-size") {
          const at = findCRLF(this.buf, this.off);
          if (at < 0) {
            if (eofSeen) { this._err("The socket connection was closed unexpectedly", "ECONNRESET"); return events + 1; }
            return events;
          }
          let line = latin1(this.buf, this.off, at);
          this.off = at + 2;
          // A chunk-size line is hex digits plus an optional extension, and
          // nothing else -- a stray CR/LF inside it is how a chunk-extension
          // smuggling payload hides a second request
          // (test-http-chunked-smuggling, test-http-dummy-characters-smuggling).
          if (!/^[0-9a-fA-F]+[ \t]*(;[^\r\n]*)?$/.test(line)) {
            this._err("Parse Error: Invalid chunk size", "HPE_INVALID_CHUNK_SIZE");
            return events + 1;
          }
          const semi = line.indexOf(";");
          if (semi !== -1) line = line.slice(0, semi);
          line = line.trim();
          const size = parseInt(line, 16);
          events++;
          if (size === 0) { this.state = "trailers"; continue; }
          this.remaining = size; this.state = "chunk-data";
          continue;
        }
        if (this.state === "chunk-data") {
          if (avail > 0) {
            const n = Math.min(avail, this.remaining);
            this._emitBody(this.buf.subarray(this.off, this.off + n));
            this.off += n; this.remaining -= n; events++;
            if (this.remaining === 0) this.state = "chunk-crlf";
            continue;
          }
          if (eofSeen) { this._err("The socket connection was closed unexpectedly", "ECONNRESET"); return events + 1; }
          return events;
        }
        if (this.state === "chunk-crlf") {
          if (avail >= 1 && this.buf[this.off] !== 13) { this._err("Invalid HTTP response (missing CRLF after chunk data)", "InvalidHTTPResponse"); return events + 1; }
          if (avail >= 2) {
            if (this.buf[this.off + 1] !== 10) { this._err("Invalid HTTP response (missing CRLF after chunk data)", "InvalidHTTPResponse"); return events + 1; }
            this.off += 2; this.state = "chunk-size"; events++;
            continue;
          }
          if (eofSeen) { this._err("The socket connection was closed unexpectedly", "ECONNRESET"); return events + 1; }
          return events;
        }
        if (this.state === "trailers") {
          // Tolerant: EOF mid-trailers or a missing final CRLF still completes the
          // body (bun accepts both — chunked-trailing.test.js).
          const at = findCRLF(this.buf, this.off);
          if (at < 0) {
            if (eofSeen) { this._finish(); return events + 1; }
            return events;
          }
          const line = latin1(this.buf, this.off, at);
          this.off = at + 2;
          if (line === "") { this._finish(); return events + 1; }
          const c = line.indexOf(":");
          if (c > 0) this.trailers[line.slice(0, c).trim().toLowerCase()] = line.slice(c + 1).trim();
          events++;
          continue;
        }
        return events;  // "done"/"error"
      }
    }
  }

  // node:http's ClientRequest parses its responses with this same incremental
  // parser rather than carrying a second HTTP/1.1 implementation.
  // _http_common.js keeps a parser free list and asserts the identity of the
  // recycled object (test-http-parser-free walks 100 keep-alive requests over
  // one socket and requires req.parser to be the *same* parser every time), so
  // expose the same alloc/free pair rather than minting one per request.
  HttpParser.prototype._reset = function (isResponse) {
    this.isResponse = !!isResponse;
    this.buf = new Uint8Array(0); this.off = 0;
    this.state = "head"; this.done = false; this.headDone = false;
    this.method = ""; this.target = ""; this.status = 0; this.statusText = ""; this.httpVersion = "1.1";
    this.headers = {}; this.rawHeaders = []; this.trailers = {};
    this.remaining = 0; this.chunked = false; this.toEof = false;
    this.reqMethod = "GET";
    this.maxHeaderPairs = 0; this.maxHeaderSize = 0;
    this.onHead = this.onBody = this.onDone = this.onError = this.onInterim = null;
    this._afterDone = null; this.socket = null; this.outgoing = null;
    this._pooled = false;
    return this;
  };
  const parserFreeList = [];
  HttpParser.alloc = function (isResponse) {
    const p = parserFreeList.pop();
    return p ? p._reset(isResponse) : new HttpParser(isResponse);
  };
  HttpParser.free = function (p) {
    if (!p || p._pooled) return;
    if (parserFreeList.length >= 1000) return;
    p._pooled = true;
    p.onHead = p.onBody = p.onDone = p.onError = p.onInterim = null;
    p.buf = new Uint8Array(0); p.off = 0;
    parserFreeList.push(p);
  };
  G.__mbunHttpParser = HttpParser;

  // ---- HTTP response serialization (shared by Bun.serve and node:http) -------
  const STATUS_TEXT = { 100: "Continue", 101: "Switching Protocols", 200: "OK", 201: "Created", 202: "Accepted", 204: "No Content", 206: "Partial Content", 301: "Moved Permanently", 302: "Found", 303: "See Other", 304: "Not Modified", 307: "Temporary Redirect", 308: "Permanent Redirect", 400: "Bad Request", 401: "Unauthorized", 403: "Forbidden", 404: "Not Found", 405: "Method Not Allowed", 408: "Request Timeout", 409: "Conflict", 413: "Payload Too Large", 418: "I'm a Teapot", 422: "Unprocessable Entity", 429: "Too Many Requests", 500: "Internal Server Error", 501: "Not Implemented", 502: "Bad Gateway", 503: "Service Unavailable" };
  const statusText = (code) => STATUS_TEXT[code] || "";
  // The reason phrase actually written to the status line.
  //
  // SECURITY — HTTP response splitting. `Response`'s statusText is stored verbatim
  // (bun Response.rs:1440 fast_get_truthy + to_bun_string, no validation), so
  // interpolating it straight into "HTTP/1.1 <status> <text>" let a CR/LF in
  // attacker-influenced data terminate the status line and inject headers, a body,
  // and a whole second response:
  //   new Response("body", { statusText: "OK\r\nX-Injected: yes\r\n\r\n<html>" })
  // reached the wire as two responses. Real bun never has this exposure because
  // RequestContext.do_write_status ignores statusText entirely and writes its own
  // HTTPStatusText table entry. Keep mbun's ability to echo a custom reason phrase,
  // but restrict it to the RFC 9112 §4.1 reason-phrase production
  // (HTAB / SP / VCHAR / obs-text) so no octet can close the line. A value carrying
  // anything else falls back to the canonical text rather than being half-written.
  const REASON_OK = /^[\t\x20-\x7e\x80-\xff]*$/;
  const reasonPhrase = (res, status) => {
    const t = res && res.statusText;
    return (typeof t === "string" && t !== "" && REASON_OK.test(t)) ? t : statusText(status);
  };

  // Drain a body value into an array of Uint8Array chunks. Supports strings,
  // (typed) arrays, Blob, our minimal ReadableStream (_chunks + optional pull),
  // reader-based streams (fetch bodies), and async iterables.
  function drainBody(body) {
    if (body == null) return Promise.resolve([]);
    if (typeof body === "string" || body instanceof Uint8Array || ArrayBuffer.isView(body) || body instanceof ArrayBuffer) return Promise.resolve([u8(body)]);
    if (body._u8 instanceof Uint8Array) return Promise.resolve([body._u8]);  // Blob
    if (body._chunks !== undefined || typeof body._pull === "function") {  // minimal ReadableStream
      const out = (body._chunks || []).map(u8);
      if (typeof body._pull !== "function") return Promise.resolve(out);
      return new Promise((resolve, reject) => {
        let closed = false;
        const ctrl = { enqueue: (c) => out.push(u8(c)), close: () => { closed = true; }, error: (e) => reject(e), get desiredSize() { return 1; } };
        const step = () => {
          if (closed) return resolve(out);
          let r;
          try { r = body._pull(ctrl); } catch (e) { return reject(e); }
          Promise.resolve(r).then(() => { if (closed) resolve(out); else G.queueMicrotask(step); }, reject);
        };
        step();
      });
    }
    if (typeof body.getReader === "function") {
      const rd = body.getReader(); const out = [];
      const step = () => rd.read().then((r) => { if (r.done) return out; out.push(u8(r.value)); return step(); });
      return step();
    }
    if (typeof body[Symbol.asyncIterator] === "function") {
      const it = body[Symbol.asyncIterator](); const out = [];
      const step = () => Promise.resolve(it.next()).then((r) => { if (r.done) return out; out.push(u8(r.value)); return step(); });
      return step();
    }
    return Promise.resolve([u8(body)]);
  }

  // Build the response head lines (minus framing) shared by the buffered and
  // streaming paths. framing = { chunked } or { contentLength }.
  const responseHeadLines = (res, status, framing, keepAlive) => {
    const lines = ["HTTP/1.1 " + status + " " + reasonPhrase(res, status)];
    let haveCT = false, haveDate = false;
    if (res.headers && typeof res.headers.forEach === "function") {
      res.headers.forEach((v, k) => {
        const lk = String(k).toLowerCase();
        if (lk === "content-length" || lk === "transfer-encoding" || lk === "connection") return;
        if (lk === "content-type") haveCT = true;
        if (lk === "date") haveDate = true;
        if (lk === "set-cookie" && typeof res.headers.getSetCookie === "function") return;
        lines.push(k + ": " + v);
      });
      if (typeof res.headers.getSetCookie === "function") for (const c of res.headers.getSetCookie()) lines.push("Set-Cookie: " + c);
    }
    if (!haveDate) lines.push("Date: " + new Date().toUTCString());
    if (framing.chunked) lines.push("Transfer-Encoding: chunked");
    else lines.push("Content-Length: " + framing.contentLength);
    lines.push("Connection: " + (keepAlive ? "keep-alive" : "close"));
    return lines.join("\r\n") + "\r\n\r\n";
  };

  // Stream a Bun `type: "direct"` ReadableStream response body. The source's
  // pull(controller) is invoked once with an HTTPResponseSink whose write/flush
  // map to chunked writes on the socket. Blueprint (bun src/runtime/server +
  // webcore/Sink.rs): status+headers commit eagerly before pull; a "direct"
  // stream terminates the socket once `async pull()` returns (a synchronous
  // pull return leaves the response open until controller.end()); write after
  // close throws the HTTPResponseSink-closed message; a client disconnect while
  // waiting for end() cancels the source. Backpressure rides the Socket write
  // queue (partial-write buffering + `drain`); desiredSize reflects it.
  const SINK_BRAND = "__mbunHttpResponseSink";
  const SINK_CLOSED_MSG = 'This HTTPResponseSink has already been closed. A "direct" ReadableStream terminates its underlying socket once `async pull()` returns.';
  // A direct stream's pull() that throws/rejects while the response is still
  // rendering surfaces the error like bun's uncaught-stream reporter: `error:
  // <message>` on stderr, without crashing the process or re-rendering error().
  const reportStreamError = (e) => {
    try { if (G.console && typeof G.console.error === "function") G.console.error("error: " + ((e && e.message != null) ? e.message : String(e))); } catch (e2) {}
  };
  function writeDirectStreamResponse(sock, res, reqMethod, keepAlive, onFinished, source) {
    const status = res.status || 200;
    const noBody = reqMethod === "HEAD" || status === 204 || status === 304;
    const HWM = 64 * 1024;
    const st = { ended: false, closed: false, bytesFlushed: false, headersSent: false, buf: [], bufLen: 0, autoFlush: false };

    const sendHead = (framing) => { if (st.headersSent) return; st.headersSent = true; sock.write(responseHeadLines(res, status, framing, keepAlive)); };
    const frameToSocket = (b) => { if (!b || !b.length) return; sendHead({ chunked: true }); sock.write(b.length.toString(16) + "\r\n"); sock.write(b); sock.write("\r\n"); st.bytesFlushed = true; };
    const flushBuf = () => {
      if (st.bufLen === 0) return;
      const merged = st.buf.length === 1 ? st.buf[0] : concatU8(st.buf);
      st.buf = []; st.bufLen = 0;
      frameToSocket(merged);
    };

    const finishOnce = () => { if (onFinished) { const f = onFinished; onFinished = null; f(); } };
    const endResponse = () => {
      if (st.ended) return;
      st.ended = true; st.closed = true;
      flushBuf();
      sendHead({ chunked: true });         // zero-body streams still commit 200 + chunked
      sock.write("0\r\n\r\n");
      if (!keepAlive) sock.end();
      finishOnce();
    };
    const forceClose = () => {
      if (st.ended) return;
      st.ended = true; st.closed = true;
      sock.destroy();
      finishOnce();
    };

    const sink = {
      write(chunk) {
        if (this == null || this[SINK_BRAND] !== true) throw new TypeError("Expected HTTPResponseSink");
        if (st.closed) throw new TypeError(SINK_CLOSED_MSG);
        const b = u8(chunk);
        st.buf.push(b); st.bufLen += b.length;
        if (!st.autoFlush) { st.autoFlush = true; Promise.resolve().then(() => { st.autoFlush = false; if (!st.closed) flushBuf(); }); }
        return b.length;
      },
      flush() {
        if (this == null || this[SINK_BRAND] !== true) throw new TypeError("Expected HTTPResponseSink");
        if (st.closed) throw new TypeError(SINK_CLOSED_MSG);
        flushBuf();
        return 0;
      },
      end() { if (this == null || this[SINK_BRAND] !== true) throw new TypeError("Expected HTTPResponseSink"); endResponse(); },
      close() { if (this == null || this[SINK_BRAND] !== true) throw new TypeError("Expected HTTPResponseSink"); endResponse(); },
      error(e) { forceClose(); },
      start() { return 0; },
      ref() {}, unref() {},
      get desiredSize() { return HWM - (sock.writableLength | 0) - st.bufLen; },
    };
    Object.defineProperty(sink, SINK_BRAND, { value: true });

    // Client disconnect while the response is still open cancels the source.
    const onGone = () => {
      if (st.ended) return;
      st.ended = true; st.closed = true;
      try { if (typeof source.cancel === "function") Promise.resolve(source.cancel(mkErr("aborted", "ABORT_ERR"))).catch(() => {}); } catch (e) {}
      finishOnce();
    };
    sock.once("close", onGone);

    if (noBody) { sendHead({ contentLength: 0 }); if (!keepAlive) sock.end(); onGone(); return; }

    if (typeof source.pull !== "function") { endResponse(); return; }
    let ret;
    try { ret = source.pull(sink); }
    catch (e) {
      // pull threw synchronously: status is already committed, so error() cannot
      // re-render. Bytes flushed -> force-close; otherwise end the stream empty.
      reportStreamError(e);
      if (st.bytesFlushed) forceClose(); else endResponse();
      return;
    }
    if (ret != null && typeof ret.then === "function") {
      ret.then(() => endResponse(), (e) => { reportStreamError(e); if (st.bytesFlushed) forceClose(); else endResponse(); });
    }
    // Synchronous return: leave the response open until controller.end()/close()
    // or a client disconnect (react renderToReadableStream shell-then-resolve).
  }

  // Serialize a Response onto a socket. `type: "direct"` ReadableStream bodies
  // stream chunked (writeDirectStreamResponse); other streams are drained first
  // and sent with Content-Length.
  function writeHttpResponse(sock, res, reqMethod, keepAlive, onFinished) {
    if (!res || typeof res !== "object") res = new G.Response("", { status: 500 });
    const status = res.status || 200;
    const S = G.__mbunStreams;
    if (res._b == null && res._stream != null && S && typeof S.directStreamSource === "function") {
      const source = S.directStreamSource(res._stream);
      if (source) { writeDirectStreamResponse(sock, res, reqMethod, keepAlive, onFinished, source); return; }
    }
    // Non-direct ReadableStream / async-iterable bodies: unknown length →
    // Transfer-Encoding: chunked, streamed as chunks arrive (bun #15355 and
    // the HEAD framing tests assert this instead of buffered Content-Length).
    const noBodyEarly = reqMethod === "HEAD" || status === 204 || status === 304;
    if (!noBodyEarly && res._b == null && res._stream != null && typeof res._stream.getReader === "function") {
      let rd = null;
      try { rd = res._stream.getReader(); } catch (e) { rd = null; }  // locked/disturbed → buffered fallback
      if (rd) {
        sock.write(responseHeadLines(res, status, { chunked: true }, keepAlive));
        // A client that vanishes mid-stream must cancel the response body stream
        // right away, NOT only once the next chunk shows up: a body that parks
        // without producing data (hono's stream() helper awaits its own onAbort,
        // which the cancel is what fires) leaves rd.read() pending forever, so the
        // `sock.destroyed` check below would never be reached and the handler would
        // hang for good. bun drives this off the socket abort callback instead —
        // RequestContext.rs:1399 on_abort → stream.abort(global_this) (:1497-1501),
        // which runs the ReadableStream's `cancel` — so cancel from "close" too.
        let fin = false;
        const finish = () => { if (fin) return; fin = true; if (onFinished) onFinished(); };
        const cancelStream = () => {
          if (fin) return;  // response already completed → reader is spent, no-op
          try { if (typeof rd.cancel === "function") Promise.resolve(rd.cancel()).catch(() => {}); } catch (e) {}
        };
        sock.once("close", () => { cancelStream(); finish(); });
        const step = () => rd.read().then((r) => {
          if (sock.destroyed) { cancelStream(); finish(); return; }
          if (r.done) { sock.write("0\r\n\r\n"); if (!keepAlive) sock.end(); finish(); return; }
          const b = u8(r.value);
          if (b.length) { sock.write(b.length.toString(16) + "\r\n"); sock.write(b); sock.write("\r\n"); }
          return step();
        }, (e) => { try { sock.destroy(); } catch (e2) {} finish(); });
        step();
        return;
      }
    }
    // Direct bodies live in res._b; ReadableStream bodies live in res._stream and
    // are drained through the Response's own bytes() (streams module). Buffer then
    // send with Content-Length (chunked response streaming is DEFERRED).
    const bodyChunks = (res._b == null && res._stream != null && typeof res.bytes === "function")
      ? res.bytes().then((u8) => (u8 && u8.length ? [u8] : []))
      : drainBody(res._b);
    bodyChunks.then((chunks) => {
      const total = concatU8(chunks);
      const noBody = reqMethod === "HEAD" || status === 204 || status === 304;
      const lines = ["HTTP/1.1 " + status + " " + reasonPhrase(res, status)];
      // For a bodiless response (HEAD/204/304) the handler-supplied framing
      // headers (Content-Length / Transfer-Encoding) describe what the body
      // WOULD be and must be echoed verbatim rather than recomputed to 0
      // (bun #15355). For responses carrying a real body we recompute
      // Content-Length from the drained bytes and drop handler framing.
      let haveCT = false, haveDate = false, cLenHdr = null, teHdr = null;
      if (res.headers && typeof res.headers.forEach === "function") {
        res.headers.forEach((v, k) => {
          const lk = String(k).toLowerCase();
          if (lk === "content-length") { cLenHdr = v; return; }  // recomputed / echoed below
          if (lk === "content-type") haveCT = true;
          if (lk === "date") haveDate = true;
          if (lk === "transfer-encoding") { teHdr = v; return; }  // echoed below
          if (lk === "connection") return;
          if (lk === "set-cookie" && typeof res.headers.getSetCookie === "function") return;
          lines.push(k + ": " + v);
        });
        if (typeof res.headers.getSetCookie === "function") for (const c of res.headers.getSetCookie()) lines.push("Set-Cookie: " + c);
      }
      if (!haveCT && typeof res._b === "string" && res._b !== "") lines.push("Content-Type: text/plain;charset=utf-8");
      if (!haveDate) lines.push("Date: " + new Date().toUTCString());
      if (noBody && teHdr != null) lines.push("Transfer-Encoding: " + teHdr);
      else if (noBody && cLenHdr != null) lines.push("Content-Length: " + cLenHdr);
      // HEAD of a stream body: the GET response WOULD be chunked (length
      // unknown), so echo that framing rather than a drained Content-Length.
      else if (noBody && res._b == null && res._stream != null) lines.push("Transfer-Encoding: chunked");
      else lines.push("Content-Length: " + total.length);
      lines.push("Connection: " + (keepAlive ? "keep-alive" : "close"));
      sock.write(lines.join("\r\n") + "\r\n\r\n");
      if (!noBody && total.length) sock.write(total);
      if (!keepAlive) sock.end();
      if (onFinished) onFinished();
    }, (e) => {
      try { sock.write("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"); } catch (e2) {}
      sock.end();
      if (onFinished) onFinished();
    });
  }

  // ---- Bun.serve routing (routes: { "/path/:param": handler | Response | {[METHOD]: ...} }) --
  // Blueprint: bun src/runtime/server/ServerConfig.zig (validateRouteName, the
  // routes Record shape) + server.zig route precedence. Match by per-segment
  // specificity (static > :param > *wildcard), ties broken by declaration order;
  // a per-method route that lacks the request method falls through to the next
  // matching route. HEAD is served by the GET handler (body stripped) when no
  // explicit HEAD is declared. params are percent-decoded with lossy UTF-8.
  const ROUTES_RECORD_ERR = [
    "'routes' expects a Record<string, Response | HTMLBundle | {[method: string]: (req: BunRequest) => Response|Promise<Response>}>", "", "To bundle frontend apps on-demand with Bun.serve(), import HTML files.", "", "Example:", "",
    "```js", "import { serve } from \"bun\";", "import app from \"./app.html\";", "", "serve({", "  routes: {",
    "    \"/index.json\": Response.json({ message: \"Hello World\" }),", "    \"/app\": app,", "    \"/path/:param\": (req) => {", "      const param = req.params.param;", "      return Response.json({ message: `Hello ${param}` });", "    },",
    "    \"/path\": {", "      GET(req) {", "        return Response.json({ message: \"Hello World\" });", "      },", "      POST(req) {", "        return Response.json({ message: \"Hello World\" });",
    "      },", "    },", "  },", "", "  fetch(request) {", "    return new Response(\"fallback response\");",
    "  },", "});", "```", "", "See https://bun.com/docs/api/http for more information.",
  ].join("\n");
  const ROUTES_NEEDS_EITHER = [
    "Bun.serve() needs either:", "", "  - A routes object:", "     routes: {", "       \"/path\": {", "         GET: (req) => new Response(\"Hello\")",
    "       }", "     }", "", "  - Or a fetch handler:", "     fetch: (req) => {", "       return new Response(\"Hello\")",
    "     }", "", "Learn more at https://bun.com/docs/api/http",
  ].join("\n");
  const ROUTES_NOT_OBJECT = [
    "Bun.serve() expects 'routes' to be an object shaped like:", "", "  {", "    \"/path\": {", "      GET: (req) => new Response(\"Hello\"),", "      POST: (req) => new Response(\"Hello\"),",
    "    },", "    \"/path2/:param\": new Response(\"Hello\"),", "    \"/path3/:param1/:param2\": (req) => new Response(\"Hello\")", "  }", "", "Learn more at https://bun.com/docs/api/http",
  ].join("\n");

  const isResponseLike = (v) => v instanceof G.Response;
  const hexVal = (c) => (c >= 48 && c <= 57) ? c - 48 : (c >= 97 && c <= 102) ? c - 87 : (c >= 65 && c <= 70) ? c - 55 : -1;
  // Decode a raw (latin1) path segment: percent-decode, then lossy UTF-8 decode.
  const decodeParam = (seg) => {
    let enc = false;
    for (let i = 0; i < seg.length; i++) { const c = seg.charCodeAt(i); if (c === 37 || c > 127) { enc = true; break; } }
    if (!enc) return seg;
    const bytes = [];
    for (let i = 0; i < seg.length; i++) {
      const c = seg.charCodeAt(i);
      if (c === 37 && i + 2 < seg.length) {
        const hv = hexVal(seg.charCodeAt(i + 1)), lv = hexVal(seg.charCodeAt(i + 2));
        if (hv >= 0 && lv >= 0) { bytes.push(hv * 16 + lv); i += 2; continue; }
      }
      bytes.push(c & 0xff);
    }
    return td.decode(new Uint8Array(bytes));
  };
  // Split a request-target into { path, query } (both latin1). Absolute-form
  // targets (RFC 9112 §3.2.2) drop the spoofed scheme+authority; only the path
  // and query survive (see bun-serve-routes "absolute-form" test).
  const splitTarget = (target) => {
    let t = target || "/";
    const am = /^[a-zA-Z][a-zA-Z0-9+.\-]*:\/\/[^/?#]*(.*)$/.exec(t);
    if (am) t = am[1] || "/";
    const qi = t.indexOf("?");
    let path = qi === -1 ? t : t.slice(0, qi);
    const query = qi === -1 ? "" : t.slice(qi);
    if (path === "" || path[0] !== "/") path = "/" + path;
    return { path, query };
  };
  const pathToSegs = (path) => { const raw = path.replace(/^\/+/, ""); return raw === "" ? [] : raw.split("/"); };
  // Compile a route pattern into segment matchers; validate parameter names.
  const parseRoutePattern = (pattern) => {
    const segs = pathToSegs(pattern), out = [], seen = Object.create(null);
    for (const part of segs) {
      if (part === "*") out.push({ kind: 0 });
      else if (part[0] === ":") {
        const name = part.slice(1);
        if (name.length && name.charCodeAt(0) >= 48 && name.charCodeAt(0) <= 57)
          throw new Error("Route parameter names cannot start with a number.\n\nIf you run into this, please file an issue and we will add support for it.");
        if (seen[name]) throw new Error("Support for duplicate route parameter names is not yet implemented.\n\nIf you run into this, please file an issue and we will add support for it.");
        seen[name] = true; out.push({ kind: 1, name });
      } else out.push({ kind: 2, lit: part });
    }
    return out;
  };
  // Higher kind = more specific: static(2) > param(1) > wildcard(0). Compare
  // element-wise; longer (more concrete segments before a wildcard) wins.
  const cmpScore = (a, b) => { const n = Math.min(a.length, b.length); for (let i = 0; i < n; i++) if (a[i] !== b[i]) return b[i] - a[i]; return b.length - a.length; };
  const matchSegs = (segs, pathSegs) => {
    const params = {};
    for (let i = 0; i < segs.length; i++) {
      const s = segs[i];
      if (s.kind === 0) return params;                 // wildcard absorbs the remainder
      if (i >= pathSegs.length) return null;
      if (s.kind === 2) { if (pathSegs[i] !== s.lit) return null; }
      else params[s.name] = decodeParam(pathSegs[i]);
    }
    return pathSegs.length === segs.length ? params : null;
  };
  const resolveRouteHandler = (handlers, method) => {
    if (handlers.ANY !== undefined) return handlers.ANY;
    if (handlers[method] !== undefined) return handlers[method];
    if (method === "HEAD" && handlers.GET !== undefined) return handlers.GET;  // implicit HEAD via GET
    return undefined;
  };
  function compileRoutes(routesObj) {
    const list = [];
    const keys = Object.keys(routesObj);
    for (let idx = 0; idx < keys.length; idx++) {
      const pattern = keys[idx];
      const value = routesObj[pattern];
      if (value === false) continue;                   // `false` skips the route (falls through to fetch)
      if (value === null || value === undefined) continue;
      let handlers;
      if (typeof value === "function" || isResponseLike(value)) handlers = { ANY: value };
      else if (typeof value === "object") {
        handlers = {}; let any = false;
        for (const mk of Object.keys(value)) {
          const mv = value[mk];
          if (typeof mv === "function" || isResponseLike(mv)) { handlers[String(mk).toUpperCase()] = mv; any = true; }
          else throw new Error(ROUTES_RECORD_ERR);
        }
        if (!any) throw new Error(ROUTES_RECORD_ERR);
      } else throw new Error(ROUTES_RECORD_ERR);
      const segs = parseRoutePattern(pattern);          // may throw param-validation errors
      list.push({ segs, handlers, score: segs.map((s) => s.kind), idx });
    }
    list.sort((a, b) => { const c = cmpScore(a.score, b.score); return c !== 0 ? c : a.idx - b.idx; });
    return {
      count: list.length,
      match(path, method) {
        const pathSegs = pathToSegs(path);
        for (const r of list) {
          const params = matchSegs(r.segs, pathSegs);
          if (!params) continue;
          const h = resolveRouteHandler(r.handlers, method);
          if (h !== undefined) return { handler: h, params };  // method miss falls through to next candidate
        }
        return null;
      },
    };
  }

  // ---- Bun.serve native transport (T-LOOP: epoll event loop + Http1Server) ---
  // When __mbunServeNative exists (Linux), HTTP parsing/framing/keep-alive/
  // backpressure run natively on the shared epoll loop (mbun.runtime_server.
  // Http1Server in async mode); this layer maps parsed requests to Request
  // objects, runs routes/fetch handlers, and answers with raw pre-framed bytes
  // through the same writeHttpResponse serializer (direct streams included).
  const NSRV = SN ? { servers: new Map(), item: null } : null;
  // Established connections still attached to a server (0 once nothing can reach
  // it). Older native layers have no such binding; treating that as 0 keeps the
  // previous retire-immediately behaviour rather than leaking the entry.
  const serveConnections = (serverId) => {
    if (!SN || typeof SN.connections !== "function") return 0;
    try { return SN.connections(serverId) | 0; } catch (e) { return 0; }
  };
  const ensureServeReactor = () => {
    if (NSRV.item) return;
    const item = {
      _poll() {
        let n = 0;
        try { n += SN.tick(0) | 0; } catch (e) { return 0; }
        for (;;) {
          const ev = SN.next();
          if (!ev) break;
          n++;
          const dispatch = NSRV.servers.get(ev.server);
          if (!dispatch) { if (ev.type === "request") { try { SN.abort(ev.server, ev.id); } catch (e) {} } continue; }
          // A throwing dispatch (e.g. an unparsable Host header while building
          // the Request) must not unregister the shared reactor item.
          try { dispatch(ev); }
          catch (e) { if (ev.type === "request") { try { SN.abort(ev.server, ev.id); } catch (e2) {} } }
        }
        return n;
      },
      _fail() {},
    };
    NSRV.item = item;
    NET.items.add(item);
  };

  // The reactor item is shared by every native server, so it must be retired
  // once the last one is gone. A member of NET.items counts as "held" work in
  // process_web's park calculation, so a leaked item makes an otherwise idle
  // process park for LONG_PARK (60s) per loop iteration instead of exiting --
  // `Bun.serve(...); server.stop()` looked like a hang (bun exits immediately).
  const maybeRetireServeReactor = () => {
    if (!NSRV || !NSRV.item) return;
    if (NSRV.servers.size !== 0) return;
    NET.items.delete(NSRV.item);
    NSRV.item = null;
  };

  function serveNativeImpl(opts, compiledRoutes, hostname, displayHost, wantPort) {
    let lh;
    try { lh = SN.listen(hostname, wantPort); }
    catch (e) { throw mkErr("Failed to start server. Is port " + wantPort + " in use?", "EADDRINUSE"); }
    const serverId = lh.id;
    const conns = new Map();  // reqId -> shim socket of an in-flight response
    let stopped = false;
    // ref-count into NET.serveActive: a listening (ref'd) server holds the
    // event loop open (bun: process stays alive until stop()/unref()).
    let refd = true;
    const handlerRef = { fetch: opts.fetch, error: opts.error, routes: compiledRoutes, ws: opts.websocket,
                         maxRequestBodySize: (typeof opts.maxRequestBodySize === "number" && opts.maxRequestBodySize > 0)
                                               ? opts.maxRequestBodySize : 0 };
    // A bare IPv6 literal must be bracketed inside a URL authority ("[::1]"),
    // but server.hostname stays the raw form ("::1"). ref bun ServerConfig.
    const urlHost = isIPv6(displayHost) ? "[" + displayHost + "]" : displayHost;
    const serverObj = {
      port: lh.port,
      hostname: displayHost,
      development: !!opts.development,
      id: opts.id || "",
      pendingRequests: 0,
      pendingWebSockets: 0,
      url: new G.URL("http://" + urlHost + ":" + lh.port + "/"),
      protocol: "http",
      fetch(req) {
        if (typeof req !== "string" && (req === null || typeof req !== "object"))
          return Promise.reject(new TypeError("fetch() expects a string, but received " +
            (req === undefined ? "Undefined" : req === null ? "Null"
             : typeof req === "boolean" ? "Boolean" : typeof req === "number" ? "Number"
             : typeof req === "symbol" ? "Symbol" : typeof req === "bigint" ? "BigInt" : "Object")));
        if (typeof handlerRef.fetch !== "function")
          return Promise.reject(new Error("fetch() requires the server to have a fetch handler"));
        const url = typeof req === "string"
          ? new G.URL(req, "http://" + urlHost + ":" + lh.port + "/").href
          : req.url;
        const r = typeof req === "string" ? new G.Request(url) : req;
        return Promise.resolve(handlerRef.fetch.call(serverObj, r, serverObj)).then((res) => {
          if (res && typeof res === "object") {
            try { Object.defineProperty(res, "url", { value: url, configurable: true, writable: true }); } catch (e) {}
          }
          return res;
        });
      },
      // RFC6455 upgrade (T-LOOP.4): handshake + native raw-tunnel takeover;
      // the WS framing/handler layer lives in mbun.jsc.js_websocket.
      upgrade(req, opts) {
        const WS = G.__mbunWS;
        if (!WS || !req || req.__mbunUpgraded) return false;
        const id = req.__mbunReqId;
        if (id === undefined || !conns.has(id)) return false;
        const key = req.headers.get("sec-websocket-key");
        if (!key || String(req.headers.get("upgrade") || "").toLowerCase() !== "websocket") return false;
        if (!handlerRef.ws || typeof handlerRef.ws !== "object")
          throw new Error('Bun.serve(): To enable websocket support, set the "websocket" object in Bun.serve({})');
        const sock = conns.get(id);
        // Mirror `upgrader.cookies.take()`: only cookies the handler *set* ride
        // the 101. Probe the descriptor instead of reading req.cookies — reading
        // it would trip the lazy getter and build a CookieMap for a handler that
        // never asked for one (bun's take() would have been None there).
        let _ck;
        const _cd = Object.getOwnPropertyDescriptor(req, "cookies");
        if (_cd && "value" in _cd && _cd.value && typeof _cd.value.toSetCookieHeaders === "function") {
          const _h = _cd.value.toSetCookieHeaders();
          if (_h && _h.length) _ck = _h;
        }
        const ws = WS.serverUpgrade({
          key, protocol: req.headers.get("sec-websocket-protocol") || "",
          headers: opts && opts.headers, setCookies: _ck,
          extensions: req.headers.get("sec-websocket-extensions") || "",
          perMessageDeflate: !!handlerRef.ws.perMessageDeflate,
          data: opts ? opts.data : undefined,
          // ServerWebSocket.remoteAddress is the upgraded socket's peer — the
          // same endpoint requestIP() reports for the upgrade request, since
          // the 101 reuses that connection. ref: bun
          // src/runtime/server/ServerWebSocket.rs (getRemoteAddress →
          // us_socket_remote_address on the upgraded socket).
          remoteAddress: req.__mbunRemote ? req.__mbunRemote.address : undefined,
          server: serverObj, handlers: handlerRef.ws,
          write: (bytes) => { try { SN.write(serverId, id, u8(bytes)); } catch (e) {} },  // typed array: no base64 round-trip
          detach: () => { try { return SN.detach(serverId, id); } catch (e) { return false; } },
          abort: () => { try { SN.abort(serverId, id); } catch (e) {} },
          onCleanup: () => { conns.delete(id); serverObj.pendingWebSockets--; },
        });
        if (!ws) return false;
        req.__mbunUpgraded = true;
        sock._ws = ws;
        serverObj.pendingWebSockets++;
        return true;
      },
      publish(topic, data, compress) { const WS = G.__mbunWS; return WS ? WS.publish(serverObj, topic, data) : 0; },
      subscriberCount(topic) { const WS = G.__mbunWS; return WS ? WS.subscriberCount(serverObj, topic) : 0; },
      // ref: bun src/runtime/server/mod.rs requestIP → getRemoteSocketInfo:
      // a fresh SocketAddress off the request's socket, or null when there is
      // no live peer (already-responded/closed request). The address is the
      // verbatim getpeername form, so a v4 client on the dual-stack default
      // listener reports { "::ffff:127.0.0.1", "IPv6" } — see PeerAddress.
      requestIP(req) {
        const r = req && req.__mbunRemote;
        return r ? { address: r.address, family: r.family, port: r.port } : null;
      },
      timeout() { return serverObj; },
      ref() { if (!refd && !stopped) { refd = true; NET.serveActive++; } return serverObj; },
      unref() { if (refd) { refd = false; NET.serveActive--; } return serverObj; },
      reload(o) {
        if (o && typeof o === "object") {
          if (typeof o.fetch === "function") handlerRef.fetch = o.fetch;
          if (o.error !== undefined) handlerRef.error = o.error;
          if (o.websocket !== undefined) handlerRef.ws = o.websocket;
          const nr = o.routes !== undefined ? o.routes : (("static" in o) ? o.static : undefined);
          if (o.routes !== undefined || ("static" in o)) handlerRef.routes = (nr && typeof nr === "object") ? compileRoutes(nr) : null;
        }
        return serverObj;
      },
      stop(force) {
        if (!stopped) { stopped = true; if (refd) { refd = false; NET.serveActive--; } }
        try { SN.stop(serverId, !!force); } catch (e) {}
        if (force) {
          NSRV.servers.delete(serverId);
          for (const s of Array.from(conns.values())) { if (s._ws) s._ws._closed(); s._emitClose(); }
          conns.clear();
        } else if (conns.size === 0 && serveConnections(serverId) === 0) {
          // Only retire the dispatch entry once nothing can reach the server.
          // After a soft stop the listen socket is gone but established
          // connections keep serving (bun mod.rs:1551), and a request arriving
          // on one of them is aborted if its entry is missing.
          NSRV.servers.delete(serverId);
        }
        maybeRetireServeReactor();
        return Promise.resolve();
      },
    };
    serverObj[Symbol.dispose] = () => serverObj.stop(true);
    serverObj[Symbol.asyncDispose] = () => serverObj.stop(true);

    const handleError = (e) => {
      if (typeof handlerRef.error === "function") {
        try { return handlerRef.error.call(serverObj, e); } catch (e2) { e = e2; }
      }
      return new G.Response("Internal Server Error\n" + String((e && e.stack) || e), { status: 500, headers: { "content-type": "text/plain" } });
    };

    // Socket shim over the native connection: writeHttpResponse's contract
    // (write/end/destroy/once("close")/writableLength) maps onto raw response
    // bytes + finish/abort on the request id.
    const mkSock = (id) => ({
      destroyed: false, _ended: false, _closeCbs: [],
      write(d) { if (this.destroyed) return true; const b = u8(d); try { SN.write(serverId, id, b); } catch (e) {} return true; },
      end() { this._ended = true; return this; },
      destroy() { if (!this.destroyed) { this.destroyed = true; conns.delete(id); try { SN.abort(serverId, id); } catch (e) {} } return this; },
      once(n, cb) { if (n === "close") this._closeCbs.push(cb); return this; },
      on(n, cb) { return this.once(n, cb); },
      removeListener() { return this; },
      _emitClose() { this.destroyed = true; const cbs = this._closeCbs; this._closeCbs = []; for (const cb of cbs) { try { cb(); } catch (e) {} } },
      _finish(keepAlive) { if (this.destroyed) return; try { SN.finish(serverId, id, !!(keepAlive && !this._ended)); } catch (e) {} if (this._ended) this.destroyed = true; },
      get writableLength() { try { return SN.backpressure(serverId, id) | 0; } catch (e) { return 0; } },
    });

    const bodies = new Map();  // reqId -> mkBodyStream state (streaming request bodies)
    const dropBody = (id, err) => {
      const b = bodies.get(id);
      if (!b) return;
      bodies.delete(id);
      try { if (err) b.error(err); else b.close(); } catch (e) {}
    };
    const dispatch = (ev) => {
      if (ev.type === "aborted") {
        dropBody(ev.id, mkErr("The socket connection was closed unexpectedly", "ECONNRESET"));
        const s = conns.get(ev.id);
        if (s) { conns.delete(ev.id); if (s._ws) s._ws._closed(); s._emitClose(); }
        return;
      }
      if (ev.type === "body") {
        // Upgraded (WebSocket) connections tunnel raw bytes as body events.
        const c = conns.get(ev.id);
        if (c && c._ws) { if (ev.chunk) c._ws._feed(fromB64(ev.chunk)); return; }
        const b = bodies.get(ev.id);
        if (b) {
          if (ev.chunk) { try { b.push(fromB64(ev.chunk)); } catch (e) {} }
          if (ev.done) dropBody(ev.id, null);
        }
        return;
      }
      const sock = mkSock(ev.id);
      conns.set(ev.id, sock);
      const hdrs = ev.headers || [];
      let hostHdr = null, connHdr = "";
      for (let i = 0; i + 1 < hdrs.length; i += 2) {
        const lk = String(hdrs[i]).toLowerCase();
        if (lk === "host" && hostHdr === null) hostHdr = hdrs[i + 1];
        else if (lk === "connection") connHdr = String(hdrs[i + 1]).toLowerCase();
      }
      const keepAlive = (ev.minor | 0) >= 1 ? connHdr.indexOf("close") === -1 : connHdr.indexOf("keep-alive") !== -1;
      const tgt = splitTarget(ev.path);
      // request.signal aborts when the client disconnects before the response
      // completes (bun RequestContext.on_abort → JS AbortSignal).
      const reqCtl = new G.AbortController();
      sock.once("close", () => { try { reqCtl.abort(); } catch (e) {} });
      // request.url is the URL-normalized request target (dot-segment
      // resolution, fragment kept) — bun-server "normlizes incoming request
      // URLs" asserts new URL(path, base).href equivalence.
      let reqUrl = "http://" + (hostHdr || (displayHost + ":" + serverObj.port)) + tgt.path + tgt.query;
      try { reqUrl = new G.URL(reqUrl).href; } catch (e) {}
      // Streaming request body (dispatch-on-headers): a request that declares
      // body framing gets a live ReadableStream fed by subsequent body events;
      // bodiless requests keep body: null (req.body === null, bun semantics).
      let bodyStream = null;
      // maxRequestBodySize: a declared Content-Length over the limit is refused
      // with a bodiless 413 and the handler never runs (issue 22353).
      let tooLarge = false;
      {
        let hasBody = false;
        for (let i = 0; i + 1 < hdrs.length; i += 2) {
          const lk = String(hdrs[i]).toLowerCase();
          if (lk === "transfer-encoding" && String(hdrs[i + 1]).toLowerCase().indexOf("chunked") !== -1) hasBody = true;
          else if (lk === "content-length" && +hdrs[i + 1] > 0) {
            hasBody = true;
            if (handlerRef.maxRequestBodySize > 0 && +hdrs[i + 1] > handlerRef.maxRequestBodySize) tooLarge = true;
          }
        }
        if (hasBody) { bodyStream = mkBodyStream(); bodies.set(ev.id, bodyStream); }
      }
      const req = new G.Request(reqUrl, {
        method: ev.method,
        body: bodyStream ? bodyStream.stream : (ev.body ? fromB64(ev.body) : null),
        signal: reqCtl.signal,
      });
      req.__mbunReqId = ev.id;  // server.upgrade(req) resolves its connection
      // Peer endpoint captured natively at accept (getpeername). Carried on the
      // Request because that is exactly what server.requestIP(req) is keyed by;
      // absent when the socket had no reportable peer → requestIP returns null.
      if (ev.remoteAddress !== undefined)
        req.__mbunRemote = { address: ev.remoteAddress, family: ev.remoteFamily, port: ev.remotePort | 0 };
      // req.cookies — lazy, like bun: the CookieMap is only materialized when a
      // handler actually touches it (upgrader.cookies stays None otherwise, so
      // an untouched request writes no Set-Cookie). ref: server_body.rs
      // `let mut cookies_to_write = upgrader.cookies.take();`
      const roThrow = () => { throw new TypeError("Attempted to assign to readonly property."); };
      Object.defineProperty(req, "cookies", {
        configurable: true, enumerable: false,
        get() {
          const cm = new G.Bun.CookieMap(String(req.headers.get("cookie") || ""));
          // readonly cache in VALUE form (the Set-Cookie writer below checks
          // `"value" in descriptor` to pull toSetCookieHeaders()); writable:false.
          Object.defineProperty(req, "cookies", { value: cm, configurable: true, enumerable: false, writable: false });
          return cm;
        },
        // bun: req.cookies is readonly — assigning before materialization throws
        // in strict AND sloppy mode (a bare writable:false is silent when sloppy).
        set: roThrow,
      });
      for (let i = 0; i + 1 < hdrs.length; i += 2) req.headers.append(hdrs[i], hdrs[i + 1]);
      serverObj.pendingRequests++;
      const matched = handlerRef.routes ? handlerRef.routes.match(tgt.path, ev.method) : null;
      req.params = matched ? matched.params : {};
      let out;
      if (tooLarge) {
        out = new G.Response(null, { status: 413 });
      } else if (matched) {
        const h = matched.handler;
        if (typeof h === "function") { try { out = h.call(serverObj, req, serverObj); } catch (e) { out = handleError(e); } }
        else out = (h && typeof h.clone === "function") ? h.clone() : h;   // static Response (clone per request)
      } else if (typeof handlerRef.fetch === "function") {
        try { out = handlerRef.fetch.call(serverObj, req, serverObj); }
        catch (e) { out = handleError(e); }
      } else {
        out = new G.Response("", { status: 404 });   // routes-only server, no match
      }
      const finish = (res) => {
        // Auto-apply cookies the handler set via req.cookies (bun server_body.rs:
        // `cookies_to_write = upgrader.cookies.take()`). Probe the descriptor so an
        // untouched request — whose lazy getter never ran — writes no Set-Cookie.
        try {
          const _cd = Object.getOwnPropertyDescriptor(req, "cookies");
          if (_cd && "value" in _cd && _cd.value && typeof _cd.value.toSetCookieHeaders === "function"
              && res && res.headers && typeof res.headers.append === "function") {
            for (const c of _cd.value.toSetCookieHeaders()) res.headers.append("Set-Cookie", c);
          }
        } catch (e) {}
        writeHttpResponse(sock, res, ev.method, keepAlive && !sock.destroyed, () => {
          serverObj.pendingRequests--;
          conns.delete(ev.id);
          // Responded before the body finished: the native side closes the
          // connection; error the leftover body stream (bun: read-after-
          // respond on an aborted request rejects).
          dropBody(ev.id, mkErr("The socket connection was closed unexpectedly", "ECONNRESET"));
          sock._finish(keepAlive);
          // Soft-stopped and drained: retire the server, but only once no
          // established connection can still deliver another request on it
          // (bun defers teardown the same way — deinit_if_we_can, mod.rs:1584).
          if (stopped && conns.size === 0) {
            try { SN.stop(serverId, false); } catch (e) {}
            if (serveConnections(serverId) === 0) {
              NSRV.servers.delete(serverId);
              maybeRetireServeReactor();
            }
          }
        });
      };
      Promise.resolve(out).then(
        (res) => {
          if (req.__mbunUpgraded) { serverObj.pendingRequests--; return; }  // 101 already sent
          finish(res == null ? handleError(new Error("fetch() returned undefined")) : res);
        },
        (e) => {
          if (req.__mbunUpgraded) { serverObj.pendingRequests--; return; }
          Promise.resolve(handleError(e)).then(finish, () => finish(new G.Response("Internal Server Error", { status: 500 })));
        });
    };

    NSRV.servers.set(serverId, dispatch);
    NET.serveActive++;
    ensureServeReactor();
    return serverObj;
  }

  // ---- Bun.serve --------------------------------------------------------------
  if (G.Bun) {
    const BunG = G.Bun;
    BunG.serve = function serve(opts) {
      if (opts === null || opts === undefined || (typeof opts !== "object" && typeof opts !== "function"))
        throw new Error("Bun.serve expects an object");
      let tlsCfg = null;
      if (opts.tls !== undefined && opts.tls !== null && !(Array.isArray(opts.tls) && opts.tls.length === 0)) {
        const t = Array.isArray(opts.tls) ? opts.tls[0] : opts.tls;  // SNI multi-cert: first entry (rest DEFERRED)
        if (Array.isArray(opts.tls)) {
          for (let __i = 1; __i < opts.tls.length; __i++) {
            const __sni = opts.tls[__i];
            if (typeof __sni !== "object" || __sni === null ||
                typeof __sni.serverName !== "string" || __sni.serverName.length === 0)
              throw new Error("SNI tls object must have a serverName");
          }
        }
        if (typeof t !== "object" || t === null) throw new Error("TLSOptions must be an object");
        const pem = (v) => v == null ? "" : Array.isArray(v) ? v.map((x) => typeof x === "string" ? x : td.decode(u8(x))).join("\n") : typeof v === "string" ? v : td.decode(u8(v));
        tlsCfg = { cert: pem(t.cert || t.certFile), key: pem(t.key || t.keyFile), ca: pem(t.ca) };
        if (!tlsCfg.cert || !tlsCfg.key) throw new Error("Bun.serve: tls requires both 'cert' and 'key'");
      }
      // unix and hostname are mutually exclusive: bun's ServerConfig keeps a
      // UnixOrHost union. Coerce `unix` up front (a throwing toString / Symbol
      // rejects here, parity with bun's JSValue→string). A truthy hostname
      // combined with a unix path is an error; empty/absent hostname is fine.
      let unixPath = "";
      if (opts.unix !== undefined && opts.unix !== null) unixPath = String(opts.unix);
      if (unixPath) {
        const rawHost = (opts.hostname === undefined || opts.hostname === null) ? "" : String(opts.hostname);
        if (rawHost) throw new Error("Bun.serve: unix and hostname are mutually exclusive");
      }
      const routesRaw = opts.routes !== undefined ? opts.routes : opts.static;
      let compiledRoutes = null;
      if (routesRaw !== undefined && routesRaw !== null) {
        if (typeof routesRaw !== "object") throw new Error(ROUTES_NOT_OBJECT);
        compiledRoutes = compileRoutes(routesRaw);       // may throw route-validation errors
      }
      if (typeof opts.fetch !== "function" && (!compiledRoutes || compiledRoutes.count === 0))
        throw new Error(ROUTES_NEEDS_EITHER);
      // Default (no hostname) binds the IPv6 wildcard dual-stack, NOT 0.0.0.0.
      // PORT-SOURCE: bun packages/bun-usockets/src/bsd.c:1160-1180 —
      // bsd_create_listen_socket(host=NULL) resolves with AI_PASSIVE/AF_UNSPEC
      // and its loop *prefers the AF_INET6 result*, then bsd.c:1124-1131 sets
      // IPV6_V6ONLY only for LIBUS_SOCKET_IPV6_ONLY (unset here) → v4 clients
      // are accepted and surface v4-mapped ("::ffff:127.0.0.1", family IPv6).
      // An *explicit* hostname is honoured verbatim, so "localhost"/"0.0.0.0"
      // still bind AF_INET and report a plain IPv4 peer — verified against
      // 1.3.14: only the defaulted host yields the mapped form. displayHost
      // already folds "::" to "localhost", so server.hostname is unchanged.
      const hostname = opts.hostname ? String(opts.hostname) : "::";
      // Only the defaulted wildcard ("::") folds to "localhost"; an explicit
      // hostname (incl. "0.0.0.0") is reported verbatim. ref bun ServerConfig.
      const displayHost = hostname === "::" ? "localhost" : hostname;
      const wantPort = opts.port !== undefined && opts.port !== null ? +opts.port : (G.process && G.process.env && +G.process.env.PORT) || 0;
      // Native epoll transport (T-LOOP): parsing/keep-alive/backpressure in
      // mbun.runtime_server.Http1Server; falls back to the JS poll reactor
      // below when the native bridge is unavailable (non-Linux) or when TLS
      // terminates in the reactor (per-fd TLS channels wrap plain sockets).
      // Native epoll transport binds AF_INET (hostname:port); a unix listener
      // must go through the JS reactor's AF_UNIX bind (NN.listenUnix) below.
      if (SN && !tlsCfg && !unixPath) return serveNativeImpl(opts, compiledRoutes, hostname, displayHost, wantPort);
      let lh;
      if (unixPath) {
        try { lh = NN.listenUnix(unixPath); }
        catch (e) { throw mkErr("Failed to listen at " + unixPath + " (" + e.message + ")", "EADDRINUSE"); }
      } else {
        try { lh = NN.listen(hostname, wantPort); }
        catch (e) { throw mkErr("Failed to start server. Is port " + wantPort + " in use?", "EADDRINUSE"); }
      }
      const netServer = new Server({ allowHalfOpen: false });
      netServer._fd = lh.fd;
      netServer._addr = unixPath ? { address: unixPath, family: "unix" }
                                 : { port: lh.port, address: hostname, family: "IPv4" };
      netServer.listening = true;
      NET.items.add(netServer);
      const proto = tlsCfg ? "https" : "http";
      const handlerRef = { fetch: opts.fetch, error: opts.error, routes: compiledRoutes, ws: opts.websocket,
                         maxRequestBodySize: (typeof opts.maxRequestBodySize === "number" && opts.maxRequestBodySize > 0)
                                               ? opts.maxRequestBodySize : 0 };
      const urlHost = isIPv6(displayHost) ? "[" + displayHost + "]" : displayHost;
      // bun unlinks a unix socket file on stop (Node/libuv order: before closing
      // the fd) so a restart can re-bind the path. Abstract sockets (leading NUL)
      // have no filesystem entry. Already-gone is fine.
      const unlinkUnix = () => {
        if (!unixPath || unixPath[0] === "\0") return;
        try { (M["fs"] || M["node:fs"]).unlinkSync(unixPath); } catch (e) {}
      };
      let urlCache;
      const serverObj = {
        port: unixPath ? undefined : lh.port,
        hostname: unixPath ? undefined : displayHost,
        unix: unixPath || undefined,
        address: unixPath || undefined,
        development: !!opts.development,
        id: opts.id || "",
        pendingRequests: 0,
        pendingWebSockets: 0,
        // Lazy, like bun's Server.url getter (server.classes.ts): a unix path
        // that does not make a parseable URL ("unix://[object Bun]") must throw
        // when `.url` is READ, not blow up inside Bun.serve() itself.
        get url() {
          if (urlCache === undefined) {
            urlCache = new G.URL(unixPath ? "unix://" + unixPath
                                          : proto + "://" + urlHost + ":" + lh.port + "/");
          }
          return urlCache;
        },
        protocol: proto,
        fetch(req) {
          if (typeof req !== "string" && (req === null || typeof req !== "object"))
            return Promise.reject(new TypeError("fetch() expects a string, but received " +
              (req === undefined ? "Undefined" : req === null ? "Null"
               : typeof req === "boolean" ? "Boolean" : typeof req === "number" ? "Number"
               : typeof req === "symbol" ? "Symbol" : typeof req === "bigint" ? "BigInt" : "Object")));
          if (typeof handlerRef.fetch !== "function")
            return Promise.reject(new Error("fetch() requires the server to have a fetch handler"));
          const url = typeof req === "string"
            ? new G.URL(req, proto + "://" + urlHost + ":" + lh.port + "/").href
            : req.url;
          const r = typeof req === "string" ? new G.Request(url) : req;
          return Promise.resolve(handlerRef.fetch.call(serverObj, r, serverObj)).then((res) => {
            if (res && typeof res === "object") {
              try { Object.defineProperty(res, "url", { value: url, configurable: true, writable: true }); } catch (e) {}
            }
            return res;
          });
        },
        // ws/wss upgrade on the reactor path (native path has its own): the
        // shared RFC6455 layer takes the Socket over after the 101.
        upgrade(req, o) {
          const WS = G.__mbunWS;
          if (!WS || !req || req.__mbunUpgraded) return false;
          const sock = req.__mbunSock;
          if (!sock || sock.destroyed) return false;
          const key = req.headers.get("sec-websocket-key");
          if (!key || String(req.headers.get("upgrade") || "").toLowerCase() !== "websocket") return false;
          if (!handlerRef.ws || typeof handlerRef.ws !== "object")
            throw new Error('Bun.serve(): To enable websocket support, set the "websocket" object in Bun.serve({})');
          const ws = WS.serverUpgrade({
            key, protocol: req.headers.get("sec-websocket-protocol") || "",
            headers: o && o.headers, data: o ? o.data : undefined,
            extensions: req.headers.get("sec-websocket-extensions") || "",
            perMessageDeflate: !!handlerRef.ws.perMessageDeflate,
            server: serverObj, handlers: handlerRef.ws,
            write: (bytes) => { try { sock.write(u8(bytes)); } catch (e) {} },
            detach: () => { sock._httpParser = null; sock._wsTaken = true; return true; },
            abort: () => { try { sock.destroy(); } catch (e) {} },
            onCleanup: () => { serverObj.pendingWebSockets--; },
          });
          if (!ws) return false;
          req.__mbunUpgraded = true;
          sock._ws = ws;
          sock.once("close", () => { if (sock._ws) sock._ws._closed(); });
          serverObj.pendingWebSockets++;
          return true;
        },
        publish(topic, data, compress) { const WS = G.__mbunWS; return WS ? WS.publish(serverObj, topic, data) : 0; },
        subscriberCount(topic) { const WS = G.__mbunWS; return WS ? WS.subscriberCount(serverObj, topic) : 0; },
        requestIP() { return { address: "127.0.0.1", family: "IPv4", port: 0 }; },
        timeout() { return serverObj; },
        ref() { return serverObj; },
        unref() { return serverObj; },
        reload(o) {
          if (o && typeof o === "object") {
            if (typeof o.fetch === "function") handlerRef.fetch = o.fetch;
            if (o.error !== undefined) handlerRef.error = o.error;
            if (o.websocket !== undefined) handlerRef.ws = o.websocket;
            const nr = o.routes !== undefined ? o.routes : (("static" in o) ? o.static : undefined);
            if (o.routes !== undefined || ("static" in o)) handlerRef.routes = (nr && typeof nr === "object") ? compileRoutes(nr) : null;
          }
          return serverObj;
        },
        stop(force) {
          unlinkUnix();
          netServer.close();
          for (const s of Array.from(netServer._conns)) { if (force || !s._httpBusy) s.destroy(); }
          return Promise.resolve();
        },
      };
      serverObj[Symbol.dispose] = () => serverObj.stop(true);
      serverObj[Symbol.asyncDispose] = () => serverObj.stop(true);

      const handleError = (e) => {
        if (typeof handlerRef.error === "function") {
          try { return handlerRef.error.call(serverObj, e); } catch (e2) { e = e2; }
        }
        return new G.Response("Internal Server Error\n" + String((e && e.stack) || e), { status: 500, headers: { "content-type": "text/plain" } });
      };

      netServer.on("connection", (sock) => {
        sock.on("error", () => {});
        if (tlsCfg) sock._startTls({ isServer: true, cert: tlsCfg.cert, key: tlsCfg.key, ca: tlsCfg.ca });
        let carry = [];  // bytes that arrive while a response is in flight
        let eofSeen = false;
        const startParser = () => {
          const parser = new HttpParser(false);
          sock._httpParser = parser;
          const bodyChunks = [];
          parser.onBody = (b) => bodyChunks.push(b.slice());
          parser.onError = () => sock.destroy();
          parser.onDone = () => {
            if (!parser.headDone) { sock.destroy(); return; }  // idle close
            sock._httpParser = null;
            carry = [parser.leftover()];
            const hostHdr = parser.headers["host"] || displayHost + ":" + serverObj.port;
            // Derive request.url from the Host header + the target's path/query,
            // dropping any spoofed authority from an absolute-form request target.
            const tgt = splitTarget(parser.target);
            // request.signal aborts on client disconnect before the response
            // completes (parity with the native transport path).
            const reqCtl = new G.AbortController();
            sock.once("close", () => { if (sock._httpBusy) { try { reqCtl.abort(); } catch (e) {} } });
            let reqUrl = proto + "://" + hostHdr + tgt.path + tgt.query;
            try { reqUrl = new G.URL(reqUrl).href; } catch (e) {}  // dot-segment normalization
            const req = new G.Request(reqUrl, {
              method: parser.method,
              body: bodyChunks.length ? concatU8(bodyChunks) : null,
              signal: reqCtl.signal,
            });
            req.__mbunSock = sock;  // server.upgrade(req) resolves its socket
            for (let i = 0; i < parser.rawHeaders.length; i += 2) req.headers.append(parser.rawHeaders[i], parser.rawHeaders[i + 1]);
            const connHdr = String(parser.headers["connection"] || "").toLowerCase();
            const keepAlive = parser.httpVersion === "1.1" ? connHdr.indexOf("close") === -1 : connHdr.indexOf("keep-alive") !== -1;
            serverObj.pendingRequests++;
            sock._httpBusy = true;
            const matched = handlerRef.routes ? handlerRef.routes.match(tgt.path, parser.method) : null;
            req.params = matched ? matched.params : {};
            let out;
            if (matched) {
              const h = matched.handler;
              if (typeof h === "function") { try { out = h.call(serverObj, req, serverObj); } catch (e) { out = handleError(e); } }
              else out = (h && typeof h.clone === "function") ? h.clone() : h;   // static Response (clone per request)
            } else if (typeof handlerRef.fetch === "function") {
              try { out = handlerRef.fetch.call(serverObj, req, serverObj); }
              catch (e) { out = handleError(e); }
            } else {
              out = new G.Response("", { status: 404 });   // routes-only server, no match
            }
            const finish = (res) => {
              writeHttpResponse(sock, res, parser.method, keepAlive && !sock.destroyed, () => {
                serverObj.pendingRequests--;
                sock._httpBusy = false;
                if (keepAlive && !sock.destroyed) { startParser(); pump(); }
              });
            };
            Promise.resolve(out).then(
              (res) => {
                if (req.__mbunUpgraded) {  // ws took the socket: feed handshake leftovers
                  serverObj.pendingRequests--; sock._httpBusy = false;
                  const pend = carry; carry = [];
                  for (const c of pend) if (c.length && sock._ws) sock._ws._feed(c);
                  return;
                }
                finish(res == null ? handleError(new Error("fetch() returned undefined")) : res);
              },
              (e) => {
                if (req.__mbunUpgraded) { serverObj.pendingRequests--; sock._httpBusy = false; return; }
                Promise.resolve(handleError(e)).then(finish, () => finish(new G.Response("Internal Server Error", { status: 500 })));
              });
          };
          const pump = () => {
            const p = sock._httpParser;
            if (!p) return;
            const pend = carry;
            carry = [];
            for (const c of pend) if (c.length) p.push(c);
            if (eofSeen && sock._httpParser === p && !p.done) p.eof();
          };
          pump();
        };
        sock.on("data", (chunk) => {
          const b = u8(chunk);
          if (sock._ws) { sock._ws._feed(b); return; }  // upgraded: raw ws frames
          const p = sock._httpParser;
          if (p && !p.done) p.push(b);
          else carry.push(b.slice());
        });
        sock.on("end", () => { eofSeen = true; const p = sock._httpParser; if (p && !p.done) p.eof(); });
        startParser();
      });
      return serverObj;
    };
  }

  // ---- node:http server transport (createServer over net.Server) -------------
  // The message classes are node's own ports in builtins/node_http.cppm. That
  // partition is evaluated before this file installs net, so it cannot make its
  // Server a net.Server -- the split is one of duties, not a shadow copy:
  // node_http owns the IncomingMessage / ServerResponse / OutgoingMessage
  // contract (header store, _storeHeader framing, writeHead validation,
  // trailers, write-after-end errors), this file owns the socket and the parser
  // loop. There is exactly one ServerResponse class in the process, so `res`
  // here is a real OutgoingMessage rather than a look-alike.
  const HTTPMOD = M["http"] || {};
  const IncomingMessage = HTTPMOD.IncomingMessage;
  const ServerResponse = HTTPMOD.ServerResponse;
  const continueExpression = /(?:^|\W)100-continue(?:$|\W)/i;

  // `baseSrv` lets node:https reuse this whole layer: https.Server is a
  // tls.Server carrying lib/_http_server.js's _connectionListener, so the only
  // differences are which object is decorated and whether the message stream
  // arrives as 'connection' (plaintext) or 'secureConnection' (a TLSSocket).
  // Every per-connection http bookkeeping therefore hangs off `srv._httpConns`
  // rather than net.Server's `_conns`: for https those are two different objects
  // (the TLSSocket vs. the raw transport), and sweeping `_conns` would inspect
  // sockets that carry none of the http state — closeIdleConnections() would
  // read `_httpInFlight === undefined` on every raw socket and destroy the
  // in-flight connection out from under a handler that called close().
  function createHttpServer(o, handler, baseSrv) {
    if (typeof o === "function") { handler = o; o = {}; }
    o = o || {};
    const srv = baseSrv || new Server({ allowHalfOpen: false });
    const connEvent = baseSrv ? "secureConnection" : "connection";
    srv._httpConns = new Set();
    const ResponseClass = typeof o.ServerResponse === "function" ? o.ServerResponse : ServerResponse;
    const RequestClass = typeof o.IncomingMessage === "function" ? o.IncomingMessage : IncomingMessage;
    // lib/_http_server.js storeHTTPOptions.
    const HI = G.__mbunHttpInternals || {};
    const vInt = HI.validateInteger || (() => {});
    const vBool = HI.validateBoolean || (() => {});
    const oor = HI.ERR_OUT_OF_RANGE || ((name, range, v) => { const e = new RangeError(name); e.code = "ERR_OUT_OF_RANGE"; return e; });
    const opt = (name, dflt, validate) => {
      const v = o[name];
      if (v === undefined) return dflt;
      (validate || vInt)(v, name, 0);
      return v;
    };
    srv.timeout = 0;
    srv.requestTimeout = opt("requestTimeout", 300000);
    srv.headersTimeout = o.headersTimeout === undefined
      ? Math.min(60000, srv.requestTimeout) : opt("headersTimeout", 60000);
    if (srv.requestTimeout > 0 && srv.headersTimeout > 0 && srv.headersTimeout > srv.requestTimeout) {
      throw oor("headersTimeout", "<= requestTimeout", o.headersTimeout);
    }
    srv.keepAliveTimeout = opt("keepAliveTimeout", 5000);
    srv.keepAliveTimeoutBuffer = opt("keepAliveTimeoutBuffer", 1000);
    srv.connectionsCheckingInterval = opt("connectionsCheckingInterval", 30000);
    srv.maxHeadersCount = null;
    srv.maxRequestsPerSocket = 0;
    srv.requireHostHeader = o.requireHostHeader === undefined ? true : (vBool(o.requireHostHeader, "options.requireHostHeader"), o.requireHostHeader);
    srv.rejectNonStandardBodyWrites = !!o.rejectNonStandardBodyWrites;
    if (o.maxHeaderSize !== undefined) vInt(o.maxHeaderSize, "maxHeaderSize", 0);
    srv.maxHeaderSize = o.maxHeaderSize;
    // lib/_http_server.js setupConnectionsTracking: an unref'd sweeper that
    // expires connections which blew past headersTimeout / requestTimeout. The
    // 408 it produces is what the server-*-timeout-* corpus asserts.
    const sweep = () => {
      if (srv.headersTimeout === 0 && srv.requestTimeout === 0) return;
      const now = Date.now();
      for (const s of Array.from(srv._httpConns)) {
        // An idle keep-alive connection has no message in flight, so neither
        // clock is running (llhttp starts them at on_message_begin).
        if (s.destroyed || s._httpMsgIdle) continue;
        const started = s._httpMsgStart || 0;
        if (!started) continue;
        const headersLate = srv.headersTimeout > 0 && !s._httpHeadersDone &&
                            (now - started) > srv.headersTimeout;
        const requestLate = srv.requestTimeout > 0 &&
                            (now - started) > srv.requestTimeout;
        if (headersLate || requestLate) onRequestTimeout(s);
      }
    };
    const onRequestTimeout = (sock) => {
      const err = new Error("Request timeout");
      err.code = "ERR_HTTP_REQUEST_TIMEOUT";
      if (!srv.emit("clientError", err, sock)) {
        if (sock.writable && sock.bytesWritten === 0) {
          try { sock.end("HTTP/1.1 408 Request Timeout\r\nConnection: close\r\n\r\n"); return; } catch (e) {}
        }
        sock.destroy();
      }
    };
    srv.on("listening", () => {
      if (srv._httpSweeper) G.clearInterval(srv._httpSweeper);
      const every = srv.connectionsCheckingInterval > 0 ? srv.connectionsCheckingInterval : 30000;
      srv._httpSweeper = G.setInterval(sweep, every);
      if (srv._httpSweeper && typeof srv._httpSweeper.unref === "function") srv._httpSweeper.unref();
    });
    srv.setTimeout = function (msecs, cb) {
      this.timeout = msecs;
      if (typeof cb === "function") this.on("timeout", cb);
      for (const s of Array.from(this._httpConns)) { try { s.setTimeout(msecs); } catch (e) {} }
      return this;
    };
    // node http.Server surface (bun _http_server.ts:364 server.stop(true) /
    // :386 closeIdleConnections): destroy every / every idle tracked socket.
    srv.closeAllConnections = function () { for (const s of Array.from(this._httpConns)) { try { s.destroy(); } catch (e) {} } };
    srv.closeIdleConnections = function () {
      for (const s of Array.from(this._httpConns)) {
        // Idle = the incoming request message has completed, which is what
        // node's native ConnectionsList tracks (last_message_start <=
        // last_message_end, driven by llhttp). Deliberately NOT keyed on the
        // response: a handler calling server.close() right after res.end()
        // still runs inside on_headers_complete, before its own request
        // message completes, and must keep that connection alive
        // (test-http-server-unconsume).
        if (!s._httpInFlight) { try { s.destroy(); } catch (e) {} }
      }
    };
    // lib/_http_server.js Server.prototype.close -> httpServerPreClose ->
    // closeIdleConnections(). Without it a keep-alive connection outlives
    // close() and pins the event loop until the keep-alive timer fires, which
    // is the difference between a test finishing and a test timing out.
    const netClose = srv.close;
    srv.close = function (...args) {
      this.closeIdleConnections();
      if (this._httpSweeper) { G.clearInterval(this._httpSweeper); this._httpSweeper = null; }
      return netClose.apply(this, args);
    };
    // node http.Server.listen fires its callback with (err, hostname, port),
    // not net.Server's zero-arg 'listening' (bun _http_server.ts:233
    // emitListeningNextTick -> emit("listening", null, hostname, port)). The
    // hostname is the bind host, defaulting to "localhost" (bun's Bun.serve
    // default), while address() keeps the wildcard "::" it actually bound.
    const netListen = srv.listen;
    srv.listen = function (...args) {
      let host = "localhost", cb = null;
      const a0 = args[0];
      if (a0 && typeof a0 === "object" && typeof a0 !== "function") {
        if (a0.host != null) host = String(a0.host);
      } else {
        for (let i = 1; i < args.length; i++) if (typeof args[i] === "string") host = args[i];
      }
      for (let i = args.length - 1; i >= 0; i--) {
        if (typeof args[i] === "function") { cb = args[i]; args[i] = undefined; break; }
      }
      if (typeof cb === "function") {
        this.once("listening", () => { const ad = this.address() || {}; cb.call(this, null, host, ad.port); });
      }
      return netListen.apply(this, args);
    };
    if (typeof handler === "function") srv.on("request", handler);

    srv.on(connEvent, (sock) => {
      // node lib/_http_server.js connectionListenerInternal: the socket learns
      // which server owns it (test-cluster-send-socket-to-worker-http-server
      // asserts it after handing a socket to a worker over IPC). connEvent is
      // 'secureConnection' for https, so this must stay on the dynamic name.
      sock.server = srv;
      sock.on("error", () => {});
      srv._httpConns.add(sock);
      sock.once("close", () => srv._httpConns.delete(sock));
      if (srv.timeout) { try { sock.setTimeout(srv.timeout); } catch (e) {} }
      sock.server = srv;
      sock._httpInFlight = 0;
      // headersTimeout / requestTimeout clocks (ConnectionsList's
      // last_message_start): reset whenever the connection is free to receive
      // the next message.
      sock._httpMsgStart = Date.now();
      sock._httpHeadersDone = false;
      sock._httpMsgIdle = false;
      // lib/_http_server.js socketOnTimeout: the request, the response and the
      // server each get a say; only if none of them claims the event does the
      // connection go away.
      sock.on("timeout", () => {
        const inFlightReq = sock._httpIncoming;
        const reqTimeout = inFlightReq && !inFlightReq.complete && inFlightReq.emit("timeout", sock);
        const res = sock._httpMessage;
        const resTimeout = res && res.emit("timeout", sock);
        const serverTimeout = srv.emit("timeout", sock);
        if (!reqTimeout && !resTimeout && !serverTimeout) sock.destroy();
      });
      let carry = [];
      let eofSeen = false;
      let requestsCount = 0;
      const outgoing = [];

      // Pipelined intake. push() runs the request handler synchronously, so the
      // parser can complete (and be replaced) in the middle of this loop: the
      // remaining chunks belong to the NEXT message and must be re-queued
      // behind the leftover onDone just put back, not fed to a finished parser
      // that drops them. (Feeding them was a silent byte loss that stalled a
      // long pipeline after ~1.7k requests.)
      const pumpCarry = () => {
        const p = sock._httpParser;
        if (!p) return;
        const pend = carry;
        carry = [];
        for (let i = 0; i < pend.length; i++) {
          const cur = sock._httpParser;
          if (!cur || cur.done) { carry = carry.concat(pend.slice(i)); return; }
          if (pend[i].length) cur.push(pend[i]);
        }
        if (eofSeen && sock._httpParser === p && !p.done) p.eof();
      };

      const startParser = () => {
        const parser = new HttpParser(false);
        if (typeof srv.maxHeadersCount === "number" && srv.maxHeadersCount > 0) {
          parser.maxHeaderPairs = srv.maxHeadersCount << 1;
        }
        if (typeof srv.maxHeaderSize === "number" && srv.maxHeaderSize > 0) parser.maxHeaderSize = srv.maxHeaderSize;
        sock._httpParser = parser;
        let im = null;
        let res = null;
        let upgraded = false;

        // lib/_http_server.js resOnFinish: dump an unread body, hand the socket
        // to the next queued response, and either close or re-arm the parser.
        const resOnFinish = () => {
          // node lib/_http_server.js resOnFinish publishes
          // 'http.server.response.finish' as its very first statement.
          if (onResponseFinishChannel.hasSubscribers) {
            onResponseFinishChannel.publish({ request: im, response: res, socket: sock, server: srv });
          }
          if (im && !im._consuming && !(im._readableState && im._readableState.resumeScheduled)) im._dump();
          if (sock._httpMessage === res) res.detachSocket(sock);
          sock._httpIncoming = null;
          G.queueMicrotask(() => { if (!res._closed) { res._closed = true; res.emit("close"); } });
          if (res._last) {
            if (typeof sock.destroySoon === "function") sock.destroySoon();
            else sock.end();
          } else if (outgoing.length) {
            const m = outgoing.shift();
            if (m) m.assignSocket(sock);
          } else if (!sock.destroyed) {
            // The connection is free again: restart both timeout clocks.
            sock._httpMsgStart = Date.now();
            sock._httpHeadersDone = false;
            sock._httpMsgIdle = true;
            // Idle keep-alive connection: arm the advertised keep-alive timeout
            // (plus node's buffer) so it cannot pin the loop forever.
            if (srv.keepAliveTimeout > 0 && typeof sock.setTimeout === "function") {
              try { sock.setTimeout(srv.keepAliveTimeout + srv.keepAliveTimeoutBuffer); } catch (e) {}
            }
            if (parser.done) { startParser(); pumpCarry(); }
            else parser._afterDone = () => { startParser(); pumpCarry(); };
          }
        };

        parser.onHead = () => {
          sock._httpInFlight = (sock._httpInFlight | 0) + 1;
          sock._httpHeadersDone = true;
          sock._httpMsgIdle = false;
          // A request is in flight: clear any armed keep-alive timeout, node
          // re-arms server.timeout instead (resetSocketTimeout).
          if (typeof sock.setTimeout === "function") { try { sock.setTimeout(srv.timeout || 0); } catch (e) {} }
          im = new RequestClass(sock);
          sock._httpIncoming = im;
          im.method = parser.method;
          im.url = parser.target;
          im.httpVersion = parser.httpVersion;
          const vp = String(parser.httpVersion).split(".");
          im.httpVersionMajor = +vp[0];
          im.httpVersionMinor = +vp[1];
          im.joinDuplicateHeaders = !!o.joinDuplicateHeaders;
          im._addHeaderLines(parser.rawHeaders, parser.rawHeaders.length);

          const hdrs = im.headers;
          const connTokens = String(hdrs["connection"] || "").toLowerCase().split(",").map((t) => t.trim());
          const isConnect = im.method === "CONNECT";
          // llhttp flags a request as an upgrade when it is a CONNECT or when
          // it carries both `Upgrade:` and `Connection: upgrade`; node then
          // hands the raw socket plus the bytes already past the head to the
          // 'upgrade'/'connect' listener and stops parsing this connection.
          if (isConnect || (hdrs["upgrade"] !== undefined && connTokens.indexOf("upgrade") !== -1)) {
            upgraded = true;
            im.upgrade = true;
            const head = G.Buffer ? G.Buffer.from(parser.leftover()) : parser.leftover();
            sock._httpParser = null;
            const ev = isConnect ? "connect" : "upgrade";
            if (srv.listenerCount(ev) > 0) srv.emit(ev, im, sock, head);
            else sock.destroy();
            return;
          }

          const keepAlive = (im.httpVersionMajor === 1 && im.httpVersionMinor === 1)
            ? connTokens.indexOf("close") === -1
            : connTokens.indexOf("keep-alive") !== -1;

          res = new ResponseClass(im, {
            highWaterMark: sock.writableHighWaterMark,
            rejectNonStandardBodyWrites: srv.rejectNonStandardBodyWrites,
          });
          res._keepAliveTimeout = srv.keepAliveTimeout;
          res._maxRequestsPerSocket = srv.maxRequestsPerSocket;
          res.shouldKeepAlive = keepAlive;
          res.req = im;
          if (sock._httpMessage) outgoing.push(res);
          else res.assignSocket(sock);
          res.on("finish", resOnFinish);

          // node lib/_http_server.js parserOnIncoming: 'http.server.request.start'
          // is published once the response object exists and before the request
          // is dispatched, so a subscriber can bind an AsyncLocalStorage context
          // that the handler then runs inside
          // (test-diagnostics-channel-http-server-start).
          if (onRequestStartChannel.hasSubscribers) {
            onRequestStartChannel.publish({ request: im, response: res, socket: sock, server: srv });
          }

          let handled = false;
          if (im.httpVersionMajor === 1 && im.httpVersionMinor === 1) {
            // RFC 7230 5.4: an HTTP/1.1 request without Host is a 400.
            if (srv.requireHostHeader && hdrs.host === undefined) {
              res.writeHead(400, ["Connection", "close"]);
              res.end();
              return;
            }
            const limitSet = typeof srv.maxRequestsPerSocket === "number" && srv.maxRequestsPerSocket > 0;
            if (limitSet) {
              requestsCount++;
              res.maxRequestsOnConnectionReached = srv.maxRequestsPerSocket <= requestsCount;
            }
            if (limitSet && srv.maxRequestsPerSocket < requestsCount) {
              handled = true;
              srv.emit("dropRequest", im, sock);
              res.writeHead(503);
              res.end();
            } else if (hdrs.expect !== undefined) {
              handled = true;
              if (continueExpression.test(hdrs.expect)) {
                res._expect_continue = true;
                if (srv.listenerCount("checkContinue") > 0) {
                  srv.emit("checkContinue", im, res);
                } else {
                  res.writeContinue();
                  srv.emit("request", im, res);
                }
              } else if (srv.listenerCount("checkExpectation") > 0) {
                srv.emit("checkExpectation", im, res);
              } else {
                res.writeHead(417);
                res.end();
              }
            }
          }
          if (!handled) srv.emit("request", im, res);
        };

        // Feed the readable buffer, don't fake the events: Readable turns these
        // into "data"/"end" once something actually reads, and until then the
        // body is buffered instead of dropped on the floor. bun does the same
        // (_http_incoming.ts:381 `if (chunk && !this._dumped) this.push(chunk)`).
        parser.onBody = (b) => { if (im && !im._dumped) im.push(G.Buffer ? G.Buffer.from(b.slice()) : b.slice()); };
        parser.onDone = () => {
          if (upgraded) return;
          if (!parser.headDone) { sock.destroy(); return; }
          sock._httpParser = null;
          // leftover() is a view over a parser we are about to drop; copying it
          // once per pipelined request made intake quadratic.
          carry.unshift(parser.leftover());
          // Request message complete -> this connection stops counting as
          // in-flight for closeIdleConnections (llhttp on_message_complete).
          if (sock._httpInFlight > 0) sock._httpInFlight--;
          // EOF: bun internal/http.ts:187 `self.push(null); self.complete = true`.
          if (im) { im.complete = true; im.push(null); }
          if (parser._afterDone) { const f = parser._afterDone; parser._afterDone = null; f(); }
        };
        parser.onError = (e) => {
          const err = e || mkErr("Parse Error", "HPE_INVALID_CONSTANT");
          // lib/_http_server.js socketOnError: the server's own answer to a
          // malformed request is a canned 400 (431 when the head overflowed),
          // and only when nobody claimed 'clientError'.
          if (!srv.emit("clientError", err, sock)) {
            if (sock.writable && sock.bytesWritten === 0) {
              const body = err.code === "HPE_HEADER_OVERFLOW"
                ? "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n"
                : "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
              try { sock.end(body); } catch (e2) { try { sock.destroy(); } catch (e3) {} }
            } else {
              sock.destroy();
            }
          }
        };
      };

      sock.on("data", (chunk) => {
        // First byte of the next message on an idle connection: llhttp's
        // on_message_begin, where headersTimeout/requestTimeout start counting.
        if (sock._httpMsgIdle) { sock._httpMsgIdle = false; sock._httpMsgStart = Date.now(); }
        const b = u8(chunk);
        const p = sock._httpParser;
        if (p && !p.done) p.push(b);
        else carry.push(b.slice());
      });
      sock.on("end", () => { eofSeen = true; const p = sock._httpParser; if (p && !p.done) p.eof(); });
      startParser();
      pumpCarry();
    });
    return srv;
  }
  // Copy DESCRIPTORS, not values: node:http's `maxHeaderSize` is a live
  // accessor over the process-wide limit, and Object.assign would freeze it
  // into a plain data property (setter never reaching the native global).
  // node http.Server is a constructor — `new http.Server([options][, requestListener])`
  // — and its historical bare-factory form works too. What was exported here was
  // net's Server class, so `http.Server(fn)` built a *net* server and fn became
  // its 'connection' listener: the handler was called with (socket), and `res`
  // was undefined. 27 corpus files died on res.write / res.statusCode /
  // req.client._events for exactly this reason. Route both call forms through
  // createHttpServer; `instanceof` keeps working because the object it returns
  // is a Server and the stand-in shares that prototype.
  function HttpServer(options, requestListener) { return createHttpServer(options, requestListener); }
  try {
    HttpServer.prototype = Server.prototype;
    Object.defineProperty(HttpServer, "name", { value: "Server", configurable: true });
    Object.defineProperty(HttpServer, "length", { value: 2, configurable: true });
  } catch (e) {}
  def(["http"], Object.assign(
    Object.defineProperties({}, Object.getOwnPropertyDescriptors(M["http"] || {})), {
    createServer: createHttpServer,
    Server: HttpServer, IncomingMessage, ServerResponse,
  }));
  // js_https_live.cppm builds https.Server out of this: an https.Server is a
  // tls.Server carrying lib/_http_server.js's _connectionListener, and the
  // listener is exactly what createHttpServer installs.
  G.__mbunHttpServerFactory = createHttpServer;

  // ---- real network fetch() ---------------------------------------------------
  // data:/blob: resolve locally; http:// goes over a real socket (Connection:
  // close, redirects followed up to 20 hops); https:// is an honest reject (TLS
  // DEFERRED). Error codes match bun: ConnectionRefused / ECONNRESET /
  // InvalidHTTPResponse.
  function mkBodyStream() {
    const st = { q: [], w: [], closed: false, err: null };
    const notify = () => {
      while (st.w.length && (st.q.length || st.closed || st.err)) {
        const w = st.w.shift();
        if (st.q.length) w.resolve({ value: st.q.shift(), done: false });
        else if (st.err) w.reject(st.err);
        else w.resolve({ value: undefined, done: true });
      }
    };
    st.push = (b) => { st.q.push(b); notify(); };
    st.close = () => { st.closed = true; notify(); };
    st.error = (e) => { st.err = e; notify(); };
    const read = () => {
      if (st.q.length) return Promise.resolve({ value: st.q.shift(), done: false });
      if (st.err) return Promise.reject(st.err);
      if (st.closed) return Promise.resolve({ value: undefined, done: true });
      const p = Promise.withResolvers();
      st.w.push(p);
      return p.promise;
    };
    st.stream = {
      locked: false,
      getReader() { return { read, releaseLock() {}, cancel() { return Promise.resolve(); }, closed: Promise.resolve() }; },
      cancel() { return Promise.resolve(); },
      [Symbol.asyncIterator]() { return { next: read }; },
    };
    return st;
  }

  function collectHeaders(init, input) {
    const out = [];
    const add = (k, v) => out.push([String(k), String(v)]);
    const from = (h) => {
      if (!h) return;
      // Our Headers keeps the original-case name in _names; bun's HTTPHeaderMap
      // serializes the given case on the wire (the JS forEach is spec-lowercased),
      // so emit _names[lk] rather than the lowercased iteration key.
      if (h._m instanceof Map && h._names instanceof Map) {
        for (const [lk, v] of h._m) {
          const name = h._names.get(lk) || lk;
          if (lk === "set-cookie" && Array.isArray(h._sc) && h._sc.length) {
            for (const c of h._sc) add(name, c);
          } else add(name, v);
        }
        return;
      }
      if (typeof h.forEach === "function" && !Array.isArray(h)) h.forEach((v, k) => add(k, v));
      else if (Array.isArray(h)) { for (const kv of h) add(kv[0], kv[1]); }
      else { for (const k of Object.keys(h)) add(k, h[k]); }
    };
    if (input && typeof input === "object" && input.headers) from(input.headers);
    from(init && init.headers);
    // Combine duplicate names before serialization (fetch header-list combine;
    // WebKit joins Cookie with "; ", others with ", ").
    const seen = new Map();
    const merged = [];
    for (const kv of out) {
      const lk = kv[0].toLowerCase();
      const at = seen.get(lk);
      if (at === undefined) { seen.set(lk, merged.length); merged.push(kv); }
      else merged[at][1] = merged[at][1] + (lk === "cookie" ? "; " : ", ") + kv[1];
    }
    return merged;
  }

  // ---- fetch keep-alive connection pool -------------------------------------
  // ref bun src/http/HTTPContext.rs:581 release_socket / :689 existing_socket.
  // Bun parks an established socket in a (hostname, port, ssl-config)-keyed hive
  // and hands it back to the next request for the same origin, so N sequential
  // fetches to one origin cost one TCP connection instead of N.
  //
  // One thing is shaped differently here, and the difference is forced: bun's
  // uSockets keeps polling a parked socket, so a peer FIN flips is_closed() and
  // existing_socket (HTTPContext.rs:790) skips the slot before ever handing it
  // out. Nothing polls a parked fd in this reactor -- the item leaves NET.items
  // when it is released -- so liveness is instead probed at checkout: an idle
  // keep-alive socket must have nothing to say, so read() == "" means alive,
  // null (EOF) or a throw means the peer went away, and unsolicited bytes mean
  // the stream desynced. The probe still races a FIN in flight, so a socket that
  // dies before the first response byte replays the request on a fresh
  // connection -- bun's allow_retry, which HTTPContext.rs:979 sets for exactly
  // and only pool-sourced sockets (:857 and :1040 clear it for fresh ones).
  const POOL_SIZE = 64;                // bun HTTPContext.rs:19 POOL_SIZE
  const MAX_KEEPALIVE_HOSTNAME = 128;  // bun HTTPContext.rs:20
  const POOL = new Map();              // key -> [{fd, tls}, ...], reused LIFO
  let poolCount = 0;

  // existing_socket keys on (hostname, port, interned ssl-config) and rejects a
  // lax socket for a strict caller (HTTPContext.rs:770 established_with_
  // reject_unauthorized). Folding verify+ca into the key gets the same exclusion
  // without an interning table: a socket can only ever be reused by a request
  // whose TLS terms are identical to the ones it was established under.
  // The key separators below must stay written as escapes, never as literal
  // NUL bytes in this file: runtime/engine.inc:817 marshals kNetJS with
  // JSStringCreateWithUTF8CString, which stops at the first NUL and would
  // silently truncate this whole JS layer -- leaving the load-only fetch stub
  // ("network requests are not implemented yet") installed instead.
  const poolKey = (host, port, secure, tlsVerify, tlsCa) =>
    host.toLowerCase() + "\u0000" + port + "\u0000" + (secure ? 1 : 0) + "\u0000" +
    (tlsVerify ? 1 : 0) + "\u0000" + tlsCa;

  const poolClose = (e) => {
    if (e.tls) { try { NN.tlsClose(e.fd); } catch (x) {} }
    try { NN.close(e.fd); } catch (x) {}
  };

  // bun existing_socket HTTPContext.rs:790 -- a closed or errored slot is
  // dropped and the scan continues; it is never handed to a caller.
  const poolTake = (key) => {
    const list = POOL.get(key);
    if (!list) return null;
    while (list.length) {
      const e = list.pop();
      poolCount--;
      let r;
      try { r = e.tls ? NN.tlsRead(e.fd) : NN.read(e.fd); }
      catch (x) { poolClose(e); continue; }     // hard error -> dead
      if (r !== "") { poolClose(e); continue; } // null = EOF, bytes = desync
      if (!list.length) POOL.delete(key);
      return e;
    }
    POOL.delete(key);
    return null;
  };

  // bun release_socket HTTPContext.rs:581 -- park only an established socket,
  // only for a hostname that fits the keepalive buffer, and only while the hive
  // has a slot free; anything else is closed.
  const poolPut = (key, host, fd, tls) => {
    if (host.length > MAX_KEEPALIVE_HOSTNAME || poolCount >= POOL_SIZE) return false;
    let list = POOL.get(key);
    if (!list) { list = []; POOL.set(key, list); }
    list.push({ fd, tls });
    poolCount++;
    return true;
  };

  function doFetch(url, init, depth, retryCount, noPool) {
    init = init || {};
    retryCount = retryCount | 0;
    url = String(url);
    if (url.slice(0, 5) === "data:") {
      const comma = url.indexOf(",");
      const meta = url.slice(5, comma), body = url.slice(comma + 1);
      const isB64 = /;base64$/i.test(meta);
      const text = isB64 ? (G.atob ? G.atob(body) : body) : decodeURIComponent(body);
      return Promise.resolve(new G.Response(text, { status: 200, headers: { "content-type": meta.replace(/;base64$/i, "") || "text/plain" } }));
    }
    const m = /^(https?):\/\/([^/:?#]+)(?::(\d+))?([^#]*)/.exec(url);
    if (!m) return Promise.reject(new TypeError("fetch() URL is invalid: " + url));
    const secure = m[1] === "https";
    const host = m[2];
    const port = m[3] ? +m[3] : (secure ? 443 : 80);
    // TLS verification: fetch(init.tls) mirrors bun — rejectUnauthorized:false
    // or a supplied ca; NODE_TLS_REJECT_UNAUTHORIZED=0 disables globally.
    const tlsOpt = init.tls || {};
    const tlsVerify = secure && tlsOpt.rejectUnauthorized !== false &&
      !(G.process && G.process.env && G.process.env.NODE_TLS_REJECT_UNAUTHORIZED === "0");
    const tlsCa = secure && tlsOpt.ca ? (Array.isArray(tlsOpt.ca) ? tlsOpt.ca.map((c) => typeof c === "string" ? c : td.decode(u8(c))).join("\n") : (typeof tlsOpt.ca === "string" ? tlsOpt.ca : td.decode(u8(tlsOpt.ca)))) : "";
    let pathq = m[4] || "/";
    if (pathq === "" || pathq[0] !== "/") pathq = "/" + pathq;
    // Collapse a leading run of slashes in the request target to one: a URL
    // like http://h//redirect has pathname "//redirect", but the origin-form
    // request target bun sends is "/redirect" (verified on the wire against
    // bun 1.4.0). Without this the origin sees a different path and 404s
    // (regression test-21049: fetch of server.url + "/redirect").
    if (pathq.length > 1 && pathq[1] === "/") pathq = "/" + pathq.replace(/^\/+/, "");
    const method = String(init.method || "GET").toUpperCase();

    const allHdrs = collectHeaders(init, null);
    // Mirror bun's HTTP client: a fixed 256-slot header buffer leaves room for
    // 250 user headers; any beyond that are silently dropped. Only headers that
    // survive the cap suppress their default (so a dropped Host/UA/Accept still
    // gets the built-in fallback rather than going missing).
    const MAX_USER_HEADERS = 250;
    const hdrs = allHdrs.length > MAX_USER_HEADERS ? allHdrs.slice(0, MAX_USER_HEADERS) : allHdrs;
    let haveHost = false, haveAccept = false, haveConn = false, haveCL = false, haveUA = false;
    // ref bun src/http/lib.rs:2408 -- a caller-supplied Connection header decides
    // keep-alive: "close" retires the socket after this exchange, "keep-alive"
    // (and the default, lib.rs:976 CONNECTION_HEADER) pools it.
    let disableKeepalive = false;
    for (const kv of hdrs) {
      const lk = kv[0].toLowerCase();
      if (lk === "host") haveHost = true;
      if (lk === "accept") haveAccept = true;
      if (lk === "connection") { haveConn = true; disableKeepalive = String(kv[1]).toLowerCase().indexOf("close") !== -1; }
      if (lk === "content-length") haveCL = true;
      if (lk === "user-agent") haveUA = true;
    }
    let bodyBytes = null;
    if (init.body != null && method !== "GET" && method !== "HEAD") bodyBytes = u8(init.body);
    const lines = [method + " " + pathq + " HTTP/1.1"];
    if (!haveHost) lines.push("Host: " + host + (port === 80 ? "" : ":" + port));
    if (!haveConn) lines.push("Connection: keep-alive");  // bun lib.rs:976 CONNECTION_HEADER
    // `--user-agent <STR>` overrides the built-in default (Arguments.rs:1062).
    if (!haveUA) {
      const ovUA = G.__mbunHttpNative && G.__mbunHttpNative.userAgent();
      lines.push("User-Agent: " + (ovUA || ("Bun/" + ((G.Bun && G.Bun.version) || "1.0"))));
    }
    if (!haveAccept) lines.push("Accept: */*");
    for (const kv of hdrs) lines.push(kv[0] + ": " + kv[1]);
    if (bodyBytes && !haveCL) lines.push("Content-Length: " + bodyBytes.length);
    const reqBytes = bodyBytes
      ? concatU8([te.encode(lines.join("\r\n") + "\r\n\r\n"), bodyBytes])
      : te.encode(lines.join("\r\n") + "\r\n\r\n");

    // AbortSignal support (fetch spec §4.1): an already-aborted signal rejects
    // before any connection; a later abort tears the socket down and rejects
    // with the signal's abort reason (bun/WebKit: "The operation was aborted.").
    const signal = init.signal;
    const abortReason = () => (signal && signal.reason !== undefined && signal.reason !== null)
      ? signal.reason
      : new G.DOMException("The operation was aborted.", "AbortError");
    if (signal && signal.aborted) return Promise.reject(abortReason());

    const pkey = poolKey(host, port, secure, tlsVerify, tlsCa);

    return new Promise((resolve, reject) => {
      let fd;
      let tls = 0;  // 0 = plain, 1 = handshaking, 2 = established
      // A parked socket is already past connect and any TLS handshake, so it
      // starts at tls = 2 and _poll writes the request on the first pass.
      const pooled = noPool ? null : poolTake(pkey);
      const reused = pooled !== null;
      if (reused) {
        fd = pooled.fd;
        tls = pooled.tls ? 2 : 0;
      } else {
        try { fd = NN.connect(host, port); }
        // The message is verbatim from bun's fetch error arm (FetchTasklet.rs:1345)
        // — no URL suffix: tests pin the exact string.
        catch (e) { return reject(mkErr("Unable to connect. Is the computer able to access the url?", "ConnectionRefused")); }
        // undici/bun arm SO_KEEPALIVE (+TCP_KEEPIDLE) on every fetch client
        // socket unless the request opts out with `keepalive: false` — the same
        // flag that disables connection pooling. node:http forwards
        // agent.keepAlive as this option.
        if (!(init && init.keepalive === false) && NN.setSockBuf) { try { NN.setSockBuf(fd, 3, 60); } catch (e) {} }
        if (secure) {
          // bun's fetch never sends a ClientHello without ALPN (it offers h2 only
          // when HTTP/2 is enabled; this client speaks HTTP/1.1). Servers and
          // middleboxes key off the extension. ref: regression 29780.
          try { NN.tlsWrap(fd, false, "", "", host, tlsVerify ? 1 : 0, tlsCa, "http/1.1"); tls = 1; }
          catch (e) { try { NN.close(fd); } catch (e2) {} return reject(mkErr("fetch: TLS setup failed (" + String((e && e.message) || e) + ")", "FailedToOpenSocket")); }
        }
      }

      const parser = new HttpParser(true);
      parser.reqMethod = method;
      const chunks = [];
      const bodyPr = Promise.withResolvers();
      bodyPr.promise.catch(() => {});  // unread bodies must not surface as unhandled rejections
      let stream = null;
      let headResolved = false;
      let receivedAny = false;
      const item = {
        _fd: fd, _wq: [reqBytes], _pendingOp: true, _done: false, _eof: false,
        _fail(e) { finishErr(e instanceof Error && e.code ? e : mkErr(String((e && e.message) || e), codeOf(e))); },
        _poll() {
          if (this._done) { NET.items.delete(this); return 0; }
          if (tls === 1) {  // TLS handshake gates the request bytes
            let st;
            try { st = NN.tlsStep(this._fd); } catch (e) { st = -1; }
            if (st < 0) { this._fail(mkErr("fetch: TLS handshake failed for " + url, "ConnectionClosed")); return 1; }
            if (st !== 1) return 0;
            tls = 2;
          }
          let progress = 0;
          while (this._wq.length) {
            // Bounded per-write base64 (see Socket._flush): a 128 MB fetch()
            // upload otherwise re-encoded the whole body on every 64 KB socket
            // write and was OOM-killed.
            const head = this._wq[0];
            const piece = head.length > WCHUNK ? head.subarray(0, WCHUNK) : head;
            let n;
            try { n = tls ? NN.tlsWrite(this._fd, toB64(piece)) : NN.write(this._fd, toB64(piece)); }
            catch (e) { this._fail(e); return progress; }
            if (n <= 0) break;
            progress++;
            if (n < head.length) { this._wq[0] = head.subarray(n); if (n < piece.length) break; continue; }
            this._wq.shift();
          }
          if (!this._eof) {
            for (let i = 0; i < 64 && !this._done; i++) {
              let r;
              try { r = tls ? NN.tlsRead(this._fd) : NN.read(this._fd); }
              catch (e) { this._fail(e); return progress; }
              if (r === "") break;
              progress++;
              if (r === null) { this._eof = true; parser.eof(); break; }
              receivedAny = true;
              parser.push(fromB64(r));
            }
          }
          return progress;
        },
      };
      // release=true parks the socket for the next request to this origin instead
      // of closing it (bun release_socket, HTTPContext.rs:581). A socket that hit
      // EOF has nothing to park. Once parked the fd deliberately stays open and
      // unpolled: it is out of NET.items and NET.pending, so it never holds the
      // event loop open, matching bun's unref'd pool.
      const cleanup = (release) => {
        if (item._done) return;
        item._done = true;
        NET.items.delete(item);
        NET.pending--;
        if (release && !item._eof && poolPut(pkey, host, fd, tls === 2)) return;
        if (tls) { try { NN.tlsClose(fd); } catch (e) {} }
        try { NN.close(fd); } catch (e) {}
      };
      const finishErr = (e) => {
        cleanup();
        // ref bun src/http/lib.rs:2107 allow_retry. A pooled socket can be closed
        // by the peer between the checkout probe and our write, so a reused socket
        // that dies before any response byte replays the request -- the server
        // demonstrably never answered it. HTTPContext.rs:979 sets allow_retry for
        // pool-sourced sockets only, and lib.rs:2120 clears it before restarting,
        // so the replay happens once and lands on a fresh connection (noPool).
        // The gate is bun's: an idempotent method (http_types/Method.rs:189) with a
        // replayable body -- always true here, since init.body is materialized to
        // bytes up front, bun's HTTPRequestBody::Bytes case.
        const IDEMPOTENT = ["GET", "HEAD", "PUT", "DELETE", "OPTIONS", "TRACE", "QUERY"];
        if (reused && !headResolved && !receivedAny && IDEMPOTENT.indexOf(method) !== -1) {
          resolve(doFetch(url, init, depth, retryCount, true));
          return;
        }
        // Retry a connection dropped before any response byte (bun allow_retry
        // on response_stage == Pending): the request may not have been
        // processed, so replaying an idempotent bodyless request is safe.
        const retriable = e && (e.code === "ECONNRESET" || e.code === "ConnectionClosed");
        if (!headResolved && !receivedAny && retriable && !bodyBytes &&
            (method === "GET" || method === "HEAD") && retryCount < 8) {
          resolve(doFetch(url, init, depth, retryCount + 1));
          return;
        }
        if (!headResolved) reject(e);
        bodyPr.reject(e);
        if (stream) stream.error(e);
      };
      parser.onError = finishErr;
      parser.onBody = (b) => { const c = b.slice(); chunks.push(c); if (stream) stream.push(c); };
      parser.onDone = () => {
        // ref bun src/http/lib.rs:2189 is_keep_alive_possible and the response
        // gates that feed it. parser.toEof is the RFC 7230 6.3 case at lib.rs:5166:
        // a body framed by connection close leaves nothing to reuse. A response
        // "Connection: close" retires the socket (lib.rs:5100); on HTTP/1.0
        // keep-alive is opt-in rather than the default (lib.rs:5105).
        // 1xx/204/304/HEAD never reach toEof (the parser gives them no body,
        // matching bun's content_length = 0 at lib.rs:5150), so they stay poolable.
        // parser.leftover() is bytes past this response's last byte. There is no
        // pipelining here, so a server has nothing legitimate left to say: the
        // stream is desynced and parking it would feed those bytes to whoever
        // reuses the socket. Bun cannot express this case (picohttp consumes the
        // exchange exactly), so this guard has no line to cite -- it is the cost
        // of parking a fd nothing polls.
        const connHdr = String(parser.headers["connection"] || "").toLowerCase();
        const allowKeepalive = !parser.toEof && !disableKeepalive &&
          parser.leftover().length === 0 &&
          (parser.httpVersion === "1.1"
            ? connHdr.indexOf("close") === -1
            : connHdr.indexOf("keep-alive") !== -1);
        cleanup(allowKeepalive);
        let body = concatU8(chunks);
        const enc = String(parser.headers["content-encoding"] || "").trim().toLowerCase();
        // bun fetch option: decompress !== false (default true) gates decoding.
        if (enc && enc !== "identity" && body.length > 0 && (!init || init.decompress !== false)) {
          // A content-encoding decode failure is a fetch rejection (code ZlibError),
          // not a silent passthrough of the still-compressed bytes. The body.length>0
          // guard matches bun (empty body → Ok). ref: bun InternalState.rs:266-269,369-382.
          try { body = decodeCE(body, enc); }
          catch (e) {
            // Per-codec decode-failure code (bun): br→BrotliDecompressionError,
            // zstd→ZstdDecompressionError, else ZlibError. ref bun src/{brotli,zstd,zlib}/lib.rs.
            const cc = enc === "br" ? "BrotliDecompressionError" : enc === "zstd" ? "ZstdDecompressionError" : "ZlibError";
            bodyPr.reject(mkErr(String((e && e.message) || e), cc)); if (stream) stream.error(mkErr(String((e && e.message) || e), cc)); return;
          }
        }
        bodyPr.resolve(body);
        if (stream) stream.close();
      };
      parser.onHead = () => {
        const loc = parser.headers["location"];
        if (loc && init.redirect !== "manual" && [301, 302, 303, 307, 308].indexOf(parser.status) !== -1) {
          // Not pooled, unlike bun's doRedirect (lib.rs:4296 "Keep-Alive release
          // in redirect"): bun redirects from progress_update, once the body is
          // fully drained and request_stage == Done. This fires from onHead, with
          // the redirect response's own body still arriving, so parking the socket
          // here would leave those bytes to surface inside the next request's
          // response. Closing costs a connection per redirect hop; a desync would
          // cost correctness.
          cleanup();
          if (depth >= 20) return reject(mkErr("Too many redirects", "TooManyRedirects"));
          // Location is a BYTE string (the parser hands header values back
          // latin1-decoded). The URL parser must see those bytes, not their
          // code points re-encoded as UTF-8 — otherwise a Location carrying
          // UTF-8 bytes comes back doubly percent-encoded (%C3%AC… instead of
          // %EC…). Percent-escape the high bytes up front, which is what bun's
          // URL join over the raw header bytes produces.
          const locBytes = typeof loc === "string" && !/[^\x00-\xff]/.test(loc)
            ? loc.replace(/[\x80-\xff]/g, (c) => "%" + c.charCodeAt(0).toString(16).toUpperCase().padStart(2, "0"))
            : loc;
          let nextUrl, nextProtocol;
          try { const u = new G.URL(locBytes, url); nextUrl = String(u); nextProtocol = u.protocol; }
          catch (e) { return reject(mkErr("InvalidRedirectURL", "InvalidRedirectURL")); }
          // bun lib.rs:5241/5374: the hop target must speak http(s); anything
          // else (file:, data:, ...) fails the fetch before the request goes
          // out, so the redirect chain never reaches the origin again.
          if (nextProtocol !== "http:" && nextProtocol !== "https:") {
            return reject(mkErr("UnsupportedRedirectProtocol", "UnsupportedRedirectProtocol"));
          }
          let ninit = init;
          if (parser.status === 303 || ((parser.status === 301 || parser.status === 302) && method === "POST")) {
            ninit = Object.assign({}, init, { method: "GET", body: undefined });
          }
          resolve(doFetch(nextUrl, ninit, depth + 1));
          return;
        }
        headResolved = true;
        const h = new G.Headers();
        for (let i = 0; i < parser.rawHeaders.length; i += 2) h.append(parser.rawHeaders[i], parser.rawHeaders[i + 1]);
        // bun strips content-encoding/content-length after decoding the body.
        const ceHdr = String(parser.headers["content-encoding"] || "").trim().toLowerCase();
        if (ceHdr && ceHdr !== "identity" && (!init || init.decompress !== false)) { h.delete("content-encoding"); h.delete("content-length"); }
        const res = new G.Response(null, { status: parser.status, statusText: parser.statusText, headers: h });
        res.url = url;
        res.redirected = depth > 0;
        stream = mkBodyStream();
        for (const c of chunks) stream.push(c);
        if (parser.done) stream.close();
        res._stream = stream.stream;   // Response.body is a getter over _stream (streams module)
        const bodyU8 = () => bodyPr.promise;
        res.text = () => bodyU8().then((b) => td.decode(b));
        res.json = () => res.text().then((t) => JSON.parse(t));
        res.arrayBuffer = () => bodyU8().then((b) => b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength));
        res.bytes = () => bodyU8().then((b) => new Uint8Array(b));
        res.blob = () => bodyU8().then((b) => new G.Blob([b], { type: h.get("content-type") || "" }));
        // A fetch Response carries its body in the buffered `bodyPr` and exposes
        // a minimal streaming `_stream` (mkBodyStream) that the generic
        // Response.clone() cannot tee — it would hand the clone an empty body, so
        // `response.clone(); await clone.bytes()` read 0 while the original read
        // the full payload (js/web/fetch/body-stream reader/streaming matrices,
        // issue #15). bun clones a fetch Response by refcounting the same body
        // store (Body.rs:1591); the equivalent here is a clone whose consume
        // methods share the same immutable `bodyPr` buffer and whose `_stream` is
        // an independent replay of it. Reads are non-destructive on a resolved
        // buffer, so original and clone stay independently readable.
        res.clone = () => {
          const c = new G.Response(null, { status: parser.status, statusText: parser.statusText, headers: new G.Headers(h) });
          c.url = url;
          c.redirected = depth > 0;
          const cs = mkBodyStream();
          bodyPr.promise.then((b) => { try { if (b && b.length) cs.push(b); cs.close(); } catch (e) {} },
                              (e) => { try { cs.error(e); } catch (e2) {} });
          c._stream = cs.stream;
          c.text = res.text;
          c.json = res.json;
          c.arrayBuffer = res.arrayBuffer;
          c.bytes = res.bytes;
          c.blob = res.blob;
          c.clone = res.clone;
          return c;
        };
        resolve(res);
      };
      if (signal && typeof signal.addEventListener === "function") {
        // An abort after the exchange completed is a no-op (the spec aborts
        // "ongoing fetches"); erroring a finished body stream would throw.
        const onAbort = () => { if (!item._done) finishErr(abortReason()); };
        signal.addEventListener("abort", onAbort);
      }
      NET.items.add(item);
      NET.pending++;
      item._poll();  // kick: connect + send the request right away
    });
  }

  G.fetch = function fetch(input, init) {
    try {
      // Eagerly snapshot init up front so every documented option getter is read
      // exactly once (bun fetch.rs reads the whole RequestInit before dispatch):
      // a throwing getter — or a throwing `headers` iterable — must REJECT the
      // returned promise, not throw synchronously and not connect first. Object
      // spread pulls every own-enumerable value (incl. proxy/timeout/unix/verbose
      // that doFetch ignores) so the throw surfaces here, inside the try.
      if (init != null && typeof init === "object") init = Object.assign({}, init);
      let url = input;
      if (input && typeof input === "object" && input.url) {
        url = input.url;
        // The signal rides on the input Request unless `init` carries its own —
        // bun fetch.rs:1141-1200 ('extract_signal): an `init.signal` that is present
        // wins (a present `null` DETACHES, with no fallback to the Request), while an
        // absent/undefined one falls back to the input Request's signal. Dropping it
        // here made `fetch(new Request(url, { signal }))` unabortable.
        const initHasSignal = init != null && typeof init === "object"
          && "signal" in init && init.signal !== undefined;
        // redirect mode rides on the input Request too (fetch.rs: the Request's
        // mode is the effective one unless init overrides it) — without this
        // fetch(new Request(url, { redirect: "manual" })) followed the redirect.
        const initHasRedirect = init != null && typeof init === "object"
          && "redirect" in init && init.redirect !== undefined;
        init = Object.assign({ method: input.method, headers: input.headers, body: input._body }, init || {});
        if (!initHasSignal) init.signal = input.signal;
        if (!initHasRedirect && input.redirect !== undefined) init.redirect = input.redirect;
      }
      // fetch(url, { headers }) validates names/values synchronously (bun builds
      // the request eagerly): a bad name/value or a throwing iterable rejects.
      // A Headers instance was already validated.
      if (init != null && typeof init === "object" && init.headers != null && !(init.headers instanceof G.Headers))
        void new G.Headers(init.headers);
      // A present non-null init.signal must be an AbortSignal (Request.rs:1408-1414).
      if (init != null && typeof init === "object" && init.signal != null && !(init.signal instanceof G.AbortSignal))
        throw new TypeError("fetch() signal is not of type AbortSignal.");
      const __gbody = init != null ? init.body : undefined;
      if (__gbody !== undefined && __gbody !== null && __gbody !== "") {
        const __gm = (init != null && init.method != null) ? String(init.method).toUpperCase() : "GET";
        if (__gm === "GET" || __gm === "HEAD" || __gm === "OPTIONS") {
          const __ge = new TypeError("fetch() request with GET/HEAD/OPTIONS method cannot have body.");
          __ge.code = "ERR_INVALID_ARG_VALUE";
          throw __ge;
        }
      }
      return doFetch(url, init, 0);
    } catch (e) { return Promise.reject(e); }
  };
  G.fetch.preconnect = function () {};
})();
)JS";

}  // namespace mbun::jsc::js_net
