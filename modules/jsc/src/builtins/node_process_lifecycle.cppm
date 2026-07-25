// Process lifecycle partition: 'exit' / 'beforeExit' emission, the uncaught
// exception path, and the internal-globals hiding step.
//
// Everything here exists because mbun had no shutdown sequence at all: the
// event-loop pump drained, `run_script` returned, and the process died without
// ever emitting `'exit'`. node's own test harness registers its mustCall
// verifier (`runCallChecks`) inside `process.on('exit')` — with no 'exit' event
// the central assertion of ~62% of the node corpus was never evaluated, and a
// file that under-called a `common.mustCall` still exited 0.
//
// The other half of the same hole: an exception escaping a timer / socket / I/O
// callback was swallowed by a bare `catch (e) {}` at each pump site. node routes
// it to `'uncaughtException'` and, unclaimed, dies with status 1. Swallowing it
// left the throwing test's handles registered, so the loop never drained and the
// file hung to the corpus timeout instead of failing.
//
// Blueprint: node lib/internal/process/execution.js (uncaught exception
// escalation order: uncaughtExceptionMonitor -> capture callback ->
// 'uncaughtException'), lib/internal/process/per_thread.js (process.exit /
// process.exitCode) and src/node.cc EmitProcessBeforeExit/EmitProcessExit.
//
// NOTE: appended AFTER the master builtins IIFE; self-contained IIFE, must
// never throw at top level (a throw would silently disable later partitions).
export module mbun.jsc.js_builtins:node_process_lifecycle;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeProcessLifecycleJS = R"JS(
(function () {
  "use strict";
  const G = globalThis;
  const p = G.process;
  if (!p || typeof p.emit !== "function" || typeof p.listenerCount !== "function") return;

  const intOr = (v, d) => (typeof v === "number" && Number.isInteger(v) ? v : d);
  const listeners = (name) => { try { return p.listenerCount(name); } catch (e) { return 0; } };

  // JSC's Error.stack carries only the frames, never the leading
  // "Name: message" line, so a report built from .stack alone loses the actual
  // error. Rebuild node's two-part shape explicitly.
  const describe = (err) => {
    try {
      if (err instanceof Error) {
        const head = (err.name || "Error") + (err.message ? ": " + err.message : "");
        const stack = err.stack ? String(err.stack) : "";
        if (!stack) return head;
        return stack.indexOf(head) === 0 ? stack : head + "\n" + stack;
      }
      if (typeof err === "symbol") return "Uncaught " + err.toString();
      return "Uncaught " + String(err);
    } catch (e) { return "Uncaught exception"; }
  };

  // ---- fatal (uncaught) exception channel ---------------------------------
  // A one-element array rather than the error itself so a thrown `undefined`
  // stays distinguishable from "nothing is pending". The C++ pump polls
  // __mbun_fatal_pending() after every phase and unwinds into the exit
  // sequence, which is how an unclaimed throw becomes status 1 instead of a
  // silently-dropped callback.
  G.__mbun_fatal = null;

  // node lib/internal/process/execution.js: an exception escaping a libuv
  // callback goes to 'uncaughtExceptionMonitor', then the capture callback,
  // then 'uncaughtException'. Returns true when something claimed it (the loop
  // keeps running); false arms the fatal channel.
  G.__mbun_uncaught = function (err) {
    if (G.__mbun_fatal) return false;
    try {
      if (listeners("uncaughtExceptionMonitor") > 0) {
        try { p.emit("uncaughtExceptionMonitor", err, "uncaughtException"); } catch (e) {}
      }
      // Once 'exit' is being emitted node no longer offers the exception to
      // handlers — the process is already leaving.
      if (!p._exiting) {
        const capture = p._mbunUncaughtCaptureCallback;
        if (typeof capture === "function") { capture(err); return true; }
        if (listeners("uncaughtException") > 0) {
          p.emit("uncaughtException", err, "uncaughtException");
          return true;
        }
      }
    } catch (nested) { G.__mbun_fatal = [nested]; return false; }
    G.__mbun_fatal = [err];
    return false;
  };

  // Microtask callbacks are a callback boundary too. JSC surfaces a throw from
  // one through its promise-rejection tracker, so it arrived as an "unhandled
  // rejection" and sailed past every process.on('uncaughtException') listener
  // the caller installed. node treats it as an uncaught exception — and so does
  // everything mbun routes through queueMicrotask, including process.nextTick
  // and every deferred emit in the net/dgram layers.
  const rawQueueMicrotask = G.queueMicrotask;
  if (typeof rawQueueMicrotask === "function") {
    G.queueMicrotask = function queueMicrotask(fn) {
      if (typeof fn !== "function") return rawQueueMicrotask(fn);  // native raises node's error
      // Same scheduling seam node_timers installs on setTimeout & friends: a
      // per-call slot, so node:domain can re-enter the scheduling domain
      // WITHOUT replacing globalThis.queueMicrotask (whose identity node's
      // test/common leak check pins at load time). process.nextTick rides on
      // this function, so hooking here covers nextTick too — and covers it
      // exactly once, which is what the removed double-wrap guard was for.
      const h = G.__mbunSchedHook;
      if (h !== undefined && h !== null) fn = h(fn);
      rawQueueMicrotask(function () { try { fn(); } catch (e) { G.__mbun_uncaught(e); } });
    };
  }

  G.__mbun_fatal_pending = function () { return G.__mbun_fatal ? 1 : 0; };
  G.__mbun_fatal_message = function () { return G.__mbun_fatal ? describe(G.__mbun_fatal[0]) : ""; };

  // ---- 'beforeExit' -------------------------------------------------------
  // node src/node.cc EmitProcessBeforeExit: fired when the loop has drained
  // with no pending work; NOT fired after an explicit process.exit() or a fatal
  // error. A listener may schedule more work, in which case the loop runs again
  // and beforeExit fires again — the caller re-pumps while __mbun_loop_alive().
  // Returns 1 when listeners actually ran (0 tells the caller to stop looping).
  G.__mbun_before_exit = function (code) {
    if (p._exiting || G.__mbun_fatal) return 0;
    if (listeners("beforeExit") === 0) return 0;
    try { p.emit("beforeExit", intOr(p.exitCode, intOr(code, 0))); }
    catch (e) { G.__mbun_uncaught(e); }
    return 1;
  };

  // Does the event loop still hold work? Mirrors the pump's own liveness terms
  // (ref'd timers, in-flight net ops, ref'd handles, live children) so a
  // beforeExit listener that re-arms the loop is observed the same way the pump
  // observes it.
  G.__mbun_loop_alive = function () {
    let n = 0;
    try { n += G.__mbun_timers_refd ? G.__mbun_timers_refd() : 0; } catch (e) {}
    try {
      const NET = G.__mbunNet;
      if (NET) n += (NET.pending | 0) + (NET.handles | 0) + (NET.serveActive > 0 ? 1 : 0);
    } catch (e) {}
    try { if (G.__mbunChildren && G.__mbunChildren.size) n += G.__mbunChildren.size; } catch (e) {}
    return n > 0 ? 1 : 0;
  };

  // ---- 'exit' -------------------------------------------------------------
  p._exiting = false;
  // Runs node's shutdown sequence and returns the process's final status.
  // Listeners run synchronously and exactly once; anything they schedule
  // (timers, microtasks) never runs, because the caller exits the moment this
  // returns. `process.exitCode` assigned inside a listener wins over the code
  // the listeners were handed — node per_thread.js reads the slot back after
  // the emit. Never throws: a throwing listener is fatal (report + status 1)
  // and the remaining listeners are skipped, as in node.
  G.__mbun_run_exit = function (code) {
    // node per_thread.js process.exit, step for step: an explicit code settles
    // the slot FIRST — including on a re-entrant call from inside an 'exit'
    // listener, which is exactly how node's own harness reports a failure
    // (runCallChecks ends in `process.exit(1)` from its 'exit' handler). Doing
    // the assignment only on the first pass silently discarded that 1, so a
    // detected mustCall mismatch still left with status 0.
    if (code !== undefined && code !== null) { try { p.exitCode = code; } catch (e) {} }
    let final = intOr(p.exitCode, 0);
    if (!p._exiting) {
      p._exiting = true;
      try {
        p.emit("exit", final);
        final = intOr(p.exitCode, final);
      } catch (e) {
        try { p.stderr.write("error: " + describe(e) + "\n"); } catch (e2) {}
        final = 1;
      }
    }
    // Leave from here, exactly as node's per_thread.js does
    // (`process.reallyExit(process.exitCode)`): returning instead would hand
    // control back through an engine call boundary, and JSC drains its
    // microtask queue on every such boundary — so a promise continuation
    // queued by an 'exit' listener would still get a turn, which it must not.
    try { if (typeof p.reallyExit === "function") p.reallyExit(final); } catch (e) {}
    return final;
  };

  // ---- internal globals must not be enumerable ----------------------------
  // node exposes exactly this set as ENUMERABLE own properties of globalThis;
  // everything else it installs (process, Buffer, URL, the web classes, its own
  // bindings) is defined non-enumerable. node's test/common/index.js walks
  // `for (const val in globalThis)` on 'exit' and fails the file for anything
  // it does not recognise — with 'exit' now firing, mbun's 148 enumerable
  // internals (__mbunNetNative, Bun, require, module, …) would fail every
  // corpus file that requires common. Hiding them is the honest fix: it makes
  // globalThis enumerate what node's does, rather than weakening the check.
  const ENUMERABLE_GLOBALS = new Set([
    "global", "queueMicrotask", "structuredClone", "atob", "btoa", "performance",
    "fetch", "crypto", "navigator", "gc",
    "setTimeout", "clearTimeout", "setInterval", "clearInterval",
    "setImmediate", "clearImmediate",
  ]);
  G.__mbun_hide_internal_globals = function () {
    let names;
    try { names = Object.getOwnPropertyNames(G); } catch (e) { return 0; }
    let hidden = 0;
    for (const name of names) {
      if (ENUMERABLE_GLOBALS.has(name)) continue;
      try {
        const desc = Object.getOwnPropertyDescriptor(G, name);
        if (!desc || !desc.enumerable || !desc.configurable) continue;
        desc.enumerable = false;
        Object.defineProperty(G, name, desc);
        hidden++;
      } catch (e) {}
    }
    return hidden;
  };
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
