// mbun.crypto.blake2 — RFC 7693 BLAKE2b / BLAKE2s (unkeyed).
//
// bun exposes these via BoringSSL's blake2b256 and node's blake2b512/blake2s256
// names; the BoringSSL dependency is unavailable here, so this is a
// self-contained RFC 7693 implementation with bit-identical output to the RFC's
// Appendix A / official known-answer tests. Little-endian word I/O per the RFC.

export module mbun.crypto.blake2;

import std;

export namespace mbun::crypto {

// ── BLAKE2b (64-bit, 128-byte block, ≤64-byte digest) ───────────────────────
class Blake2b {
public:
    // outLen: 1..64. Unkeyed.
    explicit Blake2b(std::size_t outLen) : outLen_ { outLen } {
        for (int i { 0 }; i < 8; ++i) { h_[i] = IV_[i]; }
        h_[0] ^= 0x01010000ULL ^ static_cast<std::uint64_t>(outLen);
    }

    void update(std::span<const std::uint8_t> data) {
        std::size_t inlen { data.size() };
        const std::uint8_t* in { data.data() };
        if (inlen == 0) { return; }
        std::size_t left { bufLen_ };
        std::size_t fill { 128 - left };
        if (inlen > fill) {
            bufLen_ = 0;
            std::memcpy(buf_ + left, in, fill);
            inc_counter_(128);
            compress_(buf_);
            in += fill;
            inlen -= fill;
            while (inlen > 128) {
                inc_counter_(128);
                compress_(in);
                in += 128;
                inlen -= 128;
            }
        }
        std::memcpy(buf_ + bufLen_, in, inlen);
        bufLen_ += inlen;
    }

    // Writes outLen_ bytes. Destructive (finalizes state).
    void final_(std::uint8_t* out) {
        inc_counter_(bufLen_);
        f_[0] = ~0ULL;  // last-block flag
        std::memset(buf_ + bufLen_, 0, 128 - bufLen_);
        compress_(buf_);
        for (std::size_t i { 0 }; i < outLen_; ++i) {
            out[i] = static_cast<std::uint8_t>(h_[i >> 3] >> (8 * (i & 7)));
        }
    }

private:
    static constexpr std::uint64_t IV_[8] {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
        0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
        0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL,
    };
    std::uint64_t h_[8] {};
    std::uint64_t t_[2] { 0, 0 };
    std::uint64_t f_[2] { 0, 0 };
    std::uint8_t buf_[128] {};
    std::size_t bufLen_ { 0 };
    std::size_t outLen_;

    void inc_counter_(std::uint64_t inc) {
        t_[0] += inc;
        if (t_[0] < inc) { ++t_[1]; }
    }

    static std::uint64_t load64_(const std::uint8_t* p) {
        std::uint64_t v { 0 };
        for (int i { 0 }; i < 8; ++i) { v |= static_cast<std::uint64_t>(p[i]) << (8 * i); }
        return v;
    }

    void compress_(const std::uint8_t* block) {
        static constexpr std::uint8_t SIGMA[12][16] {
            { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
            { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
            { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
            { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
            { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
            { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
            { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
            { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
            { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
            { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
            { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
            { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
        };
        std::uint64_t m[16];
        for (int i { 0 }; i < 16; ++i) { m[i] = load64_(block + i * 8); }
        std::uint64_t v[16];
        for (int i { 0 }; i < 8; ++i) {
            v[i] = h_[i];
            v[i + 8] = IV_[i];
        }
        v[12] ^= t_[0];
        v[13] ^= t_[1];
        v[14] ^= f_[0];
        v[15] ^= f_[1];

        auto g = [&](int a, int b, int c, int d, std::uint64_t x, std::uint64_t y) {
            v[a] = v[a] + v[b] + x;
            v[d] = std::rotr(v[d] ^ v[a], 32);
            v[c] = v[c] + v[d];
            v[b] = std::rotr(v[b] ^ v[c], 24);
            v[a] = v[a] + v[b] + y;
            v[d] = std::rotr(v[d] ^ v[a], 16);
            v[c] = v[c] + v[d];
            v[b] = std::rotr(v[b] ^ v[c], 63);
        };
        for (int r { 0 }; r < 12; ++r) {
            const std::uint8_t* s { SIGMA[r] };
            g(0, 4, 8, 12, m[s[0]], m[s[1]]);
            g(1, 5, 9, 13, m[s[2]], m[s[3]]);
            g(2, 6, 10, 14, m[s[4]], m[s[5]]);
            g(3, 7, 11, 15, m[s[6]], m[s[7]]);
            g(0, 5, 10, 15, m[s[8]], m[s[9]]);
            g(1, 6, 11, 12, m[s[10]], m[s[11]]);
            g(2, 7, 8, 13, m[s[12]], m[s[13]]);
            g(3, 4, 9, 14, m[s[14]], m[s[15]]);
        }
        for (int i { 0 }; i < 8; ++i) { h_[i] ^= v[i] ^ v[i + 8]; }
    }
};

// ── BLAKE2s (32-bit, 64-byte block, ≤32-byte digest) ────────────────────────
class Blake2s {
public:
    explicit Blake2s(std::size_t outLen) : outLen_ { outLen } {
        for (int i { 0 }; i < 8; ++i) { h_[i] = IV_[i]; }
        h_[0] ^= 0x01010000U ^ static_cast<std::uint32_t>(outLen);
    }

    void update(std::span<const std::uint8_t> data) {
        std::size_t inlen { data.size() };
        const std::uint8_t* in { data.data() };
        if (inlen == 0) { return; }
        std::size_t left { bufLen_ };
        std::size_t fill { 64 - left };
        if (inlen > fill) {
            bufLen_ = 0;
            std::memcpy(buf_ + left, in, fill);
            inc_counter_(64);
            compress_(buf_);
            in += fill;
            inlen -= fill;
            while (inlen > 64) {
                inc_counter_(64);
                compress_(in);
                in += 64;
                inlen -= 64;
            }
        }
        std::memcpy(buf_ + bufLen_, in, inlen);
        bufLen_ += inlen;
    }

    void final_(std::uint8_t* out) {
        inc_counter_(static_cast<std::uint32_t>(bufLen_));
        f_[0] = ~0U;
        std::memset(buf_ + bufLen_, 0, 64 - bufLen_);
        compress_(buf_);
        for (std::size_t i { 0 }; i < outLen_; ++i) {
            out[i] = static_cast<std::uint8_t>(h_[i >> 2] >> (8 * (i & 3)));
        }
    }

private:
    static constexpr std::uint32_t IV_[8] {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    std::uint32_t h_[8] {};
    std::uint32_t t_[2] { 0, 0 };
    std::uint32_t f_[2] { 0, 0 };
    std::uint8_t buf_[64] {};
    std::size_t bufLen_ { 0 };
    std::size_t outLen_;

    void inc_counter_(std::uint32_t inc) {
        t_[0] += inc;
        if (t_[0] < inc) { ++t_[1]; }
    }

    static std::uint32_t load32_(const std::uint8_t* p) {
        std::uint32_t v { 0 };
        for (int i { 0 }; i < 4; ++i) { v |= static_cast<std::uint32_t>(p[i]) << (8 * i); }
        return v;
    }

    void compress_(const std::uint8_t* block) {
        static constexpr std::uint8_t SIGMA[10][16] {
            { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
            { 14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3 },
            { 11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4 },
            { 7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8 },
            { 9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13 },
            { 2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9 },
            { 12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11 },
            { 13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10 },
            { 6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5 },
            { 10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0 },
        };
        std::uint32_t m[16];
        for (int i { 0 }; i < 16; ++i) { m[i] = load32_(block + i * 4); }
        std::uint32_t v[16];
        for (int i { 0 }; i < 8; ++i) {
            v[i] = h_[i];
            v[i + 8] = IV_[i];
        }
        v[12] ^= t_[0];
        v[13] ^= t_[1];
        v[14] ^= f_[0];
        v[15] ^= f_[1];

        auto g = [&](int a, int b, int c, int d, std::uint32_t x, std::uint32_t y) {
            v[a] = v[a] + v[b] + x;
            v[d] = std::rotr(v[d] ^ v[a], 16);
            v[c] = v[c] + v[d];
            v[b] = std::rotr(v[b] ^ v[c], 12);
            v[a] = v[a] + v[b] + y;
            v[d] = std::rotr(v[d] ^ v[a], 8);
            v[c] = v[c] + v[d];
            v[b] = std::rotr(v[b] ^ v[c], 7);
        };
        for (int r { 0 }; r < 10; ++r) {
            const std::uint8_t* s { SIGMA[r] };
            g(0, 4, 8, 12, m[s[0]], m[s[1]]);
            g(1, 5, 9, 13, m[s[2]], m[s[3]]);
            g(2, 6, 10, 14, m[s[4]], m[s[5]]);
            g(3, 7, 11, 15, m[s[6]], m[s[7]]);
            g(0, 5, 10, 15, m[s[8]], m[s[9]]);
            g(1, 6, 11, 12, m[s[10]], m[s[11]]);
            g(2, 7, 8, 13, m[s[12]], m[s[13]]);
            g(3, 4, 9, 14, m[s[14]], m[s[15]]);
        }
        for (int i { 0 }; i < 8; ++i) { h_[i] ^= v[i] ^ v[i + 8]; }
    }
};

// ── One-shot helpers (node/bun default digest lengths) ──────────────────────
inline std::vector<std::uint8_t> blake2b512(std::span<const std::uint8_t> data) {
    Blake2b h { 64 };
    h.update(data);
    std::vector<std::uint8_t> out(64);
    h.final_(out.data());
    return out;
}
inline std::vector<std::uint8_t> blake2s256(std::span<const std::uint8_t> data) {
    Blake2s h { 32 };
    h.update(data);
    std::vector<std::uint8_t> out(32);
    h.final_(out.data());
    return out;
}

}  // namespace mbun::crypto
