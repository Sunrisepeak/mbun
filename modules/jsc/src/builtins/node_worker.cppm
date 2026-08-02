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
  // Objects that stand in for a node NATIVE handle (an internalBinding class).
  // node's structured clone rejects them with a distinct message, and mbun's
  // stand-ins are ordinary JS objects that would otherwise clone into a
  // meaningless husk of their public fields. Registered by whoever mints them.
  const NATIVE_HOST = new WeakSet();
  G.__mbunMarkNativeHostObject = (o) => {
    if (o !== null && (typeof o === "object" || typeof o === "function")) NATIVE_HOST.add(o);
    return o;
  };

  // ---- JSTransferable (node lib/internal/worker/js_transferable.js) --------
  // A host object that is MOVED rather than cloned answers three symbols:
  // @@kTransferList names what it drags along, @@kTransfer hands back
  // `{ data, deserializeInfo }`, and the receiver rebuilds it by resolving
  // deserializeInfo as `module:Ctor` and calling @@kDeserialize on a fresh
  // instance. mbun had none of it, so `postMessage(fileHandle, [fileHandle])`
  // died with "Object that needs transfer was found in message but not listed
  // in transferList" — about an object that WAS listed.
  //
  // The resolution step is a security boundary, not a convenience: node looks
  // deserializeInfo up in the INTERNAL builtin table, so an object whose
  // @@kTransfer has been overridden cannot name a file on disk and have it
  // loaded (test-worker-message-port-transfer-fake-js-transferable) nor reach a
  // public class that never opted in (…-internal). Both failures are reported
  // to the RECEIVER as a 'messageerror', never to the sender.
  const nodeSymbol = (name) => {
    const reg = G.__mbunNodeSymbols || (G.__mbunNodeSymbols = { __proto__: null });
    return reg[name] || (reg[name] = Symbol(name));
  };
  const kJstTransfer = nodeSymbol("messaging_transfer_symbol");
  const kJstTransferList = nodeSymbol("messaging_transfer_list_symbol");
  const kJstDeserialize = nodeSymbol("messaging_deserialize_symbol");
  const JST_TOK = "__mbunJSTransferable__";
  const isJSTransferable = (v) => {
    if (v === null || (typeof v !== "object" && typeof v !== "function")) return false;
    try { return typeof v[kJstTransfer] === "function"; } catch (e) { return false; }
  };
  const jstMaterialise = (spec) => {
    const info = spec === null || spec === undefined ? undefined : spec.deserializeInfo;
    if (typeof info !== "string") throw new Error("Unknown deserialize spec " + String(info));
    const sep = info.indexOf(":");
    const modName = sep < 0 ? info : info.slice(0, sep);
    const ctorName = sep < 0 ? "" : info.slice(sep + 1);
    const target = M[modName] !== undefined ? M[modName] : M["node:" + modName];
    if (target === undefined || target === null) {
      throw new Error("Missing internal module '" + modName + "'");
    }
    const Ctor = target[ctorName];
    if (typeof Ctor !== "function" || Ctor.prototype === undefined ||
        typeof Ctor.prototype[kJstDeserialize] !== "function") {
      throw new Error("Unknown deserialize spec " + info);
    }
    const obj = new Ctor();
    obj[kJstDeserialize](spec.data);
    return obj;
  };
  const jstResolve = (v, specs, seen) => {
    if (v === null || typeof v !== "object") return v;
    if (Object.prototype.hasOwnProperty.call(v, JST_TOK) && typeof v[JST_TOK] === "number") {
      return jstMaterialise(specs[v[JST_TOK]]);
    }
    if (seen.has(v)) return seen.get(v);
    seen.set(v, v);
    if (Array.isArray(v)) { for (let i = 0; i < v.length; i++) v[i] = jstResolve(v[i], specs, seen); return v; }
    const proto = Object.getPrototypeOf(v);
    if (proto !== Object.prototype && proto !== null) return v;
    for (const k of Object.keys(v)) v[k] = jstResolve(v[k], specs, seen);
    return v;
  };

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
  const portDeliver = (p, data, ports, err) => {
    // A payload that could not be DESERIALISED is node's 'messageerror', not a
    // 'message': the receiver learns the transfer failed and the value never
    // materialises (test-worker-message-port-transfer-fake-js-transferable
    // asserts mustNotCall on 'message' and reads the exception off
    // 'messageerror').
    if (err !== undefined && err !== null) {
      const ee = { data: err, type: "messageerror", target: p, currentTarget: p, ports: [] };
      const oe = p[kOnMsgErr];
      if (typeof oe === "function") oe.call(p, ee);
      const dome = p[kEvt].get("messageerror");
      if (dome && dome.length) for (const l of dome.slice()) { if (l.once) p.removeEventListener("messageerror", l.fn); l.fn.call(p, ee); }
      EventEmitter.prototype.emit.call(p, "messageerror", err);
      return;
    }
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
      portDeliver(p, item.data, item.ports, item.err);
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
  const subst = (v, ports, seen, mods, jsts) => {
    // node reports a function by its SOURCE ("function foo() {} could not be
    // cloned."); JSC's own DataCloneError names it ("foo could not be cloned.")
    // and test-worker-message-port-transfer-native pins node's wording.
    if (typeof v === "function") {
      let src = "function";
      try { src = String(v); } catch (e) {}
      throw dataClone(src + " could not be cloned.");
    }
    if (v === null || typeof v !== "object") return v;
    // A native binding object has internal state no clone can carry; node's
    // serializer refuses it outright rather than emitting an empty husk.
    if (NATIVE_HOST.has(v)) throw dataClone("Cannot clone object of unsupported type.");
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
    if (Array.isArray(v)) { const out = []; seen.set(v, out); for (let k = 0; k < v.length; k++) out[k] = subst(v[k], ports, seen, mods, jsts); return out; }
    const proto = Object.getPrototypeOf(v);
    // Only a HOST object can be a JSTransferable, so the symbol probe lives on
    // the non-plain branch: a plain-object payload never pays for it.
    if (proto !== Object.prototype && proto !== null) {
      if (isJSTransferable(v)) {
        const j = jsts ? jsts.list.indexOf(v) : -1;
        if (j < 0) throw dataClone("Object that needs transfer was found in message but not listed in transferList");
        const o = {}; o[JST_TOK] = j; return o;
      }
      return v;
    }
    const out = {}; seen.set(v, out);
    for (const k of Object.keys(v)) out[k] = subst(v[k], ports, seen, mods, jsts);
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
    const keySlot = C && typeof C.__mbunKeyObjectTransferData === "function"
      ? C.__mbunKeyObjectTransferData(v) : null;
    if (keySlot) {
      const o = {};
      o[KEY_TOK] = { w: 0, kind: keySlot.kind, m: encMaterial(keySlot.material), p: keySlot.passphrase };
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
  const cloneWithPorts = (value, ports, buffers, jsts) => {
      // Two codecs compose here and both are load-bearing. subst/unsubst thread
      // `mods` so a WebAssembly.Module survives structuredClone (JSC silently
      // downgrades one to a plain object), and encodeKeysTop/decodeKeysTop tag
      // KeyObject/CryptoKey because the host clone copies a CryptoKey's
      // prototype but NOT the WeakMap its state lives in — the receiver would
      // otherwise hold something that is `instanceof CryptoKey` with no key in
      // it, which reads as a key and is worse than a failure.
      const mods = [];
      const pre = encodeKeysTop(subst(value, ports, new Map(), mods, jsts));
      const c = decodeKeysTop(G.structuredClone(pre));
    for (const b of buffers) { try { G.structuredClone(b, { transfer: [b] }); } catch (e) {} }
    return (ports.length || mods.length) ? unsubst(c, ports, new Map(), mods) : c;
  };

  // Deliver an ALREADY-SERIALISED inbound value onto a port's pair, bypassing
  // postMessage's transfer-list validation. Every worker IPC frame arrives this
  // way: the sender already ran the clone/transfer rules, so re-running them on
  // receipt is not just wasteful, it REJECTS the frame — a decoded MessagePort
  // stand-in is a port that "needs transfer but was not listed", so a message
  // carrying one died in the receiver with a DataCloneError instead of reaching
  // the handler.
  const deliverLocal = (p, data) => {
    if (!p || p[kDetached] === true) return;
    p[kQueue].push({ data, ports: [] });
    scheduleFlush(p);
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
    const ports = [], buffers = [], jstList = [];
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
      } else if (isJSTransferable(item)) {
        if (UNTRANSFERABLE.has(item)) throw dataClone("Cannot transfer object marked as untransferable");
        if (jstList.indexOf(item) >= 0) throw dataClone("Transfer list contains duplicate object");
        jstList.push(item);
      } else if (item !== null && (typeof item === "object" || typeof item === "function")) {
        if (UNTRANSFERABLE.has(item)) continue;
        throw dataClone("Object that needs transfer was found in message but not listed in transferList");
      } else {
        throw dataClone("Value at index " + buffers.length + " is not transferable");
      }
    }
    // Each JSTransferable is asked for its own transfer list and then for its
    // payload, BEFORE anything is detached — node calls both hooks exactly once
    // per postMessage, and a nested value that cannot itself be transferred
    // fails the whole call (test-worker-message-port-jstransferable-nested-
    // untransferable puts an already-detached MessagePort in the inner list).
    let jsts = null;
    if (jstList.length) {
      jsts = { list: jstList, specs: [] };
      const nested = [];
      for (const item of jstList) {
        let sub;
        try { sub = typeof item[kJstTransferList] === "function" ? item[kJstTransferList]() : []; }
        catch (e) { throw e; }
        if (sub !== null && sub !== undefined) for (const s of sub) nested.push(s);
        jsts.specs.push(item[kJstTransfer]());
      }
      for (const s of nested) {
        if (isPort(s) && s[kDetached] === true) throw dataClone("MessagePort in transfer list is already detached");
        if (UNTRANSFERABLE.has(s)) throw dataClone("Cannot transfer object marked as untransferable");
      }
    }
    if (this[kDetached] === true) return;
    const target = this[kOther];
    const postedToTarget = target !== null && seenPorts.has(target);
    const cloned = cloneWithPorts(value, ports, buffers, jsts);
    if (postedToTarget) {
      // node node_messaging.cc: the channel is lost and a process warning fires.
      if (G.process && typeof G.process.emitWarning === "function") {
        G.process.emitWarning("The target port was posted to itself, and the communication channel was lost");
      }
      severPort(this);
      return;
    }
    if (target === null) return;
    // node resolves deserializeInfo on the RECEIVING side, and a resolution
    // that fails reaches the receiver as 'messageerror' — never back to the
    // sender, which has already given the object away. Both ends of a
    // MessageChannel live in this process, so the resolution happens here and
    // the verdict is queued in the receiver's place.
    if (jsts !== null) {
      let payload;
      try { payload = jstResolve(cloned, jsts.specs, new Map()); }
      catch (e) {
        target[kQueue].push({ err: e instanceof Error ? e : new Error(String(e)), ports: [] });
        scheduleFlush(target);
        return;
      }
      target[kQueue].push({ data: payload, ports: ports.slice() });
      scheduleFlush(target);
      return;
    }
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
    // node node_messaging.cc MessagePort::MoveToContext: a detached port is
    // rejected BEFORE the context argument is looked at, because the move is a
    // transfer and there is nothing left to transfer. Validating the context
    // first reported ERR_INVALID_ARG_TYPE for a closed port
    // (test-worker-message-port-close asserts ERR_CLOSED_MESSAGE_PORT).
    if (port[kDetached] === true) {
      const e = new Error("Cannot send data on closed MessagePort");
      e.code = "ERR_CLOSED_MESSAGE_PORT"; throw e;
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

  // ---- BroadcastChannel ----------------------------------------------------
  // node's BroadcastChannel reaches every THREAD of the process; mbun's workers
  // are child processes, so the fan-out has to be relayed over the same IPC
  // channel the ports use. Without it a channel opened in a worker and one
  // opened in its parent were two unrelated objects that happened to share a
  // name, and neither ever heard the other (test-worker-broadcastchannel's
  // 'worker1' block is exactly that round trip).
  //
  // `fromTid` is where the frame came from, so a relay never echoes to its
  // source: -1 originated here, 0 arrived from the parent, >0 arrived from that
  // child. A worker forwards both up and down, which is what makes a broadcast
  // reach a SIBLING worker.
  let bcLocalDeliver = null;
  const bcRelay = (name, data, fromTid) => {
    let wire;
    try { wire = encWire(data, []); } catch (e) { return; }
    for (const [tid, w] of workerRegistry) {
      if (tid === fromTid) continue;
      try { if (!w._exited && w._child && w._child.connected) w._child.send({ t: "bc", n: name, d: wire }); }
      catch (e) {}
    }
    if (!isMainThread && fromTid !== 0) wsend({ t: "bc", n: name, d: wire });
  };
  const BroadcastChannel = G.BroadcastChannel || (function () {
    const channels = new Map();
    // Every live channel in CREATION order. node dispatches a broadcast round
    // by walking the receiving ports in the order they were constructed and
    // draining each one's queue, not by walking the messages — so with three
    // channels on one name posting in turn, c1 hears everything addressed to it
    // before c2 hears anything (test-worker-broadcastchannel-wpt pins the exact
    // six-event sequence). Scheduling one microtask PER MESSAGE, as this did,
    // interleaves the ports instead and produced 'from c1' where node has
    // 'from c3'.
    const allChannels = [];
    let drainScheduled = false;
    const drainAll = () => {
      drainScheduled = false;
      for (const ch of allChannels.slice()) {
        if (ch._closed) continue;
        const queue = broadcastQueues.get(ch);
        if (queue === undefined) continue;
        // Bounded by what was already queued: a handler that posts back must
        // not be drained inside the round it is answering.
        let n = queue.length;
        while (n-- > 0 && queue.length && !ch._closed) {
          const item = queue.shift();
          const ev = typeof G.MessageEvent === "function"
            ? new G.MessageEvent("message", { data: item.data })
            : { data: item.data, type: "message" };
          ev.target = ch;
          ev.currentTarget = ch;
          if (typeof ch.onmessage === "function") ch.onmessage(ev);
          ch.emit("message", ev);
        }
      }
      for (const ch of allChannels) {
        const queue = broadcastQueues.get(ch);
        if (!ch._closed && queue !== undefined && queue.length) { scheduleDrain(); return; }
      }
    };
    const scheduleDrain = () => {
      if (drainScheduled) return;
      drainScheduled = true;
      G.queueMicrotask(drainAll);
    };
    // The local half of a fan-out, shared by a postMessage raised here and by a
    // 'bc' frame relayed in from another process. `origin` is the channel that
    // posted (a sender never hears itself); a relayed frame has none.
    const deliver = (name, data, origin) => {
      const set = channels.get(name);
      if (!set) return;
      let queued = false;
      for (const ch of set) {
        if (ch === origin || ch._closed) continue;
        broadcastQueues.get(ch).push({ data });
        queued = true;
      }
      if (queued) scheduleDrain();
    };
    bcLocalDeliver = deliver;
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
        allChannels.push(this);
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
        deliver(this.name, data, this);
        bcRelay(this.name, data, -1);
      }
      close() {
        assertBroadcast(this);
        if (this._closed) return;
        this._closed = true;
        const set = channels.get(this.name);
        if (set) { set.delete(this); if (set.size === 0) channels.delete(this.name); }
        const at = allChannels.indexOf(this);
        if (at >= 0) allChannels.splice(at, 1);
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
    value: function (depth, options, inspect) {
      if (!(this instanceof BroadcastChannel)) {
        const e = new TypeError("Value of \"this\" must be of type BroadcastChannel");
        e.code = "ERR_INVALID_THIS";
        throw e;
      }
      if (typeof depth === "number" && depth < 0) return "BroadcastChannel";
      // node lib/internal/worker/io.js formats the {name, active} pair through
      // inspect ITSELF with the caller's options (depth decremented), so
      // `util.inspect(bc, { compact: true, breakLength: 2 })` wraps like any
      // other object. Hand-rolling the string ignored every user option.
      const fmt = typeof inspect === "function"
        ? inspect
        : (() => { const u = M["util"] || M["node:util"]; return u && u.inspect; })();
      const body = { name: this.name, active: this._closed !== true };
      if (typeof fmt === "function") {
        const opts = Object.assign({}, options);
        if (opts.depth !== null && typeof opts.depth === "number") opts.depth = opts.depth - 1;
        try { return "BroadcastChannel " + fmt(body, opts); } catch (e) {}
      }
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
  const streamM = M["stream"] || M["node:stream"];
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
    if (/;base64\s*$/i.test(meta)) {
      if (G.Buffer) return G.Buffer.from(payload, "base64").toString("utf8");
      return G.atob ? G.atob(payload) : payload;
    }
    try { return decodeURIComponent(payload); } catch (e) { return payload; }
  };

  // The media type of a data: URL — everything between `data:` and the first
  // comma, with the parameter list (`;charset=…`) and the `;base64` marker
  // stripped. An omitted type is `text/plain` per RFC 2397, but node's ESM
  // loader treats the EMPTY string as "no type given" and falls through to its
  // JavaScript default, so it is reported as "" here rather than defaulted.
  const dataUrlMime = (p) => {
    const comma = p.indexOf(",");
    if (comma === -1) return "";
    const meta = p.slice(5, comma);
    const semi = meta.indexOf(";");
    return (semi === -1 ? meta : meta.slice(0, semi)).trim();
  };

  // ---- which options a worker thread may carry ----------------------------
  // node's per-isolate + per-environment option table (src/node_options.cc), i.e.
  // exactly the options whose effect is scoped to one thread. PROCESS-wide
  // options (--title, --v8-options, --perf-*) and V8 flags (--expose-gc,
  // --stack-size, --max-old-space-size, --jitless) are deliberately absent:
  // node refuses them in a worker's execArgv, and "absent from the list" is how
  // that refusal is spelled here. Underscores are normalised to dashes first,
  // so `--pending_deprecation` and `--pending-deprecation` are one entry.
  const WORKER_EXEC_OPTIONS = new Set((
      "abort-on-uncaught-exception allow-addons allow-child-process allow-fs-read " +
      "allow-fs-write allow-net allow-wasi allow-worker conditions cpu-prof " +
      "cpu-prof-dir cpu-prof-interval cpu-prof-name disable-proto disable-sigusr1 " +
      "disable-warning dns-result-order enable-network-family-autoselection " +
      "enable-source-maps entry-url env-file env-file-if-exists experimental-abortcontroller " +
      "experimental-addon-modules experimental-config-file experimental-default-config-file " +
      "experimental-detect-module experimental-eventsource experimental-import-meta-resolve " +
      "experimental-json-modules experimental-loader experimental-modules " +
      "experimental-network-imports experimental-permission experimental-print-required-tla " +
      "experimental-quic experimental-repl-await experimental-require-module " +
      "experimental-shadow-realm experimental-specifier-resolution experimental-sqlite " +
      "experimental-strip-types experimental-test-coverage experimental-test-isolation " +
      "experimental-test-module-mocks experimental-test-snapshots experimental-transform-types " +
      "experimental-vm-modules experimental-wasi-unstable-preview1 experimental-wasm-modules " +
      "experimental-webstorage expose-internals force-context-aware " +
      "force-node-api-uncaught-exceptions-policy frozen-intrinsics " +
      "heap-prof heap-prof-dir heap-prof-interval heap-prof-name " +
      "heapsnapshot-near-heap-limit heapsnapshot-signal http-parser " +
      "icu-data-dir import input-type insecure-http-parser inspect inspect-brk " +
      "inspect-brk-node inspect-port inspect-publish-uid inspect-wait " +
      "localstorage-file max-http-header-size napi-modules network-family-autoselection-attempt-timeout " +
      "no-addons no-async-context-frame no-deprecation no-experimental-detect-module " +
      "no-experimental-fetch no-experimental-global-customevent " +
      "no-experimental-global-navigator no-experimental-global-webcrypto " +
      "no-experimental-repl-await no-experimental-require-module no-experimental-websocket " +
      "no-experimental-print-required-tla no-extra-info-on-fatal-exception " +
      "no-force-async-hooks-checks no-global-search-paths no-network-family-autoselection " +
      "no-use-system-ca no-warnings openssl-config openssl-legacy-provider " +
      "openssl-shared-config pending-deprecation policy-integrity preserve-symlinks " +
      "preserve-symlinks-main prof-process redirect-warnings report-compact report-dir " +
      "report-directory report-exclude-env report-exclude-network report-filename " +
      "report-on-fatalerror report-on-signal report-signal report-uncaught-exception " +
      "require secure-heap secure-heap-min snapshot-blob test test-concurrency " +
      "test-coverage-branches test-coverage-exclude test-coverage-functions " +
      "test-coverage-include test-coverage-lines test-force-exit test-name-pattern " +
      "test-only test-reporter test-reporter-destination test-shard test-skip-pattern " +
      "test-timeout test-udp-no-try-send throw-deprecation tls-cipher-list tls-keylog " +
      "tls-max-v1.2 tls-max-v1.3 tls-min-v1.0 tls-min-v1.1 tls-min-v1.2 tls-min-v1.3 " +
      "trace-atomics-wait trace-deprecation trace-env trace-env-js-stack " +
      "trace-env-native-stack trace-event-categories trace-event-file-pattern " +
      "trace-events-enabled trace-exit trace-promises trace-require-module trace-sigint " +
      "trace-sync-io trace-tls trace-uncaught trace-warnings track-heap-objects " +
      "unhandled-rejections use-bundled-ca use-largepages use-openssl-ca use-system-ca " +
      "watch watch-path watch-preserve-output zero-fill-buffers " +
      // Short spellings node's parser accepts, plus the bun-side per-thread
      // flags mbun answers to (worker_threads is a surface bun implements too).
      "r C smol user-agent").split(" "));
  // Options that are meaningless without an argument; node's parser reports
  // `<option> requires an argument` and the Worker constructor turns that into
  // ERR_WORKER_INVALID_EXEC_ARGV (test-worker-execargv-invalid's
  // `--redirect-warnings`).
  const WORKER_EXEC_OPTIONS_WITH_VALUE = new Set((
      "conditions cpu-prof-dir cpu-prof-interval cpu-prof-name disable-warning " +
      "dns-result-order entry-url env-file env-file-if-exists experimental-loader " +
      "experimental-specifier-resolution heap-prof-dir heap-prof-interval heap-prof-name " +
      "heapsnapshot-near-heap-limit heapsnapshot-signal icu-data-dir import input-type " +
      "localstorage-file max-http-header-size network-family-autoselection-attempt-timeout " +
      "openssl-config policy-integrity redirect-warnings report-dir report-directory " +
      "report-filename report-signal require secure-heap secure-heap-min snapshot-blob " +
      "test-concurrency test-coverage-exclude test-coverage-include test-name-pattern " +
      "test-reporter test-reporter-destination test-shard test-skip-pattern test-timeout " +
      "tls-cipher-list tls-keylog trace-event-categories trace-event-file-pattern " +
      "unhandled-rejections watch-path").split(" "));
  // NODE_OPTIONS is parsed against a DIFFERENT and larger table than a worker's
  // execArgv: node's kAllowedInEnvvar, which admits options that are
  // process-wide precisely because the environment applies to the process.
  // `--title` is the case the corpus pins from both sides — refused in execArgv
  // (test-worker-execargv-invalid) and accepted in NODE_OPTIONS
  // (test-worker-node-options, whose fixture copies the parent's env wholesale).
  // Only a genuinely UNKNOWN option is refused here.
  const NODE_OPTIONS_EXTRA = new Set((
      "diagnostic-dir interpreted-frames-native-stack max-old-space-size " +
      "max-semi-space-size perf-basic-prof perf-basic-prof-only-functions perf-prof " +
      "perf-prof-unwinding-info stack-trace-limit title tls-keylog " +
      "max-http-header-size v8-pool-size").split(" "));
  const invalidExecArgv = (what, tok) => {
    const e = new Error("Initiated Worker with invalid " + what + ": " + tok);
    e.code = "ERR_WORKER_INVALID_EXEC_ARGV";
    return e;
  };

  // node ERR_WORKER_PATH: a bare specifier is not a worker entry point.
  const workerEntryPath = (filename) => {
    let p = filename;
    if (p !== null && typeof p === "object" && typeof p.href === "string") {
      if (p.protocol === "data:") return { source: dataUrlSource(p.href), mime: dataUrlMime(p.href) };
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

  // ---- postMessageToThread (node lib/internal/worker/messaging.js) ---------
  // worker_threads exports it and mbun did not, so every `postMessageToThread`
  // call in the corpus died with "is not a function" before it could assert
  // anything (test-worker-messaging-errors-invalid / -handler / -timeout /
  // test-worker-messaging).
  //
  // node's own transport CANNOT be ported: it hands the destination thread a
  // SharedArrayBuffer, blocks the sender on AtomicsWaitAsync and has the
  // receiver AtomicsStore the delivery verdict into it. An mbun worker is a
  // child PROCESS, so that buffer is a copy and the verdict would never come
  // back. What is observable — the promise settles with exactly one of
  // DELIVERED / no-listener / listener-threw / timed-out — is a request/response
  // pair, so that is what rides the existing IPC channel: a 'wm' frame carrying
  // a correlation id, answered by a 'wr' frame carrying the verdict. The
  // SharedArrayBuffer was node's signalling mechanism, never its contract.
  //
  // Routing is main-thread-mediated exactly as node's is (every thread's port
  // goes to the main thread, which forwards): a worker's request travels up as
  // `{t:"wm", dest}` and the main thread either emits it locally (dest 0) or
  // relays it to that destination's channel.
  // ---- the worker's own IPC handle, kept after process.send is disabled -----
  // node takes process.send/chdir/abort/… away inside a worker (they are
  // process-global, not thread-local). mbun's worker wire IS process.send, so
  // every internal frame has to go through the captured original rather than
  // the public property. `sendHost` is a receiver whose `connected` still reads
  // true: the real send() checks `this.connected`, and the public property
  // becomes a throwing getter one line later.
  let rawSendW = null;
  let rawChdirW = null;
  const wsend = (frame) => {
    if (rawSendW !== null) { try { rawSendW(frame); return true; } catch (e) { return false; } }
    if (typeof proc.send !== "function") return false;
    try { proc.send(frame); return true; } catch (e) { return false; }
  };
  const wsendable = () => rawSendW !== null || typeof proc.send === "function";

  const WM_DELIVERED = 0, WM_NO_LISTENERS = 1, WM_LISTENER_ERROR = 2;
  let wmSeq = 0;
  const wmPending = new Map();
  // node reports "no listener" and "the listener threw" as DIFFERENT errors, so
  // the listener count has to be read before the emit rather than inferred from
  // process.emit()'s boolean (which a throwing listener never returns).
  const emitWorkerMessage = (value, source) => {
    let n = 0;
    try { n = typeof proc.listenerCount === "function" ? proc.listenerCount("workerMessage") : 0; } catch (e) { n = 0; }
    if (n === 0) return WM_NO_LISTENERS;
    try { proc.emit("workerMessage", value, source); } catch (e) { return WM_LISTENER_ERROR; }
    return WM_DELIVERED;
  };
  const wmSettle = (m) => {
    const res = wmPending.get(m.id);
    if (res !== undefined) { wmPending.delete(m.id); res(m.r); }
  };
  const wmAsk = (send) => {
    const id = ++wmSeq;
    return new Promise((resolve) => {
      wmPending.set(id, resolve);
      let ok = false;
      try { ok = send(id) !== false; } catch (e) { ok = false; }
      if (!ok) { wmPending.delete(id); resolve(WM_NO_LISTENERS); }
    });
  };
  const wmSendToWorker = (w, source, wire) => {
    if (!w || w._exited || !w._child || !w._child.connected) return Promise.resolve(WM_NO_LISTENERS);
    return wmAsk((id) => w._child.send({ t: "wm", id: id, src: source, d: wire }));
  };
  // Called on the main thread for a request that arrived from a worker.
  const wmRoute = (dest, source, wire) => {
    if (dest === threadId) return Promise.resolve(emitWorkerMessage(decodeKeysTop(decWire(wire)), source));
    return wmSendToWorker(workerRegistry.get(dest), source, wire);
  };
  // A timeout is the caller's deadline on the ROUND TRIP, so the timer is
  // unref'd: node's AtomicsWaitAsync does not hold the loop open either.
  const wmWithTimeout = (p, timeout) => {
    if (timeout === undefined || !(timeout >= 0) || !isFinite(timeout)) return p;
    return new Promise((resolve) => {
      let done = false;
      const t = G.setTimeout(() => { if (!done) { done = true; resolve("timeout"); } }, timeout);
      try { if (t && typeof t.unref === "function") t.unref(); } catch (e) {}
      p.then((r) => { if (!done) { done = true; try { G.clearTimeout(t); } catch (e) {} resolve(r); } });
    });
  };
  const wmErr = (code, message) => { const e = new Error(message); e.code = code; return e; };
  const postMessageToThread = function (destination, value, transferList, timeout) {
    // An async body on purpose: node's postMessageToThread is an async function,
    // so EVERY rejection (including the synchronous same-thread one) arrives as
    // a rejected promise — `assert.rejects(() => postMessageToThread(threadId))`
    // depends on it.
    return (async function () {
      if (typeof transferList === "number" && timeout === undefined) { timeout = transferList; transferList = []; }
      if (timeout !== undefined) {
        if (typeof timeout !== "number" || timeout !== timeout) {
          const e = new TypeError('The "timeout" argument must be of type number. Received ' + recvType(timeout));
          e.code = "ERR_INVALID_ARG_TYPE"; throw e;
        }
        if (timeout < 0) {
          const e = new RangeError('The value of "timeout" is out of range. It must be >= 0. Received ' + timeout);
          e.code = "ERR_OUT_OF_RANGE"; throw e;
        }
      }
      if (destination === threadId) {
        throw wmErr("ERR_WORKER_MESSAGING_SAME_THREAD", "Cannot sent a message to the same thread");
      }
      const tlPorts = [];
      for (const item of validateTransferList(transferList)) if (isPort(item)) tlPorts.push(item);
      let code;
      if (isMainThread) {
        const w = workerRegistry.get(destination);
        if (!w || w._exited) code = WM_NO_LISTENERS;
        else {
          w._wirePorts(tlPorts);
          const wire = value === undefined ? undefined : encWire(encodeKeysTop(value), w._tlPorts);
          code = await wmWithTimeout(wmSendToWorker(w, threadId, wire), timeout);
        }
      } else {
        const wire = value === undefined ? undefined : encWire(encodeKeysTop(value), tlPorts);
        const dest = destination;
        code = await wmWithTimeout(wmAsk((id) => {
          if (!wsendable()) return false;
          return wsend({ t: "wm", id: id, dest: dest, src: threadId, d: wire });
        }), timeout);
      }
      if (code === "timeout") throw wmErr("ERR_WORKER_MESSAGING_TIMEOUT", "Sending a message to another thread timed out");
      if (code === WM_NO_LISTENERS) throw wmErr("ERR_WORKER_MESSAGING_FAILED", "Cannot find the destination thread or listener");
      if (code === WM_LISTENER_ERROR) throw wmErr("ERR_WORKER_MESSAGING_ERRORED", "The destination thread threw an error while processing the message");
      return undefined;
    })();
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
      // options.name → the thread's name (node v24.6 threadName). node TRIMS
      // nothing and ignores a FALSY name (''/0/false/undefined all mean "no
      // name"), but a truthy non-string is ERR_INVALID_ARG_TYPE.
      let wname = "";
      if (options.name) {
        if (typeof options.name !== "string") {
          const e = new TypeError('The "options.name" property must be of type string. Received ' + recvType(options.name));
          e.code = "ERR_INVALID_ARG_TYPE"; throw e;
        }
        wname = options.name;
      }
      if (options.argv !== undefined && options.argv !== null && !Array.isArray(options.argv)) {
        const e = new TypeError('The "options.argv" property must be an instance of Array. Received ' + recvType(options.argv));
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      if (options.execArgv !== undefined && options.execArgv !== null && !Array.isArray(options.execArgv)) {
        const e = new TypeError('The "options.execArgv" property must be an instance of Array. Received ' + recvType(options.execArgv));
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      // node runs the execArgv array through its OWN option parser before the
      // thread exists (src/node_worker.cc Worker::New -> ParsePerIsolateOptions),
      // and rejects the whole construction with ERR_WORKER_INVALID_EXEC_ARGV if
      // any token is not an option a thread may carry. Three distinct things are
      // refused and the corpus asserts all three: an option node does not know
      // at all (`--foo`), an option that is PROCESS-wide rather than per-thread
      // (`--title=blah` renames the whole process), and a V8 option (`--expose-gc`
      // — V8 flags are set on the isolate at creation and a worker inherits the
      // parent's, so passing one per-worker is meaningless). A fourth case is an
      // option that is legal but was given without its required value
      // (`--redirect-warnings` needs a file). mbun spawns the worker as a CHILD
      // PROCESS, so it used to hand the array straight to spawn() and the bad
      // flag surfaced — if at all — as a dead child much later, never as the
      // synchronous throw the constructor's contract promises.
      // Allow-list rather than deny-list, because that is the shape of the
      // question: "is this one of the options a thread may carry", not "is this
      // one of the bad ones". Anything not listed is refused.
      const validateExecArgvList = (list, what, extra) => {
        for (let i = 0; i < list.length; i++) {
          const tok = String(list[i]);
          // A bare token is the VALUE of the option before it (`--require foo`),
          // never an option itself.
          if (tok.charCodeAt(0) !== 45 /* - */) continue;
          const eq = tok.indexOf("=");
          let name = (eq === -1 ? tok : tok.slice(0, eq));
          while (name.charCodeAt(0) === 45) name = name.slice(1);
          name = name.replace(/_/g, "-");
          // A token made of nothing but dashes is not a flag and must never be
          // looked up in the table. `--` is node's END-OF-OPTIONS separator:
          // everything after it is a positional argument, so validation stops
          // dead rather than continuing to judge script args as flags. A lone
          // `-` is node's spelling of "read the program from stdin", also not an
          // option. test-process-exec-argv passes the parent's own execArgv
          // straight into a Worker — `['--pending-deprecation', '--']` — which is
          // exactly how a real caller reaches this, and the first cut of the
          // allow-list rejected the separator as an unknown flag.
          if (name === "") { if (tok === "--") break; continue; }
          if (!WORKER_EXEC_OPTIONS.has(name) && !(extra && extra.has(name))) throw invalidExecArgv(what, tok);
          // Needs a value and got none, and there is no following token to take
          // it from.
          if (eq === -1 && WORKER_EXEC_OPTIONS_WITH_VALUE.has(name) &&
              (i + 1 >= list.length || String(list[i + 1]).charCodeAt(0) === 45)) {
            throw invalidExecArgv(what, tok);
          }
        }
      };
      if (Array.isArray(options.execArgv)) validateExecArgvList(options.execArgv.map(String), "execArgv flags");
      // NODE_OPTIONS travels in the worker's env, and node parses it with the
      // same table before the thread starts — an unparseable NODE_OPTIONS is the
      // same ERR_WORKER_INVALID_EXEC_ARGV. Only an EXPLICIT env is checked: an
      // inherited one already survived this process's own startup.
      // …and never re-checked when it is the value THIS process already started
      // with: a copied env carries the parent's NODE_OPTIONS unchanged, and the
      // parent accepting it at startup is the whole proof it is valid.
      if (options.env && options.env !== SHARE_ENV && typeof options.env.NODE_OPTIONS === "string" &&
          options.env.NODE_OPTIONS !== (proc.env && proc.env.NODE_OPTIONS)) {
        validateExecArgvList(options.env.NODE_OPTIONS.split(/\s+/).filter((s) => s !== ""),
                             "NODE_OPTIONS env variable", NODE_OPTIONS_EXTRA);
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
      this.threadName = wname;
      // node writes the worker's thread_name metadata row into the trace file
      // (src/node_worker.cc Worker::Worker). No-op while tracing is off.
      try {
        const T = G.__mbunTraceEvents;
        if (T && typeof T.emitWorkerThreadName === "function") T.emitWorkerThreadName(wname, tid);
      } catch (e) {}
      this._tempFile = null;
      let entry;
      let isEval = false;
      if (options.eval) {
        entry = writeTempWorker(String(filename), tid, ".js");
        this._tempFile = entry;
        isEval = true;
      } else {
        const r = workerEntryPath(filename);
        if (r.source !== undefined) {
          // ref: node lib/internal/modules/esm/load.js + translators.js — a
          // `data:` worker entry goes through the ESM loader, which is what
          // decides both of the observable behaviours here. mbun used to spill
          // the payload into a `.js` temp file, i.e. run it as CommonJS, and
          // that made three of test-worker-data-url's six cases silently
          // succeed where node fails: a non-JS media type has no module format
          // at all, `module.exports = {}` is a ReferenceError in a module (but
          // ordinary CJS in a script), and a rejecting top-level await is a
          // module-evaluation rejection (but not even parseable as a script).
          //
          // The media type is everything before the first `,`, minus any
          // parameters (`;charset=utf-8`) and the `;base64` marker that
          // dataUrlSource already consumed.
          const mt = String(r.mime || "").trim().toLowerCase();
          if (mt !== "" && mt !== "text/javascript" && mt !== "application/javascript" &&
              mt !== "text/ecmascript" && mt !== "application/ecmascript") {
            // Reported as an 'error' on the handle, not thrown from the
            // constructor: node discovers it inside the worker's own loader.
            entry = writeTempWorker(
                "const e = new TypeError(\"Unknown module format: \" + " + JSON.stringify(mt) +
                    ");\ne.code = 'ERR_UNKNOWN_MODULE_FORMAT';\nthrow e;\n",
                tid, ".js");
          } else {
            entry = writeTempWorker(r.source, tid, ".mjs");
          }
          this._tempFile = entry;
          isEval = true;
        } else entry = r.path;
      }
      // An entry point that does not RESOLVE is an 'error' event on the parent's
      // handle carrying the module loader's own error (node reports the CJS/ESM
      // resolution failure through internal/worker.js like any other fatal
      // error in the worker). mbun runs the entry in a child mbun process, and a
      // target that cannot be found is rejected by the CLI *before* the runtime
      // exists — there is no JS context left to report from, so the child prints
      // `error: Module not found …` and exits 1 with nothing on the wire.
      // Turning the miss into a worker whose body throws node's error routes it
      // through the already-working top-level-throw path instead of bolting a
      // second, spawn-less error path onto the constructor.
      // Only for a path that CANNOT gain a resolution: an extensionless
      // specifier still goes to the child, which probes .js/.json/index.js.
      if (!isEval && /\.(?:[cm]?js|[cm]?ts|jsx|tsx|json|node)$/.test(entry)) {
        let missing = false;
        try { missing = !fsM.existsSync(entry); } catch (e) { missing = false; }
        if (missing) {
          entry = writeTempWorker(
              "const e = new Error(\"Cannot find module '\" + " + JSON.stringify(entry) +
                  " + \"'\");\ne.code = 'MODULE_NOT_FOUND';\nthrow e;\n",
              tid, ".js");
          this._tempFile = entry;
        }
      }

      // Env: SHARE_ENV and the default both inherit; an explicit object replaces.
      const baseEnv = (options.env && options.env !== SHARE_ENV) ? options.env : (proc.env || {});
      const env = {};
      for (const k of Object.keys(baseEnv)) { const v = baseEnv[k]; if (v !== undefined && v !== null) env[k] = String(v); }
      env.MBUN_WORKER_TID = String(tid);
      env.MBUN_WORKER_DATA = workerDataJson;
      if (wname !== "") env.MBUN_WORKER_NAME = wname;
      // node evaluates an `eval: true` worker as a STRING, so its process.argv[1]
      // is the fixed placeholder '[worker eval]'. mbun materialises the string as
      // a temp file to hand the child a real entry point, so the child has to be
      // told to report node's placeholder instead of the temp path.
      if (isEval) env.MBUN_WORKER_EVAL = "1";
      // environmentData is inherited by every worker STARTED FROM HERE, as a
      // snapshot: node clones the parent's store into the new thread at spawn
      // time, so a later setEnvironmentData in the parent must not reach it.
      // Entry pairs (not an object) so non-string keys survive.
      try { env.MBUN_WORKER_ENVDATA = JSON.stringify(Array.from(environmentData)); }
      catch (e) { env.MBUN_WORKER_ENVDATA = "[]"; }

      // mbun implements Worker with a child process. Inherited execArgv must
      // remain byte-for-byte public API, but replaying its relative tsconfig
      // after process.chdir() would resolve against the wrong cwd. Pass the
      // normalized parse-time value out of band only when execArgv is inherited;
      // child startup consumes and erases this key before process.env exists.
      const inheritsExecArgv = options.execArgv == null;
      if (inheritsExecArgv && typeof proc.__mbunTsconfigOverride === "string" &&
          proc.__mbunTsconfigOverride !== "") {
        env.MBUN_INTERNAL_TSCONFIG_OVERRIDE = proc.__mbunTsconfigOverride;
      }

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
      const execArgv = stripEval(inheritsExecArgv ? ((proc.execArgv || []).map(String))
                                                  : options.execArgv.map(String));
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
      // node's worker stdout/stderr are MESSAGE PORTS, not pipes: the worker's
      // process.stdout is a Writable whose every write becomes one message, and
      // the parent turns each message back into exactly one 'data' chunk. Chunk
      // BOUNDARIES are therefore part of the contract, and the corpus asserts
      // them directly — test-worker-no-stdin-stdout-interaction wants ten writes
      // to arrive as ten 'data' events, and test-worker-message-port-drain
      // matches each worker line against a whole chunk.
      //
      // An mbun worker is a child PROCESS whose fd 1 is an OS pipe, and a pipe
      // has no message boundaries at all: ten quick writes coalesce in the
      // kernel buffer and the parent reads one chunk (measured: 1 event, not 10;
      // and "1 threadId: 1\n2 threadId: 1" as a single chunk). So the worker's
      // writes ride the IPC channel as 'so'/'se' frames — the same channel the
      // messages use, which is also what makes the drain test's ORDERING hold —
      // and this stream is where they are re-emitted, one chunk per frame.
      //
      // child.stdout is still merged in rather than dropped: anything that
      // reaches the child's real fd 1 without passing through its
      // process.stdout (the runtime's own fatal-error printer, a grandchild
      // process, a native write) has no frame and would otherwise vanish.
      // A plain Readable that is PUSHED into, not a PassThrough that is written
      // to: a Transform queues its writes and hands them on together, and a
      // byte-mode Readable's read() CONCATENATES whatever is sitting in its
      // buffer — so two frames that arrive in one IPC batch came back out as one
      // 'data' event and the boundary this whole change exists to keep was lost
      // again (measured: "1 threadId: 1\n2 threadId: 1\n" as a single chunk).
      // push() on a flowing Readable emits each chunk as it lands, which is what
      // node's ReadableWorkerStdio does.
      const mkStdioStream = () => {
        if (!streamM || typeof streamM.Readable !== "function") return null;
        try { return new streamM.Readable({ read() {} }); } catch (e) { return null; }
      };
      const outStream = mkStdioStream();
      const errStream = mkStdioStream();
      this._outStream = outStream;
      this._errStream = errStream;
      this.stdout = outStream || child.stdout;
      this.stderr = errStream || child.stderr;
      if (child.stdout) child.stdout.on("data", (d) => {
        if (outStream) { try { outStream.push(d); return; } catch (e) {} }
        if (!options.stdout) { try { proc.stdout.write(d); } catch (e) {} }
      });
      if (child.stderr) child.stderr.on("data", (d) => {
        if (errStream) { try { errStream.push(d); return; } catch (e) {} }
        if (!options.stderr) { try { proc.stderr.write(d); } catch (e) {} }
      });
      // node pipes the worker's stdio into the parent's unless the caller asked
      // to own the stream (options.stdout / options.stderr).
      if (!options.stdout && outStream) outStream.on("data", (d) => { try { proc.stdout.write(d); } catch (e) {} });
      if (!options.stderr && errStream) errStream.on("data", (d) => { try { proc.stderr.write(d); } catch (e) {} });
      workerRegistry.set(tid, this);

      const self = this;
      // A MessagePort transferred into the worker keeps living HERE; the worker
      // holds a stand-in whose two directions are these frames. 'p' is the
      // worker posting on its stand-in (delivered to this port's pair), 'pm' is
      // anything posted to this port travelling the other way.
      this._tlPorts = [];
      this._wirePorts(tlPorts);
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
          if (p) { try { deliverLocal(p[kOther], decodeKeysTop(decWire(m.d))); } catch (e) {} }
        } else if (m.t === "e") {
          // node serializes a worker's fatal error and REBUILDS it in the
          // parent with its own class (internal/error_serdes.js keeps the
          // native error constructors), so `err.constructor === RangeError`
          // holds for a stack overflow and `=== SyntaxError` for a bad entry
          // point. Rebuilding everything as a plain Error and only patching
          // .name left the constructor wrong, which is exactly what the
          // stack-overflow and syntax-error tests assert on.
          const ctors = { Error: Error, TypeError: TypeError, RangeError: RangeError,
                          SyntaxError: SyntaxError, ReferenceError: ReferenceError,
                          EvalError: EvalError, URIError: URIError };
          const nm = m.d && m.d.name;
          const Ctor = (typeof nm === "string" &&
                        Object.prototype.hasOwnProperty.call(ctors, nm)) ? ctors[nm] : Error;
          const err = new Ctor(m.d && m.d.message ? m.d.message : String(m.d));
          if (m.d && m.d.name) err.name = m.d.name;
          if (m.d && m.d.stack) err.stack = m.d.stack;
          // The worker could not READ err.stack (a Error.prepareStackTrace that
          // throws). node's error_serdes leaves the deserialized error's stack
          // undefined in that case, so the freshly-built Error's OWN stack must
          // be cleared rather than left standing in for the worker's.
          else if (m.d && m.d.noStack === true) { try { err.stack = undefined; } catch (e) {} }
          if (m.d && m.d.code) err.code = m.d.code;
          if (typeof self.onerror === "function") self.onerror(err);
          self.emit("error", err);
        } else if (m.t === "so" || m.t === "se") {
          // One stdio write in the worker, re-emitted here as exactly one chunk.
          // base64 because the IPC channel carries JSON and a worker's stdout is
          // a byte stream, not text.
          const s = m.t === "so" ? self._outStream : self._errStream;
          let buf = null;
          try { buf = G.Buffer.from(String(m.d), "base64"); } catch (e) { buf = null; }
          if (buf === null) return;
          if (s) { try { s.push(buf); } catch (e) {} }
          else { try { (m.t === "so" ? proc.stdout : proc.stderr).write(buf); } catch (e) {} }
        } else if (m.t === "me") {
          self.emit("messageerror", new Error(String(m.d)));
        } else if (m.t === "wm" && typeof m.id === "number") {
          // postMessageToThread, travelling UP from this worker. The main thread
          // is the router (as it is in node), so resolve the destination here
          // and send the verdict back down on the same correlation id.
          const reqId = m.id;
          wmRoute(typeof m.dest === "number" ? m.dest : threadId,
                  typeof m.src === "number" ? m.src : -1, m.d)
            .then((r) => {
              try { if (!self._exited && self._child.connected) self._child.send({ t: "wr", id: reqId, r: r }); }
              catch (e) {}
            });
        } else if (m.t === "wr" && typeof m.id === "number") {
          wmSettle(m);
        } else if (m.t === "bc" && typeof m.n === "string") {
          // A BroadcastChannel post from this worker: fan out here, then relay
          // to every OTHER worker (and, in a nested worker, on up).
          const d = decWire(m.d);
          if (bcLocalDeliver) bcLocalDeliver(m.n, d, null);
          bcRelay(m.n, d, tid);
        }
      });
      child.on("error", (e) => self.emit("error", e));
      child.on("exit", (code, signal) => {
        self._exited = true;
        self._exitCode = signal ? 1 : (code == null ? 1 : code);
        self._asyncMessagePortRefd = false;
        // node internal/worker.js [kOnExit]: the handle is dropped, so both
        // identifiers report "not running" — threadId -1 and threadName null
        // (test-worker-safe-getters / test-worker-thread-name assert exactly
        // this from the 'exit' listener).
        self.threadId = -1;
        self.threadName = null;
        workerRegistry.delete(tid);
        if (self._tempFile) { try { fsM.unlinkSync(self._tempFile); } catch (e) {} self._tempFile = null; }
        // The synthetic stdio streams have no fd to close themselves on: end
        // them with the thread, so a reader waiting on 'end' is not left open.
        if (self._outStream) { try { self._outStream.push(null); } catch (e) {} }
        if (self._errStream) { try { self._errStream.push(null); } catch (e) {} }
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
      // `new Worker(url, { ref: false })` — bun's web Worker option for "do not
      // keep the parent alive". mbun already had the machinery: unref() drops
      // both the keep-alive flag and the child channel's ref, and calling it
      // right after construction worked. The CONSTRUCTOR option was simply never
      // read, so the parent hung on a worker it was told not to wait for. (This
      // is the same shape as bun's own regression: `user_keep_alive` was set
      // from the option and never consulted.) node's worker_threads.Worker has
      // no `ref` option, so nothing in that corpus reaches this line.
      if (options.ref === false) { try { this.unref(); } catch (e) {} }
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
    // A MessagePort transferred to the worker keeps living HERE; the worker gets
    // a stand-in whose two directions are IPC frames carrying this port's index.
    // 'p' is the worker posting on its stand-in (delivered to this port's pair),
    // 'pm' is anything posted to this port travelling the other way.
    //
    // The index space is the WORKER's, not one transfer list's, so it grows
    // across calls: the constructor's list fills 0..n-1 and every later
    // postMessage appends. Keeping it per-call was the whole bug — Worker's
    // postMessage ignored its transferList entirely, so a port sent after
    // construction reached the worker as a bare token with no postMessage and
    // the worker's `parentPort.on('message', ({port}) => …)` never answered
    // (test-worker-message-channel and test-worker-message-port-message-before-
    // close hang on exactly that, they do not fail).
    _wirePorts(ports) {
      const self = this;
      for (const p of ports) {
        if (this._tlPorts.indexOf(p) !== -1) continue;
        const idx = this._tlPorts.length;
        this._tlPorts.push(p);
        p.on("message", (d) => {
          try {
            if (!self._exited && self._child.connected) {
              self._child.send({ t: "pm", i: idx, d: encWire(d, self._tlPorts) });
            }
          } catch (e) {}
        });
      }
    }
    postMessage(value, transferList) {
      const list = validateTransferList(transferList);
      const tlPorts = [], tlBuffers = [];
      for (const item of list) {
        if (isPort(item)) {
          if (item[kDetached] === true) throw dataClone("MessagePort in transfer list is already detached");
          tlPorts.push(item);
        } else if (item instanceof ArrayBuffer) {
          if (abDetached(item)) throw dataClone("ArrayBuffer at index " + tlBuffers.length + " is already detached");
          if (!UNTRANSFERABLE.has(item)) tlBuffers.push(item);
        }
      }
      if (this._exited || !this._child.connected) return undefined;
      // Registered BEFORE the encode: encWire resolves a port to its index in
      // this list, and an unlisted one is node's DataCloneError.
      this._wirePorts(tlPorts);
      // `{t:"m"}` with no `d` IS the undefined message: JSON drops the key.
      try { this._child.send(value === undefined ? { t: "m" } : { t: "m", d: encWire(encodeKeysTop(value), this._tlPorts) }); } catch (e) {}
      // The message is on the wire, so the transferred buffers can be detached.
      for (const b of tlBuffers) { try { G.structuredClone(b, { transfer: [b] }); } catch (e) {} }
      return undefined;
    }
    terminate() {
      // node internal/worker.js: the promise fulfils with UNDEFINED, and with
      // an already-dropped handle it is `PromiseResolve()` — never the exit
      // code (test-worker-terminate-null-handler asserts the `undefined`).
      if (this._exited) return Promise.resolve();
      try { this._child.kill("SIGTERM"); } catch (e) {}
      return new Promise((resolve) => { this._exitResolvers.push(resolve); }).then(() => undefined);
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
  // node names the process's first thread "MainThread" (src/node.cc
  // uv_thread_setname), and worker_threads reports that name as threadName.
  let threadName = "MainThread";
  let parentPort = null;
  let workerData = null;
  let workerDataRaw = null;
  if (MY_TID !== undefined && MY_TID !== null && MY_TID !== "") {
    isMainThread = false;
    // Bun.isMainThread is a plain data property installed as `true` by
    // install_bindings_ (it has no way to know it is a worker); the worker
    // child is the only place that knows otherwise. `import { isMainThread }
    // from "bun"` reads the same object (builtin_module maps "bun" → Bun).
    try { if (G.Bun) G.Bun.isMainThread = false; } catch (e) {}
    threadId = Number(MY_TID) | 0;
    threadName = typeof WENV.MBUN_WORKER_NAME === "string" ? WENV.MBUN_WORKER_NAME : "";
    // node's `eval: true` worker never has a file, so process.argv is
    // [execPath, '[worker eval]', ...argv]. The child was handed a temp file to
    // make the entry real; the placeholder is restored before user code runs.
    if (WENV.MBUN_WORKER_EVAL === "1" && Array.isArray(proc.argv) && proc.argv.length > 1) {
      try { proc.argv[1] = "[worker eval]"; } catch (e) {}
    }
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
          if (!wsendable()) return undefined;
          wsend({ t: "p", i: i, d: encWire(value, []) });
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
      delete proc.env.MBUN_WORKER_ENVDATA; delete proc.env.MBUN_WORKER_NAME;
      delete proc.env.MBUN_WORKER_EVAL;
    } catch (e) {}
    const chan = new MessageChannel();
    parentPort = chan.port1;
    Object.defineProperty(parentPort, "postMessage", {
      configurable: true, writable: true,
      value: function (value, transferList) {
        if (!wsendable()) return undefined;
        wsend(value === undefined ? { t: "m" } : { t: "m", d: encWire(encodeKeysTop(value)) });
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
    // ---- DOM WorkerGlobalScope message surface ------------------------------
    // bun's web corpus drives a worker through the WEB spelling, not node's:
    // `self.postMessage(x)`, `self.onmessage = e => ...`,
    // `self.addEventListener("message", ...)`. Every part needed for that was
    // already present and simply unconnected — parentPort carries both
    // directions, the global is wired to a hidden EventTarget with
    // addEventListener/dispatchEvent, MessageEvent exists, and
    // node_process_extra installs the `onmessage` handler-IDL accessor onto the
    // global. Nothing bridged the two, so inside a worker `postMessage` was an
    // undefined global (js/web/workers/create-port-worker.js died with
    // `ReferenceError: postMessage is not defined`) and a `self.onmessage`
    // assignment silently never fired.
    //
    // The inbound hop is spliced into the IPC frame handler below rather than
    // implemented as a parentPort listener ON PURPOSE: a node MessagePort stays
    // closed until the user attaches a sink, and receiveMessageOnPort() drains
    // that queue, so an always-on bridge listener would start the port and eat
    // the FIFO out from under it. parentPort's state machine is left untouched.
    //
    // scopeHasMessageSink asks the GLOBAL EventTarget whether anything is
    // listening for "message" there, which is what `self.onmessage = …` and
    // `self.addEventListener("message", …)` both come down to. It is read, never
    // cached: this partition is assembled BEFORE node_process_extra, so at this
    // point G.addEventListener does not exist yet and no wrapper installed here
    // could ever see a registration (the first attempt counted zero of them).
    // node_process_extra publishes the hidden global target it binds those
    // methods to as __mbunGlobalEventTarget for exactly this question.
    const scopeHasMessageSink = () => {
      if (typeof G.onmessage === "function") return true;
      const t = G.__mbunGlobalEventTarget;
      if (t && typeof t.listeners === "function") {
        try { return t.listeners("message").length > 0; } catch (e) {}
      }
      return false;
    };
    if (typeof G.postMessage !== "function") {
      G.postMessage = function postMessage(value, transferList) {
        return parentPort.postMessage(value, transferList);
      };
    }
    // Dispatched in addition to (not instead of) the parentPort delivery: a
    // worker is free to use either spelling and neither may starve the other.
    const dispatchScopeMessage = (data) => {
      if (!scopeHasMessageSink()) return;
      let ev;
      try { ev = new G.MessageEvent("message", { data: data }); }
      catch (e) { ev = { type: "message", data: data, ports: [] }; }
      try { G.dispatchEvent(ev); } catch (e) {}
    };
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
      if (!wsendable()) return;
      // EVERY field is read in its own try: they are user-visible getters. A
      // single try around the whole frame meant one throwing accessor lost the
      // WHOLE report — an Error.prepareStackTrace that throws for its own error
      // made `.stack` throw, so nothing was ever sent and the parent's 'error'
      // event never fired at all (test-worker-error-stack-getter-throws, which
      // asserts the propagated error arrives with stack === undefined).
      const d = {};
      try { d.message = e && e.message; } catch (_) {}
      // JSC spells the stack-exhaustion RangeError with a trailing full stop;
      // V8 — and therefore node's public contract, which the corpus asserts
      // verbatim — does not. This is the one message the engines disagree about
      // that crosses the worker boundary as DATA rather than as console output,
      // so the serializer that rebuilds the error in the parent is where the
      // engine's wording is translated into node's.
      if (d.message === "Maximum call stack size exceeded.") d.message = "Maximum call stack size exceeded";
      try { d.name = e && e.name; } catch (_) {}
      try { d.stack = e && e.stack; } catch (_) { d.noStack = true; }
      try { d.code = e && e.code; } catch (_) {}
      wsend({ t: "e", d });
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
        if (m.t === "m") {
          const d = decodeKeysTop(decWire(m.d));
          deliverLocal(parentPort, d);
          dispatchScopeMessage(d);
        }
        else if (m.t === "pm" && typeof m.i === "number") {
          const e = transferredPorts.get(m.i);
          if (e) deliverLocal(e.near, decodeKeysTop(decWire(m.d)));
        }
        else if (m.t === "cd" && typeof m.d === "string") { try { (rawChdirW || proc.chdir).call(proc, m.d); } catch (e) {} }
        // postMessageToThread, arriving from the main thread's router.
        else if (m.t === "wm" && typeof m.id === "number") {
          const r = emitWorkerMessage(decodeKeysTop(decWire(m.d)),
                                      typeof m.src === "number" ? m.src : 0);
          wsend({ t: "wr", id: m.id, r: r });
        }
        else if (m.t === "wr" && typeof m.id === "number") { wmSettle(m); }
        else if (m.t === "bc" && typeof m.n === "string") {
          const d = decWire(m.d);
          if (bcLocalDeliver) bcLocalDeliver(m.n, d, null);
          bcRelay(m.n, d, 0);
        }
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
    G.__mbunIpcPin = () => portHasListener(parentPort) || scopeHasMessageSink();

    // ---- the operations a worker does not get -----------------------------
    // node lib/internal/bootstrap/switches/is_not_main_thread.js: anything that
    // mutates PROCESS state rather than thread state is replaced by a stub
    // carrying `.disabled === true` and throwing ERR_WORKER_UNSUPPORTED_OPERATION
    // (test-worker-unsupported-things walks the whole list). The V8-inspector
    // debug hooks are deleted outright rather than stubbed.
    //
    // Called from node_process_extra, NOT here: process.send does not exist yet
    // at this point in the image — __mbunSetupIpcChild assigns it later — so a
    // stub installed here would simply be overwritten by the real one.
    G.__mbunWorkerDisableProcessOps = function () {
      if (typeof proc.send === "function" && proc.send.disabled !== true) {
        const realSend = proc.send;
        // A receiver that still reports an open channel: attachIpc's send()
        // guards on `this.connected`, and `connected` becomes a getter that
        // throws two blocks below.
        const sendHost = Object.create(proc);
        try { Object.defineProperty(sendHost, "connected", { value: true, writable: true, configurable: true }); } catch (e) {}
        rawSendW = (frame) => realSend.call(sendHost, frame);
      }
      if (typeof proc.chdir === "function" && proc.chdir.disabled !== true) {
        const realChdir = proc.chdir;
        rawChdirW = realChdir.bind(proc);
      }
      // ---- stdio as messages, not as a pipe ---------------------------------
      // node's worker stdout/stderr ARE message ports (internal/worker/io.js
      // WritableWorkerStdio): one write is one message, and the parent turns it
      // back into one 'data' chunk. mbun's worker is a child process whose fd 1
      // is an OS pipe, which has no boundaries — ten writes coalesce into one
      // read and the corpus notices (see the parent-side note above). Sending
      // each write as its own IPC frame restores the boundary AND the ordering
      // against postMessage, since both now ride the same channel.
      //
      // Installed from HERE rather than at partition-assembly time because
      // process.stdout does not exist yet when the worker_threads partition is
      // evaluated; node_process_extra calls this after the process object is
      // complete. The real stream stays underneath as the fallback for a frame
      // that cannot be built or sent.
      const frameStdio = (stream, tag) => {
        if (!stream || typeof stream.write !== "function" || stream.write.__mbunFramed) return;
        const realWrite = stream.write;
        const framed = function write(chunk, enc, cb) {
          if (typeof enc === "function") { cb = enc; enc = undefined; }
          let buf = null;
          try {
            buf = (G.Buffer && G.Buffer.isBuffer(chunk))
                ? chunk
                : (chunk instanceof Uint8Array ? G.Buffer.from(chunk.buffer, chunk.byteOffset, chunk.byteLength)
                                               : G.Buffer.from(String(chunk), enc || "utf8"));
          } catch (e) { buf = null; }
          if (buf === null || !wsendable()) return realWrite.call(this, chunk, enc, cb);
          let sent = false;
          try { sent = wsend({ t: tag, d: buf.toString("base64") }); } catch (e) { sent = false; }
          if (!sent) return realWrite.call(this, chunk, enc, cb);
          // node's WritableWorkerStdio calls the write callback once the message
          // is handed off, i.e. asynchronously but unconditionally
          // (test-worker-no-stdin-stdout-interaction passes common.mustSucceed()
          // as that callback).
          if (typeof cb === "function") {
            try { proc.nextTick(() => cb(null)); } catch (e) { try { cb(null); } catch (_) {} }
          }
          return true;
        };
        try {
          framed.__mbunFramed = true;
          stream.write = framed;
        } catch (e) {}
      };
      frameStdio(proc.stdout, "so");
      frameStdio(proc.stderr, "se");
      const unsupported = (name) => {
        const f = function () {
          const e = new TypeError("process." + name + "() is not supported in workers");
          e.code = "ERR_WORKER_UNSUPPORTED_OPERATION";
          throw e;
        };
        f.disabled = true;
        return f;
      };
      for (const name of ["abort", "chdir", "send", "disconnect", "setuid", "seteuid",
                          "setgid", "setegid", "setgroups", "initgroups"]) {
        try { proc[name] = unsupported(name); } catch (e) {}
      }
      // READING the umask is fine off the main thread; only setting it is a
      // process-wide change, so node keeps the getter and rejects the setter.
      {
        const realUmask = proc.umask;
        try {
          proc.umask = function umask(mask) {
            if (mask === undefined) return typeof realUmask === "function" ? realUmask.call(proc) : 0;
            const e = new TypeError("Setting process.umask() is not supported in workers");
            e.code = "ERR_WORKER_UNSUPPORTED_OPERATION";
            throw e;
          };
        } catch (e) {}
      }
      for (const name of ["channel", "connected"]) {
        try {
          Object.defineProperty(proc, name, {
            configurable: true, enumerable: false,
            get() {
              const e = new TypeError("process." + name + " is not supported in workers");
              e.code = "ERR_WORKER_UNSUPPORTED_OPERATION";
              throw e;
            },
            set() {},
          });
        } catch (e) {}
      }
      for (const name of ["_startProfilerIdleNotifier", "_stopProfilerIdleNotifier",
                          "_debugProcess", "_debugPause", "_debugEnd"]) {
        try { delete proc[name]; } catch (e) {}
      }
    };
  }

  const mod = {
    Worker,
    isMainThread,
    parentPort,
    threadId,
    threadName,
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
    postMessageToThread,
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
  //
  // The GLOBAL Worker is the WEB one, and bun's follows the web spec: its first
  // argument is a *URL string*, so `new Worker(new URL(x, import.meta.url).href)`
  // is the idiomatic form (test/js/web/workers, broadcastchannel). node's
  // `worker_threads.Worker` is deliberately stricter — a `file://` or `data:`
  // STRING is ERR_WORKER_PATH there and must stay that way (node's own
  // test-worker-invalid-filename asserts it). One class cannot satisfy both, so
  // the global gets a thin subclass that pre-wraps those two string forms in a
  // URL — which is exactly the spelling node itself accepts — and
  // `node:worker_threads` keeps the unmodified strict class.
  class WebWorker extends Worker {
    constructor(filename, options) {
      if (typeof filename === "string" &&
          (filename.startsWith("file://") || filename.startsWith("data:"))) {
        try { filename = new URL(filename); } catch (e) {}
      }
      super(filename, options);
    }
  }
  Object.defineProperty(WebWorker, "name", { value: "Worker", configurable: true });
  if (typeof G.Worker === "undefined") G.Worker = WebWorker;
  // node's global MessageChannel/MessagePort ARE the worker_threads ones (they
  // are re-exported onto globalThis since v15), so the node-parity classes win
  // over the load-order web stubs.
  G.MessageChannel = MessageChannel;
  G.MessagePort = MessagePort;
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
