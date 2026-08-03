export module mbun.runtime_image.backend;

import std;
import mbun.runtime_image.metadata;

namespace mbun::runtime_image {

export class Backend {
public:
    virtual ~Backend() = default;
    [[nodiscard]] virtual std::expected<Metadata, ImageError> probe(std::span<const std::uint8_t>) const = 0;
    [[nodiscard]] virtual std::expected<Decoded, ImageError> decode(std::span<const std::uint8_t>, std::uint64_t) const = 0;
    [[nodiscard]] virtual std::expected<Encoded, ImageError> encode(const Decoded&, EncodeOptions) const = 0;
};

export class DeferredBackend final : public Backend {
public:
    [[nodiscard]] std::expected<Metadata, ImageError> probe(std::span<const std::uint8_t>) const override {
        return std::unexpected(ImageError::backend_unavailable);
    }
    [[nodiscard]] std::expected<Decoded, ImageError> decode(std::span<const std::uint8_t>, std::uint64_t) const override {
        return std::unexpected(ImageError::backend_unavailable);
    }
    [[nodiscard]] std::expected<Encoded, ImageError> encode(const Decoded&, EncodeOptions) const override {
        return std::unexpected(ImageError::backend_unavailable);
    }
};

} // namespace mbun::runtime_image
