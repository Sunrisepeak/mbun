// webp_native.cppm — real libwebp backend that fills the mbun.image.webp seam.
//
// PORT-SOURCE: bun src/runtime/image/codec_webp.rs — WebPDecodeRGBA for decode,
// WebPEncodeRGBA (lossy) / WebPEncodeLosslessRGBA (lossless) for encode. bun
// links libwebp; mbun links the ambient system libwebp (+libsharpyuv) declared
// via the image module's ldflags. The pure-logic layer (probe/guards/data
// model) stays in webp.cppm; this leaf module includes <webp/decode.h> and
// <webp/encode.h> in a global module fragment and drives the live C API.
//
// ICCP demux/mux (WebPDemux*/WebPMux*) remains DEFERRED — the round-trip path
// bun's Image pipeline exercises is the flat RGBA encode/decode wired here.
module;

#include <webp/decode.h>
#include <webp/encode.h>

export module mbun.image.webp.native;

import std;

namespace mbun::image::webp::native_backend {

using Bytes = std::vector<std::uint8_t>;

// Decode any libwebp-supported bitstream (VP8/VP8L/VP8X) to RGBA8. Returns the
// pixel buffer plus dimensions, or nullopt when libwebp rejects the input.
export struct DecodeOut {
    Bytes rgba;
    std::uint32_t width{};
    std::uint32_t height{};
};

export std::optional<DecodeOut> decode(std::span<const std::uint8_t> bytes) {
    int w{0};
    int h{0};
    std::uint8_t* pixels{WebPDecodeRGBA(bytes.data(), bytes.size(), &w, &h)};
    if (pixels == nullptr || w <= 0 || h <= 0) {
        if (pixels != nullptr) WebPFree(pixels);
        return std::nullopt;
    }
    DecodeOut out{};
    out.width = static_cast<std::uint32_t>(w);
    out.height = static_cast<std::uint32_t>(h);
    const std::size_t n{static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4};
    out.rgba.assign(pixels, pixels + n);
    WebPFree(pixels);
    return out;
}

// Encode RGBA8. `lossless` selects WebPEncodeLosslessRGBA (bit-exact round-trip);
// otherwise WebPEncodeRGBA at the given quality [0,100]. Returns nullopt on
// libwebp failure. Stride is width*4 (tightly packed, no row padding).
export std::optional<Bytes> encode(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                   std::uint32_t height, bool lossless, float quality) {
    const int w{static_cast<int>(width)};
    const int h{static_cast<int>(height)};
    const int stride{w * 4};
    std::uint8_t* out{nullptr};
    std::size_t size{0};
    if (lossless) {
        size = WebPEncodeLosslessRGBA(rgba.data(), w, h, stride, &out);
    } else {
        size = WebPEncodeRGBA(rgba.data(), w, h, stride, quality, &out);
    }
    if (out == nullptr || size == 0) {
        if (out != nullptr) WebPFree(out);
        return std::nullopt;
    }
    Bytes bytes(out, out + size);
    WebPFree(out);
    return bytes;
}

}  // namespace mbun::image::webp::native_backend
