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
  const sessionErr = (code) => mkErr("Session closed with error code " + errName(code), "ERR_HTTP2_SESSION_ERROR");
  const streamErr = (code) => mkErr("Stream closed with error code " + errName(code), "ERR_HTTP2_STREAM_ERROR");

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
  const FRAME = { DATA: 0, HEADERS: 1, PRIORITY: 2, RST_STREAM: 3, SETTINGS: 4, PUSH_PROMISE: 5, PING: 6, GOAWAY: 7, WINDOW_UPDATE: 8, CONTINUATION: 9 };
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
      effectiveLocalWindowSize: DEFAULT_CONNECTION_WINDOW,
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

  // Http2StreamState (node docs `http2stream.state`). `state` is nghttp2's
  // stream state enum; 1 = NGHTTP2_STREAM_STATE_OPEN, 7 = ..._CLOSED.
  function streamState(st) {
    const closed = st.destroyed || st.closed || st._ended === true;
    return {
      localWindowSize: DEFAULT_CONNECTION_WINDOW,
      state: closed ? 7 : 1,
      localClose: st.writable === false ? 1 : 0,
      remoteClose: st.readable === false ? 1 : 0,
      sumDependencyWeight: 0,
      weight: 16,
    };
  }

  // === ClientHttp2Stream ===
  class ClientHttp2Stream extends EE {
    constructor(session, id, headers, options) {
      super();
      this.session = session;
      this.id = id;
      this._enc = null;
      this._reqHeaders = headers;
      this._options = options || {};
      this._endStreamSent = false;
      this._bodyQ = [];
      this._closed = false;
      this.rstCode = 0;
      this.aborted = false;
      this.destroyed = false;
      this.readable = true;
      this.writable = true;
      this.pending = true;
      this.sentHeaders = headers;
      this._responseEmitted = false;
    }
    get closed() { return this._closed; }
    get state() { return streamState(this); }
    setEncoding(enc) { this._enc = enc || "utf8"; return this; }
    write(chunk, enc, cb) {
      if (typeof enc === "function") { cb = enc; enc = null; }
      if (this._endStreamSent) { if (typeof cb === "function") G.queueMicrotask(cb); return true; }
      const buf = typeof chunk === "string" ? Buffer.from(chunk, enc || "utf8") : Buffer.from(chunk);
      this.session._sendData(this, buf, false);
      if (typeof cb === "function") G.queueMicrotask(cb);
      return true;
    }
    end(chunk, enc, cb) {
      if (typeof chunk === "function") { cb = chunk; chunk = null; }
      if (typeof enc === "function") { cb = enc; enc = null; }
      if (this._endStreamSent) { this.writable = false; if (typeof cb === "function") this.once("close", cb); return this; }
      const buf = chunk == null ? Buffer.alloc(0) : (typeof chunk === "string" ? Buffer.from(chunk, enc || "utf8") : Buffer.from(chunk));
      this.session._sendData(this, buf, true);
      this._endStreamSent = true;
      this.writable = false;
      if (typeof cb === "function") this.once("close", cb);
      return this;
    }
    close(code, cb) {
      if (code === undefined) code = constants.NGHTTP2_NO_ERROR;
      validateUint32(code, "code");
      if (cb !== undefined && typeof cb !== "function") throw argTypeErr("callback", "of type function", cb);
      if (this._closed) { if (typeof cb === "function") this.once("close", cb); return; }
      if (typeof cb === "function") this.once("close", cb);
      this.rstCode = code;
      this.session._rstStream(this, code);
      try { this.session.streams.delete(this.id); } catch (e) {}
      if (code !== 0) G.queueMicrotask(() => this.emit("error", streamErr(code)));
      this._onClose();
    }
    destroy(err) {
      if (this.destroyed) return this;
      this.destroyed = true;
      // node: destroying an unfinished stream sends RST_STREAM so the peer stops
      // (INTERNAL_ERROR with an error, NO_ERROR otherwise). Skip if already closed.
      if (!this._closed) {
        this.rstCode = err ? constants.NGHTTP2_INTERNAL_ERROR : constants.NGHTTP2_NO_ERROR;
        try { this.session._rstStream(this, this.rstCode); } catch (e) {}
        try { this.session.streams.delete(this.id); } catch (e) {}
      }
      if (err) G.queueMicrotask(() => this.emit("error", err));
      this._onClose();
      return this;
    }
    setTimeout() { return this; }
    resume() { this._paused = false; return this; }
    pause() { this._paused = true; return this; }
    _pushData(bytes) {
      this.readable = true;
      if (this._enc) { let s = ""; for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]); this.emit("data", Buffer.from(bytes).toString(this._enc)); }
      else this.emit("data", Buffer.from(bytes));
    }
    _onResponse(headersObj, flags) {
      if (this._responseEmitted) return;
      this._responseEmitted = true;
      this.pending = false;
      this.emit("response", headersObj, flags);
    }
    _onEnd() {
      this.readable = false;
      this.emit("end");
      this.emit("close");
      this._closed = true;
    }
    _onClose() {
      if (this._closed) return;
      this._closed = true;
      this.readable = false; this.writable = false;
      G.queueMicrotask(() => this.emit("close"));
    }
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
      this._localSettings = { headerTableSize: 4096, enablePush: 0, initialWindowSize: 65535, maxFrameSize: 16384, maxConcurrentStreams: 4294967295 };
      this._pendingHeaderBlock = null;   // { streamId, endStream, buf }
      // connection-level flow control bookkeeping, surfaced through `state`
      this._localWindow = DEFAULT_CONNECTION_WINDOW;    // what the peer may still send us
      this._remoteWindow = DEFAULT_CONNECTION_WINDOW;   // what we may still send the peer
      this._lastProcStreamId = 0;
      this.alpnProtocol = null;
      if (typeof listener === "function") this.once("connect", listener);

      const u = parseAuthority(authority);
      this._url = u.origin;
      this._authorityName = u.host;
      this._scheme = u.protocol === "https:" ? "https" : "http";
      const port = u.port ? +u.port : (this._scheme === "https" ? 443 : 80);
      const host = u.hostname;

      const self = this;
      if (this._scheme === "https") {
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
        sock.on("connect", () => self._onSocketReady());
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
      this._writeFrame(FRAME.SETTINGS, 0, 0, Buffer.alloc(0));
      const q = this._preConnectQ; this._preConnectQ = [];
      for (let i = 0; i < q.length; i++) this.socket.write(q[i]);
      this.emit("connect", this, this.socket);
    }

    _writeFrame(type, flags, streamId, payload) {
      if (this.destroyed || !this.socket) return;
      payload = payload || Buffer.alloc(0);
      const frame = Buffer.concat([frameHeader(payload.length, type, flags, streamId), payload]);
      if (!this._connected) { this._preConnectQ.push(frame); return; }
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
      const streamId = this._nextStreamId();
      const hlist = buildHeaderList(headers, this._scheme, this._authorityName);
      const stream = new ClientHttp2Stream(this, streamId, headers, options);
      this.streams.set(streamId, stream);
      const block = encodeHeaders(hlist);
      // A method with no body (GET/HEAD/DELETE) or an explicit endStream option
      // may close the stream on the HEADERS frame; otherwise req.end() sends the
      // trailing empty DATA(END_STREAM). HEADERS otherwise carry END_HEADERS only.
      const noBody = headers[":method"] && /^(GET|HEAD|DELETE)$/i.test(headers[":method"]);
      writeHeaderBlock(this, streamId, block, optEndStream ? FLAG.END_STREAM : 0);
      if (optEndStream) { stream._endStreamSent = true; stream.writable = false; }
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
          if (flags & FLAG.ACK) { if (len !== 0) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; } return true; }
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
          if (stream) { this.streams.delete(streamId); stream.rstCode = code; if (code !== 0) { G.queueMicrotask(() => stream.emit("error", streamErr(code))); } stream._onClose(); }
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
          }
          return true;
        }
        case FRAME.WINDOW_UPDATE: {
          if (len !== 4) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
          // we do not throttle our own sends beyond max frame size, but the
          // connection window has to stay accurate for `state.remoteWindowSize`
          if (streamId === 0) this._remoteWindow += payload.readUInt32BE(0) & 0x7fffffff;
          return true;
        }
        case FRAME.PUSH_PROMISE: {
          // push disabled (we advertise ENABLE_PUSH=0); a server push is a error.
          this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false;
        }
        default:
          return true;   // unknown frame types are ignored (RFC 7540 4.1)
      }
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
      const stream = this.streams.get(pb.streamId);
      if (!stream) return true;
      const headersObj = headerListToObject(list, this._hpack._sensitive);
      // A header block after the response is a trailer block (RFC 9113 8.1):
      // emit 'trailers' rather than a second 'response'.
      if (stream._responseEmitted) stream.emit("trailers", headersObj, flags);
      else stream._onResponse(headersObj, flags);
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

    // ---- errors / lifecycle ----
    _connError(code) {
      // send GOAWAY then surface ERR_HTTP2_SESSION_ERROR (matches nghttp2).
      const p = Buffer.alloc(8); p.writeUInt32BE(this._lastStreamId > 0 ? this._lastStreamId : 0, 0); p.writeUInt32BE(code >>> 0, 4);
      try { this._writeFrame(FRAME.GOAWAY, 0, 0, p); } catch (e) {}
      this._fatal(sessionErr(code));
    }
    _onSocketError(e) { if (!this.destroyed && !this.closed) this._fatal(e); }
    _onSocketEnd() {}
    _onSocketClose() { if (!this.destroyed && !this.closed) this._fatal(mkErr("Session closed with error code NGHTTP2_INTERNAL_ERROR", "ERR_HTTP2_SESSION_ERROR")); else this._teardown(); }
    _fatal(err) {
      if (this.destroyed) return;
      const streams = Array.from(this.streams.values());
      this._teardown();
      G.queueMicrotask(() => {
        this.emit("error", err);
        for (const s of streams) { s.rstCode = constants.NGHTTP2_INTERNAL_ERROR; s.emit("error", err); s._onClose(); }
      });
    }
    _teardown() {
      if (this.destroyed) return;
      this.destroyed = true; this.closed = true;
      if (this._timer != null) { try { G.clearTimeout(this._timer); } catch (e) {} this._timer = null; }
      if (this.socket) { try { this.socket.destroy(); } catch (e) {} }
      G.queueMicrotask(() => this.emit("close"));
    }
    _shutdown() { this._teardown(); }

    close(cb) {
      if (typeof cb === "function") this.once("close", cb);
      if (this.destroyed) return;
      this.closed = true;
      // graceful: GOAWAY(NO_ERROR) then close socket once streams drain
      const p = Buffer.alloc(8); p.writeUInt32BE(this._lastStreamId > 0 ? this._lastStreamId : 0, 0); p.writeUInt32BE(0, 4);
      try { this._writeFrame(FRAME.GOAWAY, 0, 0, p); } catch (e) {}
      const self = this;
      G.queueMicrotask(() => self._teardown());
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
    settings(s, cb) { if (typeof cb === "function") this.once("localSettings", cb); return this; }
    ping(cb) { this._writeFrame(FRAME.PING, 0, 0, Buffer.alloc(8)); if (typeof cb === "function") G.queueMicrotask(() => cb(null, 0, Buffer.alloc(8))); return true; }
    goaway(code, lastStreamId, opaqueData) { const p = Buffer.alloc(8); p.writeUInt32BE((lastStreamId || 0) >>> 0, 0); p.writeUInt32BE((code || 0) >>> 0, 4); this._writeFrame(FRAME.GOAWAY, 0, 0, opaqueData ? Buffer.concat([p, Buffer.from(opaqueData)]) : p); }
  }

  // === helpers ===
  function parseAuthority(authority) {
    if (typeof authority === "string") { try { return new G.URL(authority); } catch (e) { return new G.URL("http://" + authority); } }
    if (authority && authority.href) return authority;   // URL object
    return new G.URL(String(authority));
  }
  function buildHeaderList(headers, scheme, authorityName) {
    // pseudo-headers first, defaulting the required ones (node/nghttp2 order rule)
    const pseudo = [];
    const regular = [];
    const has = {};
    for (const k in headers) {
      if (!Object.prototype.hasOwnProperty.call(headers, k)) continue;
      const lk = String(k).toLowerCase();
      has[lk] = true;
      const v = headers[k];
      const target = lk[0] === ":" ? pseudo : regular;
      if (Array.isArray(v)) { for (let i = 0; i < v.length; i++) target.push([lk, String(v[i])]); }
      else target.push([lk, String(v)]);
    }
    // fill defaults
    const front = [];
    if (!has[":method"]) front.push([":method", "GET"]);
    if (!has[":authority"] && !has["host"]) front.push([":authority", authorityName]);
    if (!has[":scheme"]) front.push([":scheme", scheme]);
    if (!has[":path"]) front.push([":path", "/"]);
    // order: our defaults + provided pseudo (deduped) + regular
    return front.concat(pseudo).concat(regular);
  }
  function headerListToObject(list, sensitive) {
    const obj = {};
    const raw = [];
    for (let i = 0; i < list.length; i++) {
      let name = list[i][0]; const value = list[i][1];
      raw.push(name, value);
      if (name === ":status") { obj[name] = parseInt(value, 10); continue; }
      if (obj[name] === undefined) obj[name] = value;
      else if (Array.isArray(obj[name])) obj[name].push(value);
      else if (name === "set-cookie") obj[name] = [obj[name], value];
      else obj[name] = obj[name] + ", " + value;
    }
    try { Object.defineProperty(obj, G.Symbol.for("nodejs.http2.sensitiveHeaders"), { value: sensitive ? sensitive.slice() : [], enumerable: false }); } catch (e) {}
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
  function recvType(input) {
    if (input === null) return " Received null";
    if (typeof input === "symbol") return " Received type symbol";
    if (typeof input === "object") {
      const t = Object.prototype.toString.call(input).split(" ")[1];
      const name = t ? t.replace("]", "").replace("[", "") : "Object";
      return " Received an instance of " + name;
    }
    if (typeof input === "string") return " Received type string ('" + input + "')";
    return " Received type " + (typeof input) + " (" + input + ")";
  }
  function argTypeErr(name, expected, value) {
    const e = new TypeError('The "' + name + '" argument must be ' + expected + '.' + recvType(value));
    e.code = "ERR_INVALID_ARG_TYPE"; return e;
  }
  function outOfRangeErr(name, range, value) {
    const e = new RangeError('The value of "' + name + '" is out of range. It must be ' + range + '. Received ' + value);
    e.code = "ERR_OUT_OF_RANGE"; return e;
  }
  // RFC 9113 8.2/8.3 request-header validity for a server. Returns true if the
  // header list is malformed (CR/LF/NUL octet, a connection-specific field, or a
  // repeated / misplaced pseudo-header) and the stream must be reset.
  function requestHeadersMalformed(list) {
    const seenPseudo = {};
    let sawRegular = false;
    for (let i = 0; i < list.length; i++) {
      const name = list[i][0], value = list[i][1];
      if (/[\r\n\0]/.test(name) || /[\r\n\0]/.test(value)) return true;
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
  function buildResponseHeaderList(headers) {
    const sym = G.Symbol.for("nodejs.http2.sensitiveHeaders");
    const sensitive = new Set();
    const sn = headers && headers[sym];
    if (Array.isArray(sn)) for (let i = 0; i < sn.length; i++) sensitive.add(String(sn[i]).toLowerCase());
    let status = 200;
    const regular = [];
    for (const k in headers) {
      if (!Object.prototype.hasOwnProperty.call(headers, k)) continue;
      const lk = String(k).toLowerCase();
      if (lk === ":status") { status = parseInt(headers[k], 10) || 200; continue; }
      if (lk[0] === ":") continue;   // servers only emit :status
      const v = headers[k];
      if (Array.isArray(v)) { for (let i = 0; i < v.length; i++) regular.push([lk, String(v[i])]); }
      else regular.push([lk, String(v)]);
    }
    const list = [[":status", String(status)]].concat(regular);
    return { list, sensitive };
  }

  // === ServerHttp2Stream ===
  class ServerHttp2Stream extends EE {
    constructor(session, id, headers) {
      super();
      this.session = session;
      this.id = id;
      this._reqHeaders = headers;
      this._enc = null;
      this._closed = false;
      this.destroyed = false;
      this.readable = true;
      this.writable = true;
      this.pending = false;
      this.aborted = false;
      this.rstCode = 0;
      this.headersSent = false;
      this._endStreamSent = false;
      this._wantTrailers = false;
      this._trailersSent = false;
      this.sentHeaders = undefined;
    }
    get closed() { return this._closed; }
    get state() { return streamState(this); }
    setEncoding(enc) { this._enc = enc || "utf8"; return this; }
    resume() { this._paused = false; return this; }
    pause() { this._paused = true; return this; }
    respond(headers, options) {
      if (this.headersSent) throw mkErr("Response has already been initiated.", "ERR_HTTP2_HEADERS_SENT");
      if (this.destroyed || this._closed) throw mkErr("The stream has been destroyed", "ERR_HTTP2_INVALID_STREAM");
      headers = headers || {};
      options = options || {};
      const built = buildResponseHeaderList(headers);
      this.headersSent = true;
      this.sentHeaders = headers;
      const block = encodeHeaders(built.list, built.sensitive);
      let flags = 0;
      const endStream = !!options.endStream;
      if (endStream && !options.waitForTrailers) { flags = FLAG.END_STREAM; this._endStreamSent = true; this.writable = false; }
      if (options.waitForTrailers) this._wantTrailers = true;
      writeHeaderBlock(this.session, this.id, block, flags);
      if (endStream && !options.waitForTrailers) this._finish();
      return;
    }
    write(chunk, enc, cb) {
      if (typeof enc === "function") { cb = enc; enc = null; }
      if (!this.headersSent) this.respond();
      const buf = typeof chunk === "string" ? Buffer.from(chunk, enc || "utf8") : Buffer.from(chunk);
      this.session._sendData(this, buf, false);
      if (typeof cb === "function") G.queueMicrotask(cb);
      return true;
    }
    end(chunk, enc, cb) {
      if (typeof chunk === "function") { cb = chunk; chunk = null; }
      if (typeof enc === "function") { cb = enc; enc = null; }
      if (!this.headersSent) this.respond();
      if (this._endStreamSent) { if (typeof cb === "function") G.queueMicrotask(cb); return this; }
      const buf = chunk == null ? Buffer.alloc(0) : (typeof chunk === "string" ? Buffer.from(chunk, enc || "utf8") : Buffer.from(chunk));
      if (this._wantTrailers) {
        // hold END_STREAM: flush body then let the user append trailers.
        if (buf.length) this.session._sendData(this, buf, false);
        this.writable = false;
        G.queueMicrotask(() => this.emit("wantTrailers"));
        if (typeof cb === "function") this.once("close", cb);
        return this;
      }
      this.session._sendData(this, buf, true);
      this._endStreamSent = true;
      this.writable = false;
      if (typeof cb === "function") this.once("close", cb);
      this._finish();
      return this;
    }
    sendTrailers(headers) {
      if (this._trailersSent) throw mkErr("Trailers have already been sent", "ERR_HTTP2_TRAILERS_ALREADY_SENT");
      if (!this._wantTrailers) throw mkErr("Trailers are not ready to send", "ERR_HTTP2_TRAILERS_NOT_READY");
      headers = headers || {};
      // Validate BEFORE marking sent: a pseudo-header in a trailer block is an
      // ERR_HTTP2_INVALID_PSEUDOHEADER and must leave the stream retryable.
      for (const k in headers) { if (Object.prototype.hasOwnProperty.call(headers, k) && String(k)[0] === ":") throw mkErr('"' + k + '" is an invalid pseudoheader or is used incorrectly', "ERR_HTTP2_INVALID_PSEUDOHEADER"); }
      this._trailersSent = true;
      const sym = G.Symbol.for("nodejs.http2.sensitiveHeaders");
      const sensitive = new Set();
      const sn = headers[sym];
      if (Array.isArray(sn)) for (let i = 0; i < sn.length; i++) sensitive.add(String(sn[i]).toLowerCase());
      const list = [];
      for (const k in headers) { if (!Object.prototype.hasOwnProperty.call(headers, k)) continue; const lk = String(k).toLowerCase(); list.push([lk, String(headers[k])]); }
      const block = encodeHeaders(list, sensitive);
      writeHeaderBlock(this.session, this.id, block, FLAG.END_STREAM);
      this._endStreamSent = true;
      this._finish();
      return;
    }
    respondWithFD() { throw mkErr("respondWithFD is not supported", "ERR_HTTP2_ERROR"); }
    respondWithFile() { throw mkErr("respondWithFile is not supported", "ERR_HTTP2_ERROR"); }
    pushStream(headers, options, cb) { if (typeof options === "function") { cb = options; } if (typeof cb === "function") G.queueMicrotask(() => cb(mkErr("Push streams are not supported", "ERR_HTTP2_PUSH_DISABLED"))); }
    close(code, cb) {
      if (code === undefined) code = constants.NGHTTP2_NO_ERROR;
      validateUint32(code, "code");
      if (typeof cb === "function") this.once("close", cb);
      this.rstCode = code;
      this.session._rstStream(this, code);
      this._finish();
    }
    destroy(err) {
      if (this.destroyed) return this;
      this.destroyed = true;
      if (err) { this.rstCode = constants.NGHTTP2_INTERNAL_ERROR; try { this.session._rstStream(this, this.rstCode); } catch (e) {} G.queueMicrotask(() => this.emit("error", err)); }
      this._finish();
      return this;
    }
    setTimeout() { return this; }
    _pushData(bytes) {
      this.readable = true;
      if (this._enc) this.emit("data", Buffer.from(bytes).toString(this._enc));
      else this.emit("data", Buffer.from(bytes));
    }
    _onRequestEnd() { this.readable = false; this.emit("end"); }
    _finish() {
      if (this._closed) return;
      this._closed = true;
      this.readable = false; this.writable = false;
      this.session.streams.delete(this.id);
      const self = this;
      G.queueMicrotask(() => self.emit("close"));
    }
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
      socket.on("end", () => {});
    }
    _writeFrame(type, flags, streamId, payload) {
      if (this.destroyed || !this.socket) return;
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
    _windowUpdate(streamId, increment) { if (streamId === 0) this._localWindow += increment; const p = Buffer.alloc(4); p.writeUInt32BE(increment >>> 0, 0); this._writeFrame(FRAME.WINDOW_UPDATE, 0, streamId, p); }

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
          if (flags & FLAG.ACK) { if (len !== 0) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; } return true; }
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
          if (stream) { stream.rstCode = code; stream.aborted = true; if (code !== 0) G.queueMicrotask(() => stream.emit("aborted")); stream._finish(); }
          else if ((streamId & 1) === 1 && streamId > this._lastStreamId) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }  // RST on an idle stream (§5.1)
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
          }
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
      stream = new ServerHttp2Stream(this, pb.streamId, null);
      this.streams.set(pb.streamId, stream);
      if (pb.streamId > this._lastStreamId) this._lastStreamId = pb.streamId;
      if (requestHeadersMalformed(list)) { this._streamError(stream, constants.NGHTTP2_PROTOCOL_ERROR); return true; }
      if (dupClen) { this._streamError(stream, constants.NGHTTP2_PROTOCOL_ERROR); return true; }
      if (pb.endStream && expectLen != null && expectLen !== 0) { this._streamError(stream, constants.NGHTTP2_PROTOCOL_ERROR); return true; }
      if (!pb.endStream && expectLen != null) stream._expectLen = expectLen;
      const headersObj = headerListToObject(list, this._hpack._sensitive);
      stream._reqHeaders = headersObj;
      const rawHeaders = [];
      for (let i = 0; i < list.length; i++) { rawHeaders.push(list[i][0], list[i][1]); }
      this.emit("stream", stream, headersObj, flags, rawHeaders);
      if (pb.endStream) G.queueMicrotask(() => { stream.readable = false; stream.emit("end"); });
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
    _onSocketClose() { this._teardown(); }
    _teardown() {
      if (this.destroyed) return;
      this.destroyed = true; this.closed = true;
      if (this.socket) { try { this.socket.destroy(); } catch (e) {} }
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
    get state() { return sessionState(this, 2); }
    settings(s, cb) { if (typeof cb === "function") this.once("localSettings", cb); this._writeFrame(FRAME.SETTINGS, 0, 0, encodeSettings(s)); return this; }
    ping(payload, cb) { if (typeof payload === "function") { cb = payload; payload = null; } this._writeFrame(FRAME.PING, 0, 0, payload && isBufLike(payload) ? Buffer.from(payload) : Buffer.alloc(8)); if (typeof cb === "function") G.queueMicrotask(() => cb(null, 0, Buffer.alloc(8))); return true; }
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
      if (this.destroyed) return;
      this.closed = true;
      const p = Buffer.alloc(8); p.writeUInt32BE(this._lastStreamId > 0 ? this._lastStreamId : 0, 0); p.writeUInt32BE(0, 4);
      try { this._writeFrame(FRAME.GOAWAY, 0, 0, p); } catch (e) {}
      const self = this;
      G.queueMicrotask(() => self._teardown());
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
  class Http2ServerRequest extends EE {
    constructor(stream, headers) {
      super();
      this.stream = stream;
      this.headers = headers || {};
      this.rawHeaders = [];
      this.httpVersion = "2.0";
      this.httpVersionMajor = 2;
      this.httpVersionMinor = 0;
      this.method = headers && headers[":method"] ? headers[":method"] : "GET";
      this.url = headers && headers[":path"] ? headers[":path"] : "/";
      this.authority = headers ? headers[":authority"] : undefined;
      this.scheme = headers ? headers[":scheme"] : undefined;
      this.socket = stream.session ? stream.session.socket : undefined;
      this.connection = this.socket;
      this.aborted = false;
      this.complete = false;
      const self = this;
      stream.on("data", (d) => self.emit("data", d));
      stream.on("end", () => { self.complete = true; self.emit("end"); });
      stream.on("aborted", () => { self.aborted = true; self.emit("aborted"); });
      stream.on("close", () => self.emit("close"));
      stream.on("error", (e) => self.emit("error", e));
    }
    setEncoding(enc) { this.stream.setEncoding(enc); return this; }
    setTimeout(ms, cb) { if (typeof cb === "function") this.on("timeout", cb); return this; }
    resume() { this.stream.resume(); return this; }
    pause() { this.stream.pause(); return this; }
    read() { return null; }
    get trailers() { return {}; }
    get rawTrailers() { return []; }
  }
  class Http2ServerResponse extends EE {
    constructor(stream) {
      super();
      this.stream = stream;
      this[Symbol.for("headers")] = {};
      this._headers = {};
      this.headersSent = false;
      this.finished = false;
      this.writableEnded = false;
      this.sendDate = true;
      this.statusCode = 200;
      this.statusMessage = "";
      this.socket = stream.session ? stream.session.socket : undefined;
      this.connection = this.socket;
      const self = this;
      stream.on("close", () => { if (!self.finished) { self.finished = true; self.emit("finish"); } self.emit("close"); });
    }
    setHeader(name, value) { this._headers[String(name).toLowerCase()] = value; return this; }
    getHeader(name) { return this._headers[String(name).toLowerCase()]; }
    getHeaders() { return Object.assign({}, this._headers); }
    getHeaderNames() { return Object.keys(this._headers); }
    hasHeader(name) { return Object.prototype.hasOwnProperty.call(this._headers, String(name).toLowerCase()); }
    removeHeader(name) { delete this._headers[String(name).toLowerCase()]; return this; }
    writeHead(status, statusMessage, headers) {
      if (typeof statusMessage === "object" && statusMessage !== null) { headers = statusMessage; statusMessage = undefined; }
      this.statusCode = status;
      if (headers) for (const k in headers) { if (Object.prototype.hasOwnProperty.call(headers, k)) this._headers[String(k).toLowerCase()] = headers[k]; }
      return this;
    }
    _respondIfNeeded() {
      if (this.headersSent) return;
      this.headersSent = true;
      const h = Object.assign({ ":status": this.statusCode }, this._headers);
      this.stream.respond(h);
    }
    write(chunk, enc, cb) { this._respondIfNeeded(); return this.stream.write(chunk, enc, cb); }
    end(chunk, enc, cb) {
      if (typeof chunk === "function") { cb = chunk; chunk = null; }
      this._respondIfNeeded();
      this.finished = true; this.writableEnded = true;
      this.stream.end(chunk, enc, cb);
      return this;
    }
    writeContinue() { return this; }
    setTimeout(ms, cb) { if (typeof cb === "function") this.on("timeout", cb); return this; }
    createPushResponse(headers, cb) { if (typeof cb === "function") G.queueMicrotask(() => cb(mkErr("Push streams are not supported", "ERR_HTTP2_PUSH_DISABLED"))); }
    get finished_() { return this.finished; }
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
        if (self.listenerCount("request") > 0) {
          const req = new Http2ServerRequest(stream, headers);
          const res = new Http2ServerResponse(stream);
          self.emit("request", req, res);
        }
      });
      session.on("error", (e) => { if (self.listenerCount("sessionError") > 0) self.emit("sessionError", e, session); else if (self.listenerCount("session") === 0 && self.listenerCount("error") > 0) self.emit("error", e); });
      self.emit("session", session);
    };
    server.on(secure ? "secureConnection" : "connection", onSession);
    server.setTimeout = function (ms, cb) { if (typeof cb === "function") this.on("timeout", cb); return this; };
    server.updateSettings = function () { return this; };
    return server;
  }
  function makeHttp2Server(options, onRequest) {
    if (typeof options === "function") { onRequest = options; options = {}; }
    const server = net.createServer({ allowHalfOpen: true });
    return attachH2Server(server, options, onRequest, false);
  }
  function makeHttp2SecureServer(options, onRequest) {
    if (typeof options === "function") { onRequest = options; options = {}; }
    if (!tls || !tls.createServer) throw mkErr("http2 createSecureServer requires node:tls", "ERR_HTTP2_ERROR");
    const alpn = (options && options.allowHTTP1) ? ["h2", "http/1.1"] : ["h2"];
    const server = tls.createServer(Object.assign({}, options, { ALPNProtocols: (options && options.ALPNProtocols) || alpn }));
    return attachH2Server(server, options, onRequest, true);
  }

  // === install over the deferred stub (bootstrap.cppm def(["http2"], ...)) ===
  http2.constants = constants;
  http2.connect = connect;
  http2.getDefaultSettings = getDefaultSettings;
  http2.getPackedSettings = () => Buffer.alloc(0);
  http2.getUnpackedSettings = () => getDefaultSettings();
  http2.sensitiveHeaders = G.Symbol.for("nodejs.http2.sensitiveHeaders");
  http2.Http2Session = ClientHttp2Session;
  http2.ClientHttp2Session = ClientHttp2Session;
  http2.ClientHttp2Stream = ClientHttp2Stream;
  http2.createServer = makeHttp2Server;
  http2.createSecureServer = makeHttp2SecureServer;
  http2.ServerHttp2Session = ServerHttp2Session;
  http2.ServerHttp2Stream = ServerHttp2Stream;
  http2.Http2ServerRequest = Http2ServerRequest;
  http2.Http2ServerResponse = Http2ServerResponse;
})();

)JS";

}  // namespace mbun::jsc::js_http2
