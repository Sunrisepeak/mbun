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
  // BroadcastChannel exposes structured-clone failures to its caller; do not
  // swallow its DataCloneError while posting a broadcast.
  const cloneBroadcast = (data) => {
    if (typeof G.structuredClone !== "function") return data;
    try { return G.structuredClone(data); }
    catch (e) {
      // JSC says "Symbol values cannot be cloned"; Node reports the rejected
      // value itself for BroadcastChannel.
      if (typeof data === "symbol") throw dataClone(String(data) + " could not be cloned.");
      throw e;
    }
  };
  // A BroadcastChannel also has a receiveMessageOnPort-compatible inbox. A
  // WeakMap keeps that capability branded: a lookalike object cannot acquire
  // one by adding public properties.
  const broadcastQueues = new WeakMap();
  const broadcastNames = new WeakMap();
  const invalidBroadcastThis = () => {
    const e = new TypeError('Value of "this" must be of type BroadcastChannel');
    e.code = "ERR_INVALID_THIS";
    return e;
  };
  const assertBroadcast = (value) => {
    if (!broadcastQueues.has(value)) throw invalidBroadcastThis();
  };

  const kOther = Symbol("mbun.port.other");
  const kQueue = Symbol("mbun.port.queue");
  const kDetached = Symbol("mbun.port.detached");
  const kEvt = Symbol("mbun.port.domListeners");
  const kOnMsg = Symbol("mbun.port.onmessage");
  const kOnMsgErr = Symbol("mbun.port.onmessageerror");
  const kRefed = Symbol("mbun.port.refed");
  const kStarted = Symbol("mbun.port.started");
  const kAsyncHookId = Symbol("mbun.port.asyncHookId");
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

  // ---- the worker wire: a structured-clone subset over JSON IPC -----------
  // A worker is a child process and the channel to it is node:child_process's
  // JSON IPC, so anything JSON cannot express used to arrive flattened: a
  // Uint8Array as {"0":1,…}, a Date as a string, a WebAssembly.Module as {}.
  // node's worker boundary is a real structured clone. Tagging the handful of
  // types JSON drops and rebuilding them on the far side closes most of that
  // gap; ordinary JSON values go over the wire byte-for-byte as before.
  //
  // Still honestly red: a SharedArrayBuffer is COPIED, not shared — two
  // processes cannot share a JS heap object, and Atomics on the far side
  // operate on the copy.
  const WIRE = "$mbunSC";
  const b64enc = (u8) => {
    if (G.Buffer) return G.Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength).toString("base64");
    let s = "";
    for (let i = 0; i < u8.length; i += 4096) s += String.fromCharCode.apply(null, u8.subarray(i, i + 4096));
    return G.btoa(s);
  };
  // Always over an exactly-sized ArrayBuffer: Buffer.from() hands back a view
  // into a shared pool, and handing that pool's buffer to `new Int32Array(buf)`
  // would expose 8 KiB of unrelated bytes.
  const b64dec = (b) => {
    if (G.Buffer) { const bb = G.Buffer.from(String(b), "base64"); const u = new Uint8Array(bb.byteLength); u.set(bb); return u; }
    const s = G.atob(String(b));
    const u = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) u[i] = s.charCodeAt(i);
    return u;
  };

  // A compiled WebAssembly.Module has no route back to its source bytes, and
  // the bytes are the only thing that can cross a process boundary — so
  // remember them at compile time (test-worker-message-port-wasm-module posts a
  // module to a worker and instantiates it there).
  const WASM_BYTES = new WeakMap();
  const wasmRemember = (m, src) => {
    try {
      let u8 = null;
      if (src instanceof ArrayBuffer) u8 = new Uint8Array(src.slice(0));
      else if (ArrayBuffer.isView(src)) { u8 = new Uint8Array(src.byteLength); u8.set(new Uint8Array(src.buffer, src.byteOffset, src.byteLength)); }
      if (u8 && m !== null && typeof m === "object") WASM_BYTES.set(m, u8);
    } catch (e) {}
    return m;
  };
  (function installWasmMemo() {
    const W = G.WebAssembly;
    if (!W || typeof W.Module !== "function" || W.Module.__mbunWasmMemo) return;
    const RealModule = W.Module;
    const Wrapped = function Module(src) {
      // Called without `new`: let the real constructor raise node's TypeError.
      if (new.target === undefined) return RealModule(src);
      const m = Reflect.construct(RealModule, arguments,
                                  new.target === Wrapped ? RealModule : new.target);
      return wasmRemember(m, src);
    };
    Wrapped.prototype = RealModule.prototype;
    Object.setPrototypeOf(Wrapped, RealModule);
    Wrapped.__mbunWasmMemo = true;
    try { W.Module = Wrapped; } catch (e) { return; }
    if (typeof W.compile === "function") {
      const realCompile = W.compile;
      W.compile = function compile(src) { return realCompile.call(W, src).then((m) => wasmRemember(m, src)); };
    }
    if (typeof W.instantiate === "function") {
      const realInstantiate = W.instantiate;
      W.instantiate = function instantiate(src, imports) {
        return realInstantiate.call(W, src, imports).then((r) => {
          if (r !== null && typeof r === "object" && r.module !== undefined) wasmRemember(r.module, src);
          return r;
        });
      };
    }
  })();

  // `ports` is the caller's transfer list; a MessagePort found in the message
  // is replaced by its index in it, and one that is NOT in it is node's
  // DataCloneError (test-worker-workerdata-messageport asserts the wording).
  const encWire = (v, ports) => {
    if (v === undefined) { const o = {}; o[WIRE] = "u"; return o; }
    if (v === null) return null;
    const t = typeof v;
    if (t === "bigint") { const o = {}; o[WIRE] = "bi"; o.v = String(v); return o; }
    if (t !== "object") return v;
    if (isPort(v)) {
      const i = ports ? ports.indexOf(v) : -1;
      if (i < 0) throw dataClone("Object that needs transfer was found in message but not listed in transferList");
      const o = {}; o[WIRE] = "port"; o.i = i; return o;
    }
    if (typeof G.SharedArrayBuffer === "function" && v instanceof G.SharedArrayBuffer) {
      const o = {}; o[WIRE] = "sab"; o.v = b64enc(new Uint8Array(v)); return o;
    }
    if (v instanceof ArrayBuffer) { const o = {}; o[WIRE] = "ab"; o.v = b64enc(new Uint8Array(v)); return o; }
    if (ArrayBuffer.isView(v)) {
      const o = {}; o[WIRE] = "ta";
      o.k = (v.constructor && v.constructor.name) || "Uint8Array";
      o.v = b64enc(new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
      return o;
    }
    if (v instanceof Date) { const o = {}; o[WIRE] = "date"; o.v = v.getTime(); return o; }
    if (v instanceof RegExp) { const o = {}; o[WIRE] = "re"; o.s = v.source; o.f = v.flags; return o; }
    if (G.WebAssembly && typeof G.WebAssembly.Module === "function" && v instanceof G.WebAssembly.Module) {
      const bytes = WASM_BYTES.get(v);
      if (!bytes) throw dataClone("A WebAssembly.Module whose source is unknown could not be cloned.");
      const o = {}; o[WIRE] = "wasm"; o.v = b64enc(bytes); return o;
    }
    if (v instanceof Map) {
      const out = []; for (const kv of v) out.push([encWire(kv[0], ports), encWire(kv[1], ports)]);
      const o = {}; o[WIRE] = "map"; o.v = out; return o;
    }
    if (v instanceof Set) {
      const out = []; for (const x of v) out.push(encWire(x, ports));
      const o = {}; o[WIRE] = "set"; o.v = out; return o;
    }
    if (v instanceof Error) {
      const o = {}; o[WIRE] = "err"; o.n = v.name; o.m = v.message; o.s = v.stack; o.c = v.code; return o;
    }
    if (Array.isArray(v)) { const out = new Array(v.length); for (let i = 0; i < v.length; i++) out[i] = encWire(v[i], ports); return out; }
    const out = {};
    let escaped = false;
    for (const k of Object.keys(v)) { if (k === WIRE) escaped = true; out[k] = encWire(v[k], ports); }
    if (!escaped) return out;
    const o = {}; o[WIRE] = "esc"; o.v = out; return o;
  };

  // Set on the worker side (below) to materialise a MessagePort transferred in
  // from the parent. On the main thread nothing arrives this way.
  let wirePort = () => undefined;
  const transferredPorts = new Map();  // transfer-list index -> local channel

  const decWire = (v) => {
    if (v === null || typeof v !== "object") return v;
    if (Array.isArray(v)) { for (let i = 0; i < v.length; i++) v[i] = decWire(v[i]); return v; }
    const tag = Object.prototype.hasOwnProperty.call(v, WIRE) ? v[WIRE] : undefined;
    if (typeof tag === "string") {
      if (tag === "u") return undefined;
      if (tag === "bi") { try { return BigInt(v.v); } catch (e) { return v.v; } }
      if (tag === "ab") return b64dec(v.v).buffer;
      if (tag === "sab") {
        const u = b64dec(v.v);
        if (typeof G.SharedArrayBuffer !== "function") return u.buffer;
        const s = new G.SharedArrayBuffer(u.length);
        new Uint8Array(s).set(u);
        return s;
      }
      if (tag === "ta") {
        const u = b64dec(v.v);
        if (v.k === "Buffer" && G.Buffer) return G.Buffer.from(u);
        const C = G[v.k];
        if (typeof C !== "function") return u;
        try { return new C(u.buffer); } catch (e) { return u; }
      }
      if (tag === "date") return new Date(v.v);
      if (tag === "re") { try { return new RegExp(v.s, v.f); } catch (e) { return new RegExp(v.s); } }
      if (tag === "wasm") {
        try { return new G.WebAssembly.Module(b64dec(v.v)); } catch (e) { return undefined; }
      }
      if (tag === "map") { const m = new Map(); for (const kv of (v.v || [])) m.set(decWire(kv[0]), decWire(kv[1])); return m; }
      if (tag === "set") { const s = new Set(); for (const x of (v.v || [])) s.add(decWire(x)); return s; }
      if (tag === "err") {
        const e = new Error(v.m); e.name = v.n || "Error";
        if (v.s) { try { e.stack = v.s; } catch (_) {} }
        if (v.c !== undefined) e.code = v.c;
        return e;
      }
      if (tag === "port") return wirePort(v.i);
      if (tag === "esc") return decWire(v.v);
    }
    for (const k of Object.keys(v)) v[k] = decWire(v[k]);
    return v;
  };

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
    if (typeof G.__mbunAsyncHookInit === "function") p[kAsyncHookId] = G.__mbunAsyncHookInit("MESSAGEPORT", p);
    return p;
  };

  const isPort = (v) => v !== null && typeof v === "object" && v[kQueue] !== undefined;
  // BroadcastChannel has no transfer-list parameter. Node therefore rejects a
  // MessagePort anywhere in its payload instead of silently cloning the host
  // object; recurse through enumerable payload members so a MessageChannel is
  // caught through its port1/port2 fields too.
  const containsTransferable = (value, seen = new Set()) => {
    if (value === null || typeof value !== "object") return false;
    if (isPort(value)) return true;
    if (seen.has(value)) return false;
    seen.add(value);
    for (const key of Object.keys(value)) {
      if (containsTransferable(value[key], seen)) return true;
    }
    return false;
  };
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
  // Deliver only what was ALREADY queued when this turn began, then hand the
  // rest to the next turn. A handler that posts straight back to the port it is
  // draining (`port1.on('message', () => port2.postMessage(0))`) re-queues into
  // the very array this loop is testing, so an unbounded `while` never returned
  // to the event loop: 10001 iterations inside one setImmediate, and the 0 ms
  // timeout meant to break the cycle never got a turn
  // (test-worker-message-port-infinite-message-loop). node gives each round trip
  // its own loop turn.
  const portFlush = (p) => {
    let n = p[kQueue].length;
    while (n-- > 0 && p[kQueue].length && portHasSink(p)) {
      const item = p[kQueue].shift();
      portDeliver(p, item.data, item.ports);
    }
    if (p[kQueue].length && portHasSink(p)) scheduleFlush(p);
  };
  // Delivery is scheduled on the MACROtask queue (setImmediate), matching node:
  // a port message arrives from the event loop, so an endless
  // .on('message')/postMessage ping-pong cannot starve timers
  // (test-worker-message-port-infinite-message-loop).
  const scheduleFlush = (p) => {
    if (typeof G.setImmediate === "function") G.setImmediate(() => portFlush(p));
    else G.queueMicrotask(() => portFlush(p));
  };
  const portStart = (p) => { p[kStarted] = true; p[kRefed] = true; scheduleFlush(p); };

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
  const MOD_TOK = "__mbunWasmModule__";
  const isWasmModule = (v) => G.WebAssembly && typeof G.WebAssembly.Module === "function" &&
                              v instanceof G.WebAssembly.Module;
  const subst = (v, ports, seen, mods) => {
    if (v === null || typeof v !== "object") return v;
    const i = ports.indexOf(v);
    if (i >= 0) { const o = {}; o[PORT_TOK] = i; return o; }
    // JSC's structuredClone silently downgrades a WebAssembly.Module to a plain
    // object (node's preserves it — test-worker-message-port-wasm-module posts
    // one through a port and instantiates it on the far side), so swap modules
    // out for a token and put the originals back after the clone. Checked
    // BEFORE the prototype bail-out below, which would return the module as-is.
    if (mods && isWasmModule(v)) { const j = mods.length; mods.push(v); const o = {}; o[MOD_TOK] = j; return o; }
    // markAsUncloneable: node fails the clone wherever the marked object sits
    // in the graph, and has no effect on (Shared)ArrayBuffer. The wrapper on
    // globalThis.structuredClone can only see the TOP-level value, and it stops
    // seeing even that now that this walk hands the clone a rebuilt copy — so
    // the check belongs here (test-worker-message-mark-as-uncloneable).
    if (UNCLONEABLE.has(v) && !(v instanceof ArrayBuffer) &&
        !(typeof G.SharedArrayBuffer === "function" && v instanceof G.SharedArrayBuffer)) {
      let label = "Object";
      try { label = String(v); } catch (e) {}
      throw dataClone(label + " could not be cloned.");
    }
    if (seen.has(v)) return seen.get(v);
    if (Array.isArray(v)) { const out = []; seen.set(v, out); for (let k = 0; k < v.length; k++) out[k] = subst(v[k], ports, seen, mods); return out; }
    const proto = Object.getPrototypeOf(v);
    if (proto !== Object.prototype && proto !== null) return v;
    const out = {}; seen.set(v, out);
    for (const k of Object.keys(v)) out[k] = subst(v[k], ports, seen, mods);
    return out;
  };
  const unsubst = (v, ports, seen, mods) => {
    if (v === null || typeof v !== "object") return v;
    if (Object.prototype.hasOwnProperty.call(v, PORT_TOK) && typeof v[PORT_TOK] === "number") return ports[v[PORT_TOK]];
    if (mods && Object.prototype.hasOwnProperty.call(v, MOD_TOK) && typeof v[MOD_TOK] === "number") return mods[v[MOD_TOK]];
    if (seen.has(v)) return seen.get(v);
    if (Array.isArray(v)) { seen.set(v, v); for (let k = 0; k < v.length; k++) v[k] = unsubst(v[k], ports, seen, mods); return v; }
    const proto = Object.getPrototypeOf(v);
    if (proto !== Object.prototype && proto !== null) return v;
    seen.set(v, v);
    for (const k of Object.keys(v)) v[k] = unsubst(v[k], ports, seen, mods);
    return v;
  };
  // ---- node:crypto keys across the worker boundary -------------------------
  // node transfers a KeyObject / WebCrypto CryptoKey to another thread with its
  // key state intact (lib/internal/crypto/keys.js kClone / js_transferable), so
  // `new Worker(f, { workerData: key })` gives the worker a usable key. mbun runs
  // a worker as a CHILD PROCESS, so every payload crosses as JSON — and both
  // classes encode to `{}` there: a CryptoKey keeps everything in a WeakMap, and
  // a KeyObject's fields survive without the prototype that gives them meaning.
  // These helpers tag the two on the way out and rebuild them on the way in,
  // through the same internal bridges KeyObject.toCryptoKey/from already use.
  // The material only ever travels between a parent and the worker it started,
  // which is the boundary node's thread transfer crosses too.
  const KEY_TOK = "__mbunTransferredKey__";
  const cryptoMod = () => M["crypto"] || M["node:crypto"];
  const encMaterial = (m) => (typeof m === "string" ? { s: m } : { b: Buffer.from(m).toString("base64") });
  const decMaterial = (d) => (d && typeof d.s === "string" ? d.s : Buffer.from(d.b, "base64"));
  const encodeKey = (v) => {
    if (typeof G.__mbunIsCryptoKey === "function" && G.__mbunIsCryptoKey(v)) {
      const d = G.__mbunCryptoKeyToKeyObject(v);
      if (!d) return undefined;
      const o = {};
      o[KEY_TOK] = { w: 1, kind: d.kind, m: encMaterial(d.material), algorithm: d.algorithm,
                     usages: d.usages, extractable: d.extractable };
      return o;
    }
    const C = cryptoMod();
    if (C && typeof C.KeyObject === "function" && v instanceof C.KeyObject && v._kind !== undefined) {
      const o = {};
      o[KEY_TOK] = { w: 0, kind: v._kind, m: encMaterial(v._km), p: v._pass };
      return o;
    }
    return undefined;
  };
  const decodeKey = (t) => {
    try {
      if (t.w === 1) {
        const bridge = G.__mbunKeyObjectToCryptoKey;
        if (typeof bridge !== "function") return null;
        return bridge(t.kind, new Uint8Array(Buffer.from(decMaterial(t.m))), t.algorithm,
                      t.extractable, t.usages);
      }
      const C = cryptoMod();
      if (!C || typeof C.KeyObject !== "function" || C.__koBrand === undefined) return null;
      return new C.KeyObject(C.__koBrand, t.kind, decMaterial(t.m), t.p);
    } catch (e) { return null; }
  };
  const encodeKeys = (v, seen) => {
    if (v === null || typeof v !== "object") return v;
    const tagged = encodeKey(v);
    if (tagged !== undefined) return tagged;
    if (seen.has(v)) return seen.get(v);
    if (Array.isArray(v)) { const out = []; seen.set(v, out); for (const x of v) out.push(encodeKeys(x, seen)); return out; }
    const proto = Object.getPrototypeOf(v);
    if (proto !== Object.prototype && proto !== null) return v;
    const out = {}; seen.set(v, out);
    for (const k of Object.keys(v)) out[k] = encodeKeys(v[k], seen);
    return out;
  };
  const decodeKeys = (v, seen) => {
    if (v === null || typeof v !== "object") return v;
    if (Object.prototype.hasOwnProperty.call(v, KEY_TOK) && v[KEY_TOK] !== null &&
        typeof v[KEY_TOK] === "object") {
      return decodeKey(v[KEY_TOK]);
    }
    if (seen.has(v)) return seen.get(v);
    if (Array.isArray(v)) { seen.set(v, v); for (let k = 0; k < v.length; k++) v[k] = decodeKeys(v[k], seen); return v; }
    const proto = Object.getPrototypeOf(v);
    if (proto !== Object.prototype && proto !== null) return v;
    seen.set(v, v);
    for (const k of Object.keys(v)) v[k] = decodeKeys(v[k], seen);
    return v;
  };
  const hasHostTransferable = (v, seen) => {
    if (v === null || typeof v !== "object") return false;
    if (encodeKey(v) !== undefined) return true;
    if (seen.has(v)) return false;
    seen.add(v);
    if (Array.isArray(v)) { for (const x of v) if (hasHostTransferable(x, seen)) return true; return false; }
    const proto = Object.getPrototypeOf(v);
    if (proto !== Object.prototype && proto !== null) return false;
    for (const k of Object.keys(v)) if (hasHostTransferable(v[k], seen)) return true;
    return false;
  };
  // Only rewrite a payload that actually carries a key: the walk COPIES the
  // plain objects it descends through, and a copy is a different identity — it
  // would have slipped past markAsUncloneable()'s WeakSet check on the value
  // being posted (test-worker-message-mark-as-uncloneable).
  const encodeKeysTop = (v) => (hasHostTransferable(v, new Set()) ? encodeKeys(v, new Map()) : v);
  const decodeKeysTop = (v) => decodeKeys(v, new Map());

  // Clone FIRST, detach after: a transferred ArrayBuffer is frequently also the
  // backing store of the message itself (postMessage(typedArray, [ab])), and
  // detaching before the copy would hand the receiver an empty view.
  const cloneWithPorts = (value, ports, buffers) => {
      // Two codecs compose here and both are load-bearing. subst/unsubst thread
      // `mods` so a WebAssembly.Module survives structuredClone (JSC silently
      // downgrades one to a plain object), and encodeKeysTop/decodeKeysTop tag
      // KeyObject/CryptoKey because the host clone copies a CryptoKey's
      // prototype but NOT the WeakMap its state lives in — the receiver would
      // otherwise hold something that is `instanceof CryptoKey` with no key in
      // it, which reads as a key and is worse than a failure.
      const mods = [];
      const pre = encodeKeysTop(subst(value, ports, new Map(), mods));
      const c = decodeKeysTop(G.structuredClone(pre));
    for (const b of buffers) { try { G.structuredClone(b, { transfer: [b] }); } catch (e) {} }
    return (ports.length || mods.length) ? unsubst(c, ports, new Map(), mods) : c;
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
        if (UNTRANSFERABLE.has(item))
          throw dataClone("Cannot transfer object marked as untransferable");
        if (item === this) throw dataClone("Transfer list contains source port");
        if (seenPorts.has(item)) throw dataClone("Transfer list contains duplicate MessagePort");
        if (item[kDetached] === true) throw dataClone("MessagePort in transfer list is already detached");
        seenPorts.add(item); ports.push(item);
      } else if (item instanceof ArrayBuffer) {
        if (seenBufs.has(item)) throw dataClone("Transfer list contains duplicate ArrayBuffer");
        if (abDetached(item)) throw dataClone("ArrayBuffer at index " + buffers.length + " is already detached");
        if (UNTRANSFERABLE.has(item))
          throw dataClone("Cannot transfer object marked as untransferable");
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
    G.queueMicrotask(() => {
      self[kRefed] = false;
      self.emit("close");
      if (typeof G.__mbunAsyncHookDestroy === "function") G.__mbunAsyncHookDestroy(self[kAsyncHookId]);
    });
    if (other) {
      other[kOther] = null;
      G.queueMicrotask(() => {
        if (other[kDetached] === true) return;
        other[kDetached] = true;
        other[kRefed] = false;
        other.emit("close");
        if (typeof G.__mbunAsyncHookDestroy === "function") G.__mbunAsyncHookDestroy(other[kAsyncHookId]);
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
    const queue = isPort(port) ? port[kQueue] : broadcastQueues.get(port);
    if (queue === undefined) {
      const e = new TypeError('The "port" argument must be a MessagePort instance');
      e.code = "ERR_INVALID_ARG_TYPE";
      throw e;
    }
    if (queue.length) return { message: queue.shift().data };
    return undefined;
  };

  // node marks an object so a later postMessage clones it instead of moving it;
  // markAsUncloneable makes structured cloning of it fail with DataCloneError.
  const markAsUntransferable = function (obj) {
    if (obj !== null && (typeof obj === "object" || typeof obj === "function")) UNTRANSFERABLE.add(obj);
    return undefined;
  };
  const isMarkedAsUntransferable = function (obj) {
    return obj !== null && (typeof obj === "object" || typeof obj === "function") &&
           UNTRANSFERABLE.has(obj);
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
  // ---- moveMessagePortToContext -------------------------------------------
  // node node_messaging.cc: the returned handle belongs to the TARGET vm
  // context — its methods, the message event, its `data`/`ports` payload and any
  // DataCloneError it throws are all objects of that context's realm, and it is
  // deliberately NOT an instanceof the calling realm's MessagePort. So the
  // handle has to be *constructed inside* the context (vm.runInContext), with
  // this realm's real port kept on the outside and reached only through a plain
  // helper table. Transferred ports arrive wrapped the same way, and the wrapper
  // is memoised per port so `ports[0] === data.p` holds.
  const kMoveCtxSrc = "(function (h) {\n" +
    "  const DE = (typeof DOMException === 'function') ? DOMException : (function () {\n" +
    "    class DOMException extends Error {\n" +
    "      constructor(message, name) { super(message); this.name = name || 'Error'; this.code = 25; }\n" +
    "    }\n" +
    "    return DOMException;\n" +
    "  })();\n" +
    // Rebuild the payload with this realm's Object/Array so `data instanceof
    // Object` holds inside the context; a port maps to its context-side handle.
    "  const copy = function copy(v) {\n" +
    "    if (v === null || typeof v !== 'object') return v;\n" +
    "    const w = h.wrapOf(v); if (w !== undefined) return w;\n" +
    "    if (h.isArray(v)) { const a = []; const n = h.len(v); for (let i = 0; i < n; i++) a[i] = copy(h.get(v, i)); return a; }\n" +
    "    const o = {}; const ks = h.keys(v); for (let i = 0; i < ks.length; i++) o[ks[i]] = copy(h.get(v, ks[i]));\n" +
    "    return o;\n" +
    "  };\n" +
    "  return { make: function (id) {\n" +
    "    let onmsg;\n" +
    "    let onmsgerr;\n" +
    "    const port = {\n" +
    "      postMessage: function (value, transferList) {\n" +
    "        const e = h.post(id, value, transferList);\n" +
    "        if (e !== undefined) throw new DE(e.message, e.name);\n" +
    "      },\n" +
    "      start: function () { h.start(id); },\n" +
    "      close: function () { h.close(id); },\n" +
    "      ref: function () { h.ref(id); return this; },\n" +
    "      unref: function () { h.unref(id); return this; },\n" +
    "      hasRef: function () { return h.hasRef(id); },\n" +
    "    };\n" +
    "    Object.defineProperty(port, 'onmessage', {\n" +
    "      configurable: true, get() { return onmsg; }, set(v) { onmsg = v; },\n" +
    "    });\n" +
    "    Object.defineProperty(port, 'onmessageerror', {\n" +
    "      configurable: true, get() { return onmsgerr; }, set(v) { onmsgerr = v; },\n" +
    "    });\n" +
    // A payload the target context cannot deserialize arrives as an error code
    // instead of data; the Error itself is minted HERE so it belongs to the
    // context's realm, like every other object this handle hands out.
    "    h.deliverTo(id, function (data, ports, errCode) {\n" +
    "      if (errCode !== undefined) {\n" +
    "        if (typeof onmsgerr !== 'function') return;\n" +
    "        const e = new Error('A message object could not be deserialized successfully in the target vm.Context');\n" +
    "        e.code = errCode;\n" +
    "        onmsgerr.call(port, { data: e });\n" +
    "        return;\n" +
    "      }\n" +
    "      if (typeof onmsg === 'function') onmsg.call(port, { data: copy(data), ports: copy(ports) });\n" +
    "    });\n" +
    "    return port;\n" +
    "  } };\n" +
    "})";
  const moveMessagePortToContext = function (port, context) {
    const vmM = M["vm"] || M["node:vm"];
    if (!isPort(port)) {
      const e = new TypeError('The "port" argument must be a MessagePort instance');
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    if (!vmM || typeof vmM.runInContext !== "function" || context === null ||
        typeof context !== "object") {
      const e = new TypeError('The "contextifiedSandbox" argument must be a vm.Context');
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    const st = { seq: 0, byId: new Map(), wrapOf: new Map(), deliver: new Map() };
    const h = {
      post: (id, value, tl) => {
        try { st.byId.get(id).postMessage(value, tl); }
        catch (e) { return { message: e && e.message, name: e && e.name }; }
        return undefined;
      },
      start: (id) => { st.byId.get(id).start(); },
      close: (id) => { st.byId.get(id).close(); },
      ref: (id) => { st.byId.get(id).ref(); },
      unref: (id) => { st.byId.get(id).unref(); },
      hasRef: (id) => st.byId.get(id).hasRef(),
      keys: (o) => Object.keys(o),
      get: (o, k) => o[k],
      len: (o) => o.length,
      isArray: (o) => Array.isArray(o),
      wrapOf: (v) => st.wrapOf.get(v),
      deliverTo: (id, fn) => { st.deliver.set(id, fn); },
    };
    const factory = vmM.runInContext(kMoveCtxSrc, context)(h);
    const mk = (p) => {
      const seen = st.wrapOf.get(p);
      if (seen !== undefined) return seen;
      const id = ++st.seq;
      st.byId.set(id, p);
      const w = factory.make(id);
      st.wrapOf.set(p, w);
      p.onmessage = (ev) => {
        const raw = (ev && ev.ports) || [];
        for (const sp of raw) mk(sp);
        const fn = st.deliver.get(id);
        if (!fn) return;
        // node deserializes a KeyObject / CryptoKey only into the context that
        // owns its native handle — src/crypto/crypto_keys.cc
        // KeyObjectTransferData::Deserialize raises
        // ERR_MESSAGE_TARGET_CONTEXT_UNAVAILABLE for any other vm.Context, and
        // the port reports that as a 'messageerror', never as a message. The key
        // codec above already knows exactly which values are those host objects.
        if (hasHostTransferable(ev && ev.data, new Set())) {
          fn(undefined, [], "ERR_MESSAGE_TARGET_CONTEXT_UNAVAILABLE");
          return;
        }
        fn(ev && ev.data, raw);
      };
      return w;
    };
    return mk(port);
  };

  // ---- BroadcastChannel (in-process fan-out) ------------------------------
  const BroadcastChannel = G.BroadcastChannel || (function () {
    const channels = new Map();
    class BroadcastChannel extends EventEmitter {
      constructor(name) {
        if (arguments.length === 0)
          throw new TypeError('The "name" argument must be specified');
        super();
        // Node's `${name}` conversion deliberately rejects symbols instead of
        // accepting them through String(Symbol()).
        if (typeof name === "symbol")
          throw new TypeError("Cannot convert a Symbol value to a string");
        broadcastNames.set(this, `${name}`);
        this.onmessage = null;
        this.onmessageerror = null;
        this._closed = false;
        broadcastQueues.set(this, []);
        let set = channels.get(this.name);
        if (!set) { set = new Set(); channels.set(this.name, set); }
        set.add(this);
      }
      get name() { assertBroadcast(this); return broadcastNames.get(this); }
      [INSPECT_SYM](depth, options, inspect) {
        assertBroadcast(this);
        const name = typeof inspect === "function"
          ? inspect(broadcastNames.get(this))
          : JSON.stringify(broadcastNames.get(this));
        return "BroadcastChannel { name: " + name + ", active: " + (!this._closed) + " }";
      }
      postMessage(value) {
        assertBroadcast(this);
        if (arguments.length === 0)
          throw new TypeError('The "message" argument must be specified');
        if (this._closed) throw new Error("BroadcastChannel is closed");
        const set = channels.get(this.name);
        if (!set) return;
        if (containsTransferable(value))
          throw dataClone("Object that needs transfer was found in message but not listed in transferList");
        const data = cloneBroadcast(value);
        for (const ch of set) {
          if (ch === this || ch._closed) continue;
          const item = { data };
          broadcastQueues.get(ch).push(item);
          G.queueMicrotask(() => {
            const queue = broadcastQueues.get(ch);
            const index = queue.indexOf(item);
            if (index < 0) return;
            queue.splice(index, 1);
            const ev = typeof G.MessageEvent === "function"
              ? new G.MessageEvent("message", { data })
              : { data, type: "message" };
            ev.target = ch;
            ev.currentTarget = ch;
            if (typeof ch.onmessage === "function") ch.onmessage(ev);
            ch.emit("message", ev);
          });
        }
      }
      close() {
        assertBroadcast(this);
        if (this._closed) return;
        this._closed = true;
        const set = channels.get(this.name);
        if (set) { set.delete(this); if (set.size === 0) channels.delete(this.name); }
      }
      ref() { assertBroadcast(this); return this; }
      unref() { assertBroadcast(this); return this; }
      addEventListener(type, cb) { this.on(type, cb); }
      removeEventListener(type, cb) { this.off(type, cb); }
    }
    return BroadcastChannel;
  })();
  // node's custom formatter deliberately collapses a BroadcastChannel when
  // inspect has exhausted its depth budget (test-broadcastchannel-custom-inspect).
  Object.defineProperty(BroadcastChannel.prototype, INSPECT_SYM, {
    configurable: true,
    value: function (depth) {
      if (!(this instanceof BroadcastChannel)) {
        const e = new TypeError("Value of \"this\" must be of type BroadcastChannel");
        e.code = "ERR_INVALID_THIS";
        throw e;
      }
      if (typeof depth === "number" && depth < 0) return "BroadcastChannel";
      const name = String(this.name)
        .replace(/\\/g, "\\\\")
        .replace(/'/g, "\\'")
        .replace(/\n/g, "\\n")
        .replace(/\r/g, "\\r");
      return "BroadcastChannel { name: '" + name + "', active: " +
        (this._closed !== true) + " }";
    },
    writable: true,
  });

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

  // Shared node determineSpecificType (bootstrap __mbunNodeErrors); the local
  // fallback below has no `function` case, so an anonymous function reported
  // "type function (() => {})" where node says "function " (empty name).
  const recvType = (v) => {
    const NE = globalThis.__mbunNodeErrors;
    if (NE) return NE.determineSpecificType(v);
    if (v === null) return "null";
    if (v === undefined) return "undefined";
    const t = typeof v;
    if (t === "symbol") return "type symbol (" + String(v) + ")";
    if (t === "string") return "type string ('" + v + "')";
    if (t === "function") return "function " + v.name;
    if (t === "object") return "an instance of " + ((v.constructor && v.constructor.name) || "Object");
    return "type " + t + " (" + String(v) + ")";
  };

  const dataUrlSource = (p) => {
    const comma = p.indexOf(",");
    if (comma === -1) { const e = new TypeError("Invalid URL: " + p); e.code = "ERR_INVALID_URL"; throw e; }
    const meta = p.slice(5, comma);
    const payload = p.slice(comma + 1);
    const mime = meta.split(";", 1)[0].toLowerCase() || "text/plain";
    let source;
    if (/;base64\s*$/i.test(meta)) {
      source = G.Buffer ? G.Buffer.from(payload, "base64").toString("utf8")
        : (G.atob ? G.atob(payload) : payload);
    } else {
      try { source = decodeURIComponent(payload); } catch (e) { source = payload; }
    }
    return { source, javascript: mime === "text/javascript" || mime === "application/javascript" };
  };

  // node ERR_WORKER_PATH: a bare specifier is not a worker entry point.
  const workerEntryPath = (filename) => {
    let p = filename;
    if (p !== null && typeof p === "object" && typeof p.href === "string") {
      if (p.protocol === "data:") return { data: dataUrlSource(p.href) };
      if (p.protocol !== "file:") {
        const e = new TypeError("The URL must be of scheme file: Received protocol '" + p.protocol + "'");
        e.code = "ERR_INVALID_URL_SCHEME"; throw e;
      }
      const u = M["url"] || M["node:url"];
      return { path: u && u.fileURLToPath ? u.fileURLToPath(p) : p.pathname };
    }
    if (G.Buffer && typeof G.Buffer.isBuffer === "function" && G.Buffer.isBuffer(p)) p = p.toString();
    if (typeof p !== "string") {
      const e = new TypeError('The "filename" argument must be of type string or an instance of URL. Received ' + recvType(filename));
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    if (p.startsWith("data:")) {
      const e = new TypeError("The worker script or module filename must be an absolute path or a relative path starting with './' or '../'. Wrap data: URLs with `new URL`. Received \"" + p + "\"");
      e.code = "ERR_WORKER_PATH"; throw e;
    }
    if (p.startsWith("file://")) {
      const e = new TypeError("The worker script or module filename must be an absolute path or a relative path starting with './' or '../'. Wrap file:// URLs with `new URL`. Received \"" + p + "\"");
      e.code = "ERR_WORKER_PATH"; throw e;
    }
    if (!pathM.isAbsolute(p) && !/^\.\.?[/\\]/.test(p)) {
      const e = new TypeError("The worker script or module filename must be an absolute path or a relative path starting with './' or '../'. Received \"" + p + "\"");
      e.code = "ERR_WORKER_PATH"; throw e;
    }
    return { path: pathM.resolve(p) };
  };

  // ---- process.cwd across the worker boundary -----------------------------
  // node runs a worker as a THREAD, so uv_cwd() is one process-wide value and a
  // main-thread process.chdir() is already visible in every worker; all node
  // has to do is invalidate each thread's cached copy, which it does with an
  // Atomics.load on a shared counter (bootstrap/switches/is_not_main_thread.js
  // — the reason test-worker-process-cwd asserts `process.cwd.toString()`
  // mentions AtomicsLoad inside a worker and does NOT on the main thread).
  // An mbun worker is a CHILD PROCESS with its own cwd, so the move has to
  // travel: chdir broadcasts a control frame down every live worker channel and
  // a worker receiving one chdir()s itself — through this same wrapper, so the
  // move keeps propagating to ITS workers. The frame rides the ordinary IPC
  // channel, so it can never overtake a postMessage sent after the chdir.
  const rawCwd = typeof proc.cwd === "function" ? proc.cwd : null;
  const cwdCounter = new Int32Array(
      typeof G.SharedArrayBuffer === "function" ? new G.SharedArrayBuffer(4) : new ArrayBuffer(4));
  const AtomicsLoad = (G.Atomics && typeof G.Atomics.load === "function") ? G.Atomics.load : null;
  const bumpCwd = () => {
    try { G.Atomics.add(cwdCounter, 0, 1); } catch (e) { cwdCounter[0] = (cwdCounter[0] + 1) | 0; }
  };
  let cwdBroadcastInstalled = false;
  const installCwdBroadcast = () => {
    if (cwdBroadcastInstalled) return;
    cwdBroadcastInstalled = true;
    const realChdir = proc.chdir;
    if (typeof realChdir !== "function" || !rawCwd) return;
    proc.chdir = function chdir(dir) {
      const r = realChdir.call(proc, dir);
      bumpCwd();
      let now;
      try { now = rawCwd.call(proc); } catch (e) { return r; }
      for (const w of workerRegistry.values()) {
        try {
          if (!w._exited && w._child && w._child.connected) w._child.send({ t: "cd", d: now });
        } catch (e) {}
      }
      return r;
    };
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
      if (options.eval === true && typeof filename !== "string") {
        throw new TypeError("The property 'options.eval' must be false when 'filename' is not a string.");
      }
      // node splits the constructor's transferList exactly as MessagePort's
      // postMessage does: ports move to the worker, ArrayBuffers are detached
      // once the message has been serialised.
      const tlPorts = [], tlBuffers = [];
      for (const item of validateTransferList(options.transferList)) {
        if (isPort(item)) {
          if (item[kDetached] === true) throw dataClone("MessagePort in transfer list is already detached");
          tlPorts.push(item);
        } else if (item instanceof ArrayBuffer) {
          if (abDetached(item)) throw dataClone("ArrayBuffer at index " + tlBuffers.length + " is already detached");
          if (!UNTRANSFERABLE.has(item)) tlBuffers.push(item);
        }
      }
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
      // A worker inherits the parent's V8 flags, but an explicitly supplied
      // V8-only flag is rejected by node before the worker is created. Node
      // flags such as --expose-internals and --input-type remain valid here.
      if (Array.isArray(options.execArgv) && options.execArgv.some((arg) => String(arg) === "--expose-gc")) {
        const e = new Error("Initiated Worker with invalid execArgv flags: --expose-gc");
        e.code = "ERR_WORKER_INVALID_EXEC_ARGV";
        throw e;
      }
      // Serialise BEFORE the structuredClone check: the encoder is what knows
      // about the transfer list, and it owns the "needs transfer but was not
      // listed" DataCloneError whose exact wording the corpus asserts. A plain
      // structuredClone of the same value would report the port's own
      // "can only be transferred, not cloned" instead.
      let workerDataWire = null;
      if (options.workerData !== undefined) {
        // Key tagging runs FIRST: encWire turns values into $mbunSC-tagged
        // plain objects, so tagging afterwards would hand the receiver a
        // wire form nested inside a key form and it would decode in the
        // wrong order (a transferred Buffer arrived still wearing its
        // $mbunSC tag, and the MessagePort stand-in never materialised).
        workerDataWire = encWire(encodeKeysTop(options.workerData), tlPorts);
        // node clones workerData up-front, so an unclonable value throws from
        // the constructor instead of silently reaching the worker as null.
        G.structuredClone(subst(options.workerData, tlPorts, new Map(), []));
      }
      let workerDataJson = "null";
        try { workerDataJson = JSON.stringify(workerDataWire); }
      catch (e) { throw dataClone(String(options.workerData) + " could not be cloned."); }
      if (workerDataJson === undefined) throw dataClone(String(options.workerData) + " could not be cloned.");
      // The message is on the wire now, so the transferred buffers can go.
      for (const b of tlBuffers) { try { G.structuredClone(b, { transfer: [b] }); } catch (e) {} }

      const tid = nextThreadId++;
      this.threadId = tid;
      this._tempFile = null;
      let entry;
      if (options.eval) {
        entry = writeTempWorker(String(filename), tid, ".js");
        this._tempFile = entry;
      } else {
        const r = workerEntryPath(filename);
        if (r.data !== undefined) {
          // data: workers are ES modules. The runtime's loader lowers ESM into
          // the CommonJS wrapper it uses internally, so hide `module.exports`
          // from the user source while preserving the lowering's `exports`.
          let source = r.data.source;
          if (!r.data.javascript) source = "throw new TypeError('Unknown module format');";
          else source = "Object.defineProperty(module, 'exports', { get() { throw new ReferenceError('module is not defined'); }, set() { throw new ReferenceError('module is not defined'); } });\n" + source;
          entry = writeTempWorker(source, tid, ".mjs"); this._tempFile = entry;
        } else if (r.source !== undefined) { entry = writeTempWorker(r.source, tid, ".js"); this._tempFile = entry; }
        else entry = r.path;
      }

      // Env: SHARE_ENV and the default both inherit; an explicit object replaces.
      const baseEnv = (options.env && options.env !== SHARE_ENV) ? options.env : (proc.env || {});
      const env = {};
      for (const k of Object.keys(baseEnv)) { const v = baseEnv[k]; if (v !== undefined && v !== null) env[k] = String(v); }
      env.MBUN_WORKER_TID = String(tid);
      env.MBUN_WORKER_DATA = workerDataJson;
      // environmentData is inherited by every worker STARTED FROM HERE, as a
      // snapshot: node clones the parent's store into the new thread at spawn
      // time, so a later setEnvironmentData in the parent must not reach it.
      // Entry pairs (not an object) so non-string keys survive.
      try { env.MBUN_WORKER_ENVDATA = JSON.stringify(Array.from(environmentData)); }
      catch (e) { env.MBUN_WORKER_ENVDATA = "[]"; }

      // node's execArgv KEEPS the eval flag and its code (`node -e "…"` reports
      // ["-e", "…"]), because node starts a worker as a thread and never replays
      // that command line. mbun starts one as a child mbun process, so handing
      // the inherited execArgv straight to spawn() re-evaluates the PARENT's -e
      // program in every worker — and a parent whose -e program constructs a
      // Worker forks without bound (observed: repeated SIGABRT under the bounded
      // scope from `mbun -e 'new Worker(…)'`). The eval flag and the code token
      // after it are therefore dropped from the spawn argv only; process.execArgv
      // itself is untouched, so what the worker reports still matches node.
      const stripEval = (list) => {
        const out = [];
        for (let i = 0; i < list.length; i++) {
          const a = list[i];
          if (a === "-e" || a === "--eval" || a === "-p" || a === "--print" ||
              a === "-pe" || a === "-ep") { i++; continue; }
          out.push(a);
        }
        return out;
      };
      const execArgv = stripEval(Array.isArray(options.execArgv) ? options.execArgv.map(String)
                                                                 : ((proc.execArgv || []).map(String)));
      const argv = Array.isArray(options.argv) ? options.argv.map(String) : [];
      const child = CPM.spawn(String(proc.execPath || "mbun"),
                              execArgv.concat([entry], argv),
                              { env, stdio: ["pipe", "pipe", "pipe", "ipc"] });
      this._child = child;
      this._exited = false;
      this._exitCode = null;
      this._exitResolvers = [];
      this._refd = true;
      this._asyncWorkerAlive = true;
      this._asyncMessagePortRefd = false;
      this._asyncWorkerHandle = { hasRef: () => this._asyncWorkerAlive ? this._refd : undefined };
      this._asyncMessagePortHandle = { hasRef: () => this._asyncWorkerAlive ? this._asyncMessagePortRefd : undefined };
      this._asyncWorkerId = typeof G.__mbunAsyncHookInit === "function"
        ? G.__mbunAsyncHookInit("WORKER", this._asyncWorkerHandle) : undefined;
      this._asyncMessagePortId = typeof G.__mbunAsyncHookInit === "function"
        ? G.__mbunAsyncHookInit("MESSAGEPORT", this._asyncMessagePortHandle) : undefined;
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
      // A MessagePort transferred into the worker keeps living HERE; the worker
      // holds a stand-in whose two directions are these frames. 'p' is the
      // worker posting on its stand-in (delivered to this port's pair), 'pm' is
      // anything posted to this port travelling the other way.
      this._tlPorts = tlPorts;
      for (let i = 0; i < tlPorts.length; i++) {
        const p = tlPorts[i];
        const idx = i;
        p.on("message", (d) => {
          try {
            if (!self._exited && child.connected) child.send({ t: "pm", i: idx, d: encWire(d, tlPorts) });
          } catch (e) {}
        });
      }
      child.on("spawn", () => self.emit("online"));
      child.on("message", (m) => {
        if (m === null || typeof m !== "object") return;
        if (m.t === "m") {
          const d = decodeKeysTop(decWire(m.d));
          const ev = { data: d, type: "message", ports: [], target: self };
          if (typeof self.onmessage === "function") self.onmessage(ev);
          self.emit("message", d);
        } else if (m.t === "p" && typeof m.i === "number") {
          const p = self._tlPorts[m.i];
          if (p) { try { p.postMessage(decodeKeysTop(decWire(m.d))); } catch (e) {} }
        } else if (m.t === "e") {
          const err = new Error(m.d && m.d.message ? m.d.message : String(m.d));
          if (m.d && m.d.name) err.name = m.d.name;
          // Error.prepareStackTrace is user code. If it threw while the child
          // collected a fatal Error, node still forwards name/message but the
          // parent-visible error has no stack rather than a locally invented one.
          if (m.d && m.d.stackUnavailable) err.stack = undefined;
          else if (m.d && m.d.stack) err.stack = m.d.stack;
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
        self._asyncMessagePortRefd = false;
        workerRegistry.delete(tid);
        if (self._tempFile) { try { fsM.unlinkSync(self._tempFile); } catch (e) {} self._tempFile = null; }
        self.emit("exit", self._exitCode);
        const rs = self._exitResolvers.splice(0);
        for (const r of rs) r(self._exitCode);
        G.queueMicrotask(() => {
          self._asyncWorkerAlive = false;
          if (typeof G.__mbunAsyncHookDestroy === "function") {
            G.__mbunAsyncHookDestroy(self._asyncMessagePortId);
            G.__mbunAsyncHookDestroy(self._asyncWorkerId);
          }
        });
      });
      installCwdBroadcast();
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
      try { this._child.send(value === undefined ? { t: "m" } : { t: "m", d: encWire(encodeKeysTop(value)) }); } catch (e) {}
      return undefined;
    }
    terminate() {
      if (this._exited) return Promise.resolve(this._exitCode == null ? 1 : this._exitCode);
      try { this._child.kill("SIGTERM"); } catch (e) {}
      return new Promise((resolve) => { this._exitResolvers.push(resolve); });
    }
    ref() { this._refd = true; this._asyncMessagePortRefd = true; if (this._child._rec) this._child._rec.unrefd = false; return this; }
    unref() { this._refd = false; this._asyncMessagePortRefd = false; if (this._child._rec) this._child._rec.unrefd = true; return this; }
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
  let workerDataRaw = null;
  if (MY_TID !== undefined && MY_TID !== null && MY_TID !== "") {
    isMainThread = false;
    threadId = Number(MY_TID) | 0;
    // A MessagePort the parent transferred in. The real port never left the
    // parent, so this end is a local node MessagePort with its outbound half
    // redirected onto the IPC channel; the parent's 'pm' frames feed the other
    // half. Memoised per index so `ports[0] === workerData.p` still holds.
    // Installed before workerData is decoded — that decode is what asks for it.
    wirePort = (i) => {
      let entry = transferredPorts.get(i);
      if (entry) return entry.near;
      const ch = new MessageChannel();
      entry = { near: ch.port1, feed: ch.port2 };
      transferredPorts.set(i, entry);
      Object.defineProperty(ch.port1, "postMessage", {
        configurable: true, writable: true,
        value: function (value) {
          if (typeof proc.send !== "function") return undefined;
          try { proc.send({ t: "p", i: i, d: encWire(value, []) }); } catch (e) {}
          return undefined;
        },
      });
      return ch.port1;
    };
    try { workerData = decWire(JSON.parse(WENV.MBUN_WORKER_DATA || "null")); } catch (e) { workerData = null; }
    // Decoded LAZILY (see the accessor installed on `mod` below): rebuilding a
    // transferred KeyObject/CryptoKey needs node:crypto's real classes, and
    // crypto_asym.cppm patches those in AFTER this partition has run. The
    // accessor replaces itself with a plain data property on first read, so
    // `worker_threads.workerData` still behaves like node's own value.
    workerDataRaw = WENV.MBUN_WORKER_DATA || "null";
    try {
      const ed = JSON.parse(WENV.MBUN_WORKER_ENVDATA || "[]");
      if (Array.isArray(ed)) for (const kv of ed) environmentData.set(kv[0], kv[1]);
    } catch (e) {}
    // Removed before user code runs: these are mbun's transport, not the
    // worker's environment (test-worker-process-env inspects Object.keys(env)).
    try {
      delete proc.env.MBUN_WORKER_TID; delete proc.env.MBUN_WORKER_DATA;
      delete proc.env.MBUN_WORKER_ENVDATA;
    } catch (e) {}
    const chan = new MessageChannel();
    parentPort = chan.port1;
    Object.defineProperty(parentPort, "postMessage", {
      configurable: true, writable: true,
      value: function (value, transferList) {
        if (typeof proc.send !== "function") return undefined;
        try { proc.send(value === undefined ? { t: "m" } : { t: "m", d: encWire(encodeKeysTop(value)) }); } catch (e) {}
        // node detaches every ArrayBuffer in the transfer list; the message is
        // already on the wire, so the parent's copy is unaffected. Ignoring the
        // list left `crypto.sign()`'s buffer alive in the worker after
        // `postMessage(buf, [buf.buffer])`
        // (test-worker-crypto-sign-transfer-result asserts byteLength === 0).
        try {
          for (const item of (normTransfer(transferList) || [])) {
            if (item instanceof ArrayBuffer && !UNTRANSFERABLE.has(item)) {
              try { G.structuredClone(item, { transfer: [item] }); } catch (e) {}
            }
          }
        } catch (e) {}
        return undefined;
      },
    });
    // node makes process.execve main-thread only: inside a worker it throws
    // TypeError ERR_WORKER_UNSUPPORTED_OPERATION rather than replacing the
    // process image out from under the other threads.
    try {
      proc.execve = function execve() {
        const e = new TypeError("process.execve() is not available in workers");
        e.code = "ERR_WORKER_UNSUPPORTED_OPERATION";
        throw e;
      };
    } catch (e) {}
    const reportFatal = (e) => {
      if (typeof proc.send !== "function") return;
      let stack, stackUnavailable = false;
      try { stack = e && e.stack; } catch (_) { stackUnavailable = true; }
      try { proc.send({ t: "e", d: { message: e && e.message, name: e && e.name, stack, stackUnavailable, code: e && e.code } }); } catch (_) {}
    };
    // A fatal error in the worker's ENTRY POINT (a bad specifier, a throw at
    // module scope) reaches the parent as an 'error' event too — node
    // internal/worker.js reports it the same way it reports a later throw. The
    // hook is only defined in a worker process; a normal run never calls it.
    G.__mbunWorkerFatal = (e) => { if (e !== undefined) reportFatal(e); };
    // A worker's process.cwd is a cached read guarded by an Atomics.load on the
    // cwd counter, exactly as node's is — and test-worker-process-cwd asserts
    // that the function's SOURCE shows it. The counter is bumped by the chdir
    // wrapper below, which is what a 'cd' control frame from the parent drives.
    if (rawCwd && AtomicsLoad) {
      let seen = -1;
      let cached = null;
      proc.cwd = function cwd() {
        const counter = AtomicsLoad(cwdCounter, 0);
        if (cached === null || counter !== seen) { seen = counter; cached = rawCwd.call(proc); }
        return cached;
      };
    }
    // Unconditional here (the main thread installs it lazily from the Worker
    // constructor): a 'cd' frame chdir()s through this wrapper, and without it
    // the counter would never move and process.cwd() would keep the stale copy.
    installCwdBroadcast();
    if (typeof proc.on === "function") {
      proc.on("message", (m) => {
        if (m === null || typeof m !== "object") return;
        if (m.t === "m") chan.port2.postMessage(decodeKeysTop(decWire(m.d)));
        else if (m.t === "pm" && typeof m.i === "number") {
          const e = transferredPorts.get(m.i);
          if (e) e.feed.postMessage(decodeKeysTop(decWire(m.d)));
        }
        else if (m.t === "cd" && typeof m.d === "string") { try { proc.chdir(m.d); } catch (e) {} }
      });
    }
    // An uncaught throw inside a worker surfaces as an 'error' event on the
    // parent's Worker handle, not as a bare non-zero exit (node worker.js) —
    // but ONLY when nothing in the worker claimed it. Hooking the runtime's
    // uncaught dispatch instead of registering a plain 'uncaughtException'
    // listener is what makes that distinction possible: a listener would itself
    // count as a claim, so a worker with its own handler (and its own
    // process.exitCode) still died with the forced exit(1) this used to do, and
    // the error the handler ITSELF threw was replaced by the original.
    // This partition is assembled BEFORE node_process_lifecycle installs
    // __mbun_uncaught, so wrap the SLOT, not the value: reads always yield the
    // worker hook and the later plain assignment lands in `base`.
    {
      let base = G.__mbun_uncaught;
      const hook = function (err) {
        // node exits 6 ("Non-function Internal Exception Handler") when
        // process._fatalException has been replaced by a non-function. Guarded
        // on the property EXISTING so a build that never installs the stub
        // keeps the ordinary path instead of turning every throw into a 6.
        if ("_fatalException" in proc && typeof proc._fatalException !== "function") {
          try { proc.exit(6); } catch (_) {}
          return true;
        }
        let claimed = false;
        if (typeof base === "function" && base !== hook) claimed = !!base.call(this, err);
        if (!claimed) {
          // A throw FROM the 'uncaughtException' handler is status 7
          // (kExceptionInFatalExceptionHandler) on the MAIN thread only. node
          // never gives a worker that code: src/node_worker.cc turns any fatal
          // error in a worker into an 'error' on the parent's handle plus
          // exit(1), which is why process-exit-code-cases marks that case
          // `isWorker ? 1 : 7` (test-worker-exit-code case 9).
          if (G.__mbun_fatal_status === 7) { try { G.__mbun_fatal_status = 1; } catch (_) {} }
          // The error the 'uncaughtException' handler ITSELF threw is the one
          // node reports (__mbun_uncaught parks it in __mbun_fatal).
          const f = G.__mbun_fatal;
          reportFatal(f && f.length ? f[0] : err);
        }
        return claimed;
      };
      try {
        Object.defineProperty(G, "__mbun_uncaught", {
          configurable: true, enumerable: false,
          get() { return hook; },
          set(v) { base = v; },
        });
      } catch (e) {}
    }
    // node src/node_process_methods.cc: process.execve refuses to run off the
    // main thread — replacing the process image would take every other thread
    // with it — and throws ERR_WORKER_UNSUPPORTED_OPERATION (a TypeError,
    // lib/internal/errors.js '%s is not supported in workers').
    // test-process-execve-worker-threads asserts exactly that. Defined only
    // here: on the main thread mbun has no execve, and an undefined property is
    // the honest report of that.
    if (typeof proc.execve !== "function") {
      proc.execve = function execve() {
        const e = new TypeError("process.execve() is not supported in workers");
        e.code = "ERR_WORKER_UNSUPPORTED_OPERATION";
        throw e;
      };
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
    isMarkedAsUntransferable,
    markAsUncloneable,
    moveMessagePortToContext,
    setEnvironmentData,
    getEnvironmentData,
    SHARE_ENV,
  };

  if (workerDataRaw !== null) {
    const settle = (v) => {
      Object.defineProperty(mod, "workerData",
        { configurable: true, enumerable: true, writable: true, value: v });
      return v;
    };
    Object.defineProperty(mod, "workerData", {
      configurable: true, enumerable: true,
      // BOTH codecs, in the encoder's inverse order. The sender does
      // encWire(encodeKeysTop(x)), so the reader must decWire first and decode
      // keys second. Running only decodeKeysTop here left a transferred
      // MessagePort as its bare wire token, and the worker got an object with
      // no postMessage (test-worker-workerdata-messageport).
      get() { let v = null; try { v = decodeKeysTop(decWire(JSON.parse(workerDataRaw))); } catch (e) { v = null; } return settle(v); },
      set(v) { settle(v); },
    });
  }

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
