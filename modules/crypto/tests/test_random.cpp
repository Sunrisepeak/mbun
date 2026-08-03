// test_random.cpp — mbun.crypto.random known-answer + sanity checks.
//
// The ChaCha20 block KAT is RFC 8439 §2.3.2 (key 00..1f, counter 1, nonce
// 00000009 0000004a 00000000). It proves the keystream core; fill_random then
// only has to wire fresh entropy into it, which the sanity checks cover.

import std;
import mbun.crypto;

namespace {

int g_failed { 0 };

std::string hex(std::span<const std::uint8_t> b) {
    std::string s;
    for (auto v : b) { s += std::format("{:02x}", v); }
    return s;
}

}  // namespace

int main() {
    // --- RFC 8439 §2.3.2 keystream block ---
    std::uint32_t key[8];
    for (int i { 0 }; i < 8; ++i) {
        key[i] = static_cast<std::uint32_t>(i * 4) | (static_cast<std::uint32_t>(i * 4 + 1) << 8)
                 | (static_cast<std::uint32_t>(i * 4 + 2) << 16)
                 | (static_cast<std::uint32_t>(i * 4 + 3) << 24);
    }
    std::uint32_t nonce[3] { 0x09000000u, 0x4a000000u, 0x00000000u };
    std::uint8_t block[64];
    mbun::crypto::chacha20_block(key, 1, nonce, block);
    const std::string want {
        "10f1e7e4d13b5915500fdd1fa32071c4"
        "c7d1f4c733c06803"
        "0422aa9ac3d46c4e"
        "d2826446079faa0914c2d705d98b02a2"
        "b5129cd1de164eb9cbd083e8a2503c4e"
    };
    const std::string got { hex(block) };
    if (got != want) {
        ++g_failed;
        std::println("FAIL chacha20_block: got {} want {}", got, want);
    }

    // --- fill_random sanity ---
    // Two large draws must differ, and neither may be all-zero (a silent
    // no-op fill is the failure mode that matters).
    std::vector<std::uint8_t> a(4096), b(4096);
    mbun::crypto::fill_random(a);
    mbun::crypto::fill_random(b);
    if (a == b) {
        ++g_failed;
        std::println("FAIL fill_random: two 4096-byte draws are identical");
    }
    if (std::ranges::all_of(a, [](std::uint8_t v) { return v == 0; })) {
        ++g_failed;
        std::println("FAIL fill_random: draw is all zero");
    }
    // Byte-value coverage: 4096 uniform bytes hit far more than 128 of the 256
    // possible values with overwhelming probability (expected ~250).
    std::array<bool, 256> seen {};
    for (auto v : a) { seen[v] = true; }
    const auto distinct { std::ranges::count(seen, true) };
    if (distinct < 128) {
        ++g_failed;
        std::println("FAIL fill_random: only {} distinct byte values in 4096 bytes", distinct);
    }

    // Small path (<= 32 bytes) takes a different branch; check it too.
    std::vector<std::uint8_t> s1(16), s2(16);
    mbun::crypto::fill_random(s1);
    mbun::crypto::fill_random(s2);
    if (s1 == s2) {
        ++g_failed;
        std::println("FAIL fill_random: two 16-byte draws are identical");
    }

    // Exact-size fills must not touch neighbouring bytes.
    std::vector<std::uint8_t> guard(200, 0xAAu);
    mbun::crypto::fill_random(std::span { guard.data() + 50, 100 });
    for (std::size_t i { 0 }; i < 50; ++i) {
        if (guard[i] != 0xAAu || guard[150 + i] != 0xAAu) {
            ++g_failed;
            std::println("FAIL fill_random: wrote outside the requested span");
            break;
        }
    }

    if (g_failed == 0) { std::println("test_random: all checks passed"); }
    return g_failed == 0 ? 0 : 1;
}
