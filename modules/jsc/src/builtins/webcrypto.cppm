// WebCrypto builtin partition: the SubtleCrypto object model + algorithm policy
// over the OpenSSL EVP primitives already exposed for node:crypto
// (__mbunCryptoAsymNative, runtime/crypto_asym.inc) plus the HMAC fast path
// (__mbunWebCryptoNative, runtime/webcrypto.inc).
//
// ref: bun src/jsc/bindings/webcrypto/{SubtleCrypto.idl,JSSubtleCrypto.cpp,
//      CryptoAlgorithmRegistry.cpp,CryptoAlgorithm{HMAC,RSA_OAEP,ECDSA,AES_GCM,
//      RSASSA_PKCS1_v1_5,RSA_PSS,AES_CBC,AES_CTR,PBKDF2,HKDF,ECDH}.cpp,
//      CryptoKey{HMAC,RSA,EC,AES}.cpp} and the W3C WebCryptoAPI spec's
//      "normalize an algorithm" + per-operation tables.
//
// Key material never sits in a JS-visible slot: every CryptoKey is an opaque
// object whose bytes live in the `keyMetadata` WeakMap. Asymmetric keys carry
// DER (SPKI for public / PKCS8 for private) so every AN.* entry point accepts
// them as-is; symmetric keys carry their raw octets.
//
// DEFERRED — these throw NotSupportedError rather than compute a wrong answer:
//   AES-KW: EVP wrap ciphers need EVP_CIPHER_CTX_FLAG_WRAP_ALLOW, which
//     AN.cipher does not set (runtime/crypto_asym.inc, node_asym_cipher_host).
//   AES-CTR with counter length != 128: EVP always increments the full 128-bit
//     block, but the spec wraps only the low `length` bits.
//   RSA publicExponent other than 65537: AN.generateKeyPair has no pubexp arg.
export module mbun.jsc.js_builtins:webcrypto;

import std;

export namespace mbun::jsc::builtins::detail {

inline constexpr std::string_view kWebCryptoJS = R"JS(  // ---- WebCrypto ----
  {
    const WC = G.__mbunWebCryptoNative;
    const NativeCryptoKey = G.CryptoKey;
    // node brands "cannot be constructed / wrong receiver" with these two codes
    // (ERR_ILLEGAL_CONSTRUCTOR, ERR_INVALID_THIS); the corpus matches on them.
    const illegalConstructor = () => {
      const e = new TypeError("Illegal constructor");
      e.code = "ERR_ILLEGAL_CONSTRUCTOR";
      return e;
    };
    // webidl.requiredArguments -> ERR_MISSING_ARGS.
    const missingArgs = (need, got) => {
      const e = new TypeError(need + " argument" + (need > 1 ? "s" : "") +
        " required, but only " + got + " present.");
      e.code = "ERR_MISSING_ARGS";
      return e;
    };
    const invalidThis = (what) => {
      const e = new TypeError('Value of "this" must be of type ' + what);
      e.code = "ERR_INVALID_THIS";
      return e;
    };
    function CryptoKey() { throw illegalConstructor(); }
    CryptoKey.prototype = NativeCryptoKey.prototype;
    const keyMetadata = new WeakMap();
    const freeze = Object.freeze;
    const preventExtensions = Object.preventExtensions;
    const defineProperty = Object.defineProperty;
    // node's kCanonicalUsageOrder (lib/internal/crypto/util.js) — the order a
    // CryptoKey's `usages` array is materialised in, which the corpus
    // deepStrictEquals against.
    const usageNames = ["encrypt", "decrypt", "sign", "verify",
      "deriveKey", "deriveBits", "wrapKey", "unwrapKey"];
    const usageSet = new Set(usageNames);
    const metadataFor = (key) => {
      const metadata = keyMetadata.get(key);
      if (!metadata) throw invalidThis("CryptoKey");
      return metadata;
    };
    defineProperty(CryptoKey.prototype, "constructor", {
      configurable: false, enumerable: false, writable: false, value: CryptoKey,
    });
    defineProperty(CryptoKey.prototype, "type", {
      configurable: false, enumerable: true, get() { return metadataFor(this).type; },
    });
    defineProperty(CryptoKey.prototype, "extractable", {
      configurable: false, enumerable: true, get() { return metadataFor(this).extractable; },
    });
    defineProperty(CryptoKey.prototype, "algorithm", {
      configurable: false, enumerable: true, get() { return metadataFor(this).algorithm; },
    });
    defineProperty(CryptoKey.prototype, "usages", {
      configurable: false, enumerable: true, get() { return metadataFor(this).usages; },
    });
    defineProperty(CryptoKey.prototype, Symbol.toStringTag, {
      configurable: false, value: "CryptoKey",
    });
    freeze(CryptoKey.prototype);
    // node instantiates InternalCryptoKey, a subclass whose prototype chains to
    // CryptoKey.prototype and whose `constructor` still reports CryptoKey.
    // test-webcrypto-cryptokey-brand-check walks exactly that chain.
    const InternalCryptoKey = function InternalCryptoKey() {};
    InternalCryptoKey.prototype = Object.create(CryptoKey.prototype);
    defineProperty(InternalCryptoKey.prototype, "constructor", {
      configurable: false, enumerable: false, writable: false, value: CryptoKey,
    });
    freeze(InternalCryptoKey.prototype);
    defineProperty(G, "CryptoKey", {
      configurable: false, enumerable: false, writable: false, value: CryptoKey,
    });

    // engine.inc installs the EVP bridge before any builtin runs, but it is read
    // lazily so this partition carries no load-order dependency on it.
    const AN = () => {
      const n = G.__mbunCryptoAsymNative;
      if (!n) throw new G.DOMException("Crypto backend unavailable", "OperationError");
      return n;
    };
    const nodeCrypto = () => {
      const m = G.__mbunNativeModules && G.__mbunNativeModules["crypto"];
      if (!m) throw new G.DOMException("Crypto backend unavailable", "OperationError");
      return m;
    };

    const domError = (message, name) => new G.DOMException(message, name);
    // A required WebIDL dictionary member that was not supplied: node's
    // createDictionaryConverter raises ERR_MISSING_OPTION ("%s is required").
    const requiredMember = (value, name) => {
      if (value === undefined) {
        const err = new TypeError(name + " is required");
        err.code = "ERR_MISSING_OPTION";
        throw err;
      }
      return value;
    };
    const notSupported = (message) => domError(message, "NotSupportedError");
    const dataError = (message) => domError(message, "DataError");
    const operationError = (message) => domError(message, "OperationError");

    // ---- bytes ----
    // WebCrypto caps BufferSource inputs at 2^31-1 bytes; anything larger is an
    // OperationError (ref: bun/WebKit CryptoAlgorithm size guards). Checked before
    // the copy so an oversized buffer never gets duplicated.
    const MAX_BUFFER_BYTES = 0x7fffffff;
    const copyBytes = (value) => {
      if (value instanceof ArrayBuffer) {
        if (value.byteLength > MAX_BUFFER_BYTES) throw operationError("Data is too large");
        // A detached (transferred) buffer reads as zero bytes in node rather
        // than throwing — the algorithm then fails with its own OperationError.
        if (value.byteLength === 0) return new Uint8Array(0);
        return new Uint8Array(value.slice(0));
      }
      if (ArrayBuffer.isView(value)) {
        if (value.byteLength > MAX_BUFFER_BYTES) throw operationError("Data is too large");
        if (value.byteLength === 0) return new Uint8Array(0);
        return new Uint8Array(value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength));
      }
      const err = new TypeError(
        'The "data" argument must be an instance of ArrayBuffer or ArrayBufferView.');
      err.code = "ERR_INVALID_ARG_TYPE";
      throw err;
    };
    const arrayBuffer = (value) => {
      const bytes = copyBytes(value);
      return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
    };
    const concatBytes = (a, b) => {
      const out = new Uint8Array(a.length + b.length);
      out.set(a, 0); out.set(b, a.length);
      return out;
    };
    const randomBytes = (n) => G.crypto.getRandomValues(new Uint8Array(n));
    // RFC 7515 §2 base64url, unpadded.
    const b64u = (bytes) => {
      let binary = "";
      for (let i = 0; i < bytes.length; i++) binary += String.fromCharCode(bytes[i]);
      return G.btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
    };
    const unb64u = (text) => {
      const binary = G.atob(String(text).replace(/-/g, "+").replace(/_/g, "/"));
      const out = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i++) out[i] = binary.charCodeAt(i);
      return out;
    };

    // ---- algorithm normalization ----
    const normalizeAlgorithm = (algorithm) =>
      typeof algorithm === "string" ? { name: algorithm } : Object.assign({}, algorithm || {});
    // Registered WebCrypto digest names, matched case-insensitively (node's
    // normalizeAlgorithm lower-cases the name before the table lookup) but with
    // NO extra aliases: "sha256" and "SHA-224" are not WebCrypto algorithms.
    // ref: node lib/internal/crypto/hashnames.js.
    const hashNames = { "SHA-1": "SHA-1", "SHA-256": "SHA-256", "SHA-384": "SHA-384",
      "SHA-512": "SHA-512", "SHA3-256": "SHA3-256", "SHA3-384": "SHA3-384",
      "SHA3-512": "SHA3-512" };
    const normalizeHash = (hash) => {
      const value = normalizeAlgorithm(hash);
      const name = hashNames[String(value.name || "").toUpperCase()];
      if (!name) throw notSupported("Unrecognized algorithm name");
      return { name };
    };
    // `hash` as a required dictionary member (HmacKeyGenParams, EcdsaParams,
    // RsaHashedKeyGenParams, HkdfParams, Pbkdf2Params, …) — absent is
    // ERR_MISSING_OPTION, not "Unrecognized algorithm name".
    const memberHash = (alg) => normalizeHash(requiredMember(alg.hash, "algorithm.hash"));
    // WebCrypto hash spelling → OpenSSL/mbun digest name ("SHA-256" → "sha256";
    // "SHA3-256" keeps the hyphen so OpenSSL/EVP recognizes it as "sha3-256").
    const mdName = (hash) => hash.name.startsWith("SHA3-") ? hash.name.toLowerCase()
      : hash.name.replace("-", "").toLowerCase();
    // block size (bits) = HMAC block / rate. SHA-3 uses its sponge rate.
    const hashBlockBits = { "SHA-1": 512, "SHA-224": 512, "SHA-256": 512, "SHA-384": 1024, "SHA-512": 1024,
      "SHA3-256": 1088, "SHA3-384": 832, "SHA3-512": 576 };
    const hashOutBytes = { "SHA-1": 20, "SHA-224": 28, "SHA-256": 32, "SHA-384": 48, "SHA-512": 64,
      "SHA3-256": 32, "SHA3-384": 48, "SHA3-512": 64 };

    const algNames = ["RSASSA-PKCS1-v1_5", "RSA-PSS", "RSA-OAEP", "ECDSA", "ECDH", "AES-CTR",
      "AES-CBC", "AES-GCM", "AES-OCB", "AES-KW", "ChaCha20-Poly1305", "HMAC", "PBKDF2", "HKDF",
      "Ed25519", "Ed448", "X25519", "X448", "KMAC128", "KMAC256"];
    const algByUpper = new Map(algNames.map((n) => [n.toUpperCase(), n]));
    const normalizeAlg = (algorithm) => {
      const value = normalizeAlgorithm(algorithm);
      const name = algByUpper.get(String(value.name).toUpperCase());
      if (!name) throw notSupported("Unrecognized algorithm name");
      value.name = name;
      return value;
    };

    // ---- usages ----
    const normalizeUsages = (keyUsages) => {
      if (keyUsages == null || typeof keyUsages[Symbol.iterator] !== "function") {
        throw new TypeError("keyUsages must be a sequence");
      }
      const out = [];
      for (const usage of keyUsages) {
        if (typeof usage !== "string" || !usageSet.has(usage)) {
          throw new TypeError("keyUsages contains an invalid CryptoKeyUsage");
        }
        if (!out.includes(usage)) out.push(usage);
      }
      return out;
    };
    // Stable ordering (the pre-existing HMAC path froze usages this way).
    const sortUsages = (list) => freeze(usageNames.filter((n) => list.includes(n)));
    // node phrases every "usage not legal for this algorithm" rejection as
    // "Unsupported key usage for <what> key" (SyntaxError) — ref
    // lib/internal/crypto/{aes,mac,rsa,ec,cfrg,chacha20_poly1305}.js.
    const restrictUsages = (usages, allowed, what) => {
      for (const usage of usages) {
        if (!allowed.includes(usage)) {
          throw domError("Unsupported key usage for " + what + " key", "SyntaxError");
        }
      }
    };
    // node: "Usages cannot be empty when creating a key." (generateKey/deriveKey)
    // vs "Usages cannot be empty when importing a <type> key." (importKey).
    const requireUsages = (usages) => {
      if (usages.length === 0) {
        throw domError("Usages cannot be empty when creating a key.", "SyntaxError");
      }
    };
    const requireImportUsages = (usages, type) => {
      if ((type === "secret" || type === "private") && usages.length === 0) {
        throw domError("Usages cannot be empty when importing a " + type + " key.", "SyntaxError");
      }
    };
    // WebIDL enum KeyFormat. node 24 split the single "raw" spelling into
    // per-key-kind variants; an unlisted value is a plain TypeError from the
    // enum converter. ref: node lib/internal/crypto/webidl.js converters.KeyFormat.
    const KEY_FORMATS = ["raw", "raw-public", "raw-seed", "raw-secret", "raw-private",
      "spki", "pkcs8", "jwk"];
    const keyFormat = (format) => {
      const value = `${format}`;
      if (!KEY_FORMATS.includes(value)) {
        const e = new TypeError("The provided value '" + value +
          "' is not a valid enum value of type KeyFormat.");
        e.code = "ERR_INVALID_ARG_VALUE";
        throw e;
      }
      return value;
    };
    const intersectUsages = (usages, allowed) => sortUsages(usages.filter((u) => allowed.includes(u)));
    // JWK "key_ops" must be duplicate-free and cover every requested usage.
    // ref: node lib/internal/crypto/util.js validateKeyOps.
    const validateKeyOps = (keyOps, usages) => {
      if (keyOps === undefined) return;
      if (!Array.isArray(keyOps)) throw dataError("Invalid keyData");
      const seen = new Set();
      for (const op of keyOps) {
        if (!usageSet.has(op)) continue;   // unknown ops are skipped, not rejected
        if (seen.has(op)) throw dataError("Duplicate key operation");
        seen.add(op);
      }
      for (const usage of usages) {
        if (!seen.has(usage)) throw dataError("Key operations and usage mismatch");
      }
    };

    const SIGN_ALGS = new Set(["RSASSA-PKCS1-v1_5", "RSA-PSS", "ECDSA", "Ed25519", "Ed448",
      "HMAC", "KMAC128", "KMAC256"]);
    // NIST SP 800-185 KMAC. Like AES-OCB/ChaCha20-Poly1305 its raw secret only
    // travels under the explicit "raw-secret" format.
    const KMAC_ALGS = new Set(["KMAC128", "KMAC256"]);
    const DERIVE_ALGS = new Set(["ECDH", "X25519", "X448", "PBKDF2", "HKDF"]);
    const AES_ALGS = new Set(["AES-CTR", "AES-CBC", "AES-GCM", "AES-OCB", "AES-KW"]);
    // AEAD ciphers whose ciphertext carries an authentication tag (WebCrypto
    // appends it to the ciphertext). ref: node lib/internal/crypto/webidl.js
    // AeadParams.
    const AEAD_ALGS = new Set(["AES-GCM", "AES-OCB", "ChaCha20-Poly1305"]);
    // Symmetric ciphers whose raw import/export only accepts the explicit
    // "raw-secret" format (node 24+ split "raw" per key kind).
    const RAW_SECRET_ONLY = new Set(["AES-OCB", "ChaCha20-Poly1305", "KMAC128", "KMAC256"]);
    const RSA_ALGS = new Set(["RSASSA-PKCS1-v1_5", "RSA-PSS", "RSA-OAEP"]);
    const EC_ALGS = new Set(["ECDSA", "ECDH"]);
    const OKP_ALGS = new Set(["Ed25519", "Ed448", "X25519", "X448"]);
    // Legal usages per algorithm/key-type (spec operation tables).
    const allowedUsagesFor = (name, type) => {
      if (name === "HMAC" || KMAC_ALGS.has(name)) return ["sign", "verify"];
      if (SIGN_ALGS.has(name)) return type === "public" ? ["verify"] : ["sign"];
      if (name === "RSA-OAEP") {
        return type === "public" ? ["encrypt", "wrapKey"] : ["decrypt", "unwrapKey"];
      }
      if (name === "AES-KW") return ["wrapKey", "unwrapKey"];
      if (AES_ALGS.has(name) || name === "ChaCha20-Poly1305") {
        return ["encrypt", "decrypt", "wrapKey", "unwrapKey"];
      }
      if (DERIVE_ALGS.has(name)) return type === "public" ? [] : ["deriveKey", "deriveBits"];
      return [];
    };

    // ---- curves ----
    const curveToGroup = { "P-256": "prime256v1", "P-384": "secp384r1", "P-521": "secp521r1" };
    const groupToCurve = { prime256v1: "P-256", secp384r1: "P-384", secp521r1: "P-521" };
    const curveBytes = { "P-256": 32, "P-384": 48, "P-521": 66 };
    const normalizeCurve = (namedCurve) => {
      requiredMember(namedCurve, "algorithm.namedCurve");
      const name = String(namedCurve);
      if (!curveToGroup[name]) throw notSupported("Unrecognized namedCurve");
      return name;
    };

    // ---- CryptoKey construction ----
    // `extra` carries the opaque material: { material } (DER) or { secret } (raw).
    const makeKey = (type, algorithm, extractable, usages, extra) => {
      const key = Object.create(InternalCryptoKey.prototype);
      const metadata = Object.assign({
        type, algorithm: freeze(algorithm), extractable: !!extractable, usages: sortUsages(usages),
      }, extra);
      keyMetadata.set(key, freeze(metadata));
      preventExtensions(key);
      return key;
    };
    // Algorithms registered for each operation (spec's per-operation table).
    // Asking for an operation an algorithm does not define is NotSupportedError,
    // not InvalidAccessError — verified against bun 1.4.0.
    const CIPHER_ALGS = new Set(["RSA-OAEP", "AES-CTR", "AES-CBC", "AES-GCM", "AES-OCB",
      "ChaCha20-Poly1305"]);
    const WRAP_ALGS = new Set(["RSA-OAEP", "AES-CTR", "AES-CBC", "AES-GCM", "AES-OCB",
      "ChaCha20-Poly1305", "AES-KW"]);
    const opAlgs = {
      sign: SIGN_ALGS, verify: SIGN_ALGS,
      encrypt: CIPHER_ALGS, decrypt: CIPHER_ALGS,
      deriveBits: DERIVE_ALGS, deriveKey: DERIVE_ALGS,
      wrapKey: WRAP_ALGS, unwrapKey: WRAP_ALGS,
    };
    // A key of `name` usable for `usage`. Mirrors node's layering
    // (lib/internal/crypto/webcrypto.js signVerify/encryptImpl/deriveBitsImpl):
    //   not a CryptoKey at all       → TypeError (the IDL converter's brand check)
    //   algorithm not in the op table → NotSupportedError "Unrecognized algorithm name"
    //   key algorithm ≠ requested     → InvalidAccessError "Key algorithm mismatch"
    //   usage missing                 → InvalidAccessError "Unable to use this key to <op>"
    //                                   ("baseKey does not have <op> usage" for derive*)
    // derive* checks the usage BEFORE the algorithm match; the other operations
    // check the algorithm first (node's order, asserted by the corpus).
    const requireKey = (key, name, usage) => {
      const metadata = keyMetadata.get(key);
      // Object.create(CryptoKey.prototype) has the prototype but no material:
      // the IDL converter rejects it with a TypeError.
      if (!metadata) throw invalidThis("CryptoKey");
      const supported = opAlgs[usage];
      if (supported && !supported.has(name)) throw notSupported("Unrecognized algorithm name");
      const isDerive = usage === "deriveBits" || usage === "deriveKey";
      const checkUsage = () => {
        if (metadata.usages.includes(usage)) return;
        throw domError(isDerive ? "baseKey does not have " + usage + " usage"
                                : "Unable to use this key to " + usage, "InvalidAccessError");
      };
      const checkAlgorithm = () => {
        if (metadata.algorithm.name !== name) {
          throw domError("Key algorithm mismatch", "InvalidAccessError");
        }
      };
      if (isDerive) { checkUsage(); checkAlgorithm(); }
      else { checkAlgorithm(); checkUsage(); }
      return metadata;
    };

    // Cross-check imported DER against the requested algorithm; without this an
    // RSA SPKI would happily import as an ECDSA key.
    const checkMaterial = (material, name, alg) => {
      let info;
      // node: a DER blob the key parser rejects is "Invalid keyData"; a blob that
      // parses into the wrong key family is "Invalid key type" (both DataError).
      try { info = AN().keyType(material, "", true); }
      catch (e) { throw dataError("Invalid keyData"); }
      const type = info.type;
      if (RSA_ALGS.has(name)) {
        if (type !== "rsa" && type !== "rsa-pss") throw dataError("Invalid key type");
        return { modulusLength: info.modulusLength };
      }
      if (EC_ALGS.has(name)) {
        if (type !== "ec") throw dataError("Invalid key type");
        const curve = groupToCurve[info.namedCurve] || info.namedCurve;
        if (alg.namedCurve != null && normalizeCurve(alg.namedCurve) !== curve) {
          throw dataError("Named curve mismatch");
        }
        return { namedCurve: curve };
      }
      if (OKP_ALGS.has(name)) {
        if (type !== name.toLowerCase()) throw dataError("Invalid key type");
        return {};
      }
      throw notSupported("Unsupported algorithm for this key format");
    };

    // ---- JWK <-> DER ----
    const JWK_PARTS = ["n", "e", "d", "p", "q", "dp", "dq", "qi", "x", "y"];
    const jwkToDer = (jwk, isPrivate) => {
      const parts = { kty: jwk.kty };
      if (jwk.crv != null) parts.crv = jwk.crv;
      for (const k of JWK_PARTS) {
        if (typeof jwk[k] === "string") parts[k] = unb64u(jwk[k]);
      }
      return AN().jwkImport(parts, !!isPrivate);
    };
    const jwkFromDer = (material, isPublic) => {
      const raw = AN().jwkExport(material, "", !!isPublic);
      const out = {};
      for (const k of Object.keys(raw)) {
        const v = raw[k];
        out[k] = typeof v === "string" ? v : b64u(v);
      }
      return out;
    };
    // JWK "alg" (RFC 7518 §3.1/§4.1 + the WebCrypto modern-algos registrations).
    // ref: node lib/internal/crypto/webcrypto.js exportKeyJWK — EC/ECDH/X25519/
    // X448 emit no "alg"; EdDSA keys emit their algorithm name.
    const jwkAlgFor = (algorithm) => {
      const name = algorithm.name;
      const hash = algorithm.hash && algorithm.hash.name;
      const sha = hash ? hash.slice(4) : "";
      // The SHA-3 family has no registered JWK "alg" identifier, so RSA/HMAC keys
      // hashing with SHA-3 export no "alg" (node/bun omit it too).
      const sha3 = !!(hash && hash.startsWith("SHA3"));
      if (name === "RSASSA-PKCS1-v1_5") return sha3 ? undefined : "RS" + sha;
      if (name === "RSA-PSS") return sha3 ? undefined : "PS" + sha;
      if (name === "RSA-OAEP") return sha3 ? undefined : (hash === "SHA-1" ? "RSA-OAEP" : "RSA-OAEP-" + sha);
      if (name === "HMAC") return sha3 ? undefined : "HS" + sha;
      if (name === "AES-GCM") return "A" + algorithm.length + "GCM";
      if (name === "AES-CBC") return "A" + algorithm.length + "CBC";
      if (name === "AES-CTR") return "A" + algorithm.length + "CTR";
      if (name === "AES-OCB") return "A" + algorithm.length + "OCB";
      if (name === "KMAC128") return "K128";
      if (name === "KMAC256") return "K256";
      if (name === "AES-KW") return "A" + algorithm.length + "KW";
      if (name === "ChaCha20-Poly1305") return "C20P";
      if (name === "Ed25519" || name === "Ed448") return name;
      return undefined;
    };
    // JsonWebKey is a WebIDL dictionary, so its members serialize in
    // lexicographic order — not in the order the components were produced.
    const sortJwk = (jwk) => {
      const out = {};
      for (const k of Object.keys(jwk).sort()) out[k] = jwk[k];
      return out;
    };

    // ---- EC raw point <-> key material ----
    const ecPointFromMaterial = (material) => {
      const jwk = jwkFromDer(material, true);
      return concatBytes(new Uint8Array([0x04]), concatBytes(unb64u(jwk.x), unb64u(jwk.y)));
    };
    const ecKeyFromPoint = (point, curve) => {
      const size = curveBytes[curve];
      if (point.length !== 1 + 2 * size || point[0] !== 0x04) {
        throw dataError("Invalid EC public key point");
      }
      return AN().jwkImport({
        kty: "EC", crv: curve,
        x: point.subarray(1, 1 + size), y: point.subarray(1 + size),
      }, false);
    };

    // ---- HMAC (pre-existing native fast path) ----
    const importHmacKey = (raw, hash, extractable, usages, declaredLength) => {
      if (raw.byteLength === 0) throw dataError("Zero-length key is not supported");
      // HmacImportParams.length: 0 is a DataError, a non-multiple of 8 is a
      // NotSupportedError, anything else must match the key data exactly.
      // ref: node lib/internal/crypto/webidl.js validateMacKeyLength + mac.js.
      if (declaredLength !== undefined && declaredLength !== null) {
        const bits = Number(declaredLength);
        if (bits === 0) throw dataError("HmacImportParams.length cannot be 0");
        if (bits % 8 !== 0) throw notSupported("Unsupported HmacImportParams.length");
        if (bits !== raw.byteLength * 8) throw dataError("Invalid key length");
      }
      if (usages.length === 0) {
        throw domError("Usages cannot be empty when importing a secret key.", "SyntaxError");
      }
      const secret = new Uint8Array(raw);            // retained for sign/verify + export
      raw.fill(0);
      // HMAC runs through node:crypto's createHmac (which covers the SHA-2 and
      // SHA-3 families uniformly), so the key is a plain CryptoKey carrying the
      // secret bytes — no native HMAC context needed.
      return makeKey("secret", { name: "HMAC", hash: freeze({ name: hash.name }),
        length: secret.byteLength * 8 }, extractable, usages, { secret });
    };

    // ---- AES ----
    const aesCipherName = (algorithm, mode) => "aes-" + algorithm.length + "-" + mode;
    const requireAesLength = (length) => {
      const n = Number(length);
      if (n !== 128 && n !== 192 && n !== 256) {
        throw operationError("Invalid key length");
      }
      return n;
    };

    // ---- generateKey ----
    const generateRsa = (alg, name, extractable, usages) => {
      const hash = memberHash(alg);
      requiredMember(alg.modulusLength, "algorithm.modulusLength");
      requiredMember(alg.publicExponent, "algorithm.publicExponent");
      const modulusLength = Number(alg.modulusLength);
      if (!Number.isInteger(modulusLength) || modulusLength < 256) {
        throw new TypeError("Invalid modulusLength");
      }
      if (alg.publicExponent != null) {
        const e = copyBytes(alg.publicExponent);
        // AN.generateKeyPair fixes F4; any other exponent would silently be wrong.
        const isF4 = e.length === 3 && e[0] === 0x01 && e[1] === 0x00 && e[2] === 0x01;
        if (!isF4) throw notSupported("Only publicExponent 65537 is supported");
      }
      const isOaep = name === "RSA-OAEP";
      restrictUsages(usages, isOaep ? ["encrypt", "decrypt", "wrapKey", "unwrapKey"]
                                    : ["sign", "verify"], "an " + name);
      const privUsages = intersectUsages(usages, allowedUsagesFor(name, "private"));
      requireUsages(privUsages);
      const res = AN().generateKeyPair("rsa", modulusLength, "", "spki", "der", "pkcs8", "der", "", "");
      const algorithm = freeze({
        name, modulusLength,
        publicExponent: new Uint8Array([0x01, 0x00, 0x01]),
        hash: freeze({ name: hash.name }),
      });
      return {
        publicKey: makeKey("public", algorithm, true,
          intersectUsages(usages, allowedUsagesFor(name, "public")),
          { material: new Uint8Array(res.publicKey) }),
        privateKey: makeKey("private", algorithm, extractable, privUsages,
          { material: new Uint8Array(res.privateKey) }),
      };
    };
    const generateEc = (alg, name, extractable, usages) => {
      const curve = normalizeCurve(alg.namedCurve);
      restrictUsages(usages, name === "ECDSA" ? ["sign", "verify"] : ["deriveKey", "deriveBits"],
        "an " + name);
      const privUsages = intersectUsages(usages, allowedUsagesFor(name, "private"));
      requireUsages(privUsages);
      const res = AN().generateKeyPair("ec", 0, curveToGroup[curve], "spki", "der", "pkcs8", "der", "", "");
      const algorithm = freeze({ name, namedCurve: curve });
      return {
        publicKey: makeKey("public", algorithm, true,
          intersectUsages(usages, allowedUsagesFor(name, "public")),
          { material: new Uint8Array(res.publicKey) }),
        privateKey: makeKey("private", algorithm, extractable, privUsages,
          { material: new Uint8Array(res.privateKey) }),
      };
    };
    const generateOkp = (name, extractable, usages) => {
      const isEddsa = name === "Ed25519" || name === "Ed448";
      restrictUsages(usages, isEddsa ? ["sign", "verify"] : ["deriveKey", "deriveBits"],
        "an " + name);
      requireUsages(intersectUsages(usages, allowedUsagesFor(name, "private")));
      const res = AN().generateKeyPair(name.toLowerCase(), 0, "", "spki", "der", "pkcs8", "der", "", "");
      const algorithm = freeze({ name });
      return {
        publicKey: makeKey("public", algorithm, true,
          intersectUsages(usages, allowedUsagesFor(name, "public")),
          { material: new Uint8Array(res.publicKey) }),
        privateKey: makeKey("private", algorithm, extractable,
          intersectUsages(usages, allowedUsagesFor(name, "private")),
          { material: new Uint8Array(res.privateKey) }),
      };
    };

    // ---- sign / verify ----
    // EdDsaParams.context needs OpenSSL >= 3.2 (EVP_PKEY_CTX ed context params).
    // mbun links 3.1.5, so a non-empty context is refused rather than silently
    // ignored — signing with the wrong domain separator would be a wrong answer.
    const requireNoEddsaContext = (alg) => {
      if (alg.context == null) return;
      const context = copyBytes(alg.context);
      if (context.length === 0) return;
      throw notSupported("EdDSA context is not supported by this OpenSSL build");
    };
    const RSA_PKCS1_PADDING = 1, RSA_PKCS1_OAEP_PADDING = 4, RSA_PKCS1_PSS_PADDING = 6;
    const SALTLEN_DIGEST = -1;
    // RSA-PSS saltLength is an int32 bounded by the modulus and digest sizes.
    // node surfaces the range failure as an OperationError whose `cause` is the
    // original ERR_OUT_OF_RANGE (lib/internal/crypto/rsa.js rsaSignVerify).
    const pssSaltLength = (alg, algorithm) => {
      requiredMember(alg.saltLength, "algorithm.saltLength");
      const max = Math.ceil((algorithm.modulusLength - 1) / 8) -
        hashOutBytes[algorithm.hash.name] - 2;
      const saltLength = Number(alg.saltLength);
      if (!Number.isInteger(saltLength) || saltLength < 0 || saltLength > max) {
        const cause = new RangeError('The value of "algorithm.saltLength" is out of range. ' +
          "It must be >= 0 && <= " + max + ". Received " + alg.saltLength);
        cause.code = "ERR_OUT_OF_RANGE";
        cause.name = "RangeError";
        const err = operationError("The operation failed for an operation-specific reason");
        try { err.cause = cause; } catch (e) {}
        throw err;
      }
      return saltLength;
    };
    // KMAC(K, X, L, S) over the EVP_MAC binding. `outputLength` is required and
    // must be a whole number of bytes; `customization` is the optional S string.
    // ref: node lib/internal/crypto/{mac.js kmacSignVerify, webidl.js KmacParams}.
    const kmacBytes = (alg, metadata, data) => {
      requiredMember(alg.outputLength, "algorithm.outputLength");
      const bits = Number(alg.outputLength);
      if (!Number.isInteger(bits) || bits < 0 || bits % 8 !== 0) {
        throw notSupported("Unsupported KmacParams outputLength");
      }
      const custom = alg.customization != null ? copyBytes(alg.customization) : new Uint8Array(0);
      return new Uint8Array(AN().kmac(metadata.algorithm.name === "KMAC128" ? "KMAC-128" : "KMAC-256",
        metadata.secret, data, custom, bits / 8));
    };
    const signBytes = (alg, metadata, data) => {
      const name = metadata.algorithm.name;
      if (name === "RSASSA-PKCS1-v1_5") {
        return AN().sign(mdName(metadata.algorithm.hash), data, metadata.material, "",
          RSA_PKCS1_PADDING, SALTLEN_DIGEST, "");
      }
      if (name === "RSA-PSS") {
        return AN().sign(mdName(metadata.algorithm.hash), data, metadata.material, "",
          RSA_PKCS1_PSS_PADDING, pssSaltLength(alg, metadata.algorithm), "");
      }
      if (name === "ECDSA") {
        // WebCrypto ECDSA signatures are raw r||s (IEEE P1363), never DER.
        return AN().sign(mdName(memberHash(alg)), data, metadata.material, "",
          RSA_PKCS1_PADDING, SALTLEN_DIGEST, "ieee-p1363");
      }
      if (name === "Ed25519" || name === "Ed448") {
        requireNoEddsaContext(alg);
        return AN().sign("", data, metadata.material, "", RSA_PKCS1_PADDING, SALTLEN_DIGEST, "");
      }
      throw notSupported("Unrecognized algorithm name");
    };
    const verifyBytes = (alg, metadata, signature, data) => {
      const name = metadata.algorithm.name;
      if (name === "RSASSA-PKCS1-v1_5") {
        return AN().verify(mdName(metadata.algorithm.hash), data, metadata.material, "", signature,
          RSA_PKCS1_PADDING, SALTLEN_DIGEST, "");
      }
      if (name === "RSA-PSS") {
        return AN().verify(mdName(metadata.algorithm.hash), data, metadata.material, "", signature,
          RSA_PKCS1_PSS_PADDING, pssSaltLength(alg, metadata.algorithm), "");
      }
      if (name === "ECDSA") {
        // A wrong-sized r||s is a plain `false`, never an exception (spec).
        const size = curveBytes[metadata.algorithm.namedCurve];
        if (size != null && signature.length !== size * 2) return false;
        return AN().verify(mdName(memberHash(alg)), data, metadata.material, "", signature,
          RSA_PKCS1_PADDING, SALTLEN_DIGEST, "ieee-p1363");
      }
      if (name === "Ed25519" || name === "Ed448") {
        requireNoEddsaContext(alg);
        return AN().verify("", data, metadata.material, "", signature, RSA_PKCS1_PADDING,
          SALTLEN_DIGEST, "");
      }
      throw notSupported("Unrecognized algorithm name");
    };

    // ---- encrypt / decrypt ----
    // AeadParams.tagLength validation, verbatim per algorithm from node
    // lib/internal/crypto/webidl.js (the corpus matches on these strings).
    const AEAD_TAG_BITS = {
      "AES-GCM": [32, 64, 96, 104, 112, 120, 128],
      "AES-OCB": [64, 96, 128],
      "ChaCha20-Poly1305": [128],
    };
    const aeadTagBits = (alg, name) => {
      const bits = alg.tagLength == null ? 128 : Number(alg.tagLength);
      const legal = AEAD_TAG_BITS[name];
      if (legal && !legal.includes(bits)) {
        throw operationError(bits + " is not a valid " + name + " tag length");
      }
      return bits;
    };
    // AeadParams.iv per algorithm: ChaCha20-Poly1305 is fixed at 12 bytes,
    // AES-OCB accepts 1..15, AES-GCM any non-empty length.
    const aeadIv = (alg, name) => {
      requiredMember(alg.iv, "algorithm.iv");
      const iv = copyBytes(alg.iv);
      if (name === "ChaCha20-Poly1305" && iv.length !== 12) {
        throw operationError("algorithm.iv must contain exactly 12 bytes");
      }
      if (name === "AES-OCB" && iv.length > 15) {
        throw operationError("AES-OCB algorithm.iv must be no more than 15 bytes");
      }
      if (iv.length === 0) throw operationError("algorithm.iv must not be empty");
      return iv;
    };
    const aeadCipherName = (algorithm) =>
      algorithm.name === "ChaCha20-Poly1305" ? "chacha20-poly1305"
                                             : aesCipherName(algorithm, "ocb");
    const aesCtrCounter = (alg) => {
      requiredMember(alg.counter, "algorithm.counter");
      const counter = copyBytes(alg.counter);
      if (counter.length !== 16) {
        throw operationError("algorithm.counter must contain exactly 16 bytes");
      }
      return counter;
    };
    // WebCrypto AES-CTR: only the low `length` bits of the 128-bit block form the
    // counter (they wrap independently); the high bits are a fixed nonce. EVP's CTR
    // mode always increments the full 128-bit block, so for length != 128 we drive
    // CTR block-by-block ourselves: each keystream block is AES-CTR(nonce||ctr) over
    // a zero block (= AES_encrypt(block)), XORed into the data, then the low `length`
    // bits are incremented with wraparound. ref: bun AES_CTR.cpp / WebCrypto spec.
    const aesCtrIncr = (ctr, length) => {
      let carry = 1;
      for (let bit = 0; bit < length && carry; bit++) {
        const byteIdx = 15 - (bit >> 3);
        const mask = 1 << (bit & 7);
        if (ctr[byteIdx] & mask) { ctr[byteIdx] &= ~mask; carry = 1; }
        else { ctr[byteIdx] |= mask; carry = 0; }
      }
    };
    const aesCtrCrypt = (metadata, alg, data) => {
      const counter0 = aesCtrCounter(alg);
      requiredMember(alg.length, "algorithm.length");
      const length = Number(alg.length);
      if (!Number.isInteger(length) || length < 1 || length > 128) {
        throw operationError("AES-CTR algorithm.length must be between 1 and 128");
      }
      const ctrName = aesCipherName(metadata.algorithm, "ctr");
      // Fast path: a full 128-bit counter matches EVP's native increment exactly.
      if (length === 128) {
        return new Uint8Array(AN().cipher(ctrName, metadata.secret, counter0, data,
          null, true, null, 16).data);
      }
      const out = new Uint8Array(data.length);
      const ctr = counter0.slice();
      const zero16 = new Uint8Array(16);
      const nblocks = Math.ceil(data.length / 16);
      for (let bi = 0; bi < nblocks; bi++) {
        const ks = new Uint8Array(AN().cipher(ctrName, metadata.secret, ctr, zero16,
          null, true, null, 16).data);
        const base = bi * 16;
        const n = Math.min(16, data.length - base);
        for (let j = 0; j < n; j++) out[base + j] = data[base + j] ^ ks[j];
        aesCtrIncr(ctr, length);
      }
      return out;
    };
    // AES Key Wrap (RFC 3394). Driven over raw 16-byte AES-ECB blocks with padding
    // disabled (native cipher arg 8). ref: bun CryptoAlgorithmAESKW / RFC 3394.
    const aesEcbBlock = (secret, block, encrypt) => new Uint8Array(
      AN().cipher("aes-" + (secret.length * 8) + "-ecb", secret, new Uint8Array(0),
        block, null, encrypt, null, 16, true).data);
    const AESKW_IV = [0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6];
    const aesKwWrap = (secret, plain) => {
      if (plain.length < 16 || plain.length % 8 !== 0) throw operationError("AES-KW: key must be a multiple of 8 bytes and at least 16");
      const n = plain.length / 8;
      let A = new Uint8Array(AESKW_IV);
      const R = [];
      for (let i = 0; i < n; i++) R.push(plain.slice(i * 8, i * 8 + 8));
      for (let j = 0; j <= 5; j++) {
        for (let i = 1; i <= n; i++) {
          const B = aesEcbBlock(secret, concatBytes(A, R[i - 1]), true);
          A = B.slice(0, 8);
          let t = n * j + i;
          for (let k = 7; k >= 0 && t > 0; k--) { A[k] ^= (t & 0xff); t = Math.floor(t / 256); }
          R[i - 1] = B.slice(8, 16);
        }
      }
      const out = new Uint8Array((n + 1) * 8);
      out.set(A, 0);
      for (let i = 0; i < n; i++) out.set(R[i], (i + 1) * 8);
      return out;
    };
    const aesKwUnwrap = (secret, wrapped) => {
      if (wrapped.length < 24 || wrapped.length % 8 !== 0) throw operationError("AES-KW: wrapped key length is invalid");
      const n = wrapped.length / 8 - 1;
      let A = wrapped.slice(0, 8);
      const R = [];
      for (let i = 0; i < n; i++) R.push(wrapped.slice((i + 1) * 8, (i + 2) * 8));
      for (let j = 5; j >= 0; j--) {
        for (let i = n; i >= 1; i--) {
          const At = A.slice();
          let t = n * j + i;
          for (let k = 7; k >= 0 && t > 0; k--) { At[k] ^= (t & 0xff); t = Math.floor(t / 256); }
          const B = aesEcbBlock(secret, concatBytes(At, R[i - 1]), false);
          A = B.slice(0, 8);
          R[i - 1] = B.slice(8, 16);
        }
      }
      for (let k = 0; k < 8; k++) if (A[k] !== AESKW_IV[k]) throw operationError("AES-KW: integrity check failed");
      const out = new Uint8Array(n * 8);
      for (let i = 0; i < n; i++) out.set(R[i], i * 8);
      return out;
    };
    const encryptBytes = (alg, metadata, data) => {
      const name = metadata.algorithm.name;
      if (name === "AES-KW") return aesKwWrap(metadata.secret, data);
      if (name === "RSA-OAEP") {
        const label = alg.label != null ? copyBytes(alg.label) : null;
        try {
          return AN().publicEncrypt(metadata.material, "", data, RSA_PKCS1_OAEP_PADDING,
            mdName(metadata.algorithm.hash), label);
        } catch (e) { throw operationError(String((e && e.message) || e)); }
      }
      if (AEAD_ALGS.has(name)) {
        const iv = aeadIv(alg, name);
        const tagBytes = aeadTagBits(alg, name) / 8;
        const aad = alg.additionalData != null ? copyBytes(alg.additionalData) : null;
        const cipherName = name === "AES-GCM" ? aesCipherName(metadata.algorithm, "gcm")
                                              : aeadCipherName(metadata.algorithm);
        const res = AN().cipher(cipherName, metadata.secret, iv, data, aad, true, null, tagBytes);
        // WebCrypto AEAD ciphers return ciphertext || tag.
        return concatBytes(new Uint8Array(res.data), new Uint8Array(res.tag));
      }
      if (name === "AES-CBC") {
        const iv = copyBytes(alg.iv);
        if (iv.length !== 16) throw operationError("algorithm.iv must contain exactly 16 bytes");
        return new Uint8Array(AN().cipher(aesCipherName(metadata.algorithm, "cbc"), metadata.secret,
          iv, data, null, true, null, 16).data);
      }
      if (name === "AES-CTR") {
        return aesCtrCrypt(metadata, alg, data);
      }
      throw notSupported("Unrecognized algorithm name");
    };
    const decryptBytes = (alg, metadata, data) => {
      const name = metadata.algorithm.name;
      if (name === "AES-KW") return aesKwUnwrap(metadata.secret, data);
      if (name === "RSA-OAEP") {
        const label = alg.label != null ? copyBytes(alg.label) : null;
        try {
          return AN().privateDecrypt(metadata.material, "", data, RSA_PKCS1_OAEP_PADDING,
            mdName(metadata.algorithm.hash), label);
        } catch (e) { throw operationError(String((e && e.message) || e)); }
      }
      if (AEAD_ALGS.has(name)) {
        const iv = aeadIv(alg, name);
        const tagBytes = aeadTagBits(alg, name) / 8;
        if (data.length < tagBytes) throw operationError("Ciphertext is shorter than the tag");
        const aad = alg.additionalData != null ? copyBytes(alg.additionalData) : null;
        const cipherName = name === "AES-GCM" ? aesCipherName(metadata.algorithm, "gcm")
                                              : aeadCipherName(metadata.algorithm);
        try {
          return new Uint8Array(AN().cipher(cipherName,
            metadata.secret, iv, data.subarray(0, data.length - tagBytes), aad, false,
            data.subarray(data.length - tagBytes), tagBytes).data);
        } catch (e) { throw operationError("Authentication tag verification failed"); }
      }
      if (name === "AES-CBC") {
        const iv = copyBytes(alg.iv);
        if (iv.length !== 16) throw operationError("algorithm.iv must contain exactly 16 bytes");
        try {
          return new Uint8Array(AN().cipher(aesCipherName(metadata.algorithm, "cbc"),
            metadata.secret, iv, data, null, false, null, 16).data);
        } catch (e) { throw operationError("Decryption failed"); }
      }
      if (name === "AES-CTR") {
        // CTR decryption is identical to encryption (XOR against the keystream).
        return aesCtrCrypt(metadata, alg, data);
      }
      throw notSupported("Unrecognized algorithm name");
    };

    // ---- deriveBits ----
    const hmacRaw = (hashName, keyBytes, data) =>
      new Uint8Array(nodeCrypto().createHmac(mdName({ name: hashName }), G.Buffer.from(keyBytes))
        .update(G.Buffer.from(data)).digest());
    // RFC 5869 extract-then-expand over the HMAC primitive (no EVP_KDF binding).
    const hkdf = (hashName, ikm, salt, info, lengthBytes) => {
      const outLen = hashOutBytes[hashName];
      const prk = hmacRaw(hashName, salt.length ? salt : new Uint8Array(outLen), ikm);
      const n = Math.ceil(lengthBytes / outLen);
      if (n > 255) throw operationError("HKDF output is too long");
      let t = new Uint8Array(0);
      let okm = new Uint8Array(0);
      for (let i = 1; i <= n; i++) {
        t = hmacRaw(hashName, prk, concatBytes(concatBytes(t, info), new Uint8Array([i])));
        okm = concatBytes(okm, t);
      }
      return okm.subarray(0, lengthBytes);
    };
    // Spec "derive bits" tail shared by ECDH and X25519 (bun CryptoAlgorithm.cpp
    // extractDerivedBits): a null length takes the whole secret, otherwise keep
    // ceil(len/8) bytes and zero the unused low bits of the final byte. A length
    // longer than the secret is an OperationError.
    const extractDerivedBits = (secret, lengthBits) => {
      if (lengthBits == null) return secret;
      const nbytes = Math.ceil(lengthBits / 8);
      if (nbytes > secret.length) throw operationError("derived bit length is too small");
      const out = secret.slice(0, nbytes);
      const rem = lengthBits % 8;
      if (rem !== 0 && nbytes > 0) out[nbytes - 1] &= (0xff << (8 - rem)) & 0xff;
      return out;
    };
    // node: a null length is "length cannot be null", a non-multiple of 8 is
    // "length must be a multiple of 8" (both OperationError) — the KDF paths only.
    const requireKdfLength = (lengthBits) => {
      if (lengthBits == null) throw operationError("length cannot be null");
      if (lengthBits % 8 !== 0) throw operationError("length must be a multiple of 8");
    };
    // The `public` member of the ECDH/X25519/X448 algorithm dictionary is a
    // required CryptoKey: absent → ERR_MISSING_OPTION TypeError (webidl's
    // required-member step), present but not a CryptoKey → a plain TypeError.
    // EcdhKeyDeriveParams.public is validated by the IDL dictionary converter,
    // i.e. BEFORE the baseKey usage/algorithm checks. ref: node
    // lib/internal/crypto/webidl.js converters.EcdhKeyDeriveParams.
    const requirePeerKey = (alg, name) => {
      requiredMember(alg.public, "algorithm.public");
      const peer = keyMetadata.get(alg.public);
      if (!peer) {
        const err = new TypeError(
          'The "algorithm.public" property must be an instance of CryptoKey.');
        err.code = "ERR_INVALID_ARG_TYPE";
        throw err;
      }
      if (peer.type !== "public") {
        throw domError("algorithm.public must be a public key", "InvalidAccessError");
      }
      if (peer.algorithm.name.toLowerCase() !== String(name).toLowerCase()) {
        throw domError("key algorithm mismatch", "InvalidAccessError");
      }
      return peer;
    };
    // Run that converter step up front for the algorithms that declare it.
    const normalizeDeriveAlg = (alg) => {
      if (alg.name === "ECDH" || alg.name === "X25519" || alg.name === "X448") {
        requirePeerKey(alg, alg.name);
      }
      return alg;
    };
    const deriveBytes = (alg, metadata, lengthBits) => {
      const name = metadata.algorithm.name;
      if (name === "PBKDF2") {
        const hash = memberHash(alg);
        requiredMember(alg.salt, "algorithm.salt");
        requiredMember(alg.iterations, "algorithm.iterations");
        const iterations = Number(alg.iterations);
        if (!Number.isInteger(iterations) || iterations <= 0) {
          throw operationError("iterations must be a positive integer");
        }
        requireKdfLength(lengthBits);
        if (lengthBits === 0) return new Uint8Array(0);   // zero bits after param validation
        return new Uint8Array(nodeCrypto().pbkdf2Sync(G.Buffer.from(metadata.secret),
          G.Buffer.from(copyBytes(alg.salt)), iterations, lengthBits / 8, mdName(hash)));
      }
      if (name === "HKDF") {
        const hash = memberHash(alg);
        requiredMember(alg.salt, "algorithm.salt");
        requiredMember(alg.info, "algorithm.info");
        const info = copyBytes(alg.info);
        // OpenSSL's HKDF caps the info string at 1024 bytes; node validates it in
        // the HkdfParams converter, i.e. before the length checks below.
        if (info.length > 1024) throw operationError("algorithm.info must be at most 1024 bytes");
        requireKdfLength(lengthBits);
        if (lengthBits === 0) return new Uint8Array(0);
        return hkdf(hash.name, metadata.secret, copyBytes(alg.salt), info, lengthBits / 8);
      }
      if (name === "ECDH") {
        if (metadata.type !== "private") {
          throw domError("baseKey must be a private key", "InvalidAccessError");
        }
        const peer = requirePeerKey(alg, "ECDH");
        if (peer.algorithm.namedCurve !== metadata.algorithm.namedCurve) {
          throw domError("Named curve mismatch", "InvalidAccessError");
        }
        // The scalar lives inside the PKCS8 DER; recover it via the JWK bridge.
        const priv = unb64u(jwkFromDer(metadata.material, false).d);
        const secret = new Uint8Array(AN().ecdhComputeSecret(curveToGroup[metadata.algorithm.namedCurve],
          priv, ecPointFromMaterial(peer.material)));
        return extractDerivedBits(secret, lengthBits);
      }
      if (name === "X25519" || name === "X448") {
        // The base key must be the private half and `public` the peer's public
        // half; both are DER, so the scalar never surfaces in JS.
        // ref: node lib/internal/crypto/diffiehellman.js ecdhDeriveBits.
        if (metadata.type !== "private") {
          throw domError("baseKey must be a private key", "InvalidAccessError");
        }
        const peer = requirePeerKey(alg, name);
        let secret;
        // A small-order/all-zero peer point makes the native derive fail
        // (RFC 7748 section 6.1) — that is an OperationError, not a crash.
        try { secret = new Uint8Array(AN().okpDerive(metadata.material, peer.material)); }
        catch (e) { throw operationError("The operation failed for an operation-specific reason"); }
        return extractDerivedBits(secret, lengthBits);
      }
      throw notSupported("Unrecognized algorithm name");
    };

    // The public SubtleCrypto methods are NOT async functions in node: each is a
    // plain method that returns a Promise, so a bad receiver rejects rather than
    // throws (test-webcrypto-methods-not-async asserts both). The algorithm
    // bodies live on this internal object; the exposed wrappers add the
    // ERR_INVALID_THIS brand check. Internal re-entry (deriveKey/unwrapKey ->
    // importKey) goes through the impl, never through the user-visible method.
    class SubtleCryptoImpl {
      async digest(algorithm, data) {
        // cSHAKE is an XOF: its output length is a required dictionary member,
        // and (per node) must be a whole number of bytes. Without a function
        // name / customization string cSHAKE degenerates to plain SHAKE, which
        // is what node's C++ layer computes too.
        // ref: node lib/internal/crypto/{webidl.js CShakeParams, hash.js}.
        const cshake = normalizeAlgorithm(algorithm);
        const cshakeName = String(cshake.name == null ? "" : cshake.name).toUpperCase();
        if (cshakeName === "CSHAKE128" || cshakeName === "CSHAKE256") {
          if (cshake.outputLength === undefined) {
            throw new TypeError("Failed to normalize algorithm: outputLength is required");
          }
          const bits = Number(cshake.outputLength);
          if (!Number.isInteger(bits) || bits < 0 || bits % 8 !== 0) {
            throw notSupported("Unsupported CShakeParams outputLength");
          }
          const bytes = copyBytes(data);
          const md = cshakeName === "CSHAKE128" ? "shake128" : "shake256";
          return arrayBuffer(nodeCrypto().createHash(md, { outputLength: bits / 8 })
            .update(G.Buffer.from(bytes)).digest());
        }
        const name = normalizeHash(algorithm).name;
        return arrayBuffer(G.Bun.CryptoHasher.hash(name, copyBytes(data)));
      }

      async generateKey(algorithm, extractable, keyUsages) {
        if (arguments.length < 3) throw missingArgs(3, arguments.length);
        const alg = normalizeAlg(algorithm);
        const usages = normalizeUsages(keyUsages);
        const name = alg.name;
        if (RSA_ALGS.has(name)) return generateRsa(alg, name, extractable, usages);
        if (EC_ALGS.has(name)) return generateEc(alg, name, extractable, usages);
        if (OKP_ALGS.has(name)) return generateOkp(name, extractable, usages);
        if (name === "HMAC") {
          const hash = memberHash(alg);
          restrictUsages(usages, ["sign", "verify"], "an HMAC");
          const bits = alg.length == null ? hashBlockBits[hash.name] : Number(alg.length);
          if (!Number.isInteger(bits) || bits === 0 || bits % 8 !== 0) {
            throw operationError("Invalid key length");
          }
          return importHmacKey(randomBytes(bits / 8), hash, extractable, usages, null);
        }
        if (KMAC_ALGS.has(name)) {
          restrictUsages(usages, ["sign", "verify"], name);
          requireUsages(usages);
          const bits = alg.length == null ? (name === "KMAC128" ? 128 : 256) : Number(alg.length);
          if (!Number.isInteger(bits) || bits === 0 || bits % 8 !== 0) {
            throw operationError("Invalid key length");
          }
          return makeKey("secret", { name, length: bits }, extractable, usages,
            { secret: randomBytes(bits / 8) });
        }
        if (name === "ChaCha20-Poly1305") {
          // A ChaCha20-Poly1305 key is always 256 bits and carries no `length`
          // in its key algorithm. ref: node internal/crypto/chacha20_poly1305.js.
          restrictUsages(usages, allowedUsagesFor(name, "secret"), "a " + name);
          requireUsages(usages);
          return makeKey("secret", { name }, extractable, usages, { secret: randomBytes(32) });
        }
        if (AES_ALGS.has(name)) {
          const length = requireAesLength(alg.length);
          restrictUsages(usages, allowedUsagesFor(name, "secret"), "an AES");
          requireUsages(usages);
          return makeKey("secret", { name, length }, extractable, usages,
            { secret: randomBytes(length / 8) });
        }
        throw notSupported("Unrecognized algorithm name");
      }

      // Deliberately NOT async: node's KeyObject.prototype.toCryptoKey is a
      // synchronous bridge into the same import steps, and the public wrapper
      // promisifies the result anyway.
      importKey(format, keyData, algorithm, extractable, keyUsages) {
        if (arguments.length < 5) throw missingArgs(5, arguments.length);
        const convertedFormat = keyFormat(format);
        // node converts the arguments before normalizing the algorithm, so a
        // non-BufferSource keyData is ERR_INVALID_ARG_TYPE even when the
        // algorithm is unrecognized.
        if (convertedFormat !== "jwk") copyBytes(keyData);
        const alg = normalizeAlg(algorithm);
        const usages = normalizeUsages(keyUsages);
        const name = alg.name;
        const unsupportedFormat = () =>
          notSupported("Unable to import " + name + " using " + convertedFormat + " format");
        // node aliases "raw-public"/"raw-secret" onto "raw" for every key kind
        // whose raw encoding is unambiguous; AES-OCB and ChaCha20-Poly1305
        // secrets require the explicit "raw-secret" spelling.
        // ref: node lib/internal/crypto/webcrypto.js aliasKeyFormat/importKeySync.
        const rawSecret = convertedFormat === "raw-secret" ||
          (convertedFormat === "raw" && !RAW_SECRET_ONLY.has(name));
        const rawPublic = convertedFormat === "raw" || convertedFormat === "raw-public";

        // node's validateJwk for symmetric ("oct") keys.
        // ref: node lib/internal/crypto/webcrypto_util.js validateJwk.
        const octFromJwk = (expectedUse) => {
          if (keyData == null || typeof keyData !== "object" ||
              typeof keyData.kty !== "string") throw dataError("Invalid keyData");
          if (keyData.kty !== "oct") throw dataError('Invalid JWK "kty" Parameter');
          if (typeof keyData.k !== "string") throw dataError("Invalid keyData");
          if (usages.length > 0 && keyData.use !== undefined && keyData.use !== expectedUse) {
            throw dataError('Invalid JWK "use" Parameter');
          }
          validateKeyOps(keyData.key_ops, usages);
          if (keyData.ext === false && extractable) {
            throw dataError('JWK "ext" Parameter and extractable mismatch');
          }
          try { return unb64u(keyData.k); }
          catch (e) { throw dataError("Invalid keyData"); }
        };
        const checkJwkAlg = (expected) => {
          if (convertedFormat !== "jwk" || keyData.alg === undefined) return;
          if (expected !== undefined && keyData.alg !== expected) {
            throw dataError('JWK "alg" does not match the requested algorithm');
          }
        };

        if (name === "HMAC") {
          if (!rawSecret && convertedFormat !== "jwk") throw unsupportedFormat();
          restrictUsages(usages, ["sign", "verify"], "HMAC");
          const hash = memberHash(alg);
          const raw = convertedFormat === "jwk" ? octFromJwk("sig") : copyBytes(keyData);
          checkJwkAlg(jwkAlgFor({ name: "HMAC", hash }));
          return importHmacKey(raw, hash, extractable, usages, alg.length);
        }

        if (KMAC_ALGS.has(name)) {
          if (!rawSecret && convertedFormat !== "jwk") throw unsupportedFormat();
          restrictUsages(usages, ["sign", "verify"], name);
          const raw = convertedFormat === "jwk" ? octFromJwk("sig") : copyBytes(keyData);
          checkJwkAlg(name === "KMAC128" ? "K128" : "K256");
          const bits = raw.byteLength * 8;
          if (bits === 0) throw dataError("Zero-length key is not supported");
          if (alg.length !== undefined && Number(alg.length) !== bits) {
            throw dataError("Invalid key length");
          }
          requireImportUsages(usages, "secret");
          return makeKey("secret", { name, length: bits }, extractable, usages, { secret: raw });
        }
        if (name === "ChaCha20-Poly1305") {
          if (!rawSecret && convertedFormat !== "jwk") throw unsupportedFormat();
          const raw = convertedFormat === "jwk" ? octFromJwk("enc") : copyBytes(keyData);
          checkJwkAlg("C20P");
          if (raw.byteLength !== 32) throw dataError("Invalid key length");
          restrictUsages(usages, allowedUsagesFor(name, "secret"), "a " + name);
          requireImportUsages(usages, "secret");
          return makeKey("secret", { name }, extractable, usages, { secret: raw });
        }

        if (AES_ALGS.has(name)) {
          if (!rawSecret && convertedFormat !== "jwk") throw unsupportedFormat();
          const raw = convertedFormat === "jwk" ? octFromJwk("enc") : copyBytes(keyData);
          // WebCrypto ignores alg.length on raw/jwk import — key length derives
          // from the data. A bad length is a DataError here (node aes.js
          // validateKeyLength), not the OperationError generateKey raises.
          const bits = raw.byteLength * 8;
          if (bits !== 128 && bits !== 192 && bits !== 256) throw dataError("Invalid key length");
          checkJwkAlg(jwkAlgFor({ name, length: bits }));
          restrictUsages(usages, allowedUsagesFor(name, "secret"), "an AES");
          requireImportUsages(usages, "secret");
          return makeKey("secret", { name, length: bits }, extractable, usages, { secret: raw });
        }

        if (name === "PBKDF2" || name === "HKDF") {
          if (!rawSecret) throw unsupportedFormat();
          restrictUsages(usages, ["deriveKey", "deriveBits"], "a " + name);
          if (extractable) throw domError(name + " keys must not be extractable", "SyntaxError");
          requireImportUsages(usages, "secret");
          return makeKey("secret", { name }, false, usages, { secret: copyBytes(keyData) });
        }

        // --- asymmetric ---
        let material;
        let type;
        if (convertedFormat === "spki") {
          material = copyBytes(keyData); type = "public";
        } else if (convertedFormat === "pkcs8") {
          material = copyBytes(keyData); type = "private";
        } else if (convertedFormat === "jwk") {
          if (keyData == null || typeof keyData !== "object") throw dataError("Invalid keyData");
          // Validate the JWK against the requested algorithm/usages (spec import
          // steps): kty must match the family, key_ops (if present) must be a
          // superset of the requested usages, and ext:false forbids an
          // extractable import.
          const wantKty = RSA_ALGS.has(name) ? "RSA" : EC_ALGS.has(name) ? "EC"
            : OKP_ALGS.has(name) ? "OKP" : null;
          if (typeof keyData.kty !== "string") throw dataError("Invalid keyData");
          if (wantKty && keyData.kty !== wantKty) throw dataError('Invalid JWK "kty" Parameter');
          // Per-kty required members (node webcrypto_util.js validateJwk).
          const str = (v) => typeof v === "string";
          const optStr = (v) => v === undefined || typeof v === "string";
          const badKeyData = () => dataError("Invalid keyData");
          if (wantKty === "RSA") {
            if (!str(keyData.n) || !str(keyData.e) || !optStr(keyData.d)) throw badKeyData();
            if (str(keyData.d) && !(str(keyData.p) && str(keyData.q) && str(keyData.dp) &&
                str(keyData.dq) && str(keyData.qi))) throw badKeyData();
          } else if (wantKty === "EC") {
            if (!str(keyData.crv) || !str(keyData.x) || !str(keyData.y) ||
                !optStr(keyData.d)) throw badKeyData();
          } else if (wantKty === "OKP") {
            if (!str(keyData.crv) || !str(keyData.x) || !optStr(keyData.d)) throw badKeyData();
          }
          // "use" is checked against the algorithm's purpose whenever usages were
          // requested. ref: node lib/internal/crypto/{rsa,ec,cfrg}.js expectedUse.
          const expectedUse = (name === "RSA-OAEP" || name === "ECDH" ||
            name === "X25519" || name === "X448") ? "enc" : "sig";
          if (usages.length > 0 && keyData.use !== undefined && keyData.use !== expectedUse) {
            throw dataError('Invalid JWK "use" Parameter');
          }
          // The JWK "alg" member, when present, must name the same primitive.
          // ref: node lib/internal/crypto/{rsa,ec,cfrg}.js.
          if (keyData.alg !== undefined) {
            const jwkAlgMismatch = () =>
              dataError('JWK "alg" does not match the requested algorithm');
            if (RSA_ALGS.has(name)) {
              const expected = jwkAlgFor({ name, hash: memberHash(alg) });
              if (expected !== undefined && keyData.alg !== expected) throw jwkAlgMismatch();
            } else if (name === "Ed25519" || name === "Ed448") {
              if (keyData.alg !== name && keyData.alg !== "EdDSA") throw jwkAlgMismatch();
            } else if (name === "ECDSA") {
              const byAlg = { ES256: "P-256", ES384: "P-384", ES512: "P-521" }[keyData.alg];
              if (byAlg !== normalizeCurve(alg.namedCurve)) throw jwkAlgMismatch();
            }
          }
          if (OKP_ALGS.has(name) && keyData.crv !== name) {
            throw dataError('JWK "crv" Parameter and algorithm name mismatch');
          }
          validateKeyOps(keyData.key_ops, usages);
          if (keyData.ext === false && extractable) {
            throw dataError('JWK "ext" Parameter and extractable mismatch');
          }
          const isPrivate = typeof keyData.d === "string";
          try { material = new Uint8Array(jwkToDer(keyData, isPrivate)); }
          catch (e) { throw dataError("Invalid keyData"); }
          type = isPrivate ? "private" : "public";
        } else if (rawPublic) {
          // raw / raw-public: EC and OKP public keys only (spec).
          if (EC_ALGS.has(name)) {
            material = new Uint8Array(ecKeyFromPoint(copyBytes(keyData), normalizeCurve(alg.namedCurve)));
          } else if (OKP_ALGS.has(name)) {
            material = new Uint8Array(jwkToDer({ kty: "OKP", crv: name, x: b64u(copyBytes(keyData)) }, false));
          } else {
            throw unsupportedFormat();
          }
          type = "public";
        } else {
          throw unsupportedFormat();
        }
        const info = checkMaterial(material, name, alg);
        restrictUsages(usages, allowedUsagesFor(name, type), "a " + name);
        requireImportUsages(usages, type);

        const keyAlgorithm = { name };
        if (RSA_ALGS.has(name)) {
          // Reject non-interoperable RSA keys: a PKCS8/SPKI carrying the restricted
          // id-RSASSA-PSS (…01 0A) or id-RSAES-OAEP (…01 07) algorithm OID cannot be
          // imported as a generic RSA key (spec/browsers require rsaEncryption).
          if (convertedFormat === "pkcs8" || convertedFormat === "spki") {
            const oidPrefix = [0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01];
            for (let i = 0; i + oidPrefix.length < material.length; i++) {
              let hit = true;
              for (let j = 0; j < oidPrefix.length; j++) { if (material[i + j] !== oidPrefix[j]) { hit = false; break; } }
              if (hit) {
                const last = material[i + oidPrefix.length];
                if (last === 0x0a || last === 0x07) throw notSupported("unsupported algorithm");
                break;
              }
            }
          }
          keyAlgorithm.modulusLength = info.modulusLength;
          keyAlgorithm.publicExponent = new Uint8Array([0x01, 0x00, 0x01]);
          keyAlgorithm.hash = freeze({ name: memberHash(alg).name });
        } else if (EC_ALGS.has(name)) {
          keyAlgorithm.namedCurve = info.namedCurve;
        }
        return makeKey(type, keyAlgorithm, extractable, usages, { material });
      }

      exportKey(format, key) {
        if (arguments.length < 2) throw missingArgs(2, arguments.length);
        const convertedFormat = keyFormat(format);
        const metadata = keyMetadata.get(key);
        if (!metadata) throw invalidThis("CryptoKey");
        const name = metadata.algorithm.name;
        const type = metadata.type;
        // PBKDF2/HKDF are not registered for the exportKey operation at all.
        if (name === "PBKDF2" || name === "HKDF") {
          throw notSupported(name + " key export is not supported");
        }
        if (!metadata.extractable) {
          throw domError("key is not extractable", "InvalidAccessError");
        }
        let result;
        if (convertedFormat === "jwk") {
          const jwk = metadata.secret
            ? { kty: "oct", k: b64u(metadata.secret) }
            : jwkFromDer(metadata.material, type === "public");
          const alg = jwkAlgFor(metadata.algorithm);
          if (alg !== undefined) jwk.alg = alg;
          jwk.key_ops = Array.from(metadata.usages);
          jwk.ext = metadata.extractable;
          result = sortJwk(jwk);
        } else if (convertedFormat === "spki") {
          if (type === "public") {
            result = arrayBuffer(AN().keyExport(metadata.material, "", true, "spki", "der", "", ""));
          }
        } else if (convertedFormat === "pkcs8") {
          if (type === "private") {
            result = arrayBuffer(AN().keyExport(metadata.material, "", false, "pkcs8", "der", "", ""));
          }
        } else if (convertedFormat === "raw-secret" ||
                   (convertedFormat === "raw" && type === "secret")) {
          // "raw" only covers the classic secrets; AES-OCB / ChaCha20-Poly1305
          // must be spelled "raw-secret". ref: node webcrypto.js exportKeyRawSecret.
          if (type === "secret" &&
              (convertedFormat === "raw-secret" || !RAW_SECRET_ONLY.has(name))) {
            result = arrayBuffer(metadata.secret);
          }
        } else if (convertedFormat === "raw" || convertedFormat === "raw-public") {
          if (type === "public" && EC_ALGS.has(name)) {
            result = arrayBuffer(ecPointFromMaterial(metadata.material));
          } else if (type === "public" && OKP_ALGS.has(name)) {
            result = arrayBuffer(unb64u(jwkFromDer(metadata.material, true).x));
          }
        }
        if (!result) {
          throw notSupported("Unable to export " + name + " " + type + " key using " +
            convertedFormat + " format");
        }
        return result;
      }

      async sign(algorithm, key, data) {
        if (arguments.length < 3) throw missingArgs(3, arguments.length);
        const alg = normalizeAlg(algorithm);
        const metadata = requireKey(key, alg.name, "sign");
        const bytes = copyBytes(data);
        if (alg.name === "HMAC") return arrayBuffer(hmacRaw(metadata.algorithm.hash.name, metadata.secret, bytes));
        if (KMAC_ALGS.has(alg.name)) return arrayBuffer(kmacBytes(alg, metadata, bytes));
        if (metadata.type !== "private") {
          throw domError("Key must be a private key", "InvalidAccessError");
        }
        return arrayBuffer(signBytes(alg, metadata, bytes));
      }

      async verify(algorithm, key, signature, data) {
        if (arguments.length < 4) throw missingArgs(4, arguments.length);
        const alg = normalizeAlg(algorithm);
        const metadata = requireKey(key, alg.name, "verify");
        const sig = copyBytes(signature);
        const bytes = copyBytes(data);
        if (alg.name === "HMAC" || KMAC_ALGS.has(alg.name)) {
          const expected = alg.name === "HMAC"
            ? hmacRaw(metadata.algorithm.hash.name, metadata.secret, bytes)
            : kmacBytes(alg, metadata, bytes);
          if (expected.length !== sig.length) return false;
          let diff = 0;
          for (let i = 0; i < expected.length; i++) diff |= expected[i] ^ sig[i];
          return diff === 0;
        }
        if (metadata.type !== "public") {
          throw domError("Key must be a public key", "InvalidAccessError");
        }
        return verifyBytes(alg, metadata, sig, bytes);
      }

      async encrypt(algorithm, key, data) {
        if (arguments.length < 3) throw missingArgs(3, arguments.length);
        const alg = normalizeAlg(algorithm);
        return arrayBuffer(encryptBytes(alg, requireKey(key, alg.name, "encrypt"), copyBytes(data)));
      }

      async decrypt(algorithm, key, data) {
        if (arguments.length < 3) throw missingArgs(3, arguments.length);
        const alg = normalizeAlg(algorithm);
        return arrayBuffer(decryptBytes(alg, requireKey(key, alg.name, "decrypt"), copyBytes(data)));
      }

      async deriveBits(algorithm, baseKey, length = null) {
        if (arguments.length < 2) throw missingArgs(2, arguments.length);
        const alg = normalizeDeriveAlg(normalizeAlg(algorithm));
        const metadata = requireKey(baseKey, alg.name, "deriveBits");
        const bits = length == null ? null : Number(length);
        if (bits != null && (!Number.isInteger(bits) || bits < 0)) {
          throw operationError("Invalid derived length");
        }
        return arrayBuffer(deriveBytes(alg, metadata, bits));
      }

      async deriveKey(algorithm, baseKey, derivedKeyType, extractable, keyUsages) {
        if (arguments.length < 5) throw missingArgs(5, arguments.length);
        const alg = normalizeDeriveAlg(normalizeAlg(algorithm));
        const metadata = requireKey(baseKey, alg.name, "deriveKey");
        const derived = normalizeAlg(derivedKeyType);
        // The derived length comes from the target algorithm (spec "get key length").
        let bits;
        if (AES_ALGS.has(derived.name)) bits = requireAesLength(derived.length);
        else if (derived.name === "ChaCha20-Poly1305") bits = 256;
        else if (KMAC_ALGS.has(derived.name)) {
          bits = derived.length == null ? (derived.name === "KMAC128" ? 128 : 256)
                                        : Number(derived.length);
          if (bits === 0) throw dataError("KmacImportParams.length cannot be 0");
        }
        else if (derived.name === "HMAC") {
          const hash = memberHash(derived);
          bits = derived.length == null ? hashBlockBits[hash.name] : Number(derived.length);
        } else if (derived.name === "HKDF" || derived.name === "PBKDF2") {
          bits = null;   // length-less key material types consume the whole secret
        } else throw notSupported("Unrecognized algorithm name");
        // node re-imports derived material through the explicit secret format
        // (lib/internal/crypto/webcrypto.js deriveKeyImpl → importKeySync).
        return this.importKey("raw-secret", deriveBytes(alg, metadata, bits), derived,
          extractable, keyUsages);
      }

      async wrapKey(format, key, wrappingKey, wrapAlgorithm) {
        if (arguments.length < 4) throw missingArgs(4, arguments.length);
        const alg = normalizeAlg(wrapAlgorithm);
        const wrapper = requireKey(wrappingKey, alg.name, "wrapKey");
        const exported = await this.exportKey(format, key);
        let bytes = `${format}` === "jwk"
          ? new TextEncoder().encode(JSON.stringify(exported))
          : new Uint8Array(exported);
        // AES-KW (RFC 3394) requires 8-byte-aligned input; a serialized JWK rarely
        // is. Node pads the JWK JSON with trailing spaces (harmless for JSON.parse).
        if (`${format}` === "jwk" && alg.name === "AES-KW" && bytes.length % 8 !== 0) {
          const padded = new Uint8Array(Math.ceil(bytes.length / 8) * 8);
          padded.set(bytes);
          padded.fill(0x20, bytes.length);
          bytes = padded;
        }
        return arrayBuffer(encryptBytes(alg, wrapper, bytes));
      }

      async unwrapKey(format, wrappedKey, unwrappingKey, unwrapAlgorithm, unwrappedKeyAlgorithm,
                      extractable, keyUsages) {
        if (arguments.length < 7) throw missingArgs(7, arguments.length);
        const alg = normalizeAlg(unwrapAlgorithm);
        const wrapper = requireKey(unwrappingKey, alg.name, "unwrapKey");
        const bytes = decryptBytes(alg, wrapper, copyBytes(wrappedKey));
        let keyData;
        if (`${format}` === "jwk") {
          let parsed;
          try { parsed = JSON.parse(new TextDecoder().decode(bytes)); }
          catch { throw dataError("Invalid wrapped JWK key"); }
          // A parsed value that is not a JsonWebKey dictionary (missing the
          // required "kty" member) surfaces as a TypeError, per the IDL conversion.
          if (parsed == null || typeof parsed !== "object" || typeof parsed.kty !== "string") {
            throw new TypeError('The provided value is not of type \'JsonWebKey\': missing required member \'kty\'.');
          }
          keyData = parsed;
        } else {
          keyData = bytes;
        }
        return this.importKey(format, keyData, unwrappedKeyAlgorithm, extractable, keyUsages);
      }
    }

    const impl = new SubtleCryptoImpl();

    class SubtleCrypto { constructor() { throw illegalConstructor(); } }
    // The full node method list; the post-quantum encapsulation family is
    // present so the receiver check is observable, but mbun has no ML-KEM, so
    // it reports NotSupportedError rather than a fabricated key.
    const SUBTLE_METHODS = ["decrypt", "decapsulateBits", "decapsulateKey", "deriveBits",
      "deriveKey", "digest", "encapsulateBits", "encapsulateKey", "encrypt", "exportKey",
      "generateKey", "getPublicKey", "importKey", "sign", "unwrapKey", "verify", "wrapKey"];
    for (const method of SUBTLE_METHODS) {
      const body = impl[method];
      const wrapper = { [method](...args) {
        if (this !== subtle) return Promise.reject(invalidThis("SubtleCrypto"));
        if (typeof body !== "function") {
          return Promise.reject(notSupported("Unrecognized algorithm name"));
        }
        try { return Promise.resolve(body.apply(impl, args)); }
        catch (e) { return Promise.reject(e); }
      } }[method];
      defineProperty(SubtleCrypto.prototype, method, {
        configurable: true, writable: true, enumerable: true, value: wrapper,
      });
    }
    defineProperty(SubtleCrypto.prototype, Symbol.toStringTag, {
      configurable: true, value: "SubtleCrypto",
    });
    defineProperty(SubtleCrypto, "supports", {
      configurable: true, writable: true, enumerable: true,
      value: function supports(operation, algorithm) {
        if (this !== SubtleCrypto) throw invalidThis("SubtleCrypto constructor");
        if (arguments.length < 2) throw missingArgs(2, arguments.length);
        try {
          const alg = normalizeAlg(algorithm);
          const table = opAlgs[String(operation)];
          if (table) return table.has(alg.name);
          if (operation === "importKey" || operation === "exportKey" ||
              operation === "generateKey" || operation === "getKeyLength") return true;
          if (operation === "digest") return true;
          return false;
        } catch (e) { return false; }
      },
    });
    const subtle = Object.create(SubtleCrypto.prototype);
    G.SubtleCrypto = SubtleCrypto;

    // ---- Crypto (the globalThis.crypto interface) ----
    // globalThis.crypto is a plain object earlier in the bootstrap; give it the
    // real interface so `Crypto`, the receiver checks and the getRandomValues
    // argument validation behave as node's do.
    const realCrypto = G.crypto;
    const baseGetRandomValues = realCrypto.getRandomValues;
    const baseRandomUUID = realCrypto.randomUUID;
    // getRandomValues only accepts integer-typed views (no Float*/DataView) and
    // caps the request at 65536 bytes. ref: WebCrypto §Crypto-method-getRandomValues.
    const INT_TYPED_VIEWS = new Set(["Int8Array", "Int16Array", "Int32Array", "Uint8Array",
      "Uint16Array", "Uint32Array", "Uint8ClampedArray", "BigInt64Array", "BigUint64Array"]);
    const quotaExceeded = () => {
      const Ctor = G.QuotaExceededError;
      const err = typeof Ctor === "function"
        ? new Ctor("The requested length exceeds 65,536 bytes")
        : domError("The requested length exceeds 65,536 bytes", "QuotaExceededError");
      return err;
    };
    const webGetRandomValues = (array) => {
      const tag = ArrayBuffer.isView(array)
        ? Object.prototype.toString.call(array).slice(8, -1) : "";
      if (!INT_TYPED_VIEWS.has(tag)) {
        throw domError("The provided ArrayBufferView is not an integer-typed array",
          "TypeMismatchError");
      }
      if (array.byteLength > 65536) throw quotaExceeded();
      baseGetRandomValues.call(realCrypto, array);
      return array;
    };
    class Crypto { constructor() { throw illegalConstructor(); } }
    const cryptoBrand = new WeakSet();
    const requireCrypto = (self) => { if (!cryptoBrand.has(self)) throw invalidThis("Crypto"); };
    defineProperty(Crypto.prototype, "subtle", {
      configurable: true, enumerable: true,
      get() { requireCrypto(this); return subtle; },
    });
    defineProperty(Crypto.prototype, "getRandomValues", {
      configurable: true, writable: true, enumerable: true,
      value: function getRandomValues(array) {
        requireCrypto(this);
        if (arguments.length < 1) throw missingArgs(1, arguments.length);
        return webGetRandomValues(array);
      },
    });
    defineProperty(Crypto.prototype, "randomUUID", {
      configurable: true, writable: true, enumerable: true,
      value: function randomUUID() {
        requireCrypto(this);
        return baseRandomUUID.call(realCrypto);
      },
    });
    defineProperty(Crypto.prototype, Symbol.toStringTag, {
      configurable: true, value: "Crypto",
    });
    try {
      delete realCrypto.getRandomValues;
      delete realCrypto.randomUUID;
      delete realCrypto.subtle;
      Object.setPrototypeOf(realCrypto, Crypto.prototype);
      cryptoBrand.add(realCrypto);
      G.Crypto = Crypto;
    } catch (e) {
      // A frozen/exotic host crypto object keeps the legacy own-property shape.
      defineProperty(realCrypto, "subtle", {
        configurable: true, enumerable: true, get: () => subtle, set: () => {},
      });
    }

    // node:crypto <-> WebCrypto bridge: hand the raw key material of a CryptoKey
    // to the node:crypto layer (builtins/crypto_asym.cppm), which owns KeyObject,
    // and build a CryptoKey back from a KeyObject's DER/secret bytes.
    G.__mbunCryptoKeyToKeyObject = (key) => {
      const metadata = keyMetadata.get(key);
      if (!metadata) return undefined;
      return {
        kind: metadata.secret ? "secret" : metadata.type,
        material: metadata.secret ? metadata.secret : metadata.material,
        algorithm: metadata.algorithm,
        usages: metadata.usages,
        extractable: metadata.extractable,
      };
    };
    G.__mbunKeyObjectToCryptoKey = (kind, material, algorithm, extractable, keyUsages) =>
      impl.importKey(kind === "secret" ? "raw-secret" : kind === "public" ? "spki" : "pkcs8",
        material, algorithm, extractable, keyUsages);
    G.__mbunIsCryptoKey = (value) => keyMetadata.has(value);
    delete G.__mbunWebCryptoNative;
  }
)JS";

}  // namespace mbun::jsc::builtins::detail
