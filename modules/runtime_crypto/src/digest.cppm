// CryptoHasher lifecycle modeled after bun runtime/crypto/CryptoHasher.rs and
// runtime/crypto/CryptoHasher.zig. BoringSSL EVP/HMAC objects are represented
// by the backend seam; the pure implementation delegates to mbun.crypto.
export module mbun.runtime_crypto.digest;

import std;
import mbun.crypto;

export namespace mbun::runtime_crypto {

using DigestAlgorithm = mbun::crypto::DigestAlgo;
using DigestEncoding = mbun::crypto::Encoding;

inline std::optional<DigestAlgorithm> digest_algorithm_from(std::string_view name) {
    return mbun::crypto::digest_algo_from(name);
}

class DigestSession {
private:
    mbun::crypto::CryptoHasher hasher_;

public:
    explicit DigestSession(DigestAlgorithm algorithm) : hasher_ { algorithm } {}

    static std::optional<DigestSession> by_name(std::string_view name) {
        auto hasher { mbun::crypto::CryptoHasher::by_name(name) };
        if (!hasher) { return std::nullopt; }
        return DigestSession { std::move(*hasher) };
    }

    DigestAlgorithm algorithm() const { return hasher_.algorithm(); }
    std::size_t digest_length() const { return hasher_.digest_length(); }

    void update(std::span<const std::uint8_t> input) { hasher_.update(input); }
    void update(std::string_view input) { hasher_.update(input); }

    std::vector<std::uint8_t> digest() { return hasher_.digest(); }
    std::string digest(DigestEncoding encoding) { return hasher_.digest(encoding); }

    DigestSession copy() const { return DigestSession { hasher_.copy() }; }

private:
    explicit DigestSession(mbun::crypto::CryptoHasher hasher) : hasher_ { std::move(hasher) } {}
};

}  // namespace mbun::runtime_crypto
