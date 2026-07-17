// mbun.crypto.hasher — the user-facing surface:
//   * digest encodings (hex / base64 / base64url)  — bun's node Encoding subset
//   * CryptoHasher   — Bun.CryptoHasher (md5/sha1/sha224/sha256/sha384/sha512/
//                      sha512-256) with update / digest(hex|base64|buffer) / copy
//   * bun_hash       — Bun.hash dispatch over the non-crypto family
//
// This is the pure-logic core. The JSC binding layer (host_fn glue,
// JSGlobalObject argument decoding, ArrayBuffer plumbing — src/runtime/crypto/
// CryptoHasher.rs and src/runtime/api/HashObject.rs) is DEFERRED(S1) to the JSC
// subsystem, which is where JSValue/Blob/StringOrBuffer live. HMAC / sha3 /
// shake / blake2 / pbkdf2 now live in sibling modules; DEFERRED(S2): md4,
// ripemd160, XxHash3.

export module mbun.crypto.hasher;

import std;

import mbun.crypto.md5;
import mbun.crypto.sha;
import mbun.crypto.wyhash;
import mbun.crypto.noncrypto_hash;

export namespace mbun::crypto {

// ── Digest encodings ────────────────────────────────────────────────────────
enum class Encoding { Buffer, Hex, Base64, Base64Url };

// Parse a node-style encoding name (subset). Returns nullopt for unknown.
inline std::optional<Encoding> encoding_from(std::string_view name) {
    if (name == "hex") { return Encoding::Hex; }
    if (name == "base64") { return Encoding::Base64; }
    if (name == "base64url") { return Encoding::Base64Url; }
    if (name == "buffer") { return Encoding::Buffer; }
    return std::nullopt;
}

inline std::string to_hex(std::span<const std::uint8_t> bytes) {
    static constexpr char DIGITS[] { "0123456789abcdef" };
    std::string out;
    out.resize(bytes.size() * 2);
    for (std::size_t i { 0 }; i < bytes.size(); ++i) {
        out[i * 2] = DIGITS[bytes[i] >> 4];
        out[i * 2 + 1] = DIGITS[bytes[i] & 0x0f];
    }
    return out;
}

inline std::string to_base64_impl_(std::span<const std::uint8_t> bytes, bool url) {
    static constexpr char STD[] {
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" };
    static constexpr char URL[] {
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_" };
    const char* tbl { url ? URL : STD };
    std::string out;
    std::size_t n { bytes.size() };
    out.reserve(((n + 2) / 3) * 4);
    std::size_t i { 0 };
    while (i + 3 <= n) {
        std::uint32_t v { (static_cast<std::uint32_t>(bytes[i]) << 16)
                        | (static_cast<std::uint32_t>(bytes[i + 1]) << 8)
                        | static_cast<std::uint32_t>(bytes[i + 2]) };
        out.push_back(tbl[(v >> 18) & 0x3f]);
        out.push_back(tbl[(v >> 12) & 0x3f]);
        out.push_back(tbl[(v >> 6) & 0x3f]);
        out.push_back(tbl[v & 0x3f]);
        i += 3;
    }
    std::size_t rem { n - i };
    if (rem == 1) {
        std::uint32_t v { static_cast<std::uint32_t>(bytes[i]) << 16 };
        out.push_back(tbl[(v >> 18) & 0x3f]);
        out.push_back(tbl[(v >> 12) & 0x3f]);
        if (!url) {
            out.push_back('=');
            out.push_back('=');
        }
    } else if (rem == 2) {
        std::uint32_t v { (static_cast<std::uint32_t>(bytes[i]) << 16)
                        | (static_cast<std::uint32_t>(bytes[i + 1]) << 8) };
        out.push_back(tbl[(v >> 18) & 0x3f]);
        out.push_back(tbl[(v >> 12) & 0x3f]);
        out.push_back(tbl[(v >> 6) & 0x3f]);
        if (!url) { out.push_back('='); }
    }
    return out;
}
inline std::string to_base64(std::span<const std::uint8_t> bytes) {
    return to_base64_impl_(bytes, false);
}
inline std::string to_base64url(std::span<const std::uint8_t> bytes) {
    return to_base64_impl_(bytes, true);
}

// Encode raw digest bytes into `enc`. For Encoding::Buffer, returns the hex form
// only as a convenience is NOT done — callers should use digest() for raw bytes.
inline std::string encode(std::span<const std::uint8_t> bytes, Encoding enc) {
    switch (enc) {
    case Encoding::Hex: return to_hex(bytes);
    case Encoding::Base64: return to_base64(bytes);
    case Encoding::Base64Url: return to_base64url(bytes);
    case Encoding::Buffer: return to_hex(bytes);  // raw path uses digest(); fallback = hex
    }
    return {};
}

// ── CryptoHasher (Bun.CryptoHasher) ─────────────────────────────────────────
enum class DigestAlgo { Md5, Sha1, Sha224, Sha256, Sha384, Sha512, Sha512_224, Sha512_256 };

inline std::optional<DigestAlgo> digest_algo_from(std::string_view name) {
    // Case-insensitive, separator-insensitive: normalize by lowercasing and
    // dropping '-', '_', '/', '.' so bun's many spellings collapse to a single
    // canonical key (sha-256 / SHA256 / sha_256 → "sha256"; sha-512/256 /
    // sha512-256 / sha-512_256 → "sha512256"). Non-latin1 bytes stay as-is and
    // simply fail to match (→ nullopt → "Unsupported algorithm").
    std::string key;
    key.reserve(name.size());
    for (char ch : name) {
        if (ch == '-' || ch == '_' || ch == '/' || ch == '.') { continue; }
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (key == "md5") { return DigestAlgo::Md5; }
    if (key == "sha1") { return DigestAlgo::Sha1; }
    if (key == "sha224") { return DigestAlgo::Sha224; }
    if (key == "sha256") { return DigestAlgo::Sha256; }
    if (key == "sha384") { return DigestAlgo::Sha384; }
    if (key == "sha512") { return DigestAlgo::Sha512; }
    if (key == "sha512224") { return DigestAlgo::Sha512_224; }
    if (key == "sha512256") { return DigestAlgo::Sha512_256; }
    return std::nullopt;
}

class CryptoHasher {
public:
    // Construct by algorithm name (case-insensitive). Returns nullopt for an
    // unsupported/DEFERRED algorithm (sha3/blake2/md4/ripemd160/...).
    static std::optional<CryptoHasher> by_name(std::string_view name) {
        auto algo { digest_algo_from(name) };
        if (!algo) { return std::nullopt; }
        return CryptoHasher(*algo);
    }

    explicit CryptoHasher(DigestAlgo algo) : algo_ { algo } {
        switch (algo) {
        case DigestAlgo::Md5: state_.emplace<Md5>(); break;
        case DigestAlgo::Sha1: state_.emplace<Sha1>(); break;
        case DigestAlgo::Sha224: state_.emplace<Sha2_32>(true, 28); break;
        case DigestAlgo::Sha256: state_.emplace<Sha2_32>(false, 32); break;
        case DigestAlgo::Sha384: state_.emplace<Sha2_64>(Sha512Variant::S384); break;
        case DigestAlgo::Sha512: state_.emplace<Sha2_64>(Sha512Variant::S512); break;
        case DigestAlgo::Sha512_224:
            state_.emplace<Sha2_64>(Sha512Variant::S512_224);
            break;
        case DigestAlgo::Sha512_256:
            state_.emplace<Sha2_64>(Sha512Variant::S512_256);
            break;
        }
    }

    DigestAlgo algorithm() const { return algo_; }

    std::size_t digest_length() const {
        switch (algo_) {
        case DigestAlgo::Md5: return 16;
        case DigestAlgo::Sha1: return 20;
        case DigestAlgo::Sha224: return 28;
        case DigestAlgo::Sha256: return 32;
        case DigestAlgo::Sha384: return 48;
        case DigestAlgo::Sha512: return 64;
        case DigestAlgo::Sha512_224: return 28;
        case DigestAlgo::Sha512_256: return 32;
        }
        return 0;
    }

    void update(std::span<const std::uint8_t> data) {
        std::visit([&](auto& h) { h.update(data); }, state_);
    }
    void update(std::string_view text) {
        update(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                             text.size()));
    }

    // Finalize into a fresh byte vector. Destructive (pads the state), like an
    // EVP final; use copy() first to snapshot.
    std::vector<std::uint8_t> digest() {
        std::vector<std::uint8_t> out(digest_length());
        std::visit([&](auto& h) { h.final_(out.data()); }, state_);
        return out;
    }

    std::string digest(Encoding enc) {
        auto bytes { digest() };
        return encode(bytes, enc);
    }

    // Snapshot the running state (Bun.CryptoHasher#copy).
    CryptoHasher copy() const { return *this; }

    // One-shot convenience: Bun.CryptoHasher.hash(algo, data).
    static std::optional<std::vector<std::uint8_t>> hash(std::string_view algo,
                                                         std::span<const std::uint8_t> data) {
        auto h { by_name(algo) };
        if (!h) { return std::nullopt; }
        h->update(data);
        return h->digest();
    }

private:
    DigestAlgo algo_;
    std::variant<Md5, Sha1, Sha2_32, Sha2_64> state_ { Md5 {} };
};

// ── Bun.hash dispatch (non-crypto family) ───────────────────────────────────
enum class HashAlgo {
    Wyhash, Adler32, Crc32, CityHash32, CityHash64,
    XxHash32, XxHash64, XxHash3, Murmur32v2, Murmur32v3, Murmur64v2, Rapidhash,
};

struct HashResult {
    std::uint64_t value;  // u32 results are zero-extended; JS numeric value is identical
    bool is64;            // true → BigInt (u64), false → number (u32)
};

inline std::optional<HashAlgo> hash_algo_from(std::string_view name) {
    if (name == "wyhash") { return HashAlgo::Wyhash; }
    if (name == "adler32") { return HashAlgo::Adler32; }
    if (name == "crc32") { return HashAlgo::Crc32; }
    if (name == "cityHash32") { return HashAlgo::CityHash32; }
    if (name == "cityHash64") { return HashAlgo::CityHash64; }
    if (name == "xxHash32") { return HashAlgo::XxHash32; }
    if (name == "xxHash64") { return HashAlgo::XxHash64; }
    if (name == "xxHash3") { return HashAlgo::XxHash3; }
    if (name == "murmur32v2") { return HashAlgo::Murmur32v2; }
    if (name == "murmur32v3") { return HashAlgo::Murmur32v3; }
    if (name == "murmur64v2") { return HashAlgo::Murmur64v2; }
    if (name == "rapidhash") { return HashAlgo::Rapidhash; }
    return std::nullopt;
}

// Mirrors HashObject.rs: each algorithm absorbs the seed-width/order quirks.
inline HashResult bun_hash(HashAlgo algo, std::uint64_t seed,
                           std::span<const std::uint8_t> input) {
    switch (algo) {
    case HashAlgo::Wyhash:
        return { Wyhash::hash(seed, input), true };
    case HashAlgo::Adler32:
        return { Adler32::hash(input), false };  // seed ignored
    case HashAlgo::Crc32:
        return { Crc32::hash(static_cast<std::uint32_t>(seed), input), false };
    case HashAlgo::CityHash32:
        return { CityHash32::hash(input), false };  // seed ignored
    case HashAlgo::CityHash64:
        return { CityHash64::hash_with_seed(input, seed), true };
    case HashAlgo::XxHash32:
        return { XxHash32::hash(static_cast<std::uint32_t>(seed), input), false };
    case HashAlgo::XxHash64:
        return { XxHash64::hash(seed, input), true };
    case HashAlgo::XxHash3:
        // Bun.hash.xxHash3 truncates the seed to u32 (HashObject @truncate).
        return { XxHash3::hash(static_cast<std::uint32_t>(seed), input), true };
    case HashAlgo::Murmur32v2:
        return { Murmur2_32::hash_with_seed(input, static_cast<std::uint32_t>(seed)), false };
    case HashAlgo::Murmur32v3:
        return { Murmur3_32::hash_with_seed(input, static_cast<std::uint32_t>(seed)), false };
    case HashAlgo::Murmur64v2:
        return { Murmur2_64::hash_with_seed(input, seed), true };
    case HashAlgo::Rapidhash:
        return { RapidHash::hash(seed, input), true };
    }
    return { 0, false };
}

// `Bun.hash(input, seed?)` — the callable itself is wyhash.
inline std::uint64_t bun_hash_default(std::span<const std::uint8_t> input, std::uint64_t seed = 0) {
    return Wyhash::hash(seed, input);
}

}  // namespace mbun::crypto
