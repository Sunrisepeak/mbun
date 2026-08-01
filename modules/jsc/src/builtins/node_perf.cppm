// node:perf_hooks + W3C Performance payload partition. Blueprint: bun-ref
// src/js/node/perf_hooks.ts + WHATWG/W3C High Resolution Time & User Timing.
//
// Covers: global `performance` (now/timeOrigin/mark/measure/getEntries[ByName|
// ByType]/clearMarks/clearMeasures/toJSON/markResourceTiming/eventLoopUtilization/
// nodeTiming), PerformanceEntry/Mark/Measure, PerformanceObserver +
// PerformanceObserverEntryList, PerformanceNodeTiming, createHistogram +
// monitorEventLoopDelay (RecordableHistogram), and the node perf constants.
//
// High-resolution clock is Bun.nanoseconds() (native steady_clock since process
// start, see core_bindings.inc), so performance.now() is ms-since-origin and no
// extra native binding is needed. timeOrigin is the wall clock captured once at
// module load. DEFERRED: real event-loop-delay sampling precision (sampled via a
// coarse timer), native shallow-memory growth for estimateShallowMemoryUsageOf
// (that helper is a bun:jsc gap), PerformanceResourceTiming, GC/HTTP/DNS entry
// types, and observer buffering flush semantics beyond mark/measure.
//
// NOTE: appended AFTER the master builtins IIFE closes (see image_closure), so
// this is a self-contained IIFE that re-binds G = globalThis and overrides the
// earlier perf_hooks stub (bootstrap) and performance stub (markdown_web).
export module mbun.jsc.js_builtins:node_perf;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodePerfJS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules || (G.__mbunNativeModules = {});
  const NS = () => (G.Bun && G.Bun.nanoseconds ? G.Bun.nanoseconds() : 0);

  const err = (code, msg) => { const e = new (code === "ERR_INVALID_ARG_TYPE" || code === "ERR_MISSING_ARGS" ? TypeError : RangeError)(msg); e.code = code; return e; };
  const errArgType = (name, expected, actual) =>
    err("ERR_INVALID_ARG_TYPE", 'The "' + name + '" argument must be ' +
        (Array.isArray(expected) ? "one of type " + expected.join(", ") : "of type " + expected) +
        ". Received " + (typeof actual));
  const errArgTypeValue = (name, expected, actual) => {
    const type = typeof actual;
    if (actual === null || actual === undefined)
      return err("ERR_INVALID_ARG_TYPE", 'The "' + name + '" argument must be of type ' + expected + ". Received " + actual);
    if (type === "function")
      return err("ERR_INVALID_ARG_TYPE", 'The "' + name + '" argument must be of type ' + expected + ". Received function " + (actual.name || ""));
    const inspected = type === "string" ? "'" + actual + "'" : String(actual);
    return err("ERR_INVALID_ARG_TYPE", 'The "' + name + '" argument must be of type ' +
      expected + ". Received type " + type + " (" + inspected + ")");
  };
  const errRange = (name, range, actual) =>
    err("ERR_OUT_OF_RANGE", 'The value of "' + name + '" is out of range. It must be ' + range + ". Received " + String(actual));

  // ── PerformanceEntry / Mark / Measure ─────────────────────────────────────
  // WebKit/WebCore expose PerformanceEntry, Performance and PerformanceMeasure as
  // non-constructible from user code ("Illegal constructor"). Internal creation
  // goes through a private construction token (kConstruct) so mark()/measure()
  // can still build them; the declared param lists are empty (`...args`) to match
  // the native `.length === 0`. PerformanceMark stays user-constructible (matches
  // WebCore, and performance.mark() uses it).
  const kConstruct = Symbol("PerformanceEntry.construct");
  // node passes `kEmptyObject` (a frozen null-prototype object) as the default
  // for every optional options bag, so a polluted `Object.prototype` cannot
  // inject option values. `options || {}` reads straight through the pollution.
  const kEmptyObject = Object.freeze(Object.create(null));
  // `detail` is a *structured clone* of the caller's value (WebCore/Node semantics).
  const cloneDetail = (d) => (d == null ? null : (G.structuredClone ? G.structuredClone(d) : d));
  class PerformanceEntry {
    constructor(...args) {
      if (args[0] !== kConstruct) throw new TypeError("Illegal constructor");
      const name = args[1], entryType = args[2], startTime = args[3], duration = args[4], detail = args[5];
      this.name = name;
      this.entryType = entryType;
      this.startTime = startTime;
      this.duration = duration;
      if (detail !== undefined) this.detail = detail;
    }
    toJSON() {
      return { name: this.name, entryType: this.entryType, startTime: this.startTime, duration: this.duration };
    }
  }
  // node's internal entry class for the runtime-specific timelines ("function",
  // "net", "dns", "gc", "http"). It is *named* but never exported; the only
  // observable differences from PerformanceEntry are the name and that toJSON()
  // carries `detail`. ref: node lib/internal/perf/performance_entry.js.
  class PerformanceNodeEntry extends PerformanceEntry {
    toJSON() {
      const o = { name: this.name, entryType: this.entryType, startTime: this.startTime, duration: this.duration };
      if (this.detail !== undefined) o.detail = this.detail;
      return o;
    }
  }
  class PerformanceMark extends PerformanceEntry {
    constructor(name, options) {
      const o = options === undefined || options === null ? kEmptyObject : options;
      super(kConstruct, String(name), "mark", typeof o.startTime === "number" ? o.startTime : perfNow(), 0,
            o.detail !== undefined ? cloneDetail(o.detail) : null);
    }
  }
  class PerformanceMeasure extends PerformanceEntry {
    constructor(...args) {
      if (args[0] !== kConstruct) throw new TypeError("Illegal constructor");
      const name = args[1], startTime = args[2], duration = args[3], detail = args[4];
      super(kConstruct, String(name), "measure", startTime, duration, detail !== undefined ? cloneDetail(detail) : null);
    }
  }

  // process_web advances this counter once per timer/check phase. Reuse that
  // loop-turn gauge for the uv_metrics_info-compatible Node surface instead of
  // exposing the property without a live event-loop value.
  function uvMetricsInfo() {
    const timers = G.__mbunTimers;
    const loopCount = timers && Number.isSafeInteger(timers.batch) ? timers.batch : 0;
    return { loopCount, events: 0, eventsWaiting: 0 };
  }

  // ── PerformanceNodeTiming ────────────────────────────────────────────────
  class PerformanceNodeTiming extends PerformanceEntry {
    constructor() {
      super(kConstruct, "node", "node", 0, 0);
      this.nodeStart = 0;
      this.v8Start = 0;
      this.bootstrapComplete = 0;
      this.environment = 0;
      this.loopStart = 1;
      this.loopExit = -1;
      this.idleTime = 1;
    }
    get startTime() { return this.nodeStart; }
    set startTime(_v) {}
    get duration() { return perfNow(); }
    set duration(_v) {}
    get uvMetricsInfo() { return uvMetricsInfo(); }
    toJSON() {
      return {
        name: "node", entryType: "node", startTime: this.startTime, duration: this.duration,
        nodeStart: this.nodeStart, v8Start: this.v8Start, bootstrapComplete: this.bootstrapComplete,
        environment: this.environment, loopStart: this.loopStart, loopExit: this.loopExit, idleTime: this.idleTime,
      };
    }
  }

  // ── PerformanceObserverEntryList ──────────────────────────────────────────
  class PerformanceObserverEntryList {
    constructor(entries) { this.__entries = entries.slice(); }
    getEntries() { return this.__entries.slice(); }
    getEntriesByName(name, type) {
      name = String(name);
      return this.__entries.filter((e) => e.name === name && (type === undefined || e.entryType === type));
    }
    getEntriesByType(type) { return this.__entries.filter((e) => e.entryType === type); }
  }

  // ── Performance ───────────────────────────────────────────────────────────
  const timeOrigin = Date.now() - NS() / 1e6;
  function perfNow() { return NS() / 1e6; }

  const buffer = [];
  const observers = new Set();
  const resourceTimingListeners = new Set();
  let resourceTimingBufferSize = 250;
  let resourceTimingOverflow = null;
  let resourceTimingBufferFullScheduled = false;

  function resolveTime(v) {
    if (typeof v === "number") return v;
    if (typeof v === "string") {
      for (let i = buffer.length - 1; i >= 0; i--) if (buffer[i].name === v) return buffer[i].startTime;
      const nt = performanceObj.nodeTiming;
      if (nt && v in nt && typeof nt[v] === "number") return nt[v];
      return 0;
    }
    return 0;
  }

  function notifyObservers(entry) {
    for (const obs of observers) {
      if (obs.__types.has(entry.entryType)) {
        obs.__pending.push(entry);
        if (!obs.__scheduled) {
          obs.__scheduled = true;
          queueMicrotask(() => {
            obs.__scheduled = false;
            if (obs.__pending.length === 0) return;
            const list = new PerformanceObserverEntryList(obs.__pending);
            obs.__pending = [];
            try { obs.__cb(list, obs); } catch (e) {}
          });
        }
      }
    }
  }

  function addEntry(entry) { buffer.push(entry); notifyObservers(entry); }
  function timelineEntries(entries) {
    return entries.slice().sort((a, b) => a.startTime - b.startTime);
  }
  function resourceEntries() { return buffer.filter((entry) => entry.entryType === "resource"); }
  function scheduleResourceTimingBufferFull() {
    if (resourceTimingBufferFullScheduled) return;
    resourceTimingBufferFullScheduled = true;
    queueMicrotask(() => {
      resourceTimingBufferFullScheduled = false;
      const event = { type: "resourcetimingbufferfull" };
      for (const listener of [...resourceTimingListeners]) {
        try { listener.call(performanceObj, event); } catch (_) {}
      }
      if (typeof performanceObj.onresourcetimingbufferfull === "function") {
        try { performanceObj.onresourcetimingbufferfull.call(performanceObj, event); } catch (_) {}
      }
      if (resourceTimingOverflow !== null && resourceEntries().length < resourceTimingBufferSize)
        addEntry(resourceTimingOverflow);
      resourceTimingOverflow = null;
    });
  }
  function addResourceTiming(timingInfo, requestedUrl) {
    const startTime = typeof timingInfo?.startTime === "number" ? timingInfo.startTime : 0;
    const endTime = typeof timingInfo?.endTime === "number" ? timingInfo.endTime : startTime;
    const entry = new PerformanceNodeEntry(kConstruct, String(requestedUrl), "resource", startTime, endTime - startTime);
    if (resourceEntries().length < resourceTimingBufferSize) addEntry(entry);
    else {
      if (resourceTimingOverflow === null) resourceTimingOverflow = entry;
      scheduleResourceTimingBufferFull();
    }
  }

  const performanceObj = {
    get timeOrigin() { return timeOrigin; },
    now() { return perfNow(); },
    mark(name, options) {
      const entry = new PerformanceMark(name, options);
      addEntry(entry);
      return entry;
    },
    measure(name, startOrOptions, endMark) {
      let start, end, detail;
      if (startOrOptions !== null && typeof startOrOptions === "object") {
        const o = startOrOptions;
        if (o.start !== undefined) start = resolveTime(o.start);
        if (o.end !== undefined) end = resolveTime(o.end);
        if (o.duration !== undefined) {
          if (start === undefined && end !== undefined) start = end - o.duration;
          else if (end === undefined && start !== undefined) end = start + o.duration;
        }
        detail = o.detail;
      } else {
        if (startOrOptions !== undefined) start = resolveTime(startOrOptions);
        if (endMark !== undefined) end = resolveTime(endMark);
      }
      if (start === undefined) start = 0;
      if (end === undefined) end = perfNow();
      const entry = new PerformanceMeasure(kConstruct, name, start, end - start, detail);
      addEntry(entry);
      return entry;
    },
    getEntries() { return timelineEntries(buffer); },
    getEntriesByName(name, type) {
      if (arguments.length === 0) throw err("ERR_MISSING_ARGS", 'The "name" argument must be specified');
      name = String(name);
      return timelineEntries(buffer.filter((e) => e.name === name && (type === undefined || e.entryType === type)));
    },
    getEntriesByType(type) {
      if (arguments.length === 0) throw err("ERR_MISSING_ARGS", 'The "type" argument must be specified');
      return timelineEntries(buffer.filter((e) => e.entryType === type));
    },
    clearMarks(name) {
      for (let i = buffer.length - 1; i >= 0; i--)
        if (buffer[i].entryType === "mark" && (name === undefined || buffer[i].name === String(name))) buffer.splice(i, 1);
    },
    clearMeasures(name) {
      for (let i = buffer.length - 1; i >= 0; i--)
        if (buffer[i].entryType === "measure" && (name === undefined || buffer[i].name === String(name))) buffer.splice(i, 1);
    },
    clearResourceTimings() {
      for (let i = buffer.length - 1; i >= 0; i--)
        if (buffer[i].entryType === "resource") buffer.splice(i, 1);
    },
    setResourceTimingBufferSize(maxSize) {
      if (typeof maxSize === "number" && Number.isFinite(maxSize) && maxSize >= 0)
        resourceTimingBufferSize = Math.trunc(maxSize);
    },
    markResourceTiming(timingInfo, requestedUrl) { addResourceTiming(timingInfo, requestedUrl); },
    addEventListener(type, listener) {
      if (type === "resourcetimingbufferfull" && typeof listener === "function") resourceTimingListeners.add(listener);
    },
    removeEventListener(type, listener) {
      if (type === "resourcetimingbufferfull") resourceTimingListeners.delete(listener);
    },
    eventLoopUtilization() { return { idle: 0, active: 0, utilization: 0 }; },
    onresourcetimingbufferfull: null,
    nodeTiming: new PerformanceNodeTiming(),
    toJSON() { return { timeOrigin, timing: {} }; },
    // bun/WebCore Performance::memoryCost() counts the buffered entries the
    // object owns. mbun keeps that buffer in this closure, so publish its cost
    // through the hook bun:jsc's estimateShallowMemoryUsageOf consults.
    [Symbol.for("mbun.memoryCost")]() {
      let n = 0;
      for (const e of buffer) n += 48 + (typeof e.name === "string" ? e.name.length * 2 : 0);
      return n;
    },
  };
  G.performance = performanceObj;

  // ── performance.timerify (perf_hooks) ─────────────────────────────────────
  // Wrap fn so each successful call publishes a "function" timeline entry
  // (name = fn.name, entryType "function", startTime, duration). A throw
  // bubbles without an entry. ref: node lib/internal/perf/timerify.js.
  function timerify(fn, options) {
    if (typeof fn !== "function") throw errArgType("fn", "function", fn);
    let histogram;
    if (options !== undefined) {
      if (typeof options !== "object" || options === null) throw errArgType("options", "Object", options);
      histogram = options.histogram;
      if (histogram !== undefined &&
          (typeof histogram !== "object" || histogram === null || typeof histogram.record !== "function")) {
        throw errArgType("options.histogram", "RecordableHistogram", histogram);
      }
    }
    function timerified(...args) {
      const start = perfNow();
      const result = new.target !== undefined ? Reflect.construct(fn, args) : fn.apply(this, args);
      const duration = perfNow() - start;
      if (histogram) histogram.record(Math.max(1, Math.round(duration * 1e6)));
      // node publishes the call's arguments both as `detail` and as indexed own
      // properties on the entry (lib/internal/perf/timerify.js).
      const entry = new PerformanceNodeEntry(kConstruct, fn.name, "function", start, duration, args);
      for (let i = 0; i < args.length; i++) entry[i] = args[i];
      addEntry(entry);
      return result;
    }
    // Null-prototype descriptors: a polluted `Object.prototype.get` would
    // otherwise turn these into "both accessors and a value" descriptors.
    Object.defineProperty(timerified, "length", { __proto__: null, value: fn.length, configurable: true });
    Object.defineProperty(timerified, "name", { __proto__: null, value: "timerified " + fn.name, configurable: true });
    return timerified;
  }
  performanceObj.timerify = timerify;

  // ── PerformanceObserver ───────────────────────────────────────────────────
  const SUPPORTED = ["mark", "measure", "function", "net", "http"];
  class PerformanceObserver {
    constructor(callback) {
      if (typeof callback !== "function") throw errArgType("callback", "function", callback);
      this.__cb = callback;
      this.__types = new Set();
      this.__pending = [];
      this.__scheduled = false;
    }
    static get supportedEntryTypes() { return SUPPORTED.slice(); }
    observe(options) {
      if (options === undefined) options = kEmptyObject;
      if (options === null || typeof options !== "object") throw errArgTypeValue("options", "object", options);
      const hasEntryTypes = options.entryTypes !== undefined;
      const hasType = options.type !== undefined;
      if (!hasEntryTypes && !hasType)
        throw err("ERR_MISSING_ARGS", 'The "options.entryTypes" and "options.type" arguments must be specified');
      if (hasEntryTypes && !Array.isArray(options.entryTypes))
        throw errArgType("options.entryTypes", "string[]", options.entryTypes);
      if (hasEntryTypes && hasType && options.entryTypes != null && options.type != null)
        throw err("ERR_INVALID_ARG_VALUE", 'The "options.entryTypes" argument cannot be set with "options.type" together');
      let types;
      if (Array.isArray(options.entryTypes)) types = options.entryTypes;
      else if (options.type !== undefined) types = [options.type];
      else types = [];
      if (Array.isArray(options.entryTypes)) this.__types = new Set();
      for (const t of types) if (SUPPORTED.indexOf(t) !== -1) this.__types.add(t);
      observers.add(this);
    }
    disconnect() { observers.delete(this); this.__types = new Set(); this.__pending = []; }
    takeRecords() { const r = this.__pending; this.__pending = []; return r; }
  }

  // ── runtime timelines (net) ───────────────────────────────────────────────
  // node only materialises the "net" timeline while an observer is subscribed
  // (lib/internal/perf/observe.js `hasObserver`), so the socket path pays
  // nothing in the common case. `net` calls this once a connection completes.
  function hasObserverFor(type) {
    for (const obs of observers) if (obs.__types.has(type)) return true;
    return false;
  }
  Object.defineProperty(G, "__mbunPerfNetEntry", {
    configurable: true, enumerable: false, writable: true,
    value: (name, startTime, detail) => {
      if (!hasObserverFor("net")) return;
      const start = typeof startTime === "number" ? startTime : perfNow();
      addEntry(new PerformanceNodeEntry(kConstruct, name, "net", start, perfNow() - start, detail));
    },
  });
  // lib/internal/perf/observe.js hasObserver / startPerf / stopPerf, as the
  // "http" timeline uses them: _http_server.js and _http_client.js stash a
  // `{ type, name, detail, startTime }` context on the message while an
  // observer is subscribed and turn it into an entry when the exchange ends.
  // Same pay-nothing-unless-observed contract as the net timeline above.
  Object.defineProperty(G, "__mbunPerfHasObserver", {
    configurable: true, enumerable: false, writable: true,
    value: (type) => hasObserverFor(type),
  });
  Object.defineProperty(G, "__mbunPerfStart", {
    configurable: true, enumerable: false, writable: true,
    value: (name, type, detail) => ({ name, type, detail, startTime: perfNow() }),
  });
  Object.defineProperty(G, "__mbunPerfStop", {
    configurable: true, enumerable: false, writable: true,
    value: (ctx, detail) => {
      if (!ctx || !hasObserverFor(ctx.type)) return;
      addEntry(new PerformanceNodeEntry(kConstruct, ctx.name, ctx.type, ctx.startTime,
        perfNow() - ctx.startTime, Object.assign({}, ctx.detail, detail)));
    },
  });

  // ── Histogram (RecordableHistogram) ───────────────────────────────────────
  const INIT_MIN_BIG = 9223372036854775807n;
  const INIT_MIN_NUM = Number(INIT_MIN_BIG); // 9223372036854776000
  const PCT_KEYS = [50, 75, 87.5, 93.75, 96.875, 98.4375, 99.21875, 99.609375, 99.8046875, 99.90234375, 99.951171875, 100];

  class Histogram {
    constructor(lowest, highest, figures) {
      this.__lowest = lowest;
      this.__highest = highest;
      this.__figures = figures;
      this.__reset();
    }
    __reset() {
      this.__values = [];
      this.__sorted = null;
      this.__count = 0;
      this.__sum = 0;
      this.__sumsq = 0;
      this.__min = INIT_MIN_NUM;
      this.__max = 0;
      this.__exceeds = 0;
      this.__prevDelta = 0;
    }
    __sortedValues() {
      if (this.__sorted === null) this.__sorted = this.__values.slice().sort((a, b) => a - b);
      return this.__sorted;
    }
    record(value) {
      if (typeof value === "bigint") value = Number(value);
      else if (typeof value !== "number") throw errArgType("value", "number", value);
      if (!Number.isFinite(value) || value < 1) throw errRange("value", ">= 1", value);
      value = Math.trunc(value);
      if (value > this.__highest) { this.__exceeds++; return; }
      this.__count++;
      this.__sum += value;
      this.__sumsq += value * value;
      this.__values.push(value);
      this.__sorted = null;
      if (value < this.__min || this.__count === 1) this.__min = value;
      if (value > this.__max) this.__max = value;
    }
    recordDelta() {
      const now = NS();
      if (this.__prevDelta !== 0) this.record(now - this.__prevDelta);
      this.__prevDelta = now;
    }
    add(other) {
      if (!(other instanceof Histogram)) throw errArgType("other", "Histogram", other);
      if (other.__count > 0) {
        this.__count += other.__count;
        this.__sum += other.__sum;
        this.__sumsq += other.__sumsq;
        for (const v of other.__values) this.__values.push(v);
        this.__sorted = null;
        if (other.__min < this.__min) this.__min = other.__min;
        if (other.__max > this.__max) this.__max = other.__max;
      }
      this.__exceeds += other.__exceeds;
    }
    reset() { this.__reset(); }
    __percentileValue(p) {
      if (typeof p !== "number" || Number.isNaN(p) || p <= 0 || p > 100) throw errRange("percentile", "> 0 && <= 100", p);
      if (this.__count === 0) return 0;
      const sorted = this.__sortedValues();
      let idx = Math.ceil((p / 100) * this.__count);
      if (idx < 1) idx = 1;
      if (idx > this.__count) idx = this.__count;
      return sorted[idx - 1];
    }
    percentile(p) { return this.__percentileValue(p); }
    percentileBigInt(p) { return BigInt(this.__percentileValue(p)); }
    __percentileMap() {
      const m = new Map();
      for (const k of PCT_KEYS) m.set(k, BigInt(this.__percentileValue(k)));
      return m;
    }
    get percentiles() { return this.__percentileMap(); }
    get percentilesBigInt() { return this.__percentileMap(); }
    get count() { return this.__count; }
    get countBigInt() { return BigInt(this.__count); }
    get min() { return this.__count === 0 ? INIT_MIN_NUM : this.__min; }
    get minBigInt() { return this.__count === 0 ? INIT_MIN_BIG : BigInt(this.__min); }
    get max() { return this.__max; }
    get maxBigInt() { return BigInt(this.__max); }
    get exceeds() { return this.__exceeds; }
    get exceedsBigInt() { return BigInt(this.__exceeds); }
    get mean() { return this.__count === 0 ? NaN : this.__sum / this.__count; }
    get stddev() {
      if (this.__count === 0) return NaN;
      const mean = this.__sum / this.__count;
      const variance = this.__sumsq / this.__count - mean * mean;
      return Math.sqrt(variance < 0 ? 0 : variance);
    }
    toJSON() {
      const pct = {};
      for (const k of PCT_KEYS) pct[k] = this.__percentileValue(k);
      return {
        count: this.count, min: this.min, max: this.max, mean: this.mean,
        exceeds: this.exceeds, stddev: this.stddev, percentiles: pct,
      };
    }
  }

  function normNum(v, name) {
    if (typeof v === "bigint") return Number(v);
    if (typeof v === "number") return v;
    throw errArgType(name, ["number", "bigint"], v);
  }

  function createHistogram(options) {
    const opts = options === undefined || options === null ? kEmptyObject : options;
    let lowest = 1, highest = Number.MAX_SAFE_INTEGER, figures = 3;
    if (opts.lowest !== undefined) lowest = normNum(opts.lowest, "options.lowest");
    if (opts.highest !== undefined) highest = normNum(opts.highest, "options.highest");
    if (opts.figures !== undefined) {
      if (typeof opts.figures !== "number") throw errArgType("options.figures", "number", opts.figures);
      if (opts.figures < 1 || opts.figures > 5) throw errRange("options.figures", ">= 1 && <= 5", opts.figures);
      figures = opts.figures;
    }
    if (lowest < 1) throw errRange("options.lowest", ">= 1 && <= 9007199254740991", lowest);
    if (highest < 2 * lowest) throw errRange("options.highest", ">= " + 2 * lowest + " && <= 9007199254740991", highest);
    return new Histogram(lowest, highest, figures);
  }

  // ── monitorEventLoopDelay (IntervalHistogram) ─────────────────────────────
  class ELDHistogram extends Histogram {
    constructor(resolution) {
      super(1, Number.MAX_SAFE_INTEGER, 3);
      this.__resolution = resolution;
      this.__timer = null;
      this.__last = 0;
    }
    enable() {
      if (this.__timer !== null) return false;
      this.__last = NS();
      this.__timer = setInterval(() => {
        const now = NS();
        const expected = this.__resolution * 1e6;
        const delay = now - this.__last - expected;
        this.__last = now;
        if (delay > 0) { try { this.record(delay); } catch (e) {} }
      }, this.__resolution);
      if (this.__timer && typeof this.__timer.unref === "function") this.__timer.unref();
      return true;
    }
    disable() {
      if (this.__timer === null) return false;
      clearInterval(this.__timer);
      this.__timer = null;
      return true;
    }
  }

  function monitorEventLoopDelay(options) {
    const opts = options === undefined || options === null ? kEmptyObject : options;
    let resolution = 10;
    if (opts.resolution !== undefined) {
      if (typeof opts.resolution !== "number") throw errArgType("options.resolution", "number", opts.resolution);
      if (opts.resolution < 1) throw errRange("options.resolution", ">= 1", opts.resolution);
      resolution = opts.resolution;
    }
    return new ELDHistogram(resolution);
  }

  function eventLoopUtilization(_a, _b) { return { idle: 0, active: 0, utilization: 0 }; }

  const constants = {
    NODE_PERFORMANCE_ENTRY_TYPE_DNS: 4,
    NODE_PERFORMANCE_ENTRY_TYPE_GC: 0,
    NODE_PERFORMANCE_ENTRY_TYPE_HTTP: 1,
    NODE_PERFORMANCE_ENTRY_TYPE_HTTP2: 2,
    NODE_PERFORMANCE_ENTRY_TYPE_NET: 3,
    NODE_PERFORMANCE_GC_FLAGS_ALL_AVAILABLE_GARBAGE: 16,
    NODE_PERFORMANCE_GC_FLAGS_ALL_EXTERNAL_MEMORY: 32,
    NODE_PERFORMANCE_GC_FLAGS_CONSTRUCT_RETAINED: 2,
    NODE_PERFORMANCE_GC_FLAGS_FORCED: 4,
    NODE_PERFORMANCE_GC_FLAGS_NO: 0,
    NODE_PERFORMANCE_GC_FLAGS_SCHEDULE_IDLE: 64,
    NODE_PERFORMANCE_GC_FLAGS_SYNCHRONOUS_PHANTOM_PROCESSING: 8,
    NODE_PERFORMANCE_GC_INCREMENTAL: 8,
    NODE_PERFORMANCE_GC_MAJOR: 4,
    NODE_PERFORMANCE_GC_MINOR: 1,
    NODE_PERFORMANCE_GC_WEAKCB: 16,
    NODE_PERFORMANCE_MILESTONE_BOOTSTRAP_COMPLETE: 7,
    NODE_PERFORMANCE_MILESTONE_ENVIRONMENT: 2,
    NODE_PERFORMANCE_MILESTONE_LOOP_EXIT: 6,
    NODE_PERFORMANCE_MILESTONE_LOOP_START: 5,
    NODE_PERFORMANCE_MILESTONE_NODE_START: 3,
    NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN_TIMESTAMP: 0,
    NODE_PERFORMANCE_MILESTONE_TIME_ORIGIN: 1,
    NODE_PERFORMANCE_MILESTONE_V8_START: 4,
  };

  // Expose the W3C constructors on the global (Node/Bun do this).
  // `Performance` is not user-constructible in WebKit/Bun ("Illegal
  // constructor"); the class body declares no params so `Performance.length`
  // is 0, matching the native interface.
  class Performance { constructor() { throw new TypeError("Illegal constructor"); } }
  G.Performance = Performance;
  G.PerformanceEntry = PerformanceEntry;
  G.PerformanceMark = PerformanceMark;
  G.PerformanceMeasure = PerformanceMeasure;
  G.PerformanceObserver = PerformanceObserver;
  G.PerformanceObserverEntryList = PerformanceObserverEntryList;
  // WebKit exposes the Resource Timing / legacy Navigation Timing interfaces as
  // globals even in a server runtime (web-globals.test.js asserts they exist).
  // We surface only the constructors — no live timing data is produced.
  if (typeof G.PerformanceResourceTiming === "undefined") { class PerformanceResourceTiming extends PerformanceEntry {} G.PerformanceResourceTiming = PerformanceResourceTiming; }
  if (typeof G.PerformanceServerTiming === "undefined") { class PerformanceServerTiming {} G.PerformanceServerTiming = PerformanceServerTiming; }
  if (typeof G.PerformanceTiming === "undefined") { class PerformanceTiming {} G.PerformanceTiming = PerformanceTiming; }

  const mod = {
    performance: performanceObj,
    constants,
    Performance: G.Performance,
    PerformanceEntry,
    PerformanceMark,
    PerformanceMeasure,
    PerformanceObserver,
    PerformanceObserverEntryList,
    PerformanceNodeTiming,
    // node exports the Resource Timing interface from perf_hooks too; the
    // internal PerformanceNodeEntry class is deliberately *not* exported.
    PerformanceResourceTiming: G.PerformanceResourceTiming,
    monitorEventLoopDelay,
    createHistogram,
    eventLoopUtilization,
    timerify,
  };
  M["perf_hooks"] = mod;
  M["node:perf_hooks"] = mod;
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
