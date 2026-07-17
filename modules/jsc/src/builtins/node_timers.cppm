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

    const KIND = Symbol("mbun.timerKind");
    const STATE = Symbol("mbun.timerState");
    const registry = new Map(); // numeric id -> timer object (timeouts/intervals)

    const idOf = (t) => { try { const n = +t; return Number.isSafeInteger(n) ? n : null; } catch (_) { return null; } };

    function initTimer(t, kind, state) {
      if (t === null || typeof t !== "object") return t;
      t[KIND] = kind;
      t[STATE] = state;
      if (t._destroyed === undefined) {
        try { Object.defineProperty(t, "_destroyed", { value: false, writable: true, enumerable: false, configurable: true }); }
        catch (_) { t._destroyed = false; }
      } else t._destroyed = false;
      if (kind !== "immediate") {
        const id = idOf(t);
        if (id !== null) registry.set(id, t);
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
          t._destroyed = false;
          const id = idOf(t);
          if (id !== null) registry.set(id, t);
          return t;
        };
      }
      return t;
    }

    function destroyTimer(t) {
      if (t === null || typeof t !== "object") return;
      t._destroyed = true;
      const id = idOf(t);
      if (id !== null) registry.delete(id);
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

    const mySetTimeout = function setTimeout(cb, ms, ...args) {
      if (typeof cb !== "function") return oSetTimeout(cb, ms, ...args); // preserve native error behavior
      _checkCountdown(ms);
      const state = { gen: 0, ms, args };
      state.run = function (...a) {
        const g = state.gen;
        try { return cb.apply(this, a); }
        // refresh()/clear during the callback bumps gen: skip the destroy.
        finally { if (state.gen === g && state.timer) destroyTimer(state.timer); }
      };
      const t = oSetTimeout(state.run, ms, ...args);
      state.timer = t;
      state.native = t;
      return initTimer(t, "timeout", state);
    };
    const mySetInterval = function setInterval(cb, ms, ...args) {
      if (typeof cb !== "function") return oSetInterval(cb, ms, ...args);
      const state = { gen: 0, ms, args };
      const t = oSetInterval(cb, ms, ...args);
      state.timer = t;
      state.native = t;
      return initTimer(t, "interval", state);
    };
    const mySetImmediate = function setImmediate(cb, ...args) {
      if (typeof cb !== "function") return oSetImmediate(cb, ...args);
      const state = { gen: 0 };
      state.run = function (...a) {
        const g = state.gen;
        try { return cb.apply(this, a); }
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
    const scheduler = {
      wait: (delay, options) => tpSetTimeout(delay, undefined, options),
      yield: () => tpSetImmediate(),
    };

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
