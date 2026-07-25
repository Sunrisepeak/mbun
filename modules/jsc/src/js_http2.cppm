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
  // setEncoding() on an Http2Stream is stream.Readable's: a StringDecoder holds
  // the tail of a multi-byte sequence that straddles two DATA frames. Decoding
  // each frame independently corrupted every body larger than one max frame
  // whose split landed inside a UTF-8 character (test-http2-respond-file's
  // 30 kB ellipsis fixture is exactly that shape).
  function decodeChunk(stream, bytes) {
    const buf = Buffer.from(bytes);
    if (stream._decoder === undefined) {
      const SD = M["string_decoder"] || M["node:string_decoder"];
      stream._decoder = (SD && SD.StringDecoder) ? new SD.StringDecoder(stream._enc) : null;
    }
    return stream._decoder ? stream._decoder.write(buf) : buf.toString(stream._enc);
  }
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
    setEncoding(enc) { this._enc = enc || "utf8"; this._decoder = undefined; return this; }
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
      // request(headers, { waitForTrailers }): hold END_STREAM back, flush the
      // body, then let the user append a trailer block (node Http2Stream
      // kWaitForTrailers -> 'wantTrailers' -> sendTrailers()).
      if (this._options.waitForTrailers) {
        if (buf.length) this.session._sendData(this, buf, false);
        this.writable = false;
        if (typeof cb === "function") this.once("close", cb);
        G.queueMicrotask(() => { this._trailersReady = true; this.emit("wantTrailers"); });
        return this;
      }
      this.session._sendData(this, buf, true);
      this._endStreamSent = true;
      this.writable = false;
      if (typeof cb === "function") this.once("close", cb);
      return this;
    }
    sendTrailers(headers) {
      if (this._trailersSent) throw mkErr("Trailers have already been sent", "ERR_HTTP2_TRAILERS_ALREADY_SENT");
      if (!this._trailersReady) throw mkErr("Trailers are not ready to send", "ERR_HTTP2_TRAILERS_NOT_READY");
      headers = headers || {};
      for (const k of Object.keys(headers))
        if (String(k)[0] === ":") throw mkErr('"' + k + '" is an invalid pseudoheader or is used incorrectly', "ERR_HTTP2_INVALID_PSEUDOHEADER");
      this._trailersSent = true;
      const built = buildNgHeaders(Object.assign({ __proto__: null }, headers), assertValidRequestPseudoHeader, this.session._options && this.session._options.strictSingleValueFields);
      writeHeaderBlock(this.session, this.id, encodeHeaders(built.list, built.sensitive), FLAG.END_STREAM);
      this._endStreamSent = true;
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
      if (this._enc) this.emit("data", decodeChunk(this, bytes));
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
      const self = this;
      G.queueMicrotask(() => { self.emit("close"); const sess = self.session; if (sess && sess._endPending) sess._onSocketEnd(); });
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
      // Header validation runs BEFORE the stream id is consumed: node throws
      // out of request() for a malformed header block and no stream is opened.
      const built = buildHeaderList(headers, this._scheme, this._authorityName, this._options.strictSingleValueFields);
      const streamId = this._nextStreamId();
      const stream = new ClientHttp2Stream(this, streamId, headers, options);
      this.streams.set(streamId, stream);
      const block = encodeHeaders(built.list, built.sensitive);
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
      // emit 'trailers' rather than a second 'response'. A 1xx block BEFORE the
      // response is informational: node emits 'headers', and the real response
      // still follows (lib/internal/http2/core.js onSessionHeaders).
      const st = headersObj[":status"];
      if (stream._responseEmitted) stream.emit("trailers", headersObj, flags);
      else if (typeof st === "number" && st >= 100 && st < 200) {
        stream.emit("headers", headersObj, flags);
        // node ClientHttp2Stream handleHeaderContinue: a 100 informational
        // response is additionally surfaced as 'continue'.
        if (st === 100) stream.emit("continue");
      }
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
      if (this.streams && this.streams.size > 0) { this._endPending = true; return; }
      this._endPending = false;
      this._teardown();
    }
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
    const sensitive = new Set();
    const sn = map[G.Symbol.for("nodejs.http2.sensitiveHeaders")];
    if (Array.isArray(sn)) for (const s of sn) sensitive.add(String(s).toLowerCase());
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
    for (const key of Object.keys(map)) {
      const value = map[key];
      if (value === undefined || key === "") continue;
      processHeader(key, value);
    }
    return { list: pseudoHeaders.concat(headers), sensitive };
  }
  // node prepareRequestHeadersObject: defaults :method/:authority/:scheme/:path
  // and enforces the CONNECT-specific pseudo-header rules.
  function buildHeaderList(headers, scheme, authorityName, strictSingleValueFields) {
    const obj = Object.assign({ __proto__: null }, headers);
    const sym = G.Symbol.for("nodejs.http2.sensitiveHeaders");
    if (headers && headers[sym] !== undefined) obj[sym] = headers[sym];
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
    return buildNgHeaders(obj, assertValidRequestPseudoHeader, strictSingleValueFields);
  }
  function headerListToObject(list, sensitive) {
    const obj = {};
    const raw = [];
    for (let i = 0; i < list.length; i++) {
      let name = list[i][0]; const value = list[i][1];
      raw.push(name, value);
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
  // node prepareResponseHeadersObject: a response may only carry the :status
  // pseudo-header, the status must be a valid (non-informational) code, and the
  // remaining fields go through the same buildNgHeaderString validation as a
  // request's.
  function buildResponseHeaderList(headers, strictSingleValueFields, informational) {
    if (headers !== undefined && headers !== null && (typeof headers !== "object" || Array.isArray(headers)))
      throw argTypeErr("headers", "an object", headers);
    const obj = Object.assign({ __proto__: null }, headers);
    const sym = G.Symbol.for("nodejs.http2.sensitiveHeaders");
    if (headers && headers[sym] !== undefined) obj[sym] = headers[sym];
    if (informational) {
      // additionalHeaders(): only a 1xx (never 101) informational status.
      if (obj[":status"] != null) {
        const sc = obj[":status"] | 0;
        if (sc === 101) { const e = new Error("HTTP status code 101 (Switching Protocols) is forbidden in HTTP/2"); e.code = "ERR_HTTP2_STATUS_101"; throw e; }
        if (sc < 100 || sc >= 200) { const e = new RangeError("Invalid informational status code: " + obj[":status"]); e.code = "ERR_HTTP2_INVALID_INFO_STATUS"; throw e; }
        obj[":status"] = String(sc);
      }
      return buildNgHeaders(obj, assertValidResponsePseudoHeader, strictSingleValueFields);
    }
    // node validatePreparedResponseHeaders is deliberately stricter than HTTP/1:
    // a response status outside 200..599 is rejected outright.
    const status = (obj[":status"] | 0) || 200;
    if (status < 200 || status > 599) { const e = new RangeError("Invalid status code: " + status); e.code = "ERR_HTTP2_STATUS_INVALID"; throw e; }
    obj[":status"] = String(status);
    return buildNgHeaders(obj, assertValidResponsePseudoHeader, strictSingleValueFields);
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
      // node ServerHttp2Stream#headRequest: a HEAD request must not carry a
      // payload, which respondWithFile/FD check before opening anything.
      this.headRequest = !!(headers && typeof headers[":method"] === "string" && headers[":method"].toUpperCase() === "HEAD");
    }
    get closed() { return this._closed; }
    get state() { return streamState(this); }
    setEncoding(enc) { this._enc = enc || "utf8"; this._decoder = undefined; return this; }
    resume() { this._paused = false; return this; }
    pause() { this._paused = true; return this; }
    respond(headers, options) {
      if (this.headersSent) throw mkErr("Response has already been initiated.", "ERR_HTTP2_HEADERS_SENT");
      if (this.destroyed || this._closed) throw mkErr("The stream has been destroyed", "ERR_HTTP2_INVALID_STREAM");
      headers = headers || {};
      options = options || {};
      // node ServerHttp2Stream.respond({ sendDate }) stamps a Date header unless
      // the response already carries one or sendDate was turned off.
      if (options.sendDate !== false && headers["date"] === undefined && headers["Date"] === undefined) {
        try { headers = Object.assign({ __proto__: null }, headers, { date: new Date().toUTCString() }); } catch (e) {}
      }
      const built = buildResponseHeaderList(headers, this.session._options && this.session._options.strictSingleValueFields);
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
        // node only allows sendTrailers() once the 'wantTrailers' event has
        // actually fired (kState.trailersReady); before that it is
        // ERR_HTTP2_TRAILERS_NOT_READY even though waitForTrailers was set.
        G.queueMicrotask(() => { this._trailersReady = true; this.emit("wantTrailers"); });
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
      if (!this._trailersReady) throw mkErr("Trailers are not ready to send", "ERR_HTTP2_TRAILERS_NOT_READY");
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
    pushStream(headers, options, cb) { if (typeof options === "function") { cb = options; } if (typeof cb === "function") G.queueMicrotask(() => cb(mkErr("Push streams are not supported", "ERR_HTTP2_PUSH_DISABLED"))); }
    // node ServerHttp2Stream.additionalHeaders: an informational (1xx) HEADERS
    // block sent BEFORE the response headers; it does not open the response, so
    // headersSent stays false.
    additionalHeaders(headers) {
      if (this.destroyed || this._closed) throw mkErr("The stream has been destroyed", "ERR_HTTP2_INVALID_STREAM");
      if (this.headersSent) throw mkErr("Cannot specify additional headers after response initiated", "ERR_HTTP2_HEADERS_AFTER_RESPOND");
      const built = buildResponseHeaderList(headers || {}, this.session._options && this.session._options.strictSingleValueFields, true);
      writeHeaderBlock(this.session, this.id, encodeHeaders(built.list, built.sensitive), 0);
    }
    cork() { return this; }
    uncork() { return this; }
    get writableEnded() { return this._endStreamSent === true || this.writable === false; }
    get writableFinished() { return this._closed === true; }
    get writableCorked() { return 0; }
    get writableHighWaterMark() { return 16384; }
    get writableObjectMode() { return false; }
    get writableLength() { return 0; }
    get writableNeedDrain() { return false; }
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
      if (this._enc) this.emit("data", decodeChunk(this, bytes));
      else this.emit("data", Buffer.from(bytes));
    }
    _onRequestEnd() { this.readable = false; this.emit("end"); }
    _finish() {
      if (this._closed) return;
      this._closed = true;
      this.readable = false; this.writable = false;
      this.session.streams.delete(this.id);
      const self = this;
      G.queueMicrotask(() => { self.emit("close"); if (self.session._endPending) self.session._onSocketEnd(); });
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
      socket.on("end", () => self._onSocketEnd());
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
        // node surfaces the request trailer block on the stream before 'end'
        // (compat.js onStreamTrailers feeds req.trailers / req.rawTrailers).
        const rawTrailers = [];
        for (let i = 0; i < list.length; i++) rawTrailers.push(list[i][0], list[i][1]);
        stream.emit("trailers", headerListToObject(list, this._hpack._sensitive), flags, rawTrailers);
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
      // node Http2Stream#endAfterHeaders: the request carried END_STREAM on its
      // HEADERS frame, i.e. there is no request body to wait for.
      stream.endAfterHeaders = !!pb.endStream;
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
    // The peer half-closed: RFC 9113 sec. 5.4.1 — no further frames can arrive, so
    // the session is over once its streams have finished. node reaches the same
    // point through kMaybeDestroy (the last stream destroying a closed session
    // destroys the session, and with it the socket). This was a no-op, and the
    // http2 socket is allowHalfOpen, so the socket stayed open forever; once
    // handles genuinely held the event loop that became a hang.
    _onSocketEnd() {
      if (this.destroyed) return;
      if (this.streams && this.streams.size > 0) { this._endPending = true; return; }
      this._endPending = false;
      this._teardown();
    }
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
  function validateLinkHeaderValue(hints) {
    if (typeof hints === "string") {
      if (!LINK_VALUE_RE.test(hints)) throw invalidArgValue("hints.link", hints, "must be an array or string of format \"</styles.css>; rel=preload; as=style\"");
      return hints;
    } else if (Array.isArray(hints)) {
      if (hints.length === 0) return "";
      let result = "";
      for (let i = 0; i < hints.length; i++) {
        const link = hints[i];
        if (typeof link !== "string" || !LINK_VALUE_RE.test(link))
          throw invalidArgValue("hints.link", link, "must be an array or string of format \"</styles.css>; rel=preload; as=style\"");
        result += link;
        if (i !== hints.length - 1) result += ", ";
      }
      return result;
    }
    return "";
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
    get writableCorked() { return 0; }
    get writableHighWaterMark() { return 16384; }
    get writableObjectMode() { return false; }
    get writableFinished() { return this._state.closed; }
    get writableLength() { return 0; }
    get writableNeedDrain() { return false; }
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
    cork() { return this; }
    uncork() { return this; }
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
    const request = new Http2ServerRequest(stream, headers, undefined, rawHeaders);
    const response = new Http2ServerResponse(stream);
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

  // === install over the deferred stub (bootstrap.cppm def(["http2"], ...)) ===
  http2.constants = constants;
  http2.connect = connect;
  http2.getDefaultSettings = getDefaultSettings;
  http2.getPackedSettings = getPackedSettings;
  http2.getUnpackedSettings = getUnpackedSettings;
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
