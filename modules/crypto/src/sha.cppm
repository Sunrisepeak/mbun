// mbun.crypto.sha — SHA-1 / SHA-224 / SHA-256 / SHA-384 / SHA-512 / SHA-512-256.
//
// bun's CryptoHasher routes these through BoringSSL (EVP_sha*/SHA*_Init); that
// dependency is unavailable here (per task), so these are self-contained
// streaming FIPS 180-4 implementations with bit-identical output. SHA-3/SHAKE
// live in mbun.crypto.sha3 and BLAKE2 in mbun.crypto.blake2; MD4 and RIPEMD-160
// remain DEFERRED(S2) (bun backs them with BoringSSL).

export module mbun.crypto.sha;

import std;

export namespace mbun::crypto {

// ── SHA-1 ───────────────────────────────────────────────────────────────────
class Sha1 {
public:
    static constexpr std::size_t DIGEST_LENGTH { 20 };

    Sha1() { init(); }
    void init() {
        h_[0] = 0x67452301U;
        h_[1] = 0xefcdab89U;
        h_[2] = 0x98badcfeU;
        h_[3] = 0x10325476U;
        h_[4] = 0xc3d2e1f0U;
        bitLen_ = 0;
        bufLen_ = 0;
    }
    void update(std::span<const std::uint8_t> data) {
        bitLen_ += static_cast<std::uint64_t>(data.size()) * 8;
        std::size_t i { 0 };
        if (bufLen_ > 0) {
            std::size_t take { std::min<std::size_t>(64 - bufLen_, data.size()) };
            std::memcpy(buf_ + bufLen_, data.data(), take);
            bufLen_ += take;
            i += take;
            if (bufLen_ == 64) {
                process_(buf_);
                bufLen_ = 0;
            }
        }
        for (; i + 64 <= data.size(); i += 64) { process_(data.data() + i); }
        if (i < data.size()) {
            std::memcpy(buf_, data.data() + i, data.size() - i);
            bufLen_ = data.size() - i;
        }
    }
    void final_(std::uint8_t out[DIGEST_LENGTH]) {
        std::uint64_t bitLen { bitLen_ };
        std::uint8_t pad { 0x80 };
        update(std::span<const std::uint8_t>(&pad, 1));
        std::uint8_t zero { 0 };
        while (bufLen_ != 56) { update(std::span<const std::uint8_t>(&zero, 1)); }
        std::uint8_t lenBe[8];
        for (int i { 0 }; i < 8; ++i) { lenBe[i] = static_cast<std::uint8_t>(bitLen >> (56 - 8 * i)); }
        update(std::span<const std::uint8_t>(lenBe, 8));
        for (int i { 0 }; i < 5; ++i) {
            out[i * 4 + 0] = static_cast<std::uint8_t>(h_[i] >> 24);
            out[i * 4 + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
            out[i * 4 + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
            out[i * 4 + 3] = static_cast<std::uint8_t>(h_[i]);
        }
    }
    static void hash(std::span<const std::uint8_t> data, std::uint8_t out[DIGEST_LENGTH]) {
        Sha1 h;
        h.update(data);
        h.final_(out);
    }

private:
    std::uint32_t h_[5] {};
    std::uint64_t bitLen_ {};
    std::uint8_t buf_[64] {};
    std::size_t bufLen_ {};

    void process_(const std::uint8_t* block) {
        std::uint32_t w[80];
        for (int i { 0 }; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24)
                 | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16)
                 | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8)
                 | static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i { 16 }; i < 80; ++i) {
            w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        std::uint32_t a { h_[0] }, b { h_[1] }, c { h_[2] }, d { h_[3] }, e { h_[4] };
        for (int i { 0 }; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcU;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6U;
            }
            std::uint32_t t { std::rotl(a, 5) + f + e + k + w[i] };
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = t;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }
};

// ── SHA-256 core (SHA-224 / SHA-256) ────────────────────────────────────────
class Sha2_32 {
public:
    // digestLen: 28 (SHA-224) or 32 (SHA-256). is224 selects init vector.
    explicit Sha2_32(bool is224, std::size_t digestLen) : digestLen_ { digestLen } { init(is224); }
    void init(bool is224) {
        if (is224) {
            h_[0] = 0xc1059ed8U; h_[1] = 0x367cd507U; h_[2] = 0x3070dd17U; h_[3] = 0xf70e5939U;
            h_[4] = 0xffc00b31U; h_[5] = 0x68581511U; h_[6] = 0x64f98fa7U; h_[7] = 0xbefa4fa4U;
        } else {
            h_[0] = 0x6a09e667U; h_[1] = 0xbb67ae85U; h_[2] = 0x3c6ef372U; h_[3] = 0xa54ff53aU;
            h_[4] = 0x510e527fU; h_[5] = 0x9b05688cU; h_[6] = 0x1f83d9abU; h_[7] = 0x5be0cd19U;
        }
        bitLen_ = 0;
        bufLen_ = 0;
    }
    void update(std::span<const std::uint8_t> data) {
        bitLen_ += static_cast<std::uint64_t>(data.size()) * 8;
        std::size_t i { 0 };
        if (bufLen_ > 0) {
            std::size_t take { std::min<std::size_t>(64 - bufLen_, data.size()) };
            std::memcpy(buf_ + bufLen_, data.data(), take);
            bufLen_ += take;
            i += take;
            if (bufLen_ == 64) {
                process_(buf_);
                bufLen_ = 0;
            }
        }
        for (; i + 64 <= data.size(); i += 64) { process_(data.data() + i); }
        if (i < data.size()) {
            std::memcpy(buf_, data.data() + i, data.size() - i);
            bufLen_ = data.size() - i;
        }
    }
    // Writes digestLen_ bytes.
    void final_(std::uint8_t* out) {
        std::uint64_t bitLen { bitLen_ };
        std::uint8_t pad { 0x80 };
        update(std::span<const std::uint8_t>(&pad, 1));
        std::uint8_t zero { 0 };
        while (bufLen_ != 56) { update(std::span<const std::uint8_t>(&zero, 1)); }
        std::uint8_t lenBe[8];
        for (int i { 0 }; i < 8; ++i) { lenBe[i] = static_cast<std::uint8_t>(bitLen >> (56 - 8 * i)); }
        update(std::span<const std::uint8_t>(lenBe, 8));
        std::uint8_t full[32];
        for (int i { 0 }; i < 8; ++i) {
            full[i * 4 + 0] = static_cast<std::uint8_t>(h_[i] >> 24);
            full[i * 4 + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
            full[i * 4 + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
            full[i * 4 + 3] = static_cast<std::uint8_t>(h_[i]);
        }
        std::memcpy(out, full, digestLen_);
    }
    std::size_t digest_length() const { return digestLen_; }

private:
    std::uint32_t h_[8] {};
    std::uint64_t bitLen_ {};
    std::uint8_t buf_[64] {};
    std::size_t bufLen_ {};
    std::size_t digestLen_;

    void process_(const std::uint8_t* block) {
        static constexpr std::uint32_t K[64] {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2,
        };
        std::uint32_t w[64];
        for (int i { 0 }; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24)
                 | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16)
                 | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8)
                 | static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i { 16 }; i < 64; ++i) {
            std::uint32_t s0 { std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3) };
            std::uint32_t s1 { std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10) };
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a { h_[0] }, b { h_[1] }, c { h_[2] }, d { h_[3] };
        std::uint32_t e { h_[4] }, f { h_[5] }, g { h_[6] }, hh { h_[7] };
        for (int i { 0 }; i < 64; ++i) {
            std::uint32_t S1 { std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25) };
            std::uint32_t ch { (e & f) ^ (~e & g) };
            std::uint32_t t1 { hh + S1 + ch + K[i] + w[i] };
            std::uint32_t S0 { std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22) };
            std::uint32_t maj { (a & b) ^ (a & c) ^ (b & c) };
            std::uint32_t t2 { S0 + maj };
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
    }
};

// ── SHA-512 core (SHA-384 / SHA-512 / SHA-512-256) ──────────────────────────
enum class Sha512Variant { S384, S512, S512_224, S512_256 };

class Sha2_64 {
public:
    explicit Sha2_64(Sha512Variant v) { init(v); }
    void init(Sha512Variant v) {
        switch (v) {
        case Sha512Variant::S384:
            h_[0] = 0xcbbb9d5dc1059ed8ULL; h_[1] = 0x629a292a367cd507ULL;
            h_[2] = 0x9159015a3070dd17ULL; h_[3] = 0x152fecd8f70e5939ULL;
            h_[4] = 0x67332667ffc00b31ULL; h_[5] = 0x8eb44a8768581511ULL;
            h_[6] = 0xdb0c2e0d64f98fa7ULL; h_[7] = 0x47b5481dbefa4fa4ULL;
            digestLen_ = 48;
            break;
        case Sha512Variant::S512:
            h_[0] = 0x6a09e667f3bcc908ULL; h_[1] = 0xbb67ae8584caa73bULL;
            h_[2] = 0x3c6ef372fe94f82bULL; h_[3] = 0xa54ff53a5f1d36f1ULL;
            h_[4] = 0x510e527fade682d1ULL; h_[5] = 0x9b05688c2b3e6c1fULL;
            h_[6] = 0x1f83d9abfb41bd6bULL; h_[7] = 0x5be0cd19137e2179ULL;
            digestLen_ = 64;
            break;
        case Sha512Variant::S512_224:
            h_[0] = 0x8c3d37c819544da2ULL; h_[1] = 0x73e1996689dcd4d6ULL;
            h_[2] = 0x1dfab7ae32ff9c82ULL; h_[3] = 0x679dd514582f9fcfULL;
            h_[4] = 0x0f6d2b697bd44da8ULL; h_[5] = 0x77e36f7304c48942ULL;
            h_[6] = 0x3f9d85a86a1d36c8ULL; h_[7] = 0x1112e6ad91d692a1ULL;
            digestLen_ = 28;
            break;
        case Sha512Variant::S512_256:
            h_[0] = 0x22312194fc2bf72cULL; h_[1] = 0x9f555fa3c84c64c2ULL;
            h_[2] = 0x2393b86b6f53b151ULL; h_[3] = 0x963877195940eabdULL;
            h_[4] = 0x96283ee2a88effe3ULL; h_[5] = 0xbe5e1e2553863992ULL;
            h_[6] = 0x2b0199fc2c85b8aaULL; h_[7] = 0x0eb72ddc81c52ca2ULL;
            digestLen_ = 32;
            break;
        }
        bitLenLo_ = 0;
        bitLenHi_ = 0;
        bufLen_ = 0;
    }
    void update(std::span<const std::uint8_t> data) {
        std::uint64_t add { static_cast<std::uint64_t>(data.size()) * 8 };
        if (bitLenLo_ + add < bitLenLo_) { ++bitLenHi_; }
        bitLenLo_ += add;
        std::size_t i { 0 };
        if (bufLen_ > 0) {
            std::size_t take { std::min<std::size_t>(128 - bufLen_, data.size()) };
            std::memcpy(buf_ + bufLen_, data.data(), take);
            bufLen_ += take;
            i += take;
            if (bufLen_ == 128) {
                process_(buf_);
                bufLen_ = 0;
            }
        }
        for (; i + 128 <= data.size(); i += 128) { process_(data.data() + i); }
        if (i < data.size()) {
            std::memcpy(buf_, data.data() + i, data.size() - i);
            bufLen_ = data.size() - i;
        }
    }
    void final_(std::uint8_t* out) {
        std::uint64_t lo { bitLenLo_ };
        std::uint64_t hi { bitLenHi_ };
        std::uint8_t pad { 0x80 };
        update(std::span<const std::uint8_t>(&pad, 1));
        std::uint8_t zero { 0 };
        while (bufLen_ != 112) { update(std::span<const std::uint8_t>(&zero, 1)); }
        std::uint8_t lenBe[16];
        for (int i { 0 }; i < 8; ++i) { lenBe[i] = static_cast<std::uint8_t>(hi >> (56 - 8 * i)); }
        for (int i { 0 }; i < 8; ++i) { lenBe[8 + i] = static_cast<std::uint8_t>(lo >> (56 - 8 * i)); }
        update(std::span<const std::uint8_t>(lenBe, 16));
        std::uint8_t full[64];
        for (int i { 0 }; i < 8; ++i) {
            for (int j { 0 }; j < 8; ++j) {
                full[i * 8 + j] = static_cast<std::uint8_t>(h_[i] >> (56 - 8 * j));
            }
        }
        std::memcpy(out, full, digestLen_);
    }
    std::size_t digest_length() const { return digestLen_; }

private:
    std::uint64_t h_[8] {};
    std::uint64_t bitLenLo_ {};
    std::uint64_t bitLenHi_ {};
    std::uint8_t buf_[128] {};
    std::size_t bufLen_ {};
    std::size_t digestLen_ {};

    void process_(const std::uint8_t* block) {
        static constexpr std::uint64_t K[80] {
            0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
            0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
            0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
            0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
            0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
            0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
            0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
            0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
            0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
            0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
            0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
            0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
            0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
            0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
            0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
            0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
            0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
            0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
            0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
            0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
        };
        std::uint64_t w[80];
        for (int i { 0 }; i < 16; ++i) {
            std::uint64_t v { 0 };
            for (int j { 0 }; j < 8; ++j) {
                v = (v << 8) | static_cast<std::uint64_t>(block[i * 8 + j]);
            }
            w[i] = v;
        }
        for (int i { 16 }; i < 80; ++i) {
            std::uint64_t s0 { std::rotr(w[i - 15], 1) ^ std::rotr(w[i - 15], 8) ^ (w[i - 15] >> 7) };
            std::uint64_t s1 { std::rotr(w[i - 2], 19) ^ std::rotr(w[i - 2], 61) ^ (w[i - 2] >> 6) };
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint64_t a { h_[0] }, b { h_[1] }, c { h_[2] }, d { h_[3] };
        std::uint64_t e { h_[4] }, f { h_[5] }, g { h_[6] }, hh { h_[7] };
        for (int i { 0 }; i < 80; ++i) {
            std::uint64_t S1 { std::rotr(e, 14) ^ std::rotr(e, 18) ^ std::rotr(e, 41) };
            std::uint64_t ch { (e & f) ^ (~e & g) };
            std::uint64_t t1 { hh + S1 + ch + K[i] + w[i] };
            std::uint64_t S0 { std::rotr(a, 28) ^ std::rotr(a, 34) ^ std::rotr(a, 39) };
            std::uint64_t maj { (a & b) ^ (a & c) ^ (b & c) };
            std::uint64_t t2 { S0 + maj };
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
    }
};

}  // namespace mbun::crypto
