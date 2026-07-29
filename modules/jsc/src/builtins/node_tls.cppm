// node:tls JS layer partition. Patches the already-installed node:tls module
// object (globalThis.__mbunNativeModules["tls"], registered as a shape stub in
// bootstrap.cppm) with the real, offline-testable surface:
//   createSecureContext / SecureContext (full Node option validation),
//   checkServerIdentity (+ RFC 6125 wildcard/altname matching),
//   getCiphers (native OpenSSL default cipher list via __mbunNodeTlsNative),
//   rootCertificates (frozen, immutable — DEFERRED full Mozilla NSS bundle),
//   DEFAULT_ECDH_CURVE / DEFAULT_MIN_VERSION / DEFAULT_MAX_VERSION accessors,
//   convertALPNProtocols / convertProtocols (ALPN wire encoding),
//   CLIENT_RENEG_LIMIT / CLIENT_RENEG_WINDOW, parseCertString,
//   TLSSocket / Server class shapes.
// Certificate field parsing reuses the OpenSSL X509 bridge already installed by
// runtime/crypto_asym.inc (globalThis.__mbunCryptoAsymNative.x509parse).
//
// DEFERRED (honest throw): tls.connect / new tls.Server(...).listen and the live
// TLSSocket handshake — those need the socket event loop + real SSL_CTX handle,
// which land with the S-net socket/listener work (modules/tls TlsChannel).
//
// NOTE: appended AFTER the master builtins IIFE (opened in bootstrap, closed by
// image_closure) has already run, so it is a self-contained IIFE that re-binds
// G = globalThis and patches the registered module object in place.
//
// Blueprint: bun-ref src/js/node/tls.ts + src/js/internal/tls.ts; node
// lib/tls.js + lib/internal/tls/secure-context.js.
export module mbun.jsc.js_builtins:node_tls;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kNodeTlsJS = R"JS(
(function () {
  const G = globalThis;
  const M = G.__mbunNativeModules;
  if (!M) return;
  const T = M["tls"] || M["node:tls"];
  if (!T) return;
  const net = M["net"] || M["node:net"];
  const asym = G.__mbunCryptoAsymNative;
  const tlsNative = G.__mbunNodeTlsNative;
  const Buffer = G.Buffer;

  // ---- node error factories (message shapes match lib/internal/errors.js) ----
  const kTypes = new Set([
    "string", "function", "number", "object", "Function", "Object",
    "boolean", "bigint", "symbol",
  ]);
  // ERR_INVALID_ARG_VALUE / ERR_OUT_OF_RANGE report `inspect(value)`.
  const inspect = (v) => {
    if (v === undefined) return "undefined";
    if (v === null) return "null";
    if (typeof v === "string") return "'" + v + "'";
    if (typeof v === "bigint") return String(v) + "n";
    if (typeof v === "function") return "[Function]";
    if (typeof v === "object") {
      try { return JSON.stringify(v); } catch (e) { return "[Object]"; }
    }
    return String(v);
  };
  // ERR_INVALID_ARG_TYPE reports determineSpecificType(value), which is NOT the
  // same as inspect: a primitive is rendered as "type <typeof> (<value>)" and an
  // object as "an instance of <ctor>". Using inspect here produced
  // "Received 1" where node says "Received type number (1)" — invisible while
  // assert.throws ignored its expectation, and wrong the moment it does not.
  // ref lib/internal/errors.js determineSpecificType.
  const specificType = (v) => {
    if (v === null) return "null";
    if (v === undefined) return "undefined";
    const t = typeof v;
    if (t === "bigint") return "type bigint (" + String(v) + "n)";
    if (t === "number") {
      if (v === 0) return 1 / v === -Infinity ? "type number (-0)" : "type number (0)";
      if (v !== v) return "type number (NaN)";
      return "type number (" + String(v) + ")";
    }
    if (t === "boolean") return v ? "type boolean (true)" : "type boolean (false)";
    if (t === "symbol") return "type symbol (" + String(v) + ")";
    if (t === "function") return "function " + v.name;
    if (t === "object") {
      if (v.constructor && "name" in v.constructor) return "an instance of " + v.constructor.name;
      return "[Object: null prototype] {}";
    }
    if (t === "string") {
      let s = v;
      if (s.length > 28) s = s.slice(0, 25) + "...";
      return s.indexOf("'") === -1 ? "type string ('" + s + "')" : "type string (" + JSON.stringify(s) + ")";
    }
    return "type " + t + " (" + String(v) + ")";
  };
  const joinTypes = (arr, kw) => {
    const len = arr.length;
    if (len > 2) return kw + " " + arr.slice(0, len - 1).join(", ") + ", or " + arr[len - 1];
    if (len === 2) return kw + " " + arr[0] + " or " + arr[1];
    return (kw === "of type" ? "of type " : "one of ") + arr[0];
  };
  function ERR_INVALID_ARG_TYPE(name, expected, actual) {
    // Prefer the shared node-exact factory (bootstrap __mbunNodeErrors); the
    // local fallback below joins the class list without node's Oxford comma.
    const NE = G.__mbunNodeErrors;
    if (NE) return NE.ERR_INVALID_ARG_TYPE(name, expected, actual);
    if (!Array.isArray(expected)) expected = [expected];
    const determiner = String(name).includes(".") ? "property" : "argument";
    let msg = 'The "' + name + '" ' + determiner + " must be ";
    const types = [], instances = [], other = [];
    for (const e of expected) {
      if (kTypes.has(e)) types.push(String(e).toLowerCase());
      else if (/^[A-Z]/.test(e)) instances.push(e);
      else other.push(e);
    }
    const parts = [];
    if (types.length) parts.push(joinTypes(types, "of type"));
    if (instances.length) parts.push("an instance of " + instances.join(" or "));
    if (other.length) parts.push(joinTypes(other, "one of"));
    msg += parts.join(" or ") + ". Received " + specificType(actual);
    const err = new TypeError(msg);
    err.code = "ERR_INVALID_ARG_TYPE";
    return err;
  }
  function ERR_INVALID_ARG_VALUE(name, value, reason) {
    reason = reason || "is invalid";
    const determiner = String(name).includes(".") ? "property" : "argument";
    const err = new TypeError("The " + determiner + " '" + name + "' " + reason + ". Received " + inspect(value));
    err.code = "ERR_INVALID_ARG_VALUE";
    return err;
  }
  // node's ERR_OUT_OF_RANGE reads 'The value of "x" is out of range.', NOT
  // 'The "x" argument is out of range.' — delegate to the shared factory
  // (bootstrap __mbunNodeErrors) so this copy cannot drift again.
  function ERR_OUT_OF_RANGE(name, range, value) {
    const NE = G.__mbunNodeErrors;
    if (NE) return NE.ERR_OUT_OF_RANGE(name, range, value);
    const err = new RangeError('The value of "' + name + '" is out of range. It must be ' + range + ". Received " + inspect(value));
    err.code = "ERR_OUT_OF_RANGE";
    return err;
  }
  function ERR_TLS_INVALID_PROTOCOL_VERSION(version, name) {
    const err = new TypeError(version + " is not a valid " + name + " TLS protocol version");
    err.code = "ERR_TLS_INVALID_PROTOCOL_VERSION";
    return err;
  }
  // node lib/internal/errors.js: 'TLS protocol version %j conflicts with
  // secureProtocol %j' (%j is JSON, hence the quotes).
  function ERR_TLS_PROTOCOL_VERSION_CONFLICT(version, secureProtocol) {
    const err = new TypeError("TLS protocol version " + JSON.stringify(version) +
      " conflicts with secureProtocol " + JSON.stringify(secureProtocol));
    err.code = "ERR_TLS_PROTOCOL_VERSION_CONFLICT";
    return err;
  }
  function ERR_TLS_INVALID_PROTOCOL_METHOD(message) {
    const err = new TypeError(message);
    err.code = "ERR_TLS_INVALID_PROTOCOL_METHOD";
    return err;
  }
  function ERR_CRYPTO_CUSTOM_ENGINE_NOT_SUPPORTED(message) {
    const err = new Error(message);
    err.code = "ERR_CRYPTO_CUSTOM_ENGINE_NOT_SUPPORTED";
    return err;
  }
  function ERR_CRYPTO_UNSUPPORTED_OPERATION(message) {
    const err = new Error(message);
    err.code = "ERR_CRYPTO_UNSUPPORTED_OPERATION";
    return err;
  }
  function ERR_TLS_CERT_ALTNAME_INVALID(reason, host, cert) {
    const err = new Error("Hostname/IP does not match certificate's altnames: " + reason);
    err.code = "ERR_TLS_CERT_ALTNAME_INVALID";
    err.reason = reason;
    err.host = host;
    err.cert = cert;
    return err;
  }

  // ---- validators (subset of lib/internal/validators.js) ----
  const validateString = (v, name) => { if (typeof v !== "string") throw ERR_INVALID_ARG_TYPE(name, "string", v); };
  const validateBuffer = (v, name) => { if (!ArrayBuffer.isView(v)) throw ERR_INVALID_ARG_TYPE(name, ["Buffer", "TypedArray", "DataView"], v); };
  const validateFunction = (v, name) => { if (typeof v !== "function") throw ERR_INVALID_ARG_TYPE(name, "Function", v); };

  // ---- internal/tls validateKeyOrCertOption / configSecureContext ----
  // node validates every ca/cert/key value with validateKeyOrCertOption, which
  // accepts ONLY a string or an ArrayBufferView. The `{ pem, passphrase }` form
  // is not a value type: configSecureContext unwraps it (`val?.pem !== undefined
  // ? val.pem : val`) for the ELEMENTS OF A KEY ARRAY only, and validates the
  // unwrapped pem. So `key: [{ pem }]` is legal, while a bare `key: { pem }` —
  // and any `{ pem }` under `cert`/`ca` — is ERR_INVALID_ARG_TYPE.
  // Accepting the bare object made tls.createServer({ key: { pem } }) succeed
  // where node throws (test-tls-options-boolean-check /
  // test-https-options-boolean-check assert both key and cert).
  const isValidTLSItem = (o) =>
    typeof o === "string" || ArrayBuffer.isView(o) || o instanceof ArrayBuffer;
  // node's key-array element rule, returning the value the error must name.
  const unwrapKeyPem = (x) => (x != null && typeof x === "object" && x.pem !== undefined ? x.pem : x);
  const isValidTLSArray = (o, unwrapPem) => {
    if (isValidTLSItem(o)) return true;
    if (Array.isArray(o)) return o.every((x) => isValidTLSItem(unwrapPem ? unwrapKeyPem(x) : x));
    return false;
  };
  // node lib/internal/tls/secure-context.js validateKeyOrCertOption passes this
  // exact list to ERR_INVALID_ARG_TYPE. mbun additionally *accepts* a BunFile,
  // but the rejection message must be node's (no bun corpus test pins the
  // "or BunFile" wording — it was invented here).
  const VALID_TLS_ERROR_MESSAGE_TYPES = ["string", "Buffer", "TypedArray", "DataView"];
  const findInvalidTLSItem = (o, unwrapPem) => {
    if (Array.isArray(o)) {
      for (const item of o) {
        const v = unwrapPem ? unwrapKeyPem(item) : item;
        if (!isValidTLSItem(v)) return v;
      }
    }
    return o;
  };
  // `unwrapPem` is set only for options.key, matching where node unwraps.
  const throwOnInvalidTLSArray = (name, value, unwrapPem) => {
    if (!isValidTLSArray(value, unwrapPem))
      throw ERR_INVALID_ARG_TYPE(name, VALID_TLS_ERROR_MESSAGE_TYPES, findInvalidTLSItem(value, unwrapPem));
  };

  // ---- DEFAULT_CIPHERS (node src/node_constants.h DEFAULT_CIPHER_LIST_CORE) ---
  // The exact list node compiles in and exposes as both tls.DEFAULT_CIPHERS and
  // crypto.constants.defaultCoreCipherList. It is strictly a *restriction* of
  // OpenSSL's own default: the trailing !aNULL/!eNULL/!EXPORT/!DES/!RC4/!MD5/
  // !PSK/!SRP/!CAMELLIA exclusions remove unauthenticated, unencrypted, export-
  // grade and legacy suites. Copied verbatim so tests comparing the two agree.
  const DEFAULT_CORE_CIPHERS =
    "TLS_AES_256_GCM_SHA384:" +
    "TLS_CHACHA20_POLY1305_SHA256:" +
    "TLS_AES_128_GCM_SHA256:" +
    "ECDHE-RSA-AES128-GCM-SHA256:" +
    "ECDHE-ECDSA-AES128-GCM-SHA256:" +
    "ECDHE-RSA-AES256-GCM-SHA384:" +
    "ECDHE-ECDSA-AES256-GCM-SHA384:" +
    "DHE-RSA-AES128-GCM-SHA256:" +
    "ECDHE-RSA-AES128-SHA256:" +
    "DHE-RSA-AES128-SHA256:" +
    "ECDHE-RSA-AES256-SHA384:" +
    "DHE-RSA-AES256-SHA384:" +
    "ECDHE-RSA-AES256-SHA256:" +
    "DHE-RSA-AES256-SHA256:" +
    "HIGH:" +
    "!aNULL:" +
    "!eNULL:" +
    "!EXPORT:" +
    "!DES:" +
    "!RC4:" +
    "!MD5:" +
    "!PSK:" +
    "!SRP:" +
    "!CAMELLIA";

  // node src/node_options.cc --tls-cipher-list=<list>: the operator replaces the
  // compiled-in default outright. node then reports the REPLACEMENT as both
  // tls.DEFAULT_CIPHERS and crypto.constants.defaultCipherList, while
  // crypto.constants.defaultCoreCipherList keeps naming the compiled-in one, so
  // a program can still tell what it was overridden from
  // (test-tls-cipher-list asserts exactly that pair). Only ever applied on the
  // operator's explicit instruction; with no flag the core list stands.
  let DEFAULT_CIPHERS = DEFAULT_CORE_CIPHERS;
  {
    const argv = (G.process && G.process.execArgv) || [];
    for (const a of argv) {
      if (typeof a === "string" && a.startsWith("--tls-cipher-list=")) {
        DEFAULT_CIPHERS = a.slice("--tls-cipher-list=".length);
      }
    }
  }

  // ---- processCiphers (lib/internal/tls/secure-context.js) -------------------
  // OpenSSL keeps the TLS 1.3 suites in a separate slot from the <=TLS 1.2
  // cipher list, reached through SSL_CTX_set_ciphersuites rather than
  // SSL_CTX_set_cipher_list. node splits the caller's `ciphers` string on ':'
  // and routes each entry by its TLS_ prefix. An empty entry is dropped; if
  // BOTH halves end up empty the option is ERR_INVALID_ARG_VALUE, because a
  // handshake with no suites at all is impossible.
  const processCiphers = (ciphers) => {
    const parts = String(ciphers == null || ciphers === "" ? DEFAULT_CIPHERS : ciphers).split(":");
    const isSuite = (c) => c.startsWith("TLS_") || c.startsWith("!TLS_");
    return {
      cipherList: parts.filter((c) => c.length !== 0 && !isSuite(c)).join(":"),
      cipherSuites: parts.filter((c) => c.length !== 0 && isSuite(c)).join(":"),
    };
  };

  // ---- version defaults & valid set ----
  const VALID_TLS_VERSIONS = new Set(["TLSv1", "TLSv1.1", "TLSv1.2", "TLSv1.3"]);
  let DEFAULT_MIN_VERSION = "TLSv1.2";
  let DEFAULT_MAX_VERSION = "TLSv1.3";
  const DEFAULT_ECDH_CURVE = "auto";

  // node lib/tls.js: --tls-min-v1.{0,1,2,3} / --tls-max-v1.{2,3} move the default
  // protocol window. The resolution is NOT last-flag-wins — it is a fixed
  // if/else-if chain in a fixed order, so the WIDEST flag present wins no matter
  // where it appears on the command line: min checks v1.0 first, then v1.1, v1.2,
  // v1.3; max checks v1.3 first, then v1.2. test-tls-cli-min-version-1.0 passes
  // `--tls-min-v1.0 --tls-min-v1.1` and expects TLSv1, which last-flag-wins got
  // backwards. These only ever move the default window on the operator's explicit
  // instruction; nothing here changes it when no flag is given.
  {
    const argv = (G.process && G.process.execArgv) || [];
    const has = (flag) => argv.some((a) => a === flag);
    if (has("--tls-min-v1.0")) DEFAULT_MIN_VERSION = "TLSv1";
    else if (has("--tls-min-v1.1")) DEFAULT_MIN_VERSION = "TLSv1.1";
    else if (has("--tls-min-v1.2")) DEFAULT_MIN_VERSION = "TLSv1.2";
    else if (has("--tls-min-v1.3")) DEFAULT_MIN_VERSION = "TLSv1.3";
    if (has("--tls-max-v1.3")) DEFAULT_MAX_VERSION = "TLSv1.3";
    else if (has("--tls-max-v1.2")) DEFAULT_MAX_VERSION = "TLSv1.2";
  }

  // ---- secureProtocol validation (lib/internal/tls/secure-context.js) ----
  const SECURE_PROTOCOL_METHODS = new Set([
    "TLS_method", "TLS_client_method", "TLS_server_method",
    "SSLv23_method", "SSLv23_client_method", "SSLv23_server_method",
    "TLSv1_method", "TLSv1_client_method", "TLSv1_server_method",
    "TLSv1_1_method", "TLSv1_1_client_method", "TLSv1_1_server_method",
    "TLSv1_2_method", "TLSv1_2_client_method", "TLSv1_2_server_method",
  ]);
  function validateSecureProtocol(secureProtocol) {
    if (secureProtocol === undefined || secureProtocol === null) return;
    validateString(secureProtocol, "options.secureProtocol");
    if (secureProtocol.startsWith("SSLv2_")) throw ERR_TLS_INVALID_PROTOCOL_METHOD("SSLv2 methods disabled");
    if (secureProtocol.startsWith("SSLv3_")) throw ERR_TLS_INVALID_PROTOCOL_METHOD("SSLv3 methods disabled");
    if (!SECURE_PROTOCOL_METHODS.has(secureProtocol)) throw ERR_TLS_INVALID_PROTOCOL_METHOD("Unknown method: " + secureProtocol);
  }

  function validateSecureContextOptions(options) {
    const {
      ciphers, passphrase, ecdhCurve, minVersion, maxVersion, sessionTimeout,
      ticketKeys, clientCertEngine, dhparam, secureProtocol,
    } = options;
    // node internal/tls/common.js SecureContext runs BEFORE configSecureContext,
    // and inside it the order is: secureProtocol/minVersion+maxVersion conflict,
    // then toV() version validity, then context.init() which is where an unknown
    // method name becomes ERR_TLS_INVALID_PROTOCOL_METHOD. Checking the method
    // name first reported the wrong error for `{ maxVersion, secureProtocol }`
    // (test-tls-min-max-version expects the CONFLICT).
    if (secureProtocol) {
      if (minVersion != null) throw ERR_TLS_PROTOCOL_VERSION_CONFLICT(minVersion, secureProtocol);
      if (maxVersion != null) throw ERR_TLS_PROTOCOL_VERSION_CONFLICT(maxVersion, secureProtocol);
    }
    if (minVersion != null && !VALID_TLS_VERSIONS.has(minVersion)) throw ERR_TLS_INVALID_PROTOCOL_VERSION(String(minVersion), "minimum");
    if (maxVersion != null && !VALID_TLS_VERSIONS.has(maxVersion)) throw ERR_TLS_INVALID_PROTOCOL_VERSION(String(maxVersion), "maximum");
    validateSecureProtocol(secureProtocol);
    if (ciphers !== undefined && ciphers !== null) {
      validateString(ciphers, "options.ciphers");
      // node lib/internal/tls/secure-context.js processCiphers: the TLS 1.3
      // suites live behind a DIFFERENT OpenSSL setter, so the list is split on
      // ':' and the TLS_-prefixed entries go to setCipherSuites while the rest go
      // to setCiphers. Handing the whole string to SSL_CTX_set_cipher_list, as
      // mbun did, made every TLS 1.3 suite name a "No cipher match" error.
      const split = processCiphers(ciphers);
      if (split.cipherList === "" && split.cipherSuites === "") {
        const e = new TypeError("The argument 'options.ciphers' is invalid. Received " + JSON.stringify(ciphers));
        e.code = "ERR_INVALID_ARG_VALUE";
        throw e;
      }
      // node SecureContext::SetCiphers: a list OpenSSL cannot match to any
      // suite is rejected at context-creation time with the OpenSSL error
      // shape (code/library/reason), not silently ignored.
      // node SetCiphers returns early on an empty list (it leaves the context's
      // TLS1.3 suites in place), so "" is legal and must not be rejected.
      const TN = globalThis.__mbunNodeTlsNative;
      const noMatch = () => {
        const e = new Error("No cipher match");
        e.code = "ERR_SSL_NO_CIPHER_MATCH";
        e.library = "SSL routines";
        e.reason = "no cipher match";
        return e;
      };
      if (split.cipherList !== "" && TN && typeof TN.checkCipherList === "function"
          && !TN.checkCipherList(split.cipherList)) throw noMatch();
      if (split.cipherSuites !== "" && TN && typeof TN.checkCipherSuites === "function"
          && !TN.checkCipherSuites(split.cipherSuites)) throw noMatch();
    }
    if (passphrase !== undefined && passphrase !== null) validateString(passphrase, "options.passphrase");
    if (ecdhCurve !== undefined && ecdhCurve !== null) validateString(ecdhCurve, "options.ecdhCurve");
    if (clientCertEngine !== undefined && clientCertEngine !== null) {
      if (typeof clientCertEngine !== "string")
        throw ERR_INVALID_ARG_TYPE("options.clientCertEngine", ["string", "null", "undefined"], clientCertEngine);
      throw ERR_CRYPTO_CUSTOM_ENGINE_NOT_SUPPORTED("Custom engines not supported by this OpenSSL");
    }
    // `dhparam: 'auto'` is node's SetDHParam(true) → SSL_CTX_set_dh_auto, which
    // the linked OpenSSL supports. Only a BoringSSL build rejects it
    // (test-tls-dhparam-auto-boringssl is gated on
    // process.features.openssl_is_boringssl, which is false here), and throwing
    // unconditionally made every DHE server unusable.
    if (dhparam === "auto" && G.process && G.process.features
        && G.process.features.openssl_is_boringssl)
      throw ERR_CRYPTO_UNSUPPORTED_OPERATION("Automatic DH parameter selection is not supported");
    if (ticketKeys !== undefined && ticketKeys !== null) {
      validateBuffer(ticketKeys, "options.ticketKeys");
      if (ticketKeys.byteLength !== 48) throw ERR_INVALID_ARG_VALUE("options.ticketKeys", ticketKeys.byteLength, "must be exactly 48 bytes");
    }
    if (sessionTimeout !== undefined && sessionTimeout !== null) {
      if (typeof sessionTimeout !== "number") throw ERR_INVALID_ARG_TYPE("options.sessionTimeout", "number", sessionTimeout);
      if (!Number.isInteger(sessionTimeout)) throw ERR_OUT_OF_RANGE("options.sessionTimeout", "an integer", sessionTimeout);
      if (sessionTimeout < 0 || sessionTimeout > 2147483647) throw ERR_OUT_OF_RANGE("options.sessionTimeout", ">= 0 && <= 2147483647", sessionTimeout);
    }
  }

  // ---- SecureContext ----
  // The native SSL_CTX handle is DEFERRED (needs the socket/handshake layer);
  // the JS context stores the validated + normalized options and, when a cert
  // PEM is supplied, validates it through the OpenSSL X509 bridge so a malformed
  // certificate is rejected here rather than silently at connect time.
  // The native context object's shape. `_external` is node's accessor for the
  // wrapped SSL_CTX pointer; node's accessor asserts on `this`, so reading it off
  // an object that merely INHERITS from a context is a TypeError while reading it
  // off the context itself is fine (test-tls-external-accessor).
  const kContextBrand = Symbol("kNativeSecureContext");
  const SecureContextHandle = {};
  Object.defineProperty(SecureContextHandle, "_external", {
    configurable: true,
    enumerable: false,
    get() {
      if (this === null || this === undefined || !Object.hasOwn(this, kContextBrand))
        throw new TypeError("Illegal invocation");
      return null;
    },
  });
  const pemText = (v) => {
    if (v == null) return "";
    if (Array.isArray(v)) return v.map(pemText).join("\n");
    if (typeof v === "string") return v;
    // `{ pem, passphrase }` with pem as a string OR a Buffer (node
    // configSecureContext accepts both); recursing handles the Buffer form,
    // which `typeof v.pem === "string"` skipped entirely.
    if (v && v.pem != null) return pemText(v.pem);
    if (ArrayBuffer.isView(v) || v instanceof ArrayBuffer) {
      try { return Buffer.from(v.buffer ? v.buffer : v, v.byteOffset || 0, v.byteLength).toString("utf8"); }
      catch (e) { return ""; }
    }
    return "";
  };
  function newNativeSecureContext(options) {
    options = options == null ? {} : options;
    if (asym && typeof asym.x509parse === "function") {
      const cert = options.cert;
      if (typeof cert === "string" && cert.indexOf("BEGIN CERTIFICATE") !== -1) {
        try { asym.x509parse(cert); } catch (e) { /* leave to connect-time */ }
      }
    }
    // node SecureContext::SetCert/SetKey run at createSecureContext() time, so a
    // certificate whose key does not match it — or an encrypted key with the
    // wrong passphrase — throws HERE, carrying OpenSSL's own reason string.
    // ref test-tls-key-mismatch, test-tls-passphrase.
    const TN = globalThis.__mbunNodeTlsNative;
    if (TN && typeof TN.checkKeyCert === "function" && (options.cert || options.key)) {
      const certPem = pemText(options.cert);
      const keyPem = pemText(options.key);
      if (certPem.indexOf("BEGIN") !== -1 || keyPem.indexOf("BEGIN") !== -1) {
        let reason = "";
        // node configSecureContext: a `key: [{ pem, passphrase }]` entry's own
        // passphrase WINS over options.passphrase, and the engine loads one key,
        // so the first entry's is the one that decides.
        let pass = typeof options.passphrase === "string" ? options.passphrase : "";
        {
          let entry = options.key;
          if (Array.isArray(entry)) entry = entry.length > 0 ? entry[0] : null;
          if (entry && typeof entry === "object" && typeof entry.passphrase === "string") pass = entry.passphrase;
        }
        // The cipher list has to reach the probe context BEFORE the cert: node
        // runs SecureContext::Init (which sets ciphers) ahead of SetCert, and
        // `@SECLEVEL=0` in that list is the documented way to keep a key OpenSSL
        // 3 would otherwise reject as too small.
        let cipherList = "";
        if (typeof options.ciphers === "string" && options.ciphers !== "") {
          try { cipherList = processCiphers(options.ciphers).cipherList; } catch (e) { cipherList = ""; }
        }
        try {
          reason = TN.checkKeyCert(certPem, keyPem, pass, cipherList);
        } catch (e) { reason = ""; }
        if (typeof reason === "string" && reason !== "") {
          // OpenSSL's packed reason string is what node surfaces verbatim; the
          // node-style code is derived from it the same way ThrowCryptoError does.
          const first = reason.split("; ")[0];
          const err = new Error(first);
          const parts = first.split(":");
          if (parts.length >= 5) {
            err.library = parts[2];
            err.reason = parts[4];
            err.code = "ERR_OSSL_" + parts[4].replace(/[ .]/g, "_").toUpperCase();
          }
          throw err;
        }
      }
    }
    const min = options.minVersion != null ? options.minVersion : DEFAULT_MIN_VERSION;
    const max = options.maxVersion != null ? options.maxVersion : DEFAULT_MAX_VERSION;
    const cas = [];
    const handle = Object.create(SecureContextHandle);
    Object.defineProperty(handle, kContextBrand, { value: true, enumerable: false });
    handle.__mbunSecureContext = true;
    handle.minVersion = min;
    handle.maxVersion = max;
    handle._cas = cas;
    handle.addCACert = (pem) => { cas.push(pem); };
    return handle;
  }

  const InternalSecureContext = class SecureContext {
    constructor(options, cached = true) {
      if (options) {
        validateSecureContextOptions(options);
        if (options.cert) throwOnInvalidTLSArray("options.cert", options.cert);
        if (options.key) throwOnInvalidTLSArray("options.key", options.key, true);
        if (options.ca) throwOnInvalidTLSArray("options.ca", options.ca);
        if (options.servername != null && typeof options.servername !== "string")
          throw new TypeError("servername argument must be an string");
        if (options.secureOptions != null && typeof options.secureOptions !== "number")
          throw new TypeError("secureOptions argument must be an number");
        const privateKeyIdentifier = options.privateKeyIdentifier;
        if (privateKeyIdentifier !== undefined && privateKeyIdentifier !== null) {
          const privateKeyEngine = options.privateKeyEngine;
          if (privateKeyEngine === undefined || privateKeyEngine === null)
            throw ERR_INVALID_ARG_VALUE("options.privateKeyEngine", privateKeyEngine);
          if (typeof privateKeyEngine !== "string")
            throw ERR_INVALID_ARG_TYPE("options.privateKeyEngine", ["string", "null", "undefined"], privateKeyEngine);
          if (typeof privateKeyIdentifier !== "string")
            throw ERR_INVALID_ARG_TYPE("options.privateKeyIdentifier", ["string", "null", "undefined"], privateKeyIdentifier);
        }
      }
      this.context = newNativeSecureContext(options);
      this.servername = options ? options.servername : undefined;
      // Keep the validated options: node hands a SecureContext to
      // `new tls.TLSSocket(sock, { secureContext })` and the live handshake layer
      // (js_tls_live.cppm) has to recover cert/key/ca from it — without this the
      // server side started with no certificate at all and OpenSSL answered
      // every ClientHello with "no shared cipher".
      this._secureOptions = options || {};
    }
  };
  function SecureContext(options) { return new InternalSecureContext(options); }
  // NODE_EXTRA_CA_CERTS that cannot be read is a WARNING, not an error: node's
  // NewRootCertStore() prints it to raw stderr once (the root store is built
  // once) and carries on with the bundled roots only. The trust anchors
  // themselves are added by the native layer's load_extra_root_certs_(); this is
  // only the diagnostic, kept here because it must fire even for a context that
  // never opens a connection (test-tls-env-bad-extra-ca does `createServer({})`
  // and nothing else). Blueprint: node src/crypto/crypto_context.cc.
  let _extraCaWarned = false;
  function warnBadExtraCACerts() {
    if (_extraCaWarned) return;
    const path = G.process && G.process.env && G.process.env.NODE_EXTRA_CA_CERTS;
    if (!path) return;
    _extraCaWarned = true;
    let reason = null;
    try {
      const fs = M["fs"] || M["node:fs"];
      if (fs && typeof fs.readFileSync === "function") fs.readFileSync(path);
    } catch (e) {
      // OpenSSL's own reason string, which is what node interpolates. The test's
      // regex wants the capitalised strerror text, not fs's lowercased message.
      const code = e && e.code;
      const text = code === "ENOENT" ? "No such file or directory"
        : code === "EACCES" ? "Permission denied"
        : code === "EISDIR" ? "Is a directory"
        : (e && e.message) || String(e);
      reason = "error:80000002:system library::" + text;
    }
    if (reason === null) return;
    const line = "Warning: Ignoring extra certs from `" + path + "`, load failed: " + reason + "\n";
    try {
      if (G.process && G.process.stderr && typeof G.process.stderr.write === "function") G.process.stderr.write(line);
      else if (G.process && typeof G.process._rawDebug === "function") G.process._rawDebug(line);
    } catch (e) {}
  }
  function createSecureContext(options) {
    if (options instanceof InternalSecureContext) return options;
    // node reaches NewRootCertStore() only in the `else` branch of
    // `if (ca) addCACert(...) else addRootCerts()`, so an explicit `ca` neither
    // gains the extra anchors nor triggers the warning.
    if (!(options && options.ca)) warnBadExtraCACerts();
    return new InternalSecureContext(options, false);
  }

  // ---- checkServerIdentity (RFC 6125), ported from bun-ref/node lib/tls.js ----
  // Canonical text form of an IP literal (NodeTLS.cpp Bun__canonicalizeIP): both
  // sides of the IP-SAN comparison below go through it, so "fe80:0:0:0:0:0:0:1"
  // and "fe80::1" (or an uppercase-hex SAN) match.
  // Input that is NOT an IP literal comes back `undefined`, exactly as node's
  // cares_wrap CanonicalizeIP does (it returns without setting a value when
  // neither inet_pton succeeds). That is observable: a CIDR SAN like
  // "IP Address:8.8.8.0/24" lands in the `ips` list as undefined, so the failure
  // reason node prints is "…is not in the cert's list: " with nothing after it
  // (test-tls-check-server-identity case 13). Echoing the raw text back instead
  // printed the CIDR — and, worse, would let a malformed SAN compare equal to a
  // hostname that is byte-identical to it. Falling back to `ip` is kept only for
  // the case where the native binding is absent altogether.
  const canonicalizeIP = (ip) => {
    const N = globalThis.__mbunNodeTlsNative;
    if (N && typeof N.canonicalizeIP === "function") return N.canonicalizeIP(ip);
    return ip;
  };
  const netIsIP = (h) => {
    if (net && typeof net.isIP === "function") { const r = net.isIP(h); if (r) return r; }
    if (/^(\d{1,3}\.){3}\d{1,3}$/.test(h)) return 4;
    if (h.indexOf(":") !== -1) return 6;
    return 0;
  };
  const unfqdn = (host) => host.replace(/[.]$/, "");
  const splitHost = (host) => unfqdn(host).replace(/[A-Z]/g, (c) => String.fromCharCode(32 + c.charCodeAt(0))).split(".");
  function check(hostParts, pattern, wildcards) {
    if (!pattern) return false;
    const patternParts = splitHost(pattern);
    if (hostParts.length !== patternParts.length) return false;
    if (patternParts.includes("")) return false;
    const isBad = (s) => { for (let i = 0; i < s.length; i++) { const c = s.charCodeAt(i); if (c < 33 || c > 127) return true; } return false; };
    if (patternParts.some(isBad)) return false;
    for (let i = hostParts.length - 1; i > 0; i -= 1) if (hostParts[i] !== patternParts[i]) return false;
    const hostSubdomain = hostParts[0];
    const patternSubdomain = patternParts[0];
    const patternSubdomainParts = patternSubdomain.split("*");
    if (patternSubdomainParts.length === 1 || patternSubdomain.includes("xn--")) return hostSubdomain === patternSubdomain;
    if (!wildcards) return false;
    if (patternSubdomainParts.length > 2) return false;
    if (patternParts.length <= 2) return false;
    const prefix = patternSubdomainParts[0];
    const suffix = patternSubdomainParts[1];
    if (prefix.length + suffix.length > hostSubdomain.length) return false;
    if (!hostSubdomain.startsWith(prefix)) return false;
    if (!hostSubdomain.endsWith(suffix)) return false;
    return true;
  }
  const jsonStringPattern = /^"(?:[^"\\]|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*"/;
  function splitEscapedAltNames(altNames) {
    const result = [];
    let currentToken = "";
    let offset = 0;
    while (offset !== altNames.length) {
      const nextSep = altNames.indexOf(", ", offset);
      const nextQuote = altNames.indexOf('"', offset);
      if (nextQuote !== -1 && (nextSep === -1 || nextQuote < nextSep)) {
        currentToken += altNames.substring(offset, nextQuote);
        const match = jsonStringPattern.exec(altNames.substring(nextQuote));
        if (!match) { const e = new Error("Invalid subject alternative name"); e.code = "ERR_TLS_CERT_ALTNAME_FORMAT"; throw e; }
        currentToken += JSON.parse(match[0]);
        offset = nextQuote + match[0].length;
      } else if (nextSep !== -1) {
        currentToken += altNames.substring(offset, nextSep);
        result.push(currentToken);
        currentToken = "";
        offset = nextSep + 2;
      } else {
        currentToken += altNames.substring(offset);
        offset = altNames.length;
      }
    }
    result.push(currentToken);
    return result;
  }
  function checkServerIdentity(hostname, cert) {
    const subject = cert.subject;
    const altNames = cert.subjectaltname;
    const dnsNames = [];
    const ips = [];
    hostname = "" + hostname;
    if (altNames) {
      const splitAltNames = altNames.includes('"') ? splitEscapedAltNames(altNames) : altNames.split(", ");
      splitAltNames.forEach((name) => {
        if (name.startsWith("DNS:")) dnsNames.push(name.slice(4));
        else if (name.startsWith("IP Address:")) ips.push(canonicalizeIP(name.slice(11)));
      });
    }
    let valid = false;
    let reason = "Unknown reason";
    hostname = unfqdn(hostname);
    if (netIsIP(hostname)) {
      valid = ips.includes(canonicalizeIP(hostname));
      if (!valid) reason = "IP: " + hostname + " is not in the cert's list: " + ips.join(", ");
    } else {
      const hasDnsNames = dnsNames.length > 0;
      const cn = subject && subject.CN;
      if (hasDnsNames || cn) {
        const hostParts = splitHost(hostname);
        const wildcard = (pattern) => check(hostParts, pattern, true);
        if (hasDnsNames) {
          valid = dnsNames.some(wildcard);
          if (!valid) reason = "Host: " + hostname + ". is not in the cert's altnames: " + altNames;
        } else {
          if (Array.isArray(cn)) valid = cn.some(wildcard);
          else if (cn) valid = wildcard(cn);
          if (!valid) reason = "Host: " + hostname + ". is not cert's CN: " + cn;
        }
      } else {
        reason = "Cert does not contain a DNS name";
      }
    }
    if (!valid) return ERR_TLS_CERT_ALTNAME_INVALID(reason, hostname, cert);
  }

  // ---- ALPN wire encoding (lib/tls.js convertALPNProtocols) ----
  function convertProtocols(protocols) {
    const lens = new Array(protocols.length);
    let total = 0;
    for (let i = 0; i < protocols.length; i++) {
      const len = Buffer.byteLength(protocols[i]);
      if (len > 255) { const err = new RangeError("The byte length of the protocol at index " + i + " exceeds the maximum length. It must be <= 255. Received " + len); err.code = "ERR_OUT_OF_RANGE"; throw err; }
      lens[i] = len;
      total += 1 + len;
    }
    const buff = Buffer.allocUnsafe(total);
    let offset = 0;
    for (let i = 0; i < protocols.length; i++) { buff[offset++] = lens[i]; buff.write(protocols[i], offset); offset += lens[i]; }
    return buff;
  }
  function convertALPNProtocols(protocols, out) {
    if (Array.isArray(protocols)) out.ALPNProtocols = convertProtocols(protocols);
    else if (ArrayBuffer.isView(protocols)) out.ALPNProtocols = Buffer.from(protocols.buffer.slice(protocols.byteOffset, protocols.byteOffset + protocols.byteLength));
  }

  // ---- getCiphers (native OpenSSL default cipher list) ----
  function getCiphers() {
    if (tlsNative && typeof tlsNative.getCiphers === "function") return tlsNative.getCiphers();
    return [];
  }

  function parseCertString() { const e = new Error("Not implemented"); e.code = "ERR_METHOD_NOT_IMPLEMENTED"; throw e; }

  // ---- rootCertificates (immutable). DEFERRED: full Mozilla NSS root bundle
  // (getBundledRootCertificates in NodeTLS.cpp). Seeded with a real, valid X509
  // root in PEM so the export is a non-empty, frozen array of well-formed PEM. ----
  // node answers tls.rootCertificates from its vendored Mozilla NSS bundle
  // (src/node_root_certs.h). mbun vendors no bundle, so it reports the platform
  // trust store it actually verifies chains against (__mbunNodeTlsNative
  // .rootCertificates reads the same file configure_default_trust_ loads). This
  // only *reports* what is already trusted — it adds nothing to the store, and a
  // store that cannot be read falls back to the single embedded PEM below rather
  // than to an empty (and therefore silently permissive-looking) list.
  const nativeRoots = (() => {
    if (!tlsNative || typeof tlsNative.rootCertificates !== "function") return null;
    try {
      const r = tlsNative.rootCertificates();
      if (Array.isArray(r) && r.length > 0) return Object.freeze(r);
    } catch (e) {}
    return null;
  })();
  const fallbackRootCertificates = Object.freeze([
    "-----BEGIN CERTIFICATE-----\n" +
    "MIIFCzCCA3OgAwIBAgIQBg0eUuH8A64LETs9IrQIbzANBgkqhkiG9w0BAQsFADCB\n" +
    "nTEeMBwGA1UEChMVbWtjZXJ0IGRldmVsb3BtZW50IENBMTkwNwYDVQQLDDBsdWR2\n" +
    "aWdATHVkdmlncy1NYWNCb29rLVByby5sb2NhbCAoTHVkdmlnIEhvem1hbikxQDA+\n" +
    "BgNVBAMMN21rY2VydCBsdWR2aWdATHVkdmlncy1NYWNCb29rLVByby5sb2NhbCAo\n" +
    "THVkdmlnIEhvem1hbikwHhcNMjQwNTI1MTIzMjI3WhcNMzQwNTI1MTIzMjI3WjCB\n" +
    "nTEeMBwGA1UEChMVbWtjZXJ0IGRldmVsb3BtZW50IENBMTkwNwYDVQQLDDBsdWR2\n" +
    "aWdATHVkdmlncy1NYWNCb29rLVByby5sb2NhbCAoTHVkdmlnIEhvem1hbikxQDA+\n" +
    "BgNVBAMMN21rY2VydCBsdWR2aWdATHVkdmlncy1NYWNCb29rLVByby5sb2NhbCAo\n" +
    "THVkdmlnIEhvem1hbikwggGiMA0GCSqGSIb3DQEBAQUAA4IBjwAwggGKAoIBgQDT\n" +
    "vKduL//b9hSVZOCrRFPFjpARpB3uAr1sjGd7TVeEdEkeJapO5BrQ4I8Unbtqo5JC\n" +
    "2U1lZv5Gl6Odlyc7m60c/F1py15zH6vMggUUshmtSdCxmVmXPBsbYXmuaDkEhxcH\n" +
    "+sE/60IfdkX/jw8cVNa5grIy7WbCpHsRxnUIFjij32kfOuvVY5UylEy+j0x6flGH\n" +
    "fl+a7nOO4qq6tZXaeBmagg0pAPVK3la6bFZDXPyO5KjwfjIIqF7H9nB5+YlIIIAg\n" +
    "GoCLU+1wOMsOzHgQFJcNecoX0k86v0gP9K5SD0+vgW3xbJ6xBdOBWCulWhWMY8Im\n" +
    "f66lMBYkJYnVFg6MnNOjl7wIToyy0nNEZvkwwSBhETjXaKyMF1+vEHxYLtbucla9\n" +
    "JkVYDC0yU7AhZNKbsyiI+V/M0FMCKW3QZip2q7trst8GnA0vURWXOyj5iZ96nh7X\n" +
    "BbNFSkuY0wBBNwbr0p/pTHE/FF6BlBPXl6XQdpXM6/YVvrqj3dOW7P5WUIIU10cC\n" +
    "AwEAAaNFMEMwDgYDVR0PAQH/BAQDAgIEMBIGA1UdEwEB/wQIMAYBAf8CAQAwHQYD\n" +
    "VR0OBBYEFGbncunr3eyd5EhwKVHy+4S3vMkwMA0GCSqGSIb3DQEBCwUAA4IBgQC6\n" +
    "h20ry+Z7ma8G4XPGcEKhbwAROGSfYCnygmGC5V1j/Wshcro4/qrts9qDtq6MtCzC\n" +
    "5vMB40xSo60EWtDaNQbRhRZHvA1Agkzyi5NnFHQARKn+eSyNV+7wmDWRy9nb5bGH\n" +
    "A48mWREOTaQLi6BY6OPvLr376+dzdMx8GL/uMHz/1rQDU1/4e6lRxYPzrSuT8SPe\n" +
    "Zb112wpkbJuT69HvbT3mrYQVsagX5qJ1NML2/6+ichB9ou08ZIyksVd+8TKLP/zn\n" +
    "QSYhzrgcI5pTnyi2AybKRy07EjcAFNBzKiHP42S4+AudOUYUzdeNxMpgelgTiHjU\n" +
    "kkYncgeQ6qXzA3uC4ODTBZWGnslzSATY0IuLvn9/ZcgZmj1GcEeRyaxpkdE7JaX0\n" +
    "KIpPD6WIFHSB/6VwjFTUxf49+yW9U9bdaPlWOcHXUtOfoikC/EK1OXfX+sAd4OhE\n" +
    "8iyfiWz4jpOK9oBhqGsJLooaU4TXLzfMXYWyIjOOIoZX3QECUFQ4Zw3rJ9oV1A8=\n" +
    "-----END CERTIFICATE-----",
  ]);
  const rootCertificates = nativeRoots || fallbackRootCertificates;

  // ---- honest DEFERRED throw for the live socket/handshake surface ----
  function deferredTLS(what) {
    const e = new Error("tls." + what + " requires the socket event loop + real SSL_CTX handshake (DEFERRED in mbun: modules/tls TlsChannel / S-net)");
    e.code = "ERR_MBUN_DEFERRED";
    return e;
  }

  // ---- TLSSocket class shape (extends net.Socket) ----
  const NetSocket = (net && net.Socket) || G.__mbunNetSocket || class {};
  class TLSSocket extends NetSocket {
    constructor(socket, options) {
      // node _tls_wrap.js: the TLSSocket itself is never half-open
      // (allowHalfOpen hardcoded false in the net.Socket options).
      super({ ...(options || {}), allowHalfOpen: false });
      this.encrypted = true;
      this.authorized = false;
      this.authorizationError = null;
      this.alpnProtocol = null;
      this.servername = (options && options.servername) || undefined;
      this._secureEstablished = false;
      this._securePending = true;
      this.secureConnecting = false;
      this.ALPNProtocols = options && options.ALPNProtocols;
      this._handle = null;
    }
    getPeerCertificate(detailed) { return this._handle ? this._handle.getPeerCertificate && this._handle.getPeerCertificate(detailed) : undefined; }
    getCertificate() { return this._handle ? this._handle.getCertificate && this._handle.getCertificate() : null; }
    getCipher() { return this._handle ? this._handle.getCipher && this._handle.getCipher() : undefined; }
    getProtocol() { return this._handle ? this._handle.getProtocol && this._handle.getProtocol() : null; }
    getSession() { return this._handle ? this._handle.getSession && this._handle.getSession() : undefined; }
    getEphemeralKeyInfo() { return this._handle ? this._handle.getEphemeralKeyInfo && this._handle.getEphemeralKeyInfo() : null; }
    getSharedSigalgs() { return this._handle ? this._handle.getSharedSigalgs && this._handle.getSharedSigalgs() : []; }
    getFinished() { return this._handle ? this._handle.getFinished && this._handle.getFinished() : undefined; }
    getPeerFinished() { return this._handle ? this._handle.getPeerFinished && this._handle.getPeerFinished() : undefined; }
    getTLSTicket() { return this._handle ? this._handle.getTLSTicket && this._handle.getTLSTicket() : undefined; }
    isSessionReused() { return false; }
    setServername(name) { this.servername = name; return this; }
    setSession() { return this; }
    setMaxSendFragment() { return false; }
    disableRenegotiation() {}
    enableTrace() {}
    exportKeyingMaterial() { throw deferredTLS("TLSSocket.exportKeyingMaterial"); }
    renegotiate() { return false; }
    connect() { throw deferredTLS("connect"); }
  }

  // ---- Server / createServer shape ----
  const NetServer = (net && net.Server) || class {};
  class Server extends NetServer {
    constructor(options, secureConnectionListener) {
      super();
      if (typeof options === "function") { secureConnectionListener = options; options = {}; }
      // node tls.Server: the constructor runs setSecureContext(options), i.e.
      // createSecureContext — so an unusable option (an unmatched cipher list,
      // a bad secureProtocol, …) throws here, not at first connection.
      validateSecureContextOptions(options || {});
      this._sharedCreds = options || {};
      this._contexts = new Map();
      if (typeof secureConnectionListener === "function" && typeof this.on === "function")
        this.on("secureConnection", secureConnectionListener);
    }
    setSecureContext(options) { validateSecureContextOptions(options || {}); this._sharedCreds = options || {}; }
    addContext(servername, context) {
      const ctx = context instanceof InternalSecureContext ? context : new InternalSecureContext(context);
      this._contexts.set(servername, ctx);
    }
    getTicketKeys() { return Buffer.alloc(48); }
    setTicketKeys() { return this; }
    listen() { throw deferredTLS("Server.listen"); }
  }
  function createServer(options, connectionListener) { return new Server(options, connectionListener); }
  function connect() { throw deferredTLS("connect"); }
  // getCACertificates(type): 'default'|'system'|'bundled'|'extra'. mbun serves the
  // frozen rootCertificates bundle for default/system/bundled, empty for 'extra';
  // result is cached so repeated calls return the same reference (node parity).
  let _caCache = null;
  // tls.setDefaultCACertificates() replaces the 'default' store only; 'bundled'
  // and 'system' keep reporting the built-in bundle (node parity).
  let _defaultCAs = null;
  // node's getCACertificates(type) runs validateString(type, 'type') FIRST, so a
  // non-string is ERR_INVALID_ARG_TYPE and only an unrecognised *string* is
  // ERR_INVALID_ARG_VALUE (test-tls-get-ca-certificates-error asserts both).
  let _extraCAs = null;
  // 'default' + NODE_EXTRA_CA_CERTS, cached so repeated calls keep returning the
  // same frozen array (test-tls-get-ca-certificates-default compares by ===).
  let _defaultWithExtra = null;
  function getCACertificates(type) {
    const t = type === undefined ? "default" : type;
    if (typeof t !== "string") throw ERR_INVALID_ARG_TYPE("type", "string", t);
    if (t === "default") {
      if (_defaultCAs !== null) return _defaultCAs;
      // node builds the default store as bundled/system PLUS whatever
      // NODE_EXTRA_CA_CERTS contributed, so 'default' is a superset of 'extra'
      // (test-tls-get-ca-certificates-extra-subset asserts exactly that).
      const extra = getCACertificates("extra");
      if (extra.length !== 0) {
        if (_defaultWithExtra === null) {
          const merged = rootCertificates.slice();
          for (const pem of extra) if (merged.indexOf(pem) === -1) merged.push(pem);
          _defaultWithExtra = Object.freeze(merged);
        }
        return _defaultWithExtra;
      }
      if (_caCache === null) _caCache = Object.freeze(rootCertificates.slice());
      return _caCache;
    }
    // 'bundled' is node's built-in store, and node asserts it is the very same
    // array object as tls.rootCertificates (test-tls-get-ca-certificates-bundled
    // compares by reference), so hand back the frozen bundle itself.
    if (t === "bundled") return rootCertificates;
    if (t === "system") {
      if (_caCache === null) _caCache = Object.freeze(rootCertificates.slice());
      return _caCache;
    }
    // 'extra' is what NODE_EXTRA_CA_CERTS added, i.e. nothing when it is unset.
    if (t === "extra") {
      if (_extraCAs === null) {
        const blocks = [];
        try {
          const path = G.process && G.process.env && G.process.env.NODE_EXTRA_CA_CERTS;
          if (path) {
            const fs = M["fs"] || M["node:fs"];
            const text = fs && typeof fs.readFileSync === "function"
              ? String(fs.readFileSync(path, "utf8")) : "";
            const found = text.match(/-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----/g);
            // node hands back OpenSSL's PEM_write output, which terminates every
            // block with a newline; test-tls-get-ca-certificates-extra compares
            // the result against the raw fixture file byte for byte.
            if (found) for (const b of found) blocks.push(b + "\n");
          }
        } catch (e) {}
        _extraCAs = Object.freeze(blocks);
      }
      return _extraCAs;
    }
    const e = new TypeError("The argument 'type' must be one of: 'default', 'system', 'bundled', 'extra'. Received " + JSON.stringify(type));
    e.code = "ERR_INVALID_ARG_VALUE";
    throw e;
  }
  // tls.setDefaultCACertificates(certs): accepts PEM strings and any
  // ArrayBufferView holding PEM text. node feeds the concatenation to OpenSSL's
  // PEM reader, so the failure modes are: no PEM block at all in the input ->
  // ERR_CRYPTO_OPERATION_FAILED, a PEM block OpenSSL cannot decode ->
  // ERR_OSSL_PEM_ASN1_LIB. Either way the previous default store is left intact
  // (the operation is all-or-nothing) and duplicates collapse to one entry.
  const CERT_BLOCK_RE = /-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----/g;
  function toPemText(v, name) {
    name = name || "certs";
    if (typeof v === "string") return v;
    if (ArrayBuffer.isView(v)) {
      const u8 = new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
      if (Buffer) return Buffer.from(u8).toString("utf8");
      let s = ""; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
      return s;
    }
    if (v instanceof ArrayBuffer) {
      const u8 = new Uint8Array(v);
      if (Buffer) return Buffer.from(u8).toString("utf8");
      let s = ""; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
      return s;
    }
    // node validates each element with validateStringOrBufferView(cert,
    // `certs[${i}]`), so the name carries the index and the accepted class is
    // the single umbrella ArrayBufferView, not the Buffer/TypedArray/DataView
    // triple (test-tls-set-default-ca-certificates-error matches the message).
    throw ERR_INVALID_ARG_TYPE(name, ["string", "ArrayBufferView"], v);
  }
  function setDefaultCACertificates(certs) {
    if (!Array.isArray(certs)) throw ERR_INVALID_ARG_TYPE("certs", "Array", certs);
    const blocks = [];
    for (let i = 0; i < certs.length; i++) {
      const text = toPemText(certs[i], "certs[" + i + "]");
      const found = text.match(CERT_BLOCK_RE);
      if (found) for (const b of found) blocks.push(b);
    }
    if (blocks.length === 0 && certs.length !== 0) {
      const e = new Error("No valid certificates found in the provided array");
      e.code = "ERR_CRYPTO_OPERATION_FAILED";
      throw e;
    }
    const seen = new Set();
    const out = [];
    for (const pem of blocks) {
      let parsed;
      try { parsed = asym && typeof asym.x509parse === "function" ? asym.x509parse(pem) : null; }
      catch (err) {
        const e = new Error(err && err.message ? err.message : "error:0688010A:PEM routines::ASN.1 library");
        e.code = "ERR_OSSL_PEM_ASN1_LIB";
        throw e;
      }
      const key = parsed ? (String(parsed.serialNumber) + "|" + String(parsed.issuer) + "|" + String(parsed.subject)) : pem;
      if (seen.has(key)) continue;
      seen.add(key);
      out.push(pem);
    }
    _defaultCAs = Object.freeze(out);
    // The mutated store has to reach the TLS engine, not just getCACertificates:
    // node's setDefaultCACertificates replaces what addRootCerts() would have
    // installed, so a client with no explicit `ca` verifies against THIS list and
    // nothing else. Cache the concatenated PEM once — the list is typically the
    // whole root bundle and a per-connection join would be quadratic.
    _defaultCAPem = out.join("\n");
  }
  // null until the process calls setDefaultCACertificates(); afterwards the
  // complete trust store as PEM, "" meaning "trust nothing". js_tls_live reads it
  // for any client that supplied no `ca` of its own. Never widens trust: it can
  // only replace the platform store with what the caller explicitly handed over.
  let _defaultCAPem = null;
  function defaultCAPem() { return _defaultCAPem; }

  // ---- install onto the node:tls module object ----
  const assign = {
    CLIENT_RENEG_LIMIT: 3,
    CLIENT_RENEG_WINDOW: 600,
    DEFAULT_CIPHERS,
    // The compiled-in list, before any --tls-cipher-list override. Not a node
    // export: it is how js_tls_live populates crypto.constants.defaultCoreCipherList
    // without duplicating the literal.
    __mbunCoreCiphers: DEFAULT_CORE_CIPHERS,
    connect,
    convertALPNProtocols,
    createSecureContext,
    createServer,
    DEFAULT_ECDH_CURVE,
    getCiphers,
    getCACertificates,
    setDefaultCACertificates,
    parseCertString,
    // Internal: js_tls_live needs the same split before it hands the two lists
    // to the native context. Not part of node's surface.
    __mbunProcessCiphers: processCiphers,
    __mbunDefaultCAPem: defaultCAPem,
    SecureContext,
    Server,
    TLSSocket,
    checkServerIdentity,
  };
  for (const k in assign) { try { T[k] = assign[k]; } catch (e) {} }

  // DEFAULT_MIN/MAX_VERSION as live accessors (node mutates the exports object).
  try {
    Object.defineProperty(T, "DEFAULT_MIN_VERSION", { configurable: true, enumerable: true, get: () => DEFAULT_MIN_VERSION, set: (v) => { DEFAULT_MIN_VERSION = v; } });
    Object.defineProperty(T, "DEFAULT_MAX_VERSION", { configurable: true, enumerable: true, get: () => DEFAULT_MAX_VERSION, set: (v) => { DEFAULT_MAX_VERSION = v; } });
  } catch (e) {}

  // rootCertificates: non-writable, non-configurable, and the array is frozen.
  try { Object.defineProperty(T, "rootCertificates", { configurable: false, enumerable: true, writable: false, value: rootCertificates }); } catch (e) {}
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
