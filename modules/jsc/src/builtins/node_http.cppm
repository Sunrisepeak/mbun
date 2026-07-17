// node:http + node:https JS layer partition.
//
// Provides the testable, network-independent surface of Node's http stack:
//   * constants: METHODS (full llhttp method list), STATUS_CODES
//   * validators: validateHeaderName / validateHeaderValue (+ checkIsHttpToken,
//     checkInvalidHeaderChar) matching lib/_http_common.js token/char rules
//   * class shapes: OutgoingMessage (header store), ServerResponse, ClientRequest,
//     IncomingMessage, Agent / globalAgent, Server
//   * request() / get() constructing a ClientRequest, createServer()
//   * maxHeaderSize get/set, setMaxIdleHTTPParsers, header-head parse helper
//   * node:https mirror (Agent defaultPort 443, its own globalAgent)
//
// REAL SOCKET TRANSPORT IS DEFERRED: request()/ClientRequest do not open a
// connection and Server.listen does not accept live sockets. The transport is
// inert (no fabricated responses/errors) — object shapes are correct so shape,
// constant, and validation assertions pass; a real round-trip does not occur.
//
// NOTE: appended AFTER the master builtins IIFE (opened in bootstrap, closed by
// image_closure), so this is a self-contained IIFE that re-binds G = globalThis
// and overwrites the pure-JS http/https stubs registered in bootstrap. It must
// not rely on the outer IIFE's aliases.
//
// Blueprint: bun src/js/node/http.ts, https.ts, _http_common.ts, _http_outgoing.ts,
// _http_incoming.ts, _http_server.ts, _http_client.ts, _http_agent.ts,
// internal/http.ts, internal/validators.ts.
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
  const netMod = req("net");
  const EventEmitter = M["events"] || M["node:events"] || netMod.Server;
  const Readable = streamMod.Readable;
  const Writable = streamMod.Writable;
  const NetServer = netMod.Server;
  const NetSocket = netMod.Socket;
  if (!Readable || !Writable || !EventEmitter) return;

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
  const checkIsHttpToken = (val) => tokenRegExp.exec(val) !== null;
  const checkInvalidHeaderChar = (val) => headerCharRegex.exec(String(val)) !== null;
  const invalidTokenError = (label, value) => {
    const e = new TypeError(
      (label || "Header name") + " must be a valid HTTP token [\"" + value + "\"]");
    e.code = "ERR_INVALID_HTTP_TOKEN";
    return e;
  };
  const validateHeaderName = (name, label) => {
    if (typeof name !== "string" || !name || !checkIsHttpToken(name)) {
      throw invalidTokenError(label, name);
    }
  };
  const validateHeaderValue = (name, value) => {
    if (value === undefined) {
      const e = new TypeError('Invalid value "' + value + '" for header "' + name + '"');
      e.code = "ERR_HTTP_INVALID_HEADER_VALUE";
      throw e;
    }
    if (checkInvalidHeaderChar(value)) {
      const e = new TypeError('Invalid character in header content ["' + name + '"]');
      e.code = "ERR_INVALID_CHAR";
      throw e;
    }
  };

  // -------------------------------------------------- OutgoingMessage
  // Header store: this[kOut] maps lowercased name -> [originalName, value].
  const kOut = "_headers";
  class OutgoingMessage extends Writable {
    constructor() {
      super();
      this[kOut] = null;
      this._header = null;
      this.finished = false;
      this.sendDate = true;
      this.chunkedEncoding = false;
      this.shouldKeepAlive = true;
      this.useChunkedEncodingByDefault = true;
      this.outputData = [];
      this.outputSize = 0;
      this._removedConnection = false;
      this._removedContLen = false;
      this._removedTE = false;
      this._contentLength = null;
      this.writable = true;
    }
  }

  Object.defineProperty(OutgoingMessage.prototype, "headersSent",
    { get() { return this._header !== null && this._header !== undefined; }, configurable: true });
  Object.defineProperty(OutgoingMessage.prototype, "writableEnded",
    { get() { return this.finished; }, configurable: true });

  OutgoingMessage.prototype.setHeader = function (name, value) {
    validateHeaderName(name);
    validateHeaderValue(name, value);
    let headers = this[kOut];
    if (headers === null) this[kOut] = headers = { __proto__: null };
    headers[name.toLowerCase()] = [name, value];
    return this;
  };
  OutgoingMessage.prototype.appendHeader = function (name, value) {
    validateHeaderName(name);
    let headers = this[kOut];
    if (headers === null) this[kOut] = headers = { __proto__: null };
    const key = name.toLowerCase();
    const cur = headers[key];
    if (cur === undefined) {
      validateHeaderValue(name, value);
      headers[key] = [name, value];
      return this;
    }
    const list = Array.isArray(cur[1]) ? cur[1] : [cur[1]];
    headers[key] = [cur[0], list.concat(value)];
    return this;
  };
  OutgoingMessage.prototype.setHeaders = function (headers) {
    if (!headers || typeof headers !== "object") {
      const e = new TypeError('The "headers" argument must be of type object.');
      e.code = "ERR_INVALID_ARG_TYPE";
      throw e;
    }
    if (typeof headers.entries === "function") {
      for (const [k, v] of headers.entries()) this.setHeader(k, v);
    } else {
      for (const k of Object.keys(headers)) this.setHeader(k, headers[k]);
    }
    return this;
  };
  OutgoingMessage.prototype.getHeader = function (name) {
    const headers = this[kOut];
    if (headers === null) return undefined;
    const entry = headers[String(name).toLowerCase()];
    return entry === undefined ? undefined : entry[1];
  };
  OutgoingMessage.prototype.getHeaders = function () {
    const headers = this[kOut];
    const ret = { __proto__: null };
    if (headers !== null) {
      for (const k of Object.keys(headers)) ret[k] = headers[k][1];
    }
    return ret;
  };
  OutgoingMessage.prototype.getHeaderNames = function () {
    return this[kOut] === null ? [] : Object.keys(this[kOut]);
  };
  OutgoingMessage.prototype.getRawHeaderNames = function () {
    const headers = this[kOut];
    if (headers === null) return [];
    return Object.keys(headers).map((k) => headers[k][0]);
  };
  OutgoingMessage.prototype.hasHeader = function (name) {
    const headers = this[kOut];
    return headers !== null && headers[String(name).toLowerCase()] !== undefined;
  };
  OutgoingMessage.prototype.removeHeader = function (name) {
    const headers = this[kOut];
    if (headers !== null) delete headers[String(name).toLowerCase()];
    return this;
  };
  OutgoingMessage.prototype.flushHeaders = function () {
    if (!this._header) this._header = "";
    return this;
  };
  OutgoingMessage.prototype.setTimeout = function (msecs, cb) {
    if (typeof cb === "function") this.on("timeout", cb);
    return this;
  };
  OutgoingMessage.prototype.addTrailers = function () { return this; };
  OutgoingMessage.prototype.cork = function () { return this; };
  OutgoingMessage.prototype.uncork = function () { return this; };
  OutgoingMessage.prototype._implicitHeader = function () {};

  // -------------------------------------------------- ServerResponse
  class ServerResponse extends OutgoingMessage {
    constructor(reqMsg, options) {
      super();
      this.req = reqMsg;
      this.statusCode = 200;
      this.statusMessage = undefined;
      this.sendDate = true;
      this._sent100 = false;
      this._expect_continue = false;
      this._hasBody = !(reqMsg && reqMsg.method === "HEAD");
    }
  }

  ServerResponse.prototype.writeHead = function (statusCode, reason, obj) {
    statusCode |= 0;
    if (statusCode < 100 || statusCode > 999) {
      const e = new RangeError('The value "' + statusCode + '" is invalid for option "statusCode"');
      e.code = "ERR_HTTP_INVALID_STATUS_CODE";
      throw e;
    }
    if (typeof reason === "string") this.statusMessage = reason;
    else { obj = reason; this.statusMessage = this.statusMessage || undefined; }
    this.statusCode = statusCode;
    if (obj) {
      if (Array.isArray(obj)) {
        for (let i = 0; i < obj.length; i += 2) this.setHeader(obj[i], obj[i + 1]);
      } else {
        for (const k of Object.keys(obj)) this.setHeader(k, obj[k]);
      }
    }
    this._header = "";
    return this;
  };
  ServerResponse.prototype.writeContinue = function (cb) {
    this._sent100 = true;
    if (typeof cb === "function") cb();
    return this;
  };
  ServerResponse.prototype.writeProcessing = function (cb) {
    if (typeof cb === "function") cb();
    return this;
  };
  ServerResponse.prototype.writeEarlyHints = function (hints, cb) {
    if (typeof cb === "function") cb();
    return this;
  };
  ServerResponse.prototype.assignSocket = function (socket) { this.socket = socket; };
  ServerResponse.prototype.detachSocket = function () { this.socket = null; };

  // -------------------------------------------------- IncomingMessage
  class IncomingMessage extends Readable {
    constructor(socket) {
      super();
      this.socket = socket || null;
      this.connection = this.socket;
      this.httpVersionMajor = 1;
      this.httpVersionMinor = 1;
      this.httpVersion = "1.1";
      this.complete = false;
      this.headers = { __proto__: null };
      this.headersDistinct = { __proto__: null };
      this.rawHeaders = [];
      this.trailers = { __proto__: null };
      this.trailersDistinct = { __proto__: null };
      this.rawTrailers = [];
      this.aborted = false;
      this.upgrade = null;
      this.url = "";
      this.method = null;
      this.statusCode = null;
      this.statusMessage = null;
    }
  }
  IncomingMessage.prototype.setTimeout = function (msecs, cb) {
    if (typeof cb === "function") this.on("timeout", cb);
    return this;
  };
  // Readable's pull side. Whoever drives the parser (js_net's createServer over
  // a real socket) pushes body bytes straight into the readable buffer, so all
  // this has to do is let a paused socket flow again — same reasoning as bun's
  // _http_incoming.ts:315 ("parserOnBody fills up our internal buffer directly.
  // However, we do need to unpause the underlying socket so that it flows").
  // Without a _read, Readable throws ERR_METHOD_NOT_IMPLEMENTED on first read().
  IncomingMessage.prototype._read = function (_n) {
    if (!this._consuming) {
      this._readableState.readingMore = false;
      this._consuming = true;
    }
    const socket = this.socket;
    if (socket && socket.readable) socket.resume();
  };
  IncomingMessage.prototype._addHeaderLines = function (list) {
    if (!Array.isArray(list)) return;
    for (let i = 0; i < list.length; i += 2) {
      const k = String(list[i]);
      const v = list[i + 1];
      this.rawHeaders.push(k, v);
      const lk = k.toLowerCase();
      if (this.headers[lk] === undefined) this.headers[lk] = v;
    }
  };

  // -------------------------------------------------- Agent (_http_agent)
  class Agent extends EventEmitter {
    constructor(options) {
      super();
      this.options = { __proto__: null, ...(options || {}) };
      if (this.options.noDelay === undefined) this.options.noDelay = true;
      this.keepAlive = this.options.keepAlive || false;
      this.keepAliveMsecs = this.options.keepAliveMsecs || 1000;
      this.maxSockets = this.options.maxSockets || Agent.defaultMaxSockets;
      this.maxFreeSockets = this.options.maxFreeSockets || 256;
      this.maxTotalSockets = this.options.maxTotalSockets || Infinity;
      this.scheduling = this.options.scheduling || "lifo";
      this.sockets = { __proto__: null };
      this.freeSockets = { __proto__: null };
      this.requests = { __proto__: null };
      this.totalSocketCount = 0;
      this.maxCachedSessions = 100;
      this.protocol = "http:";
      this.defaultPort = 80;
    }
  }
  Agent.defaultMaxSockets = Infinity;
  Agent.prototype.getName = function (options) {
    options = options || {};
    let name = (options.host || "localhost") + ":";
    if (options.port) name += options.port;
    name += ":";
    if (options.localAddress) name += options.localAddress;
    name += ":";
    if (options.family === 4 || options.family === 6) name += options.family;
    name += ":";
    return name;
  };
  Agent.prototype.addRequest = function () {};
  Agent.prototype.createConnection = function (options, cb) {
    const s = NetSocket ? new NetSocket(options) : null;
    if (typeof cb === "function") cb(null, s);
    return s;
  };
  Agent.prototype.keepSocketAlive = function () { return true; };
  Agent.prototype.reuseSocket = function () {};
  Agent.prototype.destroy = function () {};
  const globalAgent = new Agent({ keepAlive: false });

  // -------------------------------------------------- ClientRequest (_http_client)
  // Network transport is DEFERRED: the request is fully shaped and buffers
  // outgoing data, but never opens a socket, so no response/error is fabricated.
  function normalizeClientArgs(url, options, cb) {
    if (typeof url === "string") {
      let parsed;
      try { parsed = new URL(url); } catch (e) {
        const err = new TypeError("Invalid URL");
        err.code = "ERR_INVALID_URL";
        err.input = url;
        throw err;
      }
      url = parsed;
    }
    if (url && (url instanceof URL || (typeof url === "object" && typeof url.href === "string" && typeof url.protocol === "string"))) {
      const u = url;
      url = {
        protocol: u.protocol,
        hostname: typeof u.hostname === "string" ? u.hostname.replace(/^\[|\]$/g, "") : u.hostname,
        host: u.host,
        port: u.port,
        path: (u.pathname || "/") + (u.search || ""),
        href: u.href,
      };
      if (u.username || u.password) url.auth = u.username + ":" + u.password;
    }
    if (typeof options === "function") { cb = options; options = url; }
    else if (options == null) options = url;
    else options = { ...url, ...options };
    return [options || {}, cb];
  }

  // ---- request header serialization (_http_outgoing.ts _storeHeader) --------
  // The header block is built once, in one pass, while `state` records which
  // framing headers the user already supplied. Transfer-Encoding: chunked is
  // appended ONLY when the user set neither Content-Length nor Transfer-Encoding
  // (state.contLen/state.te) -- that guard is what keeps an explicit
  // "transfer-encoding: chunked" from being emitted twice.
  const RE_CONN_CLOSE = /(?:^|\W)close(?:$|\W)/i;
  const RE_TE_CHUNKED = /(?:^|\W)chunked(?:$|\W)/i;

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
        if (RE_TE_CHUNKED.test(value)) self.chunkedEncoding = true;
        break;
      case "content-length":
        state.contLen = true;
        self._contentLength = +value;
        self._removedContLen = false;
        break;
      case "date": state.date = true; break;
      case "expect": state.expect = true; break;
      case "trailer": state.trailer = true; break;
    }
  }
  function storeHeaderLine(self, state, key, value) {
    state.header += key + ": " + value + "\r\n";
    matchHeader(self, state, key, value);
  }
  function processHeader(self, state, key, value) {
    if (Array.isArray(value)) { for (const v of value) storeHeaderLine(self, state, key, v); }
    else storeHeaderLine(self, state, key, value);
  }
  function storeHeader(self, firstLine) {
    const state = { connection: false, contLen: false, te: false, date: false, expect: false, trailer: false, header: firstLine };
    const headers = self[kOut];
    if (headers) {
      for (const k of Object.keys(headers)) {
        const entry = headers[k];
        processHeader(self, state, entry[0], entry[1]);
      }
    }
    let header = state.header;
    if (self.sendDate && !state.date) header += "Date: " + new Date().toUTCString() + "\r\n";

    if (self._removedConnection) {
      self._last = !self.shouldKeepAlive;
    } else if (!state.connection) {
      const shouldSendKeepAlive =
        self.shouldKeepAlive && (state.contLen || self.useChunkedEncodingByDefault || self.agent);
      if (shouldSendKeepAlive) header += "Connection: keep-alive\r\n";
      else { self._last = true; header += "Connection: close\r\n"; }
    }

    if (!state.contLen && !state.te) {
      if (!self._hasBody) {
        self.chunkedEncoding = false;
      } else if (!self.useChunkedEncodingByDefault) {
        self._last = true;
      } else if (!state.trailer && !self._removedContLen && typeof self._contentLength === "number") {
        header += "Content-Length: " + self._contentLength + "\r\n";
      } else if (!self._removedTE) {
        header += "Transfer-Encoding: chunked\r\n";
        self.chunkedEncoding = true;
      } else {
        self._last = true;
      }
    }
    self._header = header + "\r\n";
    return self._header;
  }

  class ClientRequest extends OutgoingMessage {
    constructor(url, options, cb) {
      super();
      construct_ClientRequest(this, url, options, cb);
    }
  }
  function construct_ClientRequest(self, url, options, cb) {
    const [opts, callback] = normalizeClientArgs(url, options, cb);
    self.agent = opts.agent !== undefined ? opts.agent : (self._defaultAgent || globalAgent);
    self.protocol = opts.protocol || (self.agent && self.agent.protocol) || "http:";
    const defaultPort = self.protocol === "https:"
      ? 443
      : (self.agent && self.agent.defaultPort ? self.agent.defaultPort : 80);
    self.method = (opts.method ? String(opts.method) : "GET").toUpperCase();
    if (!checkIsHttpToken(self.method)) throw invalidTokenError("Method", self.method);
    self.path = opts.path || "/";
    self.host = opts.hostname || opts.host || "localhost";
    // node _http_client.js: a CR/LF in the connect host is invalid on every
    // path (Host header, request line, proxy CONNECT target) — throw
    // synchronously in the ctor even when an explicit Host header is given.
    if (/[\r\n]/.test(self.host)) {
      const e = new TypeError('Invalid character in header content ["Host"]');
      e.code = "ERR_INVALID_CHAR";
      throw e;
    }
    self.port = +opts.port || defaultPort;
    self.socketPath = opts.socketPath;
    self.aborted = false;
    self.destroyed = false;
    self.reusedSocket = false;
    self.maxHeadersCount = null;
    self.res = null;
    self._ended = false;
    self._closed = false;
    // A client never stamps Date (OutgoingMessage.sendDate is false in node;
    // only ServerResponse turns it on).
    self.sendDate = false;
    // Bodyless methods do not default to chunked (_http_client.ts).
    self.useChunkedEncodingByDefault =
      !(self.method === "GET" || self.method === "HEAD" || self.method === "DELETE"
        || self.method === "OPTIONS" || self.method === "TRACE" || self.method === "CONNECT");
    self._hasBody = true;
    self._last = false;
    if (opts.headers) {
      if (Array.isArray(opts.headers)) {
        for (let i = 0; i < opts.headers.length; i += 2) self.setHeader(opts.headers[i], opts.headers[i + 1]);
      } else {
        for (const k of Object.keys(opts.headers)) self.setHeader(k, opts.headers[k]);
      }
    }
    if (opts.auth && !self.hasHeader("authorization")) {
      self.setHeader("Authorization", "Basic " + G.Buffer.from(opts.auth).toString("base64"));
    }
    // Host defaults to the target authority, bracketing IPv6 and omitting the
    // port when it is the protocol default (_http_client.ts).
    if (opts.setHost !== false && !self.hasHeader("host")) {
      let hostHeader = self.host;
      const posColon = hostHeader.indexOf(":");
      if (posColon !== -1 && hostHeader.indexOf(":", posColon + 1) !== -1 && hostHeader.charCodeAt(0) !== 91) {
        hostHeader = "[" + hostHeader + "]";
      }
      if (self.port && +self.port !== defaultPort) hostHeader += ":" + self.port;
      // node _http_client.js: header-content validation on the synthesized Host
      // (checkInvalidHeaderChar) — CR/LF must throw synchronously in the ctor.
      if (/[\r\n]/.test(hostHeader)) {
        const e = new TypeError('Invalid character in header content ["Host"]');
        e.code = "ERR_INVALID_CHAR"; throw e;
      }
      self.setHeader("Host", hostHeader);
    }
    if (typeof callback === "function") self.once("response", callback);
    if (opts.timeout !== undefined) self.timeout = opts.timeout;
    // Do NOT freeze the header block here: node serializes it lazily at the
    // first body write / end / flush (_http_outgoing.ts _implicitHeader), so
    // setHeader() calls made after construction still reach the wire.
    // superagent/supertest construct first and set Content-Type/Content-Length
    // afterwards — an eager storeHeader silently dropped those headers.
    self._firstLine = self.method + " " + self.path + " HTTP/1.1\r\n";
    self._bodyWritten = false;
    clientConnect(self, opts);
  }

  // Lazily serialize the request head (node's _implicitHeader equivalent).
  function ensureClientHeader(self) {
    if (self._header == null) storeHeader(self, self._firstLine);
  }

  // Open the socket and stream the request through it. Nothing here rides an
  // Agent pool yet: every request gets its own connection (agent:false
  // semantics), which is what the pooling-free tests exercise.
  function clientConnect(self, opts) {
    const net = M["net"] || M["node:net"];
    if (!net || typeof net.connect !== "function") {
      queueMicrotask(() => self.emit("error", new Error("node:net is unavailable")));
      return;
    }
    self._pending = [];
    self._connected = false;
    const connOpts = self.socketPath
      ? { path: self.socketPath }
      : { host: self.host, port: self.port };
    const socket = net.connect(connOpts, () => {
      self._connected = true;
      self.emit("socket", socket);
      ensureClientHeader(self); // bodyless requests may connect before end()
      socket.write(self._header);
      for (const b of self._pending) socket.write(b);
      self._pending.length = 0;
      if (self._ended) clientFinishBody(self);
    });
    self.socket = self.connection = socket;
    clientAttachParser(self, socket);
    socket.on("error", (e) => { if (!self.destroyed) self.emit("error", e); });
    if (self.timeout !== undefined && socket.setTimeout) {
      socket.setTimeout(self.timeout, () => self.emit("timeout"));
    }
  }

  // Responses go through the same incremental HTTP/1.1 parser Bun.serve and
  // fetch use (js_net.cppm), surfaced as __mbunHttpParser.
  function clientAttachParser(self, socket) {
    const Parser = G.__mbunHttpParser;
    if (!Parser) return;
    const parser = new Parser(true);
    parser.reqMethod = self.method;
    let res = null;
    parser.onHead = () => {
      res = new IncomingMessage(socket);
      res.statusCode = parser.status;
      res.statusMessage = parser.statusText;
      res.httpVersion = parser.httpVersion;
      res._addHeaderLines(parser.rawHeaders);
      self.res = res;
      self.emit("response", res);
    };
    parser.onBody = (b) => { if (res) res.push(G.Buffer.from(b)); };
    parser.onDone = () => {
      if (!res) return;
      res.complete = true;
      res.push(null);
    };
    parser.onError = (e) => { if (!self.destroyed) self.emit("error", e); };
    socket.on("data", (chunk) => parser.push(new Uint8Array(chunk.buffer || chunk, chunk.byteOffset || 0, chunk.byteLength !== undefined ? chunk.byteLength : chunk.length)));
    socket.on("end", () => { try { parser.eof(); } catch (e) {} });
    socket.on("close", () => { self._closed = true; self.emit("close"); });
  }

  // Frame one body chunk: chunked bodies get their size prefix here, identity
  // bodies go out as-is (_http_outgoing.ts write_).
  function clientSendChunk(self, chunk, encoding) {
    const buf = typeof chunk === "string" ? G.Buffer.from(chunk, encoding || "utf8") : chunk;
    if (buf.length === 0 && self.chunkedEncoding) return;
    const framed = self.chunkedEncoding
      ? [G.Buffer.from(buf.length.toString(16) + "\r\n"), buf, G.Buffer.from("\r\n")]
      : [buf];
    for (const b of framed) {
      if (self._connected) self.socket.write(b);
      else self._pending.push(b);
    }
  }
  function clientFinishBody(self) {
    if (self.chunkedEncoding && self.socket) self.socket.write("0\r\n\r\n");
    self.emit("finish");
  }

  ClientRequest.prototype._defaultAgent = globalAgent;
  ClientRequest.prototype.write = function (chunk, encoding, cb) {
    if (typeof encoding === "function") { cb = encoding; encoding = undefined; }
    ensureClientHeader(this); // framing (chunked vs identity) freezes here
    if (chunk != null) { this._bodyWritten = true; clientSendChunk(this, chunk, encoding); }
    if (typeof cb === "function") cb();
    return true;
  };
  ClientRequest.prototype.end = function (chunk, encoding, cb) {
    if (typeof chunk === "function") { cb = chunk; chunk = null; }
    else if (typeof encoding === "function") { cb = encoding; encoding = undefined; }
    if (this._ended) return this;
    // Whole body handed to end() with no prior writes and no explicit
    // Content-Length/Transfer-Encoding: node sends identity framing with a
    // computed Content-Length instead of chunked (_http_outgoing.ts end()).
    if (this._header == null && chunk != null && !this._bodyWritten && this._hasBody
        && !this.hasHeader("content-length") && !this.hasHeader("transfer-encoding")) {
      this._contentLength = typeof chunk === "string"
        ? G.Buffer.byteLength(chunk, encoding || "utf8")
        : chunk.length;
    }
    ensureClientHeader(this);
    if (chunk != null) clientSendChunk(this, chunk, encoding);
    this.finished = true;
    this._ended = true;
    if (typeof cb === "function") this.once("finish", cb);
    if (this._connected) clientFinishBody(this);
    return this;
  };
  ClientRequest.prototype.abort = function () {
    if (this.aborted) return;
    this.aborted = true;
    this.destroyed = true;
    if (this.socket) { try { this.socket.destroy(); } catch (e) {} }
    this.emit("abort");
  };
  ClientRequest.prototype.destroy = function (err) {
    this.destroyed = true;
    if (this.socket) { try { this.socket.destroy(); } catch (e) {} }
    if (err) this.emit("error", err);
    return this;
  };
  ClientRequest.prototype.setTimeout = function (msecs, cb) {
    this.timeout = msecs;
    if (typeof cb === "function") this.once("timeout", cb);
    return this;
  };
  ClientRequest.prototype.setNoDelay = function () { return this; };
  ClientRequest.prototype.setSocketKeepAlive = function () { return this; };
  ClientRequest.prototype.flushHeaders = function () { this._header = ""; return this; };
  ClientRequest.prototype.getHeaders = OutgoingMessage.prototype.getHeaders;

  // -------------------------------------------------- Server (_http_server)
  const ServerBase = NetServer || EventEmitter;
  class Server extends ServerBase {
    constructor(options, requestListener) {
      if (typeof options === "function") { requestListener = options; options = {}; }
      super(options);
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

  // -------------------------------------------------- module assembly
  function makeExports(defaultProtocol, AgentClass, defaultGlobalAgent) {
    const createServer = (options, listener) => new Server(options, listener);
    const request = function (url, options, cb) {
      const r = new ClientRequest(url, options, cb);
      return r;
    };
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
      setMaxIdleHTTPParsers() {},
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
      // through to the globals lazily instead — same observable exports.
      get WebSocket() { return G.WebSocket; },
      get CloseEvent() { return G.CloseEvent; },
      get MessageEvent() { return G.MessageEvent; },
    };
    return exp;
  }

  const httpExports = makeExports("http:", Agent, globalAgent);
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
  const httpsGlobalAgent = new HttpsAgent({ keepAlive: false });

  const httpsExports = makeExports("https:", HttpsAgent, httpsGlobalAgent);
  // https.request/get inject the https agent + protocol so port defaults to 443.
  httpsExports.request = function (url, options, cb) {
    if (typeof options === "function") { cb = options; options = undefined; }
    const merged = options ? { ...options } : {};
    if (merged.agent === undefined) merged.agent = httpsGlobalAgent;
    if (!merged.protocol) merged.protocol = "https:";
    return new ClientRequest(url, merged, cb);
  };
  httpsExports.get = function (url, options, cb) {
    const r = httpsExports.request(url, options, cb);
    r.end();
    return r;
  };
  M["https"] = M["node:https"] = httpsExports;
  G.__httpDebug = {
    smOM: Object.getPrototypeOf(ServerResponse.prototype) === OutgoingMessage.prototype,
    imR: Object.getPrototypeOf(IncomingMessage.prototype) === Readable.prototype,
    crOM: Object.getPrototypeOf(ClientRequest.prototype) === OutgoingMessage.prototype,
    smCtor: ServerResponse.prototype.constructor === ServerResponse,
    smSuper: Object.getPrototypeOf(ServerResponse) === OutgoingMessage,
  };
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
