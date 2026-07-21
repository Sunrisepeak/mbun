// node:crypto ASYMMETRIC + cipher JS layer. Patches the already-installed
// node:crypto module object (globalThis.__mbunNativeModules["crypto"]) with real
// implementations backed by the OpenSSL EVP bridge __mbunCryptoAsymNative
// (runtime/crypto_asym.inc): generateKeyPair(Sync), createSign/createVerify +
// sign/verify one-shots, publicEncrypt/privateDecrypt, createCipheriv/
// createDecipheriv (aes-*-cbc/gcm/ctr/ecb), createECDH/ECDH, X509Certificate,
// createPublicKey/createPrivateKey/KeyObject.
//
// NOTE: appended AFTER the master builtins IIFE (opened in bootstrap, closed by
// image_closure) has already run, so it is a self-contained IIFE that re-binds
// G = globalThis. Everything it needs (Buffer, node:crypto, node:stream) is on
// the global / module registry by the time this evaluates.
//
// Blueprint: bun-ref src/js/node/crypto.ts + src/runtime/crypto/crypto.classes.ts;
// node lib/internal/crypto/{sig,cipher,keys,ec,x509}.js. DEFERRED: createDiffieHellman.
export module mbun.jsc.js_builtins:crypto_asym;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kCryptoAsymJS = R"JS(
(function () {
  const G = globalThis;
  const AN = G.__mbunCryptoAsymNative;
  const M = G.__mbunNativeModules;
  if (!AN || !M) return;
  const C = M["crypto"];
  if (!C) return;
  const stream = M["stream"] || M["node:stream"];
  const Transform = stream && stream.Transform;

  // ---- RSA padding / PSS salt constants (node values == OpenSSL values) ----
  const RSA_PKCS1_PADDING = 1, RSA_NO_PADDING = 3, RSA_PKCS1_OAEP_PADDING = 4, RSA_PKCS1_PSS_PADDING = 6;
  const RSA_PSS_SALTLEN_DIGEST = -1, RSA_PSS_SALTLEN_MAX_SIGN = -2, RSA_PSS_SALTLEN_AUTO = -2;
  Object.assign(C.constants, {
    RSA_PKCS1_PADDING, RSA_NO_PADDING, RSA_PKCS1_OAEP_PADDING, RSA_PKCS1_PSS_PADDING,
    RSA_PSS_SALTLEN_DIGEST, RSA_PSS_SALTLEN_MAX_SIGN, RSA_PSS_SALTLEN_AUTO,
    RSA_X931_PADDING: 5, RSA_SSLV23_PADDING: 2,
  });

  // ---- FIPS mode (non-FIPS OpenSSL build) ----
  // node exposes getFips()/setFips()/`fips`. mbun links a stock (non-FIPS)
  // OpenSSL, so FIPS is always off; enabling it is the documented hard error.
  // ref: node lib/internal/crypto/util.js getFipsCrypto/setFipsCrypto.
  if (typeof C.getFips !== "function") {
    C.getFips = () => 0;
    C.setFips = (v) => {
      if (v) {
        const e = new Error("Cannot set FIPS mode in a non-FIPS build.");
        e.code = "ERR_CRYPTO_FIPS_UNAVAILABLE";
        throw e;
      }
    };
    Object.defineProperty(C, "fips", {
      get: () => false, set: (v) => C.setFips(v), enumerable: true, configurable: true,
    });
  }

  const isView = (v) => ArrayBuffer.isView(v);
  const toBuf = (v, enc) => {
    if (v == null) return Buffer.alloc(0);
    if (typeof v === "string") return Buffer.from(v, enc || "utf8");
    if (v instanceof ArrayBuffer) return Buffer.from(v);
    if (isView(v)) return Buffer.from(v.buffer, v.byteOffset, v.byteLength);
    return Buffer.from(v);
  };
  const enc = (buf, e) => {
    if (!e || e === "buffer") return Buffer.from(buf);
    return Buffer.from(buf).toString(e);
  };

  // Normalize a sign/verify algorithm to an OpenSSL digest name ("" == none, for
  // Ed25519/Ed448). Accepts "sha256", "RSA-SHA256", "sha1", null, etc.
  const digestName = (algo) => {
    if (algo == null) return "";
    let s = String(algo).toLowerCase();
    if (s.startsWith("rsa-")) s = s.slice(4);
    if (s.startsWith("ecdsa-with-")) s = s.slice(11);
    // Legacy OpenSSL name: DSS1 is an alias for SHA-1 (DSA signatures).
    if (s === "dss1") s = "sha1";
    return s;
  };

  // Resolve a key argument (string PEM | Buffer/DER | KeyObject | { key, format,
  // type, passphrase, padding, saltLength, dsaEncoding, oaepHash, oaepLabel }) to
  // { data, passphrase, padding, saltLength, dsaEncoding, oaepHash, oaepLabel }.
  const resolveKey = (k) => {
    if (k == null) throw new TypeError("No key provided");
    if (k instanceof KeyObject) return { data: k._km, passphrase: k._pass || "" };
    if (typeof k === "string" || isView(k) || k instanceof ArrayBuffer) return { data: k, passphrase: "" };
    // { key: <JWK object>, format: "jwk", ... } — materialize the JWK to DER up
    // front (private when `d` is present) so the native signer/verifier gets real
    // key bytes. dsaEncoding rides along for EC ieee-p1363 vs der output.
    if (typeof k === "object" && k.format === "jwk" && k.key != null && typeof k.key === "object") {
      const isPriv = k.key.d != null;
      return { data: jwkToDer(k.key, isPriv), passphrase: "", dsaEncoding: k.dsaEncoding };
    }
    if (typeof k === "object" && ("key" in k || "pem" in k)) {
      const inner = resolveKey(k.key != null ? k.key : k.pem);
      const pass = k.passphrase != null ? (typeof k.passphrase === "string" ? k.passphrase : toBuf(k.passphrase).toString("latin1")) : inner.passphrase;
      return {
        data: inner.data, passphrase: pass,
        padding: k.padding, saltLength: k.saltLength, dsaEncoding: k.dsaEncoding,
        oaepHash: k.oaepHash, oaepLabel: k.oaepLabel,
        encoding: k.encoding,
      };
    }
    return { data: k, passphrase: "" };
  };
  // publicEncrypt/privateDecrypt accept { key, encoding } where key is a hex/etc
  // string; honor the encoding when converting to bytes.
  const keyData = (r) => (typeof r.data === "string" && r.encoding && r.encoding !== "utf8")
    ? toBuf(r.data, r.encoding) : r.data;

  // ---- publicEncrypt / privateDecrypt (RSA) ----
  // The options.encoding (when key is an object) applies to string key, buffer,
  // oaepLabel and passphrase (node semantics).
  // node lib/internal/crypto/cipher.js: options.oaepHash is validated as a
  // string before reaching the native EVP layer.
  const validateOaepHash = (r) => {
    if (r.oaepHash !== undefined && typeof r.oaepHash !== "string") {
      const e = new TypeError('The "options.oaepHash" property must be of type string. Received ' +
        (r.oaepHash === null ? "null" : typeof r.oaepHash));
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
  };
  // node cipher.js: oaepLabel is validated with getArrayBufferOrView when it is
  // !== undefined (null is NOT accepted as "no label"). ref: bun JSCipher.cpp:157
  // + CryptoUtil.cpp getArrayBufferOrView2.
  const toOaepLabel = (r) => {
    const v = r.oaepLabel;
    if (v === undefined) return null;
    if (typeof v !== "string" && !(v instanceof ArrayBuffer) && !isView(v)) {
      const e = new TypeError('The "options.oaepLabel" argument must be of type string or ' +
        'an instance of ArrayBuffer, Buffer, TypedArray, or DataView. Received ' +
        (v === null ? "null" : typeof v));
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    return toBuf(v, r.encoding);
  };
  C.publicEncrypt = (key, buffer) => {
    const r = resolveKey(key);
    validateOaepHash(r);
    const label = toOaepLabel(r);
    return Buffer.from(AN.publicEncrypt(keyData(r), r.passphrase, toBuf(buffer, r.encoding),
      r.padding != null ? r.padding : RSA_PKCS1_OAEP_PADDING, r.oaepHash || "", label));
  };
  C.privateDecrypt = (key, buffer) => {
    const r = resolveKey(key);
    validateOaepHash(r);
    // node cipher.js (CVE-2023-46809): RSA_PKCS1_PADDING is rejected for
    // private decryption.
    if (r.padding === RSA_PKCS1_PADDING) {
      const e = new TypeError('The property \'options.padding\' is invalid. Received ' + r.padding);
      e.code = "ERR_INVALID_ARG_VALUE"; throw e;
    }
    const label = toOaepLabel(r);
    return Buffer.from(AN.privateDecrypt(keyData(r), r.passphrase, toBuf(buffer, r.encoding),
      r.padding != null ? r.padding : RSA_PKCS1_OAEP_PADDING, r.oaepHash || "", label));
  };
  // privateEncrypt/publicDecrypt (RSA raw sign / verify_recover paths).
  C.privateEncrypt = (key, buffer) => {
    const r = resolveKey(key);
    return Buffer.from(AN.privateEncrypt(keyData(r), r.passphrase, toBuf(buffer, r.encoding),
      r.padding != null ? r.padding : RSA_PKCS1_PADDING));
  };
  C.publicDecrypt = (key, buffer) => {
    const r = resolveKey(key);
    return Buffer.from(AN.publicDecrypt(keyData(r), r.passphrase, toBuf(buffer, r.encoding),
      r.padding != null ? r.padding : RSA_PKCS1_PADDING));
  };

  // node lib/internal/crypto/keys.js: dsaEncoding must be "der" or "ieee-p1363".
  const validateDsaEncoding = (r) => {
    if (r.dsaEncoding !== undefined && r.dsaEncoding !== "der" && r.dsaEncoding !== "ieee-p1363") {
      const e = new TypeError("The property 'options.dsaEncoding' is invalid. Received '" + r.dsaEncoding + "'");
      e.code = "ERR_INVALID_ARG_VALUE"; throw e;
    }
  };

  // ---- sign / verify (one-shot + streaming) ----
  const doSign = (algo, data, key) => {
    const r = resolveKey(key);
    validateDsaEncoding(r);
    return Buffer.from(AN.sign(digestName(algo), toBuf(data), keyData(r), r.passphrase,
      r.padding != null ? r.padding : RSA_PKCS1_PADDING,
      r.saltLength != null ? r.saltLength : RSA_PSS_SALTLEN_MAX_SIGN,
      r.dsaEncoding || ""));
  };
  const doVerify = (algo, data, key, sig) => {
    // Snapshot the data and signature bytes at call time (node reads them before
    // touching the key). A key object with a mutating `passphrase` getter must not
    // be able to change the signature bytes we verify against.
    const dataBuf = Buffer.from(toBuf(data));
    const sigBuf = Buffer.from(toBuf(sig));
    const r = resolveKey(key);
    validateDsaEncoding(r);
    return AN.verify(digestName(algo), dataBuf, keyData(r), r.passphrase, sigBuf,
      r.padding != null ? r.padding : RSA_PKCS1_PADDING,
      r.saltLength != null ? r.saltLength : RSA_PSS_SALTLEN_MAX_SIGN,
      r.dsaEncoding || "");
  };
  C.sign = (algorithm, data, key, callback) => {
    if (typeof callback === "function") {
      queueMicrotask(() => { try { callback(null, doSign(algorithm, data, key)); } catch (e) { callback(e); } });
      return;
    }
    return doSign(algorithm, data, key);
  };
  C.verify = (algorithm, data, key, signature, callback) => {
    if (typeof callback === "function") {
      queueMicrotask(() => { try { callback(null, doVerify(algorithm, data, key, signature)); } catch (e) { callback(e); } });
      return;
    }
    return doVerify(algorithm, data, key, signature);
  };

  // Sign / Verify stream objects (Writable-ish: update()/sign()/verify()).
  // node exposes these as constructors callable WITHOUT `new` (Sign('sha256')
  // returns a fresh instance) — function form + instanceof guard reproduces that.
  function Sign(algorithm) {
    if (!(this instanceof Sign)) return new Sign(algorithm);
    this._algo = algorithm; this._chunks = [];
  }
  Sign.prototype.update = function (data, inputEnc) { this._chunks.push(toBuf(data, inputEnc)); return this; };
  Sign.prototype.write = function (data, inputEnc) { this.update(data, inputEnc); return true; };
  Sign.prototype.end = function (data, inputEnc) { if (data != null) this.update(data, inputEnc); return this; };
  Sign.prototype.sign = function (key, outputEnc) {
    const out = doSign(this._algo, Buffer.concat(this._chunks), key);
    return outputEnc ? out.toString(outputEnc) : out;
  };
  function Verify(algorithm) {
    if (!(this instanceof Verify)) return new Verify(algorithm);
    this._algo = algorithm; this._chunks = [];
  }
  Verify.prototype.update = function (data, inputEnc) { this._chunks.push(toBuf(data, inputEnc)); return this; };
  Verify.prototype.write = function (data, inputEnc) { this.update(data, inputEnc); return true; };
  Verify.prototype.end = function (data, inputEnc) { if (data != null) this.update(data, inputEnc); return this; };
  Verify.prototype.verify = function (key, signature, sigEnc) {
    const sig = typeof signature === "string" ? Buffer.from(signature, sigEnc || "hex") : toBuf(signature);
    return doVerify(this._algo, Buffer.concat(this._chunks), key, sig);
  };
  C.Sign = Sign; C.Verify = Verify;
  C.createSign = (algorithm) => new Sign(algorithm);
  C.createVerify = (algorithm) => new Verify(algorithm);

  // ---- JWK (RFC 7517/7518) ----
  // AN.jwkExport/jwkImport speak raw component bytes; node's surface is
  // base64url strings. Component order is preserved from the native object
  // (kty,n,e,d,p,q,dp,dq,qi / kty,x,y,crv,d) to match node's key order.
  const JWK_PARTS = ["n", "e", "d", "p", "q", "dp", "dq", "qi", "x", "y"];
  // NOTE: decoding goes through atob rather than Buffer.from(s,"base64url").
  // Buffer.from / Buffer.byteLength do not recognize the "base64url" encoding in
  // this build (they fall back to latin1 and yield the string's ASCII bytes —
  // silently, no throw), while buf.write(s,"base64url") and
  // buf.toString("base64url") are both correct. Encoding via Buffer is therefore
  // fine; decoding is not.
  const b64uDecode = (text) => {
    const binary = G.atob(String(text).replace(/-/g, "+").replace(/_/g, "/"));
    const out = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) out[i] = binary.charCodeAt(i);
    return out;
  };
  const jwkFromKey = (material, pass, isPublic) => {
    const raw = AN.jwkExport(material, pass || "", !!isPublic);
    const out = {};
    for (const k of Object.keys(raw)) {
      const v = raw[k];
      out[k] = typeof v === "string" ? v : Buffer.from(v).toString("base64url");
    }
    return out;
  };
  const jwkToDer = (jwk, isPrivate) => {
    if (jwk == null || typeof jwk !== "object") throw new TypeError("Invalid JWK");
    const parts = { kty: jwk.kty };
    if (jwk.crv != null) parts.crv = jwk.crv;
    for (const k of JWK_PARTS) {
      if (typeof jwk[k] === "string") parts[k] = b64uDecode(jwk[k]);
    }
    return Buffer.from(AN.jwkImport(parts, !!isPrivate));
  };

  // ---- KeyObject / createPublicKey / createPrivateKey ----
  // node's KeyObject constructor is internal: user code cannot mint one from raw
  // material (it takes a native handle). We reproduce that with a private brand —
  // internal construction goes through mkKO(); `new KeyObject(...)` from user code
  // (missing/invalid handle) throws. The brand is shared with the structured-clone
  // reconstructor via C.__koBrand.
  const kKObrand = Symbol("mbun.node.KeyObject");
  class KeyObject {
    constructor(brand, kind, material, passphrase) {
      if (brand !== kKObrand) throw new TypeError("Illegal constructor");
      this._kind = kind; this._km = material; this._pass = passphrase || "";
    }
    get type() { return this._kind; }
    get [Symbol.toStringTag]() { return "KeyObject"; }
    get asymmetricKeyType() {
      if (this._kind === "secret") return undefined;
      try { return AN.keyType(this._km, this._pass, this._kind === "public").type; } catch { return undefined; }
    }
    get asymmetricKeyDetails() {
      if (this._kind === "secret") return undefined;
      try {
        const t = AN.keyType(this._km, this._pass, this._kind === "public");
        const d = {};
        if (t.modulusLength != null) d.modulusLength = t.modulusLength;
        if (t.publicExponent != null) d.publicExponent = BigInt("0x" + Buffer.from(t.publicExponent).toString("hex"));
        if (t.namedCurve != null) d.namedCurve = t.namedCurve;
        return d;
      } catch { return {}; }
    }
    get symmetricKeySize() { return this._kind === "secret" ? toBuf(this._km).length : undefined; }
    export(options) {
      // Secret keys: options are optional and default to a Buffer copy.
      if (this._kind === "secret") {
        if (options != null && typeof options === "object" && options.format === "jwk") {
          return { kty: "oct", k: Buffer.from(toBuf(this._km)).toString("base64url") };
        }
        return Buffer.from(toBuf(this._km));
      }
      // Asymmetric keys: node requires an options object (lib/internal/crypto/keys.js).
      if (options === null || typeof options !== "object") {
        const e = new TypeError('The "options" argument must be of type object. Received ' +
          (options === null ? "null" : typeof options));
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      // format:"jwk" is NOT a PEM/DER encoding — it returns a plain JWK object and
      // cannot carry encryption (cipher/passphrase).
      if (options.format === "jwk") {
        if (options.passphrase != null || options.cipher != null) {
          const e = new Error("The selected key encoding jwk does not support encryption.");
          e.code = "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS"; throw e;
        }
        return jwkFromKey(this._km, this._pass, this._kind === "public");
      }
      const type = options.type || (this._kind === "public" ? "spki" : "pkcs8");
      const format = options.format || "pem";
      // Encrypting a private key requires a cipher; a passphrase alone throws.
      if (this._kind === "private" && options.passphrase != null && options.cipher == null) {
        const e = new TypeError("The property 'options.cipher' is invalid. Received undefined");
        e.code = "ERR_INVALID_ARG_VALUE"; throw e;
      }
      const cipher = options.cipher || "";
      const outPass = options.passphrase != null ? (typeof options.passphrase === "string" ? options.passphrase : toBuf(options.passphrase).toString("latin1")) : "";
      const out = AN.keyExport(this._km, this._pass, this._kind === "public", type, format, cipher, outPass);
      // der format must be a Buffer (node returns Buffer, not a bare Uint8Array).
      return format === "der" ? Buffer.from(out) : out;
    }
    equals(other) {
      if (!(other instanceof KeyObject)) {
        const e = new TypeError('The "otherKeyObject" argument must be an instance of KeyObject. Received type ' +
          typeof other + " (" + String(other) + ")");
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      if (other._kind !== this._kind) return false;
      try { return Buffer.compare(toBuf(this.export({ format: this._kind === "secret" ? undefined : "der", type: this._kind === "public" ? "spki" : "pkcs8" })), toBuf(other.export({ format: "der", type: this._kind === "public" ? "spki" : "pkcs8" }))) === 0; } catch { return false; }
    }
    // node: KeyObject.from(cryptoKey) — only a WebCrypto CryptoKey is accepted.
    static from(key) {
      if (!(G.CryptoKey && key instanceof G.CryptoKey)) {
        const e = new TypeError('The "key" argument must be an instance of CryptoKey. Received ' +
          (key === null ? "null" : typeof key));
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      // Bridge into a node KeyObject via the WebCrypto raw export, when reachable.
      const bridge = G.__mbunCryptoKeyToKeyObject;
      if (typeof bridge === "function") { const r = bridge(key); if (r) return mkKO(r.kind, r.material, r.passphrase || ""); }
      throw new TypeError("Converting this CryptoKey to a KeyObject is not supported yet in mbun");
    }
  }
  const mkKO = (kind, material, passphrase) => new KeyObject(kKObrand, kind, material, passphrase);
  C.__koBrand = kKObrand;
  const makeKeyObject = (kind, key) => {
    if (key instanceof KeyObject) return key;
    // { key: <JWK object>, format: "jwk" } → materialize as DER up front so the
    // rest of the pipeline sees ordinary key material (node keys.js).
    if (key != null && typeof key === "object" && key.format === "jwk" && key.key != null) {
      return mkKO(kind, jwkToDer(key.key, kind === "private"), "");
    }
    const r = resolveKey(key);
    return mkKO(kind, r.data, r.passphrase);
  };
  C.KeyObject = KeyObject;
  // Map a native key-parse failure to the OpenSSL error node surfaces: PEM input
  // (has a "-----BEGIN" header) keeps the native/passphrase error; binary DER that
  // starts with a SEQUENCE tag (0x30) is a decode failure; anything else has no
  // PEM start line.
  const asymParseError = (ko, nativeErr) => {
    const isStr = typeof ko._km === "string";
    const bytes = toBuf(ko._km);
    const head = isStr ? ko._km.slice(0, 64) : Buffer.from(bytes.slice(0, 64)).toString("latin1");
    if (head.includes("-----BEGIN")) return nativeErr; // surface native parse/passphrase error
    if (!isStr && bytes.length > 0 && bytes[0] === 0x30) {
      const e = new Error("error:06000066:public key routines:OPENSSL_internal:DECODE_ERROR");
      e.code = "ERR_OSSL_UNSUPPORTED"; return e;
    }
    const e = new Error("error:0900006e:PEM routines:OPENSSL_internal:NO_START_LINE");
    e.code = "ERR_OSSL_NO_START_LINE"; return e;
  };
  C.createPrivateKey = (key) => {
    // node: passing an existing KeyObject to createPrivateKey is never allowed
    // (getKeyObjectHandle, kCreatePrivate → ERR_INVALID_ARG_TYPE).
    if (key instanceof KeyObject) {
      const e = new TypeError('The "key" argument must be of type string or an instance of ' +
        "ArrayBuffer, Buffer, TypedArray, DataView, Object, or CryptoKey. Received an instance of KeyObject");
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    const ko = makeKeyObject("private", key);
    // node/bun validate the material at construction: non-key input throws
    // ERR_OSSL_NO_START_LINE (bun 1.4.0 verified). jsonwebtoken's sign()
    // relies on that throw to fall back to createSecretKey for HS* secrets.
    let info;
    try {
      info = AN.keyType(ko._km, ko._pass, false);
    } catch (e) {
      throw asymParseError(ko, e);
    }
    // The loader is intent-agnostic: it will parse public-only material (e.g. a
    // PKCS#1 RSAPublicKey) as a pkey. node rejects that with a decode error.
    if (info && info.private === false) {
      const e = new Error("error:06000066:public key routines:OPENSSL_internal:DECODE_ERROR");
      e.code = "ERR_OSSL_UNSUPPORTED"; throw e;
    }
    return ko;
  };
  C.createPublicKey = (key) => {
    // node getKeyObjectHandle(kCreatePublic): a private KeyObject derives its
    // public half; any other KeyObject (public/secret) throws.
    if (key instanceof KeyObject) {
      if (key._kind === "private") {
        const pem = AN.keyExport(key._km, key._pass, true, "spki", "pem", "", "");
        return mkKO("public", pem, "");
      }
      const e = new TypeError("Invalid key object type " + key._kind + ", expected private.");
      e.code = "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; throw e;
    }
    const ko = makeKeyObject("public", key);
    // Same construction-time validation as createPrivateKey: jsonwebtoken's
    // verify() relies on the throw to fall back to createSecretKey for HS*
    // string secrets. Private material stays accepted (node derives the
    // public half) and certificates pass through to the native layer.
    try {
      AN.keyType(ko._km, ko._pass, true);
    } catch (ePub) {
      let privOk = false;
      try { AN.keyType(ko._km, ko._pass, false); privOk = true; } catch { /* not a private key either */ }
      if (!privOk) {
        const head = typeof ko._km === "string"
          ? ko._km.slice(0, 64)
          : Buffer.from(toBuf(ko._km).slice(0, 64)).toString("latin1");
        if (head.includes("-----BEGIN CERTIFICATE")) return ko;
        throw asymParseError(ko, ePub);
      }
    }
    return ko;
  };
  C.createSecretKey = (key, encoding) => {
    // node rejects anything that is not raw key material: a plain object (e.g. a
    // malicious `{ toString }`) must throw ERR_INVALID_ARG_TYPE, not be silently
    // coerced to an empty Buffer. jsonwebtoken's verify() depends on this throw
    // to report "secretOrPublicKey is not valid key material".
    const okType = typeof key === "string" || key instanceof ArrayBuffer || isView(key);
    if (!okType) {
      const err = new TypeError(
        'The "key" argument must be of type string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView. Received ' +
          (key === null ? "null" : typeof key));
      err.code = "ERR_INVALID_ARG_TYPE";
      throw err;
    }
    return mkKO("secret", toBuf(key, encoding), "");
  };

  // ---- generateKey / generateKeySync (symmetric secret keys) ----
  // node lib/internal/crypto/keygen.js SecretKeyGenTraits: 'hmac' length is in
  // bits (8 .. 2**31-1), 'aes' length must be one of 128/192/256.
  const genSecret = (type, options) => {
    if (typeof type !== "string") {
      const e = new TypeError('The "type" argument must be of type string. Received ' +
        (type === null ? "null" : (typeof type === "object" ? "an instance of " + (type.constructor && type.constructor.name || "Object") : "type " + typeof type + " (" + String(type) + ")")));
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    const t = type;
    if (t !== "hmac" && t !== "aes") {
      const e = new TypeError("The argument 'type' must be a supported key type. Received '" + t + "'");
      e.code = "ERR_INVALID_ARG_VALUE"; throw e;
    }
    if (typeof options !== "object" || options === null || Array.isArray(options)) {
      const e = new TypeError('The "options" argument must be of type object. Received ' +
        (options === null ? "null" : (Array.isArray(options) ? "an instance of Array" : "type " + typeof options + " (" + String(options) + ")")));
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    const length = options.length;
    if (typeof length !== "number" || !Number.isInteger(length)) {
      const e = new TypeError('The "options.length" property must be of type number.' +
        (length === undefined ? " Received undefined" : ""));
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    if (t === "aes") {
      if (length !== 128 && length !== 192 && length !== 256) {
        const e = new TypeError("The property 'options.length' must be one of: 128, 192, 256. Received " + length);
        e.code = "ERR_INVALID_ARG_VALUE"; throw e;
      }
    } else if (length < 8 || length > 2147483647) {
      const e = new RangeError('The value of "options.length" is out of range. It must be >= 8 && <= 2147483647. Received ' + length);
      e.code = "ERR_OUT_OF_RANGE"; throw e;
    }
    return mkKO("secret", Buffer.from(C.randomBytes(Math.floor(length / 8))), "");
  };
  C.generateKeySync = (type, options) => genSecret(type, options);
  C.generateKey = (type, options, callback) => {
    if (typeof callback !== "function") {
      const e = new TypeError('The "callback" argument must be of type function.');
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    // node validates type/options synchronously (throwing before any async work);
    // only the key material delivery is deferred to the callback.
    const k = genSecret(type, options);
    queueMicrotask(() => callback(null, k));
  };

  // ---- generateKeyPair / generateKeyPairSync ----
  // node validates the key type synchronously; unknown types (incl. the PQC
  // ml-dsa/ml-kem/slh-dsa families on an OpenSSL build without them) throw
  // ERR_INVALID_ARG_VALUE before any async work. ref node lib/internal/crypto/keygen.js.
  const KEYPAIR_TYPES = { rsa: 1, "rsa-pss": 1, dsa: 1, ec: 1, ed25519: 1, ed448: 1, x25519: 1, x448: 1, dh: 1 };
  const validateKeyPairType = (type) => {
    if (typeof type !== "string" || !KEYPAIR_TYPES[type]) {
      const e = new TypeError("The argument 'type' must be a supported key type. Received " +
        (typeof type === "string" ? "'" + type + "'" : (type === null ? "null" : typeof type === "object" ? "an instance of " + (type && type.constructor && type.constructor.name || "Object") : type)));
      e.code = "ERR_INVALID_ARG_VALUE"; throw e;
    }
  };
  const genKeyPair = (type, options) => {
    validateKeyPairType(type);
    options = options || {};
    const penc = options.publicKeyEncoding || {};
    const senc = options.privateKeyEncoding || {};
    const wantPubObj = !options.publicKeyEncoding;
    const wantPrivObj = !options.privateKeyEncoding;
    // format:"jwk" is not a PEM/DER encoding: node emits a plain JWK object and
    // ignores `type`/`cipher` (lib/internal/crypto/keygen.js). Ask the native
    // encoder for DER and convert, exactly like KeyObject.export({format:"jwk"}).
    const pubJwk = !wantPubObj && penc.format === "jwk";
    const privJwk = !wantPrivObj && senc.format === "jwk";
    const pubType = pubJwk ? "spki" : (penc.type || "spki");
    const pubFmt = (wantPubObj || pubJwk) ? "der" : (penc.format || "pem");
    const privType = privJwk ? "pkcs8" : (senc.type || "pkcs8");
    const privFmt = (wantPrivObj || privJwk) ? "der" : (senc.format || "pem");
    const cipher = privJwk ? "" : (senc.cipher || "");
    const pass = privJwk || senc.passphrase == null ? ""
      : (typeof senc.passphrase === "string" ? senc.passphrase : toBuf(senc.passphrase).toString("latin1"));
    const modLen = options.modulusLength || 2048;
    const curve = options.namedCurve || "";
    const res = AN.generateKeyPair(type, modLen, curve, pubType, pubFmt, privType, privFmt, cipher, pass);
    let publicKey = res.publicKey, privateKey = res.privateKey;
    if (wantPubObj) publicKey = mkKO("public", publicKey, "");
    else if (pubJwk) publicKey = jwkFromKey(publicKey, "", true);
    else if (pubFmt === "der") publicKey = Buffer.from(publicKey);
    if (wantPrivObj) privateKey = mkKO("private", privateKey, pass);
    else if (privJwk) privateKey = jwkFromKey(privateKey, "", false);
    else if (privFmt === "der") privateKey = Buffer.from(privateKey);
    return { publicKey, privateKey };
  };
  C.generateKeyPairSync = (type, options) => genKeyPair(type, options);
  C.generateKeyPair = (type, options, callback) => {
    const cb = typeof options === "function" ? options : callback;
    const opts = typeof options === "function" ? undefined : options;
    validateKeyPairType(type);   // synchronous type validation (node throws before async work)
    queueMicrotask(() => {
      try { const { publicKey, privateKey } = genKeyPair(type, opts); cb(null, publicKey, privateKey); }
      catch (e) { cb(e); }
    });
  };
  C.generateKeyPair[Symbol.for("nodejs.util.promisify.custom")] =
    (type, options) => new Promise((resolve, reject) => {
      try { resolve(genKeyPair(type, options)); } catch (e) { reject(e); }
    });

  // ---- getCipherInfo (native EVP probe) ----
  // node util.js getCipherInfo(nameOrNid[, { keyLength, ivLength }]). Validates
  // args then delegates to AN.cipherInfo (EVP_get_cipherbyname / key+iv-length
  // feasibility); an unknown cipher or unsupported length yields undefined.
  C.getCipherInfo = (nameOrNid, options) => {
    if (typeof nameOrNid !== "string" && typeof nameOrNid !== "number") {
      const e = new TypeError('The "nameOrNid" argument must be of type string or number. Received ' +
        (nameOrNid === null ? "null" : (typeof nameOrNid === "object" ? "an instance of " + (nameOrNid && nameOrNid.constructor && nameOrNid.constructor.name || "Object") : "type " + typeof nameOrNid)));
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    let keyLength, ivLength;
    if (options !== undefined) {
      if (typeof options !== "object" || options === null || Array.isArray(options)) {
        const e = new TypeError('The "options" argument must be of type object. Received ' +
          (options === null ? "null" : "type " + typeof options));
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      keyLength = options.keyLength; ivLength = options.ivLength;
      if (keyLength !== undefined && typeof keyLength !== "number") {
        const e = new TypeError('The "options.keyLength" property must be of type number.');
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      if (ivLength !== undefined && typeof ivLength !== "number") {
        const e = new TypeError('The "options.ivLength" property must be of type number.');
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
    }
    const info = AN.cipherInfo(nameOrNid, keyLength, ivLength);
    return info == null ? undefined : info;
  };

  // ---- createCipheriv / createDecipheriv ----
  // Cipheriv/Decipheriv are Transform-backed constructors, callable WITHOUT `new`
  // (returning a fresh instance) with distinct prototypes so `x instanceof
  // Decipheriv` holds. All construction-time validation matches node cipher.js.
  // Decorate a native OpenSSL error ("error:CODE:library:function:reason") with
  // node's error surface: .code (ERR_OSSL_<REASON>), .reason, .library, .function.
  const decorateOsslError = (e) => {
    const m = e && typeof e.message === "string" ? e.message : "";
    const parts = m.split(":");
    if (parts[0] === "error" && parts.length >= 5) {
      const reason = parts.slice(4).join(":");
      e.reason = reason;
      e.library = parts[2];
      e.function = parts[3];
      e.code = "ERR_OSSL_" + reason.toUpperCase().replace(/[^A-Z0-9]+/g, "_").replace(/^_+|_+$/g, "");
    }
    return e;
  };
  const validEnc = (e) => e === undefined || e === null || e === "buffer" || Buffer.isEncoding(e);
  // Track/validate an output encoding across update()/final() (node: an output
  // encoding cannot change once set; an unknown one throws ERR_UNKNOWN_ENCODING).
  const noteOutEnc = (self, e) => {
    if (e === undefined || e === null || e === "buffer") return;
    if (!Buffer.isEncoding(e)) { const err = new TypeError("Unknown encoding: " + e); err.code = "ERR_UNKNOWN_ENCODING"; throw err; }
    if (self._outEnc !== undefined && self._outEnc !== e) throw new Error("Cannot change encoding");
    self._outEnc = e;
  };
  function initCipher(self, isEncrypt, algorithm, key, iv, options) {
    if (typeof algorithm !== "string") {
      const e = new TypeError('The "cipher" argument must be of type string. Received ' +
        (algorithm === null ? "null" : (typeof algorithm === "object" ? "an instance of " + (algorithm && algorithm.constructor && algorithm.constructor.name || "Object") : "type " + typeof algorithm + " (" + String(algorithm) + ")")));
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    if (key == null) {
      const e = new TypeError('The "key" argument must be of type string or an instance of ArrayBuffer, Buffer, TypedArray, DataView, KeyObject, or CryptoKey. Received ' + (key === null ? "null" : "undefined"));
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    // A secret KeyObject is accepted as the key (node cipher.js prepareSecretKey).
    if (key instanceof KeyObject) key = key._km;
    // iv: string | ArrayBuffer/view | null accepted; number/undefined/etc rejected.
    if (iv !== null && typeof iv !== "string" && !isView(iv) && !(iv instanceof ArrayBuffer)) {
      const e = new TypeError('The "iv" argument must be of type string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView. Received ' +
        (iv === undefined ? "undefined" : (typeof iv === "object" ? "an instance of " + (iv && iv.constructor && iv.constructor.name || "Object") : "type " + typeof iv + " (" + String(iv) + ")")));
      e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    options = options || {};
    const keyBuf = toBuf(key, options.encoding);
    const ivBuf = iv == null ? Buffer.alloc(0) : toBuf(iv);
    // Cipher existence + key/iv-length validation via the native EVP probe.
    const info = C.getCipherInfo(algorithm.toLowerCase());
    if (!info) { const e = new Error("Unknown cipher"); e.code = "ERR_CRYPTO_UNKNOWN_CIPHER"; throw e; }
    if (keyBuf.length !== info.keyLength) throw new RangeError("Invalid key length");
    const mode = info.mode;
    if (mode === "ecb") { if (ivBuf.length !== 0) throw new Error("Invalid initialization vector"); }
    else if (mode === "gcm" || mode === "ccm" || mode === "ocb") { if (ivBuf.length < 1) throw new Error("Invalid initialization vector"); }
    else if (ivBuf.length !== (info.ivLength || 0)) throw new Error("Invalid initialization vector");
    if (options.authTagLength !== undefined &&
        (typeof options.authTagLength !== "number" || !Number.isInteger(options.authTagLength) || options.authTagLength < 0)) {
      throw new TypeError("The property 'options.authTagLength' is invalid. Received " + String(options.authTagLength));
    }
    self._algo = algorithm.toLowerCase();
    self._enc = isEncrypt;
    self._key = keyBuf;
    self._iv = ivBuf;
    self._auth = mode === "gcm" || mode === "ccm" || mode === "ocb";
    self._chunks = [];
    self._aad = null;
    self._tag = null;              // encrypt: output tag; decrypt: expected tag
    self._tagLen = options.authTagLength || 16;
    self._tagLenSet = options.authTagLength !== undefined;
    self._outEnc = undefined;
    self._noPad = false;
    self._done = false;
    return self;
  }
  function Cipheriv(algorithm, key, iv, options) {
    if (!(this instanceof Cipheriv)) return new Cipheriv(algorithm, key, iv, options);
    const self = Transform ? Reflect.construct(Transform, [], Cipheriv) : this;
    return initCipher(self, true, algorithm, key, iv, options);
  }
  function Decipheriv(algorithm, key, iv, options) {
    if (!(this instanceof Decipheriv)) return new Decipheriv(algorithm, key, iv, options);
    const self = Transform ? Reflect.construct(Transform, [], Decipheriv) : this;
    return initCipher(self, false, algorithm, key, iv, options);
  }
  if (Transform) {
    Cipheriv.prototype = Object.create(Transform.prototype);
    Decipheriv.prototype = Object.create(Transform.prototype);
    Object.setPrototypeOf(Cipheriv, Transform);
    Object.setPrototypeOf(Decipheriv, Transform);
  }
  const cipherProto = {
    setAAD(buffer) { this._aad = toBuf(buffer); return this; },
    setAutoPadding(ap) { this._noPad = arguments.length > 0 && !ap; return this; },
    getAuthTag() { if (!this._auth || !this._enc || this._tag == null) throw new Error("Unsupported state or unable to authenticate data"); return this._tag; },
    setAuthTag(tag) {
      const t = toBuf(tag);
      // GCM/CCM/OCB: without an explicit authTagLength the tag must be 16 bytes;
      // with one it must match exactly. node ERR_CRYPTO_INVALID_AUTH_TAG.
      if (this._auth) {
        const ok = this._tagLenSet ? (t.length === this._tagLen) : (t.length === 16);
        if (!ok) { const e = new Error("Invalid authentication tag length: " + t.length); e.code = "ERR_CRYPTO_INVALID_AUTH_TAG"; throw e; }
      }
      this._tag = t; return this;
    },
    update(data, inputEnc, outputEnc) {
      if (this._done) throw new Error("Trying to add data in an unsupported state");
      if (typeof data !== "string" && !isView(data) && !(data instanceof ArrayBuffer)) {
        const e = new TypeError('The "data" argument must be of type string or an instance of Buffer, TypedArray, or DataView.' +
          (data === undefined ? " Received undefined" : ""));
        e.code = "ERR_INVALID_ARG_TYPE"; throw e;
      }
      noteOutEnc(this, outputEnc);
      const bytes = toBuf(data, inputEnc);
      if (bytes.length >= 2147483647) { const e = new RangeError('The value of "data" is out of range. It must be <= 2147483646. Received ' + bytes.length); e.code = "ERR_OUT_OF_RANGE"; throw e; }
      this._chunks.push(bytes);
      return (outputEnc && outputEnc !== "buffer") ? "" : Buffer.alloc(0);
    },
    _run() {
      const data = Buffer.concat(this._chunks);
      let res;
      try {
        res = AN.cipher(this._algo, this._key, this._iv, data, this._aad,
          this._enc, this._enc ? null : this._tag, this._tagLen, this._noPad);
      } catch (e) { throw decorateOsslError(e); }
      if (this._enc && res.tag != null) this._tag = Buffer.from(res.tag);
      return Buffer.from(res.data);
    },
    final(outputEnc) {
      if (this._done) throw new Error("Trying to add data in an unsupported state");
      noteOutEnc(this, outputEnc);
      this._done = true;
      const out = this._run();
      return (outputEnc && outputEnc !== "buffer") ? out.toString(outputEnc) : out;
    },
    _transform(chunk, e, cb) { this._chunks.push(toBuf(chunk)); cb(); },
    _flush(cb) { try { this._done = true; this.push(this._run()); cb(); } catch (err) { cb(err); } },
  };
  Object.assign(Cipheriv.prototype, cipherProto);
  Object.assign(Decipheriv.prototype, cipherProto);
  Cipheriv.prototype.constructor = Cipheriv;
  Decipheriv.prototype.constructor = Decipheriv;
  C.Cipheriv = Cipheriv; C.Decipheriv = Decipheriv;
  C.createCipheriv = (algorithm, key, iv, options) => new Cipheriv(algorithm, key, iv, options);
  C.createDecipheriv = (algorithm, key, iv, options) => new Decipheriv(algorithm, key, iv, options);

  // ---- ECDH ----
  const CURVE_NIDS = { secp256k1: "secp256k1", prime256v1: "prime256v1", secp384r1: "secp384r1", secp521r1: "secp521r1" };
  // ECDH is a node constructor callable WITHOUT `new` (function form + guard).
  function ECDH(curve) {
    if (!(this instanceof ECDH)) return new ECDH(curve);
    if (typeof curve !== "string" || !AN.ecValidCurve(curve)) {
      throw new TypeError("Invalid EC curve name: " + curve);
    }
    this._curve = curve; this._priv = null; this._pub = null;
  }
  ECDH.prototype.generateKeys = function (encoding, format) {
    const r = AN.ecdhGenerate(this._curve);
    this._priv = Buffer.from(r.privateKey);
    this._pub = Buffer.from(r.publicKey);
    return this.getPublicKey(encoding, format);
  };
  ECDH.prototype.computeSecret = function (otherPublic, inputEnc, outputEnc) {
    const pub = typeof otherPublic === "string" ? Buffer.from(otherPublic, inputEnc) : toBuf(otherPublic);
    const sec = Buffer.from(AN.ecdhComputeSecret(this._curve, this._priv, pub));
    return (outputEnc && outputEnc !== "buffer") ? sec.toString(outputEnc) : sec;
  };
  ECDH.prototype.getPublicKey = function (encoding, format) {
    if (format !== undefined && format !== "compressed" && format !== "uncompressed" && format !== "hybrid") {
      const e = new TypeError("Invalid ECDH format: " + format); e.code = "ERR_CRYPTO_ECDH_INVALID_FORMAT"; throw e;
    }
    let pub;
    if (format === "compressed") pub = Buffer.from(AN.ecdhConvertKey(this._curve, this._pub, true));
    else {
      pub = Buffer.from(AN.ecdhConvertKey(this._curve, this._pub, false));
      // hybrid point: uncompressed X||Y prefixed 0x06 (Y even) / 0x07 (Y odd).
      if (format === "hybrid") { pub = Buffer.from(pub); pub[0] = 0x06 | (pub[pub.length - 1] & 1); }
    }
    return (encoding && encoding !== "buffer") ? pub.toString(encoding) : Buffer.from(pub);
  };
  ECDH.prototype.getPrivateKey = function (encoding) { return (encoding && encoding !== "buffer") ? this._priv.toString(encoding) : Buffer.from(this._priv); };
  ECDH.prototype.setPrivateKey = function (key, encoding) { this._priv = typeof key === "string" ? Buffer.from(key, encoding) : toBuf(key); this._pub = Buffer.from(AN.ecdhPublicFromPrivate(this._curve, this._priv)); return this; };
  ECDH.prototype.setPublicKey = function (key, encoding) { this._pub = typeof key === "string" ? Buffer.from(key, encoding) : toBuf(key); return this; };
  ECDH.convertKey = (key, curve, inputEnc, outputEnc, format) => {
    // node diffiehellman.js convertKey validation order: encoding → curve → format.
    let pt;
    if (typeof key === "string") {
      const enc = inputEnc || "utf8";
      if (!Buffer.isEncoding(enc)) { const e = new TypeError("Unknown encoding: " + enc); e.code = "ERR_UNKNOWN_ENCODING"; throw e; }
      pt = Buffer.from(key, enc);
      if (String(enc).toLowerCase() === "hex" && pt.length * 2 !== key.length) {
        const e = new TypeError("The argument 'encoding' is invalid for data of length " + key.length + ". Received '" + enc + "'");
        e.code = "ERR_INVALID_ARG_VALUE"; throw e;
      }
    } else pt = toBuf(key);
    if (typeof C.getCurves === "function" && C.getCurves().indexOf(curve) === -1) {
      const e = new TypeError("Invalid EC curve name"); e.code = "ERR_CRYPTO_INVALID_CURVE"; throw e;
    }
    if (format !== undefined && format !== "compressed" && format !== "uncompressed" && format !== "hybrid") {
      const e = new Error("Invalid ECDH format: " + format); e.code = "ERR_CRYPTO_ECDH_INVALID_FORMAT"; throw e;
    }
    const out = Buffer.from(AN.ecdhConvertKey(curve, pt, format === "compressed"));
    return (outputEnc && outputEnc !== "buffer") ? out.toString(outputEnc) : out;
  };
  C.ECDH = ECDH;
  C.createECDH = (curve) => new ECDH(curve);

  // ---- DiffieHellman (JS BigInt: MODP named groups + user-supplied primes) ----
  // modexp over BigInt; primes come from the MODP group table (RFC 2412/3526) or
  // the user. ref node lib/internal/crypto/diffiehellman.js. DEFERRED: fast native
  // safe-prime generation for createDiffieHellman(<bitLength>) (slow in JS).
  const MODP = {
    modp1: "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A63A3620FFFFFFFFFFFFFFFF",
    modp2: "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE649286651ECE65381FFFFFFFFFFFFFFFF",
    modp5: "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA237327FFFFFFFFFFFFFFFF",
    modp14: "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3BE39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183995497CEA956AE515D2261898FA051015728E5A8AACAA68FFFFFFFFFFFFFFFF",
  };
  const dhBufToBig = (buf) => { const b = toBuf(buf); let n = 0n; for (let i = 0; i < b.length; i++) n = (n << 8n) | BigInt(b[i]); return n; };
  const dhBigToBuf = (n, padLen) => {
    let hex = n.toString(16); if (hex.length & 1) hex = "0" + hex;
    const len = hex.length / 2;
    const bytes = new Uint8Array(len);
    for (let i = 0; i < len; i++) bytes[i] = parseInt(hex.substr(i * 2, 2), 16);
    if (padLen != null && len < padLen) { const out = new Uint8Array(padLen); out.set(bytes, padLen - len); return Buffer.from(out); }
    return Buffer.from(bytes);
  };
  const dhModPow = (base, exp, mod) => { let r = 1n; base %= mod; if (base < 0n) base += mod; while (exp > 0n) { if (exp & 1n) r = (r * base) % mod; exp >>= 1n; base = (base * base) % mod; } return r; };
  const dhSmallPrimes = [2n, 3n, 5n, 7n, 11n, 13n, 17n, 19n, 23n, 29n, 31n, 37n];
  const dhIsProbablePrime = (n) => {
    if (n < 2n) return false;
    for (const p of dhSmallPrimes) { if (n === p) return true; if (n % p === 0n) return false; }
    let d = n - 1n, r = 0n; while ((d & 1n) === 0n) { d >>= 1n; r++; }
    for (const a of dhSmallPrimes) {
      let x = dhModPow(a, d, n);
      if (x === 1n || x === n - 1n) continue;
      let composite = true;
      for (let i = 1n; i < r; i++) { x = (x * x) % n; if (x === n - 1n) { composite = false; break; } }
      if (composite) return false;
    }
    return true;
  };
  const dhRandPrime = (bits) => {
    // Prefer the native OpenSSL prime generator (fast); fall back to a JS
    // random-candidate + Miller-Rabin loop when unavailable.
    if (typeof AN.dhGeneratePrime === "function") return dhBufToBig(AN.dhGeneratePrime(bits));
    const bytes = Math.ceil(bits / 8);
    for (;;) {
      const r = new Uint8Array(C.randomBytes(bytes));
      r[0] |= 0x80; r[bytes - 1] |= 1;   // full bit length, odd candidate
      let n = 0n; for (let i = 0; i < bytes.length; i++) n = (n << 8n) | BigInt(r[i]);
      if (dhIsProbablePrime(n)) return n;
    }
  };
  const dhInvalidState = () => { const e = new Error("Invalid state"); e.code = "ERR_CRYPTO_INVALID_STATE"; return e; };
  const dhEnsureP = (self) => { if (self._p == null && self._size) self._p = dhRandPrime(self._size); return self._p; };
  const dhPrimeLen = (self) => dhBigToBuf(dhEnsureP(self)).length;
  // Shared read/compute surface for DiffieHellman + DiffieHellmanGroup.
  const dhShared = {
    generateKeys(enc) {
      const p = dhEnsureP(this);
      // node DH_generate_key keeps a pre-set private key and only (re)derives the
      // public half; otherwise it picks a fresh random private key.
      if (this._priv == null) {
        const len = dhBigToBuf(p).length;
        let x = dhBufToBig(C.randomBytes(len)) % (p - 3n);
        if (x < 0n) x = -x;
        this._priv = x + 2n;
      }
      this._pub = dhModPow(this._g, this._priv, p);
      return this.getPublicKey(enc);
    },
    computeSecret(other, inEnc, outEnc) {
      const p = dhEnsureP(this);
      const ob = typeof other === "string" ? Buffer.from(other, inEnc) : toBuf(other);
      const sec = dhModPow(dhBufToBig(ob), this._priv, p);
      const out = dhBigToBuf(sec, dhBigToBuf(p).length);
      return (outEnc && outEnc !== "buffer") ? out.toString(outEnc) : out;
    },
    getPrime(enc) { const b = dhBigToBuf(dhEnsureP(this)); return enc && enc !== "buffer" ? b.toString(enc) : b; },
    getGenerator(enc) { const b = dhBigToBuf(this._g); return enc && enc !== "buffer" ? b.toString(enc) : b; },
    getPublicKey(enc) { if (this._pub == null) throw dhInvalidState(); const b = dhBigToBuf(this._pub, dhPrimeLen(this)); return enc && enc !== "buffer" ? b.toString(enc) : b; },
    getPrivateKey(enc) { if (this._priv == null) throw dhInvalidState(); const b = dhBigToBuf(this._priv); return enc && enc !== "buffer" ? b.toString(enc) : b; },
  };
  // node normalization: DiffieHellman(sizeOrKey, keyEncoding, generator, genEncoding)
  // — a 2nd arg that is not an encoding is actually the generator.
  const dhNormArgs = (sizeOrKey, keyEncoding, generator, genEncoding) => {
    if (keyEncoding !== undefined && keyEncoding !== null && !Buffer.isEncoding(keyEncoding) && keyEncoding !== "buffer") {
      genEncoding = generator; generator = keyEncoding; keyEncoding = undefined;
    }
    return { sizeOrKey, keyEncoding, generator, genEncoding };
  };
  const dhInit = (self, primeArg, primeEnc, genArg, genEnc) => {
    if (typeof primeArg === "number") { self._size = primeArg; self._p = null; }
    else { self._p = dhBufToBig(typeof primeArg === "string" ? Buffer.from(primeArg, primeEnc || undefined) : primeArg); self._size = 0; }
    if (genArg == null) self._g = 2n;
    else if (typeof genArg === "number") self._g = BigInt(genArg);
    else if (typeof genArg === "string") self._g = dhBufToBig(Buffer.from(genArg, genEnc || "utf8"));
    else self._g = dhBufToBig(genArg);
    self._priv = null; self._pub = null;
  };
  function DiffieHellman(sizeOrKey, keyEncoding, generator, genEncoding) {
    if (!(this instanceof DiffieHellman)) return new DiffieHellman(sizeOrKey, keyEncoding, generator, genEncoding);
    if (typeof sizeOrKey === "number") {
      if (!Number.isInteger(sizeOrKey)) { const e = new RangeError('The value of "sizeOrKey" is out of range. It must be an integer. Received ' + sizeOrKey); e.code = "ERR_OUT_OF_RANGE"; throw e; }
    } else if (typeof sizeOrKey !== "string" && !isView(sizeOrKey) && !(sizeOrKey instanceof ArrayBuffer)) {
      const e = new TypeError('The "sizeOrKey" argument must be of type number or string or an instance of ArrayBuffer, Buffer, TypedArray, or DataView. Received ' + (sizeOrKey === null ? "null" : typeof sizeOrKey)); e.code = "ERR_INVALID_ARG_TYPE"; throw e;
    }
    const a = dhNormArgs(sizeOrKey, keyEncoding, generator, genEncoding);
    if (typeof a.generator === "number" && !Number.isInteger(a.generator)) { const e = new RangeError('The value of "generator" is out of range. It must be an integer. Received ' + a.generator); e.code = "ERR_OUT_OF_RANGE"; throw e; }
    dhInit(this, a.sizeOrKey, a.keyEncoding, a.generator, a.genEncoding);
  }
  // node's verifyError exposes DH_check() flags. We flag a too-small or composite
  // modulus as non-zero (matching node for bad user primes); ordinary probable
  // primes of DH size verify clean (0), so we don't require a safe prime here.
  const dhVerifyError = function () {
    const p = this._p;
    if (p == null) return 0;
    const bits = dhBigToBuf(p).length * 8;
    if (bits < 512) return 2;               // modulus too small / not a safe prime
    if (!dhIsProbablePrime(p)) return 1;    // DH_CHECK_P_NOT_PRIME
    return 0;
  };
  Object.assign(DiffieHellman.prototype, dhShared, {
    setPublicKey(key, enc) { this._pub = dhBufToBig(typeof key === "string" ? Buffer.from(key, enc) : toBuf(key)); return this; },
    setPrivateKey(key, enc) { this._priv = dhBufToBig(typeof key === "string" ? Buffer.from(key, enc) : toBuf(key)); return this; },
  });
  Object.defineProperty(DiffieHellman.prototype, "verifyError", { configurable: true, enumerable: true, get: dhVerifyError });
  DiffieHellman.prototype.constructor = DiffieHellman;
  function DiffieHellmanGroup(name) {
    if (!(this instanceof DiffieHellmanGroup)) return new DiffieHellmanGroup(name);
    const hex = MODP[String(name)];
    if (!hex) { const e = new Error("Unknown group: " + name); e.code = "ERR_CRYPTO_UNKNOWN_DH_GROUP"; throw e; }
    this._p = dhBufToBig(Buffer.from(hex, "hex")); this._size = 0; this._g = 2n; this._priv = null; this._pub = null;
  }
  // A named group has no setters (node DiffieHellmanGroup).
  Object.assign(DiffieHellmanGroup.prototype, dhShared);
  Object.defineProperty(DiffieHellmanGroup.prototype, "verifyError", { configurable: true, enumerable: true, get: dhVerifyError });
  DiffieHellmanGroup.prototype.constructor = DiffieHellmanGroup;
  C.DiffieHellman = DiffieHellman;
  C.DiffieHellmanGroup = DiffieHellmanGroup;
  C.createDiffieHellman = (sizeOrKey, keyEncoding, generator, genEncoding) => new DiffieHellman(sizeOrKey, keyEncoding, generator, genEncoding);
  C.createDiffieHellmanGroup = (name) => new DiffieHellmanGroup(name);
  C.getDiffieHellman = (name) => new DiffieHellmanGroup(name);

  // ---- X509Certificate ----
  const wildcardMatch = (host, pattern, allowWildcard) => {
    host = host.toLowerCase(); pattern = pattern.toLowerCase();
    if (host === pattern) return true;
    if (allowWildcard && pattern.startsWith("*.")) {
      const suffix = pattern.slice(1);                 // ".rest"
      if (!host.endsWith(suffix)) return false;
      const label = host.slice(0, host.length - suffix.length);
      return label.length > 0 && label.indexOf(".") === -1;   // wildcard = one label
    }
    return false;
  };
  class X509Certificate {
    constructor(input) {
      this._input = toBuf(input);
      const p = AN.x509parse(this._input);
      this.subject = p.subject || undefined;
      this.issuer = p.issuer;
      this.validFrom = p.validFrom;
      this.validTo = p.validTo;
      this.validFromDate = p.validFrom ? new Date(p.validFrom) : undefined;
      this.validToDate = p.validTo ? new Date(p.validTo) : undefined;
      this.serialNumber = p.serialNumber;
      this.fingerprint = p.fingerprint;
      this.fingerprint256 = p.fingerprint256;
      this.fingerprint512 = p.fingerprint512;
      this.subjectAltName = p.subjectAltName;
      this.infoAccess = p.infoAccess;
      this.keyUsage = p.keyUsage;
      this.ca = !!p.ca;
      this._publicKeyPem = p.publicKey;
      this.modulus = p.modulus;
      this.bits = p.bits;
      this._raw = p.raw ? Buffer.from(p.raw) : this._input;
    }
    get raw() { return Buffer.from(this._raw); }
    get publicKey() { return this._publicKeyPem ? C.createPublicKey(this._publicKeyPem) : undefined; }
    // Parsed SAN DNS entries (patterns), lowercase.
    _sanDns() {
      if (!this.subjectAltName) return [];
      return this.subjectAltName.split(", ").filter(s => s.startsWith("DNS:")).map(s => s.slice(4));
    }
    _sanEmail() {
      if (!this.subjectAltName) return [];
      return this.subjectAltName.split(", ").filter(s => s.startsWith("email:")).map(s => s.slice(6));
    }
    _cn() {
      const m = /CN=([^\n]+)/.exec(this.subject || "");
      return m ? m[1] : undefined;
    }
    checkHost(host, options) {
      const allowWildcard = !(options && options.wildcards === false);
      const dns = this._sanDns();
      for (const pattern of dns) if (wildcardMatch(host, pattern, allowWildcard)) return pattern;
      // Only fall back to the subject CN when there is no DNS SAN.
      if (dns.length === 0 && !(options && options.subject === "never")) { const cn = this._cn(); if (cn && cn.toLowerCase() === String(host).toLowerCase()) return cn; }
      return undefined;
    }
    checkEmail(email, options) {
      const emails = this._sanEmail();
      for (const e of emails) if (e.toLowerCase() === String(email).toLowerCase()) return email;
      if (emails.length === 0) { const m = /emailAddress=([^\n]+)/.exec(this.subject || ""); if (m && m[1].toLowerCase() === String(email).toLowerCase()) return email; }
      return undefined;
    }
    checkIP(ip) {
      if (!this.subjectAltName) return undefined;
      const ips = this.subjectAltName.split(", ").filter(s => s.startsWith("IP Address:")).map(s => s.slice(11));
      for (const e of ips) if (e === ip) return e;
      return undefined;
    }
    checkIssued(otherCert) {
      if (!(otherCert instanceof X509Certificate)) throw new TypeError("issuer must be a X509Certificate");
      return !!AN.x509checkIssued(this._raw, otherCert._raw);
    }
    toString() { return this._input.toString("utf8").includes("BEGIN") ? this._input.toString("utf8") : this._raw.toString("base64"); }
    toJSON() { return this.toString(); }
    toLegacyObject() { return { subject: this.subject, issuer: this.issuer, valid_from: this.validFrom, valid_to: this.validTo, fingerprint: this.fingerprint, fingerprint256: this.fingerprint256, serialNumber: this.serialNumber, subjectaltname: this.subjectAltName, modulus: this.modulus, bits: this.bits }; }
    get [Symbol.toStringTag]() { return "X509Certificate"; }
  }
  C.X509Certificate = X509Certificate;

})();
)JS";

}  // namespace mbun::jsc::builtins::detail
