// webp.cppm — mbun.image.webp: WebP codec seam.
//
// Blueprint references (移植三段法 step 1):
//   - bun src/runtime/image/codec_webp.rs — WebPGetInfo probe, RGBA decode,
//     lossy/lossless encode, and ICCP demux/mux ownership boundaries.
//   - bun src/runtime/image/codecs.rs — Decoded/Error/EncodeOptions model,
//     maxPixels guard before allocating the decoded canvas.
//   - Zig counterpart: src/runtime/image/codec_webp.zig and codecs.zig.
//
// libwebp linkage and the pixel codec are DEFERRED. This file intentionally
// establishes the portable data model, RIFF/VP8X probe, and native ABI seam
// without changing the existing PNG/BMP/GIF/JPEG implementations.
export module mbun.image.webp;

import std;
import mbun.image.webp.native;  // real libwebp encode/decode backend

namespace mbun::image::webp {

namespace backend = mbun::image::webp::native_backend;

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

export enum class Error {
    DecodeFailed,
    EncodeFailed,
    TooManyPixels,
    OutOfMemory,
    NativeCodecDeferred,
};

export enum class BitstreamKind {
    Lossy,
    Lossless,
    Extended,
};

export struct Probe {
    std::uint32_t width{};
    std::uint32_t height{};
    bool hasAlpha{};
    bool hasIccProfile{};
    BitstreamKind bitstream{BitstreamKind::Extended};
};

// Shared image payload shape follows codecs::Decoded. The ICCP bytes are
// copied out of the demux iterator because that iterator borrows the input.
export struct Decoded {
    Bytes rgba;
    std::uint32_t width{};
    std::uint32_t height{};
    std::optional<Bytes> iccProfile;
};

export struct EncodeOptions {
    std::uint8_t quality{80};
    bool lossless{};
    std::optional<ByteView> iccProfile;
};

export struct Encoded {
    Bytes bytes;
};

export constexpr std::string_view ERR_DECODE{"Image: decode failed"};
export constexpr std::string_view ERR_TOO_MANY_PIXELS{
    "Image: input exceeds maxPixels limit"};
export constexpr std::string_view ERR_NATIVE_CODEC_DEFERRED{
    "Image: WebP native codec deferred"};

namespace {

constexpr std::array<std::uint8_t, 4> RIFF{'R', 'I', 'F', 'F'};
constexpr std::array<std::uint8_t, 4> WEBP{'W', 'E', 'B', 'P'};

std::uint32_t le24(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0])
           | (static_cast<std::uint32_t>(p[1]) << 8)
           | (static_cast<std::uint32_t>(p[2]) << 16);
}

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0])
           | static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8);
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0])
           | (static_cast<std::uint32_t>(p[1]) << 8)
           | (static_cast<std::uint32_t>(p[2]) << 16)
           | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool fourcc(const std::uint8_t* p, std::string_view value) {
    return std::equal(value.begin(), value.end(), p);
}

bool has_chunk_payload(ByteView bytes, std::uint32_t minimumSize) {
    if (bytes.size() < 20) return false;
    const auto chunkSize{le32(bytes.data() + 16)};
    return chunkSize >= minimumSize && chunkSize <= bytes.size() - 20;
}

}  // namespace

// Header-only translation of the VP8/VP8L/VP8X dimension layouts inspected by
// Bun through WebPGetInfo. This is intentionally not a replacement validator
// or pixel decoder: libwebp remains authoritative for the full bitstream.
export std::optional<Probe> probe(ByteView bytes) {
    if (bytes.size() < 20 || !std::equal(RIFF.begin(), RIFF.end(), bytes.begin())
        || !std::equal(WEBP.begin(), WEBP.end(), bytes.begin() + 8))
        return std::nullopt;

    if (fourcc(bytes.data() + 12, "VP8 ")) {
        // A lossy key frame carries the 0x9d012a start code followed by two
        // little-endian 14-bit dimensions. Inter frames cannot start a WebP.
        if (!has_chunk_payload(bytes, 10) || (bytes[20] & 1U) != 0
            || bytes[23] != 0x9D || bytes[24] != 0x01 || bytes[25] != 0x2A)
            return std::nullopt;
        const std::uint32_t width{le16(bytes.data() + 26) & 0x3FFFU};
        const std::uint32_t height{le16(bytes.data() + 28) & 0x3FFFU};
        if (width == 0 || height == 0) return std::nullopt;
        return Probe{.width = width, .height = height, .bitstream = BitstreamKind::Lossy};
    }

    if (fourcc(bytes.data() + 12, "VP8L")) {
        if (!has_chunk_payload(bytes, 5) || bytes[20] != 0x2F) return std::nullopt;
        const std::uint32_t bits{le32(bytes.data() + 21)};
        if ((bits >> 29) != 0) return std::nullopt;  // only version zero exists
        return Probe{
            .width = (bits & 0x3FFFU) + 1U,
            .height = ((bits >> 14) & 0x3FFFU) + 1U,
            .hasAlpha = ((bits >> 28) & 1U) != 0,
            .bitstream = BitstreamKind::Lossless,
        };
    }

    if (fourcc(bytes.data() + 12, "VP8X")) {
        if (!has_chunk_payload(bytes, 10)) return std::nullopt;
        const std::uint8_t flags{bytes[20]};
        if ((flags & 0xC1U) != 0 || bytes[21] != 0 || bytes[22] != 0 || bytes[23] != 0)
            return std::nullopt;
        return Probe{
            .width = le24(bytes.data() + 24) + 1U,
            .height = le24(bytes.data() + 27) + 1U,
            .hasAlpha = (flags & 0x10U) != 0,
            .hasIccProfile = (flags & 0x20U) != 0,
            // VP8X is a feature container; its image chunk may be VP8 or VP8L.
            .bitstream = BitstreamKind::Extended,
        };
    }

    return std::nullopt;
}

export std::expected<Decoded, Error> decode(ByteView bytes, std::uint64_t maxPixels) {
    // Keep the guard ordering from codec_webp.rs/codecs.rs: probe validates the
    // container and yields dimensions for the maxPixels bomb-guard BEFORE
    // libwebp allocates the decoded canvas.
    const auto header{probe(bytes)};
    if (!header) return std::unexpected{Error::DecodeFailed};
    if (static_cast<std::uint64_t>(header->width) * header->height > maxPixels)
        return std::unexpected{Error::TooManyPixels};
    auto native{backend::decode(bytes)};
    if (!native) return std::unexpected{Error::DecodeFailed};
    Decoded d{};
    d.rgba = std::move(native->rgba);
    d.width = native->width;
    d.height = native->height;
    return d;
}

export std::expected<Encoded, Error> encode(ByteView rgba, std::uint32_t width,
                                            std::uint32_t height,
                                            EncodeOptions options = {}) {
    constexpr auto C_INT_MAX{static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())};
    if (width == 0 || height == 0 || width > C_INT_MAX / 4U || height > C_INT_MAX)
        return std::unexpected{Error::EncodeFailed};
    const auto pixels{static_cast<std::uint64_t>(width) * height};
    if (pixels > std::numeric_limits<std::size_t>::max() / 4U
        || rgba.size() != static_cast<std::size_t>(pixels * 4U))
        return std::unexpected{Error::EncodeFailed};
    // ICCP mux (WebPMux*) is still deferred; the flat RGBA encode path is live.
    auto out{backend::encode(rgba, width, height, options.lossless,
                             static_cast<float>(options.quality))};
    if (!out) return std::unexpected{Error::EncodeFailed};
    return Encoded{.bytes = std::move(*out)};
}

// C ABI declarations mirror codec_webp.rs / codec_webp.zig. They are kept
// behind the seam until the libwebp package is available in the local index.
namespace native {

export constexpr std::int32_t WEBP_DEMUX_ABI_VERSION{0x0107};
export constexpr std::int32_t WEBP_MUX_ABI_VERSION{0x0109};
export constexpr std::int32_t WEBP_FF_FORMAT_FLAGS{0};
export constexpr std::uint32_t ICCP_FLAG{0x20};
export constexpr std::int32_t WEBP_MUX_OK{1};

export struct WebPData {
    const std::uint8_t* bytes{};
    std::size_t size{};
};

export struct WebPChunkIterator {
    std::int32_t chunkNum{};
    std::int32_t numChunks{};
    WebPData chunk{};
    std::array<std::uint32_t, 6> pad{};
    void* privateData{};
};

export struct WebPDemuxer;
export struct WebPMux;

export extern "C" int WebPGetInfo(const std::uint8_t*, std::size_t, std::int32_t*,
                                   std::int32_t*);
export extern "C" std::uint8_t* WebPDecodeRGBA(const std::uint8_t*, std::size_t,
                                                std::int32_t*, std::int32_t*);
export extern "C" std::size_t WebPEncodeRGBA(const std::uint8_t*, std::int32_t, std::int32_t,
                                              std::int32_t, float, std::uint8_t**);
export extern "C" std::size_t WebPEncodeLosslessRGBA(const std::uint8_t*, std::int32_t,
                                                      std::int32_t, std::int32_t,
                                                      std::uint8_t**);
export extern "C" void WebPFree(void*);

// libwebpmux/demux entry points are declared here, but deliberately not
// referenced until the native package is wired. Their borrowed WebPData and
// iterator shapes preserve the ICCP ownership seam from codec_webp.rs/.zig.
export extern "C" WebPDemuxer* WebPDemuxInternal(const WebPData*, std::int32_t,
                                                  std::int32_t*, std::int32_t);
export extern "C" void WebPDemuxDelete(WebPDemuxer*);
export extern "C" std::uint32_t WebPDemuxGetI(const WebPDemuxer*, std::int32_t);
export extern "C" int WebPDemuxGetChunk(const WebPDemuxer*, const std::uint8_t*,
                                         std::int32_t, WebPChunkIterator*);
export extern "C" void WebPDemuxReleaseChunkIterator(WebPChunkIterator*);
export extern "C" WebPMux* WebPNewInternal(std::int32_t);
export extern "C" void WebPMuxDelete(WebPMux*);
export extern "C" int WebPMuxSetImage(WebPMux*, const WebPData*, std::int32_t);
export extern "C" int WebPMuxSetChunk(WebPMux*, const std::uint8_t*, const WebPData*,
                                       std::int32_t);
export extern "C" int WebPMuxAssemble(WebPMux*, WebPData*);

}  // namespace native

}  // namespace mbun::image::webp
