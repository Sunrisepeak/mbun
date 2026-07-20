// test_crypto.cpp — mbun.crypto vectors.
//
// Digest vectors: standard FIPS/RFC known-answers ("abc", empty).
// Non-crypto vectors: extracted verbatim from bun's Rust reference unit tests
//   src/hash/adler32.rs, rapidhash.rs, murmur.rs (smhasher), cityhash.rs
//   (smhasher), src/wyhash/lib.rs (wangyi-fudan test_vector.cpp).
// xxHash32/64 vectors from the xxHash reference (seed 0, "" and "abc").

import std;
import mbun.crypto;

namespace {

using namespace mbun::crypto;

int g_failed { 0 };

std::span<const std::uint8_t> bytes(std::string_view s) {
    return { reinterpret_cast<const std::uint8_t*>(s.data()), s.size() };
}

template <typename T>
void check(std::string_view name, T got, T want) {
    if (got != want) {
        ++g_failed;
        std::println("FAIL {}: got {:#x} want {:#x}", name, static_cast<std::uint64_t>(got),
                     static_cast<std::uint64_t>(want));
    }
}

void check_hex(std::string_view name, std::string got, std::string_view want) {
    if (got != want) {
        ++g_failed;
        std::println("FAIL {}: got {} want {}", name, got, want);
    }
}

// SMHasher verification codes (see src/hash/lib.rs::verify).
std::uint32_t smhasher32(auto hash_fn) {
    std::uint8_t buf[256];
    std::uint8_t all[256 * 4];
    for (std::uint32_t i { 0 }; i < 256; ++i) {
        buf[i] = static_cast<std::uint8_t>(i);
        std::uint32_t h { hash_fn(std::span<const std::uint8_t>(buf, i), 256 - i) };
        std::memcpy(all + i * 4, &h, 4);  // little-endian host assumed (x86)
    }
    return hash_fn(std::span<const std::uint8_t>(all, sizeof all), 0);
}
std::uint32_t smhasher64(auto hash_fn) {
    std::uint8_t buf[256];
    std::uint8_t all[256 * 8];
    for (std::uint64_t i { 0 }; i < 256; ++i) {
        buf[i] = static_cast<std::uint8_t>(i);
        std::uint64_t h { hash_fn(std::span<const std::uint8_t>(buf, i), 256 - i) };
        std::memcpy(all + i * 8, &h, 8);
    }
    return static_cast<std::uint32_t>(hash_fn(std::span<const std::uint8_t>(all, sizeof all), 0));
}

}  // namespace

int main() {
    // ── Digests ──
    check_hex("md5(abc)", to_hex(*CryptoHasher::hash("md5", bytes("abc"))),
              "900150983cd24fb0d6963f7d28e17f72");
    check_hex("md5()", to_hex(*CryptoHasher::hash("md5", bytes(""))),
              "d41d8cd98f00b204e9800998ecf8427e");
    check_hex("sha1(abc)", to_hex(*CryptoHasher::hash("sha1", bytes("abc"))),
              "a9993e364706816aba3e25717850c26c9cd0d89d");
    check_hex("sha224(abc)", to_hex(*CryptoHasher::hash("sha224", bytes("abc"))),
              "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7");
    check_hex("sha256(abc)", to_hex(*CryptoHasher::hash("sha256", bytes("abc"))),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_hex("sha256()", to_hex(*CryptoHasher::hash("sha256", bytes(""))),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_hex("sha384(abc)", to_hex(*CryptoHasher::hash("sha384", bytes("abc"))),
              "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
              "8086072ba1e7cc2358baeca134c825a7");
    check_hex(
        "sha512(abc)", to_hex(*CryptoHasher::hash("sha512", bytes("abc"))),
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");

    // ── HMAC (RFC 4231 test case 1) ──
    {
        const std::array<std::uint8_t, 20> key {
            0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
            0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        };
        const auto sha256Mac { hmac(DigestAlgo::Sha256, key, bytes("Hi There")) };
        const auto sha512Mac { hmac(DigestAlgo::Sha512, key, bytes("Hi There")) };
        check_hex("hmac-sha256 rfc4231", sha256Mac ? to_hex(*sha256Mac) : "error",
                  "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
        check_hex(
            "hmac-sha512 rfc4231", sha512Mac ? to_hex(*sha512Mac) : "error",
            "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
            "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854");

        if (!sha256Mac || !constant_time_equal(*sha256Mac, *sha256Mac)) {
            ++g_failed;
            std::println("FAIL hmac constant-time equality accepted equal values");
        }
        if (sha256Mac && sha512Mac && constant_time_equal(*sha256Mac, *sha512Mac)) {
            ++g_failed;
            std::println("FAIL hmac constant-time equality accepted different values");
        }
    }
    check_hex("sha512-256(abc)", to_hex(*CryptoHasher::hash("sha512-256", bytes("abc"))),
              "53048e2681941ef99b2e29b76b4c7dabe4c2d0c634fc6d46e0e2f13107e7af23");

    // Streaming + copy
    {
        CryptoHasher h { DigestAlgo::Sha256 };
        h.update(bytes("a"));
        h.update(bytes("b"));
        CryptoHasher snap { h.copy() };
        h.update(bytes("c"));
        check_hex("sha256 streaming abc", h.digest(Encoding::Hex),
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        check_hex("sha256 copy ab", snap.digest(Encoding::Hex),
                  "fb8e20fc2e4c3f248c60c39bd652f3c1347298bb977b8b4d5903b85055620603");
    }

    // MD4 (RFC 1320 appendix A.5) / RIPEMD-160 (Dobbertin et al. test suite),
    // one-shot and multi-part — the streaming classes back Bun.CryptoHasher's
    // "md4"/"ripemd160" algorithms, so a split update must equal the one-shot.
    check_hex("md4()", to_hex(md4(bytes(""))), "31d6cfe0d16ae931b73c59d7e0c089c0");
    check_hex("md4(abc)", to_hex(md4(bytes("abc"))), "a448017aaf21d8525fc10ae87aa6729d");
    check_hex("md4(message digest)", to_hex(md4(bytes("message digest"))),
              "d9130a8164549fe818874806e1c7014b");
    {
        Md4 h;
        h.update(bytes("mess"));
        h.update(bytes("age dig"));
        h.update(bytes("est"));
        std::vector<std::uint8_t> out(Md4::DIGEST_LENGTH);
        h.final_(out.data());
        check_hex("md4 multi-part", to_hex(out), "d9130a8164549fe818874806e1c7014b");
    }
    check_hex("ripemd160()", to_hex(ripemd160(bytes(""))),
              "9c1185a5c5e9fc54612808977ee8f548b2258d31");
    check_hex("ripemd160(abc)", to_hex(ripemd160(bytes("abc"))),
              "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc");
    check_hex("ripemd160(message digest)", to_hex(ripemd160(bytes("message digest"))),
              "5d0689ef49d2fae572b881b123a85ffa21595f36");
    {
        Ripemd160 h;
        h.update(bytes("mess"));
        h.update(bytes("age dig"));
        h.update(bytes("est"));
        std::vector<std::uint8_t> out(Ripemd160::DIGEST_LENGTH);
        h.final_(out.data());
        check_hex("ripemd160 multi-part", to_hex(out),
                  "5d0689ef49d2fae572b881b123a85ffa21595f36");
    }

    // base64 digest of sha256("")
    check_hex("sha256 base64 empty", CryptoHasher::hash("sha256", bytes("")).and_then([](auto v) {
                  return std::optional<std::string>(to_base64(v));
              }).value(),
              "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU=");

    // ── wyhash (wangyi-fudan vectors) ──
    check<std::uint64_t>("wyhash('' seed0)", Wyhash::hash(0, bytes("")), 0x0409638ee2bde459ULL);
    check<std::uint64_t>("wyhash('a' seed1)", Wyhash::hash(1, bytes("a")), 0xa8412d091b5fe0a9ULL);
    check<std::uint64_t>("wyhash('abc' seed2)", Wyhash::hash(2, bytes("abc")),
                         0x32dd92e4b2915153ULL);
    check<std::uint64_t>(
        "wyhash(long seed6)",
        Wyhash::hash(6, bytes("12345678901234567890123456789012345678901234567890"
                              "123456789012345678901234567890")),
        0xc39cab13b115aad3ULL);
    // streaming == one-shot
    {
        Wyhash w { Wyhash::init(0) };
        w.update(bytes("ab"));
        w.update(bytes("c"));
        check<std::uint64_t>("wyhash streaming abc", w.final_(), Wyhash::hash(0, bytes("abc")));
    }

    // ── adler32 (src/hash/adler32.rs) ──
    check<std::uint32_t>("adler32(a)", Adler32::hash(bytes("a")), 0x620062);
    check<std::uint32_t>("adler32(example)", Adler32::hash(bytes("example")), 0xbc002ed);

    // ── rapidhash (src/hash/rapidhash.rs vectors) ──
    {
        std::uint8_t b[1024];
        for (std::size_t i { 0 }; i < 1024; ++i) { b[i] = "abcdefgh"[i % 8]; }
        check<std::uint64_t>("rapidhash(0)", RapidHash::hash(RapidHash::RAPID_SEED, { b, 0 }),
                             0x5a6ef77074ebc84bULL);
        check<std::uint64_t>("rapidhash(16)", RapidHash::hash(RapidHash::RAPID_SEED, { b, 16 }),
                             0xed56d62eead1e402ULL);
        check<std::uint64_t>("rapidhash(1024)", RapidHash::hash(RapidHash::RAPID_SEED, { b, 1024 }),
                             0x4b575f5bf25600d6ULL);
    }

    // ── SMHasher verification codes ──
    check<std::uint32_t>("murmur2_32 smhasher",
                         smhasher32([](std::span<const std::uint8_t> b, std::uint32_t s) {
                             return Murmur2_32::hash_with_seed(b, s);
                         }),
                         0x27864C1E);
    check<std::uint32_t>("murmur3_32 smhasher",
                         smhasher32([](std::span<const std::uint8_t> b, std::uint32_t s) {
                             return Murmur3_32::hash_with_seed(b, s);
                         }),
                         0xB0F57EE3);
    check<std::uint32_t>("murmur2_64 smhasher",
                         smhasher64([](std::span<const std::uint8_t> b, std::uint64_t s) {
                             return Murmur2_64::hash_with_seed(b, s);
                         }),
                         0x1F0D3804);
    check<std::uint32_t>("cityHash32 smhasher",
                         smhasher32([](std::span<const std::uint8_t> b, std::uint32_t) {
                             return CityHash32::hash(b);
                         }),
                         0x68254F81);
    check<std::uint32_t>("cityHash64 smhasher",
                         smhasher64([](std::span<const std::uint8_t> b, std::uint64_t s) {
                             return CityHash64::hash_with_seed(b, s);
                         }),
                         0x5FABC5C5);
    check<std::uint32_t>("wyhash smhasher",
                         smhasher64([](std::span<const std::uint8_t> b, std::uint64_t s) {
                             return Wyhash::hash(s, b);
                         }),
                         0xBD5E840C);

    // ── xxHash (reference, seed 0) ──
    check<std::uint32_t>("xxhash32('')", XxHash32::hash(0, bytes("")), 0x02cc5d05U);
    check<std::uint32_t>("xxhash32(abc)", XxHash32::hash(0, bytes("abc")), 0x32d153ffU);
    check<std::uint64_t>("xxhash64('')", XxHash64::hash(0, bytes("")), 0xef46db3751d8e999ULL);
    check<std::uint64_t>("xxhash64(abc)", XxHash64::hash(0, bytes("abc")), 0x44bc2cf5ad770999ULL);

    // ── crc32 ──
    check<std::uint32_t>("crc32(123456789)", Crc32::hash(bytes("123456789")), 0xcbf43926U);

    // ── bun_hash dispatch ──
    check<std::uint64_t>("bun_hash wyhash", bun_hash(HashAlgo::Wyhash, 0, bytes("abc")).value,
                         Wyhash::hash(0, bytes("abc")));

    // ── SHA-3 (FIPS 202 known-answer tests) ──
    check_hex("sha3-224()", to_hex(sha3_224(bytes(""))),
              "6b4e03423667dbb73b6e15454f0eb1abd4597f9a1b078e3f5b5a6bc7");
    check_hex("sha3-224(abc)", to_hex(sha3_224(bytes("abc"))),
              "e642824c3f8cf24ad09234ee7d3c766fc9a3a5168d0c94ad73b46fdf");
    check_hex("sha3-256()", to_hex(sha3_256(bytes(""))),
              "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a");
    check_hex("sha3-256(abc)", to_hex(sha3_256(bytes("abc"))),
              "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532");
    check_hex("sha3-384(abc)", to_hex(sha3_384(bytes("abc"))),
              "ec01498288516fc926459f58e2c6ad8df9b473cb0fc08c2596da7cf0e49be4b2"
              "98d88cea927ac7f539f1edf228376d25");
    check_hex("sha3-512()", to_hex(sha3_512(bytes(""))),
              "a69f73cca23a9ac5c8b567dc185a756e97c982164fe25859e0d1dcc1475c80a6"
              "15b2123af1f5f94c11e3e9402c3ac558f500199d95b6d3e301758586281dcd26");
    check_hex("sha3-512(abc)", to_hex(sha3_512(bytes("abc"))),
              "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e"
              "10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0");
    // SHAKE XOFs (32-byte output, NIST vectors)
    check_hex("shake128() 32", to_hex(shake128(bytes(""), 32)),
              "7f9c2ba4e88f827d616045507605853ed73b8093f6efbc88eb1a6eacfa66ef26");
    check_hex("shake256() 32", to_hex(shake256(bytes(""), 32)),
              "46b9dd2b0ba88d13233b3feb743eeb243fcd52ea62b81b82b50c27646ed5762f");

    // ── BLAKE2 (RFC 7693 Appendix A / official KATs) ──
    check_hex("blake2b512()", to_hex(blake2b512(bytes(""))),
              "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419"
              "d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce");
    check_hex("blake2b512(abc)", to_hex(blake2b512(bytes("abc"))),
              "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
              "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");
    check_hex("blake2s256()", to_hex(blake2s256(bytes(""))),
              "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9");
    check_hex("blake2s256(abc)", to_hex(blake2s256(bytes("abc"))),
              "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982");
    // BLAKE2 streaming across a block boundary must match one-shot.
    {
        std::vector<std::uint8_t> big(300, 0x5a);
        Blake2b hb { 64 };
        hb.update(std::span(big).first(130));
        hb.update(std::span(big).subspan(130));
        std::vector<std::uint8_t> so(64);
        hb.final_(so.data());
        check_hex("blake2b streaming==oneshot", to_hex(so), to_hex(blake2b512(big)));
    }

    // ── PBKDF2 (RFC 6070 HMAC-SHA1 + RFC 7914 HMAC-SHA256) ──
    check_hex("pbkdf2-sha1 c1",
              to_hex(*pbkdf2(DigestAlgo::Sha1, bytes("password"), bytes("salt"), 1, 20)),
              "0c60c80f961f0e71f3a9b524af6012062fe037a6");
    check_hex("pbkdf2-sha1 c2",
              to_hex(*pbkdf2(DigestAlgo::Sha1, bytes("password"), bytes("salt"), 2, 20)),
              "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957");
    check_hex("pbkdf2-sha1 c4096",
              to_hex(*pbkdf2(DigestAlgo::Sha1, bytes("password"), bytes("salt"), 4096, 20)),
              "4b007901b765489abead49d926f721d065a429c1");
    check_hex("pbkdf2-sha1 long c4096",
              to_hex(*pbkdf2(DigestAlgo::Sha1, bytes("passwordPASSWORDpassword"),
                             bytes("saltSALTsaltSALTsaltSALTsaltSALTsalt"), 4096, 25)),
              "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038");
    check_hex("pbkdf2-sha256 c1",
              to_hex(*pbkdf2(DigestAlgo::Sha256, bytes("password"), bytes("salt"), 1, 32)),
              "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    check_hex("pbkdf2-sha256 c4096",
              to_hex(*pbkdf2(DigestAlgo::Sha256, bytes("password"), bytes("salt"), 4096, 32)),
              "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");

    if (g_failed == 0) {
        std::println("all crypto vectors passed");
        return 0;
    }
    std::println("{} crypto checks FAILED", g_failed);
    return 1;
}
