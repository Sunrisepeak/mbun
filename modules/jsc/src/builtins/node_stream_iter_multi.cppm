// node:stream/iter payload partition — broadcast() (push fan-out) and share()/shareSync() (pull fan-out)
//
// Mechanical 1:1 translation of bun-ref src/js/internal/streams/iter/{broadcast,share}.ts
// (三段法 stage 1): JSDoc-typed JS with bun's $-intrinsics lowered onto the shims
// in node_stream_core.cppm; algorithm, branches and error text are the
// blueprint's, not reinvented. The modules are registered on the same CJS
// registry node:stream uses, so the blueprint's lazy cross-requires resolve
// exactly as in bun.
//
// NOTE: appended AFTER the master builtins IIFE has closed (see image_closure),
// so this is a self-contained IIFE that re-binds G = globalThis and pulls shared
// helpers off G.__mbunStreamReg.
export module mbun.jsc.js_builtins:node_stream_iter_multi;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeStreamIterMultiJS = R"JS(
(function () {
  const G = globalThis;
  const R = G.__mbunStreamReg;
  if (!R) return;
  const process = G.process;
  const Buffer = G.Buffer;
  const { $ERR_INVALID_ARG_TYPE, $ERR_INVALID_ARG_VALUE, $ERR_INVALID_ARG_VALUE_RangeError, $ERR_INVALID_RETURN_VALUE, $ERR_OUT_OF_RANGE, $ERR_INVALID_STATE, $ERR_INVALID_STATE_TypeError, $ERR_INVALID_STATE_RangeError, $ERR_OPERATION_FAILED, $ERR_STREAM_WRITE_AFTER_END, $makeAbortError, __isCallable, __debug, __assert } = R.H;

  R.def("internal/streams/iter/broadcast", function (require, module, exports) {

    // New Streams API - Broadcast
    //
    // Push-model multi-consumer streaming. A single writer can push data to
    // multiple consumers. Each consumer has an independent cursor into a
    // shared buffer.

    const { validateAbortSignal, validateInteger, validateObject } = require("internal/validators");

    const { broadcastProtocol, drainableProtocol } = require("internal/streams/iter/types");

    const { isAsyncIterable, isSyncIterable } = require("internal/streams/iter/from");

    const { pull: pullWithTransforms } = require("internal/streams/iter/pull");

    const {
      kMultiConsumerDefaultHWM,
      kResolvedPromise,
      clampHWM,
      convertChunks,
      getMinCursor,
      hasProtocol,
      onSignalAbort,
      parsePullArgs,
      wrapError,
      toUint8Array,
      validateBackpressure,
    } = require("internal/streams/iter/utils");

    const { RingBuffer } = require("internal/streams/iter/ringbuffer");

    const PromiseWithResolvers = Promise.withResolvers.bind(Promise);
    const SymbolAsyncDispose = Symbol.asyncDispose;
    const SymbolAsyncIterator = Symbol.asyncIterator;
    const SymbolDispose = Symbol.dispose;

    const kCancelWriter = Symbol("kCancelWriter");
    const kWrite = Symbol("kWrite");
    const kEnd = Symbol("kEnd");
    const kAbort = Symbol("kAbort");
    const kGetDesiredSize = Symbol("kGetDesiredSize");
    const kCanWrite = Symbol("kCanWrite");
    const kOnBufferDrained = Symbol("kOnBufferDrained");

    // =============================================================================
    // Broadcast Implementation
    // =============================================================================

    class BroadcastImpl {
      #buffer = new RingBuffer();
      #bufferStart = 0;
      #consumers = new Set();
      #waiters = []; // Consumers with pending resolve (subset of #consumers)
      #ended = false;
      #error = null;
      #cancelled = false;
      #options;
      #writer = null;
      #cachedMinCursor = 0;
      #cachedMinCursorConsumers = 0;

      constructor(options) {
        this.#options = options;
        this[kOnBufferDrained] = null;
      }

      setWriter(writer) {
        this.#writer = writer;
      }

      get backpressurePolicy() {
        return this.#options.backpressure;
      }

      get highWaterMark() {
        return this.#options.highWaterMark;
      }

      get consumerCount() {
        return this.#consumers.size;
      }

      get bufferSize() {
        return this.#buffer.length;
      }

      push(...args) {
        const { transforms, options } = parsePullArgs(args);
        const rawConsumer = this.#createRawConsumer();

        // When transforms are present, delegate to pull() which creates its
        // own internal AbortController that follows the external signal.
        // When no transforms, return rawConsumer directly (controller elided
        // per PULL-02 optimization -- no transforms means no signal recipient).
        if (transforms.length > 0) {
          const pullArgs = [...transforms];
          if (options?.signal) {
            pullArgs.push({ __proto__: null, signal: options.signal });
          }
          return pullWithTransforms(rawConsumer, ...pullArgs);
        }
        return rawConsumer;
      }

      #createRawConsumer() {
        const state = {
          __proto__: null,
          // Start at the oldest buffered entry so late-joining consumers
          // can read data already in the buffer.
          cursor: this.#bufferStart,
          resolve: null,
          reject: null,
          detached: false,
        };

        this.#consumers.add(state);
        if (this.#consumers.size === 1) {
          this.#cachedMinCursor = state.cursor;
          this.#cachedMinCursorConsumers = 1;
        } else if (state.cursor === this.#cachedMinCursor) {
          this.#cachedMinCursorConsumers++;
        } else {
          this.#recomputeMinCursor();
        }
        const self = this;

        const kDone = Promise.resolve({ __proto__: null, done: true, value: undefined });

        function detach() {
          state.detached = true;
          state.resolve = null;
          state.reject = null;
          if (self.#deleteConsumer(state)) {
            self.#tryTrimBuffer();
          }
        }

        return {
          __proto__: null,
          [SymbolAsyncIterator]() {
            return {
              __proto__: null,
              next() {
                if (state.detached) {
                  if (self.#error) return Promise.reject(self.#error);
                  return kDone;
                }

                const bufferIndex = state.cursor - self.#bufferStart;
                if (bufferIndex < self.#buffer.length) {
                  const chunk = self.#buffer.get(bufferIndex);
                  const cursor = state.cursor;
                  state.cursor++;
                  if (cursor === self.#cachedMinCursor && --self.#cachedMinCursorConsumers === 0) {
                    self.#tryTrimBuffer();
                  }
                  return Promise.resolve({ __proto__: null, done: false, value: chunk });
                }

                if (self.#error) {
                  state.detached = true;
                  self.#deleteConsumer(state);
                  return Promise.reject(self.#error);
                }

                if (self.#ended || self.#cancelled) {
                  detach();
                  return kDone;
                }

                const { promise, resolve, reject } = PromiseWithResolvers();
                state.resolve = resolve;
                state.reject = reject;
                self.#waiters.push(state);
                return promise;
              },

              return() {
                detach();
                return kDone;
              },

              throw() {
                detach();
                return kDone;
              },
            };
          },
        };
      }

      cancel(reason) {
        if (this.#cancelled) return;
        this.#cancelled = true;
        this.#ended = true; // Prevents [kAbort]() from redundantly iterating consumers

        if (reason !== undefined) {
          this.#error = reason;
        }

        // Reject pending writes on the writer so the pump doesn't hang
        this.#writer?.[kCancelWriter]();

        for (const consumer of this.#consumers) {
          if (consumer.resolve) {
            if (reason !== undefined) {
              consumer.reject?.(reason);
            } else {
              consumer.resolve({ __proto__: null, done: true, value: undefined });
            }
            consumer.resolve = null;
            consumer.reject = null;
          }
          consumer.detached = true;
        }
        this.#consumers.clear();
        this.#cachedMinCursorConsumers = 0;
      }

      [SymbolDispose]() {
        this.cancel();
      }

      // Methods accessed by BroadcastWriter via symbol keys

      [kWrite](chunk) {
        if (this.#ended || this.#cancelled) return false;

        if (this.#buffer.length >= this.#options.highWaterMark) {
          switch (this.#options.backpressure) {
            case "strict":
            case "block":
              return false;
            case "drop-oldest":
              this.#buffer.shift();
              this.#bufferStart++;
              for (const consumer of this.#consumers) {
                if (consumer.cursor < this.#bufferStart) {
                  this.#deleteConsumerFromMin(consumer);
                  consumer.cursor = this.#bufferStart;
                }
              }
              this.#recomputeMinCursor();
              break;
            case "drop-newest":
              return true;
          }
        }

        this.#buffer.push(chunk);
        this.#notifyConsumers();
        return true;
      }

      [kEnd]() {
        if (this.#ended) return;
        this.#ended = true;

        for (const consumer of this.#consumers) {
          if (consumer.resolve) {
            const bufferIndex = consumer.cursor - this.#bufferStart;
            if (bufferIndex < this.#buffer.length) {
              const chunk = this.#buffer.get(bufferIndex);
              const cursor = consumer.cursor;
              consumer.cursor++;
              if (cursor === this.#cachedMinCursor && --this.#cachedMinCursorConsumers === 0) {
                this.#tryTrimBuffer();
              }
              consumer.resolve({ __proto__: null, done: false, value: chunk });
            } else {
              consumer.resolve({ __proto__: null, done: true, value: undefined });
            }
            consumer.resolve = null;
            consumer.reject = null;
          }
        }
      }

      [kAbort](reason) {
        if (this.#ended || this.#error) return;
        this.#error = reason;
        this.#ended = true;

        // Notify all waiting consumers and detach them
        for (const consumer of this.#consumers) {
          if (consumer.reject) {
            consumer.reject(reason);
            consumer.resolve = null;
            consumer.reject = null;
          }
          consumer.detached = true;
        }
        this.#consumers.clear();
        this.#cachedMinCursorConsumers = 0;
      }

      [kGetDesiredSize]() {
        if (this.#ended || this.#cancelled) return null;
        return Math.max(0, this.#options.highWaterMark - this.#buffer.length);
      }

      [kCanWrite]() {
        if (this.#ended || this.#cancelled) return false;
        if (
          (this.#options.backpressure === "strict" || this.#options.backpressure === "block") &&
          this.#buffer.length >= this.#options.highWaterMark
        ) {
          return false;
        }
        return true;
      }

      // Private methods

      #recomputeMinCursor() {
        const { minCursor, minCursorConsumers } = getMinCursor(this.#consumers, this.#bufferStart + this.#buffer.length);
        this.#cachedMinCursor = minCursor;
        this.#cachedMinCursorConsumers = minCursorConsumers;
      }

      #tryTrimBuffer() {
        if (this.#cachedMinCursorConsumers === 0) {
          this.#recomputeMinCursor();
        }
        const trimCount = this.#cachedMinCursor - this.#bufferStart;
        if (trimCount > 0) {
          this.#buffer.trimFront(trimCount);
          this.#bufferStart = this.#cachedMinCursor;

          if (this[kOnBufferDrained] && this.#buffer.length < this.#options.highWaterMark) {
            this[kOnBufferDrained]();
          }
        }
      }

      #notifyConsumers() {
        const waiters = this.#waiters;
        if (waiters.length === 0) return;
        // Swap out the waiters list so consumers that re-wait during
        // resolve don't get processed twice in this cycle.
        this.#waiters = [];
        for (let i = 0; i < waiters.length; i++) {
          const consumer = waiters[i];
          if (consumer.resolve) {
            const bufferIndex = consumer.cursor - this.#bufferStart;
            if (bufferIndex < this.#buffer.length) {
              const chunk = this.#buffer.get(bufferIndex);
              const cursor = consumer.cursor;
              consumer.cursor++;
              if (cursor === this.#cachedMinCursor && --this.#cachedMinCursorConsumers === 0) {
                this.#tryTrimBuffer();
              }
              const resolve = consumer.resolve;
              consumer.resolve = null;
              consumer.reject = null;
              resolve({ __proto__: null, done: false, value: chunk });
            } else {
              // Still waiting -- put back
              this.#waiters.push(consumer);
            }
          }
        }
      }

      #deleteConsumerFromMin(consumer) {
        if (consumer.cursor === this.#cachedMinCursor) {
          this.#cachedMinCursorConsumers--;
          return this.#cachedMinCursorConsumers === 0;
        }
        return false;
      }

      #deleteConsumer(consumer) {
        if (this.#consumers.delete(consumer)) {
          return this.#deleteConsumerFromMin(consumer);
        }
        return false;
      }
    }

    // =============================================================================
    // BroadcastWriter
    // =============================================================================

    let getBroadcastPendingWrites;

    class BroadcastWriter {
      #broadcast;
      #totalBytes = 0;
      #closed;
      #aborted = false;
      #pendingWrites = new RingBuffer();
      #pendingDrains = [];

      static {
        // Used in wireBroadcastWriteSignal ensure the signal listener can be
        // constructed without closing over the chunk data, which may be large.
        getBroadcastPendingWrites = obj => obj.#pendingWrites;
      }

      constructor(broadcastImpl) {
        this.#broadcast = broadcastImpl;

        this.#broadcast[kOnBufferDrained] = () => {
          this.#resolvePendingWrites();
          this.#resolvePendingDrains(true);
        };
      }

      // The drainable protocol works with Stream.ondrain to provide a notification
      // when the writer can accept more data after being backpressured.
      [drainableProtocol]() {
        const desired = this.desiredSize;
        if (desired === null) return null;
        if (desired > 0) return Promise.resolve(true);
        const { promise, resolve, reject } = PromiseWithResolvers();
        this.#pendingDrains.push({ __proto__: null, resolve, reject });
        return promise;
      }

      #isClosed() {
        return this.#closed !== undefined;
      }

      #isClosedOrAborted() {
        return this.#isClosed() || this.#aborted;
      }

      get desiredSize() {
        return this.#isClosedOrAborted() ? null : this.#broadcast[kGetDesiredSize]();
      }

      #canUseWriteFastPath(options) {
        return !options?.signal && !this.#isClosed() && !this.#aborted && this.#broadcast[kCanWrite]();
      }

      write(chunk, options) {
        // Fast path: no signal, writer open, buffer has space
        if (this.#canUseWriteFastPath(options)) {
          const converted = toUint8Array(chunk);
          this.#broadcast[kWrite]([converted]);
          this.#totalBytes += converted.byteLength;
          return kResolvedPromise;
        }
        return this.#writevSlow([chunk], options);
      }

      writev(chunks, options) {
        if (!Array.isArray(chunks)) {
          throw $ERR_INVALID_ARG_TYPE("chunks", "Array", chunks);
        }
        // Fast path: no signal, writer open, buffer has space
        if (this.#canUseWriteFastPath(options)) {
          const converted = convertChunks(chunks);
          this.#broadcast[kWrite](converted);
          for (let i = 0; i < converted.length; i++) {
            this.#totalBytes += converted[i].byteLength;
          }
          return kResolvedPromise;
        }
        return this.#writevSlow(chunks, options);
      }

      async #writevSlow(chunks, options) {
        const signal = options?.signal;

        // Check for pre-aborted
        signal?.throwIfAborted();

        if (this.#isClosedOrAborted()) {
          throw $ERR_INVALID_STATE_TypeError("Writer is closed");
        }

        const converted = convertChunks(chunks);

        if (this.#broadcast[kWrite](converted)) {
          for (let i = 0; i < converted.length; i++) {
            this.#totalBytes += converted[i].byteLength;
          }
          return;
        }

        const policy = this.#broadcast.backpressurePolicy;
        const hwm = this.#broadcast.highWaterMark;

        if (policy === "strict") {
          if (this.#pendingWrites.length >= hwm) {
            throw $ERR_INVALID_STATE_TypeError(
              "Backpressure violation: too many pending writes. " + "Await each write() call to respect backpressure.",
            );
          }
          return this.#createPendingWrite(converted, signal);
        }

        // 'block' policy
        return this.#createPendingWrite(converted, signal);
      }

      writeSync(chunk) {
        if (this.#isClosedOrAborted()) return false;
        if (!this.#broadcast[kCanWrite]()) return false;
        const converted = toUint8Array(chunk);
        if (this.#broadcast[kWrite]([converted])) {
          this.#totalBytes += converted.byteLength;
          return true;
        }
        return false;
      }

      writevSync(chunks) {
        if (!Array.isArray(chunks)) {
          throw $ERR_INVALID_ARG_TYPE("chunks", "Array", chunks);
        }
        if (this.#isClosedOrAborted()) return false;
        if (!this.#broadcast[kCanWrite]()) return false;
        const converted = convertChunks(chunks);
        if (this.#broadcast[kWrite](converted)) {
          for (let i = 0; i < converted.length; i++) {
            this.#totalBytes += converted[i].byteLength;
          }
          return true;
        }
        return false;
      }

      // end() is synchronous internally - signal accepted for interface compliance.
      end(_options) {
        if (this.#isClosed()) return this.#closed;
        this.#closed = Promise.resolve(this.#totalBytes);
        this.#broadcast[kEnd]();
        this.#resolvePendingDrains(false);
        return this.#closed;
      }

      endSync() {
        if (this.#closed) return this.#totalBytes;
        this.#closed = Promise.resolve(this.#totalBytes);
        this.#broadcast[kEnd]();
        this.#resolvePendingDrains(false);
        return this.#totalBytes;
      }

      fail(reason) {
        if (this.#isClosedOrAborted()) return;
        this.#aborted = true;
        this.#closed = Promise.resolve(this.#totalBytes);
        const error = reason ?? $ERR_INVALID_STATE_TypeError("Failed");
        this.#rejectPendingWrites(error);
        this.#rejectPendingDrains(error);
        this.#broadcast[kAbort](error);
      }

      [SymbolAsyncDispose]() {
        this.fail();
        return Promise.resolve();
      }

      [SymbolDispose]() {
        this.fail();
      }

      [kCancelWriter]() {
        if (this.#isClosed()) return;
        this.#closed = Promise.resolve(this.#totalBytes);
        this.#rejectPendingWrites($makeAbortError("Broadcast cancelled"));
        this.#resolvePendingDrains(false);
      }

      /**
       * Create a pending write promise, optionally racing against a signal.
       * If the signal fires, the entry is removed from pendingWrites and the
       * promise rejects. Signal listeners are cleaned up on normal resolution.
       * @returns {Promise<void>}
       */
      #createPendingWrite(chunk, signal) {
        const { promise, resolve, reject } = PromiseWithResolvers();
        const entry = { __proto__: null, chunk, resolve, reject };
        this.#pendingWrites.push(entry);
        if (signal) {
          wireBroadcastWriteSignal(entry, signal, resolve, reject, this);
        }
        return promise;
      }

      #resolvePendingWrites() {
        while (this.#pendingWrites.length > 0 && this.#broadcast[kCanWrite]()) {
          const pending = this.#pendingWrites.shift();
          const chunk = pending.chunk;
          if (this.#broadcast[kWrite](chunk)) {
            for (let i = 0; i < chunk.length; i++) {
              this.#totalBytes += chunk[i].byteLength;
            }
            pending.resolve();
          } else {
            this.#pendingWrites.unshift(pending);
            break;
          }
        }
      }

      #rejectPendingWrites(error) {
        while (this.#pendingWrites.length > 0) {
          this.#pendingWrites.shift().reject(error);
        }
      }

      #resolvePendingDrains(canWrite) {
        const drains = this.#pendingDrains;
        this.#pendingDrains = [];
        for (let i = 0; i < drains.length; i++) {
          drains[i].resolve(canWrite);
        }
      }

      #rejectPendingDrains(error) {
        const drains = this.#pendingDrains;
        this.#pendingDrains = [];
        for (let i = 0; i < drains.length; i++) {
          drains[i].reject(error);
        }
      }
    }

    function wireBroadcastWriteSignal(entry, signal, resolve, reject, self) {
      const onAbort = () => {
        const pendingWrites = getBroadcastPendingWrites(self);
        const idx = pendingWrites.indexOf(entry);
        if (idx !== -1) pendingWrites.removeAt(idx);
        entry.chunk = null;
        reject(signal.reason ?? $makeAbortError("Aborted"));
      };
      entry.resolve = function () {
        signal.removeEventListener("abort", onAbort);
        entry.chunk = null;
        resolve();
      };
      entry.reject = function (reason) {
        signal.removeEventListener("abort", onAbort);
        entry.chunk = null;
        reject(reason);
      };
      signal.addEventListener("abort", onAbort, { __proto__: null, once: true });
    }

    function onBroadcastCancel(broadcastImpl, signal) {
      onSignalAbort(signal, () => broadcastImpl.cancel(signal.reason));
    }

    // =============================================================================
    // Public API
    // =============================================================================

    /**
     * Create a broadcast channel for push-model multi-consumer streaming.
     * @param {{ highWaterMark?: number, backpressure?: string, signal?: AbortSignal }} [options]
     * @returns {{ writer: Writer, broadcast: Broadcast }}
     */
    function broadcast(options = { __proto__: null }) {
      validateObject(options, "options");
      const { highWaterMark = kMultiConsumerDefaultHWM, backpressure = "strict", signal } = options;
      validateInteger(highWaterMark, "options.highWaterMark");
      validateBackpressure(backpressure);
      if (signal !== undefined) {
        validateAbortSignal(signal, "options.signal");
      }

      const opts = {
        __proto__: null,
        highWaterMark: clampHWM(highWaterMark),
        backpressure,
        signal,
      };

      const broadcastImpl = new BroadcastImpl(opts);
      const writer = new BroadcastWriter(broadcastImpl);
      broadcastImpl.setWriter(writer);

      if (signal) {
        onBroadcastCancel(broadcastImpl, signal);
      }

      return { __proto__: null, writer, broadcast: broadcastImpl };
    }

    function isBroadcastable(value) {
      return hasProtocol(value, broadcastProtocol);
    }

    const Broadcast = {
      __proto__: null,
      from(input, options) {
        if (isBroadcastable(input)) {
          const bc = input[broadcastProtocol](options);
          if (bc === null || typeof bc !== "object") {
            throw $ERR_INVALID_RETURN_VALUE("an object", "[Symbol.for('Stream.broadcastProtocol')]", bc);
          }
          return { __proto__: null, writer: { __proto__: null }, broadcast: bc };
        }

        if (!isAsyncIterable(input) && !isSyncIterable(input)) {
          throw $ERR_INVALID_ARG_TYPE("input", ["Broadcastable", "AsyncIterable", "Iterable"], input);
        }

        const result = broadcast(options);
        const signal = options?.signal;

        const pump = async () => {
          const w = result.writer;
          try {
            if (isAsyncIterable(input)) {
              for await (const chunks of input) {
                signal?.throwIfAborted();
                if (Array.isArray(chunks)) {
                  if (!w.writevSync(chunks)) {
                    await w.writev(chunks, signal ? { signal } : undefined);
                  }
                } else if (!w.writeSync(chunks)) {
                  await w.write(chunks, signal ? { signal } : undefined);
                }
              }
            } else if (isSyncIterable(input)) {
              for (const chunks of input) {
                signal?.throwIfAborted();
                if (Array.isArray(chunks)) {
                  if (!w.writevSync(chunks)) {
                    await w.writev(chunks, signal ? { signal } : undefined);
                  }
                } else if (!w.writeSync(chunks)) {
                  await w.write(chunks, signal ? { signal } : undefined);
                }
              }
            }
            if (w.endSync() < 0) {
              await w.end(signal ? { signal } : undefined);
            }
          } catch (error) {
            w.fail(wrapError(error));
          }
        };
        pump().then(undefined, () => {});

        return result;
      },
    };

    module.exports = {
      Broadcast,
      broadcast,
    };
  });

  R.def("internal/streams/iter/share", function (require, module, exports) {

    // New Streams API - Share
    //
    // Pull-model multi-consumer streaming. Shares a single source among
    // multiple consumers with explicit buffering.

    const { shareProtocol, shareSyncProtocol } = require("internal/streams/iter/types");

    const { from, fromSync, isAsyncIterable, isSyncIterable } = require("internal/streams/iter/from");

    const { pull: pullWithTransforms, pullSync: pullSyncWithTransforms } = require("internal/streams/iter/pull");

    const {
      kMultiConsumerDefaultHWM,
      clampHWM,
      getMinCursor,
      hasProtocol,
      onSignalAbort,
      wrapError,
      parsePullArgs,
      validateBackpressure,
    } = require("internal/streams/iter/utils");

    const { RingBuffer } = require("internal/streams/iter/ringbuffer");

    const { validateAbortSignal, validateInteger, validateObject } = require("internal/validators");

    // =============================================================================
    // Async Share Implementation
    // =============================================================================

    class ShareImpl {
      #source;
      #options;
      #buffer = new RingBuffer();
      #bufferStart = 0;
      #consumers = new Set();
      #sourceIterator = null;
      #sourceExhausted = false;
      #sourceError = null;
      #cancelled = false;
      #pulling = false;
      #pullWaiters = [];
      #cachedMinCursor = 0;
      #cachedMinCursorConsumers = 0;

      constructor(source, options) {
        this.#source = source;
        this.#options = options;
      }

      get consumerCount() {
        return this.#consumers.size;
      }

      get bufferSize() {
        return this.#buffer.length;
      }

      pull(...args) {
        const { transforms, options } = parsePullArgs(args);
        const rawConsumer = this.#createRawConsumer();

        if (transforms.length > 0) {
          if (options) {
            return pullWithTransforms(rawConsumer, ...transforms, options);
          }
          return pullWithTransforms(rawConsumer, ...transforms);
        }
        return rawConsumer;
      }

      #createRawConsumer() {
        const state = {
          __proto__: null,
          cursor: this.#bufferStart,
          resolve: null,
          reject: null,
          detached: false,
          pendingNext: Promise.resolve(),
        };

        this.#consumers.add(state);
        if (this.#consumers.size === 1) {
          this.#cachedMinCursor = state.cursor;
          this.#cachedMinCursorConsumers = 1;
        } else if (state.cursor === this.#cachedMinCursor) {
          this.#cachedMinCursorConsumers++;
        } else {
          this.#recomputeMinCursor();
        }
        const self = this;

        return {
          __proto__: null,
          [Symbol.asyncIterator]() {
            const getNext = async () => {
              if (self.#sourceError) {
                state.detached = true;
                self.#consumers.delete(state);
                throw self.#sourceError;
              }

              // Loop until we get data, source is exhausted, or
              // consumer is detached. Multiple consumers may be woken
              // after a single pull - those that find no data at their
              // cursor must re-pull rather than terminating prematurely.
              for (;;) {
                if (state.detached) {
                  if (self.#sourceError) throw self.#sourceError;
                  return { __proto__: null, done: true, value: undefined };
                }

                if (self.#cancelled) {
                  state.detached = true;
                  self.#deleteConsumer(state);
                  return { __proto__: null, done: true, value: undefined };
                }

                // Check if data is available in buffer
                const bufferIndex = state.cursor - self.#bufferStart;
                if (bufferIndex < self.#buffer.length) {
                  const chunk = self.#buffer.get(bufferIndex);
                  const cursor = state.cursor;
                  state.cursor++;
                  if (cursor === self.#cachedMinCursor && --self.#cachedMinCursorConsumers === 0) {
                    self.#tryTrimBuffer();
                  }
                  return { __proto__: null, done: false, value: chunk };
                }

                if (self.#sourceExhausted) {
                  state.detached = true;
                  self.#deleteConsumer(state);
                  if (self.#sourceError) throw self.#sourceError;
                  return { __proto__: null, done: true, value: undefined };
                }

                // Need to pull from source - check buffer limit
                const canPull = await self.#waitForBufferSpace();
                if (!canPull) {
                  state.detached = true;
                  self.#deleteConsumer(state);
                  if (self.#sourceError) throw self.#sourceError;
                  return { __proto__: null, done: true, value: undefined };
                }

                await self.#pullFromSource();
              }
            };

            return {
              __proto__: null,
              next() {
                const next = state.pendingNext.then(getNext, getNext);
                state.pendingNext = next.then(undefined, () => {});
                return next;
              },

              async return() {
                state.detached = true;
                state.resolve = null;
                state.reject = null;
                if (self.#deleteConsumer(state)) {
                  self.#tryTrimBuffer();
                }
                return { __proto__: null, done: true, value: undefined };
              },

              async throw() {
                state.detached = true;
                state.resolve = null;
                state.reject = null;
                if (self.#deleteConsumer(state)) {
                  self.#tryTrimBuffer();
                }
                return { __proto__: null, done: true, value: undefined };
              },
            };
          },
        };
      }

      cancel(reason) {
        if (this.#cancelled) return;
        this.#cancelled = true;

        if (reason !== undefined) {
          this.#sourceError = reason;
        }

        if (this.#sourceIterator?.return) {
          this.#sourceIterator.return().then(undefined, () => {});
        }

        for (const consumer of this.#consumers) {
          if (consumer.resolve) {
            if (reason !== undefined) {
              consumer.reject?.(reason);
            } else {
              consumer.resolve({ __proto__: null, done: true, value: undefined });
            }
            consumer.resolve = null;
            consumer.reject = null;
          }
          consumer.detached = true;
        }
        this.#consumers.clear();

        for (let i = 0; i < this.#pullWaiters.length; i++) {
          this.#pullWaiters[i]();
        }
        this.#pullWaiters = [];
      }

      [Symbol.dispose]() {
        this.cancel();
      }

      // Internal methods

      async #waitForBufferSpace() {
        while (this.#buffer.length >= this.#options.highWaterMark) {
          if (this.#cancelled || this.#sourceError || this.#sourceExhausted) {
            return !this.#cancelled;
          }

          switch (this.#options.backpressure) {
            case "strict":
              throw $ERR_OUT_OF_RANGE("buffer size", `<= ${this.#options.highWaterMark}`, this.#buffer.length);
            case "block": {
              const { promise, resolve } = Promise.withResolvers();
              this.#pullWaiters.push(resolve);
              await promise;
              break;
            }
            case "drop-oldest":
              this.#buffer.shift();
              this.#bufferStart++;
              for (const consumer of this.#consumers) {
                if (consumer.cursor < this.#bufferStart) {
                  this.#deleteConsumerFromMin(consumer);
                  consumer.cursor = this.#bufferStart;
                }
              }
              this.#recomputeMinCursor();
              return true;
            case "drop-newest":
              return true;
          }
        }
        return true;
      }

      #pullFromSource() {
        if (this.#sourceExhausted || this.#cancelled) {
          return Promise.resolve();
        }

        if (this.#pulling) {
          const { promise, resolve } = Promise.withResolvers();
          this.#pullWaiters.push(resolve);
          return promise;
        }

        this.#pulling = true;

        return (async () => {
          try {
            if (!this.#sourceIterator) {
              if (isAsyncIterable(this.#source)) {
                this.#sourceIterator = this.#source[Symbol.asyncIterator]();
              } else if (isSyncIterable(this.#source)) {
                const syncIterator = this.#source[Symbol.iterator]();
                this.#sourceIterator = {
                  __proto__: null,
                  async next() {
                    return syncIterator.next();
                  },
                  async return() {
                    return syncIterator.return?.() ?? { __proto__: null, done: true, value: undefined };
                  },
                };
              } else {
                throw $ERR_INVALID_ARG_TYPE("source", ["AsyncIterable", "Iterable"], this.#source);
              }
            }

            const result = await this.#sourceIterator.next();

            if (result.done) {
              this.#sourceExhausted = true;
            } else {
              this.#buffer.push(result.value);
            }
          } catch (error) {
            this.#sourceError = wrapError(error);
            this.#sourceExhausted = true;
          } finally {
            this.#pulling = false;
            for (let i = 0; i < this.#pullWaiters.length; i++) {
              this.#pullWaiters[i]();
            }
            this.#pullWaiters = [];
          }
        })();
      }

      #tryTrimBuffer() {
        if (this.#cachedMinCursorConsumers === 0) {
          this.#recomputeMinCursor();
        }
        const trimCount = this.#cachedMinCursor - this.#bufferStart;
        if (trimCount > 0) {
          this.#buffer.trimFront(trimCount);
          this.#bufferStart = this.#cachedMinCursor;
          for (let i = 0; i < this.#pullWaiters.length; i++) {
            this.#pullWaiters[i]();
          }
          this.#pullWaiters = [];
        }
      }

      #recomputeMinCursor() {
        const { minCursor, minCursorConsumers } = getMinCursor(this.#consumers, this.#bufferStart + this.#buffer.length);
        this.#cachedMinCursor = minCursor;
        this.#cachedMinCursorConsumers = minCursorConsumers;
      }

      #deleteConsumerFromMin(consumer) {
        if (consumer.cursor === this.#cachedMinCursor) {
          this.#cachedMinCursorConsumers--;
          return this.#cachedMinCursorConsumers === 0;
        }
        return false;
      }

      #deleteConsumer(consumer) {
        if (this.#consumers.delete(consumer)) {
          return this.#deleteConsumerFromMin(consumer);
        }
        return false;
      }
    }

    // =============================================================================
    // Sync Share Implementation
    // =============================================================================

    class SyncShareImpl {
      #source;
      #options;
      #buffer = new RingBuffer();
      #bufferStart = 0;
      #consumers = new Set();
      #sourceIterator = null;
      #sourceExhausted = false;
      #sourceError = null;
      #cancelled = false;
      #cachedMinCursor = 0;
      #cachedMinCursorConsumers = 0;

      constructor(source, options) {
        this.#source = source;
        this.#options = options;
      }

      get consumerCount() {
        return this.#consumers.size;
      }

      get bufferSize() {
        return this.#buffer.length;
      }

      pull(...transforms) {
        const rawConsumer = this.#createRawConsumer();

        if (transforms.length > 0) {
          return pullSyncWithTransforms(rawConsumer, ...transforms);
        }
        return rawConsumer;
      }

      #createRawConsumer() {
        const state = {
          __proto__: null,
          cursor: this.#bufferStart,
          detached: false,
        };

        this.#consumers.add(state);
        if (this.#consumers.size === 1) {
          this.#cachedMinCursor = state.cursor;
          this.#cachedMinCursorConsumers = 1;
        } else if (state.cursor === this.#cachedMinCursor) {
          this.#cachedMinCursorConsumers++;
        } else {
          this.#recomputeMinCursor();
        }
        const self = this;

        return {
          __proto__: null,
          [Symbol.iterator]() {
            return {
              __proto__: null,
              next() {
                if (state.detached) {
                  return { __proto__: null, done: true, value: undefined };
                }
                if (self.#sourceError) {
                  state.detached = true;
                  self.#deleteConsumer(state);
                  throw self.#sourceError;
                }
                if (self.#cancelled) {
                  state.detached = true;
                  self.#deleteConsumer(state);
                  return { __proto__: null, done: true, value: undefined };
                }

                const bufferIndex = state.cursor - self.#bufferStart;
                if (bufferIndex < self.#buffer.length) {
                  const chunk = self.#buffer.get(bufferIndex);
                  const cursor = state.cursor;
                  state.cursor++;
                  if (cursor === self.#cachedMinCursor && --self.#cachedMinCursorConsumers === 0) {
                    self.#tryTrimBuffer();
                  }
                  return { __proto__: null, done: false, value: chunk };
                }

                if (self.#sourceExhausted) {
                  state.detached = true;
                  self.#deleteConsumer(state);
                  return { __proto__: null, done: true, value: undefined };
                }

                // Check buffer limit
                if (self.#buffer.length >= self.#options.highWaterMark) {
                  switch (self.#options.backpressure) {
                    case "strict":
                      throw $ERR_OUT_OF_RANGE("buffer size", `<= ${self.#options.highWaterMark}`, self.#buffer.length);
                    case "block":
                      throw $ERR_OUT_OF_RANGE(
                        "buffer size",
                        `<= ${self.#options.highWaterMark} ` + "(blocking not available in sync context)",
                        self.#buffer.length,
                      );
                    case "drop-oldest":
                      self.#buffer.shift();
                      self.#bufferStart++;
                      for (const consumer of self.#consumers) {
                        if (consumer.cursor < self.#bufferStart) {
                          self.#deleteConsumerFromMin(consumer);
                          consumer.cursor = self.#bufferStart;
                        }
                      }
                      self.#recomputeMinCursor();
                      break;
                    case "drop-newest":
                      state.detached = true;
                      self.#deleteConsumer(state);
                      return { __proto__: null, done: true, value: undefined };
                  }
                }

                self.#pullFromSource();

                if (self.#sourceError) {
                  state.detached = true;
                  self.#deleteConsumer(state);
                  throw self.#sourceError;
                }

                const newBufferIndex = state.cursor - self.#bufferStart;
                if (newBufferIndex < self.#buffer.length) {
                  const chunk = self.#buffer.get(newBufferIndex);
                  const cursor = state.cursor;
                  state.cursor++;
                  if (cursor === self.#cachedMinCursor && --self.#cachedMinCursorConsumers === 0) {
                    self.#tryTrimBuffer();
                  }
                  return { __proto__: null, done: false, value: chunk };
                }

                if (self.#sourceExhausted) {
                  state.detached = true;
                  self.#deleteConsumer(state);
                  return { __proto__: null, done: true, value: undefined };
                }

                return { __proto__: null, done: true, value: undefined };
              },

              return() {
                state.detached = true;
                if (self.#deleteConsumer(state)) {
                  self.#tryTrimBuffer();
                }
                return { __proto__: null, done: true, value: undefined };
              },

              throw() {
                state.detached = true;
                if (self.#deleteConsumer(state)) {
                  self.#tryTrimBuffer();
                }
                return { __proto__: null, done: true, value: undefined };
              },
            };
          },
        };
      }

      cancel(reason) {
        if (this.#cancelled) return;
        this.#cancelled = true;

        if (reason !== undefined) {
          this.#sourceError = reason;
        }

        if (this.#sourceIterator?.return) {
          this.#sourceIterator.return();
        }

        for (const consumer of this.#consumers) {
          consumer.detached = true;
        }
        this.#consumers.clear();
      }

      [Symbol.dispose]() {
        this.cancel();
      }

      #pullFromSource() {
        if (this.#sourceExhausted || this.#cancelled) return;

        try {
          this.#sourceIterator ||= this.#source[Symbol.iterator]();

          const result = this.#sourceIterator.next();

          if (result.done) {
            this.#sourceExhausted = true;
          } else {
            this.#buffer.push(result.value);
          }
        } catch (error) {
          this.#sourceError = wrapError(error);
          this.#sourceExhausted = true;
        }
      }

      #tryTrimBuffer() {
        if (this.#cachedMinCursorConsumers === 0) {
          this.#recomputeMinCursor();
        }
        const trimCount = this.#cachedMinCursor - this.#bufferStart;
        if (trimCount > 0) {
          this.#buffer.trimFront(trimCount);
          this.#bufferStart = this.#cachedMinCursor;
        }
      }

      #recomputeMinCursor() {
        const { minCursor, minCursorConsumers } = getMinCursor(this.#consumers, this.#bufferStart + this.#buffer.length);
        this.#cachedMinCursor = minCursor;
        this.#cachedMinCursorConsumers = minCursorConsumers;
      }

      #deleteConsumerFromMin(consumer) {
        if (consumer.cursor === this.#cachedMinCursor) {
          this.#cachedMinCursorConsumers--;
          return this.#cachedMinCursorConsumers === 0;
        }
        return false;
      }

      #deleteConsumer(consumer) {
        if (this.#consumers.delete(consumer)) {
          return this.#deleteConsumerFromMin(consumer);
        }
        return false;
      }
    }

    function onShareCancel(shareImpl, signal) {
      onSignalAbort(signal, () => shareImpl.cancel(signal.reason));
    }

    // =============================================================================
    // Public API
    // =============================================================================

    function share(source, options = { __proto__: null }) {
      // Normalize source via from() - accepts strings, ArrayBuffers, protocols, etc.
      const normalized = from(source);
      validateObject(options, "options");
      const { highWaterMark = kMultiConsumerDefaultHWM, backpressure = "strict", signal } = options;
      validateInteger(highWaterMark, "options.highWaterMark");
      validateBackpressure(backpressure);
      if (signal !== undefined) {
        validateAbortSignal(signal, "options.signal");
      }

      const opts = {
        __proto__: null,
        highWaterMark: clampHWM(highWaterMark),
        backpressure,
        signal,
      };

      const shareImpl = new ShareImpl(normalized, opts);

      if (signal) {
        onShareCancel(shareImpl, signal);
      }

      return shareImpl;
    }

    function shareSync(source, options = { __proto__: null }) {
      // Normalize source via fromSync() - accepts strings, ArrayBuffers, protocols, etc.
      const normalized = fromSync(source);
      validateObject(options, "options");
      const { highWaterMark = kMultiConsumerDefaultHWM, backpressure = "strict" } = options;
      validateInteger(highWaterMark, "options.highWaterMark");
      validateBackpressure(backpressure);

      const opts = {
        __proto__: null,
        highWaterMark: clampHWM(highWaterMark),
        backpressure,
      };

      return new SyncShareImpl(normalized, opts);
    }

    function isShareable(value) {
      return hasProtocol(value, shareProtocol);
    }

    function isSyncShareable(value) {
      return hasProtocol(value, shareSyncProtocol);
    }

    const Share = {
      __proto__: null,
      from(input, options) {
        if (isShareable(input)) {
          const result = input[shareProtocol](options);
          if (result === null || typeof result !== "object") {
            throw $ERR_INVALID_RETURN_VALUE("an object", "[Symbol.for('Stream.shareProtocol')]", result);
          }
          return result;
        }
        if (isAsyncIterable(input) || isSyncIterable(input)) {
          return share(input, options);
        }
        throw $ERR_INVALID_ARG_TYPE("input", ["Shareable", "AsyncIterable", "Iterable"], input);
      },
    };

    const SyncShare = {
      __proto__: null,
      fromSync(input, options) {
        if (isSyncShareable(input)) {
          const result = input[shareSyncProtocol](options);
          if (result === null || typeof result !== "object") {
            throw $ERR_INVALID_RETURN_VALUE("an object", "[Symbol.for('Stream.shareSyncProtocol')]", result);
          }
          return result;
        }
        if (isSyncIterable(input)) {
          return shareSync(input, options);
        }
        throw $ERR_INVALID_ARG_TYPE("input", ["SyncShareable", "Iterable"], input);
      },
    };

    module.exports = {
      Share,
      SyncShare,
      share,
      shareSync,
    };
  });
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
