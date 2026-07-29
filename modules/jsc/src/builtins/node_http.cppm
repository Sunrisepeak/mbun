// node:http + node:https JS layer partition.
//
// A translation of node's own http stack rather than an approximation of it:
//   * OutgoingMessage  <- lib/_http_outgoing.js  (Stream subclass, outputData
//     buffering, _storeHeader/_send/_writeRaw/_flush, trailers, cork/uncork)
//   * ServerResponse   <- lib/_http_server.js    (writeHead/writeInformation/
//     assignSocket/detachSocket over OutgoingMessage)
//   * IncomingMessage  <- lib/_http_incoming.js  (matchKnownFields header
//     folding, headersDistinct/trailersDistinct, _dump, signal)
//   * Agent            <- lib/_http_agent.js     (sockets/freeSockets/requests
//     pools, addRequest/createSocket/removeSocket/keepSocketAlive/reuseSocket)
//   * ClientRequest    <- lib/_http_client.js    (option validation, onSocket,
//     parser attach, 1xx information events, keep-alive socket release)
// plus METHODS / STATUS_CODES / validateHeaderName / validateHeaderValue and
// the node:https mirror (Agent defaultPort 443, its own globalAgent).
//
// The response parser is mbun's incremental HTTP/1.1 parser (js_net.cppm,
// surfaced as globalThis.__mbunHttpParser) standing in for llhttp; everything
// above it is node's control flow.
//
// SERVER TRANSPORT LIVES IN js_net.cppm: this partition runs before kNetJS, so
// `class Server extends net.Server` is impossible here. js_net owns
// http.Server/createServer and drives ServerResponse through assignSocket();
// the classes themselves (which need only stream + events) are defined here so
// there is exactly one ServerResponse in the process. Internals shared across
// that split are published on globalThis.__mbunHttpInternals.
//
// NOTE: appended AFTER the master builtins IIFE (opened in bootstrap, closed by
// image_closure), so this is a self-contained IIFE that re-binds G = globalThis
// and overwrites the pure-JS http/https stubs registered in bootstrap. It must
// not rely on the outer IIFE's aliases.
export module mbun.jsc.js_builtins:node_http;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeHttpJS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  if (!M) return;
  const req = (n) => M[n] || M["node:" + n] || {};
  const streamMod = req("stream");
  const eventsMod = M["events"] || M["node:events"];
  const EventEmitter = (eventsMod && eventsMod.EventEmitter) || eventsMod;
  const Stream = streamMod.Stream;
  const Readable = streamMod.Readable;
  if (!Stream || !Readable || !EventEmitter) return;
  const getDefaultHighWaterMark = typeof streamMod.getDefaultHighWaterMark === "function"
    ? streamMod.getDefaultHighWaterMark : () => 16384;
  const nextTick = (fn, ...a) => (G.process && G.process.nextTick)
    ? G.process.nextTick(fn, ...a) : G.queueMicrotask(() => fn(...a));

  // ---- built-in http diagnostics channels ----
  // node lib/_http_client.js / lib/_http_server.js resolve these once at module
  // load, but this partition is assembled BEFORE node:diagnostics_channel is
  // registered, so the lookup is memoised on first use instead. Everything that
  // reads it runs per-request, long after the whole image has loaded.
  const kNoDC = { hasSubscribers: false, publish() {} };
  let httpDCCache = null;
  const httpDC = () => {
    if (httpDCCache !== null) return httpDCCache;
    const d = M["diagnostics_channel"] || M["node:diagnostics_channel"];
    if (!d || typeof d.channel !== "function") {
      return { clientRequestCreated: kNoDC, clientRequestStart: kNoDC, clientRequestError: kNoDC,
               clientResponseFinish: kNoDC, serverResponseCreated: kNoDC };
    }
    httpDCCache = {
      clientRequestCreated: d.channel("http.client.request.created"),
      clientRequestStart: d.channel("http.client.request.start"),
      clientRequestError: d.channel("http.client.request.error"),
      clientResponseFinish: d.channel("http.client.response.finish"),
      serverResponseCreated: d.channel("http.server.response.created"),
    };
    return httpDCCache;
  };

  // -------------------------------------------------- node error factories
  // internal/errors.js message templates, reproduced verbatim so assert.throws
  // shapes ({ code, name, message }) match upstream.
  const mkErr = (Ctor, code, msg) => { const e = new Ctor(msg); e.code = code; return e; };
  const invalidArgTypeHelper = (input) => {
    if (input === undefined || input === null) return " Received " + String(input);
    if (typeof input === "function") return " Received function " + input.name;
    if (typeof input === "object") {
      if (input.constructor && input.constructor.name) return " Received an instance of " + input.constructor.name;
      return " Received " + String(input);
    }
    let inspected = typeof input === "string" ? "'" + input + "'" : String(input);
    if (inspected.length > 28) inspected = inspected.slice(0, 25) + "...";
    return " Received type " + (typeof input) + " (" + inspected + ")";
  };
  const PRIMITIVE_TYPES = ["string", "number", "bigint", "boolean", "symbol", "function", "object"];
  const orList = (a) => a.length <= 1 ? (a[0] || "")
    : a.length === 2 ? a[0] + " or " + a[1]
    : a.slice(0, -1).join(", ") + " or " + a[a.length - 1];
  function ERR_INVALID_ARG_TYPE(name, expected, actual, kind) {
    // Shared node-exact factory (bootstrap __mbunNodeErrors) when available: it
    // derives argument/property from the name, applies node's Oxford-comma
    // formatList, and classifies free-form alternatives like "Agent-like Object"
    // as `one of …` rather than `an instance of …` (classRegExp rejects any name
    // with a space or hyphen — /^[A-Z]/ here did not). `kind` becomes redundant.
    const NE = G.__mbunNodeErrors;
    if (NE) return NE.ERR_INVALID_ARG_TYPE(name, expected, actual);
    if (!Array.isArray(expected)) expected = [expected];
    const types = [], instances = [], other = [];
    for (const v of expected) {
      if (PRIMITIVE_TYPES.indexOf(v) !== -1) types.push(v);
      else if (/^[A-Z]/.test(v)) instances.push(v);
      else other.push(v);
    }
    const parts = [];
    if (types.length) parts.push("of type " + orList(types));
    if (instances.length) parts.push("an instance of " + orList(instances));
    if (other.length) parts.push("one of " + orList(other));
    return mkErr(TypeError, "ERR_INVALID_ARG_TYPE",
      'The "' + name + '" ' + (kind || "argument") + " must be " + parts.join(" or ") + "." +
      invalidArgTypeHelper(actual));
  }
  const inspectVal = (v) => typeof v === "string" ? "'" + v + "'"
    : Array.isArray(v) ? "[ " + v.map(inspectVal).join(", ") + " ]"
    : String(v);
  // util.format('%s') on a non-primitive inspects it; on a primitive it is String().
  const fmtPct = (v) => {
    if (v === null || typeof v !== "object") return String(v);
    const u = M["util"] || M["node:util"];
    try { return u && typeof u.inspect === "function" ? u.inspect(v, { depth: 0 }) : String(v); }
    catch (e) { return String(v); }
  };
  const ERR_INVALID_ARG_VALUE = (name, value, reason) => mkErr(TypeError, "ERR_INVALID_ARG_VALUE",
    "The " + (name.indexOf(".") !== -1 ? "property '" + name + "'" : "argument '" + name + "'") +
    " " + (reason || "is invalid") + ". Received " + inspectVal(value));
  const ERR_OUT_OF_RANGE = (name, range, value) => mkErr(RangeError, "ERR_OUT_OF_RANGE",
    'The value of "' + name + '" is out of range. It must be ' + range + ". Received " + inspectVal(value));
  const ERR_INVALID_HTTP_TOKEN = (label, value) => mkErr(TypeError, "ERR_INVALID_HTTP_TOKEN",
    (label || "Header name") + ' must be a valid HTTP token ["' + value + '"]');
  const ERR_HTTP_INVALID_HEADER_VALUE = (value, name) => mkErr(TypeError, "ERR_HTTP_INVALID_HEADER_VALUE",
    'Invalid value "' + value + '" for header "' + name + '"');
  const ERR_INVALID_CHAR = (field, name) => mkErr(TypeError, "ERR_INVALID_CHAR",
    name === undefined ? "Invalid character in " + field
                       : "Invalid character in " + field + ' ["' + name + '"]');
  const ERR_HTTP_HEADERS_SENT = (what) => mkErr(Error, "ERR_HTTP_HEADERS_SENT",
    "Cannot " + what + " headers after they are sent to the client");
  // node builds this through util.format('%s'), which inspects a non-primitive
  // ("{}") instead of String()-ing it ("[object Object]").
  const ERR_HTTP_INVALID_STATUS_CODE = (v) => mkErr(RangeError, "ERR_HTTP_INVALID_STATUS_CODE",
    "Invalid status code: " + fmtPct(v));
  const ERR_HTTP_TRAILER_INVALID = () => mkErr(Error, "ERR_HTTP_TRAILER_INVALID",
    "Trailers are invalid with this transfer encoding");
  const ERR_HTTP_SOCKET_ASSIGNED = () => mkErr(Error, "ERR_HTTP_SOCKET_ASSIGNED",
    "ServerResponse has an already assigned socket");
  const ERR_HTTP_BODY_NOT_ALLOWED = () => mkErr(Error, "ERR_HTTP_BODY_NOT_ALLOWED",
    "Adding content for this request method or response status is not allowed.");
  const ERR_HTTP_CONTENT_LENGTH_MISMATCH = (actual, expected) => mkErr(Error, "ERR_HTTP_CONTENT_LENGTH_MISMATCH",
    "Response body's content-length of " + actual + " byte(s) does not match the content-length of " +
    expected + " byte(s) set in header");
  const ERR_METHOD_NOT_IMPLEMENTED = (m) => mkErr(Error, "ERR_METHOD_NOT_IMPLEMENTED",
    "The " + m + " method is not implemented");
  const ERR_STREAM_WRITE_AFTER_END = () => mkErr(Error, "ERR_STREAM_WRITE_AFTER_END", "write after end");
  const ERR_STREAM_ALREADY_FINISHED = (m) => mkErr(Error, "ERR_STREAM_ALREADY_FINISHED",
    "Cannot call " + m + " after a stream was finished");
  const ERR_STREAM_DESTROYED = (m) => mkErr(Error, "ERR_STREAM_DESTROYED",
    "Cannot call " + m + " after a stream was destroyed");
  const ERR_STREAM_NULL_VALUES = () => mkErr(TypeError, "ERR_STREAM_NULL_VALUES", "May not write null values to stream");
  const ERR_STREAM_CANNOT_PIPE = () => mkErr(Error, "ERR_STREAM_CANNOT_PIPE", "Cannot pipe, not readable");
  const ERR_UNESCAPED_CHARACTERS = (what) => mkErr(TypeError, "ERR_UNESCAPED_CHARACTERS",
    what + " contains unescaped characters");
  const ERR_INVALID_PROTOCOL = (actual, expected) => mkErr(TypeError, "ERR_INVALID_PROTOCOL",
    'Protocol "' + actual + '" not supported. Expected "' + expected + '"');
  // internal/errors.js ConnResetException: an ECONNRESET-coded Error.
  const ConnResetException = (msg) => { const e = new Error(msg); e.code = "ECONNRESET"; return e; };

  // internal/validators.js
  const validateString = (v, n) => { if (typeof v !== "string") throw ERR_INVALID_ARG_TYPE(n, "string", v); };
  const validateNumber = (v, n, min, max) => {
    if (typeof v !== "number") throw ERR_INVALID_ARG_TYPE(n, "number", v);
    if ((min !== undefined && v < min) || (max !== undefined && v > max)) {
      throw ERR_OUT_OF_RANGE(n, (min !== undefined ? ">= " + min : "") +
        (min !== undefined && max !== undefined ? " && " : "") + (max !== undefined ? "<= " + max : ""), v);
    }
  };
  const validateBoolean = (v, n) => { if (typeof v !== "boolean") throw ERR_INVALID_ARG_TYPE(n, "boolean", v); };
  const validateInteger = (v, n, min, max) => {
    if (typeof v !== "number") throw ERR_INVALID_ARG_TYPE(n, "number", v);
    if (!Number.isInteger(v)) throw ERR_OUT_OF_RANGE(n, "an integer", v);
    if ((min !== undefined && v < min) || (max !== undefined && v > max)) {
      throw ERR_OUT_OF_RANGE(n, ">= " + min + " && <= " + max, v);
    }
  };
  const validateObject = (v, n) => {
    if (v === null || Array.isArray(v) || typeof v !== "object") throw ERR_INVALID_ARG_TYPE(n, "Object", v);
  };
  const validateOneOf = (v, n, list) => {
    if (list.indexOf(v) === -1) {
      throw ERR_INVALID_ARG_VALUE(n, v, "must be one of: " +
        list.map((x) => typeof x === "string" ? "'" + x + "'" : String(x)).join(", "));
    }
  };
  // internal/timers.js getTimerDuration
  const getTimerDuration = (msecs, name) => {
    validateNumber(msecs, name);
    if (msecs < 0 || !Number.isFinite(msecs)) throw ERR_OUT_OF_RANGE(name, "a non-negative finite number", msecs);
    if (msecs > 2147483647) return 2147483647;
    return msecs;
  };

  // -------------------------------------------------- constants (internal/http)
  const METHODS = Object.freeze([
    "ACL", "BIND", "CHECKOUT", "CONNECT", "COPY", "DELETE", "GET", "HEAD",
    "LINK", "LOCK", "M-SEARCH", "MERGE", "MKACTIVITY", "MKCALENDAR", "MKCOL",
    "MOVE", "NOTIFY", "OPTIONS", "PATCH", "POST", "PROPFIND", "PROPPATCH",
    "PURGE", "PUT", "QUERY", "REBIND", "REPORT", "SEARCH", "SOURCE",
    "SUBSCRIBE", "TRACE", "UNBIND", "UNLINK", "UNLOCK", "UNSUBSCRIBE",
  ]);
  const STATUS_CODES = {
    100: "Continue", 101: "Switching Protocols", 102: "Processing",
    103: "Early Hints", 200: "OK", 201: "Created", 202: "Accepted",
    203: "Non-Authoritative Information", 204: "No Content", 205: "Reset Content",
    206: "Partial Content", 207: "Multi-Status", 208: "Already Reported",
    226: "IM Used", 300: "Multiple Choices", 301: "Moved Permanently",
    302: "Found", 303: "See Other", 304: "Not Modified", 305: "Use Proxy",
    307: "Temporary Redirect", 308: "Permanent Redirect", 400: "Bad Request",
    401: "Unauthorized", 402: "Payment Required", 403: "Forbidden",
    404: "Not Found", 405: "Method Not Allowed", 406: "Not Acceptable",
    407: "Proxy Authentication Required", 408: "Request Timeout", 409: "Conflict",
    410: "Gone", 411: "Length Required", 412: "Precondition Failed",
    413: "Payload Too Large", 414: "URI Too Long", 415: "Unsupported Media Type",
    416: "Range Not Satisfiable", 417: "Expectation Failed", 418: "I'm a Teapot",
    421: "Misdirected Request", 422: "Unprocessable Entity", 423: "Locked",
    424: "Failed Dependency", 425: "Too Early", 426: "Upgrade Required",
    428: "Precondition Required", 429: "Too Many Requests",
    431: "Request Header Fields Too Large", 451: "Unavailable For Legal Reasons",
    500: "Internal Server Error", 501: "Not Implemented", 502: "Bad Gateway",
    503: "Service Unavailable", 504: "Gateway Timeout",
    505: "HTTP Version Not Supported", 506: "Variant Also Negotiates",
    507: "Insufficient Storage", 508: "Loop Detected",
    509: "Bandwidth Limit Exceeded", 510: "Not Extended",
    511: "Network Authentication Required",
  };

  // -------------------------------------------------- validators (_http_common)
  // token: RFC 7230 field-name; header content: field-vchar / obs-fold.
  const tokenRegExp = /^[\^_`a-zA-Z\-0-9!#$%&'*+.|~]+$/;
  const headerCharRegex = /[^\t\x20-\x7e\x80-\xff]/;
  const chunkExpression = /(?:^|\W)chunked(?:$|\W)/i;
  const checkIsHttpToken = (val) => tokenRegExp.exec(val) !== null;
  // node lib/_http_common.js keeps two field-value alphabets: the RFC 7230
  // strict one (default) and a Fetch-spec lenient one used only when the caller
  // opted into --insecure-http-parser. `lenient` defaults to false, so every
  // existing call site keeps the strict alphabet — this widens nothing.
  const lenientHeaderCharRegex = /[\x00\x0a\x0d]|[^\x00-\xff]/;
  const checkInvalidHeaderChar = (val, lenient) =>
    (lenient ? lenientHeaderCharRegex : headerCharRegex).exec(String(val)) !== null;
  const validateHeaderName = (name, label) => {
    if (typeof name !== "string" || !name || !checkIsHttpToken(name)) {
      throw ERR_INVALID_HTTP_TOKEN(label || "Header name", name);
    }
  };
  const validateHeaderValue = (name, value, lenient) => {
    if (value === undefined) throw ERR_HTTP_INVALID_HEADER_VALUE(value, name);
    if (checkInvalidHeaderChar(value, lenient)) throw ERR_INVALID_CHAR("header content", name);
  };

  // -------------------------------------------------- symbols (internal/http)
  const kOutHeaders = Symbol("kOutHeaders");
  const kNeedDrain = Symbol("kNeedDrain");
  const kSocket = Symbol("kSocket");
  const kCorked = Symbol("kCorked");
  const kChunkedBuffer = Symbol("kChunkedBuffer");
  const kChunkedLength = Symbol("kChunkedLength");
  const kUniqueHeaders = Symbol("kUniqueHeaders");
  const kBytesWritten = Symbol("kBytesWritten");
  const kErrored = Symbol("errored");
  const kHighWaterMark = Symbol("kHighWaterMark");
  const kRejectNonStandardBodyWrites = Symbol("kRejectNonStandardBodyWrites");
  // node's `httpValidation` option, resolved to the one bit the outgoing
  // header validators need: whether this message uses llhttp's relaxed
  // field-value alphabet (control chars allowed, NUL/CR/LF never) instead of
  // RFC 7230's strict one. Carried per MESSAGE, because a server and a client
  // in the same process can disagree.
  const kLenientHeaders = Symbol("kLenientHeaders");
  const kPath = Symbol("kPath");
  const kHeaders = Symbol("kHeaders");
  const kHeadersDistinct = Symbol("kHeadersDistinct");
  const kHeadersCount = Symbol("kHeadersCount");
  const kTrailers = Symbol("kTrailers");
  const kTrailersDistinct = Symbol("kTrailersDistinct");
  const kTrailersCount = Symbol("kTrailersCount");
  const kAbortController = Symbol("kAbortController");
  const kRequestOptions = Symbol("requestOptions");
  const kError = Symbol("kError");
  const nop = () => {};
  const utcDate = () => new Date().toUTCString();
  const isUint8Array = (v) => v instanceof Uint8Array;

  // ================================================== OutgoingMessage
  // lib/_http_outgoing.js. Deliberately a Stream (not a Writable) subclass:
  // node implements write()/end() itself over outputData, and a Writable base
  // would demand a _write() the class does not have -- a bare
  // `new OutgoingMessage()` must be usable (test-http-outgoing-buffer).
  function OutgoingMessage(options) {
    Stream.call(this);
    this.outputData = [];
    this.outputSize = 0;
    this.writable = true;
    this.destroyed = false;
    this._last = false;
    this.chunkedEncoding = false;
    this.shouldKeepAlive = true;
    this.maxRequestsOnConnectionReached = false;
    this._defaultKeepAlive = true;
    this.useChunkedEncodingByDefault = true;
    this.sendDate = false;
    this._removedConnection = false;
    this._removedContLen = false;
    this._removedTE = false;
    this.strictContentLength = false;
    this[kBytesWritten] = 0;
    this._contentLength = null;
    this._hasBody = true;
    this._trailer = "";
    this[kNeedDrain] = false;
    this[kLenientHeaders] = false;
    this.finished = false;
    this._headerSent = false;
    this[kCorked] = 0;
    this[kChunkedBuffer] = [];
    this[kChunkedLength] = 0;
    this._closed = false;
    this[kSocket] = null;
    this._header = null;
    this[kOutHeaders] = null;
    this._keepAliveTimeout = 0;
    this._onPendingData = nop;
    this[kErrored] = null;
    this[kHighWaterMark] = (options && options.highWaterMark) != null
      ? options.highWaterMark : getDefaultHighWaterMark();
    this[kRejectNonStandardBodyWrites] = (options && options.rejectNonStandardBodyWrites) || false;
  }
  Object.setPrototypeOf(OutgoingMessage.prototype, Stream.prototype);
  Object.setPrototypeOf(OutgoingMessage, Stream);

  const defGet = (obj, name, get, set) => Object.defineProperty(obj, name,
    set ? { get, set, configurable: true } : { get, configurable: true });

  defGet(OutgoingMessage.prototype, "errored", function () { return this[kErrored]; });
  defGet(OutgoingMessage.prototype, "closed", function () { return this._closed; });
  defGet(OutgoingMessage.prototype, "writableFinished", function () {
    return this.finished && this.outputSize === 0 &&
      (!this[kSocket] || this[kSocket].writableLength === 0);
  });
  defGet(OutgoingMessage.prototype, "writableObjectMode", function () { return false; });
  defGet(OutgoingMessage.prototype, "writableLength", function () {
    return this.outputSize + this[kChunkedLength] + (this[kSocket] ? (this[kSocket].writableLength | 0) : 0);
  });
  defGet(OutgoingMessage.prototype, "writableHighWaterMark", function () {
    return this[kSocket] ? this[kSocket].writableHighWaterMark : this[kHighWaterMark];
  });
  defGet(OutgoingMessage.prototype, "writableCorked", function () { return this[kCorked]; });
  defGet(OutgoingMessage.prototype, "connection",
    function () { return this[kSocket]; },
    function (val) { this.socket = val; });
  defGet(OutgoingMessage.prototype, "socket",
    function () { return this[kSocket]; },
    function (val) {
      for (let n = 0; n < this[kCorked]; n++) {
        if (val) val.cork();
        if (this[kSocket]) this[kSocket].uncork();
      }
      this[kSocket] = val;
    });
  Object.defineProperty(OutgoingMessage.prototype, "headersSent", {
    configurable: true, enumerable: true, get() { return !!this._header; },
  });
  defGet(OutgoingMessage.prototype, "writableEnded", function () { return this.finished; });
  defGet(OutgoingMessage.prototype, "writableNeedDrain", function () {
    return !this.destroyed && !this.finished && this[kNeedDrain];
  });

  // Lenient header validation is a --insecure-http-parser/httpValidation opt-in;
  // this port is always strict, so the hook exists but never relaxes.
  OutgoingMessage.prototype._isLenientHeaderValidation = function () { return false; };

  OutgoingMessage.prototype._renderHeaders = function _renderHeaders() {
    if (this._header) throw ERR_HTTP_HEADERS_SENT("render");
    const headersMap = this[kOutHeaders];
    const headers = {};
    if (headersMap !== null && headersMap !== undefined) {
      for (const key of Object.keys(headersMap)) headers[headersMap[key][0]] = headersMap[key][1];
    }
    return headers;
  };

  const crlf_buf = () => G.Buffer.from("\r\n");
  OutgoingMessage.prototype.cork = function cork() {
    this[kCorked]++;
    if (this[kSocket]) this[kSocket].cork();
  };
  OutgoingMessage.prototype.uncork = function uncork() {
    this[kCorked]--;
    if (this[kSocket]) this[kSocket].uncork();
    if (this[kCorked] || this[kChunkedBuffer].length === 0) return;
    const len = this[kChunkedLength];
    const buf = this[kChunkedBuffer];
    let callbacks = null;
    this._send(len.toString(16), "latin1", null);
    this._send(crlf_buf(), null, null);
    for (let n = 0; n < buf.length; n += 3) {
      this._send(buf[n + 0], buf[n + 1], null);
      if (buf[n + 2]) { if (!callbacks) callbacks = []; callbacks.push(buf[n + 2]); }
    }
    this._send(crlf_buf(), null, callbacks ? (err) => { for (const cb of callbacks) cb(err); } : null);
    this[kChunkedBuffer].length = 0;
    this[kChunkedLength] = 0;
  };

  OutgoingMessage.prototype.setTimeout = function setTimeout(msecs, callback) {
    if (callback) this.on("timeout", callback);
    if (!this[kSocket]) {
      this.once("socket", function socketSetTimeoutOnConnect(socket) { socket.setTimeout(msecs); });
    } else {
      this[kSocket].setTimeout(msecs);
    }
    return this;
  };

  OutgoingMessage.prototype.destroy = function destroy(error) {
    if (this.destroyed) return this;
    this.destroyed = true;
    this[kErrored] = error;
    if (this[kSocket]) this[kSocket].destroy(error);
    else nextTick(emitDestroyNT, this);
    return this;
  };
  function emitDestroyNT(self) {
    if (!self._closed) { self._closed = true; self.emit("close"); }
  }

  // Header block and first body chunk share a packet where possible.
  OutgoingMessage.prototype._send = function _send(data, encoding, callback, byteLength) {
    if (!this._headerSent && this._header !== null) {
      if (typeof data === "string" && (encoding === "utf8" || encoding === "latin1" || !encoding)) {
        data = this._header + data;
      } else {
        const header = this._header;
        this.outputData.unshift({ data: header, encoding: "latin1", callback: null });
        this.outputSize += header.length;
        this._onPendingData(header.length);
      }
      this._headerSent = true;
    }
    return this._writeRaw(data, encoding, callback, byteLength);
  };

  OutgoingMessage.prototype._writeRaw = function _writeRaw(data, encoding, callback, size) {
    const conn = this[kSocket];
    if (conn && conn.destroyed) return false;
    if (typeof encoding === "function") { callback = encoding; encoding = null; }
    if (conn && conn._httpMessage === this && conn.writable) {
      if (this.outputData.length) this._flushOutput(conn);
      return conn.write(data, encoding, callback);
    }
    this.outputData.push({ data, encoding, callback });
    this.outputSize += data.length;
    this._onPendingData(data.length);
    return this.outputSize < this[kHighWaterMark];
  };

  const RE_CONN_CLOSE = /(?:^|\W)close(?:$|\W)/i;
  const isCookieField = (s) => s.length === 6 && s.toLowerCase() === "cookie";
  const isContentDispositionField = (s) => s.length === 19 && s.toLowerCase() === "content-disposition";

  function matchHeader(self, state, field, value) {
    if (field.length < 4 || field.length > 17) return;
    field = field.toLowerCase();
    switch (field) {
      case "connection":
        state.connection = true;
        self._removedConnection = false;
        if (RE_CONN_CLOSE.test(value)) self._last = true;
        else self.shouldKeepAlive = true;
        break;
      case "transfer-encoding":
        state.te = true;
        self._removedTE = false;
        if (chunkExpression.test(value)) self.chunkedEncoding = true;
        break;
      case "content-length":
        state.contLen = true;
        self._contentLength = +value;
        self._removedContLen = false;
        break;
      case "date": case "expect": case "trailer": state[field] = true; break;
      case "keep-alive": self._defaultKeepAlive = false; break;
    }
  }
  function storeHeaderLine(self, state, key, value, validate) {
    if (validate) validateHeaderValue(key, value, self[kLenientHeaders]);
    state.header += key + ": " + value + "\r\n";
    matchHeader(self, state, key, value);
  }
  function processHeader(self, state, key, value, validate) {
    if (validate) validateHeaderName(key);
    if (isContentDispositionField(key) && self._contentLength) {
      if (Array.isArray(value)) {
        for (let i = 0; i < value.length; i++) value[i] = G.Buffer.from(value[i], "latin1");
      } else {
        value = G.Buffer.from(value, "latin1");
      }
    }
    if (Array.isArray(value)) {
      if ((value.length < 2 || !isCookieField(key)) &&
          (!self[kUniqueHeaders] || !self[kUniqueHeaders].has(key.toLowerCase()))) {
        for (let i = 0; i < value.length; i++) storeHeaderLine(self, state, key, value[i], validate);
        return;
      }
      value = value.join("; ");
    }
    storeHeaderLine(self, state, key, value, validate);
  }

  OutgoingMessage.prototype._storeHeader = function _storeHeader(firstLine, headers) {
    const state = { connection: false, contLen: false, te: false, date: false,
                    expect: false, trailer: false, header: firstLine };
    if (headers) {
      if (headers === this[kOutHeaders]) {
        for (const key in headers) {
          const entry = headers[key];
          processHeader(this, state, entry[0], entry[1], false);
        }
      } else if (Array.isArray(headers)) {
        if (headers.length && Array.isArray(headers[0])) {
          for (let i = 0; i < headers.length; i++) {
            processHeader(this, state, headers[i][0], headers[i][1], true);
          }
        } else {
          if (headers.length % 2 !== 0) throw ERR_INVALID_ARG_VALUE("headers", headers);
          for (let n = 0; n < headers.length; n += 2) {
            processHeader(this, state, headers[n], headers[n + 1], true);
          }
        }
      } else {
        for (const key of Object.keys(headers)) processHeader(this, state, key, headers[key], true);
      }
    }
    let header = state.header;
    if (this.sendDate && !state.date) header += "Date: " + utcDate() + "\r\n";

    // 204/304 must not carry a body: suppress the zero chunk and close instead.
    if (this.chunkedEncoding && (this.statusCode === 204 || this.statusCode === 304)) {
      this.chunkedEncoding = false;
      this.shouldKeepAlive = false;
    }

    if (this._removedConnection) {
      this._last = !this.shouldKeepAlive;
    } else if (!state.connection) {
      const shouldSendKeepAlive = this.shouldKeepAlive &&
        (state.contLen || this.useChunkedEncodingByDefault || this.agent);
      if (shouldSendKeepAlive && this.maxRequestsOnConnectionReached) {
        header += "Connection: close\r\n";
      } else if (shouldSendKeepAlive) {
        header += "Connection: keep-alive\r\n";
        if (this._keepAliveTimeout && this._defaultKeepAlive) {
          const timeoutSeconds = Math.floor(this._keepAliveTimeout / 1000);
          let max = "";
          if (~~this._maxRequestsPerSocket > 0) max = ", max=" + this._maxRequestsPerSocket;
          header += "Keep-Alive: timeout=" + timeoutSeconds + max + "\r\n";
        }
      } else {
        this._last = true;
        header += "Connection: close\r\n";
      }
    }

    if (!state.contLen && !state.te) {
      if (!this._hasBody) {
        this.chunkedEncoding = false;
      } else if (!this.useChunkedEncodingByDefault) {
        this._last = true;
      } else if (!state.trailer && !this._removedContLen && typeof this._contentLength === "number") {
        header += "Content-Length: " + this._contentLength + "\r\n";
      } else if (!this._removedTE) {
        header += "Transfer-Encoding: chunked\r\n";
        this.chunkedEncoding = true;
      } else {
        this._last = true;
      }
    }

    if (this.chunkedEncoding !== true && state.trailer) throw ERR_HTTP_TRAILER_INVALID();

    this._header = header + "\r\n";
    this._headerSent = false;
    if (state.expect) this._send("");
  };

  function parseUniqueHeadersOption(headers) {
    if (!Array.isArray(headers)) return null;
    const unique = new Set();
    for (const h of headers) unique.add(String(h).toLowerCase());
    return unique;
  }

  OutgoingMessage.prototype.setHeader = function setHeader(name, value) {
    if (this._header) throw ERR_HTTP_HEADERS_SENT("set");
    validateHeaderName(name);
    if (value === undefined) throw ERR_HTTP_INVALID_HEADER_VALUE(value, name);
    if (checkInvalidHeaderChar(value, this[kLenientHeaders])) throw ERR_INVALID_CHAR("header content", name);
    let headers = this[kOutHeaders];
    if (headers === null || headers === undefined) this[kOutHeaders] = headers = { __proto__: null };
    headers[name.toLowerCase()] = [name, value];
    return this;
  };

  OutgoingMessage.prototype.setHeaders = function setHeaders(headers) {
    if (this._header) throw ERR_HTTP_HEADERS_SENT("set");
    if (!headers || Array.isArray(headers) || typeof headers.keys !== "function" ||
        typeof headers.get !== "function") {
      throw ERR_INVALID_ARG_TYPE("headers", ["Headers", "Map"], headers);
    }
    let cookies = null;
    for (const entry of headers) {
      const key = entry[0], value = entry[1];
      if (key === "set-cookie") {
        if (!cookies) cookies = [];
        if (Array.isArray(value)) cookies.push(...value); else cookies.push(value);
        continue;
      }
      this.setHeader(key, value);
    }
    if (cookies !== null) this.setHeader("set-cookie", cookies);
    return this;
  };

  OutgoingMessage.prototype.appendHeader = function appendHeader(name, value) {
    if (this._header) throw ERR_HTTP_HEADERS_SENT("append");
    validateHeaderName(name);
    if (value === undefined) throw ERR_HTTP_INVALID_HEADER_VALUE(value, name);
    if (checkInvalidHeaderChar(value, this[kLenientHeaders])) throw ERR_INVALID_CHAR("header content", name);
    const field = name.toLowerCase();
    const headers = this[kOutHeaders];
    if (headers === null || headers === undefined || !headers[field]) return this.setHeader(name, value);
    if (!Array.isArray(headers[field][1])) headers[field][1] = [headers[field][1]];
    const existingValues = headers[field][1];
    if (Array.isArray(value)) { for (const v of value) existingValues.push(v); }
    else existingValues.push(value);
    return this;
  };

  OutgoingMessage.prototype.getHeader = function getHeader(name) {
    validateString(name, "name");
    const headers = this[kOutHeaders];
    if (headers === null || headers === undefined) return undefined;
    const entry = headers[name.toLowerCase()];
    return entry === undefined ? undefined : entry[1];
  };
  OutgoingMessage.prototype.getHeaderNames = function getHeaderNames() {
    return (this[kOutHeaders] !== null && this[kOutHeaders] !== undefined) ? Object.keys(this[kOutHeaders]) : [];
  };
  OutgoingMessage.prototype.getRawHeaderNames = function getRawHeaderNames() {
    const headersMap = this[kOutHeaders];
    if (headersMap === null || headersMap === undefined) return [];
    return Object.values(headersMap).map((v) => v[0]);
  };
  OutgoingMessage.prototype.getHeaders = function getHeaders() {
    const headers = this[kOutHeaders];
    const ret = { __proto__: null };
    if (headers) { for (const key of Object.keys(headers)) ret[key] = headers[key][1]; }
    return ret;
  };
  OutgoingMessage.prototype.hasHeader = function hasHeader(name) {
    validateString(name, "name");
    return (this[kOutHeaders] !== null && this[kOutHeaders] !== undefined) &&
      !!this[kOutHeaders][name.toLowerCase()];
  };
  OutgoingMessage.prototype.removeHeader = function removeHeader(name) {
    validateString(name, "name");
    if (this._header) throw ERR_HTTP_HEADERS_SENT("remove");
    const key = name.toLowerCase();
    switch (key) {
      case "connection": this._removedConnection = true; break;
      case "content-length": this._removedContLen = true; break;
      case "transfer-encoding": this._removedTE = true; break;
      case "date": this.sendDate = false; break;
    }
    if (this[kOutHeaders] !== null && this[kOutHeaders] !== undefined) delete this[kOutHeaders][key];
  };

  OutgoingMessage.prototype._implicitHeader = function _implicitHeader() {
    throw ERR_METHOD_NOT_IMPLEMENTED("_implicitHeader()");
  };

  function onError(msg, err, callback) {
    if (msg.destroyed) return;
    nextTick(emitErrorNt, msg, err, callback);
  }
  function emitErrorNt(msg, err, callback) {
    callback(err);
    if (typeof msg.emit === "function" && !msg.destroyed) msg.emit("error", err);
  }
  function strictContentLengthCheck(msg) {
    return msg.strictContentLength && msg._contentLength != null && msg._hasBody &&
      !msg._removedContLen && !msg.chunkedEncoding && !msg.hasHeader("transfer-encoding");
  }

  function write_(msg, chunk, encoding, callback, fromEnd) {
    if (typeof callback !== "function") callback = nop;
    if (chunk === null) throw ERR_STREAM_NULL_VALUES();
    if (typeof chunk !== "string" && !isUint8Array(chunk)) {
      throw ERR_INVALID_ARG_TYPE("chunk", ["string", "Buffer", "Uint8Array"], chunk);
    }
    let err;
    if (msg.finished) err = ERR_STREAM_WRITE_AFTER_END();
    else if (msg.destroyed) err = ERR_STREAM_DESTROYED("write");
    if (err) {
      if (!msg.destroyed) onError(msg, err, callback);
      else nextTick(callback, err);
      return false;
    }
    let len;
    if (msg.strictContentLength) {
      if (len === undefined) {
        len = typeof chunk === "string" ? G.Buffer.byteLength(chunk, encoding) : chunk.byteLength;
      }
      if (strictContentLengthCheck(msg) &&
          (fromEnd ? msg[kBytesWritten] + len !== msg._contentLength
                   : msg[kBytesWritten] + len > msg._contentLength)) {
        throw ERR_HTTP_CONTENT_LENGTH_MISMATCH(len + msg[kBytesWritten], msg._contentLength);
      }
      msg[kBytesWritten] += len;
    }
    if (!msg._header) {
      if (fromEnd) {
        if (len === undefined) {
          len = typeof chunk === "string" ? G.Buffer.byteLength(chunk, encoding) : chunk.byteLength;
        }
        msg._contentLength = len;
      }
      msg._implicitHeader();
    }
    if (!msg._hasBody) {
      if (msg[kRejectNonStandardBodyWrites]) throw ERR_HTTP_BODY_NOT_ALLOWED();
      nextTick(callback);
      return true;
    }
    let ret;
    if (msg.chunkedEncoding && chunk.length !== 0) {
      if (len === undefined) {
        len = typeof chunk === "string" ? G.Buffer.byteLength(chunk, encoding) : chunk.byteLength;
      }
      if (msg[kCorked] && msg._headerSent) {
        msg[kChunkedBuffer].push(chunk, encoding, callback);
        msg[kChunkedLength] += len;
        ret = msg[kChunkedLength] < msg[kHighWaterMark];
      } else {
        msg._send(len.toString(16), "latin1", null);
        msg._send(crlf_buf(), null, null);
        msg._send(chunk, encoding, null, len);
        ret = msg._send(crlf_buf(), null, callback);
      }
    } else {
      ret = msg._send(chunk, encoding, callback, len);
    }
    return ret;
  }

  OutgoingMessage.prototype.write = function write(chunk, encoding, callback) {
    if (typeof encoding === "function") { callback = encoding; encoding = null; }
    const ret = write_(this, chunk, encoding, callback, false);
    if (!ret) this[kNeedDrain] = true;
    return ret;
  };

  OutgoingMessage.prototype.addTrailers = function addTrailers(headers) {
    this._trailer = "";
    const keys = Object.keys(headers);
    const isArray = Array.isArray(headers);
    for (const key of keys) {
      let field, value;
      if (isArray) { field = headers[key][0]; value = headers[key][1]; }
      else { field = key; value = headers[key]; }
      validateHeaderName(field, "Trailer name");
      const isArrayValue = Array.isArray(value);
      if (isArrayValue && value.length > 1 &&
          (!this[kUniqueHeaders] || !this[kUniqueHeaders].has(field.toLowerCase()))) {
        for (const v of value) {
          if (checkInvalidHeaderChar(v)) throw ERR_INVALID_CHAR("trailer content", field);
          this._trailer += field + ": " + v + "\r\n";
        }
      } else {
        if (isArrayValue) value = value.join("; ");
        if (checkInvalidHeaderChar(value)) throw ERR_INVALID_CHAR("trailer content", field);
        this._trailer += field + ": " + value + "\r\n";
      }
    }
  };

  function onFinish(outmsg) {
    if (outmsg && outmsg.socket && outmsg.socket._hadError) return;
    outmsg.emit("finish");
  }

  OutgoingMessage.prototype.end = function end(chunk, encoding, callback) {
    if (typeof chunk === "function") { callback = chunk; chunk = null; encoding = null; }
    else if (typeof encoding === "function") { callback = encoding; encoding = null; }

    if (chunk) {
      if (this.finished) {
        onError(this, ERR_STREAM_WRITE_AFTER_END(), typeof callback !== "function" ? nop : callback);
        return this;
      }
      if (this[kSocket]) this[kSocket].cork();
      write_(this, chunk, encoding, null, true);
    } else if (this.finished) {
      if (typeof callback === "function") {
        if (!this.writableFinished) this.on("finish", callback);
        else callback(ERR_STREAM_ALREADY_FINISHED("end"));
      }
      return this;
    } else if (!this._header) {
      if (this[kSocket]) this[kSocket].cork();
      this._contentLength = 0;
      this._implicitHeader();
    }

    if (typeof callback === "function") this.once("finish", callback);
    if (strictContentLengthCheck(this) && this[kBytesWritten] !== this._contentLength) {
      throw ERR_HTTP_CONTENT_LENGTH_MISMATCH(this[kBytesWritten], this._contentLength);
    }

    const finish = onFinish.bind(undefined, this);
    if (this._hasBody && this.chunkedEncoding) {
      this._send("0\r\n" + this._trailer + "\r\n", "latin1", finish);
    } else if (!this._headerSent || this.writableLength || chunk) {
      this._send("", "latin1", finish);
    } else {
      nextTick(finish);
    }

    if (this[kSocket]) {
      if (this[kSocket]._writableState) this[kSocket]._writableState.corked = 1;
      this[kSocket].uncork();
    }
    this[kCorked] = 1;
    this.uncork();
    this.finished = true;

    if (this.outputData.length === 0 && this[kSocket] && this[kSocket]._httpMessage === this) {
      this._finish();
    }
    return this;
  };

  OutgoingMessage.prototype._finish = function _finish() { this.emit("prefinish"); };

  OutgoingMessage.prototype._flush = function _flush() {
    const socket = this[kSocket];
    if (socket && socket.writable) {
      this._flushOutput(socket);
      if (this.finished) this._finish();
      else if (this[kNeedDrain] && this.writableLength === 0) {
        this[kNeedDrain] = false;
        this.emit("drain");
      }
    }
  };

  OutgoingMessage.prototype._flushOutput = function _flushOutput(socket) {
    const outputLength = this.outputData.length;
    if (outputLength <= 0) return undefined;
    const outputData = this.outputData;
    socket.cork();
    let ret;
    for (let i = 0; i < outputLength; i++) {
      const item = outputData[i];
      const data = item.data;
      item.data = null;
      ret = socket.write(data, item.encoding, item.callback);
    }
    socket.uncork();
    this.outputData = [];
    this._onPendingData(-this.outputSize);
    this.outputSize = 0;
    return ret;
  };

  OutgoingMessage.prototype.flushHeaders = function flushHeaders() {
    if (!this._header) this._implicitHeader();
    this._send("");
  };

  OutgoingMessage.prototype.pipe = function pipe() {
    this.emit("error", ERR_STREAM_CANNOT_PIPE());
  };
  if (EventEmitter.captureRejectionSymbol) {
    OutgoingMessage.prototype[EventEmitter.captureRejectionSymbol] = function (err) { this.destroy(err); };
  }

  // ================================================== ServerResponse
  // lib/_http_server.js.
  function ServerResponse(reqMsg, options) {
    OutgoingMessage.call(this, options);
    if (reqMsg && reqMsg.method === "HEAD") this._hasBody = false;
    this.req = reqMsg;
    this.sendDate = true;
    this._sent100 = false;
    this._expect_continue = false;
    if (reqMsg && (reqMsg.httpVersionMajor < 1 || reqMsg.httpVersionMinor < 1)) {
      this.useChunkedEncodingByDefault = chunkExpression.test(reqMsg.headers.te);
      this.shouldKeepAlive = false;
    }
    // node lib/_http_server.js: the ServerResponse constructor's last act is to
    // publish 'http.server.response.created' with the request it answers.
    const ch = httpDC().serverResponseCreated;
    if (ch.hasSubscribers) ch.publish({ request: reqMsg, response: this });
  }
  Object.setPrototypeOf(ServerResponse.prototype, OutgoingMessage.prototype);
  Object.setPrototypeOf(ServerResponse, OutgoingMessage);
  ServerResponse.prototype.statusCode = 200;
  ServerResponse.prototype.statusMessage = undefined;

  function onServerResponseClose() {
    if (this._httpMessage) emitCloseNT(this._httpMessage);
  }
  function emitCloseNT(self) {
    if (!self._closed) { self.destroyed = true; self._closed = true; self.emit("close"); }
  }

  ServerResponse.prototype.assignSocket = function assignSocket(socket) {
    if (socket._httpMessage) throw ERR_HTTP_SOCKET_ASSIGNED();
    socket._httpMessage = this;
    socket.on("close", onServerResponseClose);
    this.socket = socket;
    this.emit("socket", socket);
    this._flush();
  };
  ServerResponse.prototype.detachSocket = function detachSocket(socket) {
    socket.removeListener("close", onServerResponseClose);
    socket._httpMessage = null;
    this.socket = null;
  };

  function processInformationHeader(name, value) {
    validateHeaderName(name);
    validateHeaderValue(name, value);
    return name + ": " + value + "\r\n";
  }
  ServerResponse.prototype.writeInformation = function writeInformation(statusCode, headers, cb) {
    if (this._header) throw ERR_HTTP_HEADERS_SENT("write");
    validateInteger(statusCode, "statusCode", 100, 199);
    if (statusCode === 101) throw ERR_HTTP_INVALID_STATUS_CODE(statusCode);
    const statusMessage = STATUS_CODES[statusCode] || "unknown";
    let head = "HTTP/1.1 " + statusCode + " " + statusMessage + "\r\n";
    if (headers !== undefined && headers !== null) {
      if (Array.isArray(headers)) {
        if (headers.length && Array.isArray(headers[0])) {
          for (const entry of headers) head += processInformationHeader(entry[0], entry[1]);
        } else {
          if (headers.length % 2 !== 0) throw ERR_INVALID_ARG_VALUE("headers", headers);
          for (let i = 0; i < headers.length; i += 2) {
            head += processInformationHeader(headers[i], headers[i + 1]);
          }
        }
      } else {
        validateObject(headers, "headers");
        for (const key of Object.keys(headers)) head += processInformationHeader(key, headers[key]);
      }
    }
    head += "\r\n";
    return this._writeRaw(head, "ascii", cb);
  };
  ServerResponse.prototype.writeContinue = function writeContinue(cb) {
    this.writeInformation(100, null, cb);
    this._sent100 = true;
  };
  ServerResponse.prototype.writeProcessing = function writeProcessing(cb) {
    this.writeInformation(102, null, cb);
  };
  // node internal/validators.js linkValueRegExp, character for character. The
  // two deviations mbun carried were both observable: `[^>]*` inside the angle
  // brackets accepted a CRLF smuggled into the URI-reference (the corpus asserts
  // ERR_INVALID_ARG_VALUE for `</styles.css\r\nSet-Cookie: evil>`), and
  // excluding `=` from the param-name class rejected shapes node accepts.
  const linkValueRegExp = /^(?:<[^>\r\n]*>)(?:\s*;\s*[^;"\s]+(?:=(")?[^;"\s]*\1)?)*$/;
  const LINK_HINT = 'must be an array or string of format "</styles.css>; rel=preload; as=style"';
  function validateLinkHeaderFormat(value, name) {
    if (typeof value === "undefined" || !linkValueRegExp.exec(value)) {
      throw ERR_INVALID_ARG_VALUE(name, value, LINK_HINT);
    }
  }
  function validateLinkHeaderValue(hints) {
    if (typeof hints === "string") { validateLinkHeaderFormat(hints, "hints.link"); return hints; }
    if (Array.isArray(hints)) {
      const length = hints.length;
      if (length === 0) return "";
      let result = "";
      for (let i = 0; i < length; i++) {
        validateLinkHeaderFormat(hints[i], "hints.link");
        result += hints[i];
        if (i !== length - 1) result += ", ";
      }
      return result;
    }
    throw ERR_INVALID_ARG_VALUE("hints.link", hints, LINK_HINT);
  }
  ServerResponse.prototype.writeEarlyHints = function writeEarlyHints(hints, cb) {
    validateObject(hints, "hints");
    if (hints.link === null || hints.link === undefined) return;
    const link = validateLinkHeaderValue(hints.link);
    if (link.length === 0) return;
    if (checkInvalidHeaderChar(link)) throw ERR_INVALID_CHAR("header content", "Link");
    const headers = { __proto__: null, Link: link };
    for (const key of Object.keys(hints)) { if (key !== "link") headers[key] = hints[key]; }
    this.writeInformation(103, headers, cb);
  };

  ServerResponse.prototype._implicitHeader = function _implicitHeader() {
    this.writeHead(this.statusCode);
  };
  ServerResponse.prototype.writeHead = function writeHead(statusCode, reason, obj) {
    if (this._header) throw ERR_HTTP_HEADERS_SENT("write");
    const originalStatusCode = statusCode;
    statusCode |= 0;
    if (statusCode < 100 || statusCode > 999) throw ERR_HTTP_INVALID_STATUS_CODE(originalStatusCode);
    if (typeof reason === "string") {
      this.statusMessage = reason;
    } else {
      if (!this.statusMessage) this.statusMessage = STATUS_CODES[statusCode] || "unknown";
      if (obj === undefined || obj === null) obj = reason;
    }
    this.statusCode = statusCode;

    let headers;
    if (this[kOutHeaders]) {
      let k;
      if (Array.isArray(obj)) {
        if (obj.length % 2 !== 0) throw ERR_INVALID_ARG_VALUE("headers", obj);
        for (let n = 0; n < obj.length; n += 2) { k = obj[n]; this.removeHeader(k); }
        for (let n = 0; n < obj.length; n += 2) { k = obj[n]; if (k) this.appendHeader(k, obj[n + 1]); }
      } else if (obj) {
        for (const key of Object.keys(obj)) { if (key) this.setHeader(key, obj[key]); }
      }
      headers = this[kOutHeaders];
    } else {
      headers = obj;
    }

    if (checkInvalidHeaderChar(this.statusMessage)) throw ERR_INVALID_CHAR("statusMessage");
    const statusLine = "HTTP/1.1 " + statusCode + " " + this.statusMessage + "\r\n";
    if (statusCode === 204 || statusCode === 304 || (statusCode >= 100 && statusCode <= 199)) {
      this._hasBody = false;
    }
    if (this._expect_continue && !this._sent100) this.shouldKeepAlive = false;
    this._storeHeader(statusLine, headers);
    return this;
  };
  ServerResponse.prototype.writeHeader = ServerResponse.prototype.writeHead;

  // ================================================== IncomingMessage
  // lib/_http_incoming.js.
  class IncomingMessage extends Readable {
    constructor(socket) {
      super(socket ? { highWaterMark: socket.readableHighWaterMark } : undefined);
      this._readableState.readingMore = true;
      // `socket`, not `socket || null`: lib/_http_incoming.js assigns the
      // argument straight through, so `new IncomingMessage()` leaves .socket /
      // .connection *undefined* (test-http-incoming-message-connection-setter
      // asserts exactly that before exercising the connection setter).
      this.socket = socket;
      this.httpVersionMajor = null;
      this.httpVersionMinor = null;
      this.httpVersion = null;
      this.complete = false;
      this[kHeaders] = null;
      this[kHeadersCount] = 0;
      this.rawHeaders = [];
      this[kTrailers] = null;
      this[kTrailersCount] = 0;
      this.rawTrailers = [];
      this.joinDuplicateHeaders = false;
      this.aborted = false;
      this.upgrade = null;
      this.url = "";
      this.method = null;
      this.statusCode = null;
      this.statusMessage = null;
      this.client = socket;
      this._consuming = false;
      this._dumped = false;
      this[kAbortController] = null;
    }
  }
  defGet(IncomingMessage.prototype, "connection",
    function () { return this.socket; }, function (v) { this.socket = v; });
  const lazyHeaderMap = (self, kCache, kCount, rawKey, adder, nullProto) => {
    if (!self[kCache]) {
      const dst = nullProto ? { __proto__: null } : {};
      self[kCache] = dst;
      const src = self[rawKey];
      for (let n = 0; n < self[kCount]; n += 2) adder.call(self, src[n], src[n + 1], dst);
    }
    return self[kCache];
  };
  defGet(IncomingMessage.prototype, "headers",
    function () { return lazyHeaderMap(this, kHeaders, kHeadersCount, "rawHeaders", _addHeaderLine, false); },
    function (v) { this[kHeaders] = v; });
  defGet(IncomingMessage.prototype, "headersDistinct",
    function () { return lazyHeaderMap(this, kHeadersDistinct, kHeadersCount, "rawHeaders", _addHeaderLineDistinct, true); },
    function (v) { this[kHeadersDistinct] = v; });
  defGet(IncomingMessage.prototype, "trailers",
    function () { return lazyHeaderMap(this, kTrailers, kTrailersCount, "rawTrailers", _addHeaderLine, false); },
    function (v) { this[kTrailers] = v; });
  defGet(IncomingMessage.prototype, "trailersDistinct",
    function () { return lazyHeaderMap(this, kTrailersDistinct, kTrailersCount, "rawTrailers", _addHeaderLineDistinct, true); },
    function (v) { this[kTrailersDistinct] = v; });
  Object.defineProperty(IncomingMessage.prototype, "signal", {
    configurable: true,
    get() {
      if (this[kAbortController] === null) {
        const ac = new G.AbortController();
        this[kAbortController] = ac;
        if (this.destroyed) ac.abort();
        else this.once("close", () => ac.abort());
      }
      return this[kAbortController].signal;
    },
  });
  IncomingMessage.prototype.setTimeout = function setTimeout(msecs, callback) {
    if (callback) this.on("timeout", callback);
    if (this.socket) this.socket.setTimeout(msecs);
    return this;
  };
  IncomingMessage.prototype._read = function _read(_n) {
    if (!this._consuming) {
      this._readableState.readingMore = false;
      this._consuming = true;
    }
    const socket = this.socket;
    if (socket && socket.readable && !socket._paused) socket.resume();
  };
  IncomingMessage.prototype._destroy = function _destroy(err, cb) {
    if (!this.readableEnded || !this.complete) {
      this.aborted = true;
      this.emit("aborted");
    }
    if (this.socket && !this.socket.destroyed && this.aborted) this.socket.destroy(err);
    nextTick(imOnError, this, err, cb);
  };
  function imOnError(self, error, cb) {
    if (typeof cb !== "function") return;
    if (self.listenerCount("error") === 0) cb();
    else cb(error);
  }
  IncomingMessage.prototype._addHeaderLines = function _addHeaderLines(headers, n) {
    if (headers && headers.length) {
      if (n === undefined) n = headers.length;
      let dest;
      if (this.complete) {
        this.rawTrailers = headers;
        this[kTrailersCount] = n;
        dest = this[kTrailers];
      } else {
        this.rawHeaders = headers;
        this[kHeadersCount] = n;
        dest = this[kHeaders];
      }
      if (dest) {
        for (let i = 0; i < n; i += 2) this._addHeaderLine(headers[i], headers[i + 1], dest);
      }
    }
  };
  // Known-field table (lib/_http_incoming.js matchKnownFields). The returned
  // name carries a flag byte: 0 = ", "-joined list, 2 = "; "-joined (Cookie),
  // 1 = the one array field (Set-Cookie); no flag = first-one-wins.
  function matchKnownFields(field, lowercased) {
    switch (field.length) {
      case 3:
        if (field === "Age" || field === "age") return "age";
        break;
      case 4:
        if (field === "Host" || field === "host") return "host";
        if (field === "From" || field === "from") return "from";
        if (field === "ETag" || field === "etag") return "etag";
        if (field === "Date" || field === "date") return "\u0000date";
        if (field === "Vary" || field === "vary") return "\u0000vary";
        break;
      case 6:
        if (field === "Server" || field === "server") return "server";
        if (field === "Cookie" || field === "cookie") return "\u0002cookie";
        if (field === "Origin" || field === "origin") return "\u0000origin";
        if (field === "Expect" || field === "expect") return "\u0000expect";
        if (field === "Accept" || field === "accept") return "\u0000accept";
        break;
      case 7:
        if (field === "Referer" || field === "referer") return "referer";
        if (field === "Expires" || field === "expires") return "expires";
        if (field === "Upgrade" || field === "upgrade") return "\u0000upgrade";
        break;
      case 8:
        if (field === "Location" || field === "location") return "location";
        if (field === "If-Match" || field === "if-match") return "\u0000if-match";
        break;
      case 10:
        if (field === "User-Agent" || field === "user-agent") return "user-agent";
        if (field === "Set-Cookie" || field === "set-cookie") return "\u0001";
        if (field === "Connection" || field === "connection") return "\u0000connection";
        break;
      case 11:
        if (field === "Retry-After" || field === "retry-after") return "retry-after";
        break;
      case 12:
        if (field === "Content-Type" || field === "content-type") return "content-type";
        if (field === "Max-Forwards" || field === "max-forwards") return "max-forwards";
        break;
      case 13:
        if (field === "Authorization" || field === "authorization") return "authorization";
        if (field === "Last-Modified" || field === "last-modified") return "last-modified";
        if (field === "Cache-Control" || field === "cache-control") return "\u0000cache-control";
        if (field === "If-None-Match" || field === "if-none-match") return "\u0000if-none-match";
        break;
      case 14:
        if (field === "Content-Length" || field === "content-length") return "content-length";
        break;
      case 15:
        if (field === "Accept-Encoding" || field === "accept-encoding") return "\u0000accept-encoding";
        if (field === "Accept-Language" || field === "accept-language") return "\u0000accept-language";
        if (field === "X-Forwarded-For" || field === "x-forwarded-for") return "\u0000x-forwarded-for";
        break;
      case 16:
        if (field === "Content-Encoding" || field === "content-encoding") return "\u0000content-encoding";
        if (field === "X-Forwarded-Host" || field === "x-forwarded-host") return "\u0000x-forwarded-host";
        break;
      case 17:
        if (field === "If-Modified-Since" || field === "if-modified-since") return "if-modified-since";
        if (field === "Transfer-Encoding" || field === "transfer-encoding") return "\u0000transfer-encoding";
        if (field === "X-Forwarded-Proto" || field === "x-forwarded-proto") return "\u0000x-forwarded-proto";
        break;
      case 19:
        if (field === "Proxy-Authorization" || field === "proxy-authorization") return "proxy-authorization";
        if (field === "If-Unmodified-Since" || field === "if-unmodified-since") return "if-unmodified-since";
        break;
    }
    if (lowercased) return "\u0000" + field;
    return matchKnownFields(field.toLowerCase(), true);
  }
  function _addHeaderLine(field, value, dest) {
    field = matchKnownFields(field);
    const flag = field.charCodeAt(0);
    if (flag === 0 || flag === 2) {
      field = field.slice(1);
      if (typeof dest[field] === "string") dest[field] += (flag === 0 ? ", " : "; ") + value;
      else dest[field] = value;
    } else if (flag === 1) {
      if (dest["set-cookie"] !== undefined) dest["set-cookie"].push(value);
      else dest["set-cookie"] = [value];
    } else if (this && this.joinDuplicateHeaders) {
      if (dest[field] === undefined) dest[field] = value;
      else dest[field] += ", " + value;
    } else if (dest[field] === undefined) {
      dest[field] = value;
    }
  }
  function _addHeaderLineDistinct(field, value, dest) {
    field = field.toLowerCase();
    if (!dest[field]) dest[field] = [value];
    else dest[field].push(value);
  }
  IncomingMessage.prototype._addHeaderLine = _addHeaderLine;
  IncomingMessage.prototype._addHeaderLineDistinct = _addHeaderLineDistinct;
  IncomingMessage.prototype._dump = function _dump() {
    if (!this._dumped) {
      this._dumped = true;
      this.removeAllListeners("data");
      this.resume();
    }
  };

  // ================================================== Agent (_http_agent.js)
  const kOnKeylog = Symbol("onkeylog");
  function freeSocketErrorListener(err) {
    const socket = this;
    socket.destroy();
    socket.emit("agentRemove");
  }
  function Agent(options) {
    if (!(this instanceof Agent)) return new Agent(options);
    EventEmitter.call(this);
    this.options = { __proto__: null, ...options };
    this.defaultPort = this.options.defaultPort || 80;
    this.protocol = this.options.protocol || "http:";
    if (this.options.noDelay === undefined) this.options.noDelay = true;
    this.options.path = null;
    this.requests = { __proto__: null };
    this.sockets = { __proto__: null };
    this.freeSockets = { __proto__: null };
    this.keepAliveMsecs = this.options.keepAliveMsecs || 1000;
    this.keepAlive = this.options.keepAlive || false;
    this.maxSockets = this.options.maxSockets || Agent.defaultMaxSockets;
    this.maxFreeSockets = this.options.maxFreeSockets || 256;
    this.scheduling = this.options.scheduling || "lifo";
    this.maxTotalSockets = this.options.maxTotalSockets;
    this.totalSocketCount = 0;
    this.maxCachedSessions = 100;
    this.agentKeepAliveTimeoutBuffer =
      typeof this.options.agentKeepAliveTimeoutBuffer === "number" &&
      this.options.agentKeepAliveTimeoutBuffer >= 0 &&
      Number.isFinite(this.options.agentKeepAliveTimeoutBuffer)
        ? this.options.agentKeepAliveTimeoutBuffer : 1000;

    validateOneOf(this.scheduling, "scheduling", ["fifo", "lifo"]);
    if (this.maxTotalSockets !== undefined) {
      validateNumber(this.maxTotalSockets, "maxTotalSockets", 1);
      // Relational comparisons deliberately leave NaN unordered, but Node's
      // validateNumber rejects it for this positive socket-count limit.
      if (Number.isNaN(this.maxTotalSockets)) {
        throw ERR_OUT_OF_RANGE("maxTotalSockets", ">= 1", this.maxTotalSockets);
      }
    } else this.maxTotalSockets = Infinity;

    this.on("free", (socket, options) => {
      const name = this.getName(options);
      if (!socket.writable) { socket.destroy(); return; }
      const requests = this.requests[name];
      if (requests && requests.length) {
        const pending = requests.shift();
        setRequestSocket(this, pending, socket);
        if (requests.length === 0) delete this.requests[name];
        return;
      }
      const msg = socket._httpMessage;
      if (!msg || !msg.shouldKeepAlive || !this.keepAlive) { socket.destroy(); return; }
      const freeSockets = this.freeSockets[name] || [];
      const freeLen = freeSockets.length;
      let count = freeLen;
      if (this.sockets[name]) count += this.sockets[name].length;
      if (this.totalSocketCount > this.maxTotalSockets || count > this.maxSockets ||
          freeLen >= this.maxFreeSockets || !this.keepSocketAlive(socket)) {
        socket.destroy();
        return;
      }
      this.freeSockets[name] = freeSockets;
      socket._httpMessage = null;
      this.removeSocket(socket, options);
      socket.once("error", freeSocketErrorListener);
      freeSockets.push(socket);
    });
    this.on("newListener", maybeEnableKeylog);
  }
  Object.setPrototypeOf(Agent.prototype, EventEmitter.prototype);
  Object.setPrototypeOf(Agent, EventEmitter);
  function maybeEnableKeylog(eventName) {
    if (eventName === "keylog") {
      this.removeListener("newListener", maybeEnableKeylog);
      const agent = this;
      this[kOnKeylog] = function onkeylog(keylog) { agent.emit("keylog", keylog, this); };
      for (const list of Object.values(this.sockets)) {
        for (const sock of list) sock.on("keylog", this[kOnKeylog]);
      }
    }
  }
  Agent.defaultMaxSockets = Infinity;

  const netModule = () => M["net"] || M["node:net"] || {};
  Agent.prototype.createConnection = function createConnection(options, cb) {
    const net = netModule();
    if (typeof net.createConnection === "function") return net.createConnection(options, cb);
    return net.connect(options, cb);
  };
  Agent.prototype.getName = function getName(options) {
    options = options || {};
    let name = options.host || "localhost";
    name += ":";
    if (options.port) name += options.port;
    name += ":";
    if (options.localAddress) name += options.localAddress;
    if (options.family === 4 || options.family === 6) name += ":" + options.family;
    if (options.socketPath) name += ":" + options.socketPath;
    return name;
  };
  // _http_agent.js normalizeServerName / calculateServerName: the TLS server name
  // a request verifies against is derived from the HOST HEADER, not from the
  // connect target. node:https then hands that to tls.connect as `servername`, so
  // `https.get({ host: undefined, headers: { host: "agent1" } })` verifies against
  // CN=agent1 — without this the peer certificate is checked against the connect
  // hostname ("localhost") and a perfectly valid fixture chain is rejected.
  // Also part of the pool key (getName includes servername), so two requests to
  // the same address with different Host headers cannot share a TLS socket.
  function calculateServerName(options, req) {
    let servername = options.host;
    const hostHeader = req && typeof req.getHeader === "function" ? req.getHeader("host") : undefined;
    if (hostHeader) {
      validateString(hostHeader, "options.headers.host");
      // abc => abc, abc:123 => abc, [::1] => ::1, [::1]:123 => ::1
      if (hostHeader[0] === "[") {
        const index = hostHeader.indexOf("]");
        servername = index === -1 ? hostHeader : hostHeader.substring(1, index);
      } else {
        servername = hostHeader.split(":", 1)[0];
      }
    }
    // Don't implicitly set invalid (IP) servernames.
    const net = netModule();
    if (typeof net.isIP === "function" && net.isIP(servername)) servername = "";
    return servername;
  }
  function normalizeServerName(options, req) {
    if (!options.servername && options.servername !== "") {
      options.servername = calculateServerName(options, req);
    }
  }
  Agent.prototype.addRequest = function addRequest(request, options, port, localAddress) {
    if (typeof options === "string") {
      options = { __proto__: null, host: options, port, localAddress };
    }
    options = { __proto__: null, ...options, ...this.options };
    if (options.socketPath) options.path = options.socketPath;
    normalizeServerName(options, request);
    const name = this.getName(options);
    if (!this.sockets[name]) this.sockets[name] = [];
    const freeSockets = this.freeSockets[name];
    let socket;
    if (freeSockets) {
      while (freeSockets.length && freeSockets[0].destroyed) freeSockets.shift();
      socket = this.scheduling === "fifo" ? freeSockets.shift() : freeSockets.pop();
      if (!freeSockets.length) delete this.freeSockets[name];
    }
    const freeLen = freeSockets ? freeSockets.length : 0;
    const sockLen = freeLen + this.sockets[name].length;
    if (socket) {
      this.reuseSocket(socket, request);
      setRequestSocket(this, request, socket);
      this.sockets[name].push(socket);
    } else if (sockLen < this.maxSockets && this.totalSocketCount < this.maxTotalSockets) {
      this.createSocket(request, options, (err, sock) => {
        if (err) { request.onSocket(sock, err); return; }
        setRequestSocket(this, request, sock);
      });
    } else {
      if (!this.requests[name]) this.requests[name] = [];
      request[kRequestOptions] = options;
      this.requests[name].push(request);
    }
  };
  const once = (fn) => { let called = false; return (...a) => { if (called) return; called = true; return fn(...a); }; };
  Agent.prototype.createSocket = function createSocket(request, options, cb) {
    options = { __proto__: null, ...options, ...this.options };
    if (options.socketPath) options.path = options.socketPath;
    normalizeServerName(options, request);
    const timeout = request.timeout || this.options.timeout || undefined;
    if (timeout) options.timeout = timeout;
    const name = this.getName(options);
    options._agentKey = name;
    options.encoding = null;
    const oncreate = once((err, s) => {
      if (err) return cb(err);
      if (!this.sockets[name]) this.sockets[name] = [];
      this.sockets[name].push(s);
      this.totalSocketCount++;
      // net.createConnection receives the option but Socket does not arm an
      // idle timer by itself. Apply the Agent timeout before onSocket() so the
      // request observes both the interval and its forwarding listener.
      if (options.timeout !== undefined && typeof s.setTimeout === "function") {
        s.setTimeout(options.timeout);
      }
      installListeners(this, s, options);
      cb(null, s);
    });
    if (this.keepAlive) {
      options.keepAlive = this.keepAlive;
      options.keepAliveInitialDelay = this.keepAliveMsecs;
    }
    let newSocket;
    try { newSocket = this.createConnection(options, oncreate); }
    catch (e) { oncreate(e); return; }
    if (newSocket) oncreate(null, newSocket);
  };
  function installListeners(agent, s, options) {
    function onFree() { agent.emit("free", s, options); }
    s.on("free", onFree);
    function onClose() { agent.totalSocketCount--; agent.removeSocket(s, options); }
    s.on("close", onClose);
    function onTimeout() {
      const sockets = agent.freeSockets;
      if (Object.keys(sockets).some((name) => sockets[name].includes(s))) return s.destroy();
    }
    s.on("timeout", onTimeout);
    function onRemove() {
      agent.totalSocketCount--;
      agent.removeSocket(s, options);
      s.removeListener("close", onClose);
      s.removeListener("free", onFree);
      s.removeListener("timeout", onTimeout);
      s.removeListener("agentRemove", onRemove);
    }
    s.on("agentRemove", onRemove);
    if (agent[kOnKeylog]) s.on("keylog", agent[kOnKeylog]);
  }
  Agent.prototype.removeSocket = function removeSocket(s, options) {
    const name = this.getName(options);
    const sets = [this.sockets];
    if (!s.writable) sets.push(this.freeSockets);
    for (const sockets of sets) {
      if (sockets[name]) {
        const index = sockets[name].indexOf(s);
        if (index !== -1) {
          sockets[name].splice(index, 1);
          if (sockets[name].length === 0) delete sockets[name];
        }
      }
    }
    let pending;
    if (this.requests[name] && this.requests[name].length) {
      pending = this.requests[name][0];
    } else {
      for (const prop of Object.keys(this.requests)) {
        if (this.sockets[prop] && this.sockets[prop].length) break;
        pending = this.requests[prop][0];
        options = pending[kRequestOptions];
        break;
      }
    }
    if (pending && options) {
      pending[kRequestOptions] = undefined;
      this.createSocket(pending, options, (err, socket) => {
        if (err) { pending.onSocket(null, err); return; }
        socket.emit("free");
      });
    }
  };
  Agent.prototype.keepSocketAlive = function keepSocketAlive(socket) {
    if (typeof socket.setKeepAlive === "function") socket.setKeepAlive(true, this.keepAliveMsecs);
    if (typeof socket.unref === "function") socket.unref();
    let agentTimeout = this.options.timeout || 0;
    let canKeepSocketAlive = true;
    const msg = socket._httpMessage;
    if (msg && msg.res) {
      const keepAliveHint = msg.res.headers["keep-alive"];
      if (keepAliveHint) {
        const m = /^timeout=(\d+)/.exec(keepAliveHint);
        if (m) {
          let serverHintTimeout = (parseInt(m[1], 10) * 1000) - this.agentKeepAliveTimeoutBuffer;
          serverHintTimeout = serverHintTimeout > 0 ? serverHintTimeout : 0;
          if (serverHintTimeout === 0) canKeepSocketAlive = false;
          else if (serverHintTimeout < agentTimeout) agentTimeout = serverHintTimeout;
        }
      }
    }
    if (socket.timeout !== agentTimeout) socket.setTimeout(agentTimeout);
    return canKeepSocketAlive;
  };
  Agent.prototype.reuseSocket = function reuseSocket(socket, request) {
    socket.removeListener("error", freeSocketErrorListener);
    request.reusedSocket = true;
    if (typeof socket.ref === "function") socket.ref();
  };
  Agent.prototype.destroy = function destroy() {
    for (const set of [this.freeSockets, this.sockets]) {
      for (const key of Object.keys(set)) {
        for (const s of set[key].slice()) s.destroy();
      }
    }
  };
  function setRequestSocket(agent, request, socket) {
    // net.connect() completes on a microtask in this runtime, whereas node
    // gives ClientRequest's next-tick socket setup a chance to run first.
    // Keep the transport in its connecting state for that one setup turn: a
    // request timeout registered before the socket event must install only
    // after the transport's eventual 'connect', not overwrite the Agent's
    // initial timeout while the request is being attached.
    if (socket && socket.connecting) socket._httpClientConnectPending = true;
    request.onSocket(socket);
    const agentTimeout = agent.options.timeout || 0;
    if (request.timeout === undefined || request.timeout === agentTimeout) return;
    socket.setTimeout(request.timeout);
  }

  // ================================================== ClientRequest
  const INVALID_PATH_REGEX = /[^\u0021-\u00ff]/;
  function validateHost(host, name) {
    if (host !== null && host !== undefined && typeof host !== "string") {
      throw ERR_INVALID_ARG_TYPE("options." + name, ["string", "undefined", "null"], host, "property");
    }
    return host;
  }
  // internal/url.js urlToHttpOptions
  function urlToHttpOptions(url) {
    const hostname = typeof url.hostname === "string" && url.hostname.startsWith("[")
      ? url.hostname.slice(1, -1) : url.hostname;
    const options = {
      __proto__: null,
      ...url,
      protocol: url.protocol,
      hostname,
      hash: url.hash,
      search: url.search,
      pathname: url.pathname,
      path: (url.pathname || "") + (url.search || ""),
      href: url.href,
    };
    if (url.port !== "") options.port = Number(url.port);
    if (url.username || url.password) {
      options.auth = decodeURIComponent(url.username) + ":" + decodeURIComponent(url.password);
    }
    return options;
  }
  const isURL = (v) => v != null && typeof v === "object" && typeof v.href === "string" &&
    typeof v.protocol === "string" && typeof v.searchParams === "object";

  function ClientRequest(input, options, cb) {
    OutgoingMessage.call(this);

    if (typeof input === "string") {
      let parsed;
      try { parsed = new URL(input); }
      catch (e) {
        const err = new TypeError("Invalid URL");
        err.code = "ERR_INVALID_URL";
        err.input = input;
        throw err;
      }
      input = urlToHttpOptions(parsed);
    } else if (isURL(input)) {
      input = urlToHttpOptions(input);
    } else {
      cb = options;
      options = input;
      input = null;
    }

    if (typeof options === "function") {
      cb = options;
      options = input || { __proto__: null };
    } else {
      options = Object.assign({ __proto__: null }, input, options);
    }

    let agent = options.agent;
    // `http.globalAgent` is writable. Consult the exported module object for
    // plain HTTP requests so a later assignment affects subsequent clients;
    // HTTPS passes its own `_defaultAgent` explicitly above this fallback.
    const exportedHttp = M["node:http"] || M["http"];
    const defaultAgent = options._defaultAgent ||
      (exportedHttp && exportedHttp.globalAgent) || this._defaultAgent || globalAgent;
    if (agent === false) {
      agent = new defaultAgent.constructor();
    } else if (agent === null || agent === undefined) {
      if (typeof options.createConnection !== "function") agent = defaultAgent;
    } else if (typeof agent.addRequest !== "function") {
      throw ERR_INVALID_ARG_TYPE("options.agent", ["Agent-like Object", "undefined", "false"], agent, "property");
    }
    this.agent = agent;

    const protocol = options.protocol || defaultAgent.protocol;
    let expectedProtocol = defaultAgent.protocol;
    if (this.agent && this.agent.protocol) expectedProtocol = this.agent.protocol;

    if (options.path) {
      const p = String(options.path);
      if (INVALID_PATH_REGEX.test(p)) throw ERR_UNESCAPED_CHARACTERS("Request path");
    }
    if (protocol !== expectedProtocol) throw ERR_INVALID_PROTOCOL(protocol, expectedProtocol);

    const defaultPort = options.defaultPort || (this.agent && this.agent.defaultPort);
    const optsWithoutSignal = { __proto__: null, ...options };
    const port = optsWithoutSignal.port = options.port || defaultPort || 80;
    const host = optsWithoutSignal.host =
      validateHost(options.hostname, "hostname") || validateHost(options.host, "host") || "localhost";

    const setHost = options.setHost !== undefined ? Boolean(options.setHost)
                                                  : options.setDefaultHeaders !== false;
    this._removedConnection = options.setDefaultHeaders === false;
    this._removedContLen = options.setDefaultHeaders === false;
    this._removedTE = options.setDefaultHeaders === false;
    this.socketPath = options.socketPath;
    if (options.timeout !== undefined) this.timeout = getTimerDuration(options.timeout, "timeout");

    const signal = options.signal;
    if (signal) {
      const abortErr = () => {
        const e = new Error("The operation was aborted");
        e.name = "AbortError";
        e.code = "ABORT_ERR";
        if (signal.reason !== undefined) e.cause = signal.reason;
        return e;
      };
      // A signal that was already aborted at construction time destroys the
      // request before http.get() returns. The later socket assignment still
      // emits its error asynchronously through onSocketNT.
      if (signal.aborted) this.destroy(abortErr());
      else signal.addEventListener("abort", () => this.destroy(abortErr()), { once: true });
      delete optsWithoutSignal.signal;
      this.signal = signal;
    }

    let method = options.method;
    if (method != null) validateString(method, "options.method");
    if (method) {
      if (!checkIsHttpToken(method)) throw ERR_INVALID_HTTP_TOKEN("Method", method);
      method = this.method = method.toUpperCase();
    } else {
      method = this.method = "GET";
    }

    const maxHeaderSize = options.maxHeaderSize;
    if (maxHeaderSize !== undefined) validateInteger(maxHeaderSize, "maxHeaderSize", 0);
    this.maxHeaderSize = maxHeaderSize;

    const insecureHTTPParser = options.insecureHTTPParser;
    if (insecureHTTPParser !== undefined) validateBoolean(insecureHTTPParser, "options.insecureHTTPParser");
    this.insecureHTTPParser = insecureHTTPParser;

    const httpValidation = options.httpValidation;
    if (httpValidation !== undefined) {
      validateOneOf(httpValidation, "options.httpValidation", ["strict", "relaxed", "insecure"]);
      if (insecureHTTPParser !== undefined) {
        throw ERR_INVALID_ARG_VALUE("options.httpValidation", httpValidation,
          "cannot be used together with options.insecureHTTPParser");
      }
    }
    this.httpValidation = httpValidation;
    // Outgoing field-value alphabet. 'relaxed' and 'insecure' both widen it to
    // the Fetch-spec set (everything but NUL/CR/LF and non-latin1); 'strict' and
    // the default do not. `insecureHTTPParser: true` is node's older spelling of
    // the same opt-in, and `false` is an explicit refusal that must stay strict.
    this[kLenientHeaders] = httpValidation === undefined
      ? insecureHTTPParser === true
      : (httpValidation === "relaxed" || httpValidation === "insecure");

    if (options.joinDuplicateHeaders !== undefined) {
      validateBoolean(options.joinDuplicateHeaders, "options.joinDuplicateHeaders");
    }
    this.joinDuplicateHeaders = options.joinDuplicateHeaders;

    this[kPath] = options.path || "/";
    if (cb) this.once("response", cb);

    this.useChunkedEncodingByDefault = !(method === "GET" || method === "HEAD" ||
      method === "DELETE" || method === "OPTIONS" || method === "TRACE" || method === "CONNECT");

    this._ended = false;
    this.res = null;
    this.aborted = false;
    this.timeoutCb = null;
    this.upgradeOrConnect = false;
    this.parser = null;
    this.maxHeadersCount = null;
    this.reusedSocket = false;
    this.host = host;
    this.protocol = protocol;

    if (this.agent) {
      if (!this.agent.keepAlive && !Number.isFinite(this.agent.maxSockets)) {
        this._last = true;
        this.shouldKeepAlive = false;
      } else {
        this._last = false;
        this.shouldKeepAlive = true;
      }
    }

    const headersArray = Array.isArray(options.headers);
    if (!headersArray) {
      if (options.headers) {
        for (const key of Object.keys(options.headers)) this.setHeader(key, options.headers[key]);
      }
      if (host && !this.getHeader("host") && setHost) {
        let hostHeader = host;
        const posColon = hostHeader.indexOf(":");
        if (posColon !== -1 && hostHeader.indexOf(":", posColon + 1) !== -1 && hostHeader.charCodeAt(0) !== 91) {
          hostHeader = "[" + hostHeader + "]";
        }
        if (port && +port !== defaultPort) hostHeader += ":" + port;
        this.setHeader("Host", hostHeader);
      }
      if (options.auth && !this.getHeader("Authorization")) {
        this.setHeader("Authorization", "Basic " + G.Buffer.from(options.auth).toString("base64"));
      }
      if (this.getHeader("expect")) {
        if (this._header) throw ERR_HTTP_HEADERS_SENT("render");
        this._storeHeader(this.method + " " + this.path + " HTTP/1.1\r\n", this[kOutHeaders]);
      }
    } else {
      this._storeHeader(this.method + " " + this.path + " HTTP/1.1\r\n", options.headers);
    }
    this[kUniqueHeaders] = parseUniqueHeadersOption(options.uniqueHeaders);

    if (this.agent) {
      this.agent.addRequest(this, optsWithoutSignal);
    } else {
      this._last = true;
      this.shouldKeepAlive = false;
      let opts = optsWithoutSignal;
      if (opts.path || opts.socketPath) {
        opts = { ...optsWithoutSignal };
        if (opts.socketPath) opts.path = opts.socketPath;
        else if (opts.path) opts.path = undefined;
      }
      if (typeof opts.createConnection === "function") {
        const oncreate = once((err, socket) => {
          if (err) nextTick(() => emitErrorEvent(this, err));
          else this.onSocket(socket);
        });
        try {
          const newSocket = opts.createConnection(opts, oncreate);
          if (newSocket) oncreate(null, newSocket);
        } catch (err) { oncreate(err); }
      } else {
        const net = netModule();
        const connect = net.createConnection || net.connect;
        if (typeof connect !== "function") {
          nextTick(() => emitErrorEvent(this, new Error("node:net is unavailable")));
        } else {
          this.onSocket(connect.call(net, opts));
        }
      }
    }
    // node lib/_http_client.js: the last statement of the ClientRequest
    // constructor announces the request on 'http.client.request.created'.
    {
      const ch = httpDC().clientRequestCreated;
      if (ch.hasSubscribers) ch.publish({ request: this });
    }
  }
  Object.setPrototypeOf(ClientRequest.prototype, OutgoingMessage.prototype);
  Object.setPrototypeOf(ClientRequest, OutgoingMessage);
  // node ClientRequest.prototype._finish: the request message is complete and on
  // its way, which is what 'http.client.request.start' reports.
  ClientRequest.prototype._finish = function _finish() {
    OutgoingMessage.prototype._finish.call(this);
    // _flush() re-enters _finish for an already-finished message, so the publish
    // is latched: node reports one 'start' per request, not per flush.
    if (this._dcStartPublished) return;
    this._dcStartPublished = true;
    const ch = httpDC().clientRequestStart;
    if (ch.hasSubscribers) ch.publish({ request: this });
  };

  // node lib/_http_client.js emitErrorEvent: 'http.client.request.error' is
  // published for every client-side request error, ahead of the 'error' event.
  function emitErrorEvent(request, error) {
    const ch = httpDC().clientRequestError;
    if (ch.hasSubscribers) ch.publish({ request, error });
    request.emit("error", error);
  }

  Object.defineProperty(ClientRequest.prototype, "path", {
    get() { return this[kPath]; },
    set(value) {
      const p = String(value);
      if (INVALID_PATH_REGEX.test(p)) throw ERR_UNESCAPED_CHARACTERS("Request path");
      this[kPath] = p;
    },
    configurable: true, enumerable: true,
  });

  ClientRequest.prototype._implicitHeader = function _implicitHeader() {
    if (this._header) throw ERR_HTTP_HEADERS_SENT("render");
    this._storeHeader(this.method + " " + this.path + " HTTP/1.1\r\n", this[kOutHeaders]);
  };

  ClientRequest.prototype.abort = function abort() {
    if (this.aborted) return;
    this.aborted = true;
    nextTick(emitAbortNT, this);
    this.destroy();
  };
  function emitAbortNT(request) { request.emit("abort"); }

  ClientRequest.prototype.destroy = function destroy(err) {
    if (this.destroyed) return this;
    this.destroyed = true;
    if (this.res) this.res._dump();
    this[kError] = err;
    if (this.socket) this.socket.destroy(err);
    return this;
  };

  // ---- socket / parser plumbing (_http_client.js) ---------------------------
  function ondrain() {
    const msg = this._httpMessage;
    if (msg && !msg.finished && msg[kNeedDrain]) {
      msg[kNeedDrain] = false;
      msg.emit("drain");
    }
  }
  function emitRequestTimeout() {
    const request = this._httpMessage;
    if (request) request.emit("timeout");
  }
  function listenSocketTimeout(request) {
    if (request.timeoutCb) return;
    request.timeoutCb = emitRequestTimeout;
    if (request.socket) request.socket.once("timeout", emitRequestTimeout);
    else request.on("socket", (socket) => socket.once("timeout", emitRequestTimeout));
  }
  function emitFreeNT(request) {
    request._closed = true;
    request.emit("close");
    if (request.socket) request.socket.emit("free");
  }
  function socketErrorListener(err) {
    const socket = this;
    const request = socket._httpMessage;
    // One error per socket: 'end' and 'error' can both fire for the same
    // failure (a peer RST reads as EOF then ECONNRESET), and node's tests count
    // the request's 'error' events exactly (test-http-set-timeout).
    if (request && !socket._hadError) {
      socket._hadError = true;
      emitErrorEvent(request, err);
    }
    socket.destroy();
  }

  function tickOnSocket(request, socket) {
    const Parser = G.__mbunHttpParser;
    request.socket = socket;
    socket._httpMessage = request;
    if (!Parser) { request.emit("socket", socket); return; }
    // _http_common.js parsers.alloc(): one recycled parser per connection, not
    // one per request (test-http-parser-free asserts the identity).
    const parser = typeof Parser.alloc === "function" ? Parser.alloc(true) : new Parser(true);
    parser.reqMethod = request.method;
    parser.socket = socket;
    parser.outgoing = request;
    request.parser = parser;
    socket.parser = parser;
    if (typeof request.maxHeaderSize === "number") parser.maxHeaderSize = request.maxHeaderSize;
    // _http_client.js: `parser.setLenientFlags(...)` when the request opted into
    // insecureHTTPParser (or the process did via --insecure-http-parser).
    parser.lenient = request.insecureHTTPParser === undefined
      ? !!(G.__mbunHttpNative && G.__mbunHttpNative.insecureHTTPParser)
      : !!request.insecureHTTPParser;

    let res = null;
    let upgraded = false;
    let detached = false;
    const detach = () => {
      if (detached) return;
      detached = true;
      socket.removeListener("data", onData);
      socket.removeListener("end", onEnd);
      socket.removeListener("drain", ondrain);
      if (socket.parser === parser) socket.parser = null;
      request.parser = null;
      if (typeof Parser.free === "function") Parser.free(parser);
    };
    const hardDetach = () => {
      detach();
      socket.removeListener("close", onSocketClose);
      socket.removeListener("error", socketErrorListener);
    };

    // 1xx interim responses: node re-arms the parser and surfaces
    // 'continue'/'information' instead of treating them as the final response.
    parser.onInterim = (info) => {
      if (info.status === 100) request.emit("continue");
      const vp = String(info.httpVersion).split(".");
      request.emit("information", {
        statusCode: info.status,
        statusMessage: info.statusText,
        httpVersion: info.httpVersion,
        httpVersionMajor: +vp[0],
        httpVersionMinor: +vp[1],
        headers: info.headers,
        rawHeaders: info.rawHeaders,
      });
    };

    parser.onHead = () => {
      if (request.res) { socket.destroy(); return; }
      res = new IncomingMessage(socket);
      res.httpVersion = parser.httpVersion;
      const vp = String(parser.httpVersion).split(".");
      res.httpVersionMajor = +vp[0];
      res.httpVersionMinor = +vp[1];
      res.statusCode = parser.status;
      res.statusMessage = parser.statusText;
      res.joinDuplicateHeaders = !!request.joinDuplicateHeaders;
      res._addHeaderLines(parser.rawHeaders, parser.rawHeaders.length);
      request.res = res;

      const isConnect = request.method === "CONNECT";
      if (parser.status === 101 || (isConnect && parser.status >= 200 && parser.status < 300)) {
        upgraded = true;
        res.upgrade = true;
        const head = G.Buffer.from(parser.leftover());
        const ev = isConnect ? "connect" : "upgrade";
        hardDetach();
        if (request.listenerCount(ev) > 0) {
          request.upgradeOrConnect = true;
          socket.emit("agentRemove");
          socket._httpMessage = null;
          request.emit(ev, res, socket, head);
          request.destroyed = true;
          request._closed = true;
          request.emit("close");
        } else {
          socket.destroy();
        }
        return;
      }

      const connHdr = String(res.headers["connection"] || "").toLowerCase();
      const peerKeepAlive = (res.httpVersionMajor > 0 && res.httpVersionMinor > 0)
        ? connHdr.indexOf("close") === -1 : connHdr.indexOf("keep-alive") !== -1;
      if (request.shouldKeepAlive && !peerKeepAlive && !request.upgradeOrConnect) {
        request.shouldKeepAlive = false;
      }
      // node parserOnIncomingClient: 'http.client.response.finish' fires once the
      // response head has been parsed, before the 'response' event.
      {
        const ch = httpDC().clientResponseFinish;
        if (ch.hasSubscribers) ch.publish({ request, response: res });
      }
      res.req = request;
      res.on("end", responseOnEnd);
      request.on("finish", requestOnFinish);
      socket.on("timeout", responseOnTimeout);
      if (request.aborted || !request.emit("response", res)) res._dump();
    };

    function responseOnTimeout() {
      const r = socket._httpMessage;
      if (!r || !r.res) return;
      r.res.emit("timeout");
    }
    function responseOnEnd() {
      const r = this.req;
      if (!r) return;
      if (r.timeoutCb) socket.removeListener("timeout", emitRequestTimeout);
      socket.removeListener("timeout", responseOnTimeout);
      r._ended = true;
      if (!r.shouldKeepAlive) {
        if (socket.writable) {
          if (typeof socket.destroySoon === "function") socket.destroySoon();
          else socket.end();
        }
      } else if (r.writableFinished && !this.aborted) {
        responseKeepAlive(r);
      }
    }
    function requestOnFinish() {
      if (request.shouldKeepAlive && request._ended && !request.destroyed) responseKeepAlive(request);
    }
    function responseKeepAlive(r) {
      if (r.timeoutCb) { socket.setTimeout(0, r.timeoutCb); r.timeoutCb = null; }
      hardDetach();
      nextTick(emitFreeNT, r);
      r.destroyed = true;
      if (r.res) r.res.socket = null;
    }

    parser.onBody = (b) => {
      if (res && !upgraded) res.push(G.Buffer.from(b.slice ? b.slice() : b));
    };
    parser.onDone = () => {
      if (!res || upgraded) return;
      res.complete = true;
      // Prefer the parser's verbatim pairs: rebuilding them from the folded
      // map lowercases every name and drops duplicates, which node's
      // res.rawTrailers keeps.
      const rawTr = parser.rawTrailers;
      if (rawTr && rawTr.length) {
        res._addHeaderLines(rawTr, rawTr.length);
      } else {
        const tr = parser.trailers;
        if (tr) {
          const raw = [];
          for (const k of Object.keys(tr)) raw.push(k, tr[k]);
          if (raw.length) res._addHeaderLines(raw, raw.length);
        }
      }
      detach();
      res.push(null);
    };
    parser.onError = (e) => {
      // A peer RST reads as EOF first: onEnd already raised 'socket hang up'
      // and the parser's own eof() error is the same failure seen twice.
      const already = socket._hadError;
      detach();
      socket._hadError = true;
      socket.destroy();
      if (!already && !request.destroyed && !upgraded) emitErrorEvent(request, e);
    };

    function onData(chunk) {
      if (upgraded) return;
      const b = chunk instanceof Uint8Array ? chunk
        : new Uint8Array(chunk.buffer || chunk, chunk.byteOffset || 0,
                         chunk.byteLength !== undefined ? chunk.byteLength : chunk.length);
      parser.push(b);
    }
    function onEnd() {
      if (upgraded) return;
      if (!request.res && !socket._hadError) {
        // EOF can be what first exposes a malformed response. Detach before
        // publishing the request error so observers never retain the parser's
        // data listener after a terminal parse failure.
        detach();
        socket._hadError = true;
        emitErrorEvent(request, ConnResetException("socket hang up"));
      }
      try { parser.eof(); } catch (e) {}
      socket.destroy();
    }
    function onSocketClose() {
      const r = socket._httpMessage;
      if (!r) return;
      const currentRes = r.res;
      r.destroyed = true;
      if (currentRes) {
        if (!currentRes.complete) currentRes.destroy(ConnResetException("aborted"));
        if (!r._closed) { r._closed = true; r.emit("close"); }
        if (!currentRes.aborted && currentRes.readable) currentRes.push(null);
      } else {
        if (!socket._hadError) {
          socket._hadError = true;
          emitErrorEvent(r, ConnResetException("socket hang up"));
        }
        if (!r._closed) { r._closed = true; r.emit("close"); }
      }
      if (r.outputData) r.outputData.length = 0;
    }

    socket.on("data", onData);
    socket.on("end", onEnd);
    socket.on("close", onSocketClose);
    socket.on("drain", ondrain);
    if (request.timeout !== undefined ||
        (request.agent && request.agent.options && request.agent.options.timeout)) {
      listenSocketTimeout(request);
    }
    request.emit("socket", socket);
  }

  ClientRequest.prototype.onSocket = function onSocket(socket, err) {
    if (socket && !err) {
      socket._httpMessage = this;
      socket.on("error", socketErrorListener);
    }
    nextTick(onSocketNT, this, socket, err);
  };
  function onSocketNT(request, socket, err) {
    if (request.destroyed || err) {
      request.destroyed = true;
      const _destroy = (e) => {
        if (!request.aborted && !e) e = ConnResetException("socket hang up");
        if (e && !(socket && socket._hadError)) emitErrorEvent(request, e);
        request._closed = true;
        request.emit("close");
      };
      if (socket) {
        if (!err && request.agent && !socket.destroyed) {
          socket.emit("free");
          socket.removeListener("error", socketErrorListener);
        } else {
          socket.destroy(err || request[kError]);
          _destroy(err || request[kError]);
          return;
        }
      }
      _destroy(err || request[kError]);
    } else {
      tickOnSocket(request, socket);
      request._flush();
    }
  }

  ClientRequest.prototype._deferToConnect = function _deferToConnect(method, args) {
    const callSocketMethod = () => {
      if (method && this.socket && typeof this.socket[method] === "function") {
        this.socket[method].apply(this.socket, args);
      }
    };
    const onSock = () => {
      if (this.socket.writable) callSocketMethod();
      else this.socket.once("connect", callSocketMethod);
    };
    if (!this.socket) this.once("socket", onSock);
    else onSock();
  };
  ClientRequest.prototype.setTimeout = function setTimeout(msecs, callback) {
    if (this._ended) return this;
    listenSocketTimeout(this);
    msecs = getTimerDuration(msecs, "msecs");
    if (callback) this.once("timeout", callback);
    const setSocketTimeout = (sock) => {
      if (sock.connecting) sock.once("connect", () => sock.setTimeout(msecs));
      else sock.setTimeout(msecs);
    };
    if (this.socket) setSocketTimeout(this.socket);
    else this.once("socket", setSocketTimeout);
    return this;
  };
  ClientRequest.prototype.setNoDelay = function setNoDelay(noDelay) {
    this._deferToConnect("setNoDelay", [noDelay]);
  };
  ClientRequest.prototype.setSocketKeepAlive = function setSocketKeepAlive(enable, initialDelay) {
    this._deferToConnect("setKeepAlive", [enable, initialDelay]);
  };
  ClientRequest.prototype.clearTimeout = function clearTimeoutFn(cb) {
    this.setTimeout(0, cb);
  };

  const globalAgent = new Agent({ keepAlive: true, keepAliveMsecs: 5000, timeout: 5000, scheduling: "lifo" });
  ClientRequest.prototype._defaultAgent = globalAgent;

  // -------------------------------------------------- Server (shape only)
  // js_net.cppm replaces this with the transport-capable server; the shape here
  // keeps `require("http").Server` usable before kNetJS runs.
  class Server extends EventEmitter {
    constructor(options, requestListener) {
      if (typeof options === "function") { requestListener = options; options = {}; }
      super();
      this.options = options || {};
      this.timeout = 0;
      this.keepAliveTimeout = 5000;
      this.headersTimeout = 60000;
      this.requestTimeout = 300000;
      this.maxHeadersCount = null;
      this.maxRequestsPerSocket = 0;
      if (typeof requestListener === "function") this.on("request", requestListener);
    }
  }
  Server.prototype.setTimeout = function (msecs, cb) {
    this.timeout = msecs;
    if (typeof cb === "function") this.on("timeout", cb);
    return this;
  };
  Server.prototype.closeAllConnections = function () {};
  Server.prototype.closeIdleConnections = function () {};

  // -------------------------------------------------- parser free list
  // node lib/internal/freelist.js FreeList, instantiated by _http_common.js as
  // `new FreeList('parsers', 1000, parsersCb)`. `http.setMaxIdleHTTPParsers`
  // writes `parsers.max` and test-http-set-max-idle-http-parser reads it back,
  // so this has to be the same object both sides see, not a private copy.
  const parsersFreeList = {
    name: "parsers",
    max: 1000,
    list: [],
    get length() { return this.list.length; },
    alloc() {
      const p = this.list.length ? this.list.pop() : null;
      if (p) return p;
      // _http_common is intentionally lazy: tests may replace its parser
      // binding before this allocator is first reached.
      const common = M["_http_common"] || M["node:_http_common"];
      const HP = (common && common.HTTPParser) || G.__mbunHttpParser;
      return HP ? new HP(false) : null;
    },
    free(obj) {
      if (this.list.length < this.max) { this.list.push(obj); return true; }
      return false;
    },
  };
  // node lib/_http_common.js kConnectionsCheckingInterval — the server's
  // headersTimeout/requestTimeout sweeper handle, asserted directly by
  // test-http-server-{clear-timer,async-dispose,close-destroy-timeout}.
  const kConnectionsCheckingInterval = Symbol("http.server.connectionsCheckingInterval");
  const kServerResponse = Symbol("ServerResponse");
  const kServerResponseStatistics = Symbol("ServerResponseStatistics");
  const kIncomingMessage = Symbol("IncomingMessage");

  // node lib/_http_common.js freeParser + clearIncoming. `parser.incoming` is
  // what keeps a finished IncomingMessage alive; clearing it is observable
  // (test-http-server-keepalive-end asserts parser.incoming === req inside the
  // request's own 'end' handler and null on the next tick, which only works if
  // the clear is *deferred to a later 'end' listener* rather than done eagerly).
  function clearIncoming(reqArg) {
    const r = reqArg || this;
    const sock = r && r.socket;
    const parser = sock && sock.parser;
    if (parser && parser.incoming === r) {
      if (r.readableEnded) {
        parser.incoming = null;
        r.parser = null;
      } else {
        r.on("end", clearIncoming);
      }
    }
  }
  function freeParser(parser, reqArg, socket) {
    if (parser && !parser._freed) {
      parser._freed = true;
      parser.incoming = null;
      parser.outgoing = null;
      // Drop everything the parser was holding before it is parked: the free
      // list is process-wide, so a retained body buffer or closure would be a
      // per-connection leak (test-http-parser-memory-retention watches for it).
      try {
        parser.onHead = parser.onBody = parser.onDone = parser.onError = parser.onInterim = null;
        parser._afterDone = null;
        parser.socket = null;
        parser.buf = new Uint8Array(0);
        parser.off = 0;
        parser.headers = {}; parser.rawHeaders = []; parser.trailers = {}; parser.rawTrailers = [];
      } catch (e) {}
      if (parsersFreeList.free(parser) === false) {
        nextTick(() => { if (typeof parser.close === "function") parser.close(); });
      } else if (typeof parser.free === "function") {
        // A user-installed `parser.free` is exactly the hook
        // test-http-server-connection-list-when-close overwrites to observe
        // that the parser left the server's connection list.
        parser.free();
      }
    }
    if (reqArg) reqArg.parser = null;
    if (socket) socket.parser = null;
  }

  // -------------------------------------------------- module assembly
  function makeExports(AgentClass, defaultGlobalAgent) {
    const createServer = (options, listener) => new Server(options, listener);
    const request = function (url, options, cb) { return new ClientRequest(url, options, cb); };
    const get = function (url, options, cb) {
      const r = request(url, options, cb);
      r.end();
      return r;
    };
    const exp = {
      METHODS, STATUS_CODES,
      Agent: AgentClass,
      Server, ServerResponse, IncomingMessage, OutgoingMessage, ClientRequest,
      createServer, request, get,
      validateHeaderName, validateHeaderValue,
      setMaxIdleHTTPParsers(max) { validateInteger(max, "max", 1); parsersFreeList.max = max; },
      setGlobalProxyFromEnv() { return function restore() {}; },
      globalAgent: defaultGlobalAgent,
      // Process-wide, read by every server per request (bun src/js/node/http.ts
      // get/setMaxHTTPHeaderSize -> bun_http::max_http_header_size).
      get maxHeaderSize() { return G.__mbunHttpNative.getMaxHeaderSize(); },
      set maxHeaderSize(v) { G.__mbunHttpNative.setMaxHeaderSize(v); },
      // bun src/js/node/http.ts:16 destructures these off globalThis at module
      // load; there they are native globals that always already exist. This
      // partition is appended before the ones that install WebSocket and the
      // *Event classes, so an eager snapshot would freeze `undefined` in. Read
      // through to the globals lazily instead -- same observable exports.
      get WebSocket() { return G.WebSocket; },
      get CloseEvent() { return G.CloseEvent; },
      get MessageEvent() { return G.MessageEvent; },
    };
    return exp;
  }

  const httpExports = makeExports(Agent, globalAgent);
  M["http"] = M["node:http"] = httpExports;

  // -------------------------------------------------- node:https mirror
  class HttpsAgent extends Agent {
    constructor(options) {
      super(options);
      this.defaultPort = 443;
      this.protocol = "https:";
      this.maxCachedSessions = (options && options.maxCachedSessions) || 100;
    }
  }
  const httpsGlobalAgent = new HttpsAgent({ keepAlive: true, keepAliveMsecs: 5000, timeout: 5000, scheduling: "lifo" });
  const httpsExports = makeExports(HttpsAgent, httpsGlobalAgent);
  httpsExports.request = function (url, options, cb) {
    if (typeof options === "function") { cb = options; options = undefined; }
    const merged = options ? { ...options } : {};
    if (merged.agent === undefined && typeof merged.createConnection !== "function") {
      merged.agent = httpsGlobalAgent;
    }
    if (!merged.protocol) merged.protocol = "https:";
    merged._defaultAgent = httpsGlobalAgent;
    return new ClientRequest(url, merged, cb);
  };
  httpsExports.get = function (url, options, cb) {
    const r = httpsExports.request(url, options, cb);
    r.end();
    return r;
  };
  M["https"] = M["node:https"] = httpsExports;

  // node's `_http_common` is what lib/internal/** reaches for when it needs the
  // RFC 7230 token/field-value validators — `internal/http2/util.js` opens with
  // `const { _checkIsHttpToken: checkIsHttpToken } = require('_http_common')`
  // and then calls it for every outgoing header name. The bootstrap stub had
  // only HTTPParser/methods, so the destructured binding was `undefined` and
  // node's own util.js died mid-header-build instead of at load, which read as
  // an unrelated TypeError in 20+ http2 corpus files.
  {
    const hc = M["_http_common"] || (M["_http_common"] = {});
    hc._checkIsHttpToken = hc.checkIsHttpToken = checkIsHttpToken;
    hc._checkInvalidHeaderChar = hc.checkInvalidHeaderChar = checkInvalidHeaderChar;
    hc.chunkExpression = chunkExpression;
    hc.parsers = parsersFreeList;
    hc.freeParser = freeParser;
    hc.prepareError = function (err) { err.rawPacket = err.rawPacket || undefined; };
    hc.kIncomingMessage = kIncomingMessage;
    hc.methods = hc.methods || METHODS;
    M["node:_http_common"] = hc;
  }

  // node splits http across `_http_agent`, `_http_client`, `_http_incoming`,
  // `_http_outgoing` and `_http_server`; its own tests require those directly
  // (`const { Agent } = require('_http_agent')`,
  // `const { kConnectionsCheckingInterval } = require('_http_server')`), and the
  // bootstrap registered them as empty objects, so the destructured binding was
  // `undefined` and `new Agent(...)` died as "not a constructor". Publish the
  // real objects. `Server`/`ServerResponse` are read through `M["http"]` because
  // js_net.cppm replaces them later with the transport-capable versions.
  {
    const pub = (name, obj) => { M[name] = M["node:" + name] = obj; };
    const httpMod = () => M["http"] || M["node:http"] || {};
    pub("_http_agent", { Agent, globalAgent });
    pub("_http_client", { ClientRequest, get parsers() { return parsersFreeList; } });
    pub("_http_incoming", {
      IncomingMessage,
      readStart(socket) { if (socket && !socket._paused && socket.readable) socket.resume(); },
      readStop(socket) { if (socket) socket.pause(); },
    });
    pub("_http_outgoing", {
      OutgoingMessage, kOutHeaders, kHighWaterMark, kUniqueHeaders,
      parseUniqueHeadersOption, validateHeaderName, validateHeaderValue,
    });
    pub("_http_server", {
      STATUS_CODES,
      get Server() { return httpMod().Server; },
      get ServerResponse() { return httpMod().ServerResponse; },
      kServerResponse, kServerResponseStatistics, kConnectionsCheckingInterval,
      storeHTTPOptions() {},
      setupConnectionsTracking() {},
      httpServerPreClose(server) { if (server && typeof server.closeIdleConnections === "function") server.closeIdleConnections(); },
      _connectionListener() {},
    });
  }

  // Shared with js_net.cppm (which owns the server transport) and with the
  // internal/http shim: one process, one set of these symbols and classes.
  G.__mbunHttpInternals = {
    kOutHeaders, kNeedDrain, kSocket, kCorked, kHighWaterMark, kUniqueHeaders,
    kHeaders, kHeadersCount, kTrailers, kTrailersCount,
    OutgoingMessage, ServerResponse, IncomingMessage, ClientRequest, Agent,
    STATUS_CODES, utcDate, chunkExpression,
    validateHeaderName, validateHeaderValue, checkIsHttpToken, checkInvalidHeaderChar,
    ERR_HTTP_HEADERS_SENT, ERR_INVALID_ARG_TYPE, ERR_INVALID_ARG_VALUE,
    ERR_HTTP_INVALID_STATUS_CODE, ERR_INVALID_CHAR, ERR_OUT_OF_RANGE,
    validateInteger, validateNumber, validateBoolean, validateObject, validateString,
    getTimerDuration,
    kConnectionsCheckingInterval, kServerResponse, kIncomingMessage, kLenientHeaders,
    parsersFreeList, freeParser, clearIncoming,
    // js_net's server transport needs node's exact abort error for
    // socketOnClose -> abortIncoming/abortOutgoing.
    ConnResetException,
  };
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
