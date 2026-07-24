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

  const kOther = Symbol("mbun.port.other");
  const kQueue = Symbol("mbun.port.queue");
  const kDetached = Symbol("mbun.port.detached");
  const kEvt = Symbol("mbun.port.domListeners");
  const kOnMsg = Symbol("mbun.port.onmessage");
  const kOnMsgErr = Symbol("mbun.port.onmessageerror");
  const kRefed = Symbol("mbun.port.refed");
  const kStarted = Symbol("mbun.port.started");
  const INSPECT_SYM = Symbol.for("nodejs.util.inspect.custom");
  // node throws a real DOMException("…", "DataCloneError") — .code === 25 and
  // `err.constructor.name === 'DOMException'` are both asserted by the corpus.
  const dataClone = (msg) => {
    if (typeof G.DOMException === "function") {
      try { return new G.DOMException(msg, "DataCloneError"); } catch (e) {}
    }
    const e = new Error(msg); e.name = "DataCloneError"; e.code = 25; return e;
  };
  const UNCLONEABLE = new WeakSet();
  const UNTRANSFERABLE = new WeakSet();

  class MessagePortBase extends EventEmitter {
    addEventListener(type, cb, opts) {
      if (typeof cb !== "function") return;
      let m = this[kEvt].get(type);
      if (!m) { m = []; this[kEvt].set(type, m); }
      m.push({ fn: cb, once: !!(opts && opts.once) });
      if (type === "message") portStart(this);
    }
    removeEventListener(type, cb) {
      const m = this[kEvt].get(type);
      if (m) { const i = m.findIndex((l) => l.fn === cb); if (i >= 0) m.splice(i, 1); }
      if (type === "message") portRecheck(this);
    }
    dispatchEvent(ev) { this.emit(ev && ev.type, ev); return true; }
    on(type, cb) { const r = super.on(type, cb); if (type === "message") portStart(this); return r; }
    addListener(type, cb) { return this.on(type, cb); }
    once(type, cb) { const r = super.once(type, cb); if (type === "message") portStart(this); return r; }
    prependListener(type, cb) { const r = super.prependListener(type, cb); if (type === "message") portStart(this); return r; }
    prependOnceListener(type, cb) { const r = super.prependOnceListener(type, cb); if (type === "message") portStart(this); return r; }
    removeListener(type, cb) { const r = super.removeListener(type, cb); if (type === "message") portRecheck(this); return r; }
    off(type, cb) { return this.removeListener(type, cb); }
    removeAllListeners(type) { const r = super.removeAllListeners(type); portRecheck(this); return r; }
    // EventEmitter listeners get the raw payload; addEventListener listeners get
    // an event object (a CustomEvent-shaped one for non-"message" types).
    emit(type, ...args) {
      const r = EventEmitter.prototype.emit.apply(this, [type].concat(args));
      const m = this[kEvt].get(type);
      if (m && m.length) {
        const first = args[0];
        const ev = (first !== null && typeof first === "object" && first.type === type)
          ? first : { type, detail: first, target: this, currentTarget: this };
        for (const l of m.slice()) { if (l.once) this.removeEventListener(type, l.fn); l.fn.call(this, ev); }
      }
      return r;
    }
  }
  Object.defineProperty(MessagePortBase.prototype, INSPECT_SYM, {
    configurable: true, writable: true,
    value: function () {
      return "MessagePort [EventTarget] { active: " + (this[kDetached] !== true) +
             ", refed: " + (this[kRefed] === true) + " }";
    },
  });

  // `MessagePort` is exposed but NOT constructible: both MessagePort() and
  // new MessagePort() are ERR_CONSTRUCT_CALL_INVALID (node io.js).
  let ALLOW_PORT_CTOR = false;
  function MessagePort() {
    if (!ALLOW_PORT_CTOR) {
      const e = new TypeError("Illegal constructor");
      e.code = "ERR_CONSTRUCT_CALL_INVALID";
      throw e;
    }
  }
  MessagePort.prototype = Object.create(MessagePortBase.prototype);
  Object.setPrototypeOf(MessagePort, MessagePortBase);
  Object.defineProperty(MessagePort.prototype, "constructor",
                        { value: MessagePort, writable: true, configurable: true });

  const newPort = () => {
    const p = new MessagePortBase();
    Object.setPrototypeOf(p, MessagePort.prototype);
    p[kEvt] = new Map();
    p[kQueue] = [];
    p[kOther] = null;
    p[kDetached] = false;
    p[kOnMsg] = null;
    p[kOnMsgErr] = null;
    p[kRefed] = false;
    p[kStarted] = false;
    return p;
  };

  const isPort = (v) => v !== null && typeof v === "object" && v[kQueue] !== undefined;
  const portHasListener = (p) => typeof p[kOnMsg] === "function" || p.listenerCount("message") > 0 ||
                                 ((p[kEvt].get("message") || []).length > 0);
  const portHasSink = (p) => p[kStarted] === true || portHasListener(p);
  // node stops delivery again when the last 'message' listener is removed
  // (setupPortReferencing's removeListener hook) — a message posted while no
  // sink exists must stay queued for a listener attached later.
  const portRecheck = (p) => { if (!portHasListener(p)) p[kStarted] = false; };
  const portDeliver = (p, data, ports) => {
    const ev = { data, type: "message", target: p, currentTarget: p, ports: ports || [] };
    const on = p[kOnMsg];
    if (typeof on === "function") on.call(p, ev);
    const dom = p[kEvt].get("message");
    if (dom && dom.length) for (const l of dom.slice()) { if (l.once) p.removeEventListener("message", l.fn); l.fn.call(p, ev); }
    EventEmitter.prototype.emit.call(p, "message", data);
  };
  const portFlush = (p) => {
    while (p[kQueue].length && portHasSink(p)) {
      const item = p[kQueue].shift();
      portDeliver(p, item.data, item.ports);
    }
  };
  // Delivery is scheduled on the MACROtask queue (setImmediate), matching node:
  // a port message arrives from the event loop, so an endless
  // .on('message')/postMessage ping-pong cannot starve timers
  // (test-worker-message-port-infinite-message-loop).
  const scheduleFlush = (p) => {
    if (typeof G.setImmediate === "function") G.setImmediate(() => portFlush(p));
    else G.queueMicrotask(() => portFlush(p));
  };
  const portStart = (p) => { p[kStarted] = true; scheduleFlush(p); };

  // ---- structured clone with a transfer list ------------------------------
  const abDetached = (ab) => {
    if (typeof ab.detached === "boolean") return ab.detached;
    try { new Uint8Array(ab); return false; } catch (e) { return true; }
  };
  const errIterable = (which) => {
    const e = new TypeError("Optional " + which + " argument must be an iterable");
    e.code = "ERR_INVALID_ARG_TYPE";
    return e;
  };
  const normTransfer = (t) => {
    if (t === undefined || t === null) return [];
    if (typeof t !== "object") throw errIterable("transferList");
    if (typeof t[Symbol.iterator] === "function") {
      try { return Array.from(t); } catch (e) { throw errIterable("transferList"); }
    }
    if ("transfer" in t) {
      const x = t.transfer;
      if (x === undefined) return [];
      if (x !== null && typeof x === "object" && typeof x[Symbol.iterator] === "function") {
        try { return Array.from(x); } catch (e) { throw errIterable("options.transfer"); }
      }
      throw errIterable("options.transfer");
    }
    return [];
  };
  const PORT_TOK = "__mbunTransferredPort__";
  const subst = (v, ports, seen) => {
    if (v === null || typeof v !== "object") return v;
    const i = ports.indexOf(v);
    if (i >= 0) { const o = {}; o[PORT_TOK] = i; return o; }
    if (seen.has(v)) return seen.get(v);
    if (Array.isArray(v)) { const out = []; seen.set(v, out); for (let k = 0; k < v.length; k++) out[k] = subst(v[k], ports, seen); return out; }
    const proto = Object.getPrototypeOf(v);
    if (proto !== Object.prototype && proto !== null) return v;
    const out = {}; seen.set(v, out);
    for (const k of Object.keys(v)) out[k] = subst(v[k], ports, seen);
    return out;
  };
  const unsubst = (v, ports, seen) => {
    if (v === null || typeof v !== "object") return v;
    if (Object.prototype.hasOwnProperty.call(v, PORT_TOK) && typeof v[PORT_TOK] === "number") return ports[v[PORT_TOK]];
    if (seen.has(v)) return seen.get(v);
    if (Array.isArray(v)) { seen.set(v, v); for (let k = 0; k < v.length; k++) v[k] = unsubst(v[k], ports, seen); return v; }
    const proto = Object.getPrototypeOf(v);
    if (proto !== Object.prototype && proto !== null) return v;
    seen.set(v, v);
    for (const k of Object.keys(v)) v[k] = unsubst(v[k], ports, seen);
    return v;
  };
  // Clone FIRST, detach after: a transferred ArrayBuffer is frequently also the
  // backing store of the message itself (postMessage(typedArray, [ab])), and
  // detaching before the copy would hand the receiver an empty view.
  const cloneWithPorts = (value, ports, buffers) => {
    const pre = ports.length ? subst(value, ports, new Map()) : value;
    const c = G.structuredClone(pre);
    for (const b of buffers) { try { G.structuredClone(b, { transfer: [b] }); } catch (e) {} }
    return ports.length ? unsubst(c, ports, new Map()) : c;
  };

  const severPort = (p) => {
    const o = p[kOther];
    p[kDetached] = true; p[kOther] = null;
    if (o) { o[kDetached] = true; o[kOther] = null; }
  };

  const defPortProp = (name, desc) => Object.defineProperty(MessagePort.prototype, name,
      Object.assign({ configurable: true }, desc));

  defPortProp("postMessage", { writable: true, value: function (value, transferList) {
    const list = normTransfer(transferList);
    const ports = [], buffers = [];
    const seenPorts = new Set(), seenBufs = new Set();
    // node validates the WHOLE list before detaching anything, so a bad entry
    // late in the list leaves earlier ArrayBuffers untouched.
    for (const item of list) {
      if (isPort(item)) {
        if (item === this) throw dataClone("Transfer list contains source port");
        if (seenPorts.has(item)) throw dataClone("Transfer list contains duplicate MessagePort");
        if (item[kDetached] === true) throw dataClone("MessagePort in transfer list is already detached");
        seenPorts.add(item); ports.push(item);
      } else if (item instanceof ArrayBuffer) {
        if (seenBufs.has(item)) throw dataClone("Transfer list contains duplicate ArrayBuffer");
        if (abDetached(item)) throw dataClone("ArrayBuffer at index " + buffers.length + " is already detached");
        if (UNTRANSFERABLE.has(item)) continue;  // markAsUntransferable: clone, don't move
        seenBufs.add(item); buffers.push(item);
      } else if (item !== null && (typeof item === "object" || typeof item === "function")) {
        if (UNTRANSFERABLE.has(item)) continue;
        throw dataClone("Object that needs transfer was found in message but not listed in transferList");
      } else {
        throw dataClone("Value at index " + buffers.length + " is not transferable");
      }
    }
    if (this[kDetached] === true) return;
    const target = this[kOther];
    const postedToTarget = target !== null && seenPorts.has(target);
    const cloned = cloneWithPorts(value, ports, buffers);
    if (postedToTarget) {
      // node node_messaging.cc: the channel is lost and a process warning fires.
      if (G.process && typeof G.process.emitWarning === "function") {
        G.process.emitWarning("The target port was posted to itself, and the communication channel was lost");
      }
      severPort(this);
      return;
    }
    if (target === null) return;
    target[kQueue].push({ data: cloned, ports: ports.slice() });
    scheduleFlush(target);
  } });

  // Detachment is synchronous and instant; the 'close' event lands on BOTH ends
  // asynchronously (node node_messaging.cc MessagePort::Close).
  defPortProp("close", { writable: true, value: function (cb) {
    if (typeof cb === "function") this.once("close", cb);
    if (this[kDetached] === true) return;
    const other = this[kOther];
    this[kDetached] = true;
    this[kOther] = null;
    const self = this;
    G.queueMicrotask(() => self.emit("close"));
    if (other) {
      other[kOther] = null;
      G.queueMicrotask(() => {
        if (other[kDetached] === true) return;
        other[kDetached] = true;
        other.emit("close");
      });
    }
  } });
  defPortProp("start", { writable: true, value: function () { portStart(this); } });
  defPortProp("ref", { writable: true, value: function () { this[kRefed] = true; } });
  defPortProp("unref", { writable: true, value: function () { this[kRefed] = false; } });
  defPortProp("hasRef", { writable: true, value: function () { return this[kRefed] === true; } });
  defPortProp("onmessage", {
    get() { return this[kOnMsg]; },
    set(v) { this[kOnMsg] = v; if (typeof v === "function") portStart(this); else portRecheck(this); },
  });
  defPortProp("onmessageerror", { get() { return this[kOnMsgErr]; }, set(v) { this[kOnMsgErr] = v; } });

  function MessageChannel() {
    if (new.target === undefined) {
      const e = new TypeError("Cannot call constructor without `new`");
      e.code = "ERR_CONSTRUCT_CALL_REQUIRED";
      throw e;
    }
    const p1 = newPort(), p2 = newPort();
    p1[kOther] = p2; p2[kOther] = p1;
    this.port1 = p1;
    this.port2 = p2;
  }

  const receiveMessageOnPort = function (port) {
    if (!isPort(port)) {
      const e = new TypeError('The "port" argument must be a MessagePort instance');
      e.code = "ERR_INVALID_ARG_TYPE";
      throw e;
    }
    if (port[kQueue].length) return { message: port[kQueue].shift().data };
    return undefined;
  };

  // node marks an object so a later postMessage clones it instead of moving it;
  // markAsUncloneable makes structured cloning of it fail with DataCloneError.
  const markAsUntransferable = function (obj) {
    if (obj !== null && (typeof obj === "object" || typeof obj === "function")) UNTRANSFERABLE.add(obj);
    return undefined;
  };
  const markAsUncloneable = function (obj) {
    if (obj !== null && (typeof obj === "object" || typeof obj === "function")) UNCLONEABLE.add(obj);
    return undefined;
  };
  {
    const nativeSC = G.structuredClone;
    if (typeof nativeSC === "function" && !nativeSC.__mbunUncloneableAware) {
      const wrapped = function structuredClone(value, options) {
        // node: markAsUncloneable has no effect on (Shared)ArrayBuffer.
        if (value !== null && (typeof value === "object" || typeof value === "function") &&
            !(value instanceof ArrayBuffer) &&
            !(typeof G.SharedArrayBuffer === "function" && value instanceof G.SharedArrayBuffer) &&
            UNCLONEABLE.has(value)) {
          throw dataClone(String(value) + " could not be cloned.");
        }
        return nativeSC.call(this, value, options);
      };
      wrapped.__mbunUncloneableAware = true;
      G.structuredClone = wrapped;
    }
  }
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
    markAsUncloneable,
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
  // node's global MessageChannel/MessagePort ARE the worker_threads ones (they
  // are re-exported onto globalThis since v15), so the node-parity classes win
  // over the load-order web stubs.
  G.MessageChannel = MessageChannel;
  G.MessagePort = MessagePort;
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
