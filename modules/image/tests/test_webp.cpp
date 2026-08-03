// WebP translation-seam tests derived from Bun's codec_webp.rs/.zig flow.
// These only pin container probing, allocation guards, and the explicit
// native-codec deferral; libwebp pixel and ICCP behavior remains deferred.
import std;
import mbun.image.webp;

namespace {

namespace webp = mbun::image::webp;
using Bytes = std::vector<std::uint8_t>;

int gChecks{};
int gFailures{};

void check(bool condition, std::string_view label) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::println("  FAIL {}", label);
    }
}

Bytes riff(std::string_view chunk, std::size_t payloadSize) {
    Bytes bytes(20 + payloadSize);
    std::ranges::copy(std::string_view{"RIFF"}, bytes.begin());
    const auto riffSize{static_cast<std::uint32_t>(bytes.size() - 8)};
    for (std::size_t i{}; i < 4; ++i)
        bytes[4 + i] = static_cast<std::uint8_t>(riffSize >> (i * 8));
    std::ranges::copy(std::string_view{"WEBP"}, bytes.begin() + 8);
    std::ranges::copy(chunk, bytes.begin() + 12);
    const auto chunkSize{static_cast<std::uint32_t>(payloadSize)};
    for (std::size_t i{}; i < 4; ++i)
        bytes[16 + i] = static_cast<std::uint8_t>(chunkSize >> (i * 8));
    return bytes;
}

Bytes vp8(std::uint16_t width, std::uint16_t height) {
    Bytes bytes{riff("VP8 ", 10)};
    bytes[23] = 0x9D;
    bytes[24] = 0x01;
    bytes[25] = 0x2A;
    bytes[26] = static_cast<std::uint8_t>(width);
    bytes[27] = static_cast<std::uint8_t>(width >> 8);
    bytes[28] = static_cast<std::uint8_t>(height);
    bytes[29] = static_cast<std::uint8_t>(height >> 8);
    return bytes;
}

Bytes vp8l(std::uint32_t width, std::uint32_t height, bool hasAlpha) {
    Bytes bytes{riff("VP8L", 5)};
    bytes[20] = 0x2F;
    const std::uint32_t bits{(width - 1) | ((height - 1) << 14)
                             | (static_cast<std::uint32_t>(hasAlpha) << 28)};
    for (std::size_t i{}; i < 4; ++i)
        bytes[21 + i] = static_cast<std::uint8_t>(bits >> (i * 8));
    return bytes;
}

Bytes vp8x(std::uint32_t width, std::uint32_t height, std::uint8_t flags) {
    Bytes bytes{riff("VP8X", 10)};
    bytes[20] = flags;
    const auto write24{[&](std::size_t offset, std::uint32_t value) {
        --value;
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    }};
    write24(24, width);
    write24(27, height);
    return bytes;
}

}  // namespace

int main() {
    {
        const auto p{webp::probe(vp8(320, 200))};
        check(p && p->width == 320 && p->height == 200
                  && p->bitstream == webp::BitstreamKind::Lossy,
              "VP8 key-frame dimensions");
    }
    {
        const auto p{webp::probe(vp8l(17, 33, true))};
        check(p && p->width == 17 && p->height == 33 && p->hasAlpha
                  && p->bitstream == webp::BitstreamKind::Lossless,
              "VP8L dimensions and alpha hint");
    }
    {
        const auto p{webp::probe(vp8x(640, 480, 0x30))};
        check(p && p->width == 640 && p->height == 480 && p->hasAlpha
                  && p->hasIccProfile && p->bitstream == webp::BitstreamKind::Extended,
              "VP8X canvas dimensions");
    }
    {
        Bytes badVp8{vp8(1, 1)};
        badVp8[23] = 0;
        Bytes badVp8l{vp8l(1, 1, false)};
        badVp8l[24] = 0x20;
        Bytes badVp8x{vp8x(1, 1, 0)};
        badVp8x[21] = 1;
        check(!webp::probe(badVp8), "VP8 start code is validated");
        check(!webp::probe(badVp8l), "VP8L version is validated");
        check(!webp::probe(badVp8x), "VP8X reserved bytes are validated");
    }
    {
        // A header-only synthetic VP8X has no image data — the maxPixels guard
        // still fires from the probe dimensions before libwebp is invoked, and
        // the real decoder rejects the empty bitstream as a decode failure.
        const Bytes input{vp8x(100, 100, 0)};
        const auto guarded{webp::decode(input, 9'999)};
        const auto rejected{webp::decode(input, 10'000)};
        check(!guarded && guarded.error() == webp::Error::TooManyPixels,
              "maxPixels precedes native decode");
        check(!rejected && rejected.error() == webp::Error::DecodeFailed,
              "libwebp rejects an empty bitstream");
    }
    {
        const Bytes rgba(4 * 3 * 4);
        const auto invalid{webp::encode(rgba, 4, 4)};
        check(!invalid && invalid.error() == webp::Error::EncodeFailed,
              "encoder validates RGBA extent");
    }
    {
        // Real round-trip: build a 6x5 RGBA gradient, lossless-encode, decode,
        // and verify dimensions and every pixel come back bit-exact.
        constexpr std::uint32_t W{6};
        constexpr std::uint32_t H{5};
        Bytes src(static_cast<std::size_t>(W) * H * 4);
        for (std::uint32_t y{0}; y < H; ++y)
            for (std::uint32_t x{0}; x < W; ++x) {
                const std::size_t p{(static_cast<std::size_t>(y) * W + x) * 4};
                src[p + 0] = static_cast<std::uint8_t>(x * 40);
                src[p + 1] = static_cast<std::uint8_t>(y * 50);
                src[p + 2] = static_cast<std::uint8_t>((x + y) * 20);
                src[p + 3] = static_cast<std::uint8_t>(255 - x * 10);
            }
        const auto enc{webp::encode(src, W, H, {.lossless = true})};
        check(enc.has_value() && !enc->bytes.empty(), "lossless encode produces bytes");
        if (enc) {
            const auto probed{webp::probe(enc->bytes)};
            check(probed && probed->width == W && probed->height == H,
                  "encoded bytes probe to source dimensions");
            const auto dec{webp::decode(enc->bytes, 1'000'000)};
            check(dec.has_value() && dec->width == W && dec->height == H,
                  "decode recovers dimensions");
            check(dec && dec->rgba == src, "lossless round-trip is bit-exact");
        }
    }

    std::println("webp seam tests: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
