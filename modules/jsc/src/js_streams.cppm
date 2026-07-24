// modules/jsc/src/js_streams.cppm — module mbun.jsc.js_streams
//
// WHATWG Streams (ReadableStream/WritableStream/TransformStream + strategies,
// readers/writers, tee/pipeTo/pipeThrough, byte streams + BYOB) implemented in
// runtime JavaScript as a faithful transcription of the spec's reference
// algorithms, plus Bun's extensions: `type: "direct"` sources, reader.readMany,
// async iteration, and the readableStreamTo* consumer family (exposed via
// globalThis.__mbunStreams for js_builtins to wire onto Bun.* / Response /
// Request / Blob). Evaluated by mbun.jsc.runtime BEFORE kNodeBuiltinsJS so the
// builtins' `typeof G.ReadableStream === "undefined"` guards pick these up.
export module mbun.jsc.js_streams;

import std;
import mbun.jsc.js_streams_inspect;  // kStreamsJS_part3 (custom-inspect methods)

namespace mbun::jsc::builtins {

// Split into two raw strings (GCC constexpr-strlen limit), joined at init.
constexpr std::string_view kStreamsJS_part1 = R"JS(
(function () {
  "use strict";
  const G = globalThis;

  // ---- helpers ----
  const noop = () => {};
  const deferred = () => { let resolve, reject; const promise = new Promise((res, rej) => { resolve = res; reject = rej; }); return { promise, resolve, reject }; };
  const markHandled = (p) => { if (p && typeof p.then === "function") p.then(noop, noop); };
  const promiseCall = (fn, thisArg, args) => { try { return Promise.resolve(fn.apply(thisArg, args)); } catch (e) { return Promise.reject(e); } };
  const invalidState = (m) => { const e = new TypeError("Invalid state: " + m); e.code = "ERR_INVALID_STATE"; return e; };
  let _te = null;
  const utf8Encode = (s) => { if (!_te) _te = new G.TextEncoder(); return _te.encode(s); };
  const isView = ArrayBuffer.isView;
  const toU8 = (chunk) => {
    if (chunk instanceof Uint8Array) return chunk;
    if (isView(chunk)) return new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    if (chunk instanceof ArrayBuffer) return new Uint8Array(chunk);
    return null;
  };
  const isDetachedBuffer = (b) => b.detached === true;
  const transferArrayBuffer = (b) =>
    typeof b.transferToFixedLength === "function" ? b.transferToFixedLength()
    : typeof b.transfer === "function" ? b.transfer() : b.slice(0);

  // ---- queue-with-sizes (spec: DequeueValue / EnqueueValueWithSize / ResetQueue) ----
  const resetQueue = (c) => { c._queue = []; c._queueTotalSize = 0; };
  const dequeueValue = (c) => {
    const entry = c._queue.shift();
    c._queueTotalSize -= entry.size;
    if (c._queueTotalSize < 0) c._queueTotalSize = 0;
    return entry.value;
  };
  const enqueueValueWithSize = (c, value, size) => {
    size = Number(size);
    if (!Number.isFinite(size) || size < 0) throw new RangeError("Chunk size is not a finite, non-negative number");
    c._queue.push({ value, size });
    c._queueTotalSize += size;
  };

  // ---- strategies (spec: ExtractHighWaterMark / ExtractSizeAlgorithm) ----
  const extractHWM = (strategy, defaultHWM) => {
    if (strategy.highWaterMark === undefined) return defaultHWM;
    const hwm = Number(strategy.highWaterMark);
    if (Number.isNaN(hwm) || hwm < 0) throw new RangeError("Invalid highWaterMark");
    return hwm;
  };
  const extractSizeAlgorithm = (strategy) => {
    const size = strategy.size;
    if (size === undefined) return () => 1;
    if (typeof size !== "function") throw new TypeError("size property of a queuing strategy must be a function");
    return (chunk) => size(chunk);
  };

  class ByteLengthQueuingStrategy {
    constructor(init) { this.highWaterMark = init.highWaterMark; }
    size(chunk) { return chunk.byteLength; }
  }
  class CountQueuingStrategy {
    constructor(init) { this.highWaterMark = init.highWaterMark; }
    size() { return 1; }
  }

  // =====================================================================
  // ReadableStream
  // =====================================================================

  const isReadableStream = (x) => x != null && typeof x === "object" && "_readableStreamController" in x;

  const readableStreamHasDefaultReader = (stream) => stream._reader !== undefined && stream._reader._readRequests !== undefined;
  const readableStreamHasBYOBReader = (stream) => stream._reader !== undefined && stream._reader._readIntoRequests !== undefined;
  const numReadRequests = (stream) => stream._reader._readRequests.length;
  const numReadIntoRequests = (stream) => stream._reader._readIntoRequests.length;

  function readableStreamFulfillReadRequest(stream, chunk, done) {
    const readRequest = stream._reader._readRequests.shift();
    if (done) readRequest.closeSteps();
    else readRequest.chunkSteps(chunk);
  }
  function readableStreamFulfillReadIntoRequest(stream, chunk, done) {
    const readIntoRequest = stream._reader._readIntoRequests.shift();
    if (done) readIntoRequest.closeSteps(chunk);
    else readIntoRequest.chunkSteps(chunk);
  }

  // node's stream[kIsClosedPromise] (internal/webstreams/{readable,writable}stream.js):
  // a per-STREAM settled-on-close promise that does NOT lock the stream. eosWeb
  // (`stream.finished(webStream)`) waits on it; without it the node layer had to
  // fall back to `stream.getReader().closed`, which locks the stream and made
  // every `finished(rs)` + `for await (rs)` pair throw "ReadableStream is locked".
  // Created on demand — a stream nobody asks about pays nothing — and settled
  // straight away when the stream is already closed/errored by then.
  function streamClosedPromise(stream) {
    let d = stream._isClosedDeferred;
    if (d === undefined) {
      d = stream._isClosedDeferred = deferred();
      markHandled(d.promise);
      if (stream._state === "closed") d.resolve(undefined);
      else if (stream._state === "errored") d.reject(stream._storedError);
    }
    return d.promise;
  }
  const resolveClosed = (stream) => { if (stream._isClosedDeferred) stream._isClosedDeferred.resolve(undefined); };
  const rejectClosed = (stream, error) => { if (stream._isClosedDeferred) stream._isClosedDeferred.reject(error); };

  function readableStreamClose(stream) {
    if (stream._state !== "readable") return;
    stream._state = "closed";
    resolveClosed(stream);
    const reader = stream._reader;
    if (reader === undefined) return;
    reader._closedDeferred.resolve(undefined);
    if (reader._readRequests !== undefined) {
      const requests = reader._readRequests;
      reader._readRequests = [];
      for (const r of requests) r.closeSteps();
    }
  }
  function readableStreamError(stream, e) {
    if (stream._state !== "readable") return;
    stream._state = "errored";
    stream._storedError = e;
    rejectClosed(stream, e);
    const reader = stream._reader;
    if (reader === undefined) return;
    reader._closedDeferred.reject(e);
    markHandled(reader._closedDeferred.promise);
    if (reader._readRequests !== undefined) {
      const requests = reader._readRequests;
      reader._readRequests = [];
      for (const r of requests) r.errorSteps(e);
    } else {
      const requests = reader._readIntoRequests;
      reader._readIntoRequests = [];
      for (const r of requests) r.errorSteps(e);
    }
  }
  function readableStreamCancel(stream, reason) {
    stream._disturbed = true;
    if (stream._state === "closed") return Promise.resolve(undefined);
    if (stream._state === "errored") return Promise.reject(stream._storedError);
    readableStreamClose(stream);
    const reader = stream._reader;
    if (reader !== undefined && reader._readIntoRequests !== undefined) {
      const requests = reader._readIntoRequests;
      reader._readIntoRequests = [];
      for (const r of requests) r.closeSteps(undefined);
    }
    return stream._readableStreamController._cancelSteps(reason).then(noop);
  }

  // ---- reader generic ops ----
  function readerGenericInitialize(reader, stream) {
    reader._stream = stream;
    stream._reader = reader;
    reader._closedDeferred = deferred();
    if (stream._state === "closed") reader._closedDeferred.resolve(undefined);
    else if (stream._state === "errored") {
      reader._closedDeferred.reject(stream._storedError);
      markHandled(reader._closedDeferred.promise);
    }
  }
  function readerGenericRelease(reader) {
    const stream = reader._stream;
    if (stream._state === "readable") {
      reader._closedDeferred.reject(invalidState("Reader released"));
    } else {
      reader._closedDeferred = deferred();
      reader._closedDeferred.reject(invalidState("Reader released"));
    }
    markHandled(reader._closedDeferred.promise);
    stream._readableStreamController._releaseSteps();
    stream._reader = undefined;
    reader._stream = undefined;
  }

  function readableStreamDefaultReaderRead(reader, readRequest) {
    const stream = reader._stream;
    stream._disturbed = true;
    if (stream._state === "closed") readRequest.closeSteps();
    else if (stream._state === "errored") readRequest.errorSteps(stream._storedError);
    else stream._readableStreamController._pullSteps(readRequest);
  }
  function readableStreamDefaultReaderErrorReadRequests(reader, e) {
    const requests = reader._readRequests;
    reader._readRequests = [];
    for (const r of requests) r.errorSteps(e);
  }

  class ReadableStreamDefaultReader {
    constructor(stream) {
      if (!isReadableStream(stream)) throw new TypeError("ReadableStreamDefaultReader needs a ReadableStream");
      if (stream.locked) throw new TypeError("ReadableStream is locked");
      readerGenericInitialize(this, stream);
      this._readRequests = [];
    }
    get closed() { return this._closedDeferred ? this._closedDeferred.promise : Promise.reject(new TypeError("closed on invalid reader")); }
    cancel(reason) {
      if (this._stream === undefined) return Promise.reject(invalidState("Reader released"));
      return readableStreamCancel(this._stream, reason);
    }
    read() {
      if (this._stream === undefined) return Promise.reject(invalidState("The reader is not attached to a stream"));
      const d = deferred();
      readableStreamDefaultReaderRead(this, {
        chunkSteps: (chunk) => d.resolve({ value: chunk, done: false }),
        closeSteps: () => d.resolve({ value: undefined, done: true }),
        errorSteps: (e) => d.reject(e),
      });
      return d.promise;
    }
    // Bun extension: drain everything currently readable as one wake.
    // For a pull-driven source the follow-up pull runs BEFORE the drain, so a
    // chunk the pull pipeline enqueues while this wake settles is batched in.
    readMany() {
      if (this._stream === undefined) return Promise.reject(invalidState("The reader is not attached to a stream"));
      const stream = this._stream;
      stream._disturbed = true;
      if (stream._state === "closed") return Promise.resolve({ value: [], size: 0, done: true });
      if (stream._state === "errored") return Promise.reject(stream._storedError);
      const controller = stream._readableStreamController;
      const drain = () => {
        const values = controller._drainQueueValues();
        let size = 0;
        for (const s of values.sizes) size += s;
        if (controller._closeRequested && controller._queue.length === 0) controller._finishClose();
        else controller._callPullIfNeeded();
        return { value: values.values, size, done: false };
      };
      if (controller._queue.length > 0) {
        // Drain (reset the queue) BEFORE the follow-up pull, so a chunk the pull
        // reentrantly enqueues against the emptied queue survives (bun semantics).
        return Promise.resolve(drain());
      }
      const d = deferred();
      controller._pullSteps({
        chunkSteps: (chunk) => {
          // Defer the batching drain one tick so the controller's pipelined
          // follow-up pull (scheduled off this same fulfillment) lands first.
          Promise.resolve().then(() => {
            if (stream._state === "errored") { d.reject(stream._storedError); return; }
            const rest = controller._queue.length > 0 ? drain() : { value: [], size: 0 };
            d.resolve({ value: [chunk].concat(rest.value), size: 1 + rest.size, done: false });
          });
        },
        closeSteps: () => d.resolve({ value: [], size: 0, done: true }),
        errorSteps: (e) => d.reject(e),
      });
      return d.promise;
    }
    releaseLock() {
      if (this._stream === undefined) return;
      readerGenericRelease(this);
      readableStreamDefaultReaderErrorReadRequests(this, invalidState("Releasing reader"));
    }
  }

  class ReadableStreamBYOBReader {
    constructor(stream) {
      if (!isReadableStream(stream)) throw new TypeError("ReadableStreamBYOBReader needs a ReadableStream");
      if (stream.locked) throw new TypeError("ReadableStream is locked");
      if (!(stream._readableStreamController instanceof ReadableByteStreamController))
        throw new TypeError("Cannot construct a ReadableStreamBYOBReader for a stream not constructed with a byte source");
      readerGenericInitialize(this, stream);
      this._readIntoRequests = [];
    }
    get closed() { return this._closedDeferred.promise; }
    cancel(reason) {
      if (this._stream === undefined) return Promise.reject(invalidState("Reader released"));
      return readableStreamCancel(this._stream, reason);
    }
    read(view, options) {
      if (this._stream === undefined) return Promise.reject(invalidState("The reader is not attached to a stream"));
      if (!isView(view) || view.byteLength === 0 || view.buffer.byteLength === 0)
        return Promise.reject(new TypeError("read() requires a non-empty ArrayBufferView"));
      let min = 1;
      if (options && options.min !== undefined) {
        min = Number(options.min);
        if (!(min >= 1)) return Promise.reject(new TypeError("options.min must be >= 1"));
        const cap = view instanceof DataView ? view.byteLength : view.length;
        if (min > cap) return Promise.reject(new RangeError("options.min is too large"));
      }
      const d = deferred();
      const readIntoRequest = {
        chunkSteps: (chunk) => d.resolve({ value: chunk, done: false }),
        closeSteps: (chunk) => d.resolve({ value: chunk, done: true }),
        errorSteps: (e) => d.reject(e),
      };
      const stream = this._stream;
      stream._disturbed = true;
      if (stream._state === "errored") readIntoRequest.errorSteps(stream._storedError);
      else byteControllerPullInto(stream._readableStreamController, view, min, readIntoRequest);
      return d.promise;
    }
    releaseLock() {
      if (this._stream === undefined) return;
      readerGenericRelease(this);
      const requests = this._readIntoRequests;
      this._readIntoRequests = [];
      for (const r of requests) r.errorSteps(invalidState("Releasing reader"));
    }
  }

  // ---- ReadableStreamDefaultController ----
  class ReadableStreamDefaultController {
    constructor() { throw new TypeError("Illegal constructor"); }
    get desiredSize() { return defaultControllerGetDesiredSize(this); }
    close() {
      // bun JSReadableStreamDefaultController.cpp:484 — ERR_INVALID_STATE wording.
      if (!defaultControllerCanCloseOrEnqueue(this)) { const e = new TypeError("Invalid state: Controller is already closed"); e.code = "ERR_INVALID_STATE"; throw e; }
      defaultControllerClose(this);
    }
    enqueue(chunk) {
      if (!defaultControllerCanCloseOrEnqueue(this)) { const e = new TypeError("Invalid state: Controller is already closed"); e.code = "ERR_INVALID_STATE"; throw e; }
      defaultControllerEnqueue(this, chunk);
    }
    error(e) { defaultControllerError(this, e); }
    _pullSteps(readRequest) {
      const stream = this._stream;
      if (this._queue.length > 0) {
        const chunk = dequeueValue(this);
        if (this._closeRequested && this._queue.length === 0) this._finishClose();
        else defaultControllerCallPullIfNeeded(this);
        readRequest.chunkSteps(chunk);
      } else {
        stream._reader._readRequests.push(readRequest);
        defaultControllerCallPullIfNeeded(this);
      }
    }
    _cancelSteps(reason) {
      resetQueue(this);
      const result = this._cancelAlgorithm(reason);
      defaultControllerClearAlgorithms(this);
      return result;
    }
    _releaseSteps() {}
    // internal helpers shared with readMany
    _drainQueueValues() {
      const values = this._queue.map((e) => e.value);
      const sizes = this._queue.map((e) => e.size);
      resetQueue(this);
      return { values, sizes };
    }
    _finishClose() { defaultControllerClearAlgorithms(this); readableStreamClose(this._stream); }
    _callPullIfNeeded() { defaultControllerCallPullIfNeeded(this); }
  }
  function defaultControllerGetDesiredSize(c) {
    const state = c._stream._state;
    if (state === "errored") return null;
    if (state === "closed") return 0;
    return c._strategyHWM - c._queueTotalSize;
  }
  function defaultControllerCanCloseOrEnqueue(c) {
    return !c._closeRequested && c._stream._state === "readable";
  }
  function defaultControllerShouldCallPull(c) {
    const stream = c._stream;
    if (!defaultControllerCanCloseOrEnqueue(c)) return false;
    if (!c._started) return false;
    if (readableStreamHasDefaultReader(stream) && numReadRequests(stream) > 0) return true;
    return defaultControllerGetDesiredSize(c) > 0;
  }
  function defaultControllerCallPullIfNeeded(c) {
    if (!defaultControllerShouldCallPull(c)) return;
    if (c._pulling) { c._pullAgain = true; return; }
    c._pulling = true;
    c._pullAlgorithm().then(
      () => {
        c._pulling = false;
        if (c._pullAgain) { c._pullAgain = false; defaultControllerCallPullIfNeeded(c); }
      },
      (e) => defaultControllerError(c, e),
    );
  }
  function defaultControllerClearAlgorithms(c) {
    c._pullAlgorithm = () => Promise.resolve();
    c._cancelAlgorithm = () => Promise.resolve();
    c._strategySizeAlgorithm = () => 1;
  }
  function defaultControllerClose(c) {
    if (!defaultControllerCanCloseOrEnqueue(c)) return;
    c._closeRequested = true;
    if (c._queue.length === 0) c._finishClose();
  }
  function defaultControllerEnqueue(c, chunk) {
    if (!defaultControllerCanCloseOrEnqueue(c)) return;
    const stream = c._stream;
    if (readableStreamHasDefaultReader(stream) && numReadRequests(stream) > 0) {
      readableStreamFulfillReadRequest(stream, chunk, false);
    } else {
      let size;
      try { size = c._strategySizeAlgorithm(chunk); } catch (e) { defaultControllerError(c, e); throw e; }
      try { enqueueValueWithSize(c, chunk, size); } catch (e) { defaultControllerError(c, e); throw e; }
    }
    defaultControllerCallPullIfNeeded(c);
  }
  function defaultControllerError(c, e) {
    const stream = c._stream;
    if (stream._state !== "readable") return;
    resetQueue(c);
    defaultControllerClearAlgorithms(c);
    readableStreamError(stream, e);
  }
  function setUpDefaultController(stream, source, highWaterMark, sizeAlgorithm) {
    const c = Object.create(ReadableStreamDefaultController.prototype);
    c._stream = stream;
    resetQueue(c);
    c._started = false; c._closeRequested = false; c._pullAgain = false; c._pulling = false;
    c._strategySizeAlgorithm = sizeAlgorithm;
    c._strategyHWM = highWaterMark;
    c._pullAlgorithm = typeof source.pull === "function" ? () => promiseCall(source.pull, source, [c]) : () => Promise.resolve();
    c._cancelAlgorithm = typeof source.cancel === "function" ? (reason) => promiseCall(source.cancel, source, [reason]) : () => Promise.resolve();
    stream._readableStreamController = c;
    const startResult = typeof source.start === "function" ? source.start.call(source, c) : undefined;
    Promise.resolve(startResult).then(
      () => { c._started = true; defaultControllerCallPullIfNeeded(c); },
      (e) => defaultControllerError(c, e),
    );
  }

  // ---- Bun `type: "direct"` controller ----
  // Semantics (bun): controller.{write,flush,end,close,error,desiredSize};
  // pull() is a per-read demand signal, never re-entered while pending, never
  // after end(); buffered writes auto-flush at end of tick; end() defers the
  // stream close until the queued final chunk is drained.
  class ReadableStreamDirectController {
    constructor() { throw new TypeError("Illegal constructor"); }
    get desiredSize() { return this._strategyHWM - this._queueTotalSize - this._sinkSize; }
    write(chunk) {
      if (this._ended || this._stream._state !== "readable") throw invalidState("Controller is already closed");
      const u8 = typeof chunk === "string" ? utf8Encode(chunk) : toU8(chunk);
      if (u8 === null) throw new TypeError("Expected a string, TypedArray, or ArrayBuffer");
      this._sink.push(u8);
      this._sinkSize += u8.byteLength;
      if (!this._autoFlushScheduled) {
        this._autoFlushScheduled = true;
        Promise.resolve().then(() => { this._autoFlushScheduled = false; this._flushNow(); });
      }
      return u8.byteLength;
    }
    flush() { this._flushNow(); return Promise.resolve(); }
    end() { this._closeInternal(); }
    close() { this._closeInternal(); }
    error(e) {
      this._sink = []; this._sinkSize = 0;
      this._ended = true;
      resetQueue(this);
      readableStreamError(this._stream, e);
    }
    _closeInternal() {
      if (this._ended) return;
      this._ended = true;
      this._flushNow();
      this._maybeFinishClose();
    }
    _flushNow() {
      if (this._sinkSize > 0) {
        const chunk = new Uint8Array(this._sinkSize);
        let off = 0;
        for (const part of this._sink) { chunk.set(part, off); off += part.byteLength; }
        this._sink = []; this._sinkSize = 0;
        this._queue.push({ value: chunk, size: chunk.byteLength });
        this._queueTotalSize += chunk.byteLength;
        this._wrote = true;
      }
      this._service();
    }
    _service() {
      const stream = this._stream;
      while (this._queue.length > 0 && readableStreamHasDefaultReader(stream) && numReadRequests(stream) > 0) {
        readableStreamFulfillReadRequest(stream, dequeueValue(this), false);
      }
      this._maybeFinishClose();
    }
    _maybeFinishClose() {
      if (this._ended && this._queue.length === 0 && this._sinkSize === 0 && this._stream._state === "readable" && !this._pulling) {
        readableStreamClose(this._stream);
      }
    }
    _pullSteps(readRequest) {
      const stream = this._stream;
      if (this._queue.length === 0 && this._sinkSize > 0) this._flushNow();
      if (this._queue.length > 0) {
        readRequest.chunkSteps(dequeueValue(this));
        this._maybeFinishCloseAsync();
        return;
      }
      if (this._ended || stream._state !== "readable") { readRequest.closeSteps(); return; }
      stream._reader._readRequests.push(readRequest);
      if (this._pulling) { this._pullAgain = true; return; }
      this._invokePull();
    }
    _maybeFinishCloseAsync() {
      if (this._ended && this._queue.length === 0 && this._sinkSize === 0) {
        if (this._pulling) return;
        readableStreamClose(this._stream);
      }
    }
    _invokePull() {
      if (this._pullFn === null || this._ended || this._stream._state !== "readable") return;
      this._pulling = true;
      this._wrote = false;
      let result;
      try { result = this._pullFn(this); } catch (e) { this._pulling = false; this.error(e); return; }
      Promise.resolve(result).then(
        () => {
          this._pulling = false;
          this._flushNow();
          const again = this._pullAgain;
          this._pullAgain = false;
          if (this._ended || this._stream._state !== "readable") { this._maybeFinishClose(); return; }
          const stream = this._stream;
          const demand = readableStreamHasDefaultReader(stream) ? numReadRequests(stream) : 0;
          if (demand > 0 && (this._wrote || again)) this._invokePull();
        },
        (e) => { this._pulling = false; if (!this._ended && this._stream._state === "readable") this.error(e); else this._maybeFinishClose(); },
      );
    }
    _cancelSteps(reason) {
      this._sink = []; this._sinkSize = 0;
      resetQueue(this);
      this._ended = true;
      const fn = this._cancelFn;
      this._pullFn = null; this._cancelFn = null;
      return fn ? promiseCall(fn, undefined, [reason]).then(noop) : Promise.resolve();
    }
    _releaseSteps() {}
    _drainQueueValues() {
      const values = this._queue.map((e) => e.value);
      const sizes = this._queue.map((e) => e.size);
      resetQueue(this);
      return { values, sizes };
    }
    get _closeRequested() { return this._ended; }
    _finishClose() { this._maybeFinishClose(); }
    _callPullIfNeeded() {
      const stream = this._stream;
      const demand = readableStreamHasDefaultReader(stream) ? numReadRequests(stream) : 0;
      if (demand > 0 && !this._pulling && !this._ended) this._invokePull();
    }
  }
  function setUpDirectController(stream, source, strategy) {
    const c = Object.create(ReadableStreamDirectController.prototype);
    c._stream = stream;
    resetQueue(c);
    c._sink = []; c._sinkSize = 0;
    c._pulling = false; c._pullAgain = false; c._wrote = false; c._ended = false;
    c._autoFlushScheduled = false;
    c._strategyHWM = (strategy && strategy.highWaterMark) || source.highWaterMark || 16384;
    c._pullFn = typeof source.pull === "function" ? source.pull.bind(source) : null;
    c._cancelFn = typeof source.cancel === "function" ? source.cancel.bind(source) : null;
    stream._readableStreamController = c;
    if (typeof source.start === "function") {
      Promise.resolve().then(() => { try { source.start.call(source, c); } catch (e) { c.error(e); } });
    }
  }

  // ---- ReadableByteStreamController (+ BYOB machinery) ----
  class ReadableStreamBYOBRequest {
    constructor() { throw new TypeError("Illegal constructor"); }
    get view() { return this._view; }
    respond(bytesWritten) {
      if (this._controller === undefined) throw new TypeError("This BYOB request has been invalidated");
      if (isDetachedBuffer(this._view.buffer)) throw new TypeError("The BYOB request's buffer has been detached");
      byteControllerRespond(this._controller, Number(bytesWritten));
    }
    respondWithNewView(view) {
      if (this._controller === undefined) throw new TypeError("This BYOB request has been invalidated");
      if (!isView(view)) throw new TypeError("respondWithNewView() requires an ArrayBufferView");
      byteControllerRespondWithNewView(this._controller, view);
    }
  }

  class ReadableByteStreamController {
    constructor() { throw new TypeError("Illegal constructor"); }
    get byobRequest() {
      if (this._byobRequest === null && this._pendingPullIntos.length > 0) {
        const firstDescriptor = this._pendingPullIntos[0];
        const view = new Uint8Array(firstDescriptor.buffer, firstDescriptor.byteOffset + firstDescriptor.bytesFilled, firstDescriptor.byteLength - firstDescriptor.bytesFilled);
        const byobRequest = Object.create(ReadableStreamBYOBRequest.prototype);
        byobRequest._controller = this;
        byobRequest._view = view;
        this._byobRequest = byobRequest;
      }
      return this._byobRequest;
    }
    get desiredSize() {
      const state = this._stream._state;
      if (state === "errored") return null;
      if (state === "closed") return 0;
      return this._strategyHWM - this._queueTotalSize;
    }
    close() {
      if (this._closeRequested || this._stream._state !== "readable") throw new TypeError("The stream is not in a state that permits close");
      byteControllerClose(this);
    }
    enqueue(chunk) {
      if (!isView(chunk)) throw new TypeError("chunk must be an ArrayBufferView");
      if (chunk.byteLength === 0) throw new TypeError("chunk must have non-zero byteLength");
      if (chunk.buffer.byteLength === 0) throw new TypeError("chunk's buffer must have non-zero byteLength");
      if (this._closeRequested || this._stream._state !== "readable") throw new TypeError("The stream is not in a state that permits enqueue");
      byteControllerEnqueue(this, chunk);
    }
    error(e) { byteControllerError(this, e); }
    _pullSteps(readRequest) {
      const stream = this._stream;
      if (this._queueTotalSize > 0) {
        byteControllerFillReadRequestFromQueue(this, readRequest);
        return;
      }
      const autoAllocateChunkSize = this._autoAllocateChunkSize;
      if (autoAllocateChunkSize !== undefined) {
        let buffer;
        try { buffer = new ArrayBuffer(autoAllocateChunkSize); } catch (e) { readRequest.errorSteps(e); return; }
        this._pendingPullIntos.push({
          buffer, bufferByteLength: autoAllocateChunkSize, byteOffset: 0, byteLength: autoAllocateChunkSize,
          bytesFilled: 0, minimumFill: 1, elementSize: 1, viewConstructor: Uint8Array, readerType: "default",
        });
      }
      stream._reader._readRequests.push(readRequest);
      byteControllerCallPullIfNeeded(this);
    }
    _cancelSteps(reason) {
      byteControllerClearPendingPullIntos(this);
      resetQueue(this);
      const result = this._cancelAlgorithm(reason);
      byteControllerClearAlgorithms(this);
      return result;
    }
    _releaseSteps() {
      if (this._pendingPullIntos.length > 0) {
        const firstPendingPullInto = this._pendingPullIntos[0];
        firstPendingPullInto.readerType = "none";
        this._pendingPullIntos = [firstPendingPullInto];
      }
    }
    _drainQueueValues() {
      const values = this._queue.map((e) => new Uint8Array(e.buffer, e.byteOffset, e.byteLength));
      const sizes = this._queue.map((e) => e.byteLength);
      resetQueue(this);
      return { values, sizes };
    }
    _finishClose() {
      if (this._queueTotalSize === 0) { byteControllerClearAlgorithms(this); readableStreamClose(this._stream); }
    }
    _callPullIfNeeded() { byteControllerCallPullIfNeeded(this); }
  }
  function byteControllerShouldCallPull(c) {
    const stream = c._stream;
    if (stream._state !== "readable") return false;
    if (c._closeRequested) return false;
    if (!c._started) return false;
    if (readableStreamHasDefaultReader(stream) && numReadRequests(stream) > 0) return true;
    if (readableStreamHasBYOBReader(stream) && numReadIntoRequests(stream) > 0) return true;
    return c._strategyHWM - c._queueTotalSize > 0;
  }
  function byteControllerCallPullIfNeeded(c) {
    if (!byteControllerShouldCallPull(c)) return;
    if (c._pulling) { c._pullAgain = true; return; }
    c._pulling = true;
    c._pullAlgorithm().then(
      () => {
        c._pulling = false;
        if (c._pullAgain) { c._pullAgain = false; byteControllerCallPullIfNeeded(c); }
      },
      (e) => byteControllerError(c, e),
    );
  }
  function byteControllerClearAlgorithms(c) {
    c._pullAlgorithm = () => Promise.resolve();
    c._cancelAlgorithm = () => Promise.resolve();
  }
  function byteControllerClearPendingPullIntos(c) {
    byteControllerInvalidateBYOBRequest(c);
    c._pendingPullIntos = [];
  }
  function byteControllerInvalidateBYOBRequest(c) {
    if (c._byobRequest === null) return;
    c._byobRequest._controller = undefined;
    c._byobRequest._view = null;
    c._byobRequest = null;
  }
  function byteControllerError(c, e) {
    const stream = c._stream;
    if (stream._state !== "readable") return;
    byteControllerClearPendingPullIntos(c);
    resetQueue(c);
    byteControllerClearAlgorithms(c);
    readableStreamError(stream, e);
  }
  function byteControllerClose(c) {
    const stream = c._stream;
    if (c._closeRequested || stream._state !== "readable") return;
    if (c._queueTotalSize > 0) { c._closeRequested = true; return; }
    if (c._pendingPullIntos.length > 0) {
      const firstPendingPullInto = c._pendingPullIntos[0];
      if (firstPendingPullInto.bytesFilled % firstPendingPullInto.elementSize !== 0) {
        const e = new TypeError("Insufficient bytes to fill elements in the given buffer");
        byteControllerError(c, e);
        throw e;
      }
    }
    byteControllerClearAlgorithms(c);
    readableStreamClose(stream);
  }
  function byteControllerEnqueueChunkToQueue(c, buffer, byteOffset, byteLength) {
    c._queue.push({ buffer, byteOffset, byteLength });
    c._queueTotalSize += byteLength;
  }
  function byteControllerEnqueueClonedChunkToQueue(c, buffer, byteOffset, byteLength) {
    let cloneResult;
    try { cloneResult = buffer.slice(byteOffset, byteOffset + byteLength); }
    catch (e) { byteControllerError(c, e); throw e; }
    byteControllerEnqueueChunkToQueue(c, cloneResult, 0, byteLength);
  }
  function byteControllerEnqueueDetachedPullIntoToQueue(c, pullIntoDescriptor) {
    if (pullIntoDescriptor.bytesFilled > 0)
      byteControllerEnqueueClonedChunkToQueue(c, pullIntoDescriptor.buffer, pullIntoDescriptor.byteOffset, pullIntoDescriptor.bytesFilled);
    byteControllerShiftPendingPullInto(c);
  }
  function byteControllerShiftPendingPullInto(c) {
    return c._pendingPullIntos.shift();
  }
  function byteControllerFillHeadPullIntoDescriptor(c, size, pullIntoDescriptor) {
    byteControllerInvalidateBYOBRequest(c);
    pullIntoDescriptor.bytesFilled += size;
  }
  function byteControllerFillPullIntoDescriptorFromQueue(c, pullIntoDescriptor) {
    const maxBytesToCopy = Math.min(c._queueTotalSize, pullIntoDescriptor.byteLength - pullIntoDescriptor.bytesFilled);
    const maxBytesFilled = pullIntoDescriptor.bytesFilled + maxBytesToCopy;
    let totalBytesToCopyRemaining = maxBytesToCopy;
    let ready = false;
    const remainderBytes = maxBytesFilled % pullIntoDescriptor.elementSize;
    const maxAlignedBytes = maxBytesFilled - remainderBytes;
    if (maxAlignedBytes >= pullIntoDescriptor.minimumFill) {
      totalBytesToCopyRemaining = maxAlignedBytes - pullIntoDescriptor.bytesFilled;
      ready = true;
    }
    const queue = c._queue;
    while (totalBytesToCopyRemaining > 0) {
      const headOfQueue = queue[0];
      const bytesToCopy = Math.min(totalBytesToCopyRemaining, headOfQueue.byteLength);
      const destStart = pullIntoDescriptor.byteOffset + pullIntoDescriptor.bytesFilled;
      new Uint8Array(pullIntoDescriptor.buffer).set(new Uint8Array(headOfQueue.buffer, headOfQueue.byteOffset, bytesToCopy), destStart);
      if (headOfQueue.byteLength === bytesToCopy) queue.shift();
      else { headOfQueue.byteOffset += bytesToCopy; headOfQueue.byteLength -= bytesToCopy; }
      c._queueTotalSize -= bytesToCopy;
      byteControllerFillHeadPullIntoDescriptor(c, bytesToCopy, pullIntoDescriptor);
      totalBytesToCopyRemaining -= bytesToCopy;
    }
    return ready;
  }
  function byteControllerConvertPullIntoDescriptor(pullIntoDescriptor) {
    const bytesFilled = pullIntoDescriptor.bytesFilled;
    const elementSize = pullIntoDescriptor.elementSize;
    const buffer = transferArrayBuffer(pullIntoDescriptor.buffer);
    if (pullIntoDescriptor.viewConstructor === DataView)
      return new DataView(buffer, pullIntoDescriptor.byteOffset, bytesFilled);
    return new pullIntoDescriptor.viewConstructor(buffer, pullIntoDescriptor.byteOffset, bytesFilled / elementSize);
  }
  function byteControllerCommitPullIntoDescriptor(stream, pullIntoDescriptor) {
    let done = false;
    if (stream._state === "closed") done = true;
    const filledView = byteControllerConvertPullIntoDescriptor(pullIntoDescriptor);
    if (pullIntoDescriptor.readerType === "default") readableStreamFulfillReadRequest(stream, filledView, done);
    else readableStreamFulfillReadIntoRequest(stream, filledView, done);
  }
  function byteControllerProcessPullIntoDescriptorsUsingQueue(c) {
    while (c._pendingPullIntos.length > 0) {
      if (c._queueTotalSize === 0) return;
      const pullIntoDescriptor = c._pendingPullIntos[0];
      if (byteControllerFillPullIntoDescriptorFromQueue(c, pullIntoDescriptor)) {
        byteControllerShiftPendingPullInto(c);
        byteControllerCommitPullIntoDescriptor(c._stream, pullIntoDescriptor);
      } else return;
    }
  }
  function byteControllerFillReadRequestFromQueue(c, readRequest) {
    const entry = c._queue.shift();
    c._queueTotalSize -= entry.byteLength;
    if (c._closeRequested && c._queueTotalSize === 0) { byteControllerClearAlgorithms(c); readableStreamClose(c._stream); }
    else byteControllerCallPullIfNeeded(c);
    const view = new Uint8Array(entry.buffer, entry.byteOffset, entry.byteLength);
    readRequest.chunkSteps(view);
  }
  function byteControllerEnqueue(c, chunk) {
    const stream = c._stream;
    if (c._closeRequested || stream._state !== "readable") return;
    const buffer = chunk.buffer;
    const byteOffset = chunk.byteOffset;
    const byteLength = chunk.byteLength;
    if (isDetachedBuffer(buffer)) throw new TypeError("chunk's buffer is detached");
    const transferredBuffer = transferArrayBuffer(buffer);
    if (c._pendingPullIntos.length > 0) {
      const firstPendingPullInto = c._pendingPullIntos[0];
      if (isDetachedBuffer(firstPendingPullInto.buffer)) throw new TypeError("The BYOB request's buffer has been detached");
      byteControllerInvalidateBYOBRequest(c);
      firstPendingPullInto.buffer = transferArrayBuffer(firstPendingPullInto.buffer);
      if (firstPendingPullInto.readerType === "none") byteControllerEnqueueDetachedPullIntoToQueue(c, firstPendingPullInto);
    }
    if (readableStreamHasDefaultReader(stream)) {
      // process read requests: drain queued bytes into waiting read requests
      while (readableStreamHasDefaultReader(stream) && numReadRequests(stream) > 0 && c._queueTotalSize > 0) {
        const readRequest = stream._reader._readRequests.shift();
        const entry = c._queue.shift();
        c._queueTotalSize -= entry.byteLength;
        readRequest.chunkSteps(new Uint8Array(entry.buffer, entry.byteOffset, entry.byteLength));
      }
      if (numReadRequests(stream) === 0) {
        byteControllerEnqueueChunkToQueue(c, transferredBuffer, byteOffset, byteLength);
      } else {
        if (c._pendingPullIntos.length > 0) byteControllerShiftPendingPullInto(c);
        const transferredView = new Uint8Array(transferredBuffer, byteOffset, byteLength);
        readableStreamFulfillReadRequest(stream, transferredView, false);
      }
    } else if (readableStreamHasBYOBReader(stream)) {
      byteControllerEnqueueChunkToQueue(c, transferredBuffer, byteOffset, byteLength);
      byteControllerProcessPullIntoDescriptorsUsingQueue(c);
    } else {
      byteControllerEnqueueChunkToQueue(c, transferredBuffer, byteOffset, byteLength);
    }
    byteControllerCallPullIfNeeded(c);
  }
  function byteControllerRespondInternal(c, bytesWritten) {
    const firstDescriptor = c._pendingPullIntos[0];
    if (isDetachedBuffer(firstDescriptor.buffer)) throw new TypeError("The BYOB request's buffer has been detached");
    byteControllerInvalidateBYOBRequest(c);
    const state = c._stream._state;
    if (state === "closed") {
      if (bytesWritten !== 0) throw new TypeError("bytesWritten must be 0 when calling respond() on a closed stream");
      byteControllerRespondInClosedState(c, firstDescriptor);
    } else {
      if (bytesWritten === 0) throw new TypeError("bytesWritten must be > 0 when calling respond() on a readable stream");
      if (firstDescriptor.bytesFilled + bytesWritten > firstDescriptor.byteLength) throw new RangeError("bytesWritten out of range");
      firstDescriptor.buffer = transferArrayBuffer(firstDescriptor.buffer);
      byteControllerRespondInReadableState(c, bytesWritten, firstDescriptor);
    }
    byteControllerCallPullIfNeeded(c);
  }
  function byteControllerRespondInClosedState(c, firstDescriptor) {
    firstDescriptor.buffer = transferArrayBuffer(firstDescriptor.buffer);
    if (firstDescriptor.readerType === "none") byteControllerShiftPendingPullInto(c);
    const stream = c._stream;
    if (readableStreamHasBYOBReader(stream)) {
      while (numReadIntoRequests(stream) > 0) {
        const pullIntoDescriptor = byteControllerShiftPendingPullInto(c);
        byteControllerCommitPullIntoDescriptor(stream, pullIntoDescriptor);
      }
    }
  }
  function byteControllerRespondInReadableState(c, bytesWritten, pullIntoDescriptor) {
    if (pullIntoDescriptor.readerType === "none") {
      byteControllerFillHeadPullIntoDescriptor(c, bytesWritten, pullIntoDescriptor);
      byteControllerEnqueueDetachedPullIntoToQueue(c, pullIntoDescriptor);
      byteControllerProcessPullIntoDescriptorsUsingQueue(c);
      return;
    }
    byteControllerFillHeadPullIntoDescriptor(c, bytesWritten, pullIntoDescriptor);
    if (pullIntoDescriptor.bytesFilled < pullIntoDescriptor.minimumFill) return;
    byteControllerShiftPendingPullInto(c);
    const remainderSize = pullIntoDescriptor.bytesFilled % pullIntoDescriptor.elementSize;
    if (remainderSize > 0) {
      const end = pullIntoDescriptor.byteOffset + pullIntoDescriptor.bytesFilled;
      byteControllerEnqueueClonedChunkToQueue(c, pullIntoDescriptor.buffer, end - remainderSize, remainderSize);
    }
    pullIntoDescriptor.bytesFilled -= remainderSize;
    byteControllerCommitPullIntoDescriptor(c._stream, pullIntoDescriptor);
    byteControllerProcessPullIntoDescriptorsUsingQueue(c);
  }
  function byteControllerRespond(c, bytesWritten) {
    const firstDescriptor = c._pendingPullIntos[0];
    const state = c._stream._state;
    if (state === "closed") {
      if (bytesWritten !== 0) throw new TypeError("bytesWritten must be 0 when calling respond() on a closed stream");
    } else {
      if (bytesWritten === 0) throw new TypeError("bytesWritten must be > 0 when calling respond() on a readable stream");
      if (firstDescriptor.bytesFilled + bytesWritten > firstDescriptor.byteLength) throw new RangeError("bytesWritten out of range");
    }
    byteControllerRespondInternal(c, bytesWritten);
  }
  function byteControllerRespondWithNewView(c, view) {
    const firstDescriptor = c._pendingPullIntos[0];
    const state = c._stream._state;
    if (state === "closed") { if (view.byteLength !== 0) throw new TypeError("The view's length must be 0 when calling respondWithNewView() on a closed stream"); }
    else if (view.byteLength === 0) throw new TypeError("The view's length must be greater than 0 when calling respondWithNewView() on a readable stream");
    if (firstDescriptor.byteOffset + firstDescriptor.bytesFilled !== view.byteOffset) throw new RangeError("The region specified by view does not match byobRequest");
    if (firstDescriptor.bufferByteLength !== view.buffer.byteLength) throw new RangeError("The buffer of view has different capacity than byobRequest");
    if (firstDescriptor.bytesFilled + view.byteLength > firstDescriptor.byteLength) throw new RangeError("The region specified by view is larger than byobRequest");
    const viewByteLength = view.byteLength;
    firstDescriptor.buffer = transferArrayBuffer(view.buffer);
    byteControllerRespondInternal(c, viewByteLength);
  }
  function byteControllerPullInto(c, view, min, readIntoRequest) {
    const stream = c._stream;
    const ctor = view.constructor;
    const elementSize = view instanceof DataView ? 1 : ctor.BYTES_PER_ELEMENT;
    const minimumFill = min * elementSize;
    const byteOffset = view.byteOffset;
    const byteLength = view.byteLength;
    let buffer;
    try { buffer = transferArrayBuffer(view.buffer); } catch (e) { readIntoRequest.errorSteps(e); return; }
    const pullIntoDescriptor = {
      buffer, bufferByteLength: buffer.byteLength, byteOffset, byteLength,
      bytesFilled: 0, minimumFill, elementSize, viewConstructor: ctor, readerType: "byob",
    };
    if (c._pendingPullIntos.length > 0) {
      c._pendingPullIntos.push(pullIntoDescriptor);
      stream._reader._readIntoRequests.push(readIntoRequest);
      return;
    }
    if (stream._state === "closed") {
      const emptyView = pullIntoDescriptor.viewConstructor === DataView
        ? new DataView(pullIntoDescriptor.buffer, pullIntoDescriptor.byteOffset, 0)
        : new pullIntoDescriptor.viewConstructor(pullIntoDescriptor.buffer, pullIntoDescriptor.byteOffset, 0);
      readIntoRequest.closeSteps(emptyView);
      return;
    }
    if (c._queueTotalSize > 0) {
      if (byteControllerFillPullIntoDescriptorFromQueue(c, pullIntoDescriptor)) {
        const filledView = byteControllerConvertPullIntoDescriptor(pullIntoDescriptor);
        if (c._closeRequested && c._queueTotalSize === 0) { byteControllerClearAlgorithms(c); readableStreamClose(c._stream); }
        readIntoRequest.chunkSteps(filledView);
        return;
      }
      if (c._closeRequested) {
        const e = new TypeError("Insufficient bytes to fill elements in the given buffer");
        byteControllerError(c, e);
        readIntoRequest.errorSteps(e);
        return;
      }
    }
    c._pendingPullIntos.push(pullIntoDescriptor);
    stream._reader._readIntoRequests.push(readIntoRequest);
    byteControllerCallPullIfNeeded(c);
  }
  function setUpByteController(stream, source, highWaterMark) {
    const c = Object.create(ReadableByteStreamController.prototype);
    c._stream = stream;
    c._pullAgain = false; c._pulling = false;
    c._byobRequest = null;
    resetQueue(c);
    c._closeRequested = false; c._started = false;
    c._strategyHWM = highWaterMark;
    c._pullAlgorithm = typeof source.pull === "function" ? () => promiseCall(source.pull, source, [c]) : () => Promise.resolve();
    c._cancelAlgorithm = typeof source.cancel === "function" ? (reason) => promiseCall(source.cancel, source, [reason]) : () => Promise.resolve();
    let autoAllocateChunkSize = source.autoAllocateChunkSize;
    if (autoAllocateChunkSize !== undefined) {
      autoAllocateChunkSize = Number(autoAllocateChunkSize);
      if (!Number.isInteger(autoAllocateChunkSize) || autoAllocateChunkSize <= 0) throw new TypeError("autoAllocateChunkSize must be a positive integer");
    }
    c._autoAllocateChunkSize = autoAllocateChunkSize;
    c._pendingPullIntos = [];
    stream._readableStreamController = c;
    const startResult = typeof source.start === "function" ? source.start.call(source, c) : undefined;
    Promise.resolve(startResult).then(
      () => { c._started = true; byteControllerCallPullIfNeeded(c); },
      (e) => byteControllerError(c, e),
    );
  }

  // ---- tee ----
  function readableStreamTee(stream) {
    const reader = new ReadableStreamDefaultReader(stream);
    let reading = false, readAgain = false, canceled1 = false, canceled2 = false;
    let reason1, reason2, branch1, branch2;
    const cancelDeferred = deferred();
    function pullAlgorithm() {
      if (reading) { readAgain = true; return Promise.resolve(); }
      reading = true;
      readableStreamDefaultReaderRead(reader, {
        chunkSteps: (chunk) => {
          Promise.resolve().then(() => {
            readAgain = false;
            if (!canceled1) { try { branch1._readableStreamController.enqueue ? branch1._readableStreamController.enqueue(chunk) : 0; } catch (e) {} }
            if (!canceled2) { try { branch2._readableStreamController.enqueue(chunk); } catch (e) {} }
            reading = false;
            if (readAgain) pullAlgorithm();
          });
        },
        closeSteps: () => {
          reading = false;
          if (!canceled1) { try { defaultControllerClose(branch1._readableStreamController); } catch (e) {} }
          if (!canceled2) { try { defaultControllerClose(branch2._readableStreamController); } catch (e) {} }
          if (!canceled1 || !canceled2) cancelDeferred.resolve(undefined);
        },
        errorSteps: () => { reading = false; },
      });
      return Promise.resolve();
    }
    function cancel1Algorithm(reason) {
      canceled1 = true; reason1 = reason;
      if (canceled2) {
        const cancelResult = readableStreamCancel(stream, [reason1, reason2]);
        cancelDeferred.resolve(cancelResult);
      }
      return cancelDeferred.promise;
    }
    function cancel2Algorithm(reason) {
      canceled2 = true; reason2 = reason;
      if (canceled1) {
        const cancelResult = readableStreamCancel(stream, [reason1, reason2]);
        cancelDeferred.resolve(cancelResult);
      }
      return cancelDeferred.promise;
    }
    branch1 = createReadableStream(noop, pullAlgorithm, cancel1Algorithm);
    branch2 = createReadableStream(noop, pullAlgorithm, cancel2Algorithm);
    reader._closedDeferred.promise.then(noop, (e) => {
      defaultControllerError(branch1._readableStreamController, e);
      defaultControllerError(branch2._readableStreamController, e);
      if (!canceled1 || !canceled2) cancelDeferred.resolve(undefined);
    });
    return [branch1, branch2];
  }
  function createReadableStream(startAlgorithm, pullAlgorithm, cancelAlgorithm, highWaterMark, sizeAlgorithm) {
    if (highWaterMark === undefined) highWaterMark = 1;
    if (sizeAlgorithm === undefined) sizeAlgorithm = () => 1;
    const stream = Object.create(ReadableStream.prototype);
    stream._state = "readable"; stream._reader = undefined; stream._storedError = undefined; stream._disturbed = false;
    setUpDefaultController(stream, { start: startAlgorithm, pull: pullAlgorithm, cancel: cancelAlgorithm }, highWaterMark, sizeAlgorithm);
    return stream;
  }

  // ---- async iterator ----
  function acquireReadableStreamAsyncIterator(stream, options) {
    const reader = new ReadableStreamDefaultReader(stream);
    const preventCancel = !!(options && options.preventCancel);
    let ongoingPromise;
    let isFinished = false;
    const nextSteps = () => {
      if (isFinished) return Promise.resolve({ value: undefined, done: true });
      if (reader._stream === undefined) return Promise.reject(invalidState("Reader released"));
      const d = deferred();
      readableStreamDefaultReaderRead(reader, {
        chunkSteps: (chunk) => d.resolve({ value: chunk, done: false }),
        closeSteps: () => { isFinished = true; try { readerGenericRelease(reader); } catch (e) {} d.resolve({ value: undefined, done: true }); },
        errorSteps: (e) => { isFinished = true; try { readerGenericRelease(reader); } catch (e2) {} d.reject(e); },
      });
      return d.promise;
    };
    const returnSteps = (value) => {
      if (isFinished) return Promise.resolve({ value, done: true });
      isFinished = true;
      if (reader._stream === undefined) return Promise.resolve({ value, done: true });
      if (!preventCancel) {
        const result = readableStreamCancel(reader._stream, value);
        try { readerGenericRelease(reader); } catch (e) {}
        return result.then(() => ({ value, done: true }));
      }
      try { readerGenericRelease(reader); } catch (e) {}
      return Promise.resolve({ value, done: true });
    };
    const iterator = {
      next() {
        ongoingPromise = ongoingPromise
          ? ongoingPromise.then(() => nextSteps(), () => nextSteps())
          : nextSteps();
        return ongoingPromise;
      },
      return(value) {
        const result = ongoingPromise
          ? ongoingPromise.then(() => returnSteps(value), () => returnSteps(value))
          : returnSteps(value);
        ongoingPromise = result.then(noop, noop);
        return result;
      },
      [Symbol.asyncIterator]() { return this; },
    };
    return iterator;
  }
)JS";

constexpr std::string_view kStreamsJS_part2 = R"JS(
  // ---- pipeTo ----
  function readableStreamPipeTo(source, dest, preventClose, preventAbort, preventCancel, signal) {
    const reader = new ReadableStreamDefaultReader(source);
    const writer = new WritableStreamDefaultWriter(dest);
    source._disturbed = true;
    let shuttingDown = false;
    let currentWrite = Promise.resolve(undefined);
    return new Promise((resolve, reject) => {
      let abortAlgorithm;
      if (signal !== undefined) {
        abortAlgorithm = () => {
          const error = signal.reason !== undefined ? signal.reason : (() => { const e = new Error("The operation was aborted"); e.name = "AbortError"; return e; })();
          const actions = [];
          if (!preventAbort) actions.push(() => (dest._state === "writable" ? writableStreamAbort(dest, error) : Promise.resolve(undefined)));
          if (!preventCancel) actions.push(() => (source._state === "readable" ? readableStreamCancel(source, error) : Promise.resolve(undefined)));
          shutdownWithAction(() => Promise.all(actions.map((a) => a())), true, error);
        };
        if (signal.aborted) { abortAlgorithm(); return; }
        if (typeof signal.addEventListener === "function") signal.addEventListener("abort", abortAlgorithm);
      }
      function pipeStep() {
        if (shuttingDown) return Promise.resolve(true);
        return writer._readyDeferred.promise.then(() => new Promise((resolveRead, rejectRead) => {
          readableStreamDefaultReaderRead(reader, {
            chunkSteps: (chunk) => {
              currentWrite = writableStreamDefaultWriterWrite(writer, chunk).then(noop, noop);
              resolveRead(false);
            },
            closeSteps: () => resolveRead(true),
            errorSteps: rejectRead,
          });
        }));
      }
      function pipeLoop() {
        pipeStep().then((done) => { if (!done) pipeLoop(); }, noop);
      }
      // error/close propagation
      const checkSourceErrored = () => {
        if (source._state === "errored") {
          if (!preventAbort) shutdownWithAction(() => writableStreamAbort(dest, source._storedError), true, source._storedError);
          else shutdown(true, source._storedError);
          return true;
        }
        return false;
      };
      const checkDestErrored = () => {
        if (dest._state === "errored") {
          if (!preventCancel) shutdownWithAction(() => readableStreamCancel(source, dest._storedError), true, dest._storedError);
          else shutdown(true, dest._storedError);
          return true;
        }
        return false;
      };
      const checkSourceClosed = () => {
        if (source._state === "closed") {
          if (!preventClose) shutdownWithAction(() => writableStreamDefaultWriterCloseWithErrorPropagation(writer));
          else shutdown();
          return true;
        }
        return false;
      };
      const checkDestClosed = () => {
        if (writableStreamCloseQueuedOrInFlight(dest) || dest._state === "closed") {
          const destClosed = new TypeError("the destination writable stream closed before all data could be piped to it");
          if (!preventCancel) shutdownWithAction(() => readableStreamCancel(source, destClosed), true, destClosed);
          else shutdown(true, destClosed);
          return true;
        }
        return false;
      };
      if (!checkSourceErrored()) reader._closedDeferred.promise.then(noop, () => checkSourceErrored());
      if (!checkDestErrored()) writer._closedDeferred.promise.then(noop, () => checkDestErrored());
      if (!checkSourceClosed()) reader._closedDeferred.promise.then(() => checkSourceClosed(), noop);
      checkDestClosed();
      pipeLoop();
      function waitForWritesToFinish() {
        const oldCurrentWrite = currentWrite;
        return currentWrite.then(() => (oldCurrentWrite !== currentWrite ? waitForWritesToFinish() : undefined));
      }
      function shutdownWithAction(action, originalIsError, originalError) {
        if (shuttingDown) return;
        shuttingDown = true;
        if (dest._state === "writable" && !writableStreamCloseQueuedOrInFlight(dest)) waitForWritesToFinish().then(doTheRest, doTheRest);
        else doTheRest();
        function doTheRest() {
          action().then(() => finalize(originalIsError, originalError), (newError) => finalize(true, newError));
        }
      }
      function shutdown(isError, error) {
        if (shuttingDown) return;
        shuttingDown = true;
        if (dest._state === "writable" && !writableStreamCloseQueuedOrInFlight(dest)) waitForWritesToFinish().then(() => finalize(isError, error), () => finalize(isError, error));
        else finalize(isError, error);
      }
      function finalize(isError, error) {
        writableStreamDefaultWriterRelease(writer);
        if (reader._stream !== undefined) readerGenericRelease(reader);
        if (signal !== undefined && abortAlgorithm !== undefined && typeof signal.removeEventListener === "function") signal.removeEventListener("abort", abortAlgorithm);
        if (isError) reject(error);
        else resolve(undefined);
      }
    });
  }

  class ReadableStream {
    constructor(underlyingSource, strategy) {
      // AsyncLocalStorage seam: a stream retains its underlying-source
      // algorithms and calls them later, so async_hooks needs to snapshot the
      // constructing frame onto start/pull/cancel. It used to do that by
      // REPLACING globalThis.ReadableStream with a subclass, which silently
      // broke identity for every stream the spec algorithms build internally
      // (tee branches, TransformStream.readable, pipeThrough results): they are
      // created from ReadableStream.prototype, so `x instanceof
      // globalThis.ReadableStream` was false and node's isReadableStream()
      // rejected them ("Received an instance of ReadableStream"). Wrapping the
      // source here instead keeps exactly one ReadableStream class.
      if (G.__mbunWrapStreamSource !== undefined && underlyingSource != null) {
        underlyingSource = G.__mbunWrapStreamSource(underlyingSource);
      }
      const source = underlyingSource == null ? {} : underlyingSource;
      strategy = strategy == null ? {} : strategy;
      if (typeof source !== "object" && typeof source !== "function") throw new TypeError("underlyingSource must be an object");
      this._state = "readable";
      this._reader = undefined;
      this._storedError = undefined;
      this._disturbed = false;
      this._readableStreamController = undefined;
      const type = source.type === undefined ? undefined : String(source.type);
      if (type === "bytes") {
        if (strategy.size !== undefined) throw new RangeError("The strategy for a byte stream cannot have a size function");
        setUpByteController(this, source, extractHWM(strategy, 0));
      } else if (type === "direct") {
        setUpDirectController(this, source, strategy);
      } else if (type === undefined) {
        setUpDefaultController(this, source, extractHWM(strategy, 1), extractSizeAlgorithm(strategy));
      } else {
        throw new RangeError("Invalid type is specified");
      }
    }
    get locked() { return this._reader !== undefined; }
    cancel(reason) {
      if (this.locked) return Promise.reject(new TypeError("Cannot cancel a locked ReadableStream"));
      return readableStreamCancel(this, reason);
    }
    getReader(options) {
      if (options != null && options.mode !== undefined) {
        if (String(options.mode) !== "byob") throw new TypeError('mode must be "byob"');
        return new ReadableStreamBYOBReader(this);
      }
      return new ReadableStreamDefaultReader(this);
    }
    pipeThrough(transform, options) {
      if (transform == null || !("readable" in transform) || !("writable" in transform)) throw new TypeError("pipeThrough requires a {readable, writable} pair");
      options = options == null ? {} : options;
      if (this.locked) throw new TypeError("ReadableStream is locked");
      if (transform.writable.locked) throw new TypeError("WritableStream is locked");
      const promise = readableStreamPipeTo(this, transform.writable, !!options.preventClose, !!options.preventAbort, !!options.preventCancel, options.signal);
      markHandled(promise);
      return transform.readable;
    }
    pipeTo(destination, options) {
      if (!(destination instanceof WritableStream)) return Promise.reject(new TypeError("pipeTo requires a WritableStream"));
      options = options == null ? {} : options;
      let signal;
      if (options.signal !== undefined) {
        signal = options.signal;
        if (signal == null || typeof signal.aborted !== "boolean") return Promise.reject(new TypeError("Invalid signal"));
      }
      if (this.locked) return Promise.reject(new TypeError("ReadableStream is locked"));
      if (destination.locked) return Promise.reject(new TypeError("WritableStream is locked"));
      return readableStreamPipeTo(this, destination, !!options.preventClose, !!options.preventAbort, !!options.preventCancel, signal);
    }
    tee() {
      if (this.locked) throw new TypeError("ReadableStream is locked");
      return readableStreamTee(this);
    }
    values(options) { return acquireReadableStreamAsyncIterator(this, options); }
    [Symbol.asyncIterator](options) { return acquireReadableStreamAsyncIterator(this, options); }
    // Bun extensions: direct consumers on the stream itself.
    text() { const e = consumerUsableError(this); return e ? Promise.reject(e) : consumeStart(this).then(consumeText); }
    json() { const e = consumerUsableError(this); return e ? Promise.reject(e) : consumeStart(this).then(consumeText).then((t) => JSON.parse(t)); }
    bytes() { const e = consumerUsableError(this); return e ? Promise.reject(e) : consumeStart(this).then(consumeBytes); }
    arrayBuffer() { const e = consumerUsableError(this); return e ? Promise.reject(e) : consumeStart(this).then(consumeBytes).then((u8) => u8.buffer); }
    blob() { const e = consumerUsableError(this); const t = this.__mbunBlobType || ""; return e ? Promise.reject(e) : consumeStart(this).then(consumeArray).then((chunks) => new G.Blob(chunks, { type: t })); }
  }

  // =====================================================================
  // WritableStream
  // =====================================================================
  const closeSentinel = Symbol("close sentinel");

  class WritableStream {
    constructor(underlyingSink, strategy) {
      const sink = underlyingSink == null ? {} : underlyingSink;
      strategy = strategy == null ? {} : strategy;
      if (sink.type !== undefined) throw new RangeError("Invalid type is specified");
      initializeWritableStream(this);
      setUpWritableStreamDefaultController(this, sink, extractHWM(strategy, 1), extractSizeAlgorithm(strategy));
    }
    get locked() { return this._writer !== undefined; }
    abort(reason) {
      if (this.locked) return Promise.reject(new TypeError("Cannot abort a locked WritableStream"));
      return writableStreamAbort(this, reason);
    }
    close() {
      if (this.locked) return Promise.reject(new TypeError("Cannot close a locked WritableStream"));
      if (writableStreamCloseQueuedOrInFlight(this)) return Promise.reject(new TypeError("Cannot close an already-closing stream"));
      return writableStreamClose(this);
    }
    getWriter() { return new WritableStreamDefaultWriter(this); }
  }
  function initializeWritableStream(stream) {
    stream._state = "writable";
    stream._storedError = undefined;
    stream._writer = undefined;
    stream._writableStreamController = undefined;
    stream._writeRequests = [];
    stream._inFlightWriteRequest = undefined;
    stream._closeRequest = undefined;
    stream._inFlightCloseRequest = undefined;
    stream._pendingAbortRequest = undefined;
    stream._backpressure = false;
  }
  function writableStreamCloseQueuedOrInFlight(stream) {
    return stream._closeRequest !== undefined || stream._inFlightCloseRequest !== undefined;
  }
  function writableStreamHasOperationMarkedInFlight(stream) {
    return stream._inFlightWriteRequest !== undefined || stream._inFlightCloseRequest !== undefined;
  }
  function writableStreamAbort(stream, reason) {
    if (stream._state === "closed" || stream._state === "errored") return Promise.resolve(undefined);
    if (stream._pendingAbortRequest !== undefined) return stream._pendingAbortRequest.deferred.promise;
    const state = stream._state;
    const wasAlreadyErroring = state === "erroring";
    if (wasAlreadyErroring) reason = undefined;
    const d = deferred();
    stream._pendingAbortRequest = { deferred: d, reason, wasAlreadyErroring };
    if (!wasAlreadyErroring) writableStreamStartErroring(stream, reason);
    return d.promise;
  }
  function writableStreamClose(stream) {
    const state = stream._state;
    if (state === "closed" || state === "errored") return Promise.reject(new TypeError("The stream is not in the writable state"));
    const d = deferred();
    stream._closeRequest = d;
    const writer = stream._writer;
    if (writer !== undefined && stream._backpressure && state === "writable") writer._readyDeferred.resolve(undefined);
    writableStreamDefaultControllerClose(stream._writableStreamController);
    return d.promise;
  }
  function writableStreamAddWriteRequest(stream) {
    const d = deferred();
    stream._writeRequests.push(d);
    return d.promise;
  }
  function writableStreamDealWithRejection(stream, error) {
    if (stream._state === "writable") { writableStreamStartErroring(stream, error); return; }
    writableStreamFinishErroring(stream);
  }
  function writableStreamStartErroring(stream, reason) {
    const controller = stream._writableStreamController;
    stream._state = "erroring";
    stream._storedError = reason;
    const writer = stream._writer;
    if (writer !== undefined) writableStreamDefaultWriterEnsureReadyPromiseRejected(writer, reason);
    if (!writableStreamHasOperationMarkedInFlight(stream) && controller._started) writableStreamFinishErroring(stream);
  }
  function writableStreamFinishErroring(stream) {
    stream._state = "errored";
    rejectClosed(stream, stream._storedError);
    stream._writableStreamController._errorSteps();
    const storedError = stream._storedError;
    for (const writeRequest of stream._writeRequests) writeRequest.reject(storedError);
    stream._writeRequests = [];
    if (stream._pendingAbortRequest === undefined) { writableStreamRejectCloseAndClosedPromiseIfNeeded(stream); return; }
    const abortRequest = stream._pendingAbortRequest;
    stream._pendingAbortRequest = undefined;
    if (abortRequest.wasAlreadyErroring) {
      abortRequest.deferred.reject(storedError);
      markHandled(abortRequest.deferred.promise);
      writableStreamRejectCloseAndClosedPromiseIfNeeded(stream);
      return;
    }
    stream._writableStreamController._abortSteps(abortRequest.reason).then(
      () => { abortRequest.deferred.resolve(undefined); writableStreamRejectCloseAndClosedPromiseIfNeeded(stream); },
      (e) => { abortRequest.deferred.reject(e); writableStreamRejectCloseAndClosedPromiseIfNeeded(stream); },
    );
  }
  function writableStreamRejectCloseAndClosedPromiseIfNeeded(stream) {
    if (stream._closeRequest !== undefined) {
      stream._closeRequest.reject(stream._storedError);
      markHandled(stream._closeRequest.promise);
      stream._closeRequest = undefined;
    }
    const writer = stream._writer;
    if (writer !== undefined) {
      writer._closedDeferred.reject(stream._storedError);
      markHandled(writer._closedDeferred.promise);
    }
  }
  function writableStreamMarkCloseRequestInFlight(stream) {
    stream._inFlightCloseRequest = stream._closeRequest;
    stream._closeRequest = undefined;
  }
  function writableStreamMarkFirstWriteRequestInFlight(stream) {
    stream._inFlightWriteRequest = stream._writeRequests.shift();
  }
  function writableStreamFinishInFlightWrite(stream) {
    stream._inFlightWriteRequest.resolve(undefined);
    stream._inFlightWriteRequest = undefined;
  }
  function writableStreamFinishInFlightWriteWithError(stream, error) {
    stream._inFlightWriteRequest.reject(error);
    markHandled(stream._inFlightWriteRequest.promise);
    stream._inFlightWriteRequest = undefined;
    writableStreamDealWithRejection(stream, error);
  }
  function writableStreamFinishInFlightClose(stream) {
    stream._inFlightCloseRequest.resolve(undefined);
    stream._inFlightCloseRequest = undefined;
    if (stream._state === "erroring") {
      stream._storedError = undefined;
      if (stream._pendingAbortRequest !== undefined) {
        stream._pendingAbortRequest.deferred.resolve(undefined);
        stream._pendingAbortRequest = undefined;
      }
    }
    stream._state = "closed";
    resolveClosed(stream);
    const writer = stream._writer;
    if (writer !== undefined) writer._closedDeferred.resolve(undefined);
  }
  function writableStreamFinishInFlightCloseWithError(stream, error) {
    stream._inFlightCloseRequest.reject(error);
    markHandled(stream._inFlightCloseRequest.promise);
    stream._inFlightCloseRequest = undefined;
    if (stream._pendingAbortRequest !== undefined) {
      stream._pendingAbortRequest.deferred.reject(error);
      markHandled(stream._pendingAbortRequest.deferred.promise);
      stream._pendingAbortRequest = undefined;
    }
    writableStreamDealWithRejection(stream, error);
  }
  function writableStreamUpdateBackpressure(stream, backpressure) {
    const writer = stream._writer;
    if (writer !== undefined && backpressure !== stream._backpressure) {
      if (backpressure) writer._readyDeferred = deferred();
      else writer._readyDeferred.resolve(undefined);
    }
    stream._backpressure = backpressure;
  }

  class WritableStreamDefaultWriter {
    constructor(stream) {
      if (!(stream instanceof WritableStream)) throw new TypeError("WritableStreamDefaultWriter needs a WritableStream");
      if (stream.locked) throw new TypeError("WritableStream is locked");
      this._stream = stream;
      stream._writer = this;
      const state = stream._state;
      this._closedDeferred = deferred();
      this._readyDeferred = deferred();
      if (state === "writable") {
        if (!writableStreamCloseQueuedOrInFlight(stream) && stream._backpressure) { /* ready stays pending */ }
        else this._readyDeferred.resolve(undefined);
      } else if (state === "erroring") {
        this._readyDeferred.reject(stream._storedError);
        markHandled(this._readyDeferred.promise);
      } else if (state === "closed") {
        this._readyDeferred.resolve(undefined);
        this._closedDeferred.resolve(undefined);
      } else {
        const storedError = stream._storedError;
        this._readyDeferred.reject(storedError);
        this._closedDeferred.reject(storedError);
        markHandled(this._readyDeferred.promise);
        markHandled(this._closedDeferred.promise);
      }
    }
    get closed() { return this._closedDeferred.promise; }
    get desiredSize() {
      if (this._stream === undefined) throw new TypeError("The writer is not attached to a stream");
      return writableStreamDefaultWriterGetDesiredSize(this);
    }
    get ready() { return this._readyDeferred.promise; }
    abort(reason) {
      if (this._stream === undefined) return Promise.reject(new TypeError("The writer is not attached to a stream"));
      return writableStreamAbort(this._stream, reason);
    }
    close() {
      const stream = this._stream;
      if (stream === undefined) return Promise.reject(new TypeError("The writer is not attached to a stream"));
      if (writableStreamCloseQueuedOrInFlight(stream)) return Promise.reject(new TypeError("Cannot close an already-closing stream"));
      return writableStreamClose(stream);
    }
    releaseLock() {
      const stream = this._stream;
      if (stream === undefined) return;
      writableStreamDefaultWriterRelease(this);
    }
    write(chunk) {
      if (this._stream === undefined) return Promise.reject(new TypeError("The writer is not attached to a stream"));
      return writableStreamDefaultWriterWrite(this, chunk);
    }
  }
  function writableStreamDefaultWriterGetDesiredSize(writer) {
    const stream = writer._stream;
    const state = stream._state;
    if (state === "errored" || state === "erroring") return null;
    if (state === "closed") return 0;
    return stream._writableStreamController._strategyHWM - stream._writableStreamController._queueTotalSize;
  }
  function writableStreamDefaultWriterEnsureReadyPromiseRejected(writer, error) {
    if (writer._readyDeferredSettled !== true) {
      // replace with a rejected ready promise
      writer._readyDeferred = deferred();
    }
    writer._readyDeferred.reject(error);
    markHandled(writer._readyDeferred.promise);
  }
  function writableStreamDefaultWriterEnsureClosedPromiseRejected(writer, error) {
    writer._closedDeferred = deferred();
    writer._closedDeferred.reject(error);
    markHandled(writer._closedDeferred.promise);
  }
  function writableStreamDefaultWriterCloseWithErrorPropagation(writer) {
    const stream = writer._stream;
    const state = stream._state;
    if (writableStreamCloseQueuedOrInFlight(stream) || state === "closed") return Promise.resolve(undefined);
    if (state === "errored") return Promise.reject(stream._storedError);
    return writableStreamClose(stream);
  }
  function writableStreamDefaultWriterRelease(writer) {
    const stream = writer._stream;
    if (stream === undefined) return;
    const releasedError = new TypeError("Writer was released and can no longer be used to monitor the stream's closedness");
    writableStreamDefaultWriterEnsureReadyPromiseRejected(writer, releasedError);
    writableStreamDefaultWriterEnsureClosedPromiseRejected(writer, releasedError);
    stream._writer = undefined;
    writer._stream = undefined;
  }
  function writableStreamDefaultWriterWrite(writer, chunk) {
    const stream = writer._stream;
    const controller = stream._writableStreamController;
    let chunkSize;
    try { chunkSize = Number(controller._strategySizeAlgorithm(chunk)); }
    catch (e) { writableStreamDefaultControllerErrorIfNeeded(controller, e); return Promise.reject(e); }
    if (stream !== writer._stream) return Promise.reject(new TypeError("The writer is not attached to this stream"));
    const state = stream._state;
    if (state === "errored") return Promise.reject(stream._storedError);
    if (writableStreamCloseQueuedOrInFlight(stream) || state === "closed") return Promise.reject(new TypeError("The stream is closing or closed"));
    if (state === "erroring") return Promise.reject(stream._storedError);
    const promise = writableStreamAddWriteRequest(stream);
    writableStreamDefaultControllerWrite(controller, chunk, chunkSize);
    return promise;
  }

  class WritableStreamDefaultController {
    constructor() { throw new TypeError("Illegal constructor"); }
    get abortReason() { return this._abortReason; }
    get signal() {
      if (this._abortController === undefined && typeof G.AbortController === "function") this._abortController = new G.AbortController();
      return this._abortController ? this._abortController.signal : undefined;
    }
    error(e) {
      if (this._stream === undefined || this._stream._state !== "writable") return;
      writableStreamDefaultControllerError(this, e);
    }
    _abortSteps(reason) {
      const result = this._abortAlgorithm(reason);
      writableStreamDefaultControllerClearAlgorithms(this);
      return result;
    }
    _errorSteps() { resetQueue(this); }
  }
  function setUpWritableStreamDefaultController(stream, sink, highWaterMark, sizeAlgorithm) {
    const c = Object.create(WritableStreamDefaultController.prototype);
    c._stream = stream;
    stream._writableStreamController = c;
    resetQueue(c);
    c._abortReason = undefined;
    c._abortController = undefined;
    c._started = false;
    c._strategySizeAlgorithm = sizeAlgorithm;
    c._strategyHWM = highWaterMark;
    c._writeAlgorithm = typeof sink.write === "function" ? (chunk) => promiseCall(sink.write, sink, [chunk, c]) : () => Promise.resolve();
    c._closeAlgorithm = typeof sink.close === "function" ? () => promiseCall(sink.close, sink, []) : () => Promise.resolve();
    c._abortAlgorithm = typeof sink.abort === "function" ? (reason) => promiseCall(sink.abort, sink, [reason]) : () => Promise.resolve();
    const backpressure = writableStreamDefaultControllerGetBackpressure(c);
    writableStreamUpdateBackpressure(stream, backpressure);
    const startResult = typeof sink.start === "function" ? sink.start.call(sink, c) : undefined;
    Promise.resolve(startResult).then(
      () => { c._started = true; writableStreamDefaultControllerAdvanceQueueIfNeeded(c); },
      (e) => { c._started = true; writableStreamDealWithRejection(stream, e); },
    );
  }
  function writableStreamDefaultControllerClearAlgorithms(c) {
    c._writeAlgorithm = () => Promise.resolve();
    c._closeAlgorithm = () => Promise.resolve();
    c._abortAlgorithm = () => Promise.resolve();
    c._strategySizeAlgorithm = () => 1;
  }
  function writableStreamDefaultControllerClose(c) {
    enqueueValueWithSize(c, closeSentinel, 0);
    writableStreamDefaultControllerAdvanceQueueIfNeeded(c);
  }
  function writableStreamDefaultControllerWrite(c, chunk, chunkSize) {
    try { enqueueValueWithSize(c, chunk, chunkSize); }
    catch (e) { writableStreamDefaultControllerErrorIfNeeded(c, e); return; }
    const stream = c._stream;
    if (!writableStreamCloseQueuedOrInFlight(stream) && stream._state === "writable") {
      writableStreamUpdateBackpressure(stream, writableStreamDefaultControllerGetBackpressure(c));
    }
    writableStreamDefaultControllerAdvanceQueueIfNeeded(c);
  }
  function writableStreamDefaultControllerAdvanceQueueIfNeeded(c) {
    const stream = c._stream;
    if (!c._started) return;
    if (stream._inFlightWriteRequest !== undefined) return;
    const state = stream._state;
    if (state === "closed" || state === "errored") return;
    if (state === "erroring") { writableStreamFinishErroring(stream); return; }
    if (c._queue.length === 0) return;
    const value = c._queue[0].value;
    if (value === closeSentinel) writableStreamDefaultControllerProcessClose(c);
    else writableStreamDefaultControllerProcessWrite(c, value);
  }
  function writableStreamDefaultControllerErrorIfNeeded(c, error) {
    if (c._stream._state === "writable") writableStreamDefaultControllerError(c, error);
  }
  function writableStreamDefaultControllerProcessClose(c) {
    const stream = c._stream;
    writableStreamMarkCloseRequestInFlight(stream);
    dequeueValue(c);
    const sinkClosePromise = c._closeAlgorithm();
    writableStreamDefaultControllerClearAlgorithms(c);
    sinkClosePromise.then(
      () => writableStreamFinishInFlightClose(stream),
      (reason) => writableStreamFinishInFlightCloseWithError(stream, reason),
    );
  }
  function writableStreamDefaultControllerProcessWrite(c, chunk) {
    const stream = c._stream;
    writableStreamMarkFirstWriteRequestInFlight(stream);
    c._writeAlgorithm(chunk).then(
      () => {
        writableStreamFinishInFlightWrite(stream);
        const state = stream._state;
        dequeueValue(c);
        if (!writableStreamCloseQueuedOrInFlight(stream) && state === "writable") {
          writableStreamUpdateBackpressure(stream, writableStreamDefaultControllerGetBackpressure(c));
        }
        writableStreamDefaultControllerAdvanceQueueIfNeeded(c);
      },
      (reason) => {
        if (stream._state === "writable") writableStreamDefaultControllerClearAlgorithms(c);
        writableStreamFinishInFlightWriteWithError(stream, reason);
      },
    );
  }
  function writableStreamDefaultControllerGetBackpressure(c) {
    return c._strategyHWM - c._queueTotalSize <= 0;
  }
  function writableStreamDefaultControllerError(c, error) {
    const stream = c._stream;
    writableStreamDefaultControllerClearAlgorithms(c);
    writableStreamStartErroring(stream, error);
  }
  function createWritableStream(startAlgorithm, writeAlgorithm, closeAlgorithm, abortAlgorithm, highWaterMark, sizeAlgorithm) {
    if (highWaterMark === undefined) highWaterMark = 1;
    if (sizeAlgorithm === undefined) sizeAlgorithm = () => 1;
    const stream = Object.create(WritableStream.prototype);
    initializeWritableStream(stream);
    setUpWritableStreamDefaultController(stream, { start: startAlgorithm, write: writeAlgorithm, close: closeAlgorithm, abort: abortAlgorithm }, highWaterMark, sizeAlgorithm);
    return stream;
  }

  // =====================================================================
  // TransformStream
  // =====================================================================
  class TransformStream {
    constructor(transformer, writableStrategy, readableStrategy) {
      transformer = transformer == null ? {} : transformer;
      writableStrategy = writableStrategy == null ? {} : writableStrategy;
      readableStrategy = readableStrategy == null ? {} : readableStrategy;
      if (transformer.readableType !== undefined) throw new RangeError("Invalid readableType");
      if (transformer.writableType !== undefined) throw new RangeError("Invalid writableType");
      const readableHWM = extractHWM(readableStrategy, 0);
      const readableSizeAlgorithm = extractSizeAlgorithm(readableStrategy);
      const writableHWM = extractHWM(writableStrategy, 1);
      const writableSizeAlgorithm = extractSizeAlgorithm(writableStrategy);
      const startDeferred = deferred();
      initializeTransformStream(this, startDeferred.promise, writableHWM, writableSizeAlgorithm, readableHWM, readableSizeAlgorithm);
      setUpTransformStreamDefaultControllerFromTransformer(this, transformer);
      const startResult = typeof transformer.start === "function" ? transformer.start.call(transformer, this._transformStreamController) : undefined;
      startDeferred.resolve(startResult);
    }
    get readable() { return this._readable; }
    get writable() { return this._writable; }
  }
  function initializeTransformStream(stream, startPromise, writableHWM, writableSizeAlgorithm, readableHWM, readableSizeAlgorithm) {
    const startAlgorithm = () => startPromise;
    stream._writable = createWritableStream(
      startAlgorithm,
      (chunk) => transformStreamDefaultSinkWriteAlgorithm(stream, chunk),
      () => transformStreamDefaultSinkCloseAlgorithm(stream),
      (reason) => transformStreamDefaultSinkAbortAlgorithm(stream, reason),
      writableHWM, writableSizeAlgorithm,
    );
    stream._readable = createReadableStream(
      startAlgorithm,
      () => transformStreamDefaultSourcePullAlgorithm(stream),
      (reason) => { transformStreamErrorWritableAndUnblockWrite(stream, reason); return Promise.resolve(undefined); },
      readableHWM, readableSizeAlgorithm,
    );
    stream._backpressure = undefined;
    stream._backpressureChangeDeferred = undefined;
    transformStreamSetBackpressure(stream, true);
    stream._transformStreamController = undefined;
  }
  function transformStreamError(stream, e) {
    defaultControllerError(stream._readable._readableStreamController, e);
    transformStreamErrorWritableAndUnblockWrite(stream, e);
  }
  function transformStreamErrorWritableAndUnblockWrite(stream, e) {
    transformStreamDefaultControllerClearAlgorithms(stream._transformStreamController);
    writableStreamDefaultControllerErrorIfNeeded(stream._writable._writableStreamController, e);
    if (stream._backpressure) transformStreamSetBackpressure(stream, false);
  }
  function transformStreamSetBackpressure(stream, backpressure) {
    if (stream._backpressureChangeDeferred !== undefined) stream._backpressureChangeDeferred.resolve(undefined);
    stream._backpressureChangeDeferred = deferred();
    stream._backpressure = backpressure;
  }

  class TransformStreamDefaultController {
    constructor() { throw new TypeError("Illegal constructor"); }
    get desiredSize() {
      if (this._stream === undefined || this._stream._readable._readableStreamController === undefined) return null;
      return defaultControllerGetDesiredSize(this._stream._readable._readableStreamController);
    }
    enqueue(chunk) { transformStreamDefaultControllerEnqueue(this, chunk); }
    error(reason) {
      if (this._stream === undefined) return;
      transformStreamError(this._stream, reason);
    }
    terminate() {
      if (this._stream === undefined) return;
      transformStreamDefaultControllerTerminate(this);
    }
  }
  function setUpTransformStreamDefaultControllerFromTransformer(stream, transformer) {
    const c = Object.create(TransformStreamDefaultController.prototype);
    c._stream = stream;
    stream._transformStreamController = c;
    c._transformAlgorithm = typeof transformer.transform === "function"
      ? (chunk) => promiseCall(transformer.transform, transformer, [chunk, c])
      : (chunk) => { try { transformStreamDefaultControllerEnqueue(c, chunk); return Promise.resolve(undefined); } catch (e) { return Promise.reject(e); } };
    c._flushAlgorithm = typeof transformer.flush === "function" ? () => promiseCall(transformer.flush, transformer, [c]) : () => Promise.resolve();
    c._cancelAlgorithm = typeof transformer.cancel === "function" ? (reason) => promiseCall(transformer.cancel, transformer, [reason]) : () => Promise.resolve();
  }
  function transformStreamDefaultControllerClearAlgorithms(c) {
    if (c === undefined) return;
    c._transformAlgorithm = () => Promise.resolve();
    c._flushAlgorithm = () => Promise.resolve();
    c._cancelAlgorithm = () => Promise.resolve();
  }
  function transformStreamDefaultControllerEnqueue(c, chunk) {
    const stream = c._stream;
    if (stream === undefined) throw new TypeError("Invalid state: controller is not attached to a stream");
    const readableController = stream._readable._readableStreamController;
    if (!defaultControllerCanCloseOrEnqueue(readableController)) throw new TypeError("The readable side is not in a state that permits enqueue");
    try { defaultControllerEnqueue(readableController, chunk); }
    catch (e) {
      transformStreamErrorWritableAndUnblockWrite(stream, e);
      throw stream._readable._storedError;
    }
    const backpressure = defaultControllerGetDesiredSize(readableController) <= 0;
    if (backpressure !== stream._backpressure) transformStreamSetBackpressure(stream, true);
  }
  function transformStreamDefaultControllerTerminate(c) {
    const stream = c._stream;
    const readableController = stream._readable._readableStreamController;
    defaultControllerClose(readableController);
    transformStreamErrorWritableAndUnblockWrite(stream, new TypeError("The stream has been terminated"));
  }
  function transformStreamDefaultSinkWriteAlgorithm(stream, chunk) {
    const controller = stream._transformStreamController;
    if (stream._backpressure) {
      const backpressureChangePromise = stream._backpressureChangeDeferred.promise;
      return backpressureChangePromise.then(() => {
        const writable = stream._writable;
        if (writable._state === "erroring") throw writable._storedError;
        return transformStreamDefaultControllerPerformTransform(controller, chunk);
      });
    }
    return transformStreamDefaultControllerPerformTransform(controller, chunk);
  }
  function transformStreamDefaultControllerPerformTransform(c, chunk) {
    return c._transformAlgorithm(chunk).then(noop, (e) => {
      transformStreamError(c._stream, e);
      throw e;
    });
  }
  function transformStreamDefaultSinkAbortAlgorithm(stream, reason) {
    const controller = stream._transformStreamController;
    const cancelPromise = controller._cancelAlgorithm(reason);
    transformStreamError(stream, reason);
    return cancelPromise.then(noop, noop);
  }
  function transformStreamDefaultSinkCloseAlgorithm(stream) {
    const readable = stream._readable;
    const controller = stream._transformStreamController;
    const flushPromise = controller._flushAlgorithm();
    transformStreamDefaultControllerClearAlgorithms(controller);
    return flushPromise.then(
      () => {
        if (readable._state === "errored") throw readable._storedError;
        try { defaultControllerClose(readable._readableStreamController); } catch (e) {}
      },
      (r) => {
        transformStreamError(stream, r);
        throw readable._storedError;
      },
    );
  }
  function transformStreamDefaultSourcePullAlgorithm(stream) {
    transformStreamSetBackpressure(stream, false);
    return stream._backpressureChangeDeferred.promise;
  }

  // =====================================================================
  // Consumers (Bun.readableStreamTo* family) + shared internals
  // =====================================================================
  function consumeStart(stream) {
    // acquire a reader (async so lock errors become rejections)
    try {
      const r = new ReadableStreamDefaultReader(stream);
      // Body consumption (Response/Request .text()/.arrayBuffer()/…) keeps the
      // reader attached: the fetch spec's "fully reading body as promise" never
      // releases it, so `req.body.locked` stays true afterwards (issue 07001).
      if (stream && stream.__mbunBodyRetainLock) r._keepLock = true;
      return Promise.resolve(r);
    }
    catch (e) { return Promise.reject(e); }
  }
  // Incremental WHATWG utf-8 decoder with U+FFFD replacement.
  function makeUtf8Decoder() {
    let cp = 0, bytesNeeded = 0, bytesSeen = 0, lower = 0x80, upper = 0xbf;
    let atStart = true;
    return {
      decode(u8, last) {
        let out = "";
        for (let i = 0; i < u8.length; i++) {
          const b = u8[i];
          if (bytesNeeded === 0) {
            if (b <= 0x7f) out += String.fromCharCode(b);
            else if (b >= 0xc2 && b <= 0xdf) { bytesNeeded = 1; cp = b & 0x1f; }
            else if (b >= 0xe0 && b <= 0xef) { if (b === 0xe0) lower = 0xa0; if (b === 0xed) upper = 0x9f; bytesNeeded = 2; cp = b & 0xf; }
            else if (b >= 0xf0 && b <= 0xf4) { if (b === 0xf0) lower = 0x90; if (b === 0xf4) upper = 0x8f; bytesNeeded = 3; cp = b & 7; }
            else out += "�";
          } else if (b < lower || b > upper) {
            cp = 0; bytesNeeded = 0; bytesSeen = 0; lower = 0x80; upper = 0xbf;
            out += "�"; i--;
          } else {
            lower = 0x80; upper = 0xbf;
            cp = (cp << 6) | (b & 0x3f);
            if (++bytesSeen === bytesNeeded) {
              out += String.fromCodePoint(cp);
              cp = 0; bytesNeeded = 0; bytesSeen = 0;
            }
          }
        }
        if (last && bytesNeeded !== 0) { bytesNeeded = 0; bytesSeen = 0; out += "�"; }
        if (atStart && out.length > 0) {
          atStart = false;
          if (out.charCodeAt(0) === 0xfeff) out = out.slice(1);
        }
        return out;
      },
    };
  }
  // `limitKind` applies bun's synthetic allocation limit to the string being
  // built (Body.rs guards every string materialization). Null skips it.
  async function consumeText(reader, limitKind) {
    let out = "";
    let seen = 0;
    let dec = null;
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (typeof value === "string") {
          if (dec) { out += dec.decode(new Uint8Array(0), true); dec = null; }
          seen += value.length;
          // Check BEFORE appending/decoding: a body can arrive as a single
          // multi-hundred-MB chunk, and decoding it first is exactly the
          // allocation the limit exists to prevent.
          if (limitKind) G.__mbunCheckAllocLimit(seen, limitKind);
          out += value.charCodeAt(0) === 0xfeff ? value.slice(1) : value;
        } else {
          const u8 = toU8(value);
          if (u8 === null) throw new TypeError("Received a chunk that is neither a string nor an ArrayBuffer/TypedArray");
          seen += u8.byteLength;
          if (limitKind) G.__mbunCheckAllocLimit(seen, limitKind);
          if (!dec) dec = makeUtf8Decoder();
          out += dec.decode(u8, false);
        }
      }
      if (dec) out += dec.decode(new Uint8Array(0), true);
      return out;
    } finally { if (reader._stream !== undefined && !reader._keepLock) reader.releaseLock(); }
  }
  // `guard` applies the synthetic allocation limit; arrayBuffer() is exempt in
  // bun (an ArrayBuffer has no 2^32-1 cap), so it passes false.
  async function consumeBytes(reader, guard) {
    const parts = [];
    let total = 0;
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        const u8 = typeof value === "string" ? utf8Encode(value) : toU8(value);
        if (u8 === null) throw new TypeError("Received a chunk that is neither a string nor an ArrayBuffer/TypedArray");
        parts.push(u8);
        total += u8.byteLength;
      }
      if (guard) G.__mbunCheckAllocLimit(total, "bytes");
      const out = new Uint8Array(total);
      let off = 0;
      for (const p of parts) { out.set(p, off); off += p.byteLength; }
      return out;
    } finally { if (reader._stream !== undefined && !reader._keepLock) reader.releaseLock(); }
  }
  async function consumeArray(reader) {
    const out = [];
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        out.push(value);
      }
      return out;
    } finally { if (reader._stream !== undefined && !reader._keepLock) reader.releaseLock(); }
  }
  // Bun's usable-state checks: consumers reject on locked/used streams and
  // synchronously reject on a detached queued chunk (validated eagerly).
  function consumerUsableError(stream) {
    if (!isReadableStream(stream)) return new TypeError("Expected a ReadableStream");
    // ByteBlobLoader.rs:223 to_buffered_value checks its store BEFORE anything
    // else: once the owning Body consumed (and detached) the blob store, every
    // stream-level consumer rejects with ERR_BODY_ALREADY_USED, not "locked".
    if (stream.__mbunBodyStoreDetached) { const e = new TypeError("Body already used"); e.code = "ERR_BODY_ALREADY_USED"; return e; }
    if (stream.locked) return new TypeError("ReadableStream is locked");
    if (stream._disturbed) { const e = new Error("ReadableStream has already been used"); e.code = "ERR_BODY_ALREADY_USED"; return e; }
    return null;
  }
  function validateQueuedChunksSync(stream) {
    const c = stream._readableStreamController;
    if (c === undefined || c._queue === undefined) return;
    for (const entry of c._queue) {
      const v = entry.value !== undefined ? entry.value : entry;
      if (isView(v) && isDetachedBuffer(v.buffer) && v !== entry) throw invalidState("Cannot validate on a detached buffer");
      if (entry.value !== undefined && isView(entry.value) && isDetachedBuffer(entry.value.buffer)) throw invalidState("Cannot validate on a detached buffer");
    }
  }
  // bun ByteBlobLoader fast-path: a blob-backed stream (Blob.stream()) carries
  // its full bytes synchronously, so consumers resolve WITHOUT a microtask hop
  // (Bun.peek.status === "fulfilled"). Claims the stream (disturbed) once taken.
  function takeBlobFastBytes(stream) {
    if (!stream || stream.locked || stream._disturbed) return null;
    const b = stream.__mbunBlobBytes;
    if (!isView(b)) return null;
    stream._disturbed = true;
    try { stream.__mbunBlobBytes = undefined; } catch (e) {}
    return b;
  }
  const consumers = {
    text(stream) {
      const err = consumerUsableError(stream);
      if (err) return Promise.reject(err);
      const fb = takeBlobFastBytes(stream);
      if (fb) { try { G.__mbunCheckAllocLimit(fb.byteLength, "text"); } catch (e) { return Promise.reject(e); }
        return Promise.resolve(new G.TextDecoder().decode(fb)); }
      return consumeStart(stream).then((r) => consumeText(r, "text"));
    },
    json(stream) {
      const err = consumerUsableError(stream);
      if (err) return Promise.reject(err);
      const fb = takeBlobFastBytes(stream);
      if (fb) { try { G.__mbunCheckAllocLimit(fb.byteLength, "json"); return Promise.resolve(JSON.parse(new G.TextDecoder().decode(fb))); } catch (e) { return Promise.reject(e); } }
      return consumeStart(stream).then((r) => consumeText(r, "json")).then((t) => JSON.parse(t));
    },
    bytes(stream) {
      const err = consumerUsableError(stream);
      if (err) return Promise.reject(err);
      const fb = takeBlobFastBytes(stream);
      if (fb) { try { G.__mbunCheckAllocLimit(fb.byteLength, "bytes"); } catch (e) { return Promise.reject(e); }
        return Promise.resolve(new Uint8Array(fb)); }
      validateQueuedChunksSync(stream);
      return consumeStart(stream).then((r) => consumeBytes(r, true));
    },
    arrayBuffer(stream) {
      const err = consumerUsableError(stream);
      if (err) return Promise.reject(err);
      const fb = takeBlobFastBytes(stream);
      if (fb) return Promise.resolve(fb.buffer.slice(fb.byteOffset, fb.byteOffset + fb.byteLength));
      validateQueuedChunksSync(stream);
      return consumeStart(stream).then((r) => consumeBytes(r, false)).then((u8) => u8.buffer);
    },
    array(stream) {
      const err = consumerUsableError(stream);
      if (err) return Promise.reject(err);
      return consumeStart(stream).then(consumeArray);
    },
    blob(stream) {
      const err = consumerUsableError(stream);
      if (err) return Promise.reject(err);
      // A stream off a Blob carries its type (see Blob.stream) — bun round-trips it.
      const t = (stream && stream.__mbunBlobType) || "";
      const fb = takeBlobFastBytes(stream);
      if (fb) return Promise.resolve(new G.Blob([fb], { type: t }));
      return consumeStart(stream).then(consumeArray).then((chunks) => new G.Blob(chunks, { type: t }));
    },
    // raw internals for Response/Request/builtins wiring
    // Mark a stream as a *body* being fully read: consumeStart then keeps the
    // reader attached so `.locked` stays true after consumption, matching the
    // fetch spec (and bun) for Response/Request body consumers (issue 07001).
    retainLock(stream) { if (stream && typeof stream === "object") { try { stream.__mbunBodyRetainLock = true; } catch (e) {} } },
    // Body.rs use_as_any_blob detached the blob store behind this body stream.
    detachBodyStore(stream) { if (stream && typeof stream === "object") { try { stream.__mbunBodyStoreDetached = true; } catch (e) {} } },
    // The *Raw variants are the non-body consumers, so they pass no allocation
    // limit kind: only Response/Request body consumption is capped (bun caps
    // the body path, not every stream read).
    consumeTextRaw: (stream) => consumeStart(stream).then((r) => consumeText(r, null)),
    consumeBytesRaw: (stream) => consumeStart(stream).then((r) => consumeBytes(r, false)),
    consumeArrayRaw: (stream) => consumeStart(stream).then(consumeArray),
    usableError: consumerUsableError,
    isReadableStream,
    isDisturbed: (s) => !!(s && s._disturbed),
    // node's stream[kIsClosedPromise]: settles when the stream closes/errors
    // WITHOUT acquiring a reader/writer. node:stream's eosWeb needs exactly this.
    closedPromise: streamClosedPromise,
    // Bun `type: "direct"` serve path: hand the raw pull/cancel functions to the
    // HTTP layer so it can drive the source with an HTTPResponseSink controller
    // (write/flush map to the socket) instead of buffering through a reader.
    // Claims the stream (marks it disturbed) so no other consumer double-reads.
    directStreamSource(stream) {
      if (!isReadableStream(stream) || stream._disturbed) return null;
      const c = stream._readableStreamController;
      if (!(c instanceof ReadableStreamDirectController)) return null;
      stream._disturbed = true;
      return { pull: c._pullFn, cancel: c._cancelFn };
    },
  };

  // ---- install globals ----
  G.ReadableStream = ReadableStream;
  G.ReadableStreamDefaultReader = ReadableStreamDefaultReader;
  G.ReadableStreamBYOBReader = ReadableStreamBYOBReader;
  G.ReadableStreamBYOBRequest = ReadableStreamBYOBRequest;
  G.ReadableStreamDefaultController = ReadableStreamDefaultController;
  G.ReadableByteStreamController = ReadableByteStreamController;
  G.WritableStream = WritableStream;
  G.WritableStreamDefaultWriter = WritableStreamDefaultWriter;
  G.WritableStreamDefaultController = WritableStreamDefaultController;
  G.TransformStream = TransformStream;
  G.TransformStreamDefaultController = TransformStreamDefaultController;
  G.ByteLengthQueuingStrategy = ByteLengthQueuingStrategy;
  G.CountQueuingStrategy = CountQueuingStrategy;
  G.__mbunStreams = consumers;
)JS";

// The IIFE opened in kStreamsJS_part1 stays OPEN through part2; it is closed at
// the end of kStreamsJS_part3 (js_streams_inspect.cppm), which — still inside
// the same closure — attaches the nodejs.util.inspect.custom methods to each
// stream prototype. Split out to keep this file under the 2000-line hard limit.
export inline const std::string kStreamsJS =
    std::string{kStreamsJS_part1}.append(kStreamsJS_part2).append(kStreamsJS_part3);

}  // namespace mbun::jsc::builtins
