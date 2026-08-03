export module mbun.bun_core.feature;

import std;

namespace mbun::bun_core {

export enum class Feature : std::uint8_t {
    tracing,
    entry_cache,
    verbose_fs,
    watch_directories,
    allow_json_single_quotes,
    enable_keepalive,
    http_buffer_pooling,
    unwrap_commonjs_to_esm,
    runtime_transpiler_cache,
};

export class FeatureSet {
    std::uint64_t bits_ {};

    [[nodiscard]] static constexpr std::uint64_t bit(Feature feature) {
        return std::uint64_t { 1 } << static_cast<unsigned>(feature);
    }

public:
    constexpr FeatureSet() = default;
    explicit constexpr FeatureSet(std::uint64_t bits) : bits_ { bits } {}

    [[nodiscard]] static constexpr FeatureSet defaults() {
        FeatureSet result {};
        result.enable(Feature::tracing);
        result.enable(Feature::entry_cache);
        result.enable(Feature::watch_directories);
        result.enable(Feature::allow_json_single_quotes);
        result.enable(Feature::enable_keepalive);
        result.enable(Feature::http_buffer_pooling);
        result.enable(Feature::unwrap_commonjs_to_esm);
        result.enable(Feature::runtime_transpiler_cache);
        return result;
    }

    constexpr void enable(Feature feature) { bits_ |= bit(feature); }
    constexpr void disable(Feature feature) { bits_ &= ~bit(feature); }
    [[nodiscard]] constexpr bool enabled(Feature feature) const { return (bits_ & bit(feature)) != 0; }
    [[nodiscard]] constexpr std::uint64_t bits() const { return bits_; }
};

} // namespace mbun::bun_core
