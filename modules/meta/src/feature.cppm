export module mbun.meta.feature;

import std;

namespace mbun::meta {

export enum class Feature : std::uint8_t {
    jit,
    native_modules,
    debug_assertions,
    experimental,
};

export class FeatureSet {
    std::uint64_t bits_ {};

    [[nodiscard]] static constexpr std::uint64_t bit(Feature feature) {
        return std::uint64_t { 1 } << static_cast<unsigned>(feature);
    }

public:
    constexpr FeatureSet() = default;
    explicit constexpr FeatureSet(std::uint64_t bits) : bits_ { bits } {}

    constexpr void enable(Feature feature) { bits_ |= bit(feature); }
    constexpr void disable(Feature feature) { bits_ &= ~bit(feature); }
    [[nodiscard]] constexpr bool enabled(Feature feature) const { return (bits_ & bit(feature)) != 0; }
    [[nodiscard]] constexpr std::uint64_t bits() const { return bits_; }
};

} // namespace mbun::meta
