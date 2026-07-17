// mbun.crypto.md4 — MD4 (RFC 1320), pure C++ reference.
//
// bun's CryptoHasher routes md4 through BoringSSL's EVP_md4; boringssl is not
// available here (per task), so this is a self-contained streaming MD4 with
// bit-identical output. update()/final digest into a 16-byte buffer.
//
// Known-answer: md4("")    = 31d6cfe0d16ae931b73c59d7e0c089c0,
//               md4("abc") = a448017aaf21d8525fc10ae87aa6729d.

export module mbun.crypto.md4;

import std;

export namespace mbun::crypto {

class Md4 {
public:
    static constexpr std::size_t DIGEST_LENGTH { 16 };

    Md4() { init(); }

    void init() {
        a_ = 0x67452301U;
        b_ = 0xefcdab89U;
        c_ = 0x98badcfeU;
        d_ = 0x10325476U;
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

    void final_(std::uint8_t* out) {
        std::uint64_t bits { bitLen_ };
        std::uint8_t pad { 0x80 };
        update(std::span<const std::uint8_t>(&pad, 1));
        std::uint8_t zero { 0 };
        while (bufLen_ != 56) { update(std::span<const std::uint8_t>(&zero, 1)); }
        std::uint8_t lenLe[8];
        for (int i { 0 }; i < 8; ++i) { lenLe[i] = static_cast<std::uint8_t>(bits >> (8 * i)); }
        update(std::span<const std::uint8_t>(lenLe, 8));
        std::uint32_t st[4] { a_, b_, c_, d_ };
        for (int i { 0 }; i < 4; ++i) {
            out[i * 4 + 0] = static_cast<std::uint8_t>(st[i]);
            out[i * 4 + 1] = static_cast<std::uint8_t>(st[i] >> 8);
            out[i * 4 + 2] = static_cast<std::uint8_t>(st[i] >> 16);
            out[i * 4 + 3] = static_cast<std::uint8_t>(st[i] >> 24);
        }
    }

    static void hash(std::span<const std::uint8_t> data, std::uint8_t* out) {
        Md4 h;
        h.update(data);
        h.final_(out);
    }

private:
    std::uint32_t a_ {}, b_ {}, c_ {}, d_ {};
    std::uint64_t bitLen_ {};
    std::uint8_t buf_[64] {};
    std::size_t bufLen_ {};

    static std::uint32_t rotl_(std::uint32_t x, int s) {
        return (x << s) | (x >> (32 - s));
    }

    void process_(const std::uint8_t* block) {
        std::uint32_t x[16];
        for (int i { 0 }; i < 16; ++i) {
            x[i] = static_cast<std::uint32_t>(block[i * 4 + 0]) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
        }
        std::uint32_t a { a_ }, b { b_ }, c { c_ }, d { d_ };

        auto F = [](std::uint32_t xx, std::uint32_t yy, std::uint32_t zz) {
            return (xx & yy) | (~xx & zz);
        };
        auto G = [](std::uint32_t xx, std::uint32_t yy, std::uint32_t zz) {
            return (xx & yy) | (xx & zz) | (yy & zz);
        };
        auto H = [](std::uint32_t xx, std::uint32_t yy, std::uint32_t zz) {
            return xx ^ yy ^ zz;
        };

        // Round 1
        static constexpr int s1[4] { 3, 7, 11, 19 };
        for (int i { 0 }; i < 16; ++i) {
            int k { i };
            a = rotl_(a + F(b, c, d) + x[k], s1[i & 3]);
            std::uint32_t t { d }; d = c; c = b; b = a; a = t;
        }
        // Round 2
        static constexpr int k2[16] { 0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15 };
        static constexpr int s2[4] { 3, 5, 9, 13 };
        for (int i { 0 }; i < 16; ++i) {
            a = rotl_(a + G(b, c, d) + x[k2[i]] + 0x5a827999U, s2[i & 3]);
            std::uint32_t t { d }; d = c; c = b; b = a; a = t;
        }
        // Round 3
        static constexpr int k3[16] { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
        static constexpr int s3[4] { 3, 9, 11, 15 };
        for (int i { 0 }; i < 16; ++i) {
            a = rotl_(a + H(b, c, d) + x[k3[i]] + 0x6ed9eba1U, s3[i & 3]);
            std::uint32_t t { d }; d = c; c = b; b = a; a = t;
        }

        a_ += a; b_ += b; c_ += c; d_ += d;
    }
};

// One-shot: MD4 over `data` → 16-byte digest.
inline std::vector<std::uint8_t> md4(std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> out(Md4::DIGEST_LENGTH);
    Md4::hash(data, out.data());
    return out;
}

}  // namespace mbun::crypto
