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
  const Buffer = G.Buffer;
  const EMPTY = "";
  const MAXS = Number.MAX_SAFE_INTEGER;

  // StreamKind ordinals (mirror mbun::compress::StreamKind).
  const K_DEFLATE = 0, K_INFLATE = 1, K_BENC = 2, K_BDEC = 3, K_ZENC = 4, K_ZDEC = 5;
  // StreamFlush ordinals.
  const F_NONE = 0, F_SYNC = 1, F_FINISH = 2;

  const b64 = (u8) => { if (!u8 || u8.length === 0) return ""; let s = ""; for (let i = 0; i < u8.length; i += 8192) s += String.fromCharCode.apply(null, u8.subarray(i, Math.min(i + 8192, u8.length))); return G.btoa(s); };
  const toBytes = (chunk, enc) => {
    if (typeof chunk === "number") return Buffer.from([chunk & 0xff]);
    if (typeof chunk === "string") return Buffer.from(chunk, enc && enc !== "buffer" ? enc : "utf8");
    if (Buffer.isBuffer(chunk)) return chunk;
    if (ArrayBuffer.isView(chunk)) return Buffer.from(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    if (chunk instanceof ArrayBuffer) return Buffer.from(chunk);
    return Buffer.from(chunk);
  };

  // node option validation: non-number → TypeError, out-of-range/non-integer →
  // RangeError, undefined → default (see checkRangeOrGetDefault in lib/zlib.js).
  const vopt = (opts, name, min, max, def) => {
    const v = opts ? opts[name] : undefined;
    if (v === undefined) return def;
    if (typeof v !== "number") {
      const e = new TypeError('The "options.' + name + '" property must be of type number. Received ' + (typeof v === "object" ? "an instance of Object" : "type " + typeof v));
      e.code = "ERR_INVALID_ARG_TYPE";
      throw e;
    }
    if (!Number.isFinite(v) || Math.floor(v) !== v || v < min || v > max) {
      const e = new RangeError('The value of "options.' + name + '" is out of range. It must be >= ' + min + " and <= " + max + ". Received " + v);
      e.code = "ERR_OUT_OF_RANGE";
      throw e;
    }
    return v;
  };

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

  // ---- shared behaviour on ZlibBase.prototype ----
  const proto = ZlibBase.prototype;

  proto._zOpen = function () {
    const c = this._zcfg, o = this._zopts;
    if (c.kind === K_DEFLATE || c.kind === K_INFLATE) {
      const mag = vopt(o, "windowBits", 8, 15, 15);
      const level = vopt(o, "level", -1, 9, -1);
      const memLevel = vopt(o, "memLevel", 1, 9, 8);
      const strategy = vopt(o, "strategy", 0, 7, 0);
      return ZN.streamOpen(c.kind, wbits(c.fmt, mag, c.kind === K_INFLATE), level, memLevel, strategy, -1, 0, 0);
    }
    if (c.kind === K_BENC) {
      const p = (o && o.params) || {};
      const q = typeof p[1] === "number" ? p[1] : -1;   // BROTLI_PARAM_QUALITY
      const lg = typeof p[2] === "number" ? p[2] : 0;    // BROTLI_PARAM_LGWIN
      const md = typeof p[0] === "number" ? p[0] : 0;    // BROTLI_PARAM_MODE
      return ZN.streamOpen(K_BENC, 0, -1, 8, 0, q, lg, md);
    }
    if (c.kind === K_BDEC) return ZN.streamOpen(K_BDEC, 0, -1, 8, 0, -1, 0, 0);
    if (c.kind === K_ZENC) {
      let level = 3;
      if (o && typeof o.level === "number") level = o.level;
      else { const p = (o && o.params) || {}; if (typeof p[100] === "number") level = p[100]; }
      return ZN.streamOpen(K_ZENC, 0, level, 8, 0, -1, 0, 0);
    }
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
    this._chunkSize = vopt(opts, "chunkSize", 64, MAXS, 16384);
    if (opts) vopt(opts, "maxOutputLength", 0, MAXS, undefined);  // validate only
    this._bytesWritten = 0;
    this._zEnded = false;
    this._zErrored = false;
    this._h = this._zOpen();
    // Optional inflate dictionary (node `dictionary` option).
    if (this._h >= 0 && cfg.kind === K_INFLATE && opts && opts.dictionary) {
      try { ZN.streamDict(this._h, b64(toBytes(opts.dictionary))); } catch (e) {}
    }
    // NOTE: the low-level native `_handle` (writeSync/write/onerror + writeState,
    // threadpool lifecycle, GC-estimated-size) is a separate concern and stays
    // DEFERRED — these streams drive the codec through _transform/_flush, not a
    // node-style _handle. Left unset so the native-handle test files see the
    // same (absent) surface as before rather than a misleading JS shim.
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
    if (!res || !res.ok) return this._zMakeErr((res && res.message) || "zlib stream error");
    this._bytesWritten += res.consumed | 0;
    if (res.b64) this._pushChunked(Buffer.from(res.b64, "base64"));
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

  proto._zMakeErr = function (msg) {
    this._zErrored = true;
    const e = new Error(msg || "zlib error");
    e.errno = -3; e.code = "Z_DATA_ERROR";
    return e;
  };

  proto._transform = function (chunk, enc, cb) {
    const err = this._zRun(toBytes(chunk, enc), F_NONE);
    cb(err);
  };

  proto._flush = function (cb) {
    if (this._zErrored) { cb(); return; }
    if (this._zEnded) { cb(); return; }
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

  // mbun's base Transform is a stub whose write()/end() do NOT drive the
  // _transform/_flush pipeline (see bootstrap Writable/Duplex). Implement the
  // pipeline here so writes are compressed/decompressed, output is emitted via
  // Readable.push ('data'), and the readable side ends (push(null) → 'end').
  proto.write = function (chunk, enc, cb) {
    if (typeof enc === "function") { cb = enc; enc = undefined; }
    if (this._writableEnded) { if (typeof cb === "function") G.queueMicrotask(cb); return false; }
    const self = this;
    // The write completion callback fires asynchronously, matching node/bun's
    // threadpool-backed zlib writes: callers that queue several writes before
    // the loop yields (e.g. the reset-race regression) rely on completions not
    // landing synchronously mid-loop.
    this._transform(chunk, enc, function (err) {
      if (err) self.emit("error", err);
      if (typeof cb === "function") G.queueMicrotask(function () { cb(err); });
    });
    return true;
  };

  proto.end = function (chunk, enc, cb) {
    if (typeof chunk === "function") { cb = chunk; chunk = undefined; enc = undefined; }
    else if (typeof enc === "function") { cb = enc; enc = undefined; }
    const self = this;
    const finish = function () {
      self._writableEnded = true;
      self._flush(function (err) {
        if (err) self.emit("error", err);
        self.emit("finish");
        // Emit 'close' only AFTER the readable side has ended, matching node's
        // stream teardown order (finish → end → close). `push(null)` schedules
        // 'end' asynchronously, so emitting 'close' synchronously here would
        // fire it before 'end'. raw-body attaches a 'close' listener whose
        // cleanup() removes the 'end'/'data' listeners, so a premature 'close'
        // strips 'end' before it fires and hangs the read (express gzip/deflate
        // request bodies). Note `_flush` may already have pushed null (decoder
        // reached stream-end), so 'end' can be pending even when _zEnded is set.
        if (!self._zClosed) {
          self._zClosed = true;
          self.once("end", function () { self.emit("close"); });
        }
        if (!self._zEnded) { self._zEnded = true; self.push(null); }  // → 'end'
        if (typeof cb === "function") cb(err);
      });
    };
    if (chunk != null) this.write(chunk, enc, finish);
    else finish();
    return this;
  };

  // node zlib .flush([kind], cb): emit everything buffered so far, then cb.
  proto.flush = function (kind, cb) {
    if (typeof kind === "function") { cb = kind; kind = F_SYNC; }
    const done = typeof cb === "function" ? cb : function () {};
    if (this._zEnded || this._zErrored || this._h < 0) { G.queueMicrotask(done); return; }
    const err = this._zRun(EMPTY, F_SYNC);
    if (err) { this.emit("error", err); G.queueMicrotask(done); return; }
    G.queueMicrotask(done);
  };

  // node zlib .reset(): return the codec to its initial state.
  proto.reset = function () {
    this._zEnded = false;
    this._zErrored = false;
    this._bytesWritten = 0;
    if (this._h >= 0 && !ZN.streamReset(this._h)) {
      ZN.streamClose(this._h);
      this._h = this._zOpen();
    }
    return this;
  };

  proto.close = function (cb) {
    if (this._h >= 0) { ZN.streamClose(this._h); this._h = -1; }
    if (typeof cb === "function") G.queueMicrotask(cb);
    return this;
  };

  proto.params = function (level, strategy, cb) {
    // Streaming param change: re-open honouring the new level/strategy.
    if (this._zopts) { this._zopts = Object.assign({}, this._zopts, { level: level, strategy: strategy }); }
    if (this._h >= 0) { ZN.streamClose(this._h); this._h = this._zOpen(); }
    if (typeof cb === "function") G.queueMicrotask(cb);
  };

  Object.defineProperty(proto, "bytesWritten", {
    get: function () { return this._bytesWritten || 0; },
    configurable: true,
  });

  // ---- leaf constructors (callable with or without `new`) ----
  const makeLeaf = (name, Parent, cfg) => {
    function Ctor(opts) {
      if (!(this instanceof Ctor)) return new Ctor(opts);
      const self = Reflect.construct(Transform, [{}], new.target || Ctor);
      self._zInit(cfg, opts);
      return self;
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
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
