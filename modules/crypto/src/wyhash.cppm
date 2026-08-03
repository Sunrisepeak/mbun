// mbun.crypto.wyhash — the wyhash "final4" variant (48-byte rounds, 4 secrets).
//
// Mechanical translation of bun's Rust reference (src/wyhash/lib.rs, `Wyhash`).
// This is the algorithm behind `Bun.hash()` / `Bun.hash.wyhash()`,
// RuntimeTranspilerCache and the router. It is NOT the legacy 32-byte
// `Wyhash11` (5-prime) variant — those produce different outputs.
//
// Reference test vectors (from wangyi-fudan/wyhash test_vector.cpp) are pinned
// in tests/. Unaligned little-endian loads via std::memcpy (portable; the Rust
// used read_unaligned on x86). 128-bit multiply via unsigned __int128 (both
// GCC 16 and current Clang support it; a std::uint128 lands later).

export module mbun.crypto.wyhash;

import std;

export namespace mbun::crypto {

class Wyhash {
public:  // Big Five (trivial POD-ish state)
    Wyhash() = default;

public:
    // Secrets — same 4 constants bun's Rust `Wyhash::SECRET` uses.
    static constexpr std::uint64_t SECRET[4] {
        0xa0761d6478bd642fULL,
        0xe7037ed1a0b428dbULL,
        0x8ebc6af09c88c6e3ULL,
        0x589965cc75374cc3ULL,
    };

    static Wyhash init(std::uint64_t seed) {
        Wyhash w;
        std::uint64_t s0 { seed ^ mix_(seed ^ SECRET[0], SECRET[1]) };
        w.state_[0] = s0;
        w.state_[1] = s0;
        w.state_[2] = s0;
        return w;
    }

    // Streaming update. Mirrors the subtle wyhash rule: the last full 48-byte
    // block is only run through final1 when exactly aligned, so update keeps up
    // to 48 bytes buffered (plus a 16-byte lookback for short tails).
    void update(std::span<const std::uint8_t> input) {
        totalLen_ += input.size();

        if (input.size() <= 48 - bufLen_) {
            std::memcpy(buf_ + bufLen_, input.data(), input.size());
            bufLen_ += input.size();
            return;
        }

        std::size_t i { 0 };
        if (bufLen_ > 0) {
            i = 48 - bufLen_;
            std::memcpy(buf_ + bufLen_, input.data(), i);
            round_(buf_);
            bufLen_ = 0;
        }

        while (i + 48 < input.size()) {
            round_(input.data() + i);
            i += 48;
        }

        std::size_t remaining { input.size() - i };
        if (remaining < 16 && i >= 48) {
            std::size_t rem { 16 - remaining };
            std::memcpy(buf_ + (48 - rem), input.data() + (i - rem), rem);
        }
        std::memcpy(buf_, input.data() + i, remaining);
        bufLen_ = remaining;
    }

    // Idempotent finalize (operates on a shallow copy, like the Rust).
    std::uint64_t final_() const {
        Wyhash s { shallow_copy_() };

        if (totalLen_ <= 16) {
            s.small_key_(buf_, bufLen_);
        } else {
            std::uint8_t scratch[16] {};
            const std::uint8_t* input { buf_ };
            std::size_t inputLen { bufLen_ };
            std::size_t offset { 0 };
            if (bufLen_ < 16) {
                std::size_t rem { 16 - bufLen_ };
                std::memcpy(scratch, buf_ + (48 - rem), rem);
                std::memcpy(scratch + rem, buf_, bufLen_);
                input = scratch;
                inputLen = rem + bufLen_;
                offset = rem;
            }
            s.final0_();
            s.final1_(input, inputLen, offset);
        }
        return s.final2_();
    }

    // One-shot hash — the hot path for `Bun.hash`.
    static std::uint64_t hash(std::uint64_t seed, std::span<const std::uint8_t> input) {
        Wyhash h { init(seed) };
        std::size_t len { input.size() };

        if (len <= 16) {
            h.small_key_(input.data(), len);
        } else {
            std::size_t i { 0 };
            if (len >= 48) {
                std::uint64_t s0 { h.state_[0] };
                std::uint64_t s1 { h.state_[1] };
                std::uint64_t s2 { h.state_[2] };
                const std::uint8_t* p { input.data() };
                std::size_t bound { len - 48 };
                while (i < bound) {
                    std::uint64_t m0a { read8_(p + i) ^ SECRET[1] };
                    std::uint64_t m0b { read8_(p + i + 8) ^ s0 };
                    s0 = mix_(m0a, m0b);
                    std::uint64_t m1a { read8_(p + i + 16) ^ SECRET[2] };
                    std::uint64_t m1b { read8_(p + i + 24) ^ s1 };
                    s1 = mix_(m1a, m1b);
                    std::uint64_t m2a { read8_(p + i + 32) ^ SECRET[3] };
                    std::uint64_t m2b { read8_(p + i + 40) ^ s2 };
                    s2 = mix_(m2a, m2b);
                    i += 48;
                }
                h.state_[0] = s0;
                h.state_[1] = s1;
                h.state_[2] = s2;
                h.final0_();
            }
            h.final1_(input.data(), len, i);
        }

        h.totalLen_ = len;
        return h.final2_();
    }

private:
    std::uint64_t a_ { 0 };
    std::uint64_t b_ { 0 };
    std::uint64_t state_[3] { 0, 0, 0 };
    std::size_t totalLen_ { 0 };
    std::uint8_t buf_[48] {};
    std::size_t bufLen_ { 0 };

    static std::uint64_t read4_(const std::uint8_t* p) {
        std::uint32_t v {};
        std::memcpy(&v, p, 4);
        return static_cast<std::uint64_t>(le32_(v));
    }
    static std::uint64_t read8_(const std::uint8_t* p) {
        std::uint64_t v {};
        std::memcpy(&v, p, 8);
        return le64_(v);
    }
    static std::uint32_t le32_(std::uint32_t v) {
        if constexpr (std::endian::native == std::endian::big) { return std::byteswap(v); }
        return v;
    }
    static std::uint64_t le64_(std::uint64_t v) {
        if constexpr (std::endian::native == std::endian::big) { return std::byteswap(v); }
        return v;
    }

    static std::uint64_t mix_(std::uint64_t a, std::uint64_t b) {
        unsigned __int128 x { static_cast<unsigned __int128>(a) * static_cast<unsigned __int128>(b) };
        return static_cast<std::uint64_t>(x) ^ static_cast<std::uint64_t>(x >> 64);
    }

    Wyhash shallow_copy_() const {
        Wyhash w;
        w.a_ = a_;
        w.b_ = b_;
        w.state_[0] = state_[0];
        w.state_[1] = state_[1];
        w.state_[2] = state_[2];
        w.totalLen_ = totalLen_;
        return w;
    }

    void small_key_(const std::uint8_t* input, std::size_t len) {
        if (len >= 4) {
            std::size_t end { len - 4 };
            std::size_t quarter { (len >> 3) << 2 };
            a_ = (read4_(input) << 32) | read4_(input + quarter);
            b_ = (read4_(input + end) << 32) | read4_(input + (end - quarter));
        } else if (len > 0) {
            a_ = (static_cast<std::uint64_t>(input[0]) << 16)
                | (static_cast<std::uint64_t>(input[len >> 1]) << 8)
                | static_cast<std::uint64_t>(input[len - 1]);
            b_ = 0;
        } else {
            a_ = 0;
            b_ = 0;
        }
    }

    void round_(const std::uint8_t* input) {
        state_[0] = mix_(read8_(input) ^ SECRET[1], read8_(input + 8) ^ state_[0]);
        state_[1] = mix_(read8_(input + 16) ^ SECRET[2], read8_(input + 24) ^ state_[1]);
        state_[2] = mix_(read8_(input + 32) ^ SECRET[3], read8_(input + 40) ^ state_[2]);
    }

    void final0_() { state_[0] ^= state_[1] ^ state_[2]; }

    // input[start..] is the live remainder; input is at least 16 bytes long.
    void final1_(const std::uint8_t* input, std::size_t inputLen, std::size_t start) {
        std::size_t i { start };
        while (i + 16 < inputLen) {
            state_[0] = mix_(read8_(input + i) ^ SECRET[1], read8_(input + i + 8) ^ state_[0]);
            i += 16;
        }
        a_ = read8_(input + (inputLen - 16));
        b_ = read8_(input + (inputLen - 8));
    }

    std::uint64_t final2_() {
        a_ ^= SECRET[1];
        b_ ^= state_[0];
        unsigned __int128 x { static_cast<unsigned __int128>(a_) * static_cast<unsigned __int128>(b_) };
        a_ = static_cast<std::uint64_t>(x);
        b_ = static_cast<std::uint64_t>(x >> 64);
        return mix_(a_ ^ SECRET[0] ^ static_cast<std::uint64_t>(totalLen_), b_ ^ SECRET[1]);
    }
};

// `Bun.hash(bytes)` — wyhash with seed 0.
inline std::uint64_t wyhash(std::span<const std::uint8_t> bytes) {
    return Wyhash::hash(0, bytes);
}
inline std::uint64_t wyhash_with_seed(std::uint64_t seed, std::span<const std::uint8_t> bytes) {
    return Wyhash::hash(seed, bytes);
}

// Integer diffusion (hash-prospector / mx3), ported from src/wyhash/lib.rs.
inline std::uint16_t hash_int16(std::uint16_t x) {
    x = static_cast<std::uint16_t>((x ^ (x >> 7)) * 0x2993);
    x = static_cast<std::uint16_t>((x ^ (x >> 5)) * 0xe877);
    x = static_cast<std::uint16_t>((x ^ (x >> 9)) * 0x0235);
    return static_cast<std::uint16_t>(x ^ (x >> 10));
}
inline std::uint32_t hash_int32(std::uint32_t x) {
    x = (x ^ (x >> 17)) * 0xed5ad4bbU;
    x = (x ^ (x >> 11)) * 0xac4c1b51U;
    x = (x ^ (x >> 15)) * 0x31848babU;
    return x ^ (x >> 14);
}
inline std::uint64_t hash_int64(std::uint64_t x) {
    constexpr std::uint64_t C { 0xbea225f9eb34556dULL };
    x = (x ^ (x >> 32)) * C;
    x = (x ^ (x >> 29)) * C;
    x = (x ^ (x >> 32)) * C;
    return x ^ (x >> 29);
}

}  // namespace mbun::crypto
