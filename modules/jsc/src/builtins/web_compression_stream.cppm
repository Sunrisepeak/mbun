// WHATWG CompressionStream / DecompressionStream over the incremental zlib bridge.
//
// node:zlib's streaming Transform classes are installed by zlib_stream.cppm.
// The web surface deliberately adapts those real codecs instead of buffering a
// whole body through the one-shot helpers: Compression Streams is observable as
// a pair of backpressure-aware Web streams, including post-handshake failures.
export module mbun.jsc.js_builtins:web_compression_stream;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kWebCompressionStreamJS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  const R = G.__mbunStreamReg;
  if (!M || !R || typeof R.require !== "function") return;

  const zlib = M["zlib"] || M["node:zlib"];
  if (!zlib) return;

  const factories = {
    gzip: ["createGzip", "createGunzip"],
    deflate: ["createDeflate", "createInflate"],
    "deflate-raw": ["createDeflateRaw", "createInflateRaw"],
    brotli: ["createBrotliCompress", "createBrotliDecompress"],
    // zstd is in the same Compression Streams format list as brotli in bun, and
    // zlib_stream.cppm has carried the real streaming ZstdCompress/ZstdDecompress
    // Transforms all along — this table was simply never extended, so
    // `new CompressionStream("zstd")` reported the format as invalid.
    zstd: ["createZstdCompress", "createZstdDecompress"],
  };

  const invalidFormat = (format) => {
    const e = new TypeError(`The argument 'format' is invalid. Received '${String(format)}'`);
    e.code = "ERR_INVALID_ARG_VALUE";
    return e;
  };

  const makePair = (format, decompress) => {
    if (typeof format !== "string" || !Object.prototype.hasOwnProperty.call(factories, format)) {
      throw invalidFormat(format);
    }
    const factory = zlib[factories[format][decompress ? 1 : 0]];
    if (typeof factory !== "function") throw invalidFormat(format);
    const duplex = factory();

    // The native zlib facade uses Error for decoder failures, while the
    // Compression Streams contract exposes malformed compressed data as a
    // TypeError on both Web-stream halves.
    if (decompress && typeof duplex._zMakeErr === "function") {
      const makeZlibError = duplex._zMakeErr;
      // BOTH arguments: _zMakeErr is (message, code), and the engine's own code
      // (ERR_<BrotliDecoderErrorString>, ZSTD_error_*, Z_NEED_DICT) arrives in
      // the second one. Forwarding only `message` silently re-derived every
      // decoder failure as the Z_DATA_ERROR fallback, so a corrupt brotli body
      // surfaced as `code: "Z_DATA_ERROR"` on the web streams even though
      // modules/compress had already resolved it to ERR__ERROR_FORMAT_PADDING_2.
      duplex._zMakeErr = function (message, code) {
        const e = makeZlibError.call(this, message, code);
        if (e) e.name = "TypeError";
        return e;
      };
    }
    const adapters = R.require("internal/webstreams_adapters");
    return adapters.newBufferSourceTransformPairFromDuplex(duplex);
  };

  const inspectSymbol = Symbol.for("nodejs.util.inspect.custom");
  const install = (name, decompress) => {
    class CompressionTransform {
      #readable;
      #writable;

      constructor(format) {
        const pair = makePair(format, decompress);
        this.#readable = pair.readable;
        this.#writable = pair.writable;
      }

      get readable() { return this.#readable; }
      get writable() { return this.#writable; }
      get [Symbol.toStringTag]() { return name; }
      [inspectSymbol]() {
        return `${name} { readable: ReadableStream, writable: WritableStream }`;
      }
    }
    Object.defineProperty(CompressionTransform, "name", { value: name, configurable: true });
    return CompressionTransform;
  };

  const CompressionStream = install("CompressionStream", false);
  const DecompressionStream = install("DecompressionStream", true);
  G.CompressionStream = CompressionStream;
  G.DecompressionStream = DecompressionStream;

  for (const streamWeb of [M["stream/web"], M["node:stream/web"]]) {
    if (!streamWeb || (typeof streamWeb !== "object" && typeof streamWeb !== "function")) continue;
    Object.defineProperties(streamWeb, {
      CompressionStream: { get: () => CompressionStream, enumerable: true, configurable: true },
      DecompressionStream: { get: () => DecompressionStream, enumerable: true, configurable: true },
    });
  }
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
