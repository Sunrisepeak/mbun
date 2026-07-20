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
  }

  fs.watch = function watch(filename, options, listener) {
    const a = normalizeArgs(options, listener);
    return new FSWatcher(filename, a.options, a.listener);
  };

  fs.watchFile = function watchFile(filename, options, listener) {
    if (typeof options === "function") { listener = options; options = {}; }
    if (options == null) options = {};
    const path = toStr(filename);
    let w = statWatchers.get(path);
    if (!w) { w = new StatWatcher(path, options); statWatchers.set(path, w); }
    if (typeof listener === "function") w.on("change", listener);
    return w;
  };

  fs.unwatchFile = function unwatchFile(filename, listener) {
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

  // fs.promises.watch: an async iterator of { eventType, filename }. Stays ref'd
  // while open -- a `for await` consumer holds the process alive in node, and an
  // unref'd watcher would let the pump exit with the pending next() unresolved.
  // return()/close() releases the ref.
  const fsp = M["fs/promises"] || M["node:fs/promises"] || fs.promises;
  if (fsp) {
    fsp.watch = function watch(filename, options) {
      const a = normalizeArgs(options, undefined);
      const opts = a.options;
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
