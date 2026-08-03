// node:zlib/iter payload partition — the compression transforms behind
// require('zlib/iter'), i.e. internal/streams/iter/transform.
//
// Mechanical 1:1 translation of bun-ref
// src/js/internal/streams/iter/transform.ts (三段法 stage 1): the factories,
// option validation, batching and buffer bookkeeping are the blueprint's,
// verbatim.
//
// One seam is mbun's: the blueprint drives THREE bare native handle classes
//   NativeZlib / NativeBrotli / NativeZstd
// out of bun's node_zlib_binding.rs, obtained through
// $rust("node_zlib_binding.rs", …). mbun's codec bridge is
// __mbunZlibNative.stream* (mbun.compress.stream — streamOpen/streamProcess/
// streamClose, payloads as base64), which is a different shape: it consumes as
// much input as it can and returns ALL of the output, instead of filling a
// caller-provided output buffer and reporting availOut/availIn through a shared
// Uint32Array. So rather than rewrite the blueprint's ~150 lines of writeState
// bookkeeping — the part most likely to go subtly wrong — this partition
// implements the handle classes on top of the bridge and leaves the blueprint
// alone:
//
//   * output that does not fit in the caller's buffer is held in `_left` and
//     handed over on the next call, with availOut reported as 0, which is
//     exactly the "engine has more output" signal the blueprint loops on;
//   * `write()` is the blueprint's async, threadpool path. mbun's bridge is
//     synchronous, so it is deferred to a microtask: the processCallback must
//     not run before write() has returned to processInputAsync(), which is
//     where the promise it resolves is created.
//
// DEFERRED: engine error CODES for brotli and zstd. node reports
// `ERR_` + BrotliDecoderErrorString(...) and `ZSTD_error_<name>`; mbun's
// mbun.compress.stream reports only a message ("brotli decode failed"), so a
// corrupt-input error here carries the engine's message and node's errno/code
// only for the zlib family (Z_DATA_ERROR / Z_NEED_DICT). Surfacing the other two
// needs a `code` on mbun::compress::StreamChunk; until then
// test-stream-iter-transform-errors stays red rather than getting invented codes.
//
// NOTE: appended AFTER the master builtins IIFE has closed (see image_closure),
// so this is a self-contained IIFE that re-binds G = globalThis and pulls shared
// helpers off G.__mbunStreamReg.
export module mbun.jsc.js_builtins:node_zlib_iter;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeZlibIterJS = R"JS(
(function () {
  const G = globalThis;
  const R = G.__mbunStreamReg;
  const ZN = G.__mbunZlibNative;
  if (!R || !ZN || typeof ZN.streamOpen !== "function") return;
  const process = G.process;
  const Buffer = G.Buffer;
  const { $ERR_INVALID_ARG_TYPE, $ERR_OUT_OF_RANGE, $makeAbortError, $ERR_BROTLI_INVALID_PARAM, $ERR_ZSTD_INVALID_PARAM, $ERR_ZLIB_INITIALIZATION_FAILED } = R.H;

  // ---- the NativeZlib / NativeBrotli / NativeZstd shim over mbun.compress ----
  // StreamKind ordinals (mirror mbun::compress::StreamKind), StreamFlush
  // ordinals, and the base64 payload convention — all as in zlib_stream.cppm,
  // which drives the same bridge for node:zlib's Transform classes.
  const K_DEFLATE = 0, K_INFLATE = 1, K_BENC = 2, K_BDEC = 3, K_ZENC = 4, K_ZDEC = 5;
  const F_NONE = 0, F_SYNC = 1, F_FINISH = 2;
  const EMPTY_U8 = new Uint8Array(0);

  const b64 = (u8) => {
    if (!u8 || u8.length === 0) return "";
    let s = "";
    for (let i = 0; i < u8.length; i += 8192) s += String.fromCharCode.apply(null, u8.subarray(i, Math.min(i + 8192, u8.length)));
    return G.btoa(s);
  };
  const unb64 = (s) => (s ? new Uint8Array(Buffer.from(s, "base64")) : EMPTY_U8);
  const asU8 = (b) => (b instanceof Uint8Array ? b : new Uint8Array(b.buffer, b.byteOffset, b.byteLength));

  // raw = -mag, zlib = mag, gzip = mag + 16 (same table as zlib_stream.cppm).
  const wbits = (fmt, mag) => {
    if (mag < 9 || mag > 15) mag = 15;
    if (fmt === "raw") return -mag;
    if (fmt === "gzip") return mag + 16;
    return mag;
  };

  // The blueprint's param arrays are Uint32Array(...).fill(-1), so "unset" reads
  // back as 0xFFFFFFFF, not -1 — testing `>= 0` would hand the engine
  // 4294967295 as a compression level (which silently produced STORED zstd
  // frames: 31000 bytes in, 31009 out).
  const UNSET = 0xFFFFFFFF;
  const set = (params, key) => params !== undefined && params !== null && params[key] !== undefined && (params[key] >>> 0) !== UNSET;

  class Codec {
    constructor() {
      this._h = -1;
      this._ws = null;      // the caller's writeState Uint32Array
      this._cb = null;      // the caller's processCallback
      this._left = null;    // output the caller's buffer had no room for
      this._end = false;
      this._closed = false;
      this._errored = false;
      this.onerror = null;
      this.buffer = null;   // the blueprint parks the in-flight input here
    }

    // node's flush constants per engine -> mbun's three-valued StreamFlush.
    _flushOf(flush) { return flush === this._finishFlush ? F_FINISH : (flush === this._syncFlush ? F_SYNC : F_NONE); }

    _fail(res) {
      this._errored = true;
      const msg = (res && res.message) || "zlib stream error";
      // Only the zlib family has a code mbun's bridge can be trusted to imply
      // (see the DEFERRED note at the top of this partition).
      let errno, code;
      if (this._kind === K_DEFLATE || this._kind === K_INFLATE) {
        if (msg.indexOf("need dictionary") >= 0) { errno = 2; code = "Z_NEED_DICT"; }
        else { errno = -3; code = "Z_DATA_ERROR"; }
      }
      if (typeof this.onerror === "function") this.onerror(msg, errno, code);
    }

    // One engine step against the caller's output window. Reports availOut in
    // writeState[0] and the input left over in writeState[1], which is the
    // protocol the blueprint's onWriteComplete/processSyncInput loop on.
    _step(flush, inBuf, inOff, availIn, outBuf, outOff, availOutBefore) {
      const ws = this._ws;
      // Hand over buffered output first; no input is fed on such a call.
      if (this._left !== null) {
        const n = Math.min(this._left.length, availOutBefore);
        if (n > 0) asU8(outBuf).set(this._left.subarray(0, n), outOff);
        this._left = this._left.length > n ? this._left.subarray(n) : null;
        ws[0] = this._left !== null ? 0 : availOutBefore - n;
        ws[1] = availIn;
        return;
      }
      // A finished stream produces nothing more; reporting availOut > 0 ends the
      // caller's loop instead of re-finishing an ended codec.
      if (this._closed || this._h < 0 || (this._end && availIn === 0)) {
        ws[0] = availOutBefore;
        ws[1] = 0;
        return;
      }
      const view = availIn > 0 ? asU8(inBuf).subarray(inOff, inOff + availIn) : EMPTY_U8;
      let res;
      try { res = ZN.streamProcess(this._h, b64(view), this._flushOf(flush)); }
      catch (e) { this._fail({ message: String((e && e.message) || e) }); return; }
      if (!res || !res.ok) { this._fail(res); return; }
      if (res.streamEnd) this._end = true;
      const out = unb64(res.b64);
      const n = Math.min(out.length, availOutBefore);
      if (n > 0) asU8(outBuf).set(out.subarray(0, n), outOff);
      this._left = out.length > n ? out.subarray(n) : null;
      ws[1] = availIn - (res.consumed | 0);
      ws[0] = this._left !== null ? 0 : availOutBefore - n;
    }

    writeSync(flush, inBuf, inOff, availIn, outBuf, outOff, availOutBefore) {
      this._step(flush, inBuf, inOff, availIn, outBuf, outOff, availOutBefore);
    }

    write(flush, inBuf, inOff, availIn, outBuf, outOff, availOutBefore) {
      // The blueprint's threadpool path: the completion callback must not run
      // before write() returns (processInputAsync creates the promise it
      // resolves after this call).
      G.queueMicrotask(() => {
        if (this._closed) return;
        this._step(flush, inBuf, inOff, availIn, outBuf, outOff, availOutBefore);
        if (this._errored) return;  // onerror already rejected
        if (typeof this._cb === "function") this._cb();
      });
    }

    close() {
      if (this._h >= 0) { try { ZN.streamClose(this._h); } catch (e) {} }
      this._h = -1;
      this._closed = true;
      this._left = null;
      this.buffer = null;
    }
  }

  // mode -> [StreamKind, windowBits format]; node's zlib modes DEFLATE(1),
  // INFLATE(2), GZIP(3), GUNZIP(4), DEFLATERAW(5), INFLATERAW(6).
  const ZLIB_MODES = { 1: [K_DEFLATE, "zlib"], 2: [K_INFLATE, "zlib"], 3: [K_DEFLATE, "gzip"], 4: [K_INFLATE, "gzip"], 5: [K_DEFLATE, "raw"], 6: [K_INFLATE, "raw"] };

  class NativeZlib extends Codec {
    constructor(mode) {
      super();
      const m = ZLIB_MODES[mode];
      if (!m) throw $ERR_INVALID_ARG_TYPE("mode", "a zlib mode", mode);
      this._kind = m[0];
      this._fmt = m[1];
      this._syncFlush = 2;    // Z_SYNC_FLUSH
      this._finishFlush = 4;  // Z_FINISH
    }
    init(windowBits, level, memLevel, strategy, writeState, processCallback, dictionary) {
      this._ws = writeState;
      this._cb = processCallback;
      this._h = ZN.streamOpen(this._kind, wbits(this._fmt, windowBits), level, memLevel, strategy, -1, 0, 0);
      if (this._h < 0) throw $ERR_ZLIB_INITIALIZATION_FAILED();
      if (dictionary !== undefined && this._kind === K_INFLATE) {
        try { ZN.streamDict(this._h, b64(asU8(dictionary))); } catch (e) {}
      }
      return true;
    }
  }

  class NativeBrotli extends Codec {
    constructor(mode) {
      super();
      this._kind = mode === 9 ? K_BENC : K_BDEC;  // BROTLI_ENCODE / BROTLI_DECODE
      this._syncFlush = 1;    // BROTLI_OPERATION_FLUSH
      this._finishFlush = 2;  // BROTLI_OPERATION_FINISH
    }
    init(params, writeState, processCallback) {
      this._ws = writeState;
      this._cb = processCallback;
      // The bridge takes quality/lgwin/mode directly rather than the sparse
      // BROTLI_PARAM_* array; -1 / 0 mean "engine default" (zlib_stream.cppm).
      const q = set(params, 1) ? params[1] : -1;    // BROTLI_PARAM_QUALITY
      const lg = set(params, 2) ? params[2] : 0;    // BROTLI_PARAM_LGWIN
      const md = set(params, 0) ? params[0] : 0;    // BROTLI_PARAM_MODE
      this._h = this._kind === K_BENC
        ? ZN.streamOpen(K_BENC, 0, -1, 8, 0, q, lg, md)
        : ZN.streamOpen(K_BDEC, 0, -1, 8, 0, -1, 0, 0);
      return this._h >= 0;
    }
  }

  class NativeZstd extends Codec {
    constructor(mode) {
      super();
      this._kind = mode === 10 ? K_ZENC : K_ZDEC;  // ZSTD_COMPRESS / ZSTD_DECOMPRESS
      this._syncFlush = 1;    // ZSTD_e_flush
      this._finishFlush = 2;  // ZSTD_e_end
    }
    init(params, pledgedSrcSize, writeState, processCallback) {
      this._ws = writeState;
      this._cb = processCallback;
      // ZSTD_c_compressionLevel is param 100; the bridge takes it as `level`.
      const level = set(params, 100) ? params[100] : 3;
      this._h = this._kind === K_ZENC
        ? ZN.streamOpen(K_ZENC, 0, level, 8, 0, -1, 0, 0)
        : ZN.streamOpen(K_ZDEC, 0, -1, 8, 0, -1, 0, 0);
      if (this._h < 0) throw $ERR_ZLIB_INITIALIZATION_FAILED();
      return true;
    }
  }

  const __codecs = { NativeZlib, NativeBrotli, NativeZstd };

  R.def("internal/streams/iter/transform", function (require, module, exports) {

    // Compression / Decompression Transforms
    //
    // Creates bare native zlib handles (NativeZlib / NativeBrotli / NativeZstd),
    // bypassing the stream.Transform / ZlibBase / EventEmitter machinery entirely.
    // Compression runs on the threadpool via handle.write() (async) so
    // I/O and upstream transforms can overlap with compression work.
    // Each factory returns a transform descriptor that can be passed to pull().

    const { Buffer } = require("node:buffer");
    const { isArrayBufferView, isAnyArrayBuffer } = require("node:util/types");
    const { kValidatedTransform } = require("internal/streams/iter/types");
    const { checkRangesOrGetDefault, validateFiniteNumber, validateObject } = require("internal/validators");

    const { NativeZlib, NativeBrotli, NativeZstd } = __codecs;
    const constants = process.binding("constants").zlib;

    const Uint8ArraySlice = Uint8Array.prototype.slice;

    // Matches node's internal/errors genericNodeError().
    function genericNodeError(message, options) {
      const error = new Error(message);
      error.errno = options.errno;
      error.code = options.code;
      return error;
    }

    const {
      // Zlib modes
      DEFLATE,
      INFLATE,
      GZIP,
      GUNZIP,
      BROTLI_ENCODE,
      BROTLI_DECODE,
      ZSTD_COMPRESS,
      ZSTD_DECOMPRESS,
      // Zlib flush
      Z_NO_FLUSH,
      Z_FINISH,
      // Zlib defaults
      Z_DEFAULT_WINDOWBITS,
      Z_DEFAULT_STRATEGY,
      // Brotli flush
      BROTLI_OPERATION_PROCESS,
      BROTLI_OPERATION_FINISH,
      // Zlib ranges
      Z_MIN_CHUNK,
      Z_MIN_WINDOWBITS,
      Z_MAX_WINDOWBITS,
      Z_MIN_LEVEL,
      Z_MAX_LEVEL,
      Z_MIN_MEMLEVEL,
      Z_MAX_MEMLEVEL,
      Z_FIXED,
      // Zstd flush
      ZSTD_e_continue,
      ZSTD_e_end,
    } = constants;

    // ---------------------------------------------------------------------------
    // Option validation helpers (matching lib/zlib.js validation patterns)
    // ---------------------------------------------------------------------------

    // Default output buffer size for compression transforms. Larger than
    // Z_DEFAULT_CHUNK (16KB) to reduce the number of threadpool re-entries
    // when the engine has more output than fits in one buffer. 64KB matches
    // BATCH_HWM and the typical input chunk size from pull().
    const DEFAULT_OUTPUT_SIZE = 64 * 1024;

    // Batch high water mark - yield output in chunks of approximately this size.
    const BATCH_HWM = DEFAULT_OUTPUT_SIZE;

    // Pre-allocated empty buffer for flush/finalize calls.
    const kEmpty = Buffer.alloc(0);

    function validateChunkSize(options) {
      let chunkSize = options.chunkSize;
      if (!validateFiniteNumber(chunkSize, "options.chunkSize")) {
        chunkSize = DEFAULT_OUTPUT_SIZE;
      } else if (chunkSize < Z_MIN_CHUNK) {
        throw $ERR_OUT_OF_RANGE("options.chunkSize", `>= ${Z_MIN_CHUNK}`, chunkSize);
      }
      return chunkSize;
    }

    function validateDictionary(dictionary) {
      if (dictionary === undefined) return undefined;
      if (isArrayBufferView(dictionary)) return dictionary;
      if (isAnyArrayBuffer(dictionary)) return Buffer.from(dictionary);
      throw $ERR_INVALID_ARG_TYPE("options.dictionary", ["Buffer", "TypedArray", "DataView", "ArrayBuffer"], dictionary);
    }

    function validateParams(params, maxParam, makeError) {
      if (params === undefined) return;
      if (typeof params !== "object" || params === null) {
        throw $ERR_INVALID_ARG_TYPE("options.params", "Object", params);
      }
      const keys = Object.keys(params);
      for (let i = 0; i < keys.length; i++) {
        const origKey = keys[i];
        const key = +origKey;
        if (Number.isNaN(key) || key < 0 || key > maxParam) {
          throw makeError(origKey);
        }
        const value = params[origKey];
        if (typeof value !== "number" && typeof value !== "boolean") {
          throw $ERR_INVALID_ARG_TYPE("options.params[key]", "number", value);
        }
      }
    }

    // ---------------------------------------------------------------------------
    // Brotli / Zstd parameter arrays (computed once, reused per init call).
    // Mirrors the pattern in lib/zlib.js.
    // ---------------------------------------------------------------------------
    const kMaxBrotliParam = Math.max(
      ...Object.entries(constants).map(({ 0: key, 1: value }) => (key.startsWith("BROTLI_PARAM_") ? value : 0)),
    );
    const brotliInitParamsArray = new Uint32Array(kMaxBrotliParam + 1);

    const kMaxZstdCParam = Math.max(...Object.keys(constants).map(key => (key.startsWith("ZSTD_c_") ? constants[key] : 0)));
    const zstdInitCParamsArray = new Uint32Array(kMaxZstdCParam + 1);

    const kMaxZstdDParam = Math.max(...Object.keys(constants).map(key => (key.startsWith("ZSTD_d_") ? constants[key] : 0)));
    const zstdInitDParamsArray = new Uint32Array(kMaxZstdDParam + 1);

    // ---------------------------------------------------------------------------
    // Handle creation - bare native handles, no Transform/EventEmitter overhead.
    //
    // Each factory accepts a processCallback (called from the threadpool
    // completion path) and an onError handler.
    // ---------------------------------------------------------------------------

    /**
     * Create a bare Zlib handle (gzip, gunzip, deflate, inflate).
     * @returns {{ handle: object, writeState: Uint32Array, chunkSize: number }}
     */
    function createZlibHandle(mode, options, processCallback, onError) {
      // Validate all options before creating the native handle to avoid
      // "close before init" assertion if validation throws.
      const chunkSize = validateChunkSize(options);
      const windowBits = checkRangesOrGetDefault(
        options.windowBits,
        "options.windowBits",
        Z_MIN_WINDOWBITS,
        Z_MAX_WINDOWBITS,
        Z_DEFAULT_WINDOWBITS,
      );
      // Default compression level 4 (not Z_DEFAULT_COMPRESSION which maps to
      // level 6). Level 4 is ~1.5x faster with only ~5-10% worse compression
      // ratio - the sweet spot for streaming and HTTP content-encoding.
      const level = checkRangesOrGetDefault(options.level, "options.level", Z_MIN_LEVEL, Z_MAX_LEVEL, 4);
      // memLevel 9 uses ~128KB more memory than 8 but provides faster hash
      // lookups during compression. Negligible memory cost for the speed gain.
      const memLevel = checkRangesOrGetDefault(options.memLevel, "options.memLevel", Z_MIN_MEMLEVEL, Z_MAX_MEMLEVEL, 9);
      const strategy = checkRangesOrGetDefault(
        options.strategy,
        "options.strategy",
        Z_DEFAULT_STRATEGY,
        Z_FIXED,
        Z_DEFAULT_STRATEGY,
      );
      const dictionary = validateDictionary(options.dictionary);

      const handle = new NativeZlib(mode);
      const writeState = new Uint32Array(2);

      handle.onerror = onError;
      handle.init(windowBits, level, memLevel, strategy, writeState, processCallback, dictionary);

      return { __proto__: null, handle, writeState, chunkSize };
    }

    /**
     * Create a bare Brotli handle.
     * @returns {{ handle: object, writeState: Uint32Array, chunkSize: number }}
     */
    function createBrotliHandle(mode, options, processCallback, onError) {
      // Validate before creating native handle.
      const chunkSize = validateChunkSize(options);
      // Note: bun's NativeBrotli.init() does not take a dictionary parameter;
      // validate for parity but the dictionary is not passed to the engine.
      validateDictionary(options.dictionary);
      validateParams(options.params, kMaxBrotliParam, key => $ERR_BROTLI_INVALID_PARAM(key));

      const handle = new NativeBrotli(mode);
      const writeState = new Uint32Array(2);

      brotliInitParamsArray.fill(-1);
      // Streaming-appropriate defaults: quality 6 (not 11) and lgwin 20 (1MB,
      // not 4MB). Quality 11 is intended for offline/build-time compression
      // and allocates ~400MB of internal state. Quality 6 is ~10x faster with
      // only ~10-15% worse compression ratio - the standard for dynamic HTTP
      // content-encoding (nginx, Caddy, Cloudflare all use 4-6).
      if (mode === BROTLI_ENCODE) {
        brotliInitParamsArray[constants.BROTLI_PARAM_QUALITY] = 6;
        brotliInitParamsArray[constants.BROTLI_PARAM_LGWIN] = 20;
      }
      const params = options.params;
      if (params) {
        // User-supplied params override the defaults above.
        const keys = Object.keys(params);
        for (let i = 0; i < keys.length; i++) {
          const key = +keys[i];
          brotliInitParamsArray[key] = params[keys[i]];
        }
      }

      handle.onerror = onError;
      if (!handle.init(brotliInitParamsArray, writeState, processCallback)) {
        throw $ERR_ZLIB_INITIALIZATION_FAILED();
      }

      return { __proto__: null, handle, writeState, chunkSize };
    }

    /**
     * Create a bare Zstd handle.
     * @returns {{ handle: object, writeState: Uint32Array, chunkSize: number }}
     */
    function createZstdHandle(mode, options, processCallback, onError) {
      const isCompress = mode === ZSTD_COMPRESS;

      // Validate before creating native handle.
      const chunkSize = validateChunkSize(options);
      // Note: bun's NativeZstd.init() does not take a dictionary parameter;
      // validate for parity but the dictionary is not passed to the engine.
      validateDictionary(options.dictionary);
      const maxParam = isCompress ? kMaxZstdCParam : kMaxZstdDParam;
      validateParams(options.params, maxParam, key => $ERR_ZSTD_INVALID_PARAM(key));

      const pledgedSrcSize = options.pledgedSrcSize;
      if (pledgedSrcSize !== undefined) {
        if (typeof pledgedSrcSize !== "number" || Number.isNaN(pledgedSrcSize)) {
          throw $ERR_INVALID_ARG_TYPE("options.pledgedSrcSize", "number", pledgedSrcSize);
        }
        if (pledgedSrcSize < 0) {
          throw $ERR_OUT_OF_RANGE("options.pledgedSrcSize", ">= 0", pledgedSrcSize);
        }
      }

      const handle = new NativeZstd(mode);
      const writeState = new Uint32Array(2);

      const initArray = isCompress ? zstdInitCParamsArray : zstdInitDParamsArray;
      initArray.fill(-1);
      const params = options.params;
      if (params) {
        const keys = Object.keys(params);
        for (let i = 0; i < keys.length; i++) {
          const key = +keys[i];
          initArray[key] = params[keys[i]];
        }
      }

      handle.onerror = onError;
      handle.init(initArray, pledgedSrcSize, writeState, processCallback);

      return { __proto__: null, handle, writeState, chunkSize };
    }

    // ---------------------------------------------------------------------------
    // Core: makeZlibTransform
    //
    // Uses async handle.write() so compression runs on the threadpool.
    // The generator manually iterates the source with pre-reading: the next
    // upstream read+transform is started before awaiting the current compression,
    // so I/O and upstream work overlap with threadpool compression.
    // ---------------------------------------------------------------------------
    function makeZlibTransform(createHandleFn, processFlag, finishFlag) {
      return {
        __proto__: null,
        [kValidatedTransform]: true,
        transform: async function* (source, options) {
          const { signal } = options;

          // Fail fast if already aborted - don't allocate a native handle.
          signal?.throwIfAborted();

          // ---- Per-invocation state shared with the write callback ----
          let outBuf;
          let outOffset = 0;
          let chunkSize;
          let pending = [];
          let pendingBytes = 0;

          // Current write operation state (read by the callback for looping).
          let resolveWrite, rejectWrite;
          let writeInput, writeFlush;
          let writeInOff, writeAvailIn, writeAvailOutBefore;

          // processCallback: called from the threadpool completion path when
          // compression completes. Collects output, loops if the engine
          // has more output to produce (availOut === 0), then resolves the
          // promise when all output for this input chunk is collected.
          function onWriteComplete() {
            const availOut = writeState[0];
            const availInAfter = writeState[1];
            const have = writeAvailOutBefore - availOut;
            const bufferExhausted = availOut === 0 || outOffset + have >= chunkSize;

            if (have > 0) {
              if (bufferExhausted && outOffset === 0) {
                // Entire buffer filled from start - yield directly, no copy.
                pending.push(outBuf);
              } else if (bufferExhausted) {
                // Tail of buffer filled and buffer is being replaced -
                // subarray is safe since outBuf reference is overwritten below.
                pending.push(outBuf.subarray(outOffset, outOffset + have));
              } else {
                // Partial fill, buffer will be reused - must copy.
                pending.push(Uint8ArraySlice.call(outBuf, outOffset, outOffset + have));
              }
              pendingBytes += have;
              outOffset += have;
            }

            // Reallocate output buffer if exhausted.
            if (bufferExhausted) {
              outBuf = Buffer.allocUnsafe(chunkSize);
              outOffset = 0;
            }

            if (availOut === 0) {
              // Engine has more output - but if aborted, don't loop.
              if (!resolveWrite) return;

              const consumed = writeAvailIn - availInAfter;
              writeInOff += consumed;
              writeAvailIn = availInAfter;
              writeAvailOutBefore = chunkSize - outOffset;

              handle.write(writeFlush, writeInput, writeInOff, writeAvailIn, outBuf, outOffset, writeAvailOutBefore);
              return; // Will call onWriteComplete again.
            }

            // All input consumed and output collected.
            handle.buffer = null;
            const resolve = resolveWrite;
            resolveWrite = undefined;
            rejectWrite = undefined;
            if (resolve) resolve();
          }

          // onError: called by the engine when it encounters an error.
          // Fires instead of onWriteComplete - reject the promise.
          function onError(message, errno, code) {
            const error = genericNodeError(message, { __proto__: null, errno, code });
            error.errno = errno;
            error.code = code;
            const reject = rejectWrite;
            resolveWrite = undefined;
            rejectWrite = undefined;
            if (reject) reject(error);
          }

          // ---- Create the handle with our callbacks ----
          const result = createHandleFn(onWriteComplete, onError);
          const handle = result.handle;
          const writeState = result.writeState;
          chunkSize = result.chunkSize;
          outBuf = Buffer.allocUnsafe(chunkSize);

          // Abort handler: reject any in-flight threadpool operation so the
          // generator doesn't block waiting for compression to finish.
          const onAbort = () => {
            const reject = rejectWrite;
            resolveWrite = undefined;
            rejectWrite = undefined;
            if (reject) {
              reject(signal.reason ?? $makeAbortError());
            }
          };
          signal.addEventListener("abort", onAbort, { __proto__: null, once: true });

          // Dispatch input to the threadpool and return a promise.
          function processInputAsync(input, flushFlag) {
            const { promise, resolve, reject } = Promise.withResolvers();
            resolveWrite = resolve;
            rejectWrite = reject;
            writeInput = input;
            writeFlush = flushFlag;
            writeInOff = 0;
            writeAvailIn = input.byteLength;
            writeAvailOutBefore = chunkSize - outOffset;

            // Keep input alive while the threadpool references it.
            handle.buffer = input;

            handle.write(flushFlag, input, 0, writeAvailIn, outBuf, outOffset, writeAvailOutBefore);
            return promise;
          }

          function drainBatch() {
            if (pendingBytes <= BATCH_HWM) {
              // Swap instead of splice - avoids copying the array.
              const batch = pending;
              pending = [];
              pendingBytes = 0;
              return batch;
            }
            const batch = [];
            let batchBytes = 0;
            while (pending.length > 0 && batchBytes < BATCH_HWM) {
              const buf = pending.shift();
              batch.push(buf);
              const len = buf.byteLength;
              batchBytes += len;
              pendingBytes -= len;
            }
            return batch;
          }

          let finalized = false;

          const iter = source[Symbol.asyncIterator]();
          try {
            // Manually iterate the source so we can pre-read: calling
            // iter.next() starts the upstream read + transform
            // before we await the current compression on the threadpool.
            let nextResult = iter.next();

            while (true) {
              const { value: chunks, done } = await nextResult;
              if (done) break;

              signal?.throwIfAborted();

              if (chunks === null) {
                // Flush signal - finalize the engine.
                if (!finalized) {
                  finalized = true;
                  await processInputAsync(kEmpty, finishFlag);
                  while (pending.length > 0) {
                    yield drainBatch();
                  }
                }
                nextResult = iter.next();
                continue;
              }

              // Pre-read: start upstream I/O + transform for the NEXT batch
              // while we compress the current batch on the threadpool.
              nextResult = iter.next();

              for (let i = 0; i < chunks.length; i++) {
                await processInputAsync(chunks[i], processFlag);
              }

              if (pendingBytes >= BATCH_HWM) {
                while (pending.length > 0 && pendingBytes >= BATCH_HWM) {
                  yield drainBatch();
                }
              }
              if (pending.length > 0) {
                yield drainBatch();
              }
            }

            // Source ended - finalize if not already done by a null signal.
            if (!finalized && !signal.aborted) {
              finalized = true;
              await processInputAsync(kEmpty, finishFlag);
              while (pending.length > 0) {
                yield drainBatch();
              }
            }
          } finally {
            signal.removeEventListener("abort", onAbort);
            handle.close();
            // Close the upstream iterator so its finally blocks run promptly
            // rather than waiting for GC.
            try {
              await iter.return?.();
            } catch {
              /* Intentional no-op. */
            }
          }
        },
      };
    }

    // ---------------------------------------------------------------------------
    // Core: makeZlibTransformSync
    //
    // Synchronous counterpart to makeZlibTransform. Uses handle.writeSync()
    // which runs compression directly on the main thread (no threadpool).
    // Returns a stateful sync transform (generator function).
    // ---------------------------------------------------------------------------
    function makeZlibTransformSync(createHandleFn, processFlag, finishFlag) {
      return {
        __proto__: null,
        transform: function* (source) {
          // The processCallback is never called in sync mode, but handle.init()
          // requires it. Pass a no-op.
          let error = null;
          function onError(message, errno, code) {
            error = genericNodeError(message, { __proto__: null, errno, code });
            error.errno = errno;
            error.code = code;
          }

          const result = createHandleFn(() => {}, onError);
          const handle = result.handle;
          const writeState = result.writeState;
          const chunkSize = result.chunkSize;
          let outBuf = Buffer.allocUnsafe(chunkSize);
          let outOffset = 0;
          let pending = [];
          let pendingBytes = 0;

          function processSyncInput(input, flushFlag) {
            let inOff = 0;
            let availIn = input.byteLength;
            let availOutBefore = chunkSize - outOffset;

            handle.writeSync(flushFlag, input, inOff, availIn, outBuf, outOffset, availOutBefore);
            if (error) throw error;

            while (true) {
              const availOut = writeState[0];
              const availInAfter = writeState[1];
              const have = availOutBefore - availOut;
              const bufferExhausted = availOut === 0 || outOffset + have >= chunkSize;

              if (have > 0) {
                if (bufferExhausted && outOffset === 0) {
                  // Entire buffer filled - yield directly, no copy.
                  pending.push(outBuf);
                } else if (bufferExhausted) {
                  // Tail filled, buffer being replaced - subarray is safe.
                  pending.push(outBuf.subarray(outOffset, outOffset + have));
                } else {
                  // Partial fill, buffer reused - must copy.
                  pending.push(Uint8ArraySlice.call(outBuf, outOffset, outOffset + have));
                }
                pendingBytes += have;
                outOffset += have;
              }

              if (bufferExhausted) {
                outBuf = Buffer.allocUnsafe(chunkSize);
                outOffset = 0;
              }

              if (availOut === 0) {
                // Engine has more output - loop.
                const consumed = availIn - availInAfter;
                inOff += consumed;
                availIn = availInAfter;
                availOutBefore = chunkSize - outOffset;

                handle.writeSync(flushFlag, input, inOff, availIn, outBuf, outOffset, availOutBefore);
                if (error) throw error;
                continue;
              }

              // All input consumed.
              break;
            }
          }

          function drainBatch() {
            if (pendingBytes <= BATCH_HWM) {
              const batch = pending;
              pending = [];
              pendingBytes = 0;
              return batch;
            }
            const batch = [];
            let batchBytes = 0;
            while (pending.length > 0 && batchBytes < BATCH_HWM) {
              const buf = pending.shift();
              const len = buf.byteLength;
              batch.push(buf);
              batchBytes += len;
              pendingBytes -= len;
            }
            return batch;
          }

          try {
            for (const batch of source) {
              if (batch === null) {
                // Flush signal - finalize the engine.
                processSyncInput(Buffer.alloc(0), finishFlag);
                while (pending.length > 0) {
                  yield drainBatch();
                }
                continue;
              }

              for (let i = 0; i < batch.length; i++) {
                processSyncInput(batch[i], processFlag);
              }

              if (pendingBytes >= BATCH_HWM) {
                while (pending.length > 0 && pendingBytes >= BATCH_HWM) {
                  yield drainBatch();
                }
              }
              if (pending.length > 0) {
                yield drainBatch();
              }
            }
          } finally {
            handle.close();
          }
        },
      };
    }

    // ---------------------------------------------------------------------------
    // Async compression factories
    // ---------------------------------------------------------------------------

    const kNullPrototype = { __proto__: null };

    function compressGzip(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransform((cb, onErr) => createZlibHandle(GZIP, options, cb, onErr), Z_NO_FLUSH, Z_FINISH);
    }

    function compressDeflate(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransform((cb, onErr) => createZlibHandle(DEFLATE, options, cb, onErr), Z_NO_FLUSH, Z_FINISH);
    }

    function compressBrotli(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransform(
        (cb, onErr) => createBrotliHandle(BROTLI_ENCODE, options, cb, onErr),
        BROTLI_OPERATION_PROCESS,
        BROTLI_OPERATION_FINISH,
      );
    }

    function compressZstd(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransform(
        (cb, onErr) => createZstdHandle(ZSTD_COMPRESS, options, cb, onErr),
        ZSTD_e_continue,
        ZSTD_e_end,
      );
    }

    // ---------------------------------------------------------------------------
    // Decompression factories
    // ---------------------------------------------------------------------------

    function decompressGzip(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransform((cb, onErr) => createZlibHandle(GUNZIP, options, cb, onErr), Z_NO_FLUSH, Z_FINISH);
    }

    function decompressDeflate(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransform((cb, onErr) => createZlibHandle(INFLATE, options, cb, onErr), Z_NO_FLUSH, Z_FINISH);
    }

    function decompressBrotli(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransform(
        (cb, onErr) => createBrotliHandle(BROTLI_DECODE, options, cb, onErr),
        BROTLI_OPERATION_PROCESS,
        BROTLI_OPERATION_FINISH,
      );
    }

    function decompressZstd(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransform(
        (cb, onErr) => createZstdHandle(ZSTD_DECOMPRESS, options, cb, onErr),
        ZSTD_e_continue,
        ZSTD_e_end,
      );
    }

    // ---------------------------------------------------------------------------
    // Sync compression factories
    // ---------------------------------------------------------------------------

    function compressGzipSync(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransformSync((cb, onErr) => createZlibHandle(GZIP, options, cb, onErr), Z_NO_FLUSH, Z_FINISH);
    }

    function compressDeflateSync(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransformSync((cb, onErr) => createZlibHandle(DEFLATE, options, cb, onErr), Z_NO_FLUSH, Z_FINISH);
    }

    function compressBrotliSync(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransformSync(
        (cb, onErr) => createBrotliHandle(BROTLI_ENCODE, options, cb, onErr),
        BROTLI_OPERATION_PROCESS,
        BROTLI_OPERATION_FINISH,
      );
    }

    function compressZstdSync(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransformSync(
        (cb, onErr) => createZstdHandle(ZSTD_COMPRESS, options, cb, onErr),
        ZSTD_e_continue,
        ZSTD_e_end,
      );
    }

    // ---------------------------------------------------------------------------
    // Sync decompression factories
    // ---------------------------------------------------------------------------

    function decompressGzipSync(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransformSync((cb, onErr) => createZlibHandle(GUNZIP, options, cb, onErr), Z_NO_FLUSH, Z_FINISH);
    }

    function decompressDeflateSync(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransformSync((cb, onErr) => createZlibHandle(INFLATE, options, cb, onErr), Z_NO_FLUSH, Z_FINISH);
    }

    function decompressBrotliSync(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransformSync(
        (cb, onErr) => createBrotliHandle(BROTLI_DECODE, options, cb, onErr),
        BROTLI_OPERATION_PROCESS,
        BROTLI_OPERATION_FINISH,
      );
    }

    function decompressZstdSync(options = kNullPrototype) {
      validateObject(options, "options");
      return makeZlibTransformSync(
        (cb, onErr) => createZstdHandle(ZSTD_DECOMPRESS, options, cb, onErr),
        ZSTD_e_continue,
        ZSTD_e_end,
      );
    }

    module.exports = {
      compressBrotli,
      compressBrotliSync,
      compressDeflate,
      compressDeflateSync,
      compressGzip,
      compressGzipSync,
      compressZstd,
      compressZstdSync,
      decompressBrotli,
      decompressBrotliSync,
      decompressDeflate,
      decompressDeflateSync,
      decompressGzip,
      decompressGzipSync,
      decompressZstd,
      decompressZstdSync,
    };
  });

})();
)JS";

}  // namespace mbun::jsc::builtins::detail
