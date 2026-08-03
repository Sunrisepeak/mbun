export module mbun.runtime_image.image;

import std;
import mbun.runtime_image.backend;
import mbun.runtime_image.format;
import mbun.runtime_image.metadata;

namespace mbun::runtime_image {

export struct ResizeOptions {
    std::uint32_t width{0};
    std::uint32_t height{0};
    bool without_enlargement{false};
};

export class Image final {
private:
    std::vector<std::uint8_t> source_{};
    std::optional<Format> format_hint_{};
    std::uint64_t max_pixels_{0x3fffULL * 0x3fffULL};
    ResizeOptions resize_{};
    std::uint16_t rotate_{0};
    bool flip_{false};
    bool flop_{false};
    std::optional<EncodeOptions> output_{};

public:
    explicit Image(std::vector<std::uint8_t> source, std::optional<Format> hint = std::nullopt)
        : source_{std::move(source)}, format_hint_{hint} {}

    [[nodiscard]] static Image from_bytes(std::span<const std::uint8_t> bytes, std::optional<Format> hint = std::nullopt) {
        return Image{std::vector<std::uint8_t>{bytes.begin(), bytes.end()}, hint};
    }
    [[nodiscard]] const std::vector<std::uint8_t>& source() const noexcept { return source_; }
    [[nodiscard]] Image& resize(ResizeOptions options) noexcept { resize_ = options; return *this; }
    [[nodiscard]] Image& rotate(std::uint16_t degrees) noexcept { rotate_ = static_cast<std::uint16_t>(degrees % 360); return *this; }
    [[nodiscard]] Image& flip() noexcept { flip_ = true; return *this; }
    [[nodiscard]] Image& flop() noexcept { flop_ = true; return *this; }
    [[nodiscard]] Image& output(EncodeOptions options) { output_ = std::move(options); return *this; }

    [[nodiscard]] std::expected<Metadata, ImageError> metadata(const Backend& backend) const {
        if (format_hint_.has_value()) {
            auto result{backend.probe(source_)};
            if (result) result->format = *format_hint_;
            return result;
        }
        return backend.probe(source_);
    }

    [[nodiscard]] std::expected<Encoded, ImageError> encode(const Backend& backend) const {
        auto decoded{backend.decode(source_, max_pixels_)};
        if (!decoded) return std::unexpected(decoded.error());
        if (output_.has_value()) return backend.encode(*decoded, *output_);
        return backend.encode(*decoded, EncodeOptions{.format = decoded->metadata.format,
                                                        .icc_profile = decoded->metadata.icc_profile});
    }

    [[nodiscard]] const ResizeOptions& resize_options() const noexcept { return resize_; }
    [[nodiscard]] std::uint16_t rotation() const noexcept { return rotate_; }
    [[nodiscard]] bool flipped() const noexcept { return flip_; }
    [[nodiscard]] bool flopped() const noexcept { return flop_; }
};

} // namespace mbun::runtime_image
