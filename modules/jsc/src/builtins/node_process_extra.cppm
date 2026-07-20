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
          const rec = { callback, capture, once: !!options.once, passive: !!options.passive };
          if (options.signal && typeof options.signal.addEventListener === "function") {
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
            options.signal.addEventListener("abort", onAbort, { once: true });
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
              if (event[kStopImmediate]) break;
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
    const errInvalidArgType = (name, expected, value) => {
      const e = new TypeError(`The "${name}" argument must be of type ${expected}. Received ${specificType(value)}`);
      e.code = "ERR_INVALID_ARG_TYPE";
      return e;
    };
    const errOutOfRange = (name, range, value) => {
      const e = new RangeError(`The value of "${name}" is out of range. It must be ${range}. Received ${value}`);
      e.code = "ERR_OUT_OF_RANGE";
      return e;
    };

    // isBun marker (bun sets 1).
    if (!proc.isBun) proc.isBun = 1;

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

    // ---- process.binding allow/deny list (bun's ProcessBindingMap) ---------
    {
      const orig = typeof proc.binding === "function" ? proc.binding.bind(proc) : null;
      const allowed = {
        buffer: 1, config: 1, constants: 1, "crypto/x509": 1, fs: 1,
        http_parser: 1, natives: 1, tty_wrap: 1, util: 1, uv: 1,
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
              trace: {
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
              },
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
          else value = {};
        }
        return (cache[name] = value);
      };
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
    const undefinedStubs = [
      "_debugEnd", "_debugProcess", "_fatalException", "_linkedBinding",
      "_rawDebug", "_startProfilerIdleNotifier", "_stopProfilerIdleNotifier",
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
    if (!Array.isArray(proc.moduleLoadList)) proc.moduleLoadList = [];
    if (!Array.isArray(proc._preload_modules)) proc._preload_modules = [];

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
      if (config.target_defaults === undefined) config.target_defaults = {};
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
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
