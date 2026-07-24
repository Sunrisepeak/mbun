// node:fs stream surface payload partition — fs.ReadStream / fs.WriteStream,
// fs.createReadStream / fs.createWriteStream and the FileHandle-backed forms.
//
// PORT-SOURCE: node lib/internal/fs/streams.js (the whole file: _construct's
// monkey-patchable open(), importFd's number-or-FileHandle contract,
// FileHandleOperations, the start/end validation and the writeAll retry loop).
//
// Before this partition fs.ReadStream WAS node:stream's Readable and
// createReadStream slurped the whole file on a microtask — so `{ fd }` and
// `{ fd: fileHandle }` were ignored, `start`/`end` were unvalidated,
// `fs.ReadStream.prototype.open` could not be patched, and a paused stream
// still emitted 'end'. Every one of those is a distinct node corpus file.
//
// mbun's fd I/O is synchronous, so the fs-operations object defers each call
// onto a microtask: the stream machinery (construct/destroy/errorOrDestroy)
// assumes a callback that lands on a later turn, and calling it inline
// re-enters _read while the readable state is still mid-update.
//
// NOTE: appended AFTER the master builtins IIFE (like node_fs_watch), so this
// is a self-contained IIFE that augments the already-registered fs module.
export module mbun.jsc.js_builtins:node_fs_streams;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeFsStreamsJS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  if (!M) return;
  const fs = M["fs"] || M["node:fs"];
  const streamMod = M["stream"] || M["node:stream"];
  if (!fs || !streamMod) return;
  const Readable = streamMod.Readable;
  const Writable = streamMod.Writable;
  const finished = streamMod.finished;
  if (!Readable || !Writable) return;

  const V = G.__mbunFsInternals || {};
  const validatePath = V.validatePath || (() => {});
  const validateEncoding = V.validateEncoding || (() => {});
  const argTypeErr = V.argTypeErr || ((n, e, v) => Object.assign(new TypeError('The "' + n + '" argument must be ' + e), { code: "ERR_INVALID_ARG_TYPE" }));
  const rangeErr = V.rangeErr || ((n, r, v) => Object.assign(new RangeError('The value of "' + n + '" is out of range. It must be ' + r + ". Received " + v), { code: "ERR_OUT_OF_RANGE" }));
  const validateInteger = V.validateInteger || (() => {});

  const kFs = Symbol("kFs");
  const kHandle = Symbol("kHandle");
  const kIsPerformingIO = Symbol("kIsPerformingIO");
  const kIoDone = Symbol("kIoDone");

  const notImplemented = (what) => {
    const e = new Error("The " + what + " method is not implemented");
    e.code = "ERR_METHOD_NOT_IMPLEMENTED";
    return e;
  };

  // node getOptions(options, {}): null/undefined/function -> defaults, a string
  // is the encoding, anything else non-object is ERR_INVALID_ARG_TYPE.
  function getOptions(options) {
    if (options == null || typeof options === "function") return {};
    if (typeof options === "string") { validateEncoding(options); return { encoding: options }; }
    if (typeof options !== "object") throw argTypeErr("options", "of type string or Object", options);
    validateEncoding(options);
    // node validateAbortSignal: `null` is NOT accepted either.
    if (options.signal !== undefined &&
        (options.signal === null || typeof options.signal !== "object" || !("aborted" in options.signal)))
      throw argTypeErr("options.signal", "an instance of AbortSignal", options.signal);
    return Object.assign({}, options);
  }

  const getValidatedFd = (fd) => { validateInteger(fd, "fd", 0, 2147483647); return fd; };

  // The default fs-operations table IS the fs module, exactly as node does
  // (`options.fs || fs`) — a test that mocks fs.fsync/fs.write must observe the
  // stream going through it. fs.read/fs.write/fs.close/fs.fsync all deliver
  // their callback on a later turn, which is what the stream machinery needs.
  const nodeFsOps = fs;

  // A FileHandle-backed operations table (node FileHandleOperations).
  const fileHandleOps = (handle) => ({
    open: () => { throw notImplemented("open()"); },
    close: (fd, cb) => { handle._unref && handle._unref(); handle.close().then(() => cb(), cb); },
    fsync: (fd, cb) => { handle.sync().then(() => cb(), cb); },
    read: (fd, buf, offset, length, pos, cb) => {
      handle.read(buf, offset, length, pos).then((r) => cb(null, r.bytesRead, r.buffer), (err) => cb(err, 0, buf));
    },
    write: (fd, buf, offset, length, pos, cb) => {
      handle.write(buf, offset, length, pos).then((r) => cb(null, r.bytesWritten, r.buffer), (err) => cb(err, 0, buf));
    },
    writev: (fd, buffers, pos, cb) => {
      handle.writev(buffers, pos).then((r) => cb(null, r.bytesWritten, r.buffers), (err) => cb(err, 0, buffers));
    },
  });

  const isFileHandle = (v) => {
    const FH = G.__mbunFsInternals && G.__mbunFsInternals.FileHandle;
    return !!(FH && v instanceof FH);
  };

  function importFd(stream, options) {
    if (typeof options.fd === "number") { stream[kFs] = options.fs || nodeFsOps; return options.fd; }
    if (options.fd !== null && typeof options.fd === "object" && isFileHandle(options.fd)) {
      if (options.fs) throw notImplemented("FileHandle with fs");
      stream[kHandle] = options.fd;
      stream[kFs] = fileHandleOps(options.fd);
      if (options.fd._ref) options.fd._ref();
      options.fd.on("close", () => stream.close());
      return options.fd.fd;
    }
    throw argTypeErr("options.fd", "of type number or an instance of FileHandle", options.fd);
  }

  // node _construct: honours a monkey-patched stream.open() (the documented
  // fs.ReadStream.prototype.open override) before falling back to kFs.open.
  function _construct(callback) {
    const stream = this;
    if (typeof stream.fd === "number") { callback(); return; }
    if (typeof stream.open === "function") {
      const orgEmit = stream.emit;
      stream.emit = function (...args) {
        if (args[0] === "open") { this.emit = orgEmit; callback(); orgEmit.apply(this, args); }
        else if (args[0] === "error") { this.emit = orgEmit; callback(args[1]); }
        else return orgEmit.apply(this, args);
      };
      stream.open();
      return;
    }
    stream[kFs].open(stream.path, stream.flags, stream.mode, (er, fd) => {
      if (er) { callback(er); return; }
      stream.fd = fd;
      callback();
      stream.emit("open", stream.fd);
      stream.emit("ready");
    });
  }

  function _closeStream(stream, err, cb) {
    stream[kFs].close(stream.fd, (er) => cb(er || err));
    stream.fd = null;
  }
  function closeStream(stream, err, cb) {
    if (!stream.fd && stream.fd !== 0) { cb(err); return; }
    // `flush: true` fsync's before closing (node lib/internal/fs/streams.js).
    if (stream.flush) { stream[kFs].fsync(stream.fd, (flushErr) => _closeStream(stream, err || flushErr, cb)); return; }
    _closeStream(stream, err, cb);
  }

  function ReadStream(path, options) {
    if (!(this instanceof ReadStream)) return new ReadStream(path, options);
    options = getOptions(options);
    if (options.highWaterMark === undefined) options.highWaterMark = 64 * 1024;
    if (options.autoDestroy === undefined) options.autoDestroy = false;
    if (options.fd == null) {
      this.fd = null;
      this[kFs] = options.fs || nodeFsOps;
      if (typeof this[kFs].open !== "function") throw argTypeErr("options.fs.open", "of type function", this[kFs].open);
      this.path = path;
      this.flags = options.flags === undefined ? "r" : options.flags;
      this.mode = options.mode === undefined ? 0o666 : options.mode;
      validatePath(this.path);
    } else {
      this.fd = getValidatedFd(importFd(this, options));
    }
    options.autoDestroy = options.autoClose === undefined ? true : options.autoClose;
    this.start = options.start;
    this.end = options.end;
    this.pos = undefined;
    this.bytesRead = 0;
    this[kIsPerformingIO] = false;
    if (this.start !== undefined) { validateInteger(this.start, "start", 0); this.pos = this.start; }
    if (this.end === undefined) this.end = Infinity;
    else if (this.end !== Infinity) {
      validateInteger(this.end, "end", 0);
      if (this.start !== undefined && this.start > this.end)
        throw rangeErr("start", '<= "end" (here: ' + this.end + ")", this.start);
    }
    Readable.call(this, options);
  }
  Object.setPrototypeOf(ReadStream.prototype, Readable.prototype);
  Object.setPrototypeOf(ReadStream, Readable);
  Object.defineProperty(ReadStream.prototype, "autoClose", {
    get() { return this._readableState.autoDestroy; },
    set(v) { this._readableState.autoDestroy = v; },
    configurable: true,
  });
  ReadStream.prototype._construct = _construct;
  ReadStream.prototype._read = function (n) {
    n = this.pos !== undefined ? Math.min(this.end - this.pos + 1, n)
                               : Math.min(this.end - this.bytesRead + 1, n);
    if (n <= 0) { this.push(null); return; }
    const buf = Buffer.allocUnsafeSlow(n);
    this[kIsPerformingIO] = true;
    this[kFs].read(this.fd, buf, 0, n, this.pos, (er, bytesRead, b) => {
      this[kIsPerformingIO] = false;
      if (this.destroyed) { this.emit(kIoDone, er); return; }
      if (er) { this.destroy(er); return; }
      if (bytesRead > 0) {
        if (this.pos !== undefined) this.pos += bytesRead;
        this.bytesRead += bytesRead;
        this.push(bytesRead !== b.length ? Buffer.from(b.subarray(0, bytesRead)) : b);
      } else {
        this.push(null);
      }
    });
  };
  ReadStream.prototype._destroy = function (err, cb) {
    if (this[kIsPerformingIO]) this.once(kIoDone, (er) => closeStream(this, err || er, cb));
    else closeStream(this, err, cb);
  };
  ReadStream.prototype.close = function (cb) {
    if (typeof cb === "function" && finished) finished(this, cb);
    this.destroy();
  };
  Object.defineProperty(ReadStream.prototype, "pending", {
    get() { return this.fd === null; }, configurable: true,
  });

  function WriteStream(path, options) {
    if (!(this instanceof WriteStream)) return new WriteStream(path, options);
    options = getOptions(options);
    options.decodeStrings = true;
    if (options.fd == null) {
      this.fd = null;
      this[kFs] = options.fs || nodeFsOps;
      this.path = path;
      this.flags = options.flags === undefined ? "w" : options.flags;
      this.mode = options.mode === undefined ? 0o666 : options.mode;
      validatePath(this.path);
    } else {
      this.fd = getValidatedFd(importFd(this, options));
    }
    options.autoDestroy = options.autoClose === undefined ? true : options.autoClose;
    this.flush = options.flush == null ? false : options.flush;
    if (typeof this.flush !== "boolean") throw argTypeErr("options.flush", "of type boolean", this.flush);
    if (this.flush && typeof this[kFs].fsync !== "function")
      throw argTypeErr("options.fs.fsync", "of type function", this[kFs].fsync);
    if (typeof this[kFs].write !== "function" && typeof this[kFs].writev !== "function")
      throw argTypeErr("options.fs.write", "of type function", this[kFs].write);
    if (typeof this[kFs].writev !== "function") this._writev = null;
    if (typeof this[kFs].write !== "function") this._write = null;
    this.start = options.start;
    this.pos = undefined;
    this.bytesWritten = 0;
    this[kIsPerformingIO] = false;
    if (this.start !== undefined) { validateInteger(this.start, "start", 0); this.pos = this.start; }
    Writable.call(this, options);
    if (options.encoding) this.setDefaultEncoding(options.encoding);
  }
  Object.setPrototypeOf(WriteStream.prototype, Writable.prototype);
  Object.setPrototypeOf(WriteStream, Writable);
  Object.defineProperty(WriteStream.prototype, "autoClose", {
    get() { return this._writableState.autoDestroy; },
    set(v) { this._writableState.autoDestroy = v; },
    configurable: true,
  });
  WriteStream.prototype._construct = _construct;
  function writeAll(data, size, pos, cb) {
    this[kFs].write(this.fd, data, 0, size, pos, (er, bytesWritten, buffer) => {
      if (this.destroyed || er) { cb(er || null); return; }
      this.bytesWritten += bytesWritten;
      if (bytesWritten < size) {
        writeAll.call(this, buffer.subarray(bytesWritten), size - bytesWritten,
                      pos == null ? null : pos + bytesWritten, cb);
        return;
      }
      cb();
    });
  }
  WriteStream.prototype._write = function (data, encoding, cb) {
    this[kIsPerformingIO] = true;
    writeAll.call(this, data, data.length, this.pos, (er) => {
      this[kIsPerformingIO] = false;
      if (this.destroyed) { cb(er); this.emit(kIoDone, er); return; }
      cb(er);
    });
    if (this.pos !== undefined) this.pos += data.length;
  };
  WriteStream.prototype._writev = function (data, cb) {
    const chunks = [];
    let size = 0;
    for (const d of data) { chunks.push(d.chunk); size += d.chunk.length; }
    this[kIsPerformingIO] = true;
    this[kFs].writev(this.fd, chunks, this.pos, (er, bytesWritten) => {
      this[kIsPerformingIO] = false;
      if (!er) this.bytesWritten += bytesWritten;
      if (this.destroyed) { cb(er); this.emit(kIoDone, er); return; }
      cb(er);
    });
    if (this.pos !== undefined) this.pos += size;
  };
  WriteStream.prototype._destroy = function (err, cb) {
    if (this[kIsPerformingIO]) this.once(kIoDone, (er) => closeStream(this, err || er, cb));
    else closeStream(this, err, cb);
  };
  WriteStream.prototype.close = function (cb) {
    if (cb) {
      if (this.closed) { G.queueMicrotask(cb); return; }
      this.on("close", cb);
    }
    if (!this.autoClose) this.on("finish", this.destroy.bind(this));
    this.end();
  };
  WriteStream.prototype.destroySoon = WriteStream.prototype.end;
  Object.defineProperty(WriteStream.prototype, "pending", {
    get() { return this.fd === null; }, configurable: true,
  });

  fs.ReadStream = ReadStream;
  fs.WriteStream = WriteStream;
  fs.FileReadStream = ReadStream;
  fs.FileWriteStream = WriteStream;
  fs.createReadStream = function createReadStream(path, options) { return new ReadStream(path, options); };
  fs.createWriteStream = function createWriteStream(path, options) { return new WriteStream(path, options); };
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
