// node:assert deep-equality partition: replaces the naive bootstrap deepEq
// with a comparator that matches bun's deepEquals contract (the acceptance
// suite compat/bun/test/js/node/assert/deep-equal.test.ts pins bun's documented
// divergences from node via test.failing, so THAT contract — not vanilla
// node — is the blueprint):
//   - both modes compare primitives with Object.is (loose does NOT coerce)
//   - both modes compare own enumerable string AND symbol keys
//   - loose mode treats undefined-valued props / holes / missing as equal
//   - strict mode matches plain-object classes BY CONSTRUCTOR NAME (missing
//     constructor is a wildcard; arrays are exempt)
//   - Date/RegExp/Map/Set/typed arrays ignore extra own properties; Errors
//     compare name/message/cause plus own properties
//   - typed arrays: strict compares bytes, loose compares elements with ===
//   - Buffer vs Uint8Array with equal bytes is equal (kind-tag comparison)
//   - WeakMap/WeakSet: equal when the same kind (strict requires same kind,
//     loose treats both as opaque empty objects)
// Also upgrades AssertionError (actual/expected/operator/generatedMessage,
// Error message passthrough) and rewires util.isDeepStrictEqual in place.
//
// NOTE: appended AFTER the master builtins IIFE; self-contained IIFE that
// re-binds G = globalThis. Top level must never throw.
export module mbun.jsc.js_builtins:node_assert_deepequal;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeAssertDeepEqualJS = R"JS(
(function () {
  const G = globalThis;
  try {
    const M = G.__mbunNativeModules;
    if (!M) return;
    const assertMod = M["assert"] || M["node:assert"];
    if (!assertMod) return;

    // ------------------------------------------------------------- brands
    const toStr = (v) => Object.prototype.toString.call(v);
    const taTagGet = Object.getOwnPropertyDescriptor(
      Object.getPrototypeOf(Uint8Array.prototype), Symbol.toStringTag).get;
    const taTag = (v) => taTagGet.call(v);
    const brand = (fn, v) => { try { fn.call(v); return true; } catch (_) { return false; } };
    const brandArg = (fn, v, a) => { try { fn.call(v, a); return true; } catch (_) { return false; } };
    const dateGetTime = Date.prototype.getTime;
    const reSource = Object.getOwnPropertyDescriptor(RegExp.prototype, "source").get;
    const reFlags = Object.getOwnPropertyDescriptor(RegExp.prototype, "flags").get;
    const mapSize = Object.getOwnPropertyDescriptor(Map.prototype, "size").get;
    const setSize = Object.getOwnPropertyDescriptor(Set.prototype, "size").get;
    const abLen = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, "byteLength").get;
    const sabLen = G.SharedArrayBuffer
      ? Object.getOwnPropertyDescriptor(G.SharedArrayBuffer.prototype, "byteLength").get : null;
    const dvLen = Object.getOwnPropertyDescriptor(DataView.prototype, "byteLength").get;
    const isDate = (v) => brand(dateGetTime, v);
    const isRegExp = (v) => brand(reSource, v);
    const isMap = (v) => brand(mapSize, v);
    const isSet = (v) => brand(setSize, v);
    const isWeakMap = (v) => brandArg(WeakMap.prototype.has, v, {});
    const isWeakSet = (v) => brandArg(WeakSet.prototype.has, v, {});
    // NOTE: this JSC build shares the byteLength getter between ArrayBuffer
    // and SharedArrayBuffer (both brands probe true), so kind is told apart
    // via instanceof on top of the slot brand.
    const isSAB = (v) => sabLen !== null && brand(sabLen, v) && v instanceof G.SharedArrayBuffer;
    const isAB = (v) => brand(abLen, v) && !isSAB(v);
    const isArrayBuf = (v) => brand(abLen, v) || (sabLen !== null && brand(sabLen, v));
    const isDataView = (v) => brand(dvLen, v);
    const isError = (v) => (typeof Error.isError === "function" ? Error.isError(v) : toStr(v) === "[object Error]") || v instanceof Error;
    const boxedKind = (v) => {
      if (brand(Boolean.prototype.valueOf, v)) return "boolean";
      if (brand(Number.prototype.valueOf, v)) return "number";
      if (brand(String.prototype.valueOf, v)) return "string";
      if (brand(Symbol.prototype.valueOf, v)) return "symbol";
      if (brand(BigInt.prototype.valueOf, v)) return "bigint";
      return null;
    };
    const unbox = (v, kind) => {
      switch (kind) {
        case "boolean": return Boolean.prototype.valueOf.call(v);
        case "number": return Number.prototype.valueOf.call(v);
        case "string": return String.prototype.valueOf.call(v);
        case "symbol": return Symbol.prototype.valueOf.call(v);
        case "bigint": return BigInt.prototype.valueOf.call(v);
      }
    };

    const bytesOf = (v) => {
      if (isArrayBuf(v)) return new Uint8Array(v);
      return new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
    };
    const equalBytes = (a, b) => {
      const ba = bytesOf(a), bb = bytesOf(b);
      if (ba.length !== bb.length) return false;
      for (let i = 0; i < ba.length; i++) if (ba[i] !== bb[i]) return false;
      return true;
    };

    const ownKeys = (o) => {
      const keys = [];
      for (const k of Object.keys(o)) keys.push(k);
      for (const s of Object.getOwnPropertySymbols(o))
        if (Object.getOwnPropertyDescriptor(o, s).enumerable) keys.push(s);
      return keys;
    };
    const ctorName = (o) => {
      try {
        const c = o.constructor;
        return c ? c.name : undefined;
      } catch (_) { return undefined; }
    };

    // ---------------------------------------------------------- comparator
    function deq(a, b, strict, memo) {
      if (Object.is(a, b)) return true;
      if (a === null || b === null || typeof a !== "object" || typeof b !== "object") return false;

      // cycles
      if (memo !== undefined) {
        const seen = memo.get(a);
        if (seen !== undefined && seen === b) return true;
      } else memo = new Map();
      memo.set(a, b);
      try {
        return deqObjects(a, b, strict, memo);
      } finally {
        memo.delete(a);
      }
    }

    function deqObjects(a, b, strict, memo) {
      // typed arrays (Buffer included; compared by kind tag only)
      const ka = taTag(a), kb = taTag(b);
      if (ka !== undefined || kb !== undefined) {
        if (ka !== kb) return false;
        if (strict) return equalBytes(a, b);
        if (a.length !== b.length) return false;
        for (let i = 0; i < a.length; i++) if (!(a[i] === b[i])) return false;
        return true;
      }
      // boxed primitives
      const bxa = boxedKind(a), bxb = boxedKind(b);
      if (bxa !== null || bxb !== null) {
        if (bxa !== bxb) return false;
        if (!Object.is(unbox(a, bxa), unbox(b, bxb))) return false;
        return deqOwnProps(a, b, strict, memo, null);
      }
      // Date: timestamp only (extra own props ignored)
      const da = isDate(a), db = isDate(b);
      if (da || db) {
        if (da !== db) return false;
        return Object.is(dateGetTime.call(a), dateGetTime.call(b));
      }
      // RegExp: source + flags only (lastIndex / extra own props ignored)
      const ra = isRegExp(a), rb = isRegExp(b);
      if (ra || rb) {
        if (ra !== rb) return false;
        return reSource.call(a) === reSource.call(b) && reFlags.call(a) === reFlags.call(b);
      }
      // ArrayBuffer / SharedArrayBuffer / DataView: byte equality; an
      // ArrayBuffer is never equal to a SharedArrayBuffer (node WPT rule).
      const aba = isArrayBuf(a), abb = isArrayBuf(b);
      if (aba || abb) {
        if (aba !== abb) return false;
        if (isAB(a) !== isAB(b)) return false;
        return equalBytes(a, b);
      }
      const dva = isDataView(a), dvb = isDataView(b);
      if (dva || dvb) return dva === dvb && equalBytes(a, b);
      // Error: name + message + cause + own props
      const ea = isError(a), eb = isError(b);
      if (ea || eb) {
        if (ea !== eb) return false;
        if (a.name !== b.name || a.message !== b.message) return false;
        const ca = "cause" in a, cb = "cause" in b;
        if (ca !== cb) return false;
        if (ca && !deq(a.cause, b.cause, strict, memo)) return false;
        return deqOwnProps(a, b, strict, memo, null);
      }
      // Map
      const ma = isMap(a), mb = isMap(b);
      if (ma || mb) {
        if (ma !== mb) return false;
        if (mapSize.call(a) !== mapSize.call(b)) return false;
        return deqMaps(a, b, strict, memo);
      }
      // Set
      const sa = isSet(a), sb = isSet(b);
      if (sa || sb) {
        if (sa !== sb) return false;
        if (setSize.call(a) !== setSize.call(b)) return false;
        return deqSets(a, b, strict, memo);
      }
      // WeakMap / WeakSet: opaque
      const wma = isWeakMap(a), wmb = isWeakMap(b);
      const wsa = isWeakSet(a), wsb = isWeakSet(b);
      if (wma || wmb || wsa || wsb) {
        if (strict) return wma === wmb && wsa === wsb;
        return true; // loose: no inspectable content, no tag check
      }
      // Arrays: indices + length only (extra own props ignored)
      const arrA = Array.isArray(a), arrB = Array.isArray(b);
      if (arrA !== arrB) return false;
      if (arrA) {
        if (strict) {
          if (a.length !== b.length) return false;
          for (let i = 0; i < a.length; i++) {
            const ia = i in a, ib = i in b;
            if (ia !== ib) return false;
            if (ia && !deq(a[i], b[i], strict, memo)) return false;
          }
          return true;
        }
        const n = Math.max(a.length, b.length);
        for (let i = 0; i < n; i++) if (!deq(a[i], b[i], strict, memo)) return false;
        return true;
      }
      // Arguments vs plain object mismatch (both modes)
      const tagA = toStr(a), tagB = toStr(b);
      if ((tagA === "[object Arguments]") !== (tagB === "[object Arguments]")) return false;
      // strict: plain-object class-name matching (missing constructor is a wildcard)
      if (strict) {
        const na = ctorName(a), nb = ctorName(b);
        if (na !== undefined && nb !== undefined && na !== nb) return false;
      }
      return deqOwnProps(a, b, strict, memo, null);
    }

    function deqOwnProps(a, b, strict, memo, skip) {
      const ksa = ownKeys(a), ksb = ownKeys(b);
      if (strict) {
        if (ksa.length !== ksb.length) return false;
        for (const k of ksa) {
          if (skip && skip.has(k)) continue;
          if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
          if (!deq(a[k], b[k], strict, memo)) return false;
        }
        return true;
      }
      // loose: union of keys; undefined-valued == missing
      const seen = new Set();
      for (const k of ksa) {
        seen.add(k);
        if (!deq(a[k], b[k], strict, memo)) return false;
      }
      for (const k of ksb) {
        if (seen.has(k)) continue;
        if (!deq(a[k], b[k], strict, memo)) return false;
      }
      return true;
    }

    function deqMaps(a, b, strict, memo) {
      const pendingA = [], usedB = new Set();
      for (const [k, v] of a) {
        if (k !== null && typeof k === "object") { pendingA.push([k, v]); continue; }
        // primitive key: SameValueZero lookup on b (native Map semantics)
        if (!b.has(k)) return false;
        if (!deq(v, b.get(k), strict, memo)) return false;
        usedB.add(k);
      }
      if (pendingA.length === 0) return true;
      const candidates = [];
      for (const [k, v] of b) {
        if (k !== null && typeof k === "object") candidates.push([k, v]);
      }
      if (candidates.length !== pendingA.length) return false;
      const taken = new Array(candidates.length).fill(false);
      for (const [k, v] of pendingA) {
        let matched = false;
        for (let i = 0; i < candidates.length; i++) {
          if (taken[i]) continue;
          if (deq(k, candidates[i][0], strict, memo) && deq(v, candidates[i][1], strict, memo)) {
            taken[i] = true;
            matched = true;
            break;
          }
        }
        if (!matched) return false;
      }
      return true;
    }

    function deqSets(a, b, strict, memo) {
      const pendingA = [];
      for (const v of a) {
        if (v !== null && typeof v === "object") { pendingA.push(v); continue; }
        if (!b.has(v)) return false;
      }
      if (pendingA.length === 0) return true;
      const candidates = [];
      for (const v of b) if (v !== null && typeof v === "object") candidates.push(v);
      if (candidates.length !== pendingA.length) return false;
      const taken = new Array(candidates.length).fill(false);
      for (const v of pendingA) {
        let matched = false;
        for (let i = 0; i < candidates.length; i++) {
          if (taken[i]) continue;
          if (deq(v, candidates[i], strict, memo)) { taken[i] = true; matched = true; break; }
        }
        if (!matched) return false;
      }
      return true;
    }

    // -------------------------------------------------------- AssertionError
    // Real class so `err instanceof assert.AssertionError` holds.
    class AssertionError extends Error {
      constructor(options) {
        options = options || {};
        super(options.message);
        this.name = "AssertionError";
        this.code = "ERR_ASSERTION";
        this.actual = options.actual;
        this.expected = options.expected;
        this.operator = options.operator;
        this.generatedMessage = !!options.generatedMessage;
      }
    }
    assertMod.AssertionError = AssertionError;
    // Bounded preview: the generated message must stay small no matter how
    // large/different the operands are (node truncates the same way).
    const inspect = (v) => {
      let s;
      try {
        const u = M["util"] || M["node:util"];
        s = u && typeof u.inspect === "function" ? u.inspect(v) : String(v);
      } catch (_) { s = "<value>"; }
      if (s.length > 512) s = s.slice(0, 512) + "...";
      const lines = s.split("\n");
      if (lines.length > 12) s = lines.slice(0, 10).join("\n") + "\n...";
      return s;
    };
    function assertionError(message, actual, expected, operator, defaultMessage) {
      if (message instanceof Error) throw message;
      let generated = false;
      let msg = message;
      if (msg === undefined || msg === null) {
        msg = defaultMessage;
        generated = true;
      }
      const e = new AssertionError({ message: msg, actual, expected, operator, generatedMessage: generated });
      return e;
    }
    const eqMsg = (a, b, kind) =>
      `Expected values to be ${kind}:\n\n` + inspect(a) + "\n\nshould equal\n\n" + inspect(b) + "\n";
    const neMsg = (b, kind) =>
      `Expected "actual" not to be ${kind} to:\n\n` + inspect(b) + "\n";

    assertMod.deepEqual = function deepEqual(actual, expected, message) {
      if (!deq(actual, expected, false))
        throw assertionError(message, actual, expected, "deepEqual", eqMsg(actual, expected, "loosely deep-equal"));
    };
    assertMod.notDeepEqual = function notDeepEqual(actual, expected, message) {
      if (deq(actual, expected, false))
        throw assertionError(message, actual, expected, "notDeepEqual", neMsg(expected, "loosely deep-equal"));
    };
    assertMod.deepStrictEqual = function deepStrictEqual(actual, expected, message) {
      if (!deq(actual, expected, true))
        throw assertionError(message, actual, expected, "deepStrictEqual", eqMsg(actual, expected, "strictly deep-equal"));
    };
    assertMod.notDeepStrictEqual = function notDeepStrictEqual(actual, expected, message) {
      if (deq(actual, expected, true))
        throw assertionError(message, actual, expected, "notDeepStrictEqual", neMsg(expected, "strictly deep-equal"));
    };
    // assertMod.strict may be self-referential (bootstrap aliases it to the
    // assert module itself) — never clobber the loose methods in that case.
    if (assertMod.strict && assertMod.strict !== assertMod &&
        (typeof assertMod.strict === "object" || typeof assertMod.strict === "function")) {
      assertMod.strict.deepEqual = assertMod.deepStrictEqual;
      assertMod.strict.notDeepEqual = assertMod.notDeepStrictEqual;
      assertMod.strict.deepStrictEqual = assertMod.deepStrictEqual;
      assertMod.strict.notDeepStrictEqual = assertMod.notDeepStrictEqual;
    }
    const strictMod = M["assert/strict"] || M["node:assert/strict"];
    if (strictMod && strictMod !== assertMod) {
      strictMod.deepEqual = assertMod.deepStrictEqual;
      strictMod.notDeepEqual = assertMod.notDeepStrictEqual;
      strictMod.deepStrictEqual = assertMod.deepStrictEqual;
      strictMod.notDeepStrictEqual = assertMod.notDeepStrictEqual;
    }

    // ------------------------------------- equal / strictEqual failure messages
    // The bootstrap stubs threw `AErr(message)` with no generated message at
    // all, so EVERY `assert.strictEqual` failure in the corpus surfaced as a
    // bare `error: AssertionError` with an empty message and no
    // actual/expected/operator — undiagnosable, and wrong: node's message is
    // part of its contract.
    //
    // This is node's `internal/assert/assertion_error.js` minus the Myers diff
    // engine: the two `isSimpleDiff` branches (short `a !== b`, and the stacked
    // `+ actual / - expected` form with the mismatch indicator) are exact, and
    // the multi-line case falls back to the block form deepStrictEqual already
    // uses instead of a line diff. DEFERRED: myersDiff/printMyersDiff.
    const kMaxShortStringLength = 12;
    const inspectValue = (v) => {
      try {
        const u = M["util"] || M["node:util"];
        if (u && typeof u.inspect === "function") {
          return u.inspect(v, { compact: false, customInspect: false, depth: 1000, maxArrayLength: Infinity,
                                showHidden: false, showProxy: false, sorted: true, getters: true });
        }
      } catch (_) {}
      try { return String(v); } catch (_) { return "<value>"; }
    };
    const trunc512 = (s) => (s.length > 512 ? s.slice(0, 509) + "..." : s);
    const simpleDiffMessage = (actual, expected, inspectedActual, inspectedExpected) => {
      let stringsLen = inspectedActual.length + inspectedExpected.length;
      if (typeof actual === "string") stringsLen -= 2;
      if (typeof expected === "string") stringsLen -= 2;
      if (stringsLen <= kMaxShortStringLength && (actual !== 0 || expected !== 0)) {
        return { header: "", message: inspectedActual + " !== " + inspectedExpected };
      }
      // getStackedDiff (no colors): the two values stacked, plus a caret under
      // the first differing character when both sides are short strings.
      let message = "\n+ " + inspectedActual + "\n- " + inspectedExpected;
      if (typeof actual === "string" && typeof expected === "string" &&
          inspectedActual.length + inspectedExpected.length <= 80) {
        let indicatorIdx = -1;
        for (let i = 0; i < inspectedActual.length; i++) {
          if (inspectedActual[i] !== inspectedExpected[i]) { if (i >= 3) indicatorIdx = i; break; }
        }
        if (indicatorIdx !== -1) message += "\n" + " ".repeat(indicatorIdx + 2) + "^";
      }
      return { header: "+ actual - expected", message };
    };
    const strictEqualDiff = (actual, expected) => {
      let operator = "strictEqual";
      const inspectedActual = inspectValue(actual);
      const inspectedExpected = inspectValue(expected);
      const splitActual = inspectedActual.split("\n");
      const splitExpected = inspectedExpected.split("\n");
      // checkOperator: two equal-looking objects that are not reference-equal
      // report the "reference-equal" wording instead.
      if (typeof actual === "object" && actual !== null && typeof expected === "object" && expected !== null &&
          inspectedActual === inspectedExpected) operator = "notIdentical";
      else if (typeof actual === "object" && actual !== null && typeof expected === "object" && expected !== null)
        operator = "strictEqualObject";
      const simple = splitActual.length === 1 && splitExpected.length === 1 &&
        (typeof actual !== "object" || actual === null || typeof expected !== "object" || expected === null);
      let header = "+ actual - expected";
      let message;
      if (simple) {
        const d = simpleDiffMessage(actual, expected, splitActual[0], splitExpected[0]);
        header = d.header; message = d.message;
        operator = "strictEqual";
      } else if (operator === "notIdentical") {
        header = ""; message = inspectedActual;
      } else {
        // Line diff DEFERRED — show both sides in full instead.
        header = "+ actual - expected";
        message = "+ " + trunc512(inspectedActual) + "\n- " + trunc512(inspectedExpected);
      }
      const readable = {
        strictEqual: "Expected values to be strictly equal:",
        strictEqualObject: 'Expected "actual" to be reference-equal to "expected":',
        notIdentical: "Values have same structure but are not reference-equal:",
      }[operator];
      return readable + "\n" + header + "\n" + message + "\n";
    };
    const notStrictEqualMessage = (actual) => {
      let base = 'Expected "actual" to be strictly unequal to:';
      if ((typeof actual === "object" && actual !== null) || typeof actual === "function")
        base = 'Expected "actual" not to be reference-equal to "expected":';
      const res = inspectValue(actual).split("\n");
      if (res.length === 1) return base + (res[0].length > 5 ? "\n\n" : " ") + res[0];
      return base + "\n\n" + res.join("\n") + "\n";
    };
    assertMod.equal = function equal(actual, expected, message) {
      // eslint-disable-next-line eqeqeq
      if (actual != expected)
        throw assertionError(message, actual, expected, "==",
          trunc512(inspectValue(actual)) + " == " + trunc512(inspectValue(expected)));
    };
    assertMod.notEqual = function notEqual(actual, expected, message) {
      // eslint-disable-next-line eqeqeq
      if (actual == expected)
        throw assertionError(message, actual, expected, "!=",
          trunc512(inspectValue(actual)) + " != " + trunc512(inspectValue(expected)));
    };
    assertMod.strictEqual = function strictEqual(actual, expected, message) {
      if (!Object.is(actual, expected))
        throw assertionError(message, actual, expected, "strictEqual", strictEqualDiff(actual, expected));
    };
    assertMod.notStrictEqual = function notStrictEqual(actual, expected, message) {
      if (Object.is(actual, expected))
        throw assertionError(message, actual, expected, "notStrictEqual", notStrictEqualMessage(actual));
    };
    for (const target of [assertMod.strict, M["assert/strict"], M["node:assert/strict"]]) {
      if (!target || target === assertMod) continue;
      if (typeof target !== "object" && typeof target !== "function") continue;
      target.equal = assertMod.strictEqual;
      target.notEqual = assertMod.notStrictEqual;
      target.strictEqual = assertMod.strictEqual;
      target.notStrictEqual = assertMod.notStrictEqual;
    }

    const util = M["util"] || M["node:util"];
    if (util) util.isDeepStrictEqual = function isDeepStrictEqual(a, b) { return deq(a, b, true); };
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
