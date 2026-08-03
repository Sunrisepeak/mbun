// Legacy (pre-class) constructor call forms for the node builtins.
//
// Node's stream/net/http constructors are ES5 functions, so both historical
// spellings work and its own corpus uses them freely:
//
//   const server = http.Server(handler);          // factory call, no `new`
//   function MyStream(o) { Readable.call(this, o); }   // util.inherits pattern
//
// mbun implements those types as ES classes, where either form is a hard
// "TypeError: Cannot call a class constructor Server without |new|" on the
// first line of the test. This partition re-exports the affected classes
// through a Proxy whose `apply` trap restores both forms — construct behavior,
// prototype identity, statics and instanceof are the target's, untouched.
//
// NOTE: appended AFTER the master builtins IIFE, so this is a self-contained
// IIFE that re-binds G = globalThis. Top level must never throw.
export module mbun.jsc.js_builtins:node_legacy_ctors;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeLegacyCtorsJS = R"JS(
(function () {
  const G = globalThis;
  try {
    const M = G.__mbunNativeModules;
    if (!M) return;

    // A plain function standing in for the class: it shares the class's
    // prototype object and static side (setPrototypeOf), so instanceof,
    // subclassing and every static keep working, while a call without `new`
    // becomes legal again. A Proxy would be shorter but changes identity:
    // `new Readable().constructor` is the target class, not the exported
    // wrapper, which node's own contract (and bun's node-stream corpus,
    // "#9242.6 Readable has constructor") requires to be the same object — so
    // the prototype's `constructor` is re-pointed at the wrapper here.
    const callable = (Cls) => {
      if (typeof Cls !== "function") return Cls;
      const wrapper = function (...args) {
        if (new.target !== undefined) return Reflect.construct(Cls, args, new.target);
        // `Ctor.call(this, opts)` (util.inherits): the receiver's prototype
        // chain already reaches the class — that is the signal. A plain
        // `http.Server(fn)` passes the module object as `this` instead, which
        // is not an instance and gets the constructed value back. Node
        // initializes the receiver in place; transplant the instance's state.
        if (this !== null && this !== undefined && typeof this === "object" &&
            this instanceof Cls) {
          Object.defineProperties(this, Object.getOwnPropertyDescriptors(
            Reflect.construct(Cls, args)));
          return undefined;
        }
        return Reflect.construct(Cls, args);
      };
      try {
        Object.setPrototypeOf(wrapper, Cls);
        wrapper.prototype = Cls.prototype;
        Object.defineProperty(wrapper, "name", { value: Cls.name, configurable: true });
        Object.defineProperty(Cls.prototype, "constructor",
                              { value: wrapper, writable: true, configurable: true });
      } catch (e) { return Cls; }
      return wrapper;
    };

    const patch = (moduleName, names) => {
      const mod = M[moduleName];
      if (!mod) return;
      for (const name of names) {
        const value = mod[name];
        if (typeof value !== "function") continue;
        // A getter-backed export (http's lazy globals) must stay lazy.
        const descriptor = Object.getOwnPropertyDescriptor(mod, name);
        if (!descriptor || descriptor.get || !descriptor.configurable) continue;
        try { mod[name] = callable(value); } catch (e) {}
      }
    };

    // Deliberately limited to node:http/node:https. The same treatment on
    // node:stream / node:net / node:tls regressed measured corpus files
    // (test-stream-inheritance, test-pipe-abstract-socket, test-pipe-unref,
    // test-tls-ip-servername-forbidden, test-fs-watch): those subsystems key
    // behavior off constructor identity in ways this stand-in does not
    // reproduce, so they keep the class form until that is understood.
    patch("http", ["Server", "Agent", "ClientRequest", "OutgoingMessage", "IncomingMessage",
                   "ServerResponse"]);
    patch("https", ["Server", "Agent"]);
    for (const alias of ["node:http", "node:https"]) {
      // The `node:`-prefixed entries are the same object in this table, so they
      // already see the patch; nothing to do unless they were cloned.
      const bare = alias.slice(5);
      if (M[alias] && M[bare] && M[alias] !== M[bare]) {
        patch(alias, Object.keys(M[bare]).filter((k) => typeof M[bare][k] === "function"));
      }
    }
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
