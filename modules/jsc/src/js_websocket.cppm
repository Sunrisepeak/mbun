// modules/jsc/src/js_websocket.cppm — module mbun.jsc.js_websocket
//
// RFC 6455 WebSocket layer (T-LOOP.4), pure JS over the net reactor:
//   - shared frame codec (parse/serialize, masking, fragmentation, control
//     frames with auto-pong) — blueprint bun packages/bun-usockets uWS
//     WebSocketProtocol.h semantics;
//   - G.__mbunWS.serverUpgrade: Bun.serve upgrade handshake (Sec-WebSocket-
//     Accept = b64(sha1(key+GUID))), ServerWebSocket API (send/sendText/
//     sendBinary/ping/pong/close/terminate/cork/subscribe/publish, per-server
//     topic registry) — blueprint bun src/api/server (ServerWebSocket) +
//     ServerConfig.websocket handlers {open,message,close,drain,ping,pong};
//   - G.WebSocket client (WHATWG surface: readyState/binaryType/on* +
//     addEventListener, masked client frames) over node:net's reactor Socket.
// Evaluated AFTER kNetJS (needs __mbunNativeModules.net + __mbunNet).
// DEFERRED honestly: wss:// (TLS), permessage-deflate (compress flags are
// accepted and ignored), backpressure-driven drain (sends buffer fully, so
// drain never fires and send never returns -1).
export module mbun.jsc.js_websocket;

import std;

namespace mbun::jsc::js_websocket {

export constexpr std::string_view kWebSocketJS = R"JS(
(function () {
  "use strict";
  const G = globalThis;
  const M = G.__mbunNativeModules || {};
  const te = new G.TextEncoder(), td = new G.TextDecoder();
  const WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

  const u8 = (d) => (d == null ? new Uint8Array(0)
    : typeof d === "string" ? te.encode(d)
    : d instanceof Uint8Array ? d
    : ArrayBuffer.isView(d) ? new Uint8Array(d.buffer, d.byteOffset, d.byteLength)
    : d instanceof ArrayBuffer ? new Uint8Array(d)
    : d._u8 instanceof Uint8Array ? d._u8 : te.encode(String(d)));
  // "Is this an ArrayBuffer or a view onto one" — the test bun's send() uses to
  // pick a Binary frame over a ToString'd Text frame (ServerWebSocket.rs: send →
  // JSValue::as_array_buffer). Deliberately excludes Blob.
  const isBufferSource = (d) => ArrayBuffer.isView(d) || d instanceof ArrayBuffer;
  const concat = (a, b) => { const out = new Uint8Array(a.length + b.length); out.set(a, 0); out.set(b, a.length); return out; };
  const fire = (fn, ...a) => { if (typeof fn === "function") { try { return fn(...a); } catch (e) { try { G.console && G.console.error("error: " + ((e && e.message) || e)); } catch (e2) {} } } };

  // Sec-WebSocket-Accept (RFC 6455 §4.2.2).
  // Sec-WebSocket-Protocol header helpers, shared by the request builder and the
  // RFC 6455 response check. `wsProtoSplit` reproduces bun's HeaderValueIterator:
  // tokenize on ',', trim " \t" off each token, drop the empties.
  // ref: compat/bun/src/http/HeaderValueIterator.rs
  const wsProtoSplit = (v) => String(v).split(",").map((t) => t.replace(/^[ \t]+|[ \t]+$/g, "")).filter((t) => t.length);
  const wsProtoValues = (head) => {
    const out = [];
    for (const line of String(head).split("\r\n")) {
      const c = line.indexOf(":");
      if (c > 0 && line.slice(0, c).trim().toLowerCase() === "sec-websocket-protocol") out.push(line.slice(c + 1));
    }
    return out;
  };
  const wsProtoTokens = (head) => {
    const out = [];
    for (const v of wsProtoValues(head)) for (const t of wsProtoSplit(v)) if (out.indexOf(t) === -1) out.push(t);
    return out;
  };

  const acceptFor = (key) => {
    try {
      const C = M["node:crypto"] || M["crypto"];
      if (C && typeof C.createHash === "function") return C.createHash("sha1").update(String(key) + WS_GUID).digest("base64");
    } catch (e) {}
    try {
      if (G.Bun && G.Bun.CryptoHasher) { const h = new G.Bun.CryptoHasher("sha1"); h.update(String(key) + WS_GUID); return h.digest("base64"); }
    } catch (e) {}
    return "";
  };

  // ---- frame codec -----------------------------------------------------------
  // Serialize one frame. Client frames are masked (RFC: client→server MUST).
  const wsFrame = (op, payload, mask) => {
    const n = payload.length;
    let head;
    if (n < 126) head = new Uint8Array([0x80 | op, n]);
    else if (n < 65536) head = new Uint8Array([0x80 | op, 126, (n >> 8) & 255, n & 255]);
    else {
      head = new Uint8Array(10);
      head[0] = 0x80 | op; head[1] = 127;
      let v = n; for (let i = 9; i >= 2; i--) { head[i] = v & 255; v = Math.floor(v / 256); }
    }
    if (!mask) return concat(head, payload);
    head[1] |= 0x80;
    const key = new Uint8Array(4);
    for (let i = 0; i < 4; i++) key[i] = (Math.random() * 256) | 0;
    const body = new Uint8Array(n);
    for (let i = 0; i < n; i++) body[i] = payload[i] ^ key[i & 3];
    return concat(concat(head, key), body);
  };

  // Incremental parser: feed(bytes) → onFrame(fin, op, payload) per frame.
  const mkParser = (onFrame, onError) => {
    let buf = new Uint8Array(0);
    return { feed(bytes) {
      buf = buf.length ? concat(buf, bytes) : u8(bytes);
      for (;;) {
        if (buf.length < 2) return;
        const b0 = buf[0], b1 = buf[1];
        const fin = (b0 & 0x80) !== 0, op = b0 & 0x0f, masked = (b1 & 0x80) !== 0;
        let len = b1 & 0x7f, off = 2;
        // RFC 6455 5.5: a control frame (opcode >= 8) carries at most a 125
        // byte payload — so it never uses the 126/127 extended-length forms —
        // and MUST NOT be fragmented. Either violation fails the connection.
        if ((op & 0x8) !== 0 && (len > 125 || !fin)) {
          buf = new Uint8Array(0);
          if (onError) onError(new Error("Received invalid control frame"));
          return;
        }
        if (len === 126) { if (buf.length < 4) return; len = (buf[2] << 8) | buf[3]; off = 4; }
        else if (len === 127) {
          if (buf.length < 10) return;
          len = 0; for (let i = 2; i < 10; i++) len = len * 256 + buf[i];
          if (len > 0x7fffffff) { if (onError) onError(new Error("frame too large")); return; }
          off = 10;
        }
        let key = null;
        if (masked) { if (buf.length < off + 4) return; key = buf.subarray(off, off + 4); off += 4; }
        if (buf.length < off + len) return;
        let payload;
        if (masked) { payload = new Uint8Array(len); const src = buf.subarray(off, off + len); for (let i = 0; i < len; i++) payload[i] = src[i] ^ key[i & 3]; }
        else payload = buf.slice(off, off + len);
        buf = buf.slice(off + len);
        onFrame(fin, op, payload);
      }
    } };
  };

  // Message assembly over fragmentation + control dispatch. `sink` supplies
  // sendRaw(bytes), onText(str), onBinary(u8), onPing/onPong(u8),
  // onClose(code, reason) — the codec auto-pongs pings (bun/uWS behavior).
  const mkWire = (sink, mask, maxPayload) => {
    let msgOp = 0, parts = [], msgLen = 0; if (maxPayload == null) maxPayload = Infinity;
    const parser = mkParser((fin, op, payload) => {
      if (op === 8) {  // close
        let code = 1005, reason = "";
        if (payload.length >= 2) { code = (payload[0] << 8) | payload[1]; reason = td.decode(payload.subarray(2)); }
        sink.onClose(code, reason);
        return;
      }
      if (op === 9) { sink.sendRaw(wsFrame(10, payload, mask)); if (sink.onPing) sink.onPing(payload); return; }
      if (op === 10) { if (sink.onPong) sink.onPong(payload); return; }
      if (op === 1 || op === 2) { msgOp = op; parts = [payload]; msgLen = payload.length; }
      else if (op === 0) { parts.push(payload); msgLen += payload.length; }
      else return;  // unknown opcode: drop
      // bun uWS refusePayloadLength: a frame/accumulated message over the cap is
      // refused with 1009. ref: WebSocketContext.h:143-146, WebSocketProtocol.h:298.
      if (msgLen > maxPayload) { sink.onClose(1009, "Received too big message"); return; }
      if (!fin) return;
      let all = parts.length === 1 ? parts[0] : parts.reduce(concat);
      const kind = msgOp; msgOp = 0; parts = [];
      if (kind === 1) sink.onText(td.decode(all));
      else sink.onBinary(all);
    }, sink.onError);
    return {
      feed: parser.feed,
      sendText: (s) => { const b = te.encode(String(s)); sink.sendRaw(wsFrame(1, b, mask)); return b.length; },
      sendBinary: (d) => { const b = u8(d); sink.sendRaw(wsFrame(2, b, mask)); return b.length; },
      sendPing: (d) => { sink.sendRaw(wsFrame(9, u8(d), mask)); return 0; },
      sendPong: (d) => { sink.sendRaw(wsFrame(10, u8(d), mask)); return 0; },
      sendClose: (code, reason) => {
        const r = te.encode(reason == null ? "" : String(reason));
        const b = new Uint8Array(2 + r.length);
        const c = code == null ? 1000 : (+code | 0);
        b[0] = (c >> 8) & 255; b[1] = c & 255; b.set(r, 2);
        sink.sendRaw(wsFrame(8, b, mask));
      },
    };
  };

  // ---- server side: G.__mbunWS ------------------------------------------------
  const WSNS = (G.__mbunWS = {});
  const SERVERS = new WeakMap();  // serverObj → { topics: Map(topic → Set(ws)) }
  const topicsOf = (server) => { let s = SERVERS.get(server); if (!s) { s = { topics: new Map() }; SERVERS.set(server, s); } return s.topics; };
  const publishTo = (server, topic, data, exclude) => {
    const set = topicsOf(server).get(String(topic));
    if (!set || set.size === 0) return 0;
    let sent = 0;
    for (const ws of Array.from(set)) {
      if (ws === exclude || ws.readyState !== 1) continue;
      sent = ws.send(data) || sent;
    }
    return sent;
  };

  WSNS.publish = (server, topic, data) => publishTo(server, topic, data, null);
  WSNS.subscriberCount = (server, topic) => { const s = topicsOf(server).get(String(topic)); return s ? s.size : 0; };

  // ctx: { key, protocol, headers, setCookies, data, server, handlers,
  //        write(bytes), detach()→bool, abort(), onCleanup() }
  WSNS.serverUpgrade = function (ctx) {
    const accept = acceptFor(ctx.key);
    if (!accept) return null;
    let head = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n";
    let proto = "";
    if (ctx.protocol) { proto = String(ctx.protocol).split(",")[0].trim(); if (proto) head += "Sec-WebSocket-Protocol: " + proto + "\r\n"; }
    // permessage-deflate: negotiated only when the server enabled it and the
    // client advertised it. Frames stay uncompressed (RSV1=0), so no inflate
    // is required — the extension is spec-valid without per-message compression.
    if (ctx.perMessageDeflate && /permessage-deflate/i.test(String(ctx.extensions || ""))) {
      head += "Sec-WebSocket-Extensions: permessage-deflate\r\n";
    }
    const hs = ctx.headers;
    if (hs) {
      if (typeof hs.forEach === "function") hs.forEach((v, k) => { const lk = String(k).toLowerCase(); if (lk !== "upgrade" && lk !== "connection" && lk !== "sec-websocket-accept") head += k + ": " + v + "\r\n"; });
      else for (const k of Object.keys(hs)) head += k + ": " + hs[k] + "\r\n";
    }
    // Cookies the handler set via req.cookies.set() before upgrading. ref: bun
    // server_body.rs — after write_status("101 Switching Protocols") the custom
    // FetchHeaders and CookieMap__write() both land on the same response.
    if (ctx.setCookies) for (const sc of ctx.setCookies) head += "Set-Cookie: " + sc + "\r\n";
    const H = ctx.handlers || {};
    const state = { done: false, closeSent: false };
    const ws = {
      data: ctx.data,
      readyState: 1,
      // Peer of the upgraded socket, captured natively at accept. The upgrader
      // (js_net serverUpgrade call) passes it through; "" only when the socket
      // reported no peer, matching bun's empty-string fallback rather than a
      // fabricated loopback IP. ref: bun src/runtime/server/ServerWebSocket.rs.
      remoteAddress: ctx.remoteAddress || "",
      binaryType: "nodebuffer",
      subscriptions: new Set(),
      // PORT-SOURCE: bun src/runtime/server/ServerWebSocket.rs:1048-1094 (fn send) —
      // only an ArrayBuffer/view goes out as a Binary frame; every other value is
      // coerced through ToString and sent as Text. A Blob is NOT an ArrayBuffer, so
      // bun sends "[object Blob]" (verified against 1.3.14) — do not special-case it.
      send(msg, compress) { if (this.readyState !== 1) return 0; return isBufferSource(msg) ? wire.sendBinary(msg) : wire.sendText(String(msg)); },
      sendText(msg) { return this.readyState === 1 ? wire.sendText(msg) : 0; },
      sendBinary(msg) { return this.readyState === 1 ? wire.sendBinary(msg) : 0; },
      ping(d) { return this.readyState === 1 ? (wire.sendPing(d == null ? "" : d), 1) : 0; },
      pong(d) { return this.readyState === 1 ? (wire.sendPong(d == null ? "" : d), 1) : 0; },
      cork(fn) { return typeof fn === "function" ? fn(this) : undefined; },
      getBufferedAmount() { return 0; },
      subscribe(topic) { topic = String(topic); this.subscriptions.add(topic); let s = topicsOf(ctx.server).get(topic); if (!s) { s = new Set(); topicsOf(ctx.server).set(topic, s); } s.add(this); return true; },
      unsubscribe(topic) { topic = String(topic); this.subscriptions.delete(topic); const s = topicsOf(ctx.server).get(topic); if (s) { s.delete(this); if (!s.size) topicsOf(ctx.server).delete(topic); } return true; },
      isSubscribed(topic) { return this.subscriptions.has(String(topic)); },
      publish(topic, data, compress) { return publishTo(ctx.server, topic, data, this); },
      publishText(topic, data) { return publishTo(ctx.server, topic, String(data), this); },
      publishBinary(topic, data) { return publishTo(ctx.server, topic, u8(data), this); },
      close(code, reason) {
        if (this.readyState !== 1) return;
        this.readyState = 2; state.closeSent = true;
        try { wire.sendClose(code, reason); } catch (e) {}
        // Peer echo (or transport close) finishes teardown; a real-time
        // unref'd fallback reaps a peer that never answers.
        const t = G.setTimeout(() => teardown(code == null ? 1000 : +code | 0, reason == null ? "" : String(reason)), 300);
        if (t && typeof t.unref === "function") t.unref();
      },
      terminate() { teardown(1006, ""); },
    };
    const teardown = (code, reason) => {
      if (state.done) return;
      state.done = true;
      ws.readyState = 3;
      for (const topic of Array.from(ws.subscriptions)) ws.unsubscribe(topic);
      try { ctx.abort(); } catch (e) {}
      if (ctx.onCleanup) { try { ctx.onCleanup(); } catch (e) {} }
      fire(H.close, ws, code, reason);
    };
    const wire = mkWire({
      sendRaw: (bytes) => ctx.write(bytes),
      onText: (s) => fire(H.message, ws, s),
      onBinary: (b) => fire(H.message, ws, G.Buffer ? G.Buffer.from(b) : b),
      onPing: (b) => fire(H.ping, ws, G.Buffer ? G.Buffer.from(b) : b),
      onPong: (b) => fire(H.pong, ws, G.Buffer ? G.Buffer.from(b) : b),
      onClose: (code, reason) => {
        if (!state.closeSent) { state.closeSent = true; try { wire.sendClose(code === 1005 ? 1000 : code, reason); } catch (e) {} }
        teardown(code, reason);
      },
      onError: () => teardown(1002, "protocol error"),
      // bun default max_payload_length = 16MB (WebSocketServerContext.rs:268,354).
    }, false, (ctx.handlers && "maxPayloadLength" in ctx.handlers)
                ? Math.max(0, ctx.handlers.maxPayloadLength | 0) : (16 * 1024 * 1024));
    ctx.write(head + "\r\n");
    if (!ctx.detach()) return null;
    if (proto) ws.protocol = proto;
    ws._feed = (bytes) => { try { wire.feed(bytes); } catch (e) {} };
    ws._closed = () => teardown(1006, "");
    fire(H.open, ws);
    return ws;
  };

  // ---- client side: G.WebSocket ------------------------------------------------
  const RS = { CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3 };
  class WebSocket {
    constructor(url, protocols) {
      this.url = String(url);
      this.readyState = RS.CONNECTING;
      // bun defaults to "nodebuffer" (verified: `new WebSocket(u).binaryType`),
      // not the browser's "blob" — binary frames arrive as a Buffer.
      //
      // An ACCESSOR, not a data property: the attribute is an enumeration, and
      // bun rejects a value outside it instead of storing it. As a plain field
      // `ws.binaryType = "invalid"` was accepted silently and then matched none
      // of the read sites, so a binary frame afterwards was delivered as the raw
      // view with no diagnostic at all.
      {
        let binaryType = "nodebuffer";
        Object.defineProperty(this, "binaryType", {
          enumerable: true, configurable: true,
          get() { return binaryType; },
          set(v) {
            const s = String(v);
            if (s !== "nodebuffer" && s !== "arraybuffer" && s !== "blob")
              throw new SyntaxError("binaryType must be either \"blob\", \"arraybuffer\" or \"nodebuffer\"");
            binaryType = s;
          },
        });
      }
      this.bufferedAmount = 0;
      this.protocol = ""; this.extensions = "";
      this.onopen = null; this.onmessage = null; this.onerror = null; this.onclose = null;
      this._ls = Object.create(null);
      this._state = { head: "", inHead: true, closeSent: false, done: false, pending: false };
      // The authority may carry userinfo (`ws://user:pass@host:port/`). Without
      // the `(?:([^/?#@]*)@)?` group the host class stopped at the first ":",
      // so `host` became the username and the connect failed with ENOTFOUND.
      // WHATWG: userinfo is percent-decoded and re-sent as HTTP Basic auth.
      // ref: regression 24388.
      const m = /^(wss?|https?):\/\/(?:([^/?#@]*)@)?(\[[^\]]+\]|[^/:?#]+)(?::(\d+))?(.*)$/.exec(this.url);
      if (!m) throw new SyntaxError("Invalid WebSocket URL: " + this.url);
      const secure = m[1] === "wss" || m[1] === "https";
      const host = m[3], port = m[4] ? +m[4] : (secure ? 443 : 80);
      let target = m[5] || "/"; if (target[0] !== "/") target = "/" + target;
      let basicAuth = null;
      if (m[2] !== undefined && m[2] !== "") {
        const at = m[2].indexOf(":");
        const dec = (s) => { try { return decodeURIComponent(s); } catch (e) { return s; } };
        const userpass = at === -1 ? dec(m[2]) : dec(m[2].slice(0, at)) + ":" + dec(m[2].slice(at + 1));
        basicAuth = "Basic " + (G.Buffer ? G.Buffer.from(userpass, "utf8").toString("base64") : G.btoa(userpass));
      }
      if (typeof protocols === "string") protocols = [protocols];
      let wsOpts = null;
      if (protocols && !Array.isArray(protocols) && typeof protocols === "object") {  // bun: options object
        wsOpts = protocols;
        protocols = wsOpts.protocols !== undefined ? (typeof wsOpts.protocols === "string" ? [wsOpts.protocols] : wsOpts.protocols) : (typeof wsOpts.protocol === "string" ? [wsOpts.protocol] : undefined);
      }
      this._protocols = Array.isArray(protocols) ? protocols.map(String) : [];
      // WHATWG WebSocket: each subprotocol must be a valid HTTP token (1*tchar);
      // separators/controls or any codepoint > U+00FF fail the validator, and
      // duplicates are rejected — throw SyntaxError before the wire layer runs.
      { const seen = new Set();
        for (const p of this._protocols) {
          if (!/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/.test(p) || seen.has(p))
            throw new G.DOMException("The subprotocol '" + p + "' is invalid.", "SyntaxError");
          seen.add(p); } }
      const netMod = M["node:net"] || M["net"];
      if (!netMod || !netMod.Socket) { G.queueMicrotask(() => this._fail(new Error("net layer unavailable"))); return; }
      const keyBytes = new Uint8Array(16);
      for (let i = 0; i < 16; i++) keyBytes[i] = (Math.random() * 256) | 0;
      let ks = ""; for (let i = 0; i < 16; i++) ks += String.fromCharCode(keyBytes[i]);
      const key = G.btoa(ks);
      // RFC 6455 §4.1 step 5: the client MUST fail the connection unless the
      // response's Sec-WebSocket-Accept is base64(sha1(key + GUID)). Computed
      // here, next to the key, and checked in _ingest.
      // PORT-SOURCE: compat/bun/src/http_jsc/websocket_client/WebSocketUpgradeClient.rs
      //              (expected_accept / compute_accept_value, check at :1538)
      this._state.expectedAccept = acceptFor(key);
      this._wire = mkWire({
        sendRaw: (bytes) => { try { this._sock.write(bytes); } catch (e) {} },
        onText: (s) => this._emit("message", { data: s }),
        onBinary: (b) => {
          let data;
          if (this.binaryType === "arraybuffer") data = b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
          else if (this.binaryType === "nodebuffer") data = G.Buffer ? G.Buffer.from(b) : b;
          else data = new G.Blob([b]);
          this._emit("message", { data });
        },
        onPing: (b) => this._emit("ping", { data: this._ctl(b) }),
        onPong: (b) => this._emit("pong", { data: this._ctl(b) }),
        onClose: (code, reason) => {
          if (!this._state.closeSent) { this._state.closeSent = true; try { this._wire.sendClose(code === 1005 ? 1000 : code, reason); } catch (e) {} }
          this._finish(code === 1005 ? 1000 : code, reason, true);
        },
        onError: () => this._fail(new Error("WebSocket protocol error")),
      }, true);  // client frames are masked
      const NET = G.__mbunNet;
      if (NET) { NET.pending++; this._state.pending = true; }
      const sock = (this._sock = new netMod.Socket({ allowHalfOpen: false }));
      // `failing` = failConnectingWebSocket() already scheduled the spec's
      // error+close pair; the socket teardown it causes must not pre-empt it
      // with a bare close event.
      sock.on("error", (e) => { if (!this._state.failing) this._fail(e); });
      sock.on("close", () => { if (!this._state.done && !this._state.failing) this._finish(1006, "", false); });
      sock.on("data", (chunk) => this._ingest(u8(chunk)));
      const sendHandshake = () => {
        let req = "GET " + target + " HTTP/1.1\r\nHost: " + host + (port === (secure ? 443 : 80) ? "" : ":" + port) +
          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
          "\r\nSec-WebSocket-Version: 13\r\n";
        // perMessageDeflate is offered by default; opt out only when the option is
        // present and falsy. ref: bun JSWebSocket.cpp:280-283 + WebSocketUpgradeClient.rs:2066.
        if (!(wsOpts && "perMessageDeflate" in wsOpts && !wsOpts.perMessageDeflate))
          req += "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n";
        if (this._protocols.length) req += "Sec-WebSocket-Protocol: " + this._protocols.join(", ") + "\r\n";
        // Collect the caller's headers first so an explicit Authorization wins
        // over the one derived from the URL's userinfo (24388 asserts exactly
        // that precedence), and so the two never both reach the wire.
        let userHeaders = "", sawAuth = false;
        if (wsOpts && wsOpts.headers) {
          const hs = wsOpts.headers;
          const add = (v, k) => { if (String(k).toLowerCase() === "authorization") sawAuth = true; userHeaders += k + ": " + v + "\r\n"; };
          if (typeof hs.forEach === "function") hs.forEach(add);
          else for (const k of Object.keys(hs)) add(hs[k], k);
        }
        if (basicAuth && !sawAuth) req += "Authorization: " + basicAuth + "\r\n";
        req += userHeaders;
        // The set of protocols the response is allowed to name is seeded from
        // the Sec-WebSocket-Protocol header we actually PUT ON THE WIRE -- which
        // may come from the `protocols` argument OR from options.headers.
        // ref: WebSocketUpgradeClient.rs:330-337 (`protocol_for_subprotocols`).
        this._state.offered = wsProtoTokens(req);
        sock.write(req + "\r\n");
      };
      if (secure) {
        // wss: TLS channel over the same reactor socket (T-TLS.3); the HTTP
        // upgrade waits for secureConnect. Verification honors options.tls.
        const tlsO = (wsOpts && wsOpts.tls) || {};
        const verify = tlsO.rejectUnauthorized !== false &&
          !(G.process && G.process.env && G.process.env.NODE_TLS_REJECT_UNAUTHORIZED === "0");
        const ca = tlsO.ca ? (typeof tlsO.ca === "string" ? tlsO.ca : (Array.isArray(tlsO.ca) ? tlsO.ca.join("\n") : "")) : "";
        sock.once("connect", () => sock._startTls({ servername: host, verify, ca }));
        sock.once("secureConnect", sendHandshake);
      } else {
        sock.once("connect", sendHandshake);
      }
      sock.connect(port, host === "localhost" ? "127.0.0.1" : host.replace(/^\[|\]$/g, ""));
    }
    _releasePending() { if (this._state.pending && G.__mbunNet) { G.__mbunNet.pending--; this._state.pending = false; } }
    _ingest(bytes) {
      if (!this._state.inHead) { this._wire.feed(bytes); return; }
      // handshake response head, byte-accumulated until CRLFCRLF
      let s = this._state.head;
      for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
      const at = s.indexOf("\r\n\r\n");
      if (at === -1) {
        // bun caps the buffered upgrade response at max_http_header_size
        // (16 KiB) and fails with WebSocketErrorCode::invalid_response
        // (WebSocket.cpp:1688) once an unterminated header exceeds it. Only
        // bytes that are provably still header are counted — a pipelined frame
        // arrives after the CRLFCRLF handled above.
        if (s.length > 16384) { this._state.head = ""; this._fail(new Error("Invalid response")); return; }
        this._state.head = s; return;
      }
      const head = s.slice(0, at);
      if (head.indexOf(" 101") === -1) { this._fail(new Error("Unexpected server response: " + (head.split("\r\n")[0] || ""))); return; }
      // Sec-WebSocket-Accept validation. bun closes 1002 with these exact
      // reasons and treats both as connection failures (error + close).
      // ref: WebSocket.cpp:1704/1720 missing_/mismatch_websocket_accept_header.
      if (this._state.expectedAccept) {
        const am = /\r\nsec-websocket-accept:\s*([^\r\n]+)/i.exec(head);
        if (!am) { this._fail(new Error("Missing websocket accept header"), 1002); return; }
        if (am[1].trim() !== this._state.expectedAccept) { this._fail(new Error("Mismatch websocket accept header"), 1002); return; }
      }
      // RFC 6455 client-side subprotocol validation. The response may carry AT
      // MOST ONE Sec-WebSocket-Protocol header naming EXACTLY ONE protocol, and
      // that protocol must be one the client offered; anything else fails the
      // connection. Both failures are CLEAN 1002 closes with no 'error' event.
      // PORT-SOURCE: compat/bun/src/http_jsc/websocket_client/WebSocketUpgradeClient.rs
      //   :1340-1381 (per-header check) and :1518-1523 (missing-header check);
      //   reasons from compat/bun/src/jsc/bindings/webcore/WebSocket.cpp:1724-1730.
      {
        const protoVals = wsProtoValues(head);
        const offered = this._state.offered || [];
        if (protoVals.length) {
          const toks = protoVals.length === 1 ? wsProtoSplit(protoVals[0]) : [];
          if (protoVals.length !== 1 || toks.length !== 1 || offered.indexOf(toks[0]) === -1) {
            this._state.inHead = false; this._state.head = "";
            this._finish(1002, "Mismatch client protocol", true); return;
          }
          this.protocol = toks[0];
        } else if (offered.length) {
          this._state.inHead = false; this._state.head = "";
          this._finish(1002, "Missing client protocol", true); return;
        }
      }
      const em = /\r\nsec-websocket-extensions:\s*([^\r\n]+)/i.exec(head);
      if (em) this.extensions = em[1].trim();
      this._state.inHead = false; this._state.head = "";
      this.readyState = RS.OPEN;
      this._emit("open", {});
      const rest = s.slice(at + 4);
      if (rest.length) { const b = new Uint8Array(rest.length); for (let i = 0; i < rest.length; i++) b[i] = rest.charCodeAt(i) & 0xff; this._wire.feed(b); }
    }
    _fail(err, code) {
      if (this._state.done) return;
      this._emit("error", { error: err, message: String((err && err.message) || err) });
      this._finish(code || 1006, String((err && err.message) || err), false);
    }
    _finish(code, reason, wasClean) {
      if (this._state.done) return;
      this._state.done = true;
      this.readyState = RS.CLOSED;
      this._releasePending();
      try { if (this._sock) this._sock.destroy(); } catch (e) {}
      this._emit("close", { code: code, reason: reason || "", wasClean: !!wasClean });
    }
    send(data) {
      if (this.readyState === RS.CONNECTING) throw new Error("WebSocket is not open: readyState 0 (CONNECTING)");
      if (this.readyState !== RS.OPEN) return;
      // WHATWG WebSocket.send accepts USVString | BufferSource | Blob. Unlike the
      // server's send(), a Blob DOES go out as a Binary frame here (verified: a
      // client `send(new Blob(["xy"]))` arrives server-side as 2 binary bytes).
      // Anything else is coerced through ToString and sent as Text.
      if (isBufferSource(data)) this._wire.sendBinary(data);
      else if (data && data._u8 instanceof Uint8Array) this._wire.sendBinary(data._u8);
      else this._wire.sendText(String(data));
    }
    // control-frame payload → JS value per binaryType (same rules as onBinary)
    _ctl(b) {
      if (this.binaryType === "arraybuffer") return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
      if (this.binaryType === "blob") return new G.Blob([b]);
      return G.Buffer ? G.Buffer.from(b) : b;
    }
    ping(d) { if (this.readyState === RS.OPEN) this._wire.sendPing(d == null ? "" : d); }
    pong(d) { if (this.readyState === RS.OPEN) this._wire.sendPong(d == null ? "" : d); }
    close(code, reason) {
      if (this.readyState === RS.CLOSED || this.readyState === RS.CLOSING) return;
      // The RFC 6455 §7.4 endpoint set, NOT the browser's "1000 or 3000-4999":
      // bun's WebSocket.cpp isValidCloseCodeForSending says so in as many words
      // ("non-browser clients legitimately send 1001 or 1011, and `ws` accepts
      // the same set"). Enforcing the browser rule here made `ws.close(1001)`
      // throw from inside the open handler, so the close event never fired and
      // the test hung rather than failing on the code. No node-corpus test
      // asserts the narrower rule (node has no WPT websocket suite vendored).
      if (code !== undefined && code !== null &&
          !((code >= 1000 && code <= 1014 && code !== 1004 && code !== 1005 && code !== 1006) ||
            (code >= 3000 && code <= 4999)))
        throw new (G.DOMException || Error)(
          "The close code must be a valid WebSocket close code (1000-1014, excluding the reserved codes 1004-1006, or in the range of 3000 to 4999). Received " + code + ".",
          "InvalidAccessError");
      if (this.readyState === RS.CONNECTING) { this._failConnecting(); return; }
      this.readyState = RS.CLOSING;
      this._state.closeSent = true;
      try { this._wire.sendClose(code == null ? 1000 : code, reason); } catch (e) {}
      const t = G.setTimeout(() => this._finish(code == null ? 1000 : +code | 0, reason == null ? "" : String(reason), true), 300);
      if (t && typeof t.unref === "function") t.unref();
    }
    terminate() { if (this.readyState === RS.CONNECTING) { this._failConnecting(); return; } this._finish(1006, "", false); }
    // WebSocket.cpp:938 failConnectingWebSocket — close()/terminate() while
    // CONNECTING moves to CLOSING (NOT straight to CLOSED), cancels the pending
    // upgrade, and posts a task that runs the spec's "fail the WebSocket
    // connection": an error event followed by close(1006, wasClean=false).
    _failConnecting() {
      if (this._state.done || this._state.failing) return;
      this._state.failing = true;
      this.readyState = RS.CLOSING;
      try { if (this._sock) this._sock.destroy(); } catch (e) {}
      const run = () => {
        if (this._state.done) return;
        const reason = "WebSocket is closed before the connection is established";
        this._state.failing = false;
        this._emit("error", { error: new Error(reason), message: reason });
        this._finish(1006, reason, false);
      };
      if (typeof G.setImmediate === "function") G.setImmediate(run);
      else G.setTimeout(run, 0);
    }
    addEventListener(type, cb) { (this._ls[type] || (this._ls[type] = [])).push(cb); }
    removeEventListener(type, cb) { const l = this._ls[type]; if (l) this._ls[type] = l.filter((x) => x !== cb); }
    dispatchEvent(ev) { this._emit(ev.type, ev); return true; }
    _emit(type, ev) {
      ev.type = type; ev.target = this; if (ev.currentTarget === undefined) ev.currentTarget = this;
      const on = this["on" + type];
      if (typeof on === "function") { try { on.call(this, ev); } catch (e) {} }
      const l = this._ls[type];
      if (l) for (const cb of l.slice()) { try { cb.call(this, ev); } catch (e) {} }
    }
  }
  WebSocket.CONNECTING = RS.CONNECTING; WebSocket.OPEN = RS.OPEN; WebSocket.CLOSING = RS.CLOSING; WebSocket.CLOSED = RS.CLOSED;
  WebSocket.prototype.CONNECTING = RS.CONNECTING; WebSocket.prototype.OPEN = RS.OPEN; WebSocket.prototype.CLOSING = RS.CLOSING; WebSocket.prototype.CLOSED = RS.CLOSED;
  G.WebSocket = WebSocket;
})();
)JS";

}  // namespace mbun::jsc::js_websocket
