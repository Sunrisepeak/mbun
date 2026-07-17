// modules/jsc/src/js_tls_live.cppm — module mbun.jsc.js_tls_live
//
// The LIVE node:tls socket surface: tls.connect() client handshake, TLSSocket,
// and tls.createServer()/Server server handshake — wired to the real reactor
// net.Socket (js_net.cppm), which already drives a BoringSSL memory-BIO
// handshake per fd via runtime/net.inc (tlsWrap/tlsStep/tlsRead/tlsWrite over
// modules/tls TlsChannel; the same path fetch(https://) rides).
//
// Split out of builtins/node_tls.cppm because the offline surface there is
// baked into the master builtins image, which runs BEFORE kNetJS installs the
// real net.Socket/net.Server. This partition is evaluated AFTER kNetJS +
// kBunSocketJS (engine.inc), so `extends net.Socket` gets the reactor class,
// then it overwrites tls.connect/createServer/TLSSocket/Server in place.
//
// Blueprint: bun-ref src/js/node/tls.ts (TLSSocket, connect, Server) +
// src/js/node/net.ts (SocketHandlers.handshake → "secure"/"secureConnect";
// ServerHandlers.handshake → "secureConnection"). The BIO pump itself lives in
// runtime/net.inc (net_tls_shuttle), translated from bun uws ssl.zig.
export module mbun.jsc.js_tls_live;

import std;

namespace mbun::jsc::js_tls_live {

export constexpr std::string_view kTlsLiveJS = R"JS(
(function () {
  "use strict";
  const G = globalThis;
  const M = G.__mbunNativeModules || {};
  const T = M["tls"] || M["node:tls"];
  const net = M["net"] || M["node:net"];
  if (!T || !net || !net.Socket || !net.Server) return;  // reactor unavailable
  const NetSocket = net.Socket, NetServer = net.Server;
  const Buffer = G.Buffer;
  const NN = G.__mbunNetNative;                 // tlsInfo(fd): negotiated params + peer cert
  const asym = G.__mbunCryptoAsymNative;        // x509parse: X509 field bridge (crypto_asym.inc)

  // ---- peer certificate: native PEM → node getPeerCertificate() object shape --
  // x509parse returns subject/issuer as newline-joined "SN=value" DN strings
  // (crypto_asym.inc asym_x509_name); node lib/_tls_common.js / bun tls.ts expose
  // them as { C, ST, L, O, OU, CN } objects (repeated attrs become arrays).
  const parseDN = (s) => {
    const o = {};
    if (typeof s !== "string" || s === "") return o;
    for (const line of s.split("\n")) {
      const eq = line.indexOf("=");
      if (eq < 0) continue;
      const k = line.slice(0, eq), v = line.slice(eq + 1);
      if (o[k] === undefined) o[k] = v;
      else if (Array.isArray(o[k])) o[k].push(v);
      else o[k] = [o[k], v];
    }
    return o;
  };
  const parseCert = (pem) => {
    if (!pem || !asym || typeof asym.x509parse !== "function") return {};
    let x; try { x = asym.x509parse(pem); } catch (e) { return {}; }
    const cert = {
      subject: parseDN(x.subject),
      issuer: parseDN(x.issuer),
      valid_from: x.validFrom, valid_to: x.validTo,
      serialNumber: x.serialNumber,
      fingerprint: x.fingerprint, fingerprint256: x.fingerprint256, fingerprint512: x.fingerprint512,
    };
    // node uses lowercase `subjectaltname`; x509parse emits `subjectAltName`
    // already in node's "DNS:a, IP Address:b" wire form.
    if (x.subjectAltName != null) cert.subjectaltname = x.subjectAltName;
    if (x.infoAccess != null) cert.infoAccess = x.infoAccess;
    if (x.bits != null) cert.bits = x.bits;
    if (x.modulus != null) cert.modulus = x.modulus;
    if (x.exponent != null) cert.exponent = x.exponent;
    // node exposes pubkey/raw as Buffers; x509parse returns Uint8Arrays.
    if (x.pubkey != null) cert.pubkey = Buffer ? Buffer.from(x.pubkey) : x.pubkey;
    if (x.raw != null) cert.raw = Buffer ? Buffer.from(x.raw) : x.raw;
    if (x.ca != null) cert.ca = x.ca;
    if (x.ext_key_usage != null) cert.ext_key_usage = x.ext_key_usage;
    return cert;
  };

  const deferredTLS = (what) => {
    const e = new Error("tls." + what + " requires the socket event loop + real SSL_CTX handshake (DEFERRED in mbun: modules/tls TlsChannel / S-net)");
    e.code = "ERR_MBUN_DEFERRED";
    return e;
  };
  const netIsIP = (h) => {
    if (typeof h !== "string") return 0;
    if (net && typeof net.isIP === "function") return net.isIP(h);
    if (/^(\d{1,3}\.){3}\d{1,3}$/.test(h)) return 4;
    if (h.indexOf(":") !== -1) return 6;
    return 0;
  };
  // node accepts cert/key/ca as string | Buffer | array-of; the native
  // TlsChannel loads a single concatenated PEM string per slot.
  const pemOf = (v) => {
    if (v == null) return "";
    if (Array.isArray(v)) return v.map(pemOf).join("\n");
    if (typeof v === "string") return v;
    if (v && typeof v.pem === "string") return v.pem;
    if (ArrayBuffer.isView(v) || v instanceof ArrayBuffer) {
      try { return (Buffer ? Buffer.from(v.buffer ? v.buffer : v, v.byteOffset || 0, v.byteLength) : v).toString("utf8"); }
      catch (e) { return String(v); }
    }
    return typeof v.toString === "function" ? v.toString() : String(v);
  };
  const isMbunNetSocket = (s) => !!s && typeof s._startTls === "function" && typeof s.on === "function";
  // ALPNProtocols may be an array of names, a comma string, or a length-prefixed
  // wire Buffer (node convertALPNProtocols). Normalize to comma-joined names for
  // the native tlsWrap. Blueprint: node lib/tls.js convertALPNProtocols.
  const alpnCsv = (v) => {
    if (v == null) return "";
    if (Array.isArray(v)) return v.filter((x) => typeof x === "string" && x).join(",");
    if (typeof v === "string") return v;
    if (ArrayBuffer.isView(v) || v instanceof ArrayBuffer) {
      const b = ArrayBuffer.isView(v) ? new Uint8Array(v.buffer, v.byteOffset, v.byteLength) : new Uint8Array(v);
      const out = [];
      for (let i = 0; i < b.length;) { const l = b[i++]; if (i + l > b.length) break; let s = ""; for (let k = 0; k < l; k++) s += String.fromCharCode(b[i + k]); out.push(s); i += l; }
      return out.join(",");
    }
    return "";
  };

  // ---- protocol version window (node lib/internal/tls/secure-context.js) ------
  // A secureProtocol like "TLSv1_2_method" pins both bounds to a single version;
  // "TLS_method"/"SSLv23_method" leave the window open. Otherwise the explicit
  // minVersion/maxVersion win, falling back to tls.DEFAULT_MIN/MAX_VERSION.
  const secureProtocolPin = (sp) => {
    if (typeof sp !== "string") return null;
    if (sp === "TLSv1_3_method") return "TLSv1.3";
    if (sp === "TLSv1_2_method") return "TLSv1.2";
    if (sp === "TLSv1_1_method") return "TLSv1.1";
    if (sp === "TLSv1_method") return "TLSv1";
    return null;
  };
  const resolveVersions = (options) => {
    const pin = secureProtocolPin(options.secureProtocol);
    let min = pin != null ? pin : options.minVersion;
    let max = pin != null ? pin : options.maxVersion;
    if (min == null) min = T.DEFAULT_MIN_VERSION;
    if (max == null) max = T.DEFAULT_MAX_VERSION;
    return { min: min || "", max: max || "" };
  };

  // ---- TLSSocket: a net.Socket that rides a transport socket in TLS mode -----
  // The heavy lifting (handshake, encrypt/decrypt) is done by the underlying
  // reactor net.Socket once _startTls wraps its fd. This class is the plaintext
  // edge: write/read/pause/resume/backpressure delegate to the transport, which
  // surfaces already-decrypted "data" and, on handshake completion,
  // "secureConnect".
  class TLSSocket extends NetSocket {
    constructor(socket, options) {
      options = options || {};
      // node _tls_wrap.js: a TLSSocket is never half-open regardless of option.
      super({ allowHalfOpen: false });
      this.allowHalfOpen = false;
      this.encrypted = true;
      this.authorized = false;
      this.authorizationError = null;
      this.alpnProtocol = null;
      this.servername = options.servername || undefined;
      this._secureEstablished = false;
      this._securePending = true;
      this.secureConnecting = !options.isServer;
      this.ALPNProtocols = options.ALPNProtocols;
      this._rejectUnauthorized = options.rejectUnauthorized !== false;
      this._requestCert = !!options.requestCert || !options.isServer;
      this._transport = null;
      this._peerCert = null;
      // http2-wrapper (JSStreamSocket) reaches for _handle._parentWrap.constructor.
      this._handle = { _parentWrap: this };
      if (isMbunNetSocket(socket)) this._wrapTransport(socket, options);
      // A Duplex/stream transport (no fd) is DEFERRED: construction still yields
      // a shaped TLSSocket (http2-wrapper only reads _handle); the live handshake
      // is gated in connect() below.
    }
    _wrapTransport(transport, options) {
      this._transport = transport;
      const self = this;
      transport.on("data", (chunk) => self.emit("data", chunk));
      transport.on("drain", () => self.emit("drain"));
      transport.on("end", () => { self.readable = false; self.emit("end"); });
      transport.on("close", (hadErr) => { self.destroyed = true; self.emit("close", !!hadErr); });
      transport.on("error", (e) => self.emit("error", e));
      transport.on("secureConnect", () => {
        self._secureEstablished = true;
        self._securePending = false;
        self.secureConnecting = false;
        // Pull negotiated params + peer cert straight off the SSL* (net.inc
        // tlsInfo). authorized = SSL_get_verify_result == X509_V_OK; with
        // rejectUnauthorized:false a self-signed peer still handshakes but stays
        // unauthorized (node parity). A required-verify handshake only reaches
        // here when the chain validated, so authorized is true there.
        let info = null;
        try { info = NN && transport._fd >= 0 ? NN.tlsInfo(transport._fd) : null; } catch (e) {}
        if (info) {
          self.authorized = !!info.authorized;
          self._protocol = info.protocol || null;
          self._cipherName = info.cipher || "";
          // node: alpnProtocol is the negotiated name, false when ALPN was
          // attempted but nothing matched, null when ALPN was not offered.
          self.alpnProtocol = info.alpnProtocol ? info.alpnProtocol : (self.ALPNProtocols ? false : null);
          if (info.peerCert) self._peerCert = parseCert(info.peerCert);
          if (info.servername) self.servername = self.servername || info.servername;
        } else {
          self.authorized = self._rejectUnauthorized;
        }
        if (!self.authorized && self._rejectUnauthorized) {
          self.authorizationError = self.authorizationError || "UNABLE_TO_VERIFY_LEAF_SIGNATURE";
        }
        if (transport.remoteAddress) self.remoteAddress = transport.remoteAddress;
        self.remotePort = transport.remotePort;
        self.emit("secure");
        self.emit("secureConnect");
      });
      const start = () => {
        if (transport.destroyed) return;
        // verify tri-state (net.inc): client verifies the server unless
        // rejectUnauthorized:false; a server only requests a client cert when
        // requestCert is set, and rejects an unverifiable one only when
        // rejectUnauthorized isn't false (else "optional": request but admit).
        const verify = options.isServer
          ? (options.requestCert ? (options.rejectUnauthorized !== false ? 1 : 2) : 0)
          : (options.rejectUnauthorized !== false ? 1 : 0);
        const ver = resolveVersions(options);
        transport._startTls({
          isServer: !!options.isServer,
          cert: pemOf(options.cert),   // server: own cert; client: mutual-TLS cert
          key: pemOf(options.key),
          ca: pemOf(options.ca),
          servername: options.servername || "",
          verify,
          alpn: alpnCsv(options.ALPNProtocols),
          minVersion: ver.min,
          maxVersion: ver.max,
        });
      };
      // A freshly-created client transport is still connecting; an accepted
      // server socket or an upgrade target is already live — start immediately.
      if (transport._fd >= 0 && !transport.connecting) start();
      else transport.once("connect", start);
      return this;
    }
    write(data, enc, cb) { return this._transport ? this._transport.write(data, enc, cb) : false; }
    end(data, enc, cb) { if (this._transport) this._transport.end(data, enc, cb); return this; }
    pause() { this._paused = true; if (this._transport) this._transport.pause(); return this; }
    resume() { this._paused = false; if (this._transport) this._transport.resume(); return this; }
    isPaused() { return this._transport ? !!this._transport._paused : !!this._paused; }
    destroy(err) { if (this._transport) this._transport.destroy(err); else { this.destroyed = true; this.emit("close", !!err); } return this; }
    setEncoding(enc) { if (this._transport) this._transport.setEncoding(enc); return this; }
    setTimeout(ms, cb) { if (this._transport) this._transport.setTimeout(ms, cb); return this; }
    setNoDelay() { return this; }
    setKeepAlive() { return this; }
    ref() { if (this._transport) this._transport.ref(); return this; }
    unref() { if (this._transport) this._transport.unref(); return this; }
    address() { return this._transport ? this._transport.address() : {}; }
    getPeerCertificate(detailed) { return this._transport ? (this._peerCert || {}) : null; }
    getCertificate() { return null; }
    getCipher() {
      const n = this._cipherName || "";
      if (!n) return {};
      return { name: n, standardName: n, version: this._protocol || "TLSv1.3" };
    }
    getProtocol() { return this._secureEstablished ? (this._protocol || "TLSv1.3") : null; }
    getSession() { return undefined; }
    getEphemeralKeyInfo() { return null; }
    getSharedSigalgs() { return []; }
    getFinished() { return undefined; }
    getPeerFinished() { return undefined; }
    getTLSTicket() { return undefined; }
    isSessionReused() { return false; }
    setServername(name) { this.servername = name; return this; }
    setSession() { return this; }
    setMaxSendFragment() { return false; }
    disableRenegotiation() {}
    enableTrace() {}
    exportKeyingMaterial() { throw deferredTLS("TLSSocket.exportKeyingMaterial"); }
    renegotiate() { return false; }
  }

  // ---- tls.connect(): open (or adopt) a socket and drive the client handshake -
  // Signatures: connect(options[,cb]) and connect(port[,host][,options][,cb]).
  function connect(...args) {
    let opts, cb = null;
    if (typeof args[0] === "object" && args[0] !== null) { opts = args[0]; cb = typeof args[1] === "function" ? args[1] : null; }
    else {
      opts = (typeof args[2] === "object" && args[2] !== null) ? Object.assign({}, args[2]) : {};
      opts.port = args[0];
      if (typeof args[1] === "string") opts.host = args[1];
      for (let i = 1; i < args.length; i++) if (typeof args[i] === "function") { cb = args[i]; break; }
    }
    const host = opts.host || opts.hostname || "localhost";
    const servername = opts.servername != null ? opts.servername
      : (typeof opts.host === "string" && !netIsIP(opts.host) ? opts.host : "");
    const transportOpt = opts.socket;
    const tlsOpts = { isServer: false, servername, ca: opts.ca, cert: opts.cert, key: opts.key, rejectUnauthorized: opts.rejectUnauthorized, ALPNProtocols: opts.ALPNProtocols,
      minVersion: opts.minVersion, maxVersion: opts.maxVersion, secureProtocol: opts.secureProtocol };

    if (isMbunNetSocket(transportOpt)) {
      const tlsSock = new TLSSocket(transportOpt, tlsOpts);
      if (cb) tlsSock.once("secureConnect", cb);
      return tlsSock;
    }
    if (transportOpt) {
      // Duplex/stream transport (no fd): live TLS over a JS Duplex is DEFERRED.
      const tlsSock = new TLSSocket(null, tlsOpts);
      if (cb) tlsSock.once("secureConnect", cb);
      G.queueMicrotask(() => tlsSock.emit("error", deferredTLS("connect (Duplex socket transport)")));
      return tlsSock;
    }
    const transport = new NetSocket({ allowHalfOpen: false });
    const tlsSock = new TLSSocket(transport, tlsOpts);
    if (cb) tlsSock.once("secureConnect", cb);
    transport.connect(opts.port | 0, String(host));
    return tlsSock;
  }

  // ---- Server / createServer: net.Server that upgrades each connection to TLS -
  // server emits "secureConnection" (with the TLSSocket), the socket emits
  // "secureConnect"; a pre-handshake failure surfaces as "tlsClientError".
  class Server extends NetServer {
    constructor(options, secureConnectionListener) {
      if (typeof options === "function") { secureConnectionListener = options; options = {}; }
      super();
      this._sharedCreds = options || {};
      this._contexts = new Map();
      if (typeof secureConnectionListener === "function" && typeof this.on === "function")
        this.on("secureConnection", secureConnectionListener);
      if (typeof this.on === "function") this.on("connection", (raw) => this._onSecureConnection(raw));
    }
    _onSecureConnection(raw) {
      const creds = this._sharedCreds || {};
      const tlsSock = new TLSSocket(raw, {
        isServer: true, cert: creds.cert, key: creds.key, ca: creds.ca,
        requestCert: creds.requestCert, rejectUnauthorized: creds.rejectUnauthorized,
        ALPNProtocols: creds.ALPNProtocols,
        minVersion: creds.minVersion, maxVersion: creds.maxVersion, secureProtocol: creds.secureProtocol,
      });
      const self = this;
      tlsSock.once("secureConnect", () => self.emit("secureConnection", tlsSock));
      raw.once("error", (e) => { if (!tlsSock._secureEstablished) self.emit("tlsClientError", e, tlsSock); });
    }
    setSecureContext(options) { this._sharedCreds = options || {}; }
    addContext(servername, context) { this._contexts.set(servername, context); }
    getTicketKeys() { return Buffer ? Buffer.alloc(48) : new Uint8Array(48); }
    setTicketKeys() { return this; }
  }
  function createServer(options, connectionListener) { return new Server(options, connectionListener); }

  // ---- overwrite the deferred stubs installed by builtins/node_tls.cppm -------
  try { T.TLSSocket = TLSSocket; } catch (e) {}
  try { T.Server = Server; } catch (e) {}
  try { T.createServer = createServer; } catch (e) {}
  try { T.connect = connect; } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::js_tls_live
