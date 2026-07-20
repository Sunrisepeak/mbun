// node:worker_threads JS layer partition.
//
// Overwrites the pure-JS worker_threads stub registered in bootstrap with a
// fuller node:worker_threads surface: isMainThread/threadId/parentPort/
// workerData/resourceLimits/SHARE_ENV constants, node-style MessageChannel/
// MessagePort with a FIFO message queue (so receiveMessageOnPort drains
// synchronously without firing "message" listeners), BroadcastChannel,
// setEnvironmentData/getEnvironmentData, and the markAsUntransferable/
// moveMessagePortToContext "not yet implemented" throws that node exposes.
//
// DEFERRED: real cross-thread Worker execution. A Worker in node:worker_threads
// runs its entry module on a NEW OS thread inside an independent JSC context,
// exchanging structured-cloned messages over the platform event loop. mbun's
// runtime is a single main-thread event-loop pump (runtime/engine.inc), with no
// per-thread JSC context / cross-thread message plumbing, so a Worker here only
// validates its options (transferList type-checks + ArrayBuffer detach) and
// emits the process "worker" event on the next tick; it never executes the
// worker script. Message/exit-driven tests therefore stay failing (honestly),
// pending a threaded event-loop seam in the engine.
//
// NOTE: appended AFTER the master builtins IIFE (opened in bootstrap, closed by
// image_closure), so this is a self-contained IIFE that re-binds G = globalThis
// and must not rely on the outer IIFE's aliases.
//
// Blueprint: bun src/js/node/worker_threads.ts, src/bun.js/api/bun/subprocess
// worker plumbing, node lib/internal/worker.js (transferList validation, the
// environmentData Map, receiveMessageOnPort FIFO contract).
export module mbun.jsc.js_builtins:node_worker;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeWorkerJS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  if (!M) return;
  const EventEmitter = M["events"] || M["node:events"];
  if (typeof EventEmitter !== "function") return;
  const proc = G.process || {};

  const SHARE_ENV = Symbol("nodejs.worker_threads.SHARE_ENV");

  // ---- environmentData (main-thread store) --------------------------------
  const environmentData = new Map();
  const setEnvironmentData = function (key, value) {
    if (value === undefined) environmentData.delete(key);
    else environmentData.set(key, value);
  };
  const getEnvironmentData = function (key) { return environmentData.get(key); };

  // ---- node-style MessagePort / MessageChannel ----------------------------
  // Distinct from the DOM MessagePort global (which delivers only via onmessage
  // microtasks): node ports keep a FIFO queue that receiveMessageOnPort drains
  // synchronously, and only auto-emit "message" once a listener/onmessage is
  // attached and a microtask flushes the queue.
  const clone = (data) => {
    if (typeof G.structuredClone === "function") {
      try { return G.structuredClone(data); } catch (e) { return data; }
    }
    return data;
  };

  class MessagePort extends EventEmitter {
    constructor() {
      super();
      this._other = null;
      this._queue = [];
      this._started = false;
      this.onmessage = null;
      this.onmessageerror = null;
    }
    _hasSink() {
      return this._started || typeof this.onmessage === "function" || this.listenerCount("message") > 0;
    }
    _flush() {
      while (this._queue.length && this._hasSink()) {
        const message = this._queue.shift();
        const ev = { data: message, type: "message" };
        if (typeof this.onmessage === "function") this.onmessage(ev);
        this.emit("message", message);
      }
    }
    postMessage(value) {
      const o = this._other;
      if (!o) return;
      o._queue.push(clone(value));
      G.queueMicrotask(() => o._flush());
    }
    start() { this._started = true; G.queueMicrotask(() => this._flush()); }
    close() { this._other = null; this.emit("close"); }
    ref() { return this; }
    unref() { return this; }
    addEventListener(type, cb) { this.on(type, cb); if (type === "message") this.start(); }
    removeEventListener(type, cb) { this.off(type, cb); }
  }
  // Attaching a "message" listener implicitly starts delivery (node/DOM parity).
  const _origOn = MessagePort.prototype.on;
  MessagePort.prototype.on = function (type, cb) {
    const r = _origOn.call(this, type, cb);
    if (type === "message") { this._started = true; G.queueMicrotask(() => this._flush()); }
    return r;
  };
  MessagePort.prototype.addListener = MessagePort.prototype.on;

  class MessageChannel {
    constructor() {
      this.port1 = new MessagePort();
      this.port2 = new MessagePort();
      this.port1._other = this.port2;
      this.port2._other = this.port1;
    }
  }

  const receiveMessageOnPort = function (port) {
    if (port === null || typeof port !== "object" || !(port instanceof MessagePort)) {
      const e = new TypeError('The "port" argument must be a MessagePort instance');
      e.code = "ERR_INVALID_ARG_TYPE";
      throw e;
    }
    if (port._queue && port._queue.length) return { message: port._queue.shift() };
    return undefined;
  };

  const markAsUntransferable = function () { throw new Error("markAsUntransferable is not yet implemented in Bun"); };
  const moveMessagePortToContext = function () { throw new Error("moveMessagePortToContext is not yet implemented in Bun"); };

  // ---- BroadcastChannel (in-process fan-out) ------------------------------
  const BroadcastChannel = G.BroadcastChannel || (function () {
    const channels = new Map();
    class BroadcastChannel extends EventEmitter {
      constructor(name) {
        super();
        this.name = String(name);
        this.onmessage = null;
        this.onmessageerror = null;
        this._closed = false;
        let set = channels.get(this.name);
        if (!set) { set = new Set(); channels.set(this.name, set); }
        set.add(this);
      }
      postMessage(value) {
        if (this._closed) throw new Error("BroadcastChannel is closed");
        const set = channels.get(this.name);
        if (!set) return;
        const data = clone(value);
        for (const ch of set) {
          if (ch === this || ch._closed) continue;
          G.queueMicrotask(() => {
            const ev = { data, type: "message" };
            if (typeof ch.onmessage === "function") ch.onmessage(ev);
            ch.emit("message", ev);
          });
        }
      }
      close() {
        if (this._closed) return;
        this._closed = true;
        const set = channels.get(this.name);
        if (set) { set.delete(this); if (set.size === 0) channels.delete(this.name); }
      }
      ref() { return this; }
      unref() { return this; }
      addEventListener(type, cb) { this.on(type, cb); }
      removeEventListener(type, cb) { this.off(type, cb); }
    }
    return BroadcastChannel;
  })();

  // ---- Worker (DEFERRED: no real cross-thread execution) ------------------
  let nextThreadId = 1;
  const validateTransferList = (transferList) => {
    if (transferList === undefined || transferList === null) return [];
    if (!Array.isArray(transferList)) {
      const e = new TypeError('The "options.transferList" property must be an instance of Array.');
      e.code = "ERR_INVALID_ARG_TYPE";
      throw e;
    }
    // Node validates every entry up-front (throws before detaching any of them).
    for (const item of transferList) {
      if (item === null || (typeof item !== "object" && typeof item !== "function")) {
        const e = new TypeError("Found invalid object in transferList");
        e.code = "ERR_INVALID_ARG_TYPE";
        throw e;
      }
    }
    return transferList;
  };

  // ---- Worker (REAL cross-thread execution via the native seam) -----------
  // globalThis.__mbunWorkerNative (runtime/worker.inc) spawns a second JSC VM on
  // its own OS thread. Messages cross the thread boundary as the JSON subset of
  // structured clone (the JSC C API exposes no SerializedScriptValue): the wire
  // handles primitives / arrays / plain objects; transferables, Date/Map/Set and
  // cyclic graphs are out of range and their tests stay honestly red.
  const WN = G.__mbunWorkerNative;
  const workerRegistry = new Map();  // threadId → Worker (parent side)

  // Resolve an entry (URL/path/URL-object) to a source string the worker runs.
  const readWorkerSource = (filename) => {
    let p = filename;
    if (p && typeof p === "object" && typeof p.href === "string") p = p.href;  // URL
    p = String(p);
    // data: URL entry point (node lib/internal/worker.js accepts one, and the
    // web Worker constructor takes any URL): the source *is* the payload.
    // RFC 2397 — `;base64` after the mediatype means base64, otherwise the
    // payload is percent-encoded.
    if (p.startsWith("data:")) {
      const comma = p.indexOf(",");
      if (comma === -1) {
        const e = new TypeError("Invalid URL: " + p);
        e.code = "ERR_INVALID_URL";
        throw e;
      }
      const meta = p.slice(5, comma);
      const payload = p.slice(comma + 1);
      if (/;base64\s*$/i.test(meta)) {
        if (G.Buffer) return G.Buffer.from(payload, "base64").toString("utf8");
        return G.atob ? G.atob(payload) : payload;
      }
      try { return decodeURIComponent(payload); } catch (e) { return payload; }
    }
    if (p.startsWith("file://")) {
      const u = M["url"] || M["node:url"];
      if (p.indexOf(":!:") !== -1 || /file:\/\/[^/]*:/.test(p)) {
        const e = new TypeError("Invalid file URL: " + p);
        e.code = "ERR_INVALID_URL";
        throw e;
      }
      try { p = u && u.fileURLToPath ? u.fileURLToPath(p) : p.slice(7); }
      catch (e) { p = p.slice(7); }
    }
    const fs = M["fs"] || M["node:fs"];
    if (!fs || typeof fs.readFileSync !== "function") throw new Error("worker: fs unavailable");
    return fs.readFileSync(p, "utf8");
  };

  class Worker extends EventEmitter {
    constructor(filename, options) {
      super();
      options = options || {};
      const transferList = validateTransferList(options.transferList);
      // Detach transferables + validate workerData exactly like node does before
      // the thread starts (observable side effects), then marshal workerData.
      let workerDataJson = null;
      if (options.workerData !== undefined || transferList.length) {
        try { G.structuredClone(options.workerData ?? null, { transfer: transferList }); }
        catch (e) { /* clone failures surface as a non-detaching no-op */ }
      }
      try { workerDataJson = JSON.stringify(options.workerData ?? null); }
      catch (e) { workerDataJson = null; }

      const source = options.eval ? String(filename) : readWorkerSource(filename);

      this.stdin = null;
      this.stdout = null;
      this.stderr = null;
      this.performance = { eventLoopUtilization: () => ({ idle: 0, active: 0, utilization: 0 }) };
      this.resourceLimits = {};
      this.onmessage = null;
      this.onmessageerror = null;
      this.onerror = null;
      this._refd = true;
      this._exited = false;
      this._exitCode = null;
      this._exitResolvers = [];

      this.threadId = WN.spawn(source, workerDataJson == null ? "" : workerDataJson) | 0;
      workerRegistry.set(this.threadId, this);
      armWorkerPump();  // start delivering worker→parent events via the timer loop

      // node emits the process "worker" event on the next tick.
      const self = this;
      const emitWorker = () => { const p = G.process; if (p && typeof p.emit === "function") p.emit("worker", self); };
      if (proc.nextTick) proc.nextTick(emitWorker); else G.queueMicrotask(emitWorker);
    }
    postMessage(value) { WN.post(this.threadId, value); return undefined; }
    terminate() {
      WN.terminate(this.threadId);
      if (this._exited) return Promise.resolve(this._exitCode == null ? 0 : this._exitCode);
      return new Promise((resolve) => { this._exitResolvers.push(resolve); });
    }
    ref() { this._refd = true; return this; }
    unref() { this._refd = false; return this; }
    addEventListener(type, cb) { this.on(type, cb); }
    removeEventListener(type, cb) { this.off(type, cb); }
    getHeapSnapshot() { return Promise.reject(new Error("Worker.getHeapSnapshot is not supported in this build")); }
    [Symbol.asyncDispose]() { return this.terminate().then(() => undefined); }
  }

  // Deliver worker→parent events (drained natively) to the JS Worker instances.
  // Invoked once per parent event-loop pump iteration (engine.inc pump).
  G.__mbunWorkerDrain = () => {
    if (!WN) return;
    let events;
    try { events = JSON.parse(WN.drain()); } catch (e) { return; }
    for (const ev of events) {
      const w = workerRegistry.get(ev.id);
      if (!w) continue;
      if (ev.t === "message") {
        const msgEv = { data: ev.d, type: "message", ports: [], target: w };
        if (typeof w.onmessage === "function") { try { w.onmessage(msgEv); } catch (e) {} }
        w.emit("message", ev.d);
      } else if (ev.t === "error") {
        const err = new Error(String(ev.d));
        if (typeof w.onerror === "function") { try { w.onerror(err); } catch (e) {} }
        if (w.listenerCount("error") > 0) w.emit("error", err);
      } else if (ev.t === "exit") {
        w._exited = true;
        w._exitCode = ev.d | 0;
        workerRegistry.delete(ev.id);
        w.emit("exit", w._exitCode);
        const rs = w._exitResolvers.splice(0);
        for (const r of rs) r(w._exitCode);
      }
    }
  };
  // A ref'd, still-running worker pins the parent event loop (engine.inc pump).
  G.__mbunWorkerActiveRefd = () => {
    let n = 0;
    for (const w of workerRegistry.values()) if (w._refd && !w._exited) n++;
    return n;
  };

  // Drive worker→parent delivery through the EXISTING timer system (setTimeout),
  // which both the main engine pump AND the bun:test runner already drain — so a
  // Worker message reaches an awaiting test without touching either hot loop
  // (a direct test-runner drain hook wedged unrelated same-thread MessagePort
  // tests). The self-rearming tick lives only while a Worker is registered, so a
  // process/test file that never spawns a Worker is completely unaffected.
  let workerPumpArmed = false;
  const armWorkerPump = () => {
    if (workerPumpArmed) return;
    workerPumpArmed = true;
    const tick = () => {
      try { G.__mbunWorkerDrain(); } catch (e) {}
      if (workerRegistry.size > 0) G.setTimeout(tick, 1);  // re-arm while workers live
      else workerPumpArmed = false;                        // all workers done → stop
    };
    G.setTimeout(tick, 1);
  };

  const mod = {
    Worker,
    isMainThread: true,
    parentPort: null,
    threadId: 0,
    workerData: null,
    resourceLimits: {},
    MessageChannel,
    MessagePort,
    BroadcastChannel,
    receiveMessageOnPort,
    markAsUntransferable,
    moveMessagePortToContext,
    setEnvironmentData,
    getEnvironmentData,
    SHARE_ENV,
  };

  M["worker_threads"] = M["node:worker_threads"] = mod;

  // Expose the web-platform globals the worker tests use unqualified. Worker is
  // real; MessageChannel/MessagePort already exist as DOM globals (web layer),
  // so only fill Worker (and back-fill the others if a build lacks them).
  if (WN && typeof G.Worker === "undefined") G.Worker = Worker;
  if (typeof G.MessageChannel === "undefined") G.MessageChannel = MessageChannel;
  if (typeof G.MessagePort === "undefined") G.MessagePort = MessagePort;
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
