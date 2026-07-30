// node:process extras + EventTarget/Event web shim partition.
//
// Fills the pure-JS gaps that compat/bun/test/js/node/process/process.test.js and
// compat/bun/test/js/node/events/event-emitter.test.ts exercise:
//   - global EventTarget / Event / CustomEvent shim (addEventListener with
//     once/signal, dispatchEvent, getEventListeners interop via a
//     non-enumerable listeners() probe used by events.getEventListeners).
//   - process.uptime / constrainedMemory / _kill / execArgv
//   - process.umask + process.cpuUsage argument validation (node error text)
//   - process.exitCode setter validation (ERR_INVALID_ARG_TYPE / OUT_OF_RANGE)
//   - process.binding allow/deny list (node's _binding whitelist as bun keeps)
//   - node "undefined stubs" (_debugEnd, _rawDebug, _tickCallback, ...),
//     array stubs (getActiveResourcesInfo, _getActiveRequests, ...) and
//     moduleLoadList/_preload_modules empty arrays
//   - set/hasUncaughtExceptionCaptureCallback registry
//   - process.isBun / release.sourceUrl / config.variables / versions dep
//     pins (mirrors bun's scripts/build/deps commits, which the bun test
//     suite asserts verbatim)
//
// Blueprint: bun src/js/node/process.ts + node lib/internal/process/*.
//
// NOTE: appended AFTER the master builtins IIFE; self-contained IIFE, must
// never throw at top level (a throw would silently disable later partitions).
export module mbun.jsc.js_builtins:node_process_extra;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeProcessExtraJS = R"JS(
(function () {
  const G = globalThis;

  // ------------------------------------------------ EventTarget / Event shim
  try {
    if (typeof G.EventTarget !== "function") {
      const kStopImmediate = Symbol("kStopImmediate");
      const kCancelBubble = Symbol("kCancelBubble");
      // Kept in the global symbol registry because the Node-facing `events`
      // and stream partitions are installed in separate builtin payloads.
      const kResistStopPropagation = Symbol.for("nodejs.event_target.resist_stop_propagation");
      const isTrustedGet = function isTrusted() { return false; };
      class Event {
        get [Symbol.toStringTag]() { return "Event"; }
        // WHATWG cancelBubble: setter latches true only on a truthy value; getter is boolean.
        get cancelBubble() { return !!this[kCancelBubble]; }
        set cancelBubble(v) { if (v) this[kCancelBubble] = true; }
        constructor(type, options) {
          if (arguments.length === 0) throw new TypeError("1 argument required, but only 0 present.");
          options = options || {};
          // Template-literal coercion throws for Symbol type (WHATWG Event spec),
          // unlike String() which stringifies it.
          this.type = `${type}`;
          this.bubbles = !!options.bubbles;
          this.cancelable = !!options.cancelable;
          this.composed = !!options.composed;
          this.defaultPrevented = false;
          this[kCancelBubble] = false;
          this.returnValue = true;
          this.eventPhase = 0;
          // WebKit [LegacyUnforgeable]: isTrusted is a shared accessor, not a
          // per-instance data property (deno/event eventIsTrusted asserts the
          // own-descriptor .get is a function).
          Object.defineProperty(this, "isTrusted", { get: isTrustedGet, enumerable: true, configurable: true });
          this.target = null;
          this.currentTarget = null;
          this.srcElement = null;
          this.timeStamp = (G.performance && G.performance.now && G.performance.now()) || 0;
          this[kStopImmediate] = false;
        }
        preventDefault() { if (this.cancelable) { this.defaultPrevented = true; this.returnValue = false; } }
        stopPropagation() { this.cancelBubble = true; }
        stopImmediatePropagation() { this.cancelBubble = true; this[kStopImmediate] = true; }
        composedPath() { return this.currentTarget ? [this.currentTarget] : []; }
      }
      Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;
      Object.assign(Event.prototype, { NONE: 0, CAPTURING_PHASE: 1, AT_TARGET: 2, BUBBLING_PHASE: 3 });

      class CustomEvent extends Event {
        get [Symbol.toStringTag]() { return "CustomEvent"; }
        constructor(type, options = undefined) {
          if (arguments.length === 0) throw new TypeError("Failed to construct 'CustomEvent': 1 argument required, but only 0 present.");
          if (options !== undefined && options !== null && typeof options !== "object") {
            const e = new TypeError('The "options" argument must be of type object.'); e.code = "ERR_INVALID_ARG_TYPE"; throw e;
          }
          super(type, options);
          // WHATWG CustomEvent exposes `detail` as a readonly accessor.
          const detail = (options && options.detail) !== undefined ? options.detail : null;
          Object.defineProperty(this, "detail", { get() { return detail; }, enumerable: true, configurable: true });
        }
      }

      const kListeners = Symbol("kEventTargetListeners");
      class EventTarget {
        get [Symbol.toStringTag]() { return "EventTarget"; }
        constructor() { Object.defineProperty(this, kListeners, { value: new Map(), writable: true }); }
        addEventListener(type, callback, options) {
          if (arguments.length < 2) throw new TypeError("2 arguments required");
          if (!this || !this[kListeners]) throw new TypeError("Illegal invocation");  // WebIDL brand check
          if (callback === null || callback === undefined) return;
          if (typeof callback !== "function" && typeof callback !== "object") throw new TypeError("callback must be an object or function");
          type = String(type);
          if (typeof options === "boolean") options = { capture: options };
          options = options || {};
          const capture = !!options.capture;
          let list = this[kListeners].get(type);
          if (!list) { list = []; this[kListeners].set(type, list); }
          for (const l of list) if (l.callback === callback && l.capture === capture) return;
          const rec = { callback, capture, once: !!options.once, passive: !!options.passive, resistStopPropagation: !!options[kResistStopPropagation], removed: false };
          if (options.signal !== undefined) {
            // WebIDL: `signal` is an AbortSignal, so anything else (including
            // null and a bare object with the right shape) is a TypeError.
            if (!(G.AbortSignal && options.signal instanceof G.AbortSignal))
              throw new TypeError("The 'signal' option must be an AbortSignal");
            if (options.signal.aborted) return;
            const target = this;
            // The abort algorithm is owned by the *listener*: whichever path
            // removes the listener (removeEventListener, `once` firing, abort
            // itself) must also drop it from the signal, otherwise a long-lived
            // signal accumulates dead closures forever
            // (WebCore RegisteredEventListener::m_abortAlgorithm).
            const onAbort = () => target.removeEventListener(type, callback, { capture });
            rec.signal = options.signal;
            rec.onAbort = onAbort;
            options.signal.addEventListener("abort", onAbort, { once: true, [kResistStopPropagation]: true });
          }
          list.push(rec);
        }
        removeEventListener(type, callback, options) {
          type = String(type);
          if (typeof options === "boolean") options = { capture: options };
          const capture = !!(options && options.capture);
          const list = this[kListeners].get(type);
          if (!list) return;
          for (let i = 0; i < list.length; i++) {
            if (list[i].callback === callback && list[i].capture === capture) {
              const rec = list[i];
              list.splice(i, 1);
              // DOM dispatch iterates a snapshot of the listener list but must
              // skip any entry removed while the event is in flight (a listener
              // that aborts the shared signal must not let the next one run).
              rec.removed = true;
              if (rec.signal && rec.onAbort) {
                const sig = rec.signal, onAbort = rec.onAbort;
                rec.signal = null; rec.onAbort = null;  // re-entrancy: abort → here
                try { sig.removeEventListener("abort", onAbort); } catch (e) {}
              }
              break;
            }
          }
          if (list.length === 0) this[kListeners].delete(type);
        }
        dispatchEvent(event) {
          if (!event || typeof event.type !== "string") throw new TypeError("Argument 1 does not implement interface Event.");
          event.target = event.currentTarget = event.srcElement = this;
          event.eventPhase = 2;
          const list = this[kListeners].get(event.type);
          if (list) {
            for (const rec of list.slice()) {
              // `events.addAbortListener()` and `events.once(..., { signal })`
              // register protected cleanup listeners. An ordinary listener may
              // stop its peers, but it must not suppress those abort handlers.
              if (event[kStopImmediate] && !rec.resistStopPropagation) continue;
              if (rec.removed) continue;
              if (rec.once) this.removeEventListener(event.type, rec.callback, { capture: rec.capture });
              try {
                if (typeof rec.callback === "function") rec.callback.call(this, event);
                else if (rec.callback && typeof rec.callback.handleEvent === "function") rec.callback.handleEvent(event);
              } catch (err) {
                if (G.reportError) G.reportError(err); else if (G.console && G.console.error) G.console.error(err);
              }
            }
          }
          event.eventPhase = 0;
          event.currentTarget = null;
          return !event.defaultPrevented;
        }
      }
      // Non-enumerable introspection hook: events.getEventListeners(target, type)
      // in the bootstrap partition probes for a callable `listeners`.
      Object.defineProperty(EventTarget.prototype, "listeners", {
        value: function listeners(type) {
          const list = this[kListeners] && this[kListeners].get(String(type));
          return list ? list.map((l) => l.callback) : [];
        },
        writable: true, configurable: true, enumerable: false,
      });

      G.Event = Event;
      G.EventTarget = EventTarget;
      if (typeof G.CustomEvent !== "function") G.CustomEvent = CustomEvent;
      // Global addEventListener/removeEventListener/dispatchEvent bound to one
      // hidden global EventTarget (bun exposes these on the global via
      // globalEventScope / ZigGlobalObject.lut.txt). .bind keeps the brand check.
      {
        const _g = new EventTarget();
        for (const m of ["addEventListener", "removeEventListener", "dispatchEvent"])
          if (typeof G[m] !== "function") G[m] = EventTarget.prototype[m].bind(_g);
      }
    }
  } catch (e) {}

  // The `performance` global is an EventTarget (WHATWG/Deno: `performance
  // instanceof EventTarget`, and it supports addEventListener/dispatchEvent).
  // node_perf runs earlier in bootstrap — before EventTarget exists — so we
  // retrofit its prototype chain here, once EventTarget is installed. A single
  // EventTarget instance sits in the prototype chain (performance is a
  // singleton), giving it the kListeners state addEventListener/dispatchEvent
  // read via `this`.
  try {
    if (typeof G.EventTarget === "function" && G.performance &&
        !(G.performance instanceof G.EventTarget) &&
        Object.getPrototypeOf(G.performance) === Object.prototype) {
      Object.setPrototypeOf(G.performance, new G.EventTarget());
    }
  } catch (e) {}

  // ---------------------------------- WHATWG global self + event handlers
  // `self` is the global's self-reference (WorkerGlobalScope.self / Window.self);
  // WebKit exposes it as a WRITABLE accessor on the global — web-globals.test.js
  // asserts the descriptor is a configurable/enumerable get+set pair and that
  // `globalThis.self = 123` sticks. onerror/onmessage/onmessageerror are the
  // global event-handler IDL attributes: assigning one registers a single
  // listener for the matching event on the global EventTarget, reassigning swaps
  // it, nulling removes it (WHATWG "event handler IDL attributes").
  try {
    if (!Object.getOwnPropertyDescriptor(G, "self")) {
      let selfValue, selfOverridden = false;
      Object.defineProperty(G, "self", {
        configurable: true, enumerable: true,
        get() { return selfOverridden ? selfValue : G; },
        set(v) { selfOverridden = true; selfValue = v; },
      });
    }
    if (typeof G.addEventListener === "function") {
      for (const [prop, evt] of [["onerror", "error"], ["onmessage", "message"], ["onmessageerror", "messageerror"]]) {
        if (Object.getOwnPropertyDescriptor(G, prop)) continue;
        let current = null;
        Object.defineProperty(G, prop, {
          configurable: true, enumerable: true,
          get() { return current; },
          set(cb) {
            if (current) G.removeEventListener(evt, current);
            current = typeof cb === "function" ? cb : null;
            if (current) G.addEventListener(evt, current);
          },
        });
      }
    }
  } catch (e) {}

  // --------------------------------- alert() / confirm() / prompt() dialogs
  // WHATWG simple dialogs backed by a synchronous stdin read (bun
  // src/runtime/webcore/prompt.rs): confirm() prints "<msg> [y/N] " and returns
  // true only for a bare "y"/"Y" line (LF or CRLF terminated); anything else is
  // false. The read is one byte at a time off fd 0 via the native positioned-I/O
  // seam, matching bun's BufferedStdin.take_byte loop.
  try {
    const NN = G.__mbunNetNative;
    if (NN && typeof NN.readByteBlocking === "function" && typeof G.confirm !== "function") {
      const writeOut = (s) => { try { const so = G.process && G.process.stdout; if (so && typeof so.write === "function") so.write(s); } catch (e) {} };
      const readByte = () => { try { return NN.readByteBlocking(0) | 0; } catch (e) { return -1; } };
      const drainLine = () => { for (;;) { const b = readByte(); if (b === -1 || b === 0x0a || b === 0x0d) break; } };
      G.confirm = function confirm(message) {
        if (arguments.length > 0) writeOut(String(message));
        writeOut(arguments.length > 0 ? " [y/N] " : "Confirm [y/N] ");
        const first = readByte();
        if (first === 0x0a) return false;               // "\n"
        if (first === 0x0d) { readByte(); return false; } // "\r" (CRLF)
        if (first === 0x79 || first === 0x59) {          // "y" / "Y"
          const next = readByte();
          if (next === 0x0a) return true;
          if (next === 0x0d && readByte() === 0x0a) return true;
        }
        drainLine();
        return false;
      };
      if (typeof G.alert !== "function")
        G.alert = function alert(message) { writeOut((arguments.length > 0 ? String(message) : "") + " [Enter] "); drainLine(); };
      if (typeof G.prompt !== "function")
        G.prompt = function prompt(message, def) {
          if (arguments.length > 0) writeOut(String(message) + " ");
          let s = "";
          for (;;) { const b = readByte(); if (b === -1) { if (s === "") return def === undefined ? null : String(def); break; } if (b === 0x0a) break; if (b === 0x0d) continue; s += String.fromCharCode(b); }
          return s;
        };
    }
  } catch (e) {}

  // -------------------------------------------------------- process extras
  try {
    const proc = G.process;
    if (!proc) return;

    // node's determineSpecificType() rendering, used by arg-type errors.
    const specificType = (v) => {
      if (v === null) return "null";
      if (v === undefined) return "undefined";
      const t = typeof v;
      if (t === "function") return "function " + (v.name || "");
      if (t === "object") return "an instance of " + (Array.isArray(v) ? "Array" : (v.constructor && v.constructor.name) || "Object");
      if (t === "string") return "type string ('" + v + "')";
      if (t === "bigint") return "type bigint (" + v + "n)";
      if (t === "symbol") return "type symbol (" + v.toString() + ")";
      return "type " + t + " (" + String(v) + ")";
    };
    // Shared node-exact factories (bootstrap __mbunNodeErrors): they derive
    // argument/property from a dotted name ("prevValue.user" is a *property*) and
    // render a class-valued expectation as "an instance of Array" rather than
    // "of type Array". The local fallbacks below do neither.
    const NE = G.__mbunNodeErrors;
    const errInvalidArgType = NE ? NE.ERR_INVALID_ARG_TYPE : (name, expected, value) => {
      const e = new TypeError(`The "${name}" argument must be of type ${expected}. Received ${specificType(value)}`);
      e.code = "ERR_INVALID_ARG_TYPE";
      return e;
    };
    const errOutOfRange = NE ? NE.ERR_OUT_OF_RANGE : (name, range, value) => {
      const e = new RangeError(`The value of "${name}" is out of range. It must be ${range}. Received ${value}`);
      e.code = "ERR_OUT_OF_RANGE";
      return e;
    };

    // isBun marker (bun sets 1).
    if (!proc.isBun) proc.isBun = 1;

    // ---- process.env special-object semantics (node lib/internal/*) ---------
    // node's process.env is a proxy-like object: string keys/values only
    // (assigning a Symbol key OR a Symbol value throws TypeError; every value is
    // String()-coerced), and Object.defineProperty is restricted to a
    // configurable+writable+enumerable *data* descriptor. All reads/enumeration
    // stay transparent to the backing object, so only `set` and `defineProperty`
    // need trapping (get/has/delete/ownKeys default to the target).
    try {
      if (proc.env && typeof proc.env === "object" && !proc.env.__mbunEnvProxy) {
        const backing = proc.env;
        let envNonScalarWarningEmitted = false;
        const invalidDefine = (msg) => {
          const e = new TypeError(msg);
          e.code = "ERR_INVALID_OBJECT_DEFINE_PROPERTY";
          return e;
        };
        // node's env setter has native side effects for a few names; TZ is the
        // one the corpus asserts (test-process-env-tz): assigning it re-points
        // the engine's local timezone, deleting it restores the system zone.
        // The C++ half (__mbunProcNative.setTimeZone) also keeps the real
        // environ in sync so a child process inherits the new TZ.
        const applyTZ = (value) => {
          try {
            const PN = G.__mbunProcNative;
            if (PN && typeof PN.setTimeZone === "function") PN.setTimeZone(value);
          } catch (e) {}
        };
        const warnNonScalarEnvValue = () => {
          if (envNonScalarWarningEmitted || !Array.isArray(proc.execArgv) ||
              !proc.execArgv.includes("--pending-deprecation")) return;
          envNonScalarWarningEmitted = true;
          proc.emitWarning(
            "Assigning any value other than a string, number, or boolean to a process.env property is deprecated. " +
            "Please make sure to convert the value to a string before setting process.env with it.",
            "DeprecationWarning", "DEP0104");
        };
        const envProxy = new Proxy(backing, {
          set(target, key, value) {
            if (typeof key === "symbol") throw new TypeError("Cannot convert a Symbol value to a string");
            if (typeof value === "symbol") throw new TypeError("Cannot convert a Symbol value to a string");
            const k = String(key);
            // node ignores an empty variable name (test-process-env).
            if (k === "") return true;
            if (typeof value !== "string" && typeof value !== "number" && typeof value !== "boolean")
              warnNonScalarEnvValue();
            target[k] = String(value);
            if (k === "TZ") applyTZ(target[k]);
            return true;
          },
          deleteProperty(target, key) {
            const k = typeof key === "symbol" ? key : String(key);
            delete target[k];
            if (k === "TZ") applyTZ(null);
            return true;
          },
          defineProperty(target, key, desc) {
            if ("get" in desc || "set" in desc)
              throw invalidDefine("'process.env' does not accept an accessor(getter/setter) descriptor");
            if (desc.configurable !== true || desc.writable !== true || desc.enumerable !== true)
              throw invalidDefine("'process.env' only accepts a configurable, writable, and enumerable data descriptor");
            if (typeof key === "symbol") throw new TypeError("Cannot convert a Symbol value to a string");
            target[String(key)] = String(desc.value);
            if (String(key) === "TZ") applyTZ(target.TZ);
            return true;
          },
        });
        try {
          Object.defineProperty(backing, "__mbunEnvProxy", { value: true, enumerable: false, configurable: true });
        } catch (e) {}
        proc.env = envProxy;
      }
    } catch (e) {}

    // uptime(): seconds since process start; the test asserts it tracks
    // performance.now()/1000, which shares the same origin.
    if (typeof proc.uptime !== "function")
      proc.uptime = function uptime() { return G.performance.now() / 1000; };

    if (typeof proc.constrainedMemory !== "function")
      proc.constrainedMemory = function constrainedMemory() { return 0; };

    if (typeof proc._kill !== "function" && typeof proc.kill === "function") {
      const realKill = proc.kill.bind(proc);
      proc._kill = function _kill(pid, sig) { return realKill(pid, sig); };
    }

    if (!Array.isArray(proc.execArgv)) proc.execArgv = [];

    // bun aligns argv[0] with execPath.
    try {
      if (Array.isArray(proc.argv) && typeof proc.execPath === "string" && proc.argv[0] !== proc.execPath) {
        const base = proc.argv[0] && String(proc.argv[0]).split("/").pop();
        const execBase = proc.execPath.split("/").pop();
        if (!proc.argv[0] || base === execBase) proc.argv[0] = proc.execPath;
      }
    } catch (e) {}

    // ---- umask (JS-emulated mask; validation follows node's process.umask) --
    {
      let currentMask = 0o22;
      // Prefer the real OS umask (native) so created file modes agree with what
      // process.umask() reports; fall back to the JS-emulated mask if the native
      // binding is unavailable.
      const NUMASK = globalThis.__mbunFsNative && globalThis.__mbunFsNative.umask;
      proc.umask = function umask(mask) {
        if (mask === undefined) return NUMASK ? NUMASK() : currentMask;
        if (typeof mask === "string") {
          if (!/^[0-7]+$/.test(mask)) {
            const e = new TypeError(`The argument 'mask' must be a 32-bit unsigned integer or an octal string. Received '${mask}'`);
            e.code = "ERR_INVALID_ARG_VALUE";
            throw e;
          }
          mask = parseInt(mask, 8);
        } else if (typeof mask !== "number") {
          throw errInvalidArgType("mask", "number", mask);
        }
        if (!Number.isInteger(mask) || mask < 0 || mask > 0xffffffff)
          throw errOutOfRange("mask", ">= 0 && <= 4294967295", mask);
        if (NUMASK) return NUMASK(mask);
        const prev = currentMask;
        currentMask = mask;
        return prev;
      };
    }

    // ---- cpuUsage (perf-clock backed until a native getrusage lands) -------
    {
      const errInvalidProp = (name, value) => {
        const e = new RangeError(`The property '${name}' is invalid. Received ${value}`);
        e.code = "ERR_INVALID_ARG_VALUE";
        return e;
      };
      proc.cpuUsage = function cpuUsage(prevValue) {
        if (prevValue !== undefined) {
          if (prevValue === null || typeof prevValue !== "object") throw errInvalidArgType("prevValue", "object", prevValue);
          if (typeof prevValue.user !== "number") throw errInvalidArgType("prevValue.user", "number", prevValue.user);
          if (typeof prevValue.system !== "number") throw errInvalidArgType("prevValue.system", "number", prevValue.system);
          if (!Number.isFinite(prevValue.user) || prevValue.user < 0) throw errInvalidProp("prevValue.user", prevValue.user);
          if (!Number.isFinite(prevValue.system) || prevValue.system < 0) throw errInvalidProp("prevValue.system", prevValue.system);
        }
        const nowUs = G.performance.now() * 1000;
        const user = Math.max(1, Math.floor(nowUs));
        const system = Math.floor(nowUs / 4);
        if (prevValue !== undefined) return { user: user - prevValue.user, system: system - prevValue.system };
        return { user, system };
      };

      proc.threadCpuUsage = function threadCpuUsage(prevValue) {
        if (prevValue !== undefined) {
          if (prevValue === null || typeof prevValue !== "object" || Array.isArray(prevValue))
            throw errInvalidArgType("prevValue", "object", prevValue);
          if (typeof prevValue.user !== "number")
            throw errInvalidArgType("prevValue.user", "number", prevValue.user);
          if (!Number.isFinite(prevValue.user) || prevValue.user < 0)
            throw errInvalidProp("prevValue.user", prevValue.user);
          if (typeof prevValue.system !== "number")
            throw errInvalidArgType("prevValue.system", "number", prevValue.system);
          if (!Number.isFinite(prevValue.system) || prevValue.system < 0)
            throw errInvalidProp("prevValue.system", prevValue.system);
        }
        const nowUs = G.performance.now() * 1000;
        const user = Math.max(1, Math.floor(nowUs));
        const system = Math.floor(nowUs / 4);
        if (prevValue !== undefined)
          return { user: Math.max(0, user - prevValue.user), system: Math.max(0, system - prevValue.system) };
        return { user, system };
      };

      proc.availableMemory = function availableMemory() {
        try {
          const os = G.require && G.require("node:os");
          return os && typeof os.freemem === "function" ? Number(os.freemem()) : 0;
        } catch (e) {
          return 0;
        }
      };
    }

    // ---- exitCode setter validation (wraps the existing slot) --------------
    try {
      const desc = Object.getOwnPropertyDescriptor(proc, "exitCode");
      if (desc && desc.configurable) {
        let store = desc.get ? undefined : desc.value;
        const rawGet = desc.get ? desc.get.bind(proc) : () => store;
        const rawSet = desc.set ? desc.set.bind(proc) : (v) => { store = v; };
        Object.defineProperty(proc, "exitCode", {
          configurable: true,
          enumerable: true,
          get() { return rawGet(); },
          set(code) {
            if (code !== null && code !== undefined) {
              if (typeof code !== "number") throw errInvalidArgType("code", "number", code);
              if (!Number.isInteger(code)) throw errOutOfRange("code", "an integer", code);
            }
            rawSet(code);
          },
        });
      }
    } catch (e) {}

    // node internal/util.js isPendingDeprecation(): a documentation-only
    // deprecation stays SILENT unless --pending-deprecation is on (either as an
    // exec argv or via NODE_PENDING_DEPRECATION), and --no-deprecation still
    // wins over it. Never invert this: the default is silence.
    const isPendingDeprecation = () => {
      const argv = proc.execArgv;
      if (Array.isArray(argv) && argv.includes("--no-deprecation")) return false;
      if (proc.noDeprecation) return false;
      if (Array.isArray(argv) && argv.includes("--pending-deprecation")) return true;
      const env = proc.env;
      return !!(env && env.NODE_PENDING_DEPRECATION && String(env.NODE_PENDING_DEPRECATION)[0] === "1");
    };

    // ---- process.binding allow/deny list (bun's ProcessBindingMap) ---------
    {
      const orig = typeof proc.binding === "function" ? proc.binding.bind(proc) : null;
      // node lib/internal/bootstrap/realm.js processBindingAllowList (+ the
      // legacyWrapperList entries `natives`/`util`), unioned with the two extra
      // names bun's ProcessBindingMap keeps ("crypto/x509", "http_parser").
      // mbun previously shipped only bun's ten, so process.binding('cares_wrap')
      // and half the list threw "No such module"
      // (test-process-binding-internalbinding-allowlist).
      const allowed = {
        buffer: 1, cares_wrap: 1, config: 1, constants: 1, contextify: 1,
        "crypto/x509": 1, fs: 1, fs_event_wrap: 1, http_parser: 1, icu: 1,
        inspector: 1, js_stream: 1, natives: 1, os: 1, pipe_wrap: 1,
        process_wrap: 1, spawn_sync: 1, stream_wrap: 1, tcp_wrap: 1,
        tls_wrap: 1, tty_wrap: 1, udp_wrap: 1, util: 1, uv: 1, zlib: 1,
      };
      const cache = { __proto__: null };
      proc.binding = function binding(name) {
        if (typeof name !== "string" || !allowed[name]) {
          const e = new Error("No such module: " + (typeof name === "string" ? name : String(name)));
          e.code = "ERR_UNKNOWN_BUILTIN_MODULE";
          throw e;
        }
        if (cache[name]) return cache[name];
        let value = null;
        if (orig) { try { value = orig(name); } catch (e) { value = null; } }
        if (!value || typeof value !== "object" || Object.keys(value).length === 0) {
          if (name === "constants") {
            // node's process.binding("constants") is GROUPED (node_constants.cc,
            // bun ProcessBindingConstants.cpp): { os, fs, crypto, zlib, trace }.
            // The flat node:constants module is the *union* of those groups, so
            // returning it here (what mbun used to do) has the wrong shape.
            const req = (m) => { try { return G.require ? G.require(m) : null; } catch (e) { return null; } };
            const os = req("os"), fs = req("fs"), cr = req("crypto"), zl = req("zlib");
            const osc = (os && os.constants) || {};
            value = {
              os: {
                // libuv's UV_UDP_REUSEADDR (bun ProcessBindingConstants.cpp:59).
                UV_UDP_REUSEADDR: 4,
                dlopen: osc.dlopen || {},
                errno: osc.errno || {},
                signals: osc.signals || {},
                priority: osc.priority || {},
              },
              fs: Object.assign({ UV_FS_SYMLINK_DIR: 1, UV_FS_SYMLINK_JUNCTION: 2 }, (fs && fs.constants) || {}),
              crypto: (cr && cr.constants) || {},
              zlib: (zl && zl.constants) || {},
              // node's trace-event phase codes (the ASCII letter of each phase).
              // Owned by the trace_events partition below so that this shim and
              // internalBinding("constants") cannot disagree about a phase.
              trace: (G.__mbunTraceEvents && G.__mbunTraceEvents.phases) || {},
            };
          }
          else if (name === "uv") {
            // bun ProcessBindingUV.cpp: UV_<NAME> = -errno for every entry of
            // libuv's error map, plus errname(err) and getErrorMap(). The map
            // itself is util.getSystemErrorMap()'s (code → [name, message]),
            // which mbun already derives from libuv's uv_errno_map.
            const util = (() => { try { return G.require ? G.require("util") : null; } catch (e) { return null; } })();
            const map = util && typeof util.getSystemErrorMap === "function" ? util.getSystemErrorMap() : new Map();
            const uv = {};
            for (const [code, entry] of map) uv["UV_" + entry[0]] = code;
            uv.errname = function errname(err) {
              // Never throws: a non-integer (or out-of-int32) argument yields the
              // generic string rather than a TypeError (bun jsErrname).
              if (typeof err !== "number" || !Number.isInteger(err) || err < -2147483648 || err > 2147483647)
                return "Unknown system error";
              const entry = map.get(err);
              if (entry) return entry[0];
              return "Unknown system error: " + err;
            };
            uv.getErrorMap = function getErrorMap() { return new Map(map); };
            value = uv;
          }
          else if (name === "tty_wrap") {
            // node tty_wrap binding shape: class TTY + isTTY(fd) (=isatty).
            const ttyMod = (() => { try { return G.require ? G.require("node:tty") : null; } catch (e) { return null; } })();
            const BRAND = new WeakSet();
            class TTY {
              constructor(fd) {
                this.fd = fd | 0;
                // node tty_wrap: uv_tty_init fails EINVAL on a non-tty fd.
                if (!(ttyMod && (() => { try { return ttyMod.isatty(this.fd); } catch (e) { return false; } })())) {
                  const e = new Error("EINVAL: invalid argument, uv_tty_init");
                  e.code = "EINVAL"; throw e;
                }
                BRAND.add(this);
              }
              getWindowSize(arr) {
                if (!BRAND.has(this)) throw new TypeError("Illegal invocation");
                if (!Array.isArray(arr)) throw new TypeError('The "size" argument must be an instance of Array');
                const out = G.process && G.process.stdout;
                arr[0] = (out && out.columns) || 80; arr[1] = (out && out.rows) || 24;
                return true;
              }
              setRawMode(flag) {
                if (!BRAND.has(this)) throw new TypeError("Illegal invocation");
                const stdin = G.process && G.process.stdin;
                if (stdin && typeof stdin.setRawMode === "function") { try { stdin.setRawMode(!!flag); } catch (e) {} }
                return 0;
              }
            }
            value = { TTY, isTTY: (fd) => { try { return !!(ttyMod && ttyMod.isatty(fd)); } catch (e) { return false; } } };
          }
          else if (name === "util") {
            // node process.binding("util") exposes the type predicates that
            // util.types is built from (node_util.cc). The test asserts the
            // binding's functions are IDENTICAL to util.types[k], so pull them
            // straight from the util module rather than reimplementing.
            const ut = (() => { try { return G.require ? G.require("util").types : null; } catch (e) { return null; } })();
            const keys = ["isAnyArrayBuffer", "isArrayBuffer", "isArrayBufferView",
              "isAsyncFunction", "isDataView", "isDate", "isExternal", "isMap",
              "isMapIterator", "isNativeError", "isPromise", "isRegExp", "isSet",
              "isSetIterator", "isTypedArray", "isUint8Array"];
            const u = {};
            if (ut) for (const k of keys) if (typeof ut[k] === "function") u[k] = ut[k];
            value = u;
          }
          else {
            // node's process.binding() for the rest of the allow list is just
            // internalBinding() (realm.js). Use the real namespace when this
            // runtime has one; an empty object only when it does not, which is
            // still what the caller gets today.
            value = null;
            if (typeof G.__mbunInternalBinding === "function") {
              try { value = G.__mbunInternalBinding(name); } catch (e) { value = null; }
            }
            if (!value || typeof value !== "object") value = {};
          }
        }
        return (cache[name] = value);
      };
      // node initializeDeprecations() (pre_execution.js) and node_uv.cc gate
      // DEP0111/DEP0119 on --pending-deprecation ONLY: process.binding() is
      // silent by default and must stay that way. Each fires once per process.
      if (isPendingDeprecation()) {
        const rawBinding = proc.binding;
        let bindingWarned = false;
        proc.binding = function binding(name) {
          if (!bindingWarned) {
            bindingWarned = true;
            proc.emitWarning("process.binding() is deprecated. Please use public APIs instead.",
                             "DeprecationWarning", "DEP0111");
          }
          const mod = rawBinding.call(this, name);
          // node uv.cc ErrName(): the DEP0119 warning is attached to errname
          // itself, so it only fires when errname is actually CALLED.
          if (name === "uv" && mod && typeof mod.errname === "function" && !mod.__mbunErrnameDeprecated) {
            const rawErrname = mod.errname;
            let errnameWarned = false;
            mod.errname = function errname(err) {
              if (!errnameWarned) {
                errnameWarned = true;
                proc.emitWarning(
                  "Directly calling process.binding('uv').errname(<val>) is being deprecated. " +
                  "Please make sure to use util.getSystemErrorName() instead.",
                  "DeprecationWarning", "DEP0119");
              }
              return rawErrname.call(this, err);
            };
            try { Object.defineProperty(mod, "__mbunErrnameDeprecated", { value: true }); } catch (e) {}
          }
          return mod;
        };
      }
    }

    // ---- process.dlopen ------------------------------------------------------
    // bun Process.cpp: with --no-addons every dlopen throws the fixed message;
    // without it mbun cannot load .node addons yet, so it reports that honestly.
    if (typeof proc.dlopen !== "function") {
      proc.dlopen = function dlopen() {
        if (proc.env && proc.env.MBUN_NO_ADDONS === "1") {
          const e = new Error("Cannot load native addon because loading addons is disabled.");
          e.code = "ERR_DLOPEN_DISABLED"; throw e;
        }
        const e = new Error("process.dlopen is not yet implemented in mbun");
        e.code = "ERR_DLOPEN_FAILED"; throw e;
      };
    }

    // ---- process.setgroups ----------------------------------------------------
    // node validates element-by-element (user accessors run — exceptions
    // propagate); each must be a >=0 integer; the privileged syscall then
    // EPERMs for a non-root runner.
    if (typeof proc.setgroups !== "function") {
      proc.setgroups = function setgroups(groups) {
        if (!Array.isArray(groups)) throw errInvalidArgType("groups", "Array", groups);
        // A Proxy passes IsArray but is not a JSArray, and bun's setgroups needs a
        // real one — it rejects the Proxy with a TypeError rather than reading
        // through the traps (regression/issue/isArray-proxy-crash). Reaching the
        // syscall instead surfaced EPERM, which is not a TypeError.
        if (G.__mbunProxyRegistry && G.__mbunProxyRegistry.has(groups)) throw errInvalidArgType("groups", "Array", groups);
        for (let i = 0; i < groups.length; i++) {
          const g = groups[i];             // accessor getters run here and may throw
          if (typeof g !== "number" || !Number.isInteger(g) || g < 0) {
            const e = new TypeError('The value of "groups[' + i + ']" is out of range.');
            e.code = "ERR_OUT_OF_RANGE"; throw e;
          }
        }
        const e = new Error("EPERM, Operation not permitted");
        e.code = "EPERM"; throw e;        // no native setgroups yet; non-root node EPERMs too
      };
    }

    // ---- node stub surface --------------------------------------------------
    // node src/node.cc RawDebug: writes its formatted arguments to stderr
    // SYNCHRONOUSLY, deliberately bypassing the stream stack — that is the whole
    // point of it, it is the diagnostic of last resort when streams are broken or
    // the loop is wedged. A silent no-op is therefore the worst possible stub: it
    // reports success while swallowing exactly the output someone reached for
    // because nothing else was working. 5 corpus files use it, and it cost an
    // agent ~15 minutes when its hang-watchdog printed nothing.
    if (typeof proc._rawDebug !== "function") {
      const rawDebug = function _rawDebug(...args) {
        const U = G.__mbunNativeModules && G.__mbunNativeModules["util"];
        const text = U && typeof U.format === "function"
          ? U.format(...args)
          : args.map((a) => (typeof a === "string" ? a : String(a))).join(" ");
        // Straight at fd 2, not through process.stderr: node's does not go
        // through the stream either.
        const FD = G.__mbunFdNative;
        if (FD && typeof FD.writeSync === "function") {
          try { FD.writeSync(2, text + "\n"); return undefined; } catch (e) {}
        }
        try { G.console.error(text); } catch (e) {}
        return undefined;
      };
      Object.defineProperty(rawDebug, "name", { value: "_rawDebug" });
      proc._rawDebug = rawDebug;
    }

    // ---- process.emitWarning + the default 'warning' printer ----------------
    // node lib/internal/process/warning.js, reproduced in full because the
    // engine prelude's emitWarning implemented only the object shaping:
    //   * process.noDeprecation suppresses a DeprecationWarning ENTIRELY — the
    //     'warning' event never fires (test-process-no-deprecation asserts the
    //     listener is not called, not merely that nothing printed);
    //   * process.throwDeprecation turns it into an uncaught throw on the next
    //     tick, so `try { emitWarning(...) } catch {}` around the call must NOT
    //     see it (test-process-warning test4);
    //   * an unclaimed warning is PRINTED — node registers onWarning as a real
    //     'warning' listener during bootstrap, which is what makes
    //     `--redirect-warnings` / NODE_REDIRECT_WARNINGS observable at all
    //     (test-process-redirect-warnings{,-env} read the file back).
    // The printer is installed as an ordinary listener exactly as node does, so
    // process.listenerCount('warning') is 1 at startup in both runtimes.
    try {
      const flagValue = (name) => {
        const argv = (proc.execArgv && proc.execArgv.length ? proc.execArgv : []) || [];
        for (const a of argv) {
          if (typeof a !== "string") continue;
          if (a === name) return "";
          if (a.startsWith(name + "=")) return a.slice(name.length + 1);
        }
        return undefined;
      };
      const hasFlag = (name) => flagValue(name) !== undefined;
      // Resolved lazily and once, like node's lazyOption(): execArgv is filled
      // in after this partition is evaluated.
      let warningFile;
      const warningTarget = () => {
        if (warningFile === undefined) {
          warningFile = flagValue("--diagnostic-dir") || flagValue("--redirect-warnings") ||
                        (proc.env && proc.env.NODE_REDIRECT_WARNINGS) || "";
        }
        return warningFile;
      };
      let traceHelperShown = false;
      let disableSet = null;
      // V8's Error.stack opens with "Name: message" and renders frames as
      // "    at fn (file:line:col)"; JSC's carries only frames, in its own
      // `fn@file:line:col` syntax. `--trace-warnings` output is read by the
      // corpus as node's shape (test-worker-execargv matches
      // /Warning: some warning[\s\S]*at Object\.<anonymous>/), so rebuild it.
      const tracedStack = (warning) => {
        const head = (warning.name || "Error") +
                     (warning.message ? ": " + warning.message : "");
        const lines = String(warning.stack).split("\n");
        // A warning built by createWarning() below already carries a V8-shaped
        // stack (Error.captureStackTrace re-renders the frames and prepends
        // "Name: message"), so it must be passed through rather than parsed as
        // JSC "fn@loc" — otherwise every line would gain a second "at ".
        // Recognised by: a head line with no '@' followed only by "at " frames.
        if (lines.length > 1 && lines[0].indexOf("@") < 0 &&
            lines.slice(1).every((l) => !l.trim() || l.trim().startsWith("at "))) {
          const kept = [];
          for (const raw of lines.slice(1)) {
            const line = raw.trim();
            if (!line) continue;
            const body = line.slice(3);
            // mbun's CallSite renders a frame with no function name as the bare
            // location; V8 names the top-level program frame Object.<anonymous>,
            // which is the shape the corpus greps for (test-worker-execargv).
            kept.push("    at " + (body.indexOf("(") < 0 ? "Object.<anonymous> (" + body + ")" : body));
          }
          return kept.length ? head + "\n" + kept.join("\n") : head;
        }
        const frames = [];
        for (const raw of lines) {
          const line = raw.trim();
          if (!line) continue;
          // Last '@': a function name cannot hold one, a file:// URL can.
          const at = line.lastIndexOf("@");
          if (at < 0) { frames.push("    at " + line); continue; }
          let fn = line.slice(0, at);
          const loc = line.slice(at + 1);
          // JSC calls the top-level program frame "global code"; V8 renders
          // the same frame as "Object.<anonymous>".
          if (fn === "global code" || fn === "module code") fn = "Object.<anonymous>";
          if (!loc) { frames.push("    at " + (fn || "<anonymous>") + " (native)"); continue; }
          frames.push(fn ? "    at " + fn + " (" + loc + ")" : "    at " + loc);
        }
        return frames.length ? head + "\n" + frames.join("\n") : head;
      };
      const onWarning = function onWarning(warning) {
        // --no-warnings suppresses the printer only; the event still fires.
        // Checked HERE rather than at install time because process.execArgv is
        // filled in after this partition runs (same ordering the `gc` accessor
        // below documents).
        if (hasFlag("--no-warnings")) return;
        if (disableSet === null) {
          disableSet = new Set();
          const argv = (proc.execArgv || []);
          for (const a of argv)
            if (typeof a === "string" && a.startsWith("--disable-warning="))
              disableSet.add(a.slice(18));
        }
        if (warning && ((warning.code && disableSet.has(warning.code)) ||
                        (warning.name && disableSet.has(warning.name)))) return;
        if (!(warning instanceof Error)) return;
        const isDeprecation = warning.name === "DeprecationWarning";
        if (isDeprecation && proc.noDeprecation) return;
        // node sets process.traceProcessWarnings / traceDeprecation from the
        // CLI in per_thread.js; mbun's process object carries neither, so the
        // flags themselves are the source of truth and `--trace-warnings` was
        // silently ignored (test-worker-execargv runs a Worker with exactly
        // that execArgv and greps its stderr for the creation site).
        const trace = !!(proc.traceProcessWarnings || hasFlag("--trace-warnings") ||
                         (isDeprecation && (proc.traceDeprecation || hasFlag("--trace-deprecation"))));
        let msg = "(" + ((proc.release && proc.release.name) || "node") + ":" + proc.pid + ") ";
        if (warning.code) msg += "[" + warning.code + "] ";
        // node falls back to Error.prototype.toString when the instance's own
        // toString is not callable (test-process-warning test5 sets it to 1).
        if (trace && warning.stack) msg += tracedStack(warning);
        else if (typeof warning.toString === "function") msg += String(warning.toString());
        else msg += Error.prototype.toString.call(warning);
        if (typeof warning.detail === "string") msg += "\n" + warning.detail;
        if (!trace && !traceHelperShown) {
          const flag = isDeprecation ? "--trace-deprecation" : "--trace-warnings";
          msg += "\n(Use `" + ((proc.release && proc.release.name) || "node") + " " + flag +
                 " ...` to show where the warning was created)";
          traceHelperShown = true;
        }
        const file = warningTarget();
        if (file) {
          try {
            const fs = G.__mbunNativeModules && G.__mbunNativeModules["fs"];
            if (fs && typeof fs.appendFileSync === "function") { fs.appendFileSync(file, msg + "\n"); return; }
          } catch (e) {}
        }
        try { G.console.error(msg); } catch (e) {}
      };

      const createWarning = (message, type, code, ctor, detail) => {
        const e = new Error(message);
        e.name = String(type || "Warning");
        if (code !== undefined) e.code = code;
        if (detail !== undefined) e.detail = detail;
        // node warning.js: ErrorCaptureStackTrace(warning, ctor || emitWarning).
        // Without it the stack is JSC's raw one, which (a) opens with the
        // createWarning/emitWarning plumbing frames node deliberately hides and
        // (b) carries no "Name: message" head, so `assert.match(w.stack, /msg/)`
        // — the corpus's usual way of inspecting a warning — cannot match.
        try { Error.captureStackTrace(e, ctor || proc.emitWarning); } catch (err) {}
        return e;
      };
      proc.emitWarning = function emitWarning(warning, type, code, ctor) {
        if (proc.noDeprecation && type === "DeprecationWarning") return;
        let detail;
        if (type !== null && typeof type === "object" && !Array.isArray(type)) {
          ctor = type.ctor;
          code = type.code;
          if (typeof type.detail === "string") detail = type.detail;
          type = type.type || "Warning";
        } else if (typeof type === "function") {
          ctor = type; code = undefined; type = "Warning";
        }
        if (type !== undefined && typeof type !== "string") throw errInvalidArgType("type", "string", type);
        if (typeof code === "function") { ctor = code; code = undefined; }
        else if (code !== undefined && typeof code !== "string") throw errInvalidArgType("code", "string", code);
        if (typeof warning === "string") warning = createWarning(warning, type, code, ctor, detail);
        else if (!(warning instanceof Error)) throw errInvalidArgType("warning", ["Error", "string"], warning);
        if (warning.name === "DeprecationWarning") {
          if (proc.noDeprecation) return;
          if (proc.throwDeprecation) {
            // Deferred, so warnings emitted earlier in this tick still print —
            // and so the emitWarning CALL does not throw synchronously.
            return proc.nextTick(() => { throw warning; });
          }
        }
        proc.nextTick(() => { proc.emit("warning", warning); });
      };
      if (typeof proc.on === "function" && proc.listenerCount("warning") === 0) {
        proc.on("warning", onWarning);
      }

      // ---- flags that arrive through NODE_OPTIONS ---------------------------
      // node applies a NODE_OPTIONS flag exactly as if it had been typed on the
      // command line, but keeps it OUT of process.execArgv — so a flag sourced
      // only from the environment has to be looked up separately. The corpus
      // reaches mbun this way whenever a test spawns a child with an env
      // (test-worker-node-options passes --title / --trace-exit to fixtures).
      const envFlagValue = (name) => {
        const own = flagValue(name);
        if (own !== undefined) return own;
        const raw = (proc.env && proc.env.NODE_OPTIONS) || "";
        for (const word of String(raw).split(/\s+/)) {
          if (word === name) return "";
          if (word.startsWith(name + "=")) return word.slice(name.length + 1);
        }
        return undefined;
      };

      // `--title=<name>` from NODE_OPTIONS. The runtime's own command line is
      // handled in C++ (engine.inc reads gExecArgv before process exists); this
      // is the environment half of the same option.
      {
        const t = envFlagValue("--title");
        if (t) { try { proc.title = t; } catch (e) {} }
      }

      // `--trace-exit`: node prints a warning plus the call site every time
      // process.exit() leaves the environment (src/node_process_methods.cc
      // ProcessExit -> Environment::Exit with trace_exit). Wrapping the native
      // exit is the whole of it — a natural loop drain is not an Exit() and
      // must stay silent.
      if (envFlagValue("--trace-exit") !== undefined && typeof proc.exit === "function" &&
          !proc.exit.__mbunTraceExit) {
        const nativeExit = proc.exit;
        const traced = function exit(code) {
          try {
            const e = new Error();
            e.name = "Trace";
            const frames = tracedStack(e).split("\n").slice(1).join("\n");
            const head = "(" + ((proc.release && proc.release.name) || "node") + ":" + proc.pid +
                         ") WARNING: Exited the environment with code " +
                         (code === undefined || code === null ? (proc.exitCode || 0) : code);
            proc.stderr.write(frames ? head + "\n" + frames + "\n" : head + "\n");
          } catch (e) {}
          return nativeExit.call(proc, code);
        };
        traced.__mbunTraceExit = true;
        proc.exit = traced;
      }

      // ---- trace_events shared state and JSON sink -------------------------
      // The engine has no native Chrome tracing backend, but node's public API,
      // internal binding, and command-line writer all share one category state.
      // Keeping it here (after process and fs are live) also makes tracing work
      // when user code never explicitly requires `trace_events`.
      const traceEvents = (() => {
        const argv = Array.isArray(proc.execArgv) ? proc.execArgv : [];
        const initial = [];
        const dynamic = new Map();
        const buffers = new Map();
        const handlers = new Set();
        const events = [];
        const activeTracings = new Set();
        let writesTrace = false;
        let flushed = false;
        let pattern;
        let sawCategories = false;
        let sawTraceFlag = false;
        let initialTitle;

        // node src/tracing/trace_event_common.h: every phase is the ASCII code
        // of the Chrome-trace `ph` letter. internalBinding("constants").trace
        // and process.binding("constants").trace both publish this table, and
        // internalBinding("trace_events").trace turns the code back into that
        // letter -- so all three have to come from one place.
        const phases = {
          TRACE_EVENT_PHASE_BEGIN: 66, TRACE_EVENT_PHASE_END: 69,
          TRACE_EVENT_PHASE_COMPLETE: 88, TRACE_EVENT_PHASE_INSTANT: 73,
          TRACE_EVENT_PHASE_ASYNC_BEGIN: 83, TRACE_EVENT_PHASE_ASYNC_STEP_INTO: 84,
          TRACE_EVENT_PHASE_ASYNC_STEP_PAST: 112, TRACE_EVENT_PHASE_ASYNC_END: 70,
          TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN: 98, TRACE_EVENT_PHASE_NESTABLE_ASYNC_END: 101,
          TRACE_EVENT_PHASE_NESTABLE_ASYNC_INSTANT: 110, TRACE_EVENT_PHASE_FLOW_BEGIN: 115,
          TRACE_EVENT_PHASE_FLOW_STEP: 116, TRACE_EVENT_PHASE_FLOW_END: 102,
          TRACE_EVENT_PHASE_METADATA: 77, TRACE_EVENT_PHASE_COUNTER: 67,
          TRACE_EVENT_PHASE_SAMPLE: 80, TRACE_EVENT_PHASE_CREATE_OBJECT: 78,
          TRACE_EVENT_PHASE_SNAPSHOT_OBJECT: 79, TRACE_EVENT_PHASE_DELETE_OBJECT: 68,
          TRACE_EVENT_PHASE_MEMORY_DUMP: 118, TRACE_EVENT_PHASE_MARK: 82,
          TRACE_EVENT_PHASE_CLOCK_SYNC: 99, TRACE_EVENT_PHASE_ENTER_CONTEXT: 40,
          TRACE_EVENT_PHASE_LEAVE_CONTEXT: 41, TRACE_EVENT_PHASE_LINK_IDS: 61,
        };

        const flagValue = (name) => {
          for (let i = 0; i < argv.length; i++) {
            const arg = argv[i];
            if (arg === name) return i + 1 < argv.length ? argv[i + 1] : "";
            if (typeof arg === "string" && arg.startsWith(name + "=")) return arg.slice(name.length + 1);
          }
          return undefined;
        };
        initialTitle = flagValue("--title") || proc.title || "node";
        const categoriesOf = (value) => {
          if (value === undefined || value === null || value === '""') return [];
          return String(value).split(",").map((v) => v.trim()).filter(Boolean);
        };
        for (const arg of argv) if (arg === "--trace-events-enabled" ||
                                    (typeof arg === "string" && arg.startsWith("--trace-events-enabled=")))
          sawTraceFlag = true;
        const cliCategories = flagValue("--trace-event-categories");
        if (cliCategories !== undefined) {
          sawCategories = true;
          initial.push(...categoriesOf(cliCategories));
        } else if (sawTraceFlag) {
          initial.push("v8", "node", "node.async_hooks");
        }
        pattern = flagValue("--trace-event-file-pattern");
        writesTrace = sawTraceFlag || sawCategories;

        const enabledNames = () => {
          const out = initial.slice();
          for (const [name, count] of dynamic) if (count > 0 && !out.includes(name)) out.push(name);
          return out;
        };
        const enabled = (name) => enabledNames().includes(name);
        const updateBuffers = () => {
          for (const [name, buffer] of buffers) buffer[0] = enabled(name) ? 1 : 0;
          for (const handler of handlers) { try { handler(); } catch (e) {} }
        };
        const getEnabledCategories = () => {
          const names = enabledNames();
          return names.length ? names.join(",") : undefined;
        };
        const categoryBuffer = (name) => {
          name = String(name);
          let buffer = buffers.get(name);
          if (!buffer) { buffer = new Uint8Array(1); buffers.set(name, buffer); }
          buffer[0] = enabled(name) ? 1 : 0;
          return buffer;
        };
        const changeCategories = (categories, delta) => {
          for (const category of categories) {
            const count = (dynamic.get(category) || 0) + delta;
            if (count > 0) dynamic.set(category, count); else dynamic.delete(category);
          }
          if (delta > 0) writesTrace = true;
          updateBuffers();
        };
        const record = (event) => {
          events.push(Object.assign({ pid: proc.pid || 0, tid: 1, ts: Date.now() * 1000 }, event));
        };
        const trace = (phase, category, name, id, data) => {
          category = String(category);
          if (!enabled(category)) return;
          const event = {
            ph: typeof phase === "number" ? String.fromCharCode(phase) : String(phase),
            cat: category,
            name: String(name),
            args: data === undefined ? {} : { data },
          };
          if (id !== undefined && id !== null) {
            const n = Number(id);
            event.id = Number.isFinite(n) ? "0x" + n.toString(16) : String(id);
          }
          record(event);
        };
        const invalidArg = () => {
          const error = new TypeError('The "options" argument must be of type object.');
          error.code = "ERR_INVALID_ARG_TYPE";
          return error;
        };
        const createTracing = (options) => {
          if (options === null || typeof options !== "object" || Array.isArray(options)) throw invalidArg();
          if (!Array.isArray(options.categories)) throw invalidArg();
          if (options.categories.length === 0) {
            const error = new TypeError("At least one category is required");
            error.code = "ERR_TRACE_EVENTS_CATEGORY_REQUIRED";
            throw error;
          }
          if (!options.categories.every((category) => typeof category === "string")) throw invalidArg();
          const categories = options.categories.slice();
          let active = false;
          const tracing = {
            get categories() { return categories.join(","); },
            get enabled() { return active; },
            enable() {
              if (active) return;
              active = true;
              activeTracings.add(tracing);
              changeCategories(categories, 1);
              if (activeTracings.size > 10 && typeof proc.emitWarning === "function") {
                proc.emitWarning("Possible trace_events memory leak detected. There are more than 10 enabled Tracing objects.");
              }
            },
            disable() {
              if (!active) return;
              active = false;
              activeTracings.delete(tracing);
              changeCategories(categories, -1);
            },
          };
          return tracing;
        };
        const metadata = () => {
          const processInfo = {
            versions: proc.versions || {}, arch: proc.arch, platform: proc.platform,
            release: proc.release || {},
          };
          const title = proc.title || "node";
          const rows = [
            { name: "thread_name", args: { name: "JavaScriptMainThread" } },
            { name: "thread_name", args: { name: "PlatformWorkerThread" } },
            { name: "version", args: { node: (proc.versions || {}).node } },
            { name: "node", args: { process: processInfo } },
            { name: "process_name", args: { name: initialTitle } },
          ];
          if (title !== initialTitle) rows.push({ name: "process_name", args: { name: title } });
          return rows.map((row) => Object.assign({ pid: proc.pid || 0, tid: 1, ts: Date.now() * 1000,
                                                     ph: "M", cat: "__metadata" }, row));
        };
        const flush = () => {
          if (flushed || !writesTrace) return;
          flushed = true;
          const file = String(pattern || "node_trace.${rotation}.log")
            .replace(/\$\{pid\}/g, String(proc.pid || 0))
            .replace(/\$\{rotation\}/g, "1");
          try {
            const fs = G.__mbunNativeModules && (G.__mbunNativeModules["fs"] || G.__mbunNativeModules["node:fs"]);
            if (fs && typeof fs.writeFileSync === "function") fs.writeFileSync(file, JSON.stringify({ traceEvents: metadata().concat(events) }));
          } catch (e) {}
        };
        return {
          phases,
          createTracing, getEnabledCategories, getCategoryEnabledBuffer: categoryBuffer,
          isTraceCategoryEnabled: enabled,
          enableCategories: (categories) => changeCategories(categories, 1),
          disableCategories: (categories) => changeCategories(categories, -1),
          setTraceCategoryStateUpdateHandler: (handler) => { if (typeof handler === "function") handlers.add(handler); },
          trace, flush,
        };
      })();
      Object.defineProperty(G, "__mbunTraceEvents", { value: traceEvents, configurable: true });
      const traceModule = {
        createTracing: traceEvents.createTracing,
        getEnabledCategories: traceEvents.getEnabledCategories,
      };
      const modules = G.__mbunNativeModules;
      if (modules) modules["trace_events"] = modules["node:trace_events"] = traceModule;
      if (typeof proc.on === "function") proc.on("exit", traceEvents.flush);
      if (typeof proc.exit === "function" && !proc.exit.__mbunTraceEvents) {
        const nativeExit = proc.exit;
        const tracedExit = function exit(code) { traceEvents.flush(); return nativeExit.call(this, code); };
        tracedExit.__mbunTraceEvents = true;
        proc.exit = tracedExit;
      }
    } catch (e) {}

    // ---- process.allowedNodeEnvironmentFlags --------------------------------
    // node lib/internal/process/per_thread.js buildAllowedFlags(): the set of
    // options node accepts inside NODE_OPTIONS, wrapped in a frozen Set whose
    // mutators are no-ops and whose has() normalises the many spellings a user
    // may pass (leading dashes optional, "_" interchangeable with "-", a
    // trailing "=value" ignored). It was an empty Set, so every membership
    // question answered "no" (test-process-env-allowed-flags).
    //
    // The list is node's own NODE_OPTIONS table, extracted from
    // compat/node/doc/api/cli.md's node-options-{node,v8} sections plus the
    // options node keeps allowed but deliberately undocumented. Entries gated
    // on a build feature mbun does not have (the inspector, and the profilers
    // that depend on it) are added only when the feature reports present, which
    // is the same condition test-process-env-allowed-flags applies.
    try {
      const FLAGS = [
      "--allow-addons", "--allow-child-process", "--allow-fs-read", "--allow-fs-write",
      "--allow-inspector", "--allow-net", "--allow-wasi", "--allow-worker", "--conditions", "-C",
      "--diagnostic-dir", "--disable-proto", "--disable-sigusr1", "--disable-warning",
      "--disable-wasm-trap-handler", "--dns-result-order", "--enable-fips",
      "--enable-network-family-autoselection", "--enable-source-maps", "--entry-url",
      "--experimental-abortcontroller", "--experimental-addon-modules",
      "--experimental-detect-module", "--experimental-eventsource",
      "--experimental-import-meta-resolve", "--experimental-json-modules",
      "--experimental-loader", "--experimental-modules", "--experimental-print-required-tla",
      "--experimental-quic", "--experimental-require-module", "--experimental-shadow-realm",
      "--experimental-specifier-resolution", "--experimental-stream-iter",
      "--experimental-test-isolation", "--experimental-top-level-await",
      "--experimental-vm-modules", "--experimental-wasi-unstable-preview1",
      "--force-context-aware", "--force-fips", "--force-node-api-uncaught-exceptions-policy",
      "--frozen-intrinsics", "--heapsnapshot-near-heap-limit", "--heapsnapshot-signal",
      "--http-parser", "--import", "--input-type", "--insecure-http-parser",
      "--localstorage-file", "--max-http-header-size", "--max-old-space-size-percentage",
      "--napi-modules", "--network-family-autoselection-attempt-timeout", "--addons",
      "--async-context-frame", "--deprecation", "--experimental-global-navigator",
      "--experimental-repl-await", "--experimental-sqlite", "--experimental-strip-types",
      "--experimental-websocket", "--experimental-webstorage", "--extra-info-on-fatal-exception",
      "--force-async-hooks-checks", "--global-search-paths", "--network-family-autoselection",
      "--strip-types", "--warnings", "--webstorage", "--node-memory-debug", "--openssl-config",
      "--openssl-legacy-provider", "--openssl-shared-config", "--pending-deprecation",
      "--permission-audit", "--permission", "--preserve-symlinks-main", "--preserve-symlinks",
      "--prof-process", "--redirect-warnings", "--report-compact", "--report-dir",
      "--report-directory", "--report-exclude-env", "--report-exclude-network",
      "--report-filename", "--report-on-fatalerror", "--report-on-signal", "--report-signal",
      "--report-uncaught-exception", "--require-module", "--require", "-r", "--secure-heap-min",
      "--secure-heap", "--snapshot-blob", "--test-coverage-branches", "--test-coverage-exclude",
      "--test-coverage-functions", "--test-coverage-include", "--test-coverage-lines",
      "--test-global-setup", "--test-isolation", "--test-name-pattern", "--test-only",
      "--test-random-seed", "--test-randomize", "--test-reporter-destination", "--test-reporter",
      "--test-rerun-failures", "--test-shard", "--test-skip-pattern", "--throw-deprecation",
      "--title", "--tls-cipher-list", "--tls-keylog", "--tls-max-v1.2", "--tls-max-v1.3",
      "--tls-min-v1.0", "--tls-min-v1.1", "--tls-min-v1.2", "--tls-min-v1.3",
      "--trace-deprecation", "--trace-env-js-stack", "--trace-env-native-stack", "--trace-env",
      "--trace-event-categories", "--trace-event-file-pattern", "--trace-events-enabled",
      "--trace-exit", "--trace-require-module", "--trace-sigint", "--trace-sync-io",
      "--trace-tls", "--trace-uncaught", "--trace-warnings", "--track-heap-objects",
      "--unhandled-rejections", "--use-bundled-ca", "--use-env-proxy", "--use-largepages",
      "--use-openssl-ca", "--use-system-ca", "--v8-pool-size", "--watch-kill-signal",
      "--watch-path", "--watch-preserve-output", "--watch", "--zero-fill-buffers",
      "--abort-on-uncaught-exception", "--disallow-code-generation-from-strings",
      "--enable-etw-stack-walking", "--expose-gc", "--interpreted-frames-native-stack",
      "--jitless", "--max-heap-size", "--max-old-space-size", "--max-semi-space-size",
      "--perf-basic-prof-only-functions", "--perf-basic-prof", "--perf-prof-unwinding-info",
      "--perf-prof", "--stack-trace-limit", "--no-addons", "--no-async-context-frame",
      "--no-deprecation", "--no-experimental-global-navigator", "--no-experimental-repl-await",
      "--no-experimental-sqlite", "--no-experimental-strip-types", "--no-experimental-websocket",
      "--no-experimental-webstorage", "--no-extra-info-on-fatal-exception",
      "--no-force-async-hooks-checks", "--no-global-search-paths",
      "--no-network-family-autoselection", "--no-strip-types", "--no-warnings", "--no-webstorage",
      "--debug-arraybuffer-allocations", "--no-debug-arraybuffer-allocations",
      "--es-module-specifier-resolution", "--experimental-fetch", "--experimental-wasm-modules",
      "--experimental-global-customevent", "--experimental-global-webcrypto",
      "--experimental-report", "--experimental-worker", "--node-snapshot", "--no-node-snapshot",
      "--loader", "--verify-base-objects", "--no-verify-base-objects", "--trace-promises",
      "--no-trace-promises",
    ];
      const INSPECTOR_FLAGS = [
      "--inspect-brk", "--inspect-port", "--debug-port", "--inspect-publish-uid",
      "--inspect-wait", "--inspect", "--cpu-prof-dir", "--cpu-prof-interval", "--cpu-prof-name",
      "--cpu-prof", "--heap-prof-dir", "--heap-prof-interval", "--heap-prof-name", "--heap-prof",
    ];
      const array = proc.features && proc.features.inspector
        ? FLAGS.concat(INSPECTOR_FLAGS) : FLAGS.slice();
      const bare = array.map((f) => f.replace(/^--?/, ""));
      // Kept OUT of the instance: the object is frozen (and class bodies are
      // strict), so caching on `this` would throw on first use.
      let cached = null;
      const cache = () => (cached || (cached = new Set(array)));
      class NodeEnvironmentFlagsSet extends Set {
        add() { return this; }
        delete() { return false; }
        clear() {}
        has(key) {
          if (typeof key !== "string") return false;
          key = key.replace(/_/g, "-");
          if (/^--?/.test(key)) return array.includes(key.replace(/=.*$/, ""));
          return bare.includes(key);
        }
        entries() { return cache().entries(); }
        forEach(cb, thisArg) { for (const v of array) cb.call(thisArg, v, v, this); }
        get size() { return array.length; }
        values() { return cache().values(); }
      }
      const values = NodeEnvironmentFlagsSet.prototype.values;
      Object.defineProperty(NodeEnvironmentFlagsSet.prototype, Symbol.iterator, { value: values });
      Object.defineProperty(NodeEnvironmentFlagsSet.prototype, "keys", { value: values });
      Object.freeze(NodeEnvironmentFlagsSet.prototype.constructor);
      Object.freeze(NodeEnvironmentFlagsSet.prototype);
      Object.defineProperty(proc, "allowedNodeEnvironmentFlags", {
        value: Object.freeze(new NodeEnvironmentFlagsSet()),
        writable: true, enumerable: true, configurable: true,
      });
    } catch (e) {}

    const undefinedStubs = [
      "_debugEnd", "_debugProcess", "_fatalException", "_linkedBinding",
      "_startProfilerIdleNotifier", "_stopProfilerIdleNotifier",
      "_tickCallback",
    ];
    for (const name of undefinedStubs) {
      if (typeof proc[name] !== "function") {
        const fn = function () { return undefined; };
        Object.defineProperty(fn, "name", { value: name });
        proc[name] = fn;
      }
    }
    const arrayStubs = ["getActiveResourcesInfo", "_getActiveRequests", "_getActiveHandles"];
    for (const name of arrayStubs) {
      if (typeof proc[name] !== "function") {
        const fn = function () { return []; };
        Object.defineProperty(fn, "name", { value: name });
        proc[name] = fn;
      }
    }

    // ---- process.ref / process.unref ---------------------------------------
    // Node first recognizes the symbol protocol, then falls back to the legacy
    // ref()/unref() methods used by timers and handles.
    const installRefMethod = (name) => {
      const symbol = Symbol.for("nodejs." + name);
      proc[name] = function (resource) {
        const method = resource != null &&
          (typeof resource[symbol] === "function" ? resource[symbol] : resource[name]);
        if (typeof method === "function") method.call(resource);
      };
    };
    installRefMethod("ref");
    installRefMethod("unref");

    if (!Array.isArray(proc.moduleLoadList)) proc.moduleLoadList = [];
    if (!Array.isArray(proc._preload_modules)) proc._preload_modules = [];

    // ---- chdir arg-type validation (node ERR_INVALID_ARG_TYPE) --------------
    // node validateString(directory, 'directory'); a non-string (or missing)
    // argument throws before touching the filesystem (test-process-chdir).
    try {
      if (typeof proc.chdir === "function") {
        const origChdir = proc.chdir.bind(proc);
        proc.chdir = function chdir(directory) {
          if (typeof directory !== "string") throw errInvalidArgType("directory", "string", directory);
          return origChdir(directory);
        };
      }
    } catch (e) {}

    // ---- nextTick callback validation ---------------------------------------
    // node: if the first argument is not a function, throw ERR_INVALID_ARG_TYPE
    // synchronously (test-process-next-tick). Delegate to the real scheduler.
    try {
      if (typeof proc.nextTick === "function") {
        const origNextTick = proc.nextTick;
        proc.nextTick = function nextTick(callback) {
          if (typeof callback !== "function") throw errInvalidArgType("callback", "function", callback);
          return origNextTick.apply(this, arguments);
        };
      }
    } catch (e) {}

    // ---- setSourceMapsEnabled boolean validation ----------------------------
    // node validateBoolean(val, 'val') (test-process-setsourcemapsenabled).
    try {
      let smEnabled = false;
      proc.setSourceMapsEnabled = function setSourceMapsEnabled(val) {
        if (typeof val !== "boolean") throw errInvalidArgType("val", "boolean", val);
        smEnabled = val;
      };
      if (typeof proc.getSourceMapsEnabled !== "function")
        proc.getSourceMapsEnabled = function getSourceMapsEnabled() { return smEnabled; };
    } catch (e) {}

    // ---- process.features shape (node lib/internal/bootstrap/node.js) -------
    // test-process-features asserts EXACTLY these 13 keys, all booleans (quic
    // may be undefined, typescript may be a string — booleans satisfy both).
    try {
      const f = proc.features || (proc.features = {});
      const defaults = {
        inspector: false, debug: false, uv: true, ipv6: true,
        openssl_is_boringssl: false, quic: false, tls_alpn: true, tls_sni: true,
        tls_ocsp: true, tls: true, cached_builtins: true, require_module: true,
        typescript: false,
      };
      for (const k in defaults) if (typeof f[k] === "undefined") f[k] = defaults[k];
    } catch (e) {}

    // ---- process.abort ------------------------------------------------------
    // A method-shorthand function has no `.prototype` and is non-constructable,
    // matching node's C++ builtin (test-process-abort checks both). Never called
    // by the test; the body is a best-effort SIGABRT.
    if (typeof proc.abort !== "function") {
      const abortImpl = {
        abort() {
          try { if (typeof proc.kill === "function" && typeof proc.pid === "number") proc.kill(proc.pid, "SIGABRT"); } catch (e) {}
          try { if (typeof proc.reallyExit === "function") return proc.reallyExit(134); } catch (e) {}
          try { return proc.exit(134); } catch (e) {}
        },
      };
      proc.abort = abortImpl.abort;
    }

    // ---- uncaughtException capture callback registry ------------------------
    if (typeof proc.setUncaughtExceptionCaptureCallback !== "function") {
      let captureFn = null;
      proc.setUncaughtExceptionCaptureCallback = function setUncaughtExceptionCaptureCallback(fn) {
        if (fn === null) { captureFn = null; return; }
        if (typeof fn !== "function") throw errInvalidArgType("fn", "function or null", fn);
        if (captureFn !== null) {
          const e = new Error("`process.setUncaughtExceptionCaptureCallback()` was called while a capture callback was already active");
          e.code = "ERR_UNCAUGHT_EXCEPTION_CAPTURE_ALREADY_SET";
          throw e;
        }
        captureFn = fn;
      };
      proc.hasUncaughtExceptionCaptureCallback = function hasUncaughtExceptionCaptureCallback() { return captureFn !== null; };
      // Expose for the runtime's uncaught-exception dispatch to consult.
      Object.defineProperty(proc, "_mbunUncaughtCaptureCallback", {
        get() { return captureFn; }, configurable: true, enumerable: false,
      });
    }

    // ---- shared uncaught-exception escalation -------------------------------
    // node's process._fatalException, as far as the JS layer can express it: a
    // capture callback registered through setUncaughtExceptionCaptureCallback
    // wins (this is the seam node:domain drives), then 'uncaughtException'
    // listeners. Returns whether anybody claimed the exception.
    //
    // Callers are the JS-side callback dispatchers that own a try/catch and so
    // have to decide the fate of a throw themselves. The timer drain used to
    // `catch (e) {}`, so an exception from a setTimeout/setInterval callback
    // reached NEITHER of these and `process.domain` never saw it.
    //
    // DEFERRED (deliberately not done here): node also makes an unclaimed
    // exception FATAL — print + exit(1). mbun currently swallows it, and a
    // 200-file corpus sample shows at least two files whose "pass" today comes
    // precisely from that swallow (test-async-wrap-promise-after-enabled and
    // test-http2-compat-serverrequest-pause both assert inside a timer callback
    // and fail the assertion). Turning it fatal is correct but is a corpus-wide
    // accounting change, so it belongs in its own measured round rather than
    // riding along with a streams/domain fix.
    Object.defineProperty(G, "__mbunEmitUncaught", {
      configurable: true, enumerable: false, writable: true,
      value: function (err, origin) {
        const p = G.process;
        try {
          const cap = p && p._mbunUncaughtCaptureCallback;
          if (typeof cap === "function") { cap(err); return true; }
          if (p && typeof p.listenerCount === "function" && p.listenerCount("uncaughtException") > 0) {
            p.emit("uncaughtException", err, origin || "uncaughtException");
            return true;
          }
        } catch (e) {}
        return false;
      },
    });

    // ---- unhandled promise rejection dispatch -------------------------------
    // The runtime's JSC rejection hook calls this with (reason, promise). This is
    // a port of node lib/internal/process/promises.js, INCLUDING its five
    // --unhandled-rejections modes. Returning false means nobody claimed the
    // rejection and the runtime falls back to its own fatal report.
    //
    // The mode is read off process.execArgv at the point a rejection is observed
    // (not snapshotted at startup) for the same reason __mbun_fatal_should_abort
    // does it: execArgv is populated after the builtins image is evaluated, so a
    // startup snapshot would silently ignore the command-line flag.
    //
    // `noSideEffectsToString` renders `reason` WITHOUT running user code: node
    // formats it through v8's ToDetailString, so `{ toString() { ... } }` prints
    // "[object Object]" rather than calling the method (js/node/promise/
    // reject-tostring.test.ts asserts exactly that).
    const noSideEffectsToString = function (reason) {
      const t = typeof reason;
      if (t === "string") return reason;
      if (t === "symbol") return reason.toString();
      if (t === "bigint") return String(reason);
      if (reason === null) return "null";
      if (t === "undefined") return "undefined";
      if (t === "object" || t === "function") {
        try { return Object.prototype.toString.call(reason); } catch (e) { return "[object Object]"; }
      }
      return String(reason);
    };
    // node's isErrorLike: an own `stack` property, NOT `instanceof Error`. A
    // class that only calls Error.captureStackTrace(this) is rejected as-is and
    // must NOT be wrapped (test-promise-unhandled-warn asserts the warning stack
    // still names the function that rejected).
    const isErrorLike = function (o) {
      return (typeof o === "object" && o !== null &&
              Object.prototype.hasOwnProperty.call(o, "stack")) || o instanceof Error;
    };
    const unhandledRejectionError = function (reason) {
      const err = new Error(
        "This error originated either by throwing inside of an async function " +
        "without a catch block, or by rejecting a promise which was not handled " +
        "with .catch(). The promise rejected with the reason \"" +
        noSideEffectsToString(reason) + "\".");
      err.code = "ERR_UNHANDLED_REJECTION";
      // node's UnhandledPromiseRejection sets `name` as an own field, which is
      // what err.toString() and the corpus's err.name assertions read.
      Object.defineProperty(err, "name", {
        value: "UnhandledPromiseRejection", writable: true, configurable: true,
      });
      return err;
    };
    let lastPromiseId = 0;
    const emitUnhandledRejectionWarning = function (p, reason, uid) {
      const warning = new Error(
        "Unhandled promise rejection. This error originated either by throwing " +
        "inside of an async function without a catch block, or by rejecting a " +
        "promise which was not handled with .catch(). To terminate the node " +
        "process on unhandled promise rejection, use the CLI flag " +
        "`--unhandled-rejections=strict` (see " +
        "https://nodejs.org/api/cli.html#cli_unhandled_rejections_mode). " +
        "(rejection id: " + uid + ")");
      Object.defineProperty(warning, "name", {
        value: "UnhandledPromiseRejectionWarning", writable: true, configurable: true,
      });
      try {
        if (isErrorLike(reason)) {
          warning.stack = reason.stack;
          p.emitWarning(reason.stack, "UnhandledPromiseRejectionWarning");
        } else {
          p.emitWarning(noSideEffectsToString(reason), "UnhandledPromiseRejectionWarning");
        }
      } catch (e) {
        try { p.emitWarning(noSideEffectsToString(reason), "UnhandledPromiseRejectionWarning"); }
        catch (e2) {}
      }
      p.emitWarning(warning);
    };
    // getUnhandledRejectionsMode(): the LAST spelling on the command line wins,
    // matching node's option parser. Both `--flag=value` and `--flag value` are
    // accepted because execArgv preserves whichever form was typed.
    const unhandledRejectionsMode = function (p) {
      let mode = "throw";
      try {
        const argv = p.execArgv;
        if (Array.isArray(argv)) {
          for (let i = 0; i < argv.length; i++) {
            const a = argv[i];
            if (typeof a !== "string") continue;
            if (a.startsWith("--unhandled-rejections=")) mode = a.slice(23);
            else if (a === "--unhandled-rejections" && i + 1 < argv.length) mode = argv[i + 1];
          }
        }
      } catch (e) {}
      return mode;
    };
    const raiseUncaught = function (p, err) {
      const cap = p._mbunUncaughtCaptureCallback;
      if (typeof cap === "function") { cap(err); return true; }
      if (p.listenerCount("uncaughtException") > 0) {
        p.emit("uncaughtException", err, "unhandledRejection");
        return true;
      }
      return false;
    };
    G.__mbunOnUnhandledRejection = function (reason, promise) {
      try {
        const p = G.process;
        if (!p || typeof p.emit !== "function" || typeof p.listenerCount !== "function") return false;
        const uid = ++lastPromiseId;
        const mode = unhandledRejectionsMode(p);
        const emitRejection = function () {
          return p.listenerCount("unhandledRejection") > 0 &&
                 p.emit("unhandledRejection", reason, promise);
        };
        // --unhandled-rejections=none: deliver the event, never warn, never exit.
        if (mode === "none") { emitRejection(); return true; }
        // =warn: deliver the event AND always warn (twice: the reason, then the
        // note explaining where the warning came from).
        if (mode === "warn") {
          emitRejection();
          emitUnhandledRejectionWarning(p, reason, uid);
          return true;
        }
        // =warn-with-error-code: warn only when unclaimed, and mark the process
        // failed without killing it.
        if (mode === "warn-with-error-code") {
          if (!emitRejection()) {
            emitUnhandledRejectionWarning(p, reason, uid);
            p.exitCode = 1;
          }
          return true;
        }
        // =strict: raise the uncaught exception FIRST, then still deliver
        // 'unhandledRejection' (and warn if nothing listened for it).
        if (mode === "strict") {
          const err = isErrorLike(reason) ? reason : unhandledRejectionError(reason);
          if (!raiseUncaught(p, err)) return false;
          if (!emitRejection()) emitUnhandledRejectionWarning(p, reason, uid);
          return true;
        }
        // =throw (node's default since v15): the event wins; only an unclaimed
        // rejection escalates to an uncaught exception.
        if (emitRejection()) return true;
        return raiseUncaught(p, isErrorLike(reason) ? reason : unhandledRejectionError(reason));
      } catch (e) {}
      return false;
    };

    // ---- release / config / versions alignment (mirrors bun) ---------------
    try {
      const release = proc.release || (proc.release = { name: "node" });
      if (!release.sourceUrl) {
        const platform = proc.platform === "win32" ? "windows" : proc.platform;
        const arch = { arm64: "aarch64", x64: "x64" }[proc.arch] || proc.arch;
        const bunV = (proc.versions && proc.versions.bun) || "0.0.0";
        release.sourceUrl = `https://github.com/oven-sh/bun/releases/download/bun-v${bunV}/bun-${platform}-${arch}.zip`;
      }
      if (!release.headersUrl) release.headersUrl = "";
      if (!release.libUrl) release.libUrl = "";
    } catch (e) {}

    try {
      const config = proc.config || (proc.config = {});
      const variables = config.variables || (config.variables = {});
      if (typeof variables.clang !== "number") variables.clang = 1;
      if (variables.host_arch === undefined) variables.host_arch = proc.arch || "x64";
      if (variables.target_arch === undefined) variables.target_arch = proc.arch || "x64";
      // node exposes the native ABI version here (test-module-version asserts it
      // is an integer > 0). Mirror process.versions.modules (the same value).
      if (variables.node_module_version === undefined) {
        const nmv = parseInt(proc.versions && proc.versions.modules, 10);
        variables.node_module_version = Number.isInteger(nmv) && nmv > 0 ? nmv : 127;
      }
      if (config.target_defaults === undefined) config.target_defaults = {};
      // node deep-freezes process.config (lib/internal/bootstrap/node.js): in
      // strict mode `process.config.variables = 42` must throw a TypeError
      // (test-process-config). Freeze after populating.
      const deepFreeze = (o) => {
        if (o && typeof o === "object" && !Object.isFrozen(o)) {
          Object.freeze(o);
          for (const k of Object.keys(o)) deepFreeze(o[k]);
        }
      };
      deepFreeze(config);
    } catch (e) {}

    try {
      const versions = proc.versions;
      if (versions && typeof versions === "object") {
        // Pinned dependency commits, kept byte-identical with bun's
        // scripts/build/deps/*.ts (asserted verbatim by process.test.js).
        const pins = {
          boringssl: "1a41b9025c2c0a37edd07ff10f6944f03e028522",
          libarchive: "ded82291ab41d5e355831b96b0e1ff49e24d8939",
          mimalloc: "afb41757285694f832e7a2f164d35f5717457f96",
          picohttpparser: "066d2b1e9ab820703db0837a7255d92d30f0c9f5",
          zlib: "12731092979c6d07f42da27da673a9f6c7b13586",
          tinycc: "12882eee073cfe5c7621bcfadf679e1372d4537b",
          lolhtml: "77127cd2b8545998756e8d64e36ee2313c4bb312",
          ares: "3ac47ee46edd8ea40370222f91613fc16c434853",
          libdeflate: "c8c56a20f8f621e6a966b716b31f1dedab6a41e3",
          zstd: "f8745da6ff1ad1e7bab384bd1f9d742439278e99",
          lshpack: "8905c024b6d052f083a3d11d0a169b3c2735c8a1",
        };
        for (const k in pins) if (versions[k] === undefined) versions[k] = pins[k];
        if (versions.usockets === undefined) versions.usockets = "0.8.8";
        if (versions.uwebsockets === undefined) versions.uwebsockets = versions.usockets;
        if (versions.icu === undefined) versions.icu = "76.1";
        if (versions.webkit === undefined) versions.webkit = "mbun-jsc";
        if (versions.zig === undefined) versions.zig = "0.14.1";
      }
    } catch (e) {}

    // ---- globalThis.gc under --expose-gc -----------------------------------
    // node only defines the global `gc` when started with --expose-gc; tests
    // that need it declare the flag in their `// Flags:` header and otherwise
    // bail out with "Run this test with --expose-gc". The flag reaches JS via
    // process.execArgv, which is only populated after the builtins image has
    // been evaluated — hence an accessor that resolves on first read rather
    // than a value installed here. Without the flag the getter yields
    // undefined, so `typeof gc === "function"` stays false exactly as before.
    try {
      if (!("gc" in G)) {
        const collect = (full) => {
          try { if (G.Bun && typeof G.Bun.gc === "function") return G.Bun.gc(full !== false); } catch (e) {}
          return undefined;
        };
        Object.defineProperty(G, "gc", {
          configurable: true,
          enumerable: false,
          get() {
            const argv = (G.process && G.process.execArgv) || [];
            for (const a of argv) {
              // V8 accepts both spellings and Node's own corpus uses the
              // underscore form in `// Flags:` headers.  execArgv deliberately
              // preserves the spelling it was given, so recognize both here
              // without exposing gc for an unrelated flag.
              if (a === "--expose-gc" || a === "--expose_gc" ||
                  (typeof a === "string" &&
                    (a.startsWith("--expose-gc=") || a.startsWith("--expose_gc=")))) {
                return collect;
              }
            }
            return undefined;
          },
          set(v) {
            Object.defineProperty(G, "gc", {
              value: v, writable: true, configurable: true, enumerable: false,
            });
          },
        });
      }
    } catch (e) {}

    // ---- V8's --expose_externalize_string globals --------------------------
    // Same contract as `gc` above: these four exist ONLY when the flag is
    // present, because common.js fails a test that leaks an unexpected global.
    // V8's versions poke at string representation; what the corpus actually
    // observes is (a) that they exist and (b) that isOneByteString reports
    // whether the string is Latin-1 — which is the same question JSC's 8-bit
    // string flag answers (test-fs-write writes both kinds through fs.write).
    try {
      const hasFlag = () => {
        const argv = (G.process && G.process.execArgv) || [];
        for (const a of argv)
          if (a === "--expose_externalize_string" || a === "--expose-externalize-string") return true;
        return false;
      };
      const defs = {
        createExternalizableString: (s) => String(s),
        createExternalizableTwoByteString: (s) => String(s),
        externalizeString: () => undefined,
        isOneByteString: (s) => {
          const str = String(s);
          for (let i = 0; i < str.length; ++i) if (str.charCodeAt(i) > 0xff) return false;
          return true;
        },
      };
      for (const name of Object.keys(defs)) {
        if (name in G) continue;
        Object.defineProperty(G, name, {
          configurable: true,
          enumerable: false,
          get() { return hasFlag() ? defs[name] : undefined; },
          set(v) {
            Object.defineProperty(G, name, {
              value: v, writable: true, configurable: true, enumerable: false,
            });
          },
        });
      }
    } catch (e) {}
  } catch (e) {}
  // Last: if this process was fork()ed with an IPC channel, wire
  // process.send/'message'/disconnect now that `process` is a full EventEmitter
  // (the plumbing itself lives in the process_web partition).
  try { if (typeof globalThis.__mbunSetupIpcChild === "function") globalThis.__mbunSetupIpcChild(); } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
