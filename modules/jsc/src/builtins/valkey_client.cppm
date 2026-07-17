// Bun.RedisClient / Bun.redis JS API shape + connection-string parsing. Kept
// as a dedicated payload partition so the client machinery does not grow
// bootstrap. The pure-logic RESP protocol codec is bridged natively via
// __mbunValkeyNative (see runtime/valkey_client.inc). The live socket/TLS
// transport + event-loop pump is DEFERRED(S-net): command EXECUTION rejects,
// but connection-string parsing, command serialization, RESP reply decoding,
// and the API surface run offline.
//
// Blueprint: bun-ref src/runtime/valkey_jsc/js_valkey.rs (constructor + URL
// parsing), js_valkey_functions.rs (command methods), and
// packages/bun-types/redis.d.ts (public shape).
export module mbun.jsc.js_builtins:valkey_client;

import std;

export namespace mbun::jsc::builtins::detail {

// NOTE: appended AFTER the master builtins IIFE has already run, so this is
// wrapped in its own self-contained IIFE re-binding G = globalThis.
inline constexpr std::string_view kValkeyClientJS = R"JS(
(function () {
  const G = globalThis;
  if (G.Bun && typeof G.Bun.RedisClient === "undefined") {
    const VKN = G.__mbunValkeyNative;   // RESP wire codec bridge (offline)
    const env = (G.process && G.process.env) || {};

    // Protocol scheme → connection kind (port of valkey::Protocol::MAP).
    const PROTO = {
      "redis": "tcp", "valkey": "tcp",
      "rediss": "tls", "valkeys": "tls", "redis+tls": "tls", "valkey+tls": "tls",
      "redis+unix": "unix", "valkey+unix": "unix",
      "redis+tls+unix": "tls+unix", "valkey+tls+unix": "tls+unix",
    };
    const isTls = (k) => k === "tls" || k === "tls+unix";
    const isUnix = (k) => k === "unix" || k === "tls+unix";

    const decodeComp = (s) => { try { return decodeURIComponent(s); } catch { return s; } };

    // Parse a redis/valkey connection string. Mirrors js_valkey.rs: no "://"
    // → prepend "valkey://"; unknown scheme / empty url / bad port throw.
    function parseURL(raw) {
      let url = raw;
      if (!url) throw new Error("Invalid URL format");
      if (url.indexOf("://") === -1) url = "valkey://" + url;
      try { new URL(url); } catch { throw new Error("Invalid URL format"); }
      const schemeEnd = url.indexOf("://");
      const scheme = url.slice(0, schemeEnd).toLowerCase();
      const kind = PROTO[scheme];
      if (!kind) {
        throw new Error(
          "Expected url protocol to be one of redis, valkey, rediss, valkeys, redis+tls, redis+unix, redis+tls+unix");
      }
      let rest = url.slice(schemeEnd + 3);
      // Split authority from pathname at the first '/'.
      const slash = rest.indexOf("/");
      let authority = slash === -1 ? rest : rest.slice(0, slash);
      const pathname = slash === -1 ? "" : rest.slice(slash);

      // userinfo@host:port
      let username = "", password = "", hostport = authority;
      const at = authority.lastIndexOf("@");
      if (at !== -1) {
        const userinfo = authority.slice(0, at);
        hostport = authority.slice(at + 1);
        const colon = userinfo.indexOf(":");
        if (colon === -1) { username = decodeComp(userinfo); }
        else { username = decodeComp(userinfo.slice(0, colon)); password = decodeComp(userinfo.slice(colon + 1)); }
      }

      let host = "", port = 6379, unixPath = "";
      if (isUnix(kind)) {
        // For unix sockets the pathname holds the socket path.
        unixPath = pathname;
        if (!unixPath) throw new Error("Expected unix socket path after valkey+unix:// or valkey+tls+unix://");
        port = 0;
      } else {
        // Handle IPv6 [::1]:port and host:port.
        if (hostport.startsWith("[")) {
          const end = hostport.indexOf("]");
          host = hostport.slice(1, end);
          const after = hostport.slice(end + 1);
          if (after.startsWith(":")) port = parsePort(after.slice(1));
        } else {
          const colon = hostport.lastIndexOf(":");
          if (colon === -1) { host = hostport; }
          else { host = hostport.slice(0, colon); port = parsePort(hostport.slice(colon + 1)); }
        }
      }

      // database from pathname (e.g. "/1" -> 1); ignored for unix.
      let database = 0;
      if (!isUnix(kind) && pathname.length > 1) {
        const n = parseInt(pathname.slice(1), 10);
        if (Number.isFinite(n)) database = n;
      }
      return { kind, scheme, host, port, unixPath, username, password, database, tls: isTls(kind) };
    }

    function parsePort(s) {
      if (s === "") return 6379;
      const n = Number(s);
      if (!Number.isInteger(n)) throw new Error("Invalid port number in URL. Port must be a number between 0 and 65535");
      if (n === 0) throw new Error("Port 0 is not valid for TCP connections");
      if (n < 0 || n > 65535) throw new Error("Invalid port number in URL. Port must be a number between 0 and 65535");
      return n;
    }

    const DEFERRED = "Redis commands over a live socket are not yet implemented in mbun (DEFERRED: needs the event loop)";

    // KeyLike (string | ArrayBufferView | Blob) → string for the wire.
    const toArg = (v) => {
      if (typeof v === "string") return v;
      if (v == null) return String(v);
      if (ArrayBuffer.isView(v)) {
        const u8 = v instanceof Uint8Array ? v : new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
        let s = ""; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return s;
      }
      return String(v);
    };

    class RedisClientImpl {
      constructor(url, options) {
        const urlStr = (url != null) ? String(url)
          : (env.REDIS_URL || env.VALKEY_URL || "valkey://localhost:6379");
        const parsed = parseURL(urlStr);
        const opts = options && typeof options === "object" ? options : {};
        this.#parsed = parsed;
        this.#options = {
          connectionTimeout: opts.connectionTimeout ?? 10000,
          idleTimeout: opts.idleTimeout ?? 0,
          autoReconnect: opts.autoReconnect ?? true,
          maxRetries: opts.maxRetries ?? 10,
          enableOfflineQueue: opts.enableOfflineQueue ?? true,
          enableAutoPipelining: opts.enableAutoPipelining ?? true,
          tls: opts.tls ?? parsed.tls,
        };
        this.onconnect = null;
        this.onclose = null;
      }
      #parsed;
      #options;

      get connected() { return false; }         // DEFERRED: no live socket
      get bufferedAmount() { return 0; }
      get options() { return this.#options; }

      connect() {
        return Promise.reject(new Error(
          "Redis connect over a live socket is not yet implemented in mbun (DEFERRED: needs the event loop)"));
      }
      close() {}
      [Symbol.dispose]() { this.close(); }

      // send(command, args) — validate shape, serialize offline, reject (no socket).
      send(command, args) {
        if (typeof command !== "string") return Promise.reject(new Error("Expected command to be a string"));
        if (args != null && !Array.isArray(args)) return Promise.reject(new Error("Expected args to be an array"));
        return Promise.reject(new Error(DEFERRED));
      }
      #cmd(name, args) { return Promise.reject(new Error(DEFERRED)); }

      get(key) { return this.#cmd("GET", [toArg(key)]); }
      getBuffer(key) { return this.#cmd("GET", [toArg(key)]); }
      set(key, value, ...rest) { return this.#cmd("SET", [toArg(key), toArg(value), ...rest.map(toArg)]); }
      del(...keys) { return this.#cmd("DEL", keys.map(toArg)); }
      incr(key) { return this.#cmd("INCR", [toArg(key)]); }
      decr(key) { return this.#cmd("DECR", [toArg(key)]); }
      exists(key) { return this.#cmd("EXISTS", [toArg(key)]); }
      expire(key, seconds) { return this.#cmd("EXPIRE", [toArg(key), String(seconds)]); }
      ttl(key) { return this.#cmd("TTL", [toArg(key)]); }
      hset(key, ...rest) { return this.#cmd("HSET", [toArg(key), ...rest.map(toArg)]); }
      hget(key, field) { return this.#cmd("HGET", [toArg(key), toArg(field)]); }
      hmget(key, ...fields) { return this.#cmd("HMGET", [toArg(key), ...fields.map(toArg)]); }
      hgetall(key) { return this.#cmd("HGETALL", [toArg(key)]); }
      publish(channel, message) { return this.#cmd("PUBLISH", [toArg(channel), toArg(message)]); }
      subscribe(channel, listener) { return this.#cmd("SUBSCRIBE", [toArg(channel)]); }
      unsubscribe(channel) { return this.#cmd("UNSUBSCRIBE", channel != null ? [toArg(channel)] : []); }
      ping(message) { return this.#cmd("PING", message != null ? [toArg(message)] : []); }
    }

    // Bun's RedisClient is a native constructor: calling it without `new`
    // throws "RedisClient constructor cannot be invoked without 'new'"
    // (a class alone yields JSC's "Cannot call a class constructor ...").
    // Wrap the class so the exact message + new.target guard match bun.
    function RedisClient(url, options) {
      if (!new.target) throw new TypeError("RedisClient constructor cannot be invoked without 'new'");
      return Reflect.construct(RedisClientImpl, [url, options], new.target);
    }
    RedisClient.prototype = RedisClientImpl.prototype;
    Object.defineProperty(RedisClient.prototype, "constructor",
      { value: RedisClient, configurable: true, writable: true });

    G.Bun.RedisClient = RedisClient;

    // Bun.redis: a lazily-initialized default client.
    let defaultClient = null;
    Object.defineProperty(G.Bun, "redis", {
      configurable: true,
      get() { if (!defaultClient) defaultClient = new RedisClient(); return defaultClient; },
    });

    // Expose the offline RESP wire codec for protocol-layer tests (injected bytes).
    if (VKN) G.Bun.__valkeyWire = VKN;
  }
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
