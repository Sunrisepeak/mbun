// Async context payload partition. Re-expressed from Bun's
// src/js/node/async_hooks.ts and AsyncContextFrame.cpp (MIT).
export module mbun.jsc.js_builtins:async_hooks;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kAsyncHooksJS = R"JS(
  // ---- async_hooks / AsyncLocalStorage ----
  // Bun stores an immutable [storage, value, ...] frame and snapshots it when
  // an async callback is registered. The Bun-patched JSC also snapshots native
  // Promise/await reactions. A bare C-API global lacks that private intrinsic,
  // so this layer covers every callback boundary exposed by mbun's JS runtime;
  // native async-function continuation tracking remains an engine seam.
  let asyncContext;
  const kAsyncFrame = Symbol("mbun.asyncContextFrame");

  const contextGet = () => asyncContext;
  const contextSet = (value) => { asyncContext = value; };
  const contextIndex = (context, storage) => {
    if (!context) return -1;
    for (let i = 0; i < context.length; i += 2) if (context[i] === storage) return i;
    return -1;
  };
  const contextWith = (context, storage, value) => {
    if (!context) return [storage, value];
    const next = context.slice();
    const index = contextIndex(next, storage);
    if (index < 0) next.push(storage, value); else next[index + 1] = value;
    return next;
  };
  const callInContext = (context, fn, thisArg, args) => {
    const previous = contextGet();
    contextSet(context);
    try { return Reflect.apply(fn, thisArg, args); }
    finally { contextSet(previous); }
  };
  const captureContext = (fn, context = contextGet()) => {
    if (typeof fn !== "function" || context === undefined || fn[kAsyncFrame]) return fn;
    const wrapped = function (...args) { return callInContext(context, fn, this, args); };
    Object.defineProperty(wrapped, kAsyncFrame, { value: true });
    // EventEmitter.removeListener compares .listener with the public callback.
    Object.defineProperty(wrapped, "listener", { value: fn.listener || fn, configurable: true });
    return wrapped;
  };

  // Keep the public async_hooks API backed by one lifecycle registry instead
  // of reporting success from createHook() while never delivering an event.
  // The C-API runtime cannot observe every JSC allocation, but it does own the
  // callback registration boundaries below and AsyncResource directly. Those
  // are real async resources, so account for them with the same id/context
  // transition rules Node exposes to user hooks.
  const activeHooks = new Set();
  // stream.finished() needs this decision at callback-registration time. Keep
  // the probe in the async-hooks owner, where both the ALS frame and active
  // hook set are authoritative.
  Object.defineProperty(G, "__mbunHasAsyncContext", {
    configurable: true, enumerable: false,
    value: () => contextGet() !== undefined || activeHooks.size !== 0,
  });
  let nextAsyncId = 1;
  // Node's bootstrap itself owns async id 1. Timers scheduled by user code at
  // top level therefore inherit triggerAsyncId 1 rather than the internal
  // sentinel 0.
  let executionId = 1;
  let executionResource;
  const hookCall = (name, ...args) => {
    for (const hook of Array.from(activeHooks)) {
      const callback = hook[name];
      if (typeof callback === "function") callback(...args);
    }
  };
  const newAsyncId = () => ++nextAsyncId;
  const runAsyncCallback = (id, resource, callback, thisArg, args) => {
    const previousId = executionId;
    const previousResource = executionResource;
    executionId = id;
    executionResource = resource;
    hookCall("before", id);
    try { return Reflect.apply(callback, thisArg, args); }
    finally {
      try { hookCall("after", id); }
      finally {
        executionId = previousId;
        executionResource = previousResource;
      }
    }
  };
  const captureAsyncCallback = (fn, type, context = contextGet(), destroyOnRun = true) => {
    if (typeof fn !== "function") return fn;
    const callback = captureContext(fn, context);
    if (activeHooks.size === 0) return callback;
    const id = newAsyncId();
    const resource = { type };
    hookCall("init", id, type, executionId, resource);
    return function (...args) {
      try { return runAsyncCallback(id, resource, callback, this, args); }
      finally {
        if (type === "PROMISE") hookCall("promiseResolve", id);
        if (destroyOnRun) hookCall("destroy", id);
      }
    };
  };

  // node_timers replaces the global scheduling functions after this payload
  // runs. Keeping timer lifecycle ownership here, but letting that final
  // wrapper register its returned facade, avoids attaching hooks to the stale
  // native handle (and makes the init resource the public Immediate/Timeout).
  const timerHooks = {
    init(resource, type) {
      if (activeHooks.size === 0) return undefined;
      const token = { id: newAsyncId(), resource, destroyed: false };
      hookCall("init", token.id, type, executionId, resource);
      return token;
    },
    run(token, callback, thisArg, args) {
      if (token === undefined) return Reflect.apply(callback, thisArg, args);
      return runAsyncCallback(token.id, token.resource, callback, thisArg, args);
    },
    destroy(token) {
      if (token === undefined || token.destroyed) return;
      token.destroyed = true;
      hookCall("destroy", token.id);
    },
  };
  Object.defineProperty(G, "__mbunAsyncHookTimer", {
    configurable: true, enumerable: false, value: timerHooks,
  });

  class AsyncLocalStorage {
    #disabled = false;
    // node >= 24 `new AsyncLocalStorage({ defaultValue })`: getStore() outside
    // any run()/enterWith() reports this instead of undefined.
    #defaultValue = undefined;

    constructor(options) {
      if (options !== null && options !== undefined && typeof options === "object") {
        this.#defaultValue = options.defaultValue;
      }
    }

    static bind(fn, ...args) {
      if (typeof fn !== "function") throw new TypeError('The "fn" argument must be of type function');
      return this.snapshot().bind(null, fn, ...args);
    }

    static snapshot() {
      const context = contextGet();
      return (fn, ...args) => {
        if (typeof fn !== "function") throw new TypeError('The "fn" argument must be of type function');
        return callInContext(context, fn, undefined, args);
      };
    }

    enterWith(store) {
      this.#disabled = false;
      contextSet(contextWith(contextGet(), this, store));
    }

    exit(callback, ...args) { return this.run(undefined, callback, ...args); }

    run(store, callback, ...args) {
      if (typeof callback !== "function") throw new TypeError('The "callback" argument must be of type function');
      const previous = contextGet();
      const wasDisabled = this.#disabled;
      this.#disabled = false;
      contextSet(contextWith(previous, this, store));
      try { return callback(...args); }
      finally { if (!wasDisabled) contextSet(previous); }
    }

    disable() {
      if (this.#disabled) return;
      this.#disabled = true;
      // Mutate the live context array in place (matching bun's disable()):
      // callback frames captured before disable() share this exact array ref,
      // so removing our storage here must invalidate their view too — even
      // after a later exit()/run() re-enables the #disabled flag. Slicing left
      // stale snapshots resolving to the pre-disable store.
      const context = contextGet();
      if (!context) return;
      const index = contextIndex(context, this);
      if (index < 0) return;
      context.splice(index, 2);
      contextSet(context.length ? context : undefined);
    }

    getStore() {
      if (this.#disabled) return this.#defaultValue;
      const context = contextGet();
      const index = contextIndex(context, this);
      return index < 0 ? this.#defaultValue : context[index + 1];
    }

    // node >= 24 `using scope = als.withScope(store)`: enters `store` and
    // restores the whole previous context on dispose (so an enterWith() made
    // inside the scope is undone too), idempotently.
    withScope(store) {
      const previous = contextGet();
      const wasDisabled = this.#disabled;
      this.#disabled = false;
      contextSet(contextWith(previous, this, store));
      let disposed = false;
      const scope = {
        dispose() {
          if (disposed) return;
          disposed = true;
          if (!wasDisabled) contextSet(previous);
        },
      };
      scope[Symbol.dispose] = scope.dispose;
      return scope;
    }

    _enable() {}
    _propagate() {}
  }

  class AsyncResource {
    #snapshot;
    #triggerAsyncId = 0;
    #asyncId = 0;
    #destroyed = false;
    #previousContexts = [];

    constructor(type, options) {
      if (typeof type !== "string") {
        const error = new TypeError('The "type" argument must be of type string');
        error.code = "ERR_INVALID_ARG_TYPE";
        throw error;
      }
      let triggerAsyncId = options;
      if (options != null && typeof options !== "number") triggerAsyncId = options.triggerAsyncId === undefined ? 1 : options.triggerAsyncId;
      if (options != null && (!Number.isSafeInteger(triggerAsyncId) || triggerAsyncId < -1)) {
        const error = new RangeError('The value of "triggerAsyncId" is out of range');
        error.code = "ERR_INVALID_ASYNC_ID";
        throw error;
      }
      this.type = type;
      // node echoes the constructor's triggerAsyncId back from
      // resource.triggerAsyncId(); with no option it is the current execution
      // async id.
      this.#triggerAsyncId = (options == null || (typeof options !== "number" && options.triggerAsyncId === undefined)) ? executionId : triggerAsyncId;
      this.#snapshot = contextGet();
      this.#asyncId = newAsyncId();
      hookCall("init", this.#asyncId, type, this.#triggerAsyncId, this);
    }

    emitBefore() {
      this.#previousContexts.push([executionId, executionResource]);
      executionId = this.#asyncId;
      executionResource = this;
      hookCall("before", this.#asyncId);
      return true;
    }
    emitAfter() {
      try { hookCall("after", this.#asyncId); }
      finally {
        const previous = this.#previousContexts.pop() || [0, undefined];
        executionId = previous[0];
        executionResource = previous[1];
      }
      return true;
    }
    asyncId() { return this.#asyncId; }
    triggerAsyncId() { return this.#triggerAsyncId; }
    emitDestroy() {
      if (!this.#destroyed) {
        this.#destroyed = true;
        hookCall("destroy", this.#asyncId);
      }
      return this;
    }
    runInAsyncScope(fn, thisArg, ...args) {
      if (typeof fn !== "function") throw new TypeError('The "fn" argument must be of type function');
      return runAsyncCallback(this.#asyncId, this, () => callInContext(this.#snapshot, fn, thisArg, args), undefined, []);
    }
    bind(fn, thisArg) {
      if (typeof fn !== "function") throw new TypeError('The "fn" argument must be of type function');
      return this.runInAsyncScope.bind(this, fn, thisArg ?? this);
    }
    static bind(fn, type, thisArg) {
      if (typeof fn !== "function") throw new TypeError('The "fn" argument must be of type function');
      return new AsyncResource(type || fn.name || "bound-anonymous-fn").bind(fn, thisArg);
    }
  }

  // Promise callbacks snapshot at reaction registration, matching Bun's
  // PromiseOperations.js contract for explicit then/catch/finally reactions.
  const promiseThen = Promise.prototype.then;
  const promiseFinally = Promise.prototype.finally;
  Promise.prototype.then = function (onFulfilled, onRejected) {
    const context = contextGet();
    return promiseThen.call(this,
      captureAsyncCallback(onFulfilled, "PROMISE", context),
      captureAsyncCallback(onRejected, "PROMISE", context));
  };
  Promise.prototype.catch = function (onRejected) { return this.then(undefined, onRejected); };
  Promise.prototype.finally = function (onFinally) {
    return promiseFinally.call(this, captureContext(onFinally, contextGet()));
  };

  const wrapCallbackApi = (owner, name, callbackIndex = 0) => {
    const original = owner && owner[name];
    if (typeof original !== "function") return;
    owner[name] = function (...args) {
      const persistent = name === "setInterval" || name === "addListener" ||
        name === "on" || name === "prependListener";
      args[callbackIndex] = captureAsyncCallback(args[callbackIndex], name, contextGet(), !persistent);
      return Reflect.apply(original, this, args);
    };
  };
  wrapCallbackApi(G, "queueMicrotask");
  if (G.process) wrapCallbackApi(G.process, "nextTick");

  // Native and JS-backed network/process objects in mbun surface callbacks via
  // EventEmitter. Preserve listener identity through wrapped.listener.
  const eventsModule = M.events || M["node:events"];
  const eventPrototype = eventsModule && eventsModule.EventEmitter && eventsModule.EventEmitter.prototype;
  if (eventPrototype) {
    // Wrap addListener once and re-alias on to it: node (and the event-emitter
    // test suite) require EventEmitter.prototype.addListener === prototype.on.
    const onWasAlias = eventPrototype.on === eventPrototype.addListener;
    wrapCallbackApi(eventPrototype, "addListener", 1);
    if (onWasAlias) eventPrototype.on = eventPrototype.addListener;
    else wrapCallbackApi(eventPrototype, "on", 1);
    wrapCallbackApi(eventPrototype, "prependListener", 1);
  }

  // Web streams retain their underlying-source algorithms and call them later,
  // so the constructing frame has to be snapshotted onto start/pull/cancel.
  //
  // This used to install a `class ContextReadableStream extends ReadableStream`
  // over globalThis.ReadableStream. That broke stream IDENTITY: the spec
  // algorithms build streams internally from ReadableStream.prototype (tee
  // branches, TransformStream.readable, pipeThrough results), so those were not
  // `instanceof globalThis.ReadableStream` and node's isReadableStream()
  // rejected them with "Received an instance of ReadableStream"; the subclass
  // prototype also had no own `locked`/`getReader`/… descriptors, which the
  // WHATWG surface tests inspect directly. Feed the wrapper through the one
  // ReadableStream class's own constructor seam instead — one class, one
  // prototype, identity preserved.
  if (typeof G.ReadableStream === "function") {
    Object.defineProperty(G, "__mbunWrapStreamSource", { configurable: true, enumerable: false, writable: true, value: (source) => {
      if (!source || (typeof source !== "object" && typeof source !== "function")) return source;
      const context = contextGet();
      if (context === undefined) return source;
      let wrapped = null;
      for (const name of ["start", "pull", "cancel"]) {
        if (typeof source[name] !== "function") continue;
        if (wrapped === null) {
          wrapped = Object.create(Object.getPrototypeOf(source));
          Object.defineProperties(wrapped, Object.getOwnPropertyDescriptors(source));
        }
        Object.defineProperty(wrapped, name, { value: captureContext(source[name], context), configurable: true, writable: true });
      }
      return wrapped === null ? source : wrapped;
    } });
  }

  // Server callbacks are retained by the native network backend. Snapshot the
  // registration frame exactly as Bun's ServerConfig does with
  // withAsyncContextIfNeeded(); the server implementation itself is unchanged.
  if (G.Bun && typeof G.Bun.serve === "function") {
    const bunServe = G.Bun.serve;
    G.Bun.serve = function (options) {
      if (!options || typeof options !== "object") return bunServe.apply(this, arguments);
      const context = contextGet();
      const next = Object.assign({}, options);
      for (const name of ["fetch", "error"]) {
        if (typeof options[name] === "function") next[name] = captureContext(options[name], context);
      }
      if (options.websocket && typeof options.websocket === "object") {
        next.websocket = Object.assign({}, options.websocket);
        for (const name of ["open", "message", "close", "drain", "ping", "pong"]) {
          if (typeof options.websocket[name] === "function") {
            next.websocket[name] = captureContext(options.websocket[name], context);
          }
        }
      }
      return bunServe.call(this, next);
    };
  }

  // Bun.spawn's onExit is a callback registration boundary. The current native
  // backend exposes completion as `exited`; bridge that without pretending the
  // still-missing worker/WebSocket/build backends exist.
  if (G.Bun && typeof G.Bun.spawn === "function") {
    const bunSpawn = G.Bun.spawn;
    G.Bun.spawn = function (command, options) {
      const opts = Array.isArray(command) ? options : command;
      const onExit = opts && typeof opts.onExit === "function" ? captureContext(opts.onExit) : undefined;
      const proc = bunSpawn.apply(this, arguments);
      if (onExit && proc && proc.exited && typeof proc.exited.then === "function") {
        proc.exited.then(
          (code) => onExit(proc, code, proc.signalCode ?? null, null),
          (error) => onExit(proc, proc.exitCode ?? null, proc.signalCode ?? null, error),
        );
      }
      return proc;
    };
  }

  const createHook = (hook) => {
    if (hook === null || typeof hook !== "object") {
      const error = new TypeError('The "hook" argument must be of type object');
      error.code = "ERR_INVALID_ARG_TYPE";
      throw error;
    }
    for (const name of ["init", "before", "after", "destroy", "promiseResolve"]) {
      if (hook[name] !== undefined && typeof hook[name] !== "function") {
        const error = new TypeError('The "hook.' + name + '" property must be of type function');
        error.code = "ERR_ASYNC_CALLBACK";
        throw error;
      }
    }
    let internalMarker;
    const setInternalHookState = (enabled) => {
      // internal/async_hooks.enabledHooksExist() owns a separate node-core
      // array. Keep its observable "some hook is active" state in step with
      // this public implementation without pretending to install native hooks.
      try {
        const internal = typeof G.require === "function" ? G.require("internal/async_hooks") : undefined;
        const arrays = internal && typeof internal.getHookArrays === "function" && internal.getHookArrays();
        if (!arrays) return;
        const hooks = arrays[0];
        if (enabled && internalMarker === undefined) {
          internalMarker = {};
          hooks.push(internalMarker);
        } else if (!enabled && internalMarker !== undefined) {
          const index = hooks.indexOf(internalMarker);
          if (index >= 0) hooks.splice(index, 1);
          internalMarker = undefined;
        }
      } catch { /* internal module unavailable during bootstrap */ }
    };
    return {
      enable() { activeHooks.add(hook); setInternalHookState(true); return this; },
      disable() { activeHooks.delete(hook); setInternalHookState(false); return this; },
    };
  };
  const asyncHooksModule = {
    AsyncLocalStorage, AsyncResource, createHook,
    executionAsyncId: () => executionId,
    triggerAsyncId: () => executionResource &&
      typeof executionResource.triggerAsyncId === "function"
      ? executionResource.triggerAsyncId()
      : (executionResource && executionResource.triggerAsyncId) || 0,
    executionAsyncResource: () => executionResource,
  };
  def(["async_hooks"], asyncHooksModule);
)JS";

}  // namespace mbun::jsc::builtins::detail
