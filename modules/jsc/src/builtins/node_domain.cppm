// node:domain — the (deprecated, still shipped) error-context API.
//
// A translation of node lib/domain.js rather than a re-invention: the domains
// STACK is the observable contract (test-domain-emit-error-handler-stack,
// -thrown-error-handler-stack and -top-level-error-handler-clears-stack read
// `domain._stack` and `process.domain` from inside an error handler and from a
// nextTick scheduled by it), and the ad-hoc version that preceded this got the
// error-marking wrong in every direction: `domain`/`domainThrown`/`domainEmitter`
// /`domainBound` were enumerable, missing, or set to the wrong value, and every
// error was funnelled through one `__handle` that could not tell a thrown error
// from an emitted one.
//
// Two deliberate substitutions for node's internals:
//
//   * node propagates the active domain across async boundaries with an
//     async_hooks init/before/after hook. mbun's `async_hooks.createHook` is a
//     no-op stub, so the same job is done by wrapping the scheduling primitives
//     (setTimeout/setInterval/setImmediate/queueMicrotask/process.nextTick):
//     each captures the domain that was active when the callback was SCHEDULED
//     and enters it around the call. The wrapper deliberately does not catch —
//     node's `bound()` doesn't either, so a throw leaves the domain entered and
//     reaches _errorHandler with `process.domain` still set, which is exactly
//     what the stack assertions check.
//   * node's process._fatalException is spelled here as the public
//     `process.setUncaughtExceptionCaptureCallback`, which mbun's uncaught
//     dispatch already consults first.
//
// Everything is installed LAZILY, on the first `require('domain')`, because
// node's is too: patching EventEmitter.init/emit and the timer functions for
// every process that never mentions domains would be both a behaviour change
// (every emitter gains a `domain` own property) and a permanent tax.
//
// NOTE: appended AFTER the master builtins IIFE, so this is a self-contained
// IIFE that re-binds G = globalThis. Top level must never throw.
export module mbun.jsc.js_builtins:node_domain;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeDomainJS = R"JS(
(function () {
  const G = globalThis;
  try {
    const M = G.__mbunNativeModules;
    if (!M) return;
    // NOTE: image_closure already pre-stubbed M["domain"] with `{}` (it fills
    // every BUILTIN_LIST name that has no module yet), so this must overwrite
    // unconditionally — an `if (M["domain"] !== undefined) return` guard here
    // silently hands every `require("domain")` that empty stub.

    let built;
    const build = () => {
      const events = M["events"];
      const EventEmitter = (events && (events.EventEmitter || events)) || null;
      if (typeof EventEmitter !== "function") return {};
      const process = G.process;

      const exports = { active: null };

      // process.domain over a one-slot cell, as node does.
      const _domain = [null];
      try {
        Object.defineProperty(process, "domain", {
          enumerable: true,
          configurable: true,
          get() { return _domain[0]; },
          set(value) { return (_domain[0] = value); },
        });
      } catch (e) {}

      // It's possible to enter one domain while already inside another one.
      // The stack is each entered domain.
      let stack = [];
      exports._stack = stack;

      const setUncaughtExceptionCaptureCallback =
        typeof process.setUncaughtExceptionCaptureCallback === "function"
          ? process.setUncaughtExceptionCaptureCallback.bind(process)
          : () => {};

      function updateExceptionCapture() {
        try {
          if (stack.every((domain) => domain.listenerCount("error") === 0)) {
            setUncaughtExceptionCaptureCallback(null);
          } else {
            setUncaughtExceptionCaptureCallback(null);
            setUncaughtExceptionCaptureCallback((er) => process.domain._errorHandler(er));
          }
        } catch (e) {}
      }

      function domainUncaughtExceptionClear() {
        stack.length = 0;
        exports.active = process.domain = null;
        updateExceptionCapture();
      }

      // The first 'uncaughtException' listener must always be the one that
      // clears the domains stack, so a user handler observes an empty stack.
      try {
        process.on("newListener", (name, listener) => {
          if (name === "uncaughtException" && listener !== domainUncaughtExceptionClear) {
            process.removeListener(name, domainUncaughtExceptionClear);
            process.prependListener(name, domainUncaughtExceptionClear);
          }
        });
        process.on("removeListener", (name, listener) => {
          if (name === "uncaughtException" && listener !== domainUncaughtExceptionClear) {
            const listeners = process.listeners("uncaughtException");
            if (listeners.length === 1 && listeners[0] === domainUncaughtExceptionClear) {
              process.removeListener(name, domainUncaughtExceptionClear);
            }
          }
        });
      } catch (e) {}

      const defineDomain = (target, value) => {
        try {
          Object.defineProperty(target, "domain", {
            configurable: true, enumerable: false, value, writable: true,
          });
        } catch (e) {}
      };

      class Domain extends EventEmitter {
        constructor() {
          super();
          this.members = [];
          this.on("removeListener", updateExceptionCapture);
          this.on("newListener", updateExceptionCapture);
        }
      }

      exports.Domain = Domain;
      exports.create = exports.createDomain = function createDomain() { return new Domain(); };
      Domain.prototype.members = undefined;

      // Called when an error was thrown under this domain.
      Domain.prototype._errorHandler = function (er) {
        let caught = false;

        if ((typeof er === "object" && er !== null) || typeof er === "function") {
          defineDomain(er, this);
          try { er.domainThrown = true; } catch (e) {}
        }

        // Pop all adjacent duplicates of the currently active domain, so a
        // domain's error handler never runs within its own context.
        while (exports.active === this) this.exit();

        if (stack.length === 0) {
          // With no handler, emitting 'error' would throw and prevent the
          // process 'uncaughtException' event from being emitted at all.
          if (this.listenerCount("error") > 0) {
            setUncaughtExceptionCaptureCallback(null);
            try { caught = this.emit("error", er); }
            finally { updateExceptionCapture(); }
          }
        } else {
          try {
            caught = this.emit("error", er);
          } catch (er2) {
            // The domain error handler threw: see if a parent domain can catch
            // THIS error, else crash on it.
            updateExceptionCapture();
            if (stack.length) {
              exports.active = process.domain = stack[stack.length - 1];
              caught = process.domain._errorHandler(er2);
            } else {
              throw er2;
            }
          }
        }

        // Uncaught exceptions end the current tick; no domain may be left on
        // the stack between ticks.
        domainUncaughtExceptionClear();
        return caught;
      };

      Domain.prototype.enter = function () {
        // This may be a no-op, but it still has to be pushed so it can be
        // popped later.
        exports.active = process.domain = this;
        stack.push(this);
        updateExceptionCapture();
      };

      Domain.prototype.exit = function () {
        const index = stack.lastIndexOf(this);
        if (index === -1) return;
        // Exit all domains until this one.
        stack.splice(index);
        exports.active = stack.length === 0 ? undefined : stack[stack.length - 1];
        process.domain = exports.active;
        updateExceptionCapture();
      };

      // Note: this works for timers as well.
      Domain.prototype.add = function (ee) {
        if (ee.domain === this) return;
        if (ee.domain) ee.domain.remove(ee);
        // Circular Domain->Domain links cause a stack overflow on emit.
        if (this.domain && ee instanceof Domain) {
          for (let d = this.domain; d; d = d.domain) if (ee === d) return;
        }
        defineDomain(ee, this);
        this.members.push(ee);
      };

      Domain.prototype.remove = function (ee) {
        ee.domain = null;
        const index = this.members.indexOf(ee);
        if (index !== -1) this.members.splice(index, 1);
      };

      Domain.prototype.run = function (fn) {
        this.enter();
        const ret = Reflect.apply(fn, this, Array.prototype.slice.call(arguments, 1));
        this.exit();
        return ret;
      };

      function intercepted(_this, self, cb, fnargs) {
        if (fnargs[0] && fnargs[0] instanceof Error) {
          const er = fnargs[0];
          er.domainBound = cb;
          er.domainThrown = false;
          defineDomain(er, self);
          self.emit("error", er);
          return;
        }
        self.enter();
        const ret = Reflect.apply(cb, _this, Array.prototype.slice.call(fnargs, 1));
        self.exit();
        return ret;
      }

      Domain.prototype.intercept = function (cb) {
        const self = this;
        return function runIntercepted() { return intercepted(this, self, cb, arguments); };
      };

      function bound(_this, self, cb, fnargs) {
        self.enter();
        const ret = Reflect.apply(cb, _this, fnargs);
        self.exit();
        return ret;
      }

      Domain.prototype.bind = function (cb) {
        const self = this;
        function runBound() { return bound(this, self, cb, arguments); }
        defineDomain(runBound, this);
        return runBound;
      };

      // Deprecated; node keeps it only as a member detach + exit.
      Domain.prototype.dispose = function () {
        for (const member of this.members.slice()) this.remove(member);
        this.exit();
        return this;
      };

      // ---- EventEmitter integration (node lib/domain.js tail) ----
      EventEmitter.usingDomains = true;

      const eventInit = EventEmitter.init;
      EventEmitter.init = function (opts) {
        defineDomain(this, null);
        if (exports.active && !(this instanceof Domain)) this.domain = exports.active;
        return eventInit.call(this, opts);
      };

      const eventEmit = EventEmitter.prototype.emit;
      EventEmitter.prototype.emit = function emit(...args) {
        const domain = this.domain;

        const type = args[0];
        const shouldEmitError = type === "error" && this.listenerCount(type) > 0;

        // Plain emit when the instance handles 'error' itself, there is no
        // domain, or this is process.
        if (shouldEmitError || domain === null || domain === undefined || this === process) {
          return Reflect.apply(eventEmit, this, args);
        }

        if (type === "error") {
          let er;
          if (args.length > 1 && args[1]) {
            er = args[1];
          } else {
            er = new Error("Unhandled error.");
            er.code = "ERR_UNHANDLED_ERROR";
          }

          if (typeof er === "object") {
            er.domainEmitter = this;
            defineDomain(er, domain);
            er.domainThrown = false;
          }

          // Remove the active domain (and its duplicates) from the stack so the
          // handler doesn't run in its own context, then restore afterwards.
          const origDomainsStack = stack.slice();
          const origActiveDomain = process.domain;

          let idx = stack.length - 1;
          while (idx > -1 && process.domain === stack[idx]) --idx;

          if (idx < 0) stack.length = 0;
          else stack.splice(idx + 1);

          if (stack.length > 0) exports.active = process.domain = stack[stack.length - 1];
          else exports.active = process.domain = null;

          updateExceptionCapture();

          domain.emit("error", er);

          exports._stack = stack = origDomainsStack;
          exports.active = process.domain = origActiveDomain;
          updateExceptionCapture();

          return false;
        }

        domain.enter();
        const ret = Reflect.apply(eventEmit, this, args);
        domain.exit();
        return ret;
      };

      // ---- scheduling propagation (stands in for node's async_hooks hook) ----
      // Re-enter the domain that was active when the callback was scheduled.
      //
      // The catch is what node's process._fatalException does one frame higher:
      // it hands the thrown value to the active domain's _errorHandler. mbun
      // cannot rely on that here because process.nextTick rides on
      // queueMicrotask, and a throw out of a microtask surfaces through the
      // unhandled-REJECTION path, which wraps a non-Error thrown value in an
      // ERR_UNHANDLED_REJECTION Error — so `d.run(() => process.nextTick(() => {
      // throw 42 }))` delivered that wrapper to the handler instead of 42
      // (test-domain-error-types / -multiple-errors throw 42, null, undefined,
      // false, a function, a string and a Symbol and assert identity).
      // The domain is still entered when the catch runs, exactly as in node, so
      // _errorHandler sees the stack it expects; an error nobody claimed is
      // re-thrown so it stays fatal.
      const wrap = (callback) => {
        if (typeof callback !== "function") return callback;
        const domain = process.domain;
        if (domain === null || domain === undefined) return callback;
        return function () {
          domain.enter();
          let ret;
          try {
            ret = Reflect.apply(callback, this, arguments);
          } catch (er) {
            if (!domain._errorHandler(er)) throw er;
            return undefined;
          }
          domain.exit();
          return ret;
        };
      };
      const hook = (holder, name) => {
        const original = holder && holder[name];
        if (typeof original !== "function") return;
        const hooked = function (callback, ...rest) {
          return Reflect.apply(original, this, [wrap(callback), ...rest]);
        };
        try {
          // Keep the original's own properties (util.promisify's
          // `__promisify__`, custom symbols) — the hook must be invisible.
          for (const key of Reflect.ownKeys(original)) {
            if (key === "length" || key === "name" || key === "prototype") continue;
            const descriptor = Object.getOwnPropertyDescriptor(original, key);
            if (descriptor) Object.defineProperty(hooked, key, descriptor);
          }
          Object.defineProperty(hooked, "name", { value: name, configurable: true });
          holder[name] = hooked;
        } catch (e) {}
      };
      hook(G, "setTimeout");
      hook(G, "setInterval");
      hook(G, "setImmediate");
      hook(G, "queueMicrotask");
      try { hook(process, "nextTick"); } catch (e) {}

      return exports;
    };

    const accessor = {
      configurable: true,
      enumerable: true,
      get() {
        if (built === undefined) {
          built = {};              // break re-entrancy while building
          built = build() || {};
        }
        return built;
      },
    };
    Object.defineProperty(M, "domain", accessor);
    Object.defineProperty(M, "node:domain", accessor);
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
