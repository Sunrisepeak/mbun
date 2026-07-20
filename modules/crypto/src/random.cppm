// random.cppm — mbun.crypto.random: bulk cryptographic random bytes.
//
// WHY THIS EXISTS: `std::random_device` yields only 32 bits per call and, on
// libstdc++/Linux, each call is an OS entropy read. Filling a 1 MB buffer that
// way costs ~250 000 reads (~450 ms measured), which made
// `crypto.getRandomValues(new Uint8Array(1e6))` the dominant cost of
// test/js/web/fetch/body.test.ts.
//
// FIX: draw a fresh 256-bit key + 96-bit nonce from `std::random_device` per
// call (11 draws, constant) and expand it with ChaCha20 (RFC 8439). This is the
// same construction arc4random / getrandom(2) use internally: a CSPRNG seeded
// from OS entropy. Because the key is per-call there is no long-lived state to
// leak across a fork() and no reseed policy to get wrong.
//
// ref: RFC 8439 (ChaCha20 and Poly1305 for IETF Protocols), §2.3.
export module mbun.crypto.random;

import std;

namespace mbun::crypto {

namespace detail {

constexpr std::uint32_t rotl32(std::uint32_t x, int n) {
    return static_cast<std::uint32_t>((x << n) | (x >> (32 - n)));
}

constexpr void quarter_round(std::uint32_t* s, int a, int b, int c, int d) {
    s[a] += s[b];
    s[d] = rotl32(s[d] ^ s[a], 16);
    s[c] += s[d];
    s[b] = rotl32(s[b] ^ s[c], 12);
    s[a] += s[b];
    s[d] = rotl32(s[d] ^ s[a], 8);
    s[c] += s[d];
    s[b] = rotl32(s[b] ^ s[c], 7);
}

}  // namespace detail

// One ChaCha20 block: 64 keystream bytes for `key` (8 words, little-endian),
// `counter`, and `nonce` (3 words). `out` must have room for 64 bytes.
export inline void chacha20_block(const std::uint32_t (&key)[8], std::uint32_t counter,
                                  const std::uint32_t (&nonce)[3], std::uint8_t* out) {
    std::uint32_t state[16] {
        0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u,
        key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
        counter, nonce[0], nonce[1], nonce[2],
    };
    std::uint32_t w[16];
    std::copy_n(state, 16, w);
    for (int i { 0 }; i < 10; ++i) {
        detail::quarter_round(w, 0, 4, 8, 12);
        detail::quarter_round(w, 1, 5, 9, 13);
        detail::quarter_round(w, 2, 6, 10, 14);
        detail::quarter_round(w, 3, 7, 11, 15);
        detail::quarter_round(w, 0, 5, 10, 15);
        detail::quarter_round(w, 1, 6, 11, 12);
        detail::quarter_round(w, 2, 7, 8, 13);
        detail::quarter_round(w, 3, 4, 9, 14);
    }
    for (int i { 0 }; i < 16; ++i) {
        const std::uint32_t v { w[i] + state[i] };
        out[i * 4 + 0] = static_cast<std::uint8_t>(v);
        out[i * 4 + 1] = static_cast<std::uint8_t>(v >> 8);
        out[i * 4 + 2] = static_cast<std::uint8_t>(v >> 16);
        out[i * 4 + 3] = static_cast<std::uint8_t>(v >> 24);
    }
}

// Fill `out` with cryptographically strong random bytes.
export inline void fill_random(std::span<std::uint8_t> out) {
    if (out.empty()) { return; }
    static thread_local std::random_device rd;

    // Small requests: the ChaCha20 setup would cost more entropy draws than the
    // request itself, so take them straight from the OS source.
    if (out.size() <= 32) {
        std::size_t i { 0 };
        while (i < out.size()) {
            const std::uint32_t r { rd() };
            for (int j { 0 }; j < 4 && i < out.size(); ++j, ++i) {
                out[i] = static_cast<std::uint8_t>(r >> (8 * j));
            }
        }
        return;
    }

    std::uint32_t key[8];
    std::uint32_t nonce[3];
    for (auto& k : key) { k = rd(); }
    for (auto& n : nonce) { n = rd(); }

    std::uint8_t block[64];
    std::uint32_t counter { 0 };
    std::size_t i { 0 };
    while (i + 64 <= out.size()) {
        chacha20_block(key, counter++, nonce, out.data() + i);
        i += 64;
    }
    if (i < out.size()) {
        chacha20_block(key, counter, nonce, block);
        std::copy_n(block, out.size() - i, out.data() + i);
    }
    // The key never outlives the call.
    std::fill_n(key, 8, 0u);
    std::fill_n(block, 64, static_cast<std::uint8_t>(0));
}

// Convenience overload for raw pointer + length call sites.
export inline void fill_random(std::uint8_t* p, std::size_t n) { fill_random(std::span { p, n }); }

}  // namespace mbun::crypto
