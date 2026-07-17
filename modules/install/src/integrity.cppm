// integrity.cppm — Subresource Integrity (SRI) parse/format for the install
// subsystem. Mechanical port of bun's Rust `src/install/integrity.rs`.
//
// ref: .mbun/bun-ref/src/install/integrity.rs
//
// Ports the PURE parse/format logic 1:1:
//   - Tag (UNKNOWN/SHA1/SHA256/SHA384/SHA512) as a transparent u8 newtype,
//     Tag::parse (prefix "sha1-"/"sha256-"/... within first 7 bytes),
//     digest_len, is_supported.
//   - Integrity::parse_sha_sum (npm hex shasum → 20-byte SHA1 digest).
//   - Integrity::parse / parse_entry (SRI "sha512-<base64>" strongest-wins),
//     with STANDARD_NO_PAD base64 decode + `?`-suffix stripping + `=` trim.
//   - Display formatting: "sha512-<base64>==" (yarn.lock consistency: "=" for
//     sha1, "==" otherwise).
//
// Hash VERIFICATION (verify/verify_by_tag/for_bytes) is wired to
// mbun.crypto.hasher (Bun.CryptoHasher's pure-logic core): for_bytes computes
// sha512 (ref: integrity.rs:174-192); verify_by_tag hashes with the entry's
// own algorithm and compares against the stored digest, returning false for
// UNKNOWN/unsupported tags (ref: integrity.rs:194-241).
export module mbun.install.integrity;

import std;
import mbun.crypto.hasher;

export namespace mbun::install {

// Digest lengths (bytes). ref: integrity.rs:8-11
inline constexpr std::size_t SHA1_DIGEST_LEN{20};
inline constexpr std::size_t SHA256_DIGEST_LEN{32};
inline constexpr std::size_t SHA384_DIGEST_LEN{48};
inline constexpr std::size_t SHA512_DIGEST_LEN{64};

// ref: integrity.rs:36-48 — max over all supported digest lengths.
inline constexpr std::size_t DIGEST_BUF_LEN{SHA512_DIGEST_LEN};

// ref: integrity.rs:273-325 — transparent newtype over u8 (any byte is a valid
// bit pattern since this is read from on-disk lockfiles).
class Tag {
public:
    std::uint8_t value_{0};

public:
    Tag() = default;
    constexpr explicit Tag(std::uint8_t v) : value_{v} {}

    static constexpr Tag UNKNOWN() { return Tag{0}; }
    static constexpr Tag SHA1() { return Tag{1}; }
    static constexpr Tag SHA256() { return Tag{2}; }
    static constexpr Tag SHA384() { return Tag{3}; }
    static constexpr Tag SHA512() { return Tag{4}; }

    friend constexpr bool operator==(Tag a, Tag b) { return a.value_ == b.value_; }

    // ref: integrity.rs:291-294
    [[nodiscard]] constexpr bool is_supported() const {
        return value_ >= 1 && value_ <= 4;
    }

    // ref: integrity.rs:296-313 — returns (tag, offset-past-dash); offset 0 on
    // failure (UNKNOWN).
    static std::pair<Tag, std::size_t> parse(std::string_view buf) {
        std::size_t lim{std::min<std::size_t>(buf.size(), 7)};
        std::size_t dash{std::string_view::npos};
        for (std::size_t i{0}; i < lim; ++i) {
            if (buf[i] == '-') {
                dash = i;
                break;
            }
        }
        if (dash == std::string_view::npos) {
            return {UNKNOWN(), 0};
        }
        if (buf.size() <= dash + 1) {
            return {UNKNOWN(), 0};
        }
        std::string_view name{buf.substr(0, dash)};
        if (name == "sha1") return {SHA1(), dash + 1};
        if (name == "sha256") return {SHA256(), dash + 1};
        if (name == "sha384") return {SHA384(), dash + 1};
        if (name == "sha512") return {SHA512(), dash + 1};
        return {UNKNOWN(), 0};
    }

    // ref: integrity.rs:315-324
    [[nodiscard]] constexpr std::size_t digest_len() const {
        switch (value_) {
        case 1: return SHA1_DIGEST_LEN;
        case 4: return SHA512_DIGEST_LEN;
        case 2: return SHA256_DIGEST_LEN;
        case 3: return SHA384_DIGEST_LEN;
        default: return 0;
        }
    }

    [[nodiscard]] std::string_view name() const {
        switch (value_) {
        case 1: return "sha1";
        case 2: return "sha256";
        case 3: return "sha384";
        case 4: return "sha512";
        default: return "";
        }
    }
};

// ── base64 STANDARD_NO_PAD helpers (ref: bun_base64::zig_base64) ────────────

namespace detail {

inline constexpr std::string_view B64_ALPHABET{
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};

// Reverse lookup: char → 0..63, or 0xFF if invalid.
inline std::array<std::uint8_t, 256> b64_decode_table() {
    std::array<std::uint8_t, 256> t{};
    t.fill(0xFF);
    for (std::size_t i{0}; i < B64_ALPHABET.size(); ++i) {
        t[static_cast<std::uint8_t>(B64_ALPHABET[i])] = static_cast<std::uint8_t>(i);
    }
    return t;
}

// ref: base64 calc_size_for_slice (no-pad). Returns nullopt for the invalid
// `len % 4 == 1` remainder.
inline std::optional<std::size_t> b64_calc_size(std::string_view input) {
    std::size_t n{input.size()};
    std::size_t rem{n % 4};
    if (rem == 1) {
        return std::nullopt;
    }
    std::size_t out{(n / 4) * 3};
    if (rem == 2) out += 1;
    else if (rem == 3) out += 2;
    return out;
}

// Decode `input` into `out` (capacity `out.size()`). Returns bytes written, or
// nullopt on invalid character / capacity overflow.
inline std::optional<std::size_t> b64_decode(std::span<std::uint8_t> out, std::string_view input) {
    static const std::array<std::uint8_t, 256> table{b64_decode_table()};
    auto sz{b64_calc_size(input)};
    if (!sz || *sz > out.size()) {
        return std::nullopt;
    }
    std::size_t oi{0};
    std::size_t i{0};
    std::uint32_t acc{0};
    int bits{0};
    for (; i < input.size(); ++i) {
        std::uint8_t v{table[static_cast<std::uint8_t>(input[i])]};
        if (v == 0xFF) {
            return std::nullopt;
        }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[oi++] = static_cast<std::uint8_t>((acc >> bits) & 0xFF);
        }
    }
    return oi;
}

// Encode bytes → base64 (no padding). ref: base64 encoder used by Display.
inline std::string b64_encode(std::span<const std::uint8_t> bytes) {
    std::string s;
    std::size_t i{0};
    while (i + 3 <= bytes.size()) {
        std::uint32_t n{static_cast<std::uint32_t>(bytes[i]) << 16 |
                        static_cast<std::uint32_t>(bytes[i + 1]) << 8 |
                        static_cast<std::uint32_t>(bytes[i + 2])};
        s.push_back(B64_ALPHABET[(n >> 18) & 63]);
        s.push_back(B64_ALPHABET[(n >> 12) & 63]);
        s.push_back(B64_ALPHABET[(n >> 6) & 63]);
        s.push_back(B64_ALPHABET[n & 63]);
        i += 3;
    }
    std::size_t rem{bytes.size() - i};
    if (rem == 1) {
        std::uint32_t n{static_cast<std::uint32_t>(bytes[i]) << 16};
        s.push_back(B64_ALPHABET[(n >> 18) & 63]);
        s.push_back(B64_ALPHABET[(n >> 12) & 63]);
    } else if (rem == 2) {
        std::uint32_t n{static_cast<std::uint32_t>(bytes[i]) << 16 |
                        static_cast<std::uint32_t>(bytes[i + 1]) << 8};
        s.push_back(B64_ALPHABET[(n >> 18) & 63]);
        s.push_back(B64_ALPHABET[(n >> 12) & 63]);
        s.push_back(B64_ALPHABET[(n >> 6) & 63]);
    }
    return s;
}

// ref: bun_core::fmt::hex_pair_value — narrows to [0-9a-f] (npm sha1 strings).
inline std::optional<std::uint8_t> hex_nibble(char c) {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
    return std::nullopt;
}
inline std::optional<std::uint8_t> hex_pair_value(char hi, char lo) {
    auto h{hex_nibble(hi)};
    auto l{hex_nibble(lo)};
    if (!h || !l) return std::nullopt;
    return static_cast<std::uint8_t>((*h << 4) | *l);
}

} // namespace detail

// ref: integrity.rs:13-34 — value is the decoded digest; `tag` selects length.
struct Integrity {
    Tag tag{Tag::UNKNOWN()};
    std::array<std::uint8_t, DIGEST_BUF_LEN> value{};

    // ref: integrity.rs:170-172
    [[nodiscard]] std::span<const std::uint8_t> slice() const {
        return {value.data(), tag.digest_len()};
    }

    // ref: integrity.rs:51-90 — npm hex shasum (e.g.
    // "3cd0599b099384b815c10f7fa7df0092b62d534f") → 20-byte SHA1 digest.
    // Returns nullopt on invalid (odd length / non-hex char).
    static std::optional<Integrity> parse_sha_sum(std::string_view buf) {
        if (buf.empty()) {
            return Integrity{};  // UNKNOWN
        }
        Integrity out{};
        out.tag = Tag::SHA1();
        std::size_t end{std::min<std::size_t>(40, buf.size())};
        if (end % 2 != 0) {
            return std::nullopt;  // InvalidCharacter
        }
        std::size_t oi{0};
        for (std::size_t i{0}; i < end; i += 2) {
            auto v{detail::hex_pair_value(buf[i], buf[i + 1])};
            if (!v) {
                return std::nullopt;  // InvalidCharacter
            }
            out.value[oi++] = *v;
        }
        return out;
    }

    // ref: integrity.rs:103-168 — parse a single whitespace-delimited SRI entry.
    static Integrity parse_entry(std::string_view buf) {
        Integrity unknown{};
        if (buf.size() < 7) {  // "sha256-".len()
            return unknown;
        }
        auto [tag, offset] = Tag::parse(buf);
        if (tag == Tag::UNKNOWN()) {
            return unknown;
        }
        std::size_t expected_len{tag.digest_len()};
        if (expected_len == 0) {
            return unknown;
        }
        std::string_view s{buf.substr(offset)};
        // cut at '?'
        if (auto q{s.find('?')}; q != std::string_view::npos) {
            s = s.substr(0, q);
        }
        // trim trailing '=' padding
        while (!s.empty() && s.back() == '=') {
            s.remove_suffix(1);
        }
        auto decoded_size{detail::b64_calc_size(s)};
        if (!decoded_size) {
            return unknown;
        }
        if (*decoded_size > expected_len) {
            return unknown;
        }
        Integrity out{};
        auto written{detail::b64_decode(
            std::span<std::uint8_t>{out.value.data(), expected_len}, s)};
        if (!written) {
            return unknown;
        }
        out.tag = tag;
        return out;
    }

    // ref: integrity.rs:92-101 — split on ascii whitespace, strongest tag wins.
    static Integrity parse(std::string_view buf) {
        Integrity strongest{};
        std::size_t i{0};
        while (i < buf.size()) {
            while (i < buf.size() && std::isspace(static_cast<unsigned char>(buf[i]))) {
                ++i;
            }
            std::size_t start{i};
            while (i < buf.size() && !std::isspace(static_cast<unsigned char>(buf[i]))) {
                ++i;
            }
            if (i > start) {
                Integrity parsed{parse_entry(buf.substr(start, i - start))};
                if (parsed.tag.value_ > strongest.tag.value_) {
                    strongest = parsed;
                }
            }
        }
        return strongest;
    }

    // ref: integrity.rs:244-267 — "<algo>-<base64>" with yarn.lock-consistent
    // trailing '=' / '==' padding. Empty string for UNKNOWN tag.
    [[nodiscard]] std::string to_string() const {
        if (!tag.is_supported()) {
            return "";
        }
        std::string s{tag.name()};
        s.push_back('-');
        s += detail::b64_encode(slice());
        s += (tag == Tag::SHA1()) ? "=" : "==";
        return s;
    }

    // ── hashing (wired to mbun.crypto.hasher) ───────────────────────────────

    // ref: integrity.rs:174-192 — compute a sha512 integrity from raw bytes
    // (e.g. a downloaded tarball); this is what bun records when the registry
    // manifest carried no SRI.
    static Integrity for_bytes(std::span<const std::uint8_t> bytes) {
        Integrity out{};
        out.tag = Tag::SHA512();
        auto digest{mbun::crypto::CryptoHasher::hash("sha512", bytes)};
        if (digest) {
            std::copy(digest->begin(), digest->end(), out.value.begin());
        }
        return out;
    }

    // ref: integrity.rs:199-241 — hash `bytes` with `tag`'s own algorithm and
    // compare against `sum[0..digest_len]`; UNKNOWN/unsupported tags → false.
    static bool verify_by_tag(Tag tag, std::span<const std::uint8_t> bytes,
                              std::span<const std::uint8_t> sum) {
        std::size_t len{tag.digest_len()};
        if (len == 0 || sum.size() < len) {
            return false;
        }
        auto digest{mbun::crypto::CryptoHasher::hash(tag.name(), bytes)};
        if (!digest || digest->size() != len) {
            return false;
        }
        return std::equal(digest->begin(), digest->end(), sum.begin());
    }

    // ref: integrity.rs:194-197
    [[nodiscard]] bool verify(std::span<const std::uint8_t> bytes) const {
        return verify_by_tag(tag, bytes, value);
    }
};

} // namespace mbun::install
