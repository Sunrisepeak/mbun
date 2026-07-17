// hmac.cppm — RFC 2104 HMAC over mbun.crypto digest implementations.
//
// ref: bun src/jsc/bindings/webcrypto/CryptoAlgorithmHMAC.cpp
//      src/jsc/bindings/webcrypto/CryptoKeyHMAC.cpp
export module mbun.crypto.hmac;

import std;
import mbun.crypto.hasher;

export namespace mbun::crypto {

constexpr std::size_t hmac_block_size(DigestAlgo algorithm) noexcept {
    switch (algorithm) {
    case DigestAlgo::Md5:
    case DigestAlgo::Sha1:
    case DigestAlgo::Sha224:
    case DigestAlgo::Sha256: return 64;
    case DigestAlgo::Sha384:
    case DigestAlgo::Sha512:
    case DigestAlgo::Sha512_256: return 128;
    }
    return 0;
}

inline std::optional<std::vector<std::uint8_t>> hmac(
    DigestAlgo algorithm, std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> data) {
    const std::size_t blockSize { hmac_block_size(algorithm) };
    if (blockSize == 0) { return std::nullopt; }

    std::vector<std::uint8_t> normalizedKey;
    if (key.size() > blockSize) {
        CryptoHasher keyHasher { algorithm };
        keyHasher.update(key);
        normalizedKey = keyHasher.digest();
        key = normalizedKey;
    }

    std::vector<std::uint8_t> innerPad(blockSize, 0x36);
    std::vector<std::uint8_t> outerPad(blockSize, 0x5c);
    for (std::size_t i { 0 }; i < key.size(); ++i) {
        innerPad[i] ^= key[i];
        outerPad[i] ^= key[i];
    }

    CryptoHasher inner { algorithm };
    inner.update(innerPad);
    inner.update(data);
    const std::vector<std::uint8_t> innerDigest { inner.digest() };

    CryptoHasher outer { algorithm };
    outer.update(outerPad);
    outer.update(innerDigest);
    return outer.digest();
}

inline bool constant_time_equal(std::span<const std::uint8_t> left,
                                std::span<const std::uint8_t> right) noexcept {
    if (left.size() != right.size()) { return false; }
    std::uint8_t difference { 0 };
    for (std::size_t i { 0 }; i < left.size(); ++i) {
        difference |= static_cast<std::uint8_t>(left[i] ^ right[i]);
    }
    return difference == 0;
}

}  // namespace mbun::crypto
