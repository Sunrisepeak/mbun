// Format dispatch follows bun runtime/image/codecs.rs and codecs.zig.
export module mbun.runtime_image.format;

import std;

namespace mbun::runtime_image {

export enum class Format : std::uint8_t {
    jpeg, png, webp, heic, avif, bmp, tiff, gif
};

export constexpr std::string_view mime_type(Format format) noexcept {
    switch (format) {
    case Format::jpeg: return "image/jpeg";
    case Format::png: return "image/png";
    case Format::webp: return "image/webp";
    case Format::heic: return "image/heic";
    case Format::avif: return "image/avif";
    case Format::bmp: return "image/bmp";
    case Format::tiff: return "image/tiff";
    case Format::gif: return "image/gif";
    }
    return "application/octet-stream";
}

export constexpr std::optional<Format> format_from_extension(std::string_view path) noexcept {
    const auto dot{path.rfind('.')};
    if (dot == std::string_view::npos) return std::nullopt;
    auto ext{path.substr(dot + 1)};
    if (ext == "jpg" || ext == "jpeg" || ext == "JPG" || ext == "JPEG") return Format::jpeg;
    if (ext == "png" || ext == "PNG") return Format::png;
    if (ext == "webp" || ext == "WEBP") return Format::webp;
    if (ext == "heic" || ext == "heif" || ext == "HEIC" || ext == "HEIF") return Format::heic;
    if (ext == "avif" || ext == "AVIF") return Format::avif;
    if (ext == "bmp" || ext == "BMP") return Format::bmp;
    if (ext == "tif" || ext == "tiff" || ext == "TIF" || ext == "TIFF") return Format::tiff;
    if (ext == "gif" || ext == "GIF") return Format::gif;
    return std::nullopt;
}

export constexpr std::optional<Format> sniff_format(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) return Format::jpeg;
    if (bytes.size() >= 8 && std::equal(bytes.begin(), bytes.begin() + 8,
                                        std::array<std::uint8_t, 8>{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a}.begin())) return Format::png;
    if (bytes.size() >= 12 && std::equal(bytes.begin(), bytes.begin() + 4, std::array<std::uint8_t, 4>{'R', 'I', 'F', 'F'}.begin())
        && std::equal(bytes.begin() + 8, bytes.begin() + 12, std::array<std::uint8_t, 4>{'W', 'E', 'B', 'P'}.begin())) return Format::webp;
    if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') return Format::bmp;
    if (bytes.size() >= 4 && ((bytes[0] == 'I' && bytes[1] == 'I' && bytes[2] == '*' && bytes[3] == 0)
        || (bytes[0] == 'M' && bytes[1] == 'M' && bytes[2] == 0 && bytes[3] == '*'))) return Format::tiff;
    if (bytes.size() >= 6 && ((bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == '8' && (bytes[4] == '7' || bytes[4] == '9') && bytes[5] == 'a'))) return Format::gif;
    if (bytes.size() >= 16 && std::equal(bytes.begin() + 4, bytes.begin() + 8, std::array<std::uint8_t, 4>{'f', 't', 'y', 'p'}.begin())) {
        for (std::size_t i{8}; i + 4 <= bytes.size() && i < 64; i += 4) {
            if (std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(i), bytes.begin() + static_cast<std::ptrdiff_t>(i + 4), std::array<std::uint8_t, 4>{'a', 'v', 'i', 'f'}.begin())) return Format::avif;
            if (std::equal(bytes.begin() + static_cast<std::ptrdiff_t>(i), bytes.begin() + static_cast<std::ptrdiff_t>(i + 4), std::array<std::uint8_t, 4>{'h', 'e', 'i', 'c'}.begin())) return Format::heic;
        }
    }
    return std::nullopt;
}

} // namespace mbun::runtime_image
