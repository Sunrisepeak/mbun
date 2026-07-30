// node:timers + node:timers/promises completion partition.
//
// bootstrap registers the "timers" module BEFORE the global timer functions
// exist, so its exports captured undefined (import { setTimeout } from
// "node:timers" was broken). This shard, appended at the end of the chain,
// wraps the live globals with thin node-aligned wrappers that add:
//   - proper .name on setTimeout/setInterval/setImmediate/clear* functions
//   - Timeout._destroyed / Immediate._destroyed lifecycle (false while armed
//     and during the callback, true after clearing or firing; refresh() re-arms)
//   - unref()/ref() chaining (always return the timer)
//   - clearTimeout/clearInterval accept numeric-string ids, reject malformed
//     ones (" 1", "1 ", "01", "+1", overflow) and never clear Immediates
//   - Symbol.for("nodejs.util.promisify.custom") hooks (promisify(setTimeout)
//     -> promise, promisify(setInterval) -> async iterator)
// and re-registers M["timers"] in place plus upgrades M["timers/promises"]
// with AbortSignal support (signal/ref options, AbortError rejection) and
// scheduler.wait/yield.
//
// NOTE: appended AFTER the master builtins IIFE; self-contained IIFE that
// re-binds G = globalThis. Top level must never throw.
//
// Blueprint: node lib/timers.js + lib/timers/promises.js semantics (bun's
// src/js/node/timers.promises.ts mirrors the same contract, including
// accepting an AbortController where options are expected via options.signal).
export module mbun.jsc.js_builtins:node_timers;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeTimersJS = R"JS(
(function () {
  const G = globalThis;
  try {
    const M = G.__mbunNativeModules;
    if (!M) return;
    const oSetTimeout = G.setTimeout, oClearTimeout = G.clearTimeout;
    const oSetInterval = G.setInterval, oClearInterval = G.clearInterval;
    const oSetImmediate = G.setImmediate, oClearImmediate = G.clearImmediate;
    if (typeof oSetTimeout !== "function" || typeof oSetInterval !== "function") return;

    // node validateFunction(callback, "callback"): a non-function callback to
    // setTimeout/setInterval/setImmediate throws ERR_INVALID_ARG_TYPE (a
    // TypeError). See lib/timers.js.
    const __recv = (v) => (v === null ? "null" : typeof v === "object" ? "an instance of " + ((v.constructor && v.constructor.name) || "Object") : typeof v === "string" ? "type string ('" + v + "')" : "type " + typeof v + " (" + String(v) + ")");
    const __invalidCb = (v) => { const e = new TypeError('The "callback" argument must be of type function. Received ' + __recv(v)); e.code = "ERR_INVALID_ARG_TYPE"; return e; };

    const KIND = Symbol("mbun.timerKind");
    const STATE = Symbol("mbun.timerState");
    // The host scheduler returns plain objects.  Node exposes stable handle
    // identities, including their public constructor names, to callbacks and
    // async-context users; keep the native object as the facade rather than
    // wrapping it so numeric ids/ref state remain shared.
    function Timeout() {}
    function Immediate() {}
    const registry = new Map(); // numeric id -> timer object (timeouts/intervals)
    // process.getActiveResourcesInfo() tracking: node reports both setTimeout
    // and setInterval handles as 'Timeout', and setImmediate as 'Immediate'.
    // A timeout/interval stays active until it is destroyed (fires or cleared);
    // an Immediate is dropped just before its callback runs (node semantics —
    // test-process-getactiveresources-track-timer-lifetime asserts 0 inside).
    const activeTimeouts = new Set();
    const activeImmediates = new Set();

    const idOf = (t) => { try { const n = +t; return Number.isSafeInteger(n) ? n : null; } catch (_) { return null; } };

    // node lib/internal/timers.js Timeout: `_idleTimeout` is the *enrolled*
    // duration (node coerces an absent / out-of-range delay to 1, the same
    // coercion _checkCountdown warns about) and `_idleStart` the libuv
    // timestamp the timer was last armed at. Both are read straight off a
    // handle that internals keep under the private `kTimeout` symbol —
    // `socket[kTimeout]._idleTimeout` in test-http-client-timeout-on-connect
    // and test-tls-wrap-timeout, `session[kTimeout]._idleTimeout` in
    // test-http2-compat-socket — and unenroll (clearTimeout, or the timer
    // firing) resets `_idleTimeout` to -1, which test-tls-wrap-timeout asserts
    // from its 'exit' handler.
    //
    // Non-enumerable, unlike node's own data properties: mbun's util.inspect
    // prints every enumerable key of a Timeout and several corpus files pin
    // that exact output, so making these visible would trade the four files
    // above for whatever asserts on an inspected timer.
    const TIMEOUT_MAX = 2147483647;
    const idleDuration = (ms) => (typeof ms === "number" && ms >= 1 && ms <= TIMEOUT_MAX ? ms : 1);
    // node's _idleStart is `libuv now` (ms since loop start), which is
    // monotonic and non-decreasing; process.uptime() is the same clock here.
    const idleNow = () => { try { return Math.trunc(G.process.uptime() * 1000); } catch (_) { return Date.now(); } };
    const setIdle = (t, ms) => {
      try {
        Object.defineProperty(t, "_idleTimeout", { value: idleDuration(ms), writable: true, enumerable: false, configurable: true });
        Object.defineProperty(t, "_idleStart", { value: idleNow(), writable: true, enumerable: false, configurable: true });
      } catch (_) {}
    };

    function initTimer(t, kind, state) {
      if (t === null || typeof t !== "object") return t;
      t[KIND] = kind;
      t[STATE] = state;
      if (kind !== "immediate") setIdle(t, state.ms);
      try {
        Object.defineProperty(t, "constructor", {
          value: kind === "immediate" ? Immediate : Timeout,
          configurable: true,
        });
      } catch (_) {}
      if (t._destroyed === undefined) {
        try { Object.defineProperty(t, "_destroyed", { value: false, writable: true, enumerable: false, configurable: true }); }
        catch (_) { t._destroyed = false; }
      } else t._destroyed = false;
      if (kind !== "immediate") {
        const id = idOf(t);
        if (id !== null) registry.set(id, t);
        activeTimeouts.add(t);
      } else {
        activeImmediates.add(t);
      }
      const timerHooks = G.__mbunAsyncHookTimer;
      if (timerHooks && typeof timerHooks.init === "function") {
        state.timerHooks = timerHooks;
        state.asyncHook = timerHooks.init(t, kind === "immediate" ? "Immediate" : "Timeout");
      }
      // unref()/ref() must chain (node returns the timer).
      const oUnref = t.unref, oRef = t.ref;
      if (typeof oUnref === "function") t.unref = function unref() { const r = oUnref.call(state.native); return r === undefined ? t : r; };
      if (typeof oRef === "function") t.ref = function ref() { const r = oRef.call(state.native); return r === undefined ? t : r; };
      if (kind === "timeout") {
        // node Timeout.refresh(): re-arms even from inside (or after) the
        // callback. The native refresh cannot re-arm a fired timer, so always
        // schedule a fresh native timer behind the stable facade object.
        t.refresh = function refresh() {
          state.gen++;
          try { oClearTimeout(state.native); } catch (_) {}
          state.native = oSetTimeout(state.run, state.ms, ...state.args);
          // node Timeout.refresh() re-enrols the handle: _idleStart advances
          // (test-tls-wrap-timeout asserts the later start is strictly greater)
          // and a previously unenrolled timer gets its duration back.
          setIdle(t, state.ms);
          t._destroyed = false;
          const id = idOf(t);
          if (id !== null) registry.set(id, t);
          activeTimeouts.add(t);
          return t;
        };
      }
      // Native Symbol.dispose clears the host timer directly, bypassing this
      // facade's registry and `_destroyed` lifecycle.  Route it through the
      // same path as clearTimeout/clearImmediate instead.
      if (Symbol.dispose) {
        try {
          Object.defineProperty(t, Symbol.dispose, {
            value: function dispose() { clearNative(t); destroyTimer(t); },
            configurable: true,
          });
        } catch (_) {}
      }
      return t;
    }

    function destroyTimer(t) {
      if (t === null || typeof t !== "object") return;
      const state = t[STATE];
      if (state && state.timerHooks && typeof state.timerHooks.destroy === "function") {
        state.timerHooks.destroy(state.asyncHook);
      }
      t._destroyed = true;
      // node unenroll(): a cleared or fired Timeout reports _idleTimeout = -1.
      if (t[KIND] !== "immediate") { try { t._idleTimeout = -1; } catch (_) {} }
      const id = idOf(t);
      if (id !== null) registry.delete(id);
      activeTimeouts.delete(t);
      activeImmediates.delete(t);
    }

    function clearNative(t) {
      const st = t[STATE];
      const n = st && st.native !== undefined ? st.native : t;
      if (st) st.gen++;
      if (t[KIND] === "immediate") { try { oClearImmediate(n); } catch (_) {} }
      else { try { oClearTimeout(n); } catch (_) {} try { oClearInterval(n); } catch (_) {} }
    }

    // ref: bun src/runtime/timer/Timer.rs (CountdownOverflowBehavior::OneMs).
    // Negative/NaN warn once per VM (warned_negative_number/warned_not_number);
    // overflow warns every time. The exact predicate matters and is the point of
    // issue #18159: setTimeout(fn) with no delay, and a non-number delay such as
    // "abc", must NOT warn — only a genuine NaN *number* does.
    let warnedNaN = false, warnedNeg = false;
    const SUFFIX = ".\nTimeout duration was set to 1.";
    const _warnCountdown = (m, t) => { try { G.process.emitWarning(m + SUFFIX, t); } catch (_) {} };
    const _checkCountdown = (ms) => {
      if (ms >= 1 && ms <= 2147483647) return;
      if (ms > 2147483647) _warnCountdown((Number.isFinite(ms) ? ms : "Infinity") + " does not fit into a 32-bit signed integer", "TimeoutOverflowWarning");
      else if (ms < 0 && !warnedNeg) { warnedNeg = true; _warnCountdown((Number.isFinite(ms) ? ms : "-Infinity") + " is a negative number", "TimeoutNegativeWarning"); }
      else if (ms !== undefined && typeof ms === "number" && Number.isNaN(ms) && !warnedNaN) { warnedNaN = true; _warnCountdown("NaN is not a number", "TimeoutNaNWarning"); }
    };

    // Scheduling seam for node:domain (and any future async-context
    // interceptor). It is a SLOT consulted per call, never a replacement of the
    // global functions: node's test/common/index.js snapshots the identity of
    // globalThis.setTimeout/setInterval/setImmediate/queueMicrotask when it
    // loads and its 'exit' listener fails the file with "Unexpected global(s)
    // found" for any global whose VALUE changed since. A lazy
    // require('domain') that swapped those four cost 43 test-repl-* files
    // (node:repl loads domain to own an eval's uncaught exceptions), and the
    // same trap waits for anything else that wants to intercept scheduling.
    // Cost when nothing is installed: one property read per schedule.
    const __sched = (cb) => {
      const h = G.__mbunSchedHook;
      return h === undefined || h === null ? cb : h(cb);
    };
    const __runTimerCallback = (state, cb, thisArg, args) => {
      if (state.timerHooks && typeof state.timerHooks.run === "function") {
        return state.timerHooks.run(state.asyncHook, cb, thisArg, args);
      }
      return cb.apply(thisArg, args);
    };

    const mySetTimeout = function setTimeout(cb, ms, ...args) {
      if (typeof cb !== "function") throw __invalidCb(cb);
      _checkCountdown(ms);
      cb = __sched(cb);
      const state = { gen: 0, ms, args };
      state.run = function (...a) {
        const g = state.gen;
        try { return __runTimerCallback(state, cb, state.timer, a); }
        // refresh()/clear during the callback bumps gen: skip the destroy.
        finally { if (state.gen === g && state.timer) destroyTimer(state.timer); }
      };
      const t = oSetTimeout(state.run, ms, ...args);
      state.timer = t;
      state.native = t;
      return initTimer(t, "timeout", state);
    };
    const mySetInterval = function setInterval(cb, ms, ...args) {
      if (typeof cb !== "function") throw __invalidCb(cb);
      _checkCountdown(ms);
      cb = __sched(cb);
      const state = { gen: 0, ms, args };
      state.run = function (...a) { return __runTimerCallback(state, cb, state.timer, a); };
      const t = oSetInterval(state.run, ms, ...args);
      state.timer = t;
      state.native = t;
      return initTimer(t, "interval", state);
    };
    const mySetImmediate = function setImmediate(cb, ...args) {
      if (typeof cb !== "function") throw __invalidCb(cb);
      cb = __sched(cb);
      const state = { gen: 0 };
      state.run = function (...a) {
        const g = state.gen;
        // node drops the Immediate from the active set before its callback runs.
        if (state.timer) activeImmediates.delete(state.timer);
        try { return __runTimerCallback(state, cb, state.timer, a); }
        finally { if (state.gen === g && state.timer) destroyTimer(state.timer); }
      };
      const t = oSetImmediate(state.run, ...args);
      state.timer = t;
      state.native = t;
      return initTimer(t, "immediate", state);
    };

    // clearTimeout/clearInterval accept the timer object, a numeric id, or a
    // canonical numeric string id. Malformed strings (" 1", "1 ", "01", "+1",
    // out-of-range) are ignored, matching node. Immediates are never cleared
    // by clearTimeout/clearInterval.
    function resolveClearable(x) {
      if (x === null || x === undefined) return { forward: false };
      if (typeof x === "object") return { forward: true, timer: x };
      if (typeof x === "number") return { forward: true, timer: registry.get(x) };
      if (typeof x === "string") {
        if (x.length === 0 || x.length > 15 || !/^[1-9][0-9]*$/.test(x)) return { forward: false };
        const id = Number(x);
        const t = registry.get(id);
        return t ? { forward: true, timer: t } : { forward: false };
      }
      return { forward: false };
    }
    const myClearTimeout = function clearTimeout(x) {
      const r = resolveClearable(x);
      if (!r.forward) return;
      const t = r.timer;
      if (t !== undefined && t !== null && typeof t === "object") {
        if (t[KIND] === "immediate") return;
        clearNative(t);
        destroyTimer(t);
      } else {
        try { oClearTimeout(x); } catch (_) {}
      }
    };
    const myClearInterval = function clearInterval(x) { return myClearTimeout(x); };
    const myClearImmediate = function clearImmediate(x) {
      if (x === null || typeof x !== "object") return;
      if (x[KIND] !== undefined && x[KIND] !== "immediate") return;
      clearNative(x);
      destroyTimer(x);
    };

    // ----------------------------------------------------- timers/promises
    function abortError(signal) {
      let reason;
      try { reason = signal ? signal.reason : undefined; } catch (_) {}
      const e = new Error("The operation was aborted");
      e.name = "AbortError";
      e.code = "ABORT_ERR";
      if (reason !== undefined) e.cause = reason;
      return e;
    }
    const signalOf = (options) => {
      // node/bun accept { signal } and (informally) an AbortController whose
      // .signal is read the same way.
      if (options === null || typeof options !== "object") return undefined;
      const s = options.signal;
      return (s && typeof s === "object") ? s : undefined;
    };

    function tpSetTimeout(after = 1, value, options) {
      const signal = signalOf(options);
      const ref = options && typeof options === "object" ? options.ref : undefined;
      return new Promise((resolve, reject) => {
        if (signal && signal.aborted) return reject(abortError(signal));
        let onAbort;
        const t = mySetTimeout(() => {
          if (signal && onAbort) { try { signal.removeEventListener("abort", onAbort); } catch (_) {} }
          resolve(value);
        }, after);
        if (ref === false && t && typeof t.unref === "function") t.unref();
        if (signal) {
          onAbort = () => { myClearTimeout(t); reject(abortError(signal)); };
          try { signal.addEventListener("abort", onAbort, { once: true }); } catch (_) {}
        }
      });
    }
    function tpSetImmediate(value, options) {
      const signal = signalOf(options);
      const ref = options && typeof options === "object" ? options.ref : undefined;
      return new Promise((resolve, reject) => {
        if (signal && signal.aborted) return reject(abortError(signal));
        let onAbort;
        const t = mySetImmediate(() => {
          if (signal && onAbort) { try { signal.removeEventListener("abort", onAbort); } catch (_) {} }
          resolve(value);
        });
        if (ref === false && t && typeof t.unref === "function") t.unref();
        if (signal) {
          onAbort = () => { myClearImmediate(t); reject(abortError(signal)); };
          try { signal.addEventListener("abort", onAbort, { once: true }); } catch (_) {}
        }
      });
    }
    function tpSetInterval(after = 1, value, options) {
      const signal = signalOf(options);
      const ref = options && typeof options === "object" ? options.ref : undefined;
      return {
        [Symbol.asyncIterator]() {
          let done = false;
          return {
            next() {
              if (done) return Promise.resolve({ value: undefined, done: true });
              if (signal && signal.aborted) { done = true; return Promise.reject(abortError(signal)); }
              return new Promise((resolve, reject) => {
                let onAbort;
                const t = mySetTimeout(() => {
                  if (signal && onAbort) { try { signal.removeEventListener("abort", onAbort); } catch (_) {} }
                  resolve({ value, done: false });
                }, after);
                if (ref === false && t && typeof t.unref === "function") t.unref();
                if (signal) {
                  onAbort = () => { myClearTimeout(t); done = true; reject(abortError(signal)); };
                  try { signal.addEventListener("abort", onAbort, { once: true }); } catch (_) {}
                }
              });
            },
            return() {
              done = true;
              return Promise.resolve({ value: undefined, done: true });
            },
          };
        },
      };
    }
    // node exposes `scheduler` as an instance of an unconstructable Scheduler
    // class: `new scheduler.constructor()` must throw ERR_ILLEGAL_CONSTRUCTOR.
    let __schedAllow = false;
    class Scheduler {
      constructor() { if (!__schedAllow) { const e = new TypeError("Illegal constructor"); e.code = "ERR_ILLEGAL_CONSTRUCTOR"; throw e; } }
      wait(delay, options) { return tpSetTimeout(delay, undefined, options); }
      yield() { return tpSetImmediate(); }
    }
    __schedAllow = true;
    const scheduler = new Scheduler();
    __schedAllow = false;

    // Upgrade the existing timers/promises module object IN PLACE so any
    // captured references (timers.promises, prior imports) see the new impls.
    const TP = M["timers/promises"] || M["node:timers/promises"] || {};
    TP.setTimeout = tpSetTimeout;
    TP.setInterval = tpSetInterval;
    TP.setImmediate = tpSetImmediate;
    TP.scheduler = scheduler;
    M["timers/promises"] = M["node:timers/promises"] = TP;

    // promisify hooks (node attaches these to the timer functions themselves).
    const custom = Symbol.for("nodejs.util.promisify.custom");
    try {
      Object.defineProperty(mySetTimeout, custom, { value: (after, value) => tpSetTimeout(after, value), configurable: true });
      Object.defineProperty(mySetImmediate, custom, { value: (value) => tpSetImmediate(value), configurable: true });
      Object.defineProperty(mySetInterval, custom, { value: (after, value) => tpSetInterval(after, value), configurable: true });
    } catch (_) {}

    // Install wrappers globally and re-register node:timers in place.
    G.setTimeout = mySetTimeout;
    G.setInterval = mySetInterval;
    G.setImmediate = mySetImmediate;
    G.clearTimeout = myClearTimeout;
    G.clearInterval = myClearInterval;
    G.clearImmediate = myClearImmediate;

    // process.getActiveResourcesInfo(): report active timer resources (node
    // groups setTimeout+setInterval as 'Timeout', setImmediate as 'Immediate').
    // Installed unconditionally so it wins over the []-returning stub regardless
    // of partition order.
    try {
      if (G.process) {
        G.process.getActiveResourcesInfo = function getActiveResourcesInfo() {
          const out = [];
          for (let i = 0; i < activeTimeouts.size; i++) out.push("Timeout");
          for (let i = 0; i < activeImmediates.size; i++) out.push("Immediate");
          return out;
        };
      }
    } catch (_) {}

    const timersMod = M["timers"] || M["node:timers"] || {};
    timersMod.setTimeout = mySetTimeout;
    timersMod.clearTimeout = myClearTimeout;
    timersMod.setInterval = mySetInterval;
    timersMod.clearInterval = myClearInterval;
    timersMod.setImmediate = mySetImmediate;
    timersMod.clearImmediate = myClearImmediate;
    timersMod.promises = TP;
    M["timers"] = M["node:timers"] = timersMod;
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
