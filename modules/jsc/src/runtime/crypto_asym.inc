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
// Anything else keeps the caller's own wording.
inline std::string asym_key_error(std::span<const std::uint8_t> data, bool hasPass,
                                  std::string_view fallback) {
    if (!hasPass) {
        if (asym_is_encrypted_pem(data)) {
            return "error:07880109:common libcrypto routines::interrupted or cancelled";
        }
        if (asym_is_encrypted_der(data)) { return "Passphrase required for encrypted key"; }
    }
    return std::string{fallback};
}

// Decode a key from PEM or DER (public or private), optional passphrase. Tries a
// private-capable selection first, then public-only (SPKI). Returns nullptr on
// failure. An engaged-but-empty `pass` is an empty PASSWORD (node allows keys
// encrypted under one); a disengaged `pass` means "never ask, never guess".
inline EVP_PKEY* asym_load_pkey(std::span<const std::uint8_t> data, const AsymPass& pass) {
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
        if (pkey != nullptr) { return pkey; }
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
            if (pk != nullptr) { return pk; }
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
        return pub;
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
    if (!pkey) { return hasher_throw(global, asym_key_error(keyBytes, pass.has_value(), "invalid key")); }
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

// EC field size in bytes (for ieee-p1363 sig + ecdh scalar padding).
inline std::size_t asym_field_bytes(EVP_PKEY* pkey) {
    const int bits{EVP_PKEY_get_bits(pkey)};
    return bits > 0 ? static_cast<std::size_t>((bits + 7) / 8) : 0;
}

// Finite-field (DSA / classic DH) keygen. OpenSSL 3 has no one-shot for these:
// the domain parameters (p, q, g) are generated first, then a key inside them.
// qbits==0 lets the provider pick the divisor size for the requested pbits.
inline EVP_PKEY* asym_ffc_gen(const char* alg, unsigned int pbits, unsigned int qbits) {
    PkeyCtxPtr pc{EVP_PKEY_CTX_new_from_name(nullptr, alg, nullptr)};
    if (!pc || EVP_PKEY_paramgen_init(pc.get()) <= 0) { return nullptr; }
    OSSL_PARAM params[3];
    int n{0};
    params[n++] = OSSL_PARAM_construct_uint(OSSL_PKEY_PARAM_FFC_PBITS, &pbits);
    if (qbits > 0) { params[n++] = OSSL_PARAM_construct_uint(OSSL_PKEY_PARAM_FFC_QBITS, &qbits); }
    params[n] = OSSL_PARAM_construct_end();
    if (EVP_PKEY_CTX_set_params(pc.get(), params) <= 0) { return nullptr; }
    EVP_PKEY* domain{nullptr};
    if (EVP_PKEY_generate(pc.get(), &domain) <= 0 || domain == nullptr) { return nullptr; }
    PkeyPtr dom{domain};
    PkeyCtxPtr kc{EVP_PKEY_CTX_new_from_pkey(nullptr, dom.get(), nullptr)};
    EVP_PKEY* key{nullptr};
    if (!kc || EVP_PKEY_keygen_init(kc.get()) <= 0 || EVP_PKEY_generate(kc.get(), &key) <= 0) {
        return nullptr;
    }
    return key;
}

// RSA-PSS keygen. EVP_RSA_gen would mint a plain rsaEncryption key; node's
// 'rsa-pss' must carry the id-RSASSA-PSS OID, and — with no hash/MGF1/salt
// restriction requested — must NOT emit an RSASSA-PSS-params sequence
// (nodejs/node#39936), which is what the bare "RSA-PSS" keymgmt produces.
inline EVP_PKEY* asym_rsa_pss_gen(std::size_t bits) {
    PkeyCtxPtr pc{EVP_PKEY_CTX_new_from_name(nullptr, "RSA-PSS", nullptr)};
    if (!pc || EVP_PKEY_keygen_init(pc.get()) <= 0) { return nullptr; }
    OSSL_PARAM params[2];
    params[0] = OSSL_PARAM_construct_size_t(OSSL_PKEY_PARAM_RSA_BITS, &bits);
    params[1] = OSSL_PARAM_construct_end();
    if (EVP_PKEY_CTX_set_params(pc.get(), params) <= 0) { return nullptr; }
    EVP_PKEY* key{nullptr};
    if (EVP_PKEY_generate(pc.get(), &key) <= 0) { return nullptr; }
    return key;
}

// __mbunCryptoAsymNative.generateKeyPair(type, modLen, namedCurve, pubType,
//   pubFmt, privType, privFmt, cipher, passphrase, divisorLength)
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

    PkeyPtr pkey;
    if (type == "rsa") {
        pkey.reset(EVP_RSA_gen(static_cast<unsigned int>(modLen > 0 ? modLen : 2048)));
    } else if (type == "rsa-pss") {
        pkey.reset(asym_rsa_pss_gen(static_cast<std::size_t>(modLen > 0 ? modLen : 2048)));
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
inline void asym_apply_rsa_sig_opts(EVP_PKEY_CTX* pctx, EVP_PKEY* pkey, int padding, int saltLen) {
    const int base{EVP_PKEY_base_id(pkey)};
    if (base != EVP_PKEY_RSA && base != EVP_PKEY_RSA_PSS) { return; }
    EVP_PKEY_CTX_set_rsa_padding(pctx, padding);
    if (padding == RSA_PKCS1_PSS_PADDING) {
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, saltLen);
    }
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
                            asym_key_error(keyBytes, pass.has_value(), "sign: invalid private key"));
    }
    MdCtxPtr md{EVP_MD_CTX_new()};
    const EVP_MD* dg{digest.empty() ? nullptr : EVP_get_digestbyname(digest.c_str())};
    if (!digest.empty() && dg == nullptr) { return hasher_throw(global, "Unknown digest: " + digest); }
    EVP_PKEY_CTX* pctx{nullptr};
    if (EVP_DigestSignInit(md.get(), &pctx, dg, nullptr, pkey.get()) <= 0) {
        return hasher_throw(global, "sign init failed: " + asym_ossl_err());
    }
    asym_apply_rsa_sig_opts(pctx, pkey.get(), padding, saltLen);
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
        return hasher_throw(global, asym_key_error(keyBytes, pass.has_value(), "verify: invalid key"));
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
    if (!pkey) { return hasher_throw(global, asym_key_error(keyBytes, pass.has_value(), "invalid key")); }
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
        return hasher_throw(global, asym_key_error(keyBytes, pass.has_value(), "Invalid key"));
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

// --- JWK component bridge (RFC 7517/7518) ---------------------------------
// OpenSSL ships no JWK codec, so components cross the provider param API. Raw
// bytes (not base64url) go to JS: the JS layer owns the base64url so this side
// stays free of a base64 impl, and WebCrypto/node share one component source.
// ref: bun-ref src/jsc/bindings/webcrypto/CryptoKeyRSAOpenSSL.cpp (exportJwk),
//      CryptoKeyECOpenSSL.cpp (x/y padded to the field size);
//      node lib/internal/crypto/keys.js (component names + order).

// OpenSSL group name → JWK "crv". RFC 7518 §6.2.1.1 spells the NIST curves
// P-256/384/521; OpenSSL uses the SECG/X9.62 aliases.
inline std::string asym_jwk_crv(std::string_view group) {
    if (group == "prime256v1") { return "P-256"; }
    if (group == "secp384r1") { return "P-384"; }
    if (group == "secp521r1") { return "P-521"; }
    return std::string{group};
}

// JWK "crv" → OpenSSL group name (inverse of asym_jwk_crv).
inline std::string asym_jwk_group(std::string_view crv) {
    if (crv == "P-256") { return "prime256v1"; }
    if (crv == "P-384") { return "secp384r1"; }
    if (crv == "P-521") { return "secp521r1"; }
    return std::string{crv};
}

// BIGNUM param → big-endian bytes. pad==0 → minimal form (RSA components, RFC
// 7518 §6.3.1.1); pad>0 → left-zero-padded to `pad` (EC field elements MUST be
// exactly ceil(log2(crv)/8) bytes, RFC 7518 §6.2.1.2).
inline bool asym_bn_bytes(EVP_PKEY* pkey, const char* param, std::size_t pad,
                          std::vector<std::uint8_t>& out) {
    BIGNUM* bn{nullptr};
    if (EVP_PKEY_get_bn_param(pkey, param, &bn) <= 0 || bn == nullptr) { return false; }
    const std::size_t n{pad > 0 ? pad : static_cast<std::size_t>(BN_num_bytes(bn))};
    out.assign(n, 0);
    const bool ok{n == 0 || BN_bn2binpad(bn, out.data(), static_cast<int>(n)) >= 0};
    BN_free(bn);
    return ok;
}

// Set `name` on `obj` to a BIGNUM param's bytes; absent params are skipped.
inline void asym_set_bn_prop(JSContextRef ctx, JSObjectRef obj, const char* name, EVP_PKEY* pkey,
                             const char* param, std::size_t pad) {
    std::vector<std::uint8_t> v;
    if (asym_bn_bytes(pkey, param, pad, v)) {
        set_prop(ctx, obj, name, static_cast<JSValueRef>(asym_u8(ctx, v)));
    }
}

// Read an octet-string param (two-call size probe).
inline bool asym_octet_param(EVP_PKEY* pkey, const char* param, std::vector<std::uint8_t>& out) {
    std::size_t len{0};
    if (EVP_PKEY_get_octet_string_param(pkey, param, nullptr, 0, &len) <= 0 || len == 0) {
        return false;
    }
    out.assign(len, 0);
    if (EVP_PKEY_get_octet_string_param(pkey, param, out.data(), len, &len) <= 0) { return false; }
    out.resize(len);
    return true;
}

// __mbunCryptoAsymNative.jwkExport(key, passphrase, isPublic) → { kty, crv?,
//   <components as raw Uint8Array> }. Private components are omitted when
//   isPublic. Property order matches node's (kty,n,e,d,p,q,dp,dq,qi / kty,x,y,
//   crv,d) so JSON.stringify round-trips byte-identically.
JSC_DEFINE_HOST_FUNCTION(node_asym_jwk_export_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    std::vector<std::uint8_t> keyBytes;
    if (!asym_read(global, native_arg(frame, 0), keyBytes)) {
        return hasher_throw(global, "jwkExport: key required");
    }
    const AsymPass pass{asym_opt_str(global, native_arg(frame, 1))};
    const bool isPublic{[&] { JSC::JSValue v{native_arg(frame, 2)}; return v && v.toBoolean(global); }()};

    PkeyPtr pkey{asym_load_pkey(keyBytes, pass)};
    if (!pkey) {
        return hasher_throw(global, asym_key_error(keyBytes, pass.has_value(), "Invalid key"));
    }
    JSObjectRef obj{JSObjectMake(ctx, nullptr, nullptr)};
    const int base{EVP_PKEY_base_id(pkey.get())};
    if (base == EVP_PKEY_RSA || base == EVP_PKEY_RSA_PSS) {
        set_prop(ctx, obj, "kty", make_string(ctx, "RSA"));
        asym_set_bn_prop(ctx, obj, "n", pkey.get(), OSSL_PKEY_PARAM_RSA_N, 0);
        asym_set_bn_prop(ctx, obj, "e", pkey.get(), OSSL_PKEY_PARAM_RSA_E, 0);
        if (!isPublic) {
            asym_set_bn_prop(ctx, obj, "d", pkey.get(), OSSL_PKEY_PARAM_RSA_D, 0);
            asym_set_bn_prop(ctx, obj, "p", pkey.get(), OSSL_PKEY_PARAM_RSA_FACTOR1, 0);
            asym_set_bn_prop(ctx, obj, "q", pkey.get(), OSSL_PKEY_PARAM_RSA_FACTOR2, 0);
            asym_set_bn_prop(ctx, obj, "dp", pkey.get(), OSSL_PKEY_PARAM_RSA_EXPONENT1, 0);
            asym_set_bn_prop(ctx, obj, "dq", pkey.get(), OSSL_PKEY_PARAM_RSA_EXPONENT2, 0);
            asym_set_bn_prop(ctx, obj, "qi", pkey.get(), OSSL_PKEY_PARAM_RSA_COEFFICIENT1, 0);
        }
    } else if (base == EVP_PKEY_EC) {
        char gname[80];
        std::size_t glen{0};
        if (EVP_PKEY_get_utf8_string_param(pkey.get(), OSSL_PKEY_PARAM_GROUP_NAME, gname,
                                           sizeof(gname), &glen) <= 0) {
            return hasher_throw(global, "jwkExport: EC group unavailable");
        }
        const std::size_t fb{asym_field_bytes(pkey.get())};
        std::vector<std::uint8_t> pub;
        if (!asym_octet_param(pkey.get(), OSSL_PKEY_PARAM_PUB_KEY, pub)) {
            return hasher_throw(global, "jwkExport: EC public point unavailable");
        }
        // Uncompressed point only: 0x04 || X(fb) || Y(fb) (OSSL's default form).
        if (fb == 0 || pub.size() != 1 + 2 * fb || pub[0] != 0x04) {
            return hasher_throw(global, "jwkExport: unsupported EC point format");
        }
        set_prop(ctx, obj, "kty", make_string(ctx, "EC"));
        set_prop(ctx, obj, "x", static_cast<JSValueRef>(asym_u8(ctx, std::span{pub}.subspan(1, fb))));
        set_prop(ctx, obj, "y",
                 static_cast<JSValueRef>(asym_u8(ctx, std::span{pub}.subspan(1 + fb, fb))));
        set_prop(ctx, obj, "crv", make_string(ctx, asym_jwk_crv(std::string_view{gname, glen})));
        if (!isPublic) {
            asym_set_bn_prop(ctx, obj, "d", pkey.get(), OSSL_PKEY_PARAM_PRIV_KEY, fb);
        }
    } else if (base == EVP_PKEY_ED25519 || base == EVP_PKEY_ED448 || base == EVP_PKEY_X25519 ||
               base == EVP_PKEY_X448) {
        // OKP (RFC 8037): x/d are opaque octet strings, not field elements.
        const char* crv{base == EVP_PKEY_ED25519   ? "Ed25519"
                        : base == EVP_PKEY_ED448   ? "Ed448"
                        : base == EVP_PKEY_X25519  ? "X25519"
                                                   : "X448"};
        set_prop(ctx, obj, "kty", make_string(ctx, "OKP"));
        set_prop(ctx, obj, "crv", make_string(ctx, crv));
        std::vector<std::uint8_t> pub;
        if (asym_octet_param(pkey.get(), OSSL_PKEY_PARAM_PUB_KEY, pub)) {
            set_prop(ctx, obj, "x", static_cast<JSValueRef>(asym_u8(ctx, pub)));
        }
        std::vector<std::uint8_t> priv;
        if (!isPublic && asym_octet_param(pkey.get(), OSSL_PKEY_PARAM_PRIV_KEY, priv)) {
            set_prop(ctx, obj, "d", static_cast<JSValueRef>(asym_u8(ctx, priv)));
        }
    } else {
        return hasher_throw(global, "jwkExport: unsupported key type");
    }
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(obj)));
}

// Read a raw-byte component off a JWK-ish object (values are Uint8Array).
inline bool asym_jwk_part(JSC::JSGlobalObject* global, JSContextRef ctx, JSObjectRef obj,
                          const char* name, std::vector<std::uint8_t>& out) {
    return asym_read(global, toJS(global, get_prop(ctx, obj, name)), out);
}

// __mbunCryptoAsymNative.jwkImport(jwk, isPrivate) → DER Uint8Array (PKCS8 when
//   isPrivate, else SPKI). Returning DER (rather than an opaque handle) lets
//   every downstream path treat a JWK-sourced key like any other key material.
JSC_DEFINE_HOST_FUNCTION(node_asym_jwk_import_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    JSC::JSValue v0{native_arg(frame, 0)};
    if (!v0 || !v0.isObject()) { return hasher_throw(global, "jwkImport: jwk object required"); }
    JSObjectRef jwk{JSValueToObject(ctx, toRef(global, v0), nullptr)};
    if (jwk == nullptr) { return hasher_throw(global, "jwkImport: jwk object required"); }
    const bool isPrivate{[&] { JSC::JSValue v{native_arg(frame, 1)}; return v && v.toBoolean(global); }()};
    const std::string kty{asym_str(global, toJS(global, get_prop(ctx, jwk, "kty")))};

    PkeyPtr pkey;
    if (kty == "RSA") {
        std::vector<std::uint8_t> n;
        std::vector<std::uint8_t> e;
        if (!asym_jwk_part(global, ctx, jwk, "n", n) || !asym_jwk_part(global, ctx, jwk, "e", e)) {
            return hasher_throw(global, "jwkImport: RSA requires n and e");
        }
        OSSL_PARAM_BLD* bld{OSSL_PARAM_BLD_new()};
        if (bld == nullptr) { return hasher_throw(global, "jwkImport: out of memory"); }
        // Every BIGNUM must outlive OSSL_PARAM_BLD_to_param, so they are freed
        // only after fromdata (push_BN copies the value, not the pointer, but
        // the builder reads it lazily at to_param time).
        std::vector<BIGNUM*> owned;
        const auto push{[&](const char* key, std::span<const std::uint8_t> b) {
            BIGNUM* bn{BN_bin2bn(b.data(), static_cast<int>(b.size()), nullptr)};
            if (bn != nullptr) {
                OSSL_PARAM_BLD_push_BN(bld, key, bn);
                owned.push_back(bn);
            }
        }};
        push(OSSL_PKEY_PARAM_RSA_N, n);
        push(OSSL_PKEY_PARAM_RSA_E, e);
        std::vector<std::uint8_t> d;
        std::vector<std::uint8_t> p;
        std::vector<std::uint8_t> q;
        std::vector<std::uint8_t> dp;
        std::vector<std::uint8_t> dq;
        std::vector<std::uint8_t> qi;
        if (isPrivate) {
            if (!asym_jwk_part(global, ctx, jwk, "d", d)) {
                OSSL_PARAM_BLD_free(bld);
                for (BIGNUM* b : owned) { BN_free(b); }
                return hasher_throw(global, "jwkImport: RSA private key requires d");
            }
            push(OSSL_PKEY_PARAM_RSA_D, d);
            // CRT parameters are optional in RFC 7518 but OpenSSL needs the full
            // set or none; push only when all five are present.
            if (asym_jwk_part(global, ctx, jwk, "p", p) && asym_jwk_part(global, ctx, jwk, "q", q) &&
                asym_jwk_part(global, ctx, jwk, "dp", dp) &&
                asym_jwk_part(global, ctx, jwk, "dq", dq) &&
                asym_jwk_part(global, ctx, jwk, "qi", qi)) {
                push(OSSL_PKEY_PARAM_RSA_FACTOR1, p);
                push(OSSL_PKEY_PARAM_RSA_FACTOR2, q);
                push(OSSL_PKEY_PARAM_RSA_EXPONENT1, dp);
                push(OSSL_PKEY_PARAM_RSA_EXPONENT2, dq);
                push(OSSL_PKEY_PARAM_RSA_COEFFICIENT1, qi);
            }
        }
        OSSL_PARAM* params{OSSL_PARAM_BLD_to_param(bld)};
        EVP_PKEY* raw{nullptr};
        PkeyCtxPtr pctx{EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)};
        if (pctx && params != nullptr && EVP_PKEY_fromdata_init(pctx.get()) > 0) {
            EVP_PKEY_fromdata(pctx.get(), &raw,
                              isPrivate ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params);
        }
        if (params != nullptr) { OSSL_PARAM_free(params); }
        OSSL_PARAM_BLD_free(bld);
        for (BIGNUM* b : owned) { BN_free(b); }
        pkey.reset(raw);
    } else if (kty == "EC") {
        const std::string crv{asym_str(global, toJS(global, get_prop(ctx, jwk, "crv")))};
        std::vector<std::uint8_t> x;
        std::vector<std::uint8_t> y;
        if (!asym_jwk_part(global, ctx, jwk, "x", x) || !asym_jwk_part(global, ctx, jwk, "y", y)) {
            return hasher_throw(global, "jwkImport: EC requires x and y");
        }
        if (x.size() != y.size()) { return hasher_throw(global, "jwkImport: EC x/y size mismatch"); }
        // Rebuild the uncompressed point the EC param API expects.
        std::vector<std::uint8_t> point;
        point.reserve(1 + x.size() + y.size());
        point.push_back(0x04);
        point.insert(point.end(), x.begin(), x.end());
        point.insert(point.end(), y.begin(), y.end());
        std::vector<std::uint8_t> d;
        if (isPrivate && !asym_jwk_part(global, ctx, jwk, "d", d)) {
            return hasher_throw(global, "jwkImport: EC private key requires d");
        }
        pkey.reset(asym_ec_from_parts(asym_jwk_group(crv), point, d));
    } else if (kty == "OKP") {
        const std::string crv{asym_str(global, toJS(global, get_prop(ctx, jwk, "crv")))};
        std::vector<std::uint8_t> x;
        std::vector<std::uint8_t> d;
        const bool haveD{isPrivate && asym_jwk_part(global, ctx, jwk, "d", d)};
        if (isPrivate && !haveD) { return hasher_throw(global, "jwkImport: OKP private key requires d"); }
        if (!isPrivate && !asym_jwk_part(global, ctx, jwk, "x", x)) {
            return hasher_throw(global, "jwkImport: OKP requires x");
        }
        OSSL_PARAM_BLD* bld{OSSL_PARAM_BLD_new()};
        if (bld == nullptr) { return hasher_throw(global, "jwkImport: out of memory"); }
        if (haveD) {
            OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PRIV_KEY, d.data(), d.size());
        } else {
            OSSL_PARAM_BLD_push_octet_string(bld, OSSL_PKEY_PARAM_PUB_KEY, x.data(), x.size());
        }
        OSSL_PARAM* params{OSSL_PARAM_BLD_to_param(bld)};
        EVP_PKEY* raw{nullptr};
        PkeyCtxPtr pctx{EVP_PKEY_CTX_new_from_name(nullptr, crv.c_str(), nullptr)};
        if (pctx && params != nullptr && EVP_PKEY_fromdata_init(pctx.get()) > 0) {
            EVP_PKEY_fromdata(pctx.get(), &raw, haveD ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY,
                              params);
        }
        if (params != nullptr) { OSSL_PARAM_free(params); }
        OSSL_PARAM_BLD_free(bld);
        pkey.reset(raw);
    } else {
        return hasher_throw(global, "jwkImport: unsupported kty: " + kty);
    }
    if (!pkey) { return hasher_throw(global, "jwkImport: invalid JWK: " + asym_ossl_err()); }

    auto enc{asym_encode_pkey(pkey.get(), isPrivate ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY,
                              isPrivate ? "PrivateKeyInfo" : "SubjectPublicKeyInfo", false, "", "")};
    if (!enc) { return hasher_throw(global, "jwkImport: encode failed: " + asym_ossl_err()); }
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(asym_u8(ctx, *enc))));
}

// __mbunCryptoAsymNative.keyExport(key, passphrase, isPublic, type, format,
//   cipher, outPassphrase) → PEM string or DER Uint8Array (KeyObject.export).
JSC_DEFINE_HOST_FUNCTION(node_asym_key_export_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    std::vector<std::uint8_t> keyBytes;
    if (!asym_read(global, native_arg(frame, 0), keyBytes)) {
        return hasher_throw(global, "keyExport: key required");
    }
    const AsymPass pass{asym_opt_str(global, native_arg(frame, 1))};
    const bool isPublic{[&] { JSC::JSValue v{native_arg(frame, 2)}; return v && v.toBoolean(global); }()};
    const std::string type{asym_str(global, native_arg(frame, 3))};
    const std::string format{asym_str(global, native_arg(frame, 4))};
    const std::string cipher{asym_str(global, native_arg(frame, 5))};
    const std::string outPass{asym_str(global, native_arg(frame, 6))};

    PkeyPtr pkey{asym_load_pkey(keyBytes, pass)};
    if (!pkey) {
        return hasher_throw(global, asym_key_error(keyBytes, pass.has_value(), "Invalid key"));
    }
    // An EC key decoded from a compressed SPKI keeps that point format, but
    // node (and WebCrypto) always re-emit the uncompressed point.
    // ref: nodejs/node#45859.
    if (EVP_PKEY_base_id(pkey.get()) == EVP_PKEY_EC) {
        EVP_PKEY_set_utf8_string_param(pkey.get(), OSSL_PKEY_PARAM_EC_POINT_CONVERSION_FORMAT,
                                       "uncompressed");
    }
    int sel{0};
    const char* structure{nullptr};
    if (!asym_enc_map(type, !isPublic, sel, structure)) {
        return hasher_throw(global, "Unsupported key export type");
    }
    const bool pem{format != "der"};
    auto enc{asym_encode_pkey(pkey.get(), sel, structure, pem, cipher, outPass)};
    if (!enc) { return hasher_throw(global, "key export failed: " + asym_ossl_err()); }
    if (pem) {
        return JSC::JSValue::encode(toJS(
            global, make_string(ctx, std::string_view{reinterpret_cast<const char*>(enc->data()),
                                                      enc->size()})));
    }
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(asym_u8(ctx, *enc))));
}

// X509_NAME → node-style multi-line "CN=..\n.." string (RFC2253 order reversed).
inline std::string asym_x509_name(X509_NAME* name) {
    std::string out;
    const int n{X509_NAME_entry_count(name)};
    for (int i = 0; i < n; ++i) {
        X509_NAME_ENTRY* e{X509_NAME_get_entry(name, i)};
        ASN1_OBJECT* obj{X509_NAME_ENTRY_get_object(e)};
        ASN1_STRING* val{X509_NAME_ENTRY_get_data(e)};
        char sn[80];
        const int nid{OBJ_obj2nid(obj)};
        const char* key{nid != NID_undef ? OBJ_nid2sn(nid) : (OBJ_obj2txt(sn, sizeof(sn), obj, 1), sn)};
        unsigned char* utf8{nullptr};
        const int len{ASN1_STRING_to_UTF8(&utf8, val)};
        if (!out.empty()) { out.push_back('\n'); }
        out.append(key);
        out.push_back('=');
        if (len > 0 && utf8 != nullptr) { out.append(reinterpret_cast<char*>(utf8), len); }
        if (utf8 != nullptr) { OPENSSL_free(utf8); }
    }
    return out;
}

// __mbunCryptoAsymNative.x509parse(pemOrDer) → { subject, issuer, validFrom,
//   validTo, serialNumber, fingerprint, fingerprint256, subjectAltName,
//   publicKey(SPKI PEM), raw(DER) }.
JSC_DEFINE_HOST_FUNCTION(node_asym_x509_parse_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    std::vector<std::uint8_t> input;
    if (!asym_read(global, native_arg(frame, 0), input)) {
        return hasher_throw(global, "X509Certificate: input required");
    }
    X509* cert{nullptr};
    BIO* bio{BIO_new_mem_buf(input.data(), static_cast<int>(input.size()))};
    if (bio != nullptr) { cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr); BIO_free(bio); }
    if (cert == nullptr) {
        const unsigned char* p{input.data()};
        cert = d2i_X509(nullptr, &p, static_cast<long>(input.size()));
    }
    if (cert == nullptr) { return hasher_throw(global, "Failed to parse X509 certificate"); }

    JSObjectRef obj{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, obj, "subject", make_string(ctx, asym_x509_name(X509_get_subject_name(cert))));
    set_prop(ctx, obj, "issuer", make_string(ctx, asym_x509_name(X509_get_issuer_name(cert))));

    auto asn1_time_str = [&](const ASN1_TIME* t) {
        std::string s;
        BIO* b{BIO_new(BIO_s_mem())};
        if (b != nullptr) {
            ASN1_TIME_print(b, t);
            char* data{nullptr};
            const long len{BIO_get_mem_data(b, &data)};
            if (len > 0) { s.assign(data, len); }
            BIO_free(b);
        }
        return s;
    };
    set_prop(ctx, obj, "validFrom", make_string(ctx, asn1_time_str(X509_get0_notBefore(cert))));
    set_prop(ctx, obj, "validTo", make_string(ctx, asn1_time_str(X509_get0_notAfter(cert))));

    // BN_bn2hex is uppercase on OpenSSL but lowercase on BoringSSL; Node.js/OpenSSL
    // emits uppercase, so normalize (cert pinning by serial/modulus string must match).
    auto to_upper_hex = [](const char* hex) {
        std::string s{hex};
        for (char& c : s) { if (c >= 'a' && c <= 'f') { c = static_cast<char>(c - 32); } }
        return s;
    };

    // Serial number (uppercase hex).
    if (ASN1_INTEGER* serial{X509_get_serialNumber(cert)}) {
        BIGNUM* bn{ASN1_INTEGER_to_BN(serial, nullptr)};
        if (bn != nullptr) {
            char* hex{BN_bn2hex(bn)};
            if (hex != nullptr) { set_prop(ctx, obj, "serialNumber", make_string(ctx, to_upper_hex(hex))); OPENSSL_free(hex); }
            BN_free(bn);
        }
    }

    auto fingerprint = [&](const EVP_MD* md) {
        unsigned char out[EVP_MAX_MD_SIZE];
        unsigned int len{0};
        std::string s;
        if (X509_digest(cert, md, out, &len) == 1) {
            static constexpr char HEX[]{"0123456789ABCDEF"};
            for (unsigned int i = 0; i < len; ++i) {
                if (i != 0) { s.push_back(':'); }
                s.push_back(HEX[out[i] >> 4]);
                s.push_back(HEX[out[i] & 0x0f]);
            }
        }
        return s;
    };
    set_prop(ctx, obj, "fingerprint", make_string(ctx, fingerprint(EVP_sha1())));
    set_prop(ctx, obj, "fingerprint256", make_string(ctx, fingerprint(EVP_sha256())));
    set_prop(ctx, obj, "fingerprint512", make_string(ctx, fingerprint(EVP_sha512())));

    // subjectAltName (node format: "DNS:a, DNS:b, IP Address:1.2.3.4").
    //
    // SECURITY: the textual form MUST NOT be produced by copying the raw ASN.1
    // bytes. A DNS SAN may legally carry an embedded NUL, a comma or a quote,
    // and every one of those lets a certificate's altname list be misread:
    //   - an embedded NUL truncates the name in any C-string consumer, so a
    //     certificate issued for "good.example.org\0.evil.example.com" reads
    //     back as "good.example.org" and checkServerIdentity accepts it for the
    //     victim host (the classic null-prefix spoof, CVE-2009-2408 class);
    //   - a comma forges an extra list entry, because checkServerIdentity splits
    //     the string on ", ".
    // node therefore JSON-escapes any name that is not "safe", using the raw
    // ASN1_STRING bytes (never ASN1_STRING_to_UTF8, which stops at the NUL).
    // Translated from deps/ncrypto/ncrypto.cc IsSafeAltName / PrintAltName /
    // PrintGeneralName. ref test-tls-0-dns-altname.
    const auto is_safe_alt_name{[](const unsigned char* d, int n) {
        for (int i = 0; i < n; ++i) {
            const char c{static_cast<char>(d[i])};
            if (c == '"' || c == '\\' || c == ',' || c == '\'') { return false; }
            if (c < ' ' || c > '~') { return false; }
        }
        return true;
    }};
    // Appends the value, escaping it exactly as node does when it is not safe.
    // The prefix ("DNS:", "email:", "URI:") is written by the caller and stays
    // OUTSIDE the quotes, matching PrintGeneralName.
    const auto append_alt_name{[&is_safe_alt_name](std::string& out,
                                                   const unsigned char* d, int n) {
        if (d == nullptr || n < 0) { return; }
        if (is_safe_alt_name(d, n)) {
            out.append(reinterpret_cast<const char*>(d), static_cast<std::size_t>(n));
            return;
        }
        static constexpr char kHex[]{"0123456789abcdef"};
        out.push_back('"');
        for (int i = 0; i < n; ++i) {
            const char c{static_cast<char>(d[i])};
            if (c == '\\') {
                out.append("\\\\");
            } else if (c == '"') {
                out.append("\\\"");
            } else if (c >= ' ' && c != ',' && c <= '~') {
                out.push_back(c);
            } else {
                // Everything else is treated as Latin-1 (the first 255 code
                // points), which is what node emits.
                out.append("\\u00");
                out.push_back(kHex[(static_cast<unsigned char>(c) & 0xF0U) >> 4U]);
                out.push_back(kHex[static_cast<unsigned char>(c) & 0x0FU]);
            }
        }
        out.push_back('"');
    }};
    std::string san;
    if (auto* names{static_cast<GENERAL_NAMES*>(
            X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr))}) {
        const int count{sk_GENERAL_NAME_num(names)};
        for (int i = 0; i < count; ++i) {
            GENERAL_NAME* gen{sk_GENERAL_NAME_value(names, i)};
            if (gen->type == GEN_DNS) {
                const ASN1_IA5STRING* nm{gen->d.dNSName};
                if (nm != nullptr && ASN1_STRING_length(nm) > 0) {
                    if (!san.empty()) { san.append(", "); }
                    san.append("DNS:");
                    append_alt_name(san, ASN1_STRING_get0_data(nm), ASN1_STRING_length(nm));
                }
            } else if (gen->type == GEN_EMAIL) {
                const ASN1_IA5STRING* nm{gen->d.rfc822Name};
                if (nm != nullptr && ASN1_STRING_length(nm) > 0) {
                    if (!san.empty()) { san.append(", "); }
                    san.append("email:");
                    append_alt_name(san, ASN1_STRING_get0_data(nm), ASN1_STRING_length(nm));
                }
            } else if (gen->type == GEN_URI) {
                const ASN1_IA5STRING* nm{gen->d.uniformResourceIdentifier};
                if (nm != nullptr && ASN1_STRING_length(nm) > 0) {
                    if (!san.empty()) { san.append(", "); }
                    san.append("URI:");
                    append_alt_name(san, ASN1_STRING_get0_data(nm), ASN1_STRING_length(nm));
                }
            } else if (gen->type == GEN_IPADD) {
                // node prints "IP Address:<v4 dotted>" or the full 8-group IPv6
                // form (no :: compression, lowercase hex) — lib/tls.js formats the
                // raw octets directly. iPAddress is 4 bytes (v4) or 16 bytes (v6).
                const ASN1_OCTET_STRING* ip{gen->d.iPAddress};
                const int len{ip != nullptr ? ASN1_STRING_length(ip) : 0};
                const unsigned char* data{ip != nullptr ? ASN1_STRING_get0_data(ip) : nullptr};
                std::string ipstr;
                if (data != nullptr && len == 4) {
                    ipstr = std::to_string(data[0]) + "." + std::to_string(data[1]) + "."
                            + std::to_string(data[2]) + "." + std::to_string(data[3]);
                } else if (data != nullptr && len == 16) {
                    char grp[8];
                    for (int g = 0; g < 8; ++g) {
                        const unsigned v{static_cast<unsigned>((data[g * 2] << 8) | data[g * 2 + 1])};
                        std::snprintf(grp, sizeof grp, "%x", v);
                        if (g != 0) { ipstr.push_back(':'); }
                        ipstr.append(grp);
                    }
                }
                if (!ipstr.empty()) {
                    if (!san.empty()) { san.append(", "); }
                    san.append("IP Address:").append(ipstr);
                }
            }
        }
        GENERAL_NAMES_free(names);
    }
    if (!san.empty()) { set_prop(ctx, obj, "subjectAltName", make_string(ctx, san)); }

    // infoAccess (AIA): node format "<method> - URI:<uri>\n" per description.
    if (auto* aia{static_cast<AUTHORITY_INFO_ACCESS*>(
            X509_get_ext_d2i(cert, NID_info_access, nullptr, nullptr))}) {
        std::string ia;
        const int adn{sk_ACCESS_DESCRIPTION_num(aia)};
        for (int i = 0; i < adn; ++i) {
            ACCESS_DESCRIPTION* ad{sk_ACCESS_DESCRIPTION_value(aia, i)};
            char m[128];
            OBJ_obj2txt(m, sizeof(m), ad->method, 0);   // long name: "OCSP"/"CA Issuers"
            ia.append(m).append(" - ");
            if (ad->location != nullptr && ad->location->type == GEN_URI) {
                unsigned char* u{nullptr};
                const int ul{ASN1_STRING_to_UTF8(&u, ad->location->d.uniformResourceIdentifier)};
                if (ul > 0 && u != nullptr) { ia.append("URI:").append(reinterpret_cast<char*>(u), ul); }
                if (u != nullptr) { OPENSSL_free(u); }
            }
            ia.push_back('\n');
        }
        AUTHORITY_INFO_ACCESS_free(aia);
        if (!ia.empty()) { set_prop(ctx, obj, "infoAccess", make_string(ctx, ia)); }
    }

    // keyUsage: extended key usage as dotted-OID string array (node semantics).
    if (auto* eku{static_cast<EXTENDED_KEY_USAGE*>(
            X509_get_ext_d2i(cert, NID_ext_key_usage, nullptr, nullptr))}) {
        const int ekn{sk_ASN1_OBJECT_num(eku)};
        std::vector<JSValueRef> items;
        items.reserve(static_cast<std::size_t>(ekn));
        for (int i = 0; i < ekn; ++i) {
            char b[128];
            OBJ_obj2txt(b, sizeof(b), sk_ASN1_OBJECT_value(eku, i), 1);   // numeric dotted OID
            items.push_back(make_string(ctx, b));
        }
        EXTENDED_KEY_USAGE_free(eku);
        JSObjectRef arr{JSObjectMakeArray(ctx, items.size(), items.data(), nullptr)};
        set_prop(ctx, obj, "keyUsage", static_cast<JSValueRef>(arr));
    }

    // Public key as SPKI PEM (for cert.publicKey).
    if (EVP_PKEY* pub{X509_get_pubkey(cert)}) {
        if (auto enc{asym_encode_pkey(pub, EVP_PKEY_PUBLIC_KEY, "SubjectPublicKeyInfo", true, {}, {})}) {
            set_prop(ctx, obj, "publicKey",
                     make_string(ctx, std::string_view{reinterpret_cast<const char*>(enc->data()),
                                                       enc->size()}));
        }
        // RSA modulus as uppercase hex (legacy object / cert pinning by modulus string).
        if (EVP_PKEY_base_id(pub) == EVP_PKEY_RSA) {
            BIGNUM* n{nullptr};
            if (EVP_PKEY_get_bn_param(pub, OSSL_PKEY_PARAM_RSA_N, &n) > 0 && n != nullptr) {
                char* hex{BN_bn2hex(n)};
                if (hex != nullptr) { set_prop(ctx, obj, "modulus", make_string(ctx, to_upper_hex(hex))); OPENSSL_free(hex); }
                BN_free(n);
            }
            // Public exponent as node's "0x<hex>" form (leading zeros stripped,
            // lowercase) — node getPeerCertificate().exponent.
            BIGNUM* e{nullptr};
            if (EVP_PKEY_get_bn_param(pub, OSSL_PKEY_PARAM_RSA_E, &e) > 0 && e != nullptr) {
                char* hex{BN_bn2hex(e)};
                if (hex != nullptr) {
                    std::string h{hex};
                    OPENSSL_free(hex);
                    std::size_t nz{h.find_first_not_of('0')};
                    h = nz == std::string::npos ? std::string{"0"} : h.substr(nz);
                    for (char& c : h) { if (c >= 'A' && c <= 'F') { c = static_cast<char>(c - 'A' + 'a'); } }
                    set_prop(ctx, obj, "exponent", make_string(ctx, "0x" + h));
                }
                BN_free(e);
            }
            set_prop(ctx, obj, "bits", JSValueMakeNumber(ctx, static_cast<double>(EVP_PKEY_bits(pub))));
        }
        // pubkey: DER of the SubjectPublicKeyInfo (node getPeerCertificate().pubkey
        // is a Buffer of the encoded public key).
        if (int pkLen{i2d_PUBKEY(pub, nullptr)}; pkLen > 0) {
            std::vector<std::uint8_t> pk(static_cast<std::size_t>(pkLen));
            unsigned char* pq{pk.data()};
            i2d_PUBKEY(pub, &pq);
            set_prop(ctx, obj, "pubkey", static_cast<JSValueRef>(asym_u8(ctx, pk)));
        }
        EVP_PKEY_free(pub);
    }
    // Whole cert DER (for .raw / .toString round-trips).
    if (int derLen{i2d_X509(cert, nullptr)}; derLen > 0) {
        std::vector<std::uint8_t> der(static_cast<std::size_t>(derLen));
        unsigned char* q{der.data()};
        i2d_X509(cert, &q);
        set_prop(ctx, obj, "raw", static_cast<JSValueRef>(asym_u8(ctx, der)));
    }
    // ca flag (basic constraints).
    set_prop(ctx, obj, "ca", JSValueMakeBoolean(ctx, X509_check_ca(cert) > 0));

    X509_free(cert);
    return JSC::JSValue::encode(toJS(global, static_cast<JSValueRef>(obj)));
}

// __mbunCryptoAsymNative.x509checkIssued(subjectDer, issuerDer) → bool.
// True iff `issuer` actually issued `subject` (X509_check_issued == X509_V_OK).
// Mirrors node's cert.checkIssued (JSX509Certificate.cpp:653 / ncrypto.cpp:1255).
JSC_DEFINE_HOST_FUNCTION(node_asym_x509_check_issued_host,
                         (JSC::JSGlobalObject* global, JSC::CallFrame* frame)) {
    JSContextRef ctx{toRef(global)};
    std::vector<std::uint8_t> subjIn, issIn;
    if (!asym_read(global, native_arg(frame, 0), subjIn) ||
        !asym_read(global, native_arg(frame, 1), issIn)) {
        return JSC::JSValue::encode(toJS(global, JSValueMakeBoolean(ctx, false)));
    }
    auto parse = [](std::vector<std::uint8_t>& in) -> X509* {
        X509* c{nullptr};
        if (BIO* b{BIO_new_mem_buf(in.data(), static_cast<int>(in.size()))}) {
            c = PEM_read_bio_X509(b, nullptr, nullptr, nullptr);
            BIO_free(b);
        }
        if (c == nullptr) {
            const unsigned char* p{in.data()};
            c = d2i_X509(nullptr, &p, static_cast<long>(in.size()));
        }
        return c;
    };
    X509* s{parse(subjIn)};
    X509* i{parse(issIn)};
    const bool ok{s != nullptr && i != nullptr && X509_check_issued(i, s) == X509_V_OK};
    if (s != nullptr) { X509_free(s); }
    if (i != nullptr) { X509_free(i); }
    return JSC::JSValue::encode(toJS(global, JSValueMakeBoolean(ctx, ok)));
}
