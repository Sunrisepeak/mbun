// test_core_compress.cpp — mbun.core.compress (DEFLATE/zlib/gzip) test suite.
//
// Coverage:
//   - crc32/adler32 known vectors ("123456789" → 0xCBF43926 / "hello" adler)
//   - round-trip identity: deflate_raw/zlib/gzip over empty, tiny, repetitive,
//     all-byte-values, and pseudo-random inputs, at levels 0 (stored), 1, 6, 9
//   - cross-implementation decode: fixtures produced by CPython's zlib/gzip
//     (fixed-Huffman "hello", fixed-Huffman repetitive text, DYNAMIC-Huffman
//     block, gzip member with mtime=0) must inflate to the original bytes —
//     this pins the complete inflate path (stored + fixed + dynamic) against
//     a canonical zlib encoder, matching how bun's zlib tests feed arbitrary
//     compressed fixtures.
//   - container validation: gzip magic/trailer, zlib header check, corrupt
//     input must return errors (not crash, not wrong data)
import std;
import mbun.core.compress;

namespace {

namespace compress = mbun::core::compress;
using Bytes = std::vector<std::uint8_t>;

int gChecks{0};
int gFailures{0};

void check(bool ok, std::string_view what) {
    ++gChecks;
    if (!ok) {
        ++gFailures;
        std::println("  FAIL {}", what);
    }
}

Bytes from_hex(std::string_view hex) {
    auto nib{[](char c) -> std::uint8_t {
        return c <= '9' ? static_cast<std::uint8_t>(c - '0') : static_cast<std::uint8_t>(c - 'a' + 10);
    }};
    Bytes out{};
    out.reserve(hex.size() / 2);
    for (std::size_t i{0}; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(nib(hex[i]) << 4 | nib(hex[i + 1])));
    return out;
}

Bytes ascii(std::string_view s) { return {s.begin(), s.end()}; }

void test_checksums() {
    check(compress::crc32(ascii("123456789")) == 0xCBF43926U, "crc32(123456789)");
    check(compress::crc32(ascii("")) == 0U, "crc32(empty)");
    check(compress::adler32(ascii("hello")) == 0x062C0215U, "adler32(hello)");
    check(compress::adler32(ascii("")) == 1U, "adler32(empty)");
    // Chained == whole.
    auto whole{compress::crc32(ascii("123456789"))};
    auto part{compress::crc32(ascii("6789"), compress::crc32(ascii("12345")))};
    check(whole == part, "crc32 chaining");
}

std::vector<Bytes> corpus() {
    std::vector<Bytes> inputs{};
    inputs.push_back({});
    inputs.push_back(ascii("a"));
    inputs.push_back(ascii("hello world"));
    Bytes rep{};
    for (int i{0}; i < 500; ++i)
        for (char c : std::string_view{"the quick brown fox jumps over the lazy dog. "})
            rep.push_back(static_cast<std::uint8_t>(c));
    inputs.push_back(std::move(rep));
    Bytes all{};
    for (int r{0}; r < 3; ++r)
        for (int b{0}; b < 256; ++b) all.push_back(static_cast<std::uint8_t>(b));
    inputs.push_back(std::move(all));
    Bytes random{};
    std::uint64_t state{0x9E3779B97F4A7C15ULL};  // deterministic xorshift
    for (int i{0}; i < 100000; ++i) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        random.push_back(static_cast<std::uint8_t>(state));
    }
    inputs.push_back(std::move(random));
    return inputs;
}

void test_roundtrips() {
    for (const auto& input : corpus()) {
        for (int level : {0, 1, 6, 9}) {
            auto tag{[&](std::string_view fmt) {
                return std::format("{} roundtrip len={} level={}", fmt, input.size(), level);
            }};
            auto raw{compress::inflate_raw(compress::deflate_raw(input, level))};
            check(raw.has_value() && *raw == input, tag("deflate_raw"));
            auto z{compress::zlib_decompress(compress::zlib_compress(input, level))};
            check(z.has_value() && *z == input, tag("zlib"));
            auto g{compress::gzip_decompress(compress::gzip_compress(input, level))};
            check(g.has_value() && *g == input, tag("gzip"));
        }
    }
}

void test_container_bytes() {
    Bytes g{compress::gzip_compress(ascii("hi"))};
    check(g.size() > 18 && g[0] == 0x1F && g[1] == 0x8B && g[2] == 8, "gzip magic + method");
    Bytes z{compress::zlib_compress(ascii("hi"))};
    check(z.size() > 6 && (z[0] & 0x0F) == 8 && (static_cast<unsigned>(z[0]) * 256 + z[1]) % 31 == 0,
          "zlib CMF/FLG header check");
    // Multi-member gzip stream decodes as the concatenation (RFC 1952 §2.2).
    Bytes two{compress::gzip_compress(ascii("foo"))};
    Bytes second{compress::gzip_compress(ascii("bar"))};
    two.insert(two.end(), second.begin(), second.end());
    auto joined{compress::gzip_decompress(two)};
    check(joined.has_value() && *joined == ascii("foobar"), "multi-member gzip");
}

void test_python_fixtures() {
    // zlib.compress(b'hello')  (CPython, fixed Huffman)
    auto hello{compress::zlib_decompress(from_hex("789ccb48cdc9c90700062c0215"))};
    check(hello.has_value() && *hello == ascii("hello"), "python zlib 'hello'");

    // zlib.compress(rep, 9) where rep = b'the quick brown fox... '*20 (fixed Huffman + LZ77)
    auto rep{compress::zlib_decompress(from_hex(
        "78da2bc94855282ccd4cce56482aca2fcf5348cbaf50c82acd2d2856c82f4b2d5228014ae72456552aa4e4a7eb"
        "8179a38a47158f2aa6aa6200e521459c"))};
    Bytes repExpect{};
    for (int i{0}; i < 20; ++i)
        for (char c : std::string_view{"the quick brown fox jumps over the lazy dog. "})
            repExpect.push_back(static_cast<std::uint8_t>(c));
    check(rep.has_value() && *rep == repExpect, "python zlib repetitive (fixed+LZ77)");

    // Raw deflate of the same data (compressobj windowBits=-15).
    auto raw{compress::inflate_raw(from_hex(
        "2bc94855282ccd4cce56482aca2fcf5348cbaf50c82acd2d2856c82f4b2d5228014ae72456552aa4e4a7eb8179"
        "a38a47158f2aa6aa6200"))};
    check(raw.has_value() && *raw == repExpect, "python raw deflate");

    // DYNAMIC-Huffman block: zlib.compress(random.seed(42) text, 9) — BTYPE=2.
    Bytes dynExpect{from_hex(
        "62616564646362206267616162646420612064206764686561636766656364666262676266666561682062676220"
        "656664626164656264626765686663666664656263206463686765206466616461666765626466646867686365"
        "636420206567676664632068626162636367626767682065206162206566626567636861652063206265206463"
        "666320206166686162666564616462626862206363682063652067642064656766682068626464626661206464"
        "616261646261666220646568642063686468676462626766676768616261676662646464206863676365686462"
        "682062612061626463676868646761636761676568656720686364656461206166616168202063612062636262"
        "646762646162672066656466646567636568666261686262206420656366626466656368206520612065626365"
        "626220636565646664652068656162676561616663656368206720616262632061662063676361656661666464"
        "626620676364636367616366676465636267616864646866656464616467666562656620672066")};
    auto dyn{compress::zlib_decompress(from_hex(
        "78da1d50c711c0300c5a85d504a8ec3f41707e3e8368acb64570ab68a360acaf4b3b2d0fb99ce9ba30881eb3dc"
        "34b76f3493b760dd363ce59a0d38be3de51ae8ddb1702c4acbdd43a39e107b75d510985b8d809a2b4ebb4c1e21"
        "1dd44903f7ce45c4e624a02b2158c30067e8e297939dc807da09d138ad0233c92b96d6de79d3ab92bd37b86315"
        "d34a39a840915e467d9114e9f60486758c135ad1cd267f03a477e75bfd1391c13249d5e811367ed9a506c9503d"
        "352f3956ce08a559b732f8d917a3f4cdd6ec10301f32eb8f57"))};
    check(dyn.has_value() && *dyn == dynExpect, "python zlib dynamic-Huffman block");

    // gzip.compress(b'hello world', mtime=0) (CPython)
    auto gz{compress::gzip_decompress(
        from_hex("1f8b08000000000002ffcb48cdc9c95728cf2fca49010085114a0d0b000000"))};
    check(gz.has_value() && *gz == ascii("hello world"), "python gzip 'hello world'");
}

void test_errors() {
    check(!compress::zlib_decompress(ascii("not zlib data")).has_value(), "zlib garbage rejected");
    check(!compress::gzip_decompress(ascii("not gzip data")).has_value(), "gzip garbage rejected");
    check(!compress::inflate_raw(from_hex("07")).has_value(), "invalid block type rejected");
    Bytes z{compress::zlib_compress(ascii("hello hello hello"))};
    Bytes truncated{z.begin(), z.begin() + static_cast<std::ptrdiff_t>(z.size() - 6)};
    check(!compress::zlib_decompress(truncated).has_value(), "truncated zlib rejected");
    Bytes corrupt{z};
    corrupt[corrupt.size() - 1] ^= 0xFF;  // break the adler32 trailer
    check(!compress::zlib_decompress(corrupt).has_value(), "zlib checksum mismatch rejected");
    Bytes g{compress::gzip_compress(ascii("hello hello hello"))};
    Bytes gcorrupt{g};
    gcorrupt[gcorrupt.size() - 5] ^= 0xFF;  // break the crc32 trailer
    check(!compress::gzip_decompress(gcorrupt).has_value(), "gzip crc mismatch rejected");
}

// A bounded inflate must reject a stream that would grow past `maxOut` instead
// of allocating it fully — this is the tarball-bomb cap bun applies as
// max_output_size (extract_tarball.rs). The check lives inside inflate_block,
// so a single oversized deflate block is caught mid-decode, not after.
void test_size_cap() {
    // Repetitive payload → one fixed-Huffman block full of length/distance
    // copies: exercises the in-block bound (no per-symbol input to exhaust).
    Bytes big{};
    for (int i{0}; i < 200000; ++i) big.push_back(static_cast<std::uint8_t>('a' + (i % 5)));

    // Raw DEFLATE has no ISIZE footer, so this can only be stopped by the bound
    // *inside* inflate_block — the load-bearing case from the issue.
    Bytes raw{compress::deflate_raw(big, 6)};
    auto capped{compress::inflate_raw(raw, nullptr, std::size_t{1024})};
    check(!capped.has_value(), "inflate_raw over cap rejected");
    check(!capped.has_value() && capped.error() == compress::DECOMPRESS_LIMIT_ERROR,
          "inflate_raw over cap returns DECOMPRESS_LIMIT_ERROR");
    auto ok{compress::inflate_raw(raw, nullptr, big.size())};
    check(ok.has_value() && *ok == big, "inflate_raw within cap succeeds");
    auto unbounded{compress::inflate_raw(raw)};
    check(unbounded.has_value() && *unbounded == big, "inflate_raw unbounded unaffected");

    // gzip carries an ISIZE footer: an over-cap payload is rejected up front
    // with zero inflate work, while an under-cap one decodes normally.
    Bytes gz{compress::gzip_compress(big, 6)};
    auto gzCapped{compress::gzip_decompress(gz, std::size_t{1024})};
    check(!gzCapped.has_value() && gzCapped.error() == compress::DECOMPRESS_LIMIT_ERROR,
          "gzip_decompress over cap rejected via ISIZE");
    auto gzOk{compress::gzip_decompress(gz, big.size())};
    check(gzOk.has_value() && *gzOk == big, "gzip_decompress within cap succeeds");
    // A cap exactly at the payload size must still pass (boundary is inclusive).
    auto gzExact{compress::gzip_decompress(compress::gzip_compress(ascii("hello"), 6), std::size_t{5})};
    check(gzExact.has_value() && *gzExact == ascii("hello"), "gzip_decompress exact-fit cap succeeds");
}

}  // namespace

int main() {
    test_checksums();
    test_roundtrips();
    test_container_bytes();
    test_python_fixtures();
    test_errors();
    test_size_cap();
    std::println("test_core_compress: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
