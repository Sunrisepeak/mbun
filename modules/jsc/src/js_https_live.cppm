// modules/jsc/src/js_https_live.cppm — module mbun.jsc.js_https_live
//
// node:https, translated from node's lib/https.js.
//
// Before this partition there was no https layer at all: builtins/node_http.cppm
// built the https module object out of the SAME makeExports() as http, so
// `https.createServer()` returned http's shape-only EventEmitter (no listen(), no
// TLS) and `https.Server` was not a tls.Server. 21 corpus files died on
// `server.listen is not a function` alone; test-https-* stood at 4/63.
//
// node's lib/https.js is 677 lines and almost all of it is one idea: an
// https.Server IS a tls.Server carrying _http_server.js's _connectionListener,
// and an https.Agent IS an _http_agent.js Agent whose createConnection is
// tls.connect and whose pool key includes every TLS option. Both halves already
// exist in this runtime, so this partition wires them rather than reimplementing
// either: `js_net.cppm`'s http server factory is invoked with the tls.Server as
// its base (it then reads 'secureConnection' instead of 'connection'), and
// node_http.cppm's Agent/ClientRequest are subclassed.
//
// DEFERRED (node has it, mbun has no substrate for it): proxy tunnelling
// (kProxyConfig / CONNECT / --use-env-proxy) — internal/http's ProxyConfig does
// not exist here, so getTunnelConfigForProxiedHttps would always return null;
// and TLS session resumption's wire half (tls.connect ignores `session`, and the
// reactor never emits 'session'), so the Agent's session cache is maintained and
// observable but never repopulated from a real handshake.
//
// Evaluated after kNetJS (the http server factory + reactor) and after
// kTlsLiveJS (tls.Server / tls.connect must be the live ones, not the offline
// stubs), and before kNodeLegacyCtorsJS (which makes https.Server / https.Agent
// callable without `new`, as node's function-style constructors are).
export module mbun.jsc.js_https_live;

import std;

namespace mbun::jsc::js_https_live {

export constexpr std::string_view kHttpsLiveJS = R"JS(
(function () {
  "use strict";
  const G = globalThis;
  const M = G.__mbunNativeModules || {};
  const http = M["http"] || M["node:http"];
  const tls = M["tls"] || M["node:tls"];
  const url = M["url"] || M["node:url"] || {};
  const HI = G.__mbunHttpInternals || {};
  const makeHttpServer = G.__mbunHttpServerFactory;
  if (!http || !tls || typeof tls.Server !== "function" || typeof makeHttpServer !== "function") return;

  const ClientRequest = HI.ClientRequest || http.ClientRequest;
  const HttpAgent = HI.Agent || http.Agent;
  const ERR_INVALID_ARG_TYPE = HI.ERR_INVALID_ARG_TYPE ||
    ((name, exp, actual) => { const e = new TypeError('The "' + name + '" argument must be of type ' + exp); e.code = "ERR_INVALID_ARG_TYPE"; return e; });
  const validateObject = HI.validateObject || ((v, n) => {
    if (v === null || typeof v !== "object" || Array.isArray(v)) throw ERR_INVALID_ARG_TYPE(n, "Object", v);
  });
  const urlToHttpOptions = typeof url.urlToHttpOptions === "function" ? url.urlToHttpOptions : (u) => {
    const hostname = typeof u.hostname === "string" && u.hostname.startsWith("[") ? u.hostname.slice(1, -1) : u.hostname;
    const o = { protocol: u.protocol, hostname, hash: u.hash, search: u.search, pathname: u.pathname,
      path: (u.pathname || "") + (u.search || ""), href: u.href };
    if (u.port !== "") o.port = Number(u.port);
    if (u.username || u.password) o.auth = decodeURIComponent(u.username) + ":" + decodeURIComponent(u.password);
    return o;
  };
  const isURL = (v) => v != null && typeof v === "object" && typeof v.href === "string" &&
    typeof v.protocol === "string" && typeof v.searchParams === "object";

  // ---------------------------------------------------------------- Server ---
  // lib/https.js Server: storeHTTPOptions + tls.Server(_connectionListener), then
  // tlsClientError → clientError routing. `makeHttpServer(opts, listener, this)`
  // is js_net.cppm's createHttpServer with an existing base server: it installs
  // storeHTTPOptions, the headersTimeout/requestTimeout sweeper, close/listen
  // wrappers and the parser loop — on 'secureConnection', because that is where a
  // tls.Server hands over a ready plaintext stream.
  class Server extends tls.Server {
    constructor(opts, requestListener) {
      let ALPNProtocols = ["http/1.1"];
      if (typeof opts === "function") { requestListener = opts; opts = {}; }
      else if (opts == null) opts = {};
      else {
        validateObject(opts, "options");
        // Only one of ALPNProtocols and ALPNCallback may be set, so only default
        // ALPNProtocols when the caller set neither.
        if (opts.ALPNProtocols || opts.ALPNCallback) ALPNProtocols = undefined;
      }
      super(Object.assign({ noDelay: true, ALPNProtocols }, opts));
      makeHttpServer(opts, requestListener, this);
      this.httpAllowHalfOpen = false;
      // A pre-handshake failure is the server's business, not an unhandled error
      // on a half-built socket (lib/https.js Server).
      this.addListener("tlsClientError", function (err, conn) {
        if (!this.emit("clientError", err, conn)) { try { conn.destroy(err); } catch (e) {} }
      });
      this.timeout = 0;
      this.maxHeadersCount = null;
    }
  }

  // lib/https.js Server.prototype[SymbolAsyncDispose]: `await using server = ...`
  // closes it and waits for 'close'.
  if (typeof Symbol.asyncDispose === "symbol") {
    Server.prototype[Symbol.asyncDispose] = async function () {
      await new Promise((resolve, reject) => {
        this.close((err) => (err ? reject(err) : resolve()));
      });
    };
  }

  function createServer(opts, requestListener) { return new Server(opts, requestListener); }

  // ----------------------------------------------------------------- Agent ---
  // lib/https.js createConnection. The proxy-tunnel branch is DEFERRED (see the
  // module header); everything else — the odd (port, host, options) signature and
  // the per-agentKey session cache — is node's.
  function createConnection(...args) {
    let options, cb;
    if (args[0] !== null && typeof args[0] === "object") options = args[0];
    else if (args[1] !== null && typeof args[1] === "object") options = { ...args[1] };
    else if (args[2] === null || typeof args[2] !== "object") options = {};
    else options = { ...args[2] };
    if (typeof args[0] === "number") options.port = args[0];
    if (typeof args[1] === "string") options.host = args[1];
    if (typeof args[args.length - 1] === "function") cb = args[args.length - 1];

    if (options._agentKey) {
      const session = this._getSession(options._agentKey);
      if (session) options = { session, ...options };
    }

    const socket = tls.connect(options);

    if (options._agentKey) {
      // Cache a new session for reuse, and evict it if the connection failed.
      socket.on("session", (session) => { this._cacheSession(options._agentKey, session); });
      socket.once("close", (err) => { if (err) this._evictSession(options._agentKey); });
    }

    return socket;
  }

  class Agent extends HttpAgent {
    constructor(options) {
      options = Object.assign({ __proto__: null }, options);
      if (options.defaultPort === undefined) options.defaultPort = 443;
      if (options.protocol === undefined) options.protocol = "https:";
      super(options);
      this.maxCachedSessions = this.options.maxCachedSessions;
      if (this.maxCachedSessions === undefined) this.maxCachedSessions = 100;
      this._sessionCache = { map: { __proto__: null }, list: [] };
    }
  }
  Agent.prototype.createConnection = createConnection;

  // lib/https.js Agent.prototype.getName: the pool key carries every option that
  // changes what the TLS handshake produces, so two requests that differ only in
  // `ca` / `rejectUnauthorized` / `servername` can never share a socket.
  Agent.prototype.getName = function getName(options) {
    options = options || {};
    let name = HttpAgent.prototype.getName.call(this, options);
    name += ":";
    if (options.ca) name += options.ca;
    name += ":";
    if (options.cert) name += options.cert;
    name += ":";
    if (options.clientCertEngine) name += options.clientCertEngine;
    name += ":";
    if (options.ciphers) name += options.ciphers;
    name += ":";
    if (options.key) name += options.key;
    name += ":";
    if (options.pfx) name += options.pfx;
    name += ":";
    if (options.rejectUnauthorized !== undefined) name += options.rejectUnauthorized;
    name += ":";
    if (options.servername && options.servername !== options.host) name += options.servername;
    name += ":";
    if (options.minVersion) name += options.minVersion;
    name += ":";
    if (options.maxVersion) name += options.maxVersion;
    name += ":";
    if (options.secureProtocol) name += options.secureProtocol;
    name += ":";
    if (options.crl) name += options.crl;
    name += ":";
    if (options.honorCipherOrder !== undefined) name += options.honorCipherOrder;
    name += ":";
    if (options.ecdhCurve) name += options.ecdhCurve;
    name += ":";
    if (options.dhparam) name += options.dhparam;
    name += ":";
    if (options.secureOptions !== undefined) name += options.secureOptions;
    name += ":";
    if (options.sessionIdContext) name += options.sessionIdContext;
    name += ":";
    if (options.sigalgs) name += JSON.stringify(options.sigalgs);
    name += ":";
    if (options.privateKeyIdentifier) name += options.privateKeyIdentifier;
    name += ":";
    if (options.privateKeyEngine) name += options.privateKeyEngine;
    return name;
  };

  Agent.prototype._getSession = function _getSession(key) { return this._sessionCache.map[key]; };

  Agent.prototype._cacheSession = function _cacheSession(key, session) {
    if (this.maxCachedSessions === 0) return;          // cache disabled
    if (this._sessionCache.map[key]) { this._sessionCache.map[key] = session; return; }
    if (this._sessionCache.list.length >= this.maxCachedSessions) {
      const oldKey = this._sessionCache.list.shift();
      delete this._sessionCache.map[oldKey];
    }
    this._sessionCache.list.push(key);
    this._sessionCache.map[key] = session;
  };

  Agent.prototype._evictSession = function _evictSession(key) {
    const index = this._sessionCache.list.indexOf(key);
    if (index === -1) return;
    this._sessionCache.list.splice(index, 1);
    delete this._sessionCache.map[key];
  };

  const globalAgent = new Agent({ keepAlive: true, scheduling: "lifo", timeout: 5000 });

  // --------------------------------------------------------------- request ---
  // lib/https.js request/get. `_defaultAgent` is read off the LIVE exports
  // object, so replacing https.globalAgent redirects subsequent requests
  // (test-https-client-override-global-agent).
  function request(...args) {
    let options = {};
    if (typeof args[0] === "string") {
      const urlStr = args.shift();
      options = urlToHttpOptions(new URL(urlStr));
    } else if (isURL(args[0])) {
      options = urlToHttpOptions(args.shift());
    }
    if (args[0] && typeof args[0] !== "function") {
      Object.assign(options, args.shift());
    }
    options._defaultAgent = exp.globalAgent;
    args.unshift(options);
    return Reflect.construct(ClientRequest, args);
  }

  function get(input, options, cb) {
    const req = request(input, options, cb);
    req.end();
    return req;
  }

  // ---- install over the http-mirror https module object ---------------------
  // Copy DESCRIPTORS: the mirror carries `maxHeaderSize` as a live accessor over
  // the process-wide limit, which Object.assign would freeze into a data property.
  const exp = Object.defineProperties({}, Object.getOwnPropertyDescriptors(M["https"] || {}));
  Object.assign(exp, { Agent, globalAgent, Server, createServer, get, request });
  M["https"] = M["node:https"] = exp;
})();
)JS";

}  // namespace mbun::jsc::js_https_live
