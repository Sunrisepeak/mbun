// mbun.s3_signing.backend — default SigningPrimitives backed by mbun.crypto.
//
// The core mbun.s3_signing module stays crypto-agnostic (injectable
// SigningPrimitives); this thin adapter wires the real SHA-256 / HMAC-SHA256
// from mbun.crypto so callers get correct signatures out of the box.
export module mbun.s3_signing.backend;

import std;
import mbun.s3_signing;
import mbun.crypto;

export namespace mbun::s3_signing {

inline Digest256 crypto_sha256_(std::string_view data) {
    mbun::crypto::CryptoHasher hasher { mbun::crypto::DigestAlgo::Sha256 };
    hasher.update(data);
    const std::vector<std::uint8_t> out { hasher.digest() };
    Digest256 digest {};
    std::copy_n(out.data(), digest.size(), digest.data());
    return digest;
}

inline Digest256 crypto_hmac_sha256_(std::span<const std::uint8_t> key, std::string_view data) {
    const auto out = mbun::crypto::hmac(mbun::crypto::DigestAlgo::Sha256, key, bytes_(data));
    Digest256 digest {};
    if (out) { std::copy_n(out->data(), digest.size(), digest.data()); }
    return digest;
}

// Process-wide default primitives; construct once and reuse.
inline SigningPrimitives default_primitives() {
    return SigningPrimitives { .sha256 = crypto_sha256_, .hmac_sha256 = crypto_hmac_sha256_ };
}

}  // namespace mbun::s3_signing
