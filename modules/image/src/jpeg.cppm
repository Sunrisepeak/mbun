// jpeg.cppm — mbun.image.jpeg: baseline (sequential DCT) JPEG codec.
//
// Blueprint references (移植三段法 step 1: faithful translation of canonical
// baseline JPEG per ITU-T T.81 Annex K + NanoJPEG-style baseline decode):
//   - Encoder: 4:4:4, float FDCT, Annex-K standard quant + Huffman tables,
//     quality-scaled quantisation, JFIF APP0, optional APP2 ICC_PROFILE
//     segmentation (bun's codec_jpeg re-encode path; Sharp/mozjpeg default
//     order APP0 → APP2(ICC) → DQT → SOF0 → DHT → SOS).
//   - Decoder: baseline sequential only (SOF0), standard Huffman decode
//     (mincode/maxcode/valptr), float IDCT, arbitrary H/V subsampling with
//     replication upsample, APP2 ICC reassembly. No progressive / arithmetic
//     / restart-interval (mbun's encoder emits none).
//
// Behaviour pinned by the jpeg / ICC-profile parts of
// compat/bun/test/js/bun/image/image.test.ts.
export module mbun.image.jpeg;

import std;

namespace mbun::image {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

namespace jpg {

// Self-contained decode result (mirrors mbun::image::Decoded; kept local so the
// jpeg submodule stays leaf — the aggregator re-exports it without a cycle).
export struct JpegImage {
    Bytes rgba;
    std::uint32_t width{};
    std::uint32_t height{};
    std::optional<Bytes> iccProfile;
};

// Reject strings mirror mbun::image's (tests match /maxPixels/, /decode failed/).
constexpr std::string_view kErrDecode{"Image: decode failed"};
constexpr std::string_view kErrTooManyPixels{"Image: input exceeds maxPixels limit"};

// ─── shared constants ───────────────────────────────────────────────────────

// zig-zag scan order → natural (row-major) index.
constexpr std::array<int, 64> kZig{
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// Annex-K base quantisation tables (natural order).
constexpr std::array<int, 64> kQY{16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,
                                  58, 60, 55, 14, 13,  16,  24,  40,  57, 69, 56, 14, 17,
                                  22, 29, 51, 87, 80,  62,  18,  22,  37, 56, 68, 109, 103,
                                  77, 24, 35, 55, 64,  81,  104, 113, 92, 49, 64, 78, 87,
                                  103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
constexpr std::array<int, 64> kQC{17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99,
                                  99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66,
                                  99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                  99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                  99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// Standard Huffman table specs (Annex K.3): counts[16] + values.
constexpr std::array<std::uint8_t, 16> kDcLumaBits{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
constexpr std::array<std::uint8_t, 12> kDcLumaVal{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr std::array<std::uint8_t, 16> kDcChromaBits{0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
constexpr std::array<std::uint8_t, 12> kDcChromaVal{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

constexpr std::array<std::uint8_t, 16> kAcLumaBits{0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
constexpr std::array<std::uint8_t, 162> kAcLumaVal{
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
    0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52,
    0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
    0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3,
    0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
    0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

constexpr std::array<std::uint8_t, 16> kAcChromaBits{0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
constexpr std::array<std::uint8_t, 162> kAcChromaVal{
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61,
    0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33,
    0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
    0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
    0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

constexpr std::string_view kIccTag{"ICC_PROFILE"};  // followed by a NUL byte.

// ─── DCT (naive, exact; images in the test suite are tiny) ───────────────────

const std::array<std::array<double, 8>, 8>& dct_cos() {
    static const std::array<std::array<double, 8>, 8> t{[] {
        std::array<std::array<double, 8>, 8> c{};
        for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
                c[u][x] = std::cos((2.0 * x + 1.0) * u * std::numbers::pi / 16.0);
        return c;
    }()};
    return t;
}

// ─── encoder ────────────────────────────────────────────────────────────────

struct HuffEnc {
    std::array<std::uint16_t, 256> code{};
    std::array<std::uint8_t, 256> size{};
};

HuffEnc build_enc(const std::uint8_t* bits, const std::uint8_t* vals, int nvals) {
    HuffEnc h{};
    std::array<std::uint8_t, 257> huffsize{};
    int k{0};
    for (int l = 1; l <= 16; ++l)
        for (int i = 0; i < bits[l - 1]; ++i) huffsize[k++] = static_cast<std::uint8_t>(l);
    const int total{k};
    std::array<std::uint16_t, 257> huffcode{};
    std::uint16_t code{0};
    int si{huffsize[0]};
    k = 0;
    while (k < total) {
        while (k < total && huffsize[k] == si) huffcode[k++] = code++;
        code = static_cast<std::uint16_t>(code << 1);
        ++si;
    }
    for (int i = 0; i < nvals; ++i) {
        h.code[vals[i]] = huffcode[i];
        h.size[vals[i]] = huffsize[i];
    }
    return h;
}

struct BitWriter {
    Bytes& out;
    std::uint32_t buf{0};
    int cnt{0};
    void put(std::uint32_t code, int len) {
        buf = (buf << len) | (code & ((1u << len) - 1));
        cnt += len;
        while (cnt >= 8) {
            const auto b{static_cast<std::uint8_t>((buf >> (cnt - 8)) & 0xFF)};
            out.push_back(b);
            if (b == 0xFF) out.push_back(0x00);  // byte-stuffing
            cnt -= 8;
        }
    }
    void flush() {  // pad the final partial byte with 1-bits.
        if (cnt > 0) {
            const int pad{8 - cnt};
            const auto b{static_cast<std::uint8_t>(((buf << pad) | ((1u << pad) - 1)) & 0xFF)};
            out.push_back(b);
            if (b == 0xFF) out.push_back(0x00);
            cnt = 0;
        }
    }
};

int magnitude(int v) {
    int a{std::abs(v)};
    int n{0};
    while (a) {
        ++n;
        a >>= 1;
    }
    return n;
}

void encode_block(BitWriter& bw, const double* px, const std::array<int, 64>& qt,
                  const HuffEnc& dc, const HuffEnc& ac, int& pred) {
    const auto& C{dct_cos()};
    std::array<int, 64> q{};
    for (int v = 0; v < 8; ++v) {
        for (int u = 0; u < 8; ++u) {
            double s{0.0};
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) s += px[y * 8 + x] * C[u][x] * C[v][y];
            const double cu{u == 0 ? std::numbers::sqrt2 / 2.0 : 1.0};
            const double cv{v == 0 ? std::numbers::sqrt2 / 2.0 : 1.0};
            const double f{0.25 * cu * cv * s};
            const int idx{v * 8 + u};
            q[idx] = static_cast<int>(std::lround(f / qt[idx]));
        }
    }
    // DC coefficient (differential).
    const int diff{q[0] - pred};
    pred = q[0];
    const int sdc{magnitude(diff)};
    bw.put(dc.code[sdc], dc.size[sdc]);
    if (sdc) bw.put(diff < 0 ? diff - 1 : diff, sdc);
    // AC coefficients (run-length over the zig-zag scan).
    int run{0};
    for (int k = 1; k < 64; ++k) {
        const int val{q[kZig[k]]};
        if (val == 0) {
            ++run;
            continue;
        }
        while (run > 15) {
            bw.put(ac.code[0xF0], ac.size[0xF0]);  // ZRL
            run -= 16;
        }
        const int s{magnitude(val)};
        const int rs{(run << 4) | s};
        bw.put(ac.code[rs], ac.size[rs]);
        bw.put(val < 0 ? val - 1 : val, s);
        run = 0;
    }
    if (run > 0) bw.put(ac.code[0x00], ac.size[0x00]);  // EOB
}

void put_marker_len(Bytes& o, std::uint8_t m, std::uint16_t payload) {
    o.push_back(0xFF);
    o.push_back(m);
    o.push_back(static_cast<std::uint8_t>((payload + 2) >> 8));
    o.push_back(static_cast<std::uint8_t>((payload + 2) & 0xFF));
}

std::array<int, 64> scale_qt(const std::array<int, 64>& base, int quality) {
    quality = std::clamp(quality, 1, 100);
    const int S{quality < 50 ? 5000 / quality : 200 - 2 * quality};
    std::array<int, 64> out{};
    for (int i = 0; i < 64; ++i)
        out[i] = std::clamp((base[i] * S + 50) / 100, 1, 255);
    return out;
}

export Bytes jpeg_encode_rgba(ByteView rgba, std::uint32_t w, std::uint32_t h, int quality,
                              bool /*progressive*/, const Bytes* icc) {
    const auto qY{scale_qt(kQY, quality)};
    const auto qC{scale_qt(kQC, quality)};
    Bytes o{};
    o.reserve(static_cast<std::size_t>(w) * h + 1024);
    // SOI.
    o.push_back(0xFF);
    o.push_back(0xD8);
    // APP0 JFIF (payload: "JFIF\0"(5) + version(2) + units(1) + Xdens(2) +
    // Ydens(2) + Xthumb(1) + Ythumb(1) = 14).
    put_marker_len(o, 0xE0, 14);
    for (char c : std::string_view{"JFIF\0", 5}) o.push_back(static_cast<std::uint8_t>(c));
    o.insert(o.end(), {0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00});
    // APP2 ICC (split into ≤65519-byte chunks).
    if (icc != nullptr && !icc->empty()) {
        constexpr std::size_t kMax{65519};
        const std::size_t total{(icc->size() + kMax - 1) / kMax};
        for (std::size_t seq = 0; seq < total; ++seq) {
            const std::size_t off{seq * kMax};
            const std::size_t n{std::min(kMax, icc->size() - off)};
            const auto payload{static_cast<std::uint16_t>(kIccTag.size() + 1 + 2 + n)};
            put_marker_len(o, 0xE2, payload);
            for (char c : kIccTag) o.push_back(static_cast<std::uint8_t>(c));
            o.push_back(0x00);
            o.push_back(static_cast<std::uint8_t>(seq + 1));
            o.push_back(static_cast<std::uint8_t>(total));
            o.insert(o.end(), icc->begin() + static_cast<std::ptrdiff_t>(off),
                     icc->begin() + static_cast<std::ptrdiff_t>(off + n));
        }
    }
    // DQT (two tables, zig-zag order).
    for (int t = 0; t < 2; ++t) {
        put_marker_len(o, 0xDB, 65);
        o.push_back(static_cast<std::uint8_t>(t));  // precision 0, id t
        const auto& q{t == 0 ? qY : qC};
        for (int i = 0; i < 64; ++i) o.push_back(static_cast<std::uint8_t>(q[kZig[i]]));
    }
    // SOF0 (baseline, 3 components, 4:4:4).
    put_marker_len(o, 0xC0, 15);
    o.push_back(0x08);  // precision
    o.push_back(static_cast<std::uint8_t>(h >> 8));
    o.push_back(static_cast<std::uint8_t>(h & 0xFF));
    o.push_back(static_cast<std::uint8_t>(w >> 8));
    o.push_back(static_cast<std::uint8_t>(w & 0xFF));
    o.push_back(0x03);
    o.insert(o.end(), {0x01, 0x11, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01});
    // DHT (4 tables).
    auto emit_dht = [&](std::uint8_t cls_id, const std::uint8_t* bits, const std::uint8_t* vals,
                        int nvals) {
        put_marker_len(o, 0xC4, static_cast<std::uint16_t>(1 + 16 + nvals));
        o.push_back(cls_id);
        o.insert(o.end(), bits, bits + 16);
        o.insert(o.end(), vals, vals + nvals);
    };
    emit_dht(0x00, kDcLumaBits.data(), kDcLumaVal.data(), kDcLumaVal.size());
    emit_dht(0x10, kAcLumaBits.data(), kAcLumaVal.data(), kAcLumaVal.size());
    emit_dht(0x01, kDcChromaBits.data(), kDcChromaVal.data(), kDcChromaVal.size());
    emit_dht(0x11, kAcChromaBits.data(), kAcChromaVal.data(), kAcChromaVal.size());
    // SOS.
    put_marker_len(o, 0xDA, 10);
    o.push_back(0x03);
    o.insert(o.end(), {0x01, 0x00, 0x02, 0x11, 0x03, 0x11});
    o.insert(o.end(), {0x00, 0x3F, 0x00});
    // Entropy-coded data.
    const HuffEnc dcY{build_enc(kDcLumaBits.data(), kDcLumaVal.data(), kDcLumaVal.size())};
    const HuffEnc acY{build_enc(kAcLumaBits.data(), kAcLumaVal.data(), kAcLumaVal.size())};
    const HuffEnc dcC{build_enc(kDcChromaBits.data(), kDcChromaVal.data(), kDcChromaVal.size())};
    const HuffEnc acC{build_enc(kAcChromaBits.data(), kAcChromaVal.data(), kAcChromaVal.size())};
    BitWriter bw{o};
    int predY{0};
    int predCb{0};
    int predCr{0};
    std::array<double, 64> bY{};
    std::array<double, 64> bCb{};
    std::array<double, 64> bCr{};
    for (std::uint32_t my = 0; my < h; my += 8) {
        for (std::uint32_t mx = 0; mx < w; mx += 8) {
            for (int yy = 0; yy < 8; ++yy) {
                const std::uint32_t sy{std::min(my + yy, h - 1)};
                for (int xx = 0; xx < 8; ++xx) {
                    const std::uint32_t sx{std::min(mx + xx, w - 1)};
                    const std::size_t p{(static_cast<std::size_t>(sy) * w + sx) * 4};
                    const double r{static_cast<double>(rgba[p])};
                    const double g{static_cast<double>(rgba[p + 1])};
                    const double b{static_cast<double>(rgba[p + 2])};
                    const int idx{yy * 8 + xx};
                    bY[idx] = 0.299 * r + 0.587 * g + 0.114 * b - 128.0;
                    bCb[idx] = -0.168736 * r - 0.331264 * g + 0.5 * b;
                    bCr[idx] = 0.5 * r - 0.418688 * g - 0.081312 * b;
                }
            }
            encode_block(bw, bY.data(), qY, dcY, acY, predY);
            encode_block(bw, bCb.data(), qC, dcC, acC, predCb);
            encode_block(bw, bCr.data(), qC, dcC, acC, predCr);
        }
    }
    bw.flush();
    // EOI.
    o.push_back(0xFF);
    o.push_back(0xD9);
    return o;
}

// ─── decoder ────────────────────────────────────────────────────────────────

struct HuffDec {
    std::array<int, 17> mincode{};
    std::array<int, 17> maxcode{};  // -1 when empty
    std::array<int, 17> valptr{};
    std::array<std::uint8_t, 256> huffval{};
    int count{0};
    bool present{false};
};

void build_dec(HuffDec& h, const std::uint8_t* counts, const std::uint8_t* vals) {
    int total{0};
    for (int l = 0; l < 16; ++l) total += counts[l];
    for (int i = 0; i < total; ++i) h.huffval[i] = vals[i];
    h.count = total;
    int code{0};
    int k{0};
    for (int l = 1; l <= 16; ++l) {
        if (counts[l - 1] == 0) {
            h.maxcode[l] = -1;
        } else {
            h.valptr[l] = k;
            h.mincode[l] = code;
            code += counts[l - 1];
            h.maxcode[l] = code - 1;
            k += counts[l - 1];
        }
        code <<= 1;
    }
    h.present = true;
}

struct Comp {
    int id{};
    int h{1};
    int v{1};
    int qt{};
    int dc{};
    int ac{};
    int pred{0};
    int cw{};
    int ch{};
    Bytes plane;
};

struct BitReader {
    ByteView d;
    std::size_t pos{0};
    std::uint32_t buf{0};
    int cnt{0};
    std::uint32_t show(int nbits) {
        while (cnt < nbits) {
            std::uint8_t nb{0xFF};
            if (pos < d.size()) {
                nb = d[pos];
                if (nb == 0xFF) {
                    const std::uint8_t m{pos + 1 < d.size() ? d[pos + 1] : static_cast<std::uint8_t>(0xD9)};
                    if (m == 0x00) {
                        pos += 2;  // stuffed literal 0xFF
                    } else {
                        pos = d.size();  // hit a marker: pad the tail with 1s
                        nb = 0xFF;
                    }
                } else {
                    ++pos;
                }
            }
            buf = (buf << 8) | nb;
            cnt += 8;
        }
        return (buf >> (cnt - nbits)) & ((1u << nbits) - 1);
    }
    std::uint32_t get(int nbits) {
        if (nbits == 0) return 0;
        const std::uint32_t v{show(nbits)};
        cnt -= nbits;
        return v;
    }
};

int huff_decode(BitReader& br, const HuffDec& h) {
    int code{0};
    for (int l = 1; l <= 16; ++l) {
        code = (code << 1) | static_cast<int>(br.get(1));
        if (h.maxcode[l] >= 0 && code <= h.maxcode[l]) {
            const int idx{h.valptr[l] + code - h.mincode[l]};
            return (idx >= 0 && idx < 256) ? h.huffval[idx] : 0;
        }
    }
    return 0;
}

int recv_extend(BitReader& br, int s) {
    if (s == 0) return 0;
    int v{static_cast<int>(br.get(s))};
    if (v < (1 << (s - 1))) v += (-(1 << s) + 1);
    return v;
}

void idct_block(const std::array<int, 64>& coef, std::array<std::uint8_t, 64>& out) {
    const auto& C{dct_cos()};
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            double s{0.0};
            for (int v = 0; v < 8; ++v) {
                for (int u = 0; u < 8; ++u) {
                    const double cu{u == 0 ? std::numbers::sqrt2 / 2.0 : 1.0};
                    const double cv{v == 0 ? std::numbers::sqrt2 / 2.0 : 1.0};
                    s += cu * cv * coef[v * 8 + u] * C[u][x] * C[v][y];
                }
            }
            const int val{static_cast<int>(std::lround(0.25 * s)) + 128};
            out[y * 8 + x] = static_cast<std::uint8_t>(std::clamp(val, 0, 255));
        }
    }
}

std::uint16_t rd16(ByteView b, std::size_t i) {
    return static_cast<std::uint16_t>(b[i]) << 8 | b[i + 1];
}

export std::optional<std::pair<std::uint32_t, std::uint32_t>> jpeg_probe(ByteView b) {
    std::size_t p{2};  // skip SOI
    while (p + 4 <= b.size()) {
        if (b[p] != 0xFF) {
            ++p;
            continue;
        }
        const std::uint8_t m{b[p + 1]};
        if (m == 0xD8 || m == 0xD9 || (m >= 0xD0 && m <= 0xD7)) {
            p += 2;
            continue;
        }
        const std::uint16_t len{rd16(b, p + 2)};
        if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (p + 9 > b.size()) return std::nullopt;
            const std::uint16_t hh{rd16(b, p + 5)};
            const std::uint16_t ww{rd16(b, p + 7)};
            return std::pair<std::uint32_t, std::uint32_t>{ww, hh};
        }
        p += 2 + len;
    }
    return std::nullopt;
}

export std::expected<JpegImage, std::string> jpeg_decode(ByteView b,
                                                                    std::uint64_t maxPixels) {
    std::array<std::array<int, 64>, 4> qt{};
    std::array<HuffDec, 4> dcTab{};
    std::array<HuffDec, 4> acTab{};
    std::vector<Comp> comps;
    std::uint32_t W{0};
    std::uint32_t H{0};
    // ICC reassembly (seq → chunk).
    std::array<Bytes, 256> iccChunks{};
    int iccTotal{0};

    std::size_t p{2};
    bool sofSeen{false};
    while (p + 2 <= b.size()) {
        if (b[p] != 0xFF) {
            ++p;
            continue;
        }
        const std::uint8_t m{b[p + 1]};
        if (m == 0xD9) break;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            p += 2;
            continue;
        }
        if (p + 4 > b.size()) break;
        const std::uint16_t len{rd16(b, p + 2)};
        if (len < 2) return std::unexpected(std::string{kErrDecode});  // guards segLen underflow
        const std::size_t seg{p + 4};
        const std::size_t segLen{static_cast<std::size_t>(len) - 2};
        if (seg + segLen > b.size()) return std::unexpected(std::string{kErrDecode});
        if (m == 0xDB) {  // DQT
            std::size_t q{seg};
            while (q < seg + segLen) {
                const int pq{b[q] >> 4};
                const int tq{b[q] & 0x0F};
                ++q;
                if (tq > 3) return std::unexpected(std::string{kErrDecode});
                // Each table reads 64 entries (128 bytes at 16-bit precision);
                // reject a segment that cannot hold them.
                if (q + static_cast<std::size_t>(pq ? 128 : 64) > seg + segLen)
                    return std::unexpected(std::string{kErrDecode});
                for (int i = 0; i < 64; ++i) {
                    int v{};
                    if (pq) {
                        v = rd16(b, q);
                        q += 2;
                    } else {
                        v = b[q++];
                    }
                    qt[tq][kZig[i]] = v;
                }
            }
        } else if (m == 0xC0 || m == 0xC1) {  // baseline / extended sequential
            // Frame header: precision(1) H(2) W(2) Nf(1) then Nf*3 component specs.
            if (segLen < 6) return std::unexpected(std::string{kErrDecode});
            sofSeen = true;
            H = rd16(b, seg + 1);
            W = rd16(b, seg + 3);
            const int nc{b[seg + 5]};
            // Corrupt Nf (e.g. the offset-167 flip 3→0xFC) would run the reader
            // far past the buffer — bound it to what the segment actually holds
            // and to the 1/3-component shapes the composer supports.
            if (nc < 1 || nc > 4
                || static_cast<std::size_t>(6) + static_cast<std::size_t>(nc) * 3 > segLen)
                return std::unexpected(std::string{kErrDecode});
            std::size_t q{seg + 6};
            for (int i = 0; i < nc; ++i) {
                Comp c{};
                c.id = b[q];
                c.h = b[q + 1] >> 4;
                c.v = b[q + 1] & 0x0F;
                c.qt = b[q + 2];
                q += 3;
                // Sampling factors index/scale the plane geometry; quant index
                // selects qt[c.qt] (size 4) in the entropy loop — both must be
                // in range or the decode reads/writes out of bounds later.
                if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4 || c.qt > 3)
                    return std::unexpected(std::string{kErrDecode});
                comps.push_back(c);
            }
        } else if (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            return std::unexpected(std::string{"ERR_IMAGE_FORMAT_UNSUPPORTED: Image: progressive "
                                               "JPEG decode requires macOS or Windows"});
        } else if (m == 0xC4) {  // DHT
            std::size_t q{seg};
            while (q < seg + segLen) {
                const int tc{b[q] >> 4};
                const int th{b[q] & 0x0F};
                ++q;
                // Need class(0/1), slot(≤3) and 16 count bytes present.
                if (tc > 1 || th > 3 || q + 16 > seg + segLen)
                    return std::unexpected(std::string{kErrDecode});
                std::array<std::uint8_t, 16> counts{};
                int total{0};
                for (int i = 0; i < 16; ++i) {
                    counts[i] = b[q + i];
                    total += counts[i];
                }
                q += 16;
                // huffval holds 256 symbols; the value bytes must fit the segment.
                if (total > 256 || q + static_cast<std::size_t>(total) > seg + segLen)
                    return std::unexpected(std::string{kErrDecode});
                if (tc == 0)
                    build_dec(dcTab[th], counts.data(), &b[q]);
                else
                    build_dec(acTab[th], counts.data(), &b[q]);
                q += total;
            }
        } else if (m == 0xE2) {  // APP2 — maybe ICC_PROFILE
            if (segLen > kIccTag.size() + 3
                && std::equal(kIccTag.begin(), kIccTag.end(), b.begin() + seg)
                && b[seg + kIccTag.size()] == 0x00) {
                const std::size_t hdr{kIccTag.size() + 1};
                const int seqno{b[seg + hdr]};
                iccTotal = b[seg + hdr + 1];
                const std::size_t off{seg + hdr + 2};
                iccChunks[seqno].assign(b.begin() + off, b.begin() + seg + segLen);
            }
        } else if (m == 0xDA) {  // SOS
            // Scan header: Ns(1) then Ns*(component-id, Td|Ta).
            if (segLen < 1) return std::unexpected(std::string{kErrDecode});
            const int ns{b[seg]};
            if (ns < 1 || static_cast<std::size_t>(1) + static_cast<std::size_t>(ns) * 2 > segLen)
                return std::unexpected(std::string{kErrDecode});
            std::size_t q{seg + 1};
            for (int i = 0; i < ns; ++i) {
                const int cid{b[q]};
                const int td{b[q + 1] >> 4};
                const int ta{b[q + 1] & 0x0F};
                q += 2;
                // Td/Ta index dcTab/acTab (size 4) in the entropy loop.
                if (td > 3 || ta > 3) return std::unexpected(std::string{kErrDecode});
                for (auto& c : comps)
                    if (c.id == cid) {
                        c.dc = td;
                        c.ac = ta;
                    }
            }
            p = seg + segLen;  // entropy data starts right after the SOS header
            break;
        }
        p = seg + segLen;
    }

    if (!sofSeen || comps.empty() || W == 0 || H == 0)
        return std::unexpected(std::string{kErrDecode});
    // The composer handles greyscale (1) or YCbCr (3); other counts would index
    // comps[1]/comps[2] out of bounds.
    if (comps.size() != 1 && comps.size() != 3)
        return std::unexpected(std::string{kErrDecode});
    if (static_cast<std::uint64_t>(W) * H > maxPixels)
        return std::unexpected(std::string{kErrTooManyPixels});

    int hmax{1};
    int vmax{1};
    for (const auto& c : comps) {
        hmax = std::max(hmax, c.h);
        vmax = std::max(vmax, c.v);
    }
    const int mcuW{8 * hmax};
    const int mcuH{8 * vmax};
    const int mcusX{static_cast<int>((W + mcuW - 1) / mcuW)};
    const int mcusY{static_cast<int>((H + mcuH - 1) / mcuH)};
    for (auto& c : comps) {
        c.cw = mcusX * c.h * 8;
        c.ch = mcusY * c.v * 8;
        c.plane.assign(static_cast<std::size_t>(c.cw) * c.ch, 0);
    }

    BitReader br{ByteView{b.begin() + p, b.end()}};
    std::array<int, 64> coef{};
    std::array<std::uint8_t, 64> blk{};
    for (int my = 0; my < mcusY; ++my) {
        for (int mx = 0; mx < mcusX; ++mx) {
            for (auto& c : comps) {
                for (int by = 0; by < c.v; ++by) {
                    for (int bx = 0; bx < c.h; ++bx) {
                        coef.fill(0);
                        const auto& Q{qt[c.qt]};
                        const int s{huff_decode(br, dcTab[c.dc])};
                        const int diff{recv_extend(br, s)};
                        c.pred += diff;
                        coef[0] = c.pred * Q[0];
                        int k{1};
                        while (k < 64) {
                            const int rs{huff_decode(br, acTab[c.ac])};
                            const int r{rs >> 4};
                            const int sz{rs & 0x0F};
                            if (sz == 0) {
                                if (r == 15) {
                                    k += 16;
                                    continue;
                                }
                                break;
                            }
                            k += r;
                            if (k > 63) break;
                            const int val{recv_extend(br, sz)};
                            coef[kZig[k]] = val * Q[kZig[k]];
                            ++k;
                        }
                        idct_block(coef, blk);
                        const int ox{(mx * c.h + bx) * 8};
                        const int oy{(my * c.v + by) * 8};
                        for (int yy = 0; yy < 8; ++yy)
                            for (int xx = 0; xx < 8; ++xx)
                                c.plane[static_cast<std::size_t>(oy + yy) * c.cw + ox + xx] =
                                    blk[yy * 8 + xx];
                    }
                }
            }
        }
    }

    // Compose RGBA with replication upsampling.
    JpegImage out{};
    out.width = W;
    out.height = H;
    out.rgba.assign(static_cast<std::size_t>(W) * H * 4, 255);
    const bool gray{comps.size() == 1};
    for (std::uint32_t y = 0; y < H; ++y) {
        for (std::uint32_t x = 0; x < W; ++x) {
            auto sample{[&](const Comp& c) -> int {
                const int sx{static_cast<int>(x) * c.h / hmax};
                const int sy{static_cast<int>(y) * c.v / vmax};
                return c.plane[static_cast<std::size_t>(sy) * c.cw + sx];
            }};
            int R{};
            int G{};
            int B{};
            if (gray) {
                R = G = B = sample(comps[0]);
            } else {
                const double Y{static_cast<double>(sample(comps[0]))};
                const double Cb{static_cast<double>(sample(comps[1])) - 128.0};
                const double Cr{static_cast<double>(sample(comps[2])) - 128.0};
                R = static_cast<int>(std::lround(Y + 1.402 * Cr));
                G = static_cast<int>(std::lround(Y - 0.344136 * Cb - 0.714136 * Cr));
                B = static_cast<int>(std::lround(Y + 1.772 * Cb));
            }
            const std::size_t o{(static_cast<std::size_t>(y) * W + x) * 4};
            out.rgba[o] = static_cast<std::uint8_t>(std::clamp(R, 0, 255));
            out.rgba[o + 1] = static_cast<std::uint8_t>(std::clamp(G, 0, 255));
            out.rgba[o + 2] = static_cast<std::uint8_t>(std::clamp(B, 0, 255));
            out.rgba[o + 3] = 255;
        }
    }
    // Reassemble ICC profile.
    if (iccTotal > 0) {
        Bytes icc;
        for (int i = 1; i <= iccTotal; ++i)
            icc.insert(icc.end(), iccChunks[i].begin(), iccChunks[i].end());
        if (!icc.empty()) out.iccProfile = std::move(icc);
    }
    return out;
}

}  // namespace jpg
}  // namespace mbun::image
