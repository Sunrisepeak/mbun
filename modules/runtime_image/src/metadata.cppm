export module mbun.runtime_image.metadata;

import std;
import mbun.runtime_image.format;

namespace mbun::runtime_image {

export enum class ImageError : std::uint8_t { invalid_input, unsupported_format, too_many_pixels, backend_unavailable };

export struct Metadata {
    std::uint32_t width{0};
    std::uint32_t height{0};
    Format format{Format::png};
    std::vector<std::uint8_t> icc_profile{};

    [[nodiscard]] constexpr std::string_view mime() const noexcept { return mime_type(format); }
};

export struct Decoded {
    std::vector<std::uint8_t> rgba{};
    Metadata metadata{};
};

export struct EncodeOptions {
    Format format{Format::png};
    int quality{-1};
    std::vector<std::uint8_t> icc_profile{};
};

export struct Encoded {
    std::vector<std::uint8_t> bytes{};
    Format format{Format::png};
};

} // namespace mbun::runtime_image
