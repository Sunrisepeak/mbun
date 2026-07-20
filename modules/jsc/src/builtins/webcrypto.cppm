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
    function CryptoKey() { throw new TypeError("Illegal constructor"); }
    CryptoKey.prototype = NativeCryptoKey.prototype;
    const keyMetadata = new WeakMap();
    const freeze = Object.freeze;
    const preventExtensions = Object.preventExtensions;
    const defineProperty = Object.defineProperty;
    const usageNames = ["decrypt", "deriveBits", "deriveKey", "encrypt", "sign",
      "unwrapKey", "verify", "wrapKey"];
    const usageSet = new Set(usageNames);
    const metadataFor = (key) => {
      const metadata = keyMetadata.get(key);
      if (!metadata) throw new TypeError("Illegal invocation");
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
    freeze(CryptoKey);
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
        return new Uint8Array(value.slice(0));
      }
      if (ArrayBuffer.isView(value)) {
        if (value.byteLength > MAX_BUFFER_BYTES) throw operationError("Data is too large");
        return new Uint8Array(value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength));
      }
      throw new TypeError("keyData and data must be a BufferSource");
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
    const hashNames = { SHA1: "SHA-1", "SHA-1": "SHA-1", SHA224: "SHA-224", "SHA-224": "SHA-224",
      SHA256: "SHA-256", "SHA-256": "SHA-256", SHA384: "SHA-384", "SHA-384": "SHA-384",
      SHA512: "SHA-512", "SHA-512": "SHA-512",
      "SHA3-256": "SHA3-256", SHA3256: "SHA3-256", "SHA3-384": "SHA3-384", SHA3384: "SHA3-384",
      "SHA3-512": "SHA3-512", SHA3512: "SHA3-512" };
    const normalizeHash = (hash) => {
      const value = normalizeAlgorithm(hash);
      const key = String(value.name || "").toUpperCase().replace(/_/g, "-");
      const name = hashNames[key];
      if (!name) throw notSupported("Unrecognized hash algorithm");
      return { name };
    };
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
      "AES-CBC", "AES-GCM", "AES-KW", "HMAC", "PBKDF2", "HKDF", "Ed25519", "X25519"];
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
    const restrictUsages = (usages, allowed, what) => {
      for (const usage of usages) {
        if (!allowed.includes(usage)) {
          throw domError("The requested operation is not valid for " + what, "SyntaxError");
        }
      }
    };
    const intersectUsages = (usages, allowed) => sortUsages(usages.filter((u) => allowed.includes(u)));

    const SIGN_ALGS = new Set(["RSASSA-PKCS1-v1_5", "RSA-PSS", "ECDSA", "Ed25519", "HMAC"]);
    const DERIVE_ALGS = new Set(["ECDH", "X25519", "PBKDF2", "HKDF"]);
    const AES_ALGS = new Set(["AES-CTR", "AES-CBC", "AES-GCM", "AES-KW"]);
    const RSA_ALGS = new Set(["RSASSA-PKCS1-v1_5", "RSA-PSS", "RSA-OAEP"]);
    const EC_ALGS = new Set(["ECDSA", "ECDH"]);
    const OKP_ALGS = new Set(["Ed25519", "X25519"]);
    // Legal usages per algorithm/key-type (spec operation tables).
    const allowedUsagesFor = (name, type) => {
      if (name === "HMAC") return ["sign", "verify"];
      if (SIGN_ALGS.has(name)) return type === "public" ? ["verify"] : ["sign"];
      if (name === "RSA-OAEP") {
        return type === "public" ? ["encrypt", "wrapKey"] : ["decrypt", "unwrapKey"];
      }
      if (name === "AES-KW") return ["wrapKey", "unwrapKey"];
      if (AES_ALGS.has(name)) return ["encrypt", "decrypt", "wrapKey", "unwrapKey"];
      if (DERIVE_ALGS.has(name)) return type === "public" ? [] : ["deriveKey", "deriveBits"];
      return [];
    };

    // ---- curves ----
    const curveToGroup = { "P-256": "prime256v1", "P-384": "secp384r1", "P-521": "secp521r1" };
    const groupToCurve = { prime256v1: "P-256", secp384r1: "P-384", secp521r1: "P-521" };
    const curveBytes = { "P-256": 32, "P-384": 48, "P-521": 66 };
    const normalizeCurve = (namedCurve) => {
      const name = String(namedCurve);
      if (!curveToGroup[name]) throw notSupported("Unrecognized namedCurve");
      return name;
    };

    // ---- CryptoKey construction ----
    // `extra` carries the opaque material: { material } (DER) or { secret } (raw).
    const makeKey = (type, algorithm, extractable, usages, extra) => {
      const key = Object.create(CryptoKey.prototype);
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
    const opAlgs = {
      sign: SIGN_ALGS, verify: SIGN_ALGS,
      encrypt: new Set(["RSA-OAEP", "AES-CTR", "AES-CBC", "AES-GCM"]),
      decrypt: new Set(["RSA-OAEP", "AES-CTR", "AES-CBC", "AES-GCM"]),
      deriveBits: DERIVE_ALGS, deriveKey: DERIVE_ALGS,
      wrapKey: new Set(["RSA-OAEP", "AES-CTR", "AES-CBC", "AES-GCM", "AES-KW"]),
      unwrapKey: new Set(["RSA-OAEP", "AES-CTR", "AES-CBC", "AES-GCM", "AES-KW"]),
    };
    // Per-operation wording of the InvalidAccessError messages, verbatim from
    // bun src/jsc/bindings/webcrypto/SubtleCrypto.cpp (node's webcrypto tests
    // match on these strings).
    const usageSubject = {
      encrypt: "CryptoKey", decrypt: "CryptoKey", sign: "CryptoKey", verify: "CryptoKey",
      deriveBits: "CryptoKey", deriveKey: "CryptoKey",
      wrapKey: "Wrapping CryptoKey", unwrapKey: "Unwrapping CryptoKey",
    };
    const usageMatchee = {
      encrypt: "AlgorithmIdentifier", decrypt: "AlgorithmIdentifier",
      sign: "AlgorithmIdentifier", verify: "AlgorithmIdentifier",
      deriveBits: "AlgorithmIdentifier", deriveKey: "AlgorithmIdentifier",
      wrapKey: "AlgorithmIdentifier", unwrapKey: "unwrap AlgorithmIdentifier",
    };
    const usageNoun = {
      encrypt: "encryption", decrypt: "decryption", sign: "signing", verify: "verification",
      deriveBits: "bits derivation", deriveKey: "CryptoKey derivation",
      wrapKey: "wrapKey operation", unwrapKey: "unwrapKey operation",
    };
    // A key of `name` usable for `usage`. Mirrors bun's layering:
    //   not a CryptoKey at all  → TypeError  (the IDL binding's brand check)
    //   algorithm lacks the op  → NotSupportedError
    //   right key, wrong alg/usage → InvalidAccessError
    const requireKey = (key, name, usage) => {
      const supported = opAlgs[usage];
      if (supported && !supported.has(name)) {
        throw notSupported(name + " does not support " + usage);
      }
      const metadata = keyMetadata.get(key);
      // Object.create(CryptoKey.prototype) has the prototype but no material:
      // bun rejects it at the binding layer with a TypeError.
      if (!metadata) {
        throw new TypeError("Argument 2 ('key') to SubtleCrypto." + usage +
          " must be an instance of CryptoKey");
      }
      if (metadata.algorithm.name !== name) {
        throw domError(usageSubject[usage] + " doesn't match " + usageMatchee[usage],
          "InvalidAccessError");
      }
      if (!metadata.usages.includes(usage)) {
        throw domError(usageSubject[usage] + " doesn't support " + usageNoun[usage],
          "InvalidAccessError");
      }
      return metadata;
    };

    // Cross-check imported DER against the requested algorithm; without this an
    // RSA SPKI would happily import as an ECDSA key.
    const checkMaterial = (material, name, alg) => {
      let info;
      try { info = AN().keyType(material, "", true); }
      catch (e) { throw dataError("Invalid key data"); }
      const type = info.type;
      if (RSA_ALGS.has(name)) {
        if (type !== "rsa" && type !== "rsa-pss") throw dataError("Key is not an RSA key");
        return { modulusLength: info.modulusLength };
      }
      if (EC_ALGS.has(name)) {
        if (type !== "ec") throw dataError("Key is not an EC key");
        const curve = groupToCurve[info.namedCurve] || info.namedCurve;
        if (alg.namedCurve != null && normalizeCurve(alg.namedCurve) !== curve) {
          throw dataError("Key curve does not match namedCurve");
        }
        return { namedCurve: curve };
      }
      if (OKP_ALGS.has(name)) {
        if (type !== name.toLowerCase()) throw dataError("Key is not an " + name + " key");
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
    // JWK "alg" (RFC 7518 §3.1/§4.1). Only the RSA/AES/HMAC export operations
    // set it: the spec's EC and Ed25519 exportKey steps emit no "alg" at all,
    // and bun 1.4.0 agrees (verified against .mbun/bin/bun-rust).
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
      if (name === "AES-KW") return "A" + algorithm.length + "KW";
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
      if (raw.byteLength === 0) throw dataError("HMAC key is empty");
      if (declaredLength != null && Number(declaredLength) !== raw.byteLength * 8) {
        throw dataError("HMAC key length does not match key data");
      }
      const usageBits = (usages.includes("sign") ? 1 : 0) | (usages.includes("verify") ? 2 : 0);
      if (usageBits === 0) throw domError("HMAC key usages are empty", "SyntaxError");
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
        throw operationError("AES key length must be 128, 192 or 256 bits");
      }
      return n;
    };

    // ---- generateKey ----
    const generateRsa = (alg, name, extractable, usages) => {
      const hash = normalizeHash(alg.hash);
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
                                    : ["sign", "verify"], "an " + name + " key");
      const privUsages = intersectUsages(usages, allowedUsagesFor(name, "private"));
      if (privUsages.length === 0) throw domError("Key usages are empty", "SyntaxError");
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
        "an " + name + " key");
      const privUsages = intersectUsages(usages, allowedUsagesFor(name, "private"));
      if (name === "ECDSA" && privUsages.length === 0) {
        throw domError("Key usages are empty", "SyntaxError");
      }
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
      restrictUsages(usages, name === "Ed25519" ? ["sign", "verify"] : ["deriveKey", "deriveBits"],
        "an " + name + " key");
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
    const RSA_PKCS1_PADDING = 1, RSA_PKCS1_OAEP_PADDING = 4, RSA_PKCS1_PSS_PADDING = 6;
    const SALTLEN_DIGEST = -1;
    const signBytes = (alg, metadata, data) => {
      const name = metadata.algorithm.name;
      if (name === "RSASSA-PKCS1-v1_5") {
        return AN().sign(mdName(metadata.algorithm.hash), data, metadata.material, "",
          RSA_PKCS1_PADDING, SALTLEN_DIGEST, "");
      }
      if (name === "RSA-PSS") {
        const saltLength = Number(alg.saltLength);
        if (!Number.isInteger(saltLength) || saltLength < 0) throw new TypeError("Invalid saltLength");
        return AN().sign(mdName(metadata.algorithm.hash), data, metadata.material, "",
          RSA_PKCS1_PSS_PADDING, saltLength, "");
      }
      if (name === "ECDSA") {
        // WebCrypto ECDSA signatures are raw r||s (IEEE P1363), never DER.
        return AN().sign(mdName(normalizeHash(alg.hash)), data, metadata.material, "",
          RSA_PKCS1_PADDING, SALTLEN_DIGEST, "ieee-p1363");
      }
      if (name === "Ed25519") {
        return AN().sign("", data, metadata.material, "", RSA_PKCS1_PADDING, SALTLEN_DIGEST, "");
      }
      throw notSupported("Unsupported signing algorithm");
    };
    const verifyBytes = (alg, metadata, signature, data) => {
      const name = metadata.algorithm.name;
      if (name === "RSASSA-PKCS1-v1_5") {
        return AN().verify(mdName(metadata.algorithm.hash), data, metadata.material, "", signature,
          RSA_PKCS1_PADDING, SALTLEN_DIGEST, "");
      }
      if (name === "RSA-PSS") {
        const saltLength = Number(alg.saltLength);
        if (!Number.isInteger(saltLength) || saltLength < 0) throw new TypeError("Invalid saltLength");
        return AN().verify(mdName(metadata.algorithm.hash), data, metadata.material, "", signature,
          RSA_PKCS1_PSS_PADDING, saltLength, "");
      }
      if (name === "ECDSA") {
        // A wrong-sized r||s is a plain `false`, never an exception (spec).
        const size = curveBytes[metadata.algorithm.namedCurve];
        if (size != null && signature.length !== size * 2) return false;
        return AN().verify(mdName(normalizeHash(alg.hash)), data, metadata.material, "", signature,
          RSA_PKCS1_PADDING, SALTLEN_DIGEST, "ieee-p1363");
      }
      if (name === "Ed25519") {
        return AN().verify("", data, metadata.material, "", signature, RSA_PKCS1_PADDING,
          SALTLEN_DIGEST, "");
      }
      throw notSupported("Unsupported verification algorithm");
    };

    // ---- encrypt / decrypt ----
    const aesGcmTagBits = (alg) => {
      const bits = alg.tagLength == null ? 128 : Number(alg.tagLength);
      if (![32, 64, 96, 104, 112, 120, 128].includes(bits)) {
        throw operationError("Invalid AES-GCM tagLength");
      }
      return bits;
    };
    const aesCtrCounter = (alg) => {
      const counter = copyBytes(alg.counter);
      if (counter.length !== 16) throw operationError("AES-CTR counter must be 16 bytes");
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
      const length = Number(alg.length);
      if (!Number.isInteger(length) || length < 1 || length > 128) {
        throw operationError("AES-CTR counter length must be between 1 and 128");
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
      if (name === "AES-GCM") {
        const iv = copyBytes(alg.iv);
        if (iv.length === 0) throw operationError("AES-GCM iv must not be empty");
        const aad = alg.additionalData != null ? copyBytes(alg.additionalData) : null;
        const res = AN().cipher(aesCipherName(metadata.algorithm, "gcm"), metadata.secret, iv,
          data, aad, true, null, aesGcmTagBits(alg) / 8);
        // WebCrypto AES-GCM returns ciphertext || tag.
        return concatBytes(new Uint8Array(res.data), new Uint8Array(res.tag));
      }
      if (name === "AES-CBC") {
        const iv = copyBytes(alg.iv);
        if (iv.length !== 16) throw operationError("AES-CBC iv must be 16 bytes");
        return new Uint8Array(AN().cipher(aesCipherName(metadata.algorithm, "cbc"), metadata.secret,
          iv, data, null, true, null, 16).data);
      }
      if (name === "AES-CTR") {
        return aesCtrCrypt(metadata, alg, data);
      }
      throw notSupported("Unsupported encryption algorithm");
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
      if (name === "AES-GCM") {
        const iv = copyBytes(alg.iv);
        if (iv.length === 0) throw operationError("AES-GCM iv must not be empty");
        const tagBytes = aesGcmTagBits(alg) / 8;
        if (data.length < tagBytes) throw operationError("Ciphertext is shorter than the tag");
        const aad = alg.additionalData != null ? copyBytes(alg.additionalData) : null;
        try {
          return new Uint8Array(AN().cipher(aesCipherName(metadata.algorithm, "gcm"),
            metadata.secret, iv, data.subarray(0, data.length - tagBytes), aad, false,
            data.subarray(data.length - tagBytes), tagBytes).data);
        } catch (e) { throw operationError("Authentication tag verification failed"); }
      }
      if (name === "AES-CBC") {
        const iv = copyBytes(alg.iv);
        if (iv.length !== 16) throw operationError("AES-CBC iv must be 16 bytes");
        try {
          return new Uint8Array(AN().cipher(aesCipherName(metadata.algorithm, "cbc"),
            metadata.secret, iv, data, null, false, null, 16).data);
        } catch (e) { throw operationError("Decryption failed"); }
      }
      if (name === "AES-CTR") {
        // CTR decryption is identical to encryption (XOR against the keystream).
        return aesCtrCrypt(metadata, alg, data);
      }
      throw notSupported("Unsupported decryption algorithm");
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
      if (nbytes > secret.length) throw operationError("Invalid derived length");
      const out = secret.slice(0, nbytes);
      const rem = lengthBits % 8;
      if (rem !== 0 && nbytes > 0) out[nbytes - 1] &= (0xff << (8 - rem)) & 0xff;
      return out;
    };
    const deriveBytes = (alg, metadata, lengthBits) => {
      const name = metadata.algorithm.name;
      if (name === "PBKDF2") {
        const hash = normalizeHash(alg.hash);
        const iterations = Number(alg.iterations);
        if (!Number.isInteger(iterations) || iterations <= 0) {
          throw operationError("PBKDF2 iterations must be a positive integer");
        }
        if (lengthBits == null || lengthBits % 8 !== 0) throw operationError("Invalid derived length");
        if (lengthBits === 0) return new Uint8Array(0);   // zero bits after param validation
        return new Uint8Array(nodeCrypto().pbkdf2Sync(G.Buffer.from(metadata.secret),
          G.Buffer.from(copyBytes(alg.salt)), iterations, lengthBits / 8, mdName(hash)));
      }
      if (name === "HKDF") {
        const hash = normalizeHash(alg.hash);
        if (lengthBits == null || lengthBits % 8 !== 0) throw operationError("Invalid derived length");
        if (lengthBits === 0) return new Uint8Array(0);
        return hkdf(hash.name, metadata.secret, copyBytes(alg.salt), copyBytes(alg.info),
          lengthBits / 8);
      }
      if (name === "ECDH") {
        const peer = keyMetadata.get(alg.public);
        if (!peer || peer.algorithm.name !== "ECDH" || peer.type !== "public") {
          throw domError("public must be an ECDH public key", "InvalidAccessError");
        }
        if (peer.algorithm.namedCurve !== metadata.algorithm.namedCurve) {
          throw domError("Curve mismatch", "InvalidAccessError");
        }
        // The scalar lives inside the PKCS8 DER; recover it via the JWK bridge.
        const priv = unb64u(jwkFromDer(metadata.material, false).d);
        const secret = new Uint8Array(AN().ecdhComputeSecret(curveToGroup[metadata.algorithm.namedCurve],
          priv, ecPointFromMaterial(peer.material)));
        return extractDerivedBits(secret, lengthBits);
      }
      if (name === "X25519") {
        // The base key must be the private half and `public` the peer's public
        // half; both are DER, so the scalar never surfaces in JS.
        // ref: bun CryptoAlgorithmX25519.cpp deriveBits.
        if (metadata.type !== "private") {
          throw domError("baseKey must be an X25519 private key", "InvalidAccessError");
        }
        const peer = keyMetadata.get(alg.public);
        if (!peer || peer.algorithm.name !== "X25519" || peer.type !== "public") {
          throw domError("public must be an X25519 public key", "InvalidAccessError");
        }
        let secret;
        // A small-order/all-zero peer point makes the native derive fail
        // (RFC 7748 section 6.1) — that is an OperationError, not a crash.
        try { secret = new Uint8Array(AN().okpDerive(metadata.material, peer.material)); }
        catch (e) { throw operationError("X25519 derivation failed"); }
        return extractDerivedBits(secret, lengthBits);
      }
      throw notSupported("Unsupported derivation algorithm");
    };

    class SubtleCrypto {
      async digest(algorithm, data) {
        const name = normalizeHash(algorithm).name;
        return arrayBuffer(G.Bun.CryptoHasher.hash(name, copyBytes(data)));
      }

      async generateKey(algorithm, extractable, keyUsages) {
        if (arguments.length < 3) throw new TypeError("Not enough arguments");
        const alg = normalizeAlg(algorithm);
        const usages = normalizeUsages(keyUsages);
        const name = alg.name;
        if (RSA_ALGS.has(name)) return generateRsa(alg, name, extractable, usages);
        if (EC_ALGS.has(name)) return generateEc(alg, name, extractable, usages);
        if (OKP_ALGS.has(name)) return generateOkp(name, extractable, usages);
        if (name === "HMAC") {
          const hash = normalizeHash(alg.hash);
          restrictUsages(usages, ["sign", "verify"], "an HMAC key");
          const bits = alg.length == null ? hashBlockBits[hash.name] : Number(alg.length);
          if (!Number.isInteger(bits) || bits === 0 || bits % 8 !== 0) {
            throw operationError("Invalid HMAC key length");
          }
          return importHmacKey(randomBytes(bits / 8), hash, extractable, usages, null);
        }
        if (AES_ALGS.has(name)) {
          const length = requireAesLength(alg.length);
          restrictUsages(usages, allowedUsagesFor(name, "secret"), "an " + name + " key");
          if (usages.length === 0) throw domError("Key usages are empty", "SyntaxError");
          return makeKey("secret", { name, length }, extractable, usages,
            { secret: randomBytes(length / 8) });
        }
        throw notSupported("Unsupported algorithm for generateKey");
      }

      async importKey(format, keyData, algorithm, extractable, keyUsages) {
        if (arguments.length < 5) throw new TypeError("Not enough arguments");
        const convertedFormat = `${format}`;
        if (convertedFormat !== "raw" && convertedFormat !== "spki" &&
            convertedFormat !== "pkcs8" && convertedFormat !== "jwk") {
          throw new TypeError('format must be one of "raw", "spki", "pkcs8", or "jwk"');
        }
        const alg = normalizeAlg(algorithm);
        const usages = normalizeUsages(keyUsages);
        const name = alg.name;

        const octFromJwk = (what) => {
          if (keyData == null || keyData.kty !== "oct" || typeof keyData.k !== "string") {
            throw dataError("Invalid JWK for " + what);
          }
          return unb64u(keyData.k);
        };

        if (name === "HMAC") {
          if (convertedFormat !== "raw" && convertedFormat !== "jwk") {
            throw notSupported("HMAC keys can only be imported from raw or jwk");
          }
          restrictUsages(usages, ["sign", "verify"], "an HMAC key");
          const hash = normalizeHash(alg.hash);
          const raw = convertedFormat === "jwk" ? octFromJwk("HMAC") : copyBytes(keyData);
          return importHmacKey(raw, hash, extractable, usages, alg.length);
        }

        if (AES_ALGS.has(name)) {
          if (convertedFormat !== "raw" && convertedFormat !== "jwk") {
            throw notSupported("AES keys can only be imported from raw or jwk");
          }
          const raw = convertedFormat === "jwk" ? octFromJwk("AES") : copyBytes(keyData);
          // WebCrypto ignores alg.length on raw/jwk import — key length derives
          // from the data (requireAesLength already validates 128/192/256). ref:
          // bun CryptoKeyAES.cpp importRaw (no parameters.length comparison).
          const length = requireAesLength(raw.byteLength * 8);
          restrictUsages(usages, allowedUsagesFor(name, "secret"), "an " + name + " key");
          return makeKey("secret", { name, length }, extractable, usages, { secret: raw });
        }

        if (name === "PBKDF2" || name === "HKDF") {
          if (convertedFormat !== "raw") {
            throw notSupported(name + " keys can only be imported from raw");
          }
          restrictUsages(usages, ["deriveKey", "deriveBits"], "a " + name + " key");
          if (extractable) throw domError(name + " keys must not be extractable", "SyntaxError");
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
          if (keyData == null || typeof keyData !== "object") throw dataError("Invalid JWK");
          // Validate the JWK against the requested algorithm/usages (spec import
          // steps): kty must match the family, key_ops (if present) must be a
          // superset of the requested usages, and ext:false forbids an
          // extractable import.
          const wantKty = RSA_ALGS.has(name) ? "RSA" : EC_ALGS.has(name) ? "EC"
            : OKP_ALGS.has(name) ? "OKP" : null;
          if (wantKty && keyData.kty !== wantKty) throw dataError("Invalid JWK: kty must be " + wantKty);
          if (Array.isArray(keyData.key_ops)) {
            for (const u of usages) {
              if (!keyData.key_ops.includes(u)) throw dataError("JWK key_ops does not include the requested usage");
            }
          }
          if (keyData.ext === false && extractable) throw dataError("JWK ext is false but an extractable key was requested");
          const isPrivate = typeof keyData.d === "string";
          try { material = new Uint8Array(jwkToDer(keyData, isPrivate)); }
          catch (e) { throw dataError("Invalid JWK: " + String((e && e.message) || e)); }
          type = isPrivate ? "private" : "public";
        } else {
          // raw: EC / OKP public keys only (spec).
          if (EC_ALGS.has(name)) {
            material = new Uint8Array(ecKeyFromPoint(copyBytes(keyData), normalizeCurve(alg.namedCurve)));
          } else if (OKP_ALGS.has(name)) {
            material = new Uint8Array(jwkToDer({ kty: "OKP", crv: name, x: b64u(copyBytes(keyData)) }, false));
          } else {
            throw notSupported("Unsupported raw key import for " + name);
          }
          type = "public";
        }
        const info = checkMaterial(material, name, alg);
        restrictUsages(usages, allowedUsagesFor(name, type), "an " + name + " " + type + " key");

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
          keyAlgorithm.hash = freeze({ name: normalizeHash(alg.hash).name });
        } else if (EC_ALGS.has(name)) {
          keyAlgorithm.namedCurve = info.namedCurve;
        }
        return makeKey(type, keyAlgorithm, type === "public" ? true : extractable, usages,
          { material });
      }

      async exportKey(format, key) {
        if (arguments.length < 2) throw new TypeError("Not enough arguments");
        const convertedFormat = `${format}`;
        const metadata = keyMetadata.get(key);
        if (!metadata) throw new TypeError("Invalid CryptoKey");
        if (!metadata.extractable) {
          throw domError("The CryptoKey is not extractable", "InvalidAccessError");
        }
        const name = metadata.algorithm.name;
        if (convertedFormat === "raw") {
          if (metadata.secret) return arrayBuffer(metadata.secret);
          if (metadata.type === "public" && EC_ALGS.has(name)) {
            return arrayBuffer(ecPointFromMaterial(metadata.material));
          }
          if (metadata.type === "public" && OKP_ALGS.has(name)) {
            return arrayBuffer(unb64u(jwkFromDer(metadata.material, true).x));
          }
          throw notSupported("Raw export is not supported for this key");
        }
        if (convertedFormat === "spki") {
          if (metadata.type !== "public") throw notSupported("Only public keys can be exported as spki");
          return arrayBuffer(AN().keyExport(metadata.material, "", true, "spki", "der", "", ""));
        }
        if (convertedFormat === "pkcs8") {
          if (metadata.type !== "private") throw notSupported("Only private keys can be exported as pkcs8");
          return arrayBuffer(AN().keyExport(metadata.material, "", false, "pkcs8", "der", "", ""));
        }
        if (convertedFormat === "jwk") {
          const jwk = metadata.secret
            ? { kty: "oct", k: b64u(metadata.secret) }
            : jwkFromDer(metadata.material, metadata.type === "public");
          const alg = jwkAlgFor(metadata.algorithm);
          if (alg !== undefined) jwk.alg = alg;
          jwk.key_ops = Array.from(metadata.usages);
          jwk.ext = metadata.extractable;
          return sortJwk(jwk);
        }
        throw new TypeError('format must be one of "raw", "spki", "pkcs8", or "jwk"');
      }

      async sign(algorithm, key, data) {
        if (arguments.length < 3) throw new TypeError("Not enough arguments");
        const alg = normalizeAlg(algorithm);
        const metadata = requireKey(key, alg.name, "sign");
        const bytes = copyBytes(data);
        if (alg.name === "HMAC") return arrayBuffer(hmacRaw(metadata.algorithm.hash.name, metadata.secret, bytes));
        if (metadata.type !== "private") {
          throw domError("The key is not a private key", "InvalidAccessError");
        }
        return arrayBuffer(signBytes(alg, metadata, bytes));
      }

      async verify(algorithm, key, signature, data) {
        if (arguments.length < 4) throw new TypeError("Not enough arguments");
        const alg = normalizeAlg(algorithm);
        const metadata = requireKey(key, alg.name, "verify");
        const sig = copyBytes(signature);
        const bytes = copyBytes(data);
        if (alg.name === "HMAC") {
          const expected = hmacRaw(metadata.algorithm.hash.name, metadata.secret, bytes);
          if (expected.length !== sig.length) return false;
          let diff = 0;
          for (let i = 0; i < expected.length; i++) diff |= expected[i] ^ sig[i];
          return diff === 0;
        }
        if (metadata.type !== "public") {
          throw domError("The key is not a public key", "InvalidAccessError");
        }
        return verifyBytes(alg, metadata, sig, bytes);
      }

      async encrypt(algorithm, key, data) {
        if (arguments.length < 3) throw new TypeError("Not enough arguments");
        const alg = normalizeAlg(algorithm);
        return arrayBuffer(encryptBytes(alg, requireKey(key, alg.name, "encrypt"), copyBytes(data)));
      }

      async decrypt(algorithm, key, data) {
        if (arguments.length < 3) throw new TypeError("Not enough arguments");
        const alg = normalizeAlg(algorithm);
        return arrayBuffer(decryptBytes(alg, requireKey(key, alg.name, "decrypt"), copyBytes(data)));
      }

      async deriveBits(algorithm, baseKey, length = null) {
        if (arguments.length < 2) throw new TypeError("Not enough arguments");
        const alg = normalizeAlg(algorithm);
        const metadata = requireKey(baseKey, alg.name, "deriveBits");
        const bits = length == null ? null : Number(length);
        if (bits != null && (!Number.isInteger(bits) || bits < 0)) {
          throw operationError("Invalid derived length");
        }
        return arrayBuffer(deriveBytes(alg, metadata, bits));
      }

      async deriveKey(algorithm, baseKey, derivedKeyType, extractable, keyUsages) {
        if (arguments.length < 5) throw new TypeError("Not enough arguments");
        const alg = normalizeAlg(algorithm);
        const metadata = requireKey(baseKey, alg.name, "deriveKey");
        const derived = normalizeAlg(derivedKeyType);
        // The derived length comes from the target algorithm (spec "get key length").
        let bits;
        if (AES_ALGS.has(derived.name)) bits = requireAesLength(derived.length);
        else if (derived.name === "HMAC") {
          const hash = normalizeHash(derived.hash);
          bits = derived.length == null ? hashBlockBits[hash.name] : Number(derived.length);
        } else if (derived.name === "HKDF" || derived.name === "PBKDF2") {
          bits = null;   // length-less key material types consume the whole secret
        } else throw notSupported("Unsupported derivedKeyType");
        return this.importKey("raw", deriveBytes(alg, metadata, bits), derived, extractable, keyUsages);
      }

      async wrapKey(format, key, wrappingKey, wrapAlgorithm) {
        if (arguments.length < 4) throw new TypeError("Not enough arguments");
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
        if (arguments.length < 7) throw new TypeError("Not enough arguments");
        const alg = normalizeAlg(unwrapAlgorithm);
        const wrapper = requireKey(unwrappingKey, alg.name, "unwrapKey");
        const bytes = decryptBytes(alg, wrapper, copyBytes(wrappedKey));
        let keyData;
        if (`${format}` === "jwk") {
          let parsed;
          try { parsed = JSON.parse(new TextDecoder().decode(bytes)); }
          catch { throw dataError("Unwrapped key is not valid JSON"); }
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

    const subtle = new SubtleCrypto();
    G.SubtleCrypto = SubtleCrypto;
    Object.defineProperty(G.crypto, "subtle", {
      configurable: true,
      enumerable: true,
      get: () => subtle,
      set: () => {},
    });
    delete G.__mbunWebCryptoNative;
  }
)JS";

}  // namespace mbun::jsc::builtins::detail
