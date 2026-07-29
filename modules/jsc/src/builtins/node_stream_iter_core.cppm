// node:stream/iter payload partition — protocol symbols, RingBuffer, shared helpers, from()/fromSync()
//
// Mechanical 1:1 translation of bun-ref src/js/internal/streams/iter/{types,ringbuffer,utils,from}.ts
// (三段法 stage 1): JSDoc-typed JS with bun's $-intrinsics lowered onto the shims
// in node_stream_core.cppm; algorithm, branches and error text are the
// blueprint's, not reinvented. The modules are registered on the same CJS
// registry node:stream uses, so the blueprint's lazy cross-requires resolve
// exactly as in bun.
//
// NOTE: appended AFTER the master builtins IIFE has closed (see image_closure),
// so this is a self-contained IIFE that re-binds G = globalThis and pulls shared
// helpers off G.__mbunStreamReg.
export module mbun.jsc.js_builtins:node_stream_iter_core;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeStreamIterCoreJS = R"JS(
(function () {
  const G = globalThis;
  const R = G.__mbunStreamReg;
  if (!R) return;
  const process = G.process;
  const Buffer = G.Buffer;
  const { $ERR_INVALID_ARG_TYPE, $ERR_INVALID_ARG_VALUE, $ERR_INVALID_ARG_VALUE_RangeError, $ERR_INVALID_RETURN_VALUE, $ERR_OUT_OF_RANGE, $ERR_INVALID_STATE, $ERR_INVALID_STATE_TypeError, $ERR_INVALID_STATE_RangeError, $ERR_OPERATION_FAILED, $ERR_STREAM_WRITE_AFTER_END, $makeAbortError, __isCallable, __debug, __assert } = R.H;

  R.def("internal/streams/iter/types", function (require, module, exports) {

    /**
     * Symbol for sync value-to-streamable conversion protocol.
     * Objects implementing this can be written to streams or yielded
     * from generators. Works in both sync and async contexts.
     *
     * Third-party: [Symbol.for('Stream.toStreamable')]() { ... }
     */
    const toStreamable = Symbol.for("Stream.toStreamable");

    /**
     * Symbol for async value-to-streamable conversion protocol.
     * Objects implementing this can be written to async streams.
     * Works in async contexts only.
     *
     * Third-party: [Symbol.for('Stream.toAsyncStreamable')]() { ... }
     */
    const toAsyncStreamable = Symbol.for("Stream.toAsyncStreamable");

    /**
     * Symbol for Broadcastable protocol - object can provide a Broadcast.
     */
    const broadcastProtocol = Symbol.for("Stream.broadcastProtocol");

    /**
     * Symbol for Shareable protocol - object can provide a Share.
     */
    const shareProtocol = Symbol.for("Stream.shareProtocol");

    /**
     * Symbol for SyncShareable protocol - object can provide a SyncShare.
     */
    const shareSyncProtocol = Symbol.for("Stream.shareSyncProtocol");

    /**
     * Symbol for Drainable protocol - object can signal when backpressure
     * clears. Used to bridge event-driven sources that need drain notification.
     */
    const drainableProtocol = Symbol.for("Stream.drainableProtocol");

    /**
     * Internal sentinel for validated stateful transforms. A transform object
     * with [kValidatedTransform] = true signals that:
     *   1. It handles source exhaustion (done) internally - no withFlushAsync
     *      wrapper needed.
     *   2. It always yields valid Uint8Array[] batches - no isUint8ArrayBatch
     *      validation needed on each yield.
     * This is NOT a public protocol symbol - it uses Symbol() not Symbol.for().
     */
    const kValidatedTransform = Symbol("kValidatedTransform");

    /**
     * Internal sentinel for validated sources. An async iterable with
     * [kValidatedSource] = true signals that it already yields valid
     * Uint8Array[] batches - no normalizeAsyncSource wrapper needed.
     * from() will return such sources directly, skipping all normalization.
     * This is NOT a public protocol symbol - it uses Symbol() not Symbol.for().
     */
    const kValidatedSource = Symbol("kValidatedSource");

    /**
     * Internal sentinel for writers whose sync write methods can return false
     * after accepting data as a backpressure signal.
     */
    const kSyncWriteAccepted = Symbol("kSyncWriteAccepted");

    /**
     * Internal sentinel for writers whose sync write methods may return false
     * after accepting data when backpressure is applied. Such writers must expose
     * desiredSize so callers can distinguish accepted backpressure from a sync
     * write that was not performed.
     */
    const kSyncWriteAcceptedOnFalse = Symbol("kSyncWriteAcceptedOnFalse");

    module.exports = {
      broadcastProtocol,
      drainableProtocol,
      kSyncWriteAccepted,
      kSyncWriteAcceptedOnFalse,
      kValidatedSource,
      kValidatedTransform,
      shareProtocol,
      shareSyncProtocol,
      toAsyncStreamable,
      toStreamable,
    };
  });

  R.def("internal/streams/iter/ringbuffer", function (require, module, exports) {

    // RingBuffer - O(1) FIFO queue with indexed access.
    //
    // Replaces plain JS arrays that are used as queues with shift()/push().
    // Array.shift() is O(n) because it copies all remaining elements;
    // RingBuffer.shift() is O(1) -- it just advances a head pointer.
    //
    // Also provides O(1) trimFront(count) to replace Array.splice(0, count).
    //
    // Capacity is always a power of 2, so modulo is replaced with bitwise AND.

    class RingBuffer {
      #backing;
      #head = 0;
      #size = 0;
      #mask;

      constructor(initialCapacity = 16) {
        this.#mask = initialCapacity - 1;
        this.#backing = new Array(initialCapacity);
      }

      get length() {
        return this.#size;
      }

      /**
       * Append an item to the tail. O(1) amortized.
       */
      push(item) {
        if (this.#size > this.#mask) {
          this.#grow();
        }
        this.#backing[(this.#head + this.#size) & this.#mask] = item;
        this.#size++;
      }

      /**
       * Prepend an item to the head. O(1) amortized.
       */
      unshift(item) {
        if (this.#size > this.#mask) {
          this.#grow();
        }
        this.#head = (this.#head - 1 + this.#mask + 1) & this.#mask;
        this.#backing[this.#head] = item;
        this.#size++;
      }

      /**
       * Remove and return the item at the head. O(1).
       * @returns {any}
       */
      shift() {
        if (this.#size === 0) return undefined;
        const item = this.#backing[this.#head];
        this.#backing[this.#head] = undefined; // Help GC
        this.#head = (this.#head + 1) & this.#mask;
        this.#size--;
        return item;
      }

      /**
       * Read item at a logical index (0 = head). O(1).
       * Returns undefined if index is out of bounds.
       * @returns {any}
       */
      get(index) {
        if (index < 0 || index >= this.#size) return undefined;
        return this.#backing[(this.#head + index) & this.#mask];
      }

      /**
       * Remove `count` items from the head without returning them.
       * O(count) for GC cleanup.
       */
      trimFront(count) {
        if (count <= 0) return;
        if (count >= this.#size) {
          this.clear();
          return;
        }
        for (let i = 0; i < count; i++) {
          this.#backing[(this.#head + i) & this.#mask] = undefined;
        }
        this.#head = (this.#head + count) & this.#mask;
        this.#size -= count;
      }

      /**
       * Find the logical index of `item` (reference equality). O(n).
       * Returns -1 if not found.
       * @returns {number}
       */
      indexOf(item) {
        for (let i = 0; i < this.#size; i++) {
          if (this.#backing[(this.#head + i) & this.#mask] === item) {
            return i;
          }
        }
        return -1;
      }

      /**
       * Remove the item at logical `index`, shifting later elements. O(n) worst case.
       * Used only on rare abort-signal cancellation path.
       */
      removeAt(index) {
        if (index < 0 || index >= this.#size) return;
        for (let i = index; i < this.#size - 1; i++) {
          const from = (this.#head + i + 1) & this.#mask;
          const to = (this.#head + i) & this.#mask;
          this.#backing[to] = this.#backing[from];
        }
        const last = (this.#head + this.#size - 1) & this.#mask;
        this.#backing[last] = undefined;
        this.#size--;
      }

      /**
       * Remove all items. O(n) for GC cleanup.
       */
      clear() {
        for (let i = 0; i < this.#size; i++) {
          this.#backing[(this.#head + i) & this.#mask] = undefined;
        }
        this.#head = 0;
        this.#size = 0;
      }

      /**
       * Double the backing capacity, linearizing the circular layout.
       */
      #grow() {
        const newCapacity = (this.#mask + 1) * 2;
        const newBacking = new Array(newCapacity);
        for (let i = 0; i < this.#size; i++) {
          newBacking[i] = this.#backing[(this.#head + i) & this.#mask];
        }
        this.#backing = newBacking;
        this.#head = 0;
        this.#mask = newCapacity - 1;
      }
    }

    module.exports = { RingBuffer };
  });

  R.def("internal/streams/iter/utils", function (require, module, exports) {

    const { isUint8Array } = require("node:util/types");

    const { validateOneOf } = require("internal/validators");

    let isError;

    // Cached resolved promise to avoid allocating a new one on every sync fast-path.
    const kResolvedPromise = Promise.resolve();

    // Shared TextEncoder instance for string conversion.
    const encoder = new TextEncoder();

    // Default high water marks for push and multi-consumer streams. These values
    // are somewhat arbitrary but have been tested across various workloads and
    // appear to yield the best overall throughput/latency balance.

    /** Default high water mark for push streams (single-consumer). */
    const kPushDefaultHWM = 4;

    /** Default high water mark for broadcast and share streams (multi-consumer). */
    const kMultiConsumerDefaultHWM = 16;

    /**
     * Clamp a high water mark to [1, MAX_SAFE_INTEGER].
     * @param {number} value
     * @returns {number}
     */
    function clampHWM(value) {
      return Math.max(1, Math.min(Number.MAX_SAFE_INTEGER, value));
    }

    /**
     * Register a handler for an AbortSignal, handling the already-aborted case.
     * If the signal is already aborted, calls handler immediately.
     * Otherwise, adds a one-time 'abort' listener.
     * @param {AbortSignal} signal
     * @param {Function} handler
     */
    function onSignalAbort(signal, handler) {
      if (signal.aborted) {
        handler();
      } else {
        signal.addEventListener("abort", handler, { __proto__: null, once: true });
      }
    }

    /**
     * Compute the minimum cursor across a set of consumers and count how many
     * consumers are at that cursor.
     * @param {Set} consumers - Set of objects with a `cursor` property
     * @param {number} fallback - Cursor to return when set is empty
     * @returns {{ minCursor: number, minCursorConsumers: number }}
     */
    function getMinCursor(consumers, fallback) {
      let minCursor = fallback;
      let minCursorConsumers = 0;
      for (const consumer of consumers) {
        const cursor = consumer.cursor;
        if (cursor < minCursor) {
          minCursor = cursor;
          minCursorConsumers = 1;
        } else if (cursor === minCursor) {
          minCursorConsumers++;
        }
      }
      return { __proto__: null, minCursor, minCursorConsumers };
    }

    /**
     * Convert a chunk (string or Uint8Array) to Uint8Array.
     * Strings are UTF-8 encoded.
     * @param {Uint8Array|string} chunk
     * @returns {Uint8Array}
     */
    function toUint8Array(chunk) {
      if (typeof chunk === "string") {
        return encoder.encode(chunk);
      }
      if (!isUint8Array(chunk)) {
        throw $ERR_INVALID_ARG_TYPE("chunk", ["string", "Uint8Array"], chunk);
      }
      return chunk;
    }

    /**
     * Check if all chunks in an array are already Uint8Array (no strings).
     * Short-circuits on the first string found.
     * @param {Array<Uint8Array|string>} chunks
     * @returns {boolean}
     */
    function allUint8Array(chunks) {
      // Ok, well, kind of. This is more a check for "no strings"...
      for (let i = 0; i < chunks.length; i++) {
        if (typeof chunks[i] === "string") return false;
      }
      return true;
    }

    /**
     * Concatenate multiple Uint8Arrays into a single Uint8Array.
     * @param {Uint8Array[]} chunks
     * @returns {Uint8Array}
     */
    function concatBytes(chunks) {
      // Empty stream: return zero-length Uint8Array
      if (chunks.length === 0) {
        return new Uint8Array(0);
      }
      // Single chunk: return directly if it covers the entire backing buffer,
      // otherwise return a copy
      if (chunks.length === 1) {
        const chunk = chunks[0];
        // If non-zero offset, skip the remaining buffer checks.
        // NB: a Buffer that covers its whole backing store is returned AS A
        // BUFFER here, exactly as node does — test-stream-iter-transform-sync
        // deepStrictEquals a bytesSync() result against a Buffer. The mirror
        // case (test-stream-iter-readable-interop wants a plain Uint8Array from
        // `bytes(from(readable))`) only holds in node because Buffer.from(str)
        // is pool-backed there, so it never covers its whole ArrayBuffer and
        // falls into the copy path below. Fixing that needs Buffer pooling, not
        // a prototype test here.
        if (chunk.byteOffset === 0) {
          // Works for both ArrayBuffer and SharedArrayBuffer backings.
          if (chunk.byteLength === chunk.buffer.byteLength) {
            return chunk;
          }
        }
        return new Uint8Array(chunk);
      }
      // Multiple chunks: concatenate
      let totalByteLength = 0;
      for (let i = 0; i < chunks.length; i++) {
        totalByteLength += chunks[i].byteLength;
      }
      const concatenated = new Uint8Array(totalByteLength);
      let offset = 0;
      for (let i = 0; i < chunks.length; i++) {
        concatenated.set(chunks[i], offset);
        offset += chunks[i].byteLength;
      }
      return concatenated;
    }

    /**
     * Convert an array of chunks (strings or Uint8Arrays) to a Uint8Array[].
     * Always returns a fresh copy of the array.
     * @param {Array<Uint8Array|string>} chunks
     * @returns {Uint8Array[]}
     */
    function convertChunks(chunks) {
      if (allUint8Array(chunks)) {
        return chunks.slice();
      }
      const len = chunks.length;
      const result = new Array(len);
      for (let i = 0; i < len; i++) {
        result[i] = toUint8Array(chunks[i]);
      }
      return result;
    }

    /**
     * Wrap a caught value as an Error, converting non-Error values.
     * @param {unknown} error
     * @returns {Error}
     */
    function wrapError(error) {
      isError ??= require("node:util").isError;
      return isError(error) ? error : $ERR_OPERATION_FAILED(String(error));
    }

    /**
     * Check if a value implements a Symbol-keyed protocol (has a function
     * at the given symbol key).
     * @param {unknown} value
     * @param {symbol} symbol
     * @returns {boolean}
     */
    function hasProtocol(value, symbol) {
      return value !== null && typeof value === "object" && symbol in value && typeof value[symbol] === "function";
    }

    /**
     * Check if a value is PullOptions (object without transform or write property).
     * @param {unknown} value
     * @returns {boolean}
     */
    function isPullOptions(value) {
      return value !== null && typeof value === "object" && !("transform" in value) && !("write" in value);
    }

    /**
     * Check if a value is a stateful transform object (has a transform method).
     * @param {unknown} value
     * @returns {boolean}
     */
    function isTransformObject(value) {
      return typeof value?.transform === "function";
    }

    /**
     * Check if a value is a valid transform (function or transform object).
     * @param {unknown} value
     * @returns {boolean}
     */
    function isTransform(value) {
      return typeof value === "function" || isTransformObject(value);
    }

    /**
     * Parse variadic arguments for pull/pullSync.
     * Returns { transforms, options }
     * @param {Array} args
     * @returns {{ transforms: Array, options: object|undefined }}
     */
    function parsePullArgs(args) {
      if (args.length === 0) {
        return { __proto__: null, transforms: [], options: undefined };
      }

      let transforms;
      let options;
      const last = args[args.length - 1];
      if (isPullOptions(last)) {
        transforms = args.slice(0, -1);
        options = last;
      } else {
        transforms = args;
        options = undefined;
      }

      for (let i = 0; i < transforms.length; i++) {
        if (!isTransform(transforms[i])) {
          throw $ERR_INVALID_ARG_TYPE(`transforms[${i}]`, ["Function", "Object with transform()"], transforms[i]);
        }
      }

      return { __proto__: null, transforms, options };
    }

    /**
     * Validate backpressure option value.
     * @param {string} value
     */
    function validateBackpressure(value) {
      validateOneOf(value, "options.backpressure", ["strict", "block", "drop-oldest", "drop-newest"]);
    }

    module.exports = {
      kMultiConsumerDefaultHWM,
      kPushDefaultHWM,
      kResolvedPromise,
      allUint8Array,
      clampHWM,
      concatBytes,
      convertChunks,
      getMinCursor,
      hasProtocol,
      isPullOptions,
      isTransform,
      isTransformObject,
      onSignalAbort,
      parsePullArgs,
      toUint8Array,
      validateBackpressure,
      wrapError,
    };
  });

  R.def("internal/streams/iter/from", function (require, module, exports) {

    // New Streams API - from() and fromSync()
    //
    // Creates normalized byte stream iterables from various input types.
    // Handles recursive flattening of nested iterables and protocol conversions.

    const { isAnyArrayBuffer, isPromise, isTypedArray, isUint8Array } = require("node:util/types");

    const { kValidatedSource, toStreamable, toAsyncStreamable } = require("internal/streams/iter/types");

    const { hasProtocol, toUint8Array } = require("internal/streams/iter/utils");

    // Maximum number of chunks to yield per batch from from()/fromSync().
    // Bounds peak memory when arrays flow through transforms, which must
    // allocate output for the entire batch at once.
    const FROM_BATCH_SIZE = 128;

    // =============================================================================
    // Type Guards and Detection
    // =============================================================================

    /**
     * Check if value is a primitive chunk (string, ArrayBuffer, or ArrayBufferView).
     * @returns {boolean}
     */
    function isPrimitiveChunk(value) {
      return typeof value === "string" || isAnyArrayBuffer(value) || ArrayBuffer.isView(value);
    }

    /**
     * Check if value is a sync iterable (has Symbol.iterator).
     * @returns {boolean}
     */
    function isSyncIterable(value) {
      // We do not consider regular strings to be sync iterables in this context.
      // We don't care about boxed strings (String objects) since they are uncommon.
      return typeof value !== "string" && typeof value?.[Symbol.iterator] === "function";
    }

    /**
     * Check if value is an async iterable (has Symbol.asyncIterator).
     * @returns {boolean}
     */
    function isAsyncIterable(value) {
      return typeof value?.[Symbol.asyncIterator] === "function";
    }

    // =============================================================================
    // Primitive Conversion
    // =============================================================================

    /**
     * Convert a primitive chunk to Uint8Array.
     * - string: UTF-8 encoded
     * - ArrayBuffer: wrapped as Uint8Array view (no copy)
     * - ArrayBufferView: converted to Uint8Array view of same memory
     * @param {string|ArrayBuffer|ArrayBufferView} chunk
     * @returns {Uint8Array}
     */
    function primitiveToUint8Array(chunk) {
      if (typeof chunk === "string") {
        return toUint8Array(chunk);
      }
      if (isAnyArrayBuffer(chunk)) {
        return new Uint8Array(chunk);
      }
      if (isUint8Array(chunk)) {
        return chunk;
      }
      // Other ArrayBufferView types (Int8Array, DataView, etc.)
      return arrayBufferViewToUint8Array(chunk);
    }

    function arrayBufferViewToUint8Array(chunk) {
      if (isTypedArray(chunk)) {
        return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
      }
      return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    }

    // =============================================================================
    // Sync Normalization (for fromSync and sync contexts)
    // =============================================================================

    /**
     * Normalize a sync streamable yield value to Uint8Array chunks.
     * Recursively flattens arrays, iterables, and protocol conversions.
     * @yields {Uint8Array}
     */
    function* normalizeSyncValue(value) {
      // Handle primitives
      if (isPrimitiveChunk(value)) {
        yield primitiveToUint8Array(value);
        return;
      }

      // Handle ToStreamable protocol
      if (hasProtocol(value, toStreamable)) {
        const result = value[toStreamable]();
        yield* normalizeSyncValue(result);
        return;
      }

      // Handle arrays (which are also iterable, but check first for efficiency)
      if (Array.isArray(value)) {
        for (let i = 0; i < value.length; i++) {
          yield* normalizeSyncValue(value[i]);
        }
        return;
      }

      // Handle other sync iterables
      if (isSyncIterable(value)) {
        for (const item of value) {
          yield* normalizeSyncValue(item);
        }
        return;
      }

      // Reject: no valid conversion
      throw $ERR_INVALID_ARG_TYPE("value", ["string", "ArrayBuffer", "ArrayBufferView", "Iterable", "toStreamable"], value);
    }

    /**
     * Check if value is already a Uint8Array[] batch (fast path).
     * @returns {boolean}
     */
    function isUint8ArrayBatch(value) {
      if (!Array.isArray(value)) return false;
      const len = value.length;
      if (len === 0) return true;
      // Fast path: single-element batch (most common from transforms)
      if (len === 1) return isUint8Array(value[0]);
      // Check first and last before iterating all elements
      if (!isUint8Array(value[0]) || !isUint8Array(value[len - 1])) return false;
      if (len === 2) return true;
      for (let i = 1; i < len - 1; i++) {
        if (!isUint8Array(value[i])) return false;
      }
      return true;
    }

    function* yieldBoundedBatch(batch) {
      if (batch.length === 0) {
        return;
      }
      if (batch.length <= FROM_BATCH_SIZE) {
        yield batch;
        return;
      }
      for (let i = 0; i < batch.length; i += FROM_BATCH_SIZE) {
        yield batch.slice(i, i + FROM_BATCH_SIZE);
      }
    }

    /**
     * Normalize a sync streamable source, yielding batches of Uint8Array.
     * @param {Iterable} source
     * @yields {Uint8Array[]}
     */
    function* normalizeSyncSource(source) {
      let batch = [];

      for (const value of source) {
        // Fast path 1: value is already a Uint8Array[] batch
        if (isUint8ArrayBatch(value)) {
          if (batch.length > 0) {
            yield batch;
            batch = [];
          }
          yield* yieldBoundedBatch(value);
          continue;
        }
        // Fast path 2: value is a single Uint8Array (very common)
        if (isUint8Array(value)) {
          batch.push(value);
          if (batch.length === FROM_BATCH_SIZE) {
            yield batch;
            batch = [];
          }
          continue;
        }
        // Slow path: normalize the value
        if (batch.length > 0) {
          yield batch;
          batch = [];
        }
        let valueBatch = [];
        for (const chunk of normalizeSyncValue(value)) {
          valueBatch.push(chunk);
          if (valueBatch.length === FROM_BATCH_SIZE) {
            yield valueBatch;
            valueBatch = [];
          }
        }
        if (valueBatch.length > 0) {
          yield valueBatch;
        }
      }

      if (batch.length > 0) {
        yield batch;
      }
    }

    // =============================================================================
    // Async Normalization (for from and async contexts)
    // =============================================================================

    /**
     * Normalize an async streamable yield value to Uint8Array chunks.
     * Recursively flattens arrays, iterables, async iterables, promises,
     * and protocol conversions.
     * @yields {Uint8Array}
     */
    async function* normalizeAsyncValue(value) {
      // Handle promises first
      if (isPromise(value)) {
        const resolved = await value;
        yield* normalizeAsyncValue(resolved);
        return;
      }

      // Handle primitives
      if (isPrimitiveChunk(value)) {
        yield primitiveToUint8Array(value);
        return;
      }

      // Handle ToAsyncStreamable protocol (check before ToStreamable)
      if (hasProtocol(value, toAsyncStreamable)) {
        const result = value[toAsyncStreamable]();
        if (isPromise(result)) {
          yield* normalizeAsyncValue(await result);
        } else {
          yield* normalizeAsyncValue(result);
        }
        return;
      }

      // Handle ToStreamable protocol
      if (hasProtocol(value, toStreamable)) {
        const result = value[toStreamable]();
        yield* normalizeAsyncValue(result);
        return;
      }

      // Handle arrays (which are also iterable, but check first for efficiency)
      if (Array.isArray(value)) {
        for (let i = 0; i < value.length; i++) {
          yield* normalizeAsyncValue(value[i]);
        }
        return;
      }

      // Handle async iterables (check before sync iterables since some objects
      // have both)
      if (isAsyncIterable(value)) {
        for await (const item of value) {
          yield* normalizeAsyncValue(item);
        }
        return;
      }

      // Handle sync iterables
      if (isSyncIterable(value)) {
        for (const item of value) {
          yield* normalizeAsyncValue(item);
        }
        return;
      }

      // Reject: no valid conversion
      throw $ERR_INVALID_ARG_TYPE(
        "value",
        ["string", "ArrayBuffer", "ArrayBufferView", "Iterable", "AsyncIterable", "toStreamable", "toAsyncStreamable"],
        value,
      );
    }

    /**
     * Normalize an async streamable source, yielding batches of Uint8Array.
     * @param {AsyncIterable|Iterable} source
     * @yields {Uint8Array[]}
     */
    async function* normalizeAsyncSource(source) {
      // Prefer async iteration if available
      if (isAsyncIterable(source)) {
        for await (const value of source) {
          // Fast path 1: value is already a Uint8Array[] batch
          if (isUint8ArrayBatch(value)) {
            if (value.length > 0) {
              yield value;
            }
            continue;
          }
          // Fast path 2: value is a single Uint8Array (very common)
          if (isUint8Array(value)) {
            yield [value];
            continue;
          }
          // Slow path: normalize the value
          const batch = [];
          for await (const chunk of normalizeAsyncValue(value)) {
            batch.push(chunk);
          }
          if (batch.length > 0) {
            yield batch;
          }
        }
        return;
      }

      // Fall back to sync iteration - batch sync values together with a bound.
      if (isSyncIterable(source)) {
        let batch = [];

        for (const value of source) {
          // Fast path 1: value is already a Uint8Array[] batch
          if (isUint8ArrayBatch(value)) {
            // Flush any accumulated batch first
            if (batch.length > 0) {
              yield batch;
              batch = [];
            }
            yield* yieldBoundedBatch(value);
            continue;
          }
          // Fast path 2: value is a single Uint8Array (very common)
          if (isUint8Array(value)) {
            batch.push(value);
            if (batch.length === FROM_BATCH_SIZE) {
              yield batch;
              batch = [];
            }
            continue;
          }
          // Slow path: normalize the value - must flush and yield individually
          if (batch.length > 0) {
            yield batch;
            batch = [];
          }
          let asyncBatch = [];
          for await (const chunk of normalizeAsyncValue(value)) {
            asyncBatch.push(chunk);
            if (asyncBatch.length === FROM_BATCH_SIZE) {
              yield asyncBatch;
              asyncBatch = [];
            }
          }
          if (asyncBatch.length > 0) {
            yield asyncBatch;
          }
        }

        // Yield any remaining batched values
        if (batch.length > 0) {
          yield batch;
        }
        return;
      }

      throw $ERR_INVALID_ARG_TYPE("source", ["Iterable", "AsyncIterable"], source);
    }

    // =============================================================================
    // Public API: from() and fromSync()
    // =============================================================================

    /**
     * Create a SyncByteStreamReadable from a ByteInput or SyncStreamable.
     * @param {string|ArrayBuffer|ArrayBufferView|Iterable} input
     * @returns {Iterable<Uint8Array[]>}
     */
    function fromSync(input) {
      if (input == null) {
        throw $ERR_INVALID_ARG_TYPE("input", "a non-null value", input);
      }

      // Check for primitives first (ByteInput)
      if (isPrimitiveChunk(input)) {
        const chunk = primitiveToUint8Array(input);
        return {
          __proto__: null,
          *[Symbol.iterator]() {
            yield [chunk];
          },
        };
      }

      // Fast path: Uint8Array[] - yield in bounded sub-batches.
      // Yielding the entire array as one batch forces downstream transforms
      // to process all data at once, causing peak memory proportional to total
      // data volume. Sub-batching keeps peak memory bounded while preserving
      // the throughput benefit of batched processing.
      if (Array.isArray(input)) {
        if (input.length === 0) {
          return {
            __proto__: null,
            *[Symbol.iterator]() {
              // Empty - yield nothing
            },
          };
        }
        // Check if it's an array of Uint8Array (common case)
        if (isUint8Array(input[0])) {
          const allUint8 = input.every(isUint8Array);
          if (allUint8) {
            const batch = input;
            return {
              __proto__: null,
              *[Symbol.iterator]() {
                if (batch.length <= FROM_BATCH_SIZE) {
                  yield batch;
                } else {
                  for (let i = 0; i < batch.length; i += FROM_BATCH_SIZE) {
                    yield batch.slice(i, i + FROM_BATCH_SIZE);
                  }
                }
              },
            };
          }
        }
      }

      // Check toStreamable protocol (takes precedence over iteration protocols).
      // toAsyncStreamable is ignored entirely in fromSync.
      if (typeof input[toStreamable] === "function") {
        return fromSync(input[toStreamable]());
      }

      // Reject explicit async inputs
      if (isAsyncIterable(input)) {
        throw $ERR_INVALID_ARG_TYPE("input", "a synchronous input (not AsyncIterable)", input);
      }
      if (typeof input === "object" && input !== null && typeof input.then === "function") {
        throw $ERR_INVALID_ARG_TYPE("input", "a synchronous input (not Promise)", input);
      }

      // Must be a SyncStreamable
      if (!isSyncIterable(input)) {
        throw $ERR_INVALID_ARG_TYPE(
          "input",
          ["string", "ArrayBuffer", "ArrayBufferView", "Iterable", "toStreamable"],
          input,
        );
      }

      return {
        __proto__: null,
        *[Symbol.iterator]() {
          yield* normalizeSyncSource(input);
        },
      };
    }

    /**
     * Create a ByteStreamReadable from a ByteInput or Streamable.
     * @param {string|ArrayBuffer|ArrayBufferView|Iterable|AsyncIterable} input
     * @returns {AsyncIterable<Uint8Array[]>}
     */
    function from(input) {
      if (input == null) {
        throw $ERR_INVALID_ARG_TYPE("input", "a non-null value", input);
      }

      // Fast path: validated source already yields valid Uint8Array[] batches
      if (input[kValidatedSource]) {
        return input;
      }

      // Check for primitives first (ByteInput)
      if (isPrimitiveChunk(input)) {
        const chunk = primitiveToUint8Array(input);
        return {
          __proto__: null,
          async *[Symbol.asyncIterator]() {
            yield [chunk];
          },
        };
      }

      // Fast path: Uint8Array[] - yield in bounded sub-batches.
      // Yielding the entire array as one batch forces downstream transforms
      // to process all data at once, causing peak memory proportional to total
      // data volume. Sub-batching keeps peak memory bounded while preserving
      // the throughput benefit of batched processing.
      if (Array.isArray(input)) {
        if (input.length === 0) {
          return {
            __proto__: null,
            async *[Symbol.asyncIterator]() {
              // Empty - yield nothing
            },
          };
        }
        if (isUint8Array(input[0])) {
          const allUint8 = input.every(isUint8Array);
          if (allUint8) {
            const batch = input;
            return {
              __proto__: null,
              async *[Symbol.asyncIterator]() {
                if (batch.length <= FROM_BATCH_SIZE) {
                  yield batch;
                } else {
                  for (let i = 0; i < batch.length; i += FROM_BATCH_SIZE) {
                    yield batch.slice(i, i + FROM_BATCH_SIZE);
                  }
                }
              },
            };
          }
        }
      }

      // Check toAsyncStreamable protocol (takes precedence over toStreamable and
      // iteration protocols)
      if (typeof input[toAsyncStreamable] === "function") {
        const result = input[toAsyncStreamable]();
        // Synchronous validated source (e.g. Readable batched iterator)
        if (result?.[kValidatedSource]) {
          return result;
        }
        return {
          __proto__: null,
          async *[Symbol.asyncIterator]() {
            // The result may be a Promise. Check validated on both the Promise
            // itself (if tagged) and the resolved value.
            const resolved = await result;
            if (resolved?.[kValidatedSource]) {
              yield* resolved[Symbol.asyncIterator]();
              return;
            }
            yield* from(resolved)[Symbol.asyncIterator]();
          },
        };
      }

      // Check toStreamable protocol (takes precedence over iteration protocols)
      if (typeof input[toStreamable] === "function") {
        return from(input[toStreamable]());
      }

      // Must be a Streamable (sync or async iterable)
      if (!isSyncIterable(input) && !isAsyncIterable(input)) {
        throw $ERR_INVALID_ARG_TYPE(
          "input",
          ["string", "ArrayBuffer", "ArrayBufferView", "Iterable", "AsyncIterable", "toStreamable", "toAsyncStreamable"],
          input,
        );
      }

      return normalizeAsyncSource(input);
    }

    // =============================================================================
    // Exports
    // =============================================================================

    module.exports = {
      arrayBufferViewToUint8Array,
      from,
      fromSync,
      isAsyncIterable,
      isPrimitiveChunk,
      isSyncIterable,
      isUint8ArrayBatch,
      normalizeAsyncSource,
      normalizeAsyncValue,
      normalizeSyncSource,
      normalizeSyncValue,
      primitiveToUint8Array,
    };
  });
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
