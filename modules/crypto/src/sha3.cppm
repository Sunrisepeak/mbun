// mbun.crypto.sha3 — FIPS 202 Keccak: SHA3-224/256/384/512 + SHAKE128/256.
//
// bun routes these through RustCrypto's `sha3` crate (no BoringSSL EVP for the
// Keccak family); that dependency is unavailable here, so this is a
// self-contained Keccak-f[1600] sponge with bit-identical output to the NIST
// known-answer tests. Little-endian lane layout per FIPS 202 (matches the
// module's existing LE-host assumption).

export module mbun.crypto.sha3;

import std;

export namespace mbun::crypto {

// ── Keccak-f[1600] sponge ───────────────────────────────────────────────────
class Keccak {
public:
    // rateBytes: sponge rate (block size). suffix: domain-separation byte
    // (0x06 for SHA-3, 0x1f for SHAKE) — the first bit of the pad10*1 padding.
    Keccak(std::size_t rateBytes, std::uint8_t suffix)
        : rate_ { rateBytes }, suffix_ { suffix } {}

    void update(std::span<const std::uint8_t> data) {
        std::uint8_t* sb { state_bytes_() };
        for (std::uint8_t b : data) {
            sb[pos_] ^= b;
            ++pos_;
            if (pos_ == rate_) {
                permute_();
                pos_ = 0;
            }
        }
    }

    // Squeeze outLen bytes. Finalizes the sponge (single call per instance).
    void squeeze(std::uint8_t* out, std::size_t outLen) {
        std::uint8_t* sb { state_bytes_() };
        sb[pos_] ^= suffix_;
        sb[rate_ - 1] ^= 0x80;
        permute_();
        pos_ = 0;
        std::size_t i { 0 };
        while (i < outLen) {
            if (pos_ == rate_) {
                permute_();
                pos_ = 0;
            }
            std::size_t take { std::min(rate_ - pos_, outLen - i) };
            for (std::size_t j { 0 }; j < take; ++j) { out[i + j] = sb[pos_ + j]; }
            pos_ += take;
            i += take;
        }
    }

private:
    std::uint64_t s_[25] {};
    std::size_t rate_;
    std::uint8_t suffix_;
    std::size_t pos_ { 0 };

    std::uint8_t* state_bytes_() { return reinterpret_cast<std::uint8_t*>(s_); }

    void permute_() {
        static constexpr int RHO[24] {
            1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
            27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44,
        };
        static constexpr int PI[24] {
            10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
            15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1,
        };
        static constexpr std::uint64_t RC[24] {
            0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
            0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
            0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
            0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
            0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
            0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
            0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
            0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
        };
        for (int round { 0 }; round < 24; ++round) {
            std::uint64_t bc[5];
            // theta
            for (int i { 0 }; i < 5; ++i) {
                bc[i] = s_[i] ^ s_[i + 5] ^ s_[i + 10] ^ s_[i + 15] ^ s_[i + 20];
            }
            for (int i { 0 }; i < 5; ++i) {
                std::uint64_t t { bc[(i + 4) % 5] ^ std::rotl(bc[(i + 1) % 5], 1) };
                for (int j { 0 }; j < 25; j += 5) { s_[j + i] ^= t; }
            }
            // rho + pi
            std::uint64_t t { s_[1] };
            for (int i { 0 }; i < 24; ++i) {
                int j { PI[i] };
                std::uint64_t tmp { s_[j] };
                s_[j] = std::rotl(t, RHO[i]);
                t = tmp;
            }
            // chi
            for (int j { 0 }; j < 25; j += 5) {
                for (int i { 0 }; i < 5; ++i) { bc[i] = s_[j + i]; }
                for (int i { 0 }; i < 5; ++i) {
                    s_[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
                }
            }
            // iota
            s_[0] ^= RC[round];
        }
    }
};

// ── Fixed-length SHA-3 digests ──────────────────────────────────────────────
// rate = 200 - 2*outLen bytes; suffix 0x06.
inline std::vector<std::uint8_t> sha3(std::size_t outLen, std::span<const std::uint8_t> data) {
    Keccak k { 200 - 2 * outLen, 0x06 };
    k.update(data);
    std::vector<std::uint8_t> out(outLen);
    k.squeeze(out.data(), outLen);
    return out;
}

inline std::vector<std::uint8_t> sha3_224(std::span<const std::uint8_t> d) { return sha3(28, d); }
inline std::vector<std::uint8_t> sha3_256(std::span<const std::uint8_t> d) { return sha3(32, d); }
inline std::vector<std::uint8_t> sha3_384(std::span<const std::uint8_t> d) { return sha3(48, d); }
inline std::vector<std::uint8_t> sha3_512(std::span<const std::uint8_t> d) { return sha3(64, d); }

// ── SHAKE XOFs ──────────────────────────────────────────────────────────────
// SHAKE128 rate 168, SHAKE256 rate 136; suffix 0x1f. outLen is caller-chosen.
inline std::vector<std::uint8_t> shake128(std::span<const std::uint8_t> data, std::size_t outLen) {
    Keccak k { 168, 0x1f };
    k.update(data);
    std::vector<std::uint8_t> out(outLen);
    k.squeeze(out.data(), outLen);
    return out;
}
inline std::vector<std::uint8_t> shake256(std::span<const std::uint8_t> data, std::size_t outLen) {
    Keccak k { 136, 0x1f };
    k.update(data);
    std::vector<std::uint8_t> out(outLen);
    k.squeeze(out.data(), outLen);
    return out;
}

}  // namespace mbun::crypto
