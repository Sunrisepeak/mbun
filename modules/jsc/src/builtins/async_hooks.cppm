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

    constructor(type, options) {
      if (typeof type !== "string") throw new TypeError('The "type" argument must be of type string');
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
      // async id, which is always 0 in mbun.
      this.#triggerAsyncId = (options == null || (typeof options !== "number" && options.triggerAsyncId === undefined)) ? 0 : triggerAsyncId;
      this.#snapshot = contextGet();
    }

    emitBefore() { return true; }
    emitAfter() { return true; }
    asyncId() { return 0; }
    triggerAsyncId() { return this.#triggerAsyncId; }
    emitDestroy() {}
    runInAsyncScope(fn, thisArg, ...args) {
      if (typeof fn !== "function") throw new TypeError('The "fn" argument must be of type function');
      return callInContext(this.#snapshot, fn, thisArg, args);
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
    return promiseThen.call(this, captureContext(onFulfilled, context), captureContext(onRejected, context));
  };
  Promise.prototype.catch = function (onRejected) { return this.then(undefined, onRejected); };
  Promise.prototype.finally = function (onFinally) {
    return promiseFinally.call(this, captureContext(onFinally, contextGet()));
  };

  const wrapCallbackApi = (owner, name, callbackIndex = 0) => {
    const original = owner && owner[name];
    if (typeof original !== "function") return;
    owner[name] = function (...args) {
      args[callbackIndex] = captureContext(args[callbackIndex]);
      return Reflect.apply(original, this, args);
    };
  };
  wrapCallbackApi(G, "queueMicrotask");
  wrapCallbackApi(G, "setTimeout");
  wrapCallbackApi(G, "setInterval");
  wrapCallbackApi(G, "setImmediate");
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

  // Web streams retain their underlying-source algorithms and call them later.
  const NativeReadableStream = G.ReadableStream;
  if (typeof NativeReadableStream === "function") {
    class ContextReadableStream extends NativeReadableStream {
      constructor(source, strategy) {
        if (source && typeof source === "object") {
          const context = contextGet();
          const wrapped = Object.create(Object.getPrototypeOf(source));
          Object.defineProperties(wrapped, Object.getOwnPropertyDescriptors(source));
          for (const name of ["start", "pull", "cancel"]) {
            if (typeof source[name] === "function") Object.defineProperty(wrapped, name, { value: captureContext(source[name], context), configurable: true, writable: true });
          }
          super(wrapped, strategy);
        } else super(source, strategy);
      }
    }
    Object.defineProperty(ContextReadableStream, "name", { value: "ReadableStream" });
    G.ReadableStream = ContextReadableStream;
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

  let hasEnabledCreateHook = false;
  const createHook = (hook) => {
    if (hook === null || typeof hook !== "object") throw new TypeError('The "hook" argument must be of type object');
    for (const name of ["init", "before", "after", "destroy", "promiseResolve"]) {
      if (hook[name] !== undefined && typeof hook[name] !== "function") throw new TypeError('The "hook.' + name + '" property must be of type function');
    }
    return { enable() { hasEnabledCreateHook = true; return this; }, disable() { return this; } };
  };
  const asyncHooksModule = {
    AsyncLocalStorage, AsyncResource, createHook,
    executionAsyncId: () => 0, triggerAsyncId: () => 0,
    executionAsyncResource: () => G.process && G.process.stdin,
  };
  def(["async_hooks"], asyncHooksModule);
)JS";

}  // namespace mbun::jsc::builtins::detail
