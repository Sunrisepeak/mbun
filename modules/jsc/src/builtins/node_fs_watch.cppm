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

  // node reports names as Buffers under encoding:"buffer", strings otherwise.
  function decodeName(name, encoding) {
    if (encoding === "buffer") return G.Buffer ? G.Buffer.from(name, "utf8") : name;
    return name;
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
      this._handle = WN.start(path, !!options.recursive);
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
      for (const ev of events) {
        // node: `if (filename != null && ignoreMatcher?.(filename)) return;`
        // (watchers.js onchange / recursive_watch.js #watchFolder). ev.name is
        // already relative to the watch root for a recursive watch and the bare
        // basename otherwise — exactly what node hands the matcher — so it is
        // matched as a string, before the encoding:"buffer" conversion.
        if (this._ignoreMatcher && ev.name && this._ignoreMatcher(ev.name)) continue;
        this.emit("change", ev.kind, decodeName(ev.name, this._enc));
      }
      return events.length;
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

    stop() {
      if (this._closed) return;
      this._closed = true;
      clearInterval(this._timer);
      this.emit("close");
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
      validateEncoding(opts);
      validateIgnore(opts.ignore);
      if (opts.signal !== undefined && opts.signal !== null &&
          (typeof opts.signal !== "object" || !("aborted" in opts.signal)))
        throw argTypeErr("options.signal", "an instance of AbortSignal", opts.signal);
      const w = new FSWatcher(filename, opts, undefined);
      const queue = [];
      let waiter = null;
      let done = false;
      w.on("change", (eventType, name) => {
        const rec = { eventType, filename: name };
        if (waiter) { const r = waiter; waiter = null; r({ value: rec, done: false }); }
        else queue.push(rec);
      });
      w.on("close", () => {
        done = true;
        if (waiter) { const r = waiter; waiter = null; r({ value: undefined, done: true }); }
      });
      const signal = opts.signal;
      if (signal) {
        if (signal.aborted) w.close();
        else signal.addEventListener("abort", () => w.close(), { once: true });
      }
      return {
        [Symbol.asyncIterator]() { return this; },
        next() {
          if (queue.length) return Promise.resolve({ value: queue.shift(), done: false });
          if (done) return Promise.resolve({ value: undefined, done: true });
          return new Promise((resolve) => { waiter = resolve; });
        },
        return() { w.close(); return Promise.resolve({ value: undefined, done: true }); },
      };
    };
  }
})();

)JS";

}  // namespace mbun::jsc::builtins::detail
