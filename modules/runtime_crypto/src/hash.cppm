// Runtime hash dispatch modeled after bun runtime/api/HashObject.rs and
// runtime/api/HashObject.zig. The algorithm implementation remains in
// mbun.crypto; this layer preserves runtime names, seed widths, and outputs.
export module mbun.runtime_crypto.hash;

import std;
import mbun.crypto;

export namespace mbun::runtime_crypto {

using HashAlgorithm = mbun::crypto::HashAlgo;
using HashResult = mbun::crypto::HashResult;

struct HashRequest {
    HashAlgorithm algorithm;
    std::uint64_t seed { 0 };
    std::span<const std::uint8_t> input {};
};

inline std::optional<HashAlgorithm> hash_algorithm_from(std::string_view name) {
    return mbun::crypto::hash_algo_from(name);
}

inline HashResult hash(const HashRequest& request) {
    return mbun::crypto::bun_hash(request.algorithm, request.seed, request.input);
}

inline std::uint64_t hash_default(std::span<const std::uint8_t> input,
                                  std::uint64_t seed = 0) {
    return mbun::crypto::bun_hash_default(input, seed);
}

}  // namespace mbun::runtime_crypto
