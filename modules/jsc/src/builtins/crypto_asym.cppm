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

  // ---- sign / verify (one-shot + streaming) ----
  const doSign = (algo, data, key) => {
    const r = resolveKey(key);
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
  class Sign {
    constructor(algorithm) { this._algo = algorithm; this._chunks = []; }
    update(data, inputEnc) { this._chunks.push(toBuf(data, inputEnc)); return this; }
    write(data, inputEnc) { this.update(data, inputEnc); return true; }
    end(data, inputEnc) { if (data != null) this.update(data, inputEnc); return this; }
    sign(key, outputEnc) {
      const out = doSign(this._algo, Buffer.concat(this._chunks), key);
      return outputEnc ? out.toString(outputEnc) : out;
    }
  }
  class Verify {
    constructor(algorithm) { this._algo = algorithm; this._chunks = []; }
    update(data, inputEnc) { this._chunks.push(toBuf(data, inputEnc)); return this; }
    write(data, inputEnc) { this.update(data, inputEnc); return true; }
    end(data, inputEnc) { if (data != null) this.update(data, inputEnc); return this; }
    verify(key, signature, sigEnc) {
      const sig = typeof signature === "string" ? Buffer.from(signature, sigEnc || "hex") : toBuf(signature);
      return doVerify(this._algo, Buffer.concat(this._chunks), key, sig);
    }
  }
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
  class KeyObject {
    constructor(kind, material, passphrase) { this._kind = kind; this._km = material; this._pass = passphrase || ""; }
    get type() { return this._kind; }
    get [Symbol.toStringTag]() { return "KeyObject"; }
    get asymmetricKeyType() {
      if (this._kind === "secret") return undefined;
      try { return AN.keyType(this._km, this._pass, this._kind === "public").type; } catch { return undefined; }
    }
    get asymmetricKeyDetails() {
      if (this._kind === "secret") return undefined;
      try { const t = AN.keyType(this._km, this._pass, this._kind === "public"); const d = {}; if (t.modulusLength != null) d.modulusLength = t.modulusLength; if (t.namedCurve != null) d.namedCurve = t.namedCurve; return d; } catch { return {}; }
    }
    get symmetricKeySize() { return this._kind === "secret" ? toBuf(this._km).length : undefined; }
    export(options) {
      options = options || {};
      // format:"jwk" is NOT a PEM/DER encoding — it returns a plain JWK object
      // (node lib/internal/crypto/keys.js). It must be intercepted before the
      // native encoder, whose `pem = format != "der"` would emit PEM for it.
      if (options.format === "jwk") {
        if (this._kind === "secret") {
          return { kty: "oct", k: Buffer.from(toBuf(this._km)).toString("base64url") };
        }
        return jwkFromKey(this._km, this._pass, this._kind === "public");
      }
      if (this._kind === "secret") return Buffer.from(toBuf(this._km));
      const type = options.type || (this._kind === "public" ? "spki" : "pkcs8");
      const format = options.format || "pem";
      const cipher = options.cipher || "";
      const outPass = options.passphrase != null ? (typeof options.passphrase === "string" ? options.passphrase : toBuf(options.passphrase).toString("latin1")) : "";
      return AN.keyExport(this._km, this._pass, this._kind === "public", type, format, cipher, outPass);
    }
    equals(other) {
      if (!(other instanceof KeyObject) || other._kind !== this._kind) return false;
      try { return Buffer.compare(toBuf(this.export({ format: this._kind === "secret" ? undefined : "der", type: this._kind === "public" ? "spki" : "pkcs8" })), toBuf(other.export({ format: "der", type: this._kind === "public" ? "spki" : "pkcs8" }))) === 0; } catch { return false; }
    }
  }
  const makeKeyObject = (kind, key) => {
    if (key instanceof KeyObject) return key;
    // { key: <JWK object>, format: "jwk" } → materialize as DER up front so the
    // rest of the pipeline sees ordinary key material (node keys.js).
    if (key != null && typeof key === "object" && key.format === "jwk" && key.key != null) {
      return new KeyObject(kind, jwkToDer(key.key, kind === "private"), "");
    }
    const r = resolveKey(key);
    return new KeyObject(kind, r.data, r.passphrase);
  };
  C.KeyObject = KeyObject;
  C.createPrivateKey = (key) => {
    const ko = makeKeyObject("private", key);
    // node/bun validate the material at construction: non-key input throws
    // ERR_OSSL_NO_START_LINE (bun 1.4.0 verified). jsonwebtoken's sign()
    // relies on that throw to fall back to createSecretKey for HS* secrets.
    try {
      AN.keyType(ko._km, ko._pass, false);
    } catch (e) {
      const head = typeof ko._km === "string"
        ? ko._km.slice(0, 64)
        : Buffer.from(toBuf(ko._km).slice(0, 64)).toString("latin1");
      if (head.includes("-----BEGIN")) throw e; // PEM present: surface the native parse/passphrase error
      const err = new Error("error:0900006e:PEM routines:OPENSSL_internal:NO_START_LINE");
      err.code = "ERR_OSSL_NO_START_LINE";
      throw err;
    }
    return ko;
  };
  C.createPublicKey = (key) => {
    // Deriving a public key from a private one: re-export public SPKI so the
    // KeyObject material is public-only (node semantics).
    if (key instanceof KeyObject && key._kind === "private") {
      const pem = AN.keyExport(key._km, key._pass, true, "spki", "pem", "", "");
      return new KeyObject("public", pem, "");
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
        if (head.includes("-----BEGIN")) throw ePub;
        const err = new Error("error:0900006e:PEM routines:OPENSSL_internal:NO_START_LINE");
        err.code = "ERR_OSSL_NO_START_LINE";
        throw err;
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
    return new KeyObject("secret", toBuf(key, encoding), "");
  };

  // ---- generateKeyPair / generateKeyPairSync ----
  const genKeyPair = (type, options) => {
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
    if (wantPubObj) publicKey = new KeyObject("public", publicKey, "");
    else if (pubJwk) publicKey = jwkFromKey(publicKey, "", true);
    else if (pubFmt === "der") publicKey = Buffer.from(publicKey);
    if (wantPrivObj) privateKey = new KeyObject("private", privateKey, pass);
    else if (privJwk) privateKey = jwkFromKey(privateKey, "", false);
    else if (privFmt === "der") privateKey = Buffer.from(privateKey);
    return { publicKey, privateKey };
  };
  C.generateKeyPairSync = (type, options) => genKeyPair(type, options);
  C.generateKeyPair = (type, options, callback) => {
    const cb = typeof options === "function" ? options : callback;
    const opts = typeof options === "function" ? undefined : options;
    queueMicrotask(() => {
      try { const { publicKey, privateKey } = genKeyPair(type, opts); cb(null, publicKey, privateKey); }
      catch (e) { cb(e); }
    });
  };
  C.generateKeyPair[Symbol.for("nodejs.util.promisify.custom")] =
    (type, options) => new Promise((resolve, reject) => {
      try { resolve(genKeyPair(type, options)); } catch (e) { reject(e); }
    });

  // ---- createCipheriv / createDecipheriv ----
  // Cipher is a real stream.Transform subclass (mirrors the Hash pattern above:
  // Reflect.construct(Transform, [], Cipher) so instanceof Transform holds and it
  // can be piped/end()/read()). update/final buffer and run the EVP one-shot once.
  function Cipher(isEncrypt, algorithm, key, iv, options) {
    if (typeof algorithm !== "string") throw new TypeError("The \"cipher\" argument must be of type string.");
    if (key == null) throw new TypeError("The \"key\" argument must be of type BufferSource. Received " + key);
    options = options || {};
    const keyBuf = toBuf(key, options.encoding);
    const ivBuf = iv == null ? Buffer.alloc(0) : toBuf(iv);
    // Synchronous key/iv-length + authTagLength validation (node throws at create).
    const info = C.getCipherInfo(algorithm);
    if (info) {
      if (keyBuf.length !== info.keyLength) throw new RangeError("Invalid key length");
      if (info.mode === "ecb") { if (ivBuf.length !== 0) throw new Error("Invalid IV length"); }
      else if (info.mode === "gcm") { if (ivBuf.length < 1) throw new Error("Invalid IV length"); }
      else if (ivBuf.length !== 16) throw new Error("Invalid IV length");
    }
    if (options.authTagLength !== undefined &&
        (typeof options.authTagLength !== "number" || !Number.isInteger(options.authTagLength) || options.authTagLength < 0)) {
      throw new TypeError("The property 'options.authTagLength' is invalid. Received " + String(options.authTagLength));
    }
    const self = Transform ? Reflect.construct(Transform, [], Cipher) : Object.create(Cipher.prototype);
    self._algo = algorithm.toLowerCase();
    self._enc = isEncrypt;
    self._key = keyBuf;
    self._iv = ivBuf;
    self._auth = info && info.mode === "gcm";
    self._chunks = [];
    self._aad = null;
    self._tag = null;              // encrypt: output tag; decrypt: expected tag
    self._tagLen = options.authTagLength || 16;
    self._done = false;
    return self;
  }
  if (Transform) { Object.setPrototypeOf(Cipher.prototype, Transform.prototype); Object.setPrototypeOf(Cipher, Transform); }
  Cipher.prototype.setAAD = function (buffer) { this._aad = toBuf(buffer); return this; };
  Cipher.prototype.setAutoPadding = function () { return this; };   // padding always on (DEFERRED: disable)
  Cipher.prototype.getAuthTag = function () { if (!this._auth || !this._enc || this._tag == null) throw new Error("Unsupported state or unable to authenticate data"); return this._tag; };
  Cipher.prototype.setAuthTag = function (tag) { this._tag = toBuf(tag); return this; };
  Cipher.prototype.update = function (data, inputEnc, outputEnc) {
    if (this._done) throw new Error("Trying to add data in an unsupported state");
    this._chunks.push(toBuf(data, inputEnc));
    return outputEnc ? "" : Buffer.alloc(0);
  };
  Cipher.prototype._run = function () {
    const data = Buffer.concat(this._chunks);
    const res = AN.cipher(this._algo, this._key, this._iv, data, this._aad,
      this._enc, this._enc ? null : this._tag, this._tagLen);
    if (this._enc && res.tag != null) this._tag = Buffer.from(res.tag);
    return Buffer.from(res.data);
  };
  Cipher.prototype.final = function (outputEnc) {
    if (this._done) throw new Error("Trying to add data in an unsupported state");
    this._done = true;
    const out = this._run();
    return outputEnc ? out.toString(outputEnc) : out;
  };
  Cipher.prototype._transform = function (chunk, e, cb) { this._chunks.push(toBuf(chunk)); cb(); };
  Cipher.prototype._flush = function (cb) { try { this._done = true; this.push(this._run()); cb(); } catch (err) { cb(err); } };
  C.Cipheriv = Cipher; C.Decipheriv = Cipher;
  C.createCipheriv = (algorithm, key, iv, options) => new Cipher(true, algorithm, key, iv, options);
  C.createDecipheriv = (algorithm, key, iv, options) => new Cipher(false, algorithm, key, iv, options);

  // ---- ECDH ----
  const CURVE_NIDS = { secp256k1: "secp256k1", prime256v1: "prime256v1", secp384r1: "secp384r1", secp521r1: "secp521r1" };
  class ECDH {
    constructor(curve) {
      if (typeof curve !== "string" || !AN.ecValidCurve(curve)) {
        throw new TypeError("Invalid EC curve name: " + curve);
      }
      this._curve = curve; this._priv = null; this._pub = null;
    }
    generateKeys(encoding, format) {
      const r = AN.ecdhGenerate(this._curve);
      this._priv = Buffer.from(r.privateKey);
      this._pub = Buffer.from(r.publicKey);
      return this.getPublicKey(encoding, format);
    }
    computeSecret(otherPublic, inputEnc, outputEnc) {
      const pub = typeof otherPublic === "string" ? Buffer.from(otherPublic, inputEnc) : toBuf(otherPublic);
      const sec = Buffer.from(AN.ecdhComputeSecret(this._curve, this._priv, pub));
      return outputEnc ? sec.toString(outputEnc) : sec;
    }
    getPublicKey(encoding, format) {
      let pub = this._pub;
      if (format === "compressed") pub = Buffer.from(AN.ecdhConvertKey(this._curve, this._pub, true));
      else if (format === "hybrid" || format === "uncompressed" || format == null) pub = Buffer.from(AN.ecdhConvertKey(this._curve, this._pub, false));
      return encoding ? pub.toString(encoding) : Buffer.from(pub);
    }
    getPrivateKey(encoding) { return encoding ? this._priv.toString(encoding) : Buffer.from(this._priv); }
    setPrivateKey(key, encoding) { this._priv = typeof key === "string" ? Buffer.from(key, encoding) : toBuf(key); this._pub = Buffer.from(AN.ecdhPublicFromPrivate(this._curve, this._priv)); return this; }
    setPublicKey(key, encoding) { this._pub = typeof key === "string" ? Buffer.from(key, encoding) : toBuf(key); return this; }
  }
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

  // getCipherInfo (best-effort shape for common AES modes).
  C.getCipherInfo = (name) => {
    const m = /^aes-(128|192|256)-(cbc|gcm|ctr|ecb)$/.exec(String(name).toLowerCase());
    if (!m) return undefined;
    const keyLength = { "128": 16, "192": 24, "256": 32 }[m[1]];
    const mode = m[2];
    const ivLength = mode === "ecb" ? undefined : (mode === "gcm" ? 12 : 16);
    return { name: `aes-${m[1]}-${mode}`, nid: 0, blockSize: mode === "gcm" || mode === "ctr" ? 1 : 16, ivLength, keyLength, mode };
  };
})();
)JS";

}  // namespace mbun::jsc::builtins::detail
