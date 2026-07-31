// modules/jsc/src/js_http2.cppm - module mbun.jsc.js_http2
//
// node:http2 CLIENT vertical slice (http2.connect / ClientHttp2Session /
// ClientHttp2Stream) over the real reactor: plain net.Socket for h2c and the
// node:tls TLSSocket (ALPN "h2") for h2. Implements the HTTP/2 wire protocol
// (RFC 7540 framing + RFC 7541 HPACK) in JS -- mbun exposes real reactor
// sockets to JS (js_net.cppm) + TLS/ALPN (js_tls_live.cppm), so no native
// framing binding is needed. Evaluated AFTER kNetJS + kTlsLiveJS.
//
// Blueprint: bun-ref src/js/node/http2.ts (constants, event/error shape,
// nameForErrorCode, ClientHttp2Session/Stream surface) + src/runtime/api/bun/
// h2_frame_parser.rs (frame validation / error codes) + RFC 7540/7541. Error
// message templates from src/jsc/bindings/ErrorCode.cpp (ERR_HTTP2_SESSION_ERROR
// / ERR_HTTP2_STREAM_ERROR). Huffman table = RFC 7541 Appendix B (canonical).
//
// The SERVER half (createServer/createSecureServer, ServerHttp2Session/Stream,
// Http2ServerRequest/Response) is implemented below over net.Server / tls.Server
// using the SAME JS framing + HPACK as the client. Response HEADERS split into
// HEADERS+CONTINUATION when they exceed the peer max frame size; trailers via
// stream.respond({waitForTrailers}) + sendTrailers().
// DEFERRED (honest): server push (PUSH_PROMISE disabled via ENABLE_PUSH=0),
// stream priority, ALTSVC, ORIGIN, and strict per-stream flow-control accounting.
export module mbun.jsc.js_http2;

import std;
import mbun.jsc.js_http2_part2;

namespace mbun::jsc::js_http2 {

// Split pre-emptively: this file reached 246,389 bytes against mcpp's ~256 KiB
// per-file limit, which it reports as "index requires mcpp >= 0.0.108 but this is
// mcpp 0.0.103" — an error naming neither the file nor the size. js_net.cppm hit
// that wall mid-merge and cost a bisect to find; 15 KiB of headroom is not enough
// to leave for the next change.
//
// The seam is a plain line boundary, NOT a JS statement boundary, and that is
// safe by construction: the parts are concatenated into one std::string before
// they are ever evaluated, so the JS never sees it.
export constexpr std::string_view kHttp2JS_part1 = R"JS(
(function () {
  "use strict";
  const G = globalThis;
  const M = G.__mbunNativeModules || {};
  const net = M["net"] || M["node:net"];
  const tls = M["tls"] || M["node:tls"];
  const http2 = M["http2"] || M["node:http2"];
  const Buffer = G.Buffer;
  if (!net || !net.Socket || !http2) return;  // reactor unavailable
  const EE = (M["events"] && M["events"].EventEmitter) || class { on() { return this; } once() { return this; } emit() { return false; } };
  // events.captureRejections routes a listener's rejected promise to the
  // emitter's `Symbol.for('nodejs.rejection')` method; without one the default
  // is `emit('error')`, which on an http2 server is an uncaught throw. node
  // gives Http2Session and Http2Server their own handlers (core.js
  // [EventEmitter.captureRejectionSymbol]).
  const kRejection = G.Symbol.for("nodejs.rejection");
  if (typeof Promise.withResolvers !== "function") {
    Promise.withResolvers = function () { let resolve, reject; const promise = new Promise((res, rej) => { resolve = res; reject = rej; }); return { promise, resolve, reject }; };
  }

  // === built-in HTTP/2 diagnostics channels ===
  // node lib/internal/http2/core.js resolves these twelve channels once at module
  // load and gates every publish on `hasSubscribers`, so an unsubscribed channel
  // costs one property read on the hot path. kNodeDiagJS ships inside the master
  // builtins blob, which is evaluated before this partition, so the module is
  // already there; the kNoDC fallback only matters if it ever is not.
  const dc = M["diagnostics_channel"] || M["node:diagnostics_channel"];
  const kNoDC = { hasSubscribers: false, publish() {} };
  const dcChan = (name) => (dc && typeof dc.channel === "function" ? dc.channel(name) : kNoDC);
  const onClientStreamCreatedChannel = dcChan("http2.client.stream.created");
  const onClientStreamStartChannel = dcChan("http2.client.stream.start");
  const onClientStreamErrorChannel = dcChan("http2.client.stream.error");
  const onClientStreamBodyChunkSentChannel = dcChan("http2.client.stream.bodyChunkSent");
  const onClientStreamBodySentChannel = dcChan("http2.client.stream.bodySent");
  const onClientStreamFinishChannel = dcChan("http2.client.stream.finish");
  const onClientStreamCloseChannel = dcChan("http2.client.stream.close");
  const onServerStreamCreatedChannel = dcChan("http2.server.stream.created");
  const onServerStreamStartChannel = dcChan("http2.server.stream.start");
  const onServerStreamErrorChannel = dcChan("http2.server.stream.error");
  const onServerStreamFinishChannel = dcChan("http2.server.stream.finish");
  const onServerStreamCloseChannel = dcChan("http2.server.stream.close");
  // node publishes 'close' exactly once per stream, from nghttp2's onStreamClose.
  // mbun reaches the same point from two directions (a graceful finish and a
  // destroy), so the publish is latched.
  function dcPublishStreamClose(stream) {
    if (stream._dcClosePublished) return;
    stream._dcClosePublished = true;
    const ch = stream._serverSide === true ? onServerStreamCloseChannel : onClientStreamCloseChannel;
    if (ch.hasSubscribers) ch.publish({ stream });
  }

  // === constants (blueprint: bun-ref src/js/node/http2.ts) ===
  const constants = {
  NGHTTP2_ERR_FRAME_SIZE_ERROR: -522,
  NGHTTP2_SESSION_SERVER: 0,
  NGHTTP2_SESSION_CLIENT: 1,
  NGHTTP2_STREAM_STATE_IDLE: 1,
  NGHTTP2_STREAM_STATE_OPEN: 2,
  NGHTTP2_STREAM_STATE_RESERVED_LOCAL: 3,
  NGHTTP2_STREAM_STATE_RESERVED_REMOTE: 4,
  NGHTTP2_STREAM_STATE_HALF_CLOSED_LOCAL: 5,
  NGHTTP2_STREAM_STATE_HALF_CLOSED_REMOTE: 6,
  NGHTTP2_STREAM_STATE_CLOSED: 7,
  NGHTTP2_FLAG_NONE: 0,
  NGHTTP2_FLAG_END_STREAM: 1,
  NGHTTP2_FLAG_END_HEADERS: 4,
  NGHTTP2_FLAG_ACK: 1,
  NGHTTP2_FLAG_PADDED: 8,
  NGHTTP2_FLAG_PRIORITY: 32,
  DEFAULT_SETTINGS_HEADER_TABLE_SIZE: 4096,
  DEFAULT_SETTINGS_ENABLE_PUSH: 1,
  DEFAULT_SETTINGS_MAX_CONCURRENT_STREAMS: 4294967295,
  DEFAULT_SETTINGS_INITIAL_WINDOW_SIZE: 65535,
  DEFAULT_SETTINGS_MAX_FRAME_SIZE: 16384,
  DEFAULT_SETTINGS_MAX_HEADER_LIST_SIZE: 65535,
  DEFAULT_SETTINGS_ENABLE_CONNECT_PROTOCOL: 0,
  MAX_MAX_FRAME_SIZE: 16777215,
  MIN_MAX_FRAME_SIZE: 16384,
  MAX_INITIAL_WINDOW_SIZE: 2147483647,
  NGHTTP2_SETTINGS_HEADER_TABLE_SIZE: 1,
  NGHTTP2_SETTINGS_ENABLE_PUSH: 2,
  NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS: 3,
  NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE: 4,
  NGHTTP2_SETTINGS_MAX_FRAME_SIZE: 5,
  NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE: 6,
  NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL: 8,
  PADDING_STRATEGY_NONE: 0,
  PADDING_STRATEGY_ALIGNED: 1,
  PADDING_STRATEGY_MAX: 2,
  PADDING_STRATEGY_CALLBACK: 1,
  NGHTTP2_NO_ERROR: 0,
  NGHTTP2_PROTOCOL_ERROR: 1,
  NGHTTP2_INTERNAL_ERROR: 2,
  NGHTTP2_FLOW_CONTROL_ERROR: 3,
  NGHTTP2_SETTINGS_TIMEOUT: 4,
  NGHTTP2_STREAM_CLOSED: 5,
  NGHTTP2_FRAME_SIZE_ERROR: 6,
  NGHTTP2_REFUSED_STREAM: 7,
  NGHTTP2_CANCEL: 8,
  NGHTTP2_COMPRESSION_ERROR: 9,
  NGHTTP2_CONNECT_ERROR: 10,
  NGHTTP2_ENHANCE_YOUR_CALM: 11,
  NGHTTP2_INADEQUATE_SECURITY: 12,
  NGHTTP2_HTTP_1_1_REQUIRED: 13,
  NGHTTP2_DEFAULT_WEIGHT: 16,
  HTTP2_HEADER_STATUS: ":status",
  HTTP2_HEADER_METHOD: ":method",
  HTTP2_HEADER_AUTHORITY: ":authority",
  HTTP2_HEADER_SCHEME: ":scheme",
  HTTP2_HEADER_PATH: ":path",
  HTTP2_HEADER_PROTOCOL: ":protocol",
  HTTP2_HEADER_ACCEPT_ENCODING: "accept-encoding",
  HTTP2_HEADER_ACCEPT_LANGUAGE: "accept-language",
  HTTP2_HEADER_ACCEPT_RANGES: "accept-ranges",
  HTTP2_HEADER_ACCEPT: "accept",
  HTTP2_HEADER_ACCESS_CONTROL_ALLOW_CREDENTIALS: "access-control-allow-credentials",
  HTTP2_HEADER_ACCESS_CONTROL_ALLOW_HEADERS: "access-control-allow-headers",
  HTTP2_HEADER_ACCESS_CONTROL_ALLOW_METHODS: "access-control-allow-methods",
  HTTP2_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN: "access-control-allow-origin",
  HTTP2_HEADER_ACCESS_CONTROL_EXPOSE_HEADERS: "access-control-expose-headers",
  HTTP2_HEADER_ACCESS_CONTROL_REQUEST_HEADERS: "access-control-request-headers",
  HTTP2_HEADER_ACCESS_CONTROL_REQUEST_METHOD: "access-control-request-method",
  HTTP2_HEADER_AGE: "age",
  HTTP2_HEADER_AUTHORIZATION: "authorization",
  HTTP2_HEADER_CACHE_CONTROL: "cache-control",
  HTTP2_HEADER_CONNECTION: "connection",
  HTTP2_HEADER_CONTENT_DISPOSITION: "content-disposition",
  HTTP2_HEADER_CONTENT_ENCODING: "content-encoding",
  HTTP2_HEADER_CONTENT_LENGTH: "content-length",
  HTTP2_HEADER_CONTENT_TYPE: "content-type",
  HTTP2_HEADER_COOKIE: "cookie",
  HTTP2_HEADER_DATE: "date",
  HTTP2_HEADER_ETAG: "etag",
  HTTP2_HEADER_FORWARDED: "forwarded",
  HTTP2_HEADER_HOST: "host",
  HTTP2_HEADER_IF_MODIFIED_SINCE: "if-modified-since",
  HTTP2_HEADER_IF_NONE_MATCH: "if-none-match",
  HTTP2_HEADER_IF_RANGE: "if-range",
  HTTP2_HEADER_LAST_MODIFIED: "last-modified",
  HTTP2_HEADER_LINK: "link",
  HTTP2_HEADER_LOCATION: "location",
  HTTP2_HEADER_RANGE: "range",
  HTTP2_HEADER_REFERER: "referer",
  HTTP2_HEADER_SERVER: "server",
  HTTP2_HEADER_SET_COOKIE: "set-cookie",
  HTTP2_HEADER_STRICT_TRANSPORT_SECURITY: "strict-transport-security",
  HTTP2_HEADER_TRANSFER_ENCODING: "transfer-encoding",
  HTTP2_HEADER_TE: "te",
  HTTP2_HEADER_UPGRADE_INSECURE_REQUESTS: "upgrade-insecure-requests",
  HTTP2_HEADER_UPGRADE: "upgrade",
  HTTP2_HEADER_USER_AGENT: "user-agent",
  HTTP2_HEADER_VARY: "vary",
  HTTP2_HEADER_X_CONTENT_TYPE_OPTIONS: "x-content-type-options",
  HTTP2_HEADER_X_FRAME_OPTIONS: "x-frame-options",
  HTTP2_HEADER_KEEP_ALIVE: "keep-alive",
  HTTP2_HEADER_PROXY_CONNECTION: "proxy-connection",
  HTTP2_HEADER_X_XSS_PROTECTION: "x-xss-protection",
  HTTP2_HEADER_ALT_SVC: "alt-svc",
  HTTP2_HEADER_CONTENT_SECURITY_POLICY: "content-security-policy",
  HTTP2_HEADER_EARLY_DATA: "early-data",
  HTTP2_HEADER_EXPECT_CT: "expect-ct",
  HTTP2_HEADER_ORIGIN: "origin",
  HTTP2_HEADER_PURPOSE: "purpose",
  HTTP2_HEADER_TIMING_ALLOW_ORIGIN: "timing-allow-origin",
  HTTP2_HEADER_X_FORWARDED_FOR: "x-forwarded-for",
  HTTP2_HEADER_PRIORITY: "priority",
  HTTP2_HEADER_ACCEPT_CHARSET: "accept-charset",
  HTTP2_HEADER_ACCESS_CONTROL_MAX_AGE: "access-control-max-age",
  HTTP2_HEADER_ALLOW: "allow",
  HTTP2_HEADER_CONTENT_LANGUAGE: "content-language",
  HTTP2_HEADER_CONTENT_LOCATION: "content-location",
  HTTP2_HEADER_CONTENT_MD5: "content-md5",
  HTTP2_HEADER_CONTENT_RANGE: "content-range",
  HTTP2_HEADER_DNT: "dnt",
  HTTP2_HEADER_EXPECT: "expect",
  HTTP2_HEADER_EXPIRES: "expires",
  HTTP2_HEADER_FROM: "from",
  HTTP2_HEADER_IF_MATCH: "if-match",
  HTTP2_HEADER_IF_UNMODIFIED_SINCE: "if-unmodified-since",
  HTTP2_HEADER_MAX_FORWARDS: "max-forwards",
  HTTP2_HEADER_PREFER: "prefer",
  HTTP2_HEADER_PROXY_AUTHENTICATE: "proxy-authenticate",
  HTTP2_HEADER_PROXY_AUTHORIZATION: "proxy-authorization",
  HTTP2_HEADER_REFRESH: "refresh",
  HTTP2_HEADER_RETRY_AFTER: "retry-after",
  HTTP2_HEADER_TRAILER: "trailer",
  HTTP2_HEADER_TK: "tk",
  HTTP2_HEADER_VIA: "via",
  HTTP2_HEADER_WARNING: "warning",
  HTTP2_HEADER_WWW_AUTHENTICATE: "www-authenticate",
  HTTP2_HEADER_HTTP2_SETTINGS: "http2-settings",
  HTTP2_METHOD_ACL: "ACL",
  HTTP2_METHOD_BASELINE_CONTROL: "BASELINE-CONTROL",
  HTTP2_METHOD_BIND: "BIND",
  HTTP2_METHOD_CHECKIN: "CHECKIN",
  HTTP2_METHOD_CHECKOUT: "CHECKOUT",
  HTTP2_METHOD_CONNECT: "CONNECT",
  HTTP2_METHOD_COPY: "COPY",
  HTTP2_METHOD_DELETE: "DELETE",
  HTTP2_METHOD_GET: "GET",
  HTTP2_METHOD_HEAD: "HEAD",
  HTTP2_METHOD_LABEL: "LABEL",
  HTTP2_METHOD_LINK: "LINK",
  HTTP2_METHOD_LOCK: "LOCK",
  HTTP2_METHOD_MERGE: "MERGE",
  HTTP2_METHOD_MKACTIVITY: "MKACTIVITY",
  HTTP2_METHOD_MKCALENDAR: "MKCALENDAR",
  HTTP2_METHOD_MKCOL: "MKCOL",
  HTTP2_METHOD_MKREDIRECTREF: "MKREDIRECTREF",
  HTTP2_METHOD_MKWORKSPACE: "MKWORKSPACE",
  HTTP2_METHOD_MOVE: "MOVE",
  HTTP2_METHOD_OPTIONS: "OPTIONS",
  HTTP2_METHOD_ORDERPATCH: "ORDERPATCH",
  HTTP2_METHOD_PATCH: "PATCH",
  HTTP2_METHOD_POST: "POST",
  HTTP2_METHOD_PRI: "PRI",
  HTTP2_METHOD_PROPFIND: "PROPFIND",
  HTTP2_METHOD_PROPPATCH: "PROPPATCH",
  HTTP2_METHOD_PUT: "PUT",
  HTTP2_METHOD_REBIND: "REBIND",
  HTTP2_METHOD_REPORT: "REPORT",
  HTTP2_METHOD_SEARCH: "SEARCH",
  HTTP2_METHOD_TRACE: "TRACE",
  HTTP2_METHOD_UNBIND: "UNBIND",
  HTTP2_METHOD_UNCHECKOUT: "UNCHECKOUT",
  HTTP2_METHOD_UNLINK: "UNLINK",
  HTTP2_METHOD_UNLOCK: "UNLOCK",
  HTTP2_METHOD_UPDATE: "UPDATE",
  HTTP2_METHOD_UPDATEREDIRECTREF: "UPDATEREDIRECTREF",
  HTTP2_METHOD_VERSION_CONTROL: "VERSION-CONTROL",
  HTTP_STATUS_CONTINUE: 100,
  HTTP_STATUS_SWITCHING_PROTOCOLS: 101,
  HTTP_STATUS_PROCESSING: 102,
  HTTP_STATUS_EARLY_HINTS: 103,
  HTTP_STATUS_OK: 200,
  HTTP_STATUS_CREATED: 201,
  HTTP_STATUS_ACCEPTED: 202,
  HTTP_STATUS_NON_AUTHORITATIVE_INFORMATION: 203,
  HTTP_STATUS_NO_CONTENT: 204,
  HTTP_STATUS_RESET_CONTENT: 205,
  HTTP_STATUS_PARTIAL_CONTENT: 206,
  HTTP_STATUS_MULTI_STATUS: 207,
  HTTP_STATUS_ALREADY_REPORTED: 208,
  HTTP_STATUS_IM_USED: 226,
  HTTP_STATUS_MULTIPLE_CHOICES: 300,
  HTTP_STATUS_MOVED_PERMANENTLY: 301,
  HTTP_STATUS_FOUND: 302,
  HTTP_STATUS_SEE_OTHER: 303,
  HTTP_STATUS_NOT_MODIFIED: 304,
  HTTP_STATUS_USE_PROXY: 305,
  HTTP_STATUS_TEMPORARY_REDIRECT: 307,
  HTTP_STATUS_PERMANENT_REDIRECT: 308,
  HTTP_STATUS_BAD_REQUEST: 400,
  HTTP_STATUS_UNAUTHORIZED: 401,
  HTTP_STATUS_PAYMENT_REQUIRED: 402,
  HTTP_STATUS_FORBIDDEN: 403,
  HTTP_STATUS_NOT_FOUND: 404,
  HTTP_STATUS_METHOD_NOT_ALLOWED: 405,
  HTTP_STATUS_NOT_ACCEPTABLE: 406,
  HTTP_STATUS_PROXY_AUTHENTICATION_REQUIRED: 407,
  HTTP_STATUS_REQUEST_TIMEOUT: 408,
  HTTP_STATUS_CONFLICT: 409,
  HTTP_STATUS_GONE: 410,
  HTTP_STATUS_LENGTH_REQUIRED: 411,
  HTTP_STATUS_PRECONDITION_FAILED: 412,
  HTTP_STATUS_PAYLOAD_TOO_LARGE: 413,
  HTTP_STATUS_URI_TOO_LONG: 414,
  HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE: 415,
  HTTP_STATUS_RANGE_NOT_SATISFIABLE: 416,
  HTTP_STATUS_EXPECTATION_FAILED: 417,
  HTTP_STATUS_TEAPOT: 418,
  HTTP_STATUS_MISDIRECTED_REQUEST: 421,
  HTTP_STATUS_UNPROCESSABLE_ENTITY: 422,
  HTTP_STATUS_LOCKED: 423,
  HTTP_STATUS_FAILED_DEPENDENCY: 424,
  HTTP_STATUS_TOO_EARLY: 425,
  HTTP_STATUS_UPGRADE_REQUIRED: 426,
  HTTP_STATUS_PRECONDITION_REQUIRED: 428,
  HTTP_STATUS_TOO_MANY_REQUESTS: 429,
  HTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE: 431,
  HTTP_STATUS_UNAVAILABLE_FOR_LEGAL_REASONS: 451,
  HTTP_STATUS_INTERNAL_SERVER_ERROR: 500,
  HTTP_STATUS_NOT_IMPLEMENTED: 501,
  HTTP_STATUS_BAD_GATEWAY: 502,
  HTTP_STATUS_SERVICE_UNAVAILABLE: 503,
  HTTP_STATUS_GATEWAY_TIMEOUT: 504,
  HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED: 505,
  HTTP_STATUS_VARIANT_ALSO_NEGOTIATES: 506,
  HTTP_STATUS_INSUFFICIENT_STORAGE: 507,
  HTTP_STATUS_LOOP_DETECTED: 508,
  HTTP_STATUS_BANDWIDTH_LIMIT_EXCEEDED: 509,
  HTTP_STATUS_NOT_EXTENDED: 510,
  HTTP_STATUS_NETWORK_AUTHENTICATION_REQUIRED: 511,
  };
  const nameForErrorCode = [
    "NGHTTP2_NO_ERROR", "NGHTTP2_PROTOCOL_ERROR", "NGHTTP2_INTERNAL_ERROR",
    "NGHTTP2_FLOW_CONTROL_ERROR", "NGHTTP2_SETTINGS_TIMEOUT", "NGHTTP2_STREAM_CLOSED",
    "NGHTTP2_FRAME_SIZE_ERROR", "NGHTTP2_REFUSED_STREAM", "NGHTTP2_CANCEL",
    "NGHTTP2_COMPRESSION_ERROR", "NGHTTP2_CONNECT_ERROR", "NGHTTP2_ENHANCE_YOUR_CALM",
    "NGHTTP2_INADEQUATE_SECURITY", "NGHTTP2_HTTP_1_1_REQUIRED",
  ];
  const errName = (code) => nameForErrorCode[code] || String(code);
  const mkErr = (msg, code) => { const e = new Error(msg); e.code = code; return e; };
  // ERR_HTTP2_SESSION_ERROR / ERR_HTTP2_STREAM_ERROR message templates:
  // ref bun-ref src/jsc/bindings/ErrorCode.cpp (ERR_HTTP2_SESSION_ERROR ->
  // "Session closed with error code " + name; ERR_HTTP2_STREAM_ERROR ->
  // "Stream closed with error code " + name).
  // `http2.sensitiveHeaders` is a Symbol whose identity matters: it is the key
  // callers put on a headers object to mark fields that must be HPACK
  // never-indexed. node's is the *private* `kSensitiveHeaders` from
  // lib/internal/http2/util.js, which cannot be reconstructed, so mbun's own is
  // the registered `Symbol.for(…)` and node's is appended to this list once the
  // internals are in play (see adoptNodeHttp2Internals below). Every read site
  // goes through sensitiveNamesOf/copySensitiveTo so both are honoured.
  let bindingRequested = false;
  const kSensitiveDefault = G.Symbol.for("nodejs.http2.sensitiveHeaders");
  const sensitiveSymbols = [kSensitiveDefault];
  function sensitiveNamesOf(map) {
    const out = new Set();
    if (map == null) return out;
    for (let i = 0; i < sensitiveSymbols.length; i++) {
      const sn = map[sensitiveSymbols[i]];
      if (Array.isArray(sn)) for (const s of sn) out.add(String(s).toLowerCase());
    }
    return out;
  }
  function copySensitiveTo(src, dst) {
    if (src == null) return dst;
    for (let i = 0; i < sensitiveSymbols.length; i++) {
      const s = sensitiveSymbols[i];
      if (src[s] !== undefined) dst[s] = src[s];
    }
    return dst;
  }
  // PORT-SOURCE: compat/node/lib/internal/http2/core.js:760,1611 —
  // `new ERR_HTTP2_SESSION_ERROR(code)` is handed the RAW numeric code, so the
  // message reads "…error code 7". Only the STREAM error is name-mapped
  // (core.js:2487 `nameForErrorCode[code] || code`), which is why streamErr
  // below keeps errName().
  const sessionErr = (code) => mkErr("Session closed with error code " + code, "ERR_HTTP2_SESSION_ERROR");
  const streamErr = (code) => mkErr("Stream closed with error code " + errName(code), "ERR_HTTP2_STREAM_ERROR");
  // ERR_HTTP2_SESSION_ERROR and ERR_HTTP2_ERROR are NOT interchangeable, and
  // mbun used the first for both. node reserves ERR_HTTP2_SESSION_ERROR for a
  // GOAWAY the PEER sent (core.js: `new ERR_HTTP2_SESSION_ERROR(code)`), while
  // a violation the LOCAL endpoint detects is nghttp2's own error object —
  // util.js `NghttpError`, code ERR_HTTP2_ERROR, message nghttp2_strerror(),
  // e.g. 'Protocol error' (-505) or 'Received bad client magic byte string'
  // (-903). Two corpus files assert on exactly that distinction.
  //
  // NghttpErrorCtor is node's real class, adopted alongside the other
  // internal/http2/util.js identities so `constructor: NghttpError` compares
  // equal; without it a shape-compatible plain Error is used.
  let NghttpErrorCtor = null;
  const NGHTTP2_ERR_PROTO = -505;
  const nghttpErr = (errno) => {
    if (typeof NghttpErrorCtor === "function") {
      try { return new NghttpErrorCtor(errno); } catch (e) {}
    }
    const e = new Error(kNghttp2Strerror[String(errno)] || ("Unknown error code " + errno));
    e.code = "ERR_HTTP2_ERROR";
    e.errno = errno;
    return e;
  };
  // The real nghttp2 binding reports submit failures as negative errno values.
  // Keep that submit boundary replaceable for the JS framing backend too: node
  // internals, instrumentation, and embedders can provide the same low-level
  // method on internalBinding('http2').Http2Stream.prototype. A failed submit
  // is stream-local, so it must become NghttpError on this stream (and its
  // existing destroy path sends the matching RST_STREAM to the peer).
  let nativeHttp2StreamPrototype = null;
  const submitNativeStream = (stream, method, ...args) => {
    const submit = nativeHttp2StreamPrototype && nativeHttp2StreamPrototype[method];
    if (typeof submit !== "function") return true;
    const errno = Reflect.apply(submit, stream, args);
    if (typeof errno !== "number" || errno >= 0) return true;
    // A caller of the native binding has also loaded the internal util module;
    // adopt its constructor before exposing the error for identity checks.
    if (bindingRequested) adoptNodeHttp2Internals();
    stream.destroy(nghttpErr(errno));
    return false;
  };
  // internal/errors.js AbortError: what request({ signal }) destroys the stream
  // with once the signal fires.
  const abortErr = (reason) => {
    const e = new Error("This operation was aborted");
    e.name = "AbortError";
    e.code = "ABORT_ERR";
    if (reason !== undefined) e.cause = reason;
    return e;
  };
  // internal/errors.js ERR_HTTP2_STREAM_CANCEL: the error a session destroy hands
  // to a stream that never got a stream id (node's `pendingStreams`).
  const streamCancelErr = (cause) => {
    let msg = "The pending stream has been canceled";
    if (cause && typeof cause.message === "string") msg += " (caused by: " + cause.message + ")";
    const e = mkErr(msg, "ERR_HTTP2_STREAM_CANCEL");
    if (cause) e.cause = cause;
    return e;
  };

  // === HPACK (RFC 7541) ===
  // Huffman canonical code table (Appendix B): code value + bit length per symbol.
  const HC = [8184,8388568,268435426,268435427,268435428,268435429,268435430,268435431,268435432,16777194,1073741820,268435433,268435434,1073741821,268435435,268435436,268435437,268435438,268435439,268435440,268435441,268435442,1073741822,268435443,268435444,268435445,268435446,268435447,268435448,268435449,268435450,268435451,20,1016,1017,4090,8185,21,248,2042,1018,1019,249,2043,250,22,23,24,0,1,2,25,26,27,28,29,30,31,92,251,32764,32,4091,1020,8186,33,93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,252,115,253,8187,524272,8188,16380,34,32765,3,35,4,36,5,37,38,39,6,116,117,40,41,42,7,43,118,44,8,9,45,119,120,121,122,123,32766,2044,16381,8189,268435452,1048550,4194258,1048551,1048552,4194259,4194260,4194261,8388569,4194262,8388570,8388571,8388572,8388573,8388574,16777195,8388575,16777196,16777197,4194263,8388576,16777198,8388577,8388578,8388579,8388580,2097116,4194264,8388581,4194265,8388582,8388583,16777199,4194266,2097117,1048553,4194267,4194268,8388584,8388585,2097118,8388586,4194269,4194270,16777200,2097119,4194271,8388587,8388588,2097120,2097121,4194272,2097122,8388589,4194273,8388590,8388591,1048554,4194274,4194275,4194276,8388592,4194277,4194278,8388593,67108832,67108833,1048555,524273,4194279,8388594,4194280,33554412,67108834,67108835,67108836,134217694,134217695,67108837,16777201,33554413,524274,2097123,67108838,134217696,134217697,67108839,134217698,16777202,2097124,2097125,67108840,67108841,268435453,134217699,134217700,134217701,1048556,16777203,1048557,2097126,4194281,2097127,2097128,8388595,4194282,4194283,33554414,33554415,16777204,16777205,67108842,8388596,67108843,134217702,67108844,67108845,134217703,134217704,134217705,134217706,134217707,268435454,134217708,134217709,134217710,134217711,134217712,67108846];
  const HL = [13,23,28,28,28,28,28,28,28,24,30,28,28,30,28,28,28,28,28,28,28,28,30,28,28,28,28,28,28,28,28,28,6,10,10,12,13,6,8,11,10,10,8,11,8,6,6,6,5,5,5,6,6,6,6,6,6,6,7,8,15,6,12,10,13,6,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,8,7,8,13,19,13,14,6,15,5,6,5,6,5,6,6,6,5,7,7,6,6,6,5,6,7,6,5,5,6,7,7,7,7,7,15,11,14,13,28,20,22,20,20,22,22,22,23,22,23,23,23,23,23,24,23,24,24,22,23,24,23,23,23,23,21,22,23,22,23,23,24,22,21,20,22,22,23,23,21,23,22,22,24,21,22,23,23,21,21,22,21,23,22,23,23,20,22,22,22,23,22,22,23,26,26,20,19,22,23,22,25,26,26,26,27,27,26,24,25,19,21,26,27,27,26,27,24,21,21,26,26,28,27,27,27,20,24,20,21,22,21,21,23,22,22,25,25,24,24,26,23,26,27,26,26,27,27,27,27,27,28,27,27,27,27,27,26];
  // Decode structure: byLen[len] = Map(code -> symbol).
  const HUFF_BY_LEN = (function () {
    const m = {};
    for (let sym = 0; sym < 256; sym++) { const l = HL[sym]; (m[l] || (m[l] = new Map())).set(HC[sym], sym); }
    return m;
  })();
  function huffDecode(bytes) {
    // MSB-first bit walk; match the shortest prefix against the canonical table.
    const out = [];
    let cur = 0, nbits = 0;
    for (let i = 0; i < bytes.length; i++) {
      cur = (cur * 256) + bytes[i]; nbits += 8;
      // try to emit as many symbols as possible
      let progress = true;
      while (progress && nbits >= 5) {
        progress = false;
        for (let l = 5; l <= nbits && l <= 30; l++) {
          const code = Math.floor(cur / Math.pow(2, nbits - l)) % Math.pow(2, l);
          const map = HUFF_BY_LEN[l];
          if (map && map.has(code)) {
            out.push(map.get(code));
            nbits -= l;
            cur = cur % Math.pow(2, nbits);
            progress = true;
            break;
          }
        }
      }
    }
    // remaining bits (< 8) must be all 1s (EOS padding) — ignore.
    return out;
  }

  // RFC 7541 Appendix A: static table (index 1..61). [name, value]
  const STATIC = [
    null,
    [":authority", ""], [":method", "GET"], [":method", "POST"], [":path", "/"],
    [":path", "/index.html"], [":scheme", "http"], [":scheme", "https"], [":status", "200"],
    [":status", "204"], [":status", "206"], [":status", "304"], [":status", "400"],
    [":status", "404"], [":status", "500"], ["accept-charset", ""], ["accept-encoding", "gzip, deflate"],
    ["accept-language", ""], ["accept-ranges", ""], ["accept", ""], ["access-control-allow-origin", ""],
    ["age", ""], ["allow", ""], ["authorization", ""], ["cache-control", ""],
    ["content-disposition", ""], ["content-encoding", ""], ["content-language", ""], ["content-length", ""],
    ["content-location", ""], ["content-range", ""], ["content-type", ""], ["cookie", ""],
    ["date", ""], ["etag", ""], ["expect", ""], ["expires", ""],
    ["from", ""], ["host", ""], ["if-match", ""], ["if-modified-since", ""],
    ["if-none-match", ""], ["if-range", ""], ["if-unmodified-since", ""], ["last-modified", ""],
    ["link", ""], ["location", ""], ["max-forwards", ""], ["proxy-authenticate", ""],
    ["proxy-authorization", ""], ["range", ""], ["referer", ""], ["refresh", ""],
    ["retry-after", ""], ["server", ""], ["set-cookie", ""], ["strict-transport-security", ""],
    ["transfer-encoding", ""], ["user-agent", ""], ["vary", ""], ["via", ""],
    ["www-authenticate", ""],
  ];

  // HPACK decoder with a per-peer dynamic table.
  function HpackDecoder() {
    this.dyn = [];              // [name, value], most-recent first
    this.size = 0;
    this.maxSize = 4096;        // == our advertised SETTINGS_HEADER_TABLE_SIZE
  }
  HpackDecoder.prototype._evict = function () {
    while (this.size > this.maxSize && this.dyn.length) {
      const e = this.dyn.pop();
      this.size -= (e[0].length + e[1].length + 32);
    }
  };
  HpackDecoder.prototype._add = function (name, value) {
    this.dyn.unshift([name, value]);
    this.size += (name.length + value.length + 32);
    this._evict();
  };
  HpackDecoder.prototype._lookup = function (idx) {
    if (idx >= 1 && idx <= 61) return STATIC[idx];
    const d = idx - 62;
    if (d >= 0 && d < this.dyn.length) return this.dyn[d];
    throw mkErr("HPACK invalid index " + idx, "ERR_HTTP2_ERROR");
  };
  // returns { headers: [[name,value]...] } or throws (compression error)
  HpackDecoder.prototype.decode = function (buf) {
    const self = this;
    self._sensitive = [];   // names that arrived as "literal never indexed" (RFC 7541 6.2.3)
    let p = 0;
    const readInt = (prefixBits) => {
      const maxPrefix = (1 << prefixBits) - 1;
      let value = buf[p] & maxPrefix;
      p++;
      if (value < maxPrefix) return value;
      let m = 0, b;
      do { b = buf[p++]; value += (b & 0x7f) * Math.pow(2, m); m += 7; } while (b & 0x80);
      return value;
    };
    const readStr = () => {
      const huff = (buf[p] & 0x80) !== 0;
      const len = readInt(7);
      const slice = buf.subarray(p, p + len);
      p += len;
      if (huff) { const syms = huffDecode(slice); let s = ""; for (let i = 0; i < syms.length; i++) s += String.fromCharCode(syms[i]); return s; }
      let s = ""; for (let i = 0; i < slice.length; i++) s += String.fromCharCode(slice[i]); return s;
    };
    const headers = [];
    while (p < buf.length) {
      const b = buf[p];
      if (b & 0x80) {                       // 1xxxxxxx indexed
        const idx = readInt(7);
        if (idx === 0) throw mkErr("HPACK index 0", "ERR_HTTP2_ERROR");
        const e = self._lookup(idx);
        headers.push([e[0], e[1]]);
      } else if (b & 0x40) {                // 01xxxxxx literal w/ incremental indexing
        const idx = readInt(6);
        const name = idx === 0 ? readStr() : self._lookup(idx)[0];
        const value = readStr();
        self._add(name, value);
        headers.push([name, value]);
      } else if ((b & 0x20) === 0x20) {     // 001xxxxx dynamic table size update
        const newMax = readInt(5);
        self.maxSize = newMax;
        self._evict();
      } else {                              // 0000xxxx literal w/o indexing / 0001xxxx never indexed
        const never = (b & 0x10) !== 0;
        const idx = readInt(4);
        const name = idx === 0 ? readStr() : self._lookup(idx)[0];
        const value = readStr();
        if (never) self._sensitive.push(name);
        headers.push([name, value]);
      }
    }
    return headers;
  };

  // deps/nghttp2 lib/nghttp2_hd.c `hd_deflate_decide_indexing()`: the deflater
  // forces NGHTTP2_NV_FLAG_NO_INDEX on two field names regardless of what the
  // caller asked for — `authorization`, and `cookie` whose value is shorter than
  // 20 octets (a short cookie is assumed to be a per-request token, so indexing
  // it would leak it into the dynamic table). node inherits that, and the peer
  // reports every never-indexed field back through `sensitiveHeaders`: node's
  // own test-http2-sensitive-headers responds with `cookie: donotindex` and
  // asserts the CLIENT sees `[ 'cookie', 'sensitive' ]` even though only
  // 'Sensitive' was listed. mbun's encoder never set the flag on its own, so
  // 'cookie' was missing from that array.
  const neverIndexByDefault = (name, value) =>
    name === "authorization" || (name === "cookie" && String(value).length < 20);

  // HPACK encoder for requests: literal-without-indexing, new name, no huffman
  // (RFC 7541 6.2.2; always valid, keeps encoder state trivial). Pseudo-headers
  // are emitted first (nghttp2/node require them before regular fields).
  function encodeHeaders(list, sensitive) {
    const parts = [];
    const pushStr = (s) => {
      const bytes = [];
      for (let i = 0; i < s.length; i++) bytes.push(s.charCodeAt(i) & 0xff);
      // length as 7-bit-prefix integer, H=0
      encInt(parts, bytes.length, 7, 0);
      for (let i = 0; i < bytes.length; i++) parts.push(bytes[i]);
    };
    for (let i = 0; i < list.length; i++) {
      // 0x00 literal without indexing (new name); 0x10 literal never indexed
      // (RFC 7541 6.2.3) for headers flagged sensitive by http2.sensitiveHeaders.
      parts.push((sensitive && sensitive.has(list[i][0])) || neverIndexByDefault(list[i][0], list[i][1]) ? 0x10 : 0x00);
      pushStr(list[i][0]);
      pushStr(list[i][1]);
    }
    return Buffer.from(parts);
  }
  function encInt(parts, value, prefixBits, highBits) {
    const maxPrefix = (1 << prefixBits) - 1;
    if (value < maxPrefix) { parts.push(highBits | value); return; }
    parts.push(highBits | maxPrefix);
    value -= maxPrefix;
    while (value >= 128) { parts.push((value % 128) + 128); value = Math.floor(value / 128); }
    parts.push(value);
  }

  // === frame helpers ===
  // ALTSVC is RFC 7838 (frame type 0xa), ORIGIN is RFC 8336 (0xc). Both are
  // extension frames: a peer that does not know them ignores them (RFC 9113
  // 5.5), which is why the `default:` arm of _handleFrame must stay permissive.
  const FRAME = { DATA: 0, HEADERS: 1, PRIORITY: 2, RST_STREAM: 3, SETTINGS: 4, PUSH_PROMISE: 5, PING: 6, GOAWAY: 7, WINDOW_UPDATE: 8, CONTINUATION: 9, ALTSVC: 0xa, ORIGIN: 0xc };
  const FLAG = { END_STREAM: 0x1, ACK: 0x1, END_HEADERS: 0x4, PADDED: 0x8, PRIORITY: 0x20 };
  const CLIENT_PREFACE = Buffer.from("505249202a20485454502f322e300d0a0d0a534d0d0a0d0a", "hex");
  // RFC 7540 6.9.2: the connection-level flow-control window starts at 65535
  // and, unlike the stream window, is NOT changed by SETTINGS_INITIAL_WINDOW_SIZE.
  const DEFAULT_CONNECTION_WINDOW = 65535;

  function frameHeader(len, type, flags, streamId) {
    const h = Buffer.alloc(9);
    h[0] = (len >>> 16) & 0xff; h[1] = (len >>> 8) & 0xff; h[2] = len & 0xff;
    h[3] = type; h[4] = flags;
    h[5] = (streamId >>> 24) & 0x7f; h[6] = (streamId >>> 16) & 0xff; h[7] = (streamId >>> 8) & 0xff; h[8] = streamId & 0xff;
    return h;
  }

  // RFC 7540 6.10: a header block larger than the peer's SETTINGS_MAX_FRAME_SIZE
  // must be split across HEADERS + CONTINUATION frames. END_HEADERS is set only
  // on the final frame; extraFlags (e.g. END_STREAM) rides on the initial HEADERS.
  function writeHeaderBlock(session, streamId, block, extraFlags) {
    const max = (session._remoteSettings && session._remoteSettings.maxFrameSize) || 16384;
    extraFlags = extraFlags || 0;
    if (block.length <= max) {
      session._writeFrame(FRAME.HEADERS, FLAG.END_HEADERS | extraFlags, streamId, block);
      return;
    }
    session._writeFrame(FRAME.HEADERS, extraFlags, streamId, block.subarray(0, max));
    let off = max;
    while (off < block.length) {
      const end = Math.min(off + max, block.length);
      const isLast = end >= block.length;
      session._writeFrame(FRAME.CONTINUATION, isLast ? FLAG.END_HEADERS : 0, streamId, block.subarray(off, end));
      off = end;
    }
  }

  // node exposes nghttp2's per-session bookkeeping as `session.state`
  // (Http2SessionState). It is not decorative: @grpc/grpc-js reads
  // `session.state.localWindowSize` on every call it starts (transport.js
  // `createCall`), and an undefined `state` turns that into a synchronous
  // throw which grpc retries forever — an unbounded allocation loop, not a
  // test failure. The session tracks the connection-level flow-control
  // windows (RFC 7540 6.9) so these are real numbers rather than constants.
  // outboundQueueSize is 0 because _writeFrame hands every frame straight to
  // the socket, and the header table sizes are 0 because encodeHeaders only
  // emits literal-without-indexing representations (see the HPACK note above),
  // so neither dynamic table ever holds an entry.
  function sessionState(s, nextStreamID) {
    const local = s._localWindow === undefined ? DEFAULT_CONNECTION_WINDOW : s._localWindow;
    const remote = s._remoteWindow === undefined ? DEFAULT_CONNECTION_WINDOW : s._remoteWindow;
    return {
      // nghttp2 keeps the *effective* local window (what setLocalWindowSize
      // asks for) separate from the announced one: shrinking it below the
      // already-announced window does not retract the announcement.
      effectiveLocalWindowSize: s._effectiveLocalWindow === undefined ? DEFAULT_CONNECTION_WINDOW : s._effectiveLocalWindow,
      effectiveRecvDataLength: DEFAULT_CONNECTION_WINDOW - local,
      nextStreamID: nextStreamID,
      localWindowSize: local,
      lastProcStreamID: s._lastProcStreamId || 0,
      remoteWindowSize: remote,
      outboundQueueSize: 0,
      deflateDynamicTableSize: 0,
      inflateDynamicTableSize: 0,
    };
  }

  // node's Http2Session keeps its inactivity timer under `internal/timers`'
  // private `kTimeout` symbol (core.js: `this[kTimeout] = null` in the
  // constructor, `setStreamTimeout` assigns the live handle), and internals
  // tests read it straight off the session: `session[kTimeout]._idleTimeout`
  // (test-http2-socket-proxy), `request.stream.session[kTimeout]._idleTimeout`
  // (test-http2-compat-socket). The symbol's identity cannot be guessed, so
  // discover it lazily the way js_net's syncTimeoutShape does — an ordinary
  // http2 program never loads the internal module and never pays for the
  // lookup.
  // Resolved at most ONCE per process, hit or miss: the timer is re-armed on
  // every inbound frame, and a failing module resolution per frame would be a
  // real cost for every ordinary http2 program. Once is enough — a program that
  // reads session[kTimeout] requires internal/timers at its own top level, long
  // before a session exists to arm.
  let http2TimeoutSymbol;
  function timeoutSymbol() {
    if (http2TimeoutSymbol !== undefined) return http2TimeoutSymbol;
    http2TimeoutSymbol = null;
    try {
      const timers = typeof G.require === "function" ? G.require("internal/timers") : null;
      if (timers && typeof timers.kTimeout === "symbol") http2TimeoutSymbol = timers.kTimeout;
    } catch (e) {}
    return http2TimeoutSymbol;
  }
  // node src/node_http2.cc `Http2Session::~Http2Session()` walks its outstanding
  // Http2Ping list and calls `ping->Done(false)`, which reaches JS as
  // pingCallback(ack=false) -> `cb(new ERR_HTTP2_PING_CANCEL())`. mbun left the
  // queue in place, so a ping issued and then destroyed never called back at all
  // (test-http2-ping-settings-heapdump). Outstanding SETTINGS acks are NOT
  // cancelled the same way — node drops those callbacks silently, which the same
  // test pins with `session.settings(undefined, common.mustNotCall())`.
  function cancelSessionPings(session) {
    const pings = session._pings;
    if (!pings || pings.length === 0) return;
    const pending = pings.slice();
    pings.length = 0;
    G.queueMicrotask(() => {
      for (const p of pending) {
        try { p.cb(mkErr("HTTP2 ping cancelled", "ERR_HTTP2_PING_CANCEL")); } catch (e) {}
      }
    });
  }
  function syncSessionTimeout(session) {
    const key = timeoutSymbol();
    if (key === null) return;
    if (!Object.prototype.hasOwnProperty.call(session, key))
      Object.defineProperty(session, key, { value: null, writable: true, configurable: true });
    session[key] = session._timer || null;
  }

  // PORT-SOURCE: compat/node/lib/internal/http2/core.js `proxySocketHandler`
  // (and the `get socket()` that lazily installs it).
  //
  // `session.socket` is NOT the transport: node hands out a Proxy whose target
  // is the *session*. Three members answer from the session itself (setTimeout /
  // ref / unref — they must drive the whole multiplexed connection, not one
  // socket), the stream-manipulating members are hard errors because touching
  // them would desynchronise the framing layer, and everything else forwards to
  // the raw transport held in `_rawSocket` (node's `session[kSocket]`).
  //
  // After cleanupSession() node clears `session[kSocket]`, so a proxy captured
  // before close keeps working as an object but every forwarded access raises
  // ERR_HTTP2_SOCKET_UNBOUND — that unbinding is exactly what
  // test-http2-unbound-socket-proxy pins, including `instanceof`, which routes
  // through the getPrototypeOf trap.
  const kProxyOwn = new Set(["setTimeout", "ref", "unref"]);
  const kProxyNoManip = new Set([
    "destroy", "emit", "end", "pause", "read", "resume", "write",
    "setEncoding", "setKeepAlive", "setNoDelay",
  ]);
  const noManipErr = () => mkErr(
    "HTTP/2 sockets should not be directly manipulated (e.g. read and written)",
    "ERR_HTTP2_NO_SOCKET_MANIPULATION");
  const unboundErr = () => mkErr("HTTP/2 socket has been disconnected", "ERR_HTTP2_SOCKET_UNBOUND");
  // `_socketUnbound` is set the moment the session commits to tearing down, so
  // the proxy stops answering at node's timing even while mbun's deferred
  // teardown still needs the live transport under `_rawSocket`.
  function sessionRawSocket(session) {
    const socket = session._rawSocket;
    if (session._socketUnbound === true || socket === undefined || socket === null) throw unboundErr();
    return socket;
  }
  const proxySocketHandler = {
    get(session, prop) {
      if (kProxyOwn.has(prop)) { const v = session[prop]; return typeof v === "function" ? v.bind(session) : v; }
      if (kProxyNoManip.has(prop)) throw noManipErr();
      const socket = sessionRawSocket(session);
      const value = socket[prop];
      return typeof value === "function" ? value.bind(socket) : value;
    },
    getPrototypeOf(session) {
      return Object.getPrototypeOf(sessionRawSocket(session));
    },
    set(session, prop, value) {
      if (kProxyOwn.has(prop)) { session[prop] = value; return true; }
      if (kProxyNoManip.has(prop)) throw noManipErr();
      sessionRawSocket(session)[prop] = value;
      return true;
    },
  };
  function sessionSocketProxy(session) {
    const p = session._proxySocket;
    if (p === null || p === undefined) return (session._proxySocket = new Proxy(session, proxySocketHandler));
    return p;
  }

  // Http2Session.setLocalWindowSize (node lib/internal/http2/core.js ->
  // nghttp2_session_set_local_window_size). Growing the window announces the
  // delta with a connection-level WINDOW_UPDATE; shrinking only lowers the
  // effective size (the peer keeps the credit it was already granted), which is
  // why state.localWindowSize can stay above state.effectiveLocalWindowSize.
  function sessionSetLocalWindowSize(session, windowSize) {
    if (session.destroyed) throw mkErr("The session has been destroyed", "ERR_HTTP2_INVALID_SESSION");
    if (typeof windowSize !== "number") throw argTypeErr("windowSize", "of type number", windowSize);
    if (!Number.isInteger(windowSize)) throw outOfRangeErr("windowSize", "an integer", windowSize);
    if (windowSize < 0 || windowSize > 2147483647) throw outOfRangeErr("windowSize", ">= 0 && <= 2147483647", windowSize);
    if (session._localWindow === undefined) session._localWindow = DEFAULT_CONNECTION_WINDOW;
    session._effectiveLocalWindow = windowSize;
    // _windowUpdate(0, delta) is what credits session._localWindow, so the
    // delta must not be added here as well.
    if (windowSize > session._localWindow) session._windowUpdate(0, windowSize - session._localWindow);
  }

  // Http2Session#ping (node lib/internal/http2/core.js): an 8-byte opaque
  // payload (random when omitted) whose callback fires on the peer's PING ACK
  // with (err, durationMs, payload) — not immediately, and not with a
  // fabricated payload. A ping issued while still connecting or after close()
  // is cancelled with ERR_HTTP2_PING_CANCEL.
  function sessionPing(session, payload, cb) {
    if (session.destroyed) throw mkErr("The session has been destroyed", "ERR_HTTP2_INVALID_SESSION");
    if (typeof payload === "function") { cb = payload; payload = undefined; }
    if (payload) {
      if (!Buffer.isBuffer(payload) && !ArrayBuffer.isView(payload)) throw argTypeErr("payload", "an instance of Buffer, TypedArray, or DataView", payload);
      if (payload.byteLength !== 8) { const e = new RangeError("HTTP2 ping payload must be 8 bytes"); e.code = "ERR_HTTP2_PING_LENGTH"; throw e; }
    }
    if (typeof cb !== "function") throw argTypeErr("callback", "of type function", cb);
    // node wraps every ping in `class Http2Ping extends AsyncResource` with the
    // type 'HTTP2PING', so async_hooks reports init/before/after/destroy for it
    // (test-http2-ping counts exactly four of each, the cancelled ping
    // included). Wrapping the callback here reproduces that lifecycle: the
    // resource is created when ping() is called and destroyed once the callback
    // has run, whether it ran with an ACK or with ERR_HTTP2_PING_CANCEL.
    if (typeof G.__mbunAsyncHookWrap === "function") cb = G.__mbunAsyncHookWrap(cb, "HTTP2PING");
    // The PING opaque data is the payload's RAW 8 bytes. `Buffer.from(view)`
    // copies a TypedArray's *elements*, so a Uint16Array([1,2,3,4]) — 8 bytes,
    // and therefore accepted by the byteLength check above — became a 4-byte
    // frame and the peer killed the connection with FRAME_SIZE_ERROR
    // (test-http2-ping). Slicing the underlying ArrayBuffer also gives the
    // echoed buffer an exactly-8-byte `.buffer`, which that test compares.
    const buf = payload
      ? Buffer.from(payload.buffer.slice(payload.byteOffset, payload.byteOffset + payload.byteLength))
      : Buffer.alloc(8);
    if (!payload) for (let i = 0; i < 8; i++) buf[i] = (Math.random() * 256) | 0;
    if (!session._pings) session._pings = [];
    // node Http2Session::AddPing refuses once maxOutstandingPings are already in
    // flight and returns false; JS then cancels the callback rather than leaving
    // it pending (lib/internal/http2/core.js `ping()`: `if (!ret) ping.error()`).
    // The same cancel-and-return-false shape covers a session that is still
    // connecting or already closed.
    const maxPings = (session._options && session._options.maxOutstandingPings) || 10;
    if (session.connecting || session.closed || session._pings.length >= maxPings) {
      G.queueMicrotask(() => cb(mkErr("HTTP2 ping cancelled", "ERR_HTTP2_PING_CANCEL")));
      return false;
    }
    session._pings.push({ key: buf.toString("hex"), cb, start: Date.now(), payload: buf });
    session._writeFrame(FRAME.PING, 0, 0, buf);
    return true;
  }
  // Http2Session#settings (node lib/internal/http2/core.js `settings()` ->
  // `submitSettings()` -> `settingsCallback()`). This was a stub that only
  // registered the callback on a 'localSettings' event nothing ever emitted, so
  // `session.settings({...}, cb)` never called back at all — the four
  // settings-round-trip corpus files hung to the 15s timeout rather than
  // failing. The real contract is:
  //   * validate the object (and reject a non-function callback),
  //   * write a SETTINGS frame and count a pending ack,
  //   * on the peer's SETTINGS ACK, adopt the values as `localSettings`, call
  //     the callback with (null, localSettings, durationMs) and emit
  //     'localSettings',
  //   * defer the whole thing to 'connect' while the session is still dialling,
  //   * destroy the session with ERR_HTTP2_MAX_PENDING_SETTINGS_ACK once more
  //     than maxOutstandingSettings acks are in flight (node's default is 10).
  // node internal/http2/util.js getSettings(): the object BOTH localSettings and
  // remoteSettings hand out is normalised — enablePush/enableConnectProtocol are
  // booleans (`!!settingsBuffer[...]`), `maxHeaderSize` is a second name for
  // maxHeaderListSize, and customSettings only appears when there are any.
  // mbun returned its raw internal record, so the server half reported
  // `enablePush: 0` and no maxHeaderSize/maxHeaderListSize at all
  // (test-http2-session-settings asserts the type of every one of those on both
  // halves). Computed on read so a SETTINGS ack that mutates the record is
  // reflected.
  // Anything outside the seven defined SETTINGS ids is a "custom" setting; the
  // filtering by `remoteCustomSettings` happens in the projection below, so the
  // wire parsers keep every one of them. `undefined` when there are none, which
  // is how the projection tells "no custom settings" from "an empty set".
  function customFromRawSettings(raw) {
    let custom;
    for (const k in raw) {
      if (!Object.prototype.hasOwnProperty.call(raw, k)) continue;
      const id = +k;
      if (id === 1 || id === 2 || id === 3 || id === 4 || id === 5 || id === 6 || id === 8) continue;
      if (custom === undefined) custom = {};
      custom[id] = raw[k];
    }
    return custom;
  }
  // `allow` is the session's `remoteCustomSettings` option: node's C++ only
  // RECORDS an inbound custom setting whose id the caller asked to be tracked,
  // so an unlisted id (155 in test-http2-session-settings) never reaches JS at
  // all, and a session with no such option reports no customSettings.
  function customSettingsView(s, allow) {
    if (s == null || s.customSettings == null || !allow || allow.length === 0) return undefined;
    const out = {};
    let n = 0;
    for (const k in s.customSettings) {
      if (!Object.prototype.hasOwnProperty.call(s.customSettings, k)) continue;
      if (allow.indexOf(+k) < 0 && allow.indexOf(String(k)) < 0) continue;
      out[k] = s.customSettings[k]; n++;
    }
    return n > 0 ? out : undefined;
  }
  function settingsView(s, allow, own) {
    if (s == null) return s;
    const listSize = s.maxHeaderListSize === undefined ? 65535 : s.maxHeaderListSize;
    const out = {
      headerTableSize: s.headerTableSize,
      enablePush: !!s.enablePush,
      initialWindowSize: s.initialWindowSize,
      maxFrameSize: s.maxFrameSize,
      maxConcurrentStreams: s.maxConcurrentStreams,
      maxHeaderListSize: listSize,
      maxHeaderSize: listSize,
      enableConnectProtocol: !!s.enableConnectProtocol,
    };
    // Our OWN settings are whatever we asked for; only the PEER's are filtered.
    const custom = own === true
      ? (s.customSettings != null ? Object.assign({}, s.customSettings) : undefined)
      : customSettingsView(s, allow);
    if (custom !== undefined) out.customSettings = custom;
    return out;
  }
  // node caches both projections on the session (kLocalSettings/kRemoteSettings)
  // and only rebuilds them when the underlying settings change, and that IDENTITY
  // is observable: test-http2-session-settings asserts
  // `stream.session.localSettings === localSettings` on a second read.
  function localSettingsOf(session) {
    if (session._localSettingsView === undefined)
      session._localSettingsView = settingsView(session._localSettings, undefined, true);
    return session._localSettingsView;
  }
  function remoteSettingsOf(session) {
    if (session._remoteSettings == null) return undefined;
    if (session._remoteSettingsView === undefined)
      session._remoteSettingsView = settingsView(session._remoteSettings, session._options && session._options.remoteCustomSettings, false);
    return session._remoteSettingsView;
  }
  function sessionPendingAck(session) { return (session._pendingSettingsAcks ? session._pendingSettingsAcks.length : 0) > 0; }
  function sessionSubmitSettings(session, settings, callback) {
    if (session.destroyed) throw mkErr("The session has been destroyed", "ERR_HTTP2_INVALID_SESSION");
    assertIsObject(settings, "settings");
    validateSettings(settings);
    if (callback !== undefined && callback !== null && typeof callback !== "function")
      throw argTypeErr("callback", "of type function", callback);
    const copy = Object.assign({}, settings);
    // A server never advertises SETTINGS_ENABLE_PUSH != 0 (RFC 9113 6.5.2);
    // mid-connection updates are clamped the same way the initial frame is.
    if (session._isServerSession && copy.enablePush !== undefined) copy.enablePush = false;
    // nghttp2 updates `pending_enable_connect_protocol` inside submit_settings,
    // i.e. before the peer has acked, and validates inbound `:protocol` against
    // it. A server that turns extended CONNECT back off therefore stops
    // accepting the header immediately (the peer, meanwhile, kills the
    // connection over the illegal withdrawal — see connectProtocolWithdrawn).
    if (copy.enableConnectProtocol !== undefined) session._localConnectProtocol = !!copy.enableConnectProtocol;
    const send = () => {
      if (session.destroyed) return;
      if (!session._pendingSettingsAcks) session._pendingSettingsAcks = [];
      const max = (session._options && session._options.maxOutstandingSettings) || 10;
      if (session._pendingSettingsAcks.length >= max) {
        session.destroy(mkErr("Maximum number of pending settings acknowledgements", "ERR_HTTP2_MAX_PENDING_SETTINGS_ACK"));
        return;
      }
      session._pendingSettingsAcks.push({ settings: copy, cb: callback, start: Date.now() });
      session._writeFrame(FRAME.SETTINGS, 0, 0, encodeSettings(copy));
    };
    if (session.connecting) session.once("connect", send);
    else send();
    return session;
  }
  // The peer acked our SETTINGS: the values we asked for are now the ones in
  // effect locally (node reads them back out of nghttp2; here we simply adopt
  // what we sent, which is the same set).
  function resolveSettingsAck(session) {
    const q = session._pendingSettingsAcks;
    if (!q || !q.length) return;
    const p = q.shift();
    const eff = session._localSettings;
    for (const k in p.settings) {
      if (!Object.prototype.hasOwnProperty.call(p.settings, k)) continue;
      if (k === "customSettings") continue;
      const v = p.settings[k];
      eff[k] = typeof v === "boolean" ? (k === "enablePush" || k === "enableConnectProtocol" ? v : (v ? 1 : 0)) : v;
    }
    session._localSettingsView = undefined;
    const snapshot = Object.assign({}, eff);
    if (typeof p.cb === "function") { try { p.cb(null, snapshot, Date.now() - p.start); } catch (e) {} }
    session.emit("localSettings", snapshot);
  }
  // Returns false when the ACK matched no outstanding ping: an UNSOLICITED
  // PING ACK, which nghttp2 treats as a connection protocol error (it is a
  // cheap flood vector -- see test-http2-ping-unsolicited-ack).
  function resolvePing(session, payload) {
    const pings = session._pings;
    if (!pings || !pings.length) return false;
    const key = Buffer.from(payload).toString("hex");
    let idx = pings.findIndex((p) => p.key === key);
    if (idx < 0) idx = 0;   // a peer that echoes a different payload still acks
    const p = pings.splice(idx, 1)[0];
    try { p.cb(null, Date.now() - p.start, p.payload); } catch (e) {}
    return true;
  }

  // Http2Session#altsvc / #origin (node lib/internal/http2/core.js). Shared by
  // both session halves: the wire format and the argument validation are
  // identical, only the direction of travel differs (a server sends them, a
  // client surfaces them as 'altsvc' / 'origin').
  //
  // node caps both frames at kMaxALTSVC = 16382 (the largest payload that fits
  // a default max frame size once the 2-byte Origin-Len is accounted for) and
  // restricts `alt` to RFC 7230 quoted-string characters.
  const kMaxALTSVC = 16382;
  // node lib/internal/http2/core.js kQuotedString. HTAB and SP are legal and so
  // is the double quote (0x22) — the RFC 7230 quoted-string production this
  // guards is the *field value* `h2=":8000"`, quotes included. Starting the
  // class at \x21 / \x23 rejected every real Alt-Svc value node accepts.
  const kQuotedString = /^[\x09\x20-\x5b\x5d-\x7e\x80-\xff]*$/;
  function getURLOrigin(url) {
    // node uses internal/url getURLOrigin: parse and read `origin`, which is
    // the string "null" for a non-special scheme (abc:, foo://bar, ...).
    let u;
    try { u = new G.URL(url); } catch (e) { const err = new TypeError("Invalid URL"); err.code = "ERR_INVALID_URL"; err.input = url; throw err; }
    return u.origin;
  }
  function altsvcInvalidOrigin() {
    const e = new TypeError("HTTP/2 ALTSVC frames require a valid origin");
    e.code = "ERR_HTTP2_ALTSVC_INVALID_ORIGIN";
    return e;
  }
  function sessionAltsvc(session, alt, originOrStream) {
    if (session.destroyed) throw mkErr("The session has been destroyed", "ERR_HTTP2_INVALID_SESSION");
    let stream = 0;
    let origin;
    if (typeof originOrStream === "string") {
      origin = getURLOrigin(originOrStream);
      if (origin === "null") throw altsvcInvalidOrigin();
    } else if (typeof originOrStream === "number") {
      if (originOrStream >>> 0 !== originOrStream || originOrStream === 0)
        throw outOfRangeErr("originOrStream", "> 0 && < 4294967296", originOrStream);
      stream = originOrStream;
    } else if (originOrStream !== undefined) {
      if (originOrStream !== null && typeof originOrStream === "object") origin = originOrStream.origin;
      if (typeof origin !== "string")
        throw argTypeErr("originOrStream", "one of type string, number, URL, or object", originOrStream);
      else if (origin === "null" || origin.length === 0) throw altsvcInvalidOrigin();
    }
    validateString(alt, "alt");
    if (!kQuotedString.test(alt)) { const e = new TypeError("Invalid character in alt"); e.code = "ERR_INVALID_CHAR"; throw e; }
    if (alt.length + (origin !== undefined ? origin.length : 0) > kMaxALTSVC) {
      const e = new TypeError("HTTP/2 ALTSVC frames are limited to " + kMaxALTSVC + " bytes");
      e.code = "ERR_HTTP2_ALTSVC_LENGTH";
      throw e;
    }
    // nghttp2_submit_altsvc() looks the stream up and returns
    // NGHTTP2_ERR_INVALID_ARGUMENT when it does not exist, so node's "won't
    // error, but won't send anything because the stream does not exist" case
    // puts nothing on the wire. Emitting it anyway gave the peer a fifth
    // 'altsvc' event (test-http2-altsvc expects exactly four).
    if (stream !== 0 && !(session.streams && session.streams.get(stream))) return;
    // RFC 7838 4: [Origin-Len(16)][Origin][Alt-Svc-Field-Value]. On a stream the
    // origin is implied by the stream, so Origin-Len is 0.
    const originBuf = Buffer.from(stream !== 0 ? "" : (origin || ""), "latin1");
    const altBuf = Buffer.from(alt, "latin1");
    const payload = Buffer.alloc(2 + originBuf.length + altBuf.length);
    payload.writeUInt16BE(originBuf.length, 0);
    originBuf.copy(payload, 2);
    altBuf.copy(payload, 2 + originBuf.length);
    session._writeFrame(FRAME.ALTSVC, 0, stream, payload);
  }
  function sessionOrigin(session, origins) {
    if (session.destroyed) throw mkErr("The session has been destroyed", "ERR_HTTP2_INVALID_SESSION");
    if (origins.length === 0) return;
    const list = [];
    let len = 0;
    for (let i = 0; i < origins.length; i++) {
      let origin = origins[i];
      if (typeof origin === "string") origin = getURLOrigin(origin);
      else if (origin != null && typeof origin === "object") origin = origin.origin;
      validateString(origin, "origin");
      if (origin === "null") { const e = new TypeError("HTTP/2 ORIGIN frames require a valid origin"); e.code = "ERR_HTTP2_INVALID_ORIGIN"; throw e; }
      list.push(origin);
      len += origin.length;
    }
    if (len > kMaxALTSVC) { const e = new TypeError("HTTP/2 ORIGIN frames are limited to " + kMaxALTSVC + " bytes"); e.code = "ERR_HTTP2_ORIGIN_LENGTH"; throw e; }
    // RFC 8336 2.1: a sequence of [Origin-Len(16)][ASCII-Origin] entries.
    const parts = [];
    for (const o of list) {
      const b = Buffer.from(o, "latin1");
      const h = Buffer.alloc(2); h.writeUInt16BE(b.length, 0);
      parts.push(h, b);
    }
    session._writeFrame(FRAME.ORIGIN, 0, 0, Buffer.concat(parts));
  }
  // Inbound ALTSVC / ORIGIN, shared by both session halves.
  function handleAltsvcFrame(session, streamId, payload) {
    if (payload.length < 2) return true;   // malformed: ignore (RFC 7838 4)
    const originLen = payload.readUInt16BE(0);
    if (2 + originLen > payload.length) return true;
    const origin = payload.toString("latin1", 2, 2 + originLen);
    const alt = payload.toString("latin1", 2 + originLen);
    session.emit("altsvc", alt, origin, streamId);
    return true;
  }
  // node internal/http2/core.js initOriginSet: the origin set of an encrypted
  // session is SEEDED from the transport before any ORIGIN frame arrives — the
  // peer's own origin is always a member. servername wins; an IP peer falls
  // back to remoteAddress, bracketed when the family is IPv6, and the whole
  // thing is normalised through URL's origin serialisation.
  function initOriginSet(session) {
    if (session._originSet === undefined) {
      session._originSet = new Set();
      const socket = session._rawSocket || {};
      let hostName = socket.servername;
      if (hostName === null || hostName === undefined || hostName === false) {
        // node reads socket.remoteAddress / remoteFamily here. Both name the
        // peer the session dialled, which is exactly the recorded authority —
        // and this transport does not carry a real peer address (net.Socket
        // reports a fixed 127.0.0.1 / IPv4 for every connection, so an IPv6
        // literal authority came back as the v4 loopback). Prefer the
        // authority; fall back to the socket when there is none.
        hostName = session._originHost !== undefined ? session._originHost : socket.remoteAddress;
        if (typeof hostName === "string" && hostName.includes(":")) hostName = "[" + hostName + "]";
      }
      if (hostName !== undefined && hostName !== null) {
        let originString = "https://" + hostName;
        const port = session._originPort !== undefined ? session._originPort : socket.remotePort;
        if (port != null) originString += ":" + port;
        try { session._originSet.add(new G.URL(originString).origin); }
        catch (e) { session._originSet.add(originString); }
      }
    }
    return session._originSet;
  }
  function handleOriginFrame(session, payload) {
    const origins = [];
    let off = 0;
    while (off + 2 <= payload.length) {
      const n = payload.readUInt16BE(off);
      if (off + 2 + n > payload.length) break;
      origins.push(payload.toString("latin1", off + 2, off + 2 + n));
      off += 2 + n;
    }
    // node onOrigin() bails out entirely on a cleartext session:
    //   `if (!session.encrypted || session.destroyed) return undefined;`
    // — the origin set is not tracked AND the 'origin' event is not emitted. A
    // plaintext server may still SEND the frame (Http2Session#origin() does not
    // check), and the last block of test-http2-origin pins exactly that
    // asymmetry with `client.on('origin', mustNotCall())`. mbun tracked the set
    // conditionally but emitted unconditionally.
    if (!session.encrypted || session.destroyed) return true;
    const set = initOriginSet(session);
    for (const o of origins) set.add(o);
    session.emit("origin", origins);
    return true;
  }

  // Http2Stream#priority (node lib/internal/http2/core.js). Priority signalling
  // was deprecated by RFC 9113 and nghttp2 stopped honouring it in 1.65, so
  // node's method is a `deprecate()` wrapper whose body only rejects a destroyed
  // stream — it does not emit a PRIORITY frame and the peer never fires
  // 'priority'. Reproduce that exactly, warning included (DEP0194), because the
  // corpus asserts both the warning and that 'priority' is NOT emitted.
  // node setAndValidatePriorityOptions() opens with `deprecateWeight(options)`,
  // a deprecateProperty() closure that fires ONCE (process-wide) the first time
  // a request/pushStream options bag carries a `weight` key at all — `'weight'
  // in options` is the test, so an explicit `weight: undefined` still warns.
  let weightWarned = false;
  function deprecateWeight(options) {
    if (weightWarned || !options || typeof options !== "object" || !("weight" in options)) return;
    weightWarned = true;
    try {
      G.process.emitWarning("Priority signaling has been deprecated as of RFC 9113.",
        "DeprecationWarning", "DEP0194");
    } catch (e) {}
  }
  let priorityWarned = false;
  function streamPriority(stream) {
    if (!priorityWarned) {
      priorityWarned = true;
      try {
        G.process.emitWarning(
          "http2Stream.priority is longer supported after priority signalling was deprecated in RFC 9113",
          "DeprecationWarning", "DEP0194");
      } catch (e) {}
    }
    if (stream.destroyed) throw mkErr("The stream has been destroyed", "ERR_HTTP2_INVALID_STREAM");
  }

  // Http2StreamState (node docs `http2stream.state`). `state` is nghttp2's
  // stream state enum; 1 = NGHTTP2_STREAM_STATE_OPEN, 7 = ..._CLOSED.
  function streamState(st) {
    const closed = st.destroyed || st.closed || st._ended === true;
    return {
      localWindowSize: DEFAULT_CONNECTION_WINDOW,
      state: closed ? 7 : 1,
      localClose: st._endStreamSent === true ? 1 : 0,
      remoteClose: st._readEnded === true ? 1 : 0,
      sumDependencyWeight: 0,
      weight: 16,
    };
  }

  // ==========================================================================
  // === Http2Stream: a real stream.Duplex ====================================
  //
  // node's Http2Stream *is* a Duplex (lib/internal/http2/core.js
  // `class Http2Stream extends Duplex`), and the corpus asserts that directly:
  // `stream.pipe(...)`, `stream.bufferSize`, `req.pause()` actually holding
  // bytes back, `res.writableLength` / `writableNeedDrain` / `writableCorked`,
  // `readableEnded` / `writableFinished`, and `util.inspect(stream)` printing
  // `Http2Stream { … readableState: … writableState: … }`. Both halves used to
  // be bare EventEmitters that re-emitted 'data' synchronously out of the frame
  // parser, so every one of those assertions failed for the same single reason:
  // the stream layer was missing, not wrong.
  //
  // Everything below is the glue between the frame parser and the Duplex:
  // inbound DATA goes through `push()` (so pause/resume, setEncoding, pipe and
  // the readable-side counters all come from stream itself), and the writable
  // side hands chunks to `session._sendData` from `_write`/`_final`.
  const streamMod = M["stream"] || M["node:stream"];
  const H2StreamBase = (streamMod && typeof streamMod.Duplex === "function") ? streamMod.Duplex : EE;
  const kHaveDuplex = H2StreamBase !== EE;
  // node Http2Stream constructor: allowHalfOpen (a half-closed HTTP/2 stream is
  // normal), decodeStrings off (DATA frames carry bytes), autoDestroy off (the
  // session decides when a stream is finished).
  const kStreamDuplexOptions = { allowHalfOpen: true, decodeStrings: false, autoDestroy: false };
  function initHttp2Stream(stream, session, id) {
    stream.session = session;
    stream.id = id;
    // Only the client session maintains this (see ClientHttp2Session#_maybeDestroy).
    if (session && typeof session._liveStreams === "number") { session._liveStreams++; stream._counted = true; }
    stream._endStreamSent = false;
    stream._closed = false;
    stream._finished = false;
    stream.rstCode = constants.NGHTTP2_NO_ERROR;
    stream.aborted = false;
    stream._writeQueueSize = 0;
    stream.sentInfoHeaders = [];
    stream.sentTrailers = undefined;
    if (!kHaveDuplex) { stream.readable = true; stream.writable = true; stream.destroyed = false; }
  }
  const bufFromChunk = (chunk, enc) => (typeof chunk === "string" ? Buffer.from(chunk, enc || "utf8") : Buffer.from(chunk));
  // The frame goes out synchronously (wire order must not change) but the write
  // callback is deferred a microtask: node's writes land in the handle's queue,
  // so `writableLength` / `bufferSize` report bytes still in flight, and a
  // synchronous `cb()` made both of them read 0 always.
  function http2StreamWrite(stream, chunk, enc, cb) {
    if (stream._endStreamSent) { G.queueMicrotask(cb); return; }
    const buf = bufFromChunk(chunk, enc);
    stream.session._sendData(stream, buf, false);
    // node Http2Stream[kWriteGeneric]: the client publishes every outbound body
    // chunk AFTER it has been handed to the transport.
    if (stream._serverSide !== true && onClientStreamBodyChunkSentChannel.hasSubscribers) {
      onClientStreamBodyChunkSentChannel.publish({ stream, writev: false, data: chunk, encoding: enc });
    }
    G.queueMicrotask(cb);
  }
  function http2StreamWritev(stream, chunks, cb) {
    if (stream._endStreamSent) { G.queueMicrotask(cb); return; }
    const parts = [];
    for (let i = 0; i < chunks.length; i++) parts.push(bufFromChunk(chunks[i].chunk, chunks[i].encoding));
    stream.session._sendData(stream, Buffer.concat(parts), false);
    // node routes the writev batch through writevGeneric, which — when every
    // entry is already a Buffer — REPLACES each `{chunk, encoding}` entry with
    // its bare chunk in place before the publish runs. A mixed batch keeps the
    // entry objects. The published `data` has to have that same shape, and
    // `encoding` is the empty string node's _writev passes through.
    if (stream._serverSide !== true && onClientStreamBodyChunkSentChannel.hasSubscribers) {
      let data = chunks;
      if (chunks.allBuffers) { data = []; for (let i = 0; i < chunks.length; i++) data.push(chunks[i].chunk); }
      onClientStreamBodyChunkSentChannel.publish({ stream, writev: true, data, encoding: "" });
    }
    G.queueMicrotask(cb);
  }
  // The writable side finished. Without waitForTrailers that is an empty
  // DATA(END_STREAM); with it, END_STREAM is held back until sendTrailers()
  // (node kWaitForTrailers -> 'wantTrailers' -> sendTrailers()).
  function http2StreamFinal(stream, waitForTrailers, cb) {
    // node Http2Stream#_final publishes bodySent on every path it takes (the
    // writable side is done regardless of whether END_STREAM rode on HEADERS).
    const bodySent = () => {
      if (stream._serverSide !== true && onClientStreamBodySentChannel.hasSubscribers) {
        onClientStreamBodySentChannel.publish({ stream });
      }
    };
    if (stream._endStreamSent) { cb(); bodySent(); maybeFinishHttp2Stream(stream); return; }
    if (waitForTrailers) {
      stream._trailersReady = true;
      // node onStreamTrailers(): `if (!stream.emit('wantTrailers')) { // There
      // are no listeners, send empty trailing HEADERS frame and close.
      // stream.sendTrailers({}); }`. Without that fallback a
      // respond(_, { waitForTrailers: true }) whose application never installs
      // a handler never sends END_STREAM, and BOTH peers wait forever.
      if (!stream.emit("wantTrailers")) { try { stream.sendTrailers({}); } catch (e) {} }
      cb();
      bodySent();
      return;
    }
    stream.session._sendData(stream, Buffer.alloc(0), true);
    stream._endStreamSent = true;
    cb();
    bodySent();
    maybeFinishHttp2Stream(stream);
  }
  function http2StreamRead(stream) {
    // Flow control is driven by the peer's DATA frames; there is nothing to pull.
    // Resuming after a pause replays whatever the parser buffered.
    const q = stream._readBacklog;
    if (q && q.length) {
      while (q.length) { if (!stream.push(q.shift())) break; }
      if (!q.length && stream._readEofPending) { stream._readEofPending = false; http2StreamEndReadable(stream); }
    }
  }
  function http2StreamPushData(stream, bytes) {
    if (stream.destroyed) return;
    const buf = Buffer.from(bytes);
    if (!kHaveDuplex) { stream.emit("data", buf); return; }
    // push() returning false only means "over the high-water mark"; a *paused*
    // stream must not receive further pushes at all or the bytes escape the
    // pause. Keep them in a per-stream backlog that _read() drains.
    if (stream._readBacklog && stream._readBacklog.length) { stream._readBacklog.push(buf); return; }
    if (!stream.push(buf)) { if (!stream._readBacklog) stream._readBacklog = []; }
  }
  // A trailer HEADERS block, delivered in stream order with the DATA before it.
  //
  // node stops reading the socket for as long as a stream's readable side is
  // paused (streamOnPause -> handle.readStop()), so a trailer block that shares
  // a read batch with still-undelivered DATA is not even parsed until the
  // consumer has drained that DATA. mbun parses the whole batch in one go, so
  // emitting 'trailers' at parse time overtakes bytes the consumer has not seen.
  // A consumer that reads "trailers" as "the call is over" then throws those
  // bytes away: grpc-js pauses after every message and outputs the final status
  // straight from the trailers, dropping every message still buffered behind
  // them (grpc-js test-server-errors "should emit data for all messages before
  // error" saw 1 of 2 messages).
  //
  // Hold the block for one I/O turn instead, which is the smallest delay that
  // outlasts a consumer built on process.nextTick — a Readable hands the last
  // buffered chunk over in a nextTick chain, so anything scheduled with
  // nextTick here still overtakes it. END_STREAM is held with the trailers so
  // the order the consumer sees stays node's: data... < trailers < end.
  function http2StreamEmitTrailers(stream, headersObj, flags, rawHeaders, endStream) {
    const buffered = kHaveDuplex && !stream.destroyed && !stream._readEnded &&
      (((stream._readBacklog && stream._readBacklog.length) > 0) || stream.readableLength > 0);
    if (!endStream || !buffered) { stream.emit("trailers", headersObj, flags, rawHeaders); return; }
    stream._trailersHeld = true;
    const nextTurn = typeof G.setImmediate === "function" ? G.setImmediate : (fn) => G.setTimeout(fn, 0);
    nextTurn(() => {
      stream._trailersHeld = false;
      if (stream.destroyed) return;
      stream.emit("trailers", headersObj, flags, rawHeaders);
      if (stream._eofHeld) { stream._eofHeld = false; http2StreamEndReadable(stream); }
    });
  }
  // The peer sent END_STREAM: EOF the readable side.
  //
  // Pushing null is not enough. node's `onStreamClose` (lib/internal/http2/
  // core.js) additionally *pokes* the readable side, and skipping that is the
  // difference between a clean exit and a 15-second hang: a Duplex only runs its
  // end-of-stream check from `read()`, so a stream nobody ever read would sit
  // with `ended` set and never emit 'end'. node calls `read(0)` to force the
  // check — and on a server session that was never read at all it `resume()`s
  // instead, dumping the request body so the stream can be destroyed. Destroy is
  // deferred until 'end' has actually fired ("Defer destroy we actually emit
  // end", same function).
  function http2StreamEndReadable(stream) {
    if (stream._readEnded) return;
    // Trailers held for the drain turn above carry this stream's END_STREAM;
    // releasing EOF first would put 'end' in front of them.
    if (stream._trailersHeld) { stream._eofHeld = true; return; }
    if (kHaveDuplex && stream._readBacklog && stream._readBacklog.length) { stream._readEofPending = true; return; }
    stream._readEnded = true;
    if (!kHaveDuplex) { stream.readable = false; stream.emit("end"); maybeFinishHttp2Stream(stream); return; }
    stream.once("end", () => maybeFinishHttp2Stream(stream));
    stream.push(null);
    if (stream.destroyed) return;
    if (stream._serverSide === true && stream._didRead !== true && stream.readableFlowing === null) stream.resume();
    else stream.read(0);
  }
  function http2StreamClose(stream, code, cb) {
    if (code === undefined) code = constants.NGHTTP2_NO_ERROR;
    validateUint32(code, "code");
    if (cb !== undefined && typeof cb !== "function") throw argTypeErr("callback", "of type function", cb);
    if (typeof cb === "function") stream.once("close", cb);
    if (stream._closed) return;
    stream._closed = true;
    stream.rstCode = code;
    const finish = () => {
      try { stream.session._rstStream(stream, code); } catch (e) {}
      try { stream.session.streams.delete(stream.id); } catch (e) {}
      // node Http2Stream#_destroy: "RST code 8 not emitted as an error as it is
      // used by clients to signify abort and is already covered by the 'aborted'
      // event" -- the guard is
      // `code !== NGHTTP2_NO_ERROR && code !== NGHTTP2_CANCEL`
      // (lib/internal/http2/core.js:2486). mbun tested only NO_ERROR, so a
      // deliberate CANCEL surfaced as an uncaught 'Stream closed with error
      // code NGHTTP2_CANCEL'.
      if (code !== constants.NGHTTP2_NO_ERROR && code !== constants.NGHTTP2_CANCEL) {
        const err = streamErr(code);
        G.queueMicrotask(() => { if (!stream.destroyed) stream.destroy(err); else stream.emit("error", err); });
        return;
      }
      http2StreamFinish(stream);
    };
    // node closeStream(): RST_STREAM waits for the writable side to finish, so
    // DATA the application already handed to the stream is not thrown away.
    // `stream.write(a); stream.write(b); stream.end(); stream.close();` must put
    // both chunks on the wire — with the Duplex, b is still buffered when
    // close() runs.
    // Only when there is something to flush: a close() before any write at all
    // (a server resetting a stream instead of responding) must reset *now*.
    if (kHaveDuplex && !stream.destroyed && !stream.writableFinished &&
        (stream.writableLength > 0 || stream.writableCorked > 0)) {
      stream.once("finish", finish);
      stream.end();
    } else finish();
  }
  // node Http2Stream#_destroy: RST_STREAM the peer if the stream had not
  // finished, then let Duplex emit 'error'/'close' in the right order.
  function http2StreamDestroy(stream, err, cb) {
    if (!stream._closed) {
      stream._closed = true;
      // node closeStream(): `if (!ending) { if (!stream.aborted) { flags |=
      // ABORTED; stream.emit('aborted'); } stream.end(); }` — destroying a
      // stream whose WRITABLE side is still open IS the abort, whatever the
      // reason. Only the incoming-RST path reported it, so a peer that
      // destroyed its own stream mid-response never told its compat
      // Http2ServerRequest and `req.on('aborted')` never fired
      // (test-http2-compat-errors).
      const wOpen = !(stream.writableEnded === true ||
                      (stream._writableState && stream._writableState.ending === true));
      if (!stream.aborted && wOpen) { stream.aborted = true; try { stream.emit("aborted"); } catch (e) {} }
      // node Http2Stream#_destroy: "Enables using AbortController to cancel
      // requests with RST code 8" — an AbortError resets with CANCEL, any other
      // error with INTERNAL_ERROR. mbun has no AbortError class, so the name is
      // the discriminator.
      stream.rstCode = err
        ? (err.name === "AbortError" ? constants.NGHTTP2_CANCEL : constants.NGHTTP2_INTERNAL_ERROR)
        : (stream.rstCode || constants.NGHTTP2_NO_ERROR);
      try { stream.session._rstStream(stream, stream.rstCode); } catch (e) {}
    }
    dcPublishStreamClose(stream);
    if (err) {
      const ch = stream._serverSide === true ? onServerStreamErrorChannel : onClientStreamErrorChannel;
      if (ch.hasSubscribers) ch.publish({ stream, error: err });
    }
    const sess = stream.session;
    // A request destroyed before the session's handshake completed must not be
    // submitted at all; drop whatever it queued (its HEADERS, and the RST_STREAM
    // _rstStream just added above). See ClientHttp2Session#_writeFrame.
    if (sess && sess._connected !== true && Array.isArray(sess._preConnectQ) && sess._preConnectQ.length > 0 && stream.id > 0) {
      const id = stream.id;
      sess._preConnectQ = sess._preConnectQ.filter((e) => e.streamId !== id);
    }
    try { sess.streams.delete(stream.id); } catch (e) {}
    if (stream._counted === true) {
      stream._counted = false;
      if (sess && typeof sess._liveStreams === "number" && sess._liveStreams > 0) sess._liveStreams--;
    }
    G.queueMicrotask(() => {
      if (!sess) return;
      if (sess._endPending) sess._onSocketEnd();
      // The last live stream releases a graceful close() that was waiting on it.
      if (sess._destroyPending && typeof sess._maybeDestroy === "function") sess._maybeDestroy();
    });
    cb(err || null);
  }
  // Both directions are done: destroy so 'close' fires exactly once, the way
  // node's kMaybeDestroy does. `readableEnded` (not `_readEnded`) is the gate —
  // it only turns true once 'end' has been emitted, i.e. once the consumer has
  // actually seen the body.
  function maybeFinishHttp2Stream(stream) {
    if (!kHaveDuplex || stream.destroyed) return;
    if (!stream._endStreamSent) return;
    if (!stream.readableEnded) return;
    // Both halves are done — nghttp2's onStreamClose point. node marks the
    // stream closed (rstCode NO_ERROR, no RST_STREAM on the wire) and publishes
    // the close channel *before* deferring the destroy, so a subscriber sees
    // `closed === true, destroyed === false`.
    stream._closed = true;
    dcPublishStreamClose(stream);
    G.queueMicrotask(() => { if (!stream.destroyed) stream.destroy(); });
  }
  function http2StreamFinish(stream) {
    if (stream._finished) return;
    stream._finished = true;
    stream._closed = true;
    // node's onStreamClose runs while the stream is closed but not yet
    // destroyed, and the corpus asserts exactly that (`closed === true`,
    // `destroyed === false`) inside the close-channel subscriber.
    dcPublishStreamClose(stream);
    if (kHaveDuplex) {
      G.queueMicrotask(() => { if (!stream.destroyed) stream.destroy(); });
      return;
    }
    stream.readable = false; stream.writable = false;
    const sess = stream.session;
    G.queueMicrotask(() => { stream.emit("close"); if (sess && sess._endPending) sess._onSocketEnd(); });
  }
  // A graceful shutdown ends the write side (flushing whatever is queued and
  // sending FIN); an error shutdown destroys the fd right away.
  // A transport EOF ends the session (RFC 9113 5.4.1): no further frames can
  // arrive, so every stream still waiting for one is CANCELLED. node does the
  // same thing on its socket's 'close'
  // (socketOnClose: state.streams.forEach(s => s.close(NGHTTP2_CANCEL))), and its
  // native session turns the read EOF into that close. mbun's http2 socket is
  // allowHalfOpen — the FIN is all it ever gets — and it used to PARK on
  // `_endPending` until the streams finished by themselves. They never did when
  // they were the ones waiting for the peer, so the socket stayed ref'd and the
  // file hung instead of reporting the abort (test-http2-compat-aborted asserts
  // request.on('aborted') fires with `complete === true`).
  function abortStreamsOnTransportEof(session) {
    if (!session.streams || session.streams.size === 0) return;
    for (const stream of Array.from(session.streams.values())) {
      if (stream.destroyed || stream._closed) continue;
      stream.rstCode = constants.NGHTTP2_CANCEL;
      if (!stream.aborted && !stream.readableEnded) {
        stream.aborted = true;
        try { stream.emit("aborted"); } catch (e) {}
      }
    }
  }

  function closeSessionSocket(socket, hard) {
    if (!socket) return;
    try {
      if (!hard && socket.writable && !socket.destroyed && typeof socket.end === "function") socket.end();
      else socket.destroy();
    } catch (e) { try { socket.destroy(); } catch (e2) {} }
  }
  // node internal/stream_base_commons setStreamTimeout: validateNumber(msecs)
  // through getTimerDuration, then an UNREF'd timer that emits 'timeout'. The
  // old body accepted anything and never fired.
  function http2StreamSetTimeout(stream, ms, cb) {
    if (stream.destroyed) return stream;
    if (typeof ms !== "number") throw argTypeErr("msecs", "of type number", ms);
    if (ms < 0 || !Number.isFinite(ms)) throw outOfRangeErr("msecs", "a non-negative finite number", ms);
    stream.timeout = ms;
    if (stream._h2Timeout != null) { try { G.clearTimeout(stream._h2Timeout); } catch (e) {} stream._h2Timeout = null; }
    if (ms === 0) {
      if (cb !== undefined) {
        if (typeof cb !== "function") throw argTypeErr("callback", "of type function", cb);
        stream.removeListener("timeout", cb);
      }
      return stream;
    }
    // Order matters and is node's: the timer is ARMED BEFORE the callback is
    // validated, so `setTimeout(100, {})` still leaves a live timer behind (and
    // any listener an earlier setTimeout registered still fires).
    const t = G.setTimeout(() => { stream._h2Timeout = null; if (!stream.destroyed) stream.emit("timeout"); }, ms);
    if (t && typeof t.unref === "function") t.unref();
    stream._h2Timeout = t;
    if (cb !== undefined) {
      if (typeof cb !== "function") throw argTypeErr("callback", "of type function", cb);
      stream.once("timeout", cb);
    }
    return stream;
  }
  // node Http2Stream#bufferSize = the bytes still queued for the wire (the
  // handle's write queue plus the writable buffer).
  function http2StreamBufferSize(stream) {
    return stream._writeQueueSize + (kHaveDuplex ? stream.writableLength : 0);
  }
  // node Http2Stream[kInspect] / Http2Session[kInspect]. The shape is asserted
  // directly (test-http2-stream-client matches /Http2Stream {/, / {2}state:/,
  // / {2}readableState:/, / {2}writableState:/), and the default inspector shows
  // none of those because they are prototype getters.
  const kCustomInspect = G.Symbol.for("nodejs.util.inspect.custom");
  function installStreamInspect(cls) {
    Object.defineProperty(cls.prototype, kCustomInspect, {
      value: function (depth, opts) {
        if (typeof depth === "number" && depth < 0) return this;
        const util = M["util"] || M["node:util"];
        const obj = {
          id: this.id || "<pending>",
          closed: this.closed,
          destroyed: this.destroyed,
          state: this.state,
          readableState: this._readableState,
          writableState: this._writableState,
        };
        if (!util || !util.inspect) return "Http2Stream " + String(obj);
        // node's Http2Stream inspect is `${name} ${inspect(obj)}` and the
        // resulting object is always far past breakLength, so node's generic
        // layout breaks it one key per line — which is what the corpus asserts
        // (`/ {2}state:/`, `/ {2}readableState:/`, `/ {2}writableState:/` in
        // test-http2-stream-client). mbun's util.inspect has no line-breaking
        // layout at all (see inspectURLSearchParamsEntries: only the maplike
        // renderer honours breakLength), so the whole record came out on one
        // line and the three indentation assertions could never match. Emit
        // node's block form here rather than teaching the generic inspector to
        // wrap: that is a corpus-wide output change, and this is the only file
        // that pins an Http2Stream's rendering.
        const parts = [];
        for (const k of Object.keys(obj)) parts.push("  " + k + ": " + util.inspect(obj[k]));
        return "Http2Stream {\n" + parts.join(",\n") + "\n}";
      },
      writable: true, configurable: true, enumerable: false,
    });
  }
  function installSessionInspect(cls, type) {
    Object.defineProperty(cls.prototype, kCustomInspect, {
      value: function (depth, opts) {
        if (typeof depth === "number" && depth < 0) return this;
        const util = M["util"] || M["node:util"];
        const obj = {
          type: this.type,
          closed: this.closed,
          destroyed: this.destroyed,
          state: this.state,
          localSettings: this.localSettings,
          remoteSettings: this.remoteSettings,
        };
        return "Http2Session " + (util && util.inspect ? util.inspect(obj) : String(obj));
      },
      writable: true, configurable: true, enumerable: false,
    });
  }

  // === ClientHttp2Stream ===
  class ClientHttp2Stream extends H2StreamBase {
    constructor(session, id, headers, options) {
      super(kStreamDuplexOptions);
      initHttp2Stream(this, session, id);
      this._reqHeaders = headers;
      this._options = options || {};
      this.pending = true;
      this.sentHeaders = headers;
      this._responseEmitted = false;
      // node ClientHttp2Stream: `endAfterHeaders` is false until the peer's
      // response HEADERS arrive carrying END_STREAM.
      this.endAfterHeaders = false;
      // node's Http2Stream constructor corks the stream ("ensures that those are
      // buffered until the handle has been assigned") and uncorks in kInit. The
      // handle only exists once the session has connected, so every write issued
      // between http2.connect() and the TCP handshake lands as ONE _writev batch
      // instead of a series of _write calls — which is exactly what the
      // bodyChunkSent diagnostics channel reports (writev: true, data: [...]).
      if (kHaveDuplex && session && session._connected !== true) {
        this.cork();
        const self = this;
        session._whenConnected(() => { try { self.uncork(); } catch (e) {} });
      }
    }
    get closed() { return this._closed; }
    get state() { return streamState(this); }
    get bufferSize() { return http2StreamBufferSize(this); }
    get pending() { return this._pending; }
    set pending(v) { this._pending = v; }
    // node Http2Stream#_write / #_final: the writable side hands the DATA
    // frames to the session, and _final closes the stream (or hands control to
    // the trailer dance when request({ waitForTrailers }) was used).
    _write(chunk, enc, cb) { http2StreamWrite(this, chunk, enc, cb); }
    _writev(chunks, cb) { http2StreamWritev(this, chunks, cb); }
    _final(cb) { http2StreamFinal(this, this._options.waitForTrailers, cb); }
    _read() { this._didRead = true; http2StreamRead(this); }
    _destroy(err, cb) { http2StreamDestroy(this, err, cb); }
    sendTrailers(headers) {
      // node lib/internal/http2/core.js sendTrailers() checks
      // `destroyed || closed` FIRST; only then the already-sent / not-ready
      // states. mbun had it the other way round, so sendTrailers() on a stream
      // that had already closed reported ERR_HTTP2_TRAILERS_ALREADY_SENT where
      // node reports ERR_HTTP2_INVALID_STREAM.
      // `destroyed` only, NOT `_closed`: mbun's sendTrailers finishes the stream
      // synchronously (http2StreamFinish sets _closed), while node's `closed`
      // only turns true when nghttp2 reports the close. Including it here would
      // make the very next sendTrailers() in the same 'wantTrailers' handler
      // report INVALID_STREAM where node reports TRAILERS_ALREADY_SENT.
      if (this.destroyed) throw mkErr("The stream has been destroyed", "ERR_HTTP2_INVALID_STREAM");
      if (this._trailersSent) throw mkErr("Trailers have already been sent", "ERR_HTTP2_TRAILERS_ALREADY_SENT");
      if (!this._trailersReady) throw mkErr("Trailers are not ready to send", "ERR_HTTP2_TRAILERS_NOT_READY");
      headers = headers || {};
      for (const k of Object.keys(headers))
        if (String(k)[0] === ":") throw mkErr('"' + k + '" is an invalid pseudoheader or is used incorrectly', "ERR_HTTP2_INVALID_PSEUDOHEADER");
      this._trailersSent = true;
      this.sentTrailers = headers;
      const built = buildNgHeaders(Object.assign({ __proto__: null }, headers), assertValidRequestPseudoHeader, this.session._options && this.session._options.strictSingleValueFields);
      // See the server stream's sendTrailers: an empty trailer set ends the
      // stream with DATA(END_STREAM), never a trailing HEADERS frame.
      if (built.list.length === 0) this.session._sendData(this, Buffer.alloc(0), true);
      else writeHeaderBlock(this.session, this.id, encodeHeaders(built.list, built.sensitive), FLAG.END_STREAM);
      this._endStreamSent = true;
    }
    close(code, cb) { http2StreamClose(this, code, cb); }
    setTimeout(ms, cb) { return http2StreamSetTimeout(this, ms, cb); }
    priority(options) { streamPriority(this); }
    _pushData(bytes) { http2StreamPushData(this, bytes); }
    _onResponse(headersObj, flags, rawHeaders) {
      if (this._responseEmitted) return;
      this._responseEmitted = true;
      this.pending = false;
      // NOT DONE HERE, deliberately: node onStreamHeaders() drops the request's
      // origin from the session's origin set on a 421 (
      // HTTP_STATUS_MISDIRECTED_REQUEST) — `originSet.delete(stream[kOrigin])`.
      // Implementing it (two lines, using `this._origin` below) does make
      // test-http2-origin's 421 block pass its assertions, but the block then
      // reaches its own teardown for the first time and the process never exits:
      // there is a THIRD defect in that file, a secure session whose close()
      // after a 421 response never releases the loop. Landing the fix therefore
      // turned a 1-second failure into a 30-second corpus timeout without making
      // the file green, so it is held back until the teardown is understood.
      this.endAfterHeaders = !!(flags & FLAG.END_STREAM);
      this.emit("response", headersObj, flags, rawHeaders || []);
    }
    _onEnd() { http2StreamEndReadable(this); }
    _onClose() { http2StreamFinish(this); }
  }

  // === ClientHttp2Session ===
  class ClientHttp2Session extends EE {
    constructor(authority, options, listener) {
      super();
      this._options = options || {};
      this.streams = new Map();
      this.destroyed = false;
      this.closed = false;
      this.connecting = true;
      this._connected = false;
      this._preConnectQ = [];       // frames requested before the preface is sent
      this._recv = Buffer.alloc(0);
      this._prefaceReceived = false;
      this._hpack = new HpackDecoder();
      this._maxFrameSize = constants.DEFAULT_SETTINGS_MAX_FRAME_SIZE;  // our advertised max (16384)
      this._remoteSettings = null;
      this._localSettings = { headerTableSize: 4096, enablePush: true, initialWindowSize: 65535, maxFrameSize: 16384, maxConcurrentStreams: 4294967295, maxHeaderListSize: 65535, enableConnectProtocol: false };
      if (this._options.settings) Object.assign(this._localSettings, this._options.settings);
      this._pendingHeaderBlock = null;   // { streamId, endStream, buf }
      // node kMaxReservedRemoteStreams (default 200): a peer that reserves more
      // push streams than this gets them RST_STREAM'd with CANCEL.
      this._maxReservedRemoteStreams = (this._options.maxReservedRemoteStreams === undefined) ? 200 : (this._options.maxReservedRemoteStreams | 0);
      this._reservedRemoteStreams = 0;
      // connection-level flow control bookkeeping, surfaced through `state`
      this._localWindow = DEFAULT_CONNECTION_WINDOW;    // what the peer may still send us
      this._remoteWindow = DEFAULT_CONNECTION_WINDOW;   // what we may still send the peer
      this._lastProcStreamId = 0;
      // node kMaybeDestroy's gate — see _maybeDestroy(). A stream that is open
      // (created, not yet destroyed) holds a graceful close() open.
      this._liveStreams = 0;
      // node/nghttp2 hold a request that would exceed the peer's
      // SETTINGS_MAX_CONCURRENT_STREAMS in the session's pending queue and
      // submit it when a slot frees. Sending it anyway earns a REFUSED_STREAM
      // from a conforming server (test-http2-too-many-streams).
      this._openRequests = 0;
      this._pendingSubmits = [];
      this._destroyPending = false;
      // node Http2Session: `encrypted` reflects the transport, `alpnProtocol` is
      // "h2c" for a cleartext session, and `originSet` stays undefined until an
      // ORIGIN frame arrives (DEFERRED).
      this.alpnProtocol = null;
      this.encrypted = false;
      this._originSet = undefined;
      if (typeof listener === "function") this.once("connect", listener);

      const u = parseAuthority(authority, options);
      // node connect(): `if (protocol !== 'http:' && protocol !== 'https:')
      // throw new ERR_HTTP2_UNSUPPORTED_PROTOCOL(protocol)`. Without it an
      // `ssh://localhost` authority silently dialled as cleartext http.
      if (u.protocol !== "http:" && u.protocol !== "https:")
        throw mkErr('protocol "' + u.protocol + '" is unsupported.', "ERR_HTTP2_UNSUPPORTED_PROTOCOL");
      this._url = u.origin;
      this._scheme = u.protocol === "https:" ? "https" : "http";
      const port = u.port ? +u.port : (this._scheme === "https" ? 443 : 80);
      // node connect(): `session[kAuthority] = `${options.servername || host}:${port}``
      // where `port` is the explicit one OR the scheme default — the authority
      // ALWAYS carries it. `URL#host` drops a default port, so `:authority` came
      // out as bare "localhost" for http://localhost:80 (test-http2-sensitive-
      // headers connects there over a duplexPair and pins the sent header).
      // Only the default-port case is rebuilt here; keeping `u.host` otherwise
      // preserves the bracketed form of an IPv6 literal, which node's
      // `host`-based formula would flatten to `::1:8080`.
      this._authorityName = u.port ? u.host : u.host + ":" + port;
      // node connect(): `host = authority.hostname; if (host[0] === '[') host =
      // host.slice(1, -1)`. A URL keeps an IPv6 literal in its bracketed form,
      // and neither net.connect nor tls.connect accepts the brackets — the
      // bracketed name went to getaddrinfo and came back ENOTFOUND `[::1]`.
      // Only the object-authority branch of parseAuthority was stripping them.
      let host = u.hostname;
      if (host && host[0] === "[") host = host.slice(1, -1);
      // node connect(): `session[kAuthority] = `${options.servername || host}:${port}``.
      // initOriginSet reproduces the same pair off the socket; keep it here so
      // it does not depend on the transport reporting a peer address.
      this._originHost = (options && options.servername) || host;
      this._originPort = port;

      const self = this;
      // node http2.connect({ createConnection }): the caller supplies the
      // transport (a real socket, a tunnel, or a Duplex pair in the corpus) and
      // the session must not dial out itself.
      if (options && typeof options.createConnection === "function") {
        const sock = options.createConnection(u, options);
        this._rawSocket = sock;
        sock.on("data", (d) => self._onData(d));
        sock.on("error", (e) => self._onSocketError(e));
        sock.on("close", () => self._onSocketClose());
        sock.on("end", () => self._onSocketEnd());
        // A freshly-dialled socket announces itself with 'connect'; an
        // already-open Duplex never will, so start on the next tick.
        if (sock.connecting) sock.once("connect", () => self._onSocketReady());
        else G.queueMicrotask(() => self._onSocketReady());
        return;
      }
      if (this._scheme === "https") {
        this.encrypted = true;
        if (!tls || !tls.connect) { this._fatal(mkErr("http2 https requires node:tls", "ERR_HTTP2_ERROR")); return; }
        // node internal/http2/core.js connect():
        //   tls.connect(port, host, initializeTLSOptions(options,
        //               net.isIP(host) ? undefined : host))
        // initializeTLSOptions hands tls.connect the CALLER'S WHOLE options
        // object with ALPNProtocols/servername overwritten — it does not
        // hand-pick ca/cert/key. Picking them by name silently dropped
        // `secureContext`, `pfx`, `passphrase`, `ciphers`, `checkServerIdentity`
        // and the rest: three corpus files pass their trust anchor only as
        // `{ secureContext: tls.createSecureContext({ ca }) }` and so failed
        // with "unable to verify the first certificate".
        // — an IP authority must NOT become the SNI ServerName (tls.connect
        // rejects that with ERR_INVALID_ARG_VALUE), so only a real hostname is
        // promoted to servername.
        const tlsOpts = Object.assign({}, options, {
          host, port,
          servername: options && options.servername ? options.servername
            : (net && typeof net.isIP === "function" && net.isIP(host) ? undefined : host),
        });
        // node initializeTLSOptions: the h2 ALPN list is only imposed when the
        // caller did NOT supply an ALPNCallback (the two are mutually exclusive
        // in tls.connect), and allowHTTP1 appends the fallback protocol.
        if (!tlsOpts.ALPNCallback) {
          tlsOpts.ALPNProtocols = options && options.allowHTTP1 === true ? ["h2", "http/1.1"] : ["h2"];
        }
        const sock = tls.connect(tlsOpts);
        this._rawSocket = sock;
        sock.on("secureConnect", () => { self.alpnProtocol = sock.alpnProtocol || "h2"; self._onSocketReady(); });
        sock.on("data", (d) => self._onData(d));
        sock.on("error", (e) => self._onSocketError(e));
        sock.on("close", () => self._onSocketClose());
        sock.on("end", () => self._onSocketEnd());
      } else {
        // node internal/http2/core.js connect(): `net.connect({ port, host,
        // ...options })`, where `port` is the STRING form of the authority port
        // (`'' + (authority.port !== '' ? authority.port : 80)`). Building the
        // socket by hand bypassed a `net.connect` the caller had replaced, and
        // handed the port as a number — test-http2-client-port-80 asserts on
        // both. Going through net.connect also lets localAddress/family/lookup
        // reach the transport the way node's spread does.
        const netOpts = Object.assign({ port: String(port), host }, options);
        const sock = typeof net.connect === "function" ? net.connect(netOpts) : new net.Socket();
        this._rawSocket = sock;
        sock.on("connect", () => { self.alpnProtocol = "h2c"; self._onSocketReady(); });
        sock.on("data", (d) => self._onData(d));
        sock.on("error", (e) => self._onSocketError(e));
        sock.on("close", () => self._onSocketClose());
        sock.on("end", () => self._onSocketEnd());
        if (typeof net.connect !== "function") sock.connect(port, host);
      }
    }

    // node Http2Session[EventEmitter.captureRejectionSymbol]: a rejected
    // 'stream' listener kills that stream, anything else kills the session.
    [kRejection](err, event, ...args) {
      if (event === "stream" && args[0] && typeof args[0].destroy === "function") args[0].destroy(err);
      else this.destroy(err);
    }

    // node keeps EVERY pending request's connect continuation in ONE array
    // (kPendingRequestCalls) drained by a SINGLE `once('connect')` listener, so
    // the eleventh concurrent request does not trip EventEmitter's
    // MaxListenersExceededWarning. Registering one listener per request did:
    // test-http2-client-request-listeners-warning asserts no warning is emitted
    // for 11 requests issued while the session is still connecting.
    _whenConnected(fn) {
      if (this._connected) { G.queueMicrotask(fn); return; }
      if (this._pendingConnectCalls) { this._pendingConnectCalls.push(fn); return; }
      this._pendingConnectCalls = [fn];
      const self = this;
      this.once("connect", () => {
        const q = self._pendingConnectCalls;
        self._pendingConnectCalls = null;
        if (q) for (const f of q) f();
      });
    }

    // node Http2Session#originSet: undefined on a cleartext or destroyed
    // session, otherwise the lazily-seeded set as an array.
    get originSet() {
      if (!this.encrypted || this.destroyed) return undefined;
      return Array.from(initOriginSet(this));
    }

    _onSocketReady() {
      if (this._connected || this.destroyed) return;
      this._connected = true;
      this.connecting = false;
      // The connection preface (RFC 7540 3.5) MUST precede every other frame.
      // request() can be called synchronously before the socket connects, so any
      // HEADERS it produced were buffered in _preConnectQ; flush them AFTER the
      // preface + our SETTINGS.
      this._rawSocket.write(CLIENT_PREFACE);
      // http2.connect(authority, { settings }) has to reach the wire: the peer
      // reads ENABLE_PUSH from it to decide whether it may push at all, and the
      // corpus asserts the server sees `remoteSettings.enablePush === false`.
      // This was an unconditionally empty payload, so every configured client
      // setting was silently dropped.
      const initial = Object.assign({}, this._localSettings, this._options && this._options.settings);
      this._writeFrame(FRAME.SETTINGS, 0, 0, encodeSettings(initial));
      // node Http2Session#pendingSettingsAck reads the C++ session's outstanding
      // Http2Settings count, and the INITIAL SETTINGS frame nghttp2 submits at
      // session creation is one of them: it stays outstanding until the peer
      // acks it, and that ack emits 'localSettings' like any other (it just has
      // no callback). mbun queued only explicit settings() calls, so
      // `client.pendingSettingsAck` was false between connect and the first ack
      // AND 'localSettings' fired once where node fires twice —
      // test-http2-session-settings pins both.
      if (!this._pendingSettingsAcks) this._pendingSettingsAcks = [];
      this._pendingSettingsAcks.push({ settings: initial, cb: null, start: Date.now() });
      const q = this._preConnectQ; this._preConnectQ = [];
      for (let i = 0; i < q.length; i++) this._rawSocket.write(q[i].buf);
      this.emit("connect", this, this._rawSocket);
    }

    _writeFrame(type, flags, streamId, payload) {
      if (this.destroyed || !this._rawSocket) return;
      payload = payload || Buffer.alloc(0);
      const frame = Buffer.concat([frameHeader(payload.length, type, flags, streamId), payload]);
      // Tagged with the stream it belongs to: node does not submit a request at
      // all until the socket has connected (requestOnConnect), so a stream
      // destroyed in between never reaches the wire. mbun consumes the id and
      // serialises the HEADERS inside request(), so the equivalent is to DROP the
      // queued frames of a stream that was destroyed before the handshake — see
      // http2StreamDestroy. Without it `client.request(); req.destroy();` still
      // opened a stream on the server (test-http2-client-destroy's
      // "destroy before connect" block asserts `server.on('stream')` never fires).
      if (!this._connected) { this._preConnectQ.push({ buf: frame, streamId }); return; }
      // See the server session's _writeFrame: net.Socket.write() on an ended or
      // destroyed socket EMITS 'error' instead of throwing, so this try/catch
      // cannot contain it. nghttp2 drops frames once the transport is gone.
      if (this._rawSocket.destroyed || this._rawSocket.writable === false) return;
      try { this._rawSocket.write(frame); } catch (e) { if (!this.closed && !this.destroyed) this._fatal(e); }
    }

    request(headers, options) {
      // node ClientHttp2Session#request: "Keep argument validation synchronous,
      // but defer session-state failures to the returned stream so request
      // retries from stream callbacks do not throw before session lifecycle
      // handlers run." Throwing synchronously here meant a request issued from
      // a stream 'close'/'error' handler during teardown blew up inside the
      // emit instead of settling the new stream with an error — the call then
      // never completed (test-http2-client-session-close-before-stream-close,
      // and the same shape behind grpc-js post-error teardown hangs).
      let requestError;
      if (this.destroyed) requestError = mkErr("The session has been destroyed", "ERR_HTTP2_INVALID_SESSION");
      else if (this.closed) requestError = mkErr("New streams cannot be created after receiving a GOAWAY", "ERR_HTTP2_GOAWAY_SESSION");
      headers = headers || {};
      options = options || {};
      deprecateWeight(options);
      // node reads these option getters synchronously while building the request
      // frame (lib/internal/http2/core.js). Some tests assert the getters fire
      // during request(); read them up front so user side effects run in order.
      const optPadding = options.paddingStrategy;
      const optExclusive = options.exclusive;
      const optParent = options.parent;
      const optWeight = options.weight;
      const optWaitTrailers = options.waitForTrailers;
      const optEndStream = options.endStream === true;
      // node setAndValidatePriorityOptions + validateBoolean(options.endStream):
      // every one of these was accepted with any type at all.
      if (options.parent !== undefined && typeof options.parent !== "number")
        throw argTypeErr("options.parent", "of type number", options.parent);
      if (options.exclusive !== undefined && typeof options.exclusive !== "boolean")
        throw argTypeErr("options.exclusive", "of type boolean", options.exclusive);
      if (options.silent !== undefined && typeof options.silent !== "boolean")
        throw argTypeErr("options.silent", "of type boolean", options.silent);
      if (options.endStream !== undefined && typeof options.endStream !== "boolean")
        throw argTypeErr("options.endStream", "of type boolean", options.endStream);
      // Header validation runs BEFORE the stream id is consumed: node throws
      // out of request() for a malformed header block and no stream is opened.
      const rawForm = Array.isArray(headers);
      const built = rawForm
        ? buildHeaderListArray(headers, this._scheme, this._authorityName, this._options.strictSingleValueFields)
        : buildHeaderList(headers, this._scheme, this._authorityName, this._options.strictSingleValueFields);
      const self = this;
      // The stream object exists before the id does: node's Http2Stream is
      // constructed with no handle and gets one in kInit, which is what lets a
      // request wait for a free concurrency slot (below) without the caller
      // seeing anything but a normal, corked stream.
      const stream = new ClientHttp2Stream(this, 0, headers, options);
      const submit = () => {
        const streamId = self._nextStreamId();
        if (streamId < 0) {
          // node requestOnConnect: a negative id from nghttp2 becomes
          // ERR_HTTP2_OUT_OF_STREAMS on the stream, asynchronously -- request()
          // still hands back a stream object.
          stream.pending = false;
          stream._closed = true;
          G.queueMicrotask(() => {
            if (!stream.destroyed) stream.destroy(mkErr("No stream ID is available because maximum stream ID has been reached", "ERR_HTTP2_OUT_OF_STREAMS"));
          });
          return;
        }
        stream.id = streamId;
        self.streams.set(streamId, stream);
        self._openRequests++;
        stream._countedOpen = true;
        const block = encodeHeaders(built.list, built.sensitive);
        // node maxSendHeaderBlockLength: nghttp2 refuses to serialise a header
        // block over the limit, which surfaces as 'frameError' on the stream plus
        // a REFUSED_STREAM stream error — nothing goes on the wire.
        const maxBlock = self._options.maxSendHeaderBlockLength;
        if (maxBlock !== undefined && block.length > maxBlock) {
          stream.pending = false;
          self.streams.delete(streamId);
          stream.rstCode = constants.NGHTTP2_REFUSED_STREAM;
          stream._closed = true;
          G.queueMicrotask(() => {
            stream.emit("frameError", FRAME.HEADERS, constants.NGHTTP2_FRAME_SIZE_ERROR, streamId);
            stream.destroy(streamErr(constants.NGHTTP2_REFUSED_STREAM));
            self._fatal(sessionErr(constants.NGHTTP2_FRAME_SIZE_ERROR));
          });
          return;
        }
        // A method with no body (GET/HEAD/DELETE) or an explicit endStream option
        // may close the stream on the HEADERS frame; otherwise req.end() sends the
        // trailing empty DATA(END_STREAM). HEADERS otherwise carry END_HEADERS only.
        // node kNoPayloadMethods: GET/HEAD/DELETE assign no meaning to a request
        // payload, so endStream defaults to true for them unless the caller says
        // otherwise. The peer then sees endAfterHeaders on its request stream.
        // node Http2Stream#sentHeaders is the prepared object, i.e. including the
        // :method/:authority/:scheme/:path defaults request() filled in.
        stream.sentHeaders = built.prepared || headers;
        // node request(): `stream[kOrigin] = `${scheme}://${authority}``, where
        // `authority` is the request's OWN :authority (a request may target a
        // different origin on the same connection). node uses it in exactly one
        // place — dropping the origin from the session's origin set on a 421 —
        // which _onResponse() explains is deliberately not wired up yet.
        {
          const reqAuthority = (built.prepared && built.prepared[":authority"]) || self._authorityName;
          try { stream._origin = getURLOrigin(self._scheme + "://" + reqAuthority); } catch (e) {}
        }
        const method = built.method === undefined ? "GET" : String(built.method);
        const noBody = /^(GET|HEAD|DELETE)$/.test(method);
        const endStream = options.endStream === undefined ? noBody : options.endStream === true;
        writeHeaderBlock(self, streamId, block, endStream ? FLAG.END_STREAM : 0);
        // node requestOnConnect: "Close the writable side of the stream if
        // options.endStream is set." A GET/HEAD/DELETE request therefore starts
        // with an already-finished writable side.
        if (endStream) { stream._endStreamSent = true; stream.end(); }
        stream.pending = false;
        // node Http2Stream[kInit] emits 'ready' the moment the stream is bound to
        // an id/handle. For a session that is still connecting node runs kInit
        // from requestOnConnect, i.e. AFTER request() returned and the caller
        // attached its listener — mbun assigns the id inside request(), so the
        // event has to be raised on the connect edge (or a microtask later for an
        // already-connected session) or nobody can ever observe it.
        {
          const readyStream = stream;
          const emitReady = () => { if (!readyStream.destroyed) readyStream.emit("ready"); };
          if (self._connected) G.queueMicrotask(emitReady);
          else self._whenConnected(() => {
            // node requestOnConnect: a session close() that lands between
            // request() and the socket handshake kills the request with
            // ERR_HTTP2_GOAWAY_SESSION instead of letting it start. mbun
            // consumes the stream id inside request(), so the check has to run
            // on the connect edge (test-http2-goaway-delayed-request).
            if (self.closed && !self.destroyed && !readyStream.destroyed) {
              readyStream._closed = true;
              readyStream.destroy(mkErr("New streams cannot be created after receiving a GOAWAY", "ERR_HTTP2_GOAWAY_SESSION"));
              return;
            }
            emitReady();
          });
        }
        // node ClientHttp2Session#request: 'created' is published with the
        // prepared header object (`sentHeaders`), 'start' once the HEADERS frame
        // has actually been submitted.
        if (onClientStreamCreatedChannel.hasSubscribers) onClientStreamCreatedChannel.publish({ stream, headers: stream.sentHeaders });
        if (onClientStreamStartChannel.hasSubscribers) onClientStreamStartChannel.publish({ stream, headers: stream.sentHeaders });
      };
      // A submitted stream holds a concurrency slot until it closes; releasing
      // it is what lets the next queued request go out.
      stream.once("close", () => {
        if (stream._countedOpen === true) {
          stream._countedOpen = false;
          if (self._openRequests > 0) self._openRequests--;
        }
        self._drainPendingSubmits();
      });
      if (requestError) {
        // node: `process.nextTick(requestOnError.bind(stream, requestError))`,
        // i.e. the stream is destroyed with the session-state error and emits
        // 'error' + 'close' like any other failed request.
        stream.pending = false;
        G.process.nextTick(() => { if (!stream.destroyed) stream.destroy(requestError); });
      } else if (this._pendingSubmits.length > 0 || this._openRequests >= this._maxConcurrentSend()) {
        // Cork exactly the way the constructor corks a not-yet-connected
        // stream: writes and end() issued by the caller are buffered until the
        // HEADERS frame has gone out.
        if (kHaveDuplex) { try { stream.cork(); } catch (e) {} }
        const queued = () => { submit(); if (kHaveDuplex) { try { stream.uncork(); } catch (e) {} } };
        queued.stream = stream;
        this._pendingSubmits.push(queued);
      } else {
        submit();
      }
      // node ClientHttp2Session#request options.signal: an aborted signal
      // destroys the stream with an AbortError (which resets with CANCEL, see
      // http2StreamDestroy).
      const signal = options.signal;
      if (signal !== undefined && signal !== null) {
        if (typeof signal !== "object" || typeof signal.aborted !== "boolean" || typeof signal.addEventListener !== "function")
          throw argTypeErr("options.signal", "an instance of AbortSignal", signal);
        const aborter = () => { if (!stream.destroyed) stream.destroy(abortErr(signal.reason)); };
        if (signal.aborted) aborter();
        else {
          signal.addEventListener("abort", aborter, { once: true });
          stream.once("close", () => { try { signal.removeEventListener("abort", aborter); } catch (e) {} });
        }
      }
      return stream;
    }

    // The peer's SETTINGS_MAX_CONCURRENT_STREAMS, or Infinity until it has sent
    // one (RFC 9113 6.5.2: unlimited until otherwise advertised).
    _maxConcurrentSend() {
      const rs = this._remoteSettings;
      return (rs && typeof rs.maxConcurrentStreams === "number") ? rs.maxConcurrentStreams : Infinity;
    }
    _drainPendingSubmits() {
      const q = this._pendingSubmits;
      if (!q || q.length === 0) return;
      while (q.length > 0 && !this.destroyed && this._openRequests < this._maxConcurrentSend()) q.shift()();
    }

    // Returns -1 when the 31-bit stream-id space is exhausted (RFC 9113 5.1.1).
    // It used to wrap back to 1, silently reusing a closed id.
    _nextStreamId() {
      if (!this._lastStreamId) this._lastStreamId = -1;
      const next = this._lastStreamId + 2;
      if (next > 2147483647) return -1;
      this._lastStreamId = next;
      if (this._lastStreamId < 1) this._lastStreamId = 1;
      return this._lastStreamId;
    }
    setNextStreamID(id) {
      // node Http2Session#setNextStreamID checks the session BEFORE validating
      // the argument, so `client.setNextStreamID()` on a destroyed session is an
      // ERR_HTTP2_INVALID_SESSION (name 'Error'), not an ERR_INVALID_ARG_TYPE
      // (name 'TypeError'). test-http2-client-destroy asserts the name.
      if (this.destroyed) throw mkErr("The session has been destroyed", "ERR_HTTP2_INVALID_SESSION");
      if (typeof id !== "number") throw argTypeErr("id", "of type number", id);
      if (!Number.isInteger(id) || id <= 0 || id > 4294967295) throw outOfRangeErr("id", "> 0 and <= 4294967295", id);
      this._lastStreamId = id - 2;   // next request() advances by 2 to `id`
    }

    _sendData(stream, buf, endStream) {
      if (this.destroyed) return;
      if (buf.length === 0 && !endStream) return;
      // chunk to max frame size
      const max = (this._remoteSettings && this._remoteSettings.maxFrameSize) || 16384;
      let off = 0;
      if (buf.length === 0) {
        this._writeFrame(FRAME.DATA, endStream ? FLAG.END_STREAM : 0, stream.id, Buffer.alloc(0));
        return;
      }
      this._remoteWindow -= buf.length;
      while (off < buf.length) {
        const end = Math.min(off + max, buf.length);
        const isLast = end >= buf.length;
        this._writeFrame(FRAME.DATA, (isLast && endStream) ? FLAG.END_STREAM : 0, stream.id, buf.subarray(off, end));
        off = end;
      }
    }

    _rstStream(stream, code) {
      const p = Buffer.alloc(4); p.writeUInt32BE(code >>> 0, 0);
      this._writeFrame(FRAME.RST_STREAM, 0, stream.id, p);
    }

    // ---- inbound ----
    _onData(chunk) {
      if (this.destroyed) return;
      if (this._timeoutMs) this._armTimeout();   // inbound activity resets the idle timer
      this._recv = this._recv.length ? Buffer.concat([this._recv, chunk]) : Buffer.from(chunk);
      this._parse();
    }

    _parse() {
      const buf = this._recv;
      let off = 0;
      while (buf.length - off >= 9) {
        // A session destroyed from inside a frame handler (a 'data' listener
        // calling session.destroy(), say) must stop consuming the rest of the
        // segment: node's nghttp2 session is gone at that point, so the frames
        // behind it are never delivered. mbun kept parsing and turned a
        // trailing RST_STREAM into an ERR_HTTP2_STREAM_ERROR on a request the
        // caller had already abandoned (test-http2-compat-errors).
        if (this.destroyed) { this._recv = Buffer.alloc(0); return; }
        const len = (buf[off] << 16) | (buf[off + 1] << 8) | buf[off + 2];
        const type = buf[off + 3];
        const flags = buf[off + 4];
        const streamId = ((buf[off + 5] & 0x7f) << 24) | (buf[off + 6] << 16) | (buf[off + 7] << 8) | buf[off + 8];
        // Connection-level frame size guard (RFC 7540 4.2): any frame larger
        // than our advertised SETTINGS_MAX_FRAME_SIZE is a FRAME_SIZE_ERROR.
        if (len > this._maxFrameSize) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return; }
        if (buf.length - off < 9 + len) break;   // need full payload
        const payload = buf.subarray(off + 9, off + 9 + len);
        off += 9 + len;
        if (!this._handleFrame(type, flags, streamId, payload, len)) return;  // fatal
      }
      this._recv = off > 0 ? buf.subarray(off) : buf;
      if (this._recv.length === 0) this._recv = Buffer.alloc(0);
    }

    _handleFrame(type, flags, streamId, payload, len) {
      switch (type) {
        case FRAME.SETTINGS: {
          if (flags & FLAG.ACK) { if (len !== 0) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; } resolveSettingsAck(this); return true; }
          if (len % 6 !== 0) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
          if (streamId !== 0) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
          // node hands `maxSettings` to nghttp2 as SETTINGS_MAX_SETTINGS: a peer
          // that packs more entries than that into one frame is a flood vector,
          // so nghttp2 fails the connection before the application is told
          // anything about it — no 'remoteSettings', no streams
          // (test-http2-max-settings). The option was accepted and ignored.
          const maxSettings = (this._options && this._options.maxSettings) || 32;
          if (len / 6 > maxSettings) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
          // RFC 8441 3: extended CONNECT cannot be switched back off once the
          // server has advertised it. See connectProtocolWithdrawn().
          if (connectProtocolWithdrawn(this, payload)) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
          const settings = this._parseSettings(payload);
          this._remoteSettings = settings; this._remoteSettingsView = undefined;
          this._writeFrame(FRAME.SETTINGS, FLAG.ACK, 0, Buffer.alloc(0));   // ack
          this.emit("remoteSettings", remoteSettingsOf(this));
          // A raised MAX_CONCURRENT_STREAMS frees slots for anything queued.
          this._drainPendingSubmits();
          return true;
        }
        case FRAME.HEADERS: {
          return this._handleHeaders(flags, streamId, payload);
        }
        case FRAME.CONTINUATION: {
          if (!this._pendingHeaderBlock || this._pendingHeaderBlock.streamId !== streamId) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
          this._pendingHeaderBlock.buf = Buffer.concat([this._pendingHeaderBlock.buf, payload]);
          if (flags & FLAG.END_HEADERS) return this._finishHeaders(flags);
          return true;
        }
        case FRAME.DATA: {
          const stream = this.streams.get(streamId);
          let data = payload;
          if (flags & FLAG.PADDED) {
            // RFC 7540 6.1: Pad Length >= remaining payload is a PROTOCOL_ERROR;
            // a PADDED frame too short to hold the Pad Length byte is a size error.
            if (payload.length < 1) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
            const pad = payload[0];
            if (pad >= payload.length) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
            data = payload.subarray(1, payload.length - pad);
          }
          this._lastProcStreamId = streamId;
          this._localWindow -= len;
          if (stream) {
            if (data.length) stream._pushData(data);
            // maintain flow-control windows so large bodies keep flowing
            if (len > 0) { this._windowUpdate(0, len); this._windowUpdate(streamId, len); }
            if (flags & FLAG.END_STREAM) { this.streams.delete(streamId); stream._onEnd(); }
          }
          return true;
        }
        case FRAME.RST_STREAM: {
          if (streamId === 0) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
          if (len !== 4) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
          const code = payload.readUInt32BE(0);
          const stream = this.streams.get(streamId);
          if (stream) {
            this.streams.delete(streamId);
            stream.rstCode = code;
            stream._closed = true;
            // node abort(stream): a reset while the WRITABLE side is still open
            // is an abort whatever the RST code, NGHTTP2_NO_ERROR included --
            // the gate is `!(writableEnded || writableEnding)`, not the code and
            // not the readable side. mbun emitted 'aborted' only for a non-zero
            // code, so a clean RST on a still-open request never reported it.
            const wOpen = !(stream.writableEnded === true ||
                            (stream._writableState && stream._writableState.ending === true));
            if (!stream.aborted && wOpen) G.queueMicrotask(() => stream.emit("aborted"));
            stream.aborted = true;
            if (code !== 0) {
              // NGHTTP2_CANCEL is 'aborted' only, never an 'error': node treats
              // RST code 8 as the client's abort signal and explicitly excludes
              // it from ERR_HTTP2_STREAM_ERROR (lib/internal/http2/core.js:2486).
              const err = code === constants.NGHTTP2_CANCEL ? null : streamErr(code);
              // node abort(stream): a stream reset before its readable side ended
              // is 'aborted', and only then the error/close pair.
              G.queueMicrotask(() => {
                if (!stream.destroyed) { if (err) stream.destroy(err); else stream.destroy(); }
              });
            }
            // node onStreamClose(): "Push a null so the stream can end whenever
            // the client consumes it completely." A clean reset (NO_ERROR) is a
            // normal end of the response, so the readable side has to END --
            // destroying the Duplex outright swallowed 'end' entirely.
            else if (!stream._readEnded) {
              stream.once("end", () => http2StreamFinish(stream));
              http2StreamEndReadable(stream);
            }
            else http2StreamFinish(stream);
          }
          return true;
        }
        case FRAME.GOAWAY: {
          if (streamId !== 0) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
          if (len < 8) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
          const lastStreamId = payload.readUInt32BE(0);
          const code = payload.readUInt32BE(4);
          this.emit("goaway", code, lastStreamId, payload.length > 8 ? Buffer.from(payload.subarray(8)) : undefined);
          if (code !== 0) { this._fatal(sessionErr(code)); return false; }
          // A graceful GOAWAY (NO_ERROR) starts an orderly shutdown but streams
          // with id <= lastStreamId keep running (RFC 9113 6.8); don't tear the
          // socket down now or an in-flight response would be lost.
          this._goawayReceived = true;
          return true;
        }
        case FRAME.PING: {
          if (len !== 8) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
          if (!(flags & FLAG.ACK)) {
            this._writeFrame(FRAME.PING, FLAG.ACK, 0, Buffer.from(payload));
            // Http2Session 'ping' — emitted for a PING the PEER initiated, never
            // for the ACK of one of ours (bun http2.ts:4213-4217 ping handler;
            // node lib/internal/http2/core.js). The 8-byte opaque payload is the
            // event argument.
            this.emit("ping", Buffer.from(payload));
          } else resolvePing(this, payload);
          return true;
        }
        case FRAME.WINDOW_UPDATE: {
          if (len !== 4) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
          // we do not throttle our own sends beyond max frame size, but the
          // connection window has to stay accurate for `state.remoteWindowSize`
          if (streamId === 0) this._remoteWindow += payload.readUInt32BE(0) & 0x7fffffff;
          return true;
        }
        case FRAME.PUSH_PROMISE: return this._handlePushPromise(flags, streamId, payload);
        case FRAME.ALTSVC: return handleAltsvcFrame(this, streamId, payload);
        case FRAME.ORIGIN: return handleOriginFrame(this, payload);
        default:
          return true;   // unknown frame types are ignored (RFC 7540 4.1)
      }
    }

    // RFC 9113 6.6: PUSH_PROMISE = [Pad Length?][Promised Stream ID (31 bits)]
    // [Header Block][Padding]. A push arriving after we advertised ENABLE_PUSH=0
    // is a connection error; otherwise the promised (even) id is reserved and
    // surfaces as a session 'stream' event carrying the *request* headers the
    // server is answering on our behalf.
    _handlePushPromise(flags, streamId, payload) {
      if (!this._localSettings.enablePush) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
      if (streamId === 0) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
      let off = 0, padLen = 0;
      if (flags & FLAG.PADDED) {
        if (payload.length < 1) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
        padLen = payload[0]; off = 1;
      }
      if (payload.length < off + 4) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
      const promisedId = payload.readUInt32BE(off) & 0x7fffffff;
      off += 4;
      if (off + padLen > payload.length) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
      const block = payload.subarray(off, payload.length - padLen);
      this._pendingHeaderBlock = { streamId, promisedId, push: true, endStream: false, buf: Buffer.from(block) };
      if (flags & FLAG.END_HEADERS) return this._finishHeaders(flags);
      return true;
    }
    _finishPushPromise(pb, list, flags) {
      const headersObj = headerListToObject(list, this._hpack._sensitive, this._strictWs());
      // node kMaxReservedRemoteStreams: over the limit the promise is refused
      // with RST_STREAM(CANCEL) and never surfaces to the application.
      if (this._reservedRemoteStreams >= this._maxReservedRemoteStreams) {
        const p = Buffer.alloc(4); p.writeUInt32BE(constants.NGHTTP2_CANCEL, 0);
        this._writeFrame(FRAME.RST_STREAM, 0, pb.promisedId, p);
        return true;
      }
      this._reservedRemoteStreams++;
      const push = new ClientHttp2Stream(this, pb.promisedId, headersObj, {});
      push.pushed = true;
      push.pending = false;
      // A pushed stream carries no request body: its writable side starts closed.
      push._endStreamSent = true;
      push.end();
      this.streams.set(pb.promisedId, push);
      // node onStreamHeaders (client + push promise): the promised stream is
      // announced on the same two channels a request() stream is.
      if (onClientStreamCreatedChannel.hasSubscribers) onClientStreamCreatedChannel.publish({ stream: push, headers: headersObj });
      if (onClientStreamStartChannel.hasSubscribers) onClientStreamStartChannel.publish({ stream: push, headers: headersObj });
      this.emit("stream", push, headersObj, flags);
      return true;
    }
    _handleHeaders(flags, streamId, payload) {
      // RFC 7540 6.2: payload = [Pad Length?][Priority(5)?][Header Block][Padding].
      const data = payload;
      let off = 0, padLen = 0;
      if (flags & FLAG.PADDED) {
        if (data.length < 1) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
        padLen = data[0]; off = 1;
      }
      if (flags & FLAG.PRIORITY) {
        if (data.length < off + 5) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
        off += 5;
      }
      if (off + padLen > data.length) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
      const block = data.subarray(off, data.length - padLen);
      this._pendingHeaderBlock = { streamId, endStream: !!(flags & FLAG.END_STREAM), buf: Buffer.from(block) };
      if (flags & FLAG.END_HEADERS) return this._finishHeaders(flags);
      return true;
    }

    _finishHeaders(flags) {
      const pb = this._pendingHeaderBlock;
      this._pendingHeaderBlock = null;
      let list;
      try { list = this._hpack.decode(pb.buf); }
      catch (e) { this._connError(constants.NGHTTP2_COMPRESSION_ERROR); return false; }
      if (pb.push) return this._finishPushPromise(pb, list, flags);
      const stream = this.streams.get(pb.streamId);
      if (!stream) return true;
      const headersObj = headerListToObject(list, this._hpack._sensitive, this._strictWs());
      // node onSessionHeaders passes the decoded raw name/value array as the third
      // argument of every header event, so a caller can see duplicates and order
      // the collapsed object cannot express.
      const rawHeaders = rawFromList(list);
      // The response HEADERS of a *pushed* stream are surfaced as 'push', not
      // 'response' (node ClientHttp2Stream: a push response is unsolicited).
      if (stream.pushed === true && !stream._responseEmitted) {
        stream._responseEmitted = true;
        stream.emit("push", headersObj, flags, rawHeaders);
        // node onStreamHeaders: the 'finish' channel covers the 'response' and
        // 'push' events only — an informational or trailer block is not a finish.
        if (onClientStreamFinishChannel.hasSubscribers) onClientStreamFinishChannel.publish({ stream, headers: headersObj, flags });
        if (pb.endStream) { this.streams.delete(pb.streamId); stream._onEnd(); }
        return true;
      }
      // A header block after the response is a trailer block (RFC 9113 8.1):
      // emit 'trailers' rather than a second 'response'. A 1xx block BEFORE the
      // response is informational: node emits 'headers', and the real response
      // still follows (lib/internal/http2/core.js onSessionHeaders).
      const st = headersObj[":status"];
      if (stream._responseEmitted) http2StreamEmitTrailers(stream, headersObj, flags, rawHeaders, pb.endStream);
      else if (typeof st === "number" && st >= 100 && st < 200) {
        stream.emit("headers", headersObj, flags, rawHeaders);
        // node ClientHttp2Stream handleHeaderContinue: a 100 informational
        // response is additionally surfaced as 'continue'.
        if (st === 100) stream.emit("continue");
      }
      else {
        stream._onResponse(headersObj, flags, rawHeaders);
        if (onClientStreamFinishChannel.hasSubscribers) onClientStreamFinishChannel.publish({ stream, headers: headersObj, flags });
      }
      if (pb.endStream) { this.streams.delete(pb.streamId); stream._onEnd(); }
      return true;
    }

    _parseSettings(payload) {
      const s = {};
      for (let i = 0; i + 6 <= payload.length; i += 6) {
        const id = (payload[i] << 8) | payload[i + 1];
        const val = (payload[i + 2] * 0x1000000) + (payload[i + 3] << 16) + (payload[i + 4] << 8) + payload[i + 5];
        s[id] = val;
      }
      const out = {
        headerTableSize: s[1] !== undefined ? s[1] : 4096,
        enablePush: s[2] !== undefined ? s[2] : 1,
        maxConcurrentStreams: s[3] !== undefined ? s[3] : 4294967295,
        initialWindowSize: s[4] !== undefined ? s[4] : 65535,
        maxFrameSize: s[5] !== undefined ? s[5] : 16384,
        maxHeaderListSize: s[6] !== undefined ? s[6] : 65535,
        enableConnectProtocol: s[8] !== undefined ? s[8] : 0,
      };
      out.customSettings = customFromRawSettings(s);
      return out;
    }

    _windowUpdate(streamId, increment) {
      if (streamId === 0) this._localWindow += increment;
      const p = Buffer.alloc(4); p.writeUInt32BE(increment >>> 0, 0);
      this._writeFrame(FRAME.WINDOW_UPDATE, 0, streamId, p);
    }
    _strictWs() { return this._options.strictFieldWhitespaceValidation !== false; }
    setLocalWindowSize(windowSize) { sessionSetLocalWindowSize(this, windowSize); }

    // ---- errors / lifecycle ----
    // A protocol violation THIS endpoint detected: GOAWAY the peer, then
    // surface nghttp2's own error (ERR_HTTP2_ERROR), not the received-GOAWAY
    // form. `errno` names the nghttp2 error; -505 (Protocol error) covers every
    // frame-level violation, and the connection-preface check passes -903.
    _connError(code, errno) {
      const p = Buffer.alloc(8); p.writeUInt32BE(this._lastStreamId > 0 ? this._lastStreamId : 0, 0); p.writeUInt32BE(code >>> 0, 4);
      try { this._writeFrame(FRAME.GOAWAY, 0, 0, p); } catch (e) {}
      this._fatal(nghttpErr(errno === undefined ? NGHTTP2_ERR_PROTO : errno));
    }
    _onSocketError(e) { if (!this.destroyed && !this.closed) this._fatal(e); }
    // Peer half-close: same reasoning as the server session below — no further
    // frames can arrive, so once the streams have finished the session (and its
    // allowHalfOpen socket, which would otherwise hold the loop) must go.
    _onSocketEnd() {
      if (this.destroyed) return;
      abortStreamsOnTransportEof(this);
      this._endPending = false;
      this._teardown();
    }
    // node socketOnClose(): cancel the open streams, then close the session —
    // and emit an error ONLY when the socket died while still connecting
    // (ERR_SOCKET_CLOSED). A transport that simply went away is not by itself a
    // session error. mbun synthesised ERR_HTTP2_SESSION_ERROR /
    // NGHTTP2_INTERNAL_ERROR for every abrupt close, which is an uncaught
    // exception in every test whose peer just destroys its socket.
    _onSocketClose() {
      if (this.destroyed) return;
      abortStreamsOnTransportEof(this);
      if (this.connecting) { this._fatal(mkErr("Socket has been disconnected from the Http2Session", "ERR_SOCKET_CLOSED")); return; }
      this._teardown();
    }
    _fatal(err) {
      if (this.destroyed) return;
      const streams = Array.from(this.streams.values());
      this._teardown();
      G.queueMicrotask(() => {
        this.emit("error", err);
        for (const s of streams) {
          s.rstCode = constants.NGHTTP2_INTERNAL_ERROR; s._closed = true;
          if (!s.destroyed) { s.destroy(err); continue; }
          // node closeSession() splits the two stream lists: the ones with a
          // handle get the session's error, but `state.pendingStreams` — the
          // requests that never got a stream id — are destroyed with
          // ERR_HTTP2_STREAM_CANCEL instead, and never see the session error at
          // all. _teardown() has already cancelled exactly those (one microtask
          // earlier, so the flag is set by the time this runs); re-reporting the
          // session error on them landed SYNCHRONOUSLY here, ahead of the
          // cancel's own async 'error', so `connect(url, { signal })` +
          // `request()` before the handshake surfaced ABORT_ERR where node
          // surfaces ERR_HTTP2_STREAM_CANCEL (test-http2-client-destroy).
          if (s._pendingCancelled !== true) s.emit("error", err);
        }
      });
    }
    // `hard` distinguishes an error teardown from a graceful one. net.Socket's
    // destroy() closes the fd immediately and drops anything still in its write
    // queue — including a GOAWAY frame written microseconds earlier, which is
    // exactly what close() does. A graceful teardown shuts the write side down
    // instead, so the frame reaches the peer.
    _teardown(hard) {
      if (this.destroyed) return;
      // A GRACEFUL teardown must not discard frames the peer has already
      // delivered. After close() the peer answers with its own GOAWAY, and those
      // bytes can be sitting in the socket's receive buffer unread — closing the
      // fd here would drop them and 'goaway' would never fire
      // (test-http2-session-graceful-close). node never loses them because its
      // read callback runs before the destroy; drain the socket once instead.
      // Must happen BEFORE `destroyed` is set: _onData ignores a destroyed session.
      if (hard === false && this._drainingSocket !== true) {
        this._drainingSocket = true;
        try { if (this._rawSocket && typeof this._rawSocket._poll === "function") this._rawSocket._poll(); } catch (e) {}
        this._drainingSocket = false;
        // A drained GOAWAY/FIN can have torn the session down already.
        if (this.destroyed) return;
      }
      this.destroyed = true; this.closed = true;
      if (this._timer != null) { try { G.clearTimeout(this._timer); } catch (e) {} this._timer = null; }
      cancelSessionPings(this);
      closeSessionSocket(this._rawSocket, hard !== false);
      // PORT-SOURCE: compat/node/lib/internal/http2/core.js cleanupSession() —
      // `session[kSocket] = undefined` runs BEFORE 'close' is emitted, which is
      // what makes a socket proxy captured earlier start raising
      // ERR_HTTP2_SOCKET_UNBOUND (test-http2-unbound-socket-proxy).
      this._rawSocket = undefined;
      // "Pending and existing streams will be destroyed. […] pending streams
      // will be destroyed using a specific ERR_HTTP2_STREAM_CANCEL error"
      // (lib/internal/http2/core.js, closeSession). mbun's client session left
      // them dangling entirely: a request made before the socket connected never
      // got its 'error'/'close' and its handle kept the loop alive
      // (test-http2-stream-removelisteners-after-close).
      // Only the PENDING ones: an open stream is already driven to its end by the
      // socket teardown, and force-finishing those cost 5 files (they emit their
      // own 'close'/'aborted' first). A stream on a session that never connected
      // has nothing to drive it at all, so it kept its handle and the loop alive.
      const pending = !this._connected ? Array.from(this.streams.values()) : [];
      // An OPEN stream is usually driven to its end by the socket teardown, and
      // force-finishing it in this microtask cost 5 files last time — it ran
      // BEFORE the stream's own 'close'/'aborted' and reordered them. But when
      // the transport dies mid-response nothing drives it at all: the stream
      // gets 'aborted' from abortStreamsOnTransportEof and then dangles, with no
      // 'close', no 'error', and a promise that never settles
      // (test-http2-client-session-close-before-stream-close). Sweep them one
      // I/O TURN later instead: every natural path has already run by then, so
      // only the genuinely dangling ones are still here.
      const open = this._connected ? Array.from(this.streams.values()) : [];
      // A request still queued behind the peer's concurrency limit has no id and
      // is not in `streams`, so the socket teardown cannot reach it. node
      // destroys its pending streams with ERR_HTTP2_STREAM_CANCEL; leaving them
      // dangling here would keep their handles (and the loop) alive forever.
      const queued = this._pendingSubmits || [];
      this._pendingSubmits = [];
      for (const entry of queued) { const s = entry.stream; if (s && !s.destroyed) { s._closed = true; s._pendingCancelled = true; s.destroy(streamCancelErr()); } }
      const self = this;
      G.queueMicrotask(() => {
        for (const s of pending) {
          if (s.destroyed) continue;
          s._closed = true;
          // Marker read by _fatal(): a stream cancelled here must not also be
          // handed the session's error.
          s._pendingCancelled = true;
          s.destroy(streamCancelErr());
        }
        self.emit("close");
        if (open.length === 0) return;
        // Settle, do not re-report: the reason the transport died has already
        // been delivered (a session 'error', the stream's own 'aborted', or
        // nothing at all for a clean close), and injecting an extra 'error'
        // here is what the 5-file cost was — an unhandled ERR_HTTP2_STREAM_CANCEL
        // on a stream whose abort the test had already observed. A bare
        // destroy() emits the missing 'close' and settles the caller.
        const nextTurn = typeof G.setImmediate === "function" ? G.setImmediate : (fn) => G.setTimeout(fn, 0);
        nextTurn(() => {
          for (const s of open) {
            if (s.destroyed) continue;
            s._closed = true;
            s.destroy();
          }
        });
      });
    }
    _shutdown() { this._teardown(); }

    close(cb) {
      if (typeof cb === "function") this.once("close", cb);
      if (this.destroyed || this.closed) return;
      this.closed = true;
      // graceful: GOAWAY(NO_ERROR), then tear down only once the open streams
      // have drained (node close() -> kMaybeDestroy). Tearing the socket down on
      // the next microtask instead destroyed in-flight frames — including the
      // GOAWAY we had just queued.
      const p = Buffer.alloc(8); p.writeUInt32BE(this._lastStreamId > 0 ? this._lastStreamId : 0, 0); p.writeUInt32BE(0, 4);
      try { this._writeFrame(FRAME.GOAWAY, 0, 0, p); } catch (e) {}
      const self = this;
      G.queueMicrotask(() => self._maybeDestroy());
    }
    // node Http2Session#close() is goaway() + kMaybeDestroy(), and kMaybeDestroy
    // returns without destroying while `state.streams.size > 0`. That wait is
    // OBSERVABLE: a client that closes from its request's 'end' handler has to
    // stay readable long enough to see the peer's own GOAWAY, which the server
    // only sends once its response has finished (test-http2-session-graceful-
    // close). Tearing the socket down on the next microtask instead dropped
    // every frame still in flight.
    // node's gate is the stream map; mbun's cannot be, because it removes a
    // stream from the routing map the moment END_STREAM arrives (node keeps it
    // until [kDestroy]) — so by the time 'end' runs the map is already empty.
    // `_liveStreams` is the node-equivalent count: it drops on stream destroy.
    _maybeDestroy() {
      if (this.destroyed || this._destroyScheduled) return;
      if (!this.closed) return;
      if (this.streams.size > 0 || this._liveStreams > 0) { this._destroyPending = true; return; }
      this._destroyPending = false;
      this._destroyScheduled = true;
      // node's close() reaches cleanupSession() synchronously once the streams
      // have drained, so `session[kSocket]` is already gone for any code that
      // runs after close() returns. mbun cannot clear `_rawSocket` here — the
      // teardown below is deliberately deferred one I/O turn and still has to
      // write/read on the transport — so unbind only the PUBLIC proxy view.
      // Internals keep using `_rawSocket`; `session.socket` starts raising
      // ERR_HTTP2_SOCKET_UNBOUND at node's moment (test-http2-unbound-socket-proxy).
      this._socketUnbound = true;
      // One I/O turn of grace, not a microtask. node's peer answers a graceful
      // close inside the same read batch (its stream closes synchronously in
      // nghttp2), so its GOAWAY is already parsed by the time close() destroys.
      // mbun's server reaches that point one microtask drain later, and the
      // pump's *second* io_tick of the same iteration is what picks the frame
      // up — a microtask teardown ran before it and dropped the socket with the
      // peer's GOAWAY still unread.
      const self = this;
      const nextTurn = typeof G.setImmediate === "function" ? G.setImmediate : (fn) => G.setTimeout(fn, 0);
      const h = nextTurn(() => self._teardown(false));
      if (h && typeof h.unref === "function") h.unref();
    }
    destroy(err, code) {
      if (this.destroyed) return;
      if (err) this._fatal(err); else this._teardown();
    }
    ref() { if (this._rawSocket && this._rawSocket.ref) this._rawSocket.ref(); return this; }
    unref() { if (this._rawSocket && this._rawSocket.unref) this._rawSocket.unref(); return this; }
    // PORT-SOURCE: compat/node/lib/internal/http2/core.js Http2Session `get socket()`
    get socket() { return sessionSocketProxy(this); }
    set socket(v) { this._rawSocket = v; }
    // node: an inactivity timeout that emits 'timeout' on the session and on
    // every open stream (lib/internal/http2/core.js Http2Session.setTimeout ->
    // #onTimeout -> forEachStream(emitTimeout)). Reset on inbound activity.
    setTimeout(ms, cb) {
      if (typeof ms !== "number") throw argTypeErr("msecs", "of type number", ms);
      if (ms < 0 || !Number.isFinite(ms)) throw outOfRangeErr("msecs", "a non-negative finite number", ms);
      this.timeout = ms;
      this._timeoutMs = ms | 0;
      this._armTimeout();
      if (cb !== undefined) {
        if (typeof cb !== "function") throw argTypeErr("callback", "of type function", cb);
        this.once("timeout", cb);
      }
      return this;
    }
    _armTimeout() {
      if (this._timer != null) { try { G.clearTimeout(this._timer); } catch (e) {} this._timer = null; }
      if (!this._timeoutMs || this.destroyed) { syncSessionTimeout(this); return; }
      const self = this;
      this._timer = G.setTimeout(() => {
        self._timer = null;
        if (self.destroyed) return;
        self.emit("timeout");
        for (const s of Array.from(self.streams.values())) { try { s.emit("timeout"); } catch (e) {} }
        // node arms ONE unref'd timer and only `refresh()`es it on activity
        // (kUpdateTimer); it is not periodic. Re-arming here made
        // `session.setTimeout(1, cb)` call cb forever.
      }, this._timeoutMs);
      if (this._timer && this._timer.unref) this._timer.unref();
      syncSessionTimeout(this);
    }
    get connected() { return this._connected; }
    get remoteSettings() { return remoteSettingsOf(this); }
    get localSettings() { return localSettingsOf(this); }
    // client streams are odd-numbered; _nextStreamId() advances _lastStreamId by 2
    get state() { return sessionState(this, this._lastStreamId > 0 ? this._lastStreamId + 2 : 1); }
    get pendingSettingsAck() { return sessionPendingAck(this); }
    get type() { return constants.NGHTTP2_SESSION_CLIENT; }
    settings(s, cb) { return sessionSubmitSettings(this, s, cb); }
    ping(payload, cb) { return sessionPing(this, payload, cb); }
    altsvc(alt, originOrStream) { return sessionAltsvc(this, alt, originOrStream); }
    origin(...origins) { return sessionOrigin(this, origins); }
    goaway(code, lastStreamId, opaqueData) { if (this.destroyed) throw mkErr("The session has been destroyed", "ERR_HTTP2_INVALID_SESSION"); const p = Buffer.alloc(8); p.writeUInt32BE((lastStreamId || 0) >>> 0, 0); p.writeUInt32BE((code || 0) >>> 0, 4); this._writeFrame(FRAME.GOAWAY, 0, 0, opaqueData ? Buffer.concat([p, Buffer.from(opaqueData)]) : p); }
  }

  // === helpers ===
  // node http2.connect(): `authority` is a string, a URL, or any object
  // exposing protocol / hostname|host / port. The plain-object form has no
  // href, so passing it to `new URL()` produced "[object Object]" cannot be
  // parsed as a URL; read the fields the way node's connect() does instead.
  function parseAuthority(authority, options) {
    if (typeof authority === "string") { try { return new G.URL(authority); } catch (e) { return new G.URL("http://" + authority); } }
    if (authority && typeof authority.href === "string") return authority;   // URL object
    if (authority && typeof authority === "object") {
      const protocol = authority.protocol || (options && options.protocol) || "https:";
      let hostname = "localhost";
      if (authority.hostname) { hostname = String(authority.hostname); if (hostname[0] === "[") hostname = hostname.slice(1, -1); }
      else if (authority.host) hostname = String(authority.host);
      const port = authority.port !== undefined && authority.port !== "" ? String(authority.port)
        : (protocol === "http:" ? "80" : "443");
      const brack = hostname.includes(":") ? "[" + hostname + "]" : hostname;
      return new G.URL(protocol + "//" + brack + ":" + port);
    }
    return new G.URL(String(authority));
  }
  // ---- outgoing header validation (node lib/internal/http2/util.js) ----------
  // buildNgHeaderString is what rejects a malformed outgoing header block before
  // a frame is ever written: a non-token field name, an HTTP/1 connection-
  // specific field, an unknown or misplaced pseudo-header, and (when the session
  // opted into strictSingleValueFields) a repeated single-value field.
  const H2_ERR = (msg, code, Ctor) => { const e = new (Ctor || TypeError)(msg); e.code = code; return e; };
  const kValidRequestPseudoHeaders = new Set([":status", ":method", ":authority", ":scheme", ":path", ":protocol"]);
  const kSingleValueFields = new Set([
    ":status", ":method", ":authority", ":scheme", ":path", ":protocol",
    "access-control-allow-credentials", "access-control-max-age", "access-control-request-method",
    "age", "authorization", "content-encoding", "content-language", "content-length",
    "content-location", "content-md5", "content-range", "content-type", "date", "dnt", "etag",
    "expires", "from", "host", "if-match", "if-modified-since", "if-none-match", "if-range",
    "if-unmodified-since", "last-modified", "location", "max-forwards", "proxy-authorization",
    "range", "referer", "retry-after", "tk", "upgrade-insecure-requests", "user-agent",
    "x-content-type-options",
  ]);
  const H2_TOKEN_RE = /^[\^_`a-zA-Z\-0-9!#$%&'*+.|~]+$/;
  function isIllegalConnectionSpecificHeader(name, value) {
    switch (name) {
      case "connection": case "upgrade": case "http2-settings":
      case "keep-alive": case "proxy-connection": case "transfer-encoding":
        return true;
      case "te": return value !== "trailers";
      default: return false;
    }
  }
  function assertValidRequestPseudoHeader(key) {
    if (!kValidRequestPseudoHeaders.has(key))
      throw H2_ERR('"' + key + '" is an invalid pseudoheader or is used incorrectly', "ERR_HTTP2_INVALID_PSEUDOHEADER");
  }
  function assertValidResponsePseudoHeader(key) {
    if (key !== ":status")
      throw H2_ERR('"' + key + '" is an invalid pseudoheader or is used incorrectly', "ERR_HTTP2_INVALID_PSEUDOHEADER");
  }
  // HTTP field-values are byte strings: HTAB, visible ASCII, and obs-text are
  // allowed, while every other code point (including CR/LF and Unicode line
  // separators) must not reach HPACK. nghttp2 drops an invalid response field
  // rather than turning it into a response-splitting payload.
  const kInvalidResponseFieldValue = /[^\t\x20-\x7e\x80-\xff]/;
  function buildNgHeaders(map, validatePseudo, strictSingleValueFields, dropInvalidFieldValues) {
    // node initializeOptions defaults options.strictSingleValueFields to true;
    // only an explicit `false` turns the single-value check off.
    if (strictSingleValueFields === undefined || strictSingleValueFields === null) strictSingleValueFields = true;
    const pseudoHeaders = [];
    const headers = [];
    const singles = new Set();
    const sensitive = sensitiveNamesOf(map);
    const processHeader = (rawKey, value) => {
      const key = String(rawKey).toLowerCase();
      const isStrictSingleValueField = !!strictSingleValueFields && kSingleValueFields.has(key);
      let isArray = Array.isArray(value);
      if (isArray) {
        if (value.length === 0) return;
        if (value.length === 1) { value = String(value[0]); isArray = false; }
        else if (isStrictSingleValueField) throw H2_ERR('Header field "' + key + '" must only have a single value', "ERR_HTTP2_HEADER_SINGLE_VALUE");
      } else value = String(value);
      // Outgoing response fields follow nghttp2's sanitizing path. Keep this
      // opt-in because request validation reports malformed user input instead
      // of silently rewriting it.
      if (dropInvalidFieldValues) {
        if (isArray) {
          value = value.map(String).filter((entry) => !kInvalidResponseFieldValue.test(entry));
          if (value.length === 0) return;
        } else if (kInvalidResponseFieldValue.test(value)) {
          return;
        }
      }
      if (isStrictSingleValueField) {
        if (singles.has(key)) throw H2_ERR('Header field "' + key + '" must only have a single value', "ERR_HTTP2_HEADER_SINGLE_VALUE");
        singles.add(key);
      }
      if (key[0] === ":") { validatePseudo(key); pseudoHeaders.push([key, value]); return; }
      if (!H2_TOKEN_RE.test(key)) throw H2_ERR('Header name must be a valid HTTP token ["' + rawKey + '"]', "ERR_INVALID_HTTP_TOKEN");
      if (isIllegalConnectionSpecificHeader(key, value))
        throw H2_ERR('HTTP/1 Connection specific headers are forbidden: "' + key + '"', "ERR_HTTP2_INVALID_CONNECTION_HEADERS");
      if (isArray) { for (const v of value) headers.push([key, String(v)]); return; }
      headers.push([key, value]);
    };
    // node buildNgHeaderString takes EITHER a header object or a flat
    // [name, value, name, value, …] array. The array form exists precisely to
    // keep the caller's order and duplicate fields verbatim, so it must not be
    // funnelled through Object.keys (which would turn `a,b … a,c` into one key
    // and expose the indices as header names).
    if (Array.isArray(map)) {
      for (let i = 0; i < map.length; i += 2) {
        const key = map[i];
        const value = map[i + 1];
        if (value === undefined || key === "") continue;
        processHeader(key, value);
      }
    } else {
      for (const key of Object.keys(map)) {
        const value = map[key];
        if (value === undefined || key === "") continue;
        processHeader(key, value);
      }
    }
    return { list: pseudoHeaders.concat(headers), sensitive };
  }
  // [[name, value], …] → node's flat raw-header array (the 3rd argument of
  // 'response'/'headers'/'trailers'/'push' and the 4th of the server's 'stream').
  function rawFromList(list) {
    const raw = [];
    for (let i = 0; i < list.length; i++) raw.push(list[i][0], list[i][1]);
    return raw;
  }
  // node Http2Stream#sentHeaders getter for a stream whose headers were given as
  // a raw array: the ORIGINAL key case is kept and a repeated field collapses
  // into an array of its values, in order.
  function rawToHeaderObject(rawHeaders) {
    const obj = { __proto__: null };
    for (let i = 0; i < rawHeaders.length; i += 2) {
      const key = rawHeaders[i];
      const value = rawHeaders[i + 1];
      const existing = obj[key];
      if (existing === undefined) obj[key] = value;
      else if (Array.isArray(existing)) existing.push(value);
      else obj[key] = [existing, value];
    }
    return copySensitiveTo(rawHeaders, obj);
  }
  // node prepareRequestHeadersObject: defaults :method/:authority/:scheme/:path
  // and enforces the CONNECT-specific pseudo-header rules.
  function buildHeaderList(headers, scheme, authorityName, strictSingleValueFields) {
    const obj = copySensitiveTo(headers, Object.assign({ __proto__: null }, headers));
    if (obj[":method"] === undefined) obj[":method"] = "GET";
    const connect = obj[":method"] === "CONNECT";
    if (!connect || obj[":protocol"] !== undefined) {
      if (obj[":authority"] === undefined && obj["host"] === undefined) obj[":authority"] = authorityName;
      if (obj[":scheme"] === undefined) obj[":scheme"] = scheme;
      if (obj[":path"] === undefined) obj[":path"] = "/";
    } else {
      if (obj[":authority"] === undefined) throw H2_ERR(":authority header is required for CONNECT requests", "ERR_HTTP2_CONNECT_AUTHORITY", Error);
      if (obj[":scheme"] !== undefined) throw H2_ERR("The :scheme header is forbidden for CONNECT requests", "ERR_HTTP2_CONNECT_SCHEME", Error);
      if (obj[":path"] !== undefined) throw H2_ERR("The :path header is forbidden for CONNECT requests", "ERR_HTTP2_CONNECT_PATH", Error);
    }
    const built = buildNgHeaders(obj, assertValidRequestPseudoHeader, strictSingleValueFields);
    built.prepared = obj;
    built.method = obj[":method"];
    return built;
  }
  // node prepareRequestHeadersArray: the same pseudo-header defaulting, except the
  // defaults are PREPENDED to the caller's array instead of merged into an object,
  // so the raw order the peer observes is `defaults… , caller's fields…`.
  function buildHeaderListArray(headers, scheme, authorityName, strictSingleValueFields) {
    let method, sch, authority, path, protocol;
    for (let i = 0; i < headers.length; i += 2) {
      const name = String(headers[i]);
      if (name[0] !== ":") continue;
      const h = name.toLowerCase();
      const v = headers[i + 1];
      if (h === ":method") method = v;
      else if (h === ":scheme") sch = v;
      else if (h === ":authority") authority = v;
      else if (h === ":path") path = v;
      else if (h === ":protocol") protocol = v;
    }
    const add = [];
    if (method === undefined) { method = "GET"; add.push(":method", method); }
    const connect = method === "CONNECT";
    if (!connect || protocol !== undefined) {
      // `headers["host"]` on an array is always undefined; node reads it anyway
      // (lib/internal/http2/util.js), so the array form always adds :authority.
      if (authority === undefined && headers["host"] === undefined) { authority = authorityName; add.push(":authority", authority); }
      if (sch === undefined) { sch = scheme; add.push(":scheme", sch); }
      if (path === undefined) add.push(":path", "/");
    } else {
      if (authority === undefined) throw H2_ERR(":authority header is required for CONNECT requests", "ERR_HTTP2_CONNECT_AUTHORITY", Error);
      if (sch !== undefined) throw H2_ERR("The :scheme header is forbidden for CONNECT requests", "ERR_HTTP2_CONNECT_SCHEME", Error);
      if (path !== undefined) throw H2_ERR("The :path header is forbidden for CONNECT requests", "ERR_HTTP2_CONNECT_PATH", Error);
    }
    // concat() drops symbol properties, so the sensitive-header marker has to be
    // carried across explicitly.
    const rawHeaders = add.length ? copySensitiveTo(headers, add.concat(headers)) : headers;
    const built = buildNgHeaders(rawHeaders, assertValidRequestPseudoHeader, strictSingleValueFields);
    built.rawHeaders = rawHeaders;
    built.prepared = rawToHeaderObject(rawHeaders);
    built.method = method;
    return built;
  }
  function headerListToObject(list, sensitive, dropPaddedFields) {
    // node toHeaderObject(): `ObjectCreate(null)`. It is not cosmetic — a peer
    // may legitimately send `constructor:` or `__proto__:` header fields, and on
    // an ordinary object the first reads back Object's constructor and the second
    // is swallowed by the prototype setter (test-http2-multiheaders asserts both).
    const obj = { __proto__: null };
    const raw = [];
    for (let i = 0; i < list.length; i++) {
      let name = list[i][0]; const value = list[i][1];
      raw.push(name, value);
      // strictFieldWhitespaceValidation (default on): a field value padded with
      // SP/HTAB is invalid per RFC 9113 8.2.1 and is dropped rather than
      // delivered (node's option turns the drop off, not the validity).
      if (dropPaddedFields && name[0] !== ":" && kPaddedFieldValue.test(value)) continue;
      if (name === ":status") { obj[name] = parseInt(value, 10); continue; }
      // node toHeaderObject: set-cookie is always an array, a repeated cookie
      // field is joined with "; " (RFC 7540 8.1.2.5), any other repeated field
      // with ", ", and a repeated single-value field keeps the first value.
      if (obj[name] === undefined) { obj[name] = name === "set-cookie" ? [value] : value; continue; }
      if (kSingleValueFields.has(name)) continue;
      if (name === "cookie") { obj[name] = obj[name] + "; " + value; continue; }
      if (name === "set-cookie") { obj[name].push(value); continue; }
      obj[name] = obj[name] + ", " + value;
    }
    // node toHeaderObject ends with a plain `obj[kSensitiveHeaders] = …`, i.e. an
    // ENUMERABLE own symbol — assert.deepStrictEqual compares own enumerable
    // symbol keys, so a received header object that hides it can never deep-equal
    // the `{ [sensitiveHeaders]: [] }` the corpus expects. Only the *effective*
    // symbol (the one http2.sensitiveHeaders resolves to) is enumerable; the
    // legacy alias stays hidden so exactly one symbol key is observable.
    const effective = sensitiveSymbols[sensitiveSymbols.length - 1];
    for (let i = 0; i < sensitiveSymbols.length; i++)
      try {
        Object.defineProperty(obj, sensitiveSymbols[i], {
          value: sensitive ? sensitive.slice() : [],
          enumerable: sensitiveSymbols[i] === effective, configurable: true, writable: true,
        });
      } catch (e) {}
    return obj;
  }
)JS";

export inline const std::string kHttp2JS =
    std::string{kHttp2JS_part1}.append(kHttp2JS_part2);

}  // namespace mbun::jsc::js_http2
