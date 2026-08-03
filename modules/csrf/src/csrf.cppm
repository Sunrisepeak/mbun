// mbun.csrf — Bun CSRF token core.
// Reference: bun-ref/src/csrf/lib.rs and bun-zig-src/src/csrf/csrf.zig.
export module mbun.csrf;

import std;
import mbun.crypto.hasher;

export namespace mbun::csrf {

enum class TokenFormat { Base64, Base64Url, Hex };
enum class Algorithm { Sha256, Sha384, Sha512, Sha512_256 };
enum class Error { EmptySecret, EmptyToken, EmptySessionId, UnsupportedAlgorithm };

struct GenerateOptions {
    std::uint64_t expiresInMs { 24ULL * 60 * 60 * 1000 };
    TokenFormat encoding { TokenFormat::Base64Url };
    Algorithm algorithm { Algorithm::Sha256 };
    std::string_view sessionId {};
    std::uint64_t nowMs {};
};

struct VerifyOptions {
    std::uint64_t maxAgeMs { 24ULL * 60 * 60 * 1000 };
    TokenFormat encoding { TokenFormat::Base64Url };
    Algorithm algorithm { Algorithm::Sha256 };
    std::string_view sessionId {};
    std::uint64_t nowMs {};
};

namespace detail {

std::uint64_t now_ms_() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string_view algorithm_name_(Algorithm algorithm) {
    switch (algorithm) {
    case Algorithm::Sha256: return "sha256";
    case Algorithm::Sha384: return "sha384";
    case Algorithm::Sha512: return "sha512";
    case Algorithm::Sha512_256: return "sha512-256";
    }
    return {};
}

std::size_t block_size_(Algorithm algorithm) {
    return algorithm == Algorithm::Sha384 || algorithm == Algorithm::Sha512
        ? 128 : 64;
}

std::vector<std::uint8_t> bytes_(std::string_view value) {
    return { reinterpret_cast<const std::uint8_t*>(value.data()),
             reinterpret_cast<const std::uint8_t*>(value.data() + value.size()) };
}

std::vector<std::uint8_t> hmac_(std::string_view secret, std::span<const std::uint8_t> message,
                               Algorithm algorithm) {
    const auto name { algorithm_name_(algorithm) };
    const std::size_t blockSize { block_size_(algorithm) };
    std::vector<std::uint8_t> key(blockSize, 0);
    auto secretBytes { bytes_(secret) };
    if (secretBytes.size() > blockSize) {
        auto digest { mbun::crypto::CryptoHasher::hash(name, secretBytes) };
        if (!digest) { return {}; }
        secretBytes = std::move(*digest);
    }
    std::copy(secretBytes.begin(), secretBytes.end(), key.begin());
    std::vector<std::uint8_t> inner(blockSize + message.size());
    std::vector<std::uint8_t> outer(blockSize);
    for (std::size_t i { 0 }; i < blockSize; ++i) {
        inner[i] = key[i] ^ 0x36;
        outer[i] = key[i] ^ 0x5c;
    }
    std::copy(message.begin(), message.end(), inner.begin() + blockSize);
    auto innerDigest { mbun::crypto::CryptoHasher::hash(name, inner) };
    if (!innerDigest) { return {}; }
    outer.insert(outer.end(), innerDigest->begin(), innerDigest->end());
    auto result { mbun::crypto::CryptoHasher::hash(name, outer) };
    return result ? std::move(*result) : std::vector<std::uint8_t> {};
}

void write_u64_(std::uint8_t* out, std::uint64_t value) {
    for (int i { 0 }; i < 8; ++i) { out[i] = static_cast<std::uint8_t>(value >> (56 - i * 8)); }
}

std::uint64_t read_u64_(const std::uint8_t* in) {
    std::uint64_t value { 0 };
    for (int i { 0 }; i < 8; ++i) { value = (value << 8) | in[i]; }
    return value;
}

std::vector<std::uint8_t> decode_(std::string_view token, TokenFormat format) {
    while (!token.empty() && (token.front() == '\r' || token.front() == '\n' || token.front() == '\t'
                              || token.front() == ' ' || token.front() == '\v')) { token.remove_prefix(1); }
    while (!token.empty() && (token.back() == '\r' || token.back() == '\n' || token.back() == '\t'
                              || token.back() == ' ' || token.back() == '\v')) { token.remove_suffix(1); }
    if (format == TokenFormat::Hex) {
        if (token.size() % 2 != 0) { return {}; }
        std::vector<std::uint8_t> out(token.size() / 2);
        for (std::size_t i { 0 }; i < out.size(); ++i) {
            auto digit = [](char c) -> int {
                if (c >= '0' && c <= '9') { return c - '0'; }
                if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
                if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
                return -1;
            };
            const int hi { digit(token[i * 2]) }, lo { digit(token[i * 2 + 1]) };
            if (hi < 0 || lo < 0) { return {}; }
            out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
        }
        return out;
    }
    std::vector<std::uint8_t> out;
    int value { 0 }, bits { 0 };
    for (char c : token) {
        if (c == '=') { break; }
        const auto alphabet { std::string_view("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") };
        auto pos { alphabet.find(c) };
        if (pos == std::string_view::npos) {
            if (format == TokenFormat::Base64Url && (c == '-' || c == '_')) { pos = c == '-' ? 62 : 63; }
            else { return {}; }
        }
        value = (value << 6) | static_cast<int>(pos);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back(static_cast<std::uint8_t>((value >> bits) & 0xff)); }
    }
    return out;
}

bool constant_time_equal_(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
    if (a.size() != b.size()) { return false; }
    std::uint8_t difference { 0 };
    for (std::size_t i { 0 }; i < a.size(); ++i) { difference |= a[i] ^ b[i]; }
    return difference == 0;
}

}

std::expected<std::string, Error> generate(std::string_view secret, GenerateOptions options = {}) {
    if (secret.empty()) { return std::unexpected(Error::EmptySecret); }
    std::array<std::uint8_t, 32> payload {};
    const auto timestamp { options.nowMs == 0 ? detail::now_ms_() : options.nowMs };
    detail::write_u64_(payload.data(), timestamp);
    std::random_device random;
    for (std::size_t i { 8 }; i < 24; i += 4) {
        const auto value { random() };
        std::memcpy(payload.data() + i, &value, 4);
    }
    detail::write_u64_(payload.data() + 24, options.expiresInMs);
    std::vector<std::uint8_t> signedMessage(payload.begin(), payload.end());
    signedMessage.insert(signedMessage.end(), options.sessionId.begin(), options.sessionId.end());
    auto digest { detail::hmac_(secret, signedMessage, options.algorithm) };
    if (digest.empty()) { return std::unexpected(Error::UnsupportedAlgorithm); }
    std::vector<std::uint8_t> token(payload.begin(), payload.end());
    token.insert(token.end(), digest.begin(), digest.end());
    switch (options.encoding) {
    case TokenFormat::Hex: return mbun::crypto::to_hex(token);
    case TokenFormat::Base64: return mbun::crypto::to_base64(token);
    case TokenFormat::Base64Url: return mbun::crypto::to_base64url(token);
    }
    return std::unexpected(Error::UnsupportedAlgorithm);
}

std::expected<bool, Error> verify(std::string_view token, std::string_view secret,
                                  VerifyOptions options = {}) {
    if (token.empty()) { return std::unexpected(Error::EmptyToken); }
    if (secret.empty()) { return std::unexpected(Error::EmptySecret); }
    auto decoded { detail::decode_(token, options.encoding) };
    if (decoded.size() < 64) { return false; }
    const auto timestamp { detail::read_u64_(decoded.data()) };
    const auto expiresIn { detail::read_u64_(decoded.data() + 24) };
    const auto now { options.nowMs == 0 ? detail::now_ms_() : options.nowMs };
    if ((expiresIn > 0 && (timestamp > std::numeric_limits<std::uint64_t>::max() - expiresIn
                           || now > timestamp + expiresIn))
        || (options.maxAgeMs > 0 && (timestamp > std::numeric_limits<std::uint64_t>::max() - options.maxAgeMs
                                     || now > timestamp + options.maxAgeMs))) { return false; }
    std::vector<std::uint8_t> message(decoded.begin(), decoded.begin() + 32);
    message.insert(message.end(), options.sessionId.begin(), options.sessionId.end());
    auto expected { detail::hmac_(secret, message, options.algorithm) };
    return detail::constant_time_equal_(std::span<const std::uint8_t>(decoded).subspan(32), expected);
}

}
