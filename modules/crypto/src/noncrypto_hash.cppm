// mbun.crypto.noncrypto_hash — non-cryptographic hashes behind `Bun.hash.*`.
//
// Mechanical translation of bun's Rust references:
//   adler32   ← src/hash/adler32.rs   (zlib adler32)
//   crc32     ← zlib crc32 (bun uses bun_zlib::crc32_bytes; standard IEEE poly)
//   cityHash  ← src/hash/cityhash.rs  (Google/Abseil CityHash32 + CityHash64)
//   murmur    ← src/hash/murmur.rs    (Murmur2_32/Murmur2_64/Murmur3_32)
//   rapidhash ← src/hash/rapidhash.rs
//   xxHash32/64 ← reference scalar xxHash (bun routes these through the Highway
//                 SIMD kernel; output is bit-identical to the scalar reference).
//
// Output is required to be bit-identical to each algorithm's reference, pinned
// by test/js/bun/util/hash.test.js. xxHash3 is the SCALAR XXH3_64bits reference
// (Yann Collet, xxhash.h v0.8 — bit-identical to the SIMD dispatch bun uses).
// DEFERRED(S2): the CityHash128 seed variants (unused by Bun.hash). Unaligned LE
// loads via std::memcpy; 128-bit mul via __int128.

export module mbun.crypto.noncrypto_hash;

import std;

namespace mbun::crypto::detail {

inline std::uint32_t le32(std::uint32_t v) {
    if constexpr (std::endian::native == std::endian::big) { return std::byteswap(v); }
    return v;
}
inline std::uint64_t le64(std::uint64_t v) {
    if constexpr (std::endian::native == std::endian::big) { return std::byteswap(v); }
    return v;
}
inline std::uint32_t fetch32(const std::uint8_t* p, std::size_t off) {
    std::uint32_t v {};
    std::memcpy(&v, p + off, 4);
    return le32(v);
}
inline std::uint64_t fetch64(const std::uint8_t* p, std::size_t off) {
    std::uint64_t v {};
    std::memcpy(&v, p + off, 8);
    return le64(v);
}

// Store a little-endian u64 at p+off (mirrors xxhash.h XXH_writeLE64).
inline void store64le(std::uint8_t* p, std::size_t off, std::uint64_t v) {
    v = le64(v);
    std::memcpy(p + off, &v, 8);
}

// 64x64→128 multiply, low ^ high (xxhash.h XXH3_mul128_fold64).
inline std::uint64_t mul128_fold64(std::uint64_t a, std::uint64_t b) {
    unsigned __int128 r { static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b) };
    return static_cast<std::uint64_t>(r) ^ static_cast<std::uint64_t>(r >> 64);
}

// Murmur3 32-bit finalizer (avalanche). Shared with CityHash32.
inline std::uint32_t fmix32(std::uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6bU;
    h ^= h >> 13;
    h *= 0xc2b2ae35U;
    return h ^ (h >> 16);
}

}  // namespace mbun::crypto::detail

export namespace mbun::crypto {

// ── Adler-32 (src/hash/adler32.rs) ─────────────────────────────────────────
class Adler32 {
public:
    static constexpr std::uint32_t BASE { 65521 };
    static constexpr std::size_t NMAX { 5552 };

    static std::uint32_t permute(std::uint32_t state, std::span<const std::uint8_t> input) {
        std::uint32_t s1 { state & 0xffff };
        std::uint32_t s2 { (state >> 16) & 0xffff };
        std::size_t len { input.size() };

        if (len == 1) {
            s1 += input[0];
            if (s1 >= BASE) { s1 -= BASE; }
            s2 += s1;
            if (s2 >= BASE) { s2 -= BASE; }
        } else if (len < 16) {
            for (auto b : input) {
                s1 += b;
                s2 += s1;
            }
            if (s1 >= BASE) { s1 -= BASE; }
            s2 %= BASE;
        } else {
            constexpr std::size_t N { NMAX / 16 };
            std::size_t i { 0 };
            while (i + NMAX <= len) {
                for (std::size_t rounds { 0 }; rounds < N; ++rounds) {
                    for (std::size_t j { 0 }; j < 16; ++j) {
                        s1 += input[i + j];
                        s2 += s1;
                    }
                    i += 16;
                }
                s1 %= BASE;
                s2 %= BASE;
            }
            if (i < len) {
                while (i + 16 <= len) {
                    for (std::size_t j { 0 }; j < 16; ++j) {
                        s1 += input[i + j];
                        s2 += s1;
                    }
                    i += 16;
                }
                while (i < len) {
                    s1 += input[i];
                    s2 += s1;
                    ++i;
                }
                s1 %= BASE;
                s2 %= BASE;
            }
        }
        return s1 | (s2 << 16);
    }

    static std::uint32_t hash(std::span<const std::uint8_t> input) { return permute(1, input); }
};

// ── CRC-32 (zlib IEEE, reflected poly 0xEDB88320) ───────────────────────────
class Crc32 {
public:
    static std::uint32_t hash(std::uint32_t seed, std::span<const std::uint8_t> input) {
        const auto& table { table_() };
        std::uint32_t c { seed ^ 0xffffffffU };
        for (auto b : input) { c = table[(c ^ b) & 0xff] ^ (c >> 8); }
        return c ^ 0xffffffffU;
    }
    static std::uint32_t hash(std::span<const std::uint8_t> input) { return hash(0, input); }

private:
    static const std::array<std::uint32_t, 256>& table_() {
        static const std::array<std::uint32_t, 256> t { build_() };
        return t;
    }
    static std::array<std::uint32_t, 256> build_() {
        std::array<std::uint32_t, 256> t {};
        for (std::uint32_t n { 0 }; n < 256; ++n) {
            std::uint32_t c { n };
            for (int k { 0 }; k < 8; ++k) {
                c = (c & 1) ? (0xedb88320U ^ (c >> 1)) : (c >> 1);
            }
            t[n] = c;
        }
        return t;
    }
};

// ── Murmur family (src/hash/murmur.rs) ──────────────────────────────────────
class Murmur2_32 {
public:
    static constexpr std::uint32_t DEFAULT_SEED { 0xc70f6907U };
    static std::uint32_t hash_with_seed(std::span<const std::uint8_t> str, std::uint32_t seed) {
        constexpr std::uint32_t M { 0x5bd1e995U };
        std::uint32_t len { static_cast<std::uint32_t>(str.size()) };
        std::uint32_t h1 { seed ^ len };
        const std::uint8_t* p { str.data() };

        std::size_t blocks { len >> 2 };
        for (std::size_t i { 0 }; i < blocks; ++i) {
            std::uint32_t k1 { detail::fetch32(p, i * 4) };
            k1 *= M;
            k1 ^= k1 >> 24;
            k1 *= M;
            h1 *= M;
            h1 ^= k1;
        }
        std::size_t offset { static_cast<std::size_t>(len & 0xfffffffcU) };
        std::uint32_t rest { len & 3 };
        if (rest >= 3) { h1 ^= static_cast<std::uint32_t>(p[offset + 2]) << 16; }
        if (rest >= 2) { h1 ^= static_cast<std::uint32_t>(p[offset + 1]) << 8; }
        if (rest >= 1) {
            h1 ^= static_cast<std::uint32_t>(p[offset]);
            h1 *= M;
        }
        h1 ^= h1 >> 13;
        h1 *= M;
        h1 ^= h1 >> 15;
        return h1;
    }
    static std::uint32_t hash(std::span<const std::uint8_t> str) { return hash_with_seed(str, DEFAULT_SEED); }
};

class Murmur2_64 {
public:
    static constexpr std::uint64_t DEFAULT_SEED { 0xc70f6907ULL };
    static std::uint64_t hash_with_seed(std::span<const std::uint8_t> str, std::uint64_t seed) {
        constexpr std::uint64_t M { 0xc6a4a7935bd1e995ULL };
        std::size_t n { str.size() };
        const std::uint8_t* p { str.data() };
        std::uint64_t h1 { seed ^ (static_cast<std::uint64_t>(n) * M) };

        std::size_t blocks { n / 8 };
        for (std::size_t i { 0 }; i < blocks; ++i) {
            std::uint64_t k1 { detail::fetch64(p, i * 8) };
            k1 *= M;
            k1 ^= k1 >> 47;
            k1 *= M;
            h1 ^= k1;
            h1 *= M;
        }
        std::size_t rest { n & 7 };
        std::size_t offset { n - rest };
        if (rest > 0) {
            std::uint8_t buf[8] {};
            std::memcpy(buf, p + offset, rest);
            std::uint64_t k1 {};
            std::memcpy(&k1, buf, 8);
            k1 = detail::le64(k1);
            h1 ^= k1;
            h1 *= M;
        }
        h1 ^= h1 >> 47;
        h1 *= M;
        h1 ^= h1 >> 47;
        return h1;
    }
    static std::uint64_t hash(std::span<const std::uint8_t> str) { return hash_with_seed(str, DEFAULT_SEED); }
};

class Murmur3_32 {
public:
    static constexpr std::uint32_t DEFAULT_SEED { 0xc70f6907U };
    static std::uint32_t hash_with_seed(std::span<const std::uint8_t> str, std::uint32_t seed) {
        constexpr std::uint32_t C1 { 0xcc9e2d51U };
        constexpr std::uint32_t C2 { 0x1b873593U };
        std::uint32_t len { static_cast<std::uint32_t>(str.size()) };
        std::uint32_t h1 { seed };
        const std::uint8_t* p { str.data() };

        std::size_t blocks { len >> 2 };
        for (std::size_t i { 0 }; i < blocks; ++i) {
            std::uint32_t k1 { detail::fetch32(p, i * 4) };
            k1 *= C1;
            k1 = std::rotl(k1, 15);
            k1 *= C2;
            h1 ^= k1;
            h1 = std::rotl(h1, 13);
            h1 = h1 * 5 + 0xe6546b64U;
        }
        {
            std::uint32_t k1 { 0 };
            std::size_t offset { static_cast<std::size_t>(len & 0xfffffffcU) };
            std::uint32_t rest { len & 3 };
            if (rest == 3) { k1 ^= static_cast<std::uint32_t>(p[offset + 2]) << 16; }
            if (rest >= 2) { k1 ^= static_cast<std::uint32_t>(p[offset + 1]) << 8; }
            if (rest >= 1) {
                k1 ^= static_cast<std::uint32_t>(p[offset]);
                k1 *= C1;
                k1 = std::rotl(k1, 15);
                k1 *= C2;
                h1 ^= k1;
            }
        }
        h1 ^= len;
        return detail::fmix32(h1);
    }
    static std::uint32_t hash(std::span<const std::uint8_t> str) { return hash_with_seed(str, DEFAULT_SEED); }
};

// ── CityHash32 / CityHash64 (src/hash/cityhash.rs) ──────────────────────────
class CityHash32 {
public:
    static std::uint32_t hash(std::span<const std::uint8_t> str);

private:
    static constexpr std::uint32_t C1 { 0xcc9e2d51U };
    static constexpr std::uint32_t C2 { 0x1b873593U };
    static std::uint32_t mur_(std::uint32_t a, std::uint32_t h) {
        a *= C1;
        a = std::rotr(a, 17);
        a *= C2;
        h ^= a;
        h = std::rotr(h, 19);
        return h * 5 + 0xe6546b64U;
    }
    static std::uint32_t len04_(const std::uint8_t* p, std::size_t len);
    static std::uint32_t len512_(const std::uint8_t* p, std::size_t len);
    static std::uint32_t len1324_(const std::uint8_t* p, std::size_t len);
};

inline std::uint32_t CityHash32::len04_(const std::uint8_t* p, std::size_t len) {
    std::uint32_t b { 0 };
    std::uint32_t c { 9 };
    for (std::size_t i { 0 }; i < len; ++i) {
        std::int8_t sv { static_cast<std::int8_t>(p[i]) };
        b = b * C1 + static_cast<std::uint32_t>(static_cast<std::int32_t>(sv));
        c ^= b;
    }
    return detail::fmix32(mur_(b, mur_(static_cast<std::uint32_t>(len), c)));
}
inline std::uint32_t CityHash32::len512_(const std::uint8_t* p, std::size_t len) {
    std::uint32_t a { static_cast<std::uint32_t>(len) };
    std::uint32_t b { a * 5 };
    std::uint32_t c { 9 };
    std::uint32_t d { b };
    a += detail::fetch32(p, 0);
    b += detail::fetch32(p, len - 4);
    c += detail::fetch32(p, (len >> 1) & 4);
    return detail::fmix32(mur_(c, mur_(b, mur_(a, d))));
}
inline std::uint32_t CityHash32::len1324_(const std::uint8_t* p, std::size_t len) {
    std::uint32_t a { detail::fetch32(p, (len >> 1) - 4) };
    std::uint32_t b { detail::fetch32(p, 4) };
    std::uint32_t c { detail::fetch32(p, len - 8) };
    std::uint32_t d { detail::fetch32(p, len >> 1) };
    std::uint32_t e { detail::fetch32(p, 0) };
    std::uint32_t f { detail::fetch32(p, len - 4) };
    return detail::fmix32(
        mur_(f, mur_(e, mur_(d, mur_(c, mur_(b, mur_(a, static_cast<std::uint32_t>(len))))))));
}
inline std::uint32_t CityHash32::hash(std::span<const std::uint8_t> str) {
    const std::uint8_t* p { str.data() };
    std::size_t n { str.size() };
    if (n <= 24) {
        if (n <= 4) { return len04_(p, n); }
        if (n <= 12) { return len512_(p, n); }
        return len1324_(p, n);
    }

    std::uint32_t len { static_cast<std::uint32_t>(n) };
    std::uint32_t h { len };
    std::uint32_t g { C1 * len };
    std::uint32_t f { g };

    auto rc = [](std::uint32_t x) { return std::rotr(x * C1, 17) * C2; };
    std::uint32_t a0 { rc(detail::fetch32(p, n - 4)) };
    std::uint32_t a1 { rc(detail::fetch32(p, n - 8)) };
    std::uint32_t a2 { rc(detail::fetch32(p, n - 16)) };
    std::uint32_t a3 { rc(detail::fetch32(p, n - 12)) };
    std::uint32_t a4 { rc(detail::fetch32(p, n - 20)) };

    h ^= a0;
    h = std::rotr(h, 19);
    h = h * 5 + 0xe6546b64U;
    h ^= a2;
    h = std::rotr(h, 19);
    h = h * 5 + 0xe6546b64U;
    g ^= a1;
    g = std::rotr(g, 19);
    g = g * 5 + 0xe6546b64U;
    g ^= a3;
    g = std::rotr(g, 19);
    g = g * 5 + 0xe6546b64U;
    f += a4;
    f = std::rotr(f, 19);
    f = f * 5 + 0xe6546b64U;

    std::size_t iters { (n - 1) / 20 };
    std::size_t off { 0 };
    while (iters != 0) {
        std::uint32_t b0 { rc(detail::fetch32(p, off)) };
        std::uint32_t b1 { detail::fetch32(p, off + 4) };
        std::uint32_t b2 { rc(detail::fetch32(p, off + 8)) };
        std::uint32_t b3 { rc(detail::fetch32(p, off + 12)) };
        std::uint32_t b4 { detail::fetch32(p, off + 16) };

        h ^= b0;
        h = std::rotr(h, 18);
        h = h * 5 + 0xe6546b64U;
        f += b1;
        f = std::rotr(f, 19);
        f = f * C1;
        g += b2;
        g = std::rotr(g, 18);
        g = g * 5 + 0xe6546b64U;
        h ^= b3 + b1;
        h = std::rotr(h, 19);
        h = h * 5 + 0xe6546b64U;
        g ^= b4;
        g = std::byteswap(g) * 5;
        h += b4 * 5;
        h = std::byteswap(h);
        f += b0;
        std::uint32_t t { h };
        h = f;
        f = g;
        g = t;
        off += 20;
        --iters;
    }

    g = std::rotr(g, 11) * C1;
    g = std::rotr(g, 17) * C1;
    f = std::rotr(f, 11) * C1;
    f = std::rotr(f, 17) * C1;
    h = std::rotr(h + g, 19);
    h = h * 5 + 0xe6546b64U;
    h = std::rotr(h, 17) * C1;
    h = std::rotr(h + f, 19);
    h = h * 5 + 0xe6546b64U;
    h = std::rotr(h, 17) * C1;
    return h;
}

class CityHash64 {
public:
    static std::uint64_t hash(std::span<const std::uint8_t> str);
    static std::uint64_t hash_with_seed(std::span<const std::uint8_t> str, std::uint64_t seed) {
        return hash_with_seeds(str, K2, seed);
    }
    static std::uint64_t hash_with_seeds(std::span<const std::uint8_t> str, std::uint64_t seed0,
                                         std::uint64_t seed1) {
        return hash_len16_(hash(str) - seed0, seed1);
    }

private:
    static constexpr std::uint64_t K0 { 0xc3a5c85c97cb3127ULL };
    static constexpr std::uint64_t K1 { 0xb492b66fbe98f273ULL };
    static constexpr std::uint64_t K2 { 0x9ae16a3b2f90404fULL };

    struct WeakPair {
        std::uint64_t first;
        std::uint64_t second;
    };
    static std::uint64_t shiftmix_(std::uint64_t v) { return v ^ (v >> 47); }
    static std::uint64_t hash_len16_mul_(std::uint64_t low, std::uint64_t high, std::uint64_t mul) {
        std::uint64_t a { (low ^ high) * mul };
        a ^= a >> 47;
        std::uint64_t b { (high ^ a) * mul };
        b ^= b >> 47;
        return b * mul;
    }
    static std::uint64_t hash_len16_(std::uint64_t u, std::uint64_t v) {
        return hash_len16_mul_(u, v, 0x9ddfea08eb382d69ULL);
    }
    static std::uint64_t len016_(const std::uint8_t* p, std::size_t len);
    static std::uint64_t len1732_(const std::uint8_t* p, std::size_t len);
    static std::uint64_t len3364_(const std::uint8_t* p, std::size_t len);
    static WeakPair weak32_(const std::uint8_t* p, std::size_t off, std::uint64_t a, std::uint64_t b);
};

inline std::uint64_t CityHash64::len016_(const std::uint8_t* p, std::size_t len) {
    if (len >= 8) {
        std::uint64_t mul { K2 + len * 2 };
        std::uint64_t a { detail::fetch64(p, 0) + K2 };
        std::uint64_t b { detail::fetch64(p, len - 8) };
        std::uint64_t c { std::rotr(b, 37) * mul + a };
        std::uint64_t d { (std::rotr(a, 25) + b) * mul };
        return hash_len16_mul_(c, d, mul);
    }
    if (len >= 4) {
        std::uint64_t mul { K2 + len * 2 };
        std::uint64_t a { detail::fetch32(p, 0) };
        return hash_len16_mul_(len + (a << 3), detail::fetch32(p, len - 4), mul);
    }
    if (len > 0) {
        std::uint8_t a { p[0] };
        std::uint8_t b { p[len >> 1] };
        std::uint8_t c { p[len - 1] };
        std::uint32_t y { static_cast<std::uint32_t>(a) + (static_cast<std::uint32_t>(b) << 8) };
        std::uint32_t z { static_cast<std::uint32_t>(len) + (static_cast<std::uint32_t>(c) << 2) };
        return shiftmix_(static_cast<std::uint64_t>(y) * K2 ^ static_cast<std::uint64_t>(z) * K0) * K2;
    }
    return K2;
}
inline std::uint64_t CityHash64::len1732_(const std::uint8_t* p, std::size_t len) {
    std::uint64_t mul { K2 + len * 2 };
    std::uint64_t a { detail::fetch64(p, 0) * K1 };
    std::uint64_t b { detail::fetch64(p, 8) };
    std::uint64_t c { detail::fetch64(p, len - 8) * mul };
    std::uint64_t d { detail::fetch64(p, len - 16) * K2 };
    return hash_len16_mul_(std::rotr(a + b, 43) + std::rotr(c, 30) + d,
                           a + std::rotr(b + K2, 18) + c, mul);
}
inline std::uint64_t CityHash64::len3364_(const std::uint8_t* p, std::size_t len) {
    std::uint64_t mul { K2 + len * 2 };
    std::uint64_t a { detail::fetch64(p, 0) * K2 };
    std::uint64_t b { detail::fetch64(p, 8) };
    std::uint64_t c { detail::fetch64(p, len - 24) };
    std::uint64_t d { detail::fetch64(p, len - 32) };
    std::uint64_t e { detail::fetch64(p, 16) * K2 };
    std::uint64_t f { detail::fetch64(p, 24) * 9 };
    std::uint64_t g { detail::fetch64(p, len - 8) };
    std::uint64_t h { detail::fetch64(p, len - 16) * mul };

    std::uint64_t u { std::rotr(a + g, 43) + (std::rotr(b, 30) + c) * 9 };
    std::uint64_t v { ((a + g) ^ d) + f + 1 };
    std::uint64_t w { std::byteswap((u + v) * mul) + h };
    std::uint64_t x { std::rotr(e + f, 42) + c };
    std::uint64_t y { (std::byteswap((v + w) * mul) + g) * mul };
    std::uint64_t z { e + f + c };
    std::uint64_t a1 { std::byteswap((x + z) * mul + y) + b };
    std::uint64_t b1 { shiftmix_((z + a1) * mul + d + h) * mul };
    return b1 + x;
}
inline CityHash64::WeakPair CityHash64::weak32_(const std::uint8_t* p, std::size_t off,
                                                std::uint64_t a, std::uint64_t b) {
    std::uint64_t w { detail::fetch64(p, off) };
    std::uint64_t x { detail::fetch64(p, off + 8) };
    std::uint64_t y { detail::fetch64(p, off + 16) };
    std::uint64_t z { detail::fetch64(p, off + 24) };
    a += w;
    b = std::rotr(b + a + z, 21);
    std::uint64_t c { a };
    a += x;
    a += y;
    b += std::rotr(a, 44);
    return WeakPair { a + z, b + c };
}
inline std::uint64_t CityHash64::hash(std::span<const std::uint8_t> str) {
    const std::uint8_t* p { str.data() };
    std::size_t n { str.size() };
    if (n <= 32) {
        if (n <= 16) { return len016_(p, n); }
        return len1732_(p, n);
    } else if (n <= 64) {
        return len3364_(p, n);
    }

    std::uint64_t len { static_cast<std::uint64_t>(n) };
    std::uint64_t x { detail::fetch64(p, n - 40) };
    std::uint64_t y { detail::fetch64(p, n - 16) + detail::fetch64(p, n - 56) };
    std::uint64_t z { hash_len16_(detail::fetch64(p, n - 48) + len, detail::fetch64(p, n - 24)) };
    WeakPair v { weak32_(p, n - 64, len, z) };
    WeakPair w { weak32_(p, n - 32, y + K1, x) };

    x = x * K1 + detail::fetch64(p, 0);
    len = (len - 1) & ~static_cast<std::uint64_t>(63);

    std::size_t off { 0 };
    do {
        x = std::rotr(x + y + v.first + detail::fetch64(p, off + 8), 37) * K1;
        y = std::rotr(y + v.second + detail::fetch64(p, off + 48), 42) * K1;
        x ^= w.second;
        y = y + v.first + detail::fetch64(p, off + 40);
        z = std::rotr(z + w.first, 33) * K1;
        v = weak32_(p, off, v.second * K1, x + w.first);
        w = weak32_(p, off + 32, z + w.second, y + detail::fetch64(p, off + 16));
        std::swap(z, x);
        off += 64;
        len -= 64;
    } while (len != 0);

    return hash_len16_(hash_len16_(v.first, w.first) + shiftmix_(y) * K1 + z,
                       hash_len16_(v.second, w.second) + x);
}

// ── RapidHash (src/hash/rapidhash.rs) ───────────────────────────────────────
class RapidHash {
public:
    static constexpr std::uint64_t RAPID_SEED { 0xbdd89aa982704029ULL };
    static std::uint64_t hash(std::uint64_t seed, std::span<const std::uint8_t> input);

private:
    static constexpr std::uint64_t SECRET[3] {
        0x2d358dccaa6c78a5ULL, 0x8bb84b93962eacc9ULL, 0x4b33a62ed433d4a3ULL,
    };
    static void mum_(std::uint64_t& a, std::uint64_t& b) {
        unsigned __int128 r { static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b) };
        a = static_cast<std::uint64_t>(r);
        b = static_cast<std::uint64_t>(r >> 64);
    }
    static std::uint64_t mix_(std::uint64_t a, std::uint64_t b) {
        std::uint64_t ca { a };
        std::uint64_t cb { b };
        mum_(ca, cb);
        return ca ^ cb;
    }
    static std::uint64_t r64_(const std::uint8_t* p) { return detail::fetch64(p, 0); }
    static std::uint64_t r32_(const std::uint8_t* p) {
        return static_cast<std::uint64_t>(detail::fetch32(p, 0));
    }
};

inline std::uint64_t RapidHash::hash(std::uint64_t seed, std::span<const std::uint8_t> input) {
    std::size_t len { input.size() };
    const std::uint8_t* k { input.data() };
    std::uint64_t a { 0 };
    std::uint64_t b { 0 };
    std::uint64_t is[3] { seed, 0, 0 };

    is[0] ^= mix_(seed ^ SECRET[0], SECRET[1]) ^ static_cast<std::uint64_t>(len);

    if (len <= 16) {
        if (len >= 4) {
            std::size_t d { (len & 24) >> (len >> 3) };
            std::size_t e { len - 4 };
            a = (r32_(k) << 32) | r32_(k + e);
            b = (r32_(k + d) << 32) | r32_(k + (e - d));
        } else if (len > 0) {
            a = (static_cast<std::uint64_t>(k[0]) << 56)
                | (static_cast<std::uint64_t>(k[len >> 1]) << 32)
                | static_cast<std::uint64_t>(k[len - 1]);
        }
    } else {
        std::size_t remain { len };
        if (len > 48) {
            is[1] = is[0];
            is[2] = is[0];
            while (remain >= 96) {
                for (std::size_t i { 0 }; i < 6; ++i) {
                    std::uint64_t m1 { r64_(k + 8 * i * 2) };
                    std::uint64_t m2 { r64_(k + 8 * (i * 2 + 1)) };
                    is[i % 3] = mix_(m1 ^ SECRET[i % 3], m2 ^ is[i % 3]);
                }
                k += 96;
                remain -= 96;
            }
            if (remain >= 48) {
                for (std::size_t i { 0 }; i < 3; ++i) {
                    std::uint64_t m1 { r64_(k + 8 * i * 2) };
                    std::uint64_t m2 { r64_(k + 8 * (i * 2 + 1)) };
                    is[i] = mix_(m1 ^ SECRET[i], m2 ^ is[i]);
                }
                k += 48;
                remain -= 48;
            }
            is[0] ^= is[1] ^ is[2];
        }
        if (remain > 16) {
            is[0] = mix_(r64_(k) ^ SECRET[2], r64_(k + 8) ^ is[0] ^ SECRET[1]);
            if (remain > 32) {
                is[0] = mix_(r64_(k + 16) ^ SECRET[2], r64_(k + 24) ^ is[0]);
            }
        }
        a = r64_(input.data() + (len - 16));
        b = r64_(input.data() + (len - 8));
    }

    a ^= SECRET[1];
    b ^= is[0];
    mum_(a, b);
    return mix_(a ^ SECRET[0] ^ static_cast<std::uint64_t>(len), b ^ SECRET[1]);
}

// ── xxHash32 / xxHash64 (scalar reference) ──────────────────────────────────
class XxHash32 {
public:
    static std::uint32_t hash(std::uint32_t seed, std::span<const std::uint8_t> input);

private:
    static constexpr std::uint32_t P1 { 2654435761U };
    static constexpr std::uint32_t P2 { 2246822519U };
    static constexpr std::uint32_t P3 { 3266489917U };
    static constexpr std::uint32_t P4 { 668265263U };
    static constexpr std::uint32_t P5 { 374761393U };
    static std::uint32_t round_(std::uint32_t acc, std::uint32_t in) {
        acc += in * P2;
        acc = std::rotl(acc, 13);
        return acc * P1;
    }
};

inline std::uint32_t XxHash32::hash(std::uint32_t seed, std::span<const std::uint8_t> input) {
    const std::uint8_t* p { input.data() };
    std::size_t len { input.size() };
    const std::uint8_t* end { p + len };
    std::uint32_t h32 {};

    if (len >= 16) {
        std::uint32_t v1 { seed + P1 + P2 };
        std::uint32_t v2 { seed + P2 };
        std::uint32_t v3 { seed };
        std::uint32_t v4 { seed - P1 };
        const std::uint8_t* limit { end - 16 };
        do {
            v1 = round_(v1, detail::fetch32(p, 0));
            v2 = round_(v2, detail::fetch32(p, 4));
            v3 = round_(v3, detail::fetch32(p, 8));
            v4 = round_(v4, detail::fetch32(p, 12));
            p += 16;
        } while (p <= limit);
        h32 = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
    } else {
        h32 = seed + P5;
    }
    h32 += static_cast<std::uint32_t>(len);

    while (p + 4 <= end) {
        h32 += detail::fetch32(p, 0) * P3;
        h32 = std::rotl(h32, 17) * P4;
        p += 4;
    }
    while (p < end) {
        h32 += static_cast<std::uint32_t>(*p) * P5;
        h32 = std::rotl(h32, 11) * P1;
        ++p;
    }
    h32 ^= h32 >> 15;
    h32 *= P2;
    h32 ^= h32 >> 13;
    h32 *= P3;
    h32 ^= h32 >> 16;
    return h32;
}

class XxHash64 {
public:
    static std::uint64_t hash(std::uint64_t seed, std::span<const std::uint8_t> input);

private:
    static constexpr std::uint64_t P1 { 11400714785074694791ULL };
    static constexpr std::uint64_t P2 { 14029467366897019727ULL };
    static constexpr std::uint64_t P3 { 1609587929392839161ULL };
    static constexpr std::uint64_t P4 { 9650029242287828579ULL };
    static constexpr std::uint64_t P5 { 2870177450012600261ULL };
    static std::uint64_t round_(std::uint64_t acc, std::uint64_t in) {
        acc += in * P2;
        acc = std::rotl(acc, 31);
        return acc * P1;
    }
    static std::uint64_t merge_(std::uint64_t acc, std::uint64_t val) {
        val = round_(0, val);
        acc ^= val;
        return acc * P1 + P4;
    }
};

inline std::uint64_t XxHash64::hash(std::uint64_t seed, std::span<const std::uint8_t> input) {
    const std::uint8_t* p { input.data() };
    std::size_t len { input.size() };
    const std::uint8_t* end { p + len };
    std::uint64_t h64 {};

    if (len >= 32) {
        std::uint64_t v1 { seed + P1 + P2 };
        std::uint64_t v2 { seed + P2 };
        std::uint64_t v3 { seed };
        std::uint64_t v4 { seed - P1 };
        const std::uint8_t* limit { end - 32 };
        do {
            v1 = round_(v1, detail::fetch64(p, 0));
            v2 = round_(v2, detail::fetch64(p, 8));
            v3 = round_(v3, detail::fetch64(p, 16));
            v4 = round_(v4, detail::fetch64(p, 24));
            p += 32;
        } while (p <= limit);
        h64 = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
        h64 = merge_(h64, v1);
        h64 = merge_(h64, v2);
        h64 = merge_(h64, v3);
        h64 = merge_(h64, v4);
    } else {
        h64 = seed + P5;
    }
    h64 += static_cast<std::uint64_t>(len);

    while (p + 8 <= end) {
        std::uint64_t k1 { round_(0, detail::fetch64(p, 0)) };
        h64 ^= k1;
        h64 = std::rotl(h64, 27) * P1 + P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h64 ^= static_cast<std::uint64_t>(detail::fetch32(p, 0)) * P1;
        h64 = std::rotl(h64, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h64 ^= static_cast<std::uint64_t>(*p) * P5;
        h64 = std::rotl(h64, 11) * P1;
        ++p;
    }
    h64 ^= h64 >> 33;
    h64 *= P2;
    h64 ^= h64 >> 29;
    h64 *= P3;
    h64 ^= h64 >> 32;
    return h64;
}

// ── xxHash3 / XXH3_64bits (scalar reference, xxhash.h v0.8) ──────────────────
// Mechanical 1:1 translation of the scalar path (XXH_VECTOR == XXH_SCALAR),
// which is bit-identical to every SIMD dispatch. Covers all length branches
// (0-16 / 17-128 / 129-240 / >240 stripe loop) and the seeded custom-secret
// path. `hash(seed, input)` == XXH3_64bits_withSeed(input, len, seed).
class XxHash3 {
public:
    static std::uint64_t hash(std::uint64_t seed, std::span<const std::uint8_t> input);

private:
    static constexpr std::size_t SECRET_SIZE { 192 };
    static constexpr std::size_t STRIPE_LEN { 64 };
    static constexpr std::size_t ACC_NB { 8 };
    static constexpr std::size_t SECRET_CONSUME_RATE { 8 };
    static constexpr std::size_t SECRET_LASTACC_START { 7 };
    static constexpr std::size_t SECRET_MERGEACCS_START { 11 };
    static constexpr std::size_t SECRET_SIZE_MIN { 136 };
    static constexpr std::size_t MIDSIZE_MAX { 240 };
    static constexpr std::size_t MIDSIZE_STARTOFFSET { 3 };
    static constexpr std::size_t MIDSIZE_LASTOFFSET { 17 };

    static constexpr std::uint64_t PRIME_MX1 { 0x165667919E3779F9ULL };
    static constexpr std::uint64_t PRIME_MX2 { 0x9FB21C651E98DF25ULL };
    static constexpr std::uint32_t P32_1 { 0x9E3779B1U };
    static constexpr std::uint32_t P32_2 { 0x85EBCA77U };
    static constexpr std::uint32_t P32_3 { 0xC2B2AE3DU };
    static constexpr std::uint64_t P64_1 { 0x9E3779B185EBCA87ULL };
    static constexpr std::uint64_t P64_2 { 0xC2B2AE3D27D4EB4FULL };
    static constexpr std::uint64_t P64_3 { 0x165667B19E3779F9ULL };
    static constexpr std::uint64_t P64_4 { 0x85EBCA77C2B2AE63ULL };
    static constexpr std::uint64_t P64_5 { 0x27D4EB2F165667C5ULL };

    // Pseudorandom default secret (FARSH), xxhash.h XXH3_kSecret.
    static const std::array<std::uint8_t, SECRET_SIZE>& k_secret_();

    static std::uint64_t xorshift64_(std::uint64_t v, int shift) { return v ^ (v >> shift); }
    static std::uint64_t avalanche_(std::uint64_t h) {
        h = xorshift64_(h, 37);
        h *= PRIME_MX1;
        h = xorshift64_(h, 32);
        return h;
    }
    static std::uint64_t avalanche64_(std::uint64_t h) {
        h ^= h >> 33;
        h *= P64_2;
        h ^= h >> 29;
        h *= P64_3;
        h ^= h >> 32;
        return h;
    }
    static std::uint64_t rrmxmx_(std::uint64_t h, std::uint64_t len) {
        h ^= std::rotl(h, 49) ^ std::rotl(h, 24);
        h *= PRIME_MX2;
        h ^= (h >> 35) + len;
        h *= PRIME_MX2;
        return xorshift64_(h, 28);
    }
    static std::uint64_t mix16b_(const std::uint8_t* input, const std::uint8_t* secret,
                                 std::uint64_t seed) {
        std::uint64_t input_lo { detail::fetch64(input, 0) };
        std::uint64_t input_hi { detail::fetch64(input, 8) };
        return detail::mul128_fold64(input_lo ^ (detail::fetch64(secret, 0) + seed),
                                     input_hi ^ (detail::fetch64(secret, 8) - seed));
    }

    static std::uint64_t len_1to3_(const std::uint8_t* p, std::size_t len,
                                   const std::uint8_t* secret, std::uint64_t seed);
    static std::uint64_t len_4to8_(const std::uint8_t* p, std::size_t len,
                                   const std::uint8_t* secret, std::uint64_t seed);
    static std::uint64_t len_9to16_(const std::uint8_t* p, std::size_t len,
                                    const std::uint8_t* secret, std::uint64_t seed);
    static std::uint64_t len_0to16_(const std::uint8_t* p, std::size_t len,
                                    const std::uint8_t* secret, std::uint64_t seed);
    static std::uint64_t len_17to128_(const std::uint8_t* p, std::size_t len,
                                      const std::uint8_t* secret, std::uint64_t seed);
    static std::uint64_t len_129to240_(const std::uint8_t* p, std::size_t len,
                                       const std::uint8_t* secret, std::uint64_t seed);

    static void accumulate_512_(std::uint64_t* acc, const std::uint8_t* input,
                                const std::uint8_t* secret);
    static void accumulate_(std::uint64_t* acc, const std::uint8_t* input,
                            const std::uint8_t* secret, std::size_t nbStripes);
    static void scramble_acc_(std::uint64_t* acc, const std::uint8_t* secret);
    static void init_custom_secret_(std::uint8_t* out, std::uint64_t seed);
    static std::uint64_t mix2accs_(const std::uint64_t* acc, const std::uint8_t* secret);
    static std::uint64_t merge_accs_(const std::uint64_t* acc, const std::uint8_t* secret,
                                     std::uint64_t start);
    static std::uint64_t hash_long_(const std::uint8_t* input, std::size_t len,
                                    const std::uint8_t* secret);
    static std::uint64_t hash_long_with_seed_(const std::uint8_t* input, std::size_t len,
                                              std::uint64_t seed);
};

inline const std::array<std::uint8_t, XxHash3::SECRET_SIZE>& XxHash3::k_secret_() {
    static constexpr std::array<std::uint8_t, SECRET_SIZE> S { {
        0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
        0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
        0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
        0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
        0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
        0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
        0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
        0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
        0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
        0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
        0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
        0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
    } };
    return S;
}

inline std::uint64_t XxHash3::len_1to3_(const std::uint8_t* p, std::size_t len,
                                        const std::uint8_t* secret, std::uint64_t seed) {
    std::uint8_t c1 { p[0] };
    std::uint8_t c2 { p[len >> 1] };
    std::uint8_t c3 { p[len - 1] };
    std::uint32_t combined { (static_cast<std::uint32_t>(c1) << 16) | (static_cast<std::uint32_t>(c2) << 24)
                             | (static_cast<std::uint32_t>(c3) << 0)
                             | (static_cast<std::uint32_t>(len) << 8) };
    std::uint64_t bitflip {
        static_cast<std::uint64_t>(detail::fetch32(secret, 0) ^ detail::fetch32(secret, 4)) + seed };
    std::uint64_t keyed { static_cast<std::uint64_t>(combined) ^ bitflip };
    return avalanche64_(keyed);
}

inline std::uint64_t XxHash3::len_4to8_(const std::uint8_t* p, std::size_t len,
                                        const std::uint8_t* secret, std::uint64_t seed) {
    seed ^= static_cast<std::uint64_t>(std::byteswap(static_cast<std::uint32_t>(seed))) << 32;
    std::uint32_t input1 { detail::fetch32(p, 0) };
    std::uint32_t input2 { detail::fetch32(p, len - 4) };
    std::uint64_t bitflip { (detail::fetch64(secret, 8) ^ detail::fetch64(secret, 16)) - seed };
    std::uint64_t input64 { input2 + (static_cast<std::uint64_t>(input1) << 32) };
    std::uint64_t keyed { input64 ^ bitflip };
    return rrmxmx_(keyed, len);
}

inline std::uint64_t XxHash3::len_9to16_(const std::uint8_t* p, std::size_t len,
                                         const std::uint8_t* secret, std::uint64_t seed) {
    std::uint64_t bitflip1 { (detail::fetch64(secret, 24) ^ detail::fetch64(secret, 32)) + seed };
    std::uint64_t bitflip2 { (detail::fetch64(secret, 40) ^ detail::fetch64(secret, 48)) - seed };
    std::uint64_t input_lo { detail::fetch64(p, 0) ^ bitflip1 };
    std::uint64_t input_hi { detail::fetch64(p, len - 8) ^ bitflip2 };
    std::uint64_t acc { len + std::byteswap(input_lo) + input_hi
                        + detail::mul128_fold64(input_lo, input_hi) };
    return avalanche_(acc);
}

inline std::uint64_t XxHash3::len_0to16_(const std::uint8_t* p, std::size_t len,
                                         const std::uint8_t* secret, std::uint64_t seed) {
    if (len > 8) { return len_9to16_(p, len, secret, seed); }
    if (len >= 4) { return len_4to8_(p, len, secret, seed); }
    if (len != 0) { return len_1to3_(p, len, secret, seed); }
    return avalanche64_(seed ^ (detail::fetch64(secret, 56) ^ detail::fetch64(secret, 64)));
}

inline std::uint64_t XxHash3::len_17to128_(const std::uint8_t* p, std::size_t len,
                                           const std::uint8_t* secret, std::uint64_t seed) {
    std::uint64_t acc { len * P64_1 };
    if (len > 32) {
        if (len > 64) {
            if (len > 96) {
                acc += mix16b_(p + 48, secret + 96, seed);
                acc += mix16b_(p + len - 64, secret + 112, seed);
            }
            acc += mix16b_(p + 32, secret + 64, seed);
            acc += mix16b_(p + len - 48, secret + 80, seed);
        }
        acc += mix16b_(p + 16, secret + 32, seed);
        acc += mix16b_(p + len - 32, secret + 48, seed);
    }
    acc += mix16b_(p + 0, secret + 0, seed);
    acc += mix16b_(p + len - 16, secret + 16, seed);
    return avalanche_(acc);
}

inline std::uint64_t XxHash3::len_129to240_(const std::uint8_t* p, std::size_t len,
                                            const std::uint8_t* secret, std::uint64_t seed) {
    std::uint64_t acc { len * P64_1 };
    unsigned nbRounds { static_cast<unsigned>(len / 16) };
    for (unsigned i { 0 }; i < 8; ++i) {
        acc += mix16b_(p + 16 * i, secret + 16 * i, seed);
    }
    std::uint64_t acc_end {
        mix16b_(p + len - 16, secret + SECRET_SIZE_MIN - MIDSIZE_LASTOFFSET, seed) };
    acc = avalanche_(acc);
    for (unsigned i { 8 }; i < nbRounds; ++i) {
        acc_end += mix16b_(p + 16 * i, secret + 16 * (i - 8) + MIDSIZE_STARTOFFSET, seed);
    }
    return avalanche_(acc + acc_end);
}

inline void XxHash3::accumulate_512_(std::uint64_t* acc, const std::uint8_t* input,
                                     const std::uint8_t* secret) {
    for (std::size_t lane { 0 }; lane < ACC_NB; ++lane) {
        std::uint64_t data_val { detail::fetch64(input, lane * 8) };
        std::uint64_t data_key { data_val ^ detail::fetch64(secret, lane * 8) };
        acc[lane ^ 1] += data_val;  // swap adjacent lanes
        acc[lane] += static_cast<std::uint64_t>(static_cast<std::uint32_t>(data_key))
                     * static_cast<std::uint64_t>(static_cast<std::uint32_t>(data_key >> 32));
    }
}

inline void XxHash3::accumulate_(std::uint64_t* acc, const std::uint8_t* input,
                                 const std::uint8_t* secret, std::size_t nbStripes) {
    for (std::size_t n { 0 }; n < nbStripes; ++n) {
        accumulate_512_(acc, input + n * STRIPE_LEN, secret + n * SECRET_CONSUME_RATE);
    }
}

inline void XxHash3::scramble_acc_(std::uint64_t* acc, const std::uint8_t* secret) {
    for (std::size_t lane { 0 }; lane < ACC_NB; ++lane) {
        std::uint64_t key64 { detail::fetch64(secret, lane * 8) };
        std::uint64_t a { acc[lane] };
        a = xorshift64_(a, 47);
        a ^= key64;
        a *= static_cast<std::uint64_t>(P32_1);
        acc[lane] = a;
    }
}

inline void XxHash3::init_custom_secret_(std::uint8_t* out, std::uint64_t seed) {
    const std::uint8_t* k { k_secret_().data() };
    constexpr int nbRounds { static_cast<int>(SECRET_SIZE) / 16 };
    for (int i { 0 }; i < nbRounds; ++i) {
        std::uint64_t lo { detail::fetch64(k, 16 * i) + seed };
        std::uint64_t hi { detail::fetch64(k, 16 * i + 8) - seed };
        detail::store64le(out, 16 * i, lo);
        detail::store64le(out, 16 * i + 8, hi);
    }
}

inline std::uint64_t XxHash3::mix2accs_(const std::uint64_t* acc, const std::uint8_t* secret) {
    return detail::mul128_fold64(acc[0] ^ detail::fetch64(secret, 0),
                                 acc[1] ^ detail::fetch64(secret, 8));
}

inline std::uint64_t XxHash3::merge_accs_(const std::uint64_t* acc, const std::uint8_t* secret,
                                          std::uint64_t start) {
    std::uint64_t result64 { start };
    for (std::size_t i { 0 }; i < 4; ++i) {
        result64 += mix2accs_(acc + 2 * i, secret + 16 * i);
    }
    return avalanche_(result64);
}

inline std::uint64_t XxHash3::hash_long_(const std::uint8_t* input, std::size_t len,
                                         const std::uint8_t* secret) {
    std::array<std::uint64_t, ACC_NB> acc { {
        P32_3, P64_1, P64_2, P64_3, P64_4, P32_2, P64_5, P32_1,
    } };
    std::size_t nbStripesPerBlock { (SECRET_SIZE - STRIPE_LEN) / SECRET_CONSUME_RATE };
    std::size_t block_len { STRIPE_LEN * nbStripesPerBlock };
    std::size_t nb_blocks { (len - 1) / block_len };
    for (std::size_t n { 0 }; n < nb_blocks; ++n) {
        accumulate_(acc.data(), input + n * block_len, secret, nbStripesPerBlock);
        scramble_acc_(acc.data(), secret + SECRET_SIZE - STRIPE_LEN);
    }
    std::size_t nbStripes { ((len - 1) - (block_len * nb_blocks)) / STRIPE_LEN };
    accumulate_(acc.data(), input + nb_blocks * block_len, secret, nbStripes);
    const std::uint8_t* last { input + len - STRIPE_LEN };
    accumulate_512_(acc.data(), last, secret + SECRET_SIZE - STRIPE_LEN - SECRET_LASTACC_START);
    return merge_accs_(acc.data(), secret + SECRET_MERGEACCS_START, len * P64_1);
}

inline std::uint64_t XxHash3::hash_long_with_seed_(const std::uint8_t* input, std::size_t len,
                                                   std::uint64_t seed) {
    if (seed == 0) { return hash_long_(input, len, k_secret_().data()); }
    std::array<std::uint8_t, SECRET_SIZE> secret {};
    init_custom_secret_(secret.data(), seed);
    return hash_long_(input, len, secret.data());
}

inline std::uint64_t XxHash3::hash(std::uint64_t seed, std::span<const std::uint8_t> input) {
    const std::uint8_t* p { input.data() };
    std::size_t len { input.size() };
    const std::uint8_t* secret { k_secret_().data() };
    if (len <= 16) { return len_0to16_(p, len, secret, seed); }
    if (len <= 128) { return len_17to128_(p, len, secret, seed); }
    if (len <= MIDSIZE_MAX) { return len_129to240_(p, len, secret, seed); }
    return hash_long_with_seed_(p, len, seed);
}

}  // namespace mbun::crypto
