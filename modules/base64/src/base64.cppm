export module mbun.base64;

import std;

namespace mbun::base64 {

// ref: bun-ref/src/base64/lib.rs DecodeResult/SIMDUTFResult status contract.
export enum class DecodeStatus : std::uint8_t { success, invalid_base64_character };
export struct DecodeResult {
    std::size_t count{};
    DecodeStatus status{DecodeStatus::success};
    bool is_successful() const { return status == DecodeStatus::success; }
};

export constexpr std::size_t encode_len_from_size(std::size_t n) { return (n + 2) / 3 * 4; }
export constexpr std::size_t url_safe_encode_len_from_size(std::size_t n) {
    return n / 3 * 4 + ((n % 3) * 4 + 2) / 3;
}
export constexpr std::size_t decode_len_upper_bound(std::size_t n) { return (n + 3) / 4 * 3; }
export constexpr std::size_t hex_encode_len_from_size(std::size_t n) { return n * 2; }

// Scalar baseline for bun-ref/src/base64/lib.rs and bun-zig-src/src/base64/base64.zig.
// SIMD/simdutf dispatch is intentionally deferred to the optimization node.
namespace {
constexpr std::string_view STANDARD{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
constexpr std::string_view URLSAFE{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"};

int b64_digit(unsigned char c, bool url) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || (url && c == '-')) return 62;
    if (c == '/' || (url && c == '_')) return 63;
    return -1;
}
bool ignored(unsigned char c) {
    return c == 0xff || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}
template <bool Url>
std::size_t encode_impl(std::span<char> out, std::span<const std::byte> in) {
    const std::string_view alphabet{Url ? URLSAFE : STANDARD};
    std::size_t w{};
    for (std::size_t i{}; i + 2 < in.size(); i += 3) {
        const auto n{(std::to_integer<unsigned>(in[i]) << 16) |
                     (std::to_integer<unsigned>(in[i + 1]) << 8) | std::to_integer<unsigned>(in[i + 2])};
        out[w++] = alphabet[(n >> 18) & 63]; out[w++] = alphabet[(n >> 12) & 63];
        out[w++] = alphabet[(n >> 6) & 63]; out[w++] = alphabet[n & 63];
    }
    const auto rem{in.size() % 3};
    if (rem) {
        const auto i{in.size() - rem};
        const auto n{std::to_integer<unsigned>(in[i]) << 16 |
                     (rem == 2 ? std::to_integer<unsigned>(in[i + 1]) << 8 : 0)};
        out[w++] = alphabet[(n >> 18) & 63]; out[w++] = alphabet[(n >> 12) & 63];
        if (rem == 2) out[w++] = alphabet[(n >> 6) & 63];
        if constexpr (!Url) { if (rem == 1) out[w++] = '='; out[w++] = '='; }
    }
    return w;
}
template <bool Lenient>
DecodeResult decode_impl(std::span<char> out, std::string_view in, bool url) {
    std::array<int, 4> q{}; std::size_t qlen{}, w{};
    for (unsigned char c : in) {
        if (c == '=') break;
        const int d{b64_digit(c, url)};
        if (d < 0) {
            if constexpr (Lenient) continue;
            if (ignored(c)) continue;
            return {w, DecodeStatus::invalid_base64_character};
        }
        q[qlen++] = d;
        if (qlen != 4) continue;
        if (w + 3 > out.size()) break;
        out[w++] = static_cast<char>((q[0] << 2) | (q[1] >> 4));
        out[w++] = static_cast<char>((q[1] << 4) | (q[2] >> 2));
        out[w++] = static_cast<char>((q[2] << 6) | q[3]); qlen = 0;
    }
    if (qlen >= 2 && w < out.size()) {
        out[w++] = static_cast<char>((q[0] << 2) | (q[1] >> 4));
        if (qlen == 3 && w < out.size()) out[w++] = static_cast<char>((q[1] << 4) | (q[2] >> 2));
    }
    return {w, DecodeStatus::success};
}
int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}

export std::size_t encode(std::span<char> out, std::span<const std::byte> in) { return encode_impl<false>(out, in); }
export std::string encode_alloc(std::span<const std::byte> in) {
    std::string out(encode_len_from_size(in.size()), '\0'); out.resize(encode(out, in)); return out;
}
export std::size_t encode_url_safe(std::span<char> out, std::span<const std::byte> in) { return encode_impl<true>(out, in); }
export std::string encode_url_safe_alloc(std::span<const std::byte> in) {
    std::string out(url_safe_encode_len_from_size(in.size()), '\0'); out.resize(encode_url_safe(out, in)); return out;
}
export DecodeResult decode(std::span<char> out, std::string_view in) { return decode_impl<false>(out, in, true); }
// Bun's lenient Buffer decoder accepts the standard and URL-safe alphabets in
// either encoding mode; `url` is retained for API parity with the Rust source.
export std::size_t decode_lenient(std::span<char> out, std::string_view in, bool /*url*/) { return decode_impl<true>(out, in, true).count; }
export std::optional<std::string> decode_alloc(std::string_view in) {
    std::string out(decode_len_upper_bound(in.size()), '\0'); const auto r{decode(out, in)};
    if (!r.is_successful()) return std::nullopt; out.resize(r.count); return out;
}

export std::size_t hex_encode(std::span<char> out, std::span<const std::byte> in) {
    constexpr std::string_view digits{"0123456789abcdef"};
    for (std::size_t i{}; i < in.size(); ++i) { const auto b{std::to_integer<unsigned>(in[i])}; out[2 * i] = digits[b >> 4]; out[2 * i + 1] = digits[b & 15]; }
    return in.size() * 2;
}
export std::string hex_encode_alloc(std::span<const std::byte> in) { std::string out(in.size() * 2, '\0'); hex_encode(out, in); return out; }
export std::size_t hex_decode(std::span<char> out, std::string_view in) {
    std::size_t w{};
    for (std::size_t i{}; i + 1 < in.size() && w < out.size(); i += 2) {
        const auto hi{static_cast<unsigned char>(in[i])}, lo{static_cast<unsigned char>(in[i + 1])};
        if (hi > 0x7f || lo > 0x7f) break; const int h{hex_digit(hi)}, l{hex_digit(lo)};
        if (h < 0 || l < 0) break; out[w++] = static_cast<char>((h << 4) | l);
    }
    return w;
}
export std::string hex_decode_alloc(std::string_view in) { std::string out(in.size() / 2, '\0'); out.resize(hex_decode(out, in)); return out; }
}
