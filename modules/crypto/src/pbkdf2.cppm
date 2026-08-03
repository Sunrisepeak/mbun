// mbun.crypto.pbkdf2 — RFC 8018 PBKDF2 over the module's HMAC.
//
// ref: bun src/jsc/bindings/webcrypto/CryptoAlgorithmPBKDF2.cpp (which delegates
//      to BoringSSL PKCS5_PBKDF2_HMAC). Pure-C++ here, built on mbun.crypto.hmac
//      so it inherits every supported PRF (HMAC-SHA1/224/256/384/512/512-256).

export module mbun.crypto.pbkdf2;

import std;
import mbun.crypto.hasher;
import mbun.crypto.hmac;

export namespace mbun::crypto {

// Derive dkLen bytes from (password, salt) using PBKDF2-HMAC-<algorithm>.
// Returns nullopt for an unsupported PRF or invalid (iterations==0/dkLen==0)
// parameters. Mirrors OpenSSL/BoringSSL PKCS5_PBKDF2_HMAC semantics.
inline std::optional<std::vector<std::uint8_t>> pbkdf2(
    DigestAlgo algorithm, std::span<const std::uint8_t> password,
    std::span<const std::uint8_t> salt, std::uint32_t iterations, std::size_t dkLen) {
    if (iterations == 0 || dkLen == 0) { return std::nullopt; }

    // hLen = HMAC output length for this PRF. Probe once with an empty message.
    auto probe { hmac(algorithm, password, {}) };
    if (!probe) { return std::nullopt; }
    const std::size_t hLen { probe->size() };
    if (hLen == 0) { return std::nullopt; }

    std::vector<std::uint8_t> dk;
    dk.reserve(dkLen);

    const std::size_t blocks { (dkLen + hLen - 1) / hLen };
    std::vector<std::uint8_t> saltBlock(salt.begin(), salt.end());
    saltBlock.resize(salt.size() + 4);

    for (std::size_t i { 1 }; i <= blocks; ++i) {
        // INT_32_BE(i) appended to the salt for U_1.
        saltBlock[salt.size() + 0] = static_cast<std::uint8_t>(i >> 24);
        saltBlock[salt.size() + 1] = static_cast<std::uint8_t>(i >> 16);
        saltBlock[salt.size() + 2] = static_cast<std::uint8_t>(i >> 8);
        saltBlock[salt.size() + 3] = static_cast<std::uint8_t>(i);

        auto u { hmac(algorithm, password, saltBlock) };
        if (!u) { return std::nullopt; }
        std::vector<std::uint8_t> t { *u };
        for (std::uint32_t j { 1 }; j < iterations; ++j) {
            u = hmac(algorithm, password, *u);
            if (!u) { return std::nullopt; }
            for (std::size_t k { 0 }; k < hLen; ++k) { t[k] ^= (*u)[k]; }
        }
        dk.insert(dk.end(), t.begin(), t.end());
    }

    dk.resize(dkLen);
    return dk;
}

}  // namespace mbun::crypto
