// MessageEvent / CloseEvent / ErrorEvent partition.
//
// bun generates these three from WebCore IDL; mbun had none of them, even
// though node_http.cppm:708 already hands G.MessageEvent / G.CloseEvent to
// the node:http websocket surface (they were plain `undefined` there). hono's
// createWSMessageEvent (src/helper/websocket/index.ts:95) does
// `new MessageEvent('message', { data })`, which threw
// "MessageEvent is not defined" and wedged the whole test file.
//
// Web IDL semantics that the JS-visible shape depends on, and which a plain
// `class ... { get x() {} }` would get WRONG:
//   - attributes are getter-only accessors ON THE PROTOTYPE and are
//     ENUMERABLE (class accessors are non-enumerable), so they are installed
//     with explicit defineProperty.
//   - every getter brand-checks and throws
//     "The <Iface>.<attr> getter can only be used on instances of <Iface>"
//     (pinned by bun test/js/node/test/parallel/test-messageevent-brandcheck.js,
//     which Reflect.get's all five MessageEvent attributes off a bare object).
//   - dictionary members default when the value is `undefined` (absent), so
//     { data: undefined } yields null, not undefined.
//   - `unsigned short` / `unsigned long` wrap (ToUint16 / ToUint32) rather
//     than clamp: CloseEvent code -1 -> 65535, 70000 -> 4464, NaN -> 0.
//   - MessageEvent.ports is a FrozenArray and every item must be a real
//     MessagePort, else TypeError.
//
// NOTE: appended AFTER the master builtins IIFE, and after node_process_extra
// (which installs the Event/EventTarget shim these three subclass).
// Self-contained IIFE that re-binds G = globalThis; must never throw at top
// level (a throw would silently disable later partitions).
//
// Blueprint: bun src/jsc/bindings/webcore/MessageEvent.idl, CloseEvent.idl,
// ErrorEvent.idl (+ the generated JS*Event.cpp brand checks).
export module mbun.jsc.js_builtins:web_events;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kWebEventsJS = R"JS(
(function () {
  const G = globalThis;
  try {
    const EventBase = G.Event;
    if (typeof EventBase !== "function") return;

    // Per-interface private slot bag. A WeakMap doubles as the brand: only
    // objects this constructor initialized are ever present.
    const defineIface = function (name, attrs, init) {
      const slots = new WeakMap();

      const Ctor = function (type, eventInitDict) {
        if (!new.target) {
          throw new TypeError(
            "Class constructor " + name + " cannot be invoked without 'new'");
        }
        if (arguments.length === 0) throw new TypeError("Not enough arguments");
        const self = Reflect.construct(
          EventBase, [type, eventInitDict], new.target);
        const o = eventInitDict === null || eventInitDict === undefined
          ? {} : eventInitDict;
        slots.set(self, init(o, self, eventInitDict));
        return self;
      };

      Ctor.prototype = Object.create(EventBase.prototype);
      Object.defineProperty(Ctor.prototype, "constructor", {
        value: Ctor, writable: true, enumerable: false, configurable: true,
      });
      Object.defineProperty(Ctor, "name",
        { value: name, configurable: true });
      Object.defineProperty(Ctor, "length", { value: 1, configurable: true });

      for (const attr of attrs) {
        Object.defineProperty(Ctor.prototype, attr, {
          get: function () {
            const bag = slots.get(this);
            if (bag === undefined) {
              throw new TypeError("The " + name + "." + attr +
                " getter can only be used on instances of " + name);
            }
            return bag[attr];
          },
          enumerable: true, configurable: true,
        });
      }

      Object.defineProperty(Ctor.prototype, Symbol.toStringTag, {
        value: name, writable: false, enumerable: false, configurable: true,
      });

      Object.defineProperty(G, name, {
        value: Ctor, writable: true, enumerable: false, configurable: true,
      });
      return { Ctor, slots };
    };

    // ------------------------------------------------ Web IDL type coercions
    // Dictionary members default when absent OR explicitly undefined.
    const orDefault = function (v, dflt) { return v === undefined ? dflt : v; };
    const toUint16 = function (v) { return Number(v) & 0xffff; };
    const toUint32 = function (v) { return Number(v) >>> 0; };

    // ------------------------------------------------------------ MessageEvent
    // Bun renders the offending value into the TypeError the way its inspector
    // would: 1 -> `1`, {} -> `{}`. JSON is the closest cheap equivalent for the
    // shapes these messages can carry; anything JSON refuses falls back to
    // ToString.
    const fmtValue = function (v) {
      if (typeof v === "string") return v;
      try { const s = JSON.stringify(v); if (s !== undefined) return s; } catch (e) {}
      return String(v);
    };
    const isMessagePort = function (v) {
      return typeof G.MessagePort === "function" && v instanceof G.MessagePort;
    };
    const me = defineIface(
      "MessageEvent",
      ["data", "origin", "lastEventId", "source", "ports"],
      function (o, self, raw) {
        // WebIDL dictionary conversion: anything that is neither
        // undefined/null nor an object is a TypeError before any member is
        // read (MessageEvent.idl -> convert<IDLDictionary<MessageEventInit>>).
        if (raw !== undefined && raw !== null &&
            typeof raw !== "object" && typeof raw !== "function") {
          throw new TypeError("MessageEvent constructor: The provided value " +
            "is not of type 'MessageEventInit'.");
        }
        let ports = orDefault(o.ports, undefined);
        if (ports === undefined) {
          ports = Object.freeze([]);
        } else {
          // sequence<MessagePort>: a non-iterable is reported as such, then
          // each item is brand-checked with its index in the message.
          if (ports === null || typeof ports[Symbol.iterator] !== "function") {
            throw new TypeError("MessageEvent constructor: eventInitDict.ports (" +
              fmtValue(ports) + ") is not iterable.");
          }
          ports = Array.from(ports);
          for (let i = 0; i < ports.length; i++) {
            if (!isMessagePort(ports[i])) {
              throw new TypeError("MessageEvent constructor: Expected " +
                "eventInitDict.ports[" + i + "] (\"" + fmtValue(ports[i]) +
                "\") to be an instance of MessagePort.");
            }
          }
          Object.freeze(ports);
        }
        const source = orDefault(o.source, null);
        if (source !== null && !isMessagePort(source)) {
          throw new TypeError("MessageEvent constructor: Expected " +
            "eventInitDict.source (\"" + fmtValue(source) +
            "\") to be an instance of MessagePort.");
        }
        return {
          data: orDefault(o.data, null),
          origin: String(orDefault(o.origin, "")),
          lastEventId: String(orDefault(o.lastEventId, "")),
          source,
          ports,
        };
      });

    // MessageEvent.idl: initMessageEvent(type, bubbles, cancelable, data,
    // originArg, lastEventId, source, messagePorts)
    Object.defineProperty(me.Ctor.prototype, "initMessageEvent", {
      value: function initMessageEvent(type, bubbles, cancelable, data,
        originArg, lastEventIdArg, source, messagePorts) {
        const bag = me.slots.get(this);
        if (bag === undefined) {
          throw new TypeError("The MessageEvent.initMessageEvent method can " +
            "only be used on instances of MessageEvent");
        }
        this.type = String(type);
        this.bubbles = !!bubbles;
        this.cancelable = !!cancelable;
        bag.data = orDefault(data, null);
        bag.origin = String(orDefault(originArg, ""));
        bag.lastEventId = String(orDefault(lastEventIdArg, ""));
        bag.source = orDefault(source, null);
        bag.ports = Object.freeze(
          messagePorts === undefined ? [] : Array.from(messagePorts));
      },
      writable: true, enumerable: true, configurable: true,
    });

    // -------------------------------------------------------------- CloseEvent
    defineIface("CloseEvent", ["wasClean", "code", "reason"], function (o) {
      return {
        wasClean: !!orDefault(o.wasClean, false),
        code: toUint16(orDefault(o.code, 0)),
        reason: String(orDefault(o.reason, "")),
      };
    });

    // -------------------------------------------------- BroadcastChannel
    // WHATWG HTML §9.4 (bun: WebCore JSBroadcastChannel global). mbun has no
    // worker threads yet, so same-context fan-out is the complete observable
    // surface. Delivers real MessageEvent instances (initMessageEvent works).
    {
      const reg = new Map(); // name -> Set<channel>
      const HANDLER = new WeakMap();
      const ERRHANDLER = new WeakMap();
      class BroadcastChannel extends G.EventTarget {
        constructor(name) {
          if (arguments.length === 0) throw new TypeError("BroadcastChannel constructor requires a name argument");
          super();
          this._name = String(name); this._closed = false;
          let s = reg.get(this._name); if (!s) reg.set(this._name, s = new Set());
          s.add(this);
        }
        get name() { return this._name; }
        get onmessage() { return HANDLER.get(this) || null; }
        set onmessage(f) {
          const prev = HANDLER.get(this);
          if (prev) this.removeEventListener("message", prev);
          if (typeof f === "function") { HANDLER.set(this, f); this.addEventListener("message", f); }
          else HANDLER.delete(this);
        }
        // WHATWG HTML §9.4 declares onmessageerror alongside onmessage; an
        // unset event handler IDL attribute reads back as null, never undefined.
        get onmessageerror() { return ERRHANDLER.get(this) || null; }
        set onmessageerror(f) {
          const prev = ERRHANDLER.get(this);
          if (prev) this.removeEventListener("messageerror", prev);
          if (typeof f === "function") { ERRHANDLER.set(this, f); this.addEventListener("messageerror", f); }
          else ERRHANDLER.delete(this);
        }
        // node lib/internal/worker/io.js BroadcastChannel[inspect.custom] and
        // bun both print the {name, active} projection through the caller's own
        // inspect options (so breakLength/compact are honoured), not the private
        // _name/_closed slots. ref node test-broadcastchannel-custom-inspect.
        [Symbol.for("nodejs.util.inspect.custom")](depth, options) {
          if (depth < 0) return this;
          const opts = Object.assign({}, options, {
            depth: options && options.depth == null ? null : (options.depth - 1),
          });
          let inspect = null;
          try {
            const M = G.__mbunNativeModules;
            const util = M && (M["util"] || M["node:util"]);
            if (util && typeof util.inspect === "function") inspect = util.inspect;
          } catch (_) { inspect = null; }
          const projection = { name: this._name, active: !this._closed };
          return "BroadcastChannel " + (inspect ? inspect(projection, opts) : "{ name: '" + this._name + "', active: " + !this._closed + " }");
        }
        postMessage(msg) {
          if (this._closed) throw new (G.DOMException || Error)("BroadcastChannel is closed", "InvalidStateError");
          if (arguments.length === 0) throw new TypeError("postMessage requires a message argument");
          const s = reg.get(this._name); if (!s) return;
          let data = msg; if (typeof G.structuredClone === "function") data = G.structuredClone(msg);
          for (const ch of s) { if (ch === this || ch._closed) continue;
            G.queueMicrotask(() => { if (!ch._closed) ch.dispatchEvent(new G.MessageEvent("message", { data })); });
          }
        }
        close() { if (this._closed) return; this._closed = true;
          const s = reg.get(this._name); if (s) { s.delete(this); if (s.size === 0) reg.delete(this._name); } }
        ref() { return this; } unref() { return this; }
      }
      Object.defineProperty(G, "BroadcastChannel", { value: BroadcastChannel, writable: true, enumerable: false, configurable: true });
    }

    // -------------------------------------------------------------- ErrorEvent
    defineIface(
      "ErrorEvent", ["message", "filename", "lineno", "colno", "error"],
      function (o) {
        return {
          message: String(orDefault(o.message, "")),
          filename: String(orDefault(o.filename, "")),
          lineno: toUint32(orDefault(o.lineno, 0)),
          colno: toUint32(orDefault(o.colno, 0)),
          error: orDefault(o.error, null),
        };
      });
  } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
