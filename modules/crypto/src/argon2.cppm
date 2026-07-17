// argon2.cppm — mbun.crypto.argon2: Argon2 d/i/id password hashing (pure C++).
//
// RFC 9106 / PHC reference implementation, built on the in-tree BLAKE2b.
// Produces and verifies the PHC string `$argon2id$v=19$m=..,t=..,p=..$salt$hash`
// with standard (unpadded) base64. Validated against the RFC 9106 §5 test
// vectors for all three variants.
// ref: RFC 9106; phc-winner-argon2 ref/{core.c,blake2/blamka-round-ref.h};
// Bun src/runtime/crypto/pwhash.rs (defaults m=65536/t=2, verify ceilings).
export module mbun.crypto.argon2;

import std;
import mbun.crypto.blake2;

namespace mbun::crypto::argon2::detail {

using Block = std::array<std::uint64_t, 128>;

inline void store_le32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

inline std::uint64_t load_le64(const std::uint8_t* p) {
    std::uint64_t v { 0 };
    for (int i { 0 }; i < 8; ++i) { v |= static_cast<std::uint64_t>(p[i]) << (8 * i); }
    return v;
}

inline void store_le64(std::uint8_t* p, std::uint64_t v) {
    for (int i { 0 }; i < 8; ++i) { p[i] = static_cast<std::uint8_t>(v >> (8 * i)); }
}

inline void load_block(Block& b, const std::uint8_t* bytes) {
    for (std::size_t i { 0 }; i < 128; ++i) { b[i] = load_le64(bytes + 8 * i); }
}

inline void store_block(std::uint8_t* bytes, const Block& b) {
    for (std::size_t i { 0 }; i < 128; ++i) { store_le64(bytes + 8 * i, b[i]); }
}

// ── BLAKE2b variable-length hash H' (argon2's blake2b_long) ─────────────────
inline void blake2b_long(std::uint8_t* out, std::uint32_t outlen,
                         std::span<const std::uint8_t> input) {
    std::array<std::uint8_t, 4> lenPrefix {};
    store_le32(lenPrefix.data(), outlen);
    if (outlen <= 64) {
        mbun::crypto::Blake2b h { outlen };
        h.update(lenPrefix);
        h.update(input);
        h.final_(out);
        return;
    }
    std::array<std::uint8_t, 64> buf {};
    {
        mbun::crypto::Blake2b h { 64 };
        h.update(lenPrefix);
        h.update(input);
        h.final_(buf.data());
    }
    std::memcpy(out, buf.data(), 32);
    std::uint32_t pos { 32 };
    std::uint32_t toProduce { outlen - 32 };
    while (toProduce > 64) {
        mbun::crypto::Blake2b h { 64 };
        h.update(buf);
        h.final_(buf.data());
        std::memcpy(out + pos, buf.data(), 32);
        pos += 32;
        toProduce -= 32;
    }
    mbun::crypto::Blake2b h { toProduce };
    h.update(buf);
    h.final_(out + pos);
}

// ── Compression function G (BlaMka permutation) ─────────────────────────────
inline std::uint64_t rotr64(std::uint64_t x, unsigned n) {
    return (x >> n) | (x << (64 - n));
}

inline std::uint64_t fbla_mka(std::uint64_t x, std::uint64_t y) {
    const std::uint64_t lx { x & 0xffffffffULL };
    const std::uint64_t ly { y & 0xffffffffULL };
    return x + y + 2 * lx * ly;
}

inline void gb(std::uint64_t& a, std::uint64_t& b, std::uint64_t& c, std::uint64_t& d) {
    a = fbla_mka(a, b);
    d = rotr64(d ^ a, 32);
    c = fbla_mka(c, d);
    b = rotr64(b ^ c, 24);
    a = fbla_mka(a, b);
    d = rotr64(d ^ a, 16);
    c = fbla_mka(c, d);
    b = rotr64(b ^ c, 63);
}

// One BLAKE2 round over 16 gathered words.
inline void round_p(std::uint64_t* v) {
    gb(v[0], v[4], v[8], v[12]);
    gb(v[1], v[5], v[9], v[13]);
    gb(v[2], v[6], v[10], v[14]);
    gb(v[3], v[7], v[11], v[15]);
    gb(v[0], v[5], v[10], v[15]);
    gb(v[1], v[6], v[11], v[12]);
    gb(v[2], v[7], v[8], v[13]);
    gb(v[3], v[4], v[9], v[14]);
}

inline void fill_block(const Block& prev, const Block& ref, Block& next, bool with_xor) {
    Block r;
    for (std::size_t i { 0 }; i < 128; ++i) { r[i] = prev[i] ^ ref[i]; }
    Block tmp { r };
    if (with_xor) {
        for (std::size_t i { 0 }; i < 128; ++i) { tmp[i] ^= next[i]; }
    }
    for (std::size_t i { 0 }; i < 8; ++i) { round_p(r.data() + 16 * i); }
    for (std::size_t i { 0 }; i < 8; ++i) {
        std::array<std::uint64_t, 16> col {};
        for (std::size_t k { 0 }; k < 8; ++k) {
            col[2 * k] = r[2 * i + 16 * k];
            col[2 * k + 1] = r[2 * i + 16 * k + 1];
        }
        round_p(col.data());
        for (std::size_t k { 0 }; k < 8; ++k) {
            r[2 * i + 16 * k] = col[2 * k];
            r[2 * i + 16 * k + 1] = col[2 * k + 1];
        }
    }
    for (std::size_t i { 0 }; i < 128; ++i) { next[i] = tmp[i] ^ r[i]; }
}

// ── Memory-fill context ─────────────────────────────────────────────────────
struct Context {
    std::vector<Block> memory;
    std::uint32_t m_prime {};   // total blocks (rounded)
    std::uint32_t lanes {};     // p
    std::uint32_t lane_len {};  // q
    std::uint32_t seg_len {};   // q / 4
    std::uint32_t passes {};    // t
    std::uint32_t type {};      // 0=d, 1=i, 2=id
};

inline void next_addresses(Block& addr, Block& input, const Block& zero) {
    input[6] += 1;
    fill_block(zero, input, addr, false);
    fill_block(zero, addr, addr, false);
}

inline std::uint32_t index_alpha(const Context& c, std::uint32_t pass, std::uint32_t slice,
                                 std::uint32_t i, std::uint32_t j1, bool same_lane) {
    std::uint64_t ref_area {};
    if (pass == 0) {
        if (slice == 0) {
            ref_area = i - 1;
        } else if (same_lane) {
            ref_area = static_cast<std::uint64_t>(slice) * c.seg_len + i - 1;
        } else {
            ref_area = static_cast<std::uint64_t>(slice) * c.seg_len + (i == 0 ? -1 : 0);
        }
    } else if (same_lane) {
        ref_area = static_cast<std::uint64_t>(c.lane_len) - c.seg_len + i - 1;
    } else {
        ref_area = static_cast<std::uint64_t>(c.lane_len) - c.seg_len + (i == 0 ? -1 : 0);
    }

    std::uint64_t rel { j1 };
    rel = (rel * rel) >> 32;
    rel = ref_area - 1 - ((ref_area * rel) >> 32);

    std::uint64_t start { 0 };
    if (pass != 0) { start = (slice == 3) ? 0 : static_cast<std::uint64_t>(slice + 1) * c.seg_len; }
    return static_cast<std::uint32_t>((start + rel) % c.lane_len);
}

inline void fill_segment(Context& c, std::uint32_t pass, std::uint32_t lane, std::uint32_t slice) {
    const bool data_independent { c.type == 1 || (c.type == 2 && pass == 0 && slice < 2) };

    Block input {};
    Block addr {};
    Block zero {};
    if (data_independent) {
        input[0] = pass;
        input[1] = lane;
        input[2] = slice;
        input[3] = c.m_prime;
        input[4] = c.passes;
        input[5] = c.type;
    }

    std::uint32_t start_i { 0 };
    if (pass == 0 && slice == 0) {
        start_i = 2;
        if (data_independent) { next_addresses(addr, input, zero); }
    }

    for (std::uint32_t i { start_i }; i < c.seg_len; ++i) {
        const std::uint32_t cur_col { slice * c.seg_len + i };
        const std::uint32_t cur_off { lane * c.lane_len + cur_col };
        const std::uint32_t prev_col { cur_col == 0 ? c.lane_len - 1 : cur_col - 1 };
        const std::uint32_t prev_off { lane * c.lane_len + prev_col };

        std::uint64_t pseudo {};
        if (data_independent) {
            if (i % 128 == 0) { next_addresses(addr, input, zero); }
            pseudo = addr[i % 128];
        } else {
            pseudo = c.memory[prev_off][0];
        }

        std::uint32_t ref_lane { lane };
        if (!(pass == 0 && slice == 0)) {
            ref_lane = static_cast<std::uint32_t>((pseudo >> 32) % c.lanes);
        }
        const std::uint32_t j1 { static_cast<std::uint32_t>(pseudo & 0xffffffffULL) };
        const std::uint32_t ref_index { index_alpha(c, pass, slice, i, j1, ref_lane == lane) };

        const Block& refb { c.memory[ref_lane * c.lane_len + ref_index] };
        fill_block(c.memory[prev_off], refb, c.memory[cur_off], pass != 0);
    }
}

// Core: derive `tag_len` bytes. secret/ad are empty for Bun but supported so
// the RFC 9106 known-answer vectors can be exercised.
inline std::vector<std::uint8_t> raw_hash(std::uint32_t type, std::uint32_t m, std::uint32_t t,
                                          std::uint32_t p, std::uint32_t tag_len,
                                          std::span<const std::uint8_t> pw,
                                          std::span<const std::uint8_t> salt,
                                          std::span<const std::uint8_t> secret,
                                          std::span<const std::uint8_t> ad) {
    Context c;
    c.lanes = p;
    c.passes = t;
    c.type = type;
    c.m_prime = 4 * p * (m / (4 * p));
    c.lane_len = c.m_prime / p;
    c.seg_len = c.lane_len / 4;
    c.memory.assign(c.m_prime, Block {});

    // H0 = BLAKE2b-512 of the parameter/input preamble.
    std::array<std::uint8_t, 64> h0 {};
    {
        mbun::crypto::Blake2b h { 64 };
        std::array<std::uint8_t, 4> le {};
        auto put32 = [&](std::uint32_t v) {
            store_le32(le.data(), v);
            h.update(le);
        };
        auto put_slice = [&](std::span<const std::uint8_t> s) {
            put32(static_cast<std::uint32_t>(s.size()));
            if (!s.empty()) { h.update(s); }
        };
        put32(p);
        put32(tag_len);
        put32(m);
        put32(t);
        put32(0x13);  // version 19
        put32(type);
        put_slice(pw);
        put_slice(salt);
        put_slice(secret);
        put_slice(ad);
        h.final_(h0.data());
    }

    // Initial two blocks per lane.
    std::array<std::uint8_t, 1024> blockBytes {};
    for (std::uint32_t lane { 0 }; lane < p; ++lane) {
        std::array<std::uint8_t, 72> in {};
        std::memcpy(in.data(), h0.data(), 64);
        store_le32(in.data() + 68, lane);
        for (std::uint32_t col { 0 }; col < 2; ++col) {
            store_le32(in.data() + 64, col);
            blake2b_long(blockBytes.data(), 1024, in);
            load_block(c.memory[lane * c.lane_len + col], blockBytes.data());
        }
    }

    for (std::uint32_t pass { 0 }; pass < t; ++pass) {
        for (std::uint32_t slice { 0 }; slice < 4; ++slice) {
            for (std::uint32_t lane { 0 }; lane < p; ++lane) { fill_segment(c, pass, lane, slice); }
        }
    }

    Block final_block { c.memory[c.lane_len - 1] };
    for (std::uint32_t lane { 1 }; lane < p; ++lane) {
        const Block& last { c.memory[lane * c.lane_len + c.lane_len - 1] };
        for (std::size_t i { 0 }; i < 128; ++i) { final_block[i] ^= last[i]; }
    }
    store_block(blockBytes.data(), final_block);

    std::vector<std::uint8_t> tag(tag_len);
    blake2b_long(tag.data(), tag_len, blockBytes);
    return tag;
}

// ── standard (unpadded) base64 ──────────────────────────────────────────────
inline constexpr std::string_view kB64 {
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
};

inline std::string b64_encode(std::span<const std::uint8_t> data) {
    std::string out;
    std::size_t i { 0 };
    while (i + 3 <= data.size()) {
        const std::uint32_t n { (std::uint32_t { data[i] } << 16) | (std::uint32_t { data[i + 1] } << 8)
                                | data[i + 2] };
        out.push_back(kB64[(n >> 18) & 0x3f]);
        out.push_back(kB64[(n >> 12) & 0x3f]);
        out.push_back(kB64[(n >> 6) & 0x3f]);
        out.push_back(kB64[n & 0x3f]);
        i += 3;
    }
    const std::size_t rem { data.size() - i };
    if (rem == 1) {
        const std::uint32_t n { std::uint32_t { data[i] } << 16 };
        out.push_back(kB64[(n >> 18) & 0x3f]);
        out.push_back(kB64[(n >> 12) & 0x3f]);
    } else if (rem == 2) {
        const std::uint32_t n { (std::uint32_t { data[i] } << 16) | (std::uint32_t { data[i + 1] } << 8) };
        out.push_back(kB64[(n >> 18) & 0x3f]);
        out.push_back(kB64[(n >> 12) & 0x3f]);
        out.push_back(kB64[(n >> 6) & 0x3f]);
    }
    return out;
}

inline std::optional<std::vector<std::uint8_t>> b64_decode(std::string_view s) {
    auto val = [](char ch) -> int {
        const auto pos { kB64.find(ch) };
        return pos == std::string_view::npos ? -1 : static_cast<int>(pos);
    };
    std::vector<std::uint8_t> out;
    std::uint32_t buf { 0 };
    int bits { 0 };
    for (char ch : s) {
        const int v { val(ch) };
        if (v < 0) { return std::nullopt; }
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buf >> bits) & 0xff));
        }
    }
    return out;
}

}  // namespace mbun::crypto::argon2::detail

export namespace mbun::crypto::argon2 {

enum class Type : std::uint32_t { D = 0, I = 1, ID = 2 };

inline std::string_view type_name(Type t) {
    switch (t) {
    case Type::D: return "argon2d";
    case Type::I: return "argon2i";
    case Type::ID: return "argon2id";
    }
    return "argon2id";
}

// Bun's verify ceilings (pwhash.rs). Above these, verify rejects up front.
inline constexpr std::uint32_t MAX_VERIFY_TIME_COST { 1u << 16 };
inline constexpr std::uint32_t MAX_VERIFY_MEMORY_COST { 1u << 22 };
inline constexpr std::uint32_t MAX_VERIFY_PARALLELISM { 64 };

// Derive `tag_len` bytes (secret/ad empty). Exposed for known-answer testing.
inline std::vector<std::uint8_t> raw(Type type, std::uint32_t m, std::uint32_t t, std::uint32_t p,
                                     std::uint32_t tag_len, std::span<const std::uint8_t> pw,
                                     std::span<const std::uint8_t> salt,
                                     std::span<const std::uint8_t> secret = {},
                                     std::span<const std::uint8_t> ad = {}) {
    return detail::raw_hash(static_cast<std::uint32_t>(type), m, t, p, tag_len, pw, salt, secret, ad);
}

// Produce a PHC-encoded hash. Bun uses tag_len=32, salt_len=32, p=1.
inline std::string hash_encoded(Type type, std::uint32_t m, std::uint32_t t, std::uint32_t p,
                                std::span<const std::uint8_t> pw,
                                std::span<const std::uint8_t> salt) {
    const std::vector<std::uint8_t> tag { detail::raw_hash(static_cast<std::uint32_t>(type), m, t, p,
                                                           32, pw, salt, {}, {}) };
    std::string out { "$" };
    out += type_name(type);
    out += "$v=19$m=";
    out += std::to_string(m);
    out += ",t=";
    out += std::to_string(t);
    out += ",p=";
    out += std::to_string(p);
    out += "$";
    out += detail::b64_encode(salt);
    out += "$";
    out += detail::b64_encode(tag);
    return out;
}

// Hash with a fresh cryptographically-random 32-byte salt.
inline std::string hash_random(Type type, std::uint32_t m, std::uint32_t t, std::uint32_t p,
                               std::span<const std::uint8_t> pw) {
    static thread_local std::random_device rd;
    std::array<std::uint8_t, 32> salt {};
    for (std::size_t i { 0 }; i < 32; i += 4) {
        const std::uint32_t r { rd() };
        salt[i + 0] = static_cast<std::uint8_t>(r);
        salt[i + 1] = static_cast<std::uint8_t>(r >> 8);
        salt[i + 2] = static_cast<std::uint8_t>(r >> 16);
        salt[i + 3] = static_cast<std::uint8_t>(r >> 24);
    }
    return hash_encoded(type, m, t, p, pw, salt);
}

enum class VerifyResult { Match, Mismatch, WeakParameters, InvalidEncoding };

// Verify a PHC-encoded argon2 hash against `pw`.
inline VerifyResult verify(std::span<const std::uint8_t> pw, std::string_view encoded) {
    // $argon2id$v=19$m=..,t=..,p=..$<salt>$<hash>
    std::vector<std::string_view> parts;
    std::size_t pos { 0 };
    while (pos <= encoded.size()) {
        const std::size_t next { encoded.find('$', pos) };
        if (next == std::string_view::npos) {
            parts.push_back(encoded.substr(pos));
            break;
        }
        parts.push_back(encoded.substr(pos, next - pos));
        pos = next + 1;
    }
    // parts: ["", "argon2id", "v=19", "m=..,t=..,p=..", salt, hash]
    if (parts.size() != 6 || !parts[0].empty()) { return VerifyResult::InvalidEncoding; }
    Type type;
    if (parts[1] == "argon2d") {
        type = Type::D;
    } else if (parts[1] == "argon2i") {
        type = Type::I;
    } else if (parts[1] == "argon2id") {
        type = Type::ID;
    } else {
        return VerifyResult::InvalidEncoding;
    }
    if (parts[2] != "v=19") { return VerifyResult::InvalidEncoding; }

    std::uint32_t m { 0 };
    std::uint32_t t { 0 };
    std::uint32_t p { 0 };
    for (std::size_t start { 0 }; start <= parts[3].size();) {
        const std::size_t comma { parts[3].find(',', start) };
        const std::string_view pair {
            parts[3].substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start)
        };
        const std::size_t eq { pair.find('=') };
        if (eq == std::string_view::npos) { return VerifyResult::InvalidEncoding; }
        const std::string_view key { pair.substr(0, eq) };
        std::uint32_t value { 0 };
        const auto* first { pair.data() + eq + 1 };
        const auto* last { pair.data() + pair.size() };
        const auto res { std::from_chars(first, last, value) };
        if (res.ec != std::errc {}) { return VerifyResult::InvalidEncoding; }
        std::uint32_t limit { 0 };
        if (key == "m") {
            m = value;
            limit = MAX_VERIFY_MEMORY_COST;
        } else if (key == "t") {
            t = value;
            limit = MAX_VERIFY_TIME_COST;
        } else if (key == "p") {
            p = value;
            limit = MAX_VERIFY_PARALLELISM;
        }
        if (limit != 0 && value > limit) { return VerifyResult::WeakParameters; }
        if (comma == std::string_view::npos) { break; }
        start = comma + 1;
    }
    if (m == 0 || t == 0 || p == 0) { return VerifyResult::InvalidEncoding; }

    const auto salt { detail::b64_decode(parts[4]) };
    const auto want { detail::b64_decode(parts[5]) };
    if (!salt || !want || want->empty() || salt->empty()) { return VerifyResult::InvalidEncoding; }

    const std::vector<std::uint8_t> got { detail::raw_hash(static_cast<std::uint32_t>(type), m, t, p,
                                                           static_cast<std::uint32_t>(want->size()),
                                                           pw, *salt, {}, {}) };
    if (got.size() != want->size()) { return VerifyResult::Mismatch; }
    unsigned diff { 0 };
    for (std::size_t i { 0 }; i < got.size(); ++i) {
        diff |= static_cast<unsigned>(got[i] ^ (*want)[i]);
    }
    return diff == 0 ? VerifyResult::Match : VerifyResult::Mismatch;
}

}  // namespace mbun::crypto::argon2
