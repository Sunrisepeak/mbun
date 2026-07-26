// modules/jsc/src/js_net_part2.cppm — module mbun.jsc.js_net_part2
//
// The second half of js_net.cppm's JS payload. Split purely to keep each .cppm
// under mcpp's ~256 KiB per-file limit, which reports a crossing as
// "index requires mcpp >= 0.0.108" — an error that names neither the file nor
// the size. The seam is a line boundary, not a JS boundary: kNetJS concatenates
// the two parts before evaluation, so the JS is byte-identical to before.
export module mbun.jsc.js_net_part2;

import std;

namespace mbun::jsc::js_net {

export constexpr std::string_view kNetJS_part2 = R"JS(

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
  // node lib/_http_server.js UpgradeStream. An `Upgrade:` request that still
  // carries a body ('upgrade' is emitted the moment the HEAD is parsed, long
  // before the body ends) must not hand the raw socket to the consumer: the
  // bytes still on the wire are the request body, and they belong to `req`.
  // node therefore wraps the socket in a Duplex that forwards writes straight
  // through but WITHHOLDS everything on the read side until
  // requestBodyCompleted() — at which point the bytes past the body become the
  // tunnel's first chunk. This is a hand-rolled Duplex rather than a
  // stream.Duplex subclass because js_net is evaluated before node:stream.
  class UpgradeStream extends EE {
    constructor(sock) {
      super();
      this._sock = sock;
      this._q = [];             // withheld / buffered read-side chunks
      this._active = false;     // requestBodyCompleted() has run
      this._flowing = false;    // a consumer is reading
      this._srcEnded = false;   // the socket's read side hit EOF
      this._endEmitted = false;
      this._closePending = false;
      this.destroyed = false;
      this.readable = true;
      this.writable = true;
      this.allowHalfOpen = !!sock.allowHalfOpen;
      sock.on("error", (err) => this.destroy(err));
      sock.on("close", () => {
        // Tearing the wrapper down here would drop bytes the consumer has not
        // read yet — and the corpus reads the tunnel from a timer, long after
        // the peer's FIN closed the socket. Hold the wrapper open until the
        // queue drains, then close.
        this._srcEnded = true;
        if (this._active && this._q.length && !this._endEmitted) { this._closePending = true; this._pump(); return; }
        this.destroy();
      });
      sock.on("end", () => { this._srcEnded = true; this._pump(); });
      // node's Duplex starts flowing as soon as a 'data' listener appears; the
      // wrapper has to reproduce that, because the corpus attaches its listener
      // from a timer long after the first tunnel byte arrived.
      this.on("newListener", (ev) => {
        if (ev !== "data" && ev !== "readable") return;
        this._flowing = true;
        if (G.process && typeof G.process.nextTick === "function") G.process.nextTick(() => this._pump());
        else G.queueMicrotask(() => this._pump());
      });
    }
    // Called once the request body has ended: from here on the socket's bytes
    // are the tunnel's, starting with whatever followed the last body byte.
    requestBodyCompleted(head) {
      if (this._active) return;
      this._active = true;
      if (head && head.length) this._q.push(head);
      this._sock.on("data", (d) => { this._q.push(d); this._pump(); });
      this._pump();
    }
    _pump() {
      if (this.destroyed || !this._active) return;
      if (this._flowing) {
        while (this._q.length) {
          const c = this._q.shift();
          this.emit("data", c);
          if (this.destroyed) return;
        }
      }
      if (this._srcEnded && this._q.length === 0 && !this._endEmitted) {
        this._endEmitted = true;
        this.readable = false;
        this.emit("end");
        if (this._closePending) { this._closePending = false; this.destroy(); }
      }
    }
    // node's UpgradeStream._read resumes BOTH the request stream and the socket:
    // reading the tunnel must not require the consumer to drain the body first.
    read() { this._flowing = true; this._sock.resume(); this._pump(); return null; }
    resume() { this._flowing = true; this._sock.resume(); this._pump(); return this; }
    pause() { this._flowing = false; this._sock.pause(); return this; }
    setEncoding(enc) { this._sock.setEncoding(enc); return this; }
    setTimeout(ms, cb) { this._sock.setTimeout(ms, cb); return this; }
    setNoDelay(v) { if (this._sock.setNoDelay) this._sock.setNoDelay(v); return this; }
    setKeepAlive(a, b) { if (this._sock.setKeepAlive) this._sock.setKeepAlive(a, b); return this; }
    address() { return this._sock.address(); }
    ref() { this._sock.ref(); return this; }
    unref() { this._sock.unref(); return this; }
    write(chunk, enc, cb) { return this._sock.write(chunk, enc, cb); }
    cork() { if (this._sock.cork) this._sock.cork(); }
    uncork() { if (this._sock.uncork) this._sock.uncork(); }
    end(chunk, enc, cb) { this.writable = false; this._sock.end(chunk, enc, cb); return this; }
    pipe(dest, opts) {
      const onData = (c) => { const ok = dest.write(c); if (ok === false) this.pause(); };
      if (dest.on) dest.on("drain", () => this.resume());
      this.on("data", onData);
      this.on("end", () => { if (!opts || opts.end !== false) { try { dest.end(); } catch (e) {} } });
      this.resume();
      if (dest.emit) dest.emit("pipe", this);
      return dest;
    }
    destroy(err) {
      if (this.destroyed) return this;
      this.destroyed = true;
      this.readable = false; this.writable = false;
      if (err) this.emit("error", err);
      this.emit("close");
      if (!this._sock.destroyed) this._sock.destroy();
      return this;
    }
  }
  ["remoteAddress", "remotePort", "remoteFamily", "localAddress", "localPort", "bytesRead", "bytesWritten", "readyState"]
    .forEach((k) => Object.defineProperty(UpgradeStream.prototype, k, {
      get() { return this._sock[k]; }, configurable: true,
    }));

  function createHttpServer(o, handler, baseSrv) {
    if (typeof o === "function") { handler = o; o = {}; }
    o = o || {};
    // lib/_http_server.js storeHTTPOptions keeps `options.highWaterMark` and
    // node's net.Server hands it to every accepted socket, which is where both
    // `req._readableState.highWaterMark` and `res[kHighWaterMark]` come from
    // (test-http-server-options-highwatermark).
    const srv = baseSrv || new Server({ allowHalfOpen: false, highWaterMark: o.highWaterMark });
    const connEvent = baseSrv ? "secureConnection" : "connection";
    srv._httpConns = new Set();
    const ResponseClass = typeof o.ServerResponse === "function" ? o.ServerResponse : ServerResponse;
    const RequestClass = typeof o.IncomingMessage === "function" ? o.IncomingMessage : IncomingMessage;
    // lib/_http_server.js storeHTTPOptions.
    const HI = G.__mbunHttpInternals || {};
    const freeParser = typeof HI.freeParser === "function" ? HI.freeParser : null;
    const clearIncoming = typeof HI.clearIncoming === "function" ? HI.clearIncoming : null;
    const ConnResetException = typeof HI.ConnResetException === "function"
      ? HI.ConnResetException
      : (msg) => { const e = new Error(msg); e.code = "ECONNRESET"; return e; };
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
    // lib/_http_server.js storeHTTPOptions: `options.shouldUpgradeCallback`
    // decides whether an `Upgrade:` request is handled as an upgrade at all.
    // The default answers "only if somebody is listening", which is what makes
    // an unclaimed upgrade continue as an ORDINARY request (200) instead of the
    // socket being destroyed.
    if (o.shouldUpgradeCallback !== undefined) {
      if (typeof o.shouldUpgradeCallback !== "function") {
        const e = new TypeError('The "options.shouldUpgradeCallback" property must be of type function. Received type ' + typeof o.shouldUpgradeCallback);
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      srv.shouldUpgradeCallback = o.shouldUpgradeCallback;
    } else {
      srv.shouldUpgradeCallback = function () { return this.listenerCount("upgrade") > 0; };
    }
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
    // lib/_http_server.js socketOnError. The canned reply is gated on
    // "nothing of an in-flight response has reached the wire yet", NOT on
    // "this socket has never been written to": a keep-alive connection whose
    // previous response completed has bytesWritten > 0 and must still get its
    // 408 (test-http-server-{headers,request}-timeout-{keepalive,pipelining}
    // all assert exactly that reply after a first successful response).
    const cannedResponse = (code) => {
      switch (code) {
        case "HPE_HEADER_OVERFLOW":
          return "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n";
        case "HPE_CHUNK_EXTENSIONS_OVERFLOW":
          return "HTTP/1.1 413 Payload Too Large\r\nConnection: close\r\n\r\n";
        case "ERR_HTTP_REQUEST_TIMEOUT":
          return "HTTP/1.1 408 Request Timeout\r\nConnection: close\r\n\r\n";
        default:
          return "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
      }
    };
    const socketOnError = (sock, err) => {
      // node lib/_http_server.js socketOnError opens with
      // `this.removeListener('error', socketOnError)` -- "ignore further
      // errors". That removal is load-bearing rather than cosmetic: the parser
      // path calls socketOnError DIRECTLY (onParserExecuteCommon does the
      // same), and the destroy(err) below re-emits 'error' on the socket. With
      // the event listener still armed, every parser error would claim
      // 'clientError' twice. Later parser errors still reach here through the
      // direct call, which is what lets test-http-socket-error-listeners see
      // one 'clientError' per invalid byte.
      if (sock._httpOnError) {
        sock.removeListener("error", sock._httpOnError);
        sock._httpOnError = null;
      }
      if (srv.emit("clientError", err, sock)) {
        // The server handled it and did not tear the socket down, so the parser
        // must keep reporting: see HttpParser#push's _repeatErrors note.
        const p = sock._httpParser || sock.parser;
        if (p) p._repeatErrors = true;
        return;
      }
      const res = sock._httpMessage;
      if (sock.writable && (!res || !res._headerSent)) {
        // end(), not write(): this reactor's destroy() drops the still-queued
        // bytes, and every corpus file here observes the reply followed by a FIN
        // on the peer.
        try { sock.end(cannedResponse(err.code)); } catch (e) {}
      }
      // node finishes socketOnError with `this.destroy(e)`, and it is destroy's
      // error ARGUMENT that makes the connection socket emit 'error':
      // test-http-server-destroy-socket-on-client-error grabs the socket from
      // 'connection' and asserts that exact object ({ code:
      // 'HPE_INVALID_METHOD', bytesParsed, rawPacket }). A previous revision
      // passed no argument, reasoning that node never shows the error to socket
      // listeners — node hides it only from a server that has NONE, which it
      // arranges by installing its own noop handler first. The connection
      // listener above already installs one, so passing the error cannot throw.
      //
      // Deferred while bytes are still queued: end() has armed the FIN, and
      // closing the fd now would drop the canned reply the peer asserts on.
      if (sock._wq && sock._wq.length) { try { sock.emit("error", err); } catch (e) {} }
      else { try { sock.destroy(err); } catch (e) {} }
    };
    const onRequestTimeout = (sock) => {
      const err = new Error("Request timeout");
      err.code = "ERR_HTTP_REQUEST_TIMEOUT";
      socketOnError(sock, err);
    };
    // node lib/_http_server.js setupConnectionsTracking is called from the
    // 'listening' handler and parks the handle on the server under
    // kConnectionsCheckingInterval; re-arming destroys the previous one, and
    // close() destroys the live one. Three corpus files assert `_destroyed` on
    // that exact handle, so it must be the published property, not a private
    // field (test-http-server-clear-timer even emits 'listening' by hand twice
    // on a server that never bound).
    const kCCI = HI.kConnectionsCheckingInterval || Symbol("connectionsCheckingInterval");
    const setupConnectionsTracking = () => {
      if (srv[kCCI]) G.clearInterval(srv[kCCI]);
      const every = srv.connectionsCheckingInterval > 0 ? srv.connectionsCheckingInterval : 30000;
      const h = G.setInterval(sweep, every);
      if (h && typeof h.unref === "function") h.unref();
      srv[kCCI] = h;
      srv._httpSweeper = h;
    };
    srv.on("listening", setupConnectionsTracking);
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
        //
        // Measured against node: "idle" is the set node's ConnectionsList calls
        // active-but-between-messages — a parser that HAS begun a message at
        // some point (on_message_begin) and is not in the middle of one now. A
        // connection that has been accepted but has not sent its first byte is
        // NOT in that set, and node keeps it. "No message in flight" alone is
        // not the same thing, and closing on it destroyed the never-used
        // connection that test-http{,s}-server-close-idle asserts survives
        // (`assert(!client1Closed)`) — that connection has in fact written a
        // PARTIAL request line, so "has begun a message" alone is not enough
        // either: the message must also have finished (_httpMsgOpen), which is
        // node's `last_message_start_ == 0`.
        if (s._httpMsgBegun && !s._httpMsgOpen && !s._httpInFlight) { try { s.destroy(); } catch (e) {} }
      }
    };
    // lib/_http_server.js Server.prototype.close -> httpServerPreClose ->
    // closeIdleConnections(). Without it a keep-alive connection outlives
    // close() and pins the event loop until the keep-alive timer fires, which
    // is the difference between a test finishing and a test timing out.
    const netClose = srv.close;
    srv.close = function (...args) {
      this.closeIdleConnections();
      if (this[kCCI]) G.clearInterval(this[kCCI]);
      this._httpSweeper = null;
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
      // node lib/_http_server.js connectionListenerInternal: `socket.on('error',
      // socketOnError)`. This was a noop, which silently swallowed every error
      // that reached the connection socket by the EVENT path rather than through
      // the parser -- including a user's own `res.socket.emit('error', ...)`, so
      // the response was never torn down. socketOnError is the same handler the
      // parser's onError already uses; the only extra part is node's
      // "ignore further errors" self-removal, which is what keeps
      // socketOnError's own `sock.emit('error', err)` branch from recursing.
      // The noop stays, UNDERNEATH socketOnError and for the whole life of the
      // socket: node keeps one too (`if (this.listenerCount('error',
      // socketOnError) === 0) this.on('error', noop)`), and it is what stops an
      // error emitted on the connection after socketOnError has disarmed itself
      // from becoming an uncaught throw. Re-installing it conditionally instead
      // is not equivalent -- it leaves a window where another layer's listener
      // (a TLSSocket's) is the only one, and a later 'write after end' on a
      // pipelined response threw (test-tls-use-after-free-regression).
      sock.on("error", () => {});
      const onSockError = (e) => {
        socketOnError(sock, e instanceof Error ? e : mkErr(String((e && e.message) || e), codeOf(e)));
      };
      sock._httpOnError = onSockError;
      sock.on("error", onSockError);
      srv._httpConns.add(sock);
      sock.once("close", () => {
        srv._httpConns.delete(sock);
        // node lib/_http_server.js socketOnClose -> freeParser(parser, null,
        // null). The corpus overwrites `parser.free` to observe exactly this
        // (test-http-server-connection-list-when-close).
        // Deferred one turn on purpose. node's socketOnClose reaches freeParser
        // after the incoming message has already ended, because its socket-close
        // and its readable-end orderings differ from this reactor's: here a peer
        // FIN can close the socket before the buffered request body has been
        // drained, and freeing eagerly would blank `req.socket.parser` out from
        // under a still-pending 'end' handler (test-http-server-keepalive-end
        // reads parser.incoming from exactly there).
        //
        // node lib/_http_server.js socketOnClose then does
        // abortIncoming(state.incoming) + abortOutgoing(state.outgoing): a
        // connection that dies mid-exchange must tear down every message still
        // attached to it, and it is that teardown -- not the loop -- the corpus
        // waits on. `req.on('aborted')`, `req.on('close')` and `req.signal`'s
        // abort all originate in IncomingMessage._destroy, and a queued
        // ServerResponse whose socket never arrived only emits 'close' when
        // something destroys it. Without this the socket vanished from the
        // reactor while the request object it carried stayed live forever, so
        // the callback holding server.close() was never reached and the test
        // timed out with a listening server and no sockets -- which reads like a
        // handle leak and is not one.
        //
        // It shares freeParser's one-turn deferral, and for the same reason:
        // destroying eagerly abandons a request whose body is fully buffered but
        // whose 'end' has not been delivered yet, turning the FIN that ends a
        // normal keep-alive exchange into a spurious abort. Measured: eager
        // abort broke test-http-server-keep-alive-defaults,
        // test-http-server-keep-alive-max-requests-null and
        // test-http-keep-alive-pipeline-max-requests, all three on a `req.on
        // ('end')` that stopped firing.
        const parserAtClose = sock.parser;
        const closeLater = () => {
          if (freeParser && parserAtClose) { try { freeParser(parserAtClose, null, sock); } catch (e) {} }
          abortIncoming();
          abortOutgoing();
        };
        if (typeof G.setImmediate === "function") G.setImmediate(closeLater);
        else if (G.process && typeof G.process.nextTick === "function") G.process.nextTick(closeLater);
        else closeLater();
      });
      if (srv.timeout) { try { sock.setTimeout(srv.timeout); } catch (e) {} }
      sock.server = srv;
      sock._httpInFlight = 0;
      // headersTimeout / requestTimeout clocks (ConnectionsList's
      // last_message_start): reset whenever the connection is free to receive
      // the next message.
      sock._httpMsgStart = Date.now();
      sock._httpHeadersDone = false;
      sock._httpMsgIdle = false;
      sock._httpMsgBegun = false;
      sock._httpMsgOpen = false;
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
      // node lib/_http_server.js `state.incoming`: every request message
      // dispatched on this connection that has not finished its response yet.
      // A single `sock._httpIncoming` slot cannot stand in for it -- a pipeline
      // has several live at once, and socketOnClose must abort all of them.
      const incoming = [];
      // lib/_http_server.js abortIncoming/abortOutgoing, verbatim: shift-and-
      // destroy so a 'close' handler that re-enters (server.close() from inside
      // one) cannot see a half-drained list.
      const abortIncoming = () => {
        while (incoming.length) {
          const req = incoming.shift();
          try { req.destroy(ConnResetException("aborted")); } catch (e) {}
        }
      };
      const abortOutgoing = () => {
        while (outgoing.length) {
          const res = outgoing.shift();
          try { res.destroy(ConnResetException("aborted")); } catch (e) {}
        }
      };

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

      // node keeps ONE llhttp parser per connection, so bytes that arrive past a
      // completed message are parsed IMMEDIATELY (parser.execute() consumes the
      // whole chunk): a pipelined next request is dispatched at once and queued
      // through `outgoing`, and a malformed tail becomes a parse error -> the
      // server's canned 400 -> socket destroyed. This translation used to re-arm
      // the parser only from resOnFinish, so a handler that never ends its
      // response left the peer's remaining bytes unparsed forever and the
      // connection hung with nothing destroyed (test-http-blank-header et al.).
      // Iterative, not recursive: a deep pipeline would otherwise nest one
      // pumpCarry frame per message and overflow the stack. The queue depth cap
      // keeps a flood from materialising unbounded res objects — beyond it the
      // bytes stay in `carry` and resOnFinish drains them, the old behaviour.
      const MAX_PIPELINE_AHEAD = 128;
      let rearming = false;
      const carryBytes = () => { let n = 0; for (let i = 0; i < carry.length; i++) n += carry[i].length; return n; };
      const rearm = () => {
        if (rearming) return;  // the running loop below observes the new carry
        rearming = true;
        try {
          for (;;) {
            if (sock.destroyed || sock._httpUpgraded) break;
            if (!sock._httpParser) {
              if (carryBytes() === 0 || outgoing.length >= MAX_PIPELINE_AHEAD) break;
              startParser();
            }
            const before = carryBytes();
            pumpCarry();
            const after = carryBytes();
            if (after === 0) break;
            // No progress with a parser still hungry for bytes: wait for more.
            if (after >= before && sock._httpParser && !sock._httpParser.done) break;
          }
        } finally { rearming = false; }
      };

      const startParser = () => {
        const parser = new HttpParser(false);
        if (typeof srv.maxHeadersCount === "number" && srv.maxHeadersCount > 0) {
          parser.maxHeaderPairs = srv.maxHeadersCount << 1;
        }
        if (typeof srv.maxHeaderSize === "number" && srv.maxHeaderSize > 0) parser.maxHeaderSize = srv.maxHeaderSize;
        sock._httpParser = parser;
        // node keeps ONE parser per connection and republishes it as
        // `socket.parser`; this translation re-arms a fresh parser per message,
        // so the first one is pinned as the connection's public parser and the
        // per-message `incoming`/`outgoing` slots are mirrored onto it. That
        // keeps the identity the corpus depends on (`req.socket.parser` observed
        // in one request must still be the object freeParser later touches).
        if (!sock.parser) {
          sock.parser = parser;
          if (typeof parser.free !== "function") parser.free = function () {};
          if (typeof parser.close !== "function") parser.close = function () {};
          parser.socket = sock;
          parser.incoming = null;
          parser.outgoing = null;
        }
        let im = null;
        let res = null;
        let upgraded = false;
        // Set at head-complete, consumed exactly once by handover().
        let doUpgrade = false;
        // The 'upgrade'/'connect' event already went out with an EMPTY head
        // because the request body had not finished (node's UpgradeStream
        // path); the bytes past the body arrive through the wrapper instead.
        let upStream = null;
        // node lib/_http_server.js onParserExecuteCommon sets
        // `socket.readableFlowing = null` immediately before handing the socket
        // over: the tunnel is delivered NOT flowing, so neither the bytes past
        // the head nor EOF reach the consumer until it starts reading. mbun's
        // reactor socket has no three-state flowing flag, so pause it and
        // resume on the first sign of read intent.
        const parkForConsumer = () => {
          // Not pause(): node keeps READING (so the peer's FIN still arrives and
          // the connection can still be torn down) and only stops EMITTING.
          // Stopping the read outright stranded a pipelined upgrade whose
          // consumer never subscribes — test-http-pipeline-socket-parser-typeerror
          // hangs on exactly that.
          sock._flowing = false;
          sock._holdForReader = true;
          const kick = () => { if (!sock.destroyed) sock.resume(); };
          const later = (f) => {
            if (G.process && typeof G.process.nextTick === "function") G.process.nextTick(f);
            else G.queueMicrotask(f);
          };
          if (sock.listenerCount("data") > 0 || sock.listenerCount("readable") > 0) { later(kick); return; }
          const onNew = (ev) => {
            if (ev !== "data" && ev !== "readable") return;
            sock.removeListener("newListener", onNew);
            later(kick);
          };
          sock.on("newListener", onNew);
        };
        const handover = () => {
          if (!doUpgrade) return;
          doUpgrade = false;
          const head = G.Buffer ? G.Buffer.from(parser.leftover()) : parser.leftover();
          sock._httpParser = null;
          // The raw socket now belongs to the upgrade/CONNECT listener: no
          // further byte on it is HTTP, so nothing may re-arm a parser for it
          // (doing so re-parsed tunnel traffic as a new request).
          sock._httpUpgraded = true;
          sock._dataSink = null;
          // node lib/_http_server.js onParserExecuteCommon drops state.onClose
          // (along with onData/onEnd/onDrain) before handing the socket over, so
          // an upgraded request is never abortIncoming()'d -- the socket is no
          // longer the http server's to tear down. This listener still has the
          // _httpConns bookkeeping to do, so forget the message instead of the
          // listener.
          if (im) {
            const upAt = incoming.indexOf(im);
            if (upAt !== -1) incoming.splice(upAt, 1);
          }
          // node removes state.onData here; this translation must too, or the
          // server's own listener keeps the socket looking "already read" to
          // parkForConsumer and keeps counting toward listenerCount('data').
          sock.removeListener("data", onSockData);
          // node onParserExecuteCommon runs unconsume() + freeParser(), which
          // takes the connection OUT of the server's ConnectionsList: an
          // upgraded socket is no longer the http server's to sweep or to close.
          // Leaving it in made server.close() -> closeIdleConnections() destroy
          // a live tunnel (test-http-upgrade-server-with-body closes the server
          // from the client's 'end', while the tunnel is still being read).
          try { srv._httpConns.delete(sock); } catch (e) {}
          if (upStream) {
            // 'upgrade' already went out with an empty head — activate the
            // wrapper instead of emitting a second time.
            upStream.requestBodyCompleted(head);
            return;
          }
          // node still checks listenerCount here: shouldUpgradeCallback may
          // claim a request the server has no handler for (and CONNECT is
          // claimed unconditionally), and node answers that with a destroy.
          const ev = im && im.method === "CONNECT" ? "connect" : "upgrade";
          if (srv.listenerCount(ev) === 0) { sock.destroy(); return; }
          parkForConsumer();
          srv.emit(ev, im, sock, head);
        };

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
          // node lib/_http_server.js resOnFinish: `state.incoming.shift()`, with
          // an assert that the head IS this request -- except when
          // abortIncoming() already emptied the list. Tolerate an out-of-order
          // entry rather than assert: a pipelined response can finish before an
          // earlier one here, and dropping the wrong element would leave a
          // finished request to be "aborted" later.
          if (im) {
            const at = incoming.indexOf(im);
            if (at !== -1) incoming.splice(at, 1);
          }
          // node lib/_http_server.js resOnFinish -> clearIncoming(req): release
          // the parser's reference to the finished message, but only once the
          // message has actually ended (otherwise defer to its 'end').
          if (clearIncoming && im) { try { clearIncoming(im); } catch (e) {} }
          // node lib/_http_server.js emitCloseNT sets `destroyed` as well as
          // `_closed`. That flag is load-bearing: OutgoingMessage.write() on a
          // *destroyed* message reports ERR_STREAM_WRITE_AFTER_END through the
          // write callback only, while on a merely-finished one it also emits
          // 'error' — and test-http-server-write-{,end-}after-end installs
          // res.on('error', mustNotCall()) and writes from a setImmediate.
          const closeNT = () => { if (!res._closed) { res.destroyed = true; res._closed = true; res.emit("close"); } };
          if (G.process && typeof G.process.nextTick === "function") G.process.nextTick(closeNT);
          else G.queueMicrotask(closeNT);
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
            if (parser.done) rearm();
            else parser._afterDone = rearm;
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
          // node lib/_http_server.js parserOnIncoming: `state.incoming.push(req)`.
          incoming.push(im);
          if (sock.parser) { sock.parser.incoming = im; im.parser = sock.parser; }
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
            im.upgrade = true;
            // node lib/_http_server.js parserOnIncoming: an upgrade request is
            // only TREATED as an upgrade when the server would handle it —
            //   req.upgrade = req.method === 'CONNECT' ||
            //                 !!server.shouldUpgradeCallback(req)
            // and the default callback answers `listenerCount('upgrade') > 0`.
            // Otherwise req.upgrade goes back to false and the message
            // continues as an ORDINARY request. This translation destroyed the
            // socket instead, so an unclaimed `Connection: upgrade` never got
            // the 200 that test-http-upgrade-server's no-listener leg and
            // test-http-upgrade-advertise's last case both wait for.
            // A throwing shouldUpgradeCallback is NOT swallowed — node lets it
            // out of parserOnIncoming and it lands on uncaughtException
            // (test-http-upgrade-server-callback's last leg asserts exactly
            // that), so do not wrap this in a try.
            const claim = isConnect || !!srv.shouldUpgradeCallback(im);
            if (!claim) { im.upgrade = false; }
            else {
            upgraded = true;
            doUpgrade = true;
            // The handover is deferred to on_message_complete, which is where
            // llhttp actually pauses an upgrade (HPE_PAUSED_UPGRADE): a request
            // that carries a body — `Upgrade:` plus Content-Length or chunked —
            // has that body parsed FIRST and delivered to `req`, and only the
            // bytes past it are the upgrade head.
            if (parser.state === "done") return;
            // A body is still streaming. node emits 'upgrade' NOW with an empty
            // head and hands over an UpgradeStream (not the raw socket) that
            // withholds every tunnel byte until the body has ended; the socket
            // itself keeps feeding the parser meanwhile. `_dataSink` takes the
            // bytes straight to the parser so that a consumer which subscribes
            // to the wrapper immediately (test-http-upgrade-server-with-large-
            // body) can never observe request-body bytes on the tunnel.
            const ev = isConnect ? "connect" : "upgrade";
            sock._dataSink = onSockData;
            const later = (f) => {
              if (G.process && typeof G.process.nextTick === "function") G.process.nextTick(f);
              else G.queueMicrotask(f);
            };
            later(() => {
              // handover() may already have run (the whole message arrived in
              // one chunk), in which case the real head went out with the emit.
              if (!doUpgrade || sock.destroyed || upStream) return;
              if (srv.listenerCount(ev) === 0) { sock._dataSink = null; sock.destroy(); return; }
              upStream = new UpgradeStream(sock);
              srv.emit(ev, im, upStream, G.Buffer ? G.Buffer.alloc(0) : new Uint8Array(0));
            });
            return;
            }
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
          if (sock.parser) sock.parser.outgoing = res;
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
          if (upgraded) {
            // The request message is complete before the socket becomes a
            // tunnel, so `req` ends exactly as node's does (its trailers are
            // stored on the already-complete message, then EOF), and only then
            // do the leftover bytes become the upgrade head.
            if (im) {
              im.complete = true;
              const rawTr0 = parser.rawTrailers;
              if (rawTr0 && rawTr0.length) im._addHeaderLines(rawTr0, rawTr0.length);
              im.push(null);
            }
            if (sock._httpInFlight > 0) sock._httpInFlight--;
            sock._httpMsgOpen = false;
            handover();
            return;
          }
          if (!parser.headDone) { sock.destroy(); return; }
          sock._httpParser = null;
          // leftover() is a view over a parser we are about to drop; copying it
          // once per pipelined request made intake quadratic.
          carry.unshift(parser.leftover());
          // Request message complete -> this connection stops counting as
          // in-flight for closeIdleConnections (llhttp on_message_complete).
          if (sock._httpInFlight > 0) sock._httpInFlight--;
          sock._httpMsgOpen = false;
          // EOF: bun internal/http.ts:187 `self.push(null); self.complete = true`.
          if (im) {
            im.complete = true;
            // _addHeaderLines routes to rawTrailers once `complete` is set —
            // the same order lib/_http_server.js uses (parserOnMessageComplete
            // stores the trailers on the already-complete message).
            const rawTr = parser.rawTrailers;
            if (rawTr && rawTr.length) im._addHeaderLines(rawTr, rawTr.length);
            im.push(null);
          }
          if (parser._afterDone) { const f = parser._afterDone; parser._afterDone = null; f(); }
          // Parse whatever the peer already sent past this message now (node's
          // single-parser semantics), instead of waiting for this response to
          // finish. No-op when the tail is empty or the pipeline is capped.
          rearm();
        };
        parser.onError = (e) => {
          // lib/_http_server.js socketOnError: the server's own answer to a
          // malformed request is a canned 400 (431 head overflow, 413 chunk
          // extensions overflow), and only when nobody claimed 'clientError'.
          socketOnError(sock, e || mkErr("Parse Error", "HPE_INVALID_CONSTANT"));
        };
      };

      // Named (node's `state.onData`) because the upgrade handover removes it:
      // once the socket is a tunnel it must stop looking like a reader to
      // parkForConsumer, and a pending-body upgrade routes the wire straight
      // here through sock._dataSink so no 'data' is emitted at all.
      const onSockData = (chunk) => {
        // First byte of the next message on an idle connection: llhttp's
        // on_message_begin, where headersTimeout/requestTimeout start counting.
        if (sock._httpMsgIdle) { sock._httpMsgIdle = false; sock._httpMsgStart = Date.now(); }
        // llhttp on_message_begin / on_message_complete, as closeIdleConnections
        // reads them: _httpMsgBegun latches for the connection's lifetime,
        // _httpMsgOpen tracks whether a message is being parsed right now.
        sock._httpMsgBegun = true;
        sock._httpMsgOpen = true;
        const b = u8(chunk);
        const p = sock._httpParser;
        if (p && !p.done) p.push(b);
        // A message that ended exactly on a chunk boundary leaves no parser
        // armed; node would still parse the next chunk on arrival, so arm one
        // and consume it rather than parking the bytes until resOnFinish.
        else if (sock._httpUpgraded) { /* tunnel bytes: not ours */ }
        else { carry.push(b.slice()); if (!p) rearm(); }
      };
      sock.on("data", onSockData);
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
