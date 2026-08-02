// CAP-S3 payload partition: the Bun.S3Client / Bun.s3 / S3File JS surface.
//
// PORT-SOURCE: bun src/runtime/webcore/{S3Client,S3File,S3Stat}.rs +
// src/runtime/webcore/s3/{credentials_jsc,simple_request,client}.rs. The SigV4
// signature is produced natively by __mbunS3Native.sign (mbun.s3_signing, wired
// in runtime/s3_native.inc); this layer parses options exactly as bun's
// credentials_jsc.rs does, drives the request over node:http/https, and maps S3
// XML error bodies to S3Error the way simple_request.rs error_with_body does.
//
// Runs inside the shared kNodeBuiltinsJS IIFE (G = globalThis, native modules on
// M); every require() is lazy so partition order does not matter.
export module mbun.jsc.js_builtins:s3;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kS3JS = R"JS(
  // ------------------------------- Bun.S3Client -----------------------------
  // Self-contained IIFE re-binding G = globalThis (partitions after image_closure
  // no longer share the outer scope's G). Top level must never throw.
  (function () {
    const G = globalThis;
    if (!(G.__mbunS3Native && G.Bun && !G.Bun.S3Client)) return;
    const S3N = G.__mbunS3Native;
    const B = G.Bun;
    const req = (spec) => (G.require || ((s) => G.__mbun_require_native(s, ".")))(spec);

    const ACL_VALUES = new Set([
      "private", "public-read", "public-read-write", "aws-exec-read",
      "authenticated-read", "bucket-owner-read", "bucket-owner-full-control",
      "log-delivery-write",
    ]);
    const STORAGE_VALUES = new Set([
      "STANDARD", "STANDARD_IA", "INTELLIGENT_TIERING", "EXPRESS_ONEZONE",
      "ONEZONE_IA", "GLACIER", "GLACIER_IR", "REDUCED_REDUNDANCY", "OUTPOSTS",
      "DEEP_ARCHIVE", "SNOW",
    ]);
    // RFC 2045 token/token content-type; params after the first ';' are kept.
    const MIME_RE = /^[!#$%&'*+.^_`|~0-9A-Za-z-]+\/[!#$%&'*+.^_`|~0-9A-Za-z-]+([ \t]*;.*)?$/;
    const MIN_PART = 5 * 1024 * 1024;
    const MAX_PART = 5 * 1024 * 1024 * 1024;

    const env = () => (typeof process !== "undefined" && process.env) || {};
    const envGet = (a, b) => { const e = env(); const v = e[a]; if (v != null && v !== "") return v; const w = e[b]; return w != null && w !== "" ? w : ""; };

    function toI32(v) { let n = Number(v); if (!Number.isFinite(n)) n = 0; return Math.trunc(n) | 0; }

    // bun URL::parse(endpoint).host_with_path() + is_http(); a trailing bare "/"
    // is dropped so the signer builds "/bucket/key", not "//bucket/key".
    function normEndpoint(raw) {
      let u;
      try { u = new G.URL(String(raw)); } catch (e) { u = null; }
      if (!u || !u.host) throw new TypeError('The "endpoint" argument must be of type string.');
      let hp = u.host;
      if (u.pathname && u.pathname !== "/") hp += u.pathname.replace(/\/+$/, "");
      return { endpoint: hp, insecureHttp: u.protocol === "http:" };
    }

    function defaultCreds() {
      const c = {
        accessKeyId: envGet("S3_ACCESS_KEY_ID", "AWS_ACCESS_KEY_ID"),
        secretAccessKey: envGet("S3_SECRET_ACCESS_KEY", "AWS_SECRET_ACCESS_KEY"),
        region: envGet("S3_REGION", "AWS_REGION"),
        endpoint: "", insecureHttp: false,
        bucket: envGet("S3_BUCKET", "AWS_BUCKET"),
        sessionToken: envGet("S3_SESSION_TOKEN", "AWS_SESSION_TOKEN"),
        virtualHostedStyle: false, requestPayer: false,
        acl: undefined, storageClass: undefined,
        queueSize: 255, retry: 3, partSize: MIN_PART,
        type: "", contentDisposition: "", contentEncoding: "",
      };
      const ep = envGet("S3_ENDPOINT", "AWS_ENDPOINT");
      if (ep) { const n = normEndpoint(ep); c.endpoint = n.endpoint; c.insecureHttp = n.insecureHttp; }
      return c;
    }

    // Merge `opts` over `base` (or env defaults) with bun credentials_jsc.rs
    // validation: TypeError for a bad acl/storageClass/endpoint/type-with-CRLF,
    // RangeError for out-of-range queueSize/retry/partSize.
    function parseOptions(opts, base) {
      const c = base ? Object.assign({}, base) : defaultCreds();
      if (opts == null) return c;
      if (typeof opts !== "object") return c;
      const str = (k) => { const v = opts[k]; return v != null && v !== "" ? String(v) : undefined; };

      let v;
      if ((v = str("accessKeyId")) !== undefined) c.accessKeyId = v;
      if ((v = str("secretAccessKey")) !== undefined) c.secretAccessKey = v;
      if ((v = str("region")) !== undefined) c.region = v;
      if (opts.endpoint != null && opts.endpoint !== "") {
        if (typeof opts.endpoint !== "string") throw new TypeError('The "endpoint" argument must be of type string.');
        const n = normEndpoint(opts.endpoint); c.endpoint = n.endpoint; c.insecureHttp = n.insecureHttp;
      }
      if ((v = str("bucket")) !== undefined) c.bucket = v;
      if (typeof opts.virtualHostedStyle === "boolean") c.virtualHostedStyle = opts.virtualHostedStyle;
      if ((v = str("sessionToken")) !== undefined) c.sessionToken = v;

      if (opts.pageSize != null) { const n = toI32(opts.pageSize); if (n < MIN_PART || n > MAX_PART) throw new RangeError('"pageSize" is out of range. It must be >= ' + MIN_PART + " and <= " + MAX_PART + ". Received " + n); c.partSize = n; }
      if (opts.partSize != null) { const n = toI32(opts.partSize); if (n < MIN_PART || n > MAX_PART) throw new RangeError('"partSize" is out of range. It must be >= ' + MIN_PART + " and <= " + MAX_PART + ". Received " + n); c.partSize = n; }
      if (opts.queueSize != null) { const n = toI32(opts.queueSize); if (n < 1) throw new RangeError('"queueSize" is out of range. It must be >= 1. Received ' + n); c.queueSize = Math.min(n, 255); }
      if (opts.retry != null) { const n = toI32(opts.retry); if (n < 0 || n > 255) throw new RangeError('"retry" is out of range. It must be >= 0 and <= 255. Received ' + n); c.retry = n; }

      if (opts.acl != null && opts.acl !== "") { const a = String(opts.acl); if (!ACL_VALUES.has(a)) throw new TypeError('"acl" is not a valid S3 ACL. Received ' + JSON.stringify(a)); c.acl = a; }
      if (opts.storageClass != null && opts.storageClass !== "") { const s = String(opts.storageClass); if (!STORAGE_VALUES.has(s)) throw new TypeError('"storageClass" is not a valid S3 storage class. Received ' + JSON.stringify(s)); c.storageClass = s; }

      if (opts.contentDisposition != null && opts.contentDisposition !== "") { const s = String(opts.contentDisposition); if (/[\r\n]/.test(s)) throw new TypeError("contentDisposition must not contain newline characters (CR/LF)"); c.contentDisposition = s; }
      if (opts.contentEncoding != null && opts.contentEncoding !== "") { const s = String(opts.contentEncoding); if (/[\r\n]/.test(s)) throw new TypeError("contentEncoding must not contain newline characters (CR/LF)"); c.contentEncoding = s; }
      if (opts.type != null && opts.type !== "") {
        const s = String(opts.type);
        if (/[\r\n]/.test(s)) throw new TypeError("type must not contain newline characters (CR/LF)");
        // A value the MIME parser rejects (e.g. an embedded control char) is
        // dropped, never stored or reflected into request headers (bun S3File.rs
        // mime_type -> None keeps the default empty content type).
        c.type = MIME_RE.test(s) ? s : "";
      }
      if (typeof opts.requestPayer === "boolean") c.requestPayer = opts.requestPayer;
      return c;
    }

    function normKey(key) {
      if (typeof key === "number") {
        if (!Number.isSafeInteger(key) || key < 0) throw new RangeError('The value of "path" is out of range.');
        return String(key);
      }
      if (key == null) throw new TypeError("Expected a path to an S3 object");
      return String(key);
    }

    // SigV4 timestamp YYYYMMDDTHHMMSSZ.
    function amzNow() { return new Date().toISOString().replace(/[-:]/g, "").replace(/\.\d{3}/, ""); }

    function sign(cred, method, key, extra) {
      extra = extra || {};
      const o = {
        accessKeyId: cred.accessKeyId || "", secretAccessKey: cred.secretAccessKey || "",
        region: cred.region || "", endpoint: cred.endpoint || "", bucket: cred.bucket || "",
        sessionToken: cred.sessionToken || "", insecureHttp: !!cred.insecureHttp,
        virtualHostedStyle: !!cred.virtualHostedStyle, requestPayer: !!cred.requestPayer,
        path: key, method: method, contentHash: "UNSIGNED-PAYLOAD",
        searchParams: extra.searchParams || "", amzDate: amzNow(),
        acl: cred.acl, storageClass: cred.storageClass,
        contentType: cred.type || "",
        contentDisposition: cred.contentDisposition || "", contentEncoding: cred.contentEncoding || "",
      };
      if (extra.expires) o.expires = extra.expires;
      if (extra.allowEmptyPath) o.allowEmptyPath = true;
      return S3N.sign(o);
    }

    // Conditional signed headers whose exact values must match what was signed.
    function signedHeaders(cred, isPut) {
      const h = {};
      if (cred.acl) h["x-amz-acl"] = cred.acl;
      if (cred.storageClass) h["x-amz-storage-class"] = cred.storageClass;
      if (cred.sessionToken) h["x-amz-security-token"] = cred.sessionToken;
      if (cred.requestPayer) h["x-amz-request-payer"] = "requester";
      if (cred.contentDisposition) h["content-disposition"] = cred.contentDisposition;
      if (cred.contentEncoding) h["content-encoding"] = cred.contentEncoding;
      if (isPut && cred.type) h["content-type"] = cred.type;
      return h;
    }

    function toBuf(data) {
      const Buf = G.Buffer;
      if (data == null) return Buf.alloc(0);
      if (Buf.isBuffer(data)) return data;
      if (typeof data === "string") return Buf.from(data, "utf-8");
      if (data instanceof G.ArrayBuffer) return Buf.from(data);
      if (ArrayBuffer.isView(data)) return Buf.from(data.buffer, data.byteOffset, data.byteLength);
      return Buf.from(String(data), "utf-8");
    }

    function makeS3Error(code, message, path) {
      const e = new Error(message);
      e.name = "S3Error"; e.code = code;
      if (path) e.path = path;
      return e;
    }

    // bun simple_request.rs error_with_body: pull <Code>/<Message> out of the XML
    // body; a 404 with no explicit code becomes NoSuchKey.
    function parseS3Error(body, notFound) {
      let code = "UnknownError", message = "an unexpected error has occurred", hasCode = false;
      const s = body ? String(body) : "";
      if (s.length) {
        message = s;
        const cs = s.indexOf("<Code>");
        if (cs >= 0) { const ce = s.indexOf("</Code>"); if (ce >= cs + 6) { code = s.slice(cs + 6, ce); hasCode = true; } }
        const ms = s.indexOf("<Message>");
        if (ms >= 0) { const me = s.indexOf("</Message>"); if (me >= ms + 9) message = s.slice(ms + 9, me); }
      }
      if (notFound && !hasCode) { code = "NoSuchKey"; message = "The specified key does not exist."; }
      return makeS3Error(code, message);
    }

    function request(signed, method, body, extraHeaders) {
      return new Promise((resolve, reject) => {
        let u;
        try { u = new G.URL(signed.url); } catch (e) { return reject(makeS3Error("UnknownError", e && e.message || "invalid S3 url")); }
        const isHttps = u.protocol === "https:";
        let mod;
        try { mod = req(isHttps ? "node:https" : "node:http"); } catch (e) { return reject(e); }
        const headers = Object.assign({
          "Host": signed.host,
          "x-amz-date": signed.amzDate,
          "x-amz-content-sha256": "UNSIGNED-PAYLOAD",
          "Authorization": signed.authorization,
        }, extraHeaders || {});
        let buf = null;
        if (body != null) { buf = toBuf(body); headers["Content-Length"] = String(buf.length); }
        else if (method === "PUT") headers["Content-Length"] = "0";
        let r;
        try {
          r = mod.request({ protocol: u.protocol, hostname: u.hostname, port: u.port, path: u.pathname + u.search, method, headers }, (res) => {
            const chunks = [];
            res.on("data", (c) => chunks.push(c));
            res.on("error", (e) => reject(makeS3Error("UnknownError", e && e.message || "an unexpected error has occurred")));
            res.on("end", () => resolve({ status: res.statusCode, headers: res.headers, body: G.Buffer.concat(chunks) }));
          });
        } catch (e) { return reject(makeS3Error("UnknownError", e && e.message || "an unexpected error has occurred")); }
        r.on("error", (e) => reject(makeS3Error("UnknownError", e && e.message || "an unexpected error has occurred")));
        if (buf) r.write(buf);
        r.end();
      });
    }

    class S3File {
      constructor(cred, key) { this._cred = cred; this._key = key; this.name = key; }
      get type() { return this._cred.type || ""; }

      async _download() {
        const r = await request(sign(this._cred, "GET", this._key), "GET", null, signedHeaders(this._cred, false));
        if (r.status === 200 || r.status === 206) return r.body;
        throw parseS3Error(r.body.toString("utf-8"), r.status === 404);
      }
      async text() { return (await this._download()).toString("utf-8"); }
      async json() { return JSON.parse((await this._download()).toString("utf-8")); }
      async bytes() { return new Uint8Array(await this._download()); }
      async arrayBuffer() { const b = await this._download(); return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength); }

      async write(data) {
        const buf = toBuf(data);
        const r = await request(sign(this._cred, "PUT", this._key), "PUT", buf, signedHeaders(this._cred, true));
        if (r.status >= 200 && r.status < 300) return buf.length;
        throw parseS3Error(r.body.toString("utf-8"), false);
      }

      presign(opts) {
        const cred = opts ? parseOptions(opts, this._cred) : this._cred;
        const method = (opts && opts.method) || "GET";
        const expires = (opts && (opts.expiresIn || opts.expires)) || 86400;
        return sign(cred, method, this._key, { expires }).url;
      }

      async exists() {
        const r = await request(sign(this._cred, "HEAD", this._key), "HEAD", null, signedHeaders(this._cred, false));
        if (r.status === 200) return true;
        if (r.status === 404) return false;
        throw parseS3Error(r.body.toString("utf-8"), false);
      }

      async stat() {
        const r = await request(sign(this._cred, "HEAD", this._key), "HEAD", null, signedHeaders(this._cred, false));
        if (r.status === 200) {
          const h = r.headers;
          return {
            etag: (h.etag || "").replace(/^"|"$/g, ""),
            lastModified: h["last-modified"] ? new Date(h["last-modified"]) : new Date(0),
            size: parseInt(h["content-length"] || "0", 10) || 0,
            type: h["content-type"] || "",
          };
        }
        if (r.status === 404) throw parseS3Error("", true);
        throw parseS3Error(r.body.toString("utf-8"), false);
      }
      async size() { return (await this.stat()).size; }

      async delete() {
        const r = await request(sign(this._cred, "DELETE", this._key), "DELETE", null, signedHeaders(this._cred, false));
        if (r.status === 200 || r.status === 204) return;
        if (r.status === 404) throw parseS3Error("", true);
        throw parseS3Error(r.body.toString("utf-8"), false);
      }
      unlink() { return this.delete(); }

      // GET streamed into a web ReadableStream; a signing failure is deferred into
      // the stream (controller.error) so `stream().text()` rejects with the
      // ERR_S3_* code rather than throwing synchronously (bun S3File.rs stream()).
      stream() {
        const cred = this._cred, key = this._key, extraH = signedHeaders(cred, false);
        let httpReq = null, resObj = null, aborted = false;
        return new G.ReadableStream({
          start(controller) {
            let signed;
            try { signed = sign(cred, "GET", key); } catch (e) { controller.error(e); return; }
            let u;
            try { u = new G.URL(signed.url); } catch (e) { controller.error(makeS3Error("UnknownError", "invalid S3 url")); return; }
            let mod;
            try { mod = req(u.protocol === "https:" ? "node:https" : "node:http"); } catch (e) { controller.error(e); return; }
            const headers = Object.assign({ "Host": signed.host, "x-amz-date": signed.amzDate, "x-amz-content-sha256": "UNSIGNED-PAYLOAD", "Authorization": signed.authorization }, extraH);
            try {
              httpReq = mod.request({ protocol: u.protocol, hostname: u.hostname, port: u.port, path: u.pathname + u.search, method: "GET", headers }, (res) => {
                resObj = res;
                if (aborted) { try { res.destroy && res.destroy(); } catch (e) {} return; }
                if (res.statusCode < 200 || res.statusCode >= 300) {
                  const chunks = [];
                  res.on("data", (c) => chunks.push(c));
                  res.on("end", () => { try { controller.error(parseS3Error(G.Buffer.concat(chunks).toString("utf-8"), res.statusCode === 404)); } catch (e) {} });
                  res.on("error", (e) => { try { controller.error(makeS3Error("UnknownError", e && e.message)); } catch (e2) {} });
                  return;
                }
                res.on("data", (c) => { if (!aborted) { try { controller.enqueue(c); } catch (e) {} } });
                res.on("end", () => { try { controller.close(); } catch (e) {} });
                res.on("error", (e) => { try { controller.error(makeS3Error("UnknownError", e && e.message)); } catch (e2) {} });
              });
              httpReq.on("error", (e) => { if (!aborted) { try { controller.error(makeS3Error("UnknownError", e && e.message)); } catch (e2) {} } });
              httpReq.end();
            } catch (e) { try { controller.error(makeS3Error("UnknownError", e && e.message)); } catch (e2) {} }
          },
          cancel() {
            aborted = true;
            try { if (resObj && resObj.destroy) resObj.destroy(); } catch (e) {}
            try { if (httpReq && httpReq.destroy) httpReq.destroy(); else if (httpReq && httpReq.abort) httpReq.abort(); } catch (e) {}
          },
        });
      }

      // Incremental writer: buffers parts, then on end() does a single PUT when
      // the total fits one part, or a CreateMultipartUpload / UploadPart* /
      // CompleteMultipartUpload sequence otherwise (bun S3File writer + multipart.rs).
      writer(options) {
        const cred = options ? parseOptions(options, this._cred) : this._cred;
        const key = this._key;
        const parts = [];
        let ended = false;
        return {
          write(data) { const b = toBuf(data); parts.push(b); return b.length; },
          async flush() { return 0; },
          async end(data) {
            if (ended) return 0;
            ended = true;
            if (data != null) parts.push(toBuf(data));
            const body = G.Buffer.concat(parts);
            const partSize = cred.partSize || MIN_PART;
            if (body.length <= partSize) {
              const r = await request(sign(cred, "PUT", key), "PUT", body, signedHeaders(cred, true));
              if (r.status >= 200 && r.status < 300) return body.length;
              throw parseS3Error(r.body.toString("utf-8"), false);
            }
            return multipartUpload(cred, key, body, partSize);
          },
        };
      }
    }

    async function multipartUpload(cred, key, body, partSize) {
      // 1. CreateMultipartUpload — storageClass/acl headers ride on this request.
      const cr = await request(sign(cred, "POST", key, { searchParams: "?uploads=" }), "POST", null, signedHeaders(cred, true));
      if (cr.status < 200 || cr.status >= 300) throw parseS3Error(cr.body.toString("utf-8"), false);
      const uploadId = tagVal(cr.body.toString("utf-8"), "UploadId");
      // The id is endpoint-supplied and is echoed into the request line of every
      // later part ("?partNumber=N&uploadId=..."), so it is validated before the
      // first reuse rather than trusted. PORT-SOURCE: bun
      // src/runtime/webcore/s3/multipart.rs:697-715
      // (on_start_multi_part_request_result) — empty, longer than
      // MAX_UPLOAD_ID_LEN (multipart.rs:179), or any byte that is `!is_ascii() ||
      // is_ascii_control()` fails the upload with UnknownError / "Failed to
      // initiate multipart upload". `[^\x20-\x7e]` is exactly that byte set.
      // encodeURIComponent below already neutralises a CR/LF for the wire, but
      // that only downgrades an injection to a silently-succeeding upload against
      // an id the service never issued; bun aborts instead.
      if (!uploadId || uploadId.length > 2000 || /[^\x20-\x7e]/.test(uploadId))
        throw makeS3Error("UnknownError", "Failed to initiate multipart upload");
      const encId = encodeURIComponent(uploadId);
      try {
        // 2. UploadPart for each partSize slice, in order.
        const done = [];
        let partNumber = 1;
        for (let off = 0; off < body.length; off += partSize, partNumber++) {
          const chunk = body.subarray(off, Math.min(off + partSize, body.length));
          const q = "?partNumber=" + partNumber + "&uploadId=" + encId;
          const pr = await request(sign(cred, "PUT", key, { searchParams: q }), "PUT", chunk, {});
          if (pr.status < 200 || pr.status >= 300) throw parseS3Error(pr.body.toString("utf-8"), false);
          done.push({ partNumber, etag: pr.headers.etag || "" });
        }
        // 3. CompleteMultipartUpload with the ordered part/etag manifest.
        const xml = "<CompleteMultipartUpload>" + done.map((p) => "<Part><PartNumber>" + p.partNumber + "</PartNumber><ETag>" + p.etag + "</ETag></Part>").join("") + "</CompleteMultipartUpload>";
        const done2 = await request(sign(cred, "POST", key, { searchParams: "?uploadId=" + encId }), "POST", xml, {});
        if (done2.status < 200 || done2.status >= 300) throw parseS3Error(done2.body.toString("utf-8"), false);
        return body.length;
      } catch (e) {
        // Best-effort AbortMultipartUpload so a failed upload leaves no parts.
        try { await request(sign(cred, "DELETE", key, { searchParams: "?uploadId=" + encId }), "DELETE", null, {}); } catch (e2) {}
        throw e;
      }
    }

    // ------ list: build the ?...&list-type=2&... query in bun's exact param
    // order (client.rs list_objects), sign the bucket root with an empty key -----
    function encodeListQuery(options) {
      const parts = [];
      if (options) {
        if (options.continuationToken != null) parts.push("continuation-token=" + encodeURIComponent(String(options.continuationToken)));
        if (options.delimiter != null) parts.push("delimiter=" + encodeURIComponent(String(options.delimiter)));
        if (options.encodingType != null) parts.push("encoding-type=url");
        if (options.fetchOwner != null) parts.push("fetch-owner=" + (options.fetchOwner ? "true" : "false"));
      }
      parts.push("list-type=2");
      if (options) {
        if (options.maxKeys != null) parts.push("max-keys=" + Math.trunc(Number(options.maxKeys)));
        if (options.prefix != null) parts.push("prefix=" + encodeURIComponent(String(options.prefix)));
        if (options.startAfter != null) parts.push("start-after=" + encodeURIComponent(String(options.startAfter)));
      }
      return "?" + parts.join("&");
    }

    async function listObjects(cred, options) {
      // bun get_list_objects_options_from_js rejects a non-object options arg.
      if (options != null && typeof options !== "object") {
        const e = new TypeError('The "options" argument must be of type object. Received ' + typeof options);
        e.code = "ERR_INVALID_ARG_TYPE";
        throw e;
      }
      // bun signs ListObjects with acl/storageClass/requestPayer forced off, so
      // only the session token rides as an extra signed header.
      const listCred = Object.assign({}, cred, { acl: undefined, storageClass: undefined, requestPayer: false, contentDisposition: "", contentEncoding: "", type: "" });
      const signed = sign(listCred, "GET", "", { searchParams: encodeListQuery(options), allowEmptyPath: true });
      const extra = {};
      if (listCred.sessionToken) extra["x-amz-security-token"] = listCred.sessionToken;
      const r = await request(signed, "GET", null, extra);
      if (r.status === 200) return parseListResult(r.body.toString("utf-8"));
      throw parseS3Error(r.body.toString("utf-8"), r.status === 404);
    }

    // Faithful port of s3/list_objects.rs parse_s3_list_objects_result + to_js:
    // only present XML elements become result keys; Contents/CommonPrefixes are
    // parsed as blocks so their inner tags never leak into the top-level scan.
    function tagVal(s, tag) {
      const o = "<" + tag + ">", i = s.indexOf(o);
      if (i < 0) return undefined;
      const e = s.indexOf("</" + tag + ">", i + o.length);
      return e < 0 ? undefined : s.slice(i + o.length, e);
    }
    function parseListResult(xml) {
      const result = {};
      if (xml.indexOf("<ListBucketResult") < 0) return result;
      const contents = [];
      { let idx = 0; for (;;) {
          const s = xml.indexOf("<Contents>", idx); if (s < 0) break;
          const e = xml.indexOf("</Contents>", s); if (e < 0) break;
          const block = xml.slice(s + 10, e); idx = e + 11;
          const key = tagVal(block, "Key");
          if (key == null) continue;
          const item = { key: key };
          const etag = tagVal(block, "ETag"); if (etag != null) item.eTag = etag.replace(/&quot;/g, '"');
          const ca = tagVal(block, "ChecksumAlgorithm"); if (ca != null) item.checksumAlgorithme = ca;
          const ct = tagVal(block, "ChecksumType"); if (ct != null) item.checksumType = ct;
          const lm = tagVal(block, "LastModified"); if (lm != null) item.lastModified = lm;
          const sz = tagVal(block, "Size"); if (sz != null) { const n = parseInt(sz, 10); if (Number.isFinite(n)) item.size = n; }
          const sc = tagVal(block, "StorageClass"); if (sc != null) item.storageClass = sc;
          const os = block.indexOf("<Owner>");
          if (os >= 0) { const oe = block.indexOf("</Owner>", os); if (oe >= 0) {
            const ob = block.slice(os + 7, oe); const oid = tagVal(ob, "ID"), odn = tagVal(ob, "DisplayName");
            if ((oid != null && oid !== "") || (odn != null && odn !== "")) { const owner = {}; if (oid != null && oid !== "") owner.id = oid; if (odn != null && odn !== "") owner.displayName = odn; item.owner = owner; }
          } }
          contents.push(item);
      } }
      let rest = xml.replace(/<Contents>[\s\S]*?<\/Contents>/g, "");
      const commonPrefixes = [];
      { let idx = 0; for (;;) {
          const s = rest.indexOf("<CommonPrefixes>", idx); if (s < 0) break;
          const e = rest.indexOf("</CommonPrefixes>", s); if (e < 0) break;
          const block = rest.slice(s + 16, e); idx = e + 17;
          let j = 0; for (;;) { const ps = block.indexOf("<Prefix>", j); if (ps < 0) break; const pe = block.indexOf("</Prefix>", ps + 8); if (pe < 0) break; commonPrefixes.push(block.slice(ps + 8, pe)); j = pe + 9; }
      } }
      rest = rest.replace(/<CommonPrefixes>[\s\S]*?<\/CommonPrefixes>/g, "");
      const setStr = (tag, key) => { const v = tagVal(rest, tag); if (v != null) result[key] = v; };
      setStr("Name", "name");
      { const p = tagVal(rest, "Prefix"); if (p != null && p !== "") result.prefix = p; }
      setStr("Delimiter", "delimiter");
      setStr("StartAfter", "startAfter");
      setStr("EncodingType", "encodingType");
      setStr("ContinuationToken", "continuationToken");
      setStr("NextContinuationToken", "nextContinuationToken");
      { const t = tagVal(rest, "IsTruncated"); if (t === "true") result.isTruncated = true; else if (t === "false") result.isTruncated = false; }
      { const kc = tagVal(rest, "KeyCount"); if (kc != null) { const n = parseInt(kc, 10); if (Number.isFinite(n)) result.keyCount = n; } }
      { const mk = tagVal(rest, "MaxKeys"); if (mk != null) { const n = parseInt(mk, 10); if (Number.isFinite(n)) result.maxKeys = n; } }
      if (contents.length) result.contents = contents;
      if (commonPrefixes.length) result.commonPrefixes = commonPrefixes.map((p) => ({ prefix: p }));
      return result;
    }

    // ---- inspect (bun S3Client.rs write_format): credentials never revealed ----
    function guessRegion(endpoint) {
      if (endpoint) {
        if (endpoint.endsWith(".r2.cloudflarestorage.com")) return "auto";
        const a = endpoint.indexOf(".amazonaws.com");
        if (a >= 0) { const s = endpoint.indexOf("s3."); if (s >= 0) return s + 3 <= a ? endpoint.slice(s + 3, a) : "us-east-1"; }
        return "auto";
      }
      return "us-east-1";
    }
    function inspectClient(c) {
      const ep = c.endpoint ? c.endpoint : (c.virtualHostedStyle ? "https://<bucket>.s3.<region>.amazonaws.com" : "https://s3.<region>.amazonaws.com");
      const region = c.region ? c.region : guessRegion(c.endpoint);
      let s = "S3Client {\n";
      s += '  endpoint: "' + ep + '",\n';
      s += '  region: "' + region + '",\n';
      if (c.accessKeyId) s += '  accessKeyId: "[REDACTED]",\n';
      if (c.secretAccessKey) s += '  secretAccessKey: "[REDACTED]",\n';
      if (c.sessionToken) s += '  sessionToken: "[REDACTED]",\n';
      if (c.acl) s += '  acl: "' + c.acl + '",\n';
      s += "  partSize: " + c.partSize + ",\n";
      s += "  queueSize: " + c.queueSize + ",\n";
      s += "  retry: " + c.retry + "\n}";
      return s;
    }

    class S3Client {
      constructor(options) { this._cred = parseOptions(options, null); }
      file(key, options) { const cred = options ? parseOptions(options, this._cred) : this._cred; return new S3File(cred, normKey(key)); }
      presign(key, options) { return this.file(key, options).presign(options); }
      write(key, data, options) { return this.file(key, options).write(data); }
      unlink(key, options) { return this.file(key, options).delete(); }
      delete(key, options) { return this.file(key, options).delete(); }
      exists(key, options) { return this.file(key, options).exists(); }
      size(key, options) { return this.file(key, options).size(); }
      stat(key, options) { return this.file(key, options).stat(); }
      list(options, clientOptions) { const cred = clientOptions ? parseOptions(clientOptions, this._cred) : this._cred; return listObjects(cred, options); }
    }
    S3Client.prototype[Symbol.for("nodejs.util.inspect.custom")] = function () { return inspectClient(this._cred); };
    try { if (B.inspect && B.inspect.custom) S3Client.prototype[B.inspect.custom] = function () { return inspectClient(this._cred); }; } catch (e) {}

    // Static surface (bun S3Client.classes.ts klass): parse options per-call.
    const staticFile = (key, options) => new S3File(parseOptions(options, null), normKey(key));
    S3Client.file = staticFile;
    S3Client.presign = (key, options) => staticFile(key, options).presign(options);
    S3Client.write = (key, data, options) => staticFile(key, options).write(data);
    S3Client.unlink = (key, options) => staticFile(key, options).delete();
    S3Client.delete = (key, options) => staticFile(key, options).delete();
    S3Client.exists = (key, options) => staticFile(key, options).exists();
    S3Client.size = (key, options) => staticFile(key, options).size();
    S3Client.stat = (key, options) => staticFile(key, options).stat();
    S3Client.list = (options, clientOptions) => listObjects(parseOptions(clientOptions, null), options);

    Object.defineProperty(S3Client, "name", { value: "S3Client", configurable: true });
    B.S3Client = S3Client;
    B.S3File = S3File;
    // Bun.s3 is the default client bound to the ambient S3_*/AWS_* credentials.
    B.s3 = new S3Client();
  })();
)JS";

}  // namespace mbun::jsc::builtins::detail
