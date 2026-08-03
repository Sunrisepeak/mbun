// modules/jsc/src/js_streams_inspect.cppm — module mbun.jsc.js_streams_inspect
//
// Continuation of the Web Streams ponyfill (js_streams.cppm). This is
// kStreamsJS_part3: it runs INSIDE the same IIFE opened in part1 (part2 no
// longer closes it), so it sees the closure's stream classes/helpers directly.
// It attaches the `[Symbol.for("nodejs.util.inspect.custom")]` method to each
// stream prototype so util.inspect/Bun.inspect print bun's clean shapes
// instead of the raw internal fields, then closes the IIFE.
//
// Ref (1:1 shapes + receiver/depth semantics):
//   bun-ref/src/jsc/bindings/webcore/streams/WebStreamsInspectCustom.cpp
//   bun-ref/src/jsc/bindings/webcore/streams/JS<Class>.cpp inspectCustom bodies
// Split out purely to keep js_streams.cppm under the 2000-line hard limit.
export module mbun.jsc.js_streams_inspect;

import std;

namespace mbun::jsc::builtins {

export inline constexpr std::string_view kStreamsJS_part3 = R"JS(
  // ---- custom inspect: nodejs.util.inspect.custom ----
  // Each method mirrors bun's native inspectCustom: return `this` for a foreign
  // receiver (dynamicDowncast miss) or depth<0; else `Name ` + inspect(shape,
  // { ...options, depth: options.depth==null ? null : options.depth-1 }). The
  // shape is built from PUBLIC accessors only. Installed non-enumerable so the
  // method itself never shows up in the printed object.
  const kInspect = Symbol.for("nodejs.util.inspect.custom");
  const defineStreamInspect = (Cls, name, shape) => {
    Object.defineProperty(Cls.prototype, kInspect, {
      value: function (depth, options, inspect) {
        // bun's dynamicDowncast<JS<Class>> returns the receiver for anything
        // that is not a genuine instance — a foreign object AND a `.prototype`
        // object (its cell is the Prototype type, not the instance type). The
        // globals are subclass wrappers, so `X.prototype instanceof Cls` is
        // true; exclude prototype objects via constructor.prototype identity.
        if (!(this instanceof Cls) || (this.constructor && this.constructor.prototype === this)) return this;
        if (depth < 0) return this;
        const childDepth = (options && options.depth != null) ? options.depth - 1 : null;
        const opts = Object.assign({}, options, { depth: childDepth });
        return name + " " + inspect(shape(this), opts);
      },
      writable: true, enumerable: false, configurable: true,
    });
  };
  defineStreamInspect(ReadableStream, "ReadableStream", (s) => ({
    locked: s.locked,
    state: s._state,
    supportsBYOB: s._readableStreamController instanceof ReadableByteStreamController,
  }));
  defineStreamInspect(WritableStream, "WritableStream", (s) => ({
    locked: s.locked,
    state: s._state,
  }));
  defineStreamInspect(ReadableStreamDefaultReader, "ReadableStreamDefaultReader", (r) => ({
    stream: r._stream,
    readRequests: r._readRequests ? r._readRequests.length : 0,
    close: r._closedDeferred ? r._closedDeferred.promise : undefined,
  }));
  defineStreamInspect(ReadableStreamBYOBReader, "ReadableStreamBYOBReader", (r) => ({
    stream: r._stream,
    readIntoRequests: r._readIntoRequests ? r._readIntoRequests.length : 0,
    close: r._closedDeferred ? r._closedDeferred.promise : undefined,
  }));
  defineStreamInspect(ReadableStreamDefaultController, "ReadableStreamDefaultController", () => ({}));
  defineStreamInspect(ReadableByteStreamController, "ReadableByteStreamController", () => ({}));
  defineStreamInspect(WritableStreamDefaultWriter, "WritableStreamDefaultWriter", (w) => ({
    stream: w._stream,
    close: w._closedDeferred ? w._closedDeferred.promise : undefined,
    ready: w._readyDeferred ? w._readyDeferred.promise : undefined,
    desiredSize: w._stream ? writableStreamDefaultWriterGetDesiredSize(w) : null,
  }));
  defineStreamInspect(ReadableStreamBYOBRequest, "ReadableStreamBYOBRequest", (q) => ({
    view: q._view != null ? q._view : null,
    controller: q._controller,
  }));
  defineStreamInspect(WritableStreamDefaultController, "WritableStreamDefaultController", (c) => ({
    stream: c._stream,
  }));
  defineStreamInspect(TransformStream, "TransformStream", (t) => ({
    readable: t._readable,
    writable: t._writable,
    backpressure: t._backpressure,
  }));
  defineStreamInspect(TransformStreamDefaultController, "TransformStreamDefaultController", (c) => ({
    stream: c._stream,
  }));
  defineStreamInspect(ByteLengthQueuingStrategy, "ByteLengthQueuingStrategy", (s) => ({
    highWaterMark: s.highWaterMark,
  }));
  defineStreamInspect(CountQueuingStrategy, "CountQueuingStrategy", (s) => ({
    highWaterMark: s.highWaterMark,
  }));

  // ---- Symbol.toStringTag ----
  // Every WebIDL interface carries a @@toStringTag equal to its interface name,
  // as `{ configurable: true, enumerable: false, writable: false }`
  // (webidl.js "Interface prototype object"). Without it `String(stream)` was
  // "[object Object]" and the class-surface tests could not identify any of
  // these; test-webstream-string-tag asserts the exact descriptor.
  for (const Cls of [ReadableStream, ReadableStreamDefaultReader, ReadableStreamBYOBReader,
                     ReadableStreamBYOBRequest, ReadableStreamDefaultController,
                     ReadableByteStreamController, WritableStream, WritableStreamDefaultWriter,
                     WritableStreamDefaultController, TransformStream,
                     TransformStreamDefaultController, ByteLengthQueuingStrategy,
                     CountQueuingStrategy]) {
    Object.defineProperty(Cls.prototype, Symbol.toStringTag, {
      value: Cls.name, writable: false, enumerable: false, configurable: true,
    });
  }
})();
)JS";

}  // namespace mbun::jsc::builtins
