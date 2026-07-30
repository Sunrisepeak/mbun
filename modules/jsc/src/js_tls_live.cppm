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

  const x509Of = (pem) => {
    if (!pem) return undefined;
    const crypto = M["crypto"] || M["node:crypto"];
    if (!crypto || typeof crypto.X509Certificate !== "function") return undefined;
    try { return new crypto.X509Certificate(pem); } catch (e) { return undefined; }
  };

  // ---- node argument validators (lib/internal/validators.js subset) ----------
  // The live surface is where node runs most of its TLS option validation
  // (internal/tls/wrap.js Server / TLSSocket / connect), and the corpus asserts
  // the exact message, so determineSpecificType is translated from
  // lib/internal/errors.js rather than approximated.
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
  // node's ERR_INVALID_ARG_VALUE reports `inspect(value)`, NOT
  // determineSpecificType — so a string value comes back as 'x', not
  // "type string ('x')". ref lib/internal/errors.js.
  const invalidArgValue = (name, value, reason) => {
    let shown;
    if (typeof value === "string") shown = "'" + value + "'";
    else if (typeof value === "bigint") shown = String(value) + "n";
    else if (typeof value === "function") shown = "[Function: " + (value.name || "anonymous") + "]";
    else if (value === null || typeof value !== "object") shown = String(value);
    else { try { shown = JSON.stringify(value); } catch (e) { shown = String(value); } }
    const e = new TypeError("The " + (String(name).indexOf(".") !== -1 ? "property" : "argument") +
      " '" + name + "' " + reason + ". Received " + shown);
    e.code = "ERR_INVALID_ARG_VALUE";
    return e;
  };
  const validateString = (v, name) => {
    if (typeof v !== "string") throw argTypeError(name, "must be of type string", v);
  };
  const validateNumber = (v, name, min, max) => {
    if (typeof v !== "number") throw argTypeError(name, "must be of type number", v);
    if ((min != null && v < min) || (max != null && v > max) || ((min != null || max != null) && v !== v))
      throw outOfRange(name, (min != null ? ">= " + min : "") + (min != null && max != null ? " && " : "") + (max != null ? "<= " + max : ""), v);
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
  const validateFunction = (v, name) => {
    if (typeof v !== "function") throw argTypeError(name, "must be of type Function", v);
  };
  const validateObject = (v, name) => {
    if (v === null || Array.isArray(v) || typeof v !== "object") throw argTypeError(name, "must be of type object", v);
  };
  const validateBuffer = (v, name) => {
    if (!ArrayBuffer.isView(v)) throw argTypeError(name === undefined ? "buffer" : name, "must be an instance of Buffer, TypedArray, or DataView", v);
  };

  // node internal/tls/wrap.js: `--tls-keylog=<file>` makes EVERY TLSSocket append
  // its NSS key-material lines to that file, with the one-time warning node
  // prints for it. This is an operator-only debugging switch: it is reachable
  // solely from an explicit command-line flag, the file is created 0600, and
  // nothing is written when the flag is absent (the reactor does not even drain
  // the lines — _keylogWanted stays false).
  const tlsKeylogFile = (() => {
    const argv = (G.process && G.process.execArgv) || [];
    for (let i = 0; i < argv.length; i++) {
      const a = argv[i];
      if (typeof a === "string" && a.startsWith("--tls-keylog=")) return a.slice(13);
    }
    return "";
  })();
  let warnedTlsKeylog = false;
  const appendKeylog = (line) => {
    const fsm = M["fs"] || M["node:fs"];
    if (!fsm || typeof fsm.appendFileSync !== "function") return;
    try { fsm.appendFileSync(tlsKeylogFile, line, { mode: 0o600 }); } catch (e) {}
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
    // node configSecureContext accepts `{ pem, passphrase }` with pem as a
    // string OR a Buffer; recursing covers both, where `typeof v.pem === string`
    // silently fell through to v.toString() → "[object Object]" for the Buffer
    // form (test-tls-passphrase's `key: [{ pem: passKey }]` cases).
    if (v && v.pem != null) return pemOf(v.pem);
    if (ArrayBuffer.isView(v) || v instanceof ArrayBuffer) {
      try { return (Buffer ? Buffer.from(v.buffer ? v.buffer : v, v.byteOffset || 0, v.byteLength) : v).toString("utf8"); }
      catch (e) { return String(v); }
    }
    return typeof v.toString === "function" ? v.toString() : String(v);
  };
  // node configSecureContext: for `key: [entry, …]` each entry may carry its own
  // `passphrase`, which WINS over options.passphrase; a plain (non-array) key
  // uses options.passphrase. mbun's engine loads one key per context, so the
  // first entry's is the one that applies.
  const keyPassphraseOf = (key, fallback) => {
    let entry = key;
    if (Array.isArray(entry)) entry = entry.length > 0 ? entry[0] : null;
    if (entry && typeof entry === "object" && typeof entry.passphrase === "string") return entry.passphrase;
    return typeof fallback === "string" ? fallback : "";
  };
  const isMbunNetSocket = (s) => !!s && typeof s._startTls === "function" && typeof s.on === "function";
  // Session material (a serialised SSL_SESSION, a 48-byte ticket key) crosses to
  // the native layer base64-encoded, the same way every other opaque byte string
  // in this reactor does. A non-buffer is dropped rather than coerced: handing
  // the engine a mangled session would silently turn a resumption into a full
  // handshake with no way to tell the two apart.
  const b64Of = (v) => {
    if (v == null || !Buffer) return "";
    try {
      if (Buffer.isBuffer(v)) return v.toString("base64");
      if (ArrayBuffer.isView(v)) return Buffer.from(v.buffer, v.byteOffset, v.byteLength).toString("base64");
      if (v instanceof ArrayBuffer) return Buffer.from(v).toString("base64");
    } catch (e) {}
    return "";
  };
  // node's tls.Server picks a random session-ticket key per Server when the
  // caller gave none (src/crypto/crypto_context.cc SecureContext::Init →
  // RandBytes). It must be a CSPRNG: the ticket key is what authenticates and
  // encrypts every resumable session this server hands out, so a predictable one
  // would be a straight session-forgery hole. If no CSPRNG is reachable, return
  // null and leave OpenSSL's own per-context random key in place — that costs
  // resumption, which is the safe way to fail.
  const randomTicketKeys = () => {
    const crypto = M["crypto"] || M["node:crypto"];
    if (crypto && typeof crypto.randomBytes === "function") {
      try { return crypto.randomBytes(48); } catch (e) {}
    }
    if (G.crypto && typeof G.crypto.getRandomValues === "function" && Buffer) {
      try { return Buffer.from(G.crypto.getRandomValues(new Uint8Array(48))); } catch (e) {}
    }
    return null;
  };
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
  // node does NOT hand min=max=0 to SecureContext::Init when `secureProtocol` is
  // set — lib/internal/tls/common.js always passes toV(minVersion,DEFAULT_MIN)
  // and toV(maxVersion,DEFAULT_MAX), and src/crypto/crypto_context.cc
  // SecureContext::Init then adjusts ONE OR BOTH bounds per method name:
  //
  //   TLS_method / _client_ / _server_   min = 0 (no floor), max = TLS1.3
  //   SSLv23_method / _client_/_server_  max = TLS1.2, min LEFT AT DEFAULT_MIN
  //   TLSv1_method   (+client/server)    min = max = TLS1.0
  //   TLSv1_1_method (+client/server)    min = max = TLS1.1
  //   TLSv1_2_method (+client/server)    min = max = TLS1.2
  //
  // The SSLv23 row is the one that matters: it is "any supported protocol at or
  // above the default minimum", not "anything at all". Treating it as fully
  // unpinned let an SSLv23 peer negotiate TLS 1.0/1.1 against a TLSv1_method
  // peer, where node fails the handshake (test-tls-min-max-version, and through
  // it all five test-tls-cli-{min,max}-version-* files, assert exactly that).
  // Widening a version window is never a safe default, so the floor stays.
  const SECURE_PROTOCOL_SUFFIXES = ["_method", "_client_method", "_server_method"];
  const securePrefix = (sp) => {
    if (typeof sp !== "string") return null;
    for (const suffix of SECURE_PROTOCOL_SUFFIXES) {
      if (sp.endsWith(suffix)) return sp.slice(0, sp.length - suffix.length);
    }
    return null;
  };
  const resolveVersions = (options) => {
    if (typeof options.secureProtocol === "string" && options.secureProtocol) {
      // "none" is the native layer's explicit-unpinned marker (net.inc
      // version_arg); "" would fall back to its TLS 1.2 default floor.
      switch (securePrefix(options.secureProtocol)) {
        case "TLS": return { min: "none", max: "TLSv1.3" };
        case "SSLv23": return { min: T.DEFAULT_MIN_VERSION || "", max: "TLSv1.2" };
        case "TLSv1": return { min: "TLSv1", max: "TLSv1" };
        case "TLSv1_1": return { min: "TLSv1.1", max: "TLSv1.1" };
        case "TLSv1_2": return { min: "TLSv1.2", max: "TLSv1.2" };
        case "TLSv1_3": return { min: "TLSv1.3", max: "TLSv1.3" };
        default: return { min: "none", max: "none" };
      }
    }
    let min = options.minVersion;
    let max = options.maxVersion;
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
  // node accepts the credentials either inline or wrapped in a SecureContext
  // (`new tls.TLSSocket(sock, { isServer: true, secureContext })`, which the
  // corpus uses whenever it drives a handshake over a socket it made itself).
  // Fold the context's stored options in underneath the explicit ones; an
  // explicitly-undefined key must not shadow the context's value.
  const mergeSecureContext = (options) => {
    const sc = options && options.secureContext;
    const base = sc && sc._secureOptions;
    if (!base) return options;
    const out = Object.assign({}, base);
    for (const k in options) if (options[k] !== undefined) out[k] = options[k];
    return out;
  };

  class TLSSocket extends NetSocket {
    constructor(socket, options) {
      options = mergeSecureContext(options || {});
      // node internal/tls/wrap.js TLSSocket: the transport must be a stream (it
      // is wrapped in a JSStreamSocket otherwise), so a bare EventEmitter is a
      // TypeError rather than a runtime error deep in the handshake.
      // ref nodejs/node#3655, test-tls-wrap-event-emmiter.
      if (socket != null && !isMbunNetSocket(socket) &&
          !(typeof socket === "object" &&
            (typeof socket.write === "function" || typeof socket.pipe === "function" ||
             typeof socket._read === "function" || typeof socket._write === "function"))) {
        throw argTypeError("socket", "must be an instance of net.Socket or stream.Duplex", socket);
      }
      if (options.SNICallback !== undefined && options.SNICallback !== null)
        validateFunction(options.SNICallback, "options.SNICallback");
      if (options.pskCallback !== undefined && options.pskCallback !== null)
        validateFunction(options.pskCallback, "options.pskCallback");
      // node internal/tls/wrap.js TLSSocket:
      //   allowHalfOpen: socket ? socket.allowHalfOpen : tlsOptions.allowHalfOpen
      // — when a transport is adopted the transport decides (a net.Socket
      // defaults to false), and only a socket-less TLSSocket honours the option.
      const halfOpen = socket ? !!socket.allowHalfOpen : !!options.allowHalfOpen;
      // PORT-SOURCE: compat/node/lib/internal/tls/wrap.js:590-601 — TLSSocket's
      // net.Socket call forwards noDelay / keepAlive / keepAliveInitialDelay. In
      // node the TLSSocket IS the connecting socket, so the Socket constructor is
      // where those land; mbun's rides a hidden transport, and dropping them here
      // left the TLSSocket reporting keep-alive off for a connection that had
      // asked for it. They are CACHED options either way — net.Socket only pushes
      // them to a handle once one exists — so forwarding them is a record of what
      // the caller asked for, not a second syscall.
      super({ allowHalfOpen: halfOpen, highWaterMark: options.highWaterMark,
              noDelay: options.noDelay, keepAlive: options.keepAlive,
              keepAliveInitialDelay: options.keepAliveInitialDelay });
      this.allowHalfOpen = halfOpen;
      this.encrypted = true;
      this.authorized = false;
      this.authorizationError = null;
      this.alpnProtocol = null;
      this.servername = options.servername || undefined;
      this._secureEstablished = false;
      this._securePending = true;
      this.secureConnecting = !options.isServer;
      this._isServer = !!options.isServer;
      this.ALPNProtocols = options.ALPNProtocols;
      this._rejectUnauthorized = options.rejectUnauthorized !== false;
      this._requestCert = !!options.requestCert || !options.isServer;
      this._transport = null;
      this._peerCert = null;
      // http2-wrapper (JSStreamSocket) reaches for _handle._parentWrap.constructor.
      this._handle = { _parentWrap: this };
      // 'keylog' costs a native drain per poll, so the reactor only performs it
      // once something has asked. Any listener added at any time flips the flag —
      // node installs its keylog callback eagerly, mbun defers the WORK, not the
      // semantics. Watching 'newListener' catches on/once/addListener/prepend*
      // alike, including the https.Agent's own onkeylog forward.
      this._keylogWanted = false;
      // Same deal for 'session': the reactor only asks OpenSSL for the sessions
      // it issued once something wants them. getSession()/isSessionReused() read
      // the SSL* directly and do not need this, so the flag tracks the EVENT.
      this._sessionWanted = false;
      this._sessionReused = false;
      // node's tls.connect({ session }) / socket.setSession(): the session is
      // offered when the handshake starts, which for a client whose TCP leg is
      // still in flight is after the caller has had a chance to call setSession.
      this._sessionToResume = options.session != null ? options.session : null;
      this.on("newListener", (ev) => {
        if (ev === "session" && !this._sessionWanted) {
          this._sessionWanted = true;
          if (this._transport) this._transport._sessionWanted = true;
          return;
        }
        if (ev !== "keylog" || this._keylogWanted) return;
        this._keylogWanted = true;
        if (this._transport) this._transport._keylogWanted = true;
      });
      // --tls-keylog=<file>: node registers this per socket in TLSSocket._init,
      // for both roles, and warns once. Registered here (after the newListener
      // hook above) so the flag itself is what turns the native drain on.
      if (tlsKeylogFile) {
        if (!warnedTlsKeylog) {
          warnedTlsKeylog = true;
          try {
            G.process.emitWarning("Using --tls-keylog makes TLS connections insecure " +
                                  "by writing secret key material to file " + tlsKeylogFile);
          } catch (e) {}
        }
        this.on("keylog", appendKeylog);
      }
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
      // A destroyed socket is neither readable nor writable — node's
      // Socket#destroy clears both, and readyState (net.js) is derived from
      // them, so leaving `writable` true made a closed TLSSocket report
      // 'writeOnly' (test-tls-set-encoding asserts 'closed' inside 'close').
      transport.on("close", (hadErr) => {
        self.destroyed = true; self.readable = false; self.writable = false;
        self.emit("close", !!hadErr);
      });
      // node internal/tls/wrap.js onConnectEnd: a transport that disconnects
      // BEFORE the handshake completes is reported as an ECONNRESET carrying the
      // connect options, not as a bare read error — the corpus asserts path /
      // host / port / localAddress on it (test-tls-wrap-econnreset*). The same
      // logic lives in _http_client.js, so keep the shape identical.
      transport.on("error", (e) => {
        if (!self._secureEstablished && e && e.code === "ECONNRESET" && !self._hadError) {
          self._hadError = true;
          const o = self._connectOptions;
          if (o) {
            e.message = "Client network socket disconnected before secure TLS connection was established";
            e.path = o.path;
            e.host = o.host;
            e.port = o.port;
            e.localAddress = o.localAddress;
          }
        }
        self.emit("error", e);
      });
      // setTimeout() arms the timer on the transport, so the 'timeout' event
      // fires there — but every consumer (node:https' server keep-alive sweep,
      // socket.setTimeout(ms, cb), the corpus' own listeners) is attached to the
      // TLSSocket. Without this forward, `server.keepAliveTimeout` silently never
      // expired an https connection: server.close() left the accepted socket
      // pinned and the process could not leave the loop (13 test-https-* files
      // hit the 15s corpus timeout on exactly that).
      transport.on("timeout", () => self.emit("timeout"));
      // node crypto_context.cc SecureContext::KeylogCallback -> tls/wrap.js
      // onkeylog: each NSS keylog line reaches the TLSSocket as a Buffer that
      // already carries its trailing newline. The reactor only asks OpenSSL for
      // the lines when something wants them (_keylogWanted below), so a process
      // with no listener never moves key material out of the engine.
      transport.on("keylog", (line) => self.emit("keylog", line));
      if (self._keylogWanted) transport._keylogWanted = true;
      // node internal/tls/wrap.js onnewsession: each session OpenSSL issues (one
      // for TLS 1.2, one per NewSessionTicket for TLS 1.3) reaches the TLSSocket
      // as a Buffer the caller can hand back to tls.connect({ session }).
      // The session most recently handed to JS also becomes what getSession()
      // reports. In node the two agree by construction — the 'session' event is
      // emitted from OpenSSL's new-session callback, at the moment SSL_get_session
      // starts returning that very session. Here the sessions are drained from a
      // poll, so several TLS 1.3 tickets can already have been processed by the
      // time the first event is delivered and a live SSL_get_session would report
      // the LAST one to every handler. Caching restores node's pairing at every
      // point JS can observe: nothing cached before the first event (so
      // getSession() is still the live handshake session, the TLS 1.3 dummy
      // included), then the session that event carried.
      transport.on("session", (session) => { self._lastSession = session; self.emit("session", session); });
      if (self._sessionWanted) transport._sessionWanted = true;
      // node's TLSSocket is a net.Socket over a real connection, so it emits
      // 'connect' when the TCP leg lands (before the handshake) and 'ready'
      // after. The corpus drives raw TLS clients from 'connect'
      // (test-https-server-close-idle / -close-all write their request there);
      // without the forward those callbacks never ran and the file hung.
      transport.on("connect", () => { self.emit("connect"); self.emit("ready"); });
      // Byte counters, buffer levels and the local endpoint live on the
      // TRANSPORT — every write() and read() this class performs is delegated
      // there — so the plaintext edge must report the transport's values, not the
      // zeros net.Socket's constructor left on it. defineProperty (not a
      // prototype getter) because super() already installed bytesRead /
      // bytesWritten as own data properties, which would shadow one.
      const alias = (name) => {
        try { Object.defineProperty(self, name, { get: () => transport[name], configurable: true, enumerable: true }); }
        catch (e) {}
      };
      alias("bytesWritten"); alias("bytesRead");
      alias("bufferSize"); alias("writableLength");
      alias("readableHighWaterMark"); alias("writableHighWaterMark");
      alias("localAddress"); alias("localPort");
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
          self._cipherStandardName = info.cipherStandardName || "";
          // node crypto_tls.cc TLSWrap::GetALPNNegotiatedProto: SSL_get0_alpn_
          // selected yielding a zero-length protocol is reported as `false`,
          // whether or not this side offered ALPN. `null` is only the
          // constructor's initial value, i.e. "the handshake has not finished".
          self.alpnProtocol = info.alpnProtocol ? info.alpnProtocol : false;
          self._sessionReused = !!info.sessionReused;
          if (info.peerCert) { self._peerCert = parseCert(info.peerCert); self._peerCertPem = info.peerCert; }
          // The verified chain (leaf first) backs getPeerCertificate(true)'s
          // `issuerCertificate` walk. node links each cert to its issuer and
          // makes the root point at ITSELF, which is the loop terminator the
          // corpus walks on (test-tls-cert-chains-*).
          if (Array.isArray(info.peerChain) && info.peerChain.length) {
            self._peerChainPem = info.peerChain;
            const parsed = info.peerChain.map((p) => parseCert(p));
            for (let i = 0; i < parsed.length; i++) {
              parsed[i].issuerCertificate = i + 1 < parsed.length ? parsed[i + 1] : parsed[i];
            }
            // node's non-detailed getPeerCertificate() carries no
            // issuerCertificate, so the flat leaf stays as parseCert produced it.
            self._peerCertDetailed = parsed[0];
          }
          if (typeof info.finished === "string" && info.finished && Buffer)
            self._finished = Buffer.from(info.finished, "base64");
          if (typeof info.peerFinished === "string" && info.peerFinished && Buffer)
            self._peerFinished = Buffer.from(info.peerFinished, "base64");
          if (info.servername) self.servername = self.servername || info.servername;
          // node TLSWrap::GetEphemeralKeyInfo, read at handshake completion:
          // absent for a static-RSA suite, which node reports as `{}`.
          if (info.ephemeralKey) self._ephemeralKeyInfo = info.ephemeralKey;
        } else {
          self.authorized = self._rejectUnauthorized;
        }
        if (!self.authorized && self._rejectUnauthorized) {
          self.authorizationError = self.authorizationError || "UNABLE_TO_VERIFY_LEAF_SIGNATURE";
        }
        // Caller-supplied peer-name check. node runs it in onConnectSecure only
        // when the CHAIN verified (a chain failure already decided the outcome),
        // records `err.code` as authorizationError, and destroys the socket with
        // that error when rejectUnauthorized is on. A function that throws is not
        // swallowed: node lets it propagate, and so must this — swallowing it
        // would turn a rejected peer into an accepted one.
        if (!options.isServer && typeof options.checkServerIdentity === "function" && self.authorized) {
          const identErr = options.checkServerIdentity(
            options.identityHost || self.servername || "",
            self.getPeerCertificate(true));
          if (identErr) {
            self.authorized = false;
            self.authorizationError = identErr.code || identErr.message;
            if (self._rejectUnauthorized) {
              self.destroy(identErr);
              return;
            }
          }
        }
        if (transport.remoteAddress) self.remoteAddress = transport.remoteAddress;
        self.remotePort = transport.remotePort;
        self.emit("secure");
        self.emit("secureConnect");
      });
      const start = () => {
        transport._tlsPending = false;
        if (transport.destroyed) return;
        // verify tri-state (net.inc): client verifies the server unless
        // rejectUnauthorized:false; a server only requests a client cert when
        // requestCert is set, and rejects an unverifiable one only when
        // rejectUnauthorized isn't false (else "optional": request but admit).
        const verify = options.isServer
          ? (options.requestCert ? (options.rejectUnauthorized !== false ? 1 : 2) : 0)
          : (options.rejectUnauthorized !== false ? 1 : 0);
        const ver = resolveVersions(options);
        // node lib/internal/tls/secure-context.js configSecureContext: a context
        // with no `ca` calls context.addRootCerts(), and
        // tls.setDefaultCACertificates() REPLACES what that installs. mbun's
        // engine falls back to the platform store when `ca` is empty, so the
        // mutated default has to be handed over explicitly or it never takes
        // effect (test-tls-set-default-ca-certificates-*-https-request).
        let caPem = pemOf(options.ca);
        let caComplete = false;
        if (!caPem && options.ca == null && T && typeof T.__mbunDefaultCAPem === "function") {
          const overridden = T.__mbunDefaultCAPem();
          if (overridden !== null && overridden !== undefined) { caPem = overridden; caComplete = true; }
        }
        // node processCiphers: TLS 1.3 suites go to SSL_CTX_set_ciphersuites,
        // everything else to SSL_CTX_set_cipher_list. An empty/absent option
        // still means "leave the engine's own defaults alone" on both slots.
        const ciphers = typeof options.ciphers === "string" && options.ciphers
          ? (T && typeof T.__mbunProcessCiphers === "function"
              ? T.__mbunProcessCiphers(options.ciphers)
              : { cipherList: options.ciphers, cipherSuites: "" })
          : { cipherList: "", cipherSuites: "" };
        // node configSecureContext: a caller who named ONLY TLS 1.3 suites has no
        // <=TLS1.2 cipher at all, so the context floor is raised to TLS 1.3
        // rather than left to fail the handshake with "no shared cipher".
        if (typeof options.ciphers === "string" && options.ciphers
            && ciphers.cipherList === "" && ciphers.cipherSuites !== ""
            && ver.min !== "TLSv1.3" && ver.max !== "TLSv1.2") {
          ver.min = "TLSv1.3";
        }
        self._ownCertPem = pemOf(options.cert) || null;
        transport._startTls({
          isServer: !!options.isServer,
          cert: pemOf(options.cert),   // server: own cert; client: mutual-TLS cert
          key: pemOf(options.key),
          ca: caPem,
          // caComplete: `ca` is the WHOLE trust store, so do not fall back to
          // the platform one — including when it is empty, which is how
          // tls.setDefaultCACertificates([]) means "trust nothing".
          caComplete: caComplete,
          servername: options.servername || "",
          verify,
          alpn: alpnCsv(options.ALPNProtocols),
          minVersion: ver.min,
          maxVersion: ver.max,
          // node SecureContext::SetCiphers / SetCipherSuites. Only a
          // caller-supplied list is sent; "" leaves that slot's engine default
          // untouched.
          ciphers: ciphers.cipherList,
          cipherSuites: ciphers.cipherSuites,
          hostCheck: !(typeof options.checkServerIdentity === "function"),
          // Client: the session to resume, read at handshake-start time so a
          // setSession() between tls.connect() and the TCP 'connect' still
          // counts. Server: the Server's stable session-ticket key, without
          // which a ticket issued here could never be decrypted by the next
          // connection (each one builds its own SSL_CTX).
          session: options.isServer ? "" : b64Of(self._sessionToResume),
          ticketKeys: options.isServer ? b64Of(options.ticketKeys) : "",
          // node configSecureContext setKey(pem, passphrase). Without this an
          // encrypted key reached PEM_read_bio_PrivateKey with a NULL password
          // and OpenSSL prompted on the terminal.
          passphrase: keyPassphraseOf(options.key, options.passphrase),
          // node tls.Server: honorCipherOrder defaults to true for a SERVER and
          // is never set for a client (internal/tls/wrap.js Server ctor).
          honorCipherOrder: options.isServer ? options.honorCipherOrder !== false : false,
          // node configSecureContext setDHParam/setECDHCurve. `dhparam: 'auto'`
          // is OpenSSL's own RFC 7919 group selection; a PEM block is loaded as
          // given. Without either, no DHE-* suite is negotiable at all.
          dhparam: options.dhparam === "auto" ? "auto" : pemOf(options.dhparam),
          ecdhCurve: typeof options.ecdhCurve === "string" ? options.ecdhCurve : "",
        });
      };
      // A live fd means the reactor's connect() already returned, whether this is
      // an accepted server socket, an upgrade target, or a client whose
      // `connecting` flag is still set until its 'connect' event fires.
      // Until start() runs, the transport must not flush anything as cleartext
      // (js_net.cppm _flush reads _tlsPending) — a caller that writes before the
      // handshake begins, as node:https ClientRequest does, would otherwise put
      // its request in front of the ClientHello.
      if (transport._fd >= 0) start();
      else { transport._tlsPending = true; transport.once("connect", start); }
      return this;
    }
    write(data, enc, cb) { return this._transport ? this._transport.write(data, enc, cb) : false; }
    end(data, enc, cb) { if (this._transport) this._transport.end(data, enc, cb); return this; }
    pause() { this._paused = true; if (this._transport) this._transport.pause(); return this; }
    resume() { this._paused = false; if (this._transport) this._transport.resume(); return this; }
    isPaused() { return this._transport ? !!this._transport._paused : !!this._paused; }
    destroy(err) { if (this._transport) this._transport.destroy(err); else { this.destroyed = true; this.emit("close", !!err); } return this; }
    setEncoding(enc) { if (this._transport) this._transport.setEncoding(enc); return this; }
    // node attaches the callback to THIS socket's 'timeout' (the transport merely
    // owns the timer); the forward installed in _wrapTransport re-emits here, so
    // registering the callback on the transport too would fire it twice.
    setTimeout(ms, cb) {
      if (typeof cb === "function") this.once("timeout", cb);
      // node net.Socket#setTimeout publishes the interval on the socket the
      // caller holds, which for TLS is this plaintext edge.
      this.timeout = (ms | 0) === 0 ? undefined : (ms | 0);
      if (this._transport) this._transport.setTimeout(ms);
      return this;
    }
    setNoDelay() { return this; }
    setKeepAlive() { return this; }
    ref() { if (this._transport) this._transport.ref(); return this; }
    unref() { if (this._transport) this._transport.unref(); return this; }
    address() { return this._transport ? this._transport.address() : {}; }
    getPeerCertificate(detailed) {
      if (!this._transport) return null;
      if (detailed && this._peerCertDetailed) return this._peerCertDetailed;
      return this._peerCert || {};
    }
    // node crypto_tls.cc TLSWrap::GetCertificate: THIS side's certificate, in
    // the same object shape getPeerCertificate() produces — for a server that is
    // the cert it presented, which is the client's peer cert. null when this
    // side has none (the ordinary client case), as node returns.
    getCertificate() {
      if (!this._ownCertPem) return null;
      if (this._ownCert === undefined) this._ownCert = parseCert(this._ownCertPem);
      return this._ownCert;
    }
    // node getX509Certificate()/getPeerX509Certificate(): the same certificates
    // getCertificate()/getPeerCertificate() report, as crypto.X509Certificate
    // objects.
    getX509Certificate() { return x509Of(this._ownCertPem); }
    // node crypto_tls.cc TLSWrap::GetPeerX509Certificate walks the whole peer
    // chain and links each certificate to its issuer. The chain is the one the
    // handshake already verified (`_peerChainPem`, leaf first) — the same list
    // getPeerCertificate(true) walks — so this adds no trust decision of its
    // own, it only reports what was verified.
    //
    // The terminator differs from getPeerCertificate(true) and the difference is
    // asserted: there node makes the ROOT point at ITSELF (a loop the corpus
    // walks until `cert === cert.issuerCertificate`), while the X509 chain
    // simply ENDS, so the root's issuerCertificate is `undefined`
    // (test-tls-getcertificate-x509 checks exactly that).
    getPeerX509Certificate() {
      const chain = (Array.isArray(this._peerChainPem) && this._peerChainPem.length)
        ? this._peerChainPem : (this._peerCertPem ? [this._peerCertPem] : []);
      if (!chain.length) return undefined;
      const certs = [];
      for (const pem of chain) {
        const c = x509Of(pem);
        // An unparseable link ends the walk rather than dropping the leaf: the
        // caller still gets the certificate the peer presented.
        if (c === undefined) break;
        certs.push(c);
      }
      if (!certs.length) return undefined;
      for (let i = 0; i + 1 < certs.length; i++) certs[i].issuerCertificate = certs[i + 1];
      return certs[0];
    }
    getCipher() {
      const n = this._cipherName || "";
      if (!n) return {};
      return { name: n, standardName: this._cipherStandardName || n, version: this._protocol || "TLSv1.3" };
    }
    // node crypto_tls.cc TLSWrap::GetProtocol reads SSL_get_version off the
    // handle, and internal/tls/wrap.js nulls the handle on close — so a closed
    // socket answers null, not the protocol it used to speak. The corpus asserts
    // exactly that inside its own 'close' listener (test-tls-getprotocol).
    getProtocol() {
      if (!this._secureEstablished || this.destroyed) return null;
      return this._protocol || "TLSv1.3";
    }
    // node crypto_tls.cc TLSWrap::GetSession: i2d_SSL_SESSION of the SSL*'s
    // CURRENT session, read live rather than cached — for TLS 1.3 the session
    // available the instant the handshake finishes is the unresumable
    // placeholder, and the resumable one only appears later (the 'session'
    // event). undefined when there is no session at all, as node returns.
    getSession() {
      if (this._lastSession !== undefined) return this._lastSession;
      const fd = this._transport ? this._transport._fd : -1;
      if (!NN || typeof NN.tlsSession !== "function" || !(fd >= 0)) return undefined;
      let b64;
      try { b64 = NN.tlsSession(fd); } catch (e) { return undefined; }
      if (typeof b64 !== "string" || b64 === "") return undefined;
      return Buffer ? Buffer.from(b64, "base64") : b64;
    }
    // node crypto_tls.cc TLSWrap::GetEphemeralKeyInfo: "tmp key is available on
    // only client" — a SERVER answers null. A client whose suite has no
    // ephemeral key (static RSA) answers `{}`, not null.
    getEphemeralKeyInfo() {
      if (this._isServer) return null;
      return this._ephemeralKeyInfo || {};
    }
    getSharedSigalgs() { return []; }
    // node: undefined until the handshake completes, then a non-empty Buffer.
    getFinished() { return this._finished; }
    getPeerFinished() { return this._peerFinished; }
    // node crypto_tls.cc TLSWrap::GetTLSTicket: SSL_SESSION_get0_ticket of the
    // current session. undefined when the session carries no ticket.
    getTLSTicket() {
      const fd = this._transport ? this._transport._fd : -1;
      if (!NN || typeof NN.tlsTicket !== "function" || !(fd >= 0)) return undefined;
      let b64;
      try { b64 = NN.tlsTicket(fd); } catch (e) { return undefined; }
      if (typeof b64 !== "string" || b64 === "") return undefined;
      return Buffer ? Buffer.from(b64, "base64") : b64;
    }
    // node internal/tls/wrap.js _destroySSL(): tears the SSL* down without
    // touching the transport. mbun's channel lives with the transport fd, so the
    // teardown is a no-op beyond dropping the cached negotiated state — the point
    // of the corpus test is only that calling it does not crash.
    _destroySSL() {
      this._secureEstablished = false;
      this._peerCert = null;
      this._peerCertDetailed = null;
    }
    // node crypto_tls.cc TLSWrap::IsSessionReused — SSL_session_reused, captured
    // when the handshake landed (the SSL* is gone once the fd is closed, and the
    // corpus asks inside 'close' handlers).
    isSessionReused() { return !!this._sessionReused; }
    // node internal/tls/wrap.js setServername: validateString, then refuse on a
    // server-side socket — SNI travels client→server only.
    setServername(name) {
      validateString(name, "name");
      if (this._isServer) {
        const e = new Error("Cannot issue SNI from a TLS server-side socket");
        e.code = "ERR_TLS_SNI_FROM_SERVER";
        throw e;
      }
      this.servername = name;
      return this;
    }
    // node internal/tls/wrap.js setSession(session): validateBuffer, then hand
    // it to the SSL* before the handshake. Here the handshake starts when the
    // transport connects, so the session is recorded and read by start(); a call
    // after the handshake has begun is a no-op, exactly as node's SSL_set_session
    // on an already-connected SSL is.
    setSession(session) {
      validateBuffer(session, "session");
      this._sessionToResume = session;
      return this;
    }
    // node: validateInt32(size, 'size'), then SSL_set_max_send_fragment.
    // DEFERRED: the fragment size is not yet threaded to the native TlsChannel,
    // so the validated call reports failure rather than claiming success.
    setMaxSendFragment(size) { validateInt32(size, "size"); return false; }
    disableRenegotiation() { this._renegotiationDisabled = true; }
    enableTrace() {}
    // node internal/tls/wrap.js exportKeyingMaterial: arguments first, then the
    // securely-established check, then SSL_export_keying_material (RFC 5705).
    exportKeyingMaterial(length, label, context) {
      validateUint32(length, "length", true);
      validateString(label, "label");
      if (context !== undefined) validateBuffer(context, "context");
      if (!this._secureEstablished) {
        const e = new Error("TLS socket connection must be securely established");
        e.code = "ERR_TLS_INVALID_STATE";
        throw e;
      }
      const fd = this._transport ? this._transport._fd : -1;
      let b64 = null;
      if (NN && typeof NN.tlsExportKeyingMaterial === "function" && fd >= 0) {
        const ctxB64 = context === undefined ? null
          : (Buffer ? Buffer.from(context.buffer ? context.buffer : context,
                                  context.byteOffset || 0, context.byteLength).toString("base64") : null);
        try { b64 = NN.tlsExportKeyingMaterial(fd, length, label, ctxB64); } catch (e) { b64 = null; }
      }
      if (typeof b64 !== "string") {
        const e = new Error("TLS keying material export failed");
        e.code = "ERR_TLS_RENEGOTIATION_FAILED";
        throw e;
      }
      return Buffer ? Buffer.from(b64, "base64") : b64;
    }
    // node internal/tls/wrap.js renegotiate(options, callback): both arguments
    // are validated before anything is attempted, so a bare renegotiate() is an
    // ERR_INVALID_ARG_TYPE rather than a silent false.
    renegotiate(options, callback) {
      validateObject(options, "options");
      if (callback !== undefined) validateFunction(callback, "callback");
      return false;
    }
  }

  // ---- tls.connect(): open (or adopt) a socket and drive the client handshake -
  // Signatures: connect(options[,cb]) and connect(port[,host][,options][,cb]).
  function connect(...args) {
    // node lib/internal/tls/wrap.js connect() -> normalizeConnectArgs: the
    // signatures are connect(options[,cb]), connect(port[,host][,options][,cb])
    // and connect(path[,options][,cb]) — host, options and cb are each
    // independently optional, so the options object can sit at index 1 or 2.
    // Reading it only from index 2 silently dropped `rejectUnauthorized:false`
    // (and every other option) from the very common
    // `tls.connect(port, { ... }, cb)` form, which then failed the handshake
    // with a raw OpenSSL "certificate verify failed".
    let opts, cb = null;
    const a = args.slice();
    if (a.length && typeof a[a.length - 1] === "function") cb = a.pop();
    if (typeof a[0] === "object" && a[0] !== null) {
      opts = a[0];
    } else {
      let host, tail;
      if (typeof a[1] === "string") { host = a[1]; tail = a[2]; }
      else tail = a[1];
      opts = (tail !== null && typeof tail === "object") ? Object.assign({}, tail) : {};
      if (typeof a[0] === "string") opts.path = a[0];
      else opts.port = a[0];
      if (host !== undefined) opts.host = host;
    }
    // node internal/tls/wrap.js connect(): the defaults are folded in first, then
    // checkServerIdentity / minDHSize are validated, then the secure context is
    // built through the tls module's own createSecureContext export (so a caller
    // that replaced it — as test-tls-client-default-ciphers does — is observed),
    // and only then is an IP servername refused.
    // node lib/internal/tls/wrap.js connect() defaults:
    //   rejectUnauthorized: !(process.env.NODE_TLS_REJECT_UNAUTHORIZED === '0')
    // The bypass is opt-in by the operator, applies ONLY when the variable is
    // literally the string '0' (not 'false', not '', not unset), is overridden by
    // an explicit `rejectUnauthorized` in the options, and is announced by the
    // one-time ProcessWarning installed at the bottom of this file — exactly
    // node's behaviour, no wider.
    opts = Object.assign({
      rejectUnauthorized: !(G.process && G.process.env &&
                            G.process.env.NODE_TLS_REJECT_UNAUTHORIZED === "0"),
      ciphers: T.DEFAULT_CIPHERS,
      checkServerIdentity: T.checkServerIdentity,
      minDHSize: 1024,
    }, opts);
    validateFunction(opts.checkServerIdentity, "options.checkServerIdentity");
    validateNumber(opts.minDHSize, "options.minDHSize", 1);
    const secureContext = opts.secureContext || (typeof T.createSecureContext === "function"
      ? T.createSecureContext(opts) : undefined);
    // A `pfx` archive carries the certificate, its key and the chain in one
    // blob. createSecureContext has just opened it (throwing here if it could
    // not — which is what test-tls-invalid-pfx / test-tls-legacy-pfx catch), so
    // read the recovered cert/key/ca back off the context it produced. The
    // handshake options below are built from `opts`, where `pfx` is still an
    // unopened Buffer, so without this a client that authenticated with a PFX
    // presented no certificate at all.
    const creds = (opts.pfx != null && secureContext && secureContext._secureOptions)
      ? secureContext._secureOptions : opts;
    if (opts.servername && netIsIP(opts.servername)) {
      throw invalidArgValue("options.servername", opts.servername,
        "Setting the TLS ServerName to an IP address is not permitted");
    }
    const host = opts.host || opts.hostname || "localhost";
    const servername = opts.servername != null ? opts.servername
      : (typeof opts.host === "string" && !netIsIP(opts.host) ? opts.host : "");
    const transportOpt = opts.socket;
    // A CALLER-SUPPLIED checkServerIdentity replaces node's default peer-name
    // check (lib/_tls_wrap.js onConnectSecure picks
    // `options.checkServerIdentity || tls.checkServerIdentity`). mbun's default
    // check lives in OpenSSL (SSL_set1_host), where a JS function cannot replace
    // it — so when, and only when, the caller passed its own, hand the name check
    // to JS and tell the native layer to stand its copy down. The default stays
    // native and unchanged. Chain verification is unaffected in both cases.
    const customIdentity = typeof opts.checkServerIdentity === "function" &&
      opts.checkServerIdentity !== T.checkServerIdentity ? opts.checkServerIdentity : null;
    const tlsOpts = { isServer: false, servername, ca: creds.ca, cert: creds.cert, key: creds.key, rejectUnauthorized: opts.rejectUnauthorized, ALPNProtocols: opts.ALPNProtocols,
      minVersion: opts.minVersion, maxVersion: opts.maxVersion, secureProtocol: opts.secureProtocol, secureContext: opts.secureContext,
      ciphers: opts.ciphers, checkServerIdentity: customIdentity, identityHost: servername || host,
      // node configSecureContext setKey(pem, options.passphrase): a top-level
      // passphrase is the one that decrypts a plain (non-`{pem,…}`) key, so it
      // has to reach the handshake options or every encrypted-key client fell
      // over with "socket disconnected before secure TLS connection".
      // `creds`, not `opts`: a key recovered from a PFX comes back already
      // decrypted, and the archive's passphrase is NOT that key's passphrase.
      passphrase: creds.passphrase,
      // node internal/tls/wrap.js connect(): `session` is handed to
      // tlssock.setSession() before the socket connects.
      session: opts.session,
      // PORT-SOURCE: compat/node/lib/internal/tls/wrap.js:597-600 — the socket
      // options node's TLSSocket hands down to net.Socket. They also reach the
      // transport through `netOpts` below; both are needed, because the caller
      // reads them back off the TLSSocket and the transport is the thing that
      // owns the fd.
      noDelay: opts.noDelay, keepAlive: opts.keepAlive,
      keepAliveInitialDelay: opts.keepAliveInitialDelay };

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
    // The plaintext edge delegates every read/write to the transport, so the
    // caller's highWaterMark has to reach the transport too — the TLSSocket
    // aliases readableHighWaterMark/writableHighWaterMark onto it.
    const transport = new NetSocket({ allowHalfOpen: false, highWaterMark: opts.highWaterMark });
    const tlsOptsHwm = Object.assign({ highWaterMark: opts.highWaterMark }, tlsOpts);
    const tlsSock = new TLSSocket(transport, tlsOptsHwm);
    // node stores the resolved connect options on the socket (kConnectOptions);
    // onConnectEnd reads path/host/port/localAddress back off them.
    tlsSock._connectOptions = opts;
    // node's TLSSocket is itself the socket that connects, so the
    // 'net.client.socket' diagnostics channel reports the TLSSocket. mbun keeps
    // the transport Socket separate; point the channel at the TLSSocket.
    transport._dcClientSocket = tlsSock;
    if (cb) tlsSock.once("secureConnect", cb);
    // node internal/tls/wrap.js connect(): only a socket this call created gets
    // the timeout armed — a caller-supplied socket stays the caller's business.
    if (opts.timeout) tlsSock.setTimeout(opts.timeout);
    // node hands its whole options object to tlssock.connect(), so the net-level
    // connect options reach net.Socket#connect — notably `lookup`, which
    // test-tls-connect-timeout-option relies on to keep the socket from ever
    // attempting the connection. A unix-socket/pipe target has a path instead of
    // a port (node net.connect dispatches on the same distinction).
    if (typeof opts.path === "string" && opts.path) transport.connect(opts.path);
    else {
      const netOpts = { port: opts.port | 0, host: String(host) };
      // `signal` rides along for the same reason as `lookup`: node's TLSSocket
      // IS the connecting socket, so tls.connect({ signal }) aborts the pending
      // connection. Here the transport owns the connect, and its 'error'
      // reaches the TLSSocket the caller holds.
      for (const k of ["lookup", "localAddress", "localPort", "family", "hints",
                       "autoSelectFamily", "autoSelectFamilyAttemptTimeout",
                       "blockList", "noDelay", "keepAlive", "keepAliveInitialDelay", "signal"]) {
        if (opts[k] !== undefined) netOpts[k] = opts[k];
      }
      transport.connect(netOpts);
    }
    return tlsSock;
  }

  // ---- Server / createServer: net.Server that upgrades each connection to TLS -
  // server emits "secureConnection" (with the TLSSocket), the socket emits
  // "secureConnect"; a pre-handshake failure surfaces as "tlsClientError".
  class Server extends NetServer {
    constructor(options, secureConnectionListener) {
      if (typeof options === "function") { secureConnectionListener = options; options = {}; }
      // node internal/tls/wrap.js Server: anything that is neither a function nor
      // an object (nor nullish) is rejected before any option is read.
      else if (options == null) options = {};
      else if (typeof options !== "object") throw argTypeError("options", "must be of type object", options);
      // node tls.Server runs net.Server.call(this, options, …), so the net-level
      // construction options reach the parent and are published on the server.
      // pauseOnConnect is deliberately NOT forwarded: this TLSSocket rides a
      // separate transport socket whose readability drives the handshake, so
      // pausing the transport would stall the ClientHello instead of merely
      // deferring the first plaintext byte. DEFERRED until the plaintext edge
      // owns its own read queue.
      super({ allowHalfOpen: options.allowHalfOpen });
      this.pauseOnConnect = !!options.pauseOnConnect;
      // node tls.Server runs setSecureContext(options) → createSecureContext in
      // the constructor, so an unusable option (a cipher list OpenSSL matches
      // nothing to, a bad secureProtocol, …) throws from createServer() rather
      // than at the first connection.
      let builtContext;
      if (T && typeof T.createSecureContext === "function") builtContext = T.createSecureContext(options || {});
      // A `pfx` server option is a PKCS#12 archive holding the certificate, its
      // key and the chain. createSecureContext has just opened it (and threw if
      // it could not); read the recovered cert/key/ca back off the context, or
      // `_sharedCreds` below carries an unopened Buffer and every accepted
      // connection is answered with "no shared cipher" — the server would have
      // had no certificate at all.
      if (options && options.pfx != null && builtContext && builtContext._secureOptions) {
        options = builtContext._secureOptions;
      }
      // node internal/tls/wrap.js Server: the negotiated ALPN list is published on
      // the server in its length-prefixed wire form (tls.convertALPNProtocols),
      // which node:https then compares against (test-https-argument-of-creating).
      if (options && options.ALPNProtocols && T && typeof T.convertALPNProtocols === "function") {
        try { T.convertALPNProtocols(options.ALPNProtocols, this); } catch (e) {}
      }
      // node internal/tls/wrap.js Server, after setSecureContext: the
      // Server-only options (the ones createSecureContext never sees) are
      // validated here. handshakeTimeout defaults to 120s BEFORE the check, so
      // `handshakeTimeout: 0` is legal and only a non-number is rejected.
      const hsTimeoutOpt = options.handshakeTimeout || (120 * 1000);
      validateNumber(hsTimeoutOpt, "options.handshakeTimeout");
      if (options.SNICallback !== undefined && options.SNICallback !== null)
        validateFunction(options.SNICallback, "options.SNICallback");
      if (options.pskCallback !== undefined && options.pskCallback !== null)
        validateFunction(options.pskCallback, "options.pskCallback");
      if (options.ALPNCallback !== undefined && options.ALPNCallback !== null) {
        validateFunction(options.ALPNCallback, "options.ALPNCallback");
        if (options.ALPNProtocols) {
          const e = new TypeError("The ALPNCallback and ALPNProtocols TLS options are mutually exclusive");
          e.code = "ERR_TLS_ALPN_CALLBACK_WITH_PROTOCOLS";
          throw e;
        }
      }
      this._sharedCreds = options || {};
      // node tls.createServer({ ticketKeys }) — and, when the caller gave none,
      // node's own per-Server random key (crypto_context.cc SecureContext::Init).
      // It has to live on the SERVER rather than on each connection: it is the
      // only thing shared between the SSL_CTX of one accepted connection and the
      // next, and therefore the only thing that can make a ticket issued by one
      // resumable by the other. null (no CSPRNG) means OpenSSL keeps its own
      // per-context key and resumption simply does not happen.
      this._ticketKeys = (options && options.ticketKeys) || randomTicketKeys();
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
        secureContext: creds.secureContext, ciphers: creds.ciphers,
        // Server-side createSecureContext options that reach the SSL_CTX:
        // the passphrase for an encrypted `key`, and node's server-default
        // honorCipherOrder (true unless the caller explicitly said false).
        passphrase: creds.passphrase, honorCipherOrder: creds.honorCipherOrder,
        dhparam: creds.dhparam, ecdhCurve: creds.ecdhCurve,
        // Read off the Server at ACCEPT time, not at construction: node's
        // server.setTicketKeys() rotates the key for connections accepted after
        // the call and the corpus (test-tls-ticket) turns exactly that into an
        // assertion about which tickets stay resumable.
        ticketKeys: this._ticketKeys,
      });
      const self = this;
      // node internal/tls/wrap.js: the server re-emits each connection's keylog
      // lines as (line, tlsSocket), and only bothers when it has a listener.
      if (typeof self.listenerCount === "function" && self.listenerCount("keylog") > 0)
        tlsSock.on("keylog", (line) => self.emit("keylog", line, tlsSock));
      // `pauseOnConnect` stays DEFERRED (see the constructor). Handing the
      // listener an already-paused TLSSocket is easy — pausing it here, after
      // the handshake, does not stall the ClientHello the way forwarding the
      // option to the net.Server would. What is NOT in place is libuv's
      // liveness rule for a paused socket: node's pause() calls readStop(), so
      // a paused connection stops holding the loop, while here it keeps being
      // polled and pins the process. Measured: pausing here turned
      // test-tls-server-parent-constructor-options from a failed assertion into
      // a 15s hang, which is strictly worse. The gate belongs in _syncEofHold.
      tlsSock.once("secureConnect", () => self.emit("secureConnection", tlsSock));
      // node _tls_wrap.js onServerSocketSecure/handshakeTimeout: a connection that
      // does not finish its handshake within options.handshakeTimeout (default
      // 120s) is destroyed and reported as 'tlsClientError'. Without it a peer
      // that opens a TCP connection to a TLS port and then says nothing pins the
      // server forever — a hang rather than the error node reports.
      const hsTimeout = creds.handshakeTimeout === undefined ? 120000 : creds.handshakeTimeout;
      if (hsTimeout > 0) {
        const timer = G.setTimeout(() => {
          if (tlsSock._secureEstablished || tlsSock.destroyed || tlsSock._errorEmitted) return;
          tlsSock._errorEmitted = true;
          const e = new Error("TLS handshake timeout");
          e.code = "ERR_TLS_HANDSHAKE_TIMEOUT";
          self.emit("tlsClientError", e, tlsSock);
          try { tlsSock.destroy(); } catch (e2) {}
        }, hsTimeout);
        const clear = () => { try { G.clearTimeout(timer); } catch (e) {} };
        tlsSock.once("secureConnect", clear);
        tlsSock.once("close", clear);
      }
      // node _tls_wrap.js: a failure BEFORE the handshake completes is the
      // server's 'tlsClientError' (with the socket), never an unhandled 'error'
      // on the TLSSocket — a client that speaks junk at a TLS port must not take
      // the server process down. The listener also has to live on the TLSSocket
      // rather than on the raw transport, because the transport forwards its
      // errors there and an unlistened 'error' re-emit is what threw.
      // node internal/tls/wrap.js onSocketClose: a peer that goes away before the
      // handshake completes is reported to the server as `tlsClientError` with
      // ConnResetException('socket hang up') — test-tls-econnreset matches that
      // message. Only one of this and the 'error' path may fire.
      raw.on("close", () => {
        if (tlsSock._secureEstablished || tlsSock._errorEmitted) return;
        tlsSock._errorEmitted = true;
        const e = new Error("socket hang up");
        e.code = "ECONNRESET";
        self.emit("tlsClientError", e, tlsSock);
      });
      tlsSock.on("error", (e) => {
        if (!tlsSock._secureEstablished) {
          if (tlsSock._errorEmitted) return;
          tlsSock._errorEmitted = true;
          // node reports a peer that vanished mid-handshake through
          // onSocketClose, i.e. as ConnResetException('socket hang up') — never
          // as the raw transport's "read ECONNRESET" (test-tls-econnreset
          // matches the message). Normalise so both arrival orders agree.
          if (e && e.code === "ECONNRESET") e.message = "socket hang up";
          self.emit("tlsClientError", e, tlsSock);
          try { tlsSock.destroy(); } catch (e2) {}
          return;
        }
        // Established connection: node routes to the socket's own listeners and
        // treats none as an unhandled error. Preserve that.
        if (tlsSock.listenerCount("error") <= 1) G.queueMicrotask(() => { throw e; });
      });
    }
    // TLS dispatches its accepted plaintext edge through `secureConnection`,
    // not net.Server's `connection`. Preserve the same capture-rejections
    // contract: an async listener rejection belongs to that TLS socket.
    [Symbol.for("nodejs.rejection")](err, event, sock) {
      if (event === "secureConnection" && sock && typeof sock.destroy === "function") {
        sock.destroy(err);
        return;
      }
      return super[Symbol.for("nodejs.rejection")](err, event, sock);
    }
    setSecureContext(options) { this._sharedCreds = options || {}; }
    // node internal/tls/wrap.js addContext: an empty servername is refused (there
    // would be nothing to match), and a plain options object is turned into a
    // SecureContext first.
    addContext(servername, context) {
      if (!servername) {
        const e = new Error('"Server name" is required for TLS server SNI context');
        e.code = "ERR_TLS_REQUIRED_SERVER_NAME";
        throw e;
      }
      this._contexts.set(servername, context);
    }
    // node internal/tls/wrap.js getTicketKeys → SecureContext::GetTicketKeys: a
    // COPY of the 48 bytes in force, so a caller mutating the returned buffer
    // cannot change what the server encrypts tickets with.
    getTicketKeys() {
      const k = this._ticketKeys;
      if (!k) return Buffer ? Buffer.alloc(48) : new Uint8Array(48);
      return Buffer ? Buffer.from(k) : k;
    }
    // node internal/tls/wrap.js setTicketKeys: validateBuffer, then assert the
    // 48-byte length (16B name + 16B HMAC key + 16B AES key).
    setTicketKeys(keys) {
      validateBuffer(keys);
      if (keys.byteLength !== 48) {
        const e = new Error("Session ticket keys must be a 48-byte buffer");
        e.code = "ERR_ASSERTION";
        e.name = "AssertionError";
        throw e;
      }
      this._ticketKeys = Buffer ? Buffer.from(keys) : keys;
      return this;
    }
  }
  function createServer(options, connectionListener) { return new Server(options, connectionListener); }

  // ---- pre-class constructor call form ---------------------------------------
  // node's tls.Server / tls.TLSSocket predate ES classes, so `tls.Server(opts,
  // cb)` and `tls.TLSSocket(sock)` without `new` are legal and the corpus uses
  // them (test-tls-connect-simple, test-tls-pause, test-tls-passphrase, …).
  // An ES class rejects that with "Cannot call a class constructor without
  // |new|", so re-export each through a plain-function stand-in that keeps
  // construct behaviour, prototype identity, statics and instanceof intact —
  // the same shape js_net.cppm uses for net.Server/net.Socket.
  const callable = (Cls) => {
    const wrapper = function (...args) {
      if (new.target !== undefined) return Reflect.construct(Cls, args, new.target);
      // `Ctor.call(this, ...)` (util.inherits subclassing): the receiver's chain
      // already reaches the class — initialise it in place.
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
  const TLSSocketW = callable(TLSSocket);

  // ---- overwrite the deferred stubs installed by builtins/node_tls.cppm -------
  try { T.TLSSocket = TLSSocketW; } catch (e) {}
  try { T.Server = ServerW; } catch (e) {}
  try { T.createServer = createServer; } catch (e) {}
  try { T.connect = connect; } catch (e) {}

  // ---- NODE_TLS_REJECT_UNAUTHORIZED=0 warning ---------------------------------
  // node lib/internal/process/pre_execution.js initializeReport/…: when the
  // variable is set to '0', node emits this warning once at startup, before any
  // connection is made, so the operator is told that verification is off. The
  // wording is node's verbatim.
  try {
    if (G.process && G.process.env && G.process.env.NODE_TLS_REJECT_UNAUTHORIZED === "0" &&
        typeof G.process.emitWarning === "function" && !G.__mbunTlsRejectUnauthorizedWarned) {
      G.__mbunTlsRejectUnauthorizedWarned = true;
      G.process.emitWarning(
        "Setting the NODE_TLS_REJECT_UNAUTHORIZED environment variable to " +
        "'0' makes TLS connections and HTTPS requests insecure by disabling " +
        "certificate verification.");
    }
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::js_tls_live
