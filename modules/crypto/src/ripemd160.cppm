// mbun.crypto.ripemd160 — RIPEMD-160, pure C++ reference.
//
// bun/node route ripemd160 through BoringSSL's EVP_ripemd160; boringssl is not
// available here, so this is a self-contained streaming RIPEMD-160 with
// bit-identical output. update()/final digest into a 20-byte buffer.
//
// spec: H. Dobbertin, A. Bosselaers, B. Preneel, "RIPEMD-160: A Strengthened
// Version of RIPEMD" (Fast Software Encryption 1996). 160-bit digest, 512-bit
// blocks, 80 steps over two parallel lines (left/right). Little-endian message
// words and length encoding, MD-style padding (identical framing to MD5).
// Known-answer: ripemd160("")    = 9c1185a5c5e9fc54612808977ee8f548b2258d31,
//               ripemd160("abc") = 8eb208f7e05d987a9b044a8e98c6b087f15a0bfc.

export module mbun.crypto.ripemd160;

import std;

export namespace mbun::crypto {

class Ripemd160 {
public:
    static constexpr std::size_t DIGEST_LENGTH { 20 };

    Ripemd160() { init(); }

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
            std::size_t need { 64 - bufLen_ };
            std::size_t take { std::min(need, data.size()) };
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
        std::uint8_t lenLe[8];
        for (int i { 0 }; i < 8; ++i) { lenLe[i] = static_cast<std::uint8_t>(bitLen >> (8 * i)); }
        update(std::span<const std::uint8_t>(lenLe, 8));
        // State now holds the digest; emit each word little-endian.
        for (int i { 0 }; i < 5; ++i) { store_(out, static_cast<std::size_t>(i) * 4, h_[i]); }
    }

    static void hash(std::span<const std::uint8_t> data, std::uint8_t out[DIGEST_LENGTH]) {
        Ripemd160 h;
        h.update(data);
        h.final_(out);
    }

private:
    std::uint32_t h_[5] {};
    std::uint64_t bitLen_ {};
    std::uint8_t buf_[64] {};
    std::size_t bufLen_ {};

    static void store_(std::uint8_t* out, std::size_t off, std::uint32_t v) {
        out[off + 0] = static_cast<std::uint8_t>(v);
        out[off + 1] = static_cast<std::uint8_t>(v >> 8);
        out[off + 2] = static_cast<std::uint8_t>(v >> 16);
        out[off + 3] = static_cast<std::uint8_t>(v >> 24);
    }
    static std::uint32_t load_(const std::uint8_t* p) {
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
             | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    }

    // Nonlinear function for step j (0..79), selected by round (j / 16).
    static std::uint32_t f_(int round, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        switch (round) {
        case 0: return x ^ y ^ z;
        case 1: return (x & y) | (~x & z);
        case 2: return (x | ~y) ^ z;
        case 3: return (x & z) | (y & ~z);
        default: return x ^ (y | ~z);  // case 4
        }
    }

    void process_(const std::uint8_t* block) {
        // Per-step message-word index (r) and rotate amount (s), left line.
        static constexpr int RL[80] {
            0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
            7, 4, 13, 1, 10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8,
            3, 10, 14, 4, 9, 15, 8, 1, 2, 7, 0, 6, 13, 11, 5, 12,
            1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15, 14, 5, 6, 2,
            4, 0, 5, 9, 7, 12, 2, 10, 14, 1, 3, 8, 11, 6, 15, 13,
        };
        static constexpr int RR[80] {
            5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12,
            6, 11, 3, 7, 0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2,
            15, 5, 1, 3, 7, 14, 6, 9, 11, 8, 12, 2, 10, 0, 4, 13,
            8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13, 9, 7, 10, 14,
            12, 15, 10, 4, 1, 5, 8, 7, 6, 2, 13, 14, 0, 3, 9, 11,
        };
        static constexpr int SL[80] {
            11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8,
            7, 6, 8, 13, 11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12,
            11, 13, 6, 7, 14, 9, 13, 15, 14, 8, 13, 6, 5, 12, 7, 5,
            11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6, 8, 6, 5, 12,
            9, 15, 5, 11, 6, 8, 13, 12, 5, 12, 13, 14, 11, 8, 5, 6,
        };
        static constexpr int SR[80] {
            8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6,
            9, 13, 15, 7, 12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11,
            9, 7, 15, 11, 8, 6, 6, 14, 12, 13, 5, 14, 13, 13, 7, 5,
            15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9, 12, 5, 15, 8,
            8, 5, 12, 9, 12, 5, 14, 6, 8, 13, 6, 5, 15, 13, 11, 11,
        };
        // Per-round additive constants (indexed by j/16). K[0] and K'[4] are 0.
        static constexpr std::uint32_t KL[5] {
            0x00000000U, 0x5a827999U, 0x6ed9eba1U, 0x8f1bbcdcU, 0xa953fd4eU };
        static constexpr std::uint32_t KR[5] {
            0x50a28be6U, 0x5c4dd124U, 0x6d703ef3U, 0x7a6d76e9U, 0x00000000U };

        std::uint32_t x[16];
        for (int i { 0 }; i < 16; ++i) { x[i] = load_(block + i * 4); }

        std::uint32_t al { h_[0] }, bl { h_[1] }, cl { h_[2] }, dl { h_[3] }, el { h_[4] };
        std::uint32_t ar { h_[0] }, br { h_[1] }, cr { h_[2] }, dr { h_[3] }, er { h_[4] };

        for (int j { 0 }; j < 80; ++j) {
            int roundL { j >> 4 };
            std::uint32_t t {
                std::rotl(al + f_(roundL, bl, cl, dl) + x[RL[j]] + KL[roundL],
                          SL[j]) + el };
            al = el;
            el = dl;
            dl = std::rotl(cl, 10);
            cl = bl;
            bl = t;

            int roundR { 4 - roundL };  // right line uses f_(79-j) = f_(4 - j/16)
            std::uint32_t tr {
                std::rotl(ar + f_(roundR, br, cr, dr) + x[RR[j]] + KR[roundL],
                          SR[j]) + er };
            ar = er;
            er = dr;
            dr = std::rotl(cr, 10);
            cr = br;
            br = tr;
        }

        std::uint32_t t { h_[1] + cl + dr };
        h_[1] = h_[2] + dl + er;
        h_[2] = h_[3] + el + ar;
        h_[3] = h_[4] + al + br;
        h_[4] = h_[0] + bl + cr;
        h_[0] = t;
    }
};

// One-shot: RIPEMD-160 over `data` → 20-byte digest.
inline std::vector<std::uint8_t> ripemd160(std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out(Ripemd160::DIGEST_LENGTH);
    Ripemd160::hash(data, out.data());
    return out;
}

}  // namespace mbun::crypto
