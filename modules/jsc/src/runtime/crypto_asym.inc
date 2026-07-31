// runtime/crypto_asym.inc — node:crypto ASYMMETRIC + cipher backend over the
// vendored OpenSSL 3.1.5 static lib (mbun.openssl local-index package; NOT host
// /usr). Exposes globalThis.__mbunCryptoAsymNative with the EVP-backed
// primitives the JS layer (builtins/crypto_asym.cppm) wires onto node:crypto:
//   generateKeyPair (rsa/ec/ed25519, PEM/DER, pkcs1/pkcs8/sec1/spki, encrypted)
//   sign / verify   (RSA PKCS1+PSS, ECDSA der/ieee-p1363, Ed25519)
//   publicEncrypt / privateDecrypt (RSA PKCS1 + OAEP)
//   cipher          (aes-*-cbc/gcm/ctr/ecb one-shot; JS buffers update/final)
//   ecdh            (generate / computeSecret / convertKey)
//   keyExport / keyType   (createPublicKey/createPrivateKey/KeyObject)
//   x509parse             (X509Certificate fields)
// ref: bun-ref src/js/node/crypto.ts + src/runtime/crypto/crypto.classes.ts;
//      node lib/internal/crypto/*. Modern OSSL 3.x provider API (OSSL_ENCODER/
//      DECODER/PARAM_BLD) — no deprecated EC_KEY/RSA legacy structs.
// DEFERRED: createDiffieHellman (modp DH), scrypt-via-openssl (JS backend fine),
//           X509 checkIP edge encodings.

// __mbunCryptoAsymNative helpers -------------------------------------------

// Uint8Array of `b` (raw bytes back to JS as a Buffer-able view).
inline JSObjectRef asym_u8(JSContextRef ctx, std::span<const std::uint8_t> b) {
    JSObjectRef arr{JSObjectMakeTypedArray(ctx, kJSTypedArrayTypeUint8Array, b.size(), nullptr)};
    if (arr != nullptr && !b.empty()) {
        if (void* p{JSObjectGetTypedArrayBytesPtr(ctx, arr, nullptr)}) {
            std::memcpy(p, b.data(), b.size());
        }
    }
    return arr;
}

// Copy a BufferSource/string arg into a byte vector. false = wrong type.
template <class V>
inline bool asym_read(JSC::JSGlobalObject* g, V v, std::vector<std::uint8_t>& out) {
    return with_hasher_bytes(g, v, [&](std::span<const std::uint8_t> b) { out.assign(b.begin(), b.end()); });
}

// Optional string arg (empty view when absent/null/undefined).
inline std::string asym_str(JSC::JSGlobalObject* g, JSC::JSValue v) {
    if (!v || v.isUndefinedOrNull() || !v.isString()) { return {}; }
    JsStrArg s{g, v};
    return std::string{s.view()};
}

inline int asym_int(JSC::JSValue v, int dflt) {
    return (v && v.isNumber()) ? static_cast<int>(v.asNumber()) : dflt;
}

// Optional string arg: nullopt when absent/null/undefined, the (possibly EMPTY)
// string when one was passed. The distinction matters for passphrases — node
// treats `passphrase: ''` (decrypt with an empty password, which is a real and
// valid password) and "no passphrase at all" (refuse, never prompt) as two
// different requests, with two different errors.
using AsymPass = std::optional<std::string>;
inline AsymPass asym_opt_str(JSC::JSGlobalObject* g, JSC::JSValue v) {
    if (!v || v.isUndefinedOrNull() || !v.isString()) { return std::nullopt; }
    JsStrArg s{g, v};
    return std::string{s.view()};
}

// Last OpenSSL error string (drained), for honest throw messages.
inline std::string asym_ossl_err() {
    unsigned long code{::ERR_get_error()};
    if (code == 0) { return "operation failed"; }
    char buf[256];
    ::ERR_error_string_n(code, buf, sizeof(buf));
    while (::ERR_get_error() != 0) {}  // drain the rest
    return std::string{buf};
}

// --- OpenSSL RAII ---
struct EvpPkeyDel { void operator()(EVP_PKEY* p) const { if (p) EVP_PKEY_free(p); } };
struct EvpMdCtxDel { void operator()(EVP_MD_CTX* p) const { if (p) EVP_MD_CTX_free(p); } };
struct EvpCipherCtxDel { void operator()(EVP_CIPHER_CTX* p) const { if (p) EVP_CIPHER_CTX_free(p); } };
struct EvpPkeyCtxDel { void operator()(EVP_PKEY_CTX* p) const { if (p) EVP_PKEY_CTX_free(p); } };
using PkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDel>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDel>;
using CipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, EvpCipherCtxDel>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, EvpPkeyCtxDel>;

// PEM passphrase callback for "we have no passphrase": returning 0 makes OpenSSL
// fail the decode instead of prompting on the terminal.
inline int asym_no_pass_cb(char*, int, int, void*) { return 0; }

// True when the material is an ENCRYPTED private-key PEM. Both spellings are
// covered: PKCS#8 "BEGIN ENCRYPTED PRIVATE KEY" and legacy PKCS#1/SEC1
// "Proc-Type: 4,ENCRYPTED" + DEK-Info.
inline bool asym_is_encrypted_pem(std::span<const std::uint8_t> data) {
    const std::string_view text{reinterpret_cast<const char*>(data.data()), data.size()};
    if (text.find("-----BEGIN ") == std::string_view::npos) { return false; }
    return text.find("ENCRYPTED PRIVATE KEY-----") != std::string_view::npos ||
           text.find("Proc-Type: 4,ENCRYPTED") != std::string_view::npos ||
           text.find("Proc-Type:4,ENCRYPTED") != std::string_view::npos;
}

// True when the material is a DER EncryptedPrivateKeyInfo (RFC 5208 §6) — the
// structure is SEQUENCE { AlgorithmIdentifier, OCTET STRING }, which is exactly
// what X509_SIG parses; a plain PrivateKeyInfo (INTEGER version first) does not.
inline bool asym_is_encrypted_der(std::span<const std::uint8_t> data) {
    if (data.empty() || data[0] != 0x30) { return false; }
    const unsigned char* p{data.data()};
    X509_SIG* sig{d2i_X509_SIG(nullptr, &p, static_cast<long>(data.size()))};
    ERR_clear_error();
    if (sig == nullptr) { return false; }
    X509_SIG_free(sig);
    return true;
}

// The message node produces when a key load fails, so the JS layer can map it to
// node's error surface. With no passphrase supplied:
//   * encrypted PEM  → node's OSSL_DECODER passphrase callback refuses rather
//     than prompting, and OpenSSL 3 reports that as CRYPTO "interrupted or
//     cancelled" (ERR_OSSL_CRYPTO_INTERRUPTED_OR_CANCELLED).
//   * encrypted DER PKCS#8 → detected structurally before any decode attempt,
//     which node reports as ERR_MISSING_PASSPHRASE.
// Otherwise the REAL OpenSSL error is preferred over the caller's wording: node's
// ThrowCryptoError reports the decoder's own string verbatim, and the corpus
// compares it literally — a wrong passphrase has to read
// `error:1C800064:Provider routines::bad decrypt`, not "invalid key". Surfacing
// it also gives the JS layer an `error:XXXXXXXX:` message it can decorate with
// .code/.reason/.library, which is what node's `library` property is. The
// caller's wording remains the fallback for a failure that left the queue empty.
//
// `preferOssl` is OPT-IN per call site, and deliberately so. The JS layer's
// asymParseError() (crypto_asym.cppm) distinguishes createPublicKey from
// createPrivateKey by seeing a bare, code-less message and attaching the code
// node's own loader would have produced (EVP "decode error" vs DECODER
// "unsupported") -- three PQC key-object files depend on exactly that. Handing
// those call sites a real OpenSSL string instead makes the message self-coding
// and that branch never fires. So only the sign / verify / cipher / raw-RSA
// paths, whose errors node passes through verbatim, ask for the real string.
inline std::string asym_key_error(std::span<const std::uint8_t> data, bool hasPass,
                                  std::string_view fallback, bool preferOssl = false) {
    if (!hasPass) {
        if (asym_is_encrypted_pem(data)) {
            return "error:07880109:common libcrypto routines::interrupted or cancelled";
        }
        if (asym_is_encrypted_der(data)) { return "Passphrase required for encrypted key"; }
    }
    if (preferOssl) {
        const std::string ossl{asym_ossl_err()};
        if (ossl != "operation failed") { return ossl; }
    }
    return std::string{fallback};
}

// Decode a key from PEM or DER (public or private), optional passphrase. Tries a
// private-capable selection first, then public-only (SPKI). Returns nullptr on
// failure. An engaged-but-empty `pass` is an empty PASSWORD (node allows keys
// encrypted under one); a disengaged `pass` means "never ask, never guess".
inline EVP_PKEY* asym_load_pkey(std::span<const std::uint8_t> data, const AsymPass& pass) {
    // Every decode below is speculative, so even a SUCCESSFUL load leaves the
    // failed attempts sitting on OpenSSL's error queue. asym_ossl_err() pops the
    // OLDEST entry, so without this the next genuine failure gets reported with a
    // stale message — a leftover DECODER "unsupported" standing in for the EVP
    // error that actually happened. Clear on the way out of every success path.
    const auto loaded{[](EVP_PKEY* p) { ERR_clear_error(); return p; }};
    for (int sel : {EVP_PKEY_KEYPAIR, EVP_PKEY_PUBLIC_KEY}) {
        EVP_PKEY* pkey{nullptr};
        OSSL_DECODER_CTX* d{
            OSSL_DECODER_CTX_new_for_pkey(&pkey, nullptr, nullptr, nullptr, sel, nullptr, nullptr)};
        if (d != nullptr) {
            if (pass.has_value()) {
                OSSL_DECODER_CTX_set_passphrase(
                    d, reinterpret_cast<const unsigned char*>(pass->data()), pass->size());
            }
            const unsigned char* p{data.data()};
            std::size_t len{data.size()};
            OSSL_DECODER_from_data(d, &p, &len);
            OSSL_DECODER_CTX_free(d);
        }
        if (pkey != nullptr) { return loaded(pkey); }
    }
    // OSSL_DECODER reads only the FIRST PEM block; a SEC1 EC key file that leads
    // with an "EC PARAMETERS" block makes it stop on the params and fail.
    // PEM_read_bio_PrivateKey skips non-key PEM blocks and reads the key that
    // follows. ref: bun-ref ncrypto.cpp EVPKeyPointer::TryParsePrivateKey.
    // With no passphrase this fallback is skipped for encrypted PEM entirely:
    // OpenSSL 3's PEM_read_bio_PrivateKey routes the pem_password_cb through the
    // decoder bridge, where a callback that answers "0 bytes" is taken as a valid
    // EMPTY password — so a key encrypted under an empty password would decrypt
    // for a caller that supplied no passphrase at all. node refuses that.
    if (pass.has_value() || !asym_is_encrypted_pem(data)) {
        std::string p{pass.value_or(std::string{})};
        BIO* pb{BIO_new_mem_buf(data.data(), static_cast<int>(data.size()))};
        if (pb != nullptr) {
            // cb=nullptr would fall back to PEM_def_callback, which PROMPTS ON THE
            // TERMINAL ("Enter PEM pass phrase:") for an encrypted key — a hang in
            // any non-interactive run. With no passphrase, refuse instead.
            EVP_PKEY* pk{!pass.has_value()
                             ? PEM_read_bio_PrivateKey(pb, nullptr, asym_no_pass_cb, nullptr)
                             : PEM_read_bio_PrivateKey(pb, nullptr, nullptr, p.data())};
            BIO_free(pb);
            if (pk != nullptr) { return loaded(pk); }
        }
    }
    // Fallback: the material may be an X.509 certificate (node's publicEncrypt /
    // createPublicKey accept a cert and use its SubjectPublicKeyInfo).
    X509* cert{nullptr};
    BIO* bio{BIO_new_mem_buf(data.data(), static_cast<int>(data.size()))};
    if (bio != nullptr) { cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr); BIO_free(bio); }
    if (cert == nullptr) {
        const unsigned char* p{data.data()};
        cert = d2i_X509(nullptr, &p, static_cast<long>(data.size()));
    }
    if (cert != nullptr) {
        EVP_PKEY* pub{X509_get_pubkey(cert)};
        X509_free(cert);
        return loaded(pub);
    }
    return nullptr;
}

// RSA raw private-encrypt (EVP_PKEY_sign, no digest) / public-decrypt
// (verify_recover). node's privateEncrypt/publicDecrypt legacy surface.
template <bool PrivateEncrypt>
inline JSC::EncodedJSValue asym_rsa_raw_host(JSC::JSGlobalObject* global, JSC::CallFrame* frame) {
    JSContextRef ctx{toRef(global)};
    std::vector<std::uint8_t> keyBytes;
    std::vector<std::uint8_t> data;
    if (!asym_read(global, native_arg(frame, 0), keyBytes) ||
        !asym_read(global, native_arg(frame, 2), data)) {
        return hasher_throw(global, "key and data are required");
    }
    const AsymPass pass{asym_opt_str(global, native_arg(frame, 1))};
    const int padding{asym_int(native_arg(frame, 3), RSA_PKCS1_PADDING)};
    PkeyPtr pkey{asym_load_pkey(keyBytes, pass)};
    if (!pkey) { return hasher_throw(global, asym_key_error(keyBytes, pass.has_value(), "invalid key", true)); }
    PkeyCtxPtr pctx{EVP_PKEY_CTX_new(pkey.get(), nullptr)};
    if (!pctx) { return hasher_throw(global, "rsa ctx failed"); }
    int init{0};
    if constexpr (PrivateEncrypt) { init = EVP_PKEY_sign_init(pctx.get()); }
    else { init = EVP_PKEY_verify_recover_init(pctx.get()); }
    if (init <= 0 || EVP_PKEY_CTX_set_rsa_padding(pctx.get(), padding) <= 0) {
        return hasher_throw(global, "rsa raw init failed");
    }
    std::size_t outlen{0};
    if constexpr (PrivateEncrypt) {
        EVP_PKEY_sign(pctx.get(), nullptr, &outlen, data.data(), data.size());
    } else {
        EVP_PKEY_verify_recover(pctx.get(), nullptr, &outlen, data.data(), data.size());
    }
    std::vector<std::uint8_t> out(outlen);
    int rc{0};
    if constexpr (PrivateEncrypt) {
        rc = EVP_PKEY_sign(pctx.get(), out.data(), &outlen, data.data(), data.size());
    } else {
        rc = EVP_PKEY_verify_recover(pctx.get(), out.data(), &outlen, data.data(), data.size());
    }
    if (rc <= 0) { return hasher_throw(global, "rsa raw failed: " + asym_ossl_err()); }
    out.resize(outlen);
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(asym_u8(ctx, out))));
}

JSC_DEFINE_HOST_FUNCTION(node_asym_private_encrypt_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    return asym_rsa_raw_host<true>(global, frame);
}
JSC_DEFINE_HOST_FUNCTION(node_asym_public_decrypt_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    return asym_rsa_raw_host<false>(global, frame);
}

// __mbunCryptoAsymNative.ecValidCurve(name) → bool (createECDH curve check).
JSC_DEFINE_HOST_FUNCTION(node_asym_ec_valid_curve_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    const std::string curve{asym_str(global, native_arg(frame, 0))};
    const int nid{OBJ_txt2nid(curve.c_str())};
    bool ok{false};
    if (nid != NID_undef) {
        EC_GROUP* g{EC_GROUP_new_by_curve_name(nid)};
        if (g != nullptr) { ok = true; EC_GROUP_free(g); }
    }
    return JSC::JSValue::encode(JSC::jsBoolean(ok));
}

// Encode an EVP_PKEY to PEM/DER with the given selection + ASN.1 structure,
// optionally cipher-encrypting the private key. nullopt on failure.
inline std::optional<std::vector<std::uint8_t>> asym_encode_pkey(
    EVP_PKEY* pkey, int selection, const char* structure, bool pem,
    std::string_view cipher, std::string_view pass) {
    // OSSL_ENCODER's legacy ("type-specific"/PKCS#1/SEC1) PEM writer collects the
    // passphrase through a callback and treats a zero-length answer as a failed
    // read (PEM routines::read key). An EMPTY passphrase is a legitimate request
    // though — node encrypts with it (nodejs/node#35898) — so route that one case
    // through PEM_write_bio_PrivateKey_traditional, which takes an explicit
    // zero-length key string instead of asking.
    if (pem && !cipher.empty() && pass.empty() && selection == EVP_PKEY_KEYPAIR &&
        structure != nullptr && std::string_view{structure} == "type-specific") {
        const EVP_CIPHER* c{EVP_get_cipherbyname(std::string{cipher}.c_str())};
        if (c == nullptr) { return std::nullopt; }
        BIO* b{BIO_new(BIO_s_mem())};
        if (b == nullptr) { return std::nullopt; }
        static const unsigned char kEmpty[1]{0};
        std::vector<std::uint8_t> v;
        if (PEM_write_bio_PrivateKey_traditional(b, pkey, c, kEmpty, 0, nullptr, nullptr) != 0) {
            char* d{nullptr};
            const long n{BIO_get_mem_data(b, &d)};
            if (n > 0 && d != nullptr) { v.assign(d, d + n); }
        }
        BIO_free(b);
        if (v.empty()) { return std::nullopt; }
        return v;
    }
    OSSL_ENCODER_CTX* e{
        OSSL_ENCODER_CTX_new_for_pkey(pkey, selection, pem ? "PEM" : "DER", structure, nullptr)};
    if (e == nullptr) { return std::nullopt; }
    if (!cipher.empty()) {
        OSSL_ENCODER_CTX_set_cipher(e, std::string{cipher}.c_str(), nullptr);
        OSSL_ENCODER_CTX_set_passphrase(
            e, reinterpret_cast<const unsigned char*>(pass.data()), pass.size());
    }
    unsigned char* out{nullptr};
    std::size_t outlen{0};
    const int ok{OSSL_ENCODER_to_data(e, &out, &outlen)};
    OSSL_ENCODER_CTX_free(e);
    if (ok == 0 || out == nullptr) { return std::nullopt; }
    std::vector<std::uint8_t> v(out, out + outlen);
    OPENSSL_free(out);
    return v;
}

// Map node type/format spellings → OSSL selection + structure. Returns false for
// combinations OSSL can't express (caller throws).
inline bool asym_enc_map(std::string_view type, bool priv, int& sel, const char*& structure) {
    if (priv) {
        sel = EVP_PKEY_KEYPAIR;
        if (type == "pkcs8" || type.empty()) { structure = "PrivateKeyInfo"; return true; }
        if (type == "pkcs1" || type == "sec1" || type == "type-specific") { structure = "type-specific"; return true; }
        return false;
    }
    sel = EVP_PKEY_PUBLIC_KEY;
    if (type == "spki" || type.empty()) { structure = "SubjectPublicKeyInfo"; return true; }
    if (type == "pkcs1" || type == "type-specific") { structure = "type-specific"; return true; }
    return false;
}

#include "crypto_asym_keygen.inc"

// __mbunCryptoAsymNative.generateKeyPair(type, modLen, namedCurve, pubType,
//   pubFmt, privType, privFmt, cipher, passphrase, divisorLength,
//   publicExponent, pssDigest, pssMgf1Digest, pssSaltLength)
//   → {publicKey, privateKey}.
// publicKey/privateKey are strings (PEM) or Uint8Array (DER).
JSC_DEFINE_HOST_FUNCTION(node_asym_generate_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    const std::string type{asym_str(global, native_arg(frame, 0))};
    const int modLen{asym_int(native_arg(frame, 1), 2048)};
    const std::string curve{asym_str(global, native_arg(frame, 2))};
    const std::string pubType{asym_str(global, native_arg(frame, 3))};
    const std::string pubFmt{asym_str(global, native_arg(frame, 4))};
    const std::string privType{asym_str(global, native_arg(frame, 5))};
    const std::string privFmt{asym_str(global, native_arg(frame, 6))};
    const std::string cipher{asym_str(global, native_arg(frame, 7))};
    const std::string pass{asym_str(global, native_arg(frame, 8))};
    const int divLen{asym_int(native_arg(frame, 9), 0)};
    const int publicExponent{asym_int(native_arg(frame, 10), 65537)};
    const std::string pssDigest{asym_str(global, native_arg(frame, 11))};
    const std::string pssMgf1Digest{asym_str(global, native_arg(frame, 12))};
    const int pssSaltLength{asym_int(native_arg(frame, 13), -1)};

    PkeyPtr pkey;
    if (type == "rsa") {
        pkey.reset(asym_rsa_gen(static_cast<std::size_t>(modLen > 0 ? modLen : 2048),
                               static_cast<unsigned long>(publicExponent)));
    } else if (type == "rsa-pss") {
        pkey.reset(asym_rsa_pss_gen(static_cast<std::size_t>(modLen > 0 ? modLen : 2048),
                                    pssDigest, pssMgf1Digest, pssSaltLength));
    } else if (type == "dsa") {
        pkey.reset(asym_ffc_gen("DSA", static_cast<unsigned int>(modLen > 0 ? modLen : 2048),
                                static_cast<unsigned int>(divLen > 0 ? divLen : 0)));
    } else if (type == "dh") {
        pkey.reset(asym_ffc_gen("DH", static_cast<unsigned int>(modLen > 0 ? modLen : 2048), 0));
    } else if (type == "ec") {
        if (curve.empty()) { return hasher_throw(global, "namedCurve is required for ec keys"); }
        pkey.reset(EVP_EC_gen(curve.c_str()));
    } else if (type == "ed25519") {
        pkey.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"));
    } else if (type == "ed448") {
        pkey.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED448"));
    } else if (type == "x25519") {
        pkey.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "X25519"));
    } else if (type == "x448") {
        pkey.reset(EVP_PKEY_Q_keygen(nullptr, nullptr, "X448"));
    } else {
        return hasher_throw(global, "Unsupported key type: " + type);
    }
    if (!pkey) { return hasher_throw(global, "key generation failed: " + asym_ossl_err()); }

    int pubSel{0};
    int privSel{0};
    const char* pubStruct{nullptr};
    const char* privStruct{nullptr};
    if (!asym_enc_map(pubType, false, pubSel, pubStruct) ||
        !asym_enc_map(privType, true, privSel, privStruct)) {
        return hasher_throw(global, "Unsupported key encoding");
    }
    const bool pubPem{pubFmt != "der"};
    const bool privPem{privFmt != "der"};
    auto pub{asym_encode_pkey(pkey.get(), pubSel, pubStruct, pubPem, {}, {})};
    auto priv{asym_encode_pkey(pkey.get(), privSel, privStruct, privPem, cipher, pass)};
    if (!pub || !priv) { return hasher_throw(global, "key encoding failed: " + asym_ossl_err()); }

    JSObjectRef obj{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, obj, "publicKey",
             pubPem ? make_string(ctx, std::string_view{
                          reinterpret_cast<const char*>(pub->data()), pub->size()})
                    : static_cast<JSValueRef>(asym_u8(ctx, *pub)));
    set_prop(ctx, obj, "privateKey",
             privPem ? make_string(ctx, std::string_view{
                           reinterpret_cast<const char*>(priv->data()), priv->size()})
                     : static_cast<JSValueRef>(asym_u8(ctx, *priv)));
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(obj)));
}

// ECDSA DER signature ↔ raw r||s (ieee-p1363) conversion.
inline std::vector<std::uint8_t> asym_ecdsa_der_to_p1363(std::span<const std::uint8_t> der,
                                                         std::size_t fieldBytes) {
    const unsigned char* p{der.data()};
    ECDSA_SIG* sig{d2i_ECDSA_SIG(nullptr, &p, static_cast<long>(der.size()))};
    if (sig == nullptr) { return {}; }
    const BIGNUM* r{nullptr};
    const BIGNUM* s{nullptr};
    ECDSA_SIG_get0(sig, &r, &s);
    std::vector<std::uint8_t> out(fieldBytes * 2, 0);
    BN_bn2binpad(r, out.data(), static_cast<int>(fieldBytes));
    BN_bn2binpad(s, out.data() + fieldBytes, static_cast<int>(fieldBytes));
    ECDSA_SIG_free(sig);
    return out;
}

inline std::vector<std::uint8_t> asym_ecdsa_p1363_to_der(std::span<const std::uint8_t> raw) {
    if (raw.size() % 2 != 0) { return {}; }
    const std::size_t f{raw.size() / 2};
    BIGNUM* r{BN_bin2bn(raw.data(), static_cast<int>(f), nullptr)};
    BIGNUM* s{BN_bin2bn(raw.data() + f, static_cast<int>(f), nullptr)};
    ECDSA_SIG* sig{ECDSA_SIG_new()};
    ECDSA_SIG_set0(sig, r, s);  // takes ownership of r,s
    unsigned char* out{nullptr};
    const int len{i2d_ECDSA_SIG(sig, &out)};
    std::vector<std::uint8_t> v;
    if (len > 0 && out != nullptr) { v.assign(out, out + len); OPENSSL_free(out); }
    ECDSA_SIG_free(sig);
    return v;
}

// Configure RSA padding/salt on a signing/verifying pkey ctx.
// `padding == 0` means the caller named none (the real RSA_*_PADDING values start
// at 1). node leaves the choice to the key in that case, and it matters: an
// rsa-pss key REJECTS RSA_PKCS1_PADDING, so hard-defaulting to PKCS1 made every
// unqualified sign/verify with an rsa-pss key die on "illegal or unsupported
// padding mode". `saltLen == kAsymSaltLenUnset` likewise means "don't touch it",
// so a parameter-restricted rsa-pss key keeps the salt length baked into its own
// parameters instead of being overridden with SALTLEN_MAX.
inline constexpr int kAsymSaltLenUnset{-0x7fffffff};
inline bool asym_apply_rsa_sig_opts(EVP_PKEY_CTX* pctx, EVP_PKEY* pkey, int padding, int saltLen) {
    const int base{EVP_PKEY_base_id(pkey)};
    if (base != EVP_PKEY_RSA && base != EVP_PKEY_RSA_PSS) { return true; }
    if (padding == 0) {
        padding = (base == EVP_PKEY_RSA_PSS) ? RSA_PKCS1_PSS_PADDING : RSA_PKCS1_PADDING;
    }
    if (EVP_PKEY_CTX_set_rsa_padding(pctx, padding) <= 0) { return false; }
    if (padding == RSA_PKCS1_PSS_PADDING && saltLen != kAsymSaltLenUnset) {
        return EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, saltLen) > 0;
    }
    return true;
}

// __mbunCryptoAsymNative.sign(digest, data, key, passphrase, padding, saltLen,
//   dsaEncoding) → Uint8Array signature.
JSC_DEFINE_HOST_FUNCTION(node_asym_sign_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    const std::string digest{asym_str(global, native_arg(frame, 0))};
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> keyBytes;
    if (!asym_read(global, native_arg(frame, 1), data) ||
        !asym_read(global, native_arg(frame, 2), keyBytes)) {
        return hasher_throw(global, "sign: data and key are required");
    }
    const AsymPass pass{asym_opt_str(global, native_arg(frame, 3))};
    const int padding{asym_int(native_arg(frame, 4), RSA_PKCS1_PADDING)};
    const int saltLen{asym_int(native_arg(frame, 5), RSA_PSS_SALTLEN_DIGEST)};
    const std::string dsaEnc{asym_str(global, native_arg(frame, 6))};

    PkeyPtr pkey{asym_load_pkey(keyBytes, pass)};
    if (!pkey) {
        return hasher_throw(global,
                            asym_key_error(keyBytes, pass.has_value(), "sign: invalid private key", true));
    }
    MdCtxPtr md{EVP_MD_CTX_new()};
    const EVP_MD* dg{digest.empty() ? nullptr : EVP_get_digestbyname(digest.c_str())};
    if (!digest.empty() && dg == nullptr) { return hasher_throw(global, "Unknown digest: " + digest); }
    // A parameter-restricted RSA-PSS key can reject BOTH the salt length and the
    // digest, and which refusal the caller sees is not arbitrary: node hashes
    // first and then signs the digest through EVP_PKEY_sign, applying the padding
    // and salt length BEFORE binding the message digest
    // (src/crypto/crypto_sig.cc Node_SignFinal → ApplyRSAOptions, then
    // EVP_PKEY_CTX_set_signature_md). EVP_DigestSignInit binds the digest first
    // and so always reports "digest not allowed", masking "pss saltlen too small"
    // whenever both are wrong. Settle the RSA options against their own context
    // first, in node's order, so the reported failure is the one node reports.
    {
        PkeyCtxPtr probe{EVP_PKEY_CTX_new(pkey.get(), nullptr)};
        if (probe && EVP_PKEY_sign_init(probe.get()) > 0 &&
            !asym_apply_rsa_sig_opts(probe.get(), pkey.get(), padding, saltLen)) {
            return hasher_throw(global, "sign init failed: " + asym_ossl_err());
        }
        ERR_clear_error();
    }
    EVP_PKEY_CTX* pctx{nullptr};
    if (EVP_DigestSignInit(md.get(), &pctx, dg, nullptr, pkey.get()) <= 0) {
        return hasher_throw(global, "sign init failed: " + asym_ossl_err());
    }
    if (!asym_apply_rsa_sig_opts(pctx, pkey.get(), padding, saltLen)) {
        return hasher_throw(global, "sign init failed: " + asym_ossl_err());
    }
    std::size_t siglen{0};
    if (EVP_DigestSign(md.get(), nullptr, &siglen, data.data(), data.size()) <= 0) {
        return hasher_throw(global, "sign failed: " + asym_ossl_err());
    }
    std::vector<std::uint8_t> sig(siglen);
    if (EVP_DigestSign(md.get(), sig.data(), &siglen, data.data(), data.size()) <= 0) {
        return hasher_throw(global, "sign failed: " + asym_ossl_err());
    }
    sig.resize(siglen);
    if (EVP_PKEY_base_id(pkey.get()) == EVP_PKEY_EC && dsaEnc == "ieee-p1363") {
        sig = asym_ecdsa_der_to_p1363(sig, asym_field_bytes(pkey.get()));
    }
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(asym_u8(ctx, sig))));
}

// __mbunCryptoAsymNative.verify(digest, data, key, passphrase, sig, padding,
//   saltLen, dsaEncoding) → bool.
JSC_DEFINE_HOST_FUNCTION(node_asym_verify_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    const std::string digest{asym_str(global, native_arg(frame, 0))};
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> keyBytes;
    std::vector<std::uint8_t> sig;
    if (!asym_read(global, native_arg(frame, 1), data) ||
        !asym_read(global, native_arg(frame, 2), keyBytes) ||
        !asym_read(global, native_arg(frame, 4), sig)) {
        return hasher_throw(global, "verify: data, key and signature are required");
    }
    const AsymPass pass{asym_opt_str(global, native_arg(frame, 3))};
    const int padding{asym_int(native_arg(frame, 5), RSA_PKCS1_PADDING)};
    const int saltLen{asym_int(native_arg(frame, 6), RSA_PSS_SALTLEN_DIGEST)};
    const std::string dsaEnc{asym_str(global, native_arg(frame, 7))};

    PkeyPtr pkey{asym_load_pkey(keyBytes, pass)};
    if (!pkey) {
        return hasher_throw(global, asym_key_error(keyBytes, pass.has_value(), "verify: invalid key", true));
    }
    if (EVP_PKEY_base_id(pkey.get()) == EVP_PKEY_EC && dsaEnc == "ieee-p1363") {
        sig = asym_ecdsa_p1363_to_der(sig);
    }
    MdCtxPtr md{EVP_MD_CTX_new()};
    const EVP_MD* dg{digest.empty() ? nullptr : EVP_get_digestbyname(digest.c_str())};
    if (!digest.empty() && dg == nullptr) { return hasher_throw(global, "Unknown digest: " + digest); }
    EVP_PKEY_CTX* pctx{nullptr};
    if (EVP_DigestVerifyInit(md.get(), &pctx, dg, nullptr, pkey.get()) <= 0) {
        return hasher_throw(global, "verify init failed: " + asym_ossl_err());
    }
    asym_apply_rsa_sig_opts(pctx, pkey.get(), padding, saltLen);
    const int rc{EVP_DigestVerify(md.get(), sig.data(), sig.size(), data.data(), data.size())};
    return JSC::JSValue::encode(JSC::jsBoolean(rc == 1));
}

// Shared RSA encrypt/decrypt (init = EVP_PKEY_encrypt_init/decrypt_init).
inline std::optional<std::vector<std::uint8_t>> asym_rsa_crypt(
    EVP_PKEY* pkey, bool encrypt, std::span<const std::uint8_t> in, int padding,
    const std::string& oaepHash, std::span<const std::uint8_t> label) {
    PkeyCtxPtr pctx{EVP_PKEY_CTX_new(pkey, nullptr)};
    if (!pctx) { return std::nullopt; }
    const int init{encrypt ? EVP_PKEY_encrypt_init(pctx.get()) : EVP_PKEY_decrypt_init(pctx.get())};
    if (init <= 0) { return std::nullopt; }
    if (EVP_PKEY_CTX_set_rsa_padding(pctx.get(), padding) <= 0) { return std::nullopt; }
    if (padding == RSA_PKCS1_OAEP_PADDING) {
        const EVP_MD* md{oaepHash.empty() ? EVP_sha1() : EVP_get_digestbyname(oaepHash.c_str())};
        if (md == nullptr) { return std::nullopt; }
        EVP_PKEY_CTX_set_rsa_oaep_md(pctx.get(), md);
        if (!label.empty()) {
            unsigned char* lbl{static_cast<unsigned char*>(OPENSSL_memdup(label.data(), label.size()))};
            if (EVP_PKEY_CTX_set0_rsa_oaep_label(pctx.get(), lbl, static_cast<int>(label.size())) <= 0) {
                OPENSSL_free(lbl);
                return std::nullopt;
            }
        }
    }
    std::size_t outlen{0};
    const auto op{encrypt ? EVP_PKEY_encrypt : EVP_PKEY_decrypt};
    if (op(pctx.get(), nullptr, &outlen, in.data(), in.size()) <= 0) { return std::nullopt; }
    std::vector<std::uint8_t> out(outlen);
    if (op(pctx.get(), out.data(), &outlen, in.data(), in.size()) <= 0) { return std::nullopt; }
    out.resize(outlen);
    return out;
}

// __mbunCryptoAsymNative.publicEncrypt(key, passphrase, data, padding, oaepHash,
//   oaepLabel) → Uint8Array.  privateDecrypt shares the impl via `decrypt` flag.
template <bool Decrypt>
inline JSC::EncodedJSValue asym_rsa_host(JSC::JSGlobalObject* global, JSC::CallFrame* frame) {
    JSContextRef ctx{toRef(global)};
    std::vector<std::uint8_t> keyBytes;
    std::vector<std::uint8_t> data;
    if (!asym_read(global, native_arg(frame, 0), keyBytes) ||
        !asym_read(global, native_arg(frame, 2), data)) {
        return hasher_throw(global, "key and data are required");
    }
    const AsymPass pass{asym_opt_str(global, native_arg(frame, 1))};
    const int padding{asym_int(native_arg(frame, 3),
                               Decrypt ? RSA_PKCS1_PADDING : RSA_PKCS1_OAEP_PADDING)};
    const std::string oaepHash{asym_str(global, native_arg(frame, 4))};
    std::vector<std::uint8_t> label;
    asym_read(global, native_arg(frame, 5), label);

    PkeyPtr pkey{asym_load_pkey(keyBytes, pass)};
    if (!pkey) { return hasher_throw(global, asym_key_error(keyBytes, pass.has_value(), "invalid key", true)); }
    auto out{asym_rsa_crypt(pkey.get(), !Decrypt, data, padding, oaepHash, label)};
    if (!out) { return hasher_throw(global, (Decrypt ? "decryption failed: " : "encryption failed: ") + asym_ossl_err()); }
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(asym_u8(ctx, *out))));
}

JSC_DEFINE_HOST_FUNCTION(node_asym_public_encrypt_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    return asym_rsa_host<false>(global, frame);
}
JSC_DEFINE_HOST_FUNCTION(node_asym_private_decrypt_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    return asym_rsa_host<true>(global, frame);
}

// __mbunCryptoAsymNative.cipher(algo, key, iv, data, aad, isEncrypt, tagIn,
//   tagLen) → { data, tag }. One-shot; JS buffers the streaming update/final.
JSC_DEFINE_HOST_FUNCTION(node_asym_cipher_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    const std::string algo{asym_str(global, native_arg(frame, 0))};
    std::vector<std::uint8_t> key;
    std::vector<std::uint8_t> iv;
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> aad;
    std::vector<std::uint8_t> tagIn;
    asym_read(global, native_arg(frame, 1), key);
    asym_read(global, native_arg(frame, 2), iv);
    asym_read(global, native_arg(frame, 3), data);
    asym_read(global, native_arg(frame, 4), aad);
    const bool enc{[&] { JSC::JSValue v{native_arg(frame, 5)}; return v && v.toBoolean(global); }()};
    asym_read(global, native_arg(frame, 6), tagIn);
    const int tagLen{asym_int(native_arg(frame, 7), 16)};
    // Arg 8 (optional): disable PKCS#7 padding. Used by the AES-KW (RFC 3394) JS
    // layer, which drives raw 16-byte ECB blocks and must not add a padding block.
    const bool noPad{[&] { JSC::JSValue v{native_arg(frame, 8)}; return v && v.toBoolean(global); }()};

    const EVP_CIPHER* c{EVP_get_cipherbyname(algo.c_str())};
    if (c == nullptr) { return hasher_throw(global, "Unknown cipher: " + algo); }
    const int mode{EVP_CIPHER_mode(c)};
    // AEAD ciphers: a variable-length IV, associated data, and an authentication
    // tag on top of the ciphertext. Enumerated explicitly rather than via
    // EVP_CIPH_FLAG_AEAD_CIPHER, which OpenSSL also sets on the stitched
    // aes-*-cbc-hmac-sha* ciphers (which are NOT tag-producing here).
    const bool ocb{mode == EVP_CIPH_OCB_MODE};
    const bool gcm{mode == EVP_CIPH_GCM_MODE};
    const bool aead{gcm || ocb || EVP_CIPHER_nid(c) == NID_chacha20_poly1305};
    // Key-length validation (node throws for a wrong-sized key/iv).
    if (key.size() != static_cast<std::size_t>(EVP_CIPHER_key_length(c))) {
        return hasher_throw(global, "Invalid key length");
    }
    if (aead) {
        if (iv.empty()) { return hasher_throw(global, "Invalid IV length"); }
    } else if (mode == EVP_CIPH_ECB_MODE) {
        if (!iv.empty()) { return hasher_throw(global, "Invalid IV length"); }
    } else if (iv.size() != static_cast<std::size_t>(EVP_CIPHER_iv_length(c))) {
        return hasher_throw(global, "Invalid IV length");
    }

    CipherCtxPtr cc{EVP_CIPHER_CTX_new()};
    if (EVP_CipherInit_ex(cc.get(), c, nullptr, nullptr, nullptr, enc ? 1 : 0) <= 0) {
        return hasher_throw(global, "cipher init failed");
    }
    if (aead) {
        EVP_CIPHER_CTX_ctrl(cc.get(), EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(iv.size()), nullptr);
    }
    // GCM infers its tag length from the tag the caller hands over; every OTHER
    // AEAD has it FIXED at init. node does this unconditionally in
    // InitAuthenticated (src/crypto/crypto_cipher.cc: "In CCM and OCB mode, [the
    // authentication tag length] must always be given by the user"), and for
    // ChaCha20-Poly1305 it pins auth_tag_len to 16 before the EVP_CTRL_AEAD_SET_TAG
    // declaration.
    //
    // SECURITY: this is what makes a decrypt with a MISSING tag fail. OpenSSL
    // leaves chacha20-poly1305's taglen at 0 until told, and its final does
    // `CRYPTO_memcmp(computed, ctx->buf, taglen)` — a 0-length compare is EQUAL,
    // so EVP_CipherFinal_ex returned success and handed back tampered plaintext
    // that was never authenticated. Declaring the length up front turns that into
    // a 16-byte compare against a zeroed buffer, which fails as it must.
    //
    // OCB additionally *requires* the length before the key is installed
    // (EVP_EncryptInit_ex derives the L table from it), so the whole block sits
    // ahead of the key/iv init for every mode.
    const bool fixedTagLen{aead && !gcm};
    if (fixedTagLen) {
        // On decrypt with a supplied tag, declare exactly that length: OCB's ctrl
        // rejects a tag whose length differs from the context's, so the length has
        // to be declared (ptr == nullptr) before the bytes are handed over.
        // ref: OpenSSL crypto/evp/e_aes.c aes_ocb_ctrl.
        const int declared{(!enc && !tagIn.empty()) ? static_cast<int>(tagIn.size()) : tagLen};
        EVP_CIPHER_CTX_ctrl(cc.get(), EVP_CTRL_AEAD_SET_TAG, declared, nullptr);
        if (!enc && !tagIn.empty()) {
            EVP_CIPHER_CTX_ctrl(cc.get(), EVP_CTRL_AEAD_SET_TAG, declared, tagIn.data());
        }
    }
    if (EVP_CipherInit_ex(cc.get(), nullptr, nullptr, key.data(), iv.empty() ? nullptr : iv.data(),
                          enc ? 1 : 0) <= 0) {
        return hasher_throw(global, "cipher key/iv setup failed");
    }
    if (!enc && gcm && !tagIn.empty()) {
        EVP_CIPHER_CTX_ctrl(cc.get(), EVP_CTRL_AEAD_SET_TAG, static_cast<int>(tagIn.size()),
                            tagIn.data());
    }
    if (noPad) { EVP_CIPHER_CTX_set_padding(cc.get(), 0); }
    int tmp{0};
    if (aead && !aad.empty()) {
        EVP_CipherUpdate(cc.get(), nullptr, &tmp, aad.data(), static_cast<int>(aad.size()));
    }
    std::vector<std::uint8_t> out(data.size() + static_cast<std::size_t>(EVP_CIPHER_block_size(c)));
    int outl{0};
    if (!data.empty() &&
        EVP_CipherUpdate(cc.get(), out.data(), &outl, data.data(), static_cast<int>(data.size())) <= 0) {
        return hasher_throw(global, "cipher update failed");
    }
    int finl{0};
    if (EVP_CipherFinal_ex(cc.get(), out.data() + outl, &finl) <= 0) {
        // GCM/AEAD decrypt failure is an authentication failure (node's fixed
        // message); block-cipher failures surface the real OpenSSL error so the
        // JS layer can recover code/library/reason (e.g. WRONG_FINAL_BLOCK_LENGTH).
        if (!enc && aead) { return hasher_throw(global, "Unsupported state or unable to authenticate data"); }
        return hasher_throw(global, asym_ossl_err());
    }
    out.resize(static_cast<std::size_t>(outl + finl));

    JSObjectRef obj{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, obj, "data", static_cast<JSValueRef>(asym_u8(ctx, out)));
    if (enc && aead) {
        std::vector<std::uint8_t> tag(static_cast<std::size_t>(tagLen));
        EVP_CIPHER_CTX_ctrl(cc.get(), EVP_CTRL_AEAD_GET_TAG, tagLen, tag.data());
        set_prop(ctx, obj, "tag", static_cast<JSValueRef>(asym_u8(ctx, tag)));
    }
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(obj)));
}

// __mbunCryptoAsymNative.cipherInfo(nameOrNid[, keyLength][, ivLength]) →
//   { name, nid, blockSize, ivLength, keyLength, mode } or null when the cipher
//   is unknown / the requested key or IV length is unsupported. Backs
//   crypto.getCipherInfo and the createCipheriv construction-time validation.
// ref: node lib/internal/crypto/util.js getCipherInfo + src/crypto/crypto_cipher.cc.
JSC_DEFINE_HOST_FUNCTION(node_asym_cipher_info_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    JSC::JSValue arg{native_arg(frame, 0)};
    const EVP_CIPHER* c{nullptr};
    if (arg && arg.isNumber()) {
        const char* sn{OBJ_nid2sn(static_cast<int>(arg.asNumber()))};
        if (sn != nullptr) { c = EVP_get_cipherbyname(sn); }
    } else if (arg && arg.isString()) {
        const std::string name{asym_str(global, arg)};
        c = EVP_get_cipherbyname(name.c_str());
    }
    if (c == nullptr) { return JSC::JSValue::encode(JSC::jsNull()); }

    const int mode{EVP_CIPHER_mode(c)};
    const int keyLen{EVP_CIPHER_key_length(c)};
    const int ivLen{EVP_CIPHER_iv_length(c)};

    // Optional key-length feasibility check (node: EVP_CIPHER_CTX_set_key_length).
    if (JSC::JSValue kArg{native_arg(frame, 1)}; kArg && kArg.isNumber()) {
        CipherCtxPtr cc{EVP_CIPHER_CTX_new()};
        if (!cc || EVP_CipherInit_ex(cc.get(), c, nullptr, nullptr, nullptr, 1) <= 0 ||
            EVP_CIPHER_CTX_set_key_length(cc.get(), static_cast<int>(kArg.asNumber())) <= 0) {
            return JSC::JSValue::encode(JSC::jsNull());
        }
    }
    // Optional IV-length feasibility check. AEAD modes (GCM/CCM/OCB) accept a
    // settable range via EVP_CTRL_AEAD_SET_IVLEN; fixed-IV modes must match exactly.
    if (JSC::JSValue ivArg{native_arg(frame, 2)}; ivArg && ivArg.isNumber()) {
        const int want{static_cast<int>(ivArg.asNumber())};
        const bool aead{mode == EVP_CIPH_GCM_MODE || mode == EVP_CIPH_CCM_MODE ||
                        mode == EVP_CIPH_OCB_MODE};
        if (aead) {
            CipherCtxPtr cc{EVP_CIPHER_CTX_new()};
            if (!cc || EVP_CipherInit_ex(cc.get(), c, nullptr, nullptr, nullptr, 1) <= 0 ||
                EVP_CIPHER_CTX_ctrl(cc.get(), EVP_CTRL_AEAD_SET_IVLEN, want, nullptr) <= 0) {
                return JSC::JSValue::encode(JSC::jsNull());
            }
        } else if (want != ivLen) {
            return JSC::JSValue::encode(JSC::jsNull());
        }
    }

    std::string name{EVP_CIPHER_get0_name(c) != nullptr ? EVP_CIPHER_get0_name(c) : std::string{}};
    for (char& ch : name) { ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }
    const char* modeStr{[&]() -> const char* {
        switch (mode) {
            case EVP_CIPH_ECB_MODE: return "ecb";
            case EVP_CIPH_CBC_MODE: return "cbc";
            case EVP_CIPH_CFB_MODE: return "cfb";
            case EVP_CIPH_OFB_MODE: return "ofb";
            case EVP_CIPH_CTR_MODE: return "ctr";
            case EVP_CIPH_GCM_MODE: return "gcm";
            case EVP_CIPH_CCM_MODE: return "ccm";
            case EVP_CIPH_XTS_MODE: return "xts";
            case EVP_CIPH_WRAP_MODE: return "wrap";
            case EVP_CIPH_OCB_MODE: return "ocb";
            default: return "stream";
        }
    }()};

    JSObjectRef obj{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, obj, "name", make_string(ctx, name));
    set_prop(ctx, obj, "nid", JSValueMakeNumber(ctx, static_cast<double>(EVP_CIPHER_nid(c))));
    set_prop(ctx, obj, "blockSize",
             JSValueMakeNumber(ctx, static_cast<double>(EVP_CIPHER_block_size(c))));
    if (ivLen > 0) {
        set_prop(ctx, obj, "ivLength", JSValueMakeNumber(ctx, static_cast<double>(ivLen)));
    }
    set_prop(ctx, obj, "keyLength", JSValueMakeNumber(ctx, static_cast<double>(keyLen)));
    set_prop(ctx, obj, "mode", make_string(ctx, modeStr));
    // mbun extension (node's getCipherInfo has no such field): whether this cipher
    // is AEAD, i.e. produces/consumes an authentication tag.
    //
    // SECURITY: the JS layer used to infer this from `mode`, and ChaCha20-Poly1305's
    // mode is "stream" — so `_auth` came out FALSE for a real AEAD, setAuthTag()
    // skipped its length check, and a zero-length tag sailed through unvalidated
    // into a native path that then skipped EVP_CTRL_AEAD_SET_TAG entirely.
    // node asks the cipher itself (IsSupportedAuthenticatedMode /
    // ncrypto::Cipher::isSupportedAuthenticatedMode), which is what this mirrors.
    // NID_chacha20_poly1305 is enumerated rather than using
    // EVP_CIPHER_FLAG_AEAD_CIPHER because OpenSSL also sets that flag on the
    // stitched aes-*-cbc-hmac-sha* ciphers, which are not tag-producing here.
    const bool isAead{mode == EVP_CIPH_GCM_MODE || mode == EVP_CIPH_CCM_MODE ||
                      mode == EVP_CIPH_OCB_MODE ||
                      EVP_CIPHER_nid(c) == NID_chacha20_poly1305};
    set_prop(ctx, obj, "aead", JSValueMakeBoolean(ctx, isAead));
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(obj)));
}

// __mbunCryptoAsymNative.dhGeneratePrime(bits) → Uint8Array (big-endian probable
// prime) for createDiffieHellman(<bitLength>). Uses BN_generate_prime_ex (fast,
// non-safe: sufficient for DH key-agreement round-trips the JS layer performs).
JSC_DEFINE_HOST_FUNCTION(node_asym_dh_prime_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    const int bits{asym_int(native_arg(frame, 0), 1024)};
    if (bits < 8 || bits > 8192) { return hasher_throw(global, "Invalid prime length"); }
    BIGNUM* p{BN_new()};
    if (p == nullptr || BN_generate_prime_ex(p, bits, 0, nullptr, nullptr, nullptr) != 1) {
        if (p != nullptr) { BN_free(p); }
        return hasher_throw(global, "DH prime generation failed");
    }
    const int n{BN_num_bytes(p)};
    std::vector<std::uint8_t> out(n > 0 ? static_cast<std::size_t>(n) : 0, 0);
    if (n > 0) { BN_bn2bin(p, out.data()); }
    BN_free(p);
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(asym_u8(ctx, out))));
}

struct BnDel { void operator()(BIGNUM* p) const { if (p) BN_free(p); } };
using BnPtr = std::unique_ptr<BIGNUM, BnDel>;
struct BnCtxDel { void operator()(BN_CTX* p) const { if (p) BN_CTX_free(p); } };
using BnCtxPtr = std::unique_ptr<BN_CTX, BnCtxDel>;

// __mbunCryptoAsymNative.generatePrime(bits, safe, add, rem) → Uint8Array
// (big-endian, BN_num_bytes long — the same minimal encoding node's
// RandomPrimeTraits::EncodeOutput produces). `add`/`rem` are optional unsigned
// big-endian byte strings; OpenSSL then constrains p % add == rem (rem defaults
// to 1, or to 3 with safe). The two guards node applies before this call
// (add wider than the requested prime, add <= rem — both of which make
// BN_generate_prime_ex spin forever) live in the JS layer, where they can carry
// node's ERR_OUT_OF_RANGE codes.
// ref: node src/crypto/crypto_random.cc RandomPrimeTraits + ncrypto
//      BignumPointer::generate.
JSC_DEFINE_HOST_FUNCTION(node_asym_generate_prime_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    const int bits{asym_int(native_arg(frame, 0), 0)};
    if (bits < 1) { return hasher_throw(global, "Invalid prime length"); }
    const bool safe{[&] { JSC::JSValue v{native_arg(frame, 1)}; return v && v.toBoolean(global); }()};
    std::vector<std::uint8_t> addBytes;
    std::vector<std::uint8_t> remBytes;
    const bool hasAdd{asym_read(global, native_arg(frame, 2), addBytes)};
    const bool hasRem{asym_read(global, native_arg(frame, 3), remBytes)};
    BnPtr add{hasAdd ? BN_bin2bn(addBytes.data(), static_cast<int>(addBytes.size()), nullptr)
                     : nullptr};
    BnPtr rem{hasRem ? BN_bin2bn(remBytes.data(), static_cast<int>(remBytes.size()), nullptr)
                     : nullptr};
    if ((hasAdd && !add) || (hasRem && !rem)) {
        return hasher_throw(global, "could not generate prime: " + asym_ossl_err());
    }
    BnPtr p{BN_new()};
    if (!p || BN_generate_prime_ex(p.get(), bits, safe ? 1 : 0, add.get(), rem.get(), nullptr) != 1) {
        return hasher_throw(global, "could not generate prime: " + asym_ossl_err());
    }
    const int n{BN_num_bytes(p.get())};
    std::vector<std::uint8_t> out(n > 0 ? static_cast<std::size_t>(n) : 0, 0);
    if (n > 0) { BN_bn2bin(p.get(), out.data()); }
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(asym_u8(ctx, out))));
}

// __mbunCryptoAsymNative.checkPrime(candidate, checks) → boolean.
// `checks` == 0 lets OpenSSL pick the Miller-Rabin round count for the bit size
// (node passes the option straight through the same way). The BN_bin2bn failure
// is NOT swallowed: an over-long candidate must surface OpenSSL's own
// "bignum too long", which the JS layer turns into ERR_OSSL_BN_BIGNUM_TOO_LONG,
// rather than being answered "not prime".
// ref: node src/crypto/crypto_random.cc CheckPrimeTraits.
JSC_DEFINE_HOST_FUNCTION(node_asym_check_prime_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    std::vector<std::uint8_t> cand;
    if (!asym_read(global, native_arg(frame, 0), cand)) {
        return hasher_throw(global, "candidate is required");
    }
    while (ERR_get_error() != 0) {}  // start from a clean queue
    BnPtr bn{BN_bin2bn(cand.data(), static_cast<int>(cand.size()), nullptr)};
    if (!bn) { return hasher_throw(global, asym_ossl_err()); }
    const int checks{asym_int(native_arg(frame, 1), 0)};
    BN_CTX* bctx{BN_CTX_new()};
    const int rc{BN_is_prime_ex(bn.get(), checks, bctx, nullptr)};
    if (bctx != nullptr) { BN_CTX_free(bctx); }
    if (rc < 0) { return hasher_throw(global, "prime check failed: " + asym_ossl_err()); }
    return JSC::JSValue::encode(JSC::jsBoolean(rc == 1));
}

// __mbunCryptoAsymNative.spkac(op, input) — SPKAC / Netscape SPKI (crypto.Certificate).
//   op 0 verifySpkac      → boolean
//   op 1 exportPublicKey  → Uint8Array (SPKI PEM) or undefined when it does not parse
//   op 2 exportChallenge  → Uint8Array (challenge string) or undefined
// The signature check is the real NETSCAPE_SPKI_verify against the key the SPKAC
// itself carries — a malformed or unsigned SPKAC returns false, never "assume ok".
// ref: node src/crypto/crypto_spkac.cc + deps/ncrypto VerifySpkac/ExportPublicKey/
//      ExportChallenge.
JSC_DEFINE_HOST_FUNCTION(node_asym_spkac_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    const int op{asym_int(native_arg(frame, 0), 0)};
    std::vector<std::uint8_t> input;
    if (!asym_read(global, native_arg(frame, 1), input)) {
        return hasher_throw(global, "spkac input is required");
    }
    NETSCAPE_SPKI* spki{NETSCAPE_SPKI_b64_decode(reinterpret_cast<const char*>(input.data()),
                                                 static_cast<int>(input.size()))};
    if (spki == nullptr) {
        while (ERR_get_error() != 0) {}
        return JSC::JSValue::encode(op == 0 ? JSC::jsBoolean(false) : JSC::jsUndefined());
    }
    JSC::EncodedJSValue result{JSC::JSValue::encode(JSC::jsUndefined())};
    if (op == 0) {
        EVP_PKEY* pkey{X509_PUBKEY_get(spki->spkac->pubkey)};
        const bool ok{pkey != nullptr && NETSCAPE_SPKI_verify(spki, pkey) > 0};
        if (pkey != nullptr) { EVP_PKEY_free(pkey); }
        result = JSC::JSValue::encode(JSC::jsBoolean(ok));
    } else if (op == 1) {
        EVP_PKEY* pkey{NETSCAPE_SPKI_get_pubkey(spki)};
        BIO* bio{BIO_new(BIO_s_mem())};
        if (pkey != nullptr && bio != nullptr && PEM_write_bio_PUBKEY(bio, pkey) > 0) {
            char* data{nullptr};
            const long len{BIO_get_mem_data(bio, &data)};
            if (len > 0) {
                std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(data),
                                                    static_cast<std::size_t>(len)};
                result = JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(asym_u8(ctx, bytes))));
            }
        }
        if (bio != nullptr) { BIO_free(bio); }
        if (pkey != nullptr) { EVP_PKEY_free(pkey); }
    } else {
        unsigned char* buf{nullptr};
        const int n{ASN1_STRING_to_UTF8(&buf, spki->spkac->challenge)};
        if (n >= 0) {
            std::span<const std::uint8_t> bytes{buf, static_cast<std::size_t>(n)};
            result = JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(asym_u8(ctx, bytes))));
        }
        if (buf != nullptr) { OPENSSL_free(buf); }
    }
    NETSCAPE_SPKI_free(spki);
    while (ERR_get_error() != 0) {}
    return result;
}

// --- ECDH (provider API via OSSL_PARAM_BLD/fromdata) ---

// Build an EC EVP_PKEY from a group name plus a public point and/or private
// scalar. Either component may be empty.
inline EVP_PKEY* asym_ec_from_parts(const std::string& curve, std::span<const std::uint8_t> pub,
                                    std::span<const std::uint8_t> priv) {
    OSSL_PARAM_BLD* bld{OSSL_PARAM_BLD_new()};
    if (bld == nullptr) { return nullptr; }
    OSSL_PARAM_BLD_push_utf8_string(bld, OSSL_PKEY_PARAM_GROUP_NAME, curve.c_str(), curve.size());
    if (!pub.empty()) {
        OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, pub.data(), pub.size());
    }
    BIGNUM* bn{nullptr};
    if (!priv.empty()) {
        bn = BN_bin2bn(priv.data(), static_cast<int>(priv.size()), nullptr);
        OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_PRIV_KEY, bn);
    }
    OSSL_PARAM* params{OSSL_PARAM_BLD_to_param(bld)};
    EVP_PKEY* pkey{nullptr};
    PkeyCtxPtr pctx{EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr)};
    if (pctx && params != nullptr) {
        const int sel{priv.empty() ? EVP_PKEY_PUBLIC_KEY : EVP_PKEY_KEYPAIR};
        if (EVP_PKEY_fromdata_init(pctx.get()) > 0) {
            EVP_PKEY_fromdata(pctx.get(), &pkey, sel, params);
        }
    }
    if (params != nullptr) { OSSL_PARAM_free(params); }
    OSSL_PARAM_BLD_free(bld);
    if (bn != nullptr) { BN_free(bn); }
    return pkey;
}

// __mbunCryptoAsymNative.ecdhGenerate(curve) → { publicKey (uncompressed pt),
//   privateKey (scalar) }.
JSC_DEFINE_HOST_FUNCTION(node_asym_ecdh_generate_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    const std::string curve{asym_str(global, native_arg(frame, 0))};
    PkeyPtr pkey{EVP_EC_gen(curve.c_str())};
    if (!pkey) { return hasher_throw(global, "Invalid EC curve name: " + curve); }
    std::size_t publen{0};
    EVP_PKEY_get_octet_string_param(pkey.get(), OSSL_PKEY_PARAM_PUB_KEY, nullptr, 0, &publen);
    std::vector<std::uint8_t> pub(publen);
    EVP_PKEY_get_octet_string_param(pkey.get(), OSSL_PKEY_PARAM_PUB_KEY, pub.data(), publen, &publen);
    pub.resize(publen);
    BIGNUM* bn{nullptr};
    EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_PRIV_KEY, &bn);
    const std::size_t fb{asym_field_bytes(pkey.get())};
    std::vector<std::uint8_t> priv(fb, 0);
    if (bn != nullptr) { BN_bn2binpad(bn, priv.data(), static_cast<int>(fb)); BN_free(bn); }

    JSObjectRef obj{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, obj, "publicKey", static_cast<JSValueRef>(asym_u8(ctx, pub)));
    set_prop(ctx, obj, "privateKey", static_cast<JSValueRef>(asym_u8(ctx, priv)));
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(obj)));
}

// __mbunCryptoAsymNative.ecdhComputeSecret(curve, privateKey, otherPubPoint) →
//   Uint8Array shared secret.
JSC_DEFINE_HOST_FUNCTION(node_asym_ecdh_compute_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    const std::string curve{asym_str(global, native_arg(frame, 0))};
    std::vector<std::uint8_t> priv;
    std::vector<std::uint8_t> peerPub;
    if (!asym_read(global, native_arg(frame, 1), priv) ||
        !asym_read(global, native_arg(frame, 2), peerPub)) {
        return hasher_throw(global, "computeSecret: private key and peer public key required");
    }
    PkeyPtr me{asym_ec_from_parts(curve, {}, priv)};
    PkeyPtr peer{asym_ec_from_parts(curve, peerPub, {})};
    if (!me || !peer) { return hasher_throw(global, "computeSecret: invalid key material"); }
    PkeyCtxPtr pctx{EVP_PKEY_CTX_new(me.get(), nullptr)};
    if (!pctx || EVP_PKEY_derive_init(pctx.get()) <= 0 ||
        EVP_PKEY_derive_set_peer(pctx.get(), peer.get()) <= 0) {
        return hasher_throw(global, "computeSecret: derive setup failed");
    }
    std::size_t seclen{0};
    EVP_PKEY_derive(pctx.get(), nullptr, &seclen);
    std::vector<std::uint8_t> sec(seclen);
    if (EVP_PKEY_derive(pctx.get(), sec.data(), &seclen) <= 0) {
        return hasher_throw(global, "computeSecret failed: " + asym_ossl_err());
    }
    sec.resize(seclen);
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(asym_u8(ctx, sec))));
}

// __mbunCryptoAsymNative.ecdhPublicFromPrivate(curve, privateKey) → Uint8Array
//   uncompressed public point (ECDH.setPrivateKey recomputes the public key).
JSC_DEFINE_HOST_FUNCTION(node_asym_ecdh_pub_from_priv_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    const std::string curve{asym_str(global, native_arg(frame, 0))};
    std::vector<std::uint8_t> priv;
    if (!asym_read(global, native_arg(frame, 1), priv)) {
        return hasher_throw(global, "setPrivateKey: private key required");
    }
    // Derive the public point: build a keypair from the scalar via a temp EC key.
    const int nid{OBJ_txt2nid(curve.c_str())};
    EC_GROUP* group{nid != NID_undef ? EC_GROUP_new_by_curve_name(nid) : nullptr};
    if (group == nullptr) { return hasher_throw(global, "Invalid EC curve name: " + curve); }
    BIGNUM* bn{BN_bin2bn(priv.data(), static_cast<int>(priv.size()), nullptr)};
    EC_POINT* pub{EC_POINT_new(group)};
    std::vector<std::uint8_t> out;
    if (bn != nullptr && EC_POINT_mul(group, pub, bn, nullptr, nullptr, nullptr) == 1) {
        const std::size_t need{
            EC_POINT_point2oct(group, pub, POINT_CONVERSION_UNCOMPRESSED, nullptr, 0, nullptr)};
        out.resize(need);
        EC_POINT_point2oct(group, pub, POINT_CONVERSION_UNCOMPRESSED, out.data(), need, nullptr);
    }
    if (bn != nullptr) { BN_free(bn); }
    EC_POINT_free(pub);
    EC_GROUP_free(group);
    if (out.empty()) { return hasher_throw(global, "setPrivateKey: invalid scalar"); }
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(asym_u8(ctx, out))));
}

// __mbunCryptoAsymNative.ecdhConvertKey(curve, point, compressed) → Uint8Array
//   re-encoded point (compressed/uncompressed).
JSC_DEFINE_HOST_FUNCTION(node_asym_ecdh_convert_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    const std::string curve{asym_str(global, native_arg(frame, 0))};
    std::vector<std::uint8_t> point;
    if (!asym_read(global, native_arg(frame, 1), point)) {
        return hasher_throw(global, "convertKey: point required");
    }
    const bool compressed{[&] { JSC::JSValue v{native_arg(frame, 2)}; return v && v.toBoolean(global); }()};
    const int nid{OBJ_txt2nid(curve.c_str())};
    EC_GROUP* group{nid != NID_undef ? EC_GROUP_new_by_curve_name(nid) : nullptr};
    if (group == nullptr) { return hasher_throw(global, "Invalid EC curve name: " + curve); }
    EC_POINT* pt{EC_POINT_new(group)};
    std::vector<std::uint8_t> out;
    if (EC_POINT_oct2point(group, pt, point.data(), point.size(), nullptr) == 1) {
        const point_conversion_form_t form{compressed ? POINT_CONVERSION_COMPRESSED
                                                      : POINT_CONVERSION_UNCOMPRESSED};
        const std::size_t need{EC_POINT_point2oct(group, pt, form, nullptr, 0, nullptr)};
        out.resize(need);
        EC_POINT_point2oct(group, pt, form, out.data(), need, nullptr);
    }
    EC_POINT_free(pt);
    EC_GROUP_free(group);
    if (out.empty()) { return hasher_throw(global, "convertKey: invalid point"); }
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(asym_u8(ctx, out))));
}

// __mbunCryptoAsymNative.okpDerive(privateKeyDer, peerPublicKeyDer) → Uint8Array
//   raw X25519/X448 shared secret. Both arguments are DER key material (PKCS8 /
//   SubjectPublicKeyInfo) exactly as webcrypto stores it, so no scalar ever
//   crosses into JS. OpenSSL's X25519 derive fails on an all-zero output, which
//   is the RFC 7748 §6.1 small-order-point rejection the spec requires.
// ref: bun src/jsc/bindings/webcrypto/CryptoAlgorithmX25519.cpp platformDeriveBits.
// __mbunCryptoAsymNative.kmac(name, key, data, customization, outLenBytes) →
//   Uint8Array with the KMAC tag. `name` is OpenSSL's MAC name ("KMAC-128" /
//   "KMAC-256"); `customization` is NIST SP 800-185's optional S string.
//   Backs WebCrypto's KMAC128/KMAC256 sign & verify.
// ref: node src/crypto/crypto_mac.cc (KmacJob) + OpenSSL EVP_MAC-KMAC(7).
JSC_DEFINE_HOST_FUNCTION(node_asym_kmac_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    const std::string name{asym_str(global, native_arg(frame, 0))};
    std::vector<std::uint8_t> key;
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> custom;
    if (!asym_read(global, native_arg(frame, 1), key)) {
        return hasher_throw(global, "kmac: key required");
    }
    asym_read(global, native_arg(frame, 2), data);
    asym_read(global, native_arg(frame, 3), custom);
    const int outLen{asym_int(native_arg(frame, 4), 32)};
    if (outLen < 0) { return hasher_throw(global, "kmac: invalid output length"); }

    struct MacDel { void operator()(EVP_MAC* m) const { if (m) EVP_MAC_free(m); } };
    struct MacCtxDel { void operator()(EVP_MAC_CTX* c) const { if (c) EVP_MAC_CTX_free(c); } };
    std::unique_ptr<EVP_MAC, MacDel> mac{EVP_MAC_fetch(nullptr, name.c_str(), nullptr)};
    if (!mac) { return hasher_throw(global, "kmac: unsupported MAC " + name); }
    std::unique_ptr<EVP_MAC_CTX, MacCtxDel> mctx{EVP_MAC_CTX_new(mac.get())};
    if (!mctx) { return hasher_throw(global, "kmac: context allocation failed"); }

    std::size_t size{static_cast<std::size_t>(outLen)};
    OSSL_PARAM params[3];
    int n{0};
    params[n++] = OSSL_PARAM_construct_size_t(OSSL_MAC_PARAM_SIZE, &size);
    if (!custom.empty()) {
        params[n++] = OSSL_PARAM_construct_octet_string(
            OSSL_MAC_PARAM_CUSTOM, custom.data(), custom.size());
    }
    params[n] = OSSL_PARAM_construct_end();

    if (EVP_MAC_init(mctx.get(), key.data(), key.size(), params) <= 0) {
        return hasher_throw(global, "kmac init failed: " + asym_ossl_err());
    }
    if (!data.empty() && EVP_MAC_update(mctx.get(), data.data(), data.size()) <= 0) {
        return hasher_throw(global, "kmac update failed: " + asym_ossl_err());
    }
    std::vector<std::uint8_t> out(size == 0 ? 1 : size);
    std::size_t outl{0};
    if (EVP_MAC_final(mctx.get(), out.data(), &outl, out.size()) <= 0) {
        return hasher_throw(global, "kmac final failed: " + asym_ossl_err());
    }
    out.resize(outl);
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(asym_u8(ctx, out))));
}

JSC_DEFINE_HOST_FUNCTION(node_asym_okp_derive_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    std::vector<std::uint8_t> priv;
    std::vector<std::uint8_t> pub;
    if (!asym_read(global, native_arg(frame, 0), priv) ||
        !asym_read(global, native_arg(frame, 1), pub)) {
        return hasher_throw(global, "deriveBits: private key and public key required");
    }
    PkeyPtr me{asym_load_pkey(priv, std::nullopt)};
    PkeyPtr peer{asym_load_pkey(pub, std::nullopt)};
    if (!me || !peer) { return hasher_throw(global, "deriveBits: invalid key material"); }
    const int mine{EVP_PKEY_base_id(me.get())};
    if (mine != EVP_PKEY_X25519 && mine != EVP_PKEY_X448) {
        return hasher_throw(global, "deriveBits: not a key-agreement key");
    }
    if (mine != EVP_PKEY_base_id(peer.get())) {
        return hasher_throw(global, "deriveBits: key type mismatch");
    }
    // Drain any error the decoder left behind so a later failure reports its own.
    while (::ERR_get_error() != 0) {}
    PkeyCtxPtr pctx{EVP_PKEY_CTX_new(me.get(), nullptr)};
    if (!pctx || EVP_PKEY_derive_init(pctx.get()) <= 0 ||
        EVP_PKEY_derive_set_peer(pctx.get(), peer.get()) <= 0) {
        return hasher_throw(global, "deriveBits: derive setup failed");
    }
    std::size_t seclen{0};
    EVP_PKEY_derive(pctx.get(), nullptr, &seclen);
    std::vector<std::uint8_t> sec(seclen);
    if (EVP_PKEY_derive(pctx.get(), sec.data(), &seclen) <= 0) {
        return hasher_throw(global, "deriveBits failed: " + asym_ossl_err());
    }
    sec.resize(seclen);
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(asym_u8(ctx, sec))));
}

// Whether an EVP_PKEY carries a private component. Needed so createPrivateKey
// rejects public-only material (the loader is intent-agnostic and will happily
// parse an RSAPublicKey as a pkey). ref: node prepareAsymmetricKey rejects a
// public key handed to createPrivateKey with a decode error.
inline bool asym_has_private(EVP_PKEY* pkey) {
    const int base{EVP_PKEY_base_id(pkey)};
    const char* bnParam{
        (base == EVP_PKEY_RSA || base == EVP_PKEY_RSA_PSS) ? OSSL_PKEY_PARAM_RSA_D
        : (base == EVP_PKEY_EC || base == EVP_PKEY_DSA || base == EVP_PKEY_DH ||
           base == EVP_PKEY_DHX)
            ? OSSL_PKEY_PARAM_PRIV_KEY
            : nullptr};
    if (bnParam != nullptr) {
        BIGNUM* bn{nullptr};
        if (EVP_PKEY_get_bn_param(pkey, bnParam, &bn) > 0 && bn != nullptr) {
            BN_free(bn);
            return true;
        }
        return false;
    }
    // Ed/X curves: the private key is an opaque octet string param.
    std::size_t len{0};
    return EVP_PKEY_get_octet_string_param(pkey, OSSL_PKEY_PARAM_PRIV_KEY, nullptr, 0, &len) > 0 &&
           len > 0;
}

// __mbunCryptoAsymNative.keyType(key, passphrase, isPublic) → { type, curve,
//   modulusLength, private } describing an asymmetric key (createPublicKey/PrivateKey).
JSC_DEFINE_HOST_FUNCTION(node_asym_key_type_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    std::vector<std::uint8_t> keyBytes;
    if (!asym_read(global, native_arg(frame, 0), keyBytes)) {
        return hasher_throw(global, "keyType: key required");
    }
    const AsymPass pass{asym_opt_str(global, native_arg(frame, 1))};
    PkeyPtr pkey{asym_load_pkey(keyBytes, pass)};
    if (!pkey) {
        // A passphrase WAS supplied and the key still would not decode: the
        // interesting failure is OpenSSL's ("bad decrypt" for a wrong password),
        // not the generic fallback. Without a passphrase the fallback stands —
        // there the queue holds speculative decoder misses, not a real verdict.
        return hasher_throw(global,
                            asym_key_error(keyBytes, pass.has_value(), "Invalid key", pass.has_value()));
    }
    const int base{EVP_PKEY_base_id(pkey.get())};
    std::string type;
    switch (base) {
        case EVP_PKEY_RSA: type = "rsa"; break;
        case EVP_PKEY_RSA_PSS: type = "rsa-pss"; break;
        case EVP_PKEY_EC: type = "ec"; break;
        case EVP_PKEY_DSA: type = "dsa"; break;
        case EVP_PKEY_DH: type = "dh"; break;
        case EVP_PKEY_DHX: type = "dh"; break;
        case EVP_PKEY_ED25519: type = "ed25519"; break;
        case EVP_PKEY_ED448: type = "ed448"; break;
        case EVP_PKEY_X25519: type = "x25519"; break;
        case EVP_PKEY_X448: type = "x448"; break;
        default: type = "unknown"; break;
    }
    JSObjectRef obj{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, obj, "type", make_string(ctx, type));
    set_prop(ctx, obj, "private", JSValueMakeBoolean(ctx, asym_has_private(pkey.get())));
    if (base == EVP_PKEY_EC) {
        char gname[80];
        std::size_t glen{0};
        if (EVP_PKEY_get_utf8_string_param(pkey.get(), OSSL_PKEY_PARAM_GROUP_NAME, gname,
                                           sizeof(gname), &glen) > 0) {
            set_prop(ctx, obj, "namedCurve", make_string(ctx, std::string_view{gname, glen}));
        }
    } else if (base == EVP_PKEY_RSA || base == EVP_PKEY_RSA_PSS) {
        set_prop(ctx, obj, "modulusLength",
                 JSValueMakeNumber(ctx, static_cast<double>(EVP_PKEY_get_bits(pkey.get()))));
        // publicExponent as big-endian bytes; the JS layer materializes the
        // node-visible BigInt (asymmetricKeyDetails.publicExponent). Inlined
        // (asym_set_bn_prop is defined further down this TU).
        BIGNUM* e{nullptr};
        if (EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_E, &e) > 0 && e != nullptr) {
            const int n{BN_num_bytes(e)};
            std::vector<std::uint8_t> eb(n > 0 ? static_cast<std::size_t>(n) : 0, 0);
            if (n > 0) { BN_bn2binpad(e, eb.data(), n); }
            set_prop(ctx, obj, "publicExponent", static_cast<JSValueRef>(asym_u8(ctx, eb)));
        }
        if (e != nullptr) { BN_free(e); }
        if (base == EVP_PKEY_RSA_PSS) {
            // OpenSSL's provider reports a restricted key's PSS parameters only
            // where they DIFFER from RFC 3447's defaults, but it always reports
            // saltlen for such a key — deliberately, "to ensure that the key isn't
            // seen as unrestricted by the recipient" (crypto/rsa/rsa_backend.c
            // ossl_rsa_pss_params_30_todata). So saltlen's presence, not the
            // digests', is the signal that the key carries an RSASSA-PSS-params
            // sequence. node reads that sequence out of the ASN.1 itself and
            // always reports the full quadruple with RFC defaults filled in
            // (src/crypto/crypto_keys.cc GetRsaKeyDetail), so a key whose
            // restriction happens to BE the default — SHA-1, MGF1-SHA-1, salt 20 —
            // must not come back looking unrestricted here.
            int saltLength{};
            if (EVP_PKEY_get_int_param(pkey.get(), OSSL_PKEY_PARAM_RSA_PSS_SALTLEN,
                                       &saltLength) > 0) {
                char digest[80];
                std::size_t digestLength{};
                const std::string_view hash{
                    EVP_PKEY_get_utf8_string_param(pkey.get(), OSSL_PKEY_PARAM_RSA_DIGEST, digest,
                                                   sizeof(digest), &digestLength) > 0
                        ? std::string_view{digest, digestLength}
                        : std::string_view{"SHA1"}};
                set_prop(ctx, obj, "hashAlgorithm", make_string(ctx, hash));
                char mgf1Digest[80];
                std::size_t mgf1DigestLength{};
                const std::string_view mgf1{
                    EVP_PKEY_get_utf8_string_param(pkey.get(), OSSL_PKEY_PARAM_RSA_MGF1_DIGEST,
                                                   mgf1Digest, sizeof(mgf1Digest),
                                                   &mgf1DigestLength) > 0
                        ? std::string_view{mgf1Digest, mgf1DigestLength}
                        : std::string_view{"SHA1"}};
                set_prop(ctx, obj, "mgf1HashAlgorithm", make_string(ctx, mgf1));
                set_prop(ctx, obj, "saltLength", JSValueMakeNumber(ctx, saltLength));
            }
        }
    } else if (base == EVP_PKEY_DSA) {
        // node asymmetricKeyDetails for DSA: { modulusLength: |p|, divisorLength: |q| }.
        set_prop(ctx, obj, "modulusLength",
                 JSValueMakeNumber(ctx, static_cast<double>(EVP_PKEY_get_bits(pkey.get()))));
        BIGNUM* q{nullptr};
        if (EVP_PKEY_get_bn_param(pkey.get(), OSSL_PKEY_PARAM_FFC_Q, &q) > 0 && q != nullptr) {
            set_prop(ctx, obj, "divisorLength",
                     JSValueMakeNumber(ctx, static_cast<double>(BN_num_bits(q))));
            BN_free(q);
        }
    }
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(obj)));
}


// The JWK component bridge continues in crypto_asym_jwk.inc. It is a separate
// physical slice because runtime/ enforces a 2000-line budget per file
// (modules/jsc/tests/test_runtime_structure.cpp) and the JWK validation work
// in waves 62-64 pushed this file to 2094. Splitting is the intended remedy;
// raising the budget would defeat a guard the repo installed on purpose.
#include "crypto_asym_jwk.inc"
