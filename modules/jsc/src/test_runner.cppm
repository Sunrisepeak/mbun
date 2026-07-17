// src/test_runner.cppm — module mbun.jsc.test_runner
//
// The `bun:test` runner (T3.4, ⭐ M3 keystone / S1-unlock gate): assembles the
// existing JSC runtime pieces into a runner that executes a bun-native test file
// and reports the same pass/fail tally as bun.
//
//     run_file(path)
//        → mbun.jsc.module_loader.load   (resolve + read + TS→JS transpile)
//        → prepare_source                (rewrite `import … from "bun:test"` to a
//                                          script-mode `const {…} = globalThis.__mbunBT`,
//                                          strip other ESM import/export decls)
//        → run_source
//             → eval(harness prelude)     (bun:test test/it/describe/expect/hooks in JS)
//             → eval(prepared test src)   (collection: register describe/test/hooks)
//             → eval("__mbun_run()")      (execution: beforeAll → (beforeEach,test,
//                                          afterEach)* → afterAll, nested by scope;
//                                          async tests complete through JSC's
//                                          end-of-script microtask drain)
//             → read pass/fail/skip/expect + per-test body back out
//
// Runs in the SHARED runtime context (mbun.jsc.runtime): the same global object
// that carries Bun.*/node bindings and the CommonJS require() module system, so a
// test file can require() local helpers and use Bun.*/process/console — matching
// bun, where test files have the full runtime. This upper layer talks to JSC only
// through mbun.jsc.runtime's eval/eval_number/eval_to_string; never a JSC header.
//
// Re-expressed in MC++ from bun's test runner (behaviour as blueprint, not a
// line port; the framework itself is authored in JS as the harness prelude):
//   - .mbun/bun-zig-src/src/test_runner/Collection.zig — collection phase
//     discovers describe/test calls into a scope tree.
//   - .mbun/bun-zig-src/src/test_runner/Execution.zig — execution runs
//     beforeAll, then (beforeEach, test, afterEach) sequences per scope, then
//     afterAll; nested describe scopes recurse.
//   - .mbun/bun-zig-src/src/test_runner/ScopeFunctions.zig — test/it/describe
//     modes (.skip/.todo/.only) surface.
//   - .mbun/bun-zig-src/src/test_runner/expect.zig — matcher surface (toBe,
//     toEqual, toThrow, …) reimplemented as the minimal JS subset below.
//
// Scope / DEFERRED (honestly reported, not faked green — core principle ①):
//   - Matchers: toBe/toEqual/toStrictEqual/toBeTruthy/toBeFalsy/toBeDefined/
//     toBeUndefined/toBeNull/toBeNaN/toBeGreaterThan[OrEqual]/toBeLessThan[OrEqual]/
//     toBeInstanceOf/toBeTypeOf/toContain/toHaveLength/toMatch/toThrow/toThrowError
//     + `.not` + expect.unreachable. Any other matcher is undefined → the test
//     fails with "X is not a function" (honest signal that it is unimplemented).
//     Snapshots, mock(), asymmetric matchers, .resolves/.rejects: DEFERRED.
//   - Async: Promise/async-await tests complete via JSC's end-of-script
//     microtask drain. setTimeout-driven async (no host timer bound to the bare
//     C-API context) does NOT complete → reported as an incomplete run. Binding
//     the native event loop's timers/queueMicrotask onto JSC needs C++
//     PrivateHeaders (design §3.3 milestone 3): DEFERRED(S1).
//   - `done` callback style, test.each, .only filtering, per-file isolation
//     across many files, real console capture: DEFERRED(S1 → T4.2).
export module mbun.jsc.test_runner;

import std;
import mbun.jsc.runtime;
import mbun.jsc.module_loader;
import mbun.js_parser;
import mbun.resolver;
import mbun.core.paths;

// ── internal helpers (not exported) ─────────────────────────────────────────
namespace mbun::jsc::test_runner::detail {

// The bun:test framework, authored in JS (installed once per run). It defines
// globalThis.__mbunBT (the module's named exports), collects describe/test/hooks
// into a scope tree, and exposes __mbun_run() which executes them and writes
// summary globals the native side reads back. Evaluating it is idempotent: the
// first eval installs the singleton runner, every later eval just resets the
// collection/execution state in place (see the __mbun_reset guard at the top).
inline constexpr std::string_view HARNESS = R"JS(
;(function () {
  "use strict";
  const G = globalThis;

  // The per-test timeout must fire on REAL wall-clock, immune to userland timer
  // mocks (sinon/jest useFakeTimers replace G.setTimeout, so a test that ticks
  // the fake clock past the timeout would falsely time out — bun's per-test
  // timeout is native and unfakeable). Capture the pristine timers once, before
  // any test file's beforeEach can install a mock.
  const natSetTimeout = G.__mbunNatSetTimeout || (G.__mbunNatSetTimeout = G.setTimeout);
  const natClearTimeout = G.__mbunNatClearTimeout || (G.__mbunNatClearTimeout = G.clearTimeout);

  // bun keeps ONE TestRunner alive for the whole process (jest.rs:112 — a single
  // `current_file` the test-registration path resolves at CALL time), so `it` /
  // `describe` are stable bindings and the per-file state lives *inside* the
  // runner. Mirror that: install once, then reset collection state IN PLACE
  // between files (__mbun_reset below).
  //
  // Re-evaluating this IIFE per file instead would mint a fresh `S` plus fresh
  // `it`/`describe`/`expect` closures over it, while long-lived references keep
  // pointing at the PREVIOUS file's now-abandoned `S`, silently dropping every
  // test they register (no error, no fail — the tests just vanish):
  //   • the globalThis mirror below is `typeof`-guarded, so bare-global `it()`
  //     stays bound to file 1's closure forever; and
  //   • a helper module that did `import { it } from "bun:test"` at module scope
  //     destructured `__mbunBT` once and is then held by the CJS module cache
  //     (engine.inc moduleCache_), so its body never re-runs.
  if (G.__mbun_reset) { G.__mbun_reset(); return; }

  function makeScope(name, parent) {
    return { name: name, parent: parent, items: [],
             beforeAll: [], afterAll: [], beforeEach: [], afterEach: [] };
  }
  const root = makeScope(null, null);
  const S = { root: root, current: root,
              pass: 0, fail: 0, skip: 0, expectCalls: 0, total: 0, out: [], customMatchers: {},
              timeoutReject: null, errors: [], asyncErr: undefined, todo: 0, pendingAsserts: [],
              sysTime: null, skippedLabel: 0 };
  G.__mbunState = S;
  // Attribute errors thrown from queueMicrotask/process.nextTick callbacks to the
  // currently-running test (bun: an async exception while a test is in flight
  // fails that test). Wrapped ONCE per process — the wrappers read the live state
  // through G.__mbunState so a re-evaluated harness (fresh S) still works.
  if (!G.__mbunAsyncWrapped) {
    G.__mbunAsyncWrapped = true;
    const wrapCb = (cb) => function () {
      try { return cb.apply(this, arguments); }
      catch (e) { const st = G.__mbunState; if (st && st.asyncErr === undefined) st.asyncErr = e; else throw e; }
    };
    if (typeof G.queueMicrotask === "function") {
      const qm = G.queueMicrotask.bind(G);
      G.queueMicrotask = function (cb) { if (typeof cb !== "function") throw new TypeError("queueMicrotask requires a function argument"); return qm(wrapCb(cb)); };
    }
    if (G.process && typeof G.process.nextTick === "function") {
      const nt = G.process.nextTick.bind(G.process);
      G.process.nextTick = function (cb, ...a) { return nt(wrapCb(cb), ...a); };
    }
  }
  // The C++ pump calls this when a run is stuck (no pending timers, no progress):
  // reject the currently-running test's timeout race so it fails individually and
  // __mbun_run advances to the next test. Returns true if a test was timed out.
  G.__mbun_timeout_current = function () {
    const r = S.timeoutReject;
    if (r) { S.timeoutReject = null; r(new Error("test timed out (no pending timers / unresolved async)")); return true; }
    return false;
  };

  // jest-ish serialization for inline snapshots (indented, keys sorted).
  function snapSerialize(v, indent) {
    indent = indent || "";
    const ni = indent + "  ";
    if (v === null) return "null";
    if (v === undefined) return "undefined";
    if (typeof v === "string") return "\"" + v + "\"";
    if (typeof v === "number" || typeof v === "boolean" || typeof v === "bigint") return String(v);
    if (v instanceof Date) return v.toISOString();   // bun pretty-format: unquoted ISO string
    if (v instanceof RegExp) return String(v);
    if (Array.isArray(v)) { if (v.length === 0) return "[]"; return "[\n" + v.map((x) => ni + snapSerialize(x, ni) + ",").join("\n") + "\n" + indent + "]"; }
    if (typeof v === "object") { const ks = Object.keys(v).sort(); if (ks.length === 0) return "{}"; return "{\n" + ks.map((k) => ni + "\"" + k + "\": " + snapSerialize(v[k], ni) + ",").join("\n") + "\n" + indent + "}"; }
    return String(v);
  }

  // Cycle-safe: matcher messages are built eagerly (even on pass), so fmt must
  // never recurse forever on circular structures.
  function fmt(v, seen) {
    if (v === null) return "null";
    if (v === undefined) return "undefined";
    const t = typeof v;
    if (t === "string") return JSON.stringify(v);
    if (t === "number" || t === "boolean" || t === "bigint") return String(v);
    if (t === "symbol") return v.toString();
    if (t === "function") return "[Function: " + (v.name || "anonymous") + "]";
    seen = seen || new Set();
    if (seen.has(v)) return "[Circular]";
    seen.add(v);
    try {
      if (Array.isArray(v)) return "[" + v.map((x) => fmt(x, seen)).join(", ") + "]";
      const ks = Object.keys(v);
      return "{" + ks.map((k) => JSON.stringify(k) + ": " + fmt(v[k], seen)).join(", ") + "}";
    } catch (e) { return String(v); }
    finally { seen.delete(v); }
  }
  // bun/jest toEqual semantics: undefined-valued own keys are ignored (strictKeys
  //=false); toStrictEqual keeps them and compares prototypes. Cycle-safe via a
  // visited-pair map. Date/RegExp/Error/Map/Set/TypedArray/ArrayBuffer aware.
  function deepEqualImpl(a, b, strictKeys, seen) {
    // asymmetric matcher on either side (expect.any / objectContaining / …)
    if (isAsym(b)) return b.match(a);
    if (isAsym(a)) return a.match(b);
    if (Object.is(a, b)) return true;
    if (typeof a !== "object" || typeof b !== "object" || a === null || b === null) return false;
    if (a instanceof Date || b instanceof Date) return a instanceof Date && b instanceof Date && a.getTime() === b.getTime();
    if (a instanceof RegExp || b instanceof RegExp) return a instanceof RegExp && b instanceof RegExp && a.source === b.source && a.flags === b.flags;
    if (a instanceof Error || b instanceof Error) { if (!(a instanceof Error && b instanceof Error)) return false; if (a.message !== b.message || a.name !== b.name) return false; }
    if (Array.isArray(a) !== Array.isArray(b)) return false;
    // circular: if this exact pair is already being compared, treat as equal
    let av = seen.get(a); if (av && av.has(b)) return true;
    if (!av) { av = new Set(); seen.set(a, av); } av.add(b);
    try {
      if (a instanceof Map || b instanceof Map) {
        if (!(a instanceof Map && b instanceof Map) || a.size !== b.size) return false;
        for (const [k, v] of a) {
          if (b.has(k)) { if (!deepEqualImpl(v, b.get(k), strictKeys, seen)) return false; continue; }
          let found = false;
          for (const [bk2, bv2] of b) if (deepEqualImpl(k, bk2, strictKeys, seen) && deepEqualImpl(v, bv2, strictKeys, seen)) { found = true; break; }
          if (!found) return false;
        }
        return true;
      }
      if (a instanceof Set || b instanceof Set) {
        if (!(a instanceof Set && b instanceof Set) || a.size !== b.size) return false;
        for (const v of a) {
          if (b.has(v)) continue;
          let found = false;
          for (const bv2 of b) if (deepEqualImpl(v, bv2, strictKeys, seen)) { found = true; break; }
          if (!found) return false;
        }
        return true;
      }
      if (ArrayBuffer.isView(a) || ArrayBuffer.isView(b)) {
        if (!(ArrayBuffer.isView(a) && ArrayBuffer.isView(b))) return false;
        const ua = new Uint8Array(a.buffer, a.byteOffset, a.byteLength), ub = new Uint8Array(b.buffer, b.byteOffset, b.byteLength);
        if (ua.length !== ub.length) return false;
        for (let i = 0; i < ua.length; i++) if (ua[i] !== ub[i]) return false;
        return true;
      }
      if (a instanceof ArrayBuffer || b instanceof ArrayBuffer) {
        if (!(a instanceof ArrayBuffer && b instanceof ArrayBuffer) || a.byteLength !== b.byteLength) return false;
        const ua = new Uint8Array(a), ub = new Uint8Array(b);
        for (let i = 0; i < ua.length; i++) if (ua[i] !== ub[i]) return false;
        return true;
      }
      if (strictKeys && Object.getPrototypeOf(a) !== Object.getPrototypeOf(b)) return false;
      const keep = (o) => (k) => strictKeys || o[k] !== undefined;
      const ak = Object.keys(a).filter(keep(a)), bk = Object.keys(b).filter(keep(b));
      if (ak.length !== bk.length) return false;
      for (const k of ak) {
        if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
        if (!deepEqualImpl(a[k], b[k], strictKeys, seen)) return false;
      }
      return true;
    } finally { av.delete(b); }
  }
  function deepEqual(a, b) { return deepEqualImpl(a, b, false, new Map()); }
  function deepEqualStrict(a, b) { return deepEqualImpl(a, b, true, new Map()); }
  function assertionError(msg) { const e = new Error(msg); e.name = "AssertionError"; return e; }

  function makeMatchers(received, isNot) {
    // The message is a thunk: bun/jest only format on failure. Building it eagerly
    // walked the whole received value on every passing assertion — a 40MB body
    // (Bun.file of a video) took minutes.
    function check(pass, message) {
      if (isNot) pass = !pass;
      if (!pass) throw assertionError(typeof message === "function" ? message() : message);
    }
    const m = {
      toBe(x) { check(Object.is(received, x), () => "expect(received).toBe(expected)\n\nExpected: " + fmt(x) + "\nReceived: " + fmt(received)); return m; },
      toEqual(x) { check(deepEqual(received, x), () => "expect(received).toEqual(expected)\n\nExpected: " + fmt(x) + "\nReceived: " + fmt(received)); return m; },
      toStrictEqual(x) { check(deepEqualStrict(received, x), () => "expect(received).toStrictEqual(expected)\n\nExpected: " + fmt(x) + "\nReceived: " + fmt(received)); return m; },
      toBeTruthy() { check(!!received, () => "expect(received).toBeTruthy()\n\nReceived: " + fmt(received)); return m; },
      toBeFalsy() { check(!received, () => "expect(received).toBeFalsy()\n\nReceived: " + fmt(received)); return m; },
      toBeDefined() { check(received !== undefined, () => "expect(received).toBeDefined()\n\nReceived: " + fmt(received)); return m; },
      toBeUndefined() { check(received === undefined, () => "expect(received).toBeUndefined()\n\nReceived: " + fmt(received)); return m; },
      toBeNull() { check(received === null, () => "expect(received).toBeNull()\n\nReceived: " + fmt(received)); return m; },
      toBeNaN() { check(typeof received === "number" && Number.isNaN(received), () => "expect(received).toBeNaN()\n\nReceived: " + fmt(received)); return m; },
      toBeGreaterThan(n) { check(received > n, () => "expect(received).toBeGreaterThan(" + fmt(n) + ")\n\nReceived: " + fmt(received)); return m; },
      toBeGreaterThanOrEqual(n) { check(received >= n, () => "expect(received).toBeGreaterThanOrEqual(" + fmt(n) + ")\n\nReceived: " + fmt(received)); return m; },
      toBeLessThan(n) { check(received < n, () => "expect(received).toBeLessThan(" + fmt(n) + ")\n\nReceived: " + fmt(received)); return m; },
      toBeLessThanOrEqual(n) { check(received <= n, () => "expect(received).toBeLessThanOrEqual(" + fmt(n) + ")\n\nReceived: " + fmt(received)); return m; },
      toBeInstanceOf(c) { check(received instanceof c, () => "expect(received).toBeInstanceOf(expected)\n\nReceived: " + fmt(received)); return m; },
      toBeTypeOf(t) { check(typeof received === t, () => "expect(received).toBeTypeOf(" + fmt(t) + ")\n\nReceived type: " + (typeof received)); return m; },
      toContain(x) {
        let ok = false;
        if (typeof received === "string") ok = received.indexOf(x) !== -1;
        else if (received && typeof received.length === "number") ok = Array.prototype.indexOf.call(received, x) !== -1;
        check(ok, () => "expect(received).toContain(" + fmt(x) + ")\n\nReceived: " + fmt(received)); return m; },
      toHaveLength(n) { check(received != null && received.length === n, () => "expect(received).toHaveLength(" + fmt(n) + ")\n\nReceived length: " + (received == null ? "n/a" : received.length)); return m; },
      toMatch(re) { const rx = (re instanceof RegExp) ? re : new RegExp(re);
        check(typeof received === "string" && rx.test(received), () => "expect(received).toMatch(" + fmt(re) + ")\n\nReceived: " + fmt(received)); return m; },
      toThrow(x) { throwMatcher(received, isNot, x); return m; },
      toThrowError(x) { throwMatcher(received, isNot, x); return m; },
      // bun:test extension matchers (strict/type/collection helpers)
      toBeTrue() { check(received === true, () => "toBeTrue\n\nReceived: " + fmt(received)); return m; },
      toBeFalse() { check(received === false, () => "toBeFalse\n\nReceived: " + fmt(received)); return m; },
      toBeNil() { check(received == null, () => "toBeNil\n\nReceived: " + fmt(received)); return m; },
      toBeEmpty() {
        let p; const v = received;
        const len = (v != null && typeof v.length === "number") ? v.length : (v != null && typeof v.size === "number") ? v.size : undefined;
        if (len !== undefined) p = len === 0;
        else if (v != null && typeof v === "object" && typeof v[Symbol.iterator] === "function") { p = true; for (const _ of v) { p = false; break; } }
        else if (v != null && typeof v === "object") p = Object.keys(v).length === 0;
        else p = v == null;
        check(p, () => "toBeEmpty\n\nReceived: " + fmt(received)); return m; },
      toBeArray() { check(Array.isArray(received), () => "toBeArray\n\nReceived: " + fmt(received)); return m; },
      toBeArrayOfSize(n) { check(Array.isArray(received) && received.length === n, () => "toBeArrayOfSize(" + n + ")\n\nReceived: " + fmt(received)); return m; },
      toBeString() { check(typeof received === "string" || received instanceof String, () => "toBeString\n\nReceived: " + fmt(received)); return m; },
      toBeNumber() { check(typeof received === "number", () => "toBeNumber\n\nReceived: " + fmt(received)); return m; },
      toBeBoolean() { check(typeof received === "boolean", () => "toBeBoolean\n\nReceived: " + fmt(received)); return m; },
      toBeSymbol() { check(typeof received === "symbol", () => "toBeSymbol\n\nReceived: " + fmt(received)); return m; },
      toBeObject() { check(typeof received === "object" && received !== null, () => "toBeObject\n\nReceived: " + fmt(received)); return m; },
      toBeFunction() { check(typeof received === "function", () => "toBeFunction\n\nReceived: " + fmt(received)); return m; },
      toBeDate() { check(received instanceof Date, () => "toBeDate\n\nReceived: " + fmt(received)); return m; },
      toBeInteger() { check(Number.isInteger(received), () => "toBeInteger\n\nReceived: " + fmt(received)); return m; },
      toBePositive() { check(Number.isFinite(received) && received > 0, () => "toBePositive\n\nReceived: " + fmt(received)); return m; },
      toBeNegative() { check(Number.isFinite(received) && received < 0, () => "toBeNegative\n\nReceived: " + fmt(received)); return m; },
      toBeFinite() { check(Number.isFinite(received), () => "toBeFinite\n\nReceived: " + fmt(received)); return m; },
      toBeCloseTo(n, digits) { const d = digits === undefined ? 2 : digits;
        check(Math.abs(received - n) < Math.pow(10, -d) / 2, () => "toBeCloseTo(" + fmt(n) + ")\n\nReceived: " + fmt(received)); return m; },
      toBeWithin(lo, hi) { check(received >= lo && received < hi, () => "toBeWithin(" + lo + "," + hi + ")\n\nReceived: " + fmt(received)); return m; },
      toStartWith(s) { check(typeof received === "string" && received.startsWith(s), () => "toStartWith(" + fmt(s) + ")\n\nReceived: " + fmt(received)); return m; },
      toEndWith(s) { check(typeof received === "string" && received.endsWith(s), () => "toEndWith(" + fmt(s) + ")\n\nReceived: " + fmt(received)); return m; },
      toInclude(s) { check(typeof received === "string" && received.indexOf(s) !== -1, () => "toInclude(" + fmt(s) + ")\n\nReceived: " + fmt(received)); return m; },
      toContainEqual(x) { let ok = false; if (received && received.length != null) { for (let i = 0; i < received.length; i++) if (deepEqual(received[i], x)) { ok = true; break; } }
        check(ok, () => "toContainEqual(" + fmt(x) + ")\n\nReceived: " + fmt(received)); return m; },
      toHaveProperty(key, val) { const path = Array.isArray(key) ? key : String(key).split(".");
        let cur = received, ok = true; for (const k of path) { if (cur == null || !(k in Object(cur))) { ok = false; break; } cur = cur[k]; }
        if (ok && arguments.length > 1) ok = deepEqual(cur, val);
        check(ok, () => "toHaveProperty(" + fmt(key) + ")\n\nReceived: " + fmt(received)); return m; },
      toMatchObject(obj) { const sub = (a, b) => { if (isAsym(b)) return b.match(a); if (typeof b !== "object" || b === null) return deepEqual(a, b); if (a == null) return false;
          for (const k of Object.keys(b)) { if (!sub(a[k], b[k])) return false; } return true; };
        // bun quirk (bug-compatible): a top-level asymmetric matcher (e.g.
        // expect.objectContaining(...)) is NOT applied — any object passes.
        const pass = isAsym(obj) ? received !== null && typeof received === "object" : sub(received, obj);
        check(pass, () => "toMatchObject(" + fmt(obj) + ")\n\nReceived: " + fmt(received)); return m; },
      toBeOneOf(list) { let ok = false; for (const x of list) if (deepEqual(received, x)) { ok = true; break; }
        check(ok, () => "toBeOneOf\n\nReceived: " + fmt(received)); return m; },
      toBeEven() { let p; if (typeof received === "bigint") p = (received % 2n) === 0n; else if (typeof received === "number") p = Number.isInteger(received) && received % 2 === 0; else p = false;
        check(p, () => "toBeEven\n\nReceived: " + fmt(received)); return m; },
      toBeOdd() { let p; if (typeof received === "bigint") p = (received % 2n) !== 0n; else if (typeof received === "number") p = Number.isInteger(received) && received % 2 !== 0; else p = false;
        check(p, () => "toBeOdd\n\nReceived: " + fmt(received)); return m; },
      toSatisfy(pred) { let r; try { r = pred(received); } catch (e) { throw assertionError("toSatisfy() predicate threw an exception"); }
        check(!!r, () => "toSatisfy\n\nReceived: " + fmt(received)); return m; },
      toIncludeRepeated(sub, count) {
        if (typeof sub !== "string") throw assertionError("toIncludeRepeated() requires the first argument to be a string");
        if (typeof count !== "number" || !Number.isInteger(count) || count < 0 || Object.is(count, -0)) throw assertionError("toIncludeRepeated() requires the second argument to be a number");
        if (typeof received !== "string") throw assertionError("toIncludeRepeated() requires the expect(value) to be a string");
        if (sub.length === 0) throw assertionError("toIncludeRepeated() requires the first argument to be a non-empty string");
        let n = 0, i = 0; while ((i = received.indexOf(sub, i)) !== -1) { n++; i += sub.length; }
        check(n === count, () => "toIncludeRepeated\n\nReceived: " + fmt(received)); return m; },
      toEqualIgnoringWhitespace(expected) {
        if (typeof expected !== "string") throw assertionError("toEqualIgnoringWhitespace() requires argument to be a string");
        const strip = (s) => s.replace(/\s/g, "");
        check(typeof received === "string" && strip(received) === strip(expected), () => "toEqualIgnoringWhitespace\n\nExpected: " + fmt(expected) + "\nReceived: " + fmt(received)); return m; },
      // bun: unconditional pass/fail (respect .not — pass under .not fails, etc).
      pass(msg) { if (arguments.length > 0 && typeof msg !== "string") throw assertionError("Expected message to be a string for 'pass'."); check(true, () => (arguments.length > 0 ? msg : "passes by .pass() assertion")); return m; },
      fail(msg) { if (arguments.length > 0 && typeof msg !== "string") throw assertionError("Expected message to be a string for 'fail'."); check(false, () => (arguments.length > 0 ? msg : "fails by .fail() assertion")); return m; },
      // mock matchers (received is a mock/spy from mock()/spyOn())
      toHaveBeenCalled() { check(!!(received && received.mock && received.mock.calls.length > 0), () => "toHaveBeenCalled"); return m; },
      toHaveBeenCalledTimes(n) { check(!!(received && received.mock) && received.mock.calls.length === n, () => "toHaveBeenCalledTimes(" + n + ")\n\nReceived: " + (received && received.mock ? received.mock.calls.length : "n/a")); return m; },
      toHaveBeenCalledWith(...a) { let ok = false; if (received && received.mock) for (const call of received.mock.calls) if (deepEqual(call, a)) { ok = true; break; } check(ok, () => "toHaveBeenCalledWith(" + fmt(a) + ")"); return m; },
      toHaveBeenLastCalledWith(...a) { const c = received && received.mock ? received.mock.calls : []; check(c.length > 0 && deepEqual(c[c.length - 1], a), () => "toHaveBeenLastCalledWith(" + fmt(a) + ")"); return m; },
      toHaveBeenNthCalledWith(n, ...a) { const c = received && received.mock ? received.mock.calls : []; check(c.length >= n && deepEqual(c[n - 1], a), () => "toHaveBeenNthCalledWith"); return m; },
      toHaveBeenCalledOnce() { check(!!(received && received.mock) && received.mock.calls.length === 1, () => "toHaveBeenCalledOnce\n\nReceived: " + (received && received.mock ? received.mock.calls.length : "n/a")); return m; },
      // jest-compat aliases. Each maps onto the canonical matcher exactly as bun
      // does in jest.classes.ts:300-337 (`toBeCalled -> toHaveBeenCalled`,
      // `toBeCalledTimes -> toHaveBeenCalledTimes`, `toBeCalledWith ->
      // toHaveBeenCalledWith`, `lastCalledWith -> toHaveBeenLastCalledWith`,
      // `nthCalledWith -> toHaveBeenNthCalledWith`). itty-router's AutoRouter
      // spec calls `expect(missing).toBeCalledWith(...)`, which was `undefined`.
      toBeCalled() { return m.toHaveBeenCalled(); },
      toBeCalledTimes(n) { return m.toHaveBeenCalledTimes(n); },
      toBeCalledWith(...a) { return m.toHaveBeenCalledWith(...a); },
      lastCalledWith(...a) { return m.toHaveBeenLastCalledWith(...a); },
      nthCalledWith(n, ...a) { return m.toHaveBeenNthCalledWith(n, ...a); },
      toHaveReturned() { check(!!(received && received.mock && received.mock.results.length > 0), () => "toHaveReturned"); return m; },
      toHaveReturnedTimes(n) { check(!!(received && received.mock) && received.mock.results.length === n, () => "toHaveReturnedTimes(" + n + ")"); return m; },
      toHaveReturnedWith(v) { if (!(received && received.mock)) throw assertionError("Expected value must be a mock function"); const rs = received.mock.results; check(rs.some((r) => r.type === "return" && deepEqual(r.value, v)), () => "toHaveReturnedWith(" + fmt(v) + ")"); return m; },
      toHaveLastReturnedWith(v) { const rs = received && received.mock ? received.mock.results : []; check(rs.length > 0 && rs[rs.length - 1].type === "return" && deepEqual(rs[rs.length - 1].value, v), () => "toHaveLastReturnedWith(" + fmt(v) + ")"); return m; },
      toHaveNthReturnedWith(n, v) { const rs = received && received.mock ? received.mock.results : []; check(rs.length >= n && rs[n - 1].type === "return" && deepEqual(rs[n - 1].value, v), () => "toHaveNthReturnedWith"); return m; },
      // snapshot matchers (best-effort: compare a jest-ish serialization when an
      // inline snapshot is supplied; record-and-pass when none is — no snapshot file)
      toMatchSnapshot() { return m; },
      toMatchInlineSnapshot(snap) {
        if (snap === undefined) return m;  // no inline arg → record mode
        const ser = snapSerialize(received);
        const norm = (s) => String(s).replace(/\r/g, "").trim().split("\n").map((l) => l.trim()).join("\n");
        check(norm(ser) === norm(snap), () => "toMatchInlineSnapshot\n\nExpected: " + fmt(snap) + "\nReceived: " + fmt(ser)); return m;
      },
      toThrowErrorMatchingSnapshot() { return m; },
      toThrowErrorMatchingInlineSnapshot() { return m; },
      // bun: expect(fileOrCmd).toRun() — spawn it and assert exit 0.
      toRun(expectedStdout) {
        // bun semantics: expect([...args]).toRun() runs `<runtime> ...args` —
        // the runtime binary is prepended in both the array and string forms.
        const cmd = [process.execPath || "bun", ...(Array.isArray(received) ? received : [received])];
        let r; try { r = Bun.spawnSync(cmd); } catch (e) { check(false, () => "toRun: spawn failed: " + e); return m; }
        check(r.exitCode === 0, () => "toRun\n\nExpected exit 0, got " + r.exitCode + "\n" + (r.stderr ? r.stderr.toString() : ""));
        if (expectedStdout !== undefined) check((r.stdout ? r.stdout.toString() : "") === expectedStdout, () => "toRun stdout mismatch"); return m;
      },
      // async matchers on a promise
      get resolves() { return makeAsyncMatchers(received, isNot, false); },
      get rejects() { return makeAsyncMatchers(received, isNot, true); },
    };
    // Custom matchers registered via expect.extend({ name(received, ...args) {...} }).
    for (const name of Object.keys(S.customMatchers)) {
      m[name] = (...args) => {
        const ctx = { isNot, equals: deepEqual, promise: "", utils: { printReceived: fmt, printExpected: fmt, matcherHint: () => "", stringify: fmt, EXPECTED_COLOR: (s) => s, RECEIVED_COLOR: (s) => s } };
        const res = S.customMatchers[name].call(ctx, received, ...args) || {};
        let pass = !!res.pass; if (isNot) pass = !pass;
        if (!pass) throw assertionError(typeof res.message === "function" ? res.message() : String(res.message || name));
        return m;
      };
    }
    Object.defineProperty(m, "not", { get() { return makeMatchers(received, !isNot); } });
    return m;
  }
  function makeAsyncMatchers(promise, isNot, wantReject) {
    const build = (fnName) => async function (...a) {
      let val, threw = false, err;
      try { val = await promise; } catch (e) { threw = true; err = e; }
      if (wantReject) {
        if (!threw) throw assertionError("expect(promise).rejects — promise resolved instead of rejecting");
        if (fnName === "toThrow" || fnName === "toThrowError") {
          const msg = (err && err.message !== undefined) ? String(err.message) : String(err);
          const expected = a[0]; let pass = true;
          if (expected !== undefined) {
            if (typeof expected === "string") pass = msg.indexOf(expected) !== -1;
            else if (expected instanceof RegExp) pass = expected.test(msg);
            else if (typeof expected === "function") pass = err instanceof expected;
          }
          if (isNot) pass = !pass;
          if (!pass) throw assertionError("expect(promise).rejects.toThrow\n\nReceived message: " + msg);
          return;
        }
        return makeMatchers(err, isNot)[fnName](...a);
      } else {
        if (threw) throw err;
        return makeMatchers(val, isNot)[fnName](...a);
      }
    };
    return new Proxy({}, { get(_, prop) { return build(prop); } });
  }
  function evalThrow(threw, err, isNot, expected) {
    let pass = threw, detail = "";
    if (threw && expected !== undefined) {
      const msg = (err && err.message !== undefined) ? String(err.message) : String(err);
      if (typeof expected === "string") { pass = msg.indexOf(expected) !== -1;
        detail = "\n\nExpected substring: " + fmt(expected) + "\nReceived message: " + fmt(msg); }
      else if (expected instanceof RegExp) { pass = expected.test(msg);
        detail = "\n\nExpected pattern: " + fmt(expected) + "\nReceived message: " + fmt(msg); }
      else if (typeof expected === "function") { pass = (err instanceof expected);
        detail = "\n\nExpected constructor: " + (expected.name || "?"); }
      else if (expected instanceof Error) { pass = (msg === String(expected.message));
        detail = "\n\nExpected message: " + fmt(String(expected.message)) + "\nReceived message: " + fmt(msg); }
    }
    if (isNot) pass = !pass;
    if (!pass) throw assertionError("expect(received).toThrow()" + detail + (threw ? "" : "\n\n(function did not throw)"));
  }
  function throwMatcher(fn, isNot, expected) {
    if (typeof fn !== "function")
      throw assertionError("expect(received).toThrow() — received value must be a function");
    let threw = false, err, ret;
    try { ret = fn(); } catch (e) { threw = true; err = e; }
    // A function returning a *native promise*: bun's toThrow awaits it and treats
    // a rejection as a throw. The matcher is sync, so defer the check into
    // S.pendingAsserts, which runTest awaits after the body settles.
    // The brand check must be `instanceof Promise`, NOT a duck-typed `.then`
    // probe: bun gates this on `return_value.as_any_promise()` (expect.rs:838
    // get_value_as_to_throw), which matches only a real JSPromise. A plain
    // object that merely *has* a `then` method falls through there and is
    // treated as an ordinary return value — never awaited. Duck-typing it here
    // hung any library whose objects are thenable-by-accident: itty-router's
    // Router proxies every unknown property (including `then`) to a
    // route-registering function that never settles, so
    // `expect(() => router.toString()).not.toThrow()` waited forever.
    if (!threw && ret instanceof Promise) {
      const p = ret.then(function () { evalThrow(false, undefined, isNot, expected); },
                         function (e) { evalThrow(true, e, isNot, expected); });
      (S.pendingAsserts || (S.pendingAsserts = [])).push(p);
      return;
    }
    evalThrow(threw, err, isNot, expected);
  }
  function expect(received) { S.expectCalls++; return makeMatchers(received, false); }
  expect.unreachable = function (msg) {
    if (msg instanceof Error) throw msg;
    throw new Error(msg === undefined ? "reached unreachable code" : String(msg));
  };
  // expect.extend({ name(received, ...args) { return { pass, message } } }) — makeMatchers
  // reads S.customMatchers per-call, so matchers added here apply to every expect().
  expect.extend = function (obj) {
    if (!obj || typeof obj !== "object") throw new TypeError("expect.extend: expected an object of matchers");
    for (const name of Object.keys(obj)) {
      if (typeof obj[name] !== "function")
        throw new TypeError("expect.extend: `" + name + "` is not a valid matcher. Received " + typeof obj[name]);
    }
    Object.assign(S.customMatchers, obj);
    return expect;
  };
  // Asymmetric matchers. Recognised by deepEqual via `instanceof AsymmetricMatcher`,
  // NOT by probing for a marker property.
  //
  // ref: bun-ref/src/jsc/bindings/bindings.cpp:309 matchAsymmetricMatcherAndGetFlags
  //      dispatches with `dynamicDowncast<JSExpectAnything/JSExpectAny/...>(matcherPropCell)`
  //      (:314,:323,:424,:471,:515,...) — a *type* check against real JSC classes, and
  //      returns NOT_MATCHER when the value is not one. A property probe is not
  //      equivalent: any Proxy with a `get` trap that answers every key (elysia's cookie
  //      jar, src/cookies.ts:355 `get(_, key) { return new Cookie(key) }`) reports a
  //      truthy marker and then a non-callable `.match`, so `expect(jar).toEqual({})`
  //      died with "a.match is not a function". `instanceof` is the JS-level analogue of
  //      dynamicDowncast: it walks the prototype chain, which a `get`-only Proxy cannot
  //      forge.
  class AsymmetricMatcher {
    constructor(name, match) { this.$$name = name; this.match = match; this.asymmetricMatch = match; }
    toString() { return this.$$name; }
  }
  const isAsym = (v) => v instanceof AsymmetricMatcher;
  const asym = (name, match) => new AsymmetricMatcher(name, match);
  expect.anything = () => asym("anything", (v) => v != null);
  expect.any = (ctor) => asym("any", (v) => {
    if (ctor === String) return typeof v === "string" || v instanceof String;
    if (ctor === Number) return typeof v === "number" || v instanceof Number;
    if (ctor === Boolean) return typeof v === "boolean";
    if (ctor === Function) return typeof v === "function";
    if (ctor === Object) return typeof v === "object" && v !== null;
    if (ctor === BigInt) return typeof v === "bigint";
    if (ctor === Symbol) return typeof v === "symbol";
    return v != null && (v instanceof ctor || (v.constructor === ctor));
  });
  expect.objectContaining = (obj) => asym("objectContaining", (v) => { if (v == null || typeof v !== "object") return false; for (const k of Object.keys(obj)) { if (!(k in v) || !deepEqual(v[k], obj[k])) return false; } return true; });
  expect.arrayContaining = (arr) => asym("arrayContaining", (v) => { if (!Array.isArray(v)) return false; return arr.every((x) => v.some((y) => deepEqual(y, x))); });
  expect.stringContaining = (s) => asym("stringContaining", (v) => typeof v === "string" && v.indexOf(s) !== -1);
  expect.stringMatching = (re) => asym("stringMatching", (v) => typeof v === "string" && (re instanceof RegExp ? re.test(v) : v.indexOf(String(re)) !== -1));
  expect.closeTo = (n, d) => asym("closeTo", (v) => typeof v === "number" && Math.abs(v - n) < Math.pow(10, -(d === undefined ? 2 : d)) / 2);
  expect.not = { objectContaining: (o) => asym("not.objectContaining", (v) => !expect.objectContaining(o).match(v)), arrayContaining: (a) => asym("not.arrayContaining", (v) => !expect.arrayContaining(a).match(v)), stringContaining: (s) => asym("not.stringContaining", (v) => !expect.stringContaining(s).match(v)), stringMatching: (r) => asym("not.stringMatching", (v) => !expect.stringMatching(r).match(v)) };
  expect.assertions = () => {}; expect.hasAssertions = () => {};

  // bun ScopeFunctions.rs:557-597 get_description: a class/function first arg
  // labels the scope by its NAME (not its source); unnamed → throw.
  function describeLabel(name) {
    if (name == null) return "";
    const t = typeof name;
    if (t === "string") return name;
    if (t === "number") return String(name);
    if (t === "function") { const n = name.name; if (n) return n; }
    throw new Error("describe() expects first argument to be a named class, named function, number, or string");
  }
  function describe(name, fn) {
    // no-label form: describe(() => { ... })
    if (typeof fn !== "function" && typeof name === "function") { fn = name; name = ""; }
    const scope = makeScope(describeLabel(name), S.current);
    scope.skipped = (S.skipDepth || 0) > 0;
    S.current.items.push({ type: "scope", scope: scope });
    const prev = S.current; S.current = scope;
    try { if (typeof fn === "function") fn(); }
    catch (e) {
      // bun (Collection.zig): a throw in a describe callback drops the scope —
      // tests already enqueued in it never run — and is reported as a file-level
      // error; siblings registered before/after still run.
      scope.items.length = 0;
      scope.beforeAll.length = 0; scope.afterAll.length = 0;
      scope.beforeEach.length = 0; scope.afterEach.length = 0;
      S.errors.push((e && e.message !== undefined) ? String(e.message) : String(e));
    }
    finally { S.current = prev; }
  }
  function makeTest(mode) {
    // Supports test(name, fn) and test(name, options, fn) (the options object —
    // e.g. { timeout, retry } — is recorded but its knobs beyond selection are
    // not yet honored). fn is whichever argument is a function.
    return function (name, a, b) {
      const fn = typeof a === "function" ? a : (typeof b === "function" ? b : undefined);
      // options may come 2nd (test(name, opts, fn)) or 3rd (test(name, fn, opts));
      // a bare number is a timeout (test(name, fn, 10)).
      let opts = undefined;
      for (const x of [a, b]) {
        if (x === fn || x == null) continue;
        if (typeof x === "object") opts = x;
        else if (typeof x === "number") opts = { timeout: x };
      }
      // bun throws at REGISTRATION when a runnable test has no body (todo/skip may omit it).
      if (fn === undefined && mode !== "todo" && mode !== "skip") throw new TypeError("test() expects a function");
      S.current.items.push({ type: "test", name: String(name), fn: fn, opts: opts, mode: (S.skipDepth > 0 ? "skip" : mode) });
    };
  }
  // %s/%d/%i/%o placeholder + %# index interpolation for test.each/describe.each.
  function interpName(name, args, idx) {
    let i = 0;
    return String(name).replace(/%[sdifjo#%]/g, (mm) => { if (mm === "%%") return "%"; if (mm === "%#") return String(idx); const v = args[i++]; return typeof v === "object" ? fmt(v) : String(v); });
  }
  function eachRegistrar(modeFn, table) {
    return function (name, fn) {
      (Array.isArray(table) ? table : []).forEach((row, idx) => {
        const args = Array.isArray(row) ? row : [row];
        const _body = (typeof fn === "function" && fn.length > args.length)
          ? function (done) { return fn.apply(null, args.concat([done])); }
          : function () { return fn.apply(null, args); };
        modeFn()(interpName(name, args, idx), _body);
      });
    };
  }
  function makeEach(modeFn) {
    return function (table) {
      // jest: the each-bound registrar keeps the modifier chain —
      // test.each(t).skipIf(c)("name", fn). Plain registrars (no modifiers)
      // back the chain so decoration can't recurse.
      const bound = eachRegistrar(modeFn, table);
      bound.skipIf = (c) => (c ? eachRegistrar(() => makeTest("skip"), table) : bound);
      bound.todoIf = (c) => (c ? eachRegistrar(() => makeTest("todo"), table) : bound);
      bound.failingIf = (c) => (c ? eachRegistrar(() => makeTest("failing"), table) : bound);
      bound.if = (c) => (c ? bound : eachRegistrar(() => makeTest("skip"), table));
      bound.skip = eachRegistrar(() => makeTest("skip"), table);
      bound.todo = eachRegistrar(() => makeTest("todo"), table);
      bound.failing = eachRegistrar(() => makeTest("failing"), table);
      bound.only = bound;
      return bound;
    };
  }
  function skipScope(name, fn) { S.skipDepth = (S.skipDepth || 0) + 1; try { describe(name, fn); } finally { S.skipDepth--; } }

  // Decorate a test() function with the full modifier chain (.skip/.todo/.only/
  // .failing/.concurrent + .each on each + .skipIf/.todoIf/.failingIf/.if, each of
  // which returns another decorated function so arbitrary chaining works, e.g.
  // test.concurrent.skipIf(cond).each(table)). `deep` gates the eager .concurrent
  // sub-function so the definition doesn't recurse infinitely; the *If arrows are
  // lazy so they can decorate recursively on demand.
  function decorate(fn, mode) {
    fn.skip = makeTest("skip"); fn.skip.each = makeEach(() => makeTest("skip"));
    fn.todo = makeTest("todo"); fn.todo.each = makeEach(() => makeTest("todo"));
    fn.failing = makeTest("failing"); fn.failing.each = makeEach(() => makeTest("failing"));
    fn.only = fn;                       // `.only` filtering DEFERRED → runs
    fn.each = makeEach(() => makeTest(mode));
    fn.skipIf = (c) => decorate(makeTest(c ? "skip" : mode), c ? "skip" : mode);
    fn.todoIf = (c) => decorate(makeTest(c ? "todo" : mode), c ? "todo" : mode);
    fn.failingIf = (c) => decorate(makeTest(c ? "failing" : mode), c ? "failing" : mode);
    fn.if = (c) => decorate(makeTest(c ? mode : "skip"), c ? mode : "skip");
    fn.concurrentIf = (c) => decorate(makeTest(c ? mode : "skip"), c ? mode : "skip");
    // .concurrent/.serial (concurrency DEFERRED → serial; .serial is already the
    // execution model) are lazy memoized getters so the chain is fully
    // bidirectional — test.concurrent.skipIf(c) AND test.skipIf(c).concurrent
    // both work — without infinite eager recursion.
    Object.defineProperty(fn, "concurrent", { configurable: true, get() { const c = decorate(makeTest(mode), mode); Object.defineProperty(fn, "concurrent", { value: c, configurable: true }); return c; } });
    Object.defineProperty(fn, "serial", { configurable: true, get() { const c = decorate(makeTest(mode), mode); Object.defineProperty(fn, "serial", { value: c, configurable: true }); return c; } });
    return fn;
  }
  const test = decorate(makeTest("run"), "run");
  const it = test;

  describe.skip = skipScope; describe.only = describe; describe.todo = skipScope;
  describe.concurrent = describe;       // concurrency DEFERRED → serial
  describe.serial = describe;           // serial is already the execution model
  describe.each = function (table) { return function (name, fn) { (Array.isArray(table) ? table : []).forEach((row, idx) => { const args = Array.isArray(row) ? row : [row]; describe(interpName(name, args, idx), function () { return fn.apply(null, args); }); }); }; };
  describe.skipIf = (c) => (c ? skipScope : describe);
  describe.todoIf = (c) => (c ? skipScope : describe);
  describe.if = (c) => (c ? describe : skipScope);

  function beforeEach(fn) { S.current.beforeEach.push(fn); }
  function afterEach(fn) { S.current.afterEach.push(fn); }
  function beforeAll(fn) { S.current.beforeAll.push(fn); }
  function afterAll(fn) { S.current.afterAll.push(fn); }

  // mock / spyOn / jest (minimal — call tracking + implementation override).
  const __allMocks = [];
  function mock(impl) {
    const f = function () { f.mock.calls.push(Array.prototype.slice.call(arguments)); const imp = f._once.length ? f._once.shift() : f._impl; let r; try { r = imp ? imp.apply(this, arguments) : undefined; f.mock.results.push({ type: "return", value: r }); } catch (e) { f.mock.results.push({ type: "throw", value: e }); throw e; } return r; };
    __allMocks.push(f);
    f._impl = impl; f._once = []; f.mock = { calls: [], results: [], instances: [], lastCall: undefined };
    f.mockReturnValue = (v) => { f._impl = () => v; return f; };
    f.mockReturnValueOnce = (v) => { f._once.push(() => v); return f; };
    f.mockResolvedValue = (v) => { f._impl = () => Promise.resolve(v); return f; };
    f.mockResolvedValueOnce = (v) => { f._once.push(() => Promise.resolve(v)); return f; };
    f.mockRejectedValue = (v) => { f._impl = () => Promise.reject(v); return f; };
    f.mockRejectedValueOnce = (v) => { f._once.push(() => Promise.reject(v)); return f; };
    f.mockImplementation = (i) => { f._impl = i; return f; };
    f.mockImplementationOnce = (i) => { f._once.push(i); return f; };
    f.mockName = () => f; f.mockReturnThis = () => { f._impl = function () { return this; }; return f; };
    f.mockReset = () => { f.mock.calls = []; f.mock.results = []; f._once = []; return f; };
    f.mockClear = () => { f.mock.calls = []; f.mock.results = []; return f; };
    f.mockRestore = () => {};
    f.getMockName = () => "mock";
    return f;
  }
  // Static helpers on the bun:test `mock` function.
  mock.module = function (name, factory) { try { const m = factory(); G.__mbunNativeModules = G.__mbunNativeModules || {}; G.__mbunNativeModules[name] = (m && m.default !== undefined && Object.keys(m).length === 1) ? m.default : m; G.__mbunNativeModules["node:" + name] = G.__mbunNativeModules[name]; } catch (e) {} };
  mock.restore = function () {};
  // clearAllMocks: reset call/result history for every mock (implementations preserved).
  mock.clearAllMocks = function () { for (const f of __allMocks) if (f && typeof f.mockClear === "function") f.mockClear(); };
  // resetAllMocks: clear history and drop queued one-off implementations.
  mock.resetAllMocks = function () { for (const f of __allMocks) if (f && typeof f.mockReset === "function") f.mockReset(); };
  // restoreAllMocks: restore spied-on originals.
  mock.restoreAllMocks = function () { for (const f of __allMocks) if (f && typeof f.mockRestore === "function") f.mockRestore(); };
  function spyOn(obj, key) { const orig = obj[key]; const f = mock(typeof orig === "function" ? orig.bind(obj) : undefined); f.mockRestore = () => { obj[key] = orig; }; obj[key] = f; return f; }
  // ── setSystemTime ───────────────────────────────────────────────────────────
  // bun installs a native setSystemTime (jest.rs:435-436 create_mock_objects →
  // JSMock__jsSetSystemTime), which pins globalObject->overridenDateNow.
  // Verified against bun 1.3.14: the set time is returned EXACTLY by Date.now()
  // (the clock does not keep ticking from it), `new Date()` observes it, a
  // no-arg call clears the override, and a raw ms number is accepted.
  // (FakeTimers.rs:90-96's rebasing offset is the fake-timers-active path; it is
  // a no-op while fake timers are inactive, which is the case here — mbun does
  // not fake timers, so the fixed-pin override is the behaviour to match.)
  //
  // The Date patch is installed ONCE per process and reads the pinned value out
  // of the live G.__mbunState, so it survives across files (same discipline as
  // the async wrappers above). S.sysTime === null means "no override".
  if (!G.__mbunDatePatched) {
    G.__mbunDatePatched = true;
    const RD = G.Date;
    const pinned = function () {
      const st = G.__mbunState;
      return (st && st.sysTime !== null && st.sysTime !== undefined) ? st.sysTime : null;
    };
    const MbunDate = function Date(...args) {
      if (!new.target) return RD();           // Date() without new → string
      const p = pinned();
      const a = (args.length === 0 && p !== null) ? [p] : args;
      // Reflect.construct preserves new.target so `class X extends Date` keeps
      // X.prototype (bun pins natively and needs no wrapper at all).
      return Reflect.construct(RD, a, new.target);
    };
    MbunDate.prototype = RD.prototype;        // instanceof / methods unchanged
    Object.setPrototypeOf(MbunDate, RD);      // inherit statics (UTC/parse/…)
    MbunDate.now = function () { const p = pinned(); return p === null ? RD.now() : p; };
    G.Date = MbunDate;
  }
  function setSystemTime(v) {
    if (v === undefined || v === null) { S.sysTime = null; return; }
    S.sysTime = (typeof v === "number") ? v : Number(v.valueOf());
  }

  // --- Fake timers (jest.useFakeTimers) ------------------------------------
  // Ported from bun's src/runtime/test_runner/timers/FakeTimers.rs. When enabled
  // we swap G.setTimeout/clearTimeout/setInterval/clearInterval for queue-backed
  // fakes keyed off a fake `now` (start 0). advanceTimersByTime(ms) advances the
  // fake clock and fires due timers in (fireAt, id) order, re-arming intervals.
  // The per-test timeout stays on natSetTimeout (line 81), so it is unfakeable.
  const FT = { on: false, now: 0, seq: 0, queue: [], saved: null };
  function ftRemoveById(id) {
    for (let i = 0; i < FT.queue.length; i++) { if (FT.queue[i].id === id) { FT.queue.splice(i, 1); return; } }
  }
  function ftSchedule(fn, delay, args, interval) {
    const id = ++FT.seq;
    let d = Number(delay); if (!isFinite(d) || d < 0) d = 0;
    FT.queue.push({ id: id, fireAt: FT.now + d, fn: fn, args: args, interval: interval });
    return id;
  }
  function ftInstall() {
    if (FT.on) { try { G.setTimeout.clock = true; } catch (e) {} return; }
    FT.on = true; FT.now = 0; FT.seq = 0; FT.queue = [];
    FT.saved = { setTimeout: G.setTimeout, clearTimeout: G.clearTimeout,
                 setInterval: G.setInterval, clearInterval: G.clearInterval };
    const fakeSetTimeout = function (fn, delay) {
      return ftSchedule(fn, delay, Array.prototype.slice.call(arguments, 2), 0);
    };
    // testing-library/react feature-detects fake timers via
    // hasOwnProperty(setTimeout, "clock"); expose it on the fake itself.
    fakeSetTimeout.clock = true;
    const fakeSetInterval = function (fn, delay) {
      let iv = Number(delay); if (!isFinite(iv) || iv <= 0) iv = 1;
      return ftSchedule(fn, delay, Array.prototype.slice.call(arguments, 2), iv);
    };
    G.setTimeout = fakeSetTimeout;
    G.setInterval = fakeSetInterval;
    G.clearTimeout = function (id) { if (id != null) ftRemoveById(id); };
    G.clearInterval = function (id) { if (id != null) ftRemoveById(id); };
  }
  function ftUninstall() {
    if (!FT.on) return;
    FT.on = false;
    if (FT.saved) {
      G.setTimeout = FT.saved.setTimeout; G.clearTimeout = FT.saved.clearTimeout;
      G.setInterval = FT.saved.setInterval; G.clearInterval = FT.saved.clearInterval;
      FT.saved = null;
    }
    FT.queue = [];
  }
  // Fire all timers with fireAt <= target in (fireAt, id) order, re-arming
  // intervals. `guard` caps runaway interval loops (FakeTimers ports do the same).
  function ftExecuteUntil(target) {
    let guard = 0;
    for (;;) {
      let next = null;
      for (const t of FT.queue) {
        if (t.fireAt <= target &&
            (next === null || t.fireAt < next.fireAt || (t.fireAt === next.fireAt && t.id < next.id))) next = t;
      }
      if (next === null) break;
      FT.now = next.fireAt;
      if (next.interval > 0) { next.fireAt = next.fireAt + next.interval; }
      else { ftRemoveById(next.id); }
      try { next.fn.apply(undefined, next.args || []); }
      catch (e) { const st = G.__mbunState; if (st && st.asyncErr === undefined) st.asyncErr = e; else throw e; }
      if (++guard > 1000000) break;
    }
    if (FT.now < target) FT.now = target;
  }
  function ftAdvanceBy(ms) {
    let n = Number(ms); if (!isFinite(n) || n < 0) n = 0;
    // advanceTimersByTime(0) advances 1ms so setTimeout(fn,0) fires — bun
    // FakeTimers.rs:449-452 (effective_advance = if arg==0 {1} else {arg}).
    const eff = (n === 0) ? 1 : n;
    ftExecuteUntil(FT.now + eff);
  }
  function ftAdvanceToNext() {
    let next = null;
    for (const t of FT.queue) { if (next === null || t.fireAt < next.fireAt || (t.fireAt === next.fireAt && t.id < next.id)) next = t; }
    if (next !== null) ftExecuteUntil(next.fireAt);
  }
  function ftRunAll() {
    let guard = 0;
    while (FT.queue.length && ++guard <= 100000) ftAdvanceToNext();
  }

  const jest = { fn: mock, spyOn: spyOn, mock: (m, f) => { if (typeof m !== "string") throw new TypeError("jest.mock() 1st argument must be a string"); if (typeof f !== "function") throw new TypeError("jest.mock() 2nd argument must be a function"); }, unmock: () => {}, useFakeTimers: () => { ftInstall(); return jest; }, useRealTimers: () => { setSystemTime(); ftUninstall(); return jest; }, setSystemTime: (v) => { setSystemTime(v); return jest; }, restoreAllMocks: () => mock.restoreAllMocks(), clearAllMocks: () => mock.clearAllMocks(), resetAllMocks: () => mock.resetAllMocks(), advanceTimersByTime: (ms) => { ftAdvanceBy(ms); return jest; }, advanceTimersToNextTimer: () => { ftAdvanceToNext(); return jest; }, runAllTimers: () => { ftRunAll(); return jest; }, runOnlyPendingTimers: () => { ftRunAll(); return jest; }, clearAllTimers: () => { FT.queue = []; return jest; }, getTimerCount: () => FT.queue.length };

  // `vi` is bun:test's vitest-compat surface. It is NOT the same object as `jest`
  // (verified against bun 1.3.14: `vi === jest` is false) and carries its own key
  // set; the timer helpers mirror the same stub level as `jest` above rather than
  // pretending to fake timers.
  const vi = { fn: mock, mock: function () {}, spyOn: spyOn,
               clearAllMocks: function () { mock.clearAllMocks(); },
               resetAllMocks: function () { mock.resetAllMocks(); },
               restoreAllMocks: function () { mock.restoreAllMocks(); },
               useFakeTimers: function () { return vi; },
               // No vi.setSystemTime: verified against bun 1.3.14, `vi` exposes
               // useRealTimers but leaves setSystemTime undefined (vitest suites
               // feature-detect on it). useRealTimers does clear the override.
               useRealTimers: function () { setSystemTime(); return vi; },
               isFakeTimers: function () { return false; }, clearAllTimers: function () {},
               runAllTimers: function () {}, runOnlyPendingTimers: function () {},
               advanceTimersByTime: function () {}, advanceTimersToNextTimer: function () {},
               getTimerCount: function () { return 0; } };

  // bun:test expectTypeOf (expect.rs:2955): runtime no-op type-assertion chain —
  // callable (never new-able); every property access / call returns a fresh chain.
  const expectTypeOf = (() => {
    const make = () => new Proxy(function expectTypeOf() {}, {
      get: (_t, p) => (p === Symbol.toPrimitive || p === "then" ? undefined : make()),
      apply: () => make(),
      construct: () => { throw new TypeError("expectTypeOf is not a constructor"); },
    });
    return make();
  })();

  G.__mbunBT = { test, it, describe, xdescribe: describe, xit: test.skip, xtest: test.skip, expect,
                 beforeEach, afterEach, beforeAll, afterAll, mock, spyOn, jest, vi, expectTypeOf,
                 setDefaultTimeout: function () {}, setSystemTime: setSystemTime,
                 spyOn: spyOn };
  // bun exposes the test globals without an explicit import; mirror onto globalThis.
  // `vi` is deliberately excluded: bun 1.3.14 exports it from bun:test but leaves
  // globalThis.vi undefined, and vitest suites feature-detect on that.
  for (const k of Object.keys(G.__mbunBT)) if (k !== "vi" && typeof G[k] === "undefined") G[k] = G.__mbunBT[k];
  // Bun.jest(path) returns the file-scoped test API (expect/test/it/describe/hooks/
  // mock) — this is what test harnesses destructure (e.g. node harness.ts's
  // createTest). Returning the mock-utility object instead left `expect` undefined
  // → hideFromStackTrace(undefined) → "Properties can only be defined on Objects".
  if (G.Bun) G.Bun.jest = function () { return G.__mbunBT; };

  function fullName(scope, name) {
    const parts = []; let s = scope;
    while (s && s.name != null) { parts.unshift(s.name); s = s.parent; }
    parts.push(name); return parts.join(" > ");
  }
  // ── --randomize / --seed: shuffle each describe scope's entries ─────────────
  // Port of bun's Order.rs. The native side sets G.__mbunTestSeed to the per-file
  // seed (decimal string, u64) when --randomize/--seed is on, else leaves it null.
  // xoshiro256++ seeded through SplitMix64 == Zig's std.Random.DefaultPrng, which
  // is what `Order::Config.randomize` carries (ref: bun-ref/src/runtime/
  // test_runner/Order.rs:268 + bun_test.rs:1047-1060). Kept bit-exact so a given
  // seed reproduces a run, matching Order.rs:273-275's cross-version contract.
  const U64 = (1n << 64n) - 1n;
  function makePrng(seedStr) {
    let sm = BigInt(seedStr) & U64;
    const smNext = () => {  // SplitMix64
      sm = (sm + 0x9e3779b97f4a7c15n) & U64;
      let z = sm;
      z = ((z ^ (z >> 30n)) * 0xbf58476d1ce4e5b9n) & U64;
      z = ((z ^ (z >> 27n)) * 0x94d049bb133111ebn) & U64;
      return (z ^ (z >> 31n)) & U64;
    };
    const s = [smNext(), smNext(), smNext(), smNext()];
    const rotl = (x, k) => ((x << k) | (x >> (64n - k))) & U64;
    return function next() {  // xoshiro256++ next_u64
      const r = (rotl((s[0] + s[3]) & U64, 23n) + s[0]) & U64;
      const t = (s[1] << 17n) & U64;
      s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
      s[2] ^= t;
      s[3] = rotl(s[3], 45n);
      return r;
    };
  }
  // Exact port of std.Random.uintLessThan(u64, less_than) — Lemire's debiased
  // bounded method (ref: Order.rs:291-313).
  function uintLessThan(next, lessThan) {
    const L = BigInt(lessThan);
    let x = next();
    let m = x * L;
    let l = m & U64;
    if (l < L) {
      let t = (-L) & U64;  // -%lessThan
      if (t >= L) { t -= L; if (t >= L) t %= L; }
      while (l < t) { x = next(); m = x * L; l = m & U64; }
    }
    return m >> 64n;
  }
  // Forward Fisher-Yates: i from 0 to len-2, j = i + uintLessThan(len - i)
  // (ref: Order.rs:277-287 shuffle_with_index).
  function shuffleWithIndex(next, buf) {
    if (buf.length < 2) return;
    const max = buf.length;
    for (let i = 0; i < max - 1; i++) {
      const j = i + Number(uintLessThan(next, max - i));
      const tmp = buf[i]; buf[i] = buf[j]; buf[j] = tmp;
    }
  }
  // One PRNG per file, advanced across scopes in generation order — bun builds a
  // single Order per file and shuffles every describe scope's `entries` from that
  // one PRNG (Order.rs:94-96 inside generate_order_describe).
  S.rand = null;
  // A skipped scope's tests count as skip WITHOUT running any of its hooks
  // (bun: describe.skip never runs beforeAll/beforeEach/afterEach/afterAll).
  function skipAllIn(scope) {
    for (const item of scope.items) {
      if (item.type === "test") {
        S.total++;
        if (item.mode === "todo") { S.todo++; S.out.push("(todo) " + fullName(scope, item.name)); }
        else { S.skip++; S.out.push("(skip) " + fullName(scope, item.name)); }
      } else skipAllIn(item.scope);
    }
  }
  // beforeAll threw: every test in the scope fails with that error (bun's hook
  // failure attribution), but sibling scopes keep running.
  function failAllIn(scope, msg) {
    for (const item of scope.items) {
      if (item.type === "test") {
        if (item.mode === "skip" || item.mode === "todo") {
          S.total++;
          if (item.mode === "todo") { S.todo++; S.out.push("(todo) " + fullName(scope, item.name)); }
          else { S.skip++; S.out.push("(skip) " + fullName(scope, item.name)); }
        } else {
          S.total++; S.fail++;
          S.out.push("(fail) " + fullName(scope, item.name));
          S.out.push("      error: " + msg);
        }
      } else failAllIn(item.scope, msg);
    }
  }
  // Run one hook, supporting done-callback style (fn.length >= 1) exactly like
  // a done-style test body: done() may fire sync, via a microtask, or via a
  // timer (setImmediate/setTimeout push into __mbunTimers.q, which the C++ pump
  // drains). Without this, a done-style beforeEach/beforeAll would not be
  // awaited and the test body would observe its side effects as not-yet-applied.
  function callHook(h) {
    if (typeof h === "function" && h.length >= 1) {
      return new Promise((resolve, reject) => {
        let settled = false;
        const done = (err) => { if (settled) return; settled = true; if (err) reject(err instanceof Error ? err : new Error(String(err))); else resolve(); };
        let r; try { r = h(done); } catch (e) { done(e); return; }
        if (r && typeof r.then === "function") { r.then(() => done(), (e) => done(e)); return; }
        (async () => { for (let i = 0; i < 8 && !settled; i++) await Promise.resolve(); if (!settled && (!G.__mbunTimers || G.__mbunTimers.q.length === 0)) { settled = true; resolve(); } })();
      });
    }
    const r = h();
    return (r && typeof r.then === "function") ? r : Promise.resolve();
  }
  async function runScope(scope, beChain, aeChain) {
    if (scope.skipped) { skipAllIn(scope); return; }
    // --randomize: shuffle this scope's entries before running them. bun does it
    // at order-generation time (Order.rs:94-96); doing it on entry to the scope
    // yields the same visited order because generation walks scopes in the same
    // depth-first sequence this runner does.
    if (S.rand !== null) shuffleWithIndex(S.rand, scope.items);
    try { for (const h of scope.beforeAll) await callHook(h); }
    catch (e) {
      failAllIn(scope, (e && e.message !== undefined) ? String(e.message) : String(e));
      return;
    }
    const be = beChain.concat(scope.beforeEach);
    const ae = scope.afterEach.concat(aeChain);
    for (const item of scope.items) {
      if (item.type === "test") await runTest(scope, item, be, ae);
      else await runScope(item.scope, be, ae);
    }
    try { for (const h of scope.afterAll) await callHook(h); }
    catch (e) { S.errors.push((e && e.message !== undefined) ? String(e.message) : String(e)); }
  }
  async function runTest(scope, t, be, ae) {
    const label = fullName(scope, t.name);
    // -t / --test-name-pattern / --grep: bun matches the compiled RegExp against
    // the full "describe > … > test" name (partial, unanchored). A non-match is
    // "skipped because label" — it runs NOTHING and counts as neither pass/skip/
    // todo/fail nor toward `total`, so did_label_filter_out_all_tests() can tell a
    // label-only run apart from a real one (ref: jest.rs:282).
    if (G.__mbunNamePattern && !G.__mbunNamePattern.test(label)) {
      S.skippedLabel++;
      return;
    }
    S.total++;
    // --todo: todo tests RUN — a failing one stays todo, a passing one FAILS
    // (bun: "marked as todo but passes").
    const runTodo = t.mode === "todo" && G.__mbunRunTodo && typeof t.fn === "function";
    if (t.mode === "skip" || (t.mode === "todo" && !runTodo)) {
      if (t.mode === "todo") { S.todo++; S.out.push("(todo) " + label); }
      else { S.skip++; S.out.push("(skip) " + label); }
      return;
    }
    // bun prints thrown (non-assertion) errors as "error: <message>"; assertion
    // failures print the expect() message directly.
    const errMsg = (e) => ((e && e.name === "AssertionError") ? "" : "error: ") +
                          ((e && e.message !== undefined) ? String(e.message) : String(e));
    let failed = false, msg = "";
    S.asyncErr = undefined;
    S.pendingAsserts = [];
    try {
      for (const h of be) await callHook(h);
      if (typeof t.fn !== "function") throw new Error("test body is not a function");
      let bodyPromise;
      if (t.fn.length >= 1) {
        // done-callback style: test('x', (done) => { …; done(); }). done() may fire
        // synchronously, via a microtask, or (unsupported here) a timer. Since there
        // is no host timer, drain a few microtask ticks; if done() has not fired and
        // the body returned no promise, treat the body as complete — otherwise an
        // arity-1 test that never calls done would hang the whole file.
        bodyPromise = new Promise((resolve, reject) => {
          let settled = false;
          const done = (err) => { if (settled) return; settled = true; if (err) reject(err instanceof Error ? err : new Error(String(err))); else resolve(); };
          let r; try { r = t.fn(done); } catch (e) { done(e); return; }
          if (r && typeof r.then === "function") { r.then(() => done(), (e) => done(e)); return; }
          // Body returned synchronously without a promise. Drain a few microtasks;
          // if done() still hasn't fired AND no timer is pending (which could call
          // done via the runner's timer pump), treat it as complete (arity-1 arg
          // that isn't a done callback). If timers ARE pending, wait for them.
          (async () => { for (let i = 0; i < 8 && !settled; i++) await Promise.resolve(); if (!settled && (!G.__mbunTimers || G.__mbunTimers.q.length === 0)) { settled = true; resolve(); } })();
        });
      } else {
        bodyPromise = Promise.resolve().then(() => { const r = t.fn(); return (r && typeof r.then === "function") ? r : undefined; });
      }
      // Per-test timeout: race the body against a promise the C++ pump can reject
      // when the run is stuck (no pending timers, no progress). A hung test then
      // fails on its own — matching bun's default per-test timeout — instead of
      // blocking the whole file's reporting. An EXPLICIT timeout (test(name, fn,
      // 10) / { timeout: 10 }) additionally arms a (virtual-time) timer so a body
      // that keeps scheduling timers still times out in timer order, like bun.
      // Real-time timers: the timeout timer fires at genuine wall-clock tmo,
      // so it rejects unconditionally (bun kills a test at its timeout no
      // matter what it awaits). Default 5000ms = bun's per-test default; the
      // stall detector stays as the fallback for the timer-less window.
      let tmo = 5000;
      if (t.opts && typeof t.opts.timeout === "number") tmo = t.opts.timeout;
      let timeoutTimer = null;
      const timeoutPromise = new Promise((_, reject) => {
        S.timeoutReject = reject;
        if (tmo > 0 && typeof natSetTimeout === "function") {
          timeoutTimer = natSetTimeout(() => {
            reject(new Error("timed out after " + tmo + "ms"));
          }, tmo);
          if (timeoutTimer && typeof timeoutTimer.unref === "function") timeoutTimer.unref();
        }
      });
      // Deferred async toThrow checks (expect(asyncFn).toThrow()) are awaited
      // INSIDE the timeout race: a pending assert whose inner promise never
      // settles (e.g. an abort listener that never fires) must time this test
      // out individually, not wedge the whole file's reporting.
      const bodyAndAsserts = (async () => {
        await bodyPromise;
        while (S.pendingAsserts.length) { const ps = S.pendingAsserts; S.pendingAsserts = []; await Promise.all(ps); }
      })();
      try { await Promise.race([bodyAndAsserts, timeoutPromise]); }
      finally {
        S.timeoutReject = null;
        S.pendingAsserts = [];  // drop asserts left dangling by a timeout
        if (timeoutTimer !== null && typeof natClearTimeout === "function") natClearTimeout(timeoutTimer);
        bodyAndAsserts.catch(function () {});  // timed-out body settling later must not surface as unhandled
      }
      // an exception from a queueMicrotask/nextTick callback while this test was
      // in flight fails the test (bun: async exceptions are attributed to it).
      if (S.asyncErr !== undefined) { const ae2 = S.asyncErr; S.asyncErr = undefined; throw ae2; }
    } catch (e) { failed = true; msg = errMsg(e); }
    try { for (const h of ae) await callHook(h); }
    catch (e) { if (!failed) { failed = true; msg = errMsg(e); } }
    if (t.mode === "todo") {  // running under --todo
      if (failed) {
        S.todo++; S.out.push("(todo) " + label);
        S.out.push(msg.split("\n").map(function (l) { return "      " + l; }).join("\n"));
      } else {
        S.fail++; S.out.push("(fail) " + label);
        S.out.push("      error: this test is marked as todo but passes. Remove `.todo` if this test should be run.");
      }
      return;
    }
    if (t.mode === "failing") {  // expected-failure test: invert
      if (failed) { S.pass++; S.out.push("(pass) " + label + " (failing)"); }
      else { S.fail++; S.out.push("(fail) " + label + " — expected to fail but passed"); }
      return;
    }
    if (failed) {
      // bun prints the error/assertion detail as the test throws, THEN the "(fail)"
      // status line once the test settles — so the detail comes first, flush-left
      // (test_command.rs). Thrown errors already carry the "error: " prefix from
      // errMsg above, which is what harness filters key off.
      // A per-test timeout is rendered bun-style: the "(fail)" line first, then a
      // caret annotation, with no flush-left "error:" line (issue 23865).
      const toM = /^error: timed out after (\d+)ms$/.exec(msg);
      if (toM) {
        S.fail++; S.out.push("(fail) " + label);
        S.out.push("  ^ this test timed out after " + toM[1] + "ms.");
      } else {
        S.fail++; S.out.push(msg); S.out.push("(fail) " + label);
      }
    } else { S.pass++; S.out.push("(pass) " + label); }
  }

  // Per-file reset — the file boundary of the singleton runner (bun switches
  // `current_file`; we clear the collection tree instead). Every field is reset
  // IN PLACE: `S` and `__allMocks` keep their object identity so the closures
  // captured by `it`/`describe`/`expect`/the globalThis mirror/cached helper
  // modules all keep writing into the live state. Mirrors the fresh-IIFE state
  // that a re-eval used to produce, so per-file semantics are unchanged.
  G.__mbun_reset = function () {
    const fresh = makeScope(null, null);
    S.root = fresh; S.current = fresh;
    S.pass = 0; S.fail = 0; S.skip = 0; S.expectCalls = 0; S.total = 0; S.todo = 0;
    S.skippedLabel = 0;
    S.out = []; S.customMatchers = {}; S.errors = []; S.pendingAsserts = [];
    S.timeoutReject = null; S.asyncErr = undefined; S.rand = null; S.skipDepth = 0;
    S.sysTime = null;  // a file's fake system time must not leak into the next
    ftUninstall();     // a file's fake timers must not leak into the next either
    __allMocks.length = 0;
  };

  G.__mbun_run = async function () {
    G.__mbun_done = false;
    // Arm the shuffle PRNG once per file, from the per-file seed the native side
    // derived (see run_source). Null/absent == --randomize off → insertion order.
    S.rand = (G.__mbunTestSeed === null || G.__mbunTestSeed === undefined)
             ? null : makePrng(G.__mbunTestSeed);
    try { await runScope(S.root, [], []); }
    catch (e) { S.fail++; S.out.push("(fail) <scope error> " + ((e && e.message) || e)); }
    G.__mbun_pass = S.pass; G.__mbun_fail = S.fail; G.__mbun_skip = S.skip;
    G.__mbun_todo = S.todo; G.__mbun_skipped_label = S.skippedLabel;
    G.__mbun_expect = S.expectCalls; G.__mbun_total = S.total;
    G.__mbun_errors = S.errors.length;
    G.__mbun_error_text = S.errors.map((m) => "error: " + m).join("\n");
    G.__mbun_output = S.out.join("\n"); G.__mbun_done = true;
  };

  if (typeof G.console === "undefined") {           // no-op console (real capture DEFERRED)
    G.console = { log() {}, error() {}, warn() {}, info() {}, debug() {}, trace() {} };
  }
})();
0;
)JS";

std::string_view trim_view(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Rewrite a single-line `import … from "spec"` (or `import "spec"`) declaration.
// For spec == "bun:test": bind the named clause off globalThis.__mbunBT (the
// injected exports object). For any other specifier: erase it (the names become
// undefined and fail honestly at first use, rather than a whole-file syntax
// error from a top-level `import` in JSC script mode). Multi-line import clauses
// are DEFERRED — such a line is erased.
//
// Bindings use `var` (not `const`): the process-level JSC context keeps its
// global lexical environment across JSEvaluateScript calls, so a `const` would
// throw "already declared" when the runner evaluates a second file in the same
// process. `var` is redeclaration-safe and models the ESM import as a global.
std::string rewrite_import(std::string_view line) {
    const std::size_t q1{line.find_first_of("\"'", 0)};
    if (q1 == std::string_view::npos) return {};  // malformed; drop
    const char quote{line[q1]};
    const std::size_t q2{line.find(quote, q1 + 1)};
    if (q2 == std::string_view::npos) return {};
    const std::string_view spec{line.substr(q1 + 1, q2 - q1 - 1)};
    if (spec != "bun:test") return {};  // unsupported specifier → strip

    const std::size_t importPos{line.find("import")};
    const std::size_t fromPos{line.rfind(" from ")};
    if (fromPos == std::string_view::npos) return {};  // side-effect import: nothing to bind
    std::string_view clause{trim_view(line.substr(importPos + 6, fromPos - (importPos + 6)))};

    const std::size_t brace1{clause.find('{')};
    if (brace1 != std::string_view::npos) {
        const std::size_t brace2{clause.rfind('}')};
        std::string inner{clause.substr(brace1 + 1, brace2 - brace1 - 1)};
        // `a as b` → `a: b` (import alias → destructuring rename).
        std::string dest;
        std::size_t i{0};
        while (i < inner.size()) {
            const std::size_t as{inner.find(" as ", i)};
            if (as == std::string::npos) { dest.append(inner, i, inner.size() - i); break; }
            dest.append(inner, i, as - i);
            dest.append(":");
            i = as + 3;  // skip " as", keep the following space before the local name
        }
        return "var {" + dest + "} = globalThis.__mbunBT;";
    }
    const std::size_t star{clause.find('*')};
    if (star != std::string_view::npos) {
        const std::size_t as{clause.find(" as ")};
        if (as != std::string_view::npos) {
            std::string_view ns{trim_view(clause.substr(as + 4))};
            return "var " + std::string{ns} + " = globalThis.__mbunBT;";
        }
        return {};
    }
    // Bare default import `import Foo from "bun:test"` — bun:test has no default.
    return "var " + std::string{trim_view(clause)} + " = globalThis.__mbunBT.default;";
}

// A real (OS) filesystem for the module loader — the unit tests use in-memory
// fs, but `mbun test <file>` reads the actual file tree.
mbun::resolver::FileSystem os_fs() {
    return mbun::resolver::FileSystem{
        .file_exists = [](std::string_view p) {
            std::error_code ec;
            return std::filesystem::is_regular_file(std::filesystem::path{p}, ec);
        },
        .dir_exists = [](std::string_view p) {
            std::error_code ec;
            return std::filesystem::is_directory(std::filesystem::path{p}, ec);
        },
        .read_file = [](std::string_view p) -> std::optional<std::string> {
            std::ifstream f{std::filesystem::path{p}, std::ios::binary};
            if (!f) return std::nullopt;
            std::ostringstream ss;
            ss << f.rdbuf();
            return ss.str();
        },
        // node preserveSymlinks=false: bare-specifier walk-up starts at the
        // importer's canonical dir (isolated-linker layouts need this).
        .real_path = [](std::string_view p) -> std::optional<std::string> {
            std::error_code ec;
            auto c{std::filesystem::canonical(std::filesystem::path{p}, ec)};
            if (ec) return std::nullopt;
            return c.string();
        },
    };
}

// The CLI layer forwards only the first positional argument to the runner, so
// runner flags (--todo, …) are recovered from the process command line. Linux
// exposes it via /proc/self/cmdline (NUL-separated); on platforms without procfs
// this gracefully reports "flag absent" until the CLI forwards flags (T4.2).
bool has_cli_flag(std::string_view flag) {
    std::ifstream f{"/proc/self/cmdline", std::ios::binary};
    if (!f) return false;
    std::string all{std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
    std::size_t pos{0};
    while (pos < all.size()) {
        std::size_t z{all.find('\0', pos)};
        if (z == std::string::npos) z = all.size();
        if (std::string_view{all}.substr(pos, z - pos) == flag) return true;
        pos = z + 1;
    }
    return false;
}

// Read a whole file, or nullopt on failure.
std::optional<std::string> read_all(const std::filesystem::path& p) {
    std::ifstream f{p, std::ios::binary};
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace mbun::jsc::test_runner::detail

export namespace mbun::jsc::test_runner {

// Outcome of running one test file: the pass/fail/skip tally + expect() call
// count bun reports, the per-test detail body, and load/eval error surfacing.
struct RunResult {
    int pass{0};
    int fail{0};
    int skip{0};
    int todo{0};
    int errors{0};       // file-level errors (describe-scope throws, afterAll throws, …)
    int expect_calls{0};
    int total{0};
    int skipped_label{0};  // tests skipped by -t/--test-name-pattern (jest.rs:282)
    std::string body;    // per-test (pass)/(fail)/(skip) lines + failure detail
    bool ok{false};      // the file loaded and the run completed (regardless of pass/fail)
    std::string error;   // load/transpile/eval/incomplete-run diagnostic (ok == false)
    std::string error_text;  // unhandled between-tests error detail (paired with errors > 0)
};

// Turn an ESM test file into JS evaluable as a JSC script: rewrite/strip import
// declarations and drop the `export ` keyword from `export const/let/var/
// function/class` (whole-file ESM linking is DEFERRED; the trailing binding just
// becomes a normal declaration). Line-oriented: multi-line import/export clauses
// are DEFERRED. `import.meta` / dynamic `import(...)` are left untouched.
std::string prepare_source(std::string_view js) {
    std::string out;
    out.reserve(js.size());
    std::size_t pos{0};
    const std::size_t n{js.size()};
    while (pos < n) {
        std::size_t eol{js.find('\n', pos)};
        if (eol == std::string_view::npos) eol = n;
        const std::string_view raw{js.substr(pos, eol - pos)};
        const std::string_view t{detail::trim_view(raw)};

        if (t.starts_with("import ") || t.starts_with("import{") || t.starts_with("import\"")
            || t.starts_with("import'")) {
            const std::string_view after{detail::trim_view(t.substr(6))};
            if (!after.starts_with(".") && !after.starts_with("(")) {  // not import.meta / import(
                out.append(detail::rewrite_import(t));
                if (eol < n) out.push_back('\n');
                pos = eol + 1;
                continue;
            }
        }
        if (t.starts_with("export ")) {
            const std::string_view rest{detail::trim_view(t.substr(7))};
            if (rest.starts_with("const ") || rest.starts_with("let ") || rest.starts_with("var ")
                || rest.starts_with("function") || rest.starts_with("class ")
                || rest.starts_with("async ")) {
                out.append(rest);  // drop `export ` keyword; keep the declaration
                if (eol < n) out.push_back('\n');
                pos = eol + 1;
                continue;
            }
            // `export default …` / `export { … }` / `export * …` → DEFERRED, drop line.
            if (eol < n) out.push_back('\n');
            pos = eol + 1;
            continue;
        }
        out.append(raw);
        if (eol < n) out.push_back('\n');
        pos = eol + 1;
    }
    return out;
}

// Run a prepared/inline bun:test source in the shared runtime context. `dir` is
// the test file's directory (for relative require()); inline sources pass ".".
// `test_seed`, when set, is this file's --randomize shuffle seed (see run_file).
RunResult run_source(std::string_view js_source, std::string_view dir = ".", bool jsx = false,
                     std::string_view filename = "",
                     std::optional<std::uint64_t> test_seed = std::nullopt,
                     std::optional<std::string_view> name_pattern = std::nullopt) {
    namespace rt = mbun::jsc::runtime;
    RunResult r;

    // 1. install/reset the harness (defines globalThis.__mbunBT that
    //    `require("bun:test")` resolves to). Idempotent: the runner is installed
    //    once per process and this resets its collection state for this file —
    //    re-minting it would strand tests registered through bindings captured by
    //    the previous file (bare-global `it`, cached helper modules).
    if (auto h{rt::eval(detail::HARNESS)}; !h) {
        r.error = "bun:test harness init failed: " + h.error();
        return r;
    }
    // 1b. runner flags: --todo makes todo tests run (pass → fail, fail → todo).
    rt::eval(detail::has_cli_flag("--todo") ? "globalThis.__mbunRunTodo=true;"
                                            : "globalThis.__mbunRunTodo=false;");
    // 1c. --randomize/--seed: hand the harness this file's shuffle seed as a
    //     decimal string (u64 exceeds JS number precision; the harness BigInt's it).
    rt::eval(test_seed ? std::format("globalThis.__mbunTestSeed=\"{}\";", *test_seed)
                       : std::string{"globalThis.__mbunTestSeed=null;"});
    // 1d. -t/--test-name-pattern/--grep: compile the label filter to a JS RegExp
    //     (partial, unanchored — bun's semantics; ref test_command.rs uses the
    //     RegExp against the concatenated describe+test name). A malformed pattern
    //     leaves the filter off (no tests are dropped) rather than aborting.
    if (name_pattern) {
        std::string lit;
        lit.reserve(name_pattern->size() + 2);
        lit.push_back('"');
        for (const char c : *name_pattern) {
            switch (c) {
                case '"':  lit += "\\\""; break;
                case '\\': lit += "\\\\"; break;
                case '\n': lit += "\\n";  break;
                case '\r': lit += "\\r";  break;
                case '\t': lit += "\\t";  break;
                default:   lit.push_back(c); break;
            }
        }
        lit.push_back('"');
        rt::eval(std::format(
            "try{{globalThis.__mbunNamePattern=new RegExp({});}}"
            "catch(e){{globalThis.__mbunNamePattern=null;}}", lit));
    } else {
        rt::eval("globalThis.__mbunNamePattern=null;");
    }
    // 2. CommonJS environment: require bound to `dir` (→ bun:test + local helpers),
    //    plus module/exports/__dirname so a CJS-lowered test file links + exports.
    const std::string cjs_file{filename.empty() ? std::string{dir} + "/<test>" : std::string{filename}};
    if (auto e{rt::install_cjs_env(std::string{dir}, cjs_file)}; !e) {
        r.error = "bun:test env init failed: " + e.error();
        return r;
    }
    // bun auto-loads .env files (test mode: mode=test, .env.local skipped).
    rt::apply_dotenv(/*isTest=*/true);
    // 3. collection: evaluate the test source; test bodies do not run yet, so a
    //    failure here is a genuine top-level/syntax error. Lower any remaining ESM
    //    to CJS with the AST-aware transpiler (template-safe, unlike the old
    //    line-oriented prepare_source); already-CJS input passes through unchanged.
    // The test file itself is a module: a .tsx test on the automatic runtime
    // needs its jsx-runtime import injected exactly like an imported module.
    mbun::js_parser::TranspileResult t{mbun::js_parser::transpile(
        js_source, {.cjs = true,
                    .jsx = jsx,
                    .jsx_options = mbun::jsc::module_loader::runtime_jsx_options(),
                    // A test file is transpiled by the same runtime transpiler as
                    // any other module, so it elides its unused TS imports too
                    // (transpiler.rs:1606-1609). Keyed off the file's own loader:
                    // an inline/anonymous source has no extension, hence no trim.
                    .trim_unused_imports = mbun::jsc::module_loader::trims_unused_imports(
                        mbun::jsc::module_loader::loader_for_path(filename))})};
    std::string prepared{t.ok ? std::move(t.code) : std::string{js_source}};
    // An ESM test file's `import`s were just lowered to require(): give it the
    // require that resolves with the "import" condition, so an ESM-only dependency
    // ({"exports":{"import":…}} with no "require" branch) resolves at all.
    if (t.ok && t.cjs_esm_module) {
        (void)rt::use_esm_require(std::string{dir});
    }
    // JSC's C-API context has no host module loader, so dynamic `import(spec)` is
    // rewritten to the __mbun_dynimport shim (resolves via the CJS require chain).
    // Match the `import` keyword only when it is a call — followed by `(` (with
    // optional spaces) and preceded by a non-identifier, non-`.` char (so
    // `import.meta`, `foo.import(`, and identifiers ending in "import" are left
    // alone). Static `import …` is already lowered to require by the transpiler.
    {
        std::string rewritten;
        rewritten.reserve(prepared.size() + 64);
        std::size_t i{0};
        const std::size_t n{prepared.size()};
        const auto isIdent{[](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '$';
        }};
        while (i < n) {
            const char c{prepared[i]};
            // Opaque spans copied verbatim so `import(` inside a string /
            // template / comment is never rewritten — the scan is textual, not
            // AST-based, so without this it corrupted string literals AND
            // toMatchInlineSnapshot argument text (regression 24387). bun lowers
            // import() in the parser, where these tokens are opaque.
            if (c == '"' || c == '\'') {
                rewritten.push_back(c);
                ++i;
                while (i < n) {
                    rewritten.push_back(prepared[i]);
                    if (prepared[i] == '\\' && i + 1 < n) { rewritten.push_back(prepared[i + 1]); i += 2; continue; }
                    if (prepared[i] == c) { ++i; break; }
                    ++i;
                }
                continue;
            }
            if (c == '`') {
                // Whole template span is opaque (a real import( inside ${…} is
                // vanishingly rare in test files; keeping it verbatim matches
                // bun's snapshot text).
                rewritten.push_back(c);
                ++i;
                while (i < n) {
                    rewritten.push_back(prepared[i]);
                    if (prepared[i] == '\\' && i + 1 < n) { rewritten.push_back(prepared[i + 1]); i += 2; continue; }
                    if (prepared[i] == '`') { ++i; break; }
                    ++i;
                }
                continue;
            }
            if (c == '/' && i + 1 < n && prepared[i + 1] == '/') {
                while (i < n && prepared[i] != '\n') { rewritten.push_back(prepared[i]); ++i; }
                continue;
            }
            if (c == '/' && i + 1 < n && prepared[i + 1] == '*') {
                rewritten.append("/*");
                i += 2;
                while (i < n) {
                    if (prepared[i] == '*' && i + 1 < n && prepared[i + 1] == '/') { rewritten.append("*/"); i += 2; break; }
                    rewritten.push_back(prepared[i]);
                    ++i;
                }
                continue;
            }
            if (prepared.compare(i, 6, "import") == 0) {
                const char prev{i == 0 ? ' ' : prepared[i - 1]};
                std::size_t j{i + 6};
                while (j < n && (prepared[j] == ' ' || prepared[j] == '\t')) ++j;
                if (!isIdent(prev) && prev != '.' && j < n && prepared[j] == '(') {
                    rewritten += "__mbun_dynimport(";
                    i = j + 1;
                    continue;
                }
            }
            rewritten.push_back(c);
            ++i;
        }
        prepared = std::move(rewritten);
    }
    // Wrap in the node module function so the file's top-level const/let stay in a
    // fresh function scope — the process-level JSC global lexical environment
    // persists across evals, so a bare top-level `const` would collide on the next
    // file/run. exports/require/module come from the injected globals.
    // If the file itself declares const/let/var __filename|__dirname (common in
    // ESM-style sources using fileURLToPath(import.meta.url)), rename the wrapper
    // param so it doesn't collide ("Cannot declare a const variable twice").
    const auto declares{[&](std::string_view name) {
        for (std::string_view kw : {"const ", "let ", "var "}) {
            std::size_t p{0};
            const std::string needle{std::string{kw} + std::string{name}};
            while ((p = prepared.find(needle, p)) != std::string::npos) {
                const std::size_t after{p + needle.size()};
                const char c{after < prepared.size() ? prepared[after] : ' '};
                if (c == ' ' || c == '=' || c == ':' || c == ';' || c == ',' || c == '\n') return true;
                p = after;
            }
        }
        return false;
    }};
    const char* pfn{declares("__filename") ? "__mbun_pfilename" : "__filename"};
    const char* pdn{declares("__dirname") ? "__mbun_pdirname" : "__dirname"};
    const std::string callArgs{
        "globalThis.exports, globalThis.require, globalThis.module, "
        "globalThis.__filename, globalThis.__dirname"};
    const std::string params{std::string{"exports, require, module, "} + pfn + ", " + pdn};
    const std::string strictPrefix{t.cjs_esm_module ? "\"use strict\";\n" : ""};
    std::string wrapped{"(function (" + params + ") {\n" + strictPrefix + prepared +
                        "\n}).call(globalThis, " + callArgs + ");"};
    if (auto reg{rt::eval(wrapped, "<test>")}; !reg) {
        // Possibly a top-level-await test file (JSC script mode has no TLA): retry
        // with an async wrapper and pump the virtual event loop so module-scope
        // awaits (and the test() collection after them) finish before execution.
        // ReferenceError "await is not defined" = top-level `await (expr)` parsed
        // as an identifier call in script goal — same TLA situation as the
        // SyntaxError form, so it takes the same async-wrapper retry.
        if (reg.error().find("SyntaxError") == std::string::npos &&
            reg.error().find("await is not defined") == std::string::npos &&
            reg.error().find("Can't find variable: await") == std::string::npos) {
            r.error = "test file evaluation error: " + reg.error();
            return r;
        }
        (void)rt::eval("globalThis.__mbun_collect_done=0;globalThis.__mbun_collect_err=undefined;");
        const std::string awrapped{
            "(async function (" + params + ") {\n" + strictPrefix + prepared +
            "\n}).call(globalThis, " + callArgs + ")"
            ".then(function(){globalThis.__mbun_collect_done=1;},"
            "function(e){globalThis.__mbun_collect_err=(e&&e.stack)||String(e);globalThis.__mbun_collect_done=1;});"};
        if (auto reg2{rt::eval(awrapped, "<test>")}; !reg2) {
            r.error = "test file evaluation error: " + reg.error();  // report the original
            return r;
        }
        rt::pump_event_loop("globalThis.__mbun_collect_done");
        if (auto err{rt::eval_to_string(
                "globalThis.__mbun_collect_err===undefined?'':String(globalThis.__mbun_collect_err)")};
            err && !err->empty()) {
            r.error = "test file evaluation error: " + *err;
            return r;
        }
    }
    // 4. execution: start the async runner, then pump the virtual-time timer queue
    //    (setTimeout/setInterval) interleaved with JSC's end-of-script microtask
    //    drain until the run completes or no schedulable timer remains. This is the
    //    manual event loop that lets setTimeout-driven async tests finish.
    rt::eval("if (globalThis.__mbun_timers_reset) globalThis.__mbun_timers_reset();");
    if (auto run{rt::eval("__mbun_run(); 0")}; !run) {
        r.error = "test run error: " + run.error();
        return r;
    }
    bool done{false};
    double lastProgress{-1.0};
    int stuckCount{0};
    for (int iter = 0; iter < 200000; ++iter) {
        if (auto d{rt::eval_number("globalThis.__mbun_done ? 1 : 0")}; d && *d == 1.0) {
            done = true;
            break;
        }
        // Fire due timers ONE at a time — each eval's end drains the resulting
        // microtasks, so promise chains resolved by an earlier timer settle before
        // the next (virtually later) timer fires. Draining a whole batch in one
        // eval would fire a later timeout timer before the microtask that clears
        // it can run (breaking test-timeout races). Cap the per-iteration count so
        // one pump turn still makes decent progress through busy timer queues.
        for (int k = 0; k < 32; ++k) {
            auto left{rt::eval_number(
                "globalThis.__mbun_drain_timers ? __mbun_drain_timers(1) : 0")};
            if (!left || *left <= 0.0) break;
        }
        rt::eval("globalThis.__mbunNetDrain && globalThis.__mbunNetDrain();");
        rt::eval("0");  // extra bare eval → drains the JSC microtask queue again
        // Pending timers OR in-flight network operations (a fetch awaiting its
        // response, a server mid-exchange) both count as schedulable work; the JS
        // net reactor self-bounds via a stall counter so it cannot pin the pump.
        // Also drive async child_process/Bun.spawn: __mbun_io_tick polls live pipe
        // fds, emits data/exit through the pump, and returns the count of still-
        // active children (>0 keeps the loop alive, like pending timers).
        auto ioBusy{rt::eval_number("globalThis.__mbun_io_tick ? __mbun_io_tick() : 0")};
        rt::eval("0");
        auto remaining{rt::eval_number(
            "(globalThis.__mbun_timers_refd ? globalThis.__mbun_timers_refd() : 0) + "
            "(globalThis.__mbunNet ? globalThis.__mbunNet.pending : 0)")};
        if (auto d{rt::eval_number("globalThis.__mbun_done ? 1 : 0")}; d && *d == 1.0) {
            done = true;
            break;
        }
        // Real-time timers: nothing due yet and no child IO → park until the
        // earliest deadline (bounded) instead of busy-spinning to it. Park in
        // poll() over the live net fds (NN.wait) so an fd event cuts a long
        // park short — idle parks can now be seconds, and a blind sleep here
        // would sit through e.g. the connection a test is awaiting.
        if (!ioBusy || *ioBusy == 0.0) {
            if (auto idleMs{rt::eval_number(
                    "globalThis.__mbun_pump_idle_ms ? __mbun_pump_idle_ms() : 0")};
                idleMs && *idleMs >= 1.0) {
                // Test pumps are completion-gated like the engine's done-flag
                // pumps: cap the park so per-test timeout bookkeeping and the
                // idle bail-out keep their pace (fd wakes still cut it short).
                int parkMs{static_cast<int>(*idleMs)};
                if (parkMs > 25) parkMs = 25;
                const auto parked{rt::eval_number(
                    "globalThis.__mbunNetNative&&globalThis.__mbunNetNative.wait?"
                    "(__mbunNetNative.wait(" +
                    std::to_string(parkMs) + "),1):0")};
                if (!parked || *parked == 0.0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{parkMs});
                }
            }
        }
        if ((remaining && *remaining > 0.0) || (ioBusy && *ioBusy > 0.0)) {
            stuckCount = 0;
            continue;  // timers pending or a child still running → keep pumping
        }
        // No timers pending, but the run may still be advancing through a chain of
        // awaited promises that resolve via microtasks (not timers). Keep draining
        // microtasks as long as the pass/fail/total tally is still moving; only
        // declare the run stuck after it stops progressing for many drains.
        auto prog{rt::eval_number(
            "(globalThis.__mbun_total||0)+(globalThis.__mbun_pass||0)+(globalThis.__mbun_fail||0)")};
        const double pv{prog ? *prog : 0.0};
        if (pv != lastProgress) {
            lastProgress = pv;
            stuckCount = 0;
            continue;  // progressed since last drain → give microtasks more turns
        }
        if (++stuckCount > 128) {
            // No timers and no progress for many microtask drains. If a test is
            // currently in flight, time IT out (fails that one test, run resumes);
            // otherwise the run is genuinely wedged between tests → give up.
            auto timedOut{rt::eval_number("globalThis.__mbun_timeout_current() ? 1 : 0")};
            if (timedOut && *timedOut == 1.0) {
                stuckCount = 0;
                lastProgress = -1.0;
                continue;
            }
            break;
        }
    }
    // 4b. Reap/kill any child_process/Bun.spawn children the test left running
    //     (e.g. sleep processes deliberately not killed). Like bun, a finished
    //     test run must not leak subprocesses — leftover children that inherited
    //     our stdio would otherwise hold the output pipe open past our exit.
    rt::eval(
        "if (globalThis.__mbunChildren && globalThis.__mbunProcNative) { "
        "for (const rec of globalThis.__mbunChildren) { try { globalThis.__mbunProcNative.kill(rec.pid, 9); } catch (e) {} try { globalThis.__mbunProcNative.wait(rec.pid, false); } catch (e) {} } "
        "globalThis.__mbunChildren.clear(); }");
    // 5. confirm the async runner actually finished (a genuinely stuck run — pending
    //    non-timer async — is reported honestly rather than faked complete).
    if (!done) {
        // Salvage: if some tests already completed before the run wedged,
        // publish their tallies from the live state and mark the file failing
        // with a synthetic entry, instead of discarding every result (bun
        // reports completed tests even when a later one hangs the file).
        const auto ran{rt::eval_number(
            "(globalThis.__mbunState && __mbunState.total) ? __mbunState.total : 0")};
        if (!ran || *ran <= 0.0) {
            r.error = "test run did not complete (pending non-timer async / unresolved promise)";
            return r;
        }
        rt::eval(
            // (the wedged test's total was already counted at its start — only fail++)
            "(function(){ const S = globalThis.__mbunState; S.fail++;"
            "S.out.push('(fail) <run wedged> pending non-timer async / unresolved promise;"
            " remaining tests in this file were NOT run');"
            "globalThis.__mbun_pass = S.pass; globalThis.__mbun_fail = S.fail;"
            "globalThis.__mbun_skip = S.skip; globalThis.__mbun_todo = S.todo;"
            "globalThis.__mbun_skipped_label = S.skippedLabel;"
            "globalThis.__mbun_expect = S.expectCalls; globalThis.__mbun_total = S.total;"
            "globalThis.__mbun_errors = S.errors.length;"
            "globalThis.__mbun_error_text = S.errors.map(function(m){return 'error: '+m;}).join('\\n');"
            "globalThis.__mbun_output = S.out.join('\\n'); })();");
    }
    // 6. read the tallies + per-test body back out.
    const auto as_int{[](std::expected<double, std::string> v) {
        return v ? static_cast<int>(*v) : 0;
    }};
    r.pass = as_int(rt::eval_number("globalThis.__mbun_pass"));
    r.fail = as_int(rt::eval_number("globalThis.__mbun_fail"));
    r.skip = as_int(rt::eval_number("globalThis.__mbun_skip"));
    r.todo = as_int(rt::eval_number("globalThis.__mbun_todo||0"));
    r.errors = as_int(rt::eval_number("globalThis.__mbun_errors||0"));
    r.expect_calls = as_int(rt::eval_number("globalThis.__mbun_expect"));
    r.total = as_int(rt::eval_number("globalThis.__mbun_total"));
    r.skipped_label = as_int(rt::eval_number("globalThis.__mbun_skipped_label||0"));
    if (auto body{rt::eval_to_string("String(globalThis.__mbun_output)")}) {
        r.body = std::move(*body);
    }
    // The report is printed by the caller (app/cli run_test), which owns the bun
    // output shape: "<path>:" → body → blank line → counts → "Ran N tests".
    // run_file() only collects the numbers + body into RunResult; printing here as
    // well would emit the whole report twice.
    if (auto et{rt::eval_to_string("String(globalThis.__mbun_error_text||'')")}) {
        r.error_text = std::move(*et);
    }
    r.ok = true;
    return r;
}

// Load a test file from disk (module_loader: resolve → read → TS→JS transpile),
// then run it. On a transpile failure we fall back to the raw file contents
// (plain-JS `.test.ts`/`.test.js` files evaluate directly), so a limitation in
// the S0 transpiler subset does not block an otherwise pure-JS bun test file.

// Derive this file's shuffle seed from (run seed, file basename) the way bun does:
// `DefaultPrng::init(wyhash(basename(path)) +% seed)` — so a file's test order
// depends only on its name and the printed --seed, not on which worker ran it or
// what ran before it (ref: bun-ref/src/runtime/test_runner/bun_test.rs:1041-1053).
// mbun has no wyhash port, so the basename hash is FNV-1a: same contract (stable,
// name-only), different constant. bun's exact permutation is not reproduced —
// only the "same seed ⇒ same order" property the flag promises.
std::uint64_t per_file_seed(std::string_view basename, std::uint32_t seed) {
    std::uint64_t h{0xcbf29ce484222325ULL};
    for (const unsigned char c : basename) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h + seed;  // wrapping_add
}

RunResult run_file(std::string_view path, std::optional<std::uint32_t> seed = std::nullopt,
                   std::optional<std::string_view> name_pattern = std::nullopt) {
    RunResult r;
    const std::filesystem::path abs{std::filesystem::absolute(std::filesystem::path{path})};
    const std::string dir{abs.parent_path().string()};
    const std::string base{"./" + abs.filename().string()};
    // bun enables JSX in .js/.jsx/.tsx/.mjs/.cjs; .ts/.mts/.cts keep `<T>` casts
    // (no JSX). run_source's ESM→CJS transpile does the JSX transform for these.
    const std::string ps{abs.string()};
    const bool jsx{ps.ends_with(".jsx") || ps.ends_with(".tsx") || ps.ends_with(".js") ||
                   ps.ends_with(".mjs") || ps.ends_with(".cjs")};

    // cjs=false: keep ESM here (type erasure only) — run_source does the single
    // ESM→CJS lowering pass (double-transpiling can corrupt template literals).
    mbun::jsc::module_loader::ModuleLoader loader{detail::os_fs(), mbun::resolver::Options{},
                                                  /*cjs=*/false};
    const mbun::jsc::module_loader::LoadResult loaded{loader.load(base, dir)};

    const std::optional<std::uint64_t> fileSeed{
        seed ? std::optional{per_file_seed(abs.filename().string(), *seed)} : std::nullopt};

    using mbun::jsc::module_loader::LoadStatus;
    if (loaded.status == LoadStatus::Success) {
        return run_source(loaded.source, dir, jsx, ps, fileSeed, name_pattern);
    }
    if (loaded.status == LoadStatus::TranspileFailed) {
        if (auto raw{detail::read_all(abs)}) {  // plain-JS-in-.ts needs no transpile
            RunResult fallback{run_source(*raw, dir, jsx, ps, fileSeed, name_pattern)};
            if (fallback.ok) return fallback;
        }
        r.error = "transpile failed: " + loaded.message;
        return r;
    }
    r.error = "cannot load '" + std::string{path} + "': " + loaded.message;
    return r;
}

}  // namespace mbun::jsc::test_runner
