// node:stream payload partition — internal/streams/{pipeline,operators,compose,duplexpair} + stream.promises + the internal/stream aggregator, then registration of node:stream over the bootstrap stub.
//
// Mechanical 1:1 translation of bun-ref src/js/internal/streams/*.ts (三段法
// stage 1): TS types stripped and bun's $-intrinsics lowered onto the shims in
// node_stream_core.cppm; algorithm, branches and error text are the blueprint's,
// not reinvented. The blueprint's deliberately-lazy cycle-breaking requires
// (readable->compose, duplex->duplexify, pipeline->readable) are preserved and
// resolved by the CJS registry, so module init order matches bun's.
//
// NOTE: appended AFTER the master builtins IIFE has closed (see image_closure),
// so this is a self-contained IIFE that re-binds G = globalThis and pulls shared
// helpers off G.__mbunStreamReg.
export module mbun.jsc.js_builtins:node_stream_pipeline;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeStreamPipelineJS = R"JS(
(function () {
  const G = globalThis;
  const R = G.__mbunStreamReg;
  if (!R) return;
  const process = G.process;
  const Buffer = G.Buffer;
  const { $ERR_INVALID_ARG_TYPE, $ERR_INVALID_ARG_VALUE, $ERR_INVALID_RETURN_VALUE, $ERR_OUT_OF_RANGE, $ERR_MISSING_ARGS, $ERR_METHOD_NOT_IMPLEMENTED, $ERR_ILLEGAL_CONSTRUCTOR, $ERR_MULTIPLE_CALLBACK, $ERR_UNKNOWN_ENCODING, $ERR_STREAM_DESTROYED, $ERR_STREAM_ALREADY_FINISHED, $ERR_STREAM_WRITE_AFTER_END, $ERR_STREAM_NULL_VALUES, $ERR_STREAM_PREMATURE_CLOSE, $ERR_STREAM_PUSH_AFTER_EOF, $ERR_STREAM_UNSHIFT_AFTER_END_EVENT, $ERR_STREAM_CANNOT_PIPE, $ERR_STREAM_UNABLE_TO_PIPE, $ERR_STREAM_ITER_MISSING_FLAG, $makeAbortError, $toClass, __isCallable, __debug, __assert, $inheritsReadableStream, $inheritsWritableStream, $inheritsTransformStream, $inheritsBlob, __hasAsyncContext, $webStreamClosedPromise, $cpp } = R.H;

  R.def("internal/streams/pipeline", function (require, module, exports) {
    const eos = require("internal/streams/end-of-stream");
    const { once } = require("internal/shared");
    const destroyImpl = require("internal/streams/destroy");
    const Duplex = require("internal/streams/duplex");
    const { aggregateTwoErrors } = require("internal/errors");
    const { validateFunction, validateAbortSignal } = require("internal/validators");
    const {
      isIterable,
      isReadable,
      isReadableNodeStream,
      isNodeStream,
      isTransformStream,
      isWebStream,
      isReadableStream,
      isReadableFinished
    } = require("internal/streams/utils");
    const SymbolAsyncIterator = Symbol.asyncIterator;
    const ArrayIsArray = Array.isArray;
    const SymbolDispose = Symbol.dispose;
    let PassThrough;
    let Readable;
    let addAbortListener;
    function destroyer(stream, reading, writing) {
      let finished = false;
      stream.on("close", () => {
        finished = true;
      });
      const cleanup = eos(stream, { readable: reading, writable: writing }, (err) => {
        finished = !err;
      });
      return {
        destroy: (err) => {
          if (finished)
            return;
          finished = true;
          destroyImpl.destroyer(stream, err || $ERR_STREAM_DESTROYED("pipe"));
        },
        cleanup
      };
    }
    function popCallback(streams) {
      validateFunction(streams[streams.length - 1], "streams[stream.length - 1]");
      return streams.pop();
    }
    function makeAsyncIterable(val) {
      if (isIterable(val)) {
        return val;
      } else if (isReadableNodeStream(val)) {
        return fromReadable(val);
      }
      throw $ERR_INVALID_ARG_TYPE("val", ["Readable", "Iterable", "AsyncIterable"], val);
    }
    async function* fromReadable(val) {
      Readable ??= require("internal/streams/readable");
      yield* Readable.prototype[SymbolAsyncIterator].call(val);
    }
    async function pumpToNode(iterable, writable, finish, { end }) {
      let error;
      let onresolve = null;
      const resume = (err) => {
        if (err) {
          error = err;
        }
        if (onresolve) {
          const callback = onresolve;
          onresolve = null;
          callback();
        }
      };
      const wait = () => new Promise((resolve, reject) => {
        if (error) {
          reject(error);
        } else {
          onresolve = () => {
            if (error) {
              reject(error);
            } else {
              resolve();
            }
          };
        }
      });
      writable.on("drain", resume);
      const cleanup = eos(writable, { readable: false }, resume);
      try {
        if (writable.writableNeedDrain) {
          await wait();
        }
        for await (const chunk of iterable) {
          if (!writable.write(chunk)) {
            await wait();
          }
        }
        if (end) {
          writable.end();
          await wait();
        }
        finish();
      } catch (err) {
        finish(error !== err ? aggregateTwoErrors(error, err) : err);
      } finally {
        cleanup();
        writable.off("drain", resume);
      }
    }
    async function pumpToWeb(readable, writable, finish, { end }) {
      if (isTransformStream(writable)) {
        writable = writable.writable;
      }
      const writer = writable.getWriter();
      try {
        for await (const chunk of readable) {
          await writer.ready;
          writer.write(chunk).catch(() => {});
        }
        await writer.ready;
        if (end) {
          await writer.close();
        }
        finish();
      } catch (err) {
        try {
          await writer.abort(err);
          finish(err);
        } catch (err) {
          finish(err);
        }
      }
    }
    function pipeline(...streams) {
      return pipelineImpl(streams, once(popCallback(streams)));
    }
    function pipelineImpl(streams, callback, opts) {
      if (streams.length === 1 && ArrayIsArray(streams[0])) {
        streams = streams[0];
      }
      if (streams.length < 2) {
        throw $ERR_MISSING_ARGS("streams");
      }
      const ac = new AbortController;
      const signal = ac.signal;
      const outerSignal = opts?.signal;
      const lastStreamCleanup = [];
      validateAbortSignal(outerSignal, "options.signal");
      function abort() {
        finishImpl($makeAbortError(undefined, { cause: outerSignal?.reason }));
      }
      addAbortListener ??= require("internal/abort_listener").addAbortListener;
      let disposable;
      if (outerSignal) {
        disposable = addAbortListener(outerSignal, abort);
      }
      let error;
      let value;
      const destroys = [];
      let finishCount = 0;
      function finish(err) {
        finishImpl(err, --finishCount === 0);
      }
      function finishOnlyHandleError(err) {
        finishImpl(err, false);
      }
      function finishImpl(err, final) {
        if (err && (!error || error.code === "ERR_STREAM_PREMATURE_CLOSE" || error.name === "AbortError")) {
          error = err;
        }
        if (!error && !final) {
          return;
        }
        while (destroys.length) {
          destroys.shift()?.(error);
        }
        disposable?.[SymbolDispose]();
        ac.abort();
        if (final) {
          if (!error) {
            lastStreamCleanup.forEach((fn) => fn());
          }
          process.nextTick(callback, error, value);
        }
      }
      let ret;
      for (let i = 0;i < streams.length; i++) {
        const stream = streams[i];
        const reading = i < streams.length - 1;
        const writing = i > 0;
        const next = i + 1 < streams.length ? streams[i + 1] : null;
        const end = reading || opts?.end !== false;
        const isLastStream = i === streams.length - 1;
        if (isNodeStream(stream)) {
          let onError = function(err) {
            if (err && err.name !== "AbortError" && err.code !== "ERR_STREAM_PREMATURE_CLOSE") {
              finishOnlyHandleError(err);
            }
          };
          if (next !== null && (next?.closed || next?.destroyed)) {
            throw $ERR_STREAM_UNABLE_TO_PIPE();
          }
          if (end) {
            const { destroy, cleanup } = destroyer(stream, reading, writing);
            destroys.push(destroy);
            if (isReadable(stream) && isLastStream) {
              lastStreamCleanup.push(cleanup);
            }
          }
          stream.on("error", onError);
          if (isReadable(stream) && isLastStream) {
            lastStreamCleanup.push(() => {
              stream.removeListener("error", onError);
            });
          }
        }
        if (i === 0) {
          if (typeof stream === "function") {
            ret = stream({ signal });
            if (!isIterable(ret)) {
              throw $ERR_INVALID_RETURN_VALUE("Iterable, AsyncIterable or Stream", "source", ret);
            }
          } else if (isIterable(stream) || isReadableNodeStream(stream) || isTransformStream(stream)) {
            ret = stream;
          } else {
            ret = Duplex.from(stream);
          }
        } else if (typeof stream === "function") {
          if (isTransformStream(ret)) {
            ret = makeAsyncIterable(ret?.readable);
          } else {
            ret = makeAsyncIterable(ret);
          }
          ret = stream(ret, { signal });
          if (reading) {
            if (!isIterable(ret, true)) {
              throw $ERR_INVALID_RETURN_VALUE("AsyncIterable", `transform[${i - 1}]`, ret);
            }
          } else {
            PassThrough ??= require("internal/streams/passthrough");
            const pt = new PassThrough({
              objectMode: true
            });
            const then = ret?.then;
            if (typeof then === "function") {
              finishCount++;
              then.call(ret, (val) => {
                value = val;
                if (val != null) {
                  pt.write(val);
                }
                if (end) {
                  pt.end();
                }
                process.nextTick(finish);
              }, (err) => {
                pt.destroy(err);
                process.nextTick(finish, err);
              });
            } else if (isIterable(ret, true)) {
              finishCount++;
              pumpToNode(ret, pt, finish, { end });
            } else if (isReadableStream(ret) || isTransformStream(ret)) {
              const toRead = ret.readable || ret;
              finishCount++;
              pumpToNode(toRead, pt, finish, { end });
            } else {
              throw $ERR_INVALID_RETURN_VALUE("AsyncIterable or Promise", "destination", ret);
            }
            ret = pt;
            const { destroy, cleanup } = destroyer(ret, false, true);
            destroys.push(destroy);
            if (isLastStream) {
              lastStreamCleanup.push(cleanup);
            }
          }
        } else if (isNodeStream(stream)) {
          if (isReadableNodeStream(ret)) {
            finishCount += 2;
            const cleanup = pipe(ret, stream, finish, finishOnlyHandleError, { end });
            if (isReadable(stream) && isLastStream) {
              lastStreamCleanup.push(cleanup);
            }
          } else if (isTransformStream(ret) || isReadableStream(ret)) {
            const toRead = ret.readable || ret;
            finishCount++;
            pumpToNode(toRead, stream, finish, { end });
          } else if (isIterable(ret)) {
            finishCount++;
            pumpToNode(ret, stream, finish, { end });
          } else {
            throw $ERR_INVALID_ARG_TYPE("val", ["Readable", "Iterable", "AsyncIterable", "ReadableStream", "TransformStream"], ret);
          }
          ret = stream;
        } else if (isWebStream(stream)) {
          if (isReadableNodeStream(ret)) {
            finishCount++;
            pumpToWeb(makeAsyncIterable(ret), stream, finish, { end });
          } else if (isReadableStream(ret) || isIterable(ret)) {
            finishCount++;
            pumpToWeb(ret, stream, finish, { end });
          } else if (isTransformStream(ret)) {
            finishCount++;
            pumpToWeb(ret.readable, stream, finish, { end });
          } else {
            throw $ERR_INVALID_ARG_TYPE("val", ["Readable", "Iterable", "AsyncIterable", "ReadableStream", "TransformStream"], ret);
          }
          ret = stream;
        } else {
          ret = Duplex.from(stream);
        }
      }
      if (signal?.aborted || outerSignal?.aborted) {
        process.nextTick(abort);
      }
      return ret;
    }
    function pipe(src, dst, finish, finishOnlyHandleError, { end }) {
      let ended = false;
      dst.on("close", () => {
        if (!ended) {
          finishOnlyHandleError($ERR_STREAM_PREMATURE_CLOSE());
        }
      });
      src.pipe(dst, { end: false });
      if (end) {
        let endFn = function() {
          ended = true;
          dst.end();
        };
        if (isReadableFinished(src)) {
          process.nextTick(endFn);
        } else {
          src.once("end", endFn);
        }
      } else {
        finish();
      }
      eos(src, { readable: true, writable: false }, (err) => {
        const rState = src._readableState;
        if (err && err.code === "ERR_STREAM_PREMATURE_CLOSE" && rState?.ended && !rState.errored && !rState.errorEmitted) {
          src.once("end", finish).once("error", finish);
        } else {
          finish(err);
        }
      });
      return eos(dst, { readable: false, writable: true }, finish);
    }
    module.exports = { pipelineImpl, pipeline };

  });

  R.def("internal/streams/operators", function (require, module, exports) {
    const { validateAbortSignal, validateFunction, validateInteger, validateObject } = require("internal/validators");
    const { kWeakHandler, kResistStopPropagation } = require("internal/shared");
    const { finished } = require("internal/streams/end-of-stream");
    const MathFloor = Math.floor;
    const PromiseResolve = Promise.resolve.bind(Promise);
    const PromiseReject = Promise.reject.bind(Promise);
    const PromisePrototypeThen = Promise.prototype.then;
    const ArrayPrototypePush = Array.prototype.push;
    const NumberIsNaN = Number.isNaN;
    const ObjectDefineProperty = Object.defineProperty;
    const kEmpty = Symbol("kEmpty");
    const kEof = Symbol("kEof");
    function map(fn, options) {
      validateFunction(fn, "fn");
      if (options != null) {
        validateObject(options, "options");
      }
      if (options?.signal != null) {
        validateAbortSignal(options.signal, "options.signal");
      }
      let concurrency = 1;
      if (options?.concurrency != null) {
        concurrency = MathFloor(options.concurrency);
      }
      let highWaterMark = concurrency - 1;
      if (options?.highWaterMark != null) {
        highWaterMark = MathFloor(options.highWaterMark);
      }
      validateInteger(concurrency, "options.concurrency", 1);
      validateInteger(highWaterMark, "options.highWaterMark", 0);
      highWaterMark += concurrency;
      return async function* map() {
        const signal = AbortSignal.any([options?.signal].filter(Boolean));
        const stream = this;
        const queue = [];
        const signalOpt = { signal };
        let next;
        let resume;
        let done = false;
        let cnt = 0;
        function onCatch() {
          done = true;
          afterItemProcessed();
        }
        function afterItemProcessed() {
          cnt -= 1;
          maybeResume();
        }
        function maybeResume() {
          if (resume && !done && cnt < concurrency && queue.length < highWaterMark) {
            resume();
            resume = null;
          }
        }
        async function pump() {
          try {
            for await (let val of stream) {
              if (done) {
                return;
              }
              if (signal.aborted) {
                throw $makeAbortError();
              }
              try {
                val = fn(val, signalOpt);
                if (val === kEmpty) {
                  continue;
                }
                val = PromiseResolve(val);
              } catch (err) {
                val = PromiseReject(err);
              }
              cnt += 1;
              PromisePrototypeThen.call(val, afterItemProcessed, onCatch);
              queue.push(val);
              if (next) {
                next();
                next = null;
              }
              if (!done && (queue.length >= highWaterMark || cnt >= concurrency)) {
                await new Promise((resolve) => {
                  resume = resolve;
                });
              }
            }
            queue.push(kEof);
          } catch (err) {
            const val = PromiseReject(err);
            PromisePrototypeThen.call(val, afterItemProcessed, onCatch);
            queue.push(val);
          } finally {
            done = true;
            if (next) {
              next();
              next = null;
            }
          }
        }
        pump();
        try {
          while (true) {
            while (queue.length > 0) {
              const val = await queue[0];
              if (val === kEof) {
                return;
              }
              if (signal.aborted) {
                throw $makeAbortError();
              }
              if (val !== kEmpty) {
                yield val;
              }
              queue.shift();
              maybeResume();
            }
            await new Promise((resolve) => {
              next = resolve;
            });
          }
        } finally {
          done = true;
          if (resume) {
            resume();
            resume = null;
          }
        }
      }.call(this);
    }
    async function some(fn, options = undefined) {
      for await (const unused of filter.call(this, fn, options)) {
        return true;
      }
      return false;
    }
    async function every(fn, options = undefined) {
      validateFunction(fn, "fn");
      return !await some.call(this, async (...args) => {
        return !await fn(...args);
      }, options);
    }
    async function find(fn, options) {
      for await (const result of filter.call(this, fn, options)) {
        return result;
      }
      return;
    }
    async function forEach(fn, options) {
      validateFunction(fn, "fn");
      async function forEachFn(value, options) {
        await fn(value, options);
        return kEmpty;
      }
      for await (const unused of map.call(this, forEachFn, options))
        ;
    }
    function filter(fn, options) {
      validateFunction(fn, "fn");
      async function filterFn(value, options) {
        if (await fn(value, options)) {
          return value;
        }
        return kEmpty;
      }
      return map.call(this, filterFn, options);
    }

    class ReduceAwareErrMissingArgs extends TypeError {
      constructor() {
        super("reduce");
        this.code = "ERR_MISSING_ARGS";
        this.message = "Reduce of an empty stream requires an initial value";
      }
    }
    async function reduce(reducer, initialValue, options) {
      validateFunction(reducer, "reducer");
      if (options != null) {
        validateObject(options, "options");
      }
      if (options?.signal != null) {
        validateAbortSignal(options.signal, "options.signal");
      }
      let hasInitialValue = arguments.length > 1;
      if (options?.signal?.aborted) {
        const err = $makeAbortError(undefined, { cause: options.signal.reason });
        this.once("error", () => {});
        await finished(this.destroy(err));
        throw err;
      }
      const ac = new AbortController;
      const signal = ac.signal;
      if (options?.signal) {
        const opts = { once: true, [kWeakHandler]: this, [kResistStopPropagation]: true };
        options.signal.addEventListener("abort", () => ac.abort(), opts);
      }
      let gotAnyItemFromStream = false;
      try {
        for await (const value of this) {
          gotAnyItemFromStream = true;
          if (options?.signal?.aborted) {
            throw $makeAbortError();
          }
          if (!hasInitialValue) {
            initialValue = value;
            hasInitialValue = true;
          } else {
            initialValue = await reducer(initialValue, value, { signal });
          }
        }
        if (!gotAnyItemFromStream && !hasInitialValue) {
          throw new ReduceAwareErrMissingArgs;
        }
      } finally {
        ac.abort();
      }
      return initialValue;
    }
    async function toArray(options) {
      if (options != null) {
        validateObject(options, "options");
      }
      if (options?.signal != null) {
        validateAbortSignal(options.signal, "options.signal");
      }
      const result = [];
      for await (const val of this) {
        if (options?.signal?.aborted) {
          throw $makeAbortError(undefined, { cause: options.signal.reason });
        }
        ArrayPrototypePush.call(result, val);
      }
      return result;
    }
    function flatMap(fn, options) {
      const values = map.call(this, fn, options);
      async function* flatMapInner() {
        for await (const val of values) {
          yield* val;
        }
      }
      return flatMapInner.call(this);
    }
    function toIntegerOrInfinity(number) {
      number = Number(number);
      if (NumberIsNaN(number)) {
        return 0;
      }
      if (number < 0) {
        throw $ERR_OUT_OF_RANGE("number", ">= 0", number);
      }
      return number;
    }
    function drop(number, options) {
      if (options != null) {
        validateObject(options, "options");
      }
      if (options?.signal != null) {
        validateAbortSignal(options.signal, "options.signal");
      }
      number = toIntegerOrInfinity(number);
      return async function* drop() {
        if (options?.signal?.aborted) {
          throw $makeAbortError();
        }
        for await (const val of this) {
          if (options?.signal?.aborted) {
            throw $makeAbortError();
          }
          if (number-- <= 0) {
            yield val;
          }
        }
      }.call(this);
    }
    ObjectDefineProperty(drop, "length", { value: 1 });
    function take(number, options) {
      if (options != null) {
        validateObject(options, "options");
      }
      if (options?.signal != null) {
        validateAbortSignal(options.signal, "options.signal");
      }
      number = toIntegerOrInfinity(number);
      return async function* take() {
        if (options?.signal?.aborted) {
          throw $makeAbortError();
        }
        for await (const val of this) {
          if (options?.signal?.aborted) {
            throw $makeAbortError();
          }
          if (number-- > 0) {
            yield val;
          }
          if (number <= 0) {
            return;
          }
        }
      }.call(this);
    }
    ObjectDefineProperty(take, "length", { value: 1 });
    module.exports = {
      streamReturningOperators: {
        drop,
        filter,
        flatMap,
        map,
        take
      },
      promiseReturningOperators: {
        every,
        forEach,
        reduce,
        toArray,
        some,
        find
      }
    };

  });

  R.def("internal/streams/compose", function (require, module, exports) {
    const { pipeline } = require("internal/streams/pipeline");
    const Duplex = require("internal/streams/duplex");
    const { destroyer } = require("internal/streams/destroy");
    const {
      isNodeStream,
      isReadable,
      isWritable,
      isWebStream,
      isTransformStream,
      isWritableStream,
      isReadableStream
    } = require("internal/streams/utils");
    const eos = require("internal/streams/end-of-stream");
    const ArrayPrototypeSlice = Array.prototype.slice;
    module.exports = function compose(...streams) {
      if (streams.length === 0) {
        throw $ERR_MISSING_ARGS("streams");
      }
      if (streams.length === 1) {
        return Duplex.from(streams[0]);
      }
      const orgStreams = ArrayPrototypeSlice.call(streams);
      if (typeof streams[0] === "function") {
        streams[0] = Duplex.from(streams[0]);
      }
      const lastIdx = streams.length - 1;
      if (typeof streams[lastIdx] === "function") {
        streams[lastIdx] = Duplex.from(streams[lastIdx]);
      }
      for (let n = 0;n < streams.length; ++n) {
        if (!isNodeStream(streams[n]) && !isWebStream(streams[n])) {
          continue;
        }
        if (n < streams.length - 1 && !(isReadable(streams[n]) || isReadableStream(streams[n]) || isTransformStream(streams[n]))) {
          throw $ERR_INVALID_ARG_VALUE(`streams[${n}]`, orgStreams[n], "must be readable");
        }
        if (n > 0 && !(isWritable(streams[n]) || isWritableStream(streams[n]) || isTransformStream(streams[n]))) {
          throw $ERR_INVALID_ARG_VALUE(`streams[${n}]`, orgStreams[n], "must be writable");
        }
      }
      let ondrain;
      let onfinish;
      let onreadable;
      let onclose;
      let d;
      function onfinished(err) {
        const cb = onclose;
        onclose = null;
        if (cb) {
          cb(err);
        } else if (err) {
          d.destroy(err);
        } else if (!readable && !writable) {
          d.destroy();
        }
      }
      const head = streams[0];
      const tail = pipeline(streams, onfinished);
      const writable = !!(isWritable(head) || isWritableStream(head) || isTransformStream(head));
      const readable = !!(isReadable(tail) || isReadableStream(tail) || isTransformStream(tail));
      d = new Duplex({
        writableObjectMode: !!head?.writableObjectMode,
        readableObjectMode: !!tail?.readableObjectMode,
        writable,
        readable
      });
      if (writable) {
        if (isNodeStream(head)) {
          d._write = function(chunk, encoding, callback) {
            if (head.write(chunk, encoding)) {
              callback();
            } else {
              ondrain = callback;
            }
          };
          d._final = function(callback) {
            head.end();
            onfinish = callback;
          };
          head.on("drain", function() {
            if (ondrain) {
              const cb = ondrain;
              ondrain = null;
              cb();
            }
          });
        } else if (isWebStream(head)) {
          const writable = isTransformStream(head) ? head.writable : head;
          const writer = writable.getWriter();
          d._write = async function(chunk, encoding, callback) {
            try {
              await writer.ready;
              writer.write(chunk).catch(() => {});
              callback();
            } catch (err) {
              callback(err);
            }
          };
          d._final = async function(callback) {
            try {
              await writer.ready;
              writer.close().catch(() => {});
              onfinish = callback;
            } catch (err) {
              callback(err);
            }
          };
        }
        const toRead = isTransformStream(tail) ? tail.readable : tail;
        eos(toRead, () => {
          if (onfinish) {
            const cb = onfinish;
            onfinish = null;
            cb();
          }
        });
      }
      if (readable) {
        if (isNodeStream(tail)) {
          tail.on("readable", function() {
            if (onreadable) {
              const cb = onreadable;
              onreadable = null;
              cb();
            }
          });
          tail.on("end", function() {
            d.push(null);
          });
          d._read = function() {
            while (true) {
              const buf = tail.read();
              if (buf === null) {
                onreadable = d._read;
                return;
              }
              if (!d.push(buf)) {
                return;
              }
            }
          };
        } else if (isWebStream(tail)) {
          const readable = isTransformStream(tail) ? tail.readable : tail;
          const reader = readable.getReader();
          d._read = async function() {
            while (true) {
              try {
                const { value, done } = await reader.read();
                if (!d.push(value)) {
                  return;
                }
                if (done) {
                  d.push(null);
                  return;
                }
              } catch {
                return;
              }
            }
          };
        }
      }
      d._destroy = function(err, callback) {
        if (!err && onclose !== null) {
          err = $makeAbortError();
        }
        onreadable = null;
        ondrain = null;
        onfinish = null;
        if (isNodeStream(tail)) {
          destroyer(tail, err);
        }
        if (onclose === null) {
          callback(err);
        } else {
          onclose = callback;
        }
      };
      return d;
    };

  });

  R.def("internal/streams/duplexpair", function (require, module, exports) {
    const Duplex = require("internal/streams/duplex");
    const kCallback = Symbol("Callback");
    const kInitOtherSide = Symbol("InitOtherSide");

    class DuplexSide extends Duplex {
      #otherSide = null;
      [kCallback] = null;
      constructor(options) {
        super(options);
        this.#otherSide = null;
      }
      [kInitOtherSide](otherSide) {
        if (this.#otherSide === null) {
          this.#otherSide = otherSide;
        } else {
          __assert(this.#otherSide === null);
        }
      }
      _read() {
        const callback = this[kCallback];
        if (callback) {
          this[kCallback] = null;
          callback();
        }
      }
      _write(chunk, encoding, callback) {
        __assert(this.#otherSide !== null);
        __assert(this.#otherSide[kCallback] === null);
        if (chunk.length === 0) {
          process.nextTick(callback);
        } else {
          this.#otherSide.push(chunk);
          this.#otherSide[kCallback] = callback;
        }
      }
      _final(callback) {
        this.#otherSide.on("end", callback);
        this.#otherSide.push(null);
      }
    }
    function duplexPair(options) {
      const side0 = new DuplexSide(options);
      const side1 = new DuplexSide(options);
      side0[kInitOtherSide](side1);
      side1[kInitOtherSide](side0);
      return [side0, side1];
    }
    module.exports = duplexPair;

  });

  R.def("internal/stream.promises", function (require, module, exports) {
    const ArrayPrototypePop = Array.prototype.pop;
    const { isIterable, isNodeStream, isWebStream } = require("internal/streams/utils");
    const { pipelineImpl: pl } = require("internal/streams/pipeline");
    const { finished } = require("internal/streams/end-of-stream");
    function pipeline(...streams) {
      return new Promise((resolve, reject) => {
        let signal;
        let end;
        const lastArg = streams[streams.length - 1];
        if (lastArg && typeof lastArg === "object" && !isNodeStream(lastArg) && !isIterable(lastArg) && !isWebStream(lastArg)) {
          const options = ArrayPrototypePop.call(streams);
          signal = options.signal;
          end = options.end;
        }
        pl(streams, (err, value) => {
          if (err) {
            reject(err);
          } else {
            resolve(value);
          }
        }, { signal, end });
      });
    }
    module.exports = {
      finished,
      pipeline
    };

  });

  R.def("internal/stream.consumers", function (require, module, exports) {
    const JSONParse = JSON.parse;
    // A second consumer on an already-locked ReadableStream must reject with
    // ERR_INVALID_STATE. Bun.readableStreamTo* throws a bare TypeError with no
    // `code`, so guard before delegating.
    const assertUnlocked = (stream) => {
      if ($inheritsReadableStream(stream) && stream.locked === true) {
        const e = new TypeError("Invalid state: The ReadableStream is locked");
        e.code = "ERR_INVALID_STATE";
        throw e;
      }
    };
    async function blob(stream) {
      assertUnlocked(stream);
      if ($inheritsReadableStream(stream))
        return Bun.readableStreamToBlob(stream);
      const chunks = [];
      for await (const chunk of stream)
        chunks.push(chunk);
      return new Blob(chunks);
    }
    async function arrayBuffer(stream) {
      assertUnlocked(stream);
      if ($inheritsReadableStream(stream))
        return Bun.readableStreamToArrayBuffer(stream);
      const ret = await blob(stream);
      return ret.arrayBuffer();
    }
    async function bytes(stream) {
      assertUnlocked(stream);
      if ($inheritsReadableStream(stream))
        return Bun.readableStreamToBytes(stream);
      const ret = await blob(stream);
      return ret.bytes();
    }
    async function buffer(stream) {
      return Buffer.from(await arrayBuffer(stream));
    }
    async function text(stream) {
      assertUnlocked(stream);
      if ($inheritsReadableStream(stream))
        return Bun.readableStreamToText(stream);
      const dec = new TextDecoder;
      let str = "";
      for await (const chunk of stream) {
        if (typeof chunk === "string")
          str += chunk;
        else {
          // node's text()/json() validate every chunk, unlike blob()/bytes(),
          // which stringify through Blob (an object-mode stream legitimately
          // yields '[object Object]' there). Decoding a non-BufferSource here
          // would otherwise coerce silently.
          if (chunk === null || typeof chunk !== "object" || !ArrayBuffer.isView(chunk)) {
            throw $ERR_INVALID_ARG_TYPE("chunk", ["string", "Buffer", "TypedArray", "DataView"], chunk);
          }
          str += dec.decode(chunk, { stream: true });
        }
      }
      str += dec.decode(undefined, { stream: false });
      return str;
    }
    async function json(stream) {
      assertUnlocked(stream);
      if ($inheritsReadableStream(stream))
        return Bun.readableStreamToJSON(stream);
      const str = await text(stream);
      return JSONParse(str);
    }
    module.exports = {
      arrayBuffer,
      bytes,
      text,
      json,
      buffer,
      blob
    };

  });

  R.def("internal/stream", function (require, module, exports) {
    const ObjectKeys = Object.keys;
    const ObjectDefineProperty = Object.defineProperty;
    const customPromisify = Symbol.for("nodejs.util.promisify.custom");
    const { streamReturningOperators, promiseReturningOperators } = require("internal/streams/operators");
    const compose = require("internal/streams/compose");
    const { setDefaultHighWaterMark, getDefaultHighWaterMark } = require("internal/streams/state");
    const { pipeline } = require("internal/streams/pipeline");
    const { destroyer } = require("internal/streams/destroy");
    const eos = require("internal/streams/end-of-stream");
    const promises = require("internal/stream.promises");
    const utils = require("internal/streams/utils");
    const { isArrayBufferView, isUint8Array } = require("node:util/types");
    const Stream = require("internal/streams/legacy").Stream;
    Stream.isDestroyed = utils.isDestroyed;
    Stream.isDisturbed = utils.isDisturbed;
    Stream.isErrored = utils.isErrored;
    Stream.isReadable = utils.isReadable;
    Stream.isWritable = utils.isWritable;
    Stream.Readable = require("internal/streams/readable");
    const streamKeys = ObjectKeys(streamReturningOperators);
    for (let i = 0;i < streamKeys.length; i++) {
      let fn = function(...args) {
        if (new.target) {
          throw $ERR_ILLEGAL_CONSTRUCTOR();
        }
        return Stream.Readable.from(op.apply(this, args));
      };
      const key = streamKeys[i];
      const op = streamReturningOperators[key];
      ObjectDefineProperty(fn, "name", { __proto__: null, value: op.name });
      ObjectDefineProperty(fn, "length", { __proto__: null, value: op.length });
      ObjectDefineProperty(Stream.Readable.prototype, key, {
        __proto__: null,
        value: fn,
        enumerable: false,
        configurable: true,
        writable: true
      });
    }
    const promiseKeys = ObjectKeys(promiseReturningOperators);
    for (let i = 0;i < promiseKeys.length; i++) {
      let fn = function(...args) {
        if (new.target) {
          throw $ERR_ILLEGAL_CONSTRUCTOR();
        }
        return Promise.resolve().then(() => op.apply(this, args));
      };
      const key = promiseKeys[i];
      const op = promiseReturningOperators[key];
      ObjectDefineProperty(fn, "name", { __proto__: null, value: op.name });
      ObjectDefineProperty(fn, "length", { __proto__: null, value: op.length });
      ObjectDefineProperty(Stream.Readable.prototype, key, {
        __proto__: null,
        value: fn,
        enumerable: false,
        configurable: true,
        writable: true
      });
    }
    Stream.Writable = require("internal/streams/writable");
    Stream.Duplex = require("internal/streams/duplex");
    Stream.Transform = require("internal/streams/transform");
    Stream.PassThrough = require("internal/streams/passthrough");
    Stream.duplexPair = require("internal/streams/duplexpair");
    Stream.pipeline = pipeline;
    const { addAbortSignal } = require("internal/streams/add-abort-signal");
    Stream.addAbortSignal = addAbortSignal;
    Stream.finished = eos;
    Stream.destroy = destroyer;
    Stream.compose = compose;
    Stream.setDefaultHighWaterMark = setDefaultHighWaterMark;
    Stream.getDefaultHighWaterMark = getDefaultHighWaterMark;
    ObjectDefineProperty(Stream, "promises", {
      __proto__: null,
      configurable: true,
      enumerable: true,
      get() {
        return promises;
      }
    });
    ObjectDefineProperty(pipeline, customPromisify, {
      __proto__: null,
      enumerable: true,
      get() {
        return promises.pipeline;
      }
    });
    ObjectDefineProperty(eos, customPromisify, {
      __proto__: null,
      enumerable: true,
      get() {
        return promises.finished;
      }
    });
    Stream.Stream = Stream;
    Stream._isArrayBufferView = isArrayBufferView;
    Stream._isUint8Array = isUint8Array;
    Stream._uint8ArrayToBuffer = function _uint8ArrayToBuffer(chunk) {
      return new Buffer(chunk.buffer, chunk.byteOffset, chunk.byteLength);
    };
    module.exports = Stream;

  });

  // ---- publish node:stream & friends over the bootstrap stubs ----
  const M = G.__mbunNativeModules;
  const def = (names, mod) => { for (const n of names) { M[n] = mod; M["node:" + n] = mod; } };
  const Stream = R.require("internal/stream");
  // bun-ref src/js/node/stream.ts:6 — node:stream is internal/stream plus .eos.
  Stream.eos = R.require("internal/streams/end-of-stream");
  def(["stream"], Stream);
  // bun-ref src/js/node/_stream_*.ts each re-export the single class, not Stream.
  // NB: "readable-stream" is deliberately NOT registered — it is an npm package,
  // not a bun builtin (bun 1.3.14: require("readable-stream") => MODULE_NOT_FOUND),
  // and shadowing it here would hide the copy a project actually installed.
  def(["_stream_readable"], R.require("internal/streams/readable"));
  def(["_stream_writable"], R.require("internal/streams/writable"));
  def(["_stream_duplex"], R.require("internal/streams/duplex"));
  def(["_stream_transform"], R.require("internal/streams/transform"));
  def(["_stream_passthrough"], R.require("internal/streams/passthrough"));
  def(["stream/promises"], Stream.promises);
  def(["stream/consumers"], R.require("internal/stream.consumers"));

  // crypto's Hash/Hmac (builtins/markdown_web.cppm) are defined inside the
  // master builtins IIFE, where `Transform` lexically resolves to the bootstrap
  // load-order stub, so their prototype chain was linked before node:stream
  // existed. Re-link onto the real Transform now that it does: node's LazyHash
  // quirk requires `createHash("sha256") instanceof Transform` (bun-ref
  // test/js/node/crypto/crypto-lazyhash.test.ts). markdown_web resolves the
  // constructor lazily (streamTransform()), so instances are real Transforms.
  const C = M["crypto"] || M["node:crypto"];
  if (C) {
    for (const k of ["Hash", "Hmac"]) {
      const F = C[k];
      if (typeof F === "function" && Object.getPrototypeOf(F.prototype) !== Stream.Transform.prototype) {
        Object.setPrototypeOf(F.prototype, Stream.Transform.prototype);
        Object.setPrototypeOf(F, Stream.Transform);
      }
    }
  }
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
