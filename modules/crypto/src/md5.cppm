// mbun.crypto.md5 — MD5 (RFC 1321), pure C++ reference.
//
// bun's CryptoHasher routes md5 through BoringSSL's EVP_md5; boringssl is not
// available here (per task), so this is a self-contained streaming MD5 with
// bit-identical output. update()/final digest into a 16-byte buffer.

export module mbun.crypto.md5;

import std;

export namespace mbun::crypto {

class Md5 {
public:
    static constexpr std::size_t DIGEST_LENGTH { 16 };

    Md5() { init(); }

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
        // Now state holds the digest; emit little-endian.
        store_(out, 0, a_);
        store_(out, 4, b_);
        store_(out, 8, c_);
        store_(out, 12, d_);
    }

    static void hash(std::span<const std::uint8_t> data, std::uint8_t out[DIGEST_LENGTH]) {
        Md5 h;
        h.update(data);
        h.final_(out);
    }

private:
    std::uint32_t a_ {}, b_ {}, c_ {}, d_ {};
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

    void process_(const std::uint8_t* block) {
        static constexpr std::uint32_t K[64] {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
            0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
            0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
            0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
            0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
            0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
            0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
            0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
            0xeb86d391,
        };
        static constexpr std::uint32_t S[64] {
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
            5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
        };
        std::uint32_t m[16];
        for (int i { 0 }; i < 16; ++i) { m[i] = load_(block + i * 4); }

        std::uint32_t a { a_ }, b { b_ }, c { c_ }, d { d_ };
        for (int i { 0 }; i < 64; ++i) {
            std::uint32_t f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) & 15;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) & 15;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) & 15;
            }
            f = f + a + K[i] + m[g];
            a = d;
            d = c;
            c = b;
            b = b + std::rotl(f, static_cast<int>(S[i]));
        }
        a_ += a;
        b_ += b;
        c_ += c;
        d_ += d;
    }
};

}  // namespace mbun::crypto
