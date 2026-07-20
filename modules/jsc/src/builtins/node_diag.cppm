// node:diagnostics_channel payload partition. Faithful port of Bun's
// src/js/node/diagnostics_channel.ts (itself a port of Node lib/diagnostics_channel.js):
// a WeakRefMap of named channels that swaps a Channel instance to the
// ActiveChannel prototype while it has subscribers/bound stores (fast no-op
// publish otherwise), plus TracingChannel with traceSync/tracePromise/
// traceCallback. bindStore/runStores route data through AsyncLocalStorage.
//
// Also back-fills util.styleText (Node lib/util.js) onto the util module
// registered in bootstrap — the other util additions (parseArgs,
// stripVTControlCharacters, getSystemErrorName, MIMEType) already live there.
//
// NOTE: appended AFTER the master builtins IIFE, so this is a self-contained
// IIFE that re-binds G = globalThis and overwrites the diagnostics_channel stub
// registered in bootstrap. DEFERRED: safe-subscriber uncaughtException delivery
// (needs process 'uncaughtException' support) — publish still isolates throws.
export module mbun.jsc.js_builtins:node_diag;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeDiagJS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  if (!M) return;

  const reportError = (err) => {
    const p = G.process;
    const emit = () => { if (G.reportError) G.reportError(err); else throw err; };
    if (p && typeof p.nextTick === "function") p.nextTick(emit);
    else queueMicrotask(emit);
  };

  const argTypeError = (name, expected, value) => {
    const e = new TypeError('The "' + name + '" argument must be of type ' + expected +
      ". Received " + (value === null ? "null" : typeof value));
    e.code = "ERR_INVALID_ARG_TYPE";
    return e;
  };
  const validateFunction = (value, name) => {
    if (typeof value !== "function") throw argTypeError(name, "function", value);
  };

  // WeakReference: a WeakRef with an explicit incRef/decRef counter (Node's
  // internal WeakReference used to keep channels alive while referenced).
  class WeakReference extends WeakRef {
    #refs = 0;
    get() { return this.deref(); }
    incRef() { return ++this.#refs; }
    decRef() { return --this.#refs; }
  }

  // Channels map keyed by name -> WeakReference(channel); GC finalization is the
  // only time an entry is removed, so a resurrected subscribe still finds it.
  class WeakRefMap extends Map {
    #finalizers = new FinalizationRegistry((key) => { this.delete(key); });
    set(key, value) {
      this.#finalizers.register(value, key);
      return super.set(key, new WeakReference(value));
    }
    get(key) { const r = super.get(key); return r ? r.get() : undefined; }
    incRef(key) { const r = super.get(key); return r ? r.incRef() : undefined; }
    decRef(key) { const r = super.get(key); return r ? r.decRef() : undefined; }
  }

  const markActive = (channel) => {
    Object.setPrototypeOf(channel, ActiveChannel.prototype);
    channel._subscribers = [];
    channel._stores = new Map();
  };
  const maybeMarkInactive = (channel) => {
    if (!channel._subscribers.length && !channel._stores.size) {
      Object.setPrototypeOf(channel, Channel.prototype);
      channel._subscribers = undefined;
      channel._stores = undefined;
    }
  };

  const defaultTransform = (data) => data;
  const wrapStoreRun = (store, data, next, transform) => {
    return () => {
      let context;
      try { context = (transform || defaultTransform)(data); }
      catch (err) { reportError(err); return next(); }
      return store.run(context, next);
    };
  };

  class ActiveChannel {
    subscribe(subscription) {
      validateFunction(subscription, "subscription");
      this._subscribers.push(subscription);
      channels.incRef(this.name);
    }
    unsubscribe(subscription) {
      const index = this._subscribers.indexOf(subscription);
      if (index === -1) return false;
      this._subscribers.splice(index, 1);
      channels.decRef(this.name);
      maybeMarkInactive(this);
      return true;
    }
    bindStore(store, transform) {
      const replacing = this._stores.has(store);
      if (!replacing) channels.incRef(this.name);
      this._stores.set(store, transform);
    }
    unbindStore(store) {
      if (!this._stores.has(store)) return false;
      this._stores.delete(store);
      channels.decRef(this.name);
      maybeMarkInactive(this);
      return true;
    }
    get hasSubscribers() { return true; }
    publish(data) {
      for (let i = 0; i < (this._subscribers ? this._subscribers.length : 0); i++) {
        try { this._subscribers[i](data, this.name); }
        catch (err) { reportError(err); }
      }
    }
    runStores(data, fn, thisArg, ...args) {
      let run = () => { this.publish(data); return fn.apply(thisArg, args); };
      for (const [store, transform] of this._stores.entries())
        run = wrapStoreRun(store, data, run, transform);
      return run();
    }
    // withStoreScope(data): enters every bound store (transform(data)) and
    // publishes `data`, returning a disposable that restores the prior store
    // values on `using` scope exit. A throwing transform reports asynchronously
    // and leaves that store untouched (parity with runStores). ref: node
    // lib/diagnostics_channel.js Channel.withStoreScope.
    withStoreScope(data) {
      const restores = [];
      for (const [store, transform] of this._stores.entries()) {
        let context;
        try { context = (transform || defaultTransform)(data); }
        catch (err) { reportError(err); continue; }
        const prev = store.getStore();
        restores.push(() => store.enterWith(prev));
        store.enterWith(context);
      }
      this.publish(data);
      return { [Symbol.dispose]() { for (let i = restores.length - 1; i >= 0; i--) restores[i](); } };
    }
  }

  class Channel {
    constructor(name) {
      this._subscribers = undefined;
      this._stores = undefined;
      this.name = name;
      channels.set(name, this);
    }
    static [Symbol.hasInstance](instance) {
      const prototype = Object.getPrototypeOf(instance);
      return prototype === Channel.prototype || prototype === ActiveChannel.prototype;
    }
    subscribe(subscription) { markActive(this); this.subscribe(subscription); }
    unsubscribe() { return false; }
    bindStore(store, transform) { markActive(this); this.bindStore(store, transform); }
    unbindStore() { return false; }
    get hasSubscribers() { return false; }
    publish() {}
    runStores(data, fn, thisArg, ...args) { return fn.apply(thisArg, args); }
    withStoreScope() { return { [Symbol.dispose]() {} }; }
  }

  const channels = new WeakRefMap();

  const channel = (name) => {
    const existing = channels.get(name);
    if (existing) return existing;
    if (typeof name !== "string" && typeof name !== "symbol")
      throw argTypeError("channel", "string or symbol", name);
    return new Channel(name);
  };
  const subscribe = (name, subscription) => channel(name).subscribe(subscription);
  const unsubscribe = (name, subscription) => channel(name).unsubscribe(subscription);
  const hasSubscribers = (name) => {
    const c = channels.get(name);
    return c ? c.hasSubscribers : false;
  };

  const traceEvents = ["start", "end", "asyncStart", "asyncEnd", "error"];
  const assertChannel = (value, name) => {
    if (!(value instanceof Channel)) throw argTypeError(name, "Channel", value);
  };

  class TracingChannel {
    constructor(nameOrChannels) {
      if (typeof nameOrChannels === "string") {
        this.start = channel("tracing:" + nameOrChannels + ":start");
        this.end = channel("tracing:" + nameOrChannels + ":end");
        this.asyncStart = channel("tracing:" + nameOrChannels + ":asyncStart");
        this.asyncEnd = channel("tracing:" + nameOrChannels + ":asyncEnd");
        this.error = channel("tracing:" + nameOrChannels + ":error");
      } else if (nameOrChannels && typeof nameOrChannels === "object") {
        const { start, end, asyncStart, asyncEnd, error } = nameOrChannels;
        assertChannel(start, "nameOrChannels.start");
        assertChannel(end, "nameOrChannels.end");
        assertChannel(asyncStart, "nameOrChannels.asyncStart");
        assertChannel(asyncEnd, "nameOrChannels.asyncEnd");
        assertChannel(error, "nameOrChannels.error");
        this.start = start; this.end = end; this.asyncStart = asyncStart;
        this.asyncEnd = asyncEnd; this.error = error;
      } else {
        throw argTypeError("nameOrChannels", "string, object, or Channel", nameOrChannels);
      }
    }
    subscribe(handlers) {
      for (const name of traceEvents) {
        if (!handlers[name]) continue;
        if (this[name]) this[name].subscribe(handlers[name]);
      }
    }
    unsubscribe(handlers) {
      let done = true;
      for (const name of traceEvents) {
        if (!handlers[name]) continue;
        if (!(this[name] && this[name].unsubscribe(handlers[name]))) done = false;
      }
      return done;
    }
    traceSync(fn, context = {}, thisArg, ...args) {
      const { start, end, error } = this;
      return start.runStores(context, () => {
        try {
          const result = fn.apply(thisArg, args);
          context.result = result;
          return result;
        } catch (err) {
          context.error = err;
          error.publish(context);
          throw err;
        } finally {
          end.publish(context);
        }
      });
    }
    tracePromise(fn, context = {}, thisArg, ...args) {
      const { start, end, asyncStart, asyncEnd, error } = this;
      const reject = (err) => {
        context.error = err;
        error.publish(context);
        asyncStart.publish(context);
        asyncEnd.publish(context);
        return Promise.reject(err);
      };
      const resolve = (result) => {
        context.result = result;
        asyncStart.publish(context);
        asyncEnd.publish(context);
        return result;
      };
      return start.runStores(context, () => {
        try {
          let promise = fn.apply(thisArg, args);
          if (!(promise instanceof Promise)) promise = Promise.resolve(promise);
          return promise.then(resolve, reject);
        } catch (err) {
          context.error = err;
          error.publish(context);
          throw err;
        } finally {
          end.publish(context);
        }
      });
    }
    traceCallback(fn, position = -1, context = {}, thisArg, ...args) {
      const { start, end, asyncStart, asyncEnd, error } = this;
      function wrappedCallback(err, res) {
        if (err) { context.error = err; error.publish(context); }
        else { context.result = res; }
        const cbArgs = arguments;
        const self = this;
        asyncStart.runStores(context, function () {
          try { if (callback) return callback.apply(self, cbArgs); }
          finally { asyncEnd.publish(context); }
        });
      }
      const callback = args.at(position);
      validateFunction(callback, "callback");
      args.splice(position, 1, wrappedCallback);
      return start.runStores(context, () => {
        try { return fn.apply(thisArg, args); }
        catch (err) { context.error = err; error.publish(context); throw err; }
        finally { end.publish(context); }
      });
    }
  }

  const tracingChannel = (nameOrChannels) => new TracingChannel(nameOrChannels);

  // BoundedChannel: a start/end channel pair for scoped tracing. `run` publishes
  // start (binding start's stores) then end in a finally; `withScope` publishes
  // start and returns a disposable that publishes end on `using` exit. ref: node
  // lib/diagnostics_channel.js BoundedChannel.
  class BoundedChannel {
    constructor(nameOrChannels) {
      if (typeof nameOrChannels === "string") {
        this.start = channel("tracing:" + nameOrChannels + ":start");
        this.end = channel("tracing:" + nameOrChannels + ":end");
      } else if (nameOrChannels && typeof nameOrChannels === "object") {
        const { start, end } = nameOrChannels;
        assertChannel(start, "nameOrChannels.start");
        assertChannel(end, "nameOrChannels.end");
        this.start = start;
        this.end = end;
      } else {
        throw argTypeError("nameOrChannels", "string, object, or Channel", nameOrChannels);
      }
    }
    get hasSubscribers() { return this.start.hasSubscribers || this.end.hasSubscribers; }
    subscribe(handlers) {
      if (handlers.start) this.start.subscribe(handlers.start);
      if (handlers.end) this.end.subscribe(handlers.end);
    }
    unsubscribe(handlers) {
      let done = true;
      if (handlers.start && !this.start.unsubscribe(handlers.start)) done = false;
      if (handlers.end && !this.end.unsubscribe(handlers.end)) done = false;
      return done;
    }
    run(context = {}, fn, thisArg, ...args) {
      const end = this.end;
      return this.start.runStores(context, () => {
        try { return fn.apply(thisArg, args); }
        finally { end.publish(context); }
      });
    }
    withScope(context = {}) {
      const scope = this.start.withStoreScope(context);
      const end = this.end;
      let disposed = false;
      return { [Symbol.dispose]() {
        if (disposed) return;  // double dispose is a no-op (node parity)
        disposed = true;
        end.publish(context);
        scope[Symbol.dispose]();
      } };
    }
  }
  const boundedChannel = (nameOrChannels) => new BoundedChannel(nameOrChannels);

  const diagnostics_channel = {
    channel, hasSubscribers, subscribe, tracingChannel, unsubscribe, Channel,
    boundedChannel, BoundedChannel,
  };
  M["diagnostics_channel"] = diagnostics_channel;
  M["node:diagnostics_channel"] = diagnostics_channel;

  // ---- util.styleText back-fill (Node lib/util.js) ----
  const util = M["util"];
  if (util && typeof util.styleText !== "function") {
    const defaultFG = 39, defaultBG = 49;
    const colors = {
      reset: [0, 0], bold: [1, 22], dim: [2, 22], italic: [3, 23], underline: [4, 24],
      blink: [5, 25], inverse: [7, 27], hidden: [8, 28], strikethrough: [9, 29],
      doubleunderline: [21, 24], black: [30, defaultFG], red: [31, defaultFG],
      green: [32, defaultFG], yellow: [33, defaultFG], blue: [34, defaultFG],
      magenta: [35, defaultFG], cyan: [36, defaultFG], white: [37, defaultFG],
      bgBlack: [40, defaultBG], bgRed: [41, defaultBG], bgGreen: [42, defaultBG],
      bgYellow: [43, defaultBG], bgBlue: [44, defaultBG], bgMagenta: [45, defaultBG],
      bgCyan: [46, defaultBG], bgWhite: [47, defaultBG], framed: [51, 54],
      overlined: [53, 55], gray: [90, defaultFG], redBright: [91, defaultFG],
      greenBright: [92, defaultFG], yellowBright: [93, defaultFG],
      blueBright: [94, defaultFG], magentaBright: [95, defaultFG],
      cyanBright: [96, defaultFG], whiteBright: [97, defaultFG],
      bgGray: [100, defaultBG], bgRedBright: [101, defaultBG],
      bgGreenBright: [102, defaultBG], bgYellowBright: [103, defaultBG],
      bgBlueBright: [104, defaultBG], bgMagentaBright: [105, defaultBG],
      bgCyanBright: [106, defaultBG], bgWhiteBright: [107, defaultBG],
    };
    const aliases = { grey: "gray", blackBright: "gray", swapColors: "inverse",
      swapcolors: "inverse", conceal: "hidden", strikeThrough: "strikethrough",
      crossedout: "strikethrough", crossedOut: "strikethrough",
      doubleUnderline: "doubleunderline", bgGrey: "bgGray", bgBlackBright: "bgGray" };
    for (const a in aliases) colors[a] = colors[aliases[a]];
    const validateFormat = (key) => {
      if (colors[key] == null) {
        const e = new TypeError('The argument \'format\' must be one of: ' +
          Object.keys(colors).map((k) => "'" + k + "'").join(", ") + '. Received ' + String(key));
        e.code = "ERR_INVALID_ARG_VALUE";
        throw e;
      }
    };
    util.styleText = function styleText(format, text) {
      if (typeof text !== "string") throw argTypeError("text", "string", text);
      if (Array.isArray(format)) {
        let left = "", right = "";
        for (const key of format) {
          validateFormat(key);
          const codes = colors[key];
          left += "[" + codes[0] + "m";
          right = "[" + codes[1] + "m" + right;
        }
        return left + text + right;
      }
      validateFormat(format);
      const codes = colors[format];
      return "[" + codes[0] + "m" + text + "[" + codes[1] + "m";
    };
  }
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
