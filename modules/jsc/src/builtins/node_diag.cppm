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

  // internal/errors.js ERR_INVALID_ARG_TYPE. `expected` is node's list of
  // acceptable types: entries in kTypes read as "of type x", CamelCase entries as
  // "an instance of X", and a lone "object" is promoted to the instance list when
  // there is another instance type (so `['string','object','TracingChannel']`
  // reads "of type string or an instance of TracingChannel or Object" — asserted
  // verbatim by test-diagnostics-channel-tracing-channel-args-types).
  const kTypes = ["string", "function", "number", "object", "Function", "Object", "boolean", "bigint", "symbol"];
  const classRegExp = /^([A-Z][a-z0-9]*)+$/;
  const formatList = (list, sep) => {
    if (list.length < 2) return list[0];
    if (list.length === 2) return list[0] + " " + sep + " " + list[1];
    return list.slice(0, -1).join(", ") + ", " + sep + " " + list[list.length - 1];
  };
  const determineSpecificType = (value) => {
    if (value === null) return "null";
    if (value === undefined) return "undefined";
    const t = typeof value;
    if (t === "object") {
      const ctor = value.constructor;
      return "an instance of " + ((ctor && ctor.name) || "Object");
    }
    if (t === "function") return "function " + (value.name || "");
    let repr;
    try { repr = t === "string" ? "'" + value + "'" : String(value); } catch (e) { repr = "..."; }
    if (repr.length > 28) repr = repr.slice(0, 25) + "...";
    return "type " + t + " (" + repr + ")";
  };
  const argTypeError = (name, expected, value) => {
    const list = Array.isArray(expected) ? expected : [expected];
    const types = [], instances = [], other = [];
    for (const v of list) {
      if (kTypes.indexOf(v) !== -1) types.push(String(v).toLowerCase());
      else if (classRegExp.exec(v) !== null) instances.push(v);
      else other.push(v);
    }
    if (instances.length > 0) {
      const pos = types.indexOf("object");
      if (pos !== -1) { types.splice(pos, 1); instances.push("Object"); }
    }
    let msg = "The " + (name.indexOf(".") !== -1 ? "\"" + name + "\" property " : "\"" + name + "\" argument ") + "must be ";
    if (types.length > 0) {
      msg += (types.length > 1 ? "one of type " : "of type ") + formatList(types, "or");
      if (instances.length > 0 || other.length > 0) msg += " or ";
    }
    if (instances.length > 0) {
      msg += "an instance of " + formatList(instances, "or");
      if (other.length > 0) msg += " or ";
    }
    if (other.length > 0) msg += other.length > 1 ? "one of " + formatList(other, "or") : other[0];
    msg += ". Received " + determineSpecificType(value);
    const e = new TypeError(msg);
    e.code = "ERR_INVALID_ARG_TYPE";
    // node's E()-generated errors carry the code in toString(), which is what
    // assert.throws(fn, /ERR_INVALID_ARG_TYPE/) matches against (assert compares
    // the RegExp with String(err)).
    Object.defineProperty(e, "toString", {
      value: function () { return this.name + " [ERR_INVALID_ARG_TYPE]: " + this.message; },
      writable: true, enumerable: false, configurable: true,
    });
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
    // node ActiveChannel: the subscriber list is copy-on-write. publish() walks
    // the array it captured, so a handler that unsubscribes itself (or anyone
    // else) must not mutate the list being iterated — an in-place splice made
    // publish() skip the following subscriber
    // (test-diagnostics-channel-sync-unsubscribe).
    subscribe(subscription) {
      validateFunction(subscription, "subscription");
      this._subscribers = this._subscribers.slice();
      this._subscribers.push(subscription);
      channels.incRef(this.name);
    }
    unsubscribe(subscription) {
      const index = this._subscribers.indexOf(subscription);
      if (index === -1) return false;
      this._subscribers = this._subscribers.slice(0, index).concat(this._subscribers.slice(index + 1));
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
      const subscribers = this._subscribers;
      for (let i = 0; i < (subscribers ? subscribers.length : 0); i++) {
        try { subscribers[i](data, this.name); }
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
      // node reaches ObjectGetPrototypeOf(undefined) here and lets V8's TypeError
      // out; the corpus matches that message verbatim
      // (test-diagnostics-channel-tracing-channel-args-types checks
      // `dc.tracingChannel({})`), and JSC words its own differently, so raise
      // node's text explicitly.
      if (instance === null || instance === undefined)
        throw new TypeError("Cannot convert undefined or null to object");
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
      throw argTypeError("channel", ["string", "symbol"], name);
    return new Channel(name);
  };
  const subscribe = (name, subscription) => channel(name).subscribe(subscription);
  const unsubscribe = (name, subscription) => channel(name).unsubscribe(subscription);
  const hasSubscribers = (name) => {
    const c = channels.get(name);
    return c ? c.hasSubscribers : false;
  };

  const boundedEvents = ["start", "end"];
  const assertChannel = (value, name) => {
    if (!(value instanceof Channel)) throw argTypeError(name, ["Channel"], value);
  };
  // node lib/diagnostics_channel.js channelFromMap: a name builds the
  // `tracing:<name>:<event>` channel, an object supplies the Channel directly.
  const channelFromMap = (nameOrChannels, name, className) => {
    if (typeof nameOrChannels === "string") return channel("tracing:" + nameOrChannels + ":" + name);
    if (typeof nameOrChannels === "object" && nameOrChannels !== null) {
      const c = nameOrChannels[name];
      assertChannel(c, "nameOrChannels." + name);
      return c;
    }
    throw argTypeError("nameOrChannels", ["string", "object", className], nameOrChannels);
  };
  const emitNonThenableWarning = (fn) => {
    const p = G.process;
    const msg = "tracePromise was called with the function '" + ((fn && fn.name) || "<anonymous>") +
      "', which returned a non-thenable.";
    if (p && typeof p.emitWarning === "function") p.emitWarning(msg);
  };

  // BoundedChannel: a start/end channel pair for scoped tracing. `withScope`
  // publishes start (binding start's stores) and returns a disposable that
  // publishes end; `run` wraps a call in one. ref: node
  // lib/diagnostics_channel.js BoundedChannel / BoundedChannelScope.
  class BoundedChannel {
    constructor(nameOrChannels) {
      for (let i = 0; i < boundedEvents.length; i++) {
        const eventName = boundedEvents[i];
        Object.defineProperty(this, eventName, {
          value: channelFromMap(nameOrChannels, eventName, "BoundedChannel"),
        });
      }
    }
    get hasSubscribers() { return this.start.hasSubscribers || this.end.hasSubscribers; }
    subscribe(handlers) {
      for (let i = 0; i < boundedEvents.length; i++) {
        const name = boundedEvents[i];
        if (!handlers[name]) continue;
        if (this[name]) this[name].subscribe(handlers[name]);
      }
    }
    unsubscribe(handlers) {
      let done = true;
      for (let i = 0; i < boundedEvents.length; i++) {
        const name = boundedEvents[i];
        if (!handlers[name]) continue;
        if (!(this[name] && this[name].unsubscribe(handlers[name]))) done = false;
      }
      return done;
    }
    // node BoundedChannelScope decides ONCE, at construction, whether the pair
    // has subscribers. A scope opened on an unsubscribed pair is inert: it
    // publishes neither start nor end, so a subscriber that arrives while the
    // scope is open sees nothing. That is the "early exit" the corpus asserts
    // for traceSync / tracePromise / traceCallback.
    withScope(context = {}) {
      if (!this.hasSubscribers) return { [Symbol.dispose]() {} };
      const end = this.end;
      const scope = this.start.withStoreScope(context);
      let disposed = false;
      return { [Symbol.dispose]() {
        if (disposed) return;  // double dispose is a no-op (node parity)
        disposed = true;
        end.publish(context);
        scope[Symbol.dispose]();
      } };
    }
    run(context = {}, fn, thisArg, ...args) {
      const scope = this.withScope(context);
      try { return fn.apply(thisArg, args); }
      finally { scope[Symbol.dispose](); }
    }
  }
  const boundedChannel = (nameOrChannels) => new BoundedChannel(nameOrChannels);

  // TracingChannel is two BoundedChannels — the call window (start/end) and the
  // continuation window (asyncStart/asyncEnd) — plus a plain error channel.
  // ref: node lib/diagnostics_channel.js TracingChannel.
  class TracingChannel {
    #callWindow;
    #continuationWindow;
    constructor(nameOrChannels) {
      if (typeof nameOrChannels === "string") {
        this.#callWindow = new BoundedChannel(nameOrChannels);
        this.#continuationWindow = new BoundedChannel({
          start: channel("tracing:" + nameOrChannels + ":asyncStart"),
          end: channel("tracing:" + nameOrChannels + ":asyncEnd"),
        });
      } else if (typeof nameOrChannels === "object") {
        this.#callWindow = new BoundedChannel({ start: nameOrChannels.start, end: nameOrChannels.end });
        this.#continuationWindow = new BoundedChannel({ start: nameOrChannels.asyncStart, end: nameOrChannels.asyncEnd });
      }
      Object.defineProperty(this, "error", {
        value: channelFromMap(nameOrChannels, "error", "TracingChannel"),
      });
    }
    get start() { return this.#callWindow.start; }
    get end() { return this.#callWindow.end; }
    get asyncStart() { return this.#continuationWindow.start; }
    get asyncEnd() { return this.#continuationWindow.end; }
    // Any of the five channels counts. Reporting only start/end made every
    // asyncStart-only subscriber invisible to the `hasSubscribers` gate the
    // built-in publishers (node:net, node:http) use.
    get hasSubscribers() {
      return this.#callWindow.hasSubscribers || this.#continuationWindow.hasSubscribers ||
        (this.error ? this.error.hasSubscribers : false);
    }
    subscribe(handlers) {
      if (handlers.start || handlers.end) {
        this.#callWindow.subscribe({ start: handlers.start, end: handlers.end });
      }
      if (handlers.asyncStart || handlers.asyncEnd) {
        this.#continuationWindow.subscribe({ start: handlers.asyncStart, end: handlers.asyncEnd });
      }
      if (handlers.error) this.error.subscribe(handlers.error);
    }
    unsubscribe(handlers) {
      let done = true;
      if (handlers.start || handlers.end) {
        if (!this.#callWindow.unsubscribe({ start: handlers.start, end: handlers.end })) done = false;
      }
      if (handlers.asyncStart || handlers.asyncEnd) {
        if (!this.#continuationWindow.unsubscribe({ start: handlers.asyncStart, end: handlers.asyncEnd })) done = false;
      }
      if (handlers.error) { if (!this.error.unsubscribe(handlers.error)) done = false; }
      return done;
    }
    traceSync(fn, context = {}, thisArg, ...args) {
      if (!this.hasSubscribers) return fn.apply(thisArg, args);
      const error = this.error;
      const scope = this.#callWindow.withScope(context);
      try {
        const result = fn.apply(thisArg, args);
        context.result = result;
        return result;
      } catch (err) {
        context.error = err;
        error.publish(context);
        throw err;
      } finally {
        scope[Symbol.dispose]();
      }
    }
    tracePromise(fn, context = {}, thisArg, ...args) {
      if (!this.hasSubscribers) {
        const bare = fn.apply(thisArg, args);
        if (bare == null || typeof bare.then !== "function") emitNonThenableWarning(fn);
        return bare;
      }
      const error = this.error;
      const continuationWindow = this.#continuationWindow;
      const reject = (err) => {
        context.error = err;
        error.publish(context);
        const s = continuationWindow.withScope(context);
        try { return Promise.reject(err); } finally { s[Symbol.dispose](); }
      };
      const resolve = (result) => {
        context.result = result;
        const s = continuationWindow.withScope(context);
        try { return result; } finally { s[Symbol.dispose](); }
      };
      const scope = this.#callWindow.withScope(context);
      try {
        const result = fn.apply(thisArg, args);
        // A non-thenable return is handed straight back (with a warning) and
        // never reaches the continuation window: there is no "after" point.
        if (result == null || typeof result.then !== "function") {
          emitNonThenableWarning(fn);
          context.result = result;
          return result;
        }
        // Custom thenables keep their own type: call .then() on the value.
        return result.then(resolve, reject);
      } catch (err) {
        context.error = err;
        error.publish(context);
        throw err;
      } finally {
        scope[Symbol.dispose]();
      }
    }
    traceCallback(fn, position = -1, context = {}, thisArg, ...args) {
      if (!this.hasSubscribers) return fn.apply(thisArg, args);
      const error = this.error;
      const continuationWindow = this.#continuationWindow;
      function wrappedCallback(err, res) {
        if (err) { context.error = err; error.publish(context); }
        else { context.result = res; }
        const s = continuationWindow.withScope(context);
        try { return callback.apply(this, arguments); }
        finally { s[Symbol.dispose](); }
      }
      const callback = args.at(position);
      validateFunction(callback, "callback");
      args.splice(position, 1, wrappedCallback);
      const scope = this.#callWindow.withScope(context);
      try {
        return fn.apply(thisArg, args);
      } catch (err) {
        context.error = err;
        error.publish(context);
        throw err;
      } finally {
        scope[Symbol.dispose]();
      }
    }
  }

  const tracingChannel = (nameOrChannels) => new TracingChannel(nameOrChannels);

  const diagnostics_channel = {
    channel, hasSubscribers, subscribe, tracingChannel, unsubscribe, Channel,
    boundedChannel, BoundedChannel,
  };
  M["diagnostics_channel"] = diagnostics_channel;
  M["node:diagnostics_channel"] = diagnostics_channel;

  // ---- util.styleText (Node lib/util.js) ----
  // Full translation, replacing a stand-in that (a) knew no hex colours and
  // (b) closed a nested style with the plain reset code instead of re-opening
  // the enclosing one, so `styleText('red', 'A' + styleText('blue','B') + 'C')`
  // printed C uncoloured.
  const util = M["util"];
  if (util) {
    const kEscape = "[";
    const kEscapeEnd = "m";
    const kBoldCode = 1, kDimCode = 2;
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
      swapcolors: "inverse", conceal: "hidden", faint: "dim", strikeThrough: "strikethrough",
      crossedout: "strikethrough", crossedOut: "strikethrough",
      doubleUnderline: "doubleunderline", bgGrey: "bgGray", bgBlackBright: "bgGray" };
    for (const a in aliases) colors[a] = colors[aliases[a]];
    // node exposes the palette as util.inspect.colors; validateOneOf reads its
    // own property names to build the "must be one of: …" message.
    if (typeof util.inspect === "function" && util.inspect.colors === undefined) {
      try { util.inspect.colors = colors; } catch (e) {}
    }
    // getStyleCache: precomputed open/close sequences. `keepClose` marks the
    // styles whose close code (22) is shared by bold and dim, so a nested one
    // must keep its own reset AND re-open the outer style.
    const styleCache = { __proto__: null };
    for (const key of Object.keys(colors)) {
      const codes = colors[key];
      if (!codes) continue;
      styleCache[key] = {
        openSeq: kEscape + codes[0] + kEscapeEnd,
        closeSeq: kEscape + codes[1] + kEscapeEnd,
        keepClose: codes[0] === kDimCode || codes[0] === kBoldCode,
      };
    }
    const hexColorRegExp = /^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6})$/;
    const hexStyleCache = new Map();
    const getHexStyle = (hex) => {
      const cached = hexStyleCache.get(hex);
      if (cached !== undefined) return cached;
      let h = hex.slice(1);
      if (h.length === 3) h = h[0] + h[0] + h[1] + h[1] + h[2] + h[2];
      const r = parseInt(h.slice(0, 2), 16), g = parseInt(h.slice(2, 4), 16), b = parseInt(h.slice(4, 6), 16);
      const style = {
        openSeq: kEscape + "38;2;" + r + ";" + g + ";" + b + kEscapeEnd,
        closeSeq: kEscape + "39" + kEscapeEnd,
      };
      if (hexStyleCache.size >= 100) hexStyleCache.delete(hexStyleCache.keys().next().value);
      hexStyleCache.set(hex, style);
      return style;
    };
    // node replaceCloseCode: every occurrence of the style's own close sequence
    // inside `str` (i.e. where a nested styleText ended) is replaced by this
    // style's OPEN sequence, so the enclosing colour resumes. A trailing close
    // at the very end of the string is left alone.
    const replaceCloseCode = (str, closeSeq, openSeq, keepClose) => {
      const closeLen = closeSeq.length;
      let index = str.indexOf(closeSeq);
      if (index === -1) return str;
      let result = "", lastIndex = 0;
      const replacement = keepClose ? closeSeq + openSeq : openSeq;
      do {
        const afterClose = index + closeLen;
        if (afterClose < str.length) { result += str.slice(lastIndex, index) + replacement; lastIndex = afterClose; }
        else break;
        index = str.indexOf(closeSeq, lastIndex);
      } while (index !== -1);
      return result + str.slice(lastIndex);
    };
    const inspectValueOf = (v) => {
      try { return typeof util.inspect === "function" ? util.inspect(v) : String(v); }
      catch (e) { return String(v); }
    };
    const invalidValue = (name, value, reason) => {
      const e = new TypeError("The " + (String(name).indexOf(".") !== -1 ? "property" : "argument") +
        " '" + name + "' " + reason + ". Received " + inspectValueOf(value));
      e.code = "ERR_INVALID_ARG_VALUE";
      return e;
    };
    const validateOneOfFormat = (key) => {
      throw invalidValue("format", key, "must be one of: " +
        Object.keys(colors).map((k) => "'" + k + "'").join(", "));
    };
    // isReadableStream / isWritableStream / isNodeStream, collapsed to the duck
    // typing those three share.
    const isStreamish = (s) =>
      s !== null && typeof s === "object" &&
      (typeof s.pipe === "function" || typeof s.write === "function" ||
       typeof s.getReader === "function" || typeof s.getWriter === "function" ||
       typeof s._read === "function" || typeof s._write === "function");
    // internal/util/colors shouldColorize.
    const shouldColorize = (stream) => {
      const env = (G.process && G.process.env) || {};
      if (env.FORCE_COLOR !== undefined) return env.FORCE_COLOR !== "0";
      if (env.NODE_DISABLE_COLORS !== undefined || env.NO_COLOR !== undefined) return false;
      return !!(stream && stream.isTTY &&
        (typeof stream.getColorDepth !== "function" || stream.getColorDepth() > 2));
    };
    util.styleText = function styleText(format, text, options) {
      const validateStream = (options === undefined || options === null || options.validateStream === undefined)
        ? true : options.validateStream;
      // Fast path: a single known format with stream validation turned off.
      if (!validateStream && typeof format === "string" && typeof text === "string") {
        if (format === "none") return text;
        const style = styleCache[format];
        if (style !== undefined) {
          return style.openSeq + replaceCloseCode(text, style.closeSeq, style.openSeq, style.keepClose) + style.closeSeq;
        }
        if (format[0] === "#") {
          let hexStyle = hexStyleCache.get(format);
          if (hexStyle === undefined && hexColorRegExp.exec(format) !== null) hexStyle = getHexStyle(format);
          if (hexStyle !== undefined) {
            return hexStyle.openSeq + replaceCloseCode(text, hexStyle.closeSeq, hexStyle.openSeq, false) + hexStyle.closeSeq;
          }
        }
      }
      if (typeof text !== "string") throw argTypeError("text", "string", text);
      if (options !== undefined && (options === null || typeof options !== "object" || Array.isArray(options)))
        throw argTypeError("options", "object", options);
      if (typeof validateStream !== "boolean") throw argTypeError("options.validateStream", "boolean", validateStream);

      let skipColorize;
      if (validateStream) {
        const stream = (options && options.stream !== undefined) ? options.stream : (G.process && G.process.stdout);
        if (!isStreamish(stream))
          throw argTypeError("stream", ["ReadableStream", "WritableStream", "Stream"], stream);
        skipColorize = !shouldColorize(stream);
      }

      const formatArray = Array.isArray(format) ? format : [format];
      let openCodes = "", closeCodes = "", processedText = text;
      for (const key of formatArray) {
        if (key === "none") continue;
        if (typeof key === "string" && key[0] === "#") {
          let hexStyle = hexStyleCache.get(key);
          if (hexStyle === undefined) {
            if (hexColorRegExp.exec(key) === null)
              throw invalidValue("format", key, "must be a valid hex color (#RGB or #RRGGBB)");
            if (skipColorize) continue;
            hexStyle = getHexStyle(key);
          } else if (skipColorize) continue;
          openCodes += hexStyle.openSeq;
          closeCodes = hexStyle.closeSeq + closeCodes;
          processedText = replaceCloseCode(processedText, hexStyle.closeSeq, hexStyle.openSeq, false);
          continue;
        }
        const style = styleCache[key];
        if (style === undefined) validateOneOfFormat(key);
        openCodes += style.openSeq;
        closeCodes = style.closeSeq + closeCodes;
        processedText = replaceCloseCode(processedText, style.closeSeq, style.openSeq, style.keepClose);
      }
      if (skipColorize) return text;
      return openCodes + processedText + closeCodes;
    };
  }
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
