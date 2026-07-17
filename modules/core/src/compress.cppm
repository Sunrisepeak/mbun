// compress.cppm — mbun.core.compress: DEFLATE (RFC 1951), zlib (RFC 1950) and
// gzip (RFC 1952) containers, plus the crc32/adler32 checksums they require.
//
// Blueprint references:
//   - bun routes node:zlib / Bun.deflateSync through vendored zlib/libdeflate
//     (src/deps/zlib, src/bun.js/api/bun/zlib.zig historically; later the Rust
//     rewrite kept the same container/windowBits semantics: deflateSync = raw
//     DEFLATE, gzipSync = gzip container, node:zlib deflateSync = zlib container).
//   - Inflate follows the canonical RFC 1951 decode structure (as in Mark
//     Adler's puff): canonical Huffman tables decoded from code-length counts,
//     stored/fixed/dynamic blocks, 32 KiB sliding window.
//   - Deflate emits fixed-Huffman blocks with greedy hash-chain LZ77 (level 0
//     emits stored blocks). Any spec-conforming inflater — including zlib and
//     bun itself — reads this output; dynamic-Huffman emission is a future
//     ratio optimization, not a correctness gap.
//
// Behaviour is pinned by bun's original zlib test suite via the JS layer
// (modules/jsc): round-trip identity, container magic/checksum validation, and
// error propagation on corrupt input (std::expected, no exceptions).
export module mbun.core.compress;

import std;

namespace mbun::core::compress {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

// ---------------------------------------------------------------------------
// Checksums
// ---------------------------------------------------------------------------

namespace {

consteval std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t n{0}; n < 256; ++n) {
        std::uint32_t c{n};
        for (int k{0}; k < 8; ++k) c = (c & 1U) != 0U ? 0xEDB88320U ^ (c >> 1) : c >> 1;
        t[n] = c;
    }
    return t;
}

constexpr auto CRC_TABLE{make_crc_table()};

}  // namespace

// CRC-32 (IEEE 802.3, reflected) — gzip trailer checksum. `crc` chains calls.
export std::uint32_t crc32(ByteView data, std::uint32_t crc = 0) {
    crc = ~crc;
    for (std::uint8_t b : data) crc = CRC_TABLE[(crc ^ b) & 0xFFU] ^ (crc >> 8);
    return ~crc;
}

// Adler-32 — zlib trailer checksum. `adler` chains calls (initial value 1).
export std::uint32_t adler32(ByteView data, std::uint32_t adler = 1) {
    constexpr std::uint32_t MOD{65521};
    std::uint32_t a{adler & 0xFFFFU};
    std::uint32_t b{(adler >> 16) & 0xFFFFU};
    std::size_t i{0};
    while (i < data.size()) {
        // 5552 is the largest n with n*(n+1)/2*255 + n*(255+1) < 2^32 (zlib NMAX).
        std::size_t chunk{std::min<std::size_t>(5552, data.size() - i)};
        for (std::size_t e{i + chunk}; i < e; ++i) {
            a += data[i];
            b += a;
        }
        a %= MOD;
        b %= MOD;
    }
    return (b << 16) | a;
}

// ---------------------------------------------------------------------------
// Shared DEFLATE tables (RFC 1951 §3.2.5)
// ---------------------------------------------------------------------------

namespace {

constexpr std::array<std::uint16_t, 29> LEN_BASE{
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> LEN_EXTRA{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> DIST_BASE{
    1,   2,   3,   4,   5,    7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
    193, 257, 385, 513, 769,  1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> DIST_EXTRA{0, 0, 0, 0, 1, 1, 2,  2,  3,  3,  4,  4,  5,  5,  6,
                                                  6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

constexpr std::size_t WINDOW_SIZE{32768};
constexpr std::size_t MIN_MATCH{3};
constexpr std::size_t MAX_MATCH{258};

// ---------------------------------------------------------------------------
// Inflate (complete: stored + fixed + dynamic Huffman)
// ---------------------------------------------------------------------------

class BitReader {
public:
    explicit BitReader(ByteView src) : src_{src} {}

    // Returns nullopt on input exhaustion.
    std::optional<std::uint32_t> bits(unsigned need) {
        while (bitCount_ < need) {
            if (pos_ >= src_.size()) return std::nullopt;
            bitBuf_ |= static_cast<std::uint64_t>(src_[pos_++]) << bitCount_;
            bitCount_ += 8;
        }
        auto v{static_cast<std::uint32_t>(bitBuf_ & ((1ULL << need) - 1))};
        bitBuf_ >>= need;
        bitCount_ -= need;
        return v;
    }

    void align_to_byte() {
        unsigned drop{bitCount_ % 8};
        bitBuf_ >>= drop;
        bitCount_ -= drop;
    }

    // Copies `n` raw bytes (caller must be byte-aligned); false on exhaustion.
    bool read_bytes(std::uint8_t* out, std::size_t n) {
        while (n > 0 && bitCount_ >= 8) {
            *out++ = static_cast<std::uint8_t>(bitBuf_ & 0xFFU);
            bitBuf_ >>= 8;
            bitCount_ -= 8;
            --n;
        }
        if (n > src_.size() - pos_) return false;
        std::copy_n(src_.data() + pos_, n, out);
        pos_ += n;
        return true;
    }

    // Bytes consumed from the underlying view (buffered-but-unread bits count
    // as consumed input; call align_to_byte() first for container trailers).
    std::size_t consumed() const { return pos_ - bitCount_ / 8; }

private:
    ByteView src_;
    std::size_t pos_{0};
    std::uint64_t bitBuf_{0};
    unsigned bitCount_{0};
};

// Canonical Huffman decoder built from per-symbol code lengths (puff-style:
// count[] per length + symbols sorted by (length, symbol)).
class Huffman {
public:
    // lengths[i] = code length of symbol i (0 = unused). Returns false when the
    // length set is oversubscribed (invalid stream).
    bool build(std::span<const std::uint8_t> lengths) {
        count_.fill(0);
        for (std::uint8_t len : lengths) ++count_[len];
        if (count_[0] == lengths.size()) {  // no codes at all: valid, decodes nothing
            complete_ = false;
            return true;
        }
        int left{1};
        for (int len{1}; len <= 15; ++len) {
            left <<= 1;
            left -= count_[len];
            if (left < 0) return false;  // oversubscribed
        }
        complete_ = left == 0;
        std::array<std::uint16_t, 16> offs{};
        for (int len{1}; len < 15; ++len) offs[len + 1] = static_cast<std::uint16_t>(offs[len] + count_[len]);
        symbol_.assign(lengths.size(), 0);
        for (std::size_t sym{0}; sym < lengths.size(); ++sym) {
            if (lengths[sym] != 0) symbol_[offs[lengths[sym]]++] = static_cast<std::uint16_t>(sym);
        }
        return true;
    }

    bool complete() const { return complete_; }

    // Decodes one symbol; -1 on invalid code / input exhaustion.
    int decode(BitReader& br) const {
        int code{0};
        int first{0};
        int index{0};
        for (int len{1}; len <= 15; ++len) {
            auto bit{br.bits(1)};
            if (!bit) return -1;
            code |= static_cast<int>(*bit);
            int cnt{count_[len]};
            if (code - cnt < first) return static_cast<int>(symbol_[index + (code - first)]);
            index += cnt;
            first += cnt;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }

private:
    std::array<int, 16> count_{};
    std::vector<std::uint16_t> symbol_{};
    bool complete_{false};
};

const Huffman& fixed_litlen_table() {
    static const Huffman table{[] {
        std::array<std::uint8_t, 288> lengths{};
        for (std::size_t i{0}; i < 144; ++i) lengths[i] = 8;
        for (std::size_t i{144}; i < 256; ++i) lengths[i] = 9;
        for (std::size_t i{256}; i < 280; ++i) lengths[i] = 7;
        for (std::size_t i{280}; i < 288; ++i) lengths[i] = 8;
        Huffman h{};
        h.build(lengths);
        return h;
    }()};
    return table;
}

const Huffman& fixed_dist_table() {
    static const Huffman table{[] {
        std::array<std::uint8_t, 30> lengths{};
        lengths.fill(5);
        Huffman h{};
        h.build(lengths);
        return h;
    }()};
    return table;
}

std::unexpected<std::string> inflate_error(std::string_view what) {
    return std::unexpected{std::string{what}};
}

// Decodes the compressed payload of one huffman-coded block into `out`.
std::optional<std::string> inflate_block(BitReader& br, const Huffman& litlen, const Huffman& dist,
                                         Bytes& out) {
    for (;;) {
        int sym{litlen.decode(br)};
        if (sym < 0) return "invalid literal/length code";
        if (sym < 256) {
            out.push_back(static_cast<std::uint8_t>(sym));
            continue;
        }
        if (sym == 256) return std::nullopt;  // end of block
        sym -= 257;
        if (sym >= 29) return "invalid length code";
        auto lenExtra{br.bits(LEN_EXTRA[sym])};
        if (!lenExtra) return "unexpected end of input";
        std::size_t length{static_cast<std::size_t>(LEN_BASE[sym]) + *lenExtra};

        int dsym{dist.decode(br)};
        if (dsym < 0 || dsym >= 30) return "invalid distance code";
        auto distExtra{br.bits(DIST_EXTRA[dsym])};
        if (!distExtra) return "unexpected end of input";
        std::size_t distance{static_cast<std::size_t>(DIST_BASE[dsym]) + *distExtra};
        if (distance > out.size()) return "invalid distance too far back";

        std::size_t from{out.size() - distance};
        out.reserve(out.size() + length);
        for (std::size_t i{0}; i < length; ++i) out.push_back(out[from + i]);  // may self-overlap
    }
}

}  // namespace

// Raw DEFLATE (RFC 1951) decompression. On success `consumed` (when non-null)
// receives the number of input bytes the stream occupied — container formats
// use it to locate their trailer.
export std::expected<Bytes, std::string> inflate_raw(ByteView src, std::size_t* consumed = nullptr) {
    BitReader br{src};
    Bytes out{};
    for (;;) {
        auto bfinal{br.bits(1)};
        auto btype{br.bits(2)};
        if (!bfinal || !btype) return inflate_error("unexpected end of input");
        switch (*btype) {
            case 0: {  // stored
                br.align_to_byte();
                auto len{br.bits(16)};
                auto nlen{br.bits(16)};
                if (!len || !nlen) return inflate_error("unexpected end of input");
                if ((*len ^ 0xFFFFU) != *nlen) return inflate_error("invalid stored block lengths");
                std::size_t old{out.size()};
                out.resize(old + *len);
                if (!br.read_bytes(out.data() + old, *len)) return inflate_error("unexpected end of input");
                break;
            }
            case 1: {  // fixed Huffman
                if (auto err{inflate_block(br, fixed_litlen_table(), fixed_dist_table(), out)})
                    return inflate_error(*err);
                break;
            }
            case 2: {  // dynamic Huffman
                auto hlit{br.bits(5)};
                auto hdist{br.bits(5)};
                auto hclen{br.bits(4)};
                if (!hlit || !hdist || !hclen) return inflate_error("unexpected end of input");
                std::size_t nlit{*hlit + 257};
                std::size_t ndist{*hdist + 1};
                std::size_t ncode{*hclen + 4};
                if (nlit > 286 || ndist > 30) return inflate_error("too many length or distance symbols");

                static constexpr std::array<std::uint8_t, 19> CLC_ORDER{
                    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                std::array<std::uint8_t, 19> clcLengths{};
                for (std::size_t i{0}; i < ncode; ++i) {
                    auto v{br.bits(3)};
                    if (!v) return inflate_error("unexpected end of input");
                    clcLengths[CLC_ORDER[i]] = static_cast<std::uint8_t>(*v);
                }
                Huffman clc{};
                if (!clc.build(clcLengths) || !clc.complete())
                    return inflate_error("invalid code lengths set");

                std::vector<std::uint8_t> lengths(nlit + ndist, 0);
                std::size_t idx{0};
                while (idx < lengths.size()) {
                    int sym{clc.decode(br)};
                    if (sym < 0) return inflate_error("invalid code length code");
                    if (sym < 16) {
                        lengths[idx++] = static_cast<std::uint8_t>(sym);
                        continue;
                    }
                    std::uint8_t repeatValue{0};
                    std::size_t repeat{0};
                    if (sym == 16) {
                        if (idx == 0) return inflate_error("invalid repeat with no previous length");
                        repeatValue = lengths[idx - 1];
                        auto r{br.bits(2)};
                        if (!r) return inflate_error("unexpected end of input");
                        repeat = 3 + *r;
                    } else if (sym == 17) {
                        auto r{br.bits(3)};
                        if (!r) return inflate_error("unexpected end of input");
                        repeat = 3 + *r;
                    } else {
                        auto r{br.bits(7)};
                        if (!r) return inflate_error("unexpected end of input");
                        repeat = 11 + *r;
                    }
                    if (idx + repeat > lengths.size()) return inflate_error("invalid bit length repeat");
                    while (repeat-- > 0) lengths[idx++] = repeatValue;
                }
                if (lengths[256] == 0) return inflate_error("missing end-of-block code");

                Huffman litlen{};
                Huffman dist{};
                if (!litlen.build(std::span{lengths}.first(nlit)))
                    return inflate_error("invalid literal/lengths set");
                if (!dist.build(std::span{lengths}.subspan(nlit)))
                    return inflate_error("invalid distances set");
                if (auto err{inflate_block(br, litlen, dist, out)}) return inflate_error(*err);
                break;
            }
            default:
                return inflate_error("invalid block type");
        }
        if (*bfinal != 0) break;
    }
    br.align_to_byte();
    if (consumed != nullptr) *consumed = br.consumed();
    return out;
}

// ---------------------------------------------------------------------------
// Deflate (fixed-Huffman + greedy hash-chain LZ77; level 0 = stored blocks)
// ---------------------------------------------------------------------------

namespace {

class BitWriter {
public:
    explicit BitWriter(Bytes& out) : out_{out} {}

    void bits(std::uint32_t value, unsigned n) {  // LSB-first
        bitBuf_ |= static_cast<std::uint64_t>(value) << bitCount_;
        bitCount_ += n;
        while (bitCount_ >= 8) {
            out_.push_back(static_cast<std::uint8_t>(bitBuf_ & 0xFFU));
            bitBuf_ >>= 8;
            bitCount_ -= 8;
        }
    }

    // Huffman codes are packed MSB-of-code first (RFC 1951 §3.1.1).
    void huff(std::uint32_t code, unsigned n) {
        std::uint32_t rev{0};
        for (unsigned i{0}; i < n; ++i) rev |= ((code >> i) & 1U) << (n - 1 - i);
        bits(rev, n);
    }

    void flush() {
        if (bitCount_ > 0) {
            out_.push_back(static_cast<std::uint8_t>(bitBuf_ & 0xFFU));
            bitBuf_ = 0;
            bitCount_ = 0;
        }
    }

private:
    Bytes& out_;
    std::uint64_t bitBuf_{0};
    unsigned bitCount_{0};
};

// Fixed litlen code for symbol (RFC 1951 §3.2.6). Returns {code, bits}.
constexpr std::pair<std::uint32_t, unsigned> fixed_litlen_code(unsigned sym) {
    if (sym < 144) return {0x30 + sym, 8};
    if (sym < 256) return {0x190 + sym - 144, 9};
    if (sym < 280) return {sym - 256, 7};
    return {0xC0 + sym - 280, 8};
}

// Maps a match length (3..258) / distance (1..32768) to its symbol index.
unsigned length_symbol(std::size_t len) {
    unsigned s{28};
    while (s > 0 && LEN_BASE[s] > len) --s;
    // LEN_BASE[28]=258 owns exactly 258; entry 27 (base 227) covers 227..257.
    if (s == 28 && len < 258) s = 27;
    return s;
}

unsigned distance_symbol(std::size_t dist) {
    unsigned s{29};
    while (s > 0 && DIST_BASE[s] > dist) --s;
    return s;
}

void emit_length(BitWriter& bw, std::size_t len) {
    unsigned s{length_symbol(len)};
    auto [code, nbits]{fixed_litlen_code(257 + s)};
    bw.huff(code, nbits);
    if (LEN_EXTRA[s] != 0U) bw.bits(static_cast<std::uint32_t>(len - LEN_BASE[s]), LEN_EXTRA[s]);
}

void emit_distance(BitWriter& bw, std::size_t dist) {
    unsigned s{distance_symbol(dist)};
    bw.huff(s, 5);
    if (DIST_EXTRA[s] != 0U) bw.bits(static_cast<std::uint32_t>(dist - DIST_BASE[s]), DIST_EXTRA[s]);
}

Bytes deflate_stored(ByteView src) {
    Bytes out{};
    out.reserve(src.size() + src.size() / 65535 * 5 + 5);
    std::size_t pos{0};
    do {
        std::size_t chunk{std::min<std::size_t>(65535, src.size() - pos)};
        bool final{pos + chunk == src.size()};
        out.push_back(final ? 1 : 0);  // BFINAL + BTYPE=00 (byte-aligned)
        out.push_back(static_cast<std::uint8_t>(chunk & 0xFFU));
        out.push_back(static_cast<std::uint8_t>(chunk >> 8));
        out.push_back(static_cast<std::uint8_t>(~chunk & 0xFFU));
        out.push_back(static_cast<std::uint8_t>((~chunk >> 8) & 0xFFU));
        out.insert(out.end(), src.begin() + static_cast<std::ptrdiff_t>(pos),
                   src.begin() + static_cast<std::ptrdiff_t>(pos + chunk));
        pos += chunk;
    } while (pos < src.size());
    return out;
}

}  // namespace

// Raw DEFLATE (RFC 1951) compression. `level` 0 emits stored blocks; 1-9 tune
// the hash-chain search depth (fixed-Huffman emission throughout).
export Bytes deflate_raw(ByteView src, int level = 6) {
    if (level == 0) return deflate_stored(src);
    level = std::clamp(level, 1, 9);

    Bytes out{};
    out.reserve(src.size() / 2 + 64);
    BitWriter bw{out};
    bw.bits(1, 1);  // BFINAL
    bw.bits(1, 2);  // BTYPE=01 fixed Huffman

    // Greedy LZ77 with a 3-byte hash head + prev chain (bun blueprint: zlib's
    // deflate_fast path). Chain depth scales with level.
    const std::size_t maxChain{static_cast<std::size_t>(1) << (level + 2)};  // 8 .. 2048
    constexpr std::size_t HASH_SIZE{1U << 15};
    constexpr std::size_t HASH_MASK{HASH_SIZE - 1};
    auto hash3{[&src](std::size_t i) {
        return (static_cast<std::size_t>(src[i]) * 506832829U
                + static_cast<std::size_t>(src[i + 1]) * 65599U + src[i + 2])
               & HASH_MASK;
    }};
    std::vector<std::int32_t> head(HASH_SIZE, -1);
    std::vector<std::int32_t> prev(src.size(), -1);

    std::size_t i{0};
    while (i < src.size()) {
        std::size_t bestLen{0};
        std::size_t bestDist{0};
        if (i + MIN_MATCH <= src.size()) {
            std::size_t h{hash3(i)};
            std::int32_t cand{head[h]};
            std::size_t chain{maxChain};
            std::size_t limit{std::min(MAX_MATCH, src.size() - i)};
            while (cand >= 0 && chain-- > 0) {
                auto c{static_cast<std::size_t>(cand)};
                if (i - c > WINDOW_SIZE) break;
                if (src[c + bestLen] == src[i + bestLen] || bestLen == 0) {  // quick reject
                    std::size_t len{0};
                    while (len < limit && src[c + len] == src[i + len]) ++len;
                    if (len > bestLen) {
                        bestLen = len;
                        bestDist = i - c;
                        if (len >= limit) break;
                    }
                }
                cand = prev[c];
            }
            // Insert current position into the chain.
            prev[i] = head[h];
            head[h] = static_cast<std::int32_t>(i);
        }
        if (bestLen >= MIN_MATCH) {
            emit_length(bw, bestLen);
            emit_distance(bw, bestDist);
            // Register the skipped positions so later matches can point here.
            std::size_t end{std::min(i + bestLen, src.size() >= MIN_MATCH ? src.size() - MIN_MATCH + 1 : 0)};
            for (std::size_t j{i + 1}; j < end; ++j) {
                std::size_t h{hash3(j)};
                prev[j] = head[h];
                head[h] = static_cast<std::int32_t>(j);
            }
            i += bestLen;
        } else {
            auto [code, nbits]{fixed_litlen_code(src[i])};
            bw.huff(code, nbits);
            ++i;
        }
    }
    auto [eob, eobBits]{fixed_litlen_code(256)};
    bw.huff(eob, eobBits);
    bw.flush();
    return out;
}

// ---------------------------------------------------------------------------
// zlib container (RFC 1950)
// ---------------------------------------------------------------------------

export Bytes zlib_compress(ByteView src, int level = 6) {
    Bytes out{};
    // CMF 0x78 (deflate, 32K window); FLG chosen so (CMF<<8 | FLG) % 31 == 0.
    out.push_back(0x78);
    out.push_back(level >= 7 ? 0xDA : level >= 2 ? 0x9C : 0x01);
    Bytes body{deflate_raw(src, level)};
    out.insert(out.end(), body.begin(), body.end());
    std::uint32_t a{adler32(src)};
    out.push_back(static_cast<std::uint8_t>(a >> 24));
    out.push_back(static_cast<std::uint8_t>(a >> 16));
    out.push_back(static_cast<std::uint8_t>(a >> 8));
    out.push_back(static_cast<std::uint8_t>(a));
    return out;
}

export std::expected<Bytes, std::string> zlib_decompress(ByteView src) {
    if (src.size() < 6) return std::unexpected{"incorrect header check"};
    std::uint8_t cmf{src[0]};
    std::uint8_t flg{src[1]};
    if ((cmf & 0x0FU) != 8) return std::unexpected{"unknown compression method"};
    if ((static_cast<unsigned>(cmf) * 256 + flg) % 31 != 0)
        return std::unexpected{"incorrect header check"};
    if ((flg & 0x20U) != 0U) return std::unexpected{"preset dictionary not supported"};
    std::size_t consumed{0};
    auto body{inflate_raw(src.subspan(2), &consumed)};
    if (!body) return body;
    ByteView trailer{src.subspan(2 + consumed)};
    if (trailer.size() < 4) return std::unexpected{"unexpected end of input"};
    std::uint32_t expect{static_cast<std::uint32_t>(trailer[0]) << 24
                         | static_cast<std::uint32_t>(trailer[1]) << 16
                         | static_cast<std::uint32_t>(trailer[2]) << 8 | trailer[3]};
    if (adler32(*body) != expect) return std::unexpected{"incorrect data check"};
    return body;
}

// ---------------------------------------------------------------------------
// gzip container (RFC 1952)
// ---------------------------------------------------------------------------

export Bytes gzip_compress(ByteView src, int level = 6) {
    Bytes out{0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
              static_cast<std::uint8_t>(level >= 9 ? 2 : level == 1 ? 4 : 0), 0x03};
    Bytes body{deflate_raw(src, level)};
    out.insert(out.end(), body.begin(), body.end());
    std::uint32_t c{crc32(src)};
    auto push_le32{[&out](std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
        out.push_back(static_cast<std::uint8_t>(v >> 16));
        out.push_back(static_cast<std::uint8_t>(v >> 24));
    }};
    push_le32(c);
    push_le32(static_cast<std::uint32_t>(src.size()));
    return out;
}

// Handles multi-member streams (concatenated gzip files), matching zlib/node.
export std::expected<Bytes, std::string> gzip_decompress(ByteView src) {
    Bytes out{};
    ByteView rest{src};
    do {
        if (rest.size() < 18) return std::unexpected{"unexpected end of file"};
        if (rest[0] != 0x1F || rest[1] != 0x8B) return std::unexpected{"incorrect header check"};
        if (rest[2] != 8) return std::unexpected{"unknown compression method"};
        std::uint8_t flg{rest[3]};
        std::size_t off{10};
        if ((flg & 0x04U) != 0U) {  // FEXTRA
            if (rest.size() < off + 2) return std::unexpected{"unexpected end of file"};
            std::size_t xlen{static_cast<std::size_t>(rest[off]) | static_cast<std::size_t>(rest[off + 1]) << 8};
            off += 2 + xlen;
        }
        for (int f{0}; f < 2; ++f) {  // FNAME then FCOMMENT: NUL-terminated
            if ((flg & (f == 0 ? 0x08U : 0x10U)) == 0U) continue;
            while (off < rest.size() && rest[off] != 0) ++off;
            ++off;
        }
        if ((flg & 0x02U) != 0U) off += 2;  // FHCRC
        if (off >= rest.size()) return std::unexpected{"unexpected end of file"};

        std::size_t consumed{0};
        auto body{inflate_raw(rest.subspan(off), &consumed)};
        if (!body) return body;
        ByteView trailer{rest.subspan(off + consumed)};
        if (trailer.size() < 8) return std::unexpected{"unexpected end of file"};
        std::uint32_t expectCrc{static_cast<std::uint32_t>(trailer[0])
                                | static_cast<std::uint32_t>(trailer[1]) << 8
                                | static_cast<std::uint32_t>(trailer[2]) << 16
                                | static_cast<std::uint32_t>(trailer[3]) << 24};
        std::uint32_t expectSize{static_cast<std::uint32_t>(trailer[4])
                                 | static_cast<std::uint32_t>(trailer[5]) << 8
                                 | static_cast<std::uint32_t>(trailer[6]) << 16
                                 | static_cast<std::uint32_t>(trailer[7]) << 24};
        if (crc32(*body) != expectCrc) return std::unexpected{"incorrect data check"};
        if (static_cast<std::uint32_t>(body->size()) != expectSize)
            return std::unexpected{"incorrect length check"};
        out.insert(out.end(), body->begin(), body->end());
        rest = trailer.subspan(8);
    } while (!rest.empty());
    return out;
}

}  // namespace mbun::core::compress
