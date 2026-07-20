// node:domain — the (deprecated, still shipped) error-context API.
//
// mbun had no `domain` module at all: `require('domain').create()` threw
// "domain.create is not a function" on the first line of every test that uses
// it. This is node lib/domain.js's observable surface implemented on top of the
// runtime's own scheduling primitives: a Domain is an EventEmitter with
// run/bind/intercept/add/remove/enter/exit, an error raised under it is
// re-emitted as its "error" event (and rethrown when it has no listener), and
// the active domain is restored around callbacks scheduled while it was
// entered (process.nextTick / setTimeout / setInterval / setImmediate) and
// around an emit on a member emitter.
//
// The scheduling hooks are strictly inert when no domain is entered
// (`process.domain === null`, i.e. always outside a domain test): they check one
// module-local and call straight through.
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
    const events = M["events"];
    const EventEmitter = (events && (events.EventEmitter || events)) || null;
    if (typeof EventEmitter !== "function") return;

    const stack = [];
    let active = null;
    const setActive = (d) => {
      active = d;
      try { G.process.domain = d; } catch (e) {}
      domainExports.active = d;
    };

    class Domain extends EventEmitter {
      constructor() {
        super();
        this.members = [];
      }
      enter() {
        stack.push(this);
        setActive(this);
        return this;
      }
      exit() {
        const index = stack.lastIndexOf(this);
        if (index >= 0) stack.splice(index, stack.length - index);
        setActive(stack.length ? stack[stack.length - 1] : null);
        return this;
      }
      add(emitter) {
        if (!emitter || emitter.domain === this) return;
        if (emitter.domain) emitter.domain.remove(emitter);
        // node refuses to add an emitter to a disposed/ancestor domain; the
        // observable part is only the back-pointer and the member list.
        emitter.domain = this;
        this.members.push(emitter);
      }
      remove(emitter) {
        if (!emitter) return false;
        emitter.domain = null;
        const index = this.members.indexOf(emitter);
        if (index === -1) return false;
        this.members.splice(index, 1);
        return true;
      }
      run(fn, ...args) {
        this.enter();
        try {
          return fn.apply(this, args);
        } catch (error) {
          this.__handle(error);
          return undefined;
        } finally {
          this.exit();
        }
      }
      bind(callback) {
        const self = this;
        const bound = function (...args) {
          self.enter();
          try {
            return callback.apply(this, args);
          } catch (error) {
            self.__handle(error);
            return undefined;
          } finally {
            self.exit();
          }
        };
        bound.domain = this;
        return bound;
      }
      intercept(callback) {
        const self = this;
        const intercepted = function (error, ...args) {
          if (error) { self.__handle(error); return undefined; }
          self.enter();
          try {
            return callback.apply(this, args);
          } catch (thrown) {
            self.__handle(thrown);
            return undefined;
          } finally {
            self.exit();
          }
        };
        intercepted.domain = this;
        return intercepted;
      }
      dispose() {
        // Deprecated no-op in modern node beyond detaching the members.
        for (const member of this.members.slice()) this.remove(member);
        this.exit();
        return this;
      }
      // node marks the error with its domain context, emits it when the domain
      // has an "error" listener, and otherwise lets it escape as uncaught.
      __handle(error) {
        if (error !== null && typeof error === "object") {
          try {
            error.domain = this;
            error.domainThrown = true;
          } catch (e) {}
        }
        if (this.listenerCount("error") > 0) { this.emit("error", error); return; }
        throw error;
      }
    }

    // Re-enter the domain that was active when the callback was scheduled.
    const wrap = (callback) => {
      if (typeof callback !== "function" || active === null) return callback;
      return active.bind(callback);
    };
    const hook = (holder, name) => {
      const original = holder && holder[name];
      if (typeof original !== "function") return;
      const hooked = function (callback, ...rest) {
        return original.call(this, wrap(callback), ...rest);
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
    try { hook(G.process, "nextTick"); } catch (e) {}

    // An emit on an emitter that belongs to a domain runs inside it, so a throw
    // from a listener (and an unhandled "error" event) lands on the domain.
    const emit = EventEmitter.prototype.emit;
    EventEmitter.prototype.emit = function (...args) {
      const owner = this.domain;
      if (!owner || owner === active) return emit.apply(this, args);
      owner.enter();
      try {
        return emit.apply(this, args);
      } catch (error) {
        owner.__handle(error);
        return false;
      } finally {
        owner.exit();
      }
    };

    const create = () => new Domain();
    const domainExports = {
      Domain,
      create,
      createDomain: create,
      active: null,
      _stack: stack,
    };
    M["domain"] = M["node:domain"] = domainExports;
    try { if (G.process.domain === undefined) G.process.domain = null; } catch (e) {}
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
