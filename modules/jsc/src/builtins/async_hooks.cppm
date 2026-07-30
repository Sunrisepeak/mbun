// async_hooks / AsyncLocalStorage.
//
// PORT, not a re-implementation. This partition is a mechanical translation of
// node's own source, in this order:
//
//   compat/node/lib/internal/async_hooks.js                   the state machine
//   compat/node/lib/async_hooks.js                            AsyncHook, AsyncResource
//   compat/node/lib/internal/async_context_frame.js           the context frame
//   compat/node/lib/internal/async_local_storage/async_context_frame.js   ALS
//   compat/node/lib/internal/async_local_storage/run_scope.js  using-scope
//
// node's algorithm, branches, field indices and error text are the blueprint
// and are kept verbatim wherever they can be; `primordials` is lowered onto
// plain globals and `internalBinding('async_wrap')` onto the local typed-array
// state node's own JS half reads and writes. Read node's file next to this one
// before changing anything here: the value of this partition is that it IS
// node's structure, so a divergence is a bug even when it looks like a cleanup.
//
// What it replaced, and why: a hand-written approximation that reported success
// from createHook() while modelling only a few callback boundaries. It gated all
// id bookkeeping on "is a hook enabled right now", which node never does, so
// executionAsyncId()/triggerAsyncId() were wrong inside every callback whose
// resource was created before the hook was installed, and the resource
// lifecycle (init/before/after/destroy, the id stack, the hook-array snapshot
// taken while hooks are running) did not exist at all.
//
// CONTEXT ACROSS `await` IS SOLVED, and an earlier version of this comment was
// wrong about why it could not be. node propagates context across await through
// V8's continuation-preserved embedder data. This build's prebuilt WebKit is
// Bun's fork with USE(BUN_JSC_ADDITIONS)=1 and carries the equivalent:
// JSGlobalObject::m_asyncContextData, which JSPromise.cpp snapshots at
// registration and JSMicrotask.cpp restores before running the job. The previous
// note called that "an engine seam ... private to JSC builtins and unreachable
// from a C-API payload". Both halves were false: the field's nearest access
// specifier in JSGlobalObject.h is `public:`, and this runtime compiles against
// full JSC internals rather than only the C API. The frame now lives in that
// tuple (host trio __mbunAsyncContextNative in runtime/core_bindings.inc, from
// the w58 bun js/node lane), adopted lazily on the first AsyncLocalStorage.
//
// The engine seams that DO remain, stated precisely so nobody re-derives them:
//
//  1. async_hooks *ids* still do not cross `await`. The engine restores the
//     context frame, not this layer's execution/trigger id pair, so
//     executionAsyncId() inside an async function after an await is still the
//     id of whatever ran the continuation. Carrying the id pair inside the frame
//     would fix it and is the next thing to try here; it is why
//     test-async-hooks-async-await, -enable-disable-enable and
//     -execution-async-resource-await are still failing while
//     test-async-local-storage-contexts (pure ALS) now passes.
//  2. Promise *creation* is only observed where JS can see it: the Promise
//     static factories and `then` (which is where every derived promise in a
//     chain comes from). A promise the engine creates internally, including the
//     one behind every `await`, is invisible. node gets these from V8's
//     PromiseHook.
//  3. Promise instrumentation is only installed WHILE a hook is enabled, which
//     is node's own structure (updatePromiseHookMode on enable,
//     disablePromiseHookIfNecessary on disable) — but for node that is only a
//     cost decision, whereas here it is also a capability limit. V8's
//     PromiseHook observes reactions that were registered before the hook
//     existed; a JS `then` wrapper can only observe reactions it wrapped
//     itself. Making it permanent so it could catch those costs one closure and
//     one extra JS call frame per reaction for every promise in the process:
//     measured, 2.2x on a 200k `.then` chain and ~1.4x on a 200k fanout, after
//     removing every other allocation from the path. That is not buyable, so
//     three corpus files stay failing on purpose and are named here so nobody
//     re-derives the tradeoff: test-async-wrap-promise-after-enabled,
//     test-async-hooks-enable-before-promise-resolve and
//     test-async-hooks-enable-during-promise, each of which enables a hook
//     after the reaction it wants to observe was already registered. Getting
//     them needs an engine hook, not more JS.
//
// PERFORMANCE, since this file sits on the promise path: two things here were
// each worth multiples on a plain `.then` chain and both are easy to reintroduce
// by "tidying up". First, tracking a promise (assigning it an async id) writes
// two symbol-keyed properties onto it, which transitions the shape of an object
// the engine otherwise keeps in one structure -- so every trackPromise call
// site is behind a hook check, exactly as node's is behind the PromiseHook being
// installed. Second, `then`'s fast path must hand the user's callbacks straight
// to the engine, unwrapped. Anything added above those two checks is paid by
// every promise the process creates; there is a probe for this, and it is not
// covered by latency_probe.py.
//
// The emission wiring at the bottom is NOT node source: node's C++ half emits
// init/before/after/destroy from every AsyncWrap, mbun has no AsyncWrap, so the
// registration boundaries mbun's JS runtime does own are wired to the ported
// emit* functions there. That section is the analogue of node's src/*_wrap.cc.
export module mbun.jsc.js_builtins:async_hooks;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kAsyncHooksJS = R"JS(
  // ---- async_hooks / AsyncLocalStorage (ported from node; see header) ----
  (function () {

  // ===========================================================================
  // internalBinding('async_wrap') — node src/async_wrap.cc + src/env.h
  // ===========================================================================
  const constants = {
    kInit: 0, kBefore: 1, kAfter: 2, kDestroy: 3, kPromiseResolve: 4,
    kTotals: 5, kCheck: 6, kStackLength: 7, kUsesExecutionAsyncResource: 8,
    kExecutionAsyncId: 0, kTriggerAsyncId: 1, kAsyncIdCounter: 2,
    kDefaultTriggerAsyncId: 3,
  };
  const {
    kInit, kBefore, kAfter, kDestroy, kTotals, kPromiseResolve,
    kCheck, kExecutionAsyncId, kAsyncIdCounter, kTriggerAsyncId,
    kDefaultTriggerAsyncId, kStackLength, kUsesExecutionAsyncResource,
  } = constants;

  const async_hook_fields = new Uint32Array(9);
  const async_id_fields = new Float64Array(4);
  // node's bootstrap owns async id 1, and starts execution inside it.
  async_id_fields[kAsyncIdCounter] = 1;
  async_id_fields[kExecutionAsyncId] = 1;
  async_id_fields[kDefaultTriggerAsyncId] = -1;
  const async_ids_stack = new Float64Array(2 * 16384);
  const execution_async_resources = [];

  const async_id_symbol = Symbol('async_id_symbol');
  const trigger_async_id_symbol = Symbol('trigger_async_id_symbol');
  const resource_symbol = Symbol('resource_symbol');
  const owner_symbol = Symbol('owner_symbol');

  const init_symbol = Symbol('init');
  const before_symbol = Symbol('before');
  const after_symbol = Symbol('after');
  const destroy_symbol = Symbol('destroy');
  const promise_resolve_symbol = Symbol('promiseResolve');
  const kNoPromiseHook = Symbol('kNoPromiseHook');

  // --- node's error classes, with node's exact codes and messages -----------
  const mkErr = (Base, code, msgFn) => (...args) => {
    const e = new Base(msgFn(...args));
    e.code = code;
    return e;
  };
  // lib/internal/errors.js: E('ERR_ASYNC_CALLBACK', '%s must be a function', TypeError)
  const ERR_ASYNC_CALLBACK = mkErr(TypeError, 'ERR_ASYNC_CALLBACK',
    (name) => `${name} must be a function`);
  // E('ERR_ASYNC_TYPE', 'Invalid name for async "type": %s', TypeError)
  const ERR_ASYNC_TYPE = mkErr(TypeError, 'ERR_ASYNC_TYPE',
    (type) => `Invalid name for async "type": ${type}`);
  // E('ERR_INVALID_ASYNC_ID', 'Invalid %s value: %s', RangeError)
  const ERR_INVALID_ASYNC_ID = mkErr(RangeError, 'ERR_INVALID_ASYNC_ID',
    (name, value) => `Invalid ${name} value: ${value}`);
  const ERR_INVALID_ARG_TYPE = mkErr(TypeError, 'ERR_INVALID_ARG_TYPE',
    (name, expected, actual) =>
      `The "${name}" argument must be of type ${expected}. Received ${
        actual === null ? 'null' : typeof actual}`);
  const ERR_INVALID_ARG_VALUE = mkErr(TypeError, 'ERR_INVALID_ARG_VALUE',
    (name, value, reason) => `The argument '${name}' ${reason}. Received ${String(value)}`);

  const validateFunction = (value, name) => {
    if (typeof value !== 'function') throw ERR_INVALID_ARG_TYPE(name, 'function', value);
  };
  const validateString = (value, name) => {
    if (typeof value !== 'string') throw ERR_INVALID_ARG_TYPE(name, 'string', value);
  };
  const validateObject = (value, name) => {
    if (value === null || typeof value !== 'object') {
      throw ERR_INVALID_ARG_TYPE(name, 'Object', value);
    }
  };

  // ===========================================================================
  // lib/internal/async_hooks.js
  // ===========================================================================
  const active_hooks = {
    array: [],
    call_depth: 0,
    tmp_array: null,
    tmp_fields: null,
  };

  const topLevelResource = {};

  function lookupPublicResource(resource) {
    if (typeof resource !== 'object' || resource === null) return resource;
    const publicResource = resource[resource_symbol];
    if (publicResource !== undefined) return publicResource;
    return resource;
  }

  function executionAsyncResource() {
    async_hook_fields[kUsesExecutionAsyncResource] = 1;
    const index = async_hook_fields[kStackLength] - 1;
    if (index === -1) return topLevelResource;
    const resource = execution_async_resources[index];
    return lookupPublicResource(resource === undefined ? topLevelResource : resource);
  }

  // node aborts the process when a hook throws (fatalError -> process.exit).
  function fatalError(e) {
    const p = G.process;
    if (p && typeof p._rawDebug === 'function') {
      p._rawDebug(typeof e?.stack === 'string' ? e.stack : String(e));
    }
    if (p && typeof p.exit === 'function') { p.exit(1); }
    throw e;
  }

  function emitInitNative(asyncId, type, triggerAsyncId, resource, isPromiseHook) {
    active_hooks.call_depth += 1;
    resource = lookupPublicResource(resource);
    try {
      for (var i = 0; i < active_hooks.array.length; i++) {
        if (typeof active_hooks.array[i][init_symbol] === 'function') {
          if (isPromiseHook && active_hooks.array[i][kNoPromiseHook]) continue;
          active_hooks.array[i][init_symbol](asyncId, type, triggerAsyncId, resource);
        }
      }
    } catch (e) {
      fatalError(e);
    } finally {
      active_hooks.call_depth -= 1;
    }
    if (active_hooks.call_depth === 0 && active_hooks.tmp_array !== null) {
      restoreActiveHooks();
    }
  }

  function emitHook(symbol, asyncId, isPromiseHook) {
    active_hooks.call_depth += 1;
    try {
      for (var i = 0; i < active_hooks.array.length; i++) {
        if (typeof active_hooks.array[i][symbol] === 'function') {
          if (isPromiseHook && active_hooks.array[i][kNoPromiseHook]) continue;
          active_hooks.array[i][symbol](asyncId);
        }
      }
    } catch (e) {
      fatalError(e);
    } finally {
      active_hooks.call_depth -= 1;
    }
    if (active_hooks.call_depth === 0 && active_hooks.tmp_array !== null) {
      restoreActiveHooks();
    }
  }

  const emitBeforeNative = (asyncId, isPromiseHook) => emitHook(before_symbol, asyncId, isPromiseHook);
  const emitAfterNative = (asyncId, isPromiseHook) => emitHook(after_symbol, asyncId, isPromiseHook);
  const emitDestroyNative = (asyncId) => emitHook(destroy_symbol, asyncId);
  const emitPromiseResolveNative = (asyncId, isPromiseHook) => emitHook(promise_resolve_symbol, asyncId, isPromiseHook);

  function getHookArrays() {
    if (active_hooks.call_depth === 0) return [active_hooks.array, async_hook_fields];
    if (active_hooks.tmp_array === null) storeActiveHooks();
    return [active_hooks.tmp_array, active_hooks.tmp_fields];
  }

  function storeActiveHooks() {
    active_hooks.tmp_array = active_hooks.array.slice();
    active_hooks.tmp_fields = [];
    copyHooks(active_hooks.tmp_fields, async_hook_fields);
  }

  function copyHooks(destination, source) {
    destination[kInit] = source[kInit];
    destination[kBefore] = source[kBefore];
    destination[kAfter] = source[kAfter];
    destination[kDestroy] = source[kDestroy];
    destination[kPromiseResolve] = source[kPromiseResolve];
  }

  function restoreActiveHooks() {
    active_hooks.array = active_hooks.tmp_array;
    copyHooks(async_hook_fields, active_hooks.tmp_fields);
    active_hooks.tmp_array = null;
    active_hooks.tmp_fields = null;
  }

  // --- destroy queue -------------------------------------------------------
  // node queues destroy ids in C++ and drains them from a native immediate
  // (Environment::DestroyAsyncIdsCallback).
  const destroyQueue = [];
  let destroyQueueScheduled = false;
  // The drain is node's internal bookkeeping, and in node it rides a *native*
  // immediate that no user hook can observe. mbun's setImmediate is instrumented
  // by this very payload, so scheduling the drain through it would emit a
  // spurious before/after pair for the drain itself. This counter tells the timer
  // instrumentation to stay out of the way for internally scheduled work.
  let internalSchedule = 0;
  // Captured before this payload installs its own wrappers, so internal
  // bookkeeping never runs through the instrumentation it is part of.
  //
  // The tick queue, NOT queueMicrotask: mbun's queueMicrotask is promise-backed,
  // so a drain scheduled through it is delivered by a promise *reaction* — and
  // this payload instruments promise reactions, which made the drain emit a
  // before/after pair of its own (test-async-hooks-top-level-clearimmediate saw
  // them as events for its cleared Immediate). The `internalSchedule` counter
  // below only covers the synchronous scheduling call, so it cannot suppress a
  // later reaction; picking a queue that is not promise-backed does.
  let internalNextTick;
  let internalQueueMicrotask;
  function scheduleInternal(fn) {
    internalSchedule++;
    try {
      if (internalNextTick !== undefined) internalNextTick(fn);
      else if (internalQueueMicrotask !== undefined) internalQueueMicrotask(fn);
      else if (typeof G.setImmediate === 'function') G.setImmediate(fn);
      else G.queueMicrotask(fn);
    } finally {
      internalSchedule--;
    }
  }
  // A microtask, not a fresh immediate. node drains destroy ids from a native
  // immediate that is part of the SAME turn as the handle close that queued
  // them; scheduling a *new* immediate here instead put the drain one full turn
  // late, which is observable: a test that disables its hook in the next
  // immediate (test-async-hooks-disable-gc-tracking,
  // test-async-hooks-prevent-double-destroy) had the queued destroy silently
  // dropped, because by drain time no destroy hook was left to receive it.
  // Draining on the microtask checkpoint keeps destroy asynchronous — node's
  // contract, and what emitDestroy()'s "only schedules calling the hook"
  // comment in lib/async_hooks.js relies on — while still delivering it inside
  // the turn that queued it.
  function drainDestroyQueue() {
    destroyQueueScheduled = false;
    while (destroyQueue.length > 0) {
      const id = destroyQueue.shift();
      if (hasHooks(kDestroy)) emitDestroyNative(id);
    }
  }
  function queueDestroyAsyncId(asyncId) {
    destroyQueue.push(asyncId);
    if (destroyQueueScheduled) return;
    destroyQueueScheduled = true;
    scheduleInternal(drainDestroyQueue);
  }

  // node registers a C++ weak callback so a GC'd resource still emits destroy;
  // FinalizationRegistry is the portable equivalent of that contract.
  const destroyRegistry = typeof G.FinalizationRegistry === 'function' ?
    new G.FinalizationRegistry((asyncId) => { emitDestroyScript(asyncId); }) : undefined;
  function registerDestroyHook(resource, asyncId, destroyed) {
    if (destroyRegistry === undefined) return;
    if (resource === null || (typeof resource !== 'object' && typeof resource !== 'function')) return;
    destroyRegistry.register(resource, asyncId);
  }

  function newAsyncId() {
    return ++async_id_fields[kAsyncIdCounter];
  }

  function getOrSetAsyncId(object) {
    if (Object.prototype.hasOwnProperty.call(object, async_id_symbol)) {
      return object[async_id_symbol];
    }
    return object[async_id_symbol] = newAsyncId();
  }

  function getDefaultTriggerAsyncId() {
    const defaultTriggerAsyncId = async_id_fields[kDefaultTriggerAsyncId];
    if (defaultTriggerAsyncId < 0) return async_id_fields[kExecutionAsyncId];
    return defaultTriggerAsyncId;
  }

  function clearDefaultTriggerAsyncId() {
    async_id_fields[kDefaultTriggerAsyncId] = -1;
  }

  function defaultTriggerAsyncIdScope(triggerAsyncId, block, ...args) {
    if (triggerAsyncId === undefined) return Reflect.apply(block, null, args);
    const oldDefaultTriggerAsyncId = async_id_fields[kDefaultTriggerAsyncId];
    async_id_fields[kDefaultTriggerAsyncId] = triggerAsyncId;
    try {
      return Reflect.apply(block, null, args);
    } finally {
      async_id_fields[kDefaultTriggerAsyncId] = oldDefaultTriggerAsyncId;
    }
  }

  function hasHooks(key) { return async_hook_fields[key] > 0; }
  function enabledHooksExist() { return active_hooks.array.length > 0; }
  function initHooksExist() { return hasHooks(kInit); }
  function afterHooksExist() { return hasHooks(kAfter); }
  function destroyHooksExist() { return hasHooks(kDestroy); }
  function promiseResolveHooksExist() { return hasHooks(kPromiseResolve); }

  function emitInitScript(asyncId, type, triggerAsyncId, resource, isPromiseHook = false) {
    if (!hasHooks(kInit)) return;
    if (triggerAsyncId === null) triggerAsyncId = getDefaultTriggerAsyncId();
    emitInitNative(asyncId, type, triggerAsyncId, resource, isPromiseHook);
  }

  function emitBeforeScript(asyncId, triggerAsyncId, resource, isPromiseHook = false) {
    pushAsyncContext(asyncId, triggerAsyncId, resource);
    if (hasHooks(kBefore)) emitBeforeNative(asyncId, isPromiseHook);
  }

  function emitAfterScript(asyncId) {
    if (hasHooks(kAfter)) emitAfterNative(asyncId);
    popAsyncContext(asyncId);
  }

  function emitDestroyScript(asyncId) {
    if (!hasHooks(kDestroy) || !(asyncId > 0)) return;
    queueDestroyAsyncId(asyncId);
  }

  function hasAsyncIdStack() { return hasHooks(kStackLength); }

  function pushAsyncContext(asyncId, triggerAsyncId, resource) {
    const offset = async_hook_fields[kStackLength];
    execution_async_resources[offset] = resource;
    if (offset * 2 >= async_ids_stack.length) return;
    async_ids_stack[offset * 2] = async_id_fields[kExecutionAsyncId];
    async_ids_stack[offset * 2 + 1] = async_id_fields[kTriggerAsyncId];
    async_hook_fields[kStackLength]++;
    async_id_fields[kExecutionAsyncId] = asyncId;
    async_id_fields[kTriggerAsyncId] = triggerAsyncId;
  }

  function popAsyncContext(asyncId) {
    const stackLength = async_hook_fields[kStackLength];
    if (stackLength === 0) return false;
    const offset = stackLength - 1;
    async_id_fields[kExecutionAsyncId] = async_ids_stack[2 * offset];
    async_id_fields[kTriggerAsyncId] = async_ids_stack[2 * offset + 1];
    execution_async_resources.pop();
    async_hook_fields[kStackLength] = offset;
    return offset > 0;
  }

  function clearAsyncIdStack() {
    async_id_fields[kExecutionAsyncId] = 0;
    async_id_fields[kTriggerAsyncId] = 0;
    async_hook_fields[kStackLength] = 0;
    execution_async_resources.length = 0;
  }

  function executionAsyncId() { return async_id_fields[kExecutionAsyncId]; }
  function triggerAsyncId() { return async_id_fields[kTriggerAsyncId]; }

  // ===========================================================================
  // lib/internal/async_context_frame.js
  // The frame algebra is node's: a copy-on-write Map keyed by storage. What node
  // gets from V8 is the *slot* — continuation-preserved embedder data, which the
  // engine carries across `await`.
  //
  // THIS BUILD HAS THE EQUIVALENT SLOT, and it is reachable. The prebuilt WebKit
  // is Bun's fork with USE(BUN_JSC_ADDITIONS)=1: JSGlobalObject holds an
  // InternalFieldTuple in m_asyncContextData, JSPromise.cpp snapshots field 0
  // when a reaction or await job is registered, and JSMicrotask.cpp restores it
  // before running the job. So the engine does the propagation — including
  // across `await`, which no JS wrapper can reach — as long as the live frame
  // lives in that tuple rather than in a JS variable.
  //
  // Mechanism and host trio (__mbunAsyncContextNative, runtime/core_bindings.inc)
  // are from the w58 bun js/node lane; this is the same adoption, driven from
  // node's frame class instead of the previous hand-written context array.
  //
  // A correction to the record, because an earlier note in this file (and my own
  // sizing of the await-context work as out of reach) said the opposite: the
  // field is NOT private and the runtime is NOT restricted to the C API. The
  // last access specifier before it in JSGlobalObject.h is `public:`, and this
  // runtime already compiles against full JSC internals. That claim was reached
  // by grepping for an accessor method, finding none, and inferring privacy
  // without checking the specifier. Do not re-derive it: measure instead.
  //
  // Adoption is lazy — on the first AsyncLocalStorage, exactly as bun enables
  // tracking — so a program that never uses ALS keeps the plain JS variable and
  // pays no host call on any callback-registration path.
  // ===========================================================================
  let continuationData;
  let engineContext = null;
  // Marks "the engine owns this registration's frame" without reading the slot.
  const kEngineFrame = Symbol('mbun.engineAsyncContextFrame');
  const frameGet = () => (engineContext !== null ? engineContext.get() : continuationData);
  const frameSet = (value) => {
    if (engineContext !== null) engineContext.set(value); else continuationData = value;
  };
  // Idempotent, and a no-op on a realm whose tuple was never installed (the
  // native reports that), so the JS fallback stays authoritative rather than
  // stores vanishing.
  const adoptEngineContext = () => {
    if (engineContext !== null) return;
    const N = G.__mbunAsyncContextNative;
    if (!N || typeof N.enable !== 'function') return;
    let ok = false;
    try { ok = N.enable() === true; } catch (e) { ok = false; }
    if (!ok) return;
    // Carry whatever the JS fallback held into the engine slot, so a frame
    // established before the first ALS is not dropped mid-flight.
    try { if (continuationData !== undefined) N.set(continuationData); } catch (e) { /* keep going */ }
    engineContext = N;
  };
  class AsyncContextFrame extends Map {
    constructor(store, data) {
      super(AsyncContextFrame.current());
      this.set(store, data);
    }
    static get enabled() { return true; }
    static current() { return frameGet(); }
    static set(frame) { frameSet(frame); }
    static exchange(frame) {
      const prior = frameGet();
      frameSet(frame);
      return prior;
    }
    static disable(store) {
      const frame = AsyncContextFrame.current();
      if (frame !== undefined && frame !== null) frame.disable(store);
    }
    disable(store) { this.delete(store); }
  }

  // ===========================================================================
  // lib/async_hooks.js
  // ===========================================================================
  class AsyncHook {
    constructor({ init, before, after, destroy, promiseResolve, trackPromises }) {
      if (init !== undefined && typeof init !== 'function') throw ERR_ASYNC_CALLBACK('hook.init');
      if (before !== undefined && typeof before !== 'function') throw ERR_ASYNC_CALLBACK('hook.before');
      if (after !== undefined && typeof after !== 'function') throw ERR_ASYNC_CALLBACK('hook.after');
      if (destroy !== undefined && typeof destroy !== 'function') throw ERR_ASYNC_CALLBACK('hook.destroy');
      if (promiseResolve !== undefined && typeof promiseResolve !== 'function') {
        throw ERR_ASYNC_CALLBACK('hook.promiseResolve');
      }
      if (trackPromises !== undefined && typeof trackPromises !== 'boolean') {
        throw ERR_INVALID_ARG_TYPE('trackPromises', 'boolean', trackPromises);
      }

      this[init_symbol] = init;
      this[before_symbol] = before;
      this[after_symbol] = after;
      this[destroy_symbol] = destroy;
      this[promise_resolve_symbol] = promiseResolve;
      if (trackPromises === false) {
        if (promiseResolve) {
          throw ERR_INVALID_ARG_VALUE('trackPromises', trackPromises,
            'must not be false when promiseResolve is enabled');
        }
        this[kNoPromiseHook] = true;
      } else {
        this[kNoPromiseHook] = false;
      }
    }

    enable() {
      const { 0: hooks_array, 1: hook_fields } = getHookArrays();
      if (hooks_array.includes(this)) return this;
      const prev_kTotals = hook_fields[kTotals];
      hook_fields[kTotals] = hook_fields[kInit] += +!!this[init_symbol];
      hook_fields[kTotals] += hook_fields[kBefore] += +!!this[before_symbol];
      hook_fields[kTotals] += hook_fields[kAfter] += +!!this[after_symbol];
      hook_fields[kTotals] += hook_fields[kDestroy] += +!!this[destroy_symbol];
      hook_fields[kTotals] += hook_fields[kPromiseResolve] += +!!this[promise_resolve_symbol];
      hooks_array.push(this);
      if (prev_kTotals === 0 && hook_fields[kTotals] > 0) enableHooks();
      syncInternalHookState(true);
      return this;
    }

    disable() {
      const { 0: hooks_array, 1: hook_fields } = getHookArrays();
      const index = hooks_array.indexOf(this);
      if (index === -1) return this;
      const prev_kTotals = hook_fields[kTotals];
      hook_fields[kTotals] = hook_fields[kInit] -= +!!this[init_symbol];
      hook_fields[kTotals] += hook_fields[kBefore] -= +!!this[before_symbol];
      hook_fields[kTotals] += hook_fields[kAfter] -= +!!this[after_symbol];
      hook_fields[kTotals] += hook_fields[kDestroy] -= +!!this[destroy_symbol];
      hook_fields[kTotals] += hook_fields[kPromiseResolve] -= +!!this[promise_resolve_symbol];
      hooks_array.splice(index, 1);
      if (prev_kTotals > 0 && hook_fields[kTotals] === 0) disableHooks();
      if (!enabledHooksExist()) syncInternalHookState(false);
      return this;
    }
  }

  function enableHooks() { async_hook_fields[kCheck] += 1; }
  function disableHooks() { async_hook_fields[kCheck] -= 1; }

  // node's `internal/async_hooks` and its public `async_hooks` are two views of
  // ONE piece of state. Here they cannot be: the vendored internal module is a
  // separate instance with its own hook array, and node-core modules (and
  // test-async-hooks-enabledhooksexits) read `enabledHooksExist()` from it. Keep
  // its observable "some hook is active" state in step, without pretending to
  // install native hooks into it.
  let internalMarker;
  function syncInternalHookState(enabled) {
    try {
      const internal = typeof G.require === 'function' ? G.require('internal/async_hooks') : undefined;
      const arrays = internal && typeof internal.getHookArrays === 'function' && internal.getHookArrays();
      if (!arrays) return;
      const hooks = arrays[0];
      if (enabled) {
        if (internalMarker === undefined) {
          internalMarker = {};
          hooks.push(internalMarker);
        }
      } else if (internalMarker !== undefined) {
        const index = hooks.indexOf(internalMarker);
        if (index >= 0) hooks.splice(index, 1);
        internalMarker = undefined;
      }
    } catch { /* internal module unavailable during bootstrap */ }
  }

  function createHook(fns) { return new AsyncHook(fns); }

  const destroyedSymbol = Symbol('destroyed');
  const contextFrameSymbol = Symbol('context_frame');

  class AsyncResource {
    constructor(type, opts = {}) {
      validateString(type, 'type');

      let triggerAsyncId = opts;
      let requireManualDestroy = false;
      if (typeof opts !== 'number') {
        triggerAsyncId = opts.triggerAsyncId === undefined ?
          getDefaultTriggerAsyncId() : opts.triggerAsyncId;
        requireManualDestroy = !!opts.requireManualDestroy;
      }

      if (!Number.isSafeInteger(triggerAsyncId) || triggerAsyncId < -1) {
        throw ERR_INVALID_ASYNC_ID('triggerAsyncId', triggerAsyncId);
      }

      this[contextFrameSymbol] = AsyncContextFrame.current();

      const asyncId = newAsyncId();
      this[async_id_symbol] = asyncId;
      this[trigger_async_id_symbol] = triggerAsyncId;

      if (initHooksExist()) {
        if (type.length === 0) throw ERR_ASYNC_TYPE(type);
        emitInitScript(asyncId, type, triggerAsyncId, this);
      }

      if (!requireManualDestroy && destroyHooksExist()) {
        const destroyed = { destroyed: false };
        this[destroyedSymbol] = destroyed;
        registerDestroyHook(this, asyncId, destroyed);
      }
    }

    runInAsyncScope(fn, thisArg, ...args) {
      const asyncId = this[async_id_symbol];
      emitBeforeScript(asyncId, this[trigger_async_id_symbol], this);

      const contextFrame = this[contextFrameSymbol];
      const prior = AsyncContextFrame.exchange(contextFrame);
      try {
        return Reflect.apply(fn, thisArg, args);
      } finally {
        AsyncContextFrame.set(prior);
        if (hasAsyncIdStack()) emitAfterScript(asyncId);
      }
    }

    emitDestroy() {
      if (this[destroyedSymbol] !== undefined) this[destroyedSymbol].destroyed = true;
      emitDestroyScript(this[async_id_symbol]);
      return this;
    }

    asyncId() { return this[async_id_symbol]; }
    triggerAsyncId() { return this[trigger_async_id_symbol]; }

    bind(fn, thisArg) {
      validateFunction(fn, 'fn');
      let bound;
      if (thisArg === undefined) {
        const resource = this;
        bound = function (...args) {
          args.unshift(fn, this);
          return Reflect.apply(resource.runInAsyncScope, resource, args);
        };
      } else {
        bound = this.runInAsyncScope.bind(this, fn, thisArg);
      }
      Object.defineProperties(bound, {
        'length': {
          __proto__: null, configurable: true, enumerable: false,
          value: fn.length, writable: false,
        },
      });
      return bound;
    }

    static bind(fn, type, thisArg) {
      validateFunction(fn, 'fn');
      type ||= fn.name;
      return (new AsyncResource(type || 'bound-anonymous-fn')).bind(fn, thisArg);
    }
  }

  // ===========================================================================
  // lib/internal/async_local_storage/run_scope.js
  // ===========================================================================
  class RunScope {
    #storage;
    #previousStore;
    #disposed = false;
    constructor(storage, store) {
      this.#storage = storage;
      this.#previousStore = storage.getStore();
      storage.enterWith(store);
    }
    dispose() {
      if (this.#disposed) return;
      this.#disposed = true;
      this.#storage.enterWith(this.#previousStore);
    }
    [Symbol.dispose]() { this.dispose(); }
  }

  // ===========================================================================
  // lib/internal/async_local_storage/async_context_frame.js
  // ===========================================================================
  class AsyncLocalStorage {
    #defaultValue = undefined;
    #name = undefined;

    constructor(options = {}) {
      // Hand the frame over to the engine here, as bun does on the first
      // AsyncLocalStorage. From this point `await` propagates the store, because
      // JSC snapshots and restores its own slot around promise reactions.
      adoptEngineContext();
      validateObject(options, 'options');
      this.#defaultValue = options.defaultValue;
      if (options.name !== undefined) this.#name = `${options.name}`;
    }

    get name() { return this.#name || ''; }

    // node's signature is bind(fn); Bun's (src/js/node/async_hooks.ts:137)
    // additionally pre-binds trailing arguments, and mbun is a Bun rewrite, so
    // that superset is kept. It cannot change node behaviour: node callers pass
    // only `fn`, and with no extra arguments this IS node's line — in
    // particular the returned function still forwards its call-site `this`,
    // which pre-binding would otherwise pin to null.
    static bind(fn, ...args) {
      validateFunction(fn, 'fn');
      const bound = AsyncResource.bind(fn);
      return args.length === 0 ? bound : bound.bind(null, ...args);
    }

    static snapshot() {
      return AsyncLocalStorage.bind((cb, ...args) => cb(...args));
    }

    disable() { AsyncContextFrame.disable(this); }

    enterWith(data) {
      const frame = new AsyncContextFrame(this, data);
      AsyncContextFrame.set(frame);
    }

    run(data, fn, ...args) {
      const prior = this.getStore();
      if (Object.is(prior, data)) return Reflect.apply(fn, null, args);
      this.enterWith(data);
      try {
        return Reflect.apply(fn, null, args);
      } finally {
        this.enterWith(prior);
      }
    }

    exit(fn, ...args) { return this.run(undefined, fn, ...args); }

    getStore() {
      const frame = AsyncContextFrame.current();
      if (frame === undefined || frame === null || !frame.has(this)) return this.#defaultValue;
      return frame.get(this);
    }

    withScope(store) { return new RunScope(this, store); }

    // node's async_hooks-backed ALS exposes these and mbun payloads call them.
    _enable() {}
    _propagate() {}
  }

  // ===========================================================================
  // mbun emission wiring — NOT node source.
  // node's C++ half emits init/before/after/destroy from every AsyncWrap. mbun
  // has no AsyncWrap, so the registration boundaries the JS runtime *does* own
  // are wired to the ported emit* functions here. This section is the analogue
  // of node's src/*_wrap.cc emission, not of any lib/ file.
  // ===========================================================================
  const kAsyncFrame = Symbol('mbun.asyncContextFrame');

  function captureContext(fn, frame = AsyncContextFrame.current()) {
    if (typeof fn !== 'function' || frame === undefined || fn[kAsyncFrame]) return fn;
    const wrapped = function (...args) {
      const prior = AsyncContextFrame.exchange(frame);
      try { return Reflect.apply(fn, this, args); }
      finally { AsyncContextFrame.set(prior); }
    };
    Object.defineProperty(wrapped, kAsyncFrame, { value: true });
    // EventEmitter.removeListener compares .listener with the public callback.
    Object.defineProperty(wrapped, 'listener', { value: fn.listener || fn, configurable: true });
    return wrapped;
  }

  // The full node resource lifecycle for one callback registration: an id and a
  // resource are allocated unconditionally (node's C++ AsyncWrap constructor does
  // the same), init/before/after/destroy are emitted only when a hook wants them,
  // and the id stack is always maintained so executionAsyncId() is correct inside
  // the callback whether or not hooks were enabled at registration time.
  function wrapAsync(fn, type, opts) {
    if (typeof fn !== 'function') return fn;
    const persistent = opts !== undefined && opts.persistent === true;
    const frame = AsyncContextFrame.current();
    const triggerAsyncId = getDefaultTriggerAsyncId();
    const asyncId = newAsyncId();
    const resource = (opts !== undefined && opts.resource !== undefined) ?
      opts.resource : { type };
    emitInitScript(asyncId, type, triggerAsyncId, resource);
    if (destroyHooksExist() && !persistent) registerDestroyHook(resource, asyncId);

    let destroyed = false;
    const run = function (...args) {
      emitBeforeScript(asyncId, triggerAsyncId, resource);
      const prior = AsyncContextFrame.exchange(frame);
      try {
        return Reflect.apply(fn, this, args);
      } finally {
        AsyncContextFrame.set(prior);
        if (hasAsyncIdStack()) emitAfterScript(asyncId);
        if (!persistent && !destroyed) {
          destroyed = true;
          emitDestroyScript(asyncId);
        }
      }
    };
    Object.defineProperty(run, kAsyncFrame, { value: true });
    Object.defineProperty(run, 'listener', { value: fn.listener || fn, configurable: true });
    return run;
  }

  // --- promise instrumentation ---------------------------------------------
  // node gets this from V8's PromiseHook. The subset reachable from JS is
  // creation through the Promise static factories and through `then` (where
  // every derived promise in a chain comes from), plus reaction execution.
  // `await` continuations are engine-created and stay invisible — the same seam
  // as the context frame above.
  function trackPromise(promise, parent) {
    if (promise[async_id_symbol]) return;
    const triggerAsyncId = parent ? getOrSetAsyncId(parent) : getDefaultTriggerAsyncId();
    promise[async_id_symbol] = newAsyncId();
    promise[trigger_async_id_symbol] = triggerAsyncId;
  }

  function promiseInitHook(promise, parent) {
    // The kInit check comes FIRST, before trackPromise. node's promise tracking
    // only exists while the V8 PromiseHook is installed (updatePromiseHookMode
    // on enable, disablePromiseHookIfNecessary on disable), so a promise created
    // with no hook listening is never given an id. Tracking unconditionally is
    // not a harmless extra: it writes two symbol-keyed properties onto every
    // promise in the process, which transitions the shape of an object the
    // engine otherwise keeps in one fixed structure. Measured, that single
    // inversion cost 4-5x on a 200k .then chain.
    if (!hasHooks(kInit)) return;
    if (promise === null || typeof promise !== 'object') return;
    // Promises this payload's own bookkeeping creates are not user resources.
    // mbun's queueMicrotask is promise-backed, so without this the destroy-queue
    // drain reported itself to user hooks as a PROMISE init.
    if (internalSchedule > 0) return;
    trackPromise(promise, parent);
    emitInitScript(promise[async_id_symbol], 'PROMISE',
      promise[trigger_async_id_symbol], promise, true);
  }

  function promiseResolveHook(promise) {
    if (!hasHooks(kPromiseResolve)) return;
    if (promise === null || typeof promise !== 'object') return;
    if (internalSchedule > 0) return;
    trackPromise(promise);
    emitPromiseResolveNative(promise[async_id_symbol], true);
  }

  // One shared, monomorphic reaction body. This is deliberately NOT a closure
  // per reaction: the bracket has to be installed on every `then` (see below),
  // so anything it allocates is paid by every promise in the process.
  //
  // Every decision here is made at REACTION time, not at registration time. A
  // hook enabled after a reaction was registered must still see before/after
  // for it — node gets that from V8's PromiseHook, which observes reactions it
  // did not itself register (test-async-wrap-promise-after-enabled,
  // test-async-hooks-enable-before-promise-resolve), and a hook enabled *during*
  // a reaction must still see that reaction's `after`
  // (test-async-hooks-enable-during-promise). So the bracket is unconditional
  // and everything expensive inside it is conditional.
  // Hooks came on during a reaction that started unobserved. node still emits
  // `after` with a lazily assigned id and skips the pop, because no matching
  // push was ever witnessed by the hook.
  function afterLateHook(derived) {
    if (derived === undefined || internalSchedule !== 0) return;
    if (hasHooks(kAfter)) {
      trackPromise(derived);
      emitAfterNative(derived[async_id_symbol], true);
    }
    promiseResolveHook(derived);
  }

  function runPromiseReaction(fn, derived, frame, thisArg, value) {
    const observed = active_hooks.array.length !== 0 && internalSchedule === 0 &&
      derived !== undefined;
    let asyncId = 0;
    if (observed) {
      trackPromise(derived);
      asyncId = derived[async_id_symbol];
      emitBeforeScript(asyncId, derived[trigger_async_id_symbol], derived, true);
    }
    // `frame === kEngineFrame` means the engine already restored the right frame
    // around this reaction (JSMicrotask.cpp), so touching the slot here would
    // only overwrite the engine's own answer — and each touch is a host call.
    const engineOwned = frame === kEngineFrame;
    const prior = engineOwned ? undefined : frameGet();
    if (!engineOwned) frameSet(frame);
    try {
      return fn.call(thisArg, value);
    } finally {
      if (!engineOwned) frameSet(prior);
      if (asyncId !== 0) {
        if (hasHooks(kAfter)) emitAfterNative(asyncId, true);
        // node's promiseAfterHook pops only if this id is still the current
        // one; it is not, if a hook was enabled mid-reaction.
        if (asyncId === async_id_fields[kExecutionAsyncId]) popAsyncContext(asyncId);
        promiseResolveHook(derived);
      } else if (active_hooks.array.length !== 0) {
        afterLateHook(derived);
      }
    }
  }

  function installPromiseInstrumentation() {
    const PromiseCtor = G.Promise;
    if (typeof PromiseCtor !== 'function') return;
    const proto = PromiseCtor.prototype;
    const nativeThen = proto.then;
    const nativeFinally = proto.finally;

    // A reaction runs with the DERIVED promise as the execution resource and the
    // promise it derives from as the trigger (node's promiseBeforeHook).
    //
    // The only per-call allocation is one closure per *function* reaction — no
    // holder object: both wrappers close over the same `derived` binding, which
    // nativeThen fills in before any reaction can run.
    proto.then = function then(onFulfilled, onRejected) {
      // Once the engine owns the frame it snapshots this registration itself and
      // restores it before the reaction runs, so there is nothing for JS to
      // carry — and asking for the current frame would be a host call on the
      // hottest path in the runtime. kEngineFrame records "the engine has this"
      // without reading the slot.
      const frame = engineContext !== null ? kEngineFrame : continuationData;
      // Zero-instrumentation path, and it is node's own structure rather than a
      // shortcut: node installs the V8 PromiseHook on enable
      // (updatePromiseHookMode) and removes it when the last hook goes away
      // (disablePromiseHookIfNecessary), so a promise created while nothing is
      // enabled is never instrumented there either. With no hook enabled and no
      // context frame to carry, the user's callbacks go straight to the engine,
      // so the reaction runs with no wrapper at all.
      //
      // The cost this avoids is not incidental. A bracket installed on every
      // `then` means one closure allocation and one extra JS call frame per
      // reaction, for every promise in the process: measured, 2.2x on a 200k
      // `.then` chain even after removing every other allocation from the path.
      // What it buys is only that a hook enabled AFTER a reaction was registered
      // can still observe it — which V8's PromiseHook gives node for free and
      // which nothing in JS can reproduce without the permanent wrapper. Three
      // corpus files want it (see the header); they are not worth a 2.2x tax on
      // every promise, so they stay failing and are recorded as such.
      // With the engine owning the frame this is reached for ALS code too, not
      // just for code with no frame at all: the engine propagates the store, so
      // an `als.run()` body no longer forces the instrumented path.
      if ((frame === undefined || frame === kEngineFrame) &&
          active_hooks.array.length === 0) {
        return nativeThen.call(this, onFulfilled, onRejected);
      }
      let derived;
      // The wrapper's own fast path: when nothing is observing and the frame it
      // captured is already the current one, there is no context to exchange and
      // no event to emit, so the reaction is invoked directly — no extra call
      // frame and no try/finally. The post-call re-check is what still catches a
      // hook enabled *during* the reaction
      // (test-async-hooks-enable-during-promise); it is deliberately not a
      // `finally`, because on the fast path there is nothing to unwind, and a
      // reaction that both throws and turns hooks on mid-flight is not a case
      // node's own ordering pins down.
      const wrappedFulfilled = typeof onFulfilled === 'function' ?
        function (value) {
          if (active_hooks.array.length === 0 &&
              (frame === kEngineFrame || continuationData === frame)) {
            const result = onFulfilled.call(this, value);
            if (active_hooks.array.length !== 0) afterLateHook(derived);
            return result;
          }
          return runPromiseReaction(onFulfilled, derived, frame, this, value);
        } : onFulfilled;
      const wrappedRejected = typeof onRejected === 'function' ?
        function (value) {
          if (active_hooks.array.length === 0 &&
              (frame === kEngineFrame || continuationData === frame)) {
            const result = onRejected.call(this, value);
            if (active_hooks.array.length !== 0) afterLateHook(derived);
            return result;
          }
          return runPromiseReaction(onRejected, derived, frame, this, value);
        } : onRejected;
      derived = nativeThen.call(this, wrappedFulfilled, wrappedRejected);
      if (async_hook_fields[kInit] !== 0) promiseInitHook(derived, this);
      return derived;
    };

    proto.catch = function (onRejected) { return this.then(undefined, onRejected); };

    proto.finally = function (onFinally) {
      const derived = Reflect.apply(nativeFinally, this, [captureContext(onFinally)]);
      promiseInitHook(derived, this);
      return derived;
    };

    // The static factories are ordinary functions, so wrapping them cannot
    // perturb promise identity the way replacing the constructor would. Both
    // take exactly one argument, so these take it by name: a rest parameter
    // would allocate an arguments array on a path that runs once per promise.
    // Unlike `then` there is nothing to bracket here, only creation to report,
    // so the whole body can be skipped when no hook wants it.
    const nativeResolve = PromiseCtor.resolve;
    if (typeof nativeResolve === 'function') {
      PromiseCtor.resolve = function resolve(value) {
        const p = nativeResolve.call(this, value);
        if (async_hook_fields[kInit] !== 0 || async_hook_fields[kPromiseResolve] !== 0) {
          promiseInitHook(p, undefined);
          promiseResolveHook(p);
        }
        return p;
      };
    }
    const nativeReject = PromiseCtor.reject;
    if (typeof nativeReject === 'function') {
      PromiseCtor.reject = function reject(reason) {
        const p = nativeReject.call(this, reason);
        if (async_hook_fields[kInit] !== 0 || async_hook_fields[kPromiseResolve] !== 0) {
          promiseInitHook(p, undefined);
          promiseResolveHook(p);
        }
        return p;
      };
    }
  }

  // --- bridges consumed by mbun's other payloads ---------------------------
  // Same names and shapes as before the port, so no other payload changes.
  function installBridges() {
    Object.defineProperty(G, '__mbunHasAsyncContext', {
      configurable: true, enumerable: false,
      value: () => AsyncContextFrame.current() !== undefined || enabledHooksExist(),
    });

    G.__mbunAsyncHookInit = (type, resource) => {
      const id = newAsyncId();
      emitInitScript(id, type, getDefaultTriggerAsyncId(), resource || { type });
      return id;
    };
    G.__mbunAsyncHookDestroy = (id) => { if (id !== undefined) emitDestroyScript(id); };

    Object.defineProperty(G, '__mbunAsyncHookWrap', {
      configurable: true, enumerable: false,
      value: (fn, type) => wrapAsync(fn, type),
    });

    // Timer lifecycle. node's Timeout/Immediate are real async resources whose id
    // is allocated at schedule time whether or not a hook is listening.
    const timerHooks = {
      init(resource, type) {
        // Internally scheduled bookkeeping is not an observable async resource.
        if (internalSchedule > 0) return undefined;
        const frame = AsyncContextFrame.current();
        const triggerAsyncId = getDefaultTriggerAsyncId();
        const asyncId = newAsyncId();
        emitInitScript(asyncId, type, triggerAsyncId, resource);
        return { frame, resource, asyncId, triggerAsyncId, destroyed: false };
      },
      run(token, callback, thisArg, args) {
        if (token === undefined) return Reflect.apply(callback, thisArg, args);
        emitBeforeScript(token.asyncId, token.triggerAsyncId, token.resource);
        const prior = AsyncContextFrame.exchange(token.frame);
        try {
          return Reflect.apply(callback, thisArg, args);
        } finally {
          AsyncContextFrame.set(prior);
          if (hasAsyncIdStack()) emitAfterScript(token.asyncId);
        }
      },
      destroy(token) {
        if (token === undefined || token.destroyed) return;
        token.destroyed = true;
        emitDestroyScript(token.asyncId);
      },
    };
    Object.defineProperty(G, '__mbunAsyncHookTimer', {
      configurable: true, enumerable: false, value: timerHooks,
    });

    if (typeof G.ReadableStream === 'function') {
      Object.defineProperty(G, '__mbunWrapStreamSource', {
        configurable: true, enumerable: false, writable: true,
        value: (source) => {
          if (!source || (typeof source !== 'object' && typeof source !== 'function')) return source;
          const frame = AsyncContextFrame.current();
          if (frame === undefined) return source;
          let wrapped = null;
          for (const name of ['start', 'pull', 'cancel']) {
            if (typeof source[name] !== 'function') continue;
            if (wrapped === null) {
              wrapped = Object.create(Object.getPrototypeOf(source));
              Object.defineProperties(wrapped, Object.getOwnPropertyDescriptors(source));
            }
            Object.defineProperty(wrapped, name, {
              value: captureContext(source[name], frame), configurable: true, writable: true,
            });
          }
          return wrapped === null ? source : wrapped;
        },
      });
    }
  }

  // --- callback-registration boundaries mbun owns --------------------------
  function installCallbackWiring() {
    // process.nextTick is a real async resource in node (TickObject), always —
    // which is what makes triggerAsyncId() inside a nested tick differ from the
    // top level.
    if (G.process && typeof G.process.nextTick === 'function') {
      const nativeNextTick = G.process.nextTick;
      const processRef = G.process;
      internalNextTick = (fn) => Reflect.apply(nativeNextTick, processRef, [fn]);
      G.process.nextTick = function nextTick(callback, ...args) {
        if (typeof callback !== 'function') return Reflect.apply(nativeNextTick, this, arguments);
        // Indexed copy, not `[wrapped, ...args]`: array-literal spread reads
        // `Array.prototype[Symbol.iterator]` at call time, so user code that
        // deleted the array iterator would break every nextTick after it.
        // node's own tick queue is written against primordials for this reason.
        const call = [wrapAsync(callback, 'TickObject')];
        for (let i = 0; i < args.length; i++) call[call.length] = args[i];
        return Reflect.apply(nativeNextTick, this, call);
      };
    }

    if (typeof G.queueMicrotask === 'function') {
      const nativeQueueMicrotask = G.queueMicrotask;
      internalQueueMicrotask = nativeQueueMicrotask;
      G.queueMicrotask = function queueMicrotask(callback) {
        if (typeof callback !== 'function') return nativeQueueMicrotask(callback);
        return nativeQueueMicrotask(wrapAsync(callback, 'Microtask'));
      };
    }

    const M = G.__mbunNativeModules || {};
    const eventsModule = M.events || M['node:events'];
    const eventPrototype = eventsModule && eventsModule.EventEmitter &&
      eventsModule.EventEmitter.prototype;
    if (eventPrototype) {
      const wrapListener = (name) => {
        const original = eventPrototype[name];
        if (typeof original !== 'function') return;
        eventPrototype[name] = function (...args) {
          args[1] = captureContext(args[1]);
          return Reflect.apply(original, this, args);
        };
      };
      // node requires EventEmitter.prototype.addListener === prototype.on.
      const onWasAlias = eventPrototype.on === eventPrototype.addListener;
      wrapListener('addListener');
      if (onWasAlias) eventPrototype.on = eventPrototype.addListener;
      else wrapListener('on');
      wrapListener('prependListener');
    }

    if (G.Bun && typeof G.Bun.serve === 'function') {
      const bunServe = G.Bun.serve;
      G.Bun.serve = function (options) {
        if (!options || typeof options !== 'object') return bunServe.apply(this, arguments);
        const next = Object.assign({}, options);
        for (const name of ['fetch', 'error']) {
          if (typeof options[name] === 'function') next[name] = captureContext(options[name]);
        }
        if (options.websocket && typeof options.websocket === 'object') {
          next.websocket = Object.assign({}, options.websocket);
          for (const name of ['open', 'message', 'close', 'drain', 'ping', 'pong']) {
            if (typeof options.websocket[name] === 'function') {
              next.websocket[name] = captureContext(options.websocket[name]);
            }
          }
        }
        return bunServe.call(this, next);
      };
    }

    if (G.Bun && typeof G.Bun.spawn === 'function') {
      const bunSpawn = G.Bun.spawn;
      G.Bun.spawn = function (command, options) {
        const opts = Array.isArray(command) ? options : command;
        const onExit = opts && typeof opts.onExit === 'function' ?
          captureContext(opts.onExit) : undefined;
        const proc = bunSpawn.apply(this, arguments);
        if (onExit && proc && proc.exited && typeof proc.exited.then === 'function') {
          proc.exited.then(
            (code) => onExit(proc, code, proc.signalCode ?? null, null),
            (error) => onExit(proc, proc.exitCode ?? null, proc.signalCode ?? null, error),
          );
        }
        return proc;
      };
    }
  }

  installPromiseInstrumentation();
  installBridges();
  installCallbackWiring();

  def(["async_hooks"], {
    AsyncLocalStorage,
    AsyncResource,
    createHook,
    executionAsyncId,
    triggerAsyncId,
    executionAsyncResource,
    asyncWrapProviders: Object.freeze({ __proto__: null, NONE: 0 }),
  });

  })();
)JS";

}  // namespace mbun::jsc::builtins::detail
