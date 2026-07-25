// node:worker_threads JS layer partition.
//
// Provides the node:worker_threads surface: isMainThread/threadId/parentPort/
// workerData/resourceLimits/SHARE_ENV, a node-parity MessageChannel/MessagePort
// (transfer lists, DataCloneError DOMExceptions, synchronous detach with an
// asynchronous 'close' on both ends, receiveMessageOnPort's FIFO contract),
// BroadcastChannel, markAsUntransferable/markAsUncloneable,
// setEnvironmentData/getEnvironmentData, and a real Worker.
//
// Worker execution model: one CHILD mbun process per Worker, driven over the
// node:child_process fork() IPC channel. A Worker needs the full runtime — its
// own module loader, node builtins, process.env, __filename — because the
// corpus overwhelmingly does `new Worker(__filename)` and re-enters the same
// test file. mbun's runtime is a process-wide singleton (one JSC VM, one
// event-loop pump, process-global DNS/net/timer state), so a second in-process
// VM could only ever carry a hand-written subset of that surface; a child
// process carries all of it, and the message wire is the same JSON
// structured-clone subset either way.
//
// DEFERRED, and honestly red: SharedArrayBuffer/Atomics shared across the
// boundary, transferring a MessagePort into a Worker, resourceLimits, heap
// snapshots / CPU profiles, and terminate()'s "stop mid-microtask" guarantee
// (the child is signalled instead). runtime/worker.inc still registers the old
// second-VM-on-a-thread seam as __mbunWorkerNative; nothing consumes it now and
// it should be retired once no branch in flight depends on it.
//
// NOTE: appended AFTER the master builtins IIFE (opened in bootstrap, closed by
// image_closure), so this is a self-contained IIFE that re-binds G = globalThis
// and must not rely on the outer IIFE's aliases.
//
// Blueprint: node lib/internal/worker.js + lib/internal/worker/io.js +
// src/node_messaging.cc; bun src/js/node/worker_threads.ts.
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

  // transferList validation shared by Worker#postMessage and the constructor.
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

  // ---- Worker (real execution: one mbun process per worker) ---------------
  // A node Worker needs the FULL runtime — its own module loader, node
  // builtins, process.env, __filename — because the corpus overwhelmingly does
  // `new Worker(__filename)` and re-enters the same test file. mbun's runtime is
  // a process-wide singleton (one JSC VM, one event-loop pump, process-global
  // DNS/net/timer state), so a second in-process VM can only ever carry a
  // hand-written subset of that surface. Running the entry in a CHILD mbun
  // process over the fork() IPC channel gives it the real thing; messages are
  // the same JSON structured-clone subset either way.
  // DEFERRED, and honestly red: SharedArrayBuffer/Atomics shared between the
  // two sides, transferring a MessagePort across the boundary, resourceLimits,
  // heap snapshots/CPU profiles, and Worker.terminate()'s "stop mid-microtask"
  // guarantee (the child is signalled instead).
  const CPM = M["child_process"] || M["node:child_process"];
  const pathM = M["path"] || M["node:path"];
  const fsM = M["fs"] || M["node:fs"];
  const osM = M["os"] || M["node:os"];
  const workerRegistry = new Map();  // threadId -> Worker (parent side)
  let nextThreadId = 1;

  const recvType = (v) => {
    if (v === null) return "null";
    if (v === undefined) return "undefined";
    const t = typeof v;
    if (t === "symbol") return "type symbol (" + String(v) + ")";
    if (t === "string") return "type string ('" + v + "')";
    if (t === "object") return "an instance of " + ((v.constructor && v.constructor.name) || "Object");
    return "type " + t + " (" + String(v) + ")";
  };

  const dataUrlSource = (p) => {
    const comma = p.indexOf(",");
    if (comma === -1) { const e = new TypeError("Invalid URL: " + p); e.code = "ERR_INVALID_URL"; throw e; }
    const meta = p.slice(5, comma);
    const payload = p.slice(comma + 1);
    if (/;base64\s*$/i.test(meta)) {
      if (G.Buffer) return G.Buffer.from(payload, "base64").toString("utf8");
      return G.atob ? G.atob(payload) : payload;
    }
    try { return decodeURIComponent(payload); } catch (e) { return payload; }
  };

  // node ERR_WORKER_PATH: a bare specifier is not a worker entry point.
  const workerEntryPath = (filename) => {
    let p = filename;
    if (p !== null && typeof p === "object" && typeof p.href === "string") {
      if (p.protocol === "data:") return { source: dataUrlSource(p.href) };
      if (p.protocol !== "file:") {
        const e = new TypeError("The URL must be of scheme file: Received protocol '" + p.protocol + "'");
        e.code = "ERR_UNSUPPORTED_ESM_URL_SCHEME"; throw e;
      }
      const u = M["url"] || M["node:url"];
      return { path: u && u.fileURLToPath ? u.fileURLToPath(p) : p.pathname };
    }
    if (G.Buffer && typeof G.Buffer.isBuffer === "function" && G.Buffer.isBuffer(p)) p = p.toString();
    if (typeof p !== "string") {
      const e = new TypeError('The "filename" argument must be of type string or an instance of URL. Received ' + recvType(filename));
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    if (p.startsWith("data:")) return { source: dataUrlSource(p) };
    if (p.startsWith("file://")) {
      const u = M["url"] || M["node:url"];
      try { return { path: u && u.fileURLToPath ? u.fileURLToPath(p) : p.slice(7) }; }
      catch (e) { const err = new TypeError("Invalid file URL: " + p); err.code = "ERR_INVALID_URL"; throw err; }
    }
    if (!pathM.isAbsolute(p) && !/^\.\.?[/\\]/.test(p)) {
      const e = new TypeError("The worker script or module filename must be an absolute path or a relative path starting with './' or '../'. Received \"" + p + "\"");
      e.code = "ERR_WORKER_PATH"; throw e;
    }
    return { path: pathM.resolve(p) };
  };

  const writeTempWorker = (source, tid, ext) => {
    const dir = osM && osM.tmpdir ? osM.tmpdir() : "/tmp";
    const p = pathM.join(dir, "mbun-worker-" + (proc.pid || 0) + "-" + tid + (ext || ".js"));
    fsM.writeFileSync(p, source);
    return p;
  };

  class Worker extends EventEmitter {
    constructor(filename, options) {
      super();
      // Permission Model: node gates the WorkerThreads scope in Worker::New
      // (src/node_worker.cc), before anything is created. This check only decides
      // WHICH error is reported: mbun runs a worker as a child mbun process, and
      // that spawn is gated in C++ (permission_deny_spawn) whether or not this
      // line exists — without it the caller would see the ChildProcess denial
      // for what is really a worker.
      {
        const PN = G.__mbunPermissionNative;
        if (PN && PN.enabled && !PN.has("worker")) throw PN.denyError("worker", "");
      }
      options = options || {};
      validateTransferList(options.transferList);
      if (options.env !== undefined && options.env !== null && options.env !== SHARE_ENV &&
          (typeof options.env !== "object" || Array.isArray(options.env))) {
        const e = new TypeError('The "options.env" property must be of type object or one of undefined, null, or worker_threads.SHARE_ENV. Received ' + recvType(options.env));
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      if (options.argv !== undefined && options.argv !== null && !Array.isArray(options.argv)) {
        const e = new TypeError('The "options.argv" property must be an instance of Array. Received ' + recvType(options.argv));
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      if (options.execArgv !== undefined && options.execArgv !== null && !Array.isArray(options.execArgv)) {
        const e = new TypeError('The "options.execArgv" property must be an instance of Array. Received ' + recvType(options.execArgv));
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      // node clones workerData up-front, so an unclonable value throws from the
      // constructor instead of silently reaching the worker as null.
      if (options.workerData !== undefined) G.structuredClone(options.workerData);
      let workerDataJson = "null";
      try { workerDataJson = JSON.stringify(options.workerData === undefined ? null : options.workerData); }
      catch (e) { throw dataClone(String(options.workerData) + " could not be cloned."); }
      if (workerDataJson === undefined) throw dataClone(String(options.workerData) + " could not be cloned.");

      const tid = nextThreadId++;
      this.threadId = tid;
      this._tempFile = null;
      let entry;
      if (options.eval) {
        entry = writeTempWorker(String(filename), tid, ".js");
        this._tempFile = entry;
      } else {
        const r = workerEntryPath(filename);
        if (r.source !== undefined) { entry = writeTempWorker(r.source, tid, ".js"); this._tempFile = entry; }
        else entry = r.path;
      }

      // Env: SHARE_ENV and the default both inherit; an explicit object replaces.
      const baseEnv = (options.env && options.env !== SHARE_ENV) ? options.env : (proc.env || {});
      const env = {};
      for (const k of Object.keys(baseEnv)) { const v = baseEnv[k]; if (v !== undefined && v !== null) env[k] = String(v); }
      env.MBUN_WORKER_TID = String(tid);
      env.MBUN_WORKER_DATA = workerDataJson;

      const execArgv = Array.isArray(options.execArgv) ? options.execArgv.map(String)
                                                       : ((proc.execArgv || []).map(String));
      const argv = Array.isArray(options.argv) ? options.argv.map(String) : [];
      const child = CPM.spawn(String(proc.execPath || "mbun"),
                              execArgv.concat([entry], argv),
                              { env, stdio: ["pipe", "pipe", "pipe", "ipc"] });
      this._child = child;
      this._exited = false;
      this._exitCode = null;
      this._exitResolvers = [];
      this._refd = true;
      this.resourceLimits = {};
      this.performance = { eventLoopUtilization: () => ({ idle: 0, active: 0, utilization: 0 }) };
      this.onmessage = null;
      this.onmessageerror = null;
      this.onerror = null;
      // node: the worker's stdio is piped into the parent's unless the caller
      // asked for the streams (options.stdout / options.stderr / options.stdin).
      this.stdin = options.stdin ? child.stdin : null;
      if (!options.stdin && child.stdin) { try { child.stdin.end(); } catch (e) {} }
      this.stdout = child.stdout;
      this.stderr = child.stderr;
      if (!options.stdout && child.stdout) child.stdout.on("data", (d) => { try { proc.stdout.write(d); } catch (e) {} });
      if (!options.stderr && child.stderr) child.stderr.on("data", (d) => { try { proc.stderr.write(d); } catch (e) {} });
      workerRegistry.set(tid, this);

      const self = this;
      child.on("spawn", () => self.emit("online"));
      child.on("message", (m) => {
        if (m === null || typeof m !== "object") return;
        if (m.t === "m") {
          const ev = { data: m.d, type: "message", ports: [], target: self };
          if (typeof self.onmessage === "function") self.onmessage(ev);
          self.emit("message", m.d);
        } else if (m.t === "e") {
          const err = new Error(m.d && m.d.message ? m.d.message : String(m.d));
          if (m.d && m.d.name) err.name = m.d.name;
          if (m.d && m.d.stack) err.stack = m.d.stack;
          if (m.d && m.d.code) err.code = m.d.code;
          if (typeof self.onerror === "function") self.onerror(err);
          self.emit("error", err);
        } else if (m.t === "me") {
          self.emit("messageerror", new Error(String(m.d)));
        }
      });
      child.on("error", (e) => self.emit("error", e));
      child.on("exit", (code, signal) => {
        self._exited = true;
        self._exitCode = signal ? 1 : (code == null ? 1 : code);
        workerRegistry.delete(tid);
        if (self._tempFile) { try { fsM.unlinkSync(self._tempFile); } catch (e) {} self._tempFile = null; }
        self.emit("exit", self._exitCode);
        const rs = self._exitResolvers.splice(0);
        for (const r of rs) r(self._exitCode);
      });
      const emitWorker = () => { if (proc && typeof proc.emit === "function") proc.emit("worker", self); };
      if (proc.nextTick) proc.nextTick(emitWorker); else G.queueMicrotask(emitWorker);
      // node internal/worker.js: the constructor's last act is to announce the
      // worker on the 'worker_threads' diagnostics channel. The lookup is lazy
      // because this partition is assembled before node:diagnostics_channel.
      {
        const d = M["diagnostics_channel"] || M["node:diagnostics_channel"];
        if (d && typeof d.channel === "function") {
          const ch = d.channel("worker_threads");
          if (ch.hasSubscribers) ch.publish({ worker: this });
        }
      }
    }
    postMessage(value, transferList) {
      validateTransferList(transferList);
      if (this._exited || !this._child.connected) return undefined;
      // `{t:"m"}` with no `d` IS the undefined message: JSON drops the key.
      try { this._child.send(value === undefined ? { t: "m" } : { t: "m", d: value }); } catch (e) {}
      return undefined;
    }
    terminate() {
      if (this._exited) return Promise.resolve(this._exitCode == null ? 1 : this._exitCode);
      try { this._child.kill("SIGTERM"); } catch (e) {}
      return new Promise((resolve) => { this._exitResolvers.push(resolve); });
    }
    ref() { this._refd = true; if (this._child._rec) this._child._rec.unrefd = false; return this; }
    unref() { this._refd = false; if (this._child._rec) this._child._rec.unrefd = true; return this; }
    addEventListener(type, cb) { this.on(type, cb); }
    removeEventListener(type, cb) { this.off(type, cb); }
    getHeapSnapshot() { return Promise.reject(new Error("Worker.getHeapSnapshot is not supported in this build")); }
    [Symbol.asyncDispose]() { return this.terminate().then(() => undefined); }
  }

  // ---- worker side: this process IS a worker (MBUN_WORKER_TID) ------------
  const WENV = proc.env || {};
  const MY_TID = WENV.MBUN_WORKER_TID;
  let isMainThread = true;
  let threadId = 0;
  let parentPort = null;
  let workerData = null;
  if (MY_TID !== undefined && MY_TID !== null && MY_TID !== "") {
    isMainThread = false;
    threadId = Number(MY_TID) | 0;
    try { workerData = JSON.parse(WENV.MBUN_WORKER_DATA || "null"); } catch (e) { workerData = null; }
    try { delete proc.env.MBUN_WORKER_TID; delete proc.env.MBUN_WORKER_DATA; } catch (e) {}
    const chan = new MessageChannel();
    parentPort = chan.port1;
    Object.defineProperty(parentPort, "postMessage", {
      configurable: true, writable: true,
      value: function (value) {
        if (typeof proc.send !== "function") return undefined;
        try { proc.send(value === undefined ? { t: "m" } : { t: "m", d: value }); } catch (e) {}
        return undefined;
      },
    });
    if (typeof proc.on === "function") {
      proc.on("message", (m) => { if (m !== null && typeof m === "object" && m.t === "m") chan.port2.postMessage(m.d); });
      // An uncaught throw inside a worker surfaces as an 'error' event on the
      // parent's Worker handle, not as a bare non-zero exit (node worker.js).
      proc.on("uncaughtException", (e) => {
        try { proc.send({ t: "e", d: { message: e && e.message, name: e && e.name, stack: e && e.stack, code: e && e.code } }); } catch (_) {}
        proc.exit(1);
      });
    }
    // The channel pins this process's event loop only while parentPort has a
    // sink — otherwise a worker that never listens would never exit.
    G.__mbunIpcPin = () => portHasListener(parentPort);
  }

  const mod = {
    Worker,
    isMainThread,
    parentPort,
    threadId,
    workerData,
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
  if (typeof G.Worker === "undefined") G.Worker = Worker;
  // node's global MessageChannel/MessagePort ARE the worker_threads ones (they
  // are re-exported onto globalThis since v15), so the node-parity classes win
  // over the load-order web stubs.
  G.MessageChannel = MessageChannel;
  G.MessagePort = MessagePort;
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
