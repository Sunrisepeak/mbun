// node:zlib streaming Transform classes partition.
//
// The one-shot sync/async node:zlib entry points (deflateSync, gzip, brotli*,
// zstd*, …) are wired in bootstrap on top of __mbunZlibNative → mbun.compress.
// This partition adds the *streaming* half: the zlib.Gzip/Deflate/Inflate/…,
// Brotli* and Zstd* Transform classes plus their create* factories, driven by
// the stateful incremental handles __mbunZlibNative.stream* (mbun.compress.
// stream). Each instance owns a native codec handle and feeds data through it
// chunk by chunk, so streams that never call end() (a decoder that reaches
// end-of-stream mid-write) still auto-end their readable side, and bytesWritten
// reports exactly the bytes the engine consumed.
//
// NOTE: this partition is appended AFTER the master builtins IIFE (opened in
// bootstrap, closed by image_closure) has already run, so it is a self-contained
// IIFE that re-binds G = globalThis and patches the already-registered zlib
// module object in place. It must not rely on the outer IIFE's aliases.
//
// Blueprint: node lib/zlib.js (ZlibBase/Zlib/Brotli/Zstd class hierarchy,
// _processChunk, flush, reset, bytesWritten) and bun's node:zlib native handle.
export module mbun.jsc.js_builtins:zlib_stream;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kZlibStreamJS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  const ZN = G.__mbunZlibNative;
  if (!M || !ZN || typeof ZN.streamOpen !== "function") return;
  const zmod = M["zlib"] || M["node:zlib"];
  const streamMod = M["stream"] || M["node:stream"];
  if (!zmod || !streamMod || !streamMod.Transform) return;
  const Transform = streamMod.Transform;
  const finished = streamMod.finished;
  const Buffer = G.Buffer;
  const EMPTY = "";
  const MAXS = Number.MAX_SAFE_INTEGER;

  // StreamKind ordinals (mirror mbun::compress::StreamKind).
  const K_DEFLATE = 0, K_INFLATE = 1, K_BENC = 2, K_BDEC = 3, K_ZENC = 4, K_ZDEC = 5;
  // StreamFlush ordinals (mirror mbun::compress::StreamFlush).
  const F_NONE = 0, F_SYNC = 1, F_FINISH = 2, F_PARTIAL = 3, F_FULL = 4, F_BLOCK = 5;

  // node's per-family flush constant -> StreamFlush ordinal. The zlib family has
  // six distinct modes (Z_NO_FLUSH .. Z_BLOCK); brotli and zstd have one flush
  // operation each, so everything that is not "process" or "finish" is a flush.
  const ZLIB_FLUSH_MAP = [F_NONE, F_PARTIAL, F_SYNC, F_FULL, F_FINISH, F_BLOCK];
  const flushOrdinal = (kind, nodeFlush) => {
    if (kind === K_DEFLATE || kind === K_INFLATE) {
      const m = ZLIB_FLUSH_MAP[nodeFlush];
      return m === undefined ? F_NONE : m;
    }
    // brotli: 0 PROCESS, 1 FLUSH, 2 FINISH, 3 EMIT_METADATA.
    // zstd:   0 continue, 1 flush, 2 end.
    if (nodeFlush === 2 && (kind === K_ZENC || kind === K_ZDEC)) return F_FINISH;
    if (kind === K_BENC || kind === K_BDEC) return nodeFlush === 2 ? F_FINISH : (nodeFlush === 1 ? F_SYNC : F_NONE);
    return nodeFlush === 1 ? F_SYNC : F_NONE;
  };
  // node's `.flush()` default (ZlibBase _defaultFullFlushFlag): Z_FULL_FLUSH for
  // the zlib family, the single flush operation for brotli/zstd.
  const defaultFullFlush = (kind) => (kind === K_DEFLATE || kind === K_INFLATE) ? 3 : 1;

  // node lib/zlib.js smuggles a flush request through the write queue as a
  // zero-length chunk carrying the flush flag, so `.flush()` is ordered against
  // `.write()` instead of jumping the queue (test-zlib-flush-write-sync-interleaved).
  const kFlushFlag = G.Symbol("kFlushFlag");
  const flushChunks = new Map();
  const flushChunk = (nodeFlush) => {
    let b = flushChunks.get(nodeFlush);
    if (b === undefined) {
      b = Buffer.from(new ArrayBuffer(0));
      b[kFlushFlag] = nodeFlush;
      flushChunks.set(nodeFlush, b);
    }
    return b;
  };

  const b64 = (u8) => { if (!u8 || u8.length === 0) return ""; let s = ""; for (let i = 0; i < u8.length; i += 8192) s += String.fromCharCode.apply(null, u8.subarray(i, Math.min(i + 8192, u8.length))); return G.btoa(s); };
  const toBytes = (chunk, enc) => {
    if (typeof chunk === "number") return Buffer.from([chunk & 0xff]);
    if (typeof chunk === "string") return Buffer.from(chunk, enc && enc !== "buffer" ? enc : "utf8");
    if (Buffer.isBuffer(chunk)) return chunk;
    if (ArrayBuffer.isView(chunk)) return Buffer.from(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    if (chunk instanceof ArrayBuffer) return Buffer.from(chunk);
    return Buffer.from(chunk);
  };

  // node-style option/arg validation, blueprint lib/zlib.js checkRangeOrGetDefault
  // + internal/errors ERR_INVALID_ARG_TYPE / ERR_OUT_OF_RANGE.
  //  - undefined/null  → default
  //  - non-number      → TypeError  ERR_INVALID_ARG_TYPE  (property vs argument
  //                      chosen by whether `name` contains a '.')
  //  - ±Infinity       → RangeError "It must be a finite number"
  //  - out of [min,max]→ RangeError "It must be >= min[ and <= max]"
  //                      (upper omitted when max === Infinity)
  const fmtRecv = (v) => {
    if (v === null) return "null";
    if (v === undefined) return "undefined";
    const t = typeof v;
    if (t === "string") { let s = v; if (s.length > 25) s = s.slice(0, 25) + "..."; return "type string ('" + s + "')"; }
    if (t === "number" || t === "boolean" || t === "bigint") return "type " + t + " (" + String(v) + ")";
    if (t === "function") return v.name ? "function " + v.name : "an instance of Function";
    if (t === "object") { const cn = v.constructor && v.constructor.name; return "an instance of " + (cn || "Object"); }
    return "type " + t;
  };
  const errType = (name, expected, v) => {
    const kind = name.indexOf(".") >= 0 ? "property" : "argument";
    const e = new TypeError('The "' + name + '" ' + kind + " must be " + expected + ". Received " + fmtRecv(v));
    e.code = "ERR_INVALID_ARG_TYPE";
    return e;
  };
  const errRange = (name, rangeMsg, v) => {
    const e = new RangeError('The value of "' + name + '" is out of range. It must be ' + rangeMsg + ". Received " + String(v));
    e.code = "ERR_OUT_OF_RANGE";
    return e;
  };
  const rangeStr = (min, max) => max === Infinity ? ">= " + min : ">= " + min + " and <= " + max;
  const checkNum = (v, name, min, max, def) => {
    if (v === undefined || v === null) return def;
    if (typeof v !== "number") throw errType(name, "of type number", v);
    if (!Number.isFinite(v)) throw errRange(name, "a finite number", v);
    if (v < min || v > max) throw errRange(name, rangeStr(min, max), v);
    return v;
  };
  // level & strategy additionally treat NaN as "use default" (node behaviour:
  // createGzip({level:NaN}) yields _level === Z_DEFAULT_COMPRESSION).
  const checkLevel = (v, name, min, max, def) => {
    if (v === undefined || v === null) return def;
    if (typeof v !== "number") throw errType(name, "of type number", v);
    if (Number.isNaN(v)) return def;
    if (!Number.isFinite(v)) throw errRange(name, "a finite number", v);
    if (v < min || v > max) throw errRange(name, rangeStr(min, max), v);
    return v;
  };
  const vopt = (opts, name, min, max, def) => checkNum(opts ? opts[name] : undefined, "options." + name, min, max, def);

  // raw = -mag, zlib = mag, gzip = mag + 16, auto-detect (unzip) = mag + 32.
  const wbits = (fmt, mag, decompress) => {
    if (mag < 9 || mag > 15) mag = 15;
    if (fmt === "raw") return -mag;
    if (fmt === "gzip") return mag + 16;
    if (fmt === "auto") return decompress ? mag + 32 : mag;
    return mag;
  };

  // Abstract identity ctor: a prototype-chain link with the right .name so the
  // node class hierarchy checks (Class.prototype.__proto__.constructor.name)
  // resolve to "Zlib" / "Brotli" / "Zstd".
  const makeAbstract = (name, ParentProto, ParentCtor) => {
    function Ctor() {}
    Ctor.prototype = Object.create(ParentProto);
    Object.setPrototypeOf(Ctor, ParentCtor);
    Object.defineProperty(Ctor.prototype, "constructor", { value: Ctor, configurable: true, writable: true });
    Object.defineProperty(Ctor, "name", { value: name, configurable: true });
    return Ctor;
  };

  const ZlibBase = makeAbstract("ZlibBase", Transform.prototype, Transform);
  const Zlib = makeAbstract("Zlib", ZlibBase.prototype, ZlibBase);
  const Brotli = makeAbstract("Brotli", ZlibBase.prototype, ZlibBase);
  const Zstd = makeAbstract("Zstd", ZlibBase.prototype, ZlibBase);

  // ---- options.params validation (node lib/zlib.js Brotli/Zstd constructors) ----
  // The accepted key space is derived from the exported constants exactly the way
  // node derives kMaxBrotliParam / kMaxZstd{C,D}Param, so a key outside it (or a
  // key seen twice, e.g. "0" and "00") is ERR_BROTLI_INVALID_PARAM /
  // ERR_ZSTD_INVALID_PARAM. A key that is in range but carries a value the codec
  // itself rejects surfaces as ERR_ZLIB_INITIALIZATION_FAILED, matching the
  // failed BrotliEncoderSetParameter / ZSTD_CCtx_setParameter node reports.
  const zc = (zmod.constants || {});
  const maxParamWithPrefix = (prefix) => {
    let max = 0;
    for (const k of Object.keys(zc)) if (k.startsWith(prefix) && zc[k] > max) max = zc[k];
    return max;
  };
  const maxBrotliParam = maxParamWithPrefix("BROTLI_PARAM_");
  const maxZstdCParam = maxParamWithPrefix("ZSTD_c_");
  const maxZstdDParam = maxParamWithPrefix("ZSTD_d_");
  const errInvalidParam = (code, origKey, what) => {
    const e = new RangeError(origKey + " is not a valid " + what + " parameter");
    e.code = code;
    return e;
  };
  const errInitFailed = (message) => {
    const e = new Error(message);
    e.code = "ERR_ZLIB_INITIALIZATION_FAILED";
    return e;
  };
  // brotli encoder.c BrotliEncoderSetParameter: the two boolean flags and
  // NPOSTFIX reject out-of-range values, which node reports as a failed init.
  const brotliSetParam = (key, value) => {
    if ((key === 4 || key === 6) && value !== 0 && value !== 1) throw errInitFailed("Initialization failed");
    if (key === 7 && (value < 0 || value > 3)) throw errInitFailed("Initialization failed");
  };
  // ZSTD_CCtx_setParameter / ZSTD_DCtx_setParameter bounds (ZSTD_cParam_getBounds).
  const zstdSetCParam = (key, value) => {
    if (key === 107 && (value < 0 || value > 9)) throw errInitFailed("Setting parameter failed");   // ZSTD_c_strategy
  };
  const zstdSetDParam = (key, value) => {
    if (key === 100 && (value < 10 || value > 31)) throw errInitFailed("Setting parameter failed"); // ZSTD_d_windowLogMax
  };
  const flushMax = (kind) => (kind === K_BENC || kind === K_BDEC) ? 3 : ((kind === K_ZENC || kind === K_ZDEC) ? 2 : 5);
  const checkParams = (o, maxParam, code, what, setParam) => {
    const p = (o && o.params) || {};
    const seen = new Set();
    const out = {};
    for (const origKey of Object.keys(p)) {
      const key = +origKey;
      if (Number.isNaN(key) || key < 0 || key > maxParam || seen.has(key)) throw errInvalidParam(code, origKey, what);
      seen.add(key);
      const pv = p[origKey];
      if (typeof pv !== "number" && typeof pv !== "boolean") throw errType("options.params[" + origKey + "]", "of type number", pv);
      const value = typeof pv === "boolean" ? (pv ? 1 : 0) : pv;
      setParam(key, value);
      out[key] = value;
    }
    return out;
  };

  // ---- shared behaviour on ZlibBase.prototype ----
  const proto = ZlibBase.prototype;

  proto._zOpen = function () {
    const c = this._zcfg, o = this._zopts;
    if (c.kind === K_DEFLATE || c.kind === K_INFLATE) {
      const isDec = c.kind === K_INFLATE;
      // gzip requires windowBits >= 9; zlib/raw allow >= 8. On the decompression
      // side windowBits 0 is special (auto-detect from the stream header).
      const wbMin = c.fmt === "gzip" ? 9 : 8;
      const wbRaw = o ? o.windowBits : undefined;
      const mag = (isDec && wbRaw === 0) ? 15 : checkNum(wbRaw, "options.windowBits", wbMin, 15, 15);
      const level = checkLevel(o ? o.level : undefined, "options.level", -1, 9, -1);
      const memLevel = checkNum(o ? o.memLevel : undefined, "options.memLevel", 1, 9, 8);
      const strategy = checkLevel(o ? o.strategy : undefined, "options.strategy", 0, 4, 0);
      this._level = level;
      this._strategy = strategy;
      return ZN.streamOpen(c.kind, wbits(c.fmt, mag, isDec), level, memLevel, strategy, -1, 0, 0);
    }
    if (c.kind === K_BENC || c.kind === K_BDEC) {
      // Brotli param values must be numbers or booleans (node coerces booleans).
      const p = checkParams(o, maxBrotliParam, "ERR_BROTLI_INVALID_PARAM", "Brotli", brotliSetParam);
      const q = typeof p[1] === "number" ? p[1] : -1;   // BROTLI_PARAM_QUALITY
      const lg = typeof p[2] === "number" ? p[2] : 0;    // BROTLI_PARAM_LGWIN
      const md = typeof p[0] === "number" ? p[0] : 0;    // BROTLI_PARAM_MODE
      if (c.kind === K_BDEC) return ZN.streamOpen(K_BDEC, 0, -1, 8, 0, -1, 0, 0);
      return ZN.streamOpen(K_BENC, 0, -1, 8, 0, q, lg, md);
    }
    if (c.kind === K_ZENC) {
      const p = checkParams(o, maxZstdCParam, "ERR_ZSTD_INVALID_PARAM", "zstd", zstdSetCParam);
      let level = 3;
      if (o && typeof o.level === "number") level = o.level;
      else if (typeof p[100] === "number") level = p[100];   // ZSTD_c_compressionLevel
      const pledged = checkNum(o ? o.pledgedSrcSize : undefined, "options.pledgedSrcSize", 0, MAXS, undefined);
      const h = ZN.streamOpen(K_ZENC, 0, level, 8, 0, -1, 0, 0);
      // node's `pledgedSrcSize` goes into the frame header, so it has to be set
      // before the first byte is compressed — not on the first write.
      if (h >= 0 && pledged !== undefined && typeof ZN.streamZstdPledged === "function")
        ZN.streamZstdPledged(h, pledged);
      return h;
    }
    checkParams(o, maxZstdDParam, "ERR_ZSTD_INVALID_PARAM", "zstd", zstdSetDParam);
    return ZN.streamOpen(K_ZDEC, 0, -1, 8, 0, -1, 0, 0);
  };

  proto._zInit = function (cfg, opts) {
    this._zcfg = cfg;
    this._zopts = opts || {};
    // NOTE: do NOT name this `_decoder` — node's streams1 convention reserves
    // `stream._decoder` for a StringDecoder instance, and consumers like
    // raw-body reject any stream whose `_decoder` is truthy ("stream encoding
    // should not be set"). Use a private, non-colliding flag name instead.
    this._zIsDecoder = cfg.kind === K_INFLATE || cfg.kind === K_BDEC || cfg.kind === K_ZDEC;
    this._chunkSize = vopt(opts, "chunkSize", 64, Infinity, 16384);
    this._maxOutputLength = undefined;   // no cap unless the option asks for one
    if (opts) {
      // SECURITY: this used to be "validate only" — the option was range-checked
      // and then thrown away, so every streaming decompressor
      // (createGunzip/createInflate/createInflateRaw/createBrotliDecompress/
      // createZstdDecompress/createUnzip) ignored the cap and expanded a zip bomb
      // in full. maxOutputLength IS the documented defence for decompressing
      // untrusted input, and the streaming API is where untrusted input arrives;
      // only the one-shot sync/async helpers were enforcing it. A control that
      // reports success while doing nothing is worse than an absent one, because
      // code auditing as bounded was not.
      // node lib/zlib.js: _maxOutputLength is checked against the running output
      // total on every produced chunk and raises ERR_BUFFER_TOO_LARGE.
      this._maxOutputLength = vopt(opts, "maxOutputLength", 0, Infinity, undefined);
      // Each family has its own flush enum: zlib Z_NO_FLUSH..Z_BLOCK (0..5),
      // brotli BROTLI_OPERATION_PROCESS..EMIT_METADATA (0..3), zstd
      // ZSTD_e_continue..ZSTD_e_end (0..2).
      const maxFlush = flushMax(cfg.kind);
      vopt(opts, "flush", 0, maxFlush, 0);
      vopt(opts, "finishFlush", 0, maxFlush, Math.min(4, maxFlush));
      if (opts.dictionary !== undefined && opts.dictionary !== null &&
          !ArrayBuffer.isView(opts.dictionary) && !(opts.dictionary instanceof ArrayBuffer) &&
          !(G.SharedArrayBuffer && opts.dictionary instanceof G.SharedArrayBuffer)) {
        throw errType("options.dictionary", "an instance of Buffer, TypedArray, DataView, or ArrayBuffer", opts.dictionary);
      }
    }
    this._zOutLen = 0;             // running decompressed-output total for the cap
    this._bytesWritten = 0;
    this._zEnded = false;
    this._zErrored = false;
    this._zWritePending = false;
    this._defaultFullFlushFlag = defaultFullFlush(cfg.kind);
    this._h = this._zOpen();
    // Optional inflate dictionary (node `dictionary` option).
    if (this._h >= 0 && cfg.kind === K_INFLATE && opts && opts.dictionary) {
      try { ZN.streamDict(this._h, b64(toBytes(opts.dictionary))); } catch (e) {}
    }
    // node exposes the native codec as `stream._handle` and derives `_closed`
    // from it (`ZlibBase.prototype._closed` is a getter for `!this._handle`), so
    // the lifecycle every zlib test observes IS this object: destroy() nulls it,
    // .reset()/.params() go through it, and resetting mid-write throws rather
    // than reaching a codec the threadpool still owns. It is a JS shim over the
    // handle id in `_h` rather than a separate native object.
    const self = this;
    this._handle = {
      reset() {
        if (self._zWritePending)
          throw new Error("Cannot reset zlib stream while a write is in progress");
        self._zEnded = false;
        self._zErrored = false;
        self._bytesWritten = 0;
        if (self._h >= 0 && !ZN.streamReset(self._h)) {
          ZN.streamClose(self._h);
          self._h = self._zOpen();
        }
      },
      params(level, strategy) {
        if (self._h >= 0) ZN.streamParams(self._h, level, strategy);
      },
      close() {
        if (self._h >= 0) { try { ZN.streamClose(self._h); } catch (e) {} }
        self._h = -1;
      },
    };
  };

  proto._pushChunked = function (u8) {
    if (!u8 || u8.length === 0) return;
    const cs = this._chunkSize || 16384;
    if (u8.length <= cs) { this.push(Buffer.from(u8)); return; }
    for (let i = 0; i < u8.length; i += cs) this.push(Buffer.from(u8.subarray(i, Math.min(i + cs, u8.length))));
  };

  // Feed `bytes` through the native handle with the given flush mode. Pushes any
  // produced output, tracks consumed bytes and end-of-stream. Returns an Error
  // (to hand to a stream callback → 'error' event) or null.
  // Feed the native codec in bounded slices. The JS↔native bridge marshals
  // payloads as base64, so passing a whole multi-MB write through in one call
  // builds a huge transient string (and mbun's base64 encoder is memory-heavy);
  // a 2 GiB streamed write would OOM the process. The native codec is stateful
  // and format-agnostic to write boundaries under Z_NO_FLUSH, so slicing is
  // semantically identical while keeping memory constant — real streaming.
  const Z_SLICE = 1 << 20;  // 1 MiB in → ~1.4 MiB base64, bounded
  proto._zStep = function (piece, flushMode) {
    let res;
    try { res = ZN.streamProcess(this._h, piece && piece.length ? b64(piece) : EMPTY, flushMode); }
    catch (e) { return this._zMakeErr(String(e && e.message || e)); }
    if (!res || !res.ok) return this._zMakeErr((res && res.message) || "zlib stream error", res && res.code);
    this._bytesWritten += res.consumed | 0;
    if (res.b64) {
      const produced = Buffer.from(res.b64, "base64");
      // Enforce maxOutputLength on the running total BEFORE pushing, so a bomb is
      // stopped at the cap instead of after the whole expansion is in memory.
      // Decoders only: the cap exists to bound what a compressed input can expand
      // to, which is also the direction mbun's one-shot helpers check.
      if (this._zIsDecoder && this._maxOutputLength !== undefined) {
        this._zOutLen += produced.length;
        if (this._zOutLen > this._maxOutputLength) {
          this._zErrored = true;
          const e = new RangeError("Cannot create a Buffer larger than " +
                                   this._maxOutputLength + " bytes");
          e.code = "ERR_BUFFER_TOO_LARGE";
          return e;
        }
      }
      this._pushChunked(produced);
    }
    if (res.streamEnd) { this._zEnded = true; this.push(null); }
    return null;
  };

  proto._zRun = function (bytes, flushMode) {
    if (this._zEnded || this._zErrored || this._h < 0) return null;
    const total = bytes ? bytes.length : 0;
    if (total <= Z_SLICE) return this._zStep(bytes, flushMode);
    for (let off = 0; off < total; off += Z_SLICE) {
      const end = Math.min(off + Z_SLICE, total);
      // The requested flush mode applies only to the final slice; the rest feed
      // through with Z_NO_FLUSH so no premature flush/finish is emitted.
      const err = this._zStep(bytes.subarray(off, end), end >= total ? flushMode : F_NONE);
      if (err) return err;
      if (this._zEnded) break;  // decoder hit end-of-stream: ignore any trailing bytes
    }
    return null;
  };

  proto._zMakeErr = function (msg, code) {
    this._zErrored = true;
    const e = new Error(msg || "zlib error");
    // The engine's own code when the bridge reported one (Z_NEED_DICT,
    // ERR_BROTLI_DECODER_*, ZSTD_error_*); Z_DATA_ERROR is only the fallback.
    e.errno = -3; e.code = code || "Z_DATA_ERROR";
    return e;
  };

  proto._transform = function (chunk, enc, cb) {
    let flushMode = F_NONE;
    if (chunk != null && typeof chunk[kFlushFlag] === "number")
      flushMode = flushOrdinal(this._zcfg.kind, chunk[kFlushFlag]);
    const bytes = toBytes(chunk, enc);
    const self = this;
    // node dispatches every zlib write to the threadpool, so the write is still
    // "in progress" when write() returns. Tests depend on that: a stream with a
    // small highWaterMark must report needDrain before the write lands
    // (test-zlib-flush-drain) and .reset() during a pending write must throw
    // (test-zlib-reset-during-write). Completing synchronously would make both
    // unobservable, so the codec step is deferred a tick.
    this._zWritePending = true;
    G.process.nextTick(function () {
      const err = self._zRun(bytes, flushMode);
      self._zWritePending = false;
      cb(err);
    });
  };

  proto._flush = function (cb) {
    if (this._zErrored || this._zEnded || this.destroyed || this._h < 0) { cb(); return; }
    const err = this._zRun(EMPTY, F_FINISH);
    if (err) { cb(err); return; }
    if (this._zIsDecoder && !this._zEnded) {
      const e = new Error("unexpected end of file");
      e.errno = -5; e.code = "Z_BUF_ERROR";
      cb(e);
      return;
    }
    cb();
  };

  // node's synchronous internal: run the whole buffer through the codec with the
  // given zlib flush flag and return the concatenated output (used by the sync
  // convenience helpers and directly by consumers like test-zlib-sync-no-event).
  proto._processChunk = function (chunk, flushFlag) {
    const bytes = toBytes(chunk);
    const out = [];
    const savedPush = this.push;
    this.push = function (b) { if (b != null) out.push(Buffer.isBuffer(b) ? b : Buffer.from(b)); return true; };
    let err;
    try { err = this._zRun(bytes, flushFlag === 4 ? F_FINISH : F_SYNC); }  // Z_FINISH === 4
    finally { this.push = savedPush; }
    if (err) throw err;
    return out.length === 1 ? out[0] : Buffer.concat(out);
  };

  // NOTE: node:stream's Transform (the 1:1 port in node_stream_*.cppm) drives the
  // _transform/_flush pipeline itself, so this partition deliberately does NOT
  // override write()/end() any more. It used to — back when `M["stream"]` was
  // bootstrap's stub — and that hand-rolled pipeline was what kept _writableState,
  // needDrain, ordered flushes, autoDestroy and the destroy/close lifecycle out of
  // reach of node:zlib. Everything below is now the node lib/zlib.js surface
  // layered on top of the real stream machinery.

  // node lib/zlib.js: `_closed` is a getter over the handle, not a stored flag,
  // so a stream that errored (error → destroy → _destroy nulls the handle)
  // reports closed by the time the 'error' listener runs.
  Object.defineProperty(proto, "_closed", {
    configurable: true,
    enumerable: true,
    get: function () { return !this._handle; },
  });

  // node lib/zlib.js _close(engine): close the codec and drop the handle.
  const zClose = function (self) {
    if (self._handle) { self._handle.close(); self._handle = null; }
    self._h = -1;
    self._zEnded = true;
  };

  proto._destroy = function (err, cb) {
    zClose(this);
    cb(err);
  };

  // node lib/zlib.js ZlibBase.close: wait for the stream to finish, then destroy.
  proto.close = function (cb) {
    if (typeof cb === "function") finished(this, cb);
    this.destroy();
    return this;
  };

  // node lib/zlib.js ZlibBase.flush: a flush is a zero-length write carrying the
  // flush flag, so it is ordered against the writes around it.
  proto.flush = function (kind, cb) {
    if (typeof kind === "function" || (kind === undefined && !cb)) {
      cb = kind;
      kind = this._defaultFullFlushFlag;
    }
    if (this.writableFinished) {
      if (typeof cb === "function") G.process.nextTick(cb);
    } else if (this.writableEnded) {
      if (typeof cb === "function") this.once("end", cb);
    } else {
      this.write(flushChunk(kind), "", cb);
    }
    return this;
  };

  // node lib/zlib.js ZlibBase.reset: asserts the handle is live, then resets it.
  proto.reset = function () {
    if (!this._handle) throw new Error("zlib binding closed");
    this._handle.reset();
    return this;
  };

  // node lib/zlib.js Zlib.params: flush what is buffered, then change the live
  // codec's level/strategy with deflateParams — the stream continues, so the
  // output must NOT restart with a fresh zlib header (test-zlib-params).
  proto.params = function (level, strategy, cb) {
    checkNum(level, "level", -1, 9, undefined);
    checkNum(strategy, "strategy", 0, 4, undefined);
    if (this._level !== level || this._strategy !== strategy) {
      const self = this;
      this.flush(2 /* Z_SYNC_FLUSH */, function () {
        if (!self._handle) throw new Error("zlib binding closed");
        self._handle.params(level, strategy);
        if (!self._zErrored) {
          self._level = level;
          self._strategy = strategy;
          if (typeof cb === "function") cb();
        }
      });
    } else if (typeof cb === "function") {
      G.process.nextTick(cb);
    }
  };

  Object.defineProperty(proto, "bytesWritten", {
    get: function () { return this._bytesWritten || 0; },
    configurable: true,
  });

  // node lib/zlib.js ZlibBase: the user's options are forwarded to Transform
  // (highWaterMark matters — test-zlib-flush-drain sets 16), with autoDestroy on
  // and the object/encoding modes forced off because a codec is byte-oriented.
  const transformOpts = (opts) => {
    const o = Object.assign({ autoDestroy: true }, opts && typeof opts === "object" ? opts : null);
    if (o.encoding || o.objectMode || o.writableObjectMode) {
      o.encoding = null; o.objectMode = false; o.writableObjectMode = false;
    }
    return o;
  };

  // ---- leaf constructors (callable with or without `new`) ----
  const makeLeaf = (name, Parent, cfg) => {
    let warnedNoNew = false;
    function Ctor(opts) {
      // Called as a plain function on an object that already inherits from Ctor
      // (`DeflateRaw.call(this, options)` from a setPrototypeOf-style subclass):
      // initialise THAT object. Reflect.construct used to build a fresh instance
      // and return it, which such a caller discards — leaving its own object
      // without any stream state (test-zlib-deflate-raw-inherits).
      if (!(this instanceof Ctor)) {
        // node internal/util deprecateInstantiation (DEP0184), emitted once per
        // class: `zlib.Gzip()` still works but warns.
        if (!warnedNoNew) {
          warnedNoNew = true;
          try {
            G.process.emitWarning("Instantiating " + name + " without the 'new' keyword has been deprecated.",
                                  "DeprecationWarning", "DEP0184");
          } catch (e) {}
        }
        return new Ctor(opts);
      }
      Transform.call(this, transformOpts(opts));
      this._zInit(cfg, opts);
      return this;
    }
    Ctor.prototype = Object.create(Parent.prototype);
    Object.setPrototypeOf(Ctor, Parent);
    Object.defineProperty(Ctor.prototype, "constructor", { value: Ctor, configurable: true, writable: true });
    Object.defineProperty(Ctor, "name", { value: name, configurable: true });
    return Ctor;
  };

  const Deflate = makeLeaf("Deflate", Zlib, { kind: K_DEFLATE, fmt: "zlib" });
  const Inflate = makeLeaf("Inflate", Zlib, { kind: K_INFLATE, fmt: "zlib" });
  const Gzip = makeLeaf("Gzip", Zlib, { kind: K_DEFLATE, fmt: "gzip" });
  const Gunzip = makeLeaf("Gunzip", Zlib, { kind: K_INFLATE, fmt: "gzip" });
  const DeflateRaw = makeLeaf("DeflateRaw", Zlib, { kind: K_DEFLATE, fmt: "raw" });
  const InflateRaw = makeLeaf("InflateRaw", Zlib, { kind: K_INFLATE, fmt: "raw" });
  const Unzip = makeLeaf("Unzip", Zlib, { kind: K_INFLATE, fmt: "auto" });
  const BrotliCompress = makeLeaf("BrotliCompress", Brotli, { kind: K_BENC });
  const BrotliDecompress = makeLeaf("BrotliDecompress", Brotli, { kind: K_BDEC });
  const ZstdCompress = makeLeaf("ZstdCompress", Zstd, { kind: K_ZENC });
  const ZstdDecompress = makeLeaf("ZstdDecompress", Zstd, { kind: K_ZDEC });

  const patch = {
    Gzip, Gunzip, Deflate, Inflate, DeflateRaw, InflateRaw, Unzip,
    BrotliCompress, BrotliDecompress, ZstdCompress, ZstdDecompress,
    createGzip: (o) => new Gzip(o),
    createGunzip: (o) => new Gunzip(o),
    createDeflate: (o) => new Deflate(o),
    createInflate: (o) => new Inflate(o),
    createDeflateRaw: (o) => new DeflateRaw(o),
    createInflateRaw: (o) => new InflateRaw(o),
    createUnzip: (o) => new Unzip(o),
    createBrotliCompress: (o) => new BrotliCompress(o),
    createBrotliDecompress: (o) => new BrotliDecompress(o),
    createZstdCompress: (o) => new ZstdCompress(o),
    createZstdDecompress: (o) => new ZstdDecompress(o),
  };
  for (const k of Object.keys(patch)) {
    try { Object.defineProperty(zmod, k, { value: patch[k], writable: true, enumerable: true, configurable: true }); }
    catch (e) { zmod[k] = patch[k]; }
  }

  // ---- one-shot decompression with a non-default finishFlush ----------------
  // node's convenience helpers are `zlibBuffer(new Ctor(opts), …)`, so
  // `finishFlush: Z_SYNC_FLUSH` makes a truncated stream yield whatever decoded
  // before the input ran out instead of erroring (test-zlib-truncated). The
  // bootstrap one-shots call the whole-buffer natives, which have no such mode,
  // so route just that case through the incremental handle here.
  const decodeThroughHandle = (cfg, data, opts, finalFlush) => {
    const isInflate = cfg.kind === K_INFLATE;
    const mag = isInflate ? checkNum(opts ? opts.windowBits : undefined, "options.windowBits", cfg.fmt === "gzip" ? 9 : 8, 15, 15) : 0;
    const h = ZN.streamOpen(cfg.kind, isInflate ? wbits(cfg.fmt, mag, true) : 0, -1, 8, 0, -1, 0, 0);
    if (h < 0) throw errInitFailed("Initialization failed");
    try {
      const bytes = toBytes(data);
      const out = [];
      let off = 0, ended = false;
      do {
        const end = Math.min(off + Z_SLICE, bytes.length);
        const res = ZN.streamProcess(h, end > off ? b64(bytes.subarray(off, end)) : EMPTY, end >= bytes.length ? finalFlush : F_NONE);
        if (!res || !res.ok) { const e = new Error((res && res.message) || "zlib stream error"); e.errno = -3; e.code = (res && res.code) || "Z_DATA_ERROR"; throw e; }
        if (res.b64) out.push(Buffer.from(res.b64, "base64"));
        if (res.streamEnd) { ended = true; break; }
        off = end;
      } while (off < bytes.length);
      // Same end-of-stream check _flush performs: a decoder that never reached
      // the codec's stream end ran out of input mid-member.
      if (finalFlush === F_FINISH && !ended) { const e = new Error("unexpected end of file"); e.errno = -5; e.code = "Z_BUF_ERROR"; throw e; }
      return out.length === 1 ? out[0] : Buffer.concat(out);
    } finally { try { ZN.streamClose(h); } catch (e) {} }
  };
  // The one-shot helpers (deflateSync/gzipSync/gunzipSync/…) call the
  // whole-buffer natives directly, which take the options as plain numbers and
  // range-check nothing: `zlib.gzipSync(buf, { windowBits: 8 })` silently used 8
  // where the gzip framing needs >= 9, instead of node's ERR_OUT_OF_RANGE. node
  // builds each one-shot out of the same class as the streaming API, so it gets
  // one validator; this is that validator, shared with the Transform path above.
  const validateZlibOpts = function (cfg, opts) {
    if (!opts || typeof opts !== "object") return;
    if (cfg.kind === K_DEFLATE || cfg.kind === K_INFLATE) {
      const isDec = cfg.kind === K_INFLATE;
      // windowBits 0 on the decompression side means "read it from the header".
      if (!(isDec && opts.windowBits === 0))
        checkNum(opts.windowBits, "options.windowBits", cfg.fmt === "gzip" ? 9 : 8, 15, 15);
      checkLevel(opts.level, "options.level", -1, 9, -1);
      checkNum(opts.memLevel, "options.memLevel", 1, 9, 8);
      checkLevel(opts.strategy, "options.strategy", 0, 4, 0);
    }
    vopt(opts, "chunkSize", 64, Infinity, 16384);
    const maxFlushVal = flushMax(cfg.kind);
    vopt(opts, "flush", 0, maxFlushVal, 0);
    vopt(opts, "finishFlush", 0, maxFlushVal, Math.min(4, maxFlushVal));
  };
  const compressorOneShots = {
    deflateSync: { kind: K_DEFLATE, fmt: "zlib" },
    deflateRawSync: { kind: K_DEFLATE, fmt: "raw" },
    gzipSync: { kind: K_DEFLATE, fmt: "gzip" },
  };
  const compressorAsyncOf = { deflateSync: "deflate", deflateRawSync: "deflateRaw", gzipSync: "gzip" };
  for (const name of Object.keys(compressorOneShots)) {
    const cfg = compressorOneShots[name];
    const orig = zmod[name];
    if (typeof orig !== "function") continue;
    const sync = function (data, opts) { validateZlibOpts(cfg, opts); return orig(data, opts); };
    const async = function (data, opts, cb) {
      if (typeof opts === "function") { cb = opts; opts = undefined; }
      if (typeof cb !== "function") throw errType("callback", "of type function", cb);
      validateZlibOpts(cfg, opts);
      G.queueMicrotask(function () { let r; try { r = orig(data, opts); } catch (e) { cb(e); return; } cb(null, r); });
    };
    try { Object.defineProperty(zmod, name, { value: sync, writable: true, enumerable: true, configurable: true }); } catch (e) { zmod[name] = sync; }
    const an = compressorAsyncOf[name];
    if (an && typeof zmod[an] === "function") {
      try { Object.defineProperty(zmod, an, { value: async, writable: true, enumerable: true, configurable: true }); } catch (e) { zmod[an] = async; }
    }
  }

  const decoderOneShots = {
    inflateSync: { kind: K_INFLATE, fmt: "zlib", Engine: Inflate },
    inflateRawSync: { kind: K_INFLATE, fmt: "raw", Engine: InflateRaw },
    gunzipSync: { kind: K_INFLATE, fmt: "gzip", Engine: Gunzip },
    unzipSync: { kind: K_INFLATE, fmt: "auto", Engine: Unzip },
    brotliDecompressSync: { kind: K_BDEC, Engine: BrotliDecompress },
    zstdDecompressSync: { kind: K_ZDEC, Engine: ZstdDecompress },
  };
  const asyncOf = { inflateSync: "inflate", inflateRawSync: "inflateRaw", gunzipSync: "gunzip", unzipSync: "unzip", brotliDecompressSync: "brotliDecompress", zstdDecompressSync: "zstdDecompress" };
  for (const name of Object.keys(decoderOneShots)) {
    const cfg = decoderOneShots[name];
    const orig = zmod[name];
    if (typeof orig !== "function") continue;
    const finishDefault = Math.min(4, flushMax(cfg.kind));   // Z_FINISH / BROTLI_OPERATION_FINISH / ZSTD_e_end
    const sync = function (data, opts) {
      validateZlibOpts(cfg, opts);
      if (opts && typeof opts === "object" && typeof opts.finishFlush === "number" && opts.finishFlush !== finishDefault) {
        const buf = decodeThroughHandle(cfg, data, opts, F_SYNC);
        return opts.info ? { buffer: buf, engine: Object.create(cfg.Engine.prototype) } : buf;
      }
      try { return orig(data, opts); }
      catch (e) {
        // The whole-buffer natives collapse every decode failure into one generic
        // message; node distinguishes truncated input ("unexpected end of file")
        // from corrupt data. Re-run the failure through the incremental codec,
        // which does report the distinction, purely to classify it. Only a codec
        // error is reclassified — an ERR_BUFFER_TOO_LARGE cap is not.
        if (!e || e.code !== "Z_DATA_ERROR") throw e;
        try { decodeThroughHandle(cfg, data, opts, F_FINISH); }
        catch (e2) { if (e2 && e2.message && e2.message !== e.message) throw e2; }
        throw e;
      }
    };
    const async = function (data, opts, cb) {
      if (typeof opts === "function") { cb = opts; opts = undefined; }
      if (typeof cb !== "function") throw errType("callback", "of type function", cb);
      G.queueMicrotask(function () { let r; try { r = sync(data, opts); } catch (e) { cb(e); return; } cb(null, r); });
    };
    try { Object.defineProperty(zmod, name, { value: sync, writable: true, enumerable: true, configurable: true }); } catch (e) { zmod[name] = sync; }
    const an = asyncOf[name];
    if (an && typeof zmod[an] === "function") {
      try { Object.defineProperty(zmod, an, { value: async, writable: true, enumerable: true, configurable: true }); } catch (e) { zmod[an] = async; }
    }
  }
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
