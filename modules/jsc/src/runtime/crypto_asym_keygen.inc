// Key-generation helpers split from crypto_asym.inc to keep each runtime
// implementation slice within the structural 2000-line budget.

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
inline EVP_PKEY* asym_rsa_pss_gen(std::size_t bits, std::string_view digest,
                                  std::string_view mgf1Digest, int saltLength) {
    PkeyCtxPtr pc{EVP_PKEY_CTX_new_from_name(nullptr, "RSA-PSS", nullptr)};
    if (!pc || EVP_PKEY_keygen_init(pc.get()) <= 0) { return nullptr; }
    // A bare RSA-PSS key has no RSASSA-PSS-params sequence. Once node's
    // hashAlgorithm option is present, the restrictions must be embedded in
    // the key itself so they survive PEM/DER serialization and keyType().
    std::string digestName{digest};
    std::string mgf1Name{mgf1Digest};
    OSSL_PARAM params[5];
    int n{};
    params[n++] = OSSL_PARAM_construct_size_t(OSSL_PKEY_PARAM_RSA_BITS, &bits);
    if (!digestName.empty()) {
        params[n++] = OSSL_PARAM_construct_utf8_string(
            OSSL_PKEY_PARAM_RSA_DIGEST, digestName.data(), 0);
        params[n++] = OSSL_PARAM_construct_utf8_string(
            OSSL_PKEY_PARAM_RSA_MGF1_DIGEST, mgf1Name.data(), 0);
        params[n++] = OSSL_PARAM_construct_int(OSSL_PKEY_PARAM_RSA_PSS_SALTLEN, &saltLength);
    }
    params[n] = OSSL_PARAM_construct_end();
    if (EVP_PKEY_CTX_set_params(pc.get(), params) <= 0) { return nullptr; }
    EVP_PKEY* key{nullptr};
    if (EVP_PKEY_generate(pc.get(), &key) <= 0) { return nullptr; }
    return key;
}

// RSA keygen with a caller-selected public exponent. EVP_RSA_gen hard-codes
// F4, while node forwards options.publicExponent to OpenSSL's RSA keymgmt.
inline EVP_PKEY* asym_rsa_gen(std::size_t bits, unsigned long exponent) {
    PkeyCtxPtr pc{EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr)};
    BIGNUM* e{BN_new()};
    OSSL_PARAM_BLD* bld{OSSL_PARAM_BLD_new()};
    if (!pc || !e || !bld || BN_set_word(e, exponent) != 1 ||
        EVP_PKEY_keygen_init(pc.get()) <= 0) {
        if (bld != nullptr) { OSSL_PARAM_BLD_free(bld); }
        if (e != nullptr) { BN_free(e); }
        return nullptr;
    }
    OSSL_PARAM_BLD_push_size_t(bld, OSSL_PKEY_PARAM_RSA_BITS, bits);
    OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e);
    OSSL_PARAM* params{OSSL_PARAM_BLD_to_param(bld)};
    const bool configured{params != nullptr && EVP_PKEY_CTX_set_params(pc.get(), params) > 0};
    if (params != nullptr) { OSSL_PARAM_free(params); }
    OSSL_PARAM_BLD_free(bld);
    BN_free(e);
    EVP_PKEY* key{nullptr};
    if (!configured || EVP_PKEY_generate(pc.get(), &key) <= 0) { return nullptr; }
    return key;
}
