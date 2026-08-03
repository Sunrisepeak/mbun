// Runtime binding adapters modeled after CryptoHasher's host functions and
// HashObject's monomorphic hash wrappers. JSValue/CallFrame decoding stays in
// a future JSC adapter; this module exposes typed, testable request boundaries.
export module mbun.runtime_crypto.binding;

import std;
import mbun.runtime_crypto.backend;
import mbun.runtime_crypto.digest;
import mbun.runtime_crypto.hash;

export namespace mbun::runtime_crypto {

enum class BindingErrorKind : std::uint8_t {
    missing_algorithm,
    unsupported_algorithm,
    backend_unavailable,
};

struct BindingError {
    BindingErrorKind kind { BindingErrorKind::unsupported_algorithm };
    std::string algorithm {};
    std::string detail {};
};

inline std::expected<HashResult, BindingError> hash_binding(std::string_view algorithm,
                                                            std::uint64_t seed,
                                                            std::span<const std::uint8_t> input) {
    if (algorithm.empty()) {
        return std::unexpected(BindingError {BindingErrorKind::missing_algorithm, {}, "algorithm is required"});
    }
    auto selected { hash_algorithm_from(algorithm) };
    if (!selected) {
        return std::unexpected(BindingError {BindingErrorKind::unsupported_algorithm,
                                              std::string { algorithm }, "unknown Bun hash algorithm"});
    }
    return hash(HashRequest {*selected, seed, input});
}

inline std::expected<DigestSession, BindingError> digest_binding(std::string_view algorithm) {
    if (algorithm.empty()) {
        return std::unexpected(BindingError {BindingErrorKind::missing_algorithm, {}, "algorithm is required"});
    }
    auto session { DigestSession::by_name(algorithm) };
    if (!session) {
        return std::unexpected(BindingError {BindingErrorKind::unsupported_algorithm,
                                              std::string { algorithm }, "digest is not in the pure runtime subset"});
    }
    return std::move(*session);
}

inline std::expected<void, BindingError> require_backend(const CryptoBackend& backend,
                                                         BackendOperation operation,
                                                         std::string_view algorithm = {}) {
    if (backend.capabilities().supports(operation)) { return {}; }
    auto error { DeferredBackend::unavailable(operation) };
    return std::unexpected(BindingError {BindingErrorKind::backend_unavailable,
                                          std::string { algorithm }, std::move(error.detail)});
}

}  // namespace mbun::runtime_crypto
