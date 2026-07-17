// node:stream payload partition — internal/streams/{writable,duplex,duplexify,transform,passthrough}.
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
export module mbun.jsc.js_builtins:node_stream_writable;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeStreamWritableJS = R"JS(
(function () {
  const G = globalThis;
  const R = G.__mbunStreamReg;
  if (!R) return;
  const process = G.process;
  const Buffer = G.Buffer;
  const { $ERR_INVALID_ARG_TYPE, $ERR_INVALID_ARG_VALUE, $ERR_INVALID_RETURN_VALUE, $ERR_OUT_OF_RANGE, $ERR_MISSING_ARGS, $ERR_METHOD_NOT_IMPLEMENTED, $ERR_ILLEGAL_CONSTRUCTOR, $ERR_MULTIPLE_CALLBACK, $ERR_UNKNOWN_ENCODING, $ERR_STREAM_DESTROYED, $ERR_STREAM_ALREADY_FINISHED, $ERR_STREAM_WRITE_AFTER_END, $ERR_STREAM_NULL_VALUES, $ERR_STREAM_PREMATURE_CLOSE, $ERR_STREAM_PUSH_AFTER_EOF, $ERR_STREAM_UNSHIFT_AFTER_END_EVENT, $ERR_STREAM_CANNOT_PIPE, $ERR_STREAM_UNABLE_TO_PIPE, $ERR_STREAM_ITER_MISSING_FLAG, $makeAbortError, $toClass, __isCallable, __debug, __assert, $inheritsReadableStream, $inheritsWritableStream, $inheritsTransformStream, $inheritsBlob, __hasAsyncContext, $webStreamClosedPromise, $cpp } = R.H;

  R.def("internal/streams/writable", function (require, module, exports) {
    const EE = require("node:events");
    const { Stream } = require("internal/streams/legacy");
    const destroyImpl = require("internal/streams/destroy");
    const eos = require("internal/streams/end-of-stream");
    const { addAbortSignal } = require("internal/streams/add-abort-signal");
    const { getHighWaterMark, getDefaultHighWaterMark } = require("internal/streams/state");
    const {
      kOnConstructed,
      kState,
      kObjectMode,
      kErrorEmitted,
      kAutoDestroy,
      kEmitClose,
      kDestroyed,
      kClosed,
      kCloseEmitted,
      kErrored,
      kConstructed
    } = require("internal/streams/utils");
    const ObjectDefineProperties = Object.defineProperties;
    const ArrayPrototypeSlice = Array.prototype.slice;
    const ObjectDefineProperty = Object.defineProperty;
    const SymbolHasInstance = Symbol.hasInstance;
    const FunctionPrototypeSymbolHasInstance = Function.prototype[Symbol.hasInstance];
    const StringPrototypeToLowerCase = String.prototype.toLowerCase;
    const SymbolAsyncDispose = Symbol.asyncDispose;
    const { errorOrDestroy } = destroyImpl;
    function nop() {}
    const kOnFinishedValue = Symbol("kOnFinishedValue");
    const kErroredValue = Symbol("kErroredValue");
    const kDefaultEncodingValue = Symbol("kDefaultEncodingValue");
    const kWriteCbValue = Symbol("kWriteCbValue");
    const kAfterWriteTickInfoValue = Symbol("kAfterWriteTickInfoValue");
    const kBufferedValue = Symbol("kBufferedValue");
    const kSync = 1 << 9;
    const kFinalCalled = 1 << 10;
    const kNeedDrain = 1 << 11;
    const kEnding = 1 << 12;
    const kFinished = 1 << 13;
    const kDecodeStrings = 1 << 14;
    const kWriting = 1 << 15;
    const kBufferProcessing = 1 << 16;
    const kPrefinished = 1 << 17;
    const kAllBuffers = 1 << 18;
    const kAllNoop = 1 << 19;
    const kOnFinished = 1 << 20;
    const kHasWritable = 1 << 21;
    const kWritable = 1 << 22;
    const kCorked = 1 << 23;
    const kDefaultUTF8Encoding = 1 << 24;
    const kWriteCb = 1 << 25;
    const kExpectWriteCb = 1 << 26;
    const kAfterWriteTickInfo = 1 << 27;
    const kAfterWritePending = 1 << 28;
    const kBuffered = 1 << 29;
    const kEnded = 1 << 30;
    function makeBitMapDescriptor(bit) {
      return {
        enumerable: false,
        get() {
          return (this[kState] & bit) !== 0;
        },
        set(value) {
          if (value)
            this[kState] |= bit;
          else
            this[kState] &= ~bit;
        }
      };
    }
    WritableState.prototype = {};
    ObjectDefineProperties(WritableState.prototype, {
      objectMode: makeBitMapDescriptor(kObjectMode),
      finalCalled: makeBitMapDescriptor(kFinalCalled),
      needDrain: makeBitMapDescriptor(kNeedDrain),
      ending: makeBitMapDescriptor(kEnding),
      ended: makeBitMapDescriptor(kEnded),
      finished: makeBitMapDescriptor(kFinished),
      destroyed: makeBitMapDescriptor(kDestroyed),
      decodeStrings: makeBitMapDescriptor(kDecodeStrings),
      writing: makeBitMapDescriptor(kWriting),
      sync: makeBitMapDescriptor(kSync),
      bufferProcessing: makeBitMapDescriptor(kBufferProcessing),
      constructed: makeBitMapDescriptor(kConstructed),
      prefinished: makeBitMapDescriptor(kPrefinished),
      errorEmitted: makeBitMapDescriptor(kErrorEmitted),
      emitClose: makeBitMapDescriptor(kEmitClose),
      autoDestroy: makeBitMapDescriptor(kAutoDestroy),
      closed: makeBitMapDescriptor(kClosed),
      closeEmitted: makeBitMapDescriptor(kCloseEmitted),
      allBuffers: makeBitMapDescriptor(kAllBuffers),
      allNoop: makeBitMapDescriptor(kAllNoop),
      errored: {
        __proto__: null,
        enumerable: false,
        get() {
          return (this[kState] & kErrored) !== 0 ? this[kErroredValue] : null;
        },
        set(value) {
          if (value) {
            this[kErroredValue] = value;
            this[kState] |= kErrored;
          } else {
            this[kState] &= ~kErrored;
          }
        }
      },
      writable: {
        __proto__: null,
        enumerable: false,
        get() {
          return (this[kState] & kHasWritable) !== 0 ? (this[kState] & kWritable) !== 0 : undefined;
        },
        set(value) {
          if (value == null) {
            this[kState] &= ~(kHasWritable | kWritable);
          } else if (value) {
            this[kState] |= kHasWritable | kWritable;
          } else {
            this[kState] |= kHasWritable;
            this[kState] &= ~kWritable;
          }
        }
      },
      defaultEncoding: {
        __proto__: null,
        enumerable: false,
        get() {
          return (this[kState] & kDefaultUTF8Encoding) !== 0 ? "utf8" : this[kDefaultEncodingValue];
        },
        set(value) {
          if (value === "utf8" || value === "utf-8") {
            this[kState] |= kDefaultUTF8Encoding;
          } else {
            this[kState] &= ~kDefaultUTF8Encoding;
            this[kDefaultEncodingValue] = value;
          }
        }
      },
      writecb: {
        __proto__: null,
        enumerable: false,
        get() {
          return (this[kState] & kWriteCb) !== 0 ? this[kWriteCbValue] : nop;
        },
        set(value) {
          this[kWriteCbValue] = value;
          if (value) {
            this[kState] |= kWriteCb;
          } else {
            this[kState] &= ~kWriteCb;
          }
        }
      },
      afterWriteTickInfo: {
        __proto__: null,
        enumerable: false,
        get() {
          return (this[kState] & kAfterWriteTickInfo) !== 0 ? this[kAfterWriteTickInfoValue] : null;
        },
        set(value) {
          this[kAfterWriteTickInfoValue] = value;
          if (value) {
            this[kState] |= kAfterWriteTickInfo;
          } else {
            this[kState] &= ~kAfterWriteTickInfo;
          }
        }
      },
      buffered: {
        __proto__: null,
        enumerable: false,
        get() {
          return (this[kState] & kBuffered) !== 0 ? this[kBufferedValue] : [];
        },
        set(value) {
          this[kBufferedValue] = value;
          if (value) {
            this[kState] |= kBuffered;
          } else {
            this[kState] &= ~kBuffered;
          }
        }
      }
    });
    function WritableState(options, stream, isDuplex) {
      this[kState] = kSync | kConstructed | kEmitClose | kAutoDestroy;
      if (options?.objectMode)
        this[kState] |= kObjectMode;
      if (isDuplex && options?.writableObjectMode)
        this[kState] |= kObjectMode;
      this.highWaterMark = options ? getHighWaterMark(this, options, "writableHighWaterMark", isDuplex) : getDefaultHighWaterMark(false);
      if (!options || options.decodeStrings !== false)
        this[kState] |= kDecodeStrings;
      if (options && options.emitClose === false)
        this[kState] &= ~kEmitClose;
      if (options && options.autoDestroy === false)
        this[kState] &= ~kAutoDestroy;
      const defaultEncoding = options ? options.defaultEncoding : null;
      if (defaultEncoding == null || defaultEncoding === "utf8" || defaultEncoding === "utf-8") {
        this[kState] |= kDefaultUTF8Encoding;
      } else if (Buffer.isEncoding(defaultEncoding)) {
        this[kState] &= ~kDefaultUTF8Encoding;
        this[kDefaultEncodingValue] = defaultEncoding;
      } else {
        throw $ERR_UNKNOWN_ENCODING(defaultEncoding);
      }
      this.length = 0;
      this.corked = 0;
      this.onwrite = onwrite.bind(undefined, stream);
      this.writelen = 0;
      resetBuffer(this);
      this.pendingcb = 0;
    }
    function resetBuffer(state) {
      state[kBufferedValue] = null;
      state.bufferedIndex = 0;
      state[kState] |= kAllBuffers | kAllNoop;
      state[kState] &= ~kBuffered;
    }
    WritableState.prototype.getBuffer = function getBuffer() {
      return (this[kState] & kBuffered) === 0 ? [] : ArrayPrototypeSlice.call(this.buffered, this.bufferedIndex);
    };
    ObjectDefineProperty(WritableState.prototype, "bufferedRequestCount", {
      __proto__: null,
      get() {
        return (this[kState] & kBuffered) === 0 ? 0 : this[kBufferedValue].length - this.bufferedIndex;
      }
    });
    WritableState.prototype[kOnConstructed] = function onConstructed(stream) {
      if ((this[kState] & kWriting) === 0) {
        clearBuffer(stream, this);
      }
      if ((this[kState] & kEnding) !== 0) {
        finishMaybe(stream, this);
      }
    };
    function Writable(options) {
      if (!(this instanceof Writable))
        return new Writable(options);
      this._events ??= {
        close: undefined,
        error: undefined,
        prefinish: undefined,
        finish: undefined,
        drain: undefined
      };
      this._writableState = new WritableState(options, this, false);
      if (options) {
        const { write, writev, destroy, final, construct, signal } = options;
        if (typeof write === "function")
          this._write = write;
        if (typeof writev === "function")
          this._writev = writev;
        if (typeof destroy === "function")
          this._destroy = destroy;
        if (typeof final === "function")
          this._final = final;
        if (typeof construct === "function")
          this._construct = construct;
        if (signal)
          addAbortSignal(signal, this);
      }
      Stream.call(this, options);
      if (this._construct != null) {
        destroyImpl.construct(this, () => {
          this._writableState[kOnConstructed](this);
        });
      }
    }
    $toClass(Writable, "Writable", Stream);
    Writable.WritableState = WritableState;
    ObjectDefineProperty(Writable, SymbolHasInstance, {
      __proto__: null,
      value: function(object) {
        if (FunctionPrototypeSymbolHasInstance.call(this, object))
          return true;
        if (this !== Writable)
          return false;
        return object && object._writableState instanceof WritableState;
      }
    });
    Writable.prototype.pipe = function() {
      errorOrDestroy(this, $ERR_STREAM_CANNOT_PIPE());
    };
    function _write(stream, chunk, encoding, cb) {
      const state = stream._writableState;
      if (cb == null || typeof cb !== "function") {
        cb = nop;
      }
      if (chunk === null) {
        throw $ERR_STREAM_NULL_VALUES();
      }
      if ((state[kState] & kObjectMode) === 0) {
        if (!encoding) {
          encoding = (state[kState] & kDefaultUTF8Encoding) !== 0 ? "utf8" : state.defaultEncoding;
        } else if (encoding !== "buffer" && !Buffer.isEncoding(encoding)) {
          throw $ERR_UNKNOWN_ENCODING(encoding);
        }
        if (typeof chunk === "string") {
          if (encoding === "buffer") {
            throw $ERR_UNKNOWN_ENCODING(encoding);
          }
          if ((state[kState] & kDecodeStrings) !== 0) {
            chunk = Buffer.from(chunk, encoding);
            encoding = "buffer";
          }
        } else if (chunk instanceof Buffer) {
          encoding = "buffer";
        } else if (Stream._isArrayBufferView(chunk)) {
          chunk = Stream._uint8ArrayToBuffer(chunk);
          encoding = "buffer";
        } else {
          throw $ERR_INVALID_ARG_TYPE("chunk", ["string", "Buffer", "TypedArray", "DataView"], chunk);
        }
      }
      let err;
      if ((state[kState] & kEnding) !== 0) {
        err = $ERR_STREAM_WRITE_AFTER_END();
      } else if ((state[kState] & kDestroyed) !== 0) {
        err = $ERR_STREAM_DESTROYED("write");
      }
      if (err) {
        process.nextTick(cb, err);
        errorOrDestroy(stream, err, true);
        return err;
      }
      state.pendingcb++;
      return writeOrBuffer(stream, state, chunk, encoding, cb);
    }
    Writable.prototype.write = function(chunk, encoding, cb) {
      if (encoding != null && typeof encoding === "function") {
        cb = encoding;
        encoding = null;
      }
      return _write(this, chunk, encoding, cb) === true;
    };
    Writable.prototype.cork = function() {
      const state = this._writableState;
      state[kState] |= kCorked;
      state.corked++;
    };
    Writable.prototype.uncork = function() {
      const state = this._writableState;
      if (state.corked) {
        state.corked--;
        if (!state.corked) {
          state[kState] &= ~kCorked;
        }
        if ((state[kState] & kWriting) === 0)
          clearBuffer(this, state);
      }
    };
    Writable.prototype.setDefaultEncoding = function setDefaultEncoding(encoding) {
      if (typeof encoding === "string")
        encoding = StringPrototypeToLowerCase.call(encoding);
      if (!Buffer.isEncoding(encoding))
        throw $ERR_UNKNOWN_ENCODING(encoding);
      this._writableState.defaultEncoding = encoding;
      return this;
    };
    function writeOrBuffer(stream, state, chunk, encoding, callback) {
      const len = (state[kState] & kObjectMode) !== 0 ? 1 : chunk.length;
      state.length += len;
      if ((state[kState] & (kWriting | kErrored | kCorked | kConstructed)) !== kConstructed) {
        if ((state[kState] & kBuffered) === 0) {
          state[kState] |= kBuffered;
          state[kBufferedValue] = [];
        }
        state[kBufferedValue].push({ chunk, encoding, callback });
        if ((state[kState] & kAllBuffers) !== 0 && encoding !== "buffer") {
          state[kState] &= ~kAllBuffers;
        }
        if ((state[kState] & kAllNoop) !== 0 && callback !== nop) {
          state[kState] &= ~kAllNoop;
        }
      } else {
        state.writelen = len;
        if (callback !== nop) {
          state.writecb = callback;
        }
        state[kState] |= kWriting | kSync | kExpectWriteCb;
        stream._write(chunk, encoding, state.onwrite);
        state[kState] &= ~kSync;
      }
      const ret = state.length < state.highWaterMark || state.length === 0;
      if (!ret) {
        state[kState] |= kNeedDrain;
      }
      return ret && (state[kState] & (kDestroyed | kErrored)) === 0;
    }
    function doWrite(stream, state, writev, len, chunk, encoding, cb) {
      state.writelen = len;
      if (cb !== nop) {
        state.writecb = cb;
      }
      state[kState] |= kWriting | kSync | kExpectWriteCb;
      if ((state[kState] & kDestroyed) !== 0)
        state.onwrite($ERR_STREAM_DESTROYED("write"));
      else if (writev)
        stream._writev(chunk, state.onwrite);
      else
        stream._write(chunk, encoding, state.onwrite);
      state[kState] &= ~kSync;
    }
    function onwriteError(stream, state, er, cb) {
      --state.pendingcb;
      cb(er);
      errorBuffer(state);
      errorOrDestroy(stream, er);
    }
    function onwrite(stream, er) {
      const state = stream._writableState;
      if ((state[kState] & kExpectWriteCb) === 0) {
        errorOrDestroy(stream, $ERR_MULTIPLE_CALLBACK());
        return;
      }
      const sync = (state[kState] & kSync) !== 0;
      const cb = (state[kState] & kWriteCb) !== 0 ? state[kWriteCbValue] : nop;
      state.writecb = null;
      state[kState] &= ~(kWriting | kExpectWriteCb);
      state.length -= state.writelen;
      state.writelen = 0;
      if (er) {
        er.stack;
        if ((state[kState] & kErrored) === 0) {
          state[kErroredValue] = er;
          state[kState] |= kErrored;
        }
        const readableState = stream._readableState;
        if (readableState && !readableState.errored) {
          readableState.errored = er;
        }
        if (sync) {
          process.nextTick(onwriteError, stream, state, er, cb);
        } else {
          onwriteError(stream, state, er, cb);
        }
      } else {
        if ((state[kState] & kBuffered) !== 0) {
          clearBuffer(stream, state);
        }
        if (sync) {
          const needDrain = (state[kState] & kNeedDrain) !== 0 && state.length === 0;
          const needTick = needDrain || state[kState] & Number(kDestroyed !== 0) || cb !== nop;
          if (cb === nop) {
            if ((state[kState] & kAfterWritePending) === 0 && needTick) {
              process.nextTick(afterWrite, stream, state, 1, cb);
              state[kState] |= kAfterWritePending;
            } else {
              state.pendingcb--;
              if ((state[kState] & kEnding) !== 0) {
                finishMaybe(stream, state, true);
              }
            }
          } else if ((state[kState] & kAfterWriteTickInfo) !== 0 && state[kAfterWriteTickInfoValue].cb === cb) {
            state[kAfterWriteTickInfoValue].count++;
          } else if (needTick) {
            state[kAfterWriteTickInfoValue] = { count: 1, cb, stream, state };
            process.nextTick(afterWriteTick, state[kAfterWriteTickInfoValue]);
            state[kState] |= kAfterWritePending | kAfterWriteTickInfo;
          } else {
            state.pendingcb--;
            if ((state[kState] & kEnding) !== 0) {
              finishMaybe(stream, state, true);
            }
          }
        } else {
          afterWrite(stream, state, 1, cb);
        }
      }
    }
    function afterWriteTick({ stream, state, count, cb }) {
      state[kState] &= ~kAfterWriteTickInfo;
      state[kAfterWriteTickInfoValue] = null;
      return afterWrite(stream, state, count, cb);
    }
    function afterWrite(stream, state, count, cb) {
      state[kState] &= ~kAfterWritePending;
      const needDrain = (state[kState] & (kEnding | kNeedDrain | kDestroyed)) === kNeedDrain && state.length === 0;
      if (needDrain) {
        state[kState] &= ~kNeedDrain;
        stream.emit("drain");
      }
      while (count-- > 0) {
        state.pendingcb--;
        cb(null);
      }
      if ((state[kState] & kDestroyed) !== 0) {
        errorBuffer(state);
      }
      if ((state[kState] & kEnding) !== 0) {
        finishMaybe(stream, state, true);
      }
    }
    function errorBuffer(state) {
      if ((state[kState] & kWriting) !== 0) {
        return;
      }
      if ((state[kState] & kBuffered) !== 0) {
        for (let n = state.bufferedIndex;n < state.buffered.length; ++n) {
          const { chunk, callback } = state[kBufferedValue][n];
          const len = (state[kState] & kObjectMode) !== 0 ? 1 : chunk.length;
          state.length -= len;
          callback(state.errored ?? $ERR_STREAM_DESTROYED("write"));
        }
      }
      callFinishedCallbacks(state, state.errored ?? $ERR_STREAM_DESTROYED("end"));
      resetBuffer(state);
    }
    function clearBuffer(stream, state) {
      if ((state[kState] & (kDestroyed | kBufferProcessing | kCorked | kBuffered | kConstructed)) !== (kBuffered | kConstructed)) {
        return;
      }
      const objectMode = (state[kState] & kObjectMode) !== 0;
      const { [kBufferedValue]: buffered, bufferedIndex } = state;
      const bufferedLength = buffered.length - bufferedIndex;
      if (!bufferedLength) {
        return;
      }
      let i = bufferedIndex;
      state[kState] |= kBufferProcessing;
      if (bufferedLength > 1 && stream._writev) {
        state.pendingcb -= bufferedLength - 1;
        const callback = (state[kState] & kAllNoop) !== 0 ? nop : (err) => {
          for (let n = i;n < buffered.length; ++n) {
            buffered[n].callback(err);
          }
        };
        const chunks = (state[kState] & kAllNoop) !== 0 && i === 0 ? buffered : ArrayPrototypeSlice.call(buffered, i);
        chunks.allBuffers = (state[kState] & kAllBuffers) !== 0;
        doWrite(stream, state, true, state.length, chunks, "", callback);
        resetBuffer(state);
      } else {
        do {
          const { chunk, encoding, callback } = buffered[i];
          buffered[i++] = null;
          const len = objectMode ? 1 : chunk.length;
          doWrite(stream, state, false, len, chunk, encoding, callback);
        } while (i < buffered.length && (state[kState] & kWriting) === 0);
        if (i === buffered.length) {
          resetBuffer(state);
        } else if (i > 256) {
          buffered.splice(0, i);
          state.bufferedIndex = 0;
        } else {
          state.bufferedIndex = i;
        }
      }
      state[kState] &= ~kBufferProcessing;
    }
    Writable.prototype._write = function(chunk, encoding, cb) {
      if (this._writev) {
        this._writev([{ chunk, encoding }], cb);
      } else {
        throw $ERR_METHOD_NOT_IMPLEMENTED("_write()");
      }
    };
    Writable.prototype._writev = null;
    Writable.prototype.end = function(chunk, encoding, cb) {
      const state = this._writableState;
      if (typeof chunk === "function") {
        cb = chunk;
        chunk = null;
        encoding = null;
      } else if (typeof encoding === "function") {
        cb = encoding;
        encoding = null;
      }
      let err;
      if (chunk != null) {
        const ret = _write(this, chunk, encoding);
        if (Error.isError(ret)) {
          err = ret;
        }
      }
      if ((state[kState] & kCorked) !== 0) {
        state.corked = 1;
        this.uncork();
      }
      if (err) {} else if ((state[kState] & (kEnding | kErrored)) === 0) {
        state[kState] |= kEnding;
        finishMaybe(this, state, true);
        state[kState] |= kEnded;
      } else if ((state[kState] & kFinished) !== 0) {
        err = $ERR_STREAM_ALREADY_FINISHED("end");
      } else if ((state[kState] & kDestroyed) !== 0) {
        err = $ERR_STREAM_DESTROYED("end");
      }
      if (typeof cb === "function") {
        if (err) {
          process.nextTick(cb, err);
        } else if ((state[kState] & kErrored) !== 0) {
          process.nextTick(cb, state[kErroredValue]);
        } else if ((state[kState] & kFinished) !== 0) {
          process.nextTick(cb, null);
        } else {
          state[kState] |= kOnFinished;
          state[kOnFinishedValue] ??= [];
          state[kOnFinishedValue].push(cb);
        }
      }
      return this;
    };
    function needFinish(state) {
      return (state[kState] & (kEnding | kDestroyed | kConstructed | kFinished | kWriting | kErrorEmitted | kCloseEmitted | kErrored | kBuffered)) === (kEnding | kConstructed) && state.length === 0;
    }
    function onFinish(stream, state, err) {
      if ((state[kState] & kPrefinished) !== 0) {
        errorOrDestroy(stream, err ?? $ERR_MULTIPLE_CALLBACK());
        return;
      }
      state.pendingcb--;
      if (err) {
        callFinishedCallbacks(state, err);
        errorOrDestroy(stream, err, (state[kState] & kSync) !== 0);
      } else if (needFinish(state)) {
        state[kState] |= kPrefinished;
        stream.emit("prefinish");
        state.pendingcb++;
        process.nextTick(finish, stream, state);
      }
    }
    function prefinish(stream, state) {
      if ((state[kState] & (kPrefinished | kFinalCalled)) !== 0) {
        return;
      }
      if (typeof stream._final === "function" && (state[kState] & kDestroyed) === 0) {
        state[kState] |= kFinalCalled | kSync;
        state.pendingcb++;
        try {
          stream._final((err) => onFinish(stream, state, err));
        } catch (err) {
          onFinish(stream, state, err);
        }
        state[kState] &= ~kSync;
      } else {
        state[kState] |= kFinalCalled | kPrefinished;
        stream.emit("prefinish");
      }
    }
    function finishMaybe(stream, state, sync) {
      if (needFinish(state)) {
        prefinish(stream, state);
        if (state.pendingcb === 0) {
          if (sync) {
            state.pendingcb++;
            process.nextTick((stream, state) => {
              if (needFinish(state)) {
                finish(stream, state);
              } else {
                state.pendingcb--;
              }
            }, stream, state);
          } else if (needFinish(state)) {
            state.pendingcb++;
            finish(stream, state);
          }
        }
      }
    }
    function finish(stream, state) {
      state.pendingcb--;
      state[kState] |= kFinished;
      callFinishedCallbacks(state, null);
      stream.emit("finish");
      if ((state[kState] & kAutoDestroy) !== 0) {
        const rState = stream._readableState;
        const autoDestroy = !rState || rState.autoDestroy && (rState.endEmitted || rState.readable === false);
        if (autoDestroy) {
          stream.destroy();
        }
      }
    }
    function callFinishedCallbacks(state, err) {
      if ((state[kState] & kOnFinished) === 0) {
        return;
      }
      const onfinishCallbacks = state[kOnFinishedValue];
      state[kOnFinishedValue] = null;
      state[kState] &= ~kOnFinished;
      for (let i = 0;i < onfinishCallbacks.length; i++) {
        onfinishCallbacks[i](err);
      }
    }
    ObjectDefineProperties(Writable.prototype, {
      closed: {
        __proto__: null,
        get() {
          return this._writableState ? (this._writableState[kState] & kClosed) !== 0 : false;
        }
      },
      destroyed: {
        __proto__: null,
        get() {
          return this._writableState ? (this._writableState[kState] & kDestroyed) !== 0 : false;
        },
        set(value) {
          if (!this._writableState)
            return;
          if (value)
            this._writableState[kState] |= kDestroyed;
          else
            this._writableState[kState] &= ~kDestroyed;
        }
      },
      writable: {
        __proto__: null,
        get() {
          const w = this._writableState;
          return !!w && w.writable !== false && (w[kState] & (kEnding | kEnded | kDestroyed | kErrored)) === 0;
        },
        set(val) {
          const state = this._writableState;
          if (state) {
            state.writable = !!val;
          }
        }
      },
      writableFinished: {
        __proto__: null,
        get() {
          const state = this._writableState;
          return state ? (state[kState] & kFinished) !== 0 : false;
        }
      },
      writableObjectMode: {
        __proto__: null,
        get() {
          const state = this._writableState;
          return state ? (state[kState] & kObjectMode) !== 0 : false;
        }
      },
      writableBuffer: {
        __proto__: null,
        get() {
          const state = this._writableState;
          return state && state.getBuffer();
        }
      },
      writableEnded: {
        __proto__: null,
        get() {
          const state = this._writableState;
          return state ? (state[kState] & kEnding) !== 0 : false;
        }
      },
      writableNeedDrain: {
        __proto__: null,
        get() {
          const state = this._writableState;
          return state ? (state[kState] & (kDestroyed | kEnding | kNeedDrain)) === kNeedDrain : false;
        }
      },
      writableHighWaterMark: {
        __proto__: null,
        get() {
          const state = this._writableState;
          return state?.highWaterMark;
        }
      },
      writableCorked: {
        __proto__: null,
        get() {
          const state = this._writableState;
          return state ? state.corked : 0;
        }
      },
      writableLength: {
        __proto__: null,
        get() {
          const state = this._writableState;
          return state?.length;
        }
      },
      errored: {
        __proto__: null,
        enumerable: false,
        get() {
          const state = this._writableState;
          return state ? state.errored : null;
        }
      },
      writableAborted: {
        __proto__: null,
        get: function() {
          const state = this._writableState;
          return (state[kState] & (kHasWritable | kWritable)) !== kHasWritable && (state[kState] & (kDestroyed | kErrored)) !== 0 && (state[kState] & kFinished) === 0;
        }
      }
    });
    const destroy = destroyImpl.destroy;
    Writable.prototype.destroy = function(err, cb) {
      const state = this._writableState;
      if ((state[kState] & (kBuffered | kOnFinished)) !== 0 && (state[kState] & kDestroyed) === 0) {
        process.nextTick(errorBuffer, state);
      }
      destroy.call(this, err, cb);
      return this;
    };
    Writable.prototype._undestroy = destroyImpl.undestroy;
    Writable.prototype._destroy = function(err, cb) {
      cb(err);
    };
    Writable.prototype[EE.captureRejectionSymbol] = function(err) {
      this.destroy(err);
    };
    let webStreamsAdapters;
    function lazyWebStreams() {
      if (webStreamsAdapters === undefined)
        webStreamsAdapters = require("internal/webstreams_adapters");
      return webStreamsAdapters;
    }
    Writable.fromWeb = function(writableStream, options) {
      return lazyWebStreams().newStreamWritableFromWritableStream(writableStream, options);
    };
    Writable.toWeb = function(streamWritable) {
      return lazyWebStreams().newWritableStreamFromStreamWritable(streamWritable);
    };
    Writable.prototype[SymbolAsyncDispose] = function() {
      let error;
      if (!this.destroyed) {
        error = this.writableFinished ? null : $makeAbortError();
        this.destroy(error);
      }
      return new Promise((resolve, reject) => eos(this, (err) => err && err.name !== "AbortError" ? reject(err) : resolve(null)));
    };
    module.exports = Writable;

  });

  R.def("internal/streams/duplex", function (require, module, exports) {
    const Stream = require("internal/streams/legacy").Stream;
    const Readable = require("internal/streams/readable");
    const Writable = require("internal/streams/writable");
    const { addAbortSignal } = require("internal/streams/add-abort-signal");
    const destroyImpl = require("internal/streams/destroy");
    const { kOnConstructed } = require("internal/streams/utils");
    const ObjectKeys = Object.keys;
    const ObjectDefineProperties = Object.defineProperties;
    const ObjectGetOwnPropertyDescriptor = Object.getOwnPropertyDescriptor;
    function Duplex(options) {
      if (!(this instanceof Duplex))
        return new Duplex(options);
      this._events ??= {
        close: undefined,
        error: undefined,
        prefinish: undefined,
        finish: undefined,
        drain: undefined,
        data: undefined,
        end: undefined,
        readable: undefined
      };
      this._readableState = new Readable.ReadableState(options, this, true);
      this._writableState = new Writable.WritableState(options, this, true);
      if (options) {
        this.allowHalfOpen = options.allowHalfOpen !== false;
        if (options.readable === false) {
          this._readableState.readable = false;
          this._readableState.ended = true;
          this._readableState.endEmitted = true;
        }
        if (options.writable === false) {
          this._writableState.writable = false;
          this._writableState.ending = true;
          this._writableState.ended = true;
          this._writableState.finished = true;
        }
        const { read, write, writev, destroy, final, construct, signal } = options;
        if (typeof read === "function")
          this._read = read;
        if (typeof write === "function")
          this._write = write;
        if (typeof writev === "function")
          this._writev = writev;
        if (typeof destroy === "function")
          this._destroy = destroy;
        if (typeof final === "function")
          this._final = final;
        if (typeof construct === "function")
          this._construct = construct;
        if (signal)
          addAbortSignal(signal, this);
      } else {
        this.allowHalfOpen = true;
      }
      Stream.call(this, options);
      if (this._construct != null) {
        destroyImpl.construct(this, () => {
          this._readableState[kOnConstructed](this);
          this._writableState[kOnConstructed](this);
        });
      }
    }
    $toClass(Duplex, "Duplex", Readable);
    Duplex.prototype.destroy = Writable.prototype.destroy;
    {
      const keys = ObjectKeys(Writable.prototype);
      for (let i = 0;i < keys.length; i++) {
        const method = keys[i];
        Duplex.prototype[method] ||= Writable.prototype[method];
      }
    }
    ObjectDefineProperties(Duplex.prototype, {
      writable: { __proto__: null, ...ObjectGetOwnPropertyDescriptor(Writable.prototype, "writable") },
      writableHighWaterMark: {
        __proto__: null,
        ...ObjectGetOwnPropertyDescriptor(Writable.prototype, "writableHighWaterMark")
      },
      writableObjectMode: { __proto__: null, ...ObjectGetOwnPropertyDescriptor(Writable.prototype, "writableObjectMode") },
      writableBuffer: { __proto__: null, ...ObjectGetOwnPropertyDescriptor(Writable.prototype, "writableBuffer") },
      writableLength: { __proto__: null, ...ObjectGetOwnPropertyDescriptor(Writable.prototype, "writableLength") },
      writableFinished: { __proto__: null, ...ObjectGetOwnPropertyDescriptor(Writable.prototype, "writableFinished") },
      writableCorked: { __proto__: null, ...ObjectGetOwnPropertyDescriptor(Writable.prototype, "writableCorked") },
      writableEnded: { __proto__: null, ...ObjectGetOwnPropertyDescriptor(Writable.prototype, "writableEnded") },
      writableNeedDrain: { __proto__: null, ...ObjectGetOwnPropertyDescriptor(Writable.prototype, "writableNeedDrain") },
      destroyed: {
        __proto__: null,
        get() {
          if (this._readableState === undefined || this._writableState === undefined) {
            return false;
          }
          return this._readableState.destroyed && this._writableState.destroyed;
        },
        set(value) {
          const readableState = this._readableState;
          let writableState;
          if (readableState && (writableState = this._writableState)) {
            readableState.destroyed = value;
            writableState.destroyed = value;
          }
        }
      }
    });
    let webStreamsAdapters;
    function lazyWebStreams() {
      if (webStreamsAdapters === undefined)
        webStreamsAdapters = require("internal/webstreams_adapters");
      return webStreamsAdapters;
    }
    Duplex.fromWeb = function(pair, options) {
      return lazyWebStreams().newStreamDuplexFromReadableWritablePair(pair, options);
    };
    Duplex.toWeb = function(duplex, options) {
      return lazyWebStreams().newReadableWritablePairFromDuplex(duplex, options);
    };
    let duplexify;
    Duplex.from = function(body) {
      duplexify ??= require("internal/streams/duplexify");
      return duplexify(body, "body");
    };
    module.exports = Duplex;

  });

  R.def("internal/streams/duplexify", function (require, module, exports) {
    const {
      isReadable,
      isWritable,
      isIterable,
      isNodeStream,
      isReadableNodeStream,
      isWritableNodeStream,
      isDuplexNodeStream,
      isReadableStream,
      isWritableStream
    } = require("internal/streams/utils");
    const eos = require("internal/streams/end-of-stream");
    const { destroyer } = require("internal/streams/destroy");
    const Duplex = require("internal/streams/duplex");
    const Readable = require("internal/streams/readable");
    const Writable = require("internal/streams/writable");
    const from = require("internal/streams/from");
    const PromiseWithResolvers = Promise.withResolvers.bind(Promise);

    class Duplexify extends Duplex {
      constructor(options) {
        super(options);
        if (options?.readable === false) {
          this._readableState.readable = false;
          this._readableState.ended = true;
          this._readableState.endEmitted = true;
        }
        if (options?.writable === false) {
          this._writableState.writable = false;
          this._writableState.ending = true;
          this._writableState.ended = true;
          this._writableState.finished = true;
        }
      }
    }
    function duplexify(body, name) {
      if (isDuplexNodeStream(body)) {
        return body;
      }
      if (isReadableNodeStream(body)) {
        return _duplexify({ readable: body });
      }
      if (isWritableNodeStream(body)) {
        return _duplexify({ writable: body });
      }
      if (isNodeStream(body)) {
        return _duplexify({ writable: false, readable: false });
      }
      if (isReadableStream(body)) {
        return _duplexify({ readable: Readable.fromWeb(body) });
      }
      if (isWritableStream(body)) {
        return _duplexify({ writable: Writable.fromWeb(body) });
      }
      if (typeof body === "function") {
        const { value, write, final, destroy } = fromAsyncGen(body);
        if (isDuplexNodeStream(value)) {
          return value;
        }
        if (isIterable(value)) {
          return from(Duplexify, value, {
            objectMode: true,
            write,
            final,
            destroy
          });
        }
        const then = value?.then;
        if (typeof then === "function") {
          let d;
          const promise = then.call(value, (val) => {
            if (val != null) {
              throw $ERR_INVALID_RETURN_VALUE("nully", "body", val);
            }
          }, (err) => {
            destroyer(d, err);
          });
          return d = new Duplexify({
            objectMode: true,
            readable: false,
            write,
            final(cb) {
              final(async () => {
                try {
                  await promise;
                  process.nextTick(cb, null);
                } catch (err) {
                  process.nextTick(cb, err);
                }
              });
            },
            destroy
          });
        }
        throw $ERR_INVALID_RETURN_VALUE("Iterable, AsyncIterable or AsyncFunction", name, value);
      }
      if ($inheritsBlob(body)) {
        return duplexify(body.arrayBuffer());
      }
      if (isIterable(body)) {
        return from(Duplexify, body, {
          objectMode: true,
          writable: false
        });
      }
      if (isReadableStream(body?.readable) && isWritableStream(body?.writable)) {
        return Duplexify.fromWeb(body);
      }
      if (typeof body?.writable === "object" || typeof body?.readable === "object") {
        const readable = body?.readable ? isReadableNodeStream(body?.readable) ? body?.readable : duplexify(body.readable) : undefined;
        const writable = body?.writable ? isWritableNodeStream(body?.writable) ? body?.writable : duplexify(body.writable) : undefined;
        return _duplexify({ readable, writable });
      }
      const then = body?.then;
      if (typeof then === "function") {
        let d;
        then.call(body, (val) => {
          if (val != null) {
            d.push(val);
          }
          d.push(null);
        }, (err) => {
          destroyer(d, err);
        });
        return d = new Duplexify({
          objectMode: true,
          writable: false,
          read() {}
        });
      }
      throw $ERR_INVALID_ARG_TYPE(name, [
        "Blob",
        "ReadableStream",
        "WritableStream",
        "Stream",
        "Iterable",
        "AsyncIterable",
        "Function",
        "{ readable, writable } pair",
        "Promise"
      ], body);
    }
    function fromAsyncGen(fn) {
      let { promise, resolve } = PromiseWithResolvers();
      const ac = new AbortController;
      const signal = ac.signal;
      const value = fn(async function* () {
        while (true) {
          const _promise = promise;
          promise = null;
          const { chunk, done, cb } = await _promise;
          process.nextTick(cb);
          if (done)
            return;
          if (signal.aborted)
            throw $makeAbortError(undefined, { cause: signal.reason });
          ({ promise, resolve } = PromiseWithResolvers());
          yield chunk;
        }
      }(), { signal });
      return {
        value,
        write(chunk, encoding, cb) {
          const _resolve = resolve;
          resolve = null;
          _resolve({ chunk, done: false, cb });
        },
        final(cb) {
          const _resolve = resolve;
          resolve = null;
          _resolve({ done: true, cb });
        },
        destroy(err, cb) {
          ac.abort(err);
          if (resolve !== null) {
            const _resolve = resolve;
            resolve = null;
            _resolve({ __proto__: null, done: true, cb() {} });
          }
          cb(err);
        }
      };
    }
    function _duplexify(pair) {
      const r = pair.readable && typeof pair.readable.read !== "function" ? Readable.wrap(pair.readable) : pair.readable;
      const w = pair.writable;
      let readable = !!isReadable(r);
      let writable = !!isWritable(w);
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
        }
      }
      d = new Duplexify({
        readableObjectMode: !!r?.readableObjectMode,
        writableObjectMode: !!w?.writableObjectMode,
        readable,
        writable
      });
      if (writable) {
        eos(w, (err) => {
          writable = false;
          if (err) {
            destroyer(r, err);
          }
          onfinished(err);
        });
        d._write = function(chunk, encoding, callback) {
          if (w.write(chunk, encoding)) {
            callback();
          } else {
            ondrain = callback;
          }
        };
        d._final = function(callback) {
          w.end();
          onfinish = callback;
        };
        w.on("drain", function() {
          if (ondrain) {
            const cb = ondrain;
            ondrain = null;
            cb();
          }
        });
        w.on("finish", function() {
          if (onfinish) {
            const cb = onfinish;
            onfinish = null;
            cb();
          }
        });
      }
      if (readable) {
        eos(r, (err) => {
          readable = false;
          if (err) {
            destroyer(w, err);
          }
          onfinished(err);
        });
        r.on("readable", function() {
          if (onreadable) {
            const cb = onreadable;
            onreadable = null;
            cb();
          }
        });
        r.on("end", function() {
          d.push(null);
        });
        d._read = function() {
          while (true) {
            const buf = r.read();
            if (buf === null) {
              onreadable = d._read;
              return;
            }
            if (!d.push(buf)) {
              return;
            }
          }
        };
      }
      d._destroy = function(err, callback) {
        if (!err && onclose !== null) {
          err = $makeAbortError();
        }
        onreadable = null;
        ondrain = null;
        onfinish = null;
        if (onclose === null) {
          callback(err);
        } else {
          onclose = callback;
          destroyer(w, err);
          destroyer(r, err);
        }
      };
      return d;
    }
    module.exports = duplexify;

  });

  R.def("internal/streams/transform", function (require, module, exports) {
    const Duplex = require("internal/streams/duplex");
    const { getHighWaterMark } = require("internal/streams/state");
    const kCallback = Symbol("kCallback");
    function Transform(options) {
      if (!(this instanceof Transform))
        return new Transform(options);
      const readableHighWaterMark = options ? getHighWaterMark(this, options, "readableHighWaterMark", true) : null;
      if (readableHighWaterMark === 0) {
        options = {
          ...options,
          highWaterMark: null,
          readableHighWaterMark,
          writableHighWaterMark: options.writableHighWaterMark || 0
        };
      }
      Duplex.call(this, options);
      this._readableState.sync = false;
      this[kCallback] = null;
      if (options) {
        const { transform, flush } = options;
        if (typeof transform === "function")
          this._transform = transform;
        if (typeof flush === "function")
          this._flush = flush;
      }
      this.on("prefinish", prefinish);
    }
    $toClass(Transform, "Transform", Duplex);
    function final(cb) {
      if (typeof this._flush === "function" && !this.destroyed) {
        this._flush((er, data) => {
          if (er) {
            if (cb) {
              cb(er);
            } else {
              this.destroy(er);
            }
            return;
          }
          if (data != null) {
            this.push(data);
          }
          this.push(null);
          if (cb) {
            cb();
          }
        });
      } else {
        this.push(null);
        if (cb) {
          cb();
        }
      }
    }
    function prefinish() {
      if (this._final !== final) {
        final.call(this);
      }
    }
    Transform.prototype._final = final;
    Transform.prototype._transform = function(_chunk, _encoding, _callback) {
      throw $ERR_METHOD_NOT_IMPLEMENTED("_transform()");
    };
    Transform.prototype._write = function(chunk, encoding, callback) {
      const rState = this._readableState;
      const wState = this._writableState;
      const length = rState.length;
      this._transform(chunk, encoding, (err, val) => {
        if (err) {
          callback(err);
          return;
        }
        if (val != null) {
          this.push(val);
        }
        if (rState.ended) {
          process.nextTick(callback);
        } else if (wState.ended || length === rState.length || rState.length < rState.highWaterMark) {
          callback();
        } else {
          this[kCallback] = callback;
        }
      });
    };
    Transform.prototype._read = function() {
      if (this[kCallback]) {
        const callback = this[kCallback];
        this[kCallback] = null;
        callback();
      }
    };
    module.exports = Transform;

  });

  R.def("internal/streams/passthrough", function (require, module, exports) {
    const Transform = require("internal/streams/transform");
    function PassThrough(options) {
      if (!(this instanceof PassThrough))
        return new PassThrough(options);
      Transform.call(this, options);
    }
    $toClass(PassThrough, "PassThrough", Transform);
    PassThrough.prototype._transform = function(chunk, encoding, cb) {
      cb(null, chunk);
    };
    module.exports = PassThrough;

  });
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
