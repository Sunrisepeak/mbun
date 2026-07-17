// modules/jsc/src/js_bun_socket.cppm — module mbun.jsc.js_bun_socket
//
// Bun.sleepSync (real blocking sleep) + Bun.connect / Bun.listen (bun's TCP
// socket API: socket:{open,data,drain,close,error,end,connectError} handlers,
// per-socket .data) as a thin adapter over the node:net reactor Socket/Server
// registered by mbun.jsc.js_net. Split out of js_net.cppm for the ≤2000-line
// budget; evaluated right AFTER kNetJS (needs __mbunNativeModules.net,
// __mbunNet and __mbunNetNative). Blueprint: bun src/api/bun/socket (uSockets
// handler shape). TLS/unix stay honest throws, like the rest of the JS layer.
export module mbun.jsc.js_bun_socket;

import std;

namespace mbun::jsc::js_bun_socket {

export constexpr std::string_view kBunSocketJS = R"JS(
(function () {
  "use strict";
  const G = globalThis;
  const NN = G.__mbunNetNative;
  const NET = G.__mbunNet;
  const M = G.__mbunNativeModules || {};
  const netMod = M["net"] || M["node:net"];
  if (!NN || !NET || !netMod || !netMod.Socket) return;  // reactor unavailable
  const Socket = netMod.Socket, Server = netMod.Server;
  const te = new G.TextEncoder();
  const u8 = (d) => (d == null ? new Uint8Array(0) : typeof d === "string" ? te.encode(d) : d instanceof Uint8Array ? d : ArrayBuffer.isView(d) ? new Uint8Array(d.buffer, d.byteOffset, d.byteLength) : d instanceof ArrayBuffer ? new Uint8Array(d) : d._u8 instanceof Uint8Array ? d._u8 : te.encode(String(d)));
  const mkErr = (msg, code) => { const e = new Error(msg); e.code = code; return e; };

  // ---- TLS option coercion (bun socket `tls: TLSOptions | boolean`) ----------
  // node/bun accept cert/key/ca as string | Buffer | array-of; the native
  // TlsChannel loads one concatenated PEM string per slot (runtime/net.inc).
  const pemOf = (v) => {
    if (v == null) return "";
    if (Array.isArray(v)) return v.map(pemOf).join("\n");
    if (typeof v === "string") return v;
    if (v && typeof v.pem === "string") return v.pem;
    if (ArrayBuffer.isView(v) || v instanceof ArrayBuffer) {
      try { return G.Buffer.from(v.buffer ? v.buffer : v, v.byteOffset || 0, v.byteLength).toString("utf8"); }
      catch (e) { return String(v); }
    }
    return typeof v.toString === "function" ? v.toString() : String(v);
  };
  const isIPish = (h) => typeof h === "string" && (/^[\d.]+$/.test(h) || h.indexOf(":") !== -1);
  // Normalize `tls` (true | TLSOptions) into the _startTls option bag. Blueprint:
  // bun-zig-src src/runtime/socket/socket.zig upgradeTLS + SSLConfig field names
  // (serverName SNI, rejectUnauthorized verify, requestCert server-side).
  const normalizeTls = (tls, hostname, isServer) => {
    if (tls == null || tls === false) return null;
    // bun: `tls` must be a boolean or an object (SSLConfig). ref bun-zig-src
    // src/runtime/socket/SSLConfig.zig inflateHeader / JSValue coercion.
    if (tls !== true && typeof tls !== "object") throw new TypeError("TLSOptions must be an object");
    const o = tls === true ? {} : tls;
    const sn = o.serverName || o.servername || (!isServer && !isIPish(hostname) ? String(hostname || "") : "");
    return {
      isServer: !!isServer,
      cert: pemOf(o.cert), key: pemOf(o.key), ca: pemOf(o.ca),
      servername: sn,
      requestCert: !!o.requestCert,
      verify: isServer ? !!o.requestCert : (o.rejectUnauthorized !== false),
    };
  };

  // ---- Bun.sleepSync (real blocking sleep over net.sleep/nanosleep) ---------
  // Overwrites the no-op stub from process_web (bun: blocks the thread for the
  // full duration — `elapsed >= ms` assertions must hold).
  if (G.Bun && NN && typeof NN.sleep === "function") {
    G.Bun.sleepSync = function (ms) {
      if (typeof ms !== "number") throw new TypeError('The "milliseconds" argument must be of type number. Received ' + typeof ms);
      ms = ms | 0;
      if (!(ms >= 0)) throw new RangeError("argument to sleepSync must not be negative, got " + ms);
      const end = Date.now() + ms;
      for (;;) { const left = end - Date.now(); if (left <= 0) break; NN.sleep(left > 1000 ? 1000 : left); }
    };
  }

  // ---- Bun.connect / Bun.listen (bun TCP + TLS socket API over the reactor) --
  // Blueprint: bun src/api/bun/socket + bun-zig-src src/runtime/socket/socket.zig
  // (uSockets handler shape socket:{open,data,drain,close,error,end,connectError,
  // handshake}, per-socket .data). Thin adapter over the node:net Socket/Server
  // reactor; write() buffers fully (returns byte count), backpressure surfaces
  // via drain. TLS rides net.Socket._startTls (the same BoringSSL memory-BIO
  // handshake fetch(https) and node:tls use). Firing order per bun.d.ts
  // SocketHandler: for a TLS socket with NO `handshake` handler, `open` fires
  // only after the handshake completes; WITH a `handshake` handler, `open` fires
  // on TCP connect and `handshake(sock, success, authError)` on completion.
  if (G.Bun) {
    const call = (fn, ...a) => { if (typeof fn === "function") { try { return fn(...a); } catch (e) {} } };
    const bunSockWrap = (sock, handlers, userData, isTls) => {
      const bs = {
        data: userData,
        remoteAddress: sock.remoteAddress, localAddress: sock.localAddress,
        localPort: sock.localPort, remotePort: sock.remotePort,
        readyState: "open",
        // TLS surface (bun Socket): populated on handshake; safe defaults before.
        authorized: false, alpnProtocol: null,
        write(d) { const b = u8(d); sock.write(b); return b.length; },
        end(d) { if (d !== undefined && d !== null) this.write(d); sock.end(); return this; },
        flush() {},
        shutdown() { sock.end(); },
        terminate() { sock.destroy(); },
        ref() { return this; }, unref() { return this; },
        timeout() { return this; },
        get _fd() { return sock._fd; },
      };
      if (isTls) {
        bs.getPeerCertificate = () => ({});
        bs.getCipher = () => ({});
        bs.getProtocol = () => (bs.authorized || sock._tls === 2 ? "TLSv1.3" : null);
        bs.getServername = () => bs.servername || "";
        bs.setVerifyMode = () => {};
      }
      // Honor binaryType (bun SocketConfigHandlersBinaryType: Arraybuffer/Buffer
      // (default)/Uint8array — Handlers.rs:76). u8(chunk) is a Buffer.
      const bt = handlers.binaryType;
      sock.on("data", (chunk) => {
        const b = u8(chunk);
        const payload = bt === "arraybuffer" ? b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength)
                      : bt === "uint8array"  ? new Uint8Array(b.buffer, b.byteOffset, b.byteLength)
                      :                        b;
        call(handlers.data, bs, payload);
      });
      sock.on("drain", () => call(handlers.drain, bs));
      sock.on("end", () => call(handlers.end, bs));
      sock.on("close", () => { bs.readyState = "closed"; call(handlers.close, bs); });
      sock.on("error", (e) => call(handlers.error, bs, e));
      return bs;
    };
    G.Bun.connect = function (opts) {
      if (!opts || typeof opts !== "object" || !opts.socket || typeof opts.socket !== "object")
        return Promise.reject(new TypeError('Expected "socket" option'));
      // unix and hostname/port are exclusive (bun's UnixOrHost union). A unix
      // path dials the AF_UNIX reactor connect; the AF_INET path is unchanged.
      const unixPath = opts.unix ? String(opts.unix) : "";
      const tlsCfg = normalizeTls(opts.tls, opts.hostname, false);
      return new Promise((resolve, reject) => {
        const handlers = opts.socket;
        const sock = new Socket({ allowHalfOpen: false });
        const bs = bunSockWrap(sock, handlers, opts.data, !!tlsCfg);
        const hasHandshake = typeof handlers.handshake === "function";
        let opened = false, settled = false;
        const fireOpen = () => { if (opened) return; opened = true; call(handlers.open, bs); };
        sock.once("connect", () => {
          bs.remotePort = sock.remotePort;
          if (tlsCfg) {
            bs.servername = tlsCfg.servername;
            sock._startTls(tlsCfg);
            if (hasHandshake) fireOpen();  // open on TCP connect when handshake handler present
          } else {
            fireOpen();
            if (!settled) { settled = true; resolve(bs); }
          }
        });
        if (tlsCfg) {
          sock.once("secureConnect", () => {
            bs.authorized = tlsCfg.verify;
            if (hasHandshake) call(handlers.handshake, bs, true, null);
            fireOpen();  // open after handshake when there is no handshake handler
            if (!settled) { settled = true; resolve(bs); }
          });
        }
        sock.once("error", (e) => {
          if (!opened && !tlsCfg) call(handlers.connectError, bs, e);
          else if (tlsCfg && hasHandshake && opened) call(handlers.handshake, bs, false, e);
          else if (tlsCfg && !opened) call(handlers.connectError, bs, e);
          if (!settled) { settled = true; reject(e); }
        });
        if (unixPath) sock.connect({ path: unixPath });
        else sock.connect(opts.port | 0, String(opts.hostname || "localhost"));
      });
    };
    G.Bun.listen = function (opts) {
      if (!opts || typeof opts !== "object" || !opts.socket || typeof opts.socket !== "object")
        throw new TypeError('Expected "socket" option');
      const handlers = opts.socket;
      const tlsCfg = normalizeTls(opts.tls, opts.hostname, true);
      // unix and hostname/port are exclusive: bun's Listener keeps a UnixOrHost
      // union, so the unused side of the pair reads back as undefined.
      const unixPath = opts.unix ? String(opts.unix) : "";
      const hostname = unixPath ? undefined : String(opts.hostname || "0.0.0.0");
      let lh;
      if (unixPath) {
        try { lh = NN.listenUnix(unixPath); }
        catch (e) { throw mkErr("Failed to listen at " + unixPath + " (" + e.message + ")", "EADDRINUSE"); }
      } else {
        try { lh = NN.listen(hostname, opts.port | 0); }
        catch (e) { throw mkErr("Failed to listen at " + hostname + ":" + (opts.port | 0), "EADDRINUSE"); }
      }
      const netServer = new Server({ allowHalfOpen: false });
      netServer._fd = lh.fd;
      netServer._addr = unixPath ? { address: unixPath, family: "unix" }
                                 : { port: lh.port, address: hostname, family: "IPv4" };
      netServer.listening = true;
      NET.items.add(netServer);
      netServer.on("connection", (sock) => {
        const bs = bunSockWrap(sock, handlers, opts.data, !!tlsCfg);
        const hasHandshake = typeof handlers.handshake === "function";
        let opened = false;
        const fireOpen = () => { if (opened) return; opened = true; call(handlers.open, bs); };
        if (tlsCfg) {
          sock._startTls(tlsCfg);          // server-side accept handshake
          if (hasHandshake) fireOpen();     // open on accept when handshake handler present
          sock.once("secureConnect", () => {
            bs.authorized = tlsCfg.verify;
            if (hasHandshake) call(handlers.handshake, bs, true, null);
            fireOpen();                     // open after handshake when no handshake handler
          });
        } else {
          fireOpen();
        }
      });
      // bun unlinks the socket file BEFORE closing the listening fd (Node/libuv
      // order: unlinking after close would race another process binding the same
      // path). Already-gone is fine — callers may unlink it themselves first.
      const unlinkUnix = () => {
        if (!unixPath || unixPath[0] === "\0") return;
        try { (M["fs"] || M["node:fs"]).unlinkSync(unixPath); } catch (e) {}
      };
      const listener = {
        port: unixPath ? undefined : lh.port,
        hostname,
        unix: unixPath || undefined,
        data: opts.data,
        stop(force) { unlinkUnix(); netServer.close(); if (force) { for (const s of Array.from(netServer._conns || [])) s.destroy(); } },
        ref() { return listener; }, unref() { return listener; },
        reload() { return listener; },
        getsockname(out) {
          if (out === null || typeof out !== "object")
            throw new TypeError('Expected "out" to be an object');
          const a = netServer._addr || {};
          out.family = a.family || "IPv4";
          out.address = a.address || hostname;
          out.port = (a.port !== undefined ? a.port : lh.port) | 0;
          return undefined;
        },
      };
      listener[Symbol.dispose] = () => listener.stop(true);
      return listener;
    };
  }

})();
)JS";

}  // namespace mbun::jsc::js_bun_socket
