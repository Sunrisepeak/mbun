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
import mbun.jsc.js_net_part2;

namespace mbun::jsc::js_net {

// The JS payload is split across two files, joined below before evaluation.
// Not for readability: a single .cppm over ~256 KiB makes mcpp fail with
// "index requires mcpp >= 0.0.108 but this is mcpp 0.0.103", which names neither
// the file nor the size and sent me bisecting merged commits. This file was
// 254 KiB and the composed merge pushed it to 274 KiB.
//
// The cut is a plain line boundary, NOT a JS statement boundary, and that is
// safe by construction: the parts are concatenated into one std::string before
// they are ever evaluated, so the JS never sees the seam. (js_streams.cppm cuts
// at a statement boundary for readability; it does not have to.)
export constexpr std::string_view kNetJS_part1 = R"JS(
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
  // node internal/errors.js UVExceptionWithHostPort — the form a *listen*
  // failure takes (a connect failure uses ExceptionWithHostPort, which has no
  // uv description). `listen EADDRINUSE: address already in use 127.0.0.1:1234`.
  // The strings are libuv's uv_strerror() texts, which node copies verbatim
  // into the message and the corpus matches on
  // (test-net-server-listen-path, test-net-server-listen-handle).
  const UV_MSG = {
    EACCES: "permission denied",
    EADDRINUSE: "address already in use",
    EADDRNOTAVAIL: "address not available",
    EAFNOSUPPORT: "address family not supported",
    EINVAL: "invalid argument",
    ELOOP: "too many symbolic links encountered",
    EMFILE: "too many open files",
    ENAMETOOLONG: "name too long",
    ENFILE: "file table overflow",
    ENOENT: "no such file or directory",
    ENOTDIR: "not a directory",
    ENOTSOCK: "socket operation on non-socket",
    EPERM: "operation not permitted",
  };
  // node lib/net.js Server: a bind failure carries the requested address/port and
  // syscall so `err.address`/`err.port` are usable (test-net-better-error-messages-*).
  const listenError = (nativeError, address, port) => {
    if (isAccessDenied(nativeError)) return nativeError;
    // A bind failure the natives describe without an errno ("Is port N in use?")
    // is the address-in-use case, which is what the previous hard-coded value
    // covered — keep it as the fallback so EADDRINUSE detection is unchanged.
    const code = codeOf(nativeError, "EADDRINUSE");
    const desc = UV_MSG[code] ? ": " + UV_MSG[code] : "";
    const error = mkErr("listen " + code + desc + " " + address + (port === undefined ? "" : ":" + port), code);
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
      this._paused = false; this._enc = null; this._everRead = false;
        // http's read-side interception and park queue (w5/agent-http): a socket
        // that receives bytes before anything reads used to DROP them.
        this._dataSink = null;
        this._rq = null; this._rqLen = 0; this._rqPaused = false; this._rqEnd = false;
        this._flowing = null; this._holdForReader = false;
      // node net.Socket({ onread }): the socket reads INTO the caller's buffer
      // and hands that exact object back, so `buf === sockBuf` holds and no
      // per-chunk allocation happens (test-net-onread-static-buffer). `buffer`
      // may also be a generator, which node calls once before the first read
      // and again after every callback.
      this._onread = null; this._onreadBuf = null; this._onreadGen = null; this._onreadPend = null;
      {
        const orOpt = opts.onread;
        if (orOpt && typeof orOpt === "object" && typeof orOpt.callback === "function" &&
            (typeof orOpt.buffer === "function" || ArrayBuffer.isView(orOpt.buffer))) {
          this._onread = orOpt.callback;
          if (typeof orOpt.buffer === "function") this._onreadGen = orOpt.buffer;
          else this._onreadBuf = orOpt.buffer;
        } else if (orOpt && typeof orOpt.callback === "function") {
          // No usable buffer: keep the previous "hand the chunk over" shape.
          this._onread = orOpt.callback;
        }
      }
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
      // node's Duplex keeps the two sides' marks apart, and net.createConnection
      // forwards `readableHighWaterMark` / `writableHighWaterMark` straight into
      // it — lib/_http_incoming.js then builds the IncomingMessage with
      // `socket.readableHighWaterMark`, which is how a caller's createConnection
      // sizes the response stream (test-http-incoming-message-options).
      this._rhwm = typeof opts.readableHighWaterMark === "number" ? opts.readableHighWaterMark : this._hwm;
      this._whwm = typeof opts.writableHighWaterMark === "number" ? opts.writableHighWaterMark : this._hwm;
      this._readableState = { endEmitted: false, ended: false, destroyed: false, length: 0, flowing: true, readable: true, objectMode: false, highWaterMark: this._rhwm };
      // node:http's OutgoingMessage.end() sets _writableState.corked before its
      // final uncork(); keep the cork counter in one place.
      this._corked = 0;
      this._writableState = { corked: 0, ended: false, finished: false, destroyed: false, length: 0, objectMode: false, highWaterMark: this._hwm };
      // node `new net.Socket({ fd })`: the socket takes over an already-open
      // descriptor (createHandle + this[kHandle].open(fd)) and is live
      // immediately — no connect(). That is how a child reads the extra stdio
      // slots its parent opened, e.g. `new net.Socket({ fd: 4 })` against
      // `cluster.setupPrimary({ stdio: [..., 'pipe'] })`
      // (test-cluster-fork-stdio). `readable`/`writable` default to true here
      // because this transport is duplex either way; node only uses them to
      // decide which halves to start.
      if (typeof opts.fd === "number" && opts.fd >= 0) {
        this._adopt(opts.fd);
        if (opts.readable === false) this.readable = false;
        if (opts.writable === false) this.writable = false;
      }
      // The first reader to show up collects whatever _deliver parked for it.
      // node's Readable.on('data') resumes the stream unless it was explicitly
      // paused (`if (state.flowing !== false) this.resume()`).
      this.on("newListener", (ev) => {
        if (ev !== "data") return;
        if (this._flowing !== false) this._flowing = true;
        if (this._rq && this._rq.length) G.queueMicrotask(() => this._flushRq());
      });
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
      // node lib/net.js addClientAbortSignalOption: options.signal aborts the
      // connection with an AbortError. Only Server.listen honoured a signal, so
      // `agent.createConnection({ ..., signal })` ignored it entirely and the
      // caller's `await once(connection, 'error')` never settled.
      if (optArg && optArg.signal !== undefined && optArg.signal !== null) {
        const sig = optArg.signal;
        if (typeof sig !== "object" || typeof sig.addEventListener !== "function" || !("aborted" in sig)) {
          const e = new TypeError('The "options.signal" argument must be an instance of AbortSignal. Received ' +
            (typeof sig === "string" ? "type string ('" + sig + "')" : "type " + typeof sig));
          e.code = "ERR_INVALID_ARG_TYPE"; throw e;
        }
        // Always asynchronous, even when the abort arrives synchronously from
        // the caller's own ac.abort(): the caller attaches its 'error' listener
        // AFTER that call (`ac.abort(); await once(connection, 'error')`), so a
        // synchronous emit would be an uncaught exception instead.
        const abortNow = () => {
          if (self.destroyed) return;
          const e = new Error("The operation was aborted");
          e.name = "AbortError"; e.code = "ABORT_ERR";
          self.connecting = false;
          const fire = () => { if (!self.destroyed) { self.emit("error", e); self.destroy(); } };
          if (G.process && typeof G.process.nextTick === "function") G.process.nextTick(fire);
          else G.queueMicrotask(fire);
        };
        // An ALREADY-aborted signal registers no listener at all -- the corpus
        // asserts listenerCount(signal, 'abort') === 0 for that case and === 1
        // for the live one, so the difference is observable.
        if (sig.aborted) G.queueMicrotask(abortNow);
        else {
          const onAbort = () => abortNow();
          sig.addEventListener("abort", onAbort, { once: true });
          this.once("close", () => { try { sig.removeEventListener("abort", onAbort); } catch (e) {} });
        }
      }
      // node net.js blockList / custom-lookup pre-connect resolution. Isolated to
      // the case where the caller actually passes options.blockList or
      // options.lookup, so the normal (http/tls/numeric-host) path below is
      // byte-for-byte unchanged. A resolved address that the BlockList rejects is
      // ERR_IP_BLOCKED; a lookup that yields a non-4/6 family is
      // ERR_INVALID_ADDRESS_FAMILY (both surfaced on 'error', as in node).
      const _blockList = optArg && optArg.blockList;
      const _lookup = optArg && optArg.lookup;
      // node lookupAndConnect -> internalConnect: options.localAddress /
      // localPort are bound on the handle BEFORE connect(2), so a source
      // address this host does not own fails the connection instead of being
      // ignored. The bound name is then what socket.localAddress/localPort
      // report — read back with getsockname rather than echoed, because the
      // kernel picks both when neither was requested.
      const _localAddr = (optArg && optArg.localAddress != null) ? String(optArg.localAddress) : null;
      const _localPort = (optArg && optArg.localPort != null) ? (optArg.localPort | 0) : 0;
      const _adoptLocal = (sock, sfd) => {
        try {
          const sn = NN.sockname ? NN.sockname(sfd) : null;
          if (typeof sn === "string") {
            const i = sn.lastIndexOf(":");
            sock.localAddress = sn.slice(0, i);
            sock.localPort = +sn.slice(i + 1);
          }
        } catch (e) {}
      };
      const _famOf = (v) => isIPv6(v) ? 6 : isIPv4(v) ? 4 : 0;
      // node net.js lookupAndConnect: a host that is not an IP literal is
      // resolved through dns.lookup before connect(2), and the result (or the
      // failure) is announced on 'lookup'. The reactor's native connect only
      // understands literals plus "localhost"/wildcards, so every other name
      // used to die as `net.connect: invalid address` — reported to the caller
      // as a bogus ECONNRESET with no 'lookup' at all (test-net-dns-error).
      // "localhost" and the wildcards stay on the synchronous fast path they
      // have always taken: net.inc resolves them itself.
      const _needsDns = !unixPath && !_famOf(host) &&
        host !== "localhost" && host !== "" && host !== "*";
      if (!unixPath && ((_blockList && _famOf(host)) || _lookup || _needsDns)) {
        const failWith = (err) => { self.connecting = false; G.queueMicrotask(() => { self.emit("error", err); self.destroy(); }); return self; };
        const dialResolved = (addr, fam) => {
          if (fam !== 4 && fam !== 6) { const e = mkErr("Invalid address family: " + fam + " " + host + ":" + port, "ERR_INVALID_ADDRESS_FAMILY"); e.host = host; e.port = port; return failWith(e); }
          if (_blockList && _blockList.check(addr, fam === 6 ? "ipv6" : "ipv4")) return failWith(mkErr("IP is blocked by net.BlockList", "ERR_IP_BLOCKED"));
          const dh = (addr === "::1" || addr === "::" || addr === "::0") ? "127.0.0.1" : addr;
          let fd2;
          try { fd2 = NN.connect(dh, port, _localAddr, _localPort); }
          catch (e) { self.connecting = false; const err = connectError(e, addr, port); G.queueMicrotask(() => { if (self.destroyed) return; self.emit("error", err); self.destroy(); }); return self; }
          self._adopt(fd2); self.remotePort = port; _adoptLocal(self, fd2);
          self.connecting = true;
          G.queueMicrotask(() => { if (self.destroyed) { self.connecting = false; return; } self.connecting = false; self._flushPreConnect(null); self.emit("connect"); self.emit("ready"); });
          return self;
        };
        const hf = _famOf(host);
        if (hf) return dialResolved(host, hf);
        // host needs resolution — drive the caller-supplied lookup (node passes
        // { family, hints, all }; all is set under autoSelectFamily).
        const lopts = { family: (optArg && optArg.family) || 0, hints: (optArg && optArg.hints) || 0, all: !!(optArg && optArg.autoSelectFamily) };
        const _resolver = _lookup || ((M["dns"] || M["node:dns"] || {}).lookup);
        if (typeof _resolver !== "function") {
          const e = mkErr("getaddrinfo ENOTFOUND " + host, "ENOTFOUND"); e.host = host; e.port = port;
          self.emit("lookup", e, undefined, undefined, host);
          return failWith(e);
        }
        try {
          _resolver(host, lopts, (err, address, family) => {
            // node emits 'lookup' with (err, ip, addressType, host) for every
            // resolution it performs, success or failure, BEFORE it acts on it.
            self.emit("lookup", err || null, err ? undefined : address, err ? undefined : family, host);
            if (self.destroyed) return;
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
      try { if (unixPath && pipePathTooLong(unixPath)) throw new Error("EINVAL"); fd = unixPath ? NN.connectUnix(unixPath) : NN.connect(dialHost, port, _localAddr, _localPort); }
      catch (e) { this.connecting = false; const err = connectError(e, unixPath || host, unixPath ? undefined : port); G.queueMicrotask(() => { if (this.destroyed) return; this.emit("error", err); this.destroy(); }); return this; }
      this._adopt(fd);
      this.remotePort = port;
      if (!unixPath) _adoptLocal(this, fd);
      // node reports `connecting === true` from the moment connect() returns
      // until the 'connect' event fires; _adopt cleared it because the reactor's
      // connect() already completed synchronously. A destroy() in between must
      // cancel the pending 'connect' rather than resurrect the socket.
      this.connecting = true;
      G.queueMicrotask(() => { if (this.destroyed) { this.connecting = false; return; } this.connecting = false; this._flushPreConnect(null); this.emit("connect"); this.emit("ready"); });
      return this;
    }
    setEncoding(enc) { this._enc = enc || "utf8"; return this; }
    // node net.Socket.setTimeout: an idle timer that emits 'timeout' after
    // `ms` with no read/write activity (0 clears it). Node does NOT destroy the
    // socket on timeout — the listener decides. Re-armed on every I/O below.
    setTimeout(ms, cb) {
      // node internal/stream_base_commons.js setStreamTimeout: a destroyed
      // stream ignores the call entirely (no validation, no listener), then
      // getTimerDuration(msecs, 'msecs') applies validateNumber +
      // "non-negative finite" — so '100'/{}/undefined are ERR_INVALID_ARG_TYPE
      // and -1/NaN/Infinity are ERR_OUT_OF_RANGE, not a silent `| 0` to 0.
      if (this.destroyed) return this;
      this.timeout = ms;
      if (typeof ms !== "number") {
        const NE = G.__mbunNodeErrors;
        if (NE) throw NE.ERR_INVALID_ARG_TYPE("msecs", "number", ms);
        const e = new TypeError('The "msecs" argument must be of type number. Received ' + typeof ms);
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      if (ms < 0 || !Number.isFinite(ms)) {
        const e = new RangeError('The value of "msecs" is out of range. It must be a non-negative finite number. Received ' + String(ms));
        e.code = "ERR_OUT_OF_RANGE"; throw e;
      }
      if (cb !== undefined && typeof cb !== "function") {
        const NE = G.__mbunNodeErrors;
        if (NE) throw NE.ERR_INVALID_ARG_TYPE("callback", "function", cb);
        const e = new TypeError('The "callback" argument must be of type function. Received ' + typeof cb);
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      // node: msecs === 0 REMOVES the listener instead of adding one.
      if (typeof cb === "function") { if (ms === 0) this.removeListener("timeout", cb); else this.once("timeout", cb); }
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
    get writableHighWaterMark() { return this._whwm; }
    get readableHighWaterMark() { return this._rhwm; }
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
      const r = this._rq;
      if (r && r.length) {
        this._rq = null; this._rqLen = 0; this._readableState.length = 0;
        this._holdForReader = false;
        if (this._rqPaused) { this._rqPaused = false; this._paused = false; }
        // Draining the buffer is what releases the 'end' EOF earned.
        if (this._rqEnd) {
          this._rqEnd = false;
          G.queueMicrotask(() => {
            if (this.destroyed || this._readableState.endEmitted) return;
            this._readableState.endEmitted = true;
            this.emit("end");
          });
        }
        return r.length === 1 ? r[0] : (G.Buffer ? G.Buffer.concat(r) : r[0]);
      }
      return null;
    }
    // node net.js afterConnect: `if (readable && !self.isPaused()) self.read(0)`
    // — a socket paused BEFORE it ever started reading never calls readStart,
    // so libuv never makes the handle active and uv_loop_alive() ignores it.
    // `net.connect(port).pause()` therefore lets the process exit
    // (test-net-connect-paused-connection). A socket paused after data has
    // already flowed keeps its hold: node only stops the read once the readable
    // buffer fills, which this reactor cannot observe.
    pause() { this._paused = true; this._flowing = false; this._syncEofHold(); return this; }
    resume() {
      // Composed from both sides of the w5/agent-http merge, and BOTH halves are
      // load-bearing. HEAD's half is the loop-hold bookkeeping (_syncEofHold) and
      // the onread feed. The http half is the readable-flow state: without
      // `_flowing = true`, _deliver's _hasReader() stays false for a socket that
      // was resumed but has no 'data' listener, so every byte parks in _rq,
      // _rqPending() keeps the handle alive forever and the peer's writer
      // stalls. That is exactly what five large-write corpus files hit
      // (test-net-write-fully-async-*, -bytes-written-large,
      // test-http-outgoing-drain-writable-length,
      // test-http-pipeline-requests-connection-leak) when this composition
      // silently failed to apply.
      this._paused = false;
      this._flowing = true;
      this._holdForReader = false;
      this._rqPaused = false;
      this._syncEofHold();
      if (this._onreadPend) this._onreadFeed();
      if (this._unshiftQ && this._unshiftQ.length) G.queueMicrotask(() => this._flushUnshift());
      if (this._rq && this._rq.length) G.queueMicrotask(() => this._flushRq());
      return this;
    }
    // node internal/stream_base_commons.js onStreamRead, kBuffer branch: each
    // read fills the user buffer (never more than its length), the callback is
    // invoked with (nread, thatSameBuffer), a `false` return stops the flow, and
    // a buffer generator is re-run after every callback. Bytes that arrive
    // faster than the buffer can carry them wait here rather than being dropped.
    _onreadFeed() {
      while (this._onreadPend && this._onreadPend.length > 0 && !this._paused && !this.destroyed) {
        if (!this._onreadBuf) {
          if (!this._onreadGen) {
            // onread without a usable buffer (not node-reachable, but this
            // runtime accepted it before): hand the whole chunk over as-is.
            const all = this._onreadPend; this._onreadPend = null;
            this._onread(all.length, G.Buffer ? G.Buffer.from(all) : all);
            break;
          }
          const nb = this._onreadGen();
          if (!ArrayBuffer.isView(nb)) { this._paused = true; break; }
          this._onreadBuf = nb;
        }
        const buf = this._onreadBuf;
        const n = Math.min(buf.length, this._onreadPend.length);
        if (n <= 0) break;
        buf.set(this._onreadPend.subarray(0, n), 0);
        this._onreadPend = this._onreadPend.length > n ? this._onreadPend.subarray(n) : null;
        let ok = this._onread(n, buf) !== false;
        if (this._onreadGen) {
          const nb = this._onreadGen();
          if (ArrayBuffer.isView(nb)) this._onreadBuf = nb; else ok = false;
        }
        if (!ok) this._paused = true;
      }
    }
    _onreadPush(bytes) {
      this._onreadPend = (this._onreadPend && this._onreadPend.length)
        ? concatU8([this._onreadPend, bytes]) : u8(bytes);
      this._onreadFeed();
    }
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
      if (this._onread) this._onreadPush(b);
      else this.emit("data", this._enc && G.Buffer ? b.toString(this._enc) : b);
      return true;
    }
    // node's net.Socket is a Readable: bytes that arrive before anything reads
    // are BUFFERED, not dropped. This transport emits 'data' as bytes arrive, so
    // a chunk delivered while nobody is listening used to vanish — a server that
    // answers before its peer attaches a 'data' listener (every http upgrade
    // handshake, and test-http-upgrade-server-with-large-body's 101) lost its
    // first reply outright. Park those chunks instead and release them the
    // moment a reader appears.
    // node's three-state `readableFlowing`: null = nothing has asked to read
    // yet (buffer), true = flowing (emit, even with no listener attached —
    // `socket.resume()` with no 'data' handler is how half the corpus drains a
    // response), false = explicitly paused.
    // Bytes read off the wire that a consumer has been PROMISED but has not
    // taken yet. Only the http upgrade handover sets _holdForReader (node's
    // `readableFlowing = null` right before the tunnel is emitted): there the
    // socket must outlive the peer's FIN, because the corpus subscribes to the
    // tunnel from a timer. Every other socket keeps the old teardown timing —
    // holding them all open turned fifteen green files into hangs.
    _rqPending() { return !!(this._holdForReader && this._rq && this._rq.length); }
    // Bytes read off the wire that no consumer has taken yet. _rqPending() above
    // is the LOOP-HOLD test and is deliberately narrow (holding every such
    // socket open turned fifteen green files into hangs); TEARDOWN is a separate
    // question and must not be narrowed the same way. destroy() drops _rq on the
    // floor and then emits 'end' as though the buffer had been drained, so a
    // peer that writes its whole answer and FINs in one breath — to a socket
    // whose reader attaches one microtask later, which is every socket created
    // inside another socket's 'data' handler — lost the answer outright and the
    // client reported "socket hang up" (test-http-should-keep-alive).
    _rqUndelivered() { return !!(this._rq && this._rq.length); }
    _hasReader() { return !!(this._onread || this._dataSink || this._flowing === true || this.listenerCount("data") > 0); }
    _deliver(chunk) {
      if (this._onread) { this._onreadPush(chunk); return; }
      if (this._dataSink) { this._dataSink(chunk); return; }
      if (!this._hasReader()) {
        if (!this._rq) { this._rq = []; this._rqLen = 0; }
        this._rq.push(chunk);
        this._rqLen += chunk.length;
        this._readableState.length = this._rqLen;
        // Readable stops pulling once the buffer passes the high-water mark;
        // without this an unread socket would buffer the peer without bound.
        if (this._rqLen >= this._hwm && !this._paused) { this._paused = true; this._rqPaused = true; }
        return;
      }
      this.emit("data", this._enc && G.Buffer ? chunk.toString(this._enc) : chunk);
    }
    _flushRq() {
      const q = this._rq;
      if (!q || !q.length || !this._hasReader()) return;
      this._rq = null; this._rqLen = 0; this._readableState.length = 0;
      this._holdForReader = false;
      if (this._rqPaused) { this._rqPaused = false; this._paused = false; }
      for (const c of q) {
        if (this.destroyed) return;
        if (this._onread) this._onreadPush(c);
        else if (this._dataSink) this._dataSink(c);
        else this.emit("data", this._enc && G.Buffer ? c.toString(this._enc) : c);
      }
      // 'end' is owed after the parked bytes, never before them.
      if (this._rqEnd && !this.destroyed) {
        this._rqEnd = false;
        this._readableState.endEmitted = true;
        this.emit("end");
        // The teardown that EOF deferred while bytes were still owed.
        if (this._eof && this._shutSent && this._wq.length === 0) this.destroy();
      }
    }
    _flushUnshift() {
      const q = this._unshiftQ;
      if (!q || !q.length) return;
      // A paused socket has no reader; emitting into it would drop the bytes on
      // the floor. Hold them until resume() (which flushes) or the read loop.
      if (this._paused) return;
      this._unshiftQ = null;
      for (const c of q) {
        if (this.destroyed) return;
          if (this._onread) this._onreadPush(c);
          else if (this._dataSink) this._dataSink(c);
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
        // The hand-built message appended ". Received …" to a string that already
        // ended in "." and produced a doubled period.
        const NE = G.__mbunNodeErrors;
        if (NE) throw NE.ERR_INVALID_ARG_TYPE("chunk", ["string", "Buffer", "TypedArray", "DataView"], data);
        const recv = data === undefined ? " Received undefined"
          : (typeof data === "object")
            ? " Received an instance of " + ((data && data.constructor && data.constructor.name) || "Object")
            : " Received type " + typeof data + " (" + String(data) + ")";
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
      // node lib/net.js: a write issued while the socket is still connecting is
      // a *pending* write — its callback fires only once the connection is
      // established, and a destroy() in that window completes it with
      // ERR_SOCKET_CLOSED_BEFORE_CONNECTION instead of success
      // (test-net-write-cb-on-destroy-before-connect).
      if (typeof cb === "function" && this.connecting) {
        (this._preConnectCbs || (this._preConnectCbs = [])).push(cb);
        if (this._wqLen >= HWM) { this._needDrain = true; return false; }
        return true;
      }
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
    // Completes the write callbacks that were queued while the socket was still
    // connecting (node keeps them in the writable queue until the handle is up).
    _flushPreConnect(err) {
      const q = this._preConnectCbs;
      if (!q || !q.length) return;
      this._preConnectCbs = null;
      const fire = () => { for (const f of q) f(err); };
      if (G.process && typeof G.process.nextTick === "function") G.process.nextTick(fire);
      else G.queueMicrotask(fire);
    }
    destroy(err) {
      if (this.destroyed) return this;
      // EOF has already been observed but 'end' was held back behind bytes the
      // consumer never came for. node's Readable would have emitted it as soon
      // as the buffer was drained; a socket that is torn down instead must
      // still report that its read side ended, or a 'close' handler asserting
      // on it fires first (test-net-socket-close-after-end).
      if (this._rqEnd) {
        this._rqEnd = false; this._rq = null; this._rqLen = 0;
        this._readableState.endEmitted = true;
        this.emit("end");
      }
      const wasConnecting = this.connecting;
      this.destroyed = true; this.connecting = false; this.readable = false; this.writable = false;
      if (wasConnecting) {
        this._flushPreConnect(mkErr("Socket closed before the connection was established",
                                    "ERR_SOCKET_CLOSED_BEFORE_CONNECTION"));
      } else this._flushPreConnect(err || null);
      if (this._readableState) { this._readableState.destroyed = true; this._readableState.readable = false; }
      if (this._timeoutTimer) { G.clearTimeout(this._timeoutTimer); this._timeoutTimer = null; }
      if (this._fd >= 0) { try { NN.close(this._fd); } catch (e) {} this._fd = -1; }
      NET.items.delete(this);
      this._loopOpen = false; NET.release(this);
      if (err) this.emit("error", err);
      if (!this._closeEmitted) { this._closeEmitted = true; G.queueMicrotask(() => this.emit("close", !!err)); }
      return this;
    }
    // node lib/net.js Socket.prototype.destroySoon: end() first, then destroy on
    // 'finish' -- NEVER in the same turn. This used to be a bare destroy(), and
    // that one difference is visible from the protocol level: _http_server's
    // resOnFinish calls destroySoon() when res._last, which for a
    // maxRequestsPerSocket-capped connection runs synchronously inside the
    // handler dispatched from parser.onHead. Closing the fd there discards the
    // bytes of the request that was already pipelined behind it, so the second
    // message is never parsed and 'dropRequest' never fires.
    destroySoon() {
      if (this.destroyed) return this;
      if (!this._shutW) this.end();
      this._destroySoon = true;
      this._armDestroySoon();
      return this;
    }
    // Fires once the write queue is empty (node's 'finish'), one tick later so
    // whatever the parser still has buffered gets dispatched first.
    _armDestroySoon() {
      if (!this._destroySoon || this.destroyed || this._dsArmed) return;
      if (this._wq && this._wq.length) return;   // retried from _flush
      this._dsArmed = true;
      const fin = () => { if (!this.destroyed) this.destroy(); };
      if (G.process && typeof G.process.nextTick === "function") G.process.nextTick(fin);
      else G.queueMicrotask(fin);
    }
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
                   o.ciphers || "",
                   // hostCheck: fold the peer-name check into OpenSSL's verify
                   // (SSL_set1_host) unless the caller replaced it with its own
                   // tls.connect({ checkServerIdentity }), which node runs in JS
                   // INSTEAD of the default. Default true — omitting the flag must
                   // never be the same as switching the check off.
                   o.hostCheck === false ? false : true,
                   // TLS 1.3 suites (node SetCipherSuites); a separate OpenSSL
                   // slot from the <=TLS1.2 cipher list above.
                   o.cipherSuites || "",
                   // caComplete: o.ca is the entire trust store; never fall back
                   // to the platform one (an empty store must stay empty).
                   o.caComplete === true,
                   // session: base64 DER of a session to resume (client), and
                   // ticketKeys: base64 of the server's stable 48-byte session
                   // ticket key. Both are "" when the caller wants a full
                   // handshake / OpenSSL's own per-context random ticket key.
                   o.session || "", o.ticketKeys || "");
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
        // …unless bytes read off the wire are still owed to a consumer that has
        // not started reading (see _rqPending): node's socket is only fully
        // done once its readable side has ENDED, which cannot happen while the
        // buffer still holds data.
        if (this._eof && !this._rqPending() && !this._rqUndelivered()) this.destroy();
      }
      if (this._needDrain && this._wqLen === 0 && !this.destroyed) { this._needDrain = false; progress++; this.emit("drain"); }
      if (this._destroySoon) this._armDestroySoon();
      this._syncEofHold();
      return progress;
    }
    // libuv's liveness rule, which this reactor was missing. A stream handle is
    // ACTIVE only while it has a read started or a write request pending;
    // uv_loop_alive() counts active handles, not merely open ones. So once node
    // has seen EOF it calls readStop(), and a socket that is still *open and
    // writable* — the whole point of allowHalfOpen — stops holding the loop.
    //
    // Here every open socket held it unconditionally, so a half-open socket the
    // peer had already FIN'd pinned the process forever even though nothing
    // could ever read from it again. That is the `sockets outlive server`
    // timeout shape: server closed, one or more EOF'd Sockets left with
    // readable=false, writable=true, _wq=0.
    //
    // Deliberately narrow: only a socket that has ALREADY seen EOF is released,
    // and only while its write queue is empty. Re-holding on a queued write is
    // what keeps a later `socket.write()` on a half-open socket flushable —
    // libuv's write request makes the handle active again in exactly the same
    // way. Sockets that never see EOF, and non-half-open sockets (which shut
    // and destroy on EOF anyway), are unaffected.
    _syncEofHold() {
      if (this.destroyed || !this._loopOpen) return;
        // Composed release condition. HEAD: a socket paused before it ever read
        // never made the handle active, so it must not pin the loop. http: bytes
        // parked for a reader that has not arrived keep it alive. Both hold.
        if (((this._eof && !this._rqPending()) || (this._paused && !this._everRead)) &&
            this._wq.length === 0) NET.release(this);
      else NET.hold(this);
    }
    // Hand OpenSSL's NSS keylog lines to whoever asked for them (node's
    // TLSSocket 'keylog'). Only runs when a listener exists — the native drain is
    // a per-poll call and key material must not be moved out of the engine on
    // spec. Each line arrives without its newline; node's event carries one.
    _drainKeylog() {
      if (!this._keylogWanted || !this._tls || this._fd < 0 || !NN.tlsKeylog) return;
      let lines;
      try { lines = NN.tlsKeylog(this._fd); } catch (e) { return; }
      if (!lines || lines.length === 0) return;
      for (const line of lines) this.emit("keylog", G.Buffer ? G.Buffer.from(line + "\n") : line + "\n");
    }
    // Hand OpenSSL's newly-issued TLS sessions to node's 'session' event. Gated
    // on a listener for the same reason as keylog: it is a per-poll native call.
    // TLS 1.2 queues its session DURING the handshake, TLS 1.3 only once the
    // post-handshake NewSessionTicket has been read — so this is called both at
    // the top of the poll and inside the read loop, because a peer that FINs
    // immediately after its ticket (the common `socket.end('x')` server shape in
    // the corpus) destroys this socket before another poll ever runs.
    _drainSessions() {
      if (!this._sessionWanted || this._tls !== 2 || this._fd < 0 || !NN.tlsNewSessions) return;
      let blobs;
      try { blobs = NN.tlsNewSessions(this._fd); } catch (e) { return; }
      if (!blobs || blobs.length === 0) return;
      for (const b64 of blobs) this.emit("session", G.Buffer ? G.Buffer.from(b64, "base64") : b64);
    }
    _poll() {
      if (this.destroyed || this._fd < 0) { NET.items.delete(this); return 0; }
      if (this._keylogWanted) this._drainKeylog();
      if (this._sessionWanted) this._drainSessions();
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
        // A TLS 1.3 handshake produces all five keylog lines at completion, and
        // the socket may be destroyed from the secureConnect continuation — drain
        // here rather than waiting for the next poll that may never come.
        this._drainKeylog();
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
          // tlsRead is what pumps TLS 1.3's post-handshake NewSessionTicket into
          // the engine, so the session it produces has to be picked up here —
          // the EOF branch below can destroy this socket before the next poll.
          if (this._sessionWanted) this._drainSessions();
          if (r === "") break;
          progress++;
          if (r === null) {
            this._eof = true; this.readable = false;
            // Bytes parked for a reader that has not arrived yet come first:
            // 'end' means "no more data", so it may not overtake data already
            // received. _flushRq emits it once the queue drains.
            if (this._rq && this._rq.length) this._rqEnd = true;
            else { this._readableState.endEmitted = true; this.emit("end"); }
            // node net.js onReadableStreamEnd auto-ends the write side on the
            // NEXT tick, not inline, so data written synchronously right after
            // 'end'/'secureConnect' (e.g. a TLS1.2 peer that FINs one flight
            // early) still flushes instead of hitting ERR_STREAM_WRITE_AFTER_END.
            if (!this.allowHalfOpen && !this._shutW) {
              G.queueMicrotask(() => {
                if (this.destroyed || this._shutW) return;
                this._shutW = true; this.writable = false; this._flush();
                if (this._eof && this._shutSent && this._wq.length === 0 && !this._rqPending() && !this._rqUndelivered()) this.destroy();
              });
            }
            if (this._shutSent && this._wq.length === 0 && !this._rqPending() && !this._rqUndelivered()) this.destroy();
            // EOF: the read side is stopped, so this handle is only active while
            // a write is queued (see _syncEofHold).
            this._syncEofHold();
            break;
          }
          const bytes = fromB64(r);
          this._everRead = true;
          this.bytesRead += bytes.length;
          if (this._timeoutMs) this._armTimeout();
          const chunk = G.Buffer ? G.Buffer.from(bytes) : bytes;
          if (this._unshiftQ) this._flushUnshift();   // unshifted bytes come first
          // _deliver routes to onread / the http upgrade sink / 'data', and
          // parks the chunk when nothing is reading yet.
          if (G.Buffer) this._deliver(chunk);
          else if (this._onread) this._onread(bytes.length, chunk);
          else this.emit("data", this._enc ? latin1(bytes, 0, bytes.length) : chunk);
          if (this.destroyed || this._paused) break;
        }
      }
      if (this._eof && this._shutSent && this._wq.length === 0 && !this._rqPending() && !this._rqUndelivered()) this.destroy();
      return progress;
    }
  }
  // node net.js Socket.prototype.readyState: a deprecated but still-asserted
  // view of the two stream flags — 'opening' while connecting, then 'open' /
  // 'readOnly' / 'writeOnly' by which half is still live, and 'closed' once
  // neither is. Purely derived, so it costs nothing until read.
  Object.defineProperty(Socket.prototype, "readyState", {
    configurable: true,
    get() {
      if (this.connecting) return "opening";
      if (this.readable && this.writable) return "open";
      if (this.readable) return "readOnly";
      if (this.writable) return "writeOnly";
      return "closed";
    },
  });
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
    // node net.js Server.prototype[EventEmitter.captureRejectionSymbol]: with
    // events.captureRejections on, a connection listener that returns a
    // rejected promise tears that connection down instead of throwing out of
    // the microtask queue. Without this hook EventEmitter falls back to
    // emit('error') on a server nobody is listening to, which is a fatal
    // uncaught exception.
    [Symbol.for("nodejs.rejection")](err, event, sock) {
      if (event === "connection" && sock && typeof sock.destroy === "function") { sock.destroy(err); return; }
      this.emit("error", err);
    }
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
      // node lib/net.js normalizeArgs: EVERY listen form — (), (port[, host]),
      // (path), (handle), (options) — collapses to one options object before
      // anything is decided. That is what makes listen(false) an *options*
      // error (ERR_INVALID_ARG_VALUE) rather than a silently-ignored argument,
      // and what gives listen({}) / listen({ host }) their "must have the
      // property port or path" message (test-net-server-listen-options).
      const isPipeNameStr = (v) => typeof v === "string" && !(Number(v) >= 0);
      let o;
      if (a.length === 0) o = {};
      else if (typeof a[0] === "object" && a[0] !== null) o = a[0];
      else if (isPipeNameStr(a[0])) o = { path: a[0] };
      else { o = { port: a[0] }; if (typeof a[1] === "string") o.host = a[1]; }
      { const last = a[a.length - 1]; if (typeof last === "function") cb = last; }
      // node: `options = options._handle || options.handle || options`.
      o = o._handle || o.handle || o;
      // node lib/net.js: listen(), listen(cb), listen(null) and an options
      // object whose `port` is present-but-undefined/null all mean "an
      // arbitrary unused port". node mutates the caller's object here, so the
      // diagnostics-channel payload below sees the normalized value too.
      if (a.length === 0 || typeof a[0] === "function" ||
          (typeof o.port === "undefined" && ("port" in o)) || o.port === null) {
        o.port = 0;
      }
      const errListenOptions = (reason) => {
        let recv;
        try { recv = JSON.stringify(o); } catch (e) { recv = undefined; }
        if (recv === undefined) recv = String(o);
        const e = new TypeError("The argument 'options' " + reason + ". Received " + recv);
        e.code = "ERR_INVALID_ARG_VALUE"; return e;
      };
      // A descriptor handed over by child_process (listen(handle) / {fd}).
      // node adopts it; this runtime has no JS-visible handle to adopt, so it
      // keeps the previous behaviour (an ephemeral bind) rather than throwing.
      const fdOpt = typeof o.fd === "number" && o.fd >= 0;
      if (typeof o.port === "number" || typeof o.port === "string") {
        validateListenPort(o.port); port = o.port | 0;
      } else if (o.path != null && isPipeNameStr(o.path)) {
        unixPath = String(o.path);
      } else if (fdOpt) {
        port = 0;
      } else if (!(("port" in o) || ("path" in o))) {
        throw errListenOptions('must have the property "port" or "path"');
      } else {
        throw errListenOptions("is invalid");
      }
      {
        if (o.host != null) host = String(o.host);
        if (o.exclusive != null) this._exclusive = !!o.exclusive;
        // node lib/net.js Server.listen: `reusePort` implies `exclusive`, so the
        // worker binds its own SO_REUSEPORT socket instead of asking the primary
        // (test-cluster-net-reuseport asserts cluster._getServer is NOT called).
        if (o.reusePort === true) { this._exclusive = true; this._reusePort = true; }
        if (o.backlog != null) this._backlog = o.backlog | 0;
        if (o.ipv6Only) this._ipv6Only = true;
        // node lib/net.js Server.listen({ path, readableAll, writableAll }):
        // uv_pipe_chmod widens the socket file's group/other bits after bind.
        if (unixPath != null && (o.readableAll || o.writableAll)) {
          this._pipeMode = (o.readableAll ? 0o044 : 0) | (o.writableAll ? 0o022 : 0);
        }
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
        // node lib/net.js Server.listen: readableAll/writableAll are applied
        // right after the bind via uv_pipe_chmod, which ORs the group/other
        // read/write bits into the socket file's existing mode.
        if (this._pipeMode && unixPath[0] !== "\0") {
          try {
            const fsMod = M["fs"] || M["node:fs"];
            const cur = fsMod.statSync(unixPath).mode & 0o7777;
            fsMod.chmodSync(unixPath, cur | this._pipeMode);
          } catch (e) {}
        }
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
            const sock = new Socket({ allowHalfOpen: !!this._opts.allowHalfOpen, highWaterMark: this._opts.highWaterMark })._adopt(clientHandle.fd);
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
        const sock = new Socket({ allowHalfOpen: !!this._opts.allowHalfOpen, highWaterMark: this._opts.highWaterMark })._adopt(cfd);
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
  // llhttp's METHOD_MAP (== http.METHODS): a server rejects anything else with
  // HPE_INVALID_METHOD, and it does so as soon as the token stops matching.
  const HTTP_METHODS = [
    "ACL", "BIND", "CHECKOUT", "CONNECT", "COPY", "DELETE", "GET", "HEAD",
    "LINK", "LOCK", "M-SEARCH", "MERGE", "MKACTIVITY", "MKCALENDAR", "MKCOL",
    "MOVE", "NOTIFY", "OPTIONS", "PATCH", "POST", "PROPFIND", "PROPPATCH",
    "PURGE", "PUT", "QUERY", "REBIND", "REPORT", "SEARCH", "SOURCE",
    "SUBSCRIBE", "TRACE", "UNBIND", "UNLINK", "UNLOCK", "UNSUBSCRIBE",
  ];
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
  // src/node_http_parser.cc kMaxChunkExtensionsSize.
  const MAX_CHUNK_EXTENSIONS_SIZE = 16384;
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
      this.headers = {}; this.rawHeaders = []; this.trailers = {}; this.rawTrailers = [];
      this.remaining = 0; this.chunked = false; this.toEof = false;
      this.reqMethod = "GET";
      // node: parser.maxHeaderPairs = server.maxHeadersCount << 1 (0 = no cap);
      // maxHeaderSize caps the whole head block (--max-http-header-size).
      this.maxHeaderPairs = 0;
      this.maxHeaderSize = 0;
      // insecureHTTPParser / --insecure-http-parser: llhttp's lenient flags.
      // The only one mbun needs so far is obs-fold (RFC 7230 3.2.4 line
      // folding), which strict llhttp rejects and lenient llhttp joins onto the
      // previous field value with a single SP.
      this.lenient = false;
      this.onHead = null; this.onBody = null; this.onDone = null; this.onError = null;
      // 1xx interim heads are not the final response: node re-arms the parser
      // and raises 'continue'/'information' on the ClientRequest instead
      // (_http_client.js parserOnIncomingClient -> `return 1`).
      this.onInterim = null;
      this._lastChunk = null; this._errMsg = null; this._errCode = null;
    }
    leftover() { return this.buf.subarray(this.off); }
    push(bytes) {
      if (this.done) return 0;
      // Deliberately NOT re-reporting the error for every further chunk the way
      // llhttp's execute() does. Measured: re-delivery turned
      // test-http-{blank-header,response-splitting,pipeline-socket-parser-typeerror}
      // from pass into a hang, because each repeat re-enters socketOnError on a
      // connection that is already ending. One report per errored parser is
      // enough for the corpus (test-http-socket-error-listeners asserts
      // mustCallAtLeast(1)).
      if (this.state === "error") {
        // ...unless the server CLAIMED the previous error through a
        // 'clientError' listener. That connection is deliberately still alive,
        // and node reports one error per invalid byte on it -- which is the
        // whole point of test-http-socket-error-listeners (21 errors on one
        // socket). The three files above have no 'clientError' listener, so
        // they keep the single-report behaviour.
        if (this._repeatErrors && bytes && bytes.length) {
          this._lastChunk = bytes;
          if (this.onError) this.onError(this._mkErr(this._errMsg, this._errCode));
          return 1;
        }
        return 0;
      }
      if (bytes && bytes.length) {
        this._lastChunk = bytes;
        this.buf = this.off === this.buf.length ? bytes : concatU8([this.buf.subarray(this.off), bytes]);
        this.off = 0;
      }
      return this._process(false);
    }
    eof() { if (this.done || this.state === "error") return 0; return this._process(true); }
    // _http_common.js prepareError: every parse error carries the bytes it
    // choked on. The corpus reads it directly (`err.rawPacket.toString()`), and
    // `bytesParsed` is what node's own 400/431 path reports.
    _mkErr(msg, code) {
      const e = mkErr(msg, code);
      e.bytesParsed = this.off;
      const raw = this._lastChunk && this._lastChunk.length ? this._lastChunk : this.buf;
      try { e.rawPacket = G.Buffer ? G.Buffer.from(raw.slice ? raw.slice() : raw) : raw; } catch (x) {}
      return e;
    }
    _err(msg, code) {
      this.state = "error";
      this._errMsg = msg; this._errCode = code;
      if (this.onError) this.onError(this._mkErr(msg, code));
    }
    // Is the request-line method already known to be bad? llhttp matches the
    // method against its METHOD_MAP one byte at a time, so an unknown token
    // fails at the first character that cannot continue any known method — it
    // never waits for the rest of the head (http.METHODS is exactly this list).
    // Returns false while the bytes so far are still a viable prefix.
    _badMethod() {
      let i = this.off;
      const b = this.buf;
      // llhttp tolerates blank lines before a request line (lenient CRLF skip).
      while (i < b.length && (b[i] === 13 || b[i] === 10)) i++;
      let end = i;
      while (end < b.length && b[end] !== 32) end++;
      if (end - i > 24) return true;  // longest known method is 12 bytes
      if (end === i && end === b.length) return false;  // nothing to judge yet
      const tok = latin1(b, i, end);
      const exact = end < b.length;  // the space arrived: the token is complete
      const M = HTTP_METHODS;
      for (let k = 0; k < M.length; k++) {
        if (exact ? M[k] === tok : M[k].lastIndexOf(tok, 0) === 0) return false;
      }
      return true;
    }
    _finish() { this.done = true; if (this.onDone) this.onDone(); }
    _emitBody(b) { if (b.length && this.onBody) this.onBody(b); }
    _process(eofSeen) {
      let events = 0;
      for (;;) {
        const avail = this.buf.length - this.off;
        if (this.state === "head") {
          // llhttp's s_start_req swallows any CR/LF that precedes a request
          // line, and RFC 9112 2.2 requires a server to ignore at least one
          // empty line before the request-line. mbun did not, so a pipelined
          // request separated from the previous message by a stray blank line
          // parsed as an empty request line and became a 400 that destroyed the
          // connection (test-http-keep-alive-drop-requests writes exactly that
          // shape: `...Host: localhost\r\n` + `\r\n\r\n`). Requests only —
          // _badMethod already applies the same skip.
          if (!this.isResponse) {
            while (this.off < this.buf.length && (this.buf[this.off] === 13 || this.buf[this.off] === 10)) this.off++;
          }
          const at = findSeq(this.buf, this.off, CRLF2);
          const hardLimit = this.maxHeaderSize > 0 ? this.maxHeaderSize
            : (G.__mbunHttpNative && G.__mbunHttpNative.getMaxHeaderSize ? G.__mbunHttpNative.getMaxHeaderSize() | 0 : 0);
          if (at < 0) {
            // llhttp validates the head BYTE BY BYTE, so both checks below fire
            // long before "\r\n\r\n" arrives. Deferring them until the head is
            // terminated meant a peer that never terminates it — a header flood,
            // or garbage pipelined behind a request ("hello world") — parked the
            // connection forever with nothing destroyed and no clientError, which
            // is what these files were timing out on rather than failing.
            if (hardLimit > 0 && avail > hardLimit) {
              this.off = this.buf.length;
              this._err("Parse Error: Header overflow", "HPE_HEADER_OVERFLOW");
              return events + 1;
            }
            if (!this.isResponse && this._badMethod()) {
              this._err("Parse Error: Invalid method encountered", "HPE_INVALID_METHOD");
              return events + 1;
            }
            if (!eofSeen) return events;
            if (avail === 0 && !this.isResponse) { this.done = true; return events; }  // idle conn closed
            // llhttp_finish() on a REQUEST that stopped mid-head is
            // HTTP_FINISH_UNSAFE -> reason "Invalid EOF state" /
            // HPE_INVALID_EOF_STATE (deps/llhttp/src/api.c), which node reports
            // verbatim through 'clientError' (test-http-parser-finish-error
            // matches both /^Parse Error/ and the code). Responses keep the
            // ECONNRESET wording the client path turns into "socket hang up".
            if (!this.isResponse) {
              this._err("Parse Error: Invalid EOF state", "HPE_INVALID_EOF_STATE");
              return events + 1;
            }
            this._err("The socket connection was closed unexpectedly", "ECONNRESET");
            return events + 1;
          }
          const headLen = at + 4 - this.off;
          if (hardLimit > 0 && headLen > hardLimit) {
            this.off = at + 4;
            this._err("Parse Error: Header overflow", "HPE_HEADER_OVERFLOW");
            return events + 1;
          }
          let head = latin1(this.buf, this.off, at);
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
          // obs-fold: a header line that continues on the next line, indented by
          // SP/HTAB. Strict llhttp rejects it (HPE_INVALID_HEADER_TOKEN, which
          // is what the loop below produces for a continuation line — it has no
          // colon); with the lenient flags set it splices the continuation onto
          // the previous field value separated by one SP, which is what node's
          // insecureHTTPParser gives (test-http-multi-line-headers).
          if (this.lenient) head = head.replace(/\r\n[ \t]+/g, " ");
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
              const wellFormed = codings.length > 0 && chunkedAt === codings.length - 1 &&
                                 codings.lastIndexOf("chunked") === chunkedAt;
              // A SECOND Transfer-Encoding line after chunked has already been
              // announced: llhttp rejects that inside on_header_value_complete,
              // i.e. BEFORE on_headers_complete, so the request is never
              // observed by the application at all. That is a different failure
              // from a single header whose coding list cannot frame a body
              // ("chunkedchunked"), which llhttp accepts as a header and only
              // then refuses to frame — the te-invalid path below, which node
              // does observe once (test-http-transfer-encoding-repeated-chunked
              // vs test-http-header-value-relaxed's duplicate-TE server).
              // kLenientTransferEncoding (part of kLenientAll, i.e.
              // insecureHTTPParser / httpValidation:'insecure' — NOT 'relaxed')
              // accepts the duplicate instead.
              if (sawTE && teChunked && !this.lenient) {
                this._err("Parse Error: Invalid transfer encoding", "HPE_INVALID_TRANSFER_ENCODING");
                return events + 1;
              }
              teChunked = this.lenient ? (teChunked || wellFormed) : wellFormed;
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
            // llhttp's own wording, which the corpus compares verbatim
            // (test-http-client-error-rawbytes). The rejection itself is
            // unchanged: CL+TE is still a hard parse error.
            this._err("Parse Error: Transfer-Encoding can't be present with Content-Length",
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
          // src/node_http_parser.cc caps the chunk-extension bytes of one
          // chunk at 16 KiB; past that llhttp raises
          // HPE_CHUNK_EXTENSIONS_OVERFLOW and node answers 413
          // (test-http-chunk-extensions-limit). This only ADDS a rejection —
          // an over-long extension that used to be accepted is now an error,
          // so it cannot loosen any smuggling check.
          if (semi !== -1 && (line.length - semi) > MAX_CHUNK_EXTENSIONS_SIZE) {
            this._err("Parse Error: Chunk extensions overflow", "HPE_CHUNK_EXTENSIONS_OVERFLOW");
            return events + 1;
          }
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
          if (c > 0) {
            // Keep the wire form as well as the folded map: node exposes
            // rawTrailers verbatim (original case, duplicates preserved, in
            // arrival order) and derives .trailers/.trailersDistinct from it,
            // so a lowercased last-wins object cannot reconstruct it
            // (test-http-raw-headers / test-http-multiple-headers send the same
            // trailer name twice with different case).
            const tname = line.slice(0, c).trim();
            const tval = line.slice(c + 1).trim();
            this.rawTrailers.push(tname, tval);
            this.trailers[tname.toLowerCase()] = tval;
          }
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
    this.headers = {}; this.rawHeaders = []; this.trailers = {}; this.rawTrailers = [];
    this.remaining = 0; this.chunked = false; this.toEof = false;
    this.reqMethod = "GET";
    this.maxHeaderPairs = 0; this.maxHeaderSize = 0;
    this.lenient = false;
    this.onHead = this.onBody = this.onDone = this.onError = this.onInterim = null;
    this._lastChunk = null; this._errMsg = null; this._errCode = null;
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
)JS";

export inline const std::string kNetJS =
    std::string{kNetJS_part1}.append(kNetJS_part2);

}  // namespace mbun::jsc::js_net
