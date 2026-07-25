// node:fs watch surface payload partition — fs.watch / fs.watchFile /
// fs.unwatchFile / fs.promises.watch, plus the FSWatcher and StatWatcher
// classes they return.
//
// PORT-SOURCE: bun-ref/src/js/node/fs.ts (the watch/watchFile/unwatchFile JS
// surface: option coercion, encoding handling, listener wiring) over
// bun-ref/src/watcher/ + path_watcher.rs (PathWatcher/PathWatcherManager: the
// recursive subtree registration and rename-vs-change event classification).
// The inotify mechanics live in mbun.watcher (modules/watcher) and reach here
// through __mbunWatchNative (runtime/watch.inc).
//
// Loop model: bun runs its directory crawl on a work pool and funnels events
// back through a mutex-guarded manager. mbun has no such thread -- FSWatcher is
// a __mbunNet reactor item whose _poll() drains inotify non-blockingly on the JS
// thread, so the close()-vs-pool-scan deadlock bun's fs.watch.deadlock test
// guards against is structurally absent rather than merely fixed.
//
// A persistent (ref'd) watcher holds the loop open via __mbunNet.pending, the
// same channel ref'd timers and in-flight sockets use -- node's fs.watch keeps
// the process alive until close()/unref().
//
// NOTE: appended AFTER the master builtins IIFE has closed (see image_closure),
// so this is a self-contained IIFE that re-binds G = globalThis and augments the
// fs / fs/promises modules bootstrap already registered.
export module mbun.jsc.js_builtins:node_fs_watch;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeFsWatchJS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  if (!M) return;
  const fs = M["fs"] || M["node:fs"];
  if (!fs) return;
  const EE = (M["events"] && M["events"].EventEmitter) || (M["node:events"] && M["node:events"].EventEmitter);
  const WN = G.__mbunWatchNative;

  const toStr = (p) => (typeof p === "string" ? p : p && p.pathname !== undefined ? p.pathname : String(p));
  // Reuse bootstrap's node validators so watch/watchFile/unwatchFile reject a
  // bad path / encoding / listener with the SAME ERR_* shapes the rest of fs
  // does (ref test-fs-null-bytes, test-fs-assert-encoding-error,
  // test-fs-watchfile, test-fs-watch-ignore-invalid).
  const V = G.__mbunFsInternals || {};
  const validatePath = V.validatePath || (() => {});
  const validateEncoding = V.validateEncoding || (() => {});
  const argTypeErr = V.argTypeErr || ((n, e, v) => Object.assign(new TypeError('The "' + n + '" argument must be ' + e), { code: "ERR_INVALID_ARG_TYPE" }));
  const argValueErr = V.argValueErr || ((n, v, r) => Object.assign(new TypeError("The argument '" + n + "' " + r), { code: "ERR_INVALID_ARG_VALUE" }));
  // node validateBoolean / validateInteger / validateOneOf (internal/validators.js),
  // reached from the option blocks below. Reusing bootstrap's argTypeErr keeps the
  // ERR_INVALID_ARG_TYPE shape identical to the rest of fs.
  function validateBoolean(v, name) {
    if (typeof v !== "boolean") throw argTypeErr(name, "of type boolean", v);
  }
  function validateInteger(v, name) {
    if (typeof V.validateInteger === "function") return V.validateInteger(v, name);
    if (typeof v !== "number") throw argTypeErr(name, "of type number", v);
    if (!Number.isInteger(v)) throw argValueErr(name, v, "must be an integer");
  }
  function validateOneOf(v, name, allowed) {
    if (allowed.indexOf(v) === -1)
      throw argValueErr(name, v, "must be one of: " + allowed.join(", "));
  }
  // node's AbortError (internal/errors.js): name AbortError, code ABORT_ERR, and
  // the signal's `reason` carried through as `cause`. NOT a DOMException — a
  // DOMException's `code` is the numeric legacy code (20), and the corpus matches
  // on the string 'ABORT_ERR' (test-fs-watch-recursive-promise).
  function abortError(reason) {
    const e = new Error("The operation was aborted");
    e.name = "AbortError";
    e.code = "ABORT_ERR";
    e.cause = reason;
    return e;
  }
  // node validateIgnoreOption (lib/internal/validators.js): `ignore` is a
  // non-empty string glob, a RegExp, a Function, or an array of any of those.
  // Functions were rejected outright before, and the value was validated then
  // dropped — no event was ever filtered (test-fs-watch-ignore-*).
  const isRe = (v) => v instanceof RegExp || Object.prototype.toString.call(v) === "[object RegExp]";
  function validateIgnore(ignore) {
    if (ignore === undefined || ignore === null) return;
    const one = (v, name) => {
      if (typeof v === "string") {
        if (v.length === 0) throw argValueErr(name, v, "must be a non-empty string");
        return;
      }
      if (isRe(v) || typeof v === "function") return;
      throw argTypeErr(name, "one of type string, RegExp, or Function", v);
    };
    if (Array.isArray(ignore)) {
      for (let i = 0; i < ignore.length; ++i) one(ignore[i], "options.ignore[" + i + "]");
      return;
    }
    one(ignore, "options.ignore");
  }

  // node compiles string patterns with minimatch({ matchBase: true }): a pattern
  // with no "/" is also tried against the basename, so '*.log' ignores
  // 'subdir/file.log' under a recursive watch. ref createIgnoreMatcher in
  // lib/internal/fs/watchers.js. Only the "**", "*", "?" subset is needed.
  function globToRegExp(pat) {
    let re = "";
    for (let i = 0; i < pat.length; ++i) {
      const c = pat[i];
      if (c === "*") {
        if (pat[i + 1] === "*") {
          ++i;
          if (pat[i + 1] === "/") { ++i; re += "(?:.*\\/)?"; } else re += ".*";
        } else re += "[^/]*";
      } else if (c === "?") re += "[^/]";
      else re += c.replace(/[.+^${}()|[\]\\]/g, "\\$&");
    }
    return new RegExp("^" + re + "$");
  }
  function globPredicate(pat) {
    const matchBase = pat.indexOf("/") === -1;
    const re = globToRegExp(pat);
    return (f) => re.test(f) ||
      (matchBase && f.indexOf("/") !== -1 && re.test(f.slice(f.lastIndexOf("/") + 1)));
  }
  function createIgnoreMatcher(ignore) {
    if (ignore === undefined || ignore === null) return null;
    const list = Array.isArray(ignore) ? ignore : [ignore];
    const compiled = [];
    for (const m of list) {
      if (typeof m === "string") compiled.push(globPredicate(m));
      else if (isRe(m)) compiled.push((f) => { m.lastIndex = 0; return m.test(f); });
      else compiled.push(m);
    }
    if (compiled.length === 0) return null;
    return (filename) => {
      for (const p of compiled) { if (p(filename)) return true; }
      return false;
    };
  }

  function unavailable() {
    const e = new Error("The fs.watch API is not available on this platform");
    e.code = "ERR_FEATURE_UNAVAILABLE_ON_PLATFORM";
    return e;
  }

  // fs.watch(filename[, options][, listener]) argument shape: options may be a
  // bare encoding string (node accepts watch(p, "buffer", cb)).
  function normalizeArgs(options, listener) {
    if (typeof options === "function") { listener = options; options = {}; }
    else if (typeof options === "string") { options = { encoding: options }; }
    else if (options == null) { options = {}; }
    else if (typeof options !== "object") { throw new TypeError('The "options" argument must be of type object'); }
    return { options, listener };
  }

  // node hands the raw name bytes to Buffer and then re-encodes with whatever
  // `encoding` says (src/fs_event_wrap.cc OnEvent → StringBytes::Encode with the
  // watcher's encoding): "buffer" yields the Buffer itself, and EVERY other
  // supported encoding — hex, base64, latin1, ucs2 … — is a re-encoding of those
  // bytes, not a passthrough. Only utf8 is the identity. Returning the utf8
  // string unchanged for `encoding: 'hex'` meant test-fs-watch-encoding's hex
  // watcher never recognised its own filename, so its `done` never fired and the
  // test's refresh interval kept the loop alive forever (classified as a hang).
  function decodeName(name, encoding) {
    if (name === null || name === undefined) return name;
    if (encoding === "buffer") return G.Buffer ? G.Buffer.from(name, "utf8") : name;
    if (encoding === undefined || encoding === null || encoding === "utf8" || encoding === "utf-8")
      return name;
    if (!G.Buffer) return name;
    return G.Buffer.from(name, "utf8").toString(encoding);
  }

  // Loop-ref channel: only a persistent, open watcher holds the process alive.
  function loopRef(delta) {
    const NET = G.__mbunNet;
    if (NET) NET.pending += delta;
  }
  function loopItems() {
    const NET = G.__mbunNet;
    return NET ? NET.items : null;
  }

  class FSWatcher extends EE {
    constructor(filename, options, listener) {
      super();
      if (!WN) throw unavailable();
      const path = toStr(filename);
      this._path = path;
      this._enc = options.encoding === undefined ? "utf8" : options.encoding;
      this._ignoreMatcher = createIgnoreMatcher(options.ignore);
      this._persistent = options.persistent === undefined ? true : !!options.persistent;
      this._closed = false;
      this._refd = false;
      // node kFSWatchStart: `throwIfNoEntry: false` swallows a UV_ENOENT from
      // handle.start and RETURNS the watcher un-started (watchers.js). An
      // un-started watcher is not in the poll set and holds no loop reference —
      // otherwise it would pin the process forever with nothing to report
      // (test-fs-watch-enoent's third block closes it and expects the process to
      // leave).
      try {
        this._handle = WN.start(path, !!options.recursive);
      } catch (e) {
        if (options.throwIfNoEntry === false && e && e.code === "ENOENT") {
          this._handle = -1;
          this._closed = true;
          if (typeof listener === "function") this.on("change", listener);
          return;
        }
        throw e;
      }
      if (typeof listener === "function") this.on("change", listener);
      const items = loopItems();
      if (items) items.add(this);
      if (this._persistent) { loopRef(1); this._refd = true; }
      const signal = options.signal;
      if (signal) {
        if (signal.aborted) { queueMicrotask(() => this.close()); }
        else signal.addEventListener("abort", () => this.close(), { once: true });
      }
    }

    // Reactor contract (__mbunNetDrain): drain what inotify has right now and
    // report how many events we delivered; never block waiting for one.
    _poll() {
      if (this._closed) return 0;
      let events;
      try { events = WN.poll(this._handle); }
      catch (e) { this._closed = true; this._release(); this.emit("error", e); return 0; }
      let delivered = 0;
      for (const ev of events) {
        // A listener may close() (or the watcher may be aborted) while we are
        // still walking a batch inotify handed us in one read. node cannot
        // deliver those: closing frees the uv handle, so every event still
        // queued behind the current one is dropped. Draining the rest here
        // instead surfaced as wrong *values* — test-fs-watch-recursive-add-file
        // asserts the first event for a created file is "rename", closes, and
        // then saw the trailing "change" from the same batch.
        if (this._closed) break;
        // node: `if (filename != null && ignoreMatcher?.(filename)) return;`
        // (watchers.js onchange / recursive_watch.js #watchFolder). ev.name is
        // already relative to the watch root for a recursive watch and the bare
        // basename otherwise — exactly what node hands the matcher — so it is
        // matched as a string, before the encoding:"buffer" conversion.
        if (this._ignoreMatcher && ev.name && this._ignoreMatcher(ev.name)) continue;
        delivered += 1;
        this.emit("change", ev.kind, decodeName(ev.name, this._enc));
      }
      return delivered;
    }

    _release() {
      const items = loopItems();
      if (items) items.delete(this);
      if (this._refd) { loopRef(-1); this._refd = false; }
    }

    close() {
      if (this._closed) return;
      this._closed = true;
      this._release();
      try { WN.stop(this._handle); } catch (e) {}
      this._handle = -1;
      this.emit("close");
    }

    ref() {
      if (!this._closed && !this._refd) { loopRef(1); this._refd = true; }
      return this;
    }

    unref() {
      if (this._refd) { loopRef(-1); this._refd = false; }
      return this;
    }
  }

  // watchFile polls stat on an interval (node's default 5007ms) rather than
  // riding inotify, and reports (curr, prev) Stats -- matching node's StatWatcher.
  const statWatchers = new Map();

  // An all-zero Stats/BigIntStats, which is what node/bun hand to the listener
  // for a path that does not exist (`bun_core::ffi::zeroed::<PosixStat>()` in
  // bun-ref/src/runtime/node/node_fs_stat_watcher.rs restat/initial-stat).
  const zeroStats = (bigint) => {
    if (bigint && fs.BigIntStats) {
      return new fs.BigIntStats(0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n);
    }
    return new fs.Stats(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  };

  class StatWatcher extends EE {
    constructor(filename, options) {
      super();
      this._path = toStr(filename);
      this._interval = options.interval === undefined ? 5007 : options.interval;
      this._persistent = options.persistent === undefined ? true : !!options.persistent;
      this._bigint = !!options.bigint;
      this._prev = this._stat();
      this._closed = false;
      this._timer = setInterval(() => this._check(), this._interval);
      if (!this._persistent && this._timer && this._timer.unref) this._timer.unref();
      // bun fires the listener once with (zeroed, zeroed) when the *initial*
      // stat fails, then keeps polling -- see initial_stat_error_on_main_thread
      // in bun-ref/src/runtime/node/node_fs_stat_watcher.rs. Deferred to a tick
      // because watchFile() attaches the listener after the constructor returns.
      // A throwing listener must not stop the poll loop (the interval is
      // already armed above), and surfaces as an uncaughtException.
      if (this._missing) {
        process.nextTick(() => {
          if (this._closed) return;
          this.emit("change", this._prev, this._prev);
        });
      }
    }

    _stat() {
      try {
        const s = fs.statSync(this._path, this._bigint ? { bigint: true } : undefined);
        this._missing = false;
        return s;
      } catch (e) {
        // node hands back a zeroed Stats for a missing file rather than throwing.
        this._missing = true;
        return zeroStats(this._bigint);
      }
    }

    _check() {
      if (this._closed) return;
      const curr = this._stat();
      const prev = this._prev;
      const mt = (s) => (s && s.mtimeMs !== undefined ? s.mtimeMs : 0);
      const sz = (s) => (s && s.size !== undefined ? s.size : 0);
      if (mt(curr) !== mt(prev) || sz(curr) !== sz(prev)) {
        this._prev = curr;
        this.emit("change", curr, prev);
      }
    }

    // node StatWatcher.prototype.stop (internal/fs/watchers.js): the event is
    // named 'stop', not 'close', and it is emitted on a LATER tick —
    // `defaultTriggerAsyncIdScope(..., process.nextTick, emitStop, this)`. Both
    // halves are observable: test-fs-watch-stop-async asserts the listener has
    // NOT run by the time stop() returns and HAS run by the next setImmediate,
    // and test-fs-watch-stop-sync removes the listener right after stop() and
    // asserts it never fires. Stopping an already-stopped watcher is a noop, so
    // 'stop' is emitted exactly once (test-fs-watchfile, -bigint).
    stop() {
      if (this._closed) return;
      this._closed = true;
      clearInterval(this._timer);
      process.nextTick(() => this.emit("stop"));
    }

    // node's StatWatcher exposes ref()/unref() (internal/fs/watchers.js); they
    // proxy the poll timer's loop reference and return `this`.
    // ref test-fs-watchfile-ref-unref.
    ref() {
      if (this._timer && this._timer.ref) this._timer.ref();
      return this;
    }

    unref() {
      if (this._timer && this._timer.unref) this._timer.unref();
      return this;
    }
  }

  fs.watch = function watch(filename, options, listener) {
    const a = normalizeArgs(options, listener);
    validatePath(filename);
    validateEncoding(a.options);
    validateIgnore(a.options.ignore);
    // node lib/fs.js watch(): a TRUTHY `recursive` on Linux routes to the
    // non-native watcher (internal/fs/recursive_watch.js), whose constructor is
    // the only place the option types are checked. That asymmetry is node's, not
    // a simplification: `fs.watch(p, { persistent: 1 })` without `recursive`
    // reaches the native FSEvent and is never validated, so validating it
    // unconditionally would reject calls node accepts. Guarding the block on
    // `recursive` reproduces both halves.
    //
    // Getting this wrong presented as a HANG rather than a wrong error:
    // test-fs-watch-recursive-validate expects `{ recursive: '1' }` to throw, and
    // when it did not, the watcher it accidentally created was persistent, never
    // closed, and pinned the event loop until the corpus timeout.
    if (a.options.recursive) {
      const o = a.options;
      if (o.recursive != null) validateBoolean(o.recursive, "options.recursive");
      if (o.persistent != null) validateBoolean(o.persistent, "options.persistent");
      if (o.throwIfNoEntry != null) validateBoolean(o.throwIfNoEntry, "options.throwIfNoEntry");
      // recursive_watch.js throws ERR_INVALID_ARG_VALUE (not _TYPE) here, to
      // match what macOS/Windows report for a non-string encoding.
      if (o.encoding != null && typeof o.encoding !== "string")
        throw argValueErr("options.encoding", o.encoding, "is invalid");
    }
    return new FSWatcher(filename, a.options, a.listener);
  };

  fs.watchFile = function watchFile(filename, options, listener) {
    if (typeof options === "function") { listener = options; options = {}; }
    if (options == null) options = {};
    validatePath(filename);
    if (typeof listener !== "function") throw argTypeErr("listener", "of type function", listener);
    const path = toStr(filename);
    let w = statWatchers.get(path);
    if (!w) { w = new StatWatcher(path, options); statWatchers.set(path, w); }
    if (typeof listener === "function") w.on("change", listener);
    return w;
  };

  fs.unwatchFile = function unwatchFile(filename, listener) {
    validatePath(filename);
    const path = toStr(filename);
    const w = statWatchers.get(path);
    if (!w) return;
    if (typeof listener === "function") {
      w.removeListener("change", listener);
      if (w.listenerCount("change") > 0) return;
    }
    w.stop();
    statWatchers.delete(path);
  };

  fs.FSWatcher = FSWatcher;
  fs.StatWatcher = StatWatcher;
  // node's `fs` does not export either (they live in internal/fs/watchers and are
  // only reachable through watch()/watchFile()). Keep them reachable but out of
  // Object.keys(require("fs")) -- see bootstrap's hideFsExtras.
  if (typeof fs.__mbunHideFsExtras === "function") fs.__mbunHideFsExtras();

  // fs.promises.watch: an async iterator of { eventType, filename }. Stays ref'd
  // while open -- a `for await` consumer holds the process alive in node, and an
  // unref'd watcher would let the pump exit with the pending next() unresolved.
  // return()/close() releases the ref.
  const fsp = M["fs/promises"] || M["node:fs/promises"] || fs.promises;
  if (fsp) {
    fsp.watch = function watch(filename, options) {
      if (options !== undefined && options !== null && typeof options !== "object" && typeof options !== "string")
        throw argTypeErr("options", "of type object", options);
      const a = normalizeArgs(options, undefined);
      const opts = a.options;
      validatePath(filename);
      if (opts.persistent !== undefined && typeof opts.persistent !== "boolean")
        throw argTypeErr("options.persistent", "of type boolean", opts.persistent);
      if (opts.recursive !== undefined && typeof opts.recursive !== "boolean")
        throw argTypeErr("options.recursive", "of type boolean", opts.recursive);
      // maxQueue / overflow are validated by node's NATIVE async generator only
      // (internal/fs/watchers.js watch). The recursive path on Linux goes through
      // recursive_watch.js, which does not know these options — so the guard is
      // conditional, exactly as node's dispatch is. Unvalidated, `{ maxQueue:
      // 'silly' }` fell through to handle.start and surfaced as the start
      // failure's ENOENT instead of ERR_INVALID_ARG_TYPE (test-fs-promises-watch).
      if (!opts.recursive) {
        if (opts.maxQueue !== undefined) validateInteger(opts.maxQueue, "options.maxQueue");
        if (opts.overflow !== undefined) validateOneOf(opts.overflow, "options.overflow", ["ignore", "error"]);
      }
      validateEncoding(opts);
      validateIgnore(opts.ignore);
      if (opts.signal !== undefined && opts.signal !== null &&
          (typeof opts.signal !== "object" || !("aborted" in opts.signal)))
        throw argTypeErr("options.signal", "an instance of AbortSignal", opts.signal);
      const signal = opts.signal;
      // An ALREADY-aborted signal must not open a watcher at all: node's
      // generator throws `new AbortError(undefined, { cause: signal.reason })`
      // before `new FSEvent()`. Closing a freshly-opened watcher instead ended
      // the iteration cleanly, so `for await` completed and the awaiting
      // assert.rejects never saw a rejection (test-fs-watch-recursive-promise).
      if (signal && signal.aborted) {
        const err = abortError(signal.reason);
        return {
          [Symbol.asyncIterator]() { return this; },
          next() { return Promise.reject(err); },
          return() { return Promise.resolve({ value: undefined, done: true }); },
        };
      }
      // The signal is handled HERE, not by FSWatcher: its own abort listener is
      // registered first and would close() the watcher, resolving an outstanding
      // next() with { done: true } before this layer ever got to reject it. The
      // iterator owns the abort semantics because only it can turn an abort into
      // a rejection.
      const wopts = { __proto__: null };
      for (const k of Object.keys(opts)) { if (k !== "signal") wopts[k] = opts[k]; }
      const w = new FSWatcher(filename, wopts, undefined);
      const maxQueue = opts.maxQueue === undefined ? 2048 : opts.maxQueue;
      const overflow = opts.overflow === undefined ? "ignore" : opts.overflow;
      const queue = [];
      // A pending next()'s settle pair. `waiter` resolves a value, `failer`
      // rejects it — an abort mid-iteration has to reach the promise that is
      // already outstanding, not just the next call.
      let waiter = null;
      let failer = null;
      let done = false;
      let pendingError = null;
      const settleError = (err) => {
        if (done) return;
        pendingError = err;
        if (failer) { const f = failer; waiter = null; failer = null; f(err); }
      };
      w.on("change", (eventType, name) => {
        const rec = { eventType, filename: name };
        if (waiter) { const r = waiter; waiter = null; failer = null; r({ value: rec, done: false }); return; }
        if (queue.length < maxQueue) queue.push(rec);
        else if (overflow === "error") {
          queue.length = 0;
          settleError(Object.assign(new Error("The queue of fs.watch events has exceeded the limit of " + maxQueue),
                                    { code: "ERR_FS_WATCH_QUEUE_OVERFLOW" }));
        } else if (process.emitWarning) process.emitWarning("fs.watch maxQueue exceeded");
      });
      w.on("close", () => {
        done = true;
        if (waiter) { const r = waiter; waiter = null; failer = null; r({ value: undefined, done: true }); }
      });
      if (signal) {
        signal.addEventListener("abort", () => {
          const err = abortError(signal.reason);
          if (failer) { const f = failer; waiter = null; failer = null; w.close(); f(err); }
          else { pendingError = err; w.close(); }
        }, { once: true });
      }
      return {
        [Symbol.asyncIterator]() { return this; },
        next() {
          if (pendingError) { const e = pendingError; pendingError = null; done = true; return Promise.reject(e); }
          if (queue.length) return Promise.resolve({ value: queue.shift(), done: false });
          if (done) return Promise.resolve({ value: undefined, done: true });
          return new Promise((resolve, reject) => { waiter = resolve; failer = reject; });
        },
        return() { w.close(); return Promise.resolve({ value: undefined, done: true }); },
      };
    };
  }
})();

)JS";

}  // namespace mbun::jsc::builtins::detail
