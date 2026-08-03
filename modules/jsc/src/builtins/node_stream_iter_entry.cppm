// node:stream/iter payload partition — the public entry point and its flag gate.
//
// Mechanical 1:1 translation of bun-ref src/js/node/stream.iter.ts (identical in
// shape to node's own lib/stream/iter.js): it destructures the six
// internal/streams/iter/* modules the partitions above registered and freezes
// them into the `Stream` namespace plus the flat named exports.
//
// Two seams are mbun's, not the blueprint's:
//
//   * The module is EXPERIMENTAL and gated. node hides it behind
//     `experimentalModuleList` (internal/bootstrap/realm.js) plus
//     `BuiltinModule.allowRequireByUsers('stream/iter')` in pre_execution.js,
//     both driven by --experimental-stream-iter; bun gates it at module
//     resolution. mbun's builtin table is globalThis.__mbunNativeModules, which
//     the native require() reads with JSObjectGetProperty — so the gate is an
//     accessor: without the flag it returns undefined and require() falls
//     through to the resolver, i.e. exactly today's MODULE_NOT_FOUND. The flag
//     cannot be read at image-evaluation time (process.execArgv does not exist
//     yet), which is also why node defers it, so it is read on first access.
//   * The namespace is built lazily on that first access. Eagerly requiring
//     eleven modules at boot would cost every process that never touches
//     stream/iter; the CJS registry caches the result, so the second require()
//     is a property read.
//
// The `--experimental-stream-iter` gate is shared with node:stream itself:
// Readable.prototype[Symbol.for("Stream.toAsyncStreamable")] (the interop
// protocol, node_stream_readable.cppm) consults the same flag through the
// $cpp("NodeModuleModule.cpp", "createStreamIterEnabledFlag") shim.
//
// NOTE: appended AFTER the master builtins IIFE has closed (see image_closure),
// so this is a self-contained IIFE that re-binds G = globalThis.
export module mbun.jsc.js_builtins:node_stream_iter_entry;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeStreamIterEntryJS = R"JS(
(function () {
  const G = globalThis;
  const R = G.__mbunStreamReg;
  const M = G.__mbunNativeModules;
  if (!R || !M) return;

  // node's emitExperimentalWarning('stream/iter'), once per process.
  let warned = false;
  const emitExperimentalWarning = () => {
    if (warned) return;
    warned = true;
    const p = G.process;
    if (p && typeof p.emitWarning === "function") {
      p.emitWarning("stream/iter is an experimental feature and might change at any time",
                    "ExperimentalWarning");
    }
  };

  const build = () => {
    emitExperimentalWarning();
    const require = R.require;

    // Protocol symbols
    const {
      toStreamable,
      toAsyncStreamable,
      broadcastProtocol,
      shareProtocol,
      shareSyncProtocol,
      drainableProtocol,
    } = require("internal/streams/iter/types");

    // Factories
    const { push } = require("internal/streams/iter/push");
    const { duplex } = require("internal/streams/iter/duplex");
    const { from, fromSync } = require("internal/streams/iter/from");

    // Pipelines
    const { pull, pullSync, pipeTo, pipeToSync } = require("internal/streams/iter/pull");

    // Consumers
    const {
      bytes,
      bytesSync,
      text,
      textSync,
      arrayBuffer,
      arrayBufferSync,
      array,
      arraySync,
      tap,
      tapSync,
      merge,
      ondrain,
    } = require("internal/streams/iter/consumers");

    // Classic stream interop (Node.js-specific, not part of the spec)
    const { fromReadable, fromWritable, toReadable, toReadableSync, toWritable } =
      require("internal/streams/iter/classic");

    // Multi-consumer
    const { broadcast, Broadcast } = require("internal/streams/iter/broadcast");
    const { share, shareSync, Share, SyncShare } = require("internal/streams/iter/share");

    const Stream = Object.freeze({
      // Factories
      push,
      duplex,
      from,
      fromSync,

      // Pipelines
      pull,
      pullSync,

      // Pipe to destination
      pipeTo,
      pipeToSync,

      // Consumers (async)
      bytes,
      text,
      arrayBuffer,
      array,

      // Consumers (sync)
      bytesSync,
      textSync,
      arrayBufferSync,
      arraySync,

      // Combining
      merge,

      // Multi-consumer (push model)
      broadcast,

      // Multi-consumer (pull model)
      share,
      shareSync,

      // Utilities
      tap,
      tapSync,

      // Drain utility for event source integration
      ondrain,

      // Protocol symbols
      toStreamable,
      toAsyncStreamable,
      broadcastProtocol,
      shareProtocol,
      shareSyncProtocol,
      drainableProtocol,
    });

    return {
      // The Stream namespace
      Stream,

      // Also export everything individually for destructured imports

      // Protocol symbols
      toStreamable,
      toAsyncStreamable,
      broadcastProtocol,
      shareProtocol,
      shareSyncProtocol,
      drainableProtocol,

      // Factories
      push,
      duplex,
      from,
      fromSync,

      // Pipelines
      pull,
      pullSync,
      pipeTo,
      pipeToSync,

      // Consumers (async)
      bytes,
      text,
      arrayBuffer,
      array,

      // Consumers (sync)
      bytesSync,
      textSync,
      arrayBufferSync,
      arraySync,

      // Combining
      merge,

      // Multi-consumer
      broadcast,
      Broadcast,
      share,
      shareSync,
      Share,
      SyncShare,

      // Utilities
      tap,
      tapSync,
      ondrain,

      // Classic stream interop
      fromReadable,
      fromWritable,
      toReadable,
      toReadableSync,
      toWritable,
    };
  };

  // zlib/iter is the same feature behind the same flag; its transforms are
  // registered by the :node_zlib_iter partition. Kept in one table so the gate
  // and the lazy-build policy cannot drift between the two entry points.
  const ENTRIES = {
    "stream/iter": build,
    "zlib/iter": () => {
      emitExperimentalWarning();
      const {
        compressGzip, compressGzipSync, compressDeflate, compressDeflateSync,
        compressBrotli, compressBrotliSync, compressZstd, compressZstdSync,
        decompressGzip, decompressGzipSync, decompressDeflate, decompressDeflateSync,
        decompressBrotli, decompressBrotliSync, decompressZstd, decompressZstdSync,
      } = R.require("internal/streams/iter/transform");
      return {
        // Compression transforms (async)
        compressGzip, compressDeflate, compressBrotli, compressZstd,
        // Compression transforms (sync)
        compressGzipSync, compressDeflateSync, compressBrotliSync, compressZstdSync,
        // Decompression transforms (async)
        decompressGzip, decompressDeflate, decompressBrotli, decompressZstd,
        // Decompression transforms (sync)
        decompressGzipSync, decompressDeflateSync, decompressBrotliSync, decompressZstdSync,
      };
    },
  };

  for (const name of Object.keys(ENTRIES)) {
    let cached;
    const get = () => {
      if (!R.H.$cpp("NodeModuleModule.cpp", "createStreamIterEnabledFlag")) return undefined;
      // A module that fails to build must not be cached as undefined: the next
      // require() has to raise the same error rather than report the module
      // missing.
      if (cached === undefined) cached = ENTRIES[name]();
      return cached;
    };
    for (const key of [name, "node:" + name]) {
      Object.defineProperty(M, key, { get, configurable: true });
    }
  }
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
