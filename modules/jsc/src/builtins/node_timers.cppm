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

    // Node's internal Timeout links point at a TimersList sentinel. The host
    // timer object has no such links, but http2's session.socket proxy exposes
    // the handle through internal/timers and Node tests inspect those links.
    // Keep the compatibility shape on the stable host handle; scheduling and
    // lifecycle remain owned by the native timer underneath.
    const INSPECT = Symbol.for("nodejs.util.inspect.custom");
    const timerLists = new WeakMap();
    const inspectIndent = (depth) => {
      const d = typeof depth === "number" && depth >= 0 ? depth : 2;
      return "  ".repeat(Math.max(1, 3 - d));
    };
    function TimersList() {
      this._idleNext = null;
      this._idlePrev = null;
    }
    Object.defineProperty(TimersList.prototype, INSPECT, {
      value: function (depth) {
        const pad = inspectIndent(depth);
        const close = pad.slice(0, Math.max(0, pad.length - 2));
        return "TimersList {\n" + pad + "_idleNext: [Timeout],\n" +
          pad + "_idlePrev: [Timeout]\n" + close + "}";
      },
      configurable: true,
    });
    const installTimerShape = (timer) => {
      let list = timerLists.get(timer);
      if (!list) { list = new TimersList(); timerLists.set(timer, list); }
      list._idleNext = timer;
      list._idlePrev = timer;
      try {
        Object.defineProperty(timer, "_idlePrev", { value: list, writable: true, enumerable: true, configurable: true });
        Object.defineProperty(timer, "_idleNext", { value: list, writable: true, enumerable: true, configurable: true });
        Object.defineProperty(timer, INSPECT, {
          value: function (depth) {
            const pad = inspectIndent(depth);
            const close = pad.slice(0, Math.max(0, pad.length - 2));
            return "Timeout {\n" + pad + "_idlePrev: [TimersList],\n" +
              pad + "_idleNext: [TimersList]\n" + close + "}";
          },
          configurable: true,
        });
      } catch (_) {}
    };
    const removeTimerShape = (timer) => {
      const list = timerLists.get(timer);
      if (list) { list._idleNext = null; list._idlePrev = null; }
      try { timer._idlePrev = null; timer._idleNext = null; } catch (_) {}
    };

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
      if (kind !== "immediate") { setIdle(t, state.ms); installTimerShape(t); }
      // node's Timeout holds its callback on `_onTimeout`, and listOnTimeout
      // DROPS a timer whose `_onTimeout` was nulled instead of running it
      // (lib/internal/timers.js). timers-fixture-unref.js cancels an interval
      // exactly that way. Non-enumerable for the same reason as _idleTimeout.
      if (kind !== "immediate" && typeof state.cb === "function" && t._onTimeout === undefined) {
        try { Object.defineProperty(t, "_onTimeout", { value: state.cb, writable: true, enumerable: false, configurable: true }); }
        catch (_) {}
      }
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
      if (state.refed === undefined) state.refed = !(typeof t.hasRef === "function") || t.hasRef();
      if (typeof oUnref === "function") t.unref = function unref() {
        state.refed = false;
        const r = oUnref.call(state.native);
        return r === undefined ? t : r;
      };
      if (typeof oRef === "function") t.ref = function ref() {
        state.refed = true;
        const r = oRef.call(state.native);
        return r === undefined ? t : r;
      };
      t.hasRef = function hasRef() { return state.refed; };
      if (kind === "timeout") {
        // node Timeout.refresh(): re-arms even from inside (or after) the
        // callback. The native refresh cannot re-arm a fired timer, so always
        // schedule a fresh native timer behind the stable facade object.
        t.refresh = function refresh() {
          // node refresh() ends in insert(this, this._idleTimeout), and insert()
          // returns early for a negative msecs -- so refreshing a timer the
          // caller UNENROLLED (_idleTimeout = -1) does not re-arm it. A timer
          // that already fired or was cleared also reads -1 here, and node DOES
          // re-arm that one, so _destroyed is what separates the two cases.
          if (!t._destroyed && t._idleTimeout < 0) return t;
          state.gen++;
          try { oClearTimeout(state.native); } catch (_) {}
          state.native = __applySchedule(oSetTimeout, [state.run, state.ms],
                                        state.args || []);
          if (state.refed) { try { if (state.native && state.native.ref) state.native.ref(); } catch (_) {} }
          else { try { if (state.native && state.native.unref) state.native.unref(); } catch (_) {} }
          // node Timeout.refresh() re-enrols the handle: _idleStart advances
          // (test-tls-wrap-timeout asserts the later start is strictly greater)
          // and a previously unenrolled timer gets its duration back.
          setIdle(t, state.ms);
          installTimerShape(t);
          t._destroyed = false;
          const id = idOf(t);
          if (id !== null) registry.set(id, t);
          activeTimeouts.add(t);
          return t;
        };
      } else if (kind === "interval") {
        // Same unenrolled-refresh rule for intervals; everything else stays on
        // the native refresh the "refreshed setInterval should not reschedule
        // again" case already exercises.
        const oRefresh = t.refresh;
        if (typeof oRefresh === "function") {
          t.refresh = function refresh() {
            if (!t._destroyed && t._idleTimeout < 0) return t;
            state.rearmed = true;
            state.gen++;
            try { oClearInterval(state.native); } catch (_) {}
            try { oClearTimeout(state.native); } catch (_) {}
            state.native = __applySchedule(oSetInterval, [state.run, state.ms], state.args || []);
            if (state.refed) { try { if (state.native && state.native.ref) state.native.ref(); } catch (_) {} }
            else { try { if (state.native && state.native.unref) state.native.unref(); } catch (_) {} }
            setIdle(t, state.ms);
            installTimerShape(t);
            t._destroyed = false;
            const id = idOf(t);
            if (id !== null) registry.set(id, t);
            activeTimeouts.add(t);
            return t;
          };
        }
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
      if (t[KIND] !== "immediate") removeTimerShape(t);
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

    // Forward to a native scheduler without ever writing `f(a, ...args)`.
    //
    // Spread in a CALL goes through the iterator protocol — it reads
    // `Array.prototype[Symbol.iterator]` at the moment of the call — while a
    // rest PARAMETER does not (it is built with CreateArrayFromList) and
    // `Function.prototype.apply` does not either (CreateListFromArrayLike).
    // node's timers are written against primordials for exactly this reason,
    // so `delete Array.prototype[Symbol.iterator]` cannot break scheduling.
    // mbun's spread version could: the first setTimeout after that deletion
    // threw `Spread syntax requires ...iterable[Symbol.iterator] to be a
    // function` out of readline's line handler, which killed the REPL driver
    // in test-repl-autocomplete / test-repl-history-navigation before they
    // could restore the intrinsic, and the leaked deletion then took down
    // test/common/tmpdir's exit-time cleanup.
    // PORT-SOURCE: node lib/timers.js (ArrayPrototypePush + ReflectApply)
    const __applySchedule = (fn, head, args) => {
      const call = head;
      for (let i = 0; i < args.length; i++) call[call.length] = args[i];
      return fn.apply(undefined, call);
    };

    const mySetTimeout = function setTimeout(cb, ms, ...args) {
      if (typeof cb !== "function") throw __invalidCb(cb);
      _checkCountdown(ms);
      cb = __sched(cb);
      const state = { gen: 0, ms, args, cb };
      state.run = function (...a) {
        const g = state.gen;
        if (state.timer && state.timer._onTimeout === null) { destroyTimer(state.timer); return; }
        try { return __runTimerCallback(state, cb, state.timer, a); }
        // refresh()/clear during the callback bumps gen: skip the destroy.
        finally { if (state.gen === g && state.timer) destroyTimer(state.timer); }
      };
      const t = __applySchedule(oSetTimeout, [state.run, ms], args);
      state.timer = t;
      state.native = t;
      return initTimer(t, "timeout", state);
    };
    const mySetInterval = function setInterval(cb, ms, ...args) {
      if (typeof cb !== "function") throw __invalidCb(cb);
      _checkCountdown(ms);
      cb = __sched(cb);
      const state = { gen: 0, ms, args, cb };
      state.run = function (...a) {
        const t = state.timer;
        // node listOnTimeout drops a timer whose _onTimeout was nulled without
        // running it, and only re-inserts a repeating timer while its
        // _idleTimeout is still enrolled -- unenrolling it to -1 from inside the
        // callback stops the interval. ref: node lib/internal/timers.js.
        if (t && t._onTimeout === null) { clearNative(t); destroyTimer(t); return; }
        // A refresh() from inside the callback re-inserts the timer itself, so
        // node's "don't re-insert an unenrolled repeater" branch cannot reach
        // it -- it fires once more. `rearmed` reproduces that.
        state.rearmed = false;
        try { return __runTimerCallback(state, cb, t, a); }
        finally { if (t && !t._destroyed && !state.rearmed && t._idleTimeout < 0) { clearNative(t); destroyTimer(t); } }
      };
      // Scheduled through __applySchedule, NOT `oSetInterval(state.run, ms,
      // ...args)`. The spread form reads Array.prototype[Symbol.iterator], and
      // test-repl-autocomplete / test-repl-history-navigation delete that
      // intrinsic on purpose -- call-spread here made every setInterval throw
      // for the rest of the process. Same call, no iterator dependency.
      const t = __applySchedule(oSetInterval, [state.run, ms], args);
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
      const t = __applySchedule(oSetImmediate, [state.run], args);
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
    // node lib/internal/validators.js, as used by lib/timers/promises.js.
    // Message shapes come from ERR_INVALID_ARG_TYPE; __recv above already
    // renders node's "Received ..." tail.
    // node internal/errors.js: a name containing '.' names a PROPERTY, and
    // makeNodeErrorWithCode gives every NodeError a toString() of
    // `${name} [${code}]: ${message}` — assert.rejects(p, /ERR_INVALID_ARG_TYPE/)
    // matches String(err), so without it the code is invisible to the matcher.
    const tpErr = (name, expected, v) => {
      const noun = name.indexOf(".") >= 0 ? "property" : "argument";
      const e = new TypeError('The "' + name + '" ' + noun + " must be " + expected + ". Received " + __recv(v));
      e.code = "ERR_INVALID_ARG_TYPE";
      Object.defineProperty(e, "toString", {
        value() { return "TypeError [ERR_INVALID_ARG_TYPE]: " + this.message; },
        configurable: true, writable: true,
      });
      return e;
    };
    const vNumber = (v, name) => { if (typeof v !== "number") throw tpErr(name, "of type number", v); };
    const vBoolean = (v, name) => { if (typeof v !== "boolean") throw tpErr(name, "of type boolean", v); };
    const vObject = (v, name) => {
      if (v === null || Array.isArray(v) || typeof v !== "object") throw tpErr(name, "of type object", v);
    };
    const vAbortSignal = (v, name) => {
      if (v !== undefined && (v === null || typeof v !== "object" || !("aborted" in v)))
        throw tpErr(name, "an instance of AbortSignal", v);
    };
    // node: `options = kEmptyObject` default; validation runs before any use.
    const tpValidate = (options) => {
      vObject(options, "options");
      if (typeof options.signal !== "undefined") vAbortSignal(options.signal, "options.signal");
      if (typeof options.ref !== "undefined") vBoolean(options.ref, "options.ref");
    };
    const kEmptyObj = Object.freeze({});

    function tpSetTimeout(after, value, options = kEmptyObj) {
      try {
        if (typeof after !== "undefined") vNumber(after, "delay");
        tpValidate(options);
      } catch (err) { return Promise.reject(err); }
      const signal = options.signal;
      const ref = options.ref === undefined ? true : options.ref;
      if (signal && signal.aborted) return Promise.reject(abortError(signal));
      let resolve, reject;
      const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
      const t = mySetTimeout(() => resolve(value), after);
      if (ref === false && t && typeof t.unref === "function") t.unref();
      if (!signal) return promise;
      // node cancelListenerHandler: a timer that already fired is not cancelled.
      const oncancel = () => {
        if (!t._destroyed) { myClearTimeout(t); reject(abortError(signal)); }
      };
      try { signal.addEventListener("abort", oncancel); } catch (_) {}
      // node SafePromisePrototypeFinally: drop the listener however we settle.
      return promise.finally(() => {
        try { signal.removeEventListener("abort", oncancel); } catch (_) {}
      });
    }
    function tpSetImmediate(value, options = kEmptyObj) {
      try { tpValidate(options); } catch (err) { return Promise.reject(err); }
      const signal = options.signal;
      const ref = options.ref === undefined ? true : options.ref;
      if (signal && signal.aborted) return Promise.reject(abortError(signal));
      let resolve, reject;
      const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
      const t = mySetImmediate(() => resolve(value));
      if (ref === false && t && typeof t.unref === "function") t.unref();
      if (!signal) return promise;
      const oncancel = () => {
        if (!t._destroyed) { myClearImmediate(t); reject(abortError(signal)); }
      };
      try { signal.addEventListener("abort", oncancel); } catch (_) {}
      return promise.finally(() => {
        try { signal.removeEventListener("abort", oncancel); } catch (_) {}
      });
    }
    // node lib/timers/promises.js setInterval is an `async function*`: calling
    // it returns an async GENERATOR, so the returned value is itself the
    // iterator (`.next()`/`.return()` live on it) as well as the iterable, and
    // argument validation is deferred to the first next() — both of which
    // test-timers-interval-promisified.js pins. A single repeating interval
    // backs it, and ticks that arrive while the consumer is awaiting are
    // counted (`notYielded`) and replayed rather than dropped.
    async function* tpSetInterval(after, value, options = kEmptyObj) {
      if (typeof after !== "undefined") vNumber(after, "delay");
      tpValidate(options);
      const signal = options.signal;
      const ref = options.ref === undefined ? true : options.ref;
      if (signal && signal.aborted) throw abortError(signal);
      let onCancel;
      let interval;
      try {
        let notYielded = 0;
        let callback;
        interval = mySetInterval(() => {
          notYielded++;
          if (callback) { callback(); callback = undefined; }
        }, after);
        if (ref === false && interval && typeof interval.unref === "function") interval.unref();
        if (signal) {
          onCancel = () => {
            myClearInterval(interval);
            if (callback) {
              callback(Promise.reject(abortError(signal)));
              callback = undefined;
            }
          };
          try { signal.addEventListener("abort", onCancel, { once: true }); } catch (_) {}
        }
        while (!(signal && signal.aborted)) {
          if (notYielded === 0) await new Promise((resolve) => { callback = resolve; });
          for (; notYielded > 0; notYielded--) yield value;
        }
        throw abortError(signal);
      } finally {
        myClearInterval(interval);
        if (signal && onCancel) { try { signal.removeEventListener("abort", onCancel); } catch (_) {} }
      }
    }
    // node's exports carry the plain names.
    try {
      Object.defineProperty(tpSetTimeout, "name", { value: "setTimeout", configurable: true });
      Object.defineProperty(tpSetImmediate, "name", { value: "setImmediate", configurable: true });
      Object.defineProperty(tpSetInterval, "name", { value: "setInterval", configurable: true });
    } catch (_) {}
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
      // node lib/timers.js hangs the timers/promises functions THEMSELVES off
      // the promisify.custom slot, so promisify(timers.setTimeout) === the
      // timers/promises export (test-timers-timeout-promisified.js asserts
      // strict equality). A forwarding arrow breaks that identity.
      Object.defineProperty(mySetTimeout, custom, { value: tpSetTimeout, configurable: true });
      Object.defineProperty(mySetImmediate, custom, { value: tpSetImmediate, configurable: true });
      Object.defineProperty(mySetInterval, custom, { value: tpSetInterval, configurable: true });
    } catch (_) {}

    // Install wrappers globally and re-register node:timers in place.
    G.setTimeout = mySetTimeout;
    G.setInterval = mySetInterval;
    G.setImmediate = mySetImmediate;
    // An internal "run this on the next loop turn" with no node-visible
    // resource attached: the raw host immediate, so it registers no Immediate
    // in activeImmediates, no async-hook id, and no Timeout facade. node:fs
    // uses it to drain its completion queue — an fs completion is an
    // FSREQCALLBACK request, not an Immediate, and getActiveResourcesInfo()
    // must not report the drain alongside the requests it is draining. Throws
    // land in the pump's own uncaught channel, the same place __runTimerCallback
    // sends them.
    G.__mbunSystemImmediate = function (fn) { return oSetImmediate(fn); };
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
          // The other half of node's answer: libuv HANDLES (live sockets and
          // servers). The registry lives on the global because its producer is
          // node:net; see builtins/node_process_extra.cppm.
          const HT = G.__mbunHandleTrack;
          if (HT) for (const t of HT.types()) out.push(t);
          // …and libuv REQUESTS. node:fs registers one per async operation
          // whose completion has not been delivered yet; node reports those as
          // FSREQCALLBACK (test-process-getactiveresources-track-active-requests
          // fires 12 fs.open and asserts the count synchronously).
          const FSR = G.__mbunFsActiveRequests;
          if (FSR) for (let i = 0; i < FSR.size; i++) out.push("FSREQCALLBACK");
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
