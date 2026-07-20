// runtime/node_crypto.inc — node:crypto native backend (globalThis.__mbunCryptoNative).
//
// Backs node:crypto's createHash / createHmac / pbkdf2 / randomBytes /
// randomFillSync / randomUUID and Bun's crypto surface on the pure-C++ mbun.crypto
// algorithms (md5 / sha1 / sha2 family via CryptoHasher; sha3 / shake via Keccak;
// blake2b/2s; RFC 2104 HMAC; RFC 8018 PBKDF2). No OpenSSL / host libs.
// ref: bun src/runtime/crypto (CryptoHasher.rs), src/jsc/bindings/webcrypto/*.
// DEFERRED: md4, sha512-224 (absent from mbun.crypto); asymmetric
// (RSA/EC/x509/generateKeyPair/sign/verify/createCipheriv) — those stay honest
// throwing stubs in the JS layer until an OpenSSL bridge lands.

// Normalize an algorithm spelling to the canonical key used below (lowercase,
// separators removed): sha-256 / SHA256 / sha_256 → "sha256"; sha-512/256 →
// "sha512256"; sha3-256 → "sha3256".
inline std::string crypto_norm_algo(std::string_view name) {
    std::string key;
    key.reserve(name.size());
    for (char ch : name) {
        if (ch == '-' || ch == '_' || ch == '/' || ch == '.') { continue; }
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    // bun/BoringSSL name aliases (verified against real bun 1.4): rmd160 is a
    // synonym for ripemd160; sha128 resolves to sha1.
    if (key == "rmd160") { return "ripemd160"; }
    if (key == "sha128") { return "sha1"; }
    return key;
}

// One-shot digest over `data`. outLen only affects the XOF/variable-length
// algorithms (shake128/256, blake2b256); ignored otherwise. nullopt = the
// algorithm is unknown or DEFERRED.
inline std::optional<std::vector<std::uint8_t>> crypto_digest_bytes(
    const std::string& key, std::span<const std::uint8_t> data, std::size_t outLen) {
    if (auto algo{mbun::crypto::digest_algo_from(key)}) {
        mbun::crypto::CryptoHasher h{*algo};
        h.update(data);
        return h.digest();
    }
    if (key == "md4") { return mbun::crypto::md4(data); }
    if (key == "ripemd160") { return mbun::crypto::ripemd160(data); }
    if (key == "sha3224") { return mbun::crypto::sha3_224(data); }
    if (key == "sha3256") { return mbun::crypto::sha3_256(data); }
    if (key == "sha3384") { return mbun::crypto::sha3_384(data); }
    if (key == "sha3512") { return mbun::crypto::sha3_512(data); }
    if (key == "shake128") { return mbun::crypto::shake128(data, outLen != 0 ? outLen : 16); }
    if (key == "shake256") { return mbun::crypto::shake256(data, outLen != 0 ? outLen : 32); }
    if (key == "blake2b512") { return mbun::crypto::blake2b512(data); }
    if (key == "blake2s256") { return mbun::crypto::blake2s256(data); }
    if (key == "blake2b256") {
        mbun::crypto::Blake2b h{32};
        h.update(data);
        std::vector<std::uint8_t> out(32);
        h.final_(out.data());
        return out;
    }
    return std::nullopt;
}

// HMAC block size (bytes) for the RFC-2104 generic path used for the algorithms
// mbun::crypto::hmac (DigestAlgo only) does not cover. 0 = unsupported.
inline std::size_t crypto_hmac_block_size(const std::string& key) {
    if (key == "md4") { return 64; }
    if (key == "ripemd160") { return 64; }
    if (key == "sha3224") { return 144; }
    if (key == "sha3256") { return 136; }
    if (key == "sha3384") { return 104; }
    if (key == "sha3512") { return 72; }
    if (key == "blake2b512" || key == "blake2b256") { return 128; }
    if (key == "blake2s256") { return 64; }
    return 0;
}

// Cryptographic RNG. mbun::crypto::fill_random seeds ChaCha20 (RFC 8439) from
// std::random_device — the OS entropy source (getrandom / /dev/urandom on POSIX,
// RtlGenRandom on Windows) — with a fresh per-call key, so no vendored lib and no
// host path. Draining random_device 4 bytes at a time (its whole 32-bit range per
// call) cost one OS entropy read per word: filling 1 MB took ~450 ms and made
// crypto.getRandomValues() the dominant cost of large-buffer body tests.
inline void crypto_fill_random(std::uint8_t* p, std::size_t n) {
    mbun::crypto::fill_random(p, n);
}

// __mbunCryptoNative.digest(algorithm, data, outputLength?) → Uint8Array raw
// digest. The JS layer owns encoding (hex/base64/buffer) via Buffer.
JSC_DEFINE_HOST_FUNCTION(node_crypto_digest_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JsStrArg algoArg{global, native_arg(frame, 0)};
    std::string key{crypto_norm_algo(algoArg.view())};
    std::size_t outLen{0};
    JSC::JSValue lenV{native_arg(frame, 2)};
    if (lenV && lenV.isNumber()) { outLen = static_cast<std::size_t>(lenV.asNumber()); }

    std::optional<std::vector<std::uint8_t>> out;
    const bool ok{with_hasher_bytes(
        global, native_arg(frame, 1),
        [&](std::span<const std::uint8_t> b) { out = crypto_digest_bytes(key, b, outLen); })};
    if (!ok) {
        return hasher_throw(global,
                            "The \"data\" argument must be of type string or an instance of "
                            "Buffer, TypedArray, DataView, or ArrayBuffer.");
    }
    if (!out) { return hasher_throw(global, "Digest method not supported"); }
    return JSC::JSValue::encode(make_u8array(global, *out));
}

// __mbunCryptoNative.hmac(algorithm, key, data) → Uint8Array MAC.
JSC_DEFINE_HOST_FUNCTION(node_crypto_hmac_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JsStrArg algoArg{global, native_arg(frame, 0)};
    std::string key{crypto_norm_algo(algoArg.view())};

    std::vector<std::uint8_t> keyBytes;
    std::vector<std::uint8_t> dataBytes;
    const bool kok{with_hasher_bytes(global, native_arg(frame, 1), [&](std::span<const std::uint8_t> b) {
        keyBytes.assign(b.begin(), b.end());
    })};
    const bool dok{with_hasher_bytes(global, native_arg(frame, 2), [&](std::span<const std::uint8_t> b) {
        dataBytes.assign(b.begin(), b.end());
    })};
    if (!kok || !dok) { return hasher_throw(global, "HMAC key/data must be string or BufferSource"); }

    // Fast path: the DigestAlgo family goes through the tested mbun::crypto::hmac.
    if (auto algo{mbun::crypto::digest_algo_from(key)}) {
        if (auto mac{mbun::crypto::hmac(*algo, keyBytes, dataBytes)}) {
            return JSC::JSValue::encode(make_u8array(global, *mac));
        }
    }
    // Generic RFC-2104 path for sha3 / blake2 (not covered by mbun::crypto::hmac).
    const std::size_t block{crypto_hmac_block_size(key)};
    if (block == 0) { return hasher_throw(global, "Digest method not supported"); }
    std::vector<std::uint8_t> k{keyBytes};
    if (k.size() > block) {
        auto kd{crypto_digest_bytes(key, k, 0)};
        if (!kd) { return hasher_throw(global, "Digest method not supported"); }
        k = std::move(*kd);
    }
    std::vector<std::uint8_t> ipad(block, 0x36);
    std::vector<std::uint8_t> opad(block, 0x5c);
    for (std::size_t i{0}; i < k.size(); ++i) {
        ipad[i] ^= k[i];
        opad[i] ^= k[i];
    }
    std::vector<std::uint8_t> inner;
    inner.reserve(block + dataBytes.size());
    inner.insert(inner.end(), ipad.begin(), ipad.end());
    inner.insert(inner.end(), dataBytes.begin(), dataBytes.end());
    auto innerDigest{crypto_digest_bytes(key, inner, 0)};
    if (!innerDigest) { return hasher_throw(global, "Digest method not supported"); }
    std::vector<std::uint8_t> outer;
    outer.reserve(block + innerDigest->size());
    outer.insert(outer.end(), opad.begin(), opad.end());
    outer.insert(outer.end(), innerDigest->begin(), innerDigest->end());
    auto mac{crypto_digest_bytes(key, outer, 0)};
    if (!mac) { return hasher_throw(global, "Digest method not supported"); }
    return JSC::JSValue::encode(make_u8array(global, *mac));
}

// __mbunCryptoNative.pbkdf2(password, salt, iterations, keylen, digest) →
// Uint8Array derived key (DigestAlgo PRFs only; JS falls back for the rest).
JSC_DEFINE_HOST_FUNCTION(node_crypto_pbkdf2_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    std::vector<std::uint8_t> pw;
    std::vector<std::uint8_t> salt;
    const bool pok{with_hasher_bytes(global, native_arg(frame, 0), [&](std::span<const std::uint8_t> b) {
        pw.assign(b.begin(), b.end());
    })};
    const bool sok{with_hasher_bytes(global, native_arg(frame, 1), [&](std::span<const std::uint8_t> b) {
        salt.assign(b.begin(), b.end());
    })};
    if (!pok || !sok) { return hasher_throw(global, "pbkdf2 password/salt must be string or BufferSource"); }
    JSC::JSValue iterV{native_arg(frame, 2)};
    JSC::JSValue lenV{native_arg(frame, 3)};
    const std::uint32_t iterations{iterV.isNumber() ? static_cast<std::uint32_t>(iterV.asNumber()) : 0};
    const double lenD{lenV.isNumber() ? lenV.asNumber() : 0};
    // Defensive bound: the JS layer already rejects keylen outside [0, 2^31); this
    // guards the native reserve() against ever being handed an absurd length.
    if (lenD < 0 || lenD > static_cast<double>(1u << 26)) {
        return hasher_throw(global, "Invalid PBKDF2 parameters");
    }
    const std::size_t keylen{static_cast<std::size_t>(lenD)};
    JsStrArg digestArg{global, native_arg(frame, 4)};
    std::string key{crypto_norm_algo(digestArg.view())};

    auto algo{mbun::crypto::digest_algo_from(key)};
    if (!algo) { return hasher_throw(global, "Digest method not supported"); }
    auto dk{mbun::crypto::pbkdf2(*algo, pw, salt, iterations, keylen)};
    if (!dk) { return hasher_throw(global, "Invalid PBKDF2 parameters"); }
    return JSC::JSValue::encode(make_u8array(global, *dk));
}

// __mbunCryptoNative.randomBytes(size) → Uint8Array of `size` random bytes.
JSC_DEFINE_HOST_FUNCTION(node_crypto_random_bytes_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSC::JSValue nV{native_arg(frame, 0)};
    long n{nV.isNumber() ? static_cast<long>(nV.asNumber()) : 0};
    if (n < 0) { n = 0; }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(n));
    if (!buf.empty()) { crypto_fill_random(buf.data(), buf.size()); }
    return JSC::JSValue::encode(make_u8array(global, buf));
}

// __mbunCryptoNative.randomFillSync(view) → fills the whole TypedArray/DataView
// backing region with random bytes in place and returns it.
JSC_DEFINE_HOST_FUNCTION(node_crypto_random_fill_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSC::JSValue v{native_arg(frame, 0)};
    if (v && v.isCell()) {
        if (auto* view{WTF::dynamicDowncast<JSC::JSArrayBufferView>(v.asCell())}) {
            if (!view->isDetached() && view->vector() != nullptr && view->byteLength() != 0) {
                crypto_fill_random(static_cast<std::uint8_t*>(view->vector()), view->byteLength());
            }
        }
    }
    return JSC::JSValue::encode(v);
}

// __mbunCryptoNative.randomUUID() → RFC 4122 v4 UUID string.
JSC_DEFINE_HOST_FUNCTION(node_crypto_random_uuid_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    std::uint8_t b[16];
    crypto_fill_random(b, 16);
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0f) | 0x40);  // version 4
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3f) | 0x80);  // RFC 4122 variant
    static constexpr char HEX[]{"0123456789abcdef"};
    std::string s;
    s.reserve(36);
    for (int i{0}; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) { s.push_back('-'); }
        s.push_back(HEX[b[i] >> 4]);
        s.push_back(HEX[b[i] & 0x0f]);
    }
    return JSC::JSValue::encode(toJS(global, make_string(toRef(global), s)));
}
