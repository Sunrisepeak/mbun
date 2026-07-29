// modules/jsc/src/js_http2_part2.cppm — module mbun.jsc.js_http2_part2
//
// Second half of js_http2.cppm's JS payload. Split only to stay under mcpp's
// ~256 KiB per-file limit, which reports a crossing as "index requires
// mcpp >= 0.0.108" without naming the file or the size. The seam is a line
// boundary; kHttp2JS concatenates the parts before evaluation, so the JS is
// byte-identical to before the split.
export module mbun.jsc.js_http2_part2;

import std;

namespace mbun::jsc::js_http2 {

export constexpr std::string_view kHttp2JS_part2 = R"JS(

  function connect(authority, options, listener) {
    if (typeof options === "function") { listener = options; options = undefined; }
    return new ClientHttp2Session(authority, options || {}, listener);
  }
  // node lib/internal/http2/core.js "Support util.promisify": connect()'s
  // listener is called with the SESSION, not node's (err, value) callback
  // shape, so a plain promisify would reject with the session. node ships a
  // promisify.custom that resolves with it (and rejects on 'error') -- without
  // it `util.promisify(http2.connect)(...)` never settled and the test hung.
  // Symbol.for("nodejs.util.promisify.custom") is the registered symbol
  // util.promisify looks up, so this needs no require of node:util here (which
  // is not loadable yet at this module's own evaluation time).
  Object.defineProperty(connect, Symbol.for("nodejs.util.promisify.custom"), {
    value: function (authority, options) {
      return new Promise((resolve, reject) => {
        const session = connect(authority, options, () => {
          session.removeListener("error", reject);
          resolve(session);
        });
        session.once("error", reject);
      });
    },
    configurable: true,
  });

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
    // node's ERR_INVALID_ARG_VALUE reports inspect(value) — NOT
    // determineSpecificType: "Received true", not "Received type boolean (true)".
    const NE = globalThis.__mbunNodeErrors;
    if (NE) return NE.ERR_INVALID_ARG_VALUE(name, value, reason);
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
        // RFC 9113 8.3.1 / nghttp2 http_request_on_header: a REQUEST may carry
        // only the request pseudo-headers. `:status` is a response one and
        // anything else is undefined; both are a malformed request. Accepting
        // them opened a normal stream for a header block that should have been
        // rejected outright.
        if (name !== ":method" && name !== ":scheme" && name !== ":path" &&
            name !== ":authority" && name !== ":protocol") return true;
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
    if (value < 0 || value > 4294967295) throw outOfRangeErr(name, ">= 0 && <= 4294967295", value);
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
      const infoBuilt = buildNgHeaders(obj, assertValidResponsePseudoHeader, strictSingleValueFields, true);
      infoBuilt.prepared = Object.assign({ __proto__: null }, obj);
      if (infoBuilt.prepared[":status"] !== undefined) infoBuilt.prepared[":status"] = parseInt(infoBuilt.prepared[":status"], 10);
      return infoBuilt;
    }
    // node validatePreparedResponseHeaders is deliberately stricter than HTTP/1:
    // a response status outside 200..599 is rejected outright.
    const status = (obj[":status"] | 0) || 200;
    if (status < 200 || status > 599) { const e = new RangeError("Invalid status code: " + status); e.code = "ERR_HTTP2_STATUS_INVALID"; throw e; }
    obj[":status"] = String(status);
    const built = buildNgHeaders(obj, assertValidResponsePseudoHeader, strictSingleValueFields, true);
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
    const built = buildNgHeaders(headers, assertValidResponsePseudoHeader, strictSingleValueFields, true);
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
      // Order matters: node checks `destroyed || closed` before headersSent
      // (lib/internal/http2/core.js Http2ServerStream#respond), so respond()
      // on a destroyed stream that had already responded is
      // ERR_HTTP2_INVALID_STREAM, not ERR_HTTP2_HEADERS_SENT.
      if (this.destroyed || this._closed) throw mkErr("The stream has been destroyed", "ERR_HTTP2_INVALID_STREAM");
      if (this.headersSent) throw mkErr("Response has already been initiated.", "ERR_HTTP2_HEADERS_SENT");
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
      if (!submitNativeStream(this, "respond", built.list, options)) return;
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
      // node ServerHttp2Stream#respond: 'finish' fires once the response HEADERS
      // have been submitted (never when the submit itself failed).
      if (onServerStreamFinishChannel.hasSubscribers) onServerStreamFinishChannel.publish({ stream: this, headers: this.sentHeaders, flags });
      if (endStream) { this.end(); http2StreamFinish(this); }
      return;
    }
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
      // Validate BEFORE marking sent: a pseudo-header in a trailer block is an
      // ERR_HTTP2_INVALID_PSEUDOHEADER and must leave the stream retryable.
      for (const k in headers) { if (Object.prototype.hasOwnProperty.call(headers, k) && String(k)[0] === ":") throw mkErr('"' + k + '" is an invalid pseudoheader or is used incorrectly', "ERR_HTTP2_INVALID_PSEUDOHEADER"); }
      this._trailersSent = true;
      this.sentTrailers = headers;
      const sensitive = sensitiveNamesOf(headers);
      const list = [];
      for (const k in headers) { if (!Object.prototype.hasOwnProperty.call(headers, k)) continue; const lk = String(k).toLowerCase(); list.push([lk, String(headers[k])]); }
      // node Http2Stream::SubmitTrailers: an EMPTY trailer set is not a HEADERS
      // frame at all -- it resumes the data provider, so the stream ends with an
      // empty DATA(END_STREAM). Emitting a real (if empty) trailing HEADERS
      // block instead made the peer raise a spurious 'trailers' event, which is
      // what test-http2-no-wanttrailers-listener asserts must never happen.
      if (list.length === 0) {
        this.session._sendData(this, Buffer.alloc(0), true);
      } else {
        const block = encodeHeaders(list, sensitive);
        writeHeaderBlock(this.session, this.id, block, FLAG.END_STREAM);
      }
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
      // node passes ['number','FileHandle'] to ERR_INVALID_ARG_TYPE, which reads
      // "of type number or an instance of FileHandle" — FileHandle is a class,
      // not a second primitive type.
      if (typeof fd !== "number") {
        const NE = globalThis.__mbunNodeErrors;
        if (NE) throw NE.ERR_INVALID_ARG_TYPE("fd", ["number", "FileHandle"], fd);
        throw argTypeErr("fd", "of type number or an instance of FileHandle", fd);
      }
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
      else {
        // A filesystem failure belongs to the HTTP/2 stream boundary, not the
        // public fs API: node closes both peers with INTERNAL_ERROR. Preserve
        // the HTTP/2-specific errors made above (directory/non-seekable file),
        // but translate raw errno values such as EBADF before destroy() emits
        // the server-side error and sends RST_STREAM to the client.
        const isHttp2Error = err && typeof err.code === "string" && err.code.indexOf("ERR_HTTP2_") === 0;
        this.destroy(isHttp2Error ? err : streamErr(constants.NGHTTP2_INTERNAL_ERROR));
      }
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
      // Submit HEADERS before the file source is consumed. A later read error
      // resets an already-open response stream, so the client observes both the
      // response event and the INTERNAL_ERROR RST, matching node/nghttp2.
      this.respond(headers, options.waitForTrailers ? { waitForTrailers: true } : undefined);
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
      // node ServerHttp2Stream#pushStream publishes 'created' synchronously and
      // defers 'start' to the tick that invokes the callback, so createdTime is
      // strictly before startTime here too.
      if (onServerStreamCreatedChannel.hasSubscribers) onServerStreamCreatedChannel.publish({ stream: push, headers: h });
      G.queueMicrotask(() => {
        if (onServerStreamStartChannel.hasSubscribers) onServerStreamStartChannel.publish({ stream: push, headers: h });
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
      if (!submitNativeStream(this, "info", built.list)) return;
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
      // RFC 9113 6.5.2: a SETTINGS_ENABLE_PUSH value other than 0 sent by a
      // server MUST be treated as a connection error by the client. Node's
      // server half hard-wires it off, so a caller-supplied `enablePush: true`
      // is overridden rather than advertised.
      // Only CLAMP a caller-supplied enablePush; never inject one. Injecting it
      // made the server's initial SETTINGS 6 bytes where node sends an EMPTY
      // frame, which broke node's test-http2-settings-unsolicited-ack.js (it
      // deep-equals the raw frame). An empty frame already satisfies bun's
      // regression/29073 requirement of never advertising a non-zero value.
      this._isServerSession = true;
      const _srvSettings = server && server._h2options && server._h2options.settings;
      this._writeFrame(FRAME.SETTINGS, 0, 0, encodeSettings(
        _srvSettings && "enablePush" in _srvSettings
          ? Object.assign({}, _srvSettings, { enablePush: false })
          : _srvSettings));
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
    // node Http2Session[EventEmitter.captureRejectionSymbol] (shared by both
    // halves): a rejected 'stream' listener kills that stream, anything else
    // kills the session.
    [kRejection](err, event, ...args) {
      if (event === "stream" && args[0] && typeof args[0].destroy === "function") args[0].destroy(err);
      else this.destroy(err);
    }
    // Server-initiated (push) streams use even ids starting at 2 (RFC 9113 5.1.1).
    _nextPushId() { this._lastPushId = (this._lastPushId || 0) + 2; return this._lastPushId > 2147483647 ? 0 : this._lastPushId; }
    _windowUpdate(streamId, increment) { if (streamId === 0) this._localWindow += increment; const p = Buffer.alloc(4); p.writeUInt32BE(increment >>> 0, 0); this._writeFrame(FRAME.WINDOW_UPDATE, 0, streamId, p); }
    _strictWs() { return this._options.strictFieldWhitespaceValidation !== false; }
    setLocalWindowSize(windowSize) { sessionSetLocalWindowSize(this, windowSize); }

    _onData(chunk) {
      if (this.destroyed) return;
      if (this._timeoutMs) this._armTimeout();   // inbound activity resets the idle timer
      this._recv = this._recv.length ? Buffer.concat([this._recv, chunk]) : Buffer.from(chunk);
      if (!this._prefaceRead) {
        if (this._recv.length < 24) return;
        // -903 = NGHTTP2_ERR_BAD_CLIENT_MAGIC, "Received bad client magic byte
        // string": what an HTTP/1 client talking to an http2 server produces.
        for (let i = 0; i < 24; i++) { if (this._recv[i] !== CLIENT_PREFACE[i]) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR, -903); return; } }
        this._prefaceRead = true;
        this._recv = this._recv.subarray(24);
      }
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
          } else if (!resolvePing(this, payload)) {
            // A PING ACK nobody asked for. nghttp2 terminates the session, which
            // is what stops a peer from using unmatched ACKs as a flood.
            this._connError(constants.NGHTTP2_PROTOCOL_ERROR);
            return false;
          }
          return true;
        }
        case FRAME.WINDOW_UPDATE: {
          if (len !== 4) { this._connError(constants.NGHTTP2_FRAME_SIZE_ERROR); return false; }
          const inc = payload.readUInt32BE(0) & 0x7fffffff;
          if (inc === 0 && streamId === 0) { this._connError(constants.NGHTTP2_PROTOCOL_ERROR); return false; }  // §6.9
          if (streamId === 0) {
            // RFC 9113 6.9.1: a flow-control window that would grow past
            // 2^31-1 is a connection-level FLOW_CONTROL_ERROR. Without this the
            // session stayed alive but unusable after the overflow.
            if (this._remoteWindow + inc > 2147483647) { this._connError(constants.NGHTTP2_FLOW_CONTROL_ERROR); return false; }
            this._remoteWindow += inc;
          }
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
      // node onStreamHeaders (server): 'created' then 'start', both before the
      // application sees the 'stream' event. The order is observable —
      // test-diagnostics-channel-http2-server-stream-created-start-timing
      // asserts createdTime < startTime for every stream.
      if (onServerStreamCreatedChannel.hasSubscribers) onServerStreamCreatedChannel.publish({ stream, headers: headersObj });
      if (onServerStreamStartChannel.hasSubscribers) onServerStreamStartChannel.publish({ stream, headers: headersObj });
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
      // node maxSessionInvalidFrames (nghttp2's on_invalid_frame_recv budget,
      // default 1000): a peer that keeps sending frames the server has to
      // reject is torn down instead of being RST'd forever. Without the budget
      // a client generating invalid streams in a loop kept the session -- and
      // the event loop -- alive indefinitely.
      const maxInvalid = this._options && this._options.maxSessionInvalidFrames !== undefined
        ? this._options.maxSessionInvalidFrames : 1000;
      this._invalidFrames = (this._invalidFrames | 0) + 1;
      if (maxInvalid > 0 && this._invalidFrames >= maxInvalid) this._connError(constants.NGHTTP2_PROTOCOL_ERROR);
    }
    // See the client session's _connError: a locally detected violation is
    // ERR_HTTP2_ERROR (nghttp2_strerror), never ERR_HTTP2_SESSION_ERROR.
    _connError(code, errno) {
      const p = Buffer.alloc(8); p.writeUInt32BE(this._lastStreamId > 0 ? this._lastStreamId : 0, 0); p.writeUInt32BE(code >>> 0, 4);
      try { this._writeFrame(FRAME.GOAWAY, 0, 0, p); } catch (e) {}
      const err = nghttpErr(errno === undefined ? NGHTTP2_ERR_PROTO : errno);
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
      if (this._timer != null) { try { G.clearTimeout(this._timer); } catch (e) {} this._timer = null; }
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
      // node Http2Session#goaway: a destroyed session cannot emit any frame.
      if (this.destroyed) throw mkErr("The session has been destroyed", "ERR_HTTP2_INVALID_SESSION");
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
      G.queueMicrotask(() => self._maybeDestroy());
    }
    // node kMaybeDestroy: a graceful close lets the OPEN streams complete on
    // their own and destroys the session only once the last one is gone. The
    // server used to tear down on the next microtask instead, which truncated
    // the response of any stream still in flight when server.close() ran
    // (test-http2-graceful-close writes 1 MB after calling server.close()).
    // Unlike the client, the server's stream map is only ever cleared by a
    // stream destroy, so it is a valid gate on its own.
    _maybeDestroy() {
      if (this.destroyed) return;
      if (!this.closed) return;
      if (this.streams.size > 0) { this._destroyPending = true; return; }
      this._destroyPending = false;
      this._teardown(false);
    }
    destroy(err, code) { if (this.destroyed) return; if (err) { const self = this; this._teardown(); G.queueMicrotask(() => self.emit("error", err)); } else this._teardown(); }
    ref() { if (this.socket && this.socket.ref) this.socket.ref(); return this; }
    unref() { if (this.socket && this.socket.unref) this.socket.unref(); return this; }
    // node Http2Session#setTimeout is on BOTH halves (it lives on the shared
    // Http2Session prototype); this one was a no-op stub, so an http2 SERVER
    // could never reap an idle session and `server.setTimeout()` / `server.timeout`
    // had nothing to arm. Same shape as ClientHttp2Session's: one UNREF'd timer,
    // refreshed by inbound activity, emitting 'timeout' on the session and on
    // every open stream.
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
      if (!this._timeoutMs || this.destroyed) return;
      const self = this;
      this._timer = G.setTimeout(() => {
        self._timer = null;
        if (self.destroyed) return;
        self.emit("timeout");
        for (const s of Array.from(self.streams.values())) { try { s.emit("timeout"); } catch (e) {} }
      }, this._timeoutMs);
      if (this._timer && this._timer.unref) this._timer.unref();
    }
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
      // node compat.js attaches onStreamError, a DELIBERATELY EMPTY handler:
      // "errors in compatibility mode are not forwarded to the request and
      // response objects". Without it the stream error a compat write-after-end
      // raises (Http2ServerResponse#write destroys the stream with
      // ERR_STREAM_WRITE_AFTER_END, exactly as node's does) has no listener and
      // becomes an uncaught exception instead of just the callback's `err`.
      stream.on("error", () => {});
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
    // node Http2SecureServer: when `allowHTTP1` is set the server is ALSO a
    // full HTTP/1.1 server -- storeHTTPOptions + setupConnectionsTracking are
    // called on it, and connectionListener hands any socket whose ALPN result
    // is `false`/'http/1.1' to _http_server.js's _connectionListener instead of
    // building an Http2Session. Here the equivalent of that whole bundle is
    // js_net_part2.cppm's createHttpServer(options, undefined, baseServer): it
    // installs the parser loop on 'secureConnection', the timeouts sweeper, and
    // the close()/listen() wrappers (its close() is httpServerPreClose, which
    // is what lets an idle keep-alive connection be reaped by server.close()).
    //
    // Its 'secureConnection' listener is unconditional, so it is captured and
    // detached immediately: the h2 listener below is the only dispatcher, and
    // it calls this one only for the non-h2 ALPN results. Without the detach
    // both stacks would try to parse every connection.
    let http1Listener = null;
    if (secure && options && options.allowHTTP1 === true &&
        typeof G.__mbunHttpServerFactory === "function") {
      // node initializeOptions: the deprecated top-level Http1IncomingMessage /
      // Http1ServerResponse migrate into the `http1Options` bag (DEP0201), and
      // storeHTTPOptions receives `{ ...options, ...options.http1Options }`.
      const h1opts = Object.assign({}, options, options.http1Options);
      if (options.Http1IncomingMessage !== undefined && h1opts.IncomingMessage === undefined)
        h1opts.IncomingMessage = options.Http1IncomingMessage;
      if (options.Http1ServerResponse !== undefined && h1opts.ServerResponse === undefined)
        h1opts.ServerResponse = options.Http1ServerResponse;
      const before = server.listeners("secureConnection");
      G.__mbunHttpServerFactory(h1opts, undefined, server);
      for (const listener of server.listeners("secureConnection")) {
        if (before.indexOf(listener) === -1) {
          http1Listener = listener;
          server.removeListener("secureConnection", listener);
        }
      }
    }
    if (typeof onRequest === "function") server.on("request", onRequest);
    const self = server;
    // node Http2Server.prototype[EventEmitter.captureRejectionSymbol]: an async
    // 'stream'/'request' listener that rejects must not become an uncaught
    // throw. A stream that already sent headers is destroyed with the error; a
    // stream that has not gets a bare 500, and a response that has not sent
    // headers is rewritten as 500 with every header the handler set removed
    // ("don't leak headers"). Anything else falls back to net.Server's handler.
    server[kRejection] = function (err, event, ...args) {
      if (event === "stream") {
        const stream = args[0];
        if (!stream) return;
        if (stream.sentHeaders) { stream.destroy(err); return; }
        try { stream.respond({ ":status": 500 }); stream.end(); } catch (e) { try { stream.destroy(err); } catch (e2) {} }
        return;
      }
      if (event === "request") {
        const res = args[1];
        if (!res) return;
        if (!res.headersSent && !res.finished) {
          for (const name of res.getHeaderNames()) res.removeHeader(name);
          res.statusCode = 500;
          const http1 = M["http"] || M["node:http"] || {};
          res.end((http1.STATUS_CODES && http1.STATUS_CODES[500]) || "Internal Server Error");
        } else {
          res.destroy();
        }
        return;
      }
      if (event === "connection" || event === "secureConnection") {
        const sock = args[0];
        if (sock && typeof sock.destroy === "function") sock.destroy(err);
        return;
      }
      this.emit("error", err);
    };
    // node Http2Server#close(): `NETServer.prototype.close` THEN
    // closeAllSessions(this) — closing the listener is not enough, the live
    // sessions have to be told to shut down gracefully too, or an idle client
    // keeps the connection (and the loop) alive forever
    // (test-http2-server-close-idle-connection).
    const sessions = new Set();
    const netClose = server.close.bind(server);
    server.close = function (cb) {
      const result = netClose(cb);
      for (const session of Array.from(sessions)) { try { session.close(); } catch (e) {} }
      return result;
    };
    const onSession = (socket) => {
      // node connectionListener: an ALPN result of `false` (nothing negotiated)
      // or 'http/1.1' is not an h2 connection. With allowHTTP1 it becomes an
      // ordinary HTTP/1.1 connection; otherwise the socket is offered to an
      // 'unknownProtocol' listener, and if nobody claims it the server replies
      // with the 403 that names the missing ALPN protocol and destroys the
      // socket after options.unknownProtocolTimeout (default 10s, unref'd).
      if (secure && (socket.alpnProtocol === false || socket.alpnProtocol === "http/1.1")) {
        if (http1Listener !== null) return http1Listener.call(self, socket);
        if (self.emit("unknownProtocol", socket)) return;
        const ms = (options && options.unknownProtocolTimeout !== undefined)
          ? options.unknownProtocolTimeout : 10000;
        const timer = G.setTimeout(() => { if (!socket.destroyed) socket.destroy(); }, ms);
        if (timer && typeof timer.unref === "function") timer.unref();
        socket.once("close", () => G.clearTimeout(timer));
        socket.end("HTTP/1.0 403 Forbidden\r\n" +
                   "Content-Type: text/plain\r\n\r\n" +
                   "Missing ALPN Protocol, expected `h2` to be available.\n" +
                   "If this is a HTTP request: The server was not " +
                   "configured with the `allowHTTP1` option or a " +
                   "listener for the `unknownProtocol` event.\n");
        return;
      }
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
      sessions.add(session);
      session.on("close", () => sessions.delete(session));
      // node connectionListener: `if (this.timeout) session.setTimeout(this.timeout,
      // sessionOnTimeout)`, and sessionOnTimeout re-raises 'timeout' on the
      // SERVER with the session as its argument, falling back to a graceful
      // session.close() when nothing is listening. `server.timeout` was recorded
      // and then never used, so an idle session was never reaped and the server
      // never emitted 'timeout'.
      if (self.timeout) {
        session.setTimeout(self.timeout, () => {
          if (session.destroyed) return;
          if (!self.emit("timeout", session)) { try { session.close(); } catch (e) {} }
        });
      }
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
    // node Http2Server/Http2SecureServer keep their normalised construction
    // options under a Symbol('options'); `settings` starts as a copy of
    // options.settings and updateSettings() merges into it. The corpus reaches
    // for that symbol by name (test-http2-update-settings), so it has to exist
    // and it has to be the thing updateSettings writes to.
    const kServerOptions = Symbol("options");
    server[kServerOptions] = Object.assign({}, options, {
      settings: Object.assign({}, options && options.settings),
    });
    server.updateSettings = function (settings) {
      if (settings === undefined || settings === null || typeof settings !== "object" || Array.isArray(settings)) {
        const e = new TypeError('The "settings" argument must be of type object. Received ' +
          (settings === null ? "null" : Array.isArray(settings) ? "an instance of Array"
            : typeof settings === "string" ? "type string ('" + settings + "')"
            : "type " + typeof settings + " (" + String(settings) + ")"));
        e.code = "ERR_INVALID_ARG_TYPE";
        throw e;
      }
      validateSettings(settings);
      this[kServerOptions].settings = Object.assign({}, this[kServerOptions].settings, settings);
    };
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
    // node Http2SecureServer only imposes the h2 ALPN list when the caller did
    // NOT bring an ALPNCallback — tls rejects the two together, so forcing the
    // list turned a legal `{ ALPNCallback }` server into
    // ERR_TLS_ALPN_CALLBACK_WITH_PROTOCOLS (test-http2-alpn). When the caller
    // DID pass both, the rejection is correct and tls.createServer raises it.
    const alpn = (options && options.allowHTTP1) ? ["h2", "http/1.1"] : ["h2"];
    const tlsSrvOpts = Object.assign({}, options);
    if (!(options && options.ALPNCallback)) tlsSrvOpts.ALPNProtocols = (options && options.ALPNProtocols) || alpn;
    const server = tls.createServer(tlsSrvOpts);
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
    // Same reasoning as the symbols: `NghttpError` is a class identity the
    // corpus compares against (`common.expectsError({ constructor: NghttpError
    // })` in test-http2-server-http1-client), and a structurally identical
    // clone is not equal to it. Adopt the real one when node's util.js is
    // loaded; nghttpErr() falls back to a plain ERR_HTTP2_ERROR otherwise.
    if (typeof u.NghttpError === "function") NghttpErrorCtor = u.NghttpError;
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
      nativeHttp2StreamPrototype = Http2Stream.prototype;
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
