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

namespace mbun::jsc::js_http2 {

export constexpr std::string_view kHttp2JS = R"JS(
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
  if (typeof Promise.withResolvers !== "function") {
    Promise.withResolvers = function () { let resolve, reject; const promise = new Promise((res, rej) => { resolve = res; reject = rej; }); return { promise, resolve, reject }; };
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
  const sessionErr = (code) => mkErr("Session closed with error code " + errName(code), "ERR_HTTP2_SESSION_ERROR");
  const streamErr = (code) => mkErr("Stream closed with error code " + errName(code), "ERR_HTTP2_STREAM_ERROR");
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
      parts.push(sensitive && sensitive.has(list[i][0]) ? 0x10 : 0x00);
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
    const buf = payload ? Buffer.from(payload) : Buffer.alloc(8);
    if (!payload) for (let i = 0; i < 8; i++) buf[i] = (Math.random() * 256) | 0;
    if (session.connecting || session.closed) {
      G.queueMicrotask(() => cb(mkErr("HTTP2 ping cancelled", "ERR_HTTP2_PING_CANCEL")));
      return;
    }
    if (!session._pings) session._pings = [];
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
  function sessionPendingAck(session) { return (session._pendingSettingsAcks ? session._pendingSettingsAcks.length : 0) > 0; }
  function sessionSubmitSettings(session, settings, callback) {
    if (session.destroyed) throw mkErr("The session has been destroyed", "ERR_HTTP2_INVALID_SESSION");
    assertIsObject(settings, "settings");
    validateSettings(settings);
    if (callback !== undefined && callback !== null && typeof callback !== "function")
      throw argTypeErr("callback", "of type function", callback);
    const copy = Object.assign({}, settings);
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
    const snapshot = Object.assign({}, eff);
    if (typeof p.cb === "function") { try { p.cb(null, snapshot, Date.now() - p.start); } catch (e) {} }
    session.emit("localSettings", snapshot);
  }
  function resolvePing(session, payload) {
    const pings = session._pings;
    if (!pings || !pings.length) return;
    const key = Buffer.from(payload).toString("hex");
    let idx = pings.findIndex((p) => p.key === key);
    if (idx < 0) idx = 0;   // a peer that echoes a different payload still acks
    const p = pings.splice(idx, 1)[0];
    try { p.cb(null, Date.now() - p.start, p.payload); } catch (e) {}
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
  const kQuotedString = /^[\x21\x23-\x5b\x5d-\x7e\x80-\xff]*$/;
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
  function handleOriginFrame(session, payload) {
    const origins = [];
    let off = 0;
    while (off + 2 <= payload.length) {
      const n = payload.readUInt16BE(off);
      if (off + 2 + n > payload.length) break;
      origins.push(payload.toString("latin1", off + 2, off + 2 + n));
      off += 2 + n;
    }
    // node onOrigin: the origin set is only tracked for an encrypted session.
    if (session.encrypted) {
      if (session.originSet === undefined) session.originSet = [];
      for (const o of origins) if (session.originSet.indexOf(o) < 0) session.originSet.push(o);
    }
    session.emit("origin", origins);
    return true;
  }

  // Http2Stream#priority (node lib/internal/http2/core.js). Priority signalling
  // was deprecated by RFC 9113 and nghttp2 stopped honouring it in 1.65, so
  // node's method is a `deprecate()` wrapper whose body only rejects a destroyed
  // stream — it does not emit a PRIORITY frame and the peer never fires
  // 'priority'. Reproduce that exactly, warning included (DEP0194), because the
  // corpus asserts both the warning and that 'priority' is NOT emitted.
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
    G.queueMicrotask(cb);
  }
  function http2StreamWritev(stream, chunks, cb) {
    if (stream._endStreamSent) { G.queueMicrotask(cb); return; }
    const parts = [];
    for (let i = 0; i < chunks.length; i++) parts.push(bufFromChunk(chunks[i].chunk, chunks[i].encoding));
    stream.session._sendData(stream, Buffer.concat(parts), false);
    G.queueMicrotask(cb);
  }
  // The writable side finished. Without waitForTrailers that is an empty
  // DATA(END_STREAM); with it, END_STREAM is held back until sendTrailers()
  // (node kWaitForTrailers -> 'wantTrailers' -> sendTrailers()).
  function http2StreamFinal(stream, waitForTrailers, cb) {
    if (stream._endStreamSent) { cb(); maybeFinishHttp2Stream(stream); return; }
    if (waitForTrailers) {
      stream._trailersReady = true;
      stream.emit("wantTrailers");
      cb();
      return;
    }
    stream.session._sendData(stream, Buffer.alloc(0), true);
    stream._endStreamSent = true;
    cb();
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
    if (cb !== undefined && cb !== null && typeof cb !== "function") throw argTypeErr("callback", "of type function", cb);
    if (typeof cb === "function") stream.once("close", cb);
    if (stream._closed) return;
    stream._closed = true;
    stream.rstCode = code;
    const finish = () => {
      try { stream.session._rstStream(stream, code); } catch (e) {}
      try { stream.session.streams.delete(stream.id); } catch (e) {}
      // node: an rst with a non-zero code surfaces as an 'error' on the stream.
      if (code !== constants.NGHTTP2_NO_ERROR) {
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
      stream.rstCode = err ? constants.NGHTTP2_INTERNAL_ERROR : (stream.rstCode || constants.NGHTTP2_NO_ERROR);
      try { stream.session._rstStream(stream, stream.rstCode); } catch (e) {}
    }
    try { stream.session.streams.delete(stream.id); } catch (e) {}
    const sess = stream.session;
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
    G.queueMicrotask(() => { if (!stream.destroyed) stream.destroy(); });
  }
  function http2StreamFinish(stream) {
    if (stream._finished) return;
    stream._finished = true;
    stream._closed = true;
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
  function http2StreamSetTimeout(stream, ms, cb) {
    if (typeof cb === "function") stream.once("timeout", cb);
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
        return "Http2Stream " + (util && util.inspect ? util.inspect(obj) : String(obj));
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
      if (this._trailersSent) throw mkErr("Trailers have already been sent", "ERR_HTTP2_TRAILERS_ALREADY_SENT");
      if (!this._trailersReady) throw mkErr("Trailers are not ready to send", "ERR_HTTP2_TRAILERS_NOT_READY");
      headers = headers || {};
      for (const k of Object.keys(headers))
        if (String(k)[0] === ":") throw mkErr('"' + k + '" is an invalid pseudoheader or is used incorrectly', "ERR_HTTP2_INVALID_PSEUDOHEADER");
      this._trailersSent = true;
      this.sentTrailers = headers;
      const built = buildNgHeaders(Object.assign({ __proto__: null }, headers), assertValidRequestPseudoHeader, this.session._options && this.session._options.strictSingleValueFields);
      writeHeaderBlock(this.session, this.id, encodeHeaders(built.list, built.sensitive), FLAG.END_STREAM);
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
      this._destroyPending = false;
      // node Http2Session: `encrypted` reflects the transport, `alpnProtocol` is
      // "h2c" for a cleartext session, and `originSet` stays undefined until an
      // ORIGIN frame arrives (DEFERRED).
      this.alpnProtocol = null;
      this.encrypted = false;
      this.originSet = undefined;
      if (typeof listener === "function") this.once("connect", listener);

      const u = parseAuthority(authority, options);
      this._url = u.origin;
      this._authorityName = u.host;
      this._scheme = u.protocol === "https:" ? "https" : "http";
      const port = u.port ? +u.port : (this._scheme === "https" ? 443 : 80);
      const host = u.hostname;

      const self = this;
      // node http2.connect({ createConnection }): the caller supplies the
      // transport (a real socket, a tunnel, or a Duplex pair in the corpus) and
      // the session must not dial out itself.
      if (options && typeof options.createConnection === "function") {
        const sock = options.createConnection(u, options);
        this.socket = sock;
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
        const sock = tls.connect({
          host, port, servername: options && options.servername ? options.servername : host,
          ALPNProtocols: ["h2"], ca: options && options.ca, cert: options && options.cert,
          key: options && options.key, rejectUnauthorized: options && options.rejectUnauthorized !== undefined ? options.rejectUnauthorized : true,
        });
        this.socket = sock;
        sock.on("secureConnect", () => { self.alpnProtocol = sock.alpnProtocol || "h2"; self._onSocketReady(); });
        sock.on("data", (d) => self._onData(d));
        sock.on("error", (e) => self._onSocketError(e));
        sock.on("close", () => self._onSocketClose());
        sock.on("end", () => self._onSocketEnd());
      } else {
        const sock = new net.Socket();
        this.socket = sock;
        sock.on("connect", () => { self.alpnProtocol = "h2c"; self._onSocketReady(); });
        sock.on("data", (d) => self._onData(d));
        sock.on("error", (e) => self._onSocketError(e));
        sock.on("close", () => self._onSocketClose());
        sock.on("end", () => self._onSocketEnd());
        sock.connect(port, host);
      }
    }

    _onSocketReady() {
      if (this._connected || this.destroyed) return;
      this._connected = true;
      this.connecting = false;
      // The connection preface (RFC 7540 3.5) MUST precede every other frame.
      // request() can be called synchronously before the socket connects, so any
      // HEADERS it produced were buffered in _preConnectQ; flush them AFTER the
      // preface + our SETTINGS.
      this.socket.write(CLIENT_PREFACE);
      // http2.connect(authority, { settings }) has to reach the wire: the peer
      // reads ENABLE_PUSH from it to decide whether it may push at all, and the
      // corpus asserts the server sees `remoteSettings.enablePush === false`.
      // This was an unconditionally empty payload, so every configured client
      // setting was silently dropped.
      const initial = Object.assign({}, this._localSettings, this._options && this._options.settings);
      this._writeFrame(FRAME.SETTINGS, 0, 0, encodeSettings(initial));
      const q = this._preConnectQ; this._preConnectQ = [];
      for (let i = 0; i < q.length; i++) this.socket.write(q[i]);
      this.emit("connect", this, this.socket);
    }

    _writeFrame(type, flags, streamId, payload) {
      if (this.destroyed || !this.socket) return;
      payload = payload || Buffer.alloc(0);
      const frame = Buffer.concat([frameHeader(payload.length, type, flags, streamId), payload]);
      if (!this._connected) { this._preConnectQ.push(frame); return; }
      // See the server session's _writeFrame: net.Socket.write() on an ended or
      // destroyed socket EMITS 'error' instead of throwing, so this try/catch
      // cannot contain it. nghttp2 drops frames once the transport is gone.
      if (this.socket.destroyed || this.socket.writable === false) return;
      try { this.socket.write(frame); } catch (e) { if (!this.closed && !this.destroyed) this._fatal(e); }
    }

    request(headers, options) {
      if (this.destroyed) throw mkErr("The session has been destroyed", "ERR_HTTP2_INVALID_SESSION");
      headers = headers || {};
      options = options || {};
      // node reads these option getters synchronously while building the request
      // frame (lib/internal/http2/core.js). Some tests assert the getters fire
      // during request(); read them up front so user side effects run in order.
      const optPadding = options.paddingStrategy;
      const optExclusive = options.exclusive;
      const optParent = options.parent;
      const optWeight = options.weight;
      const optWaitTrailers = options.waitForTrailers;
      const optEndStream = options.endStream === true;
      // Header validation runs BEFORE the stream id is consumed: node throws
      // out of request() for a malformed header block and no stream is opened.
      const rawForm = Array.isArray(headers);
      const built = rawForm
        ? buildHeaderListArray(headers, this._scheme, this._authorityName, this._options.strictSingleValueFields)
        : buildHeaderList(headers, this._scheme, this._authorityName, this._options.strictSingleValueFields);
      const streamId = this._nextStreamId();
      const stream = new ClientHttp2Stream(this, streamId, headers, options);
      this.streams.set(streamId, stream);
      const block = encodeHeaders(built.list, built.sensitive);
      // node maxSendHeaderBlockLength: nghttp2 refuses to serialise a header
      // block over the limit, which surfaces as 'frameError' on the stream plus
      // a REFUSED_STREAM stream error — nothing goes on the wire.
      const maxBlock = this._options.maxSendHeaderBlockLength;
      if (maxBlock !== undefined && block.length > maxBlock) {
        stream.pending = false;
        this.streams.delete(streamId);
        stream.rstCode = constants.NGHTTP2_REFUSED_STREAM;
        stream._closed = true;
        const self = this;
        G.queueMicrotask(() => {
          stream.emit("frameError", FRAME.HEADERS, constants.NGHTTP2_FRAME_SIZE_ERROR, streamId);
          stream.destroy(streamErr(constants.NGHTTP2_REFUSED_STREAM));
          self._fatal(sessionErr(constants.NGHTTP2_FRAME_SIZE_ERROR));
        });
        return stream;
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
      const method = built.method === undefined ? "GET" : String(built.method);
      const noBody = /^(GET|HEAD|DELETE)$/.test(method);
      const endStream = options.endStream === undefined ? noBody : options.endStream === true;
      writeHeaderBlock(this, streamId, block, endStream ? FLAG.END_STREAM : 0);
      // node requestOnConnect: "Close the writable side of the stream if
      // options.endStream is set." A GET/HEAD/DELETE request therefore starts
      // with an already-finished writable side.
      if (endStream) { stream._endStreamSent = true; stream.end(); }
      stream.pending = false;
      return stream;
    }

    _nextStreamId() {
      if (!this._lastStreamId) this._lastStreamId = -1;
      this._lastStreamId += 2;
      if (this._lastStreamId < 1) this._lastStreamId = 1;
      return this._lastStreamId;
    }
    setNextStreamID(id) {
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
          const settings = this._parseSettings(payload);
          this._remoteSettings = settings;
          this._writeFrame(FRAME.SETTINGS, FLAG.ACK, 0, Buffer.alloc(0));   // ack
          this.emit("remoteSettings", settingsToObject(settings));
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
            stream.aborted = true;
            if (code !== 0) {
              const err = streamErr(code);
              // node abort(stream): a stream reset before its readable side ended
              // is 'aborted', and only then the error/close pair.
              G.queueMicrotask(() => {
                if (!stream.readableEnded) stream.emit("aborted");
                if (!stream.destroyed) stream.destroy(err);
              });
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
        if (pb.endStream) { this.streams.delete(pb.streamId); stream._onEnd(); }
        return true;
      }
      // A header block after the response is a trailer block (RFC 9113 8.1):
      // emit 'trailers' rather than a second 'response'. A 1xx block BEFORE the
      // response is informational: node emits 'headers', and the real response
      // still follows (lib/internal/http2/core.js onSessionHeaders).
      const st = headersObj[":status"];
      if (stream._responseEmitted) stream.emit("trailers", headersObj, flags, rawHeaders);
      else if (typeof st === "number" && st >= 100 && st < 200) {
        stream.emit("headers", headersObj, flags, rawHeaders);
        // node ClientHttp2Stream handleHeaderContinue: a 100 informational
        // response is additionally surfaced as 'continue'.
        if (st === 100) stream.emit("continue");
      }
      else stream._onResponse(headersObj, flags, rawHeaders);
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
    _connError(code) {
      // send GOAWAY then surface ERR_HTTP2_SESSION_ERROR (matches nghttp2).
      const p = Buffer.alloc(8); p.writeUInt32BE(this._lastStreamId > 0 ? this._lastStreamId : 0, 0); p.writeUInt32BE(code >>> 0, 4);
      try { this._writeFrame(FRAME.GOAWAY, 0, 0, p); } catch (e) {}
      this._fatal(sessionErr(code));
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
        for (const s of streams) { s.rstCode = constants.NGHTTP2_INTERNAL_ERROR; s._closed = true; if (!s.destroyed) s.destroy(err); else s.emit("error", err); }
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
        try { if (this.socket && typeof this.socket._poll === "function") this.socket._poll(); } catch (e) {}
        this._drainingSocket = false;
        // A drained GOAWAY/FIN can have torn the session down already.
        if (this.destroyed) return;
      }
      this.destroyed = true; this.closed = true;
      if (this._timer != null) { try { G.clearTimeout(this._timer); } catch (e) {} this._timer = null; }
      closeSessionSocket(this.socket, hard !== false);
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
      const self = this;
      G.queueMicrotask(() => {
        for (const s of pending) {
          if (s.destroyed) continue;
          s._closed = true;
          s.destroy(streamCancelErr());
        }
        self.emit("close");
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
    ref() { if (this.socket && this.socket.ref) this.socket.ref(); return this; }
    unref() { if (this.socket && this.socket.unref) this.socket.unref(); return this; }
    // node: an inactivity timeout that emits 'timeout' on the session and on
    // every open stream (lib/internal/http2/core.js Http2Session.setTimeout ->
    // #onTimeout -> forEachStream(emitTimeout)). Reset on inbound activity.
    setTimeout(ms, cb) {
      if (typeof cb === "function") this.on("timeout", cb);
      this._timeoutMs = ms | 0;
      this._armTimeout();
      return this;
    }
    _armTimeout() {
      if (this._timer != null) { try { G.clearTimeout(this._timer); } catch (e) {} this._timer = null; }
      if (!this._timeoutMs || this.destroyed) return;
      const self = this;
      this._timer = G.setTimeout(() => {
        self._timer = null;
        if (self.destroyed) return;
        self.emit("timeout");
        for (const s of Array.from(self.streams.values())) { try { s.emit("timeout"); } catch (e) {} }
        self._armTimeout();
      }, this._timeoutMs);
      if (this._timer && this._timer.unref) this._timer.unref();
    }
    get connected() { return this._connected; }
    get remoteSettings() { return this._remoteSettings ? settingsToObject(this._remoteSettings) : undefined; }
    get localSettings() { return this._localSettings; }
    // client streams are odd-numbered; _nextStreamId() advances _lastStreamId by 2
    get state() { return sessionState(this, this._lastStreamId > 0 ? this._lastStreamId + 2 : 1); }
    get pendingSettingsAck() { return sessionPendingAck(this); }
    get type() { return constants.NGHTTP2_SESSION_CLIENT; }
    settings(s, cb) { return sessionSubmitSettings(this, s, cb); }
    ping(payload, cb) { return sessionPing(this, payload, cb); }
    altsvc(alt, originOrStream) { return sessionAltsvc(this, alt, originOrStream); }
    origin(...origins) { return sessionOrigin(this, origins); }
    goaway(code, lastStreamId, opaqueData) { const p = Buffer.alloc(8); p.writeUInt32BE((lastStreamId || 0) >>> 0, 0); p.writeUInt32BE((code || 0) >>> 0, 4); this._writeFrame(FRAME.GOAWAY, 0, 0, opaqueData ? Buffer.concat([p, Buffer.from(opaqueData)]) : p); }
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
  function buildNgHeaders(map, validatePseudo, strictSingleValueFields) {
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
  function settingsToObject(s) {
    return {
      headerTableSize: s.headerTableSize, enablePush: !!s.enablePush,
      initialWindowSize: s.initialWindowSize, maxFrameSize: s.maxFrameSize,
      maxConcurrentStreams: s.maxConcurrentStreams, maxHeaderListSize: s.maxHeaderListSize,
      enableConnectProtocol: !!s.enableConnectProtocol,
    };
  }

  function connect(authority, options, listener) {
    if (typeof options === "function") { listener = options; options = undefined; }
    return new ClientHttp2Session(authority, options || {}, listener);
  }

  function getDefaultSettings() {
    return { headerTableSize: 4096, enablePush: true, initialWindowSize: 65535, maxFrameSize: 16384, maxConcurrentStreams: 4294967295, maxHeaderListSize: 65535, enableConnectProtocol: false };
  }

  // ==========================================================================
  // === SERVER half: createServer / createSecureServer over net/tls ==========
  // Blueprint: bun-ref src/js/node/http2.ts (Http2Server / Http2SecureServer /
  // ServerHttp2Session / ServerHttp2Stream / Http2ServerRequest/Response) but
  // driven by mbun's JS framing + HPACK (shared with the client above) instead
  // of the native nghttp2 binding. Server streams are peer-initiated (odd id
  // from the client); we decode request HEADERS, emit 'stream', and respond via
  // ServerHttp2Stream.respond()/write()/end() (HEADERS + DATA + flow control).
  // ==========================================================================

  // node argument validators (message templates: node lib/internal/errors.js).
  // determineSpecificType() is translated verbatim from lib/internal/errors.js
  // (and mirrored by test/common invalidArgTypeHelper, which the corpus uses to
  // build the expected message): a function reports its NAME, undefined reports
  // "undefined" (not "type undefined (undefined)"), an object reports its
  // constructor name, and -0/NaN/±Infinity each have their own spelling.
  function recvType(input) { return " Received " + determineSpecificType(input); }
  function determineSpecificType(value) {
    if (value === null) return "null";
    if (value === undefined) return "undefined";
    const type = typeof value;
    switch (type) {
      case "bigint": return "type bigint (" + value + "n)";
      case "number":
        if (value === 0) return 1 / value === -Infinity ? "type number (-0)" : "type number (0)";
        if (value !== value) return "type number (NaN)";
        if (value === Infinity) return "type number (Infinity)";
        if (value === -Infinity) return "type number (-Infinity)";
        return "type number (" + value + ")";
      case "boolean": return value ? "type boolean (true)" : "type boolean (false)";
      case "symbol": return "type symbol (" + String(value) + ")";
      case "function": return "function " + value.name;
      case "object":
        if (value.constructor && "name" in value.constructor) return "an instance of " + value.constructor.name;
        return inspectShallow(value);
      case "string": {
        let s = value;
        if (s.length > 28) s = s.slice(0, 25) + "...";
        if (s.indexOf("'") === -1) return "type string ('" + s + "')";
        return "type string (" + JSON.stringify(s) + ")";
      }
      default: {
        let s = String(value);
        if (s.length > 28) s = s.slice(0, 25) + "...";
        return "type " + type + " (" + s + ")";
      }
    }
  }
  // util.inspect(value, { depth: -1 }) for the only case that reaches it: a
  // null-prototype object, which node renders as "[Object: null prototype] {}".
  function inspectShallow(value) {
    if (Array.isArray(value)) return "[Array]";
    return "[Object: null prototype] {}";
  }
  function argTypeErr(name, expected, value) {
    // node lib/internal/errors.js: a dotted name is a *property*, not an argument.
    const determiner = String(name).includes(".") ? "property" : "argument";
    const e = new TypeError('The "' + name + '" ' + determiner + " must be " + expected + "." + recvType(value));
    e.code = "ERR_INVALID_ARG_TYPE"; return e;
  }
  function outOfRangeErr(name, range, value) {
    const e = new RangeError('The value of "' + name + '" is out of range. It must be ' + range + '. Received ' + value);
    e.code = "ERR_OUT_OF_RANGE"; return e;
  }
  function invalidArgValue(name, value, reason) {
    const determiner = String(name).includes(".") ? "property" : "argument";
    const e = new TypeError("The " + determiner + " '" + name + "' " + (reason || "is invalid") + ". Received " + determineSpecificType(value));
    e.code = "ERR_INVALID_ARG_VALUE"; return e;
  }
  // RFC 9113 8.2/8.3 request-header validity for a server. Returns true if the
  // header list is malformed (CR/LF/NUL octet, a connection-specific field, or a
  // repeated / misplaced pseudo-header) and the stream must be reset.
  // RFC 9113 8.2.1: a field value must not contain any control character other
  // than HTAB, and a *pseudo*-header value must not contain SP either (nghttp2
  // rejects `:path` containing any byte <= 0x20, which is what
  // test-http2-client-unescaped-path walks through). A violation is a stream
  // PROTOCOL_ERROR, not something to pass to the application.
  const kBadFieldValue = /[\x00-\x08\x0a-\x1f\x7f]/;
  const kBadPseudoValue = /[\x00-\x20\x7f]/;
  // Leading or trailing SP/HTAB in a field value is invalid (RFC 9113 8.2.1).
  // node's `strictFieldWhitespaceValidation` (default on) drops such a field
  // instead of delivering it; turning it off keeps the raw value.
  const kPaddedFieldValue = /^[ \t]|[ \t]$/;
  function requestHeadersMalformed(list) {
    const seenPseudo = {};
    let sawRegular = false;
    for (let i = 0; i < list.length; i++) {
      const name = list[i][0], value = list[i][1];
      if (/[\r\n\0]/.test(name) || /[\r\n\0]/.test(value)) return true;
      if (name[0] === ":" ? kBadPseudoValue.test(value) : kBadFieldValue.test(value)) return true;
      if (name[0] === ":") {
        if (sawRegular) return true;              // pseudo after a regular field
        if (seenPseudo[name]) return true;        // repeated pseudo-header
        seenPseudo[name] = true;
      } else {
        sawRegular = true;
        const lk = name.toLowerCase();
        if (lk === "connection" || lk === "keep-alive" || lk === "proxy-connection" || lk === "transfer-encoding" || lk === "upgrade") return true;
        if (lk === "te" && value !== "trailers") return true;
      }
    }
    return false;
  }
  function validateNumber(value, name) {
    if (typeof value !== "number") throw argTypeErr(name, "of type number", value);
  }
  function validateUint32(value, name) {
    if (typeof value !== "number") throw argTypeErr(name, "of type number", value);
    if (!Number.isInteger(value)) throw outOfRangeErr(name, "an integer", value);
    if (value < 0 || value > 4294967295) throw outOfRangeErr(name, ">= 0 and <= 4294967295", value);
  }
  const isBufLike = (v) => Buffer.isBuffer(v) || ArrayBuffer.isView(v);

  // Build a server response header list (pseudo :status first). Returns
  // { list, sensitive:Set } — sensitive names flagged via http2.sensitiveHeaders.
  // node prepareResponseHeadersObject: a response may only carry the :status
  // pseudo-header, the status must be a valid (non-informational) code, and the
  // remaining fields go through the same buildNgHeaderString validation as a
  // request's.
  function buildResponseHeaderList(headers, strictSingleValueFields, informational) {
    if (headers !== undefined && headers !== null && (typeof headers !== "object" || Array.isArray(headers)))
      throw argTypeErr("headers", "an object", headers);
    const obj = copySensitiveTo(headers, Object.assign({ __proto__: null }, headers));
    if (informational) {
      // additionalHeaders(): only a 1xx (never 101) informational status.
      if (obj[":status"] != null) {
        const sc = obj[":status"] | 0;
        if (sc === 101) { const e = new Error("HTTP status code 101 (Switching Protocols) is forbidden in HTTP/2"); e.code = "ERR_HTTP2_STATUS_101"; throw e; }
        if (sc < 100 || sc >= 200) { const e = new RangeError("Invalid informational status code: " + obj[":status"]); e.code = "ERR_HTTP2_INVALID_INFO_STATUS"; throw e; }
        obj[":status"] = String(sc);
      }
      const infoBuilt = buildNgHeaders(obj, assertValidResponsePseudoHeader, strictSingleValueFields);
      infoBuilt.prepared = Object.assign({ __proto__: null }, obj);
      if (infoBuilt.prepared[":status"] !== undefined) infoBuilt.prepared[":status"] = parseInt(infoBuilt.prepared[":status"], 10);
      return infoBuilt;
    }
    // node validatePreparedResponseHeaders is deliberately stricter than HTTP/1:
    // a response status outside 200..599 is rejected outright.
    const status = (obj[":status"] | 0) || 200;
    if (status < 200 || status > 599) { const e = new RangeError("Invalid status code: " + status); e.code = "ERR_HTTP2_STATUS_INVALID"; throw e; }
    obj[":status"] = String(status);
    const built = buildNgHeaders(obj, assertValidResponsePseudoHeader, strictSingleValueFields);
    // node ServerHttp2Stream#sentHeaders reports the *numeric* status alongside
    // the fields the response actually carried.
    built.prepared = Object.assign({ __proto__: null }, obj, { ":status": status });
    return built;
  }

  // node prepareResponseHeadersArray: the raw form MUTATES the caller's array —
  // a missing :status is unshifted (as the NUMBER, which is why sentHeaders[':status']
  // is numeric here but a string when the caller supplied it) and the Date is
  // appended.
  function buildResponseHeaderListArray(headers, options, strictSingleValueFields) {
    let statusCode;
    let isDateSet = false;
    for (let i = 0; i < headers.length; i += 2) {
      const h = String(headers[i]).toLowerCase();
      if (h === ":status") statusCode = headers[i + 1] | 0;
      else if (h === "date") isDateSet = true;
    }
    if (!statusCode) { statusCode = 200; headers.unshift(":status", statusCode); }
    if (!isDateSet && (options.sendDate == null || options.sendDate)) headers.push("date", new Date().toUTCString());
    if (statusCode < 200 || statusCode > 599) {
      const e = new RangeError("Invalid status code: " + statusCode); e.code = "ERR_HTTP2_STATUS_INVALID"; throw e;
    }
    const built = buildNgHeaders(headers, assertValidResponsePseudoHeader, strictSingleValueFields);
    built.rawHeaders = headers;
    built.prepared = rawToHeaderObject(headers);
    return built;
  }

  // === ServerHttp2Stream ===
  class ServerHttp2Stream extends H2StreamBase {
    constructor(session, id, headers) {
      super(kStreamDuplexOptions);
      initHttp2Stream(this, session, id);
      this._serverSide = true;
      this._reqHeaders = headers;
      this.pending = false;
      this.headersSent = false;
      this._wantTrailers = false;
      this._trailersSent = false;
      this.sentHeaders = undefined;
      // node ServerHttp2Stream#headRequest: a HEAD request must not carry a
      // payload, which respondWithFile/FD check before opening anything.
      this.headRequest = !!(headers && typeof headers[":method"] === "string" && headers[":method"].toUpperCase() === "HEAD");
    }
    get closed() { return this._closed; }
    get state() { return streamState(this); }
    get bufferSize() { return http2StreamBufferSize(this); }
    // node ServerHttp2Stream#pushAllowed: the peer must have left SETTINGS
    // ENABLE_PUSH on and both stream and session must still be usable.
    get pushAllowed() {
      return !this.destroyed && !this._closed && !this.session.closed && !this.session.destroyed &&
        !!(this.session._remoteSettings ? this.session._remoteSettings.enablePush : constants.DEFAULT_SETTINGS_ENABLE_PUSH);
    }
    _write(chunk, enc, cb) { if (!this.headersSent) this.respond(); http2StreamWrite(this, chunk, enc, cb); }
    _writev(chunks, cb) { if (!this.headersSent) this.respond(); http2StreamWritev(this, chunks, cb); }
    _final(cb) { if (!this.headersSent) this.respond(); http2StreamFinal(this, this._wantTrailers, cb); }
    _read() { this._didRead = true; http2StreamRead(this); }
    _destroy(err, cb) { http2StreamDestroy(this, err, cb); }
    respond(headers, options) {
      if (this.headersSent) throw mkErr("Response has already been initiated.", "ERR_HTTP2_HEADERS_SENT");
      if (this.destroyed || this._closed) throw mkErr("The stream has been destroyed", "ERR_HTTP2_INVALID_STREAM");
      headers = headers || {};
      options = options || {};
      const strictSingle = this.session._options && this.session._options.strictSingleValueFields;
      let built;
      if (Array.isArray(headers)) {
        built = buildResponseHeaderListArray(headers, options, strictSingle);
      } else {
        // node ServerHttp2Stream.respond({ sendDate }) stamps a Date header unless
        // the response already carries one or sendDate was turned off.
        if (options.sendDate !== false && headers["date"] === undefined && headers["Date"] === undefined) {
          try { headers = Object.assign({ __proto__: null }, headers, { date: new Date().toUTCString() }); } catch (e) {}
        }
        built = buildResponseHeaderList(headers, strictSingle);
      }
      // node ServerHttp2Stream.respond: DATA frames are forbidden for 204/205/304
      // and for a HEAD request, so the HEADERS frame carries END_STREAM itself.
      const st = parseInt(built.list[0] && built.list[0][1], 10);
      if (st === 204 || st === 205 || st === 304 || this.headRequest === true) options = Object.assign({}, options, { endStream: true });
      this.headersSent = true;
      this.sentHeaders = built.prepared || headers;
      const block = encodeHeaders(built.list, built.sensitive);
      let flags = 0;
      const endStream = !!options.endStream;
      // node ServerHttp2Stream.respond passes STREAM_OPTION_EMPTY_PAYLOAD *and*
      // STREAM_OPTION_GET_TRAILERS, and nghttp2 lets the empty payload win: with
      // no data provider the HEADERS frame carries END_STREAM and 'wantTrailers'
      // never fires. The compat layer always asks for trailers, so treating
      // waitForTrailers as the stronger option left the response HEADERS without
      // END_STREAM — a HEAD/204/304 response then reported flags 4 instead of 5.
      if (endStream) { flags = FLAG.END_STREAM; this._endStreamSent = true; }
      else if (options.waitForTrailers) this._wantTrailers = true;
      writeHeaderBlock(this.session, this.id, block, flags);
      if (endStream) { this.end(); http2StreamFinish(this); }
      return;
    }
    sendTrailers(headers) {
      if (this._trailersSent) throw mkErr("Trailers have already been sent", "ERR_HTTP2_TRAILERS_ALREADY_SENT");
      if (!this._trailersReady) throw mkErr("Trailers are not ready to send", "ERR_HTTP2_TRAILERS_NOT_READY");
      headers = headers || {};
      // Validate BEFORE marking sent: a pseudo-header in a trailer block is an
      // ERR_HTTP2_INVALID_PSEUDOHEADER and must leave the stream retryable.
      for (const k in headers) { if (Object.prototype.hasOwnProperty.call(headers, k) && String(k)[0] === ":") throw mkErr('"' + k + '" is an invalid pseudoheader or is used incorrectly', "ERR_HTTP2_INVALID_PSEUDOHEADER"); }
      this._trailersSent = true;
      this.sentTrailers = headers;
      const sensitive = sensitiveNamesOf(headers);
      const list = [];
      for (const k in headers) { if (!Object.prototype.hasOwnProperty.call(headers, k)) continue; const lk = String(k).toLowerCase(); list.push([lk, String(headers[k])]); }
      const block = encodeHeaders(list, sensitive);
      writeHeaderBlock(this.session, this.id, block, FLAG.END_STREAM);
      this._endStreamSent = true;
      http2StreamFinish(this);
      return;
    }
    // respondWithFD / respondWithFile (node lib/internal/http2/core.js
    // ServerHttp2Stream.respondWithFD / .respondWithFile -> afterOpen ->
    // doSendFileFD -> processRespondWithFD). Both send the response HEADERS and
    // then the file body as DATA frames; options.statCheck may amend the headers
    // (and cancel the response by returning false), options.offset/length window
    // the body, and options.onError receives an open/stat failure instead of the
    // stream being destroyed. Content-Length is filled in from the stat size for
    // a regular file, matching node.
    respondWithFD(fd, headersParam, options) {
      this._prepareFileResponse(options, headersParam, "fd");
      options = options || {};
      const fs = M["fs"] || M["node:fs"];
      if (fd !== null && typeof fd === "object" && typeof fd.fd === "number") fd = fd.fd;
      if (typeof fd !== "number") throw argTypeErr("fd", "one of type number or FileHandle", fd);
      if (options.statCheck === undefined) { this._sendFd(fs, fd, headersParam, options, null, false); return; }
      let stat = null;
      try { stat = fs.fstatSync(fd); } catch (e) { this._fileError(options, e); return; }
      this._sendFd(fs, fd, headersParam, options, stat, false);
    }
    respondWithFile(path, headersParam, options) {
      this._prepareFileResponse(options, headersParam, "file");
      options = options || {};
      const fs = M["fs"] || M["node:fs"];
      let fd;
      try { fd = fs.openSync(path, "r"); } catch (e) { this._fileError(options, e); return; }
      let stat;
      try { stat = fs.fstatSync(fd); } catch (e) { try { fs.closeSync(fd); } catch (e2) {} this._fileError(options, e); return; }
      if (!stat.isFile()) {
        // node: a directory is ERR_HTTP2_SEND_FILE; anything else non-regular
        // with an explicit offset/length window is ERR_HTTP2_SEND_FILE_NOSEEK.
        const isDir = stat.isDirectory();
        const e = isDir
          ? mkErr("Directories cannot be sent", "ERR_HTTP2_SEND_FILE")
          : mkErr("Offset or length can only be specified for regular files", "ERR_HTTP2_SEND_FILE_NOSEEK");
        try { fs.closeSync(fd); } catch (e2) {}
        this._fileError(options, e);
        return;
      }
      this._sendFd(fs, fd, headersParam, options, stat, true);
    }
    _prepareFileResponse(options, headersParam, kind) {
      if (this.destroyed || this._closed) throw mkErr("The stream has been destroyed", "ERR_HTTP2_INVALID_STREAM");
      if (this.headersSent) throw mkErr("Response has already been initiated.", "ERR_HTTP2_HEADERS_SENT");
      if (options !== undefined && (options === null || typeof options !== "object" || Array.isArray(options)))
        throw argTypeErr("options", "an object", options);
      const o = options || {};
      if (o.offset !== undefined && typeof o.offset !== "number") throw invalidArgValue("options.offset", o.offset);
      if (o.length !== undefined && typeof o.length !== "number") throw invalidArgValue("options.length", o.length);
      if (o.statCheck !== undefined && typeof o.statCheck !== "function") throw invalidArgValue("options.statCheck", o.statCheck);
      // DATA frames are forbidden for these statuses / for a HEAD request.
      let status = 200;
      if (headersParam && headersParam[":status"] !== undefined) status = parseInt(headersParam[":status"], 10) || 200;
      if (status === 204 || status === 205 || status === 304 || this.headRequest)
        throw mkErr("Responses with " + status + " status must not have a payload", "ERR_HTTP2_PAYLOAD_FORBIDDEN");
    }
    _fileError(options, err) {
      if (options && typeof options.onError === "function") options.onError(err);
      else this.destroy(err);
    }
    _sendFd(fs, fd, headersParam, options, stat, ownsFd) {
      const headers = Object.assign({}, headersParam);
      if (stat && typeof options.statCheck === "function") {
        if (options.statCheck.call(this, stat, headers, { offset: options.offset !== undefined ? options.offset : 0, length: options.length !== undefined ? options.length : -1 }) === false) {
          if (ownsFd) { try { fs.closeSync(fd); } catch (e) {} }
          return;
        }
        if (this.headersSent) { if (ownsFd) { try { fs.closeSync(fd); } catch (e) {} } return; }
      }
      const offset = options.offset !== undefined ? (options.offset | 0) : 0;
      let length = options.length !== undefined ? (options.length | 0) : -1;
      if (stat) {
        length = length < 0 ? stat.size - offset : Math.min(stat.size - offset, length);
        headers["content-length"] = length;
      }
      let body;
      try {
        if (length < 0) {
          const parts = []; const tmp = Buffer.alloc(65536);
          let pos = offset, n;
          while ((n = fs.readSync(fd, tmp, 0, tmp.length, pos)) > 0) { parts.push(Buffer.from(tmp.subarray(0, n))); pos += n; }
          body = Buffer.concat(parts);
        } else {
          body = Buffer.alloc(length);
          if (length > 0) {
            const n = fs.readSync(fd, body, 0, length, offset);
            if (n < length) body = body.subarray(0, n < 0 ? 0 : n);
          }
        }
      } catch (e) { if (ownsFd) { try { fs.closeSync(fd); } catch (e2) {} } this._fileError(options, e); return; }
      if (ownsFd) { try { fs.closeSync(fd); } catch (e) {} }
      this.respond(headers, options.waitForTrailers ? { waitForTrailers: true } : undefined);
      this.end(body);
    }
    // node ServerHttp2Stream#pushStream (lib/internal/http2/core.js). The
    // PUSH_PROMISE frame (RFC 9113 6.6) is sent on THIS stream and announces an
    // even-numbered server-initiated stream carrying the request the server is
    // answering unasked; the callback receives that new stream, on which the
    // application then calls respond()/end() exactly as for a normal one.
    pushStream(headers, options, callback) {
      if (typeof options === "function") { callback = options; options = undefined; }
      if (!this.pushAllowed) throw mkErr("Push stream is not allowed", "ERR_HTTP2_PUSH_DISABLED");
      if ((this.id % 2) === 0) throw mkErr("A push stream cannot initiate another push stream.", "ERR_HTTP2_NESTED_PUSH");
      if (typeof callback !== "function") throw argTypeErr("callback", "of type function", callback);
      assertIsObject(options, "options");
      const opts = Object.assign({}, options);
      opts.endStream = !!opts.endStream;
      assertIsObject(headers, "headers");
      const h = Object.assign({ __proto__: null }, headers);
      const parent = this._reqHeaders || {};
      if (h[":method"] === undefined) h[":method"] = constants.HTTP2_METHOD_GET;
      if (h[":authority"] === undefined && h["host"] === undefined) h[":authority"] = parent[":authority"] !== undefined ? parent[":authority"] : parent["host"];
      if (h[":scheme"] === undefined) h[":scheme"] = parent[":scheme"];
      if (h[":path"] === undefined) h[":path"] = "/";
      let headRequest = false;
      if (String(h[":method"]).toUpperCase() === "HEAD") headRequest = opts.endStream = true;
      const strict = this.session._options && this.session._options.strictSingleValueFields;
      const built = buildNgHeaders(h, assertValidRequestPseudoHeader, strict);
      const session = this.session;
      const id = session._nextPushId();
      if (id === 0) { G.queueMicrotask(() => callback(mkErr("Out of streams", "ERR_HTTP2_OUT_OF_STREAMS"))); return; }
      const block = encodeHeaders(built.list, built.sensitive);
      const promised = Buffer.alloc(4); promised.writeUInt32BE(id, 0);
      // PUSH_PROMISE splits into PUSH_PROMISE + CONTINUATION the same way
      // HEADERS does; the promised-id prefix rides on the first frame only.
      const max = (session._remoteSettings && session._remoteSettings.maxFrameSize) || 16384;
      if (4 + block.length <= max) {
        session._writeFrame(FRAME.PUSH_PROMISE, FLAG.END_HEADERS, this.id, Buffer.concat([promised, block]));
      } else {
        const first = block.subarray(0, max - 4);
        session._writeFrame(FRAME.PUSH_PROMISE, 0, this.id, Buffer.concat([promised, first]));
        let off = max - 4;
        while (off < block.length) {
          const end = Math.min(off + max, block.length);
          session._writeFrame(FRAME.CONTINUATION, end >= block.length ? FLAG.END_HEADERS : 0, this.id, block.subarray(off, end));
          off = end;
        }
      }
      const push = new ServerHttp2Stream(session, id, h);
      push.pushed = true;
      push.headRequest = headRequest;
      // A pushed stream has no request body, so its readable side is already at
      // EOF and `endAfterHeaders` is true.
      push.endAfterHeaders = true;
      session.streams.set(id, push);
      G.queueMicrotask(() => {
        push._onRequestEnd();
        callback(null, push, h);
      });
    }
    // node ServerHttp2Stream.additionalHeaders: an informational (1xx) HEADERS
    // block sent BEFORE the response headers; it does not open the response, so
    // headersSent stays false.
    additionalHeaders(headers) {
      if (this.destroyed || this._closed) throw mkErr("The stream has been destroyed", "ERR_HTTP2_INVALID_STREAM");
      if (this.headersSent) throw mkErr("Cannot specify additional headers after response initiated", "ERR_HTTP2_HEADERS_AFTER_RESPOND");
      const built = buildResponseHeaderList(headers || {}, this.session._options && this.session._options.strictSingleValueFields, true);
      writeHeaderBlock(this.session, this.id, encodeHeaders(built.list, built.sensitive), 0);
      // node Http2Stream#sentInfoHeaders: every 1xx block sent so far, in order.
      this.sentInfoHeaders.push(built.prepared || headers || {});
    }
    priority(options) { streamPriority(this); }
    close(code, cb) { http2StreamClose(this, code, cb); }
    setTimeout(ms, cb) { return http2StreamSetTimeout(this, ms, cb); }
    _pushData(bytes) { http2StreamPushData(this, bytes); }
    _onRequestEnd() { http2StreamEndReadable(this); }
    _finish() { http2StreamFinish(this); }
  }

  // === ServerHttp2Session ===
  class ServerHttp2Session extends EE {
    constructor(server, socket, options) {
      super();
      this.server = server;
      this.socket = socket;
      this._options = options || {};
      this.streams = new Map();
      this.destroyed = false;
      this.closed = false;
      this._recv = Buffer.alloc(0);
      this._prefaceRead = false;
      this._hpack = new HpackDecoder();
      this._maxFrameSize = constants.DEFAULT_SETTINGS_MAX_FRAME_SIZE;
      this._remoteSettings = null;
      this._localSettings = { headerTableSize: 4096, enablePush: 0, initialWindowSize: 65535, maxFrameSize: 16384, maxConcurrentStreams: 4294967295 };
      this._pendingHeaderBlock = null;
      this._lastStreamId = 0;
      // connection-level flow control bookkeeping, surfaced through `state`
      this._localWindow = DEFAULT_CONNECTION_WINDOW;
      this._remoteWindow = DEFAULT_CONNECTION_WINDOW;
      this._lastProcStreamId = 0;
      this.alpnProtocol = socket.alpnProtocol || null;
      const self = this;
      // A server may send its SETTINGS immediately (RFC 7540 3.5); the client's
      // preface can still be in flight. Apply any configured settings values.
      this._writeFrame(FRAME.SETTINGS, 0, 0, encodeSettings(server && server._h2options && server._h2options.settings));
      socket.on("data", (d) => self._onData(d));
      socket.on("error", (e) => self._onSocketError(e));
      socket.on("close", () => self._onSocketClose());
      socket.on("end", () => self._onSocketEnd());
    }
    _writeFrame(type, flags, streamId, payload) {
      if (this.destroyed || !this.socket) return;
      // net.Socket.write() on an ended/destroyed socket EMITS 'error' rather than
      // throwing, so the try/catch below cannot contain it: the session has no
      // 'error' listener on that path and "write after end" surfaced as an
      // uncaught exception. nghttp2 simply drops frames once the transport is
      // gone, so drop them here too.
      if (this.socket.destroyed || this.socket.writable === false) return;
      payload = payload || Buffer.alloc(0);
      try { this.socket.write(Buffer.concat([frameHeader(payload.length, type, flags, streamId), payload])); } catch (e) {}
    }
    _sendData(stream, buf, endStream) {
      if (this.destroyed) return;
      const max = (this._remoteSettings && this._remoteSettings.maxFrameSize) || 16384;
      if (buf.length === 0) { this._writeFrame(FRAME.DATA, endStream ? FLAG.END_STREAM : 0, stream.id, Buffer.alloc(0)); return; }
      let off = 0;
      this._remoteWindow -= buf.length;
      while (off < buf.length) {
        const end = Math.min(off + max, buf.length);
        const isLast = end >= buf.length;
        this._writeFrame(FRAME.DATA, (isLast && endStream) ? FLAG.END_STREAM : 0, stream.id, buf.subarray(off, end));
        off = end;
      }
    }
    _rstStream(stream, code) { const p = Buffer.alloc(4); p.writeUInt32BE(code >>> 0, 0); this._writeFrame(FRAME.RST_STREAM, 0, stream.id, p); }
    // Server-initiated (push) streams use even ids starting at 2 (RFC 9113 5.1.1).
    _nextPushId() { this._lastPushId = (this._lastPushId || 0) + 2; return this._lastPushId > 2147483647 ? 0 : this._lastPushId; }
    _windowUpdate(streamId, increment) { if (streamId === 0) this._localWindow += increment; const p = Buffer.alloc(4); p.writeUInt32BE(increment >>> 0, 0); this._writeFrame(FRAME.WINDOW_UPDATE, 0, streamId, p); }
    _strictWs() { return this._options.strictFieldWhitespaceValidation !== false; }
    setLocalWindowSize(windowSize) { sessionSetLocalWindowSize(this, windowSize); }

    _onData(chunk) {
      if (this.destroyed) return;
      this._recv = this._recv.length ? Buffer.concat([this._recv, chunk]) : Buffer.from(chunk);
      if (!this._prefaceRead) {
        if (this._recv.length < 24) return;
        for (let i = 0; i < 24; i++) { if (this._recv[i] !== CLIENT_PREFACE[i]) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return; } }
        this._prefaceRead = true;
        this._recv = this._recv.subarray(24);
      }
      this._parse();
    }
    _parse() {
      const buf = this._recv;
      let off = 0;
      while (buf.length - off >= 9) {
        const len = (buf[off] << 16) | (buf[off + 1] << 8) | buf[off + 2];
        const type = buf[off + 3];
        const flags = buf[off + 4];
        const streamId = ((buf[off + 5] & 0x7f) << 24) | (buf[off + 6] << 16) | (buf[off + 7] << 8) | buf[off + 8];
        if (len > this._maxFrameSize) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return; }
        if (buf.length - off < 9 + len) break;
        const payload = buf.subarray(off + 9, off + 9 + len);
        off += 9 + len;
        if (!this._handleFrame(type, flags, streamId, payload, len)) return;
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
          const rangeErr = settingsRangeError(payload);
          if (rangeErr) { this._connError(rangeErr); return false; }
          this._remoteSettings = parseSettingsPayload(payload);
          this._writeFrame(FRAME.SETTINGS, FLAG.ACK, 0, Buffer.alloc(0));
          this.emit("remoteSettings", settingsToObject(this._remoteSettings));
          return true;
        }
        case FRAME.HEADERS: {
          if (streamId === 0) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
          return this._handleHeaders(flags, streamId, payload);
        }
        case FRAME.CONTINUATION: {
          if (!this._pendingHeaderBlock || this._pendingHeaderBlock.streamId !== streamId) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
          this._pendingHeaderBlock.buf = Buffer.concat([this._pendingHeaderBlock.buf, payload]);
          if (flags & FLAG.END_HEADERS) return this._finishHeaders(flags);
          return true;
        }
        case FRAME.DATA: {
          if (streamId === 0) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
          const stream = this.streams.get(streamId);
          let data = payload;
          if (flags & FLAG.PADDED) {
            if (payload.length < 1) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
            const pad = payload[0];
            if (pad >= payload.length) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
            data = payload.subarray(1, payload.length - pad);
          }
          this._lastProcStreamId = streamId;
          this._localWindow -= len;
          if (stream) {
            // content-length accounting (RFC 9113 8.1.1): a body diverging from a
            // declared content-length is a stream PROTOCOL_ERROR.
            if (stream._expectLen != null) {
              stream._recvLen = (stream._recvLen || 0) + data.length;
              if (stream._recvLen > stream._expectLen) { this._streamError(stream, constants.NGHTTP2_PROTOCOL_ERROR); return true; }
              if ((flags & FLAG.END_STREAM) && stream._recvLen !== stream._expectLen) { this._streamError(stream, constants.NGHTTP2_PROTOCOL_ERROR); return true; }
            }
            if (data.length) stream._pushData(data);
            if (len > 0) { this._windowUpdate(0, len); this._windowUpdate(streamId, len); }
            if (flags & FLAG.END_STREAM) stream._onRequestEnd();
          }
          return true;
        }
        case FRAME.RST_STREAM: {
          if (streamId === 0) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
          if (len !== 4) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
          const code = payload.readUInt32BE(0);
          const stream = this.streams.get(streamId);
          if (stream) {
            stream.rstCode = code; stream.aborted = true;
            if (code !== 0) G.queueMicrotask(() => stream.emit("aborted"));
            stream._closed = true;
            http2StreamFinish(stream);
          }
          else if ((streamId & 1) === 1 && streamId > this._lastStreamId) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }  // RST on an idle stream (§5.1)
          else if ((streamId & 1) === 0 && streamId > (this._lastPushId || 0)) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
          return true;
        }
        case FRAME.GOAWAY: {
          if (streamId !== 0) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
          if (len < 8) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
          this.emit("goaway", payload.readUInt32BE(4), payload.readUInt32BE(0), payload.length > 8 ? Buffer.from(payload.subarray(8)) : undefined);
          this._teardown();
          return false;
        }
        case FRAME.PING: {
          if (len !== 8) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
          if (streamId !== 0) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
          if (!(flags & FLAG.ACK)) {
            this._writeFrame(FRAME.PING, FLAG.ACK, 0, Buffer.from(payload));
            // See the client session's PING case: 'ping' is emitted only for a
            // peer-initiated PING (bun http2.ts:5173).
            this.emit("ping", Buffer.from(payload));
          } else resolvePing(this, payload);
          return true;
        }
        case FRAME.WINDOW_UPDATE: {
          if (len !== 4) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
          const inc = payload.readUInt32BE(0) & 0x7fffffff;
          if (inc === 0 && streamId === 0) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }  // §6.9
          if (streamId === 0) this._remoteWindow += inc;
          return true;
        }
        case FRAME.PRIORITY: {
          if (len !== 5) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }  // §6.3 fixed length
          return true;
        }
        case FRAME.PUSH_PROMISE: { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }
        case FRAME.ALTSVC: return handleAltsvcFrame(this, streamId, payload);
        case FRAME.ORIGIN: return handleOriginFrame(this, payload);
        default: return true;
      }
    }
    _handleHeaders(flags, streamId, payload) {
      const data = payload;
      let off = 0, padLen = 0;
      if (flags & FLAG.PADDED) { if (data.length < 1) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; } padLen = data[0]; off = 1; }
      if (flags & FLAG.PRIORITY) { if (data.length < off + 5) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; } off += 5; }
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
      let stream = this.streams.get(pb.streamId);
      if (stream) {
        // A second header block on an open stream is a trailer block (RFC 9113
        // 8.1): it must carry END_STREAM and MUST NOT contain pseudo-headers.
        if (!pb.endStream) { this._streamError(stream, constants.NGHTTP2_PROTOCOL_ERROR); return true; }
        for (let i = 0; i < list.length; i++) { if (list[i][0][0] === ":") { this._streamError(stream, constants.NGHTTP2_PROTOCOL_ERROR); return true; } }
        if (stream._expectLen != null && (stream._recvLen || 0) !== stream._expectLen) { this._streamError(stream, constants.NGHTTP2_PROTOCOL_ERROR); return true; }
        // node surfaces the request trailer block on the stream before 'end'
        // (compat.js onStreamTrailers feeds req.trailers / req.rawTrailers).
        const rawTrailers = [];
        for (let i = 0; i < list.length; i++) rawTrailers.push(list[i][0], list[i][1]);
        stream.emit("trailers", headerListToObject(list, this._hpack._sensitive, this._strictWs()), flags, rawTrailers);
        stream._onRequestEnd();
        return true;
      }
      // Only client-initiated (odd) ids open request streams.
      if ((pb.streamId & 1) === 0) return true;   // even == server-initiated, N/A
      // content-length pre-validation (RFC 9113 8.1.1): a duplicate/invalid field,
      // or an empty body that declared a non-zero length, is a stream error.
      let expectLen = null, dupClen = false;
      for (let i = 0; i < list.length; i++) {
        if (list[i][0] === "content-length") {
          if (expectLen !== null) dupClen = true;
          const n = parseInt(list[i][1], 10);
          expectLen = isNaN(n) ? null : n;
        }
      }
      // Inbound resource limits, checked BEFORE the application is told the
      // stream exists (node hands these to nghttp2 as session options, so the
      // 'stream' event never fires for a request that trips one):
      //   * maxHeaderListPairs — nghttp2 max_header_pairs,
      //   * SETTINGS_MAX_HEADER_LIST_SIZE — the RFC 7541 4.1 header-list size,
      //     i.e. sum(name.length + value.length + 32),
      //   * SETTINGS_MAX_CONCURRENT_STREAMS.
      // The first two are ENHANCE_YOUR_CALM, the third REFUSED_STREAM.
      const openStreams = this.streams.size;
      const settings = this._options.settings || {};
      const maxPairs = this._options.maxHeaderListPairs;
      const maxListSize = settings.maxHeaderListSize !== undefined ? settings.maxHeaderListSize : settings.maxHeaderSize;
      const maxConcurrent = settings.maxConcurrentStreams;
      let limitCode = 0;
      if (maxPairs !== undefined && list.length > maxPairs) limitCode = constants.NGHTTP2_ENHANCE_YOUR_CALM;
      if (limitCode === 0 && maxListSize !== undefined) {
        let listSize = 0;
        for (let i = 0; i < list.length; i++) listSize += list[i][0].length + list[i][1].length + 32;
        if (listSize > maxListSize) limitCode = constants.NGHTTP2_ENHANCE_YOUR_CALM;
      }
      if (limitCode === 0 && maxConcurrent !== undefined && openStreams >= maxConcurrent) limitCode = constants.NGHTTP2_REFUSED_STREAM;
      stream = new ServerHttp2Stream(this, pb.streamId, null);
      // node Http2Stream#endAfterHeaders: the request carried END_STREAM on its
      // HEADERS frame, i.e. there is no request body to wait for.
      stream.endAfterHeaders = !!pb.endStream;
      this.streams.set(pb.streamId, stream);
      if (pb.streamId > this._lastStreamId) this._lastStreamId = pb.streamId;
      if (limitCode !== 0) { this._streamError(stream, limitCode); return true; }
      if (requestHeadersMalformed(list)) { this._streamError(stream, constants.NGHTTP2_PROTOCOL_ERROR); return true; }
      if (dupClen) { this._streamError(stream, constants.NGHTTP2_PROTOCOL_ERROR); return true; }
      if (pb.endStream && expectLen != null && expectLen !== 0) { this._streamError(stream, constants.NGHTTP2_PROTOCOL_ERROR); return true; }
      if (!pb.endStream && expectLen != null) stream._expectLen = expectLen;
      const headersObj = headerListToObject(list, this._hpack._sensitive, this._strictWs());
      stream._reqHeaders = headersObj;
      // The constructor cannot compute this: it is handed `null` for headers and
      // only learns them here. Left unset, respond() never applied the implicit
      // END_STREAM a HEAD response requires (the peer saw flags 4, not 5).
      stream.headRequest = typeof headersObj[":method"] === "string" && headersObj[":method"].toUpperCase() === "HEAD";
      const rawHeaders = [];
      for (let i = 0; i < list.length; i++) { rawHeaders.push(list[i][0], list[i][1]); }
      this.emit("stream", stream, headersObj, flags, rawHeaders);
      if (pb.endStream) G.queueMicrotask(() => stream._onRequestEnd());
      return true;
    }
    // A stream-level error: RST_STREAM the offending stream but keep the
    // connection alive (RFC 9113 5.4.2), then finish the stream object.
    _streamError(stream, code) {
      if (!stream || stream._closed) return;
      try { this._rstStream(stream, code); } catch (e) {}
      // Only surface 'error' if the user attached a handler (a stream rejected
      // before its 'stream' event has no listeners; emitting would throw).
      if (stream.listenerCount && stream.listenerCount("error") > 0) G.queueMicrotask(() => stream.emit("error", streamErr(code)));
      stream._finish();
    }
    _connError(code) {
      const p = Buffer.alloc(8); p.writeUInt32BE(this._lastStreamId > 0 ? this._lastStreamId : 0, 0); p.writeUInt32BE(code >>> 0, 4);
      try { this._writeFrame(FRAME.GOAWAY, 0, 0, p); } catch (e) {}
      const err = sessionErr(code);
      const self = this;
      this._teardown();
      G.queueMicrotask(() => self.emit("error", err));
    }
    _onSocketError(e) { if (!this.destroyed) { const self = this; this._teardown(); G.queueMicrotask(() => self.emit("error", e)); } }
    // The peer half-closed: RFC 9113 sec. 5.4.1 — no further frames can arrive, so
    // the session is over once its streams have finished. node reaches the same
    // point through kMaybeDestroy (the last stream destroying a closed session
    // destroys the session, and with it the socket). This was a no-op, and the
    // http2 socket is allowHalfOpen, so the socket stayed open forever; once
    // handles genuinely held the event loop that became a hang.
    _onSocketEnd() {
      if (this.destroyed) return;
      abortStreamsOnTransportEof(this);
      this._endPending = false;
      this._teardown();
    }
    _onSocketClose() { this._teardown(); }
    _teardown(hard) {
      if (this.destroyed) return;
      this.destroyed = true; this.closed = true;
      closeSessionSocket(this.socket, hard !== false);
      const streams = Array.from(this.streams.values());
      const self = this;
      G.queueMicrotask(() => { for (const s of streams) s._finish(); self.emit("close"); });
    }
    // ---- public surface ----
    get connected() { return !this.destroyed; }
    get remoteSettings() { return this._remoteSettings ? settingsToObject(this._remoteSettings) : undefined; }
    get localSettings() { return this._localSettings; }
    // server-initiated streams are even-numbered; we never push, so the next id
    // a server session would allocate stays 2 (node reports the same).
    get state() { return sessionState(this, (this._lastPushId || 0) + 2); }
    get pendingSettingsAck() { return sessionPendingAck(this); }
    get type() { return constants.NGHTTP2_SESSION_SERVER; }
    settings(s, cb) { return sessionSubmitSettings(this, s, cb); }
    ping(payload, cb) { return sessionPing(this, payload, cb); }
    altsvc(alt, originOrStream) { return sessionAltsvc(this, alt, originOrStream); }
    origin(...origins) { return sessionOrigin(this, origins); }
    goaway(code, lastStreamID, opaqueData) {
      if (code === undefined) code = 0;
      validateNumber(code, "code");
      if (lastStreamID === undefined) lastStreamID = 0;
      validateNumber(lastStreamID, "lastStreamID");
      // node: an explicitly-passed opaqueData (including null) must be a byte view.
      if (opaqueData !== undefined && !isBufLike(opaqueData))
        throw argTypeErr("opaqueData", "an instance of Buffer, TypedArray, or DataView", opaqueData);
      // node: lastStreamID <= 0 means "the last stream this session processed".
      const effLast = (lastStreamID && lastStreamID > 0) ? lastStreamID : this._lastStreamId;
      const p = Buffer.alloc(8); p.writeUInt32BE((effLast || 0) >>> 0, 0); p.writeUInt32BE((code || 0) >>> 0, 4);
      this._writeFrame(FRAME.GOAWAY, 0, 0, (opaqueData !== undefined && opaqueData !== null) ? Buffer.concat([p, Buffer.from(opaqueData)]) : p);
    }
    close(cb) {
      if (typeof cb === "function") this.once("close", cb);
      if (this.destroyed || this.closed) return;
      this.closed = true;
      const p = Buffer.alloc(8); p.writeUInt32BE(this._lastStreamId > 0 ? this._lastStreamId : 0, 0); p.writeUInt32BE(0, 4);
      try { this._writeFrame(FRAME.GOAWAY, 0, 0, p); } catch (e) {}
      const self = this;
      G.queueMicrotask(() => self._teardown(false));
    }
    destroy(err, code) { if (this.destroyed) return; if (err) { const self = this; this._teardown(); G.queueMicrotask(() => self.emit("error", err)); } else this._teardown(); }
    ref() { if (this.socket && this.socket.ref) this.socket.ref(); return this; }
    unref() { if (this.socket && this.socket.unref) this.socket.unref(); return this; }
    setTimeout() { return this; }
  }

  // RFC 7540 6.5.2 value-range validation for a peer SETTINGS payload. Returns a
  // connection error code (or 0 if valid). ENABLE_PUSH must be 0/1, MAX_FRAME_SIZE
  // in [2^14, 2^24-1], INITIAL_WINDOW_SIZE <= 2^31-1.
  function settingsRangeError(payload) {
    for (let i = 0; i + 6 <= payload.length; i += 6) {
      const id = (payload[i] << 8) | payload[i + 1];
      const val = (payload[i + 2] * 0x1000000) + (payload[i + 3] << 16) + (payload[i + 4] << 8) + payload[i + 5];
      if (id === 2 && val !== 0 && val !== 1) return constants.NGHTTP2_PROTOCOL_ERROR;
      if (id === 4 && val > 2147483647) return constants.NGHTTP2_FLOW_CONTROL_ERROR;
      if (id === 5 && (val < 16384 || val > 16777215)) return constants.NGHTTP2_PROTOCOL_ERROR;
    }
    return 0;
  }
  function parseSettingsPayload(payload) {
    const s = {};
    for (let i = 0; i + 6 <= payload.length; i += 6) {
      const id = (payload[i] << 8) | payload[i + 1];
      const val = (payload[i + 2] * 0x1000000) + (payload[i + 3] << 16) + (payload[i + 4] << 8) + payload[i + 5];
      s[id] = val;
    }
    return {
      headerTableSize: s[1] !== undefined ? s[1] : 4096,
      enablePush: s[2] !== undefined ? s[2] : 1,
      maxConcurrentStreams: s[3] !== undefined ? s[3] : 4294967295,
      initialWindowSize: s[4] !== undefined ? s[4] : 65535,
      maxFrameSize: s[5] !== undefined ? s[5] : 16384,
      maxHeaderListSize: s[6] !== undefined ? s[6] : 65535,
      enableConnectProtocol: s[8] !== undefined ? s[8] : 0,
    };
  }
  function encodeSettings(settings) {
    if (!settings || typeof settings !== "object") return Buffer.alloc(0);
    const ids = { headerTableSize: 1, enablePush: 2, maxConcurrentStreams: 3, initialWindowSize: 4, maxFrameSize: 5, maxHeaderListSize: 6, enableConnectProtocol: 8 };
    const entries = [];
    for (const k in ids) {
      if (settings[k] === undefined) continue;
      let v = settings[k];
      if (typeof v === "boolean") v = v ? 1 : 0;
      entries.push([ids[k], v >>> 0]);
    }
    const p = Buffer.alloc(entries.length * 6);
    for (let i = 0; i < entries.length; i++) { p.writeUInt16BE(entries[i][0], i * 6); p.writeUInt32BE(entries[i][1], i * 6 + 2); }
    return p;
  }

  // === Http2ServerRequest / Http2ServerResponse (createServer((req,res)) compat) ===
  // Translated from node lib/internal/http2/compat.js. The request keeps the
  // Http2Stream's flowing-mode 'data' relay rather than a real Readable (mbun's
  // Http2Stream is an EventEmitter, not a stream), but every observable member
  // node's compat layer defines — header validation and its error codes, the
  // socket proxy, statusCode/statusMessage semantics, writeHead's array form,
  // trailers, informational responses — is ported as written there.
  const HTTP_STATUS_CONTINUE = 100, HTTP_STATUS_EARLY_HINTS = 103;
  const HTTP_STATUS_EXPECTATION_FAILED = 417, HTTP_STATUS_METHOD_NOT_ALLOWED = 405;
  const kValidPseudoHeaders = new Set([":status", ":method", ":path", ":authority", ":scheme"]);
  const isPseudoHeader = (name) => kValidPseudoHeaders.has(name);
  // _http_common checkIsHttpToken: RFC 7230 3.2.6 tchar.
  const TOKEN_RE = /^[\^_`a-zA-Z\-0-9!#$%&'*+.|~]+$/;
  const checkIsHttpToken = (s) => TOKEN_RE.test(s);
  let statusMessageWarned = false, statusConnectionHeaderWarned = false;
  function statusMessageWarn() {
    if (statusMessageWarned) return;
    statusMessageWarned = true;
    try { G.process.emitWarning("Status message is not supported by HTTP/2 (RFC7540 8.1.2.4)", "UnsupportedWarning"); } catch (e) {}
  }
  function connectionHeaderMessageWarn() {
    if (statusConnectionHeaderWarned) return;
    statusConnectionHeaderWarned = true;
    try { G.process.emitWarning("The provided connection header is not valid, the value will be dropped from the header and will never be in use.", "UnsupportedWarning"); } catch (e) {}
  }
  const isConnectionHeaderAllowed = (name, value) => name !== "connection" || value === "trailers";
  function invalidHttpToken(what, value) {
    const e = new TypeError(what + " must be a valid HTTP token" + (value === undefined ? "" : ' ["' + value + '"]'));
    e.code = "ERR_INVALID_HTTP_TOKEN"; return e;
  }
  function assertValidHeader(name, value) {
    if (name === "" || typeof name !== "string" || name.includes(" ")) throw invalidHttpToken("Header name", name);
    if (isPseudoHeader(name)) { const e = new TypeError("Cannot set HTTP/2 pseudo-headers"); e.code = "ERR_HTTP2_PSEUDOHEADER_NOT_ALLOWED"; throw e; }
    if (value === undefined || value === null) { const e = new TypeError("Invalid value \"" + value + '" for header "' + name + '"'); e.code = "ERR_HTTP2_INVALID_HEADER_VALUE"; throw e; }
    if (!isConnectionHeaderAllowed(name, value)) connectionHeaderMessageWarn();
  }
  function assertValidPseudoHeader(key) {
    if (!kValidPseudoHeaders.has(key)) { const e = new TypeError('"' + key + '" is an invalid pseudoheader or is used incorrectly'); e.code = "ERR_HTTP2_INVALID_PSEUDOHEADER"; throw e; }
  }
  const validateString = (v, name) => { if (typeof v !== "string") throw argTypeErr(name, "of type string", v); };
  const validateFunction = (v, name) => { if (typeof v !== "function") throw argTypeErr(name, "of type function", v); };
  const validateObject = (v, name) => { if (v === null || typeof v !== "object" || Array.isArray(v)) throw argTypeErr(name, "of type object", v); };
  // node lib/internal/validators validateLinkHeaderValue: accepts a string or an
  // array of strings, each `<uri>; rel=…` shaped.
  const LINK_VALUE_RE = /^(?:<[^>]*>)(?:\s*;\s*[^;"\s]+(?:=(")?[^;"\s]*\1)?)*$/;
  const LINK_FORMAT_REASON = "must be an array or string of format \"</styles.css>; rel=preload; as=style\"";
  function validateLinkHeaderValue(hints) {
    if (typeof hints === "string") {
      if (!LINK_VALUE_RE.test(hints)) throw invalidArgValue("hints", hints, LINK_FORMAT_REASON);
      return hints;
    } else if (Array.isArray(hints)) {
      if (hints.length === 0) return "";
      let result = "";
      for (let i = 0; i < hints.length; i++) {
        const link = hints[i];
        if (typeof link !== "string" || !LINK_VALUE_RE.test(link)) throw invalidArgValue("hints", link, LINK_FORMAT_REASON);
        result += link;
        if (i !== hints.length - 1) result += ", ";
      }
      return result;
    }
    // Anything that is neither a string nor an array is rejected outright
    // (node lib/internal/validators.js validateLinkHeaderValue).
    throw invalidArgValue("hints", hints, LINK_FORMAT_REASON);
  }
  function http2StatusInvalid(code) {
    const e = new RangeError("Invalid status code: " + code); e.code = "ERR_HTTP2_STATUS_INVALID"; return e;
  }
  // node's socket proxy: the object a compat request/response calls `socket`.
  // Reads/writes route to the session's real socket, but the stream-manipulating
  // members are hard errors (ERR_HTTP2_NO_SOCKET_MANIPULATION) because touching
  // them would desynchronise the multiplexed session.
  function makeProxySocket(stream) {
    const noManip = new Set(["write", "read", "pause", "resume"]);
    const bindStream = new Set(["on", "once", "end", "emit", "destroy", "removeListener", "addListener", "off"]);
    const refOf = () => (stream.session !== undefined && stream.session ? stream.session.socket : stream);
    return new Proxy(stream, {
      has(t, prop) { const ref = refOf(); return (prop in t) || (ref != null && prop in ref); },
      get(t, prop) {
        if (bindStream.has(prop)) { const f = t[prop]; return typeof f === "function" ? f.bind(t) : f; }
        if (prop === "writable" || prop === "destroyed") return t[prop];
        if (prop === "readable") { if (t.destroyed) return false; const req = t._compatRequest; return req ? req.readable : t.readable; }
        if (prop === "setTimeout") { const s = t.session; if (s !== undefined && s) return s.setTimeout.bind(s); return t.setTimeout.bind(t); }
        if (noManip.has(prop)) { const e = new Error("HTTP/2 sockets should not be directly manipulated (e.g. read and written)"); e.code = "ERR_HTTP2_NO_SOCKET_MANIPULATION"; throw e; }
        const ref = refOf();
        if (ref == null) return undefined;
        const value = ref[prop];
        return typeof value === "function" ? value.bind(ref) : value;
      },
      getPrototypeOf(t) { const ref = refOf(); return Object.getPrototypeOf(ref == null ? t : ref); },
      set(t, prop, value) {
        if (bindStream.has(prop) || prop === "writable" || prop === "readable" || prop === "destroyed") { t[prop] = value; return true; }
        if (prop === "setTimeout") { const s = t.session; if (s !== undefined && s) s.setTimeout = value; else t.setTimeout = value; return true; }
        if (noManip.has(prop)) { const e = new Error("HTTP/2 sockets should not be directly manipulated (e.g. read and written)"); e.code = "ERR_HTTP2_NO_SOCKET_MANIPULATION"; throw e; }
        const ref = refOf();
        if (ref != null) ref[prop] = value;
        return true;
      },
    });
  }
  function proxySocketOf(stream) {
    if (stream._proxySocket == null) stream._proxySocket = makeProxySocket(stream);
    return stream._proxySocket;
  }

  class Http2ServerRequest extends EE {
    constructor(stream, headers, options, rawHeaders) {
      super();
      this._state = { closed: false, didRead: false };
      this._headers = headers || {};
      this._rawHeaders = rawHeaders || [];
      this._trailers = {};
      this._rawTrailers = [];
      this._stream = stream;
      this._aborted = false;
      this.readable = true;
      this.readableEnded = false;
      this.destroyed = false;
      stream._proxySocket = null;
      stream._compatRequest = this;
      const self = this;
      stream.on("data", (d) => self.emit("data", d));
      stream.on("trailers", (trailers, flags, raw) => {
        Object.assign(self._trailers, trailers || {});
        if (Array.isArray(raw)) for (const v of raw) self._rawTrailers.push(v);
      });
      stream.on("end", () => { self.readableEnded = true; self.readable = false; self.emit("end"); });
      stream.on("aborted", () => { if (!self._state.closed) { self._aborted = true; self.emit("aborted"); } });
      stream.on("close", () => { self._state.closed = true; stream._proxySocket = null; self.emit("close"); });
      stream.on("timeout", () => self.emit("timeout"));
    }
    get aborted() { return this._aborted; }
    get complete() { return this._aborted || this.readableEnded || this._state.closed || this._stream.destroyed; }
    get stream() { return this._stream; }
    get headers() { return this._headers; }
    get rawHeaders() { return this._rawHeaders; }
    get trailers() { return this._trailers; }
    get rawTrailers() { return this._rawTrailers; }
    get httpVersionMajor() { return 2; }
    get httpVersionMinor() { return 0; }
    get httpVersion() { return "2.0"; }
    get socket() { return proxySocketOf(this._stream); }
    get connection() { return this.socket; }
    get method() { return this._headers[":method"]; }
    set method(method) {
      validateString(method, "method");
      if (method.trim() === "") throw invalidArgValue("method", method);
      this._headers[":method"] = method;
    }
    get authority() { return this._headers[":authority"] !== undefined ? this._headers[":authority"] : this._headers["host"]; }
    get scheme() { return this._headers[":scheme"]; }
    get url() { return this._headers[":path"]; }
    set url(url) { this._headers[":path"] = url; }
    setEncoding(enc) { this._stream.setEncoding(enc); return this; }
    setTimeout(msecs, callback) { if (!this._state.closed) this._stream.setTimeout(msecs, callback); return this; }
    resume() { this._stream.resume(); this.emit("resume"); return this; }
    pause() { this._stream.pause(); this.emit("pause"); return this; }
    read() { return null; }
    destroy(err) { if (this.destroyed) return this; this.destroyed = true; this._stream.destroy(err); return this; }
    pipe(dest, options) {
      const self = this;
      const endDest = !(options && options.end === false);
      this.on("data", (chunk) => { dest.write(chunk); });
      this.on("end", () => { if (endDest && typeof dest.end === "function") dest.end(); });
      try { if (typeof dest.emit === "function") dest.emit("pipe", self); } catch (e) {}
      return dest;
    }
  }

  class Http2ServerResponse extends EE {
    constructor(stream, options) {
      super();
      this._state = { closed: false, ending: false, destroyed: false, headRequest: false, sendDate: true, statusCode: 200 };
      this._hdrs = { __proto__: null };
      this._trailers = { __proto__: null };
      this._stream = stream;
      stream._proxySocket = null;
      stream._compatResponse = this;
      this.writable = true;
      this.req = stream._compatRequest;
      const self = this;
      stream.on("drain", () => self.emit("drain"));
      stream.on("close", () => {
        if (self._state.closed) return;
        self._state.closed = true;
        stream._proxySocket = null;
        self.emit("finish");
        self.emit("close");
      });
      stream.on("wantTrailers", () => { try { stream.sendTrailers(self._trailers); } catch (e) {} });
      stream.on("timeout", () => self.emit("timeout"));
    }
    get _header() { return this.headersSent; }
    get writableEnded() { return this._state.ending; }
    get finished() { return this._state.ending; }
    get socket() { if (this._state.closed) return undefined; return proxySocketOf(this._stream); }
    get connection() { return this.socket; }
    get stream() { return this._stream; }
    get headersSent() { return this._stream.headersSent; }
    get sendDate() { return this._state.sendDate; }
    set sendDate(bool) { this._state.sendDate = !!bool; }
    get statusCode() { return this._state.statusCode; }
    set statusCode(code) {
      code |= 0;
      if (code >= 100 && code < 200) { const e = new RangeError("Informational status codes cannot be used"); e.code = "ERR_HTTP2_INFO_STATUS_NOT_ALLOWED"; throw e; }
      if (code < 100 || code > 599) throw http2StatusInvalid(code);
      this._state.statusCode = code;
    }
    // node compat.js delegates every writable-side counter to the Http2Stream.
    get writableCorked() { return this._stream.writableCorked; }
    get writableHighWaterMark() { return this._stream.writableHighWaterMark; }
    get writableObjectMode() { return this._stream.writableObjectMode; }
    get writableFinished() { return this._stream.writableFinished; }
    get writableLength() { return this._stream.writableLength; }
    get writableNeedDrain() { return this._stream.writableNeedDrain; }
    get statusMessage() { statusMessageWarn(); return ""; }
    set statusMessage(msg) { statusMessageWarn(); }
    setTrailer(name, value) {
      validateString(name, "name");
      name = name.trim().toLowerCase();
      assertValidHeader(name, value);
      this._trailers[name] = value;
    }
    addTrailers(headers) { for (const key of Object.keys(headers)) this.setTrailer(key, headers[key]); }
    getHeader(name) { validateString(name, "name"); return this._hdrs[name.trim().toLowerCase()]; }
    getHeaderNames() { return Object.keys(this._hdrs); }
    getHeaders() { return Object.assign({ __proto__: null }, this._hdrs); }
    hasHeader(name) { validateString(name, "name"); return Object.prototype.hasOwnProperty.call(this._hdrs, name.trim().toLowerCase()); }
    removeHeader(name) {
      validateString(name, "name");
      if (this._stream.headersSent) throw mkErr("Response has already been initiated.", "ERR_HTTP2_HEADERS_SENT");
      name = name.trim().toLowerCase();
      if (name === "date") { this._state.sendDate = false; return; }
      delete this._hdrs[name];
    }
    setHeader(name, value) {
      validateString(name, "name");
      if (this._stream.headersSent) throw mkErr("Response has already been initiated.", "ERR_HTTP2_HEADERS_SENT");
      this._setHeader(name, value);
      return this;
    }
    _setHeader(name, value) {
      name = name.trim().toLowerCase();
      assertValidHeader(name, value);
      if (!isConnectionHeaderAllowed(name, value)) return;
      if (name[0] === ":") assertValidPseudoHeader(name);
      else if (!checkIsHttpToken(name)) this.destroy(invalidHttpToken("Header name", name));
      this._hdrs[name] = value;
    }
    appendHeader(name, value) {
      validateString(name, "name");
      if (this._stream.headersSent) throw mkErr("Response has already been initiated.", "ERR_HTTP2_HEADERS_SENT");
      this._appendHeader(name, value);
      return this;
    }
    _appendHeader(name, value) {
      name = name.trim().toLowerCase();
      assertValidHeader(name, value);
      if (!isConnectionHeaderAllowed(name, value)) return;
      if (name[0] === ":") assertValidPseudoHeader(name);
      else if (!checkIsHttpToken(name)) this.destroy(invalidHttpToken("Header name", name));
      const headers = this._hdrs;
      if (headers === null || !headers[name]) { this._hdrs[name] = value; return; }
      if (!Array.isArray(headers[name])) headers[name] = [headers[name]];
      const existing = headers[name];
      if (Array.isArray(value)) { for (const v of value) existing.push(v); }
      else existing.push(value);
    }
    flushHeaders() {
      const state = this._state;
      if (!state.closed && !this._stream.headersSent) this.writeHead(state.statusCode);
    }
    writeHead(statusCode, statusMessage, headers) {
      const state = this._state;
      if (state.closed || this._stream.destroyed || this._stream.closed) return this;
      if (this._stream.headersSent) throw mkErr("Response has already been initiated.", "ERR_HTTP2_HEADERS_SENT");
      if (typeof statusMessage === "string") statusMessageWarn();
      if (headers === undefined && typeof statusMessage === "object") headers = statusMessage;
      if (Array.isArray(headers)) {
        // Array form overrides previously-set headers but keeps explicit
        // duplicates: drop the conflicting names first, then append.
        if (this._hdrs) {
          if (headers.length && Array.isArray(headers[0])) { for (let n = 0; n < headers.length; n++) this.removeHeader(headers[n][0]); }
          else { for (let n = 0; n < headers.length; n += 2) this.removeHeader(headers[n]); }
        }
        if (headers.length && Array.isArray(headers[0])) { for (const h of headers) this._appendHeader(h[0], h[1]); }
        else {
          if (headers.length % 2 !== 0) throw invalidArgValue("headers", headers);
          for (let i = 0; i < headers.length; i += 2) this._appendHeader(headers[i], headers[i + 1]);
        }
      } else if (headers !== null && typeof headers === "object") {
        for (const key of Object.keys(headers)) this._setHeader(key, headers[key]);
      }
      state.statusCode = statusCode;
      this._beginSend();
      return this;
    }
    cork() { this._stream.cork(); return this; }
    uncork() { this._stream.uncork(); return this; }
    write(chunk, encoding, cb) {
      const state = this._state;
      if (typeof encoding === "function") { cb = encoding; encoding = "utf8"; }
      let err;
      if (state.ending) { err = mkErr("write after end", "ERR_STREAM_WRITE_AFTER_END"); }
      else if (state.closed) { err = mkErr("The stream has been destroyed", "ERR_HTTP2_INVALID_STREAM"); }
      else if (state.destroyed) return false;
      if (err) {
        if (typeof cb === "function") G.queueMicrotask(() => cb(err));
        this.destroy(err);
        return false;
      }
      const stream = this._stream;
      if (!stream.headersSent) this.writeHead(state.statusCode);
      return stream.write(chunk, encoding, cb);
    }
    end(chunk, encoding, cb) {
      const stream = this._stream;
      const state = this._state;
      if (typeof chunk === "function") { cb = chunk; chunk = null; }
      else if (typeof encoding === "function") { cb = encoding; encoding = "utf8"; }
      if ((state.closed || state.ending) && state.headRequest === stream.headRequest) {
        if (typeof cb === "function") G.queueMicrotask(cb);
        return this;
      }
      if (chunk !== null && chunk !== undefined) this.write(chunk, encoding);
      state.headRequest = stream.headRequest;
      state.ending = true;
      if (typeof cb === "function") this.once("finish", cb);
      if (!stream.headersSent) this.writeHead(state.statusCode);
      if (state.closed || stream.destroyed) {
        if (!state.closed) { state.closed = true; this.emit("finish"); this.emit("close"); }
      } else stream.end();
      return this;
    }
    destroy(err) {
      if (this._state.destroyed) return;
      this._state.destroyed = true;
      this._stream.destroy(err);
    }
    setTimeout(msecs, callback) { if (this._state.closed) return this; this._stream.setTimeout(msecs, callback); return this; }
    createPushResponse(headers, callback) {
      validateFunction(callback, "callback");
      if (this._state.closed) { G.queueMicrotask(() => callback(mkErr("The stream has been destroyed", "ERR_HTTP2_INVALID_STREAM"))); return; }
      this._stream.pushStream(headers, {}, (err, stream) => {
        if (err) { callback(err); return; }
        callback(null, new Http2ServerResponse(stream));
      });
    }
    _beginSend() {
      const state = this._state;
      const headers = this._hdrs;
      headers[":status"] = state.statusCode;
      this._stream.respond(headers, { endStream: state.ending, waitForTrailers: true, sendDate: state.sendDate });
    }
    writeInformation(statusCode, headers) {
      if (typeof statusCode !== "number" || statusCode < 100 || statusCode > 199) throw http2StatusInvalid(statusCode);
      if (statusCode === 101) throw http2StatusInvalid(statusCode);
      const stream = this._stream;
      if (stream.headersSent || this._state.closed) return false;
      const outHeaders = { __proto__: null };
      if (headers !== undefined && headers !== null) {
        validateObject(headers, "headers");
        for (const k of Object.keys(headers)) outHeaders[k] = headers[k];
      }
      outHeaders[":status"] = statusCode;
      stream.additionalHeaders(outHeaders);
      return true;
    }
    writeContinue() { return this.writeInformation(HTTP_STATUS_CONTINUE); }
    writeEarlyHints(hints) {
      validateObject(hints, "hints");
      const headers = { __proto__: null };
      const linkHeaderValue = validateLinkHeaderValue(hints.link);
      for (const key of Object.keys(hints)) {
        if (key === "link") continue;
        const name = key.trim().toLowerCase();
        assertValidHeader(name, hints[key]);
        if (!checkIsHttpToken(name)) throw invalidHttpToken("Header name", name);
        headers[name] = hints[key];
      }
      if (linkHeaderValue.length === 0) return false;
      headers.Link = linkHeaderValue;
      return this.writeInformation(HTTP_STATUS_EARLY_HINTS, headers);
    }
  }

  // node compat.js onServerStream: CONNECT and Expect: 100-continue are routed
  // to their own server events before the plain 'request' dispatch.
  function onServerStream(server, stream, headers, flags, rawHeaders) {
    // node Http2Server({ Http2ServerRequest, Http2ServerResponse }): the compat
    // classes are overridable per server so an application can subclass them
    // (lib/internal/http2/core.js onServerStream reads `options` off the
    // session). Defaulting to the built-ins keeps the common path unchanged.
    const opts = (server && server._h2options) || {};
    const Req = typeof opts.Http2ServerRequest === "function" ? opts.Http2ServerRequest : Http2ServerRequest;
    const Res = typeof opts.Http2ServerResponse === "function" ? opts.Http2ServerResponse : Http2ServerResponse;
    const request = new Req(stream, headers, undefined, rawHeaders);
    const response = new Res(stream);
    const method = headers[":method"];
    if (method === "CONNECT") {
      if (!server.emit("connect", request, response)) {
        response.statusCode = HTTP_STATUS_METHOD_NOT_ALLOWED;
        response.end();
      }
      return;
    }
    if (headers.expect !== undefined) {
      if (headers.expect === "100-continue") {
        if (server.listenerCount("checkContinue")) server.emit("checkContinue", request, response);
        else { response.writeContinue(); server.emit("request", request, response); }
      } else if (server.listenerCount("checkExpectation")) {
        server.emit("checkExpectation", request, response);
      } else {
        response.statusCode = HTTP_STATUS_EXPECTATION_FAILED;
        response.end();
      }
      return;
    }
    server.emit("request", request, response);
  }

  // === Http2Server / Http2SecureServer ===
  function attachH2Server(server, options, onRequest, secure) {
    server._h2options = options || {};
    if (typeof onRequest === "function") server.on("request", onRequest);
    const self = server;
    const onSession = (socket) => {
      // For an ALPN mismatch on a secure server (client spoke http/1.1) node
      // routes to the http1 path; here we only handle h2, so proceed if the
      // negotiated protocol is h2 (or plaintext h2c).
      const session = new ServerHttp2Session(self, socket, options);
      session.on("stream", (stream, headers, flags, rawHeaders) => {
        self.emit("stream", stream, headers, flags, rawHeaders);
        // node Http2Server routes every stream through compat.js onServerStream
        // when any of the compat events is listened for, not only 'request':
        // CONNECT and Expect: 100-continue have their own server events.
        if (self.listenerCount("request") > 0 || self.listenerCount("checkContinue") > 0 ||
            self.listenerCount("checkExpectation") > 0 || self.listenerCount("connect") > 0)
          onServerStream(self, stream, headers, flags, rawHeaders);
      });
      session.on("error", (e) => { if (self.listenerCount("sessionError") > 0) self.emit("sessionError", e, session); else if (self.listenerCount("session") === 0 && self.listenerCount("error") > 0) self.emit("error", e); });
      self.emit("session", session);
    };
    server.on(secure ? "secureConnection" : "connection", onSession);
    // node Http2Server#setTimeout(msecs, callback): validateFunction on the
    // callback, and `server.timeout` records the value.
    server.timeout = 0;
    server.setTimeout = function (ms, cb) {
      this.timeout = ms;
      if (cb !== undefined) { if (typeof cb !== "function") throw argTypeErr("callback", "of type function", cb); this.on("timeout", cb); }
      return this;
    };
    server.updateSettings = function () { return this; };
    return server;
  }
  // node Http2Server/Http2SecureServer constructor -> initializeOptions:
  // `options` must be a plain object, `options.settings` too (and is validated
  // as a settings object), and the session-limit counters must be non-negative
  // integers.
  function validateServerOptions(options, allowFunction) {
    if (options === undefined) return {};
    if (options === null || typeof options !== "object" || Array.isArray(options) || (!allowFunction && typeof options === "function"))
      throw argTypeErr("options", "of type object", options);
    if (options.settings !== undefined) {
      const s = options.settings;
      if (s === null || typeof s !== "object" || Array.isArray(s)) throw argTypeErr("options.settings", "of type object", s);
      validateSettings(s);
    }
    for (const key of ["maxSessionInvalidFrames", "maxSessionRejectedStreams"]) {
      const v = options[key];
      if (v === undefined) continue;
      if (typeof v !== "number") throw argTypeErr("options." + key, "of type number", v);
      if (!Number.isInteger(v)) throw outOfRangeErr("options." + key, "an integer", v);
      if (v < 0 || v > 4294967295) throw outOfRangeErr("options." + key, ">= 0 && <= 4294967295", v);
    }
    return options;
  }
  function makeHttp2Server(options, onRequest) {
    if (typeof options === "function") { onRequest = options; options = {}; }
    validateServerOptions(options);
    const server = net.createServer({ allowHalfOpen: true });
    return attachH2Server(server, options, onRequest, false);
  }
  // node http2.performServerHandshake(socket[, options]): drive the server side
  // of the connection preface over a socket the caller already owns (a Duplex
  // pair in the corpus), with no Http2Server in front of it. A socket may only
  // back one session — node marks it and reports ERR_HTTP2_SOCKET_BOUND on a
  // second attempt.
  const kBoundSession = G.Symbol("mbun.http2.boundSession");
  function performServerHandshake(socket, options) {
    if (socket && socket[kBoundSession] !== undefined) {
      const e = new Error("The socket is already bound to an Http2Session");
      e.code = "ERR_HTTP2_SOCKET_BOUND";
      throw e;
    }
    const opts = validateServerOptions(options) || {};
    const session = new ServerHttp2Session(undefined, socket, opts);
    try { socket[kBoundSession] = session; } catch (e) {}
    return session;
  }
  function makeHttp2SecureServer(options, onRequest) {
    // Unlike createServer, createSecureServer has no options-less form: a
    // function in the first position is an invalid `options`, not the handler.
    validateServerOptions(options);
    if (!tls || !tls.createServer) throw mkErr("http2 createSecureServer requires node:tls", "ERR_HTTP2_ERROR");
    const alpn = (options && options.allowHTTP1) ? ["h2", "http/1.1"] : ["h2"];
    const server = tls.createServer(Object.assign({}, options, { ALPNProtocols: (options && options.ALPNProtocols) || alpn }));
    return attachH2Server(server, options, onRequest, true);
  }

  // === settings validation / (un)packing =====================================
  // Blueprint: node lib/internal/http2/core.js validateSettings +
  // getPackedSettings/getUnpackedSettings and lib/internal/http2/util.js
  // updateSettingsBuffer (which fixes the wire order: HEADER_TABLE_SIZE,
  // ENABLE_PUSH, MAX_CONCURRENT_STREAMS, INITIAL_WINDOW_SIZE, MAX_FRAME_SIZE,
  // MAX_HEADER_LIST_SIZE, ENABLE_CONNECT_PROTOCOL, then customSettings).
  const kMaxInt = (2 ** 32) - 1;
  const kMaxFrameSizeSetting = (2 ** 24) - 1;
  const kMaxInitialWindowSize = (2 ** 31) - 1;
  const kMaxStreams = (2 ** 32) - 1;
  const MAX_ADDITIONAL_SETTINGS = 10;
  function invalidSettingValue(name, value, range) {
    const msg = 'Invalid value for setting "' + name + '": ' + value;
    const e = range ? new RangeError(msg) : new TypeError(msg);
    e.code = "ERR_HTTP2_INVALID_SETTING_VALUE";
    e.actual = value;
    return e;
  }
  function tooManyCustomSettings() {
    const e = new Error("Number of custom settings exceeds MAX_ADDITIONAL_SETTINGS");
    e.code = "ERR_HTTP2_TOO_MANY_CUSTOM_SETTINGS";
    return e;
  }
  function assertWithinRange(name, value, min, max) {
    if (value !== undefined && (typeof value !== "number" || value < min || value > max))
      throw invalidSettingValue(name, value, true);
  }
  function assertIsObject(value, name, types) {
    if (value !== undefined && (value === null || typeof value !== "object" || Array.isArray(value)))
      throw argTypeErr(name, "an object" + (types ? " of " + types + "s" : ""), value);
  }
  function validateSettings(settings) {
    if (settings === undefined) return;
    assertIsObject(settings.customSettings, "customSettings", "Number");
    if (settings.customSettings) {
      const keys = Object.keys(settings.customSettings);
      if (keys.length > MAX_ADDITIONAL_SETTINGS) throw tooManyCustomSettings();
      for (const key of keys) {
        assertWithinRange("customSettings:id", Number(key), 0, 0xffff);
        assertWithinRange("customSettings:value", Number(settings.customSettings[key]), 0, kMaxInt);
      }
    }
    assertWithinRange("headerTableSize", settings.headerTableSize, 0, kMaxInt);
    assertWithinRange("initialWindowSize", settings.initialWindowSize, 0, kMaxInitialWindowSize);
    assertWithinRange("maxFrameSize", settings.maxFrameSize, 16384, kMaxFrameSizeSetting);
    assertWithinRange("maxConcurrentStreams", settings.maxConcurrentStreams, 0, kMaxStreams);
    assertWithinRange("maxHeaderListSize", settings.maxHeaderListSize, 0, kMaxInt);
    assertWithinRange("maxHeaderSize", settings.maxHeaderSize, 0, kMaxInt);
    if (settings.enablePush !== undefined && typeof settings.enablePush !== "boolean")
      throw invalidSettingValue("enablePush", settings.enablePush, false);
    if (settings.enableConnectProtocol !== undefined && typeof settings.enableConnectProtocol !== "boolean")
      throw invalidSettingValue("enableConnectProtocol", settings.enableConnectProtocol, false);
  }
  function getPackedSettings(settings) {
    assertIsObject(settings, "settings");
    validateSettings(settings);
    const s = settings || {};
    const entries = [];
    if (typeof s.headerTableSize === "number") entries.push([1, s.headerTableSize]);
    if (typeof s.enablePush === "boolean") entries.push([2, s.enablePush ? 1 : 0]);
    if (typeof s.maxConcurrentStreams === "number") entries.push([3, s.maxConcurrentStreams]);
    if (typeof s.initialWindowSize === "number") entries.push([4, s.initialWindowSize]);
    if (typeof s.maxFrameSize === "number") entries.push([5, s.maxFrameSize]);
    if (typeof s.maxHeaderListSize === "number" || typeof s.maxHeaderSize === "number")
      entries.push([6, typeof s.maxHeaderSize === "number" ? s.maxHeaderSize : s.maxHeaderListSize]);
    if (typeof s.enableConnectProtocol === "boolean") entries.push([8, s.enableConnectProtocol ? 1 : 0]);
    if (s.customSettings && typeof s.customSettings === "object") {
      let n = 0;
      for (const key in s.customSettings) {
        if (!Object.prototype.hasOwnProperty.call(s.customSettings, key)) continue;
        const val = s.customSettings[key];
        if (typeof val !== "number") continue;
        const id = Number(key);
        // updateSettingsBuffer rejects a non-numeric / out-of-range custom id or
        // value here rather than at frame-serialisation time.
        if (id !== id || id <= 0 || id > 0xffff) throw invalidSettingValue("customSettings:id", id, true);
        if (val !== val || val <= 0 || val > kMaxInt) throw invalidSettingValue("customSettings:value", val, true);
        if (n === MAX_ADDITIONAL_SETTINGS) throw tooManyCustomSettings();
        n++;
        entries.push([id, val]);
      }
    }
    const out = Buffer.alloc(entries.length * 6);
    for (let i = 0; i < entries.length; i++) {
      out.writeUInt16BE(entries[i][0], i * 6);
      out.writeUInt32BE(entries[i][1] >>> 0, i * 6 + 2);
    }
    return out;
  }
  function getUnpackedSettings(buf, options) {
    // node requires a Buffer/TypedArray with a `length`: a DataView has
    // byteLength but no length, so it is rejected.
    if (!ArrayBuffer.isView(buf) || buf.length === undefined)
      throw argTypeErr("buf", "an instance of Buffer or TypedArray", buf);
    if (buf.length % 6 !== 0) {
      const e = new RangeError("Packed settings length must be a multiple of six");
      e.code = "ERR_HTTP2_INVALID_PACKED_SETTINGS_LENGTH";
      throw e;
    }
    // A non-Uint8Array TypedArray is read byte-wise through its element values
    // (node reads the same `buf[i]` indices), so mirror that with readUInt*.
    const at = (i) => buf[i] & 0xff;
    const u16 = (i) => (at(i) << 8) | at(i + 1);
    const u32 = (i) => (at(i) * 0x1000000) + (at(i + 1) << 16) + (at(i + 2) << 8) + at(i + 3);
    const settings = {};
    let offset = 0;
    while (offset < buf.length) {
      const id = u16(offset);
      const value = u32(offset + 2);
      switch (id) {
        case 1: settings.headerTableSize = value; break;
        case 2: settings.enablePush = value !== 0; break;
        case 3: settings.maxConcurrentStreams = value; break;
        case 4: settings.initialWindowSize = value; break;
        case 5: settings.maxFrameSize = value; break;
        case 6: settings.maxHeaderListSize = settings.maxHeaderSize = value; break;
        case 8: settings.enableConnectProtocol = value !== 0; break;
        default:
          if (!settings.customSettings) settings.customSettings = {};
          settings.customSettings[id] = value;
      }
      offset += 6;
    }
    if (options != null && options.validate) validateSettings(settings);
    return settings;
  }

  // === internalBinding("http2") ==============================================
  // node's `lib/internal/http2/util.js` (and `core.js`/`compat.js` through it)
  // is evaluated with `internalBinding` in scope and reads a real C++ binding
  // from it. mbun resolves `require("internal/http2/util")` to node's OWN file,
  // so 23 corpus files — every one carrying `--expose-internals` — died on
  // `Error: No such binding: http2` before their first assertion: the six
  // `test-http2-util-*` / `-misc-util` files test that module directly, and the
  // rest only wanted a Symbol or a constant out of it.
  //
  // Blueprint: node src/node_http2.cc `Initialize()` (the exported typed-array
  // state buffers, the `nghttp2ErrorString`/`refreshDefaultSettings`/
  // `packSettings` methods and the `Http2Session`/`Http2Stream`/`Http2Ping`/
  // `Http2Settings` constructor templates), src/node_http2_state.h (the buffer
  // index enums that fix each array's length), src/node_http2.h
  // (`HTTP2_HIDDEN_CONSTANTS` — the NGHTTP2_HCAT_*/NV_FLAG_*/ERR_*/
  // STREAM_OPTION_* names that are on the binding but NOT on the public
  // `http2.constants`) and deps/nghttp2 `nghttp2_strerror()`.
  //
  // The state buffers are the real communication channel between node's JS and
  // C++ halves. mbun's http2 is implemented in JS and does not drive them, so
  // they are plain typed arrays of the right length and stay zero: that is what
  // `updateSettingsBuffer` / `updateOptionsBuffer` write into and read back,
  // which is all the util-level tests observe. Nothing here makes mbun's own
  // sessions route through the binding — a test that monkey-patches
  // `Http2Stream.prototype` still does not affect them (see DEFERRED).
  const kNghttp2Strerror = {
    0: "Success",
    "-501": "Invalid argument",
    "-502": "Out of buffer space",
    "-503": "Unsupported SPDY version",
    "-504": "Operation would block",
    "-505": "Protocol error",
    "-506": "Invalid frame octets",
    "-507": "EOF",
    "-508": "Data transfer deferred",
    "-509": "No more Stream ID available",
    "-510": "Stream was already closed or invalid",
    "-511": "Stream is closing",
    "-512": "The transmission is not allowed for this stream",
    "-513": "Stream ID is invalid",
    "-514": "Invalid stream state",
    "-515": "Another DATA frame has already been deferred",
    "-516": "request HEADERS is not allowed",
    "-517": "GOAWAY has already been sent",
    "-518": "Invalid header block",
    "-519": "Invalid state",
    "-521": "The user callback function failed due to the temporal error",
    "-522": "The length of the frame is invalid",
    "-523": "Header compression/decompression error",
    "-524": "Flow control error",
    "-525": "Insufficient buffer size given to function",
    "-526": "Callback was paused by the application",
    "-527": "Too many inflight SETTINGS",
    "-528": "Server push is disabled by peer",
    "-529": "DATA or HEADERS frame has already been submitted for the stream",
    "-530": "The current session is closing",
    "-531": "Invalid HTTP header field was received",
    "-532": "Violation in HTTP messaging rule",
    "-533": "Stream was refused",
    "-534": "Internal error",
    "-535": "Cancel",
    "-536": "When a local endpoint expects to receive SETTINGS frame, it receives an other type of frame",
    "-537": "SETTINGS frame contained more than the maximum allowed entries",
    "-901": "Out of memory",
    "-902": "The user callback function failed",
    "-903": "Received bad client magic byte string",
    "-904": "Flooding was detected in this HTTP/2 session, and it must be closed",
    "-905": "Too many CONTINUATION frames following a HEADER frame",
  };
  // node's `internal/http2/util.js` keys several observable properties off
  // *private* symbols it mints at load time (`kSocket`, `kSensitiveHeaders`,
  // `kProxySocket`, `kRequest`), and the corpus reaches for them directly:
  // `client[kSocket].destroy()`, `session[kSocket].emit('error', …)`,
  // `{ [sensitiveHeaders]: ['xyz'] }`. Symbol identity cannot be guessed, so
  // adopt the real ones from that module — but only once something has already
  // pulled the module in, which is exactly when `internalBinding("http2")` is
  // asked for. A program that never touches node internals never pays for it.
  let adoptedInternals = false;
  function adoptNodeHttp2Internals() {
    // Only meaningful once something asked for the binding; before that the
    // module is not loaded and requiring it here would be a cost (and a
    // resolution failure) for every ordinary http2 program.
    if (adoptedInternals || !bindingRequested) return;
    let u = null;
    try { u = G.require("internal/http2/util"); } catch (e) { adoptedInternals = true; return; }
    // The factory runs from util.js's own top level, so a very early call can
    // see a half-initialised module. Leave the flag clear and retry later
    // rather than locking in an empty result.
    if (u == null || typeof u.kSocket !== "symbol") return;
    adoptedInternals = true;
    // The Http2Session's real transport. node stores it under kSocket and
    // exposes the *proxy* as `.socket`; mbun's `.socket` is already the raw
    // socket, so both names resolve to the same object.
    const desc = { get() { return this.socket; }, set(v) { this.socket = v; }, configurable: true };
    Object.defineProperty(ClientHttp2Session.prototype, u.kSocket, desc);
    Object.defineProperty(ServerHttp2Session.prototype, u.kSocket, desc);
    if (typeof u.kSensitiveHeaders === "symbol" && sensitiveSymbols.indexOf(u.kSensitiveHeaders) < 0)
      sensitiveSymbols.push(u.kSensitiveHeaders);
  }
  if (typeof G.__mbunInternalBindingDefine === "function") {
    G.__mbunInternalBindingDefine("http2", () => {
      bindingRequested = true;
      G.queueMicrotask(adoptNodeHttp2Internals);
      // src/node_http2.h HTTP2_HIDDEN_CONSTANTS: on the binding only.
      const bindingConstants = Object.assign({ __proto__: null }, constants, {
        NGHTTP2_HCAT_REQUEST: 0,
        NGHTTP2_HCAT_RESPONSE: 1,
        NGHTTP2_HCAT_PUSH_RESPONSE: 2,
        NGHTTP2_HCAT_HEADERS: 3,
        NGHTTP2_NV_FLAG_NONE: 0,
        NGHTTP2_NV_FLAG_NO_INDEX: 1,
        NGHTTP2_ERR_DEFERRED: -508,
        NGHTTP2_ERR_STREAM_ID_NOT_AVAILABLE: -509,
        NGHTTP2_ERR_INVALID_ARGUMENT: -501,
        NGHTTP2_ERR_STREAM_CLOSED: -510,
        NGHTTP2_ERR_NOMEM: -901,
        STREAM_OPTION_EMPTY_PAYLOAD: 0x1,
        STREAM_OPTION_GET_TRAILERS: 0x2,
      });
      // Lengths from src/node_http2_state.h: IDX_SETTINGS_COUNT(7) + 1 flags
      // word + 1 count word + 2 * MAX_ADDITIONAL_SETTINGS(10) id/value pairs;
      // IDX_OPTIONS_FLAGS(13) + 1; IDX_SESSION_STATE_COUNT(9);
      // IDX_STREAM_STATE_COUNT(6); IDX_STREAM_STATS_COUNT(6);
      // IDX_SESSION_STATS_COUNT(9).
      const settingsBuffer = new Uint32Array(7 + 1 + 1 + 2 * MAX_ADDITIONAL_SETTINGS);
      const optionsBuffer = new Uint32Array(13 + 1);
      const sessionState = new Float64Array(9);
      const streamState = new Float64Array(6);
      // node Http2Settings::RefreshDefaults(): writes the seven compiled-in
      // defaults in Http2SettingsIndex order and then the *flags* word at
      // IDX_SETTINGS_COUNT saying which indices it filled. The flags word is not
      // optional — internal/http2/util.js getDefaultSettings() reads it and
      // returns an empty object for every bit that is clear.
      const refreshDefaultSettings = () => {
        settingsBuffer[0] = constants.DEFAULT_SETTINGS_HEADER_TABLE_SIZE;
        settingsBuffer[1] = constants.DEFAULT_SETTINGS_ENABLE_PUSH;
        settingsBuffer[2] = constants.DEFAULT_SETTINGS_INITIAL_WINDOW_SIZE;
        settingsBuffer[3] = constants.DEFAULT_SETTINGS_MAX_FRAME_SIZE;
        settingsBuffer[4] = constants.DEFAULT_SETTINGS_MAX_CONCURRENT_STREAMS;
        settingsBuffer[5] = constants.DEFAULT_SETTINGS_MAX_HEADER_LIST_SIZE;
        settingsBuffer[6] = constants.DEFAULT_SETTINGS_ENABLE_CONNECT_PROTOCOL;
        settingsBuffer[7] = 0x7f;   // IDX_SETTINGS_COUNT: all seven present
        settingsBuffer[8] = 0;      // no additional (custom) settings
      };
      refreshDefaultSettings();
      // node Http2Settings::Pack(): serialise whatever updateSettingsBuffer left
      // in settingsBuffer as a wire SETTINGS payload, in Http2SettingsIndex
      // order, honouring the flags word.
      const wireId = [1, 2, 4, 5, 3, 6, 8];
      const packSettings = () => {
        const flags = settingsBuffer[7];
        const entries = [];
        for (let i = 0; i < 7; i++)
          if (flags & (1 << i)) entries.push([wireId[i], settingsBuffer[i]]);
        const n = settingsBuffer[8];
        for (let i = 0; i < n; i++)
          entries.push([settingsBuffer[9 + 2 * i], settingsBuffer[10 + 2 * i]]);
        const out = Buffer.alloc(entries.length * 6);
        for (let i = 0; i < entries.length; i++) {
          out.writeUInt16BE(entries[i][0], i * 6);
          out.writeUInt32BE(entries[i][1] >>> 0, i * 6 + 2);
        }
        return out;
      };
      // The four constructor templates node exports. mbun's sessions/streams are
      // plain JS classes and never instantiate these, but a test that reads
      // `Http2Stream.prototype` to stub a method needs the object to exist.
      class Http2Session {}
      class Http2Stream {}
      class Http2Ping {}
      class Http2Settings {}
      return {
        constants: bindingConstants,
        settingsBuffer, optionsBuffer, sessionState, streamState,
        streamStats: new Float64Array(6),
        sessionStats: new Float64Array(9),
        nghttp2ErrorString: (code) => {
          const s = kNghttp2Strerror[String(code | 0)];
          return s === undefined ? "Unknown error code" : s;
        },
        refreshDefaultSettings,
        packSettings,
        setCallbackFunctions: () => {},
        Http2Session, Http2Stream, Http2Ping, Http2Settings,
        // src/node_http2.cc NODE_DEFINE_CONSTANT block (session uint8 fields).
        kBitfield: 0,
        kSessionPriorityListenerCount: 1,
        kSessionFrameErrorListenerCount: 2,
        kSessionMaxInvalidFrames: 3,
        kSessionMaxRejectedStreams: 4,
        kSessionUint8FieldCount: 5,
        kSessionHasRemoteSettingsListeners: 0,
        kSessionRemoteSettingsIsUpToDate: 1,
        kSessionHasPingListeners: 2,
        kSessionHasAltsvcListeners: 3,
      };
    });
  }

  // === install over the deferred stub (bootstrap.cppm def(["http2"], ...)) ===
  http2.constants = constants;
  http2.connect = connect;
  http2.getDefaultSettings = getDefaultSettings;
  http2.getPackedSettings = getPackedSettings;
  http2.getUnpackedSettings = getUnpackedSettings;
  // A getter, not a value: when node's internals are in play the correct symbol
  // is node's own private one, and it only becomes reachable after
  // internal/http2/util.js has finished loading.
  Object.defineProperty(http2, "sensitiveHeaders", {
    get() { adoptNodeHttp2Internals(); return sensitiveSymbols[sensitiveSymbols.length - 1]; },
    enumerable: true, configurable: true,
  });
  http2.Http2Session = ClientHttp2Session;
  http2.ClientHttp2Session = ClientHttp2Session;
  http2.ClientHttp2Stream = ClientHttp2Stream;
  http2.createServer = makeHttp2Server;
  http2.createSecureServer = makeHttp2SecureServer;
  http2.performServerHandshake = performServerHandshake;
  installStreamInspect(ClientHttp2Stream);
  installStreamInspect(ServerHttp2Stream);
  installSessionInspect(ClientHttp2Session);
  installSessionInspect(ServerHttp2Session);
  http2.ServerHttp2Session = ServerHttp2Session;
  http2.ServerHttp2Stream = ServerHttp2Stream;
  http2.Http2ServerRequest = Http2ServerRequest;
  http2.Http2ServerResponse = Http2ServerResponse;
})();

)JS";

}  // namespace mbun::jsc::js_http2
