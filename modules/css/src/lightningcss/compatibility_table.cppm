// Lightning CSS browser-compatibility metadata seam.
//
// Blueprint: generated Bun Rust css/compat.rs and Zig css/compat.zig, plus
// css/targets.{rs,zig}. Versions use the upstream 24-bit major/minor/patch
// representation. Every selected feature has an explicit row for every Bun
// browser target, including browsers where the feature is unavailable.
export module mbun.css.lightningcss.compatibility_table;

import std;

export namespace mbun::css::lightningcss {

enum class Browser : std::uint8_t {
    android,
    chrome,
    edge,
    firefox,
    ie,
    ios_saf,
    opera,
    safari,
    samsung,
};

enum class Feature : std::uint8_t {
    calc_function,
    color_function,
    conic_gradient,
    nesting,
    oklab_colors,
    webkit_fill_available_size,
};

struct BrowserTargets {
    std::array<std::optional<std::uint32_t>, 9> versions{};

    constexpr std::optional<std::uint32_t>& operator[](Browser browser) {
        return versions[static_cast<std::size_t>(browser)];
    }

    constexpr const std::optional<std::uint32_t>& operator[](Browser browser) const {
        return versions[static_cast<std::size_t>(browser)];
    }
};

struct FeatureSupport {
    Feature feature;
    Browser browser;
    std::uint32_t minimum_version;
    bool available;
};

inline constexpr std::array FEATURE_SUPPORT_TABLE{
    FeatureSupport{Feature::calc_function, Browser::android, 8585216, true},
    FeatureSupport{Feature::calc_function, Browser::chrome, 1703936, true},
    FeatureSupport{Feature::calc_function, Browser::edge, 786432, true},
    FeatureSupport{Feature::calc_function, Browser::firefox, 1048576, true},
    FeatureSupport{Feature::calc_function, Browser::ie, 0, false},
    FeatureSupport{Feature::calc_function, Browser::ios_saf, 458752, true},
    FeatureSupport{Feature::calc_function, Browser::opera, 983040, true},
    FeatureSupport{Feature::calc_function, Browser::safari, 393472, true},
    FeatureSupport{Feature::calc_function, Browser::samsung, 262144, true},

    FeatureSupport{Feature::color_function, Browser::android, 7274496, true},
    FeatureSupport{Feature::color_function, Browser::chrome, 7274496, true},
    FeatureSupport{Feature::color_function, Browser::edge, 7274496, true},
    FeatureSupport{Feature::color_function, Browser::firefox, 7405568, true},
    FeatureSupport{Feature::color_function, Browser::ie, 0, false},
    FeatureSupport{Feature::color_function, Browser::ios_saf, 656128, true},
    FeatureSupport{Feature::color_function, Browser::opera, 4915200, true},
    FeatureSupport{Feature::color_function, Browser::safari, 655616, true},
    FeatureSupport{Feature::color_function, Browser::samsung, 1441792, true},

    FeatureSupport{Feature::conic_gradient, Browser::android, 4521984, true},
    FeatureSupport{Feature::conic_gradient, Browser::chrome, 4521984, true},
    FeatureSupport{Feature::conic_gradient, Browser::edge, 5177344, true},
    FeatureSupport{Feature::conic_gradient, Browser::firefox, 5439488, true},
    FeatureSupport{Feature::conic_gradient, Browser::ie, 0, false},
    FeatureSupport{Feature::conic_gradient, Browser::ios_saf, 786944, true},
    FeatureSupport{Feature::conic_gradient, Browser::opera, 3145728, true},
    FeatureSupport{Feature::conic_gradient, Browser::safari, 786688, true},
    FeatureSupport{Feature::conic_gradient, Browser::samsung, 655360, true},

    FeatureSupport{Feature::nesting, Browser::android, 8585216, true},
    FeatureSupport{Feature::nesting, Browser::chrome, 7864320, true},
    FeatureSupport{Feature::nesting, Browser::edge, 7864320, true},
    FeatureSupport{Feature::nesting, Browser::firefox, 7667712, true},
    FeatureSupport{Feature::nesting, Browser::ie, 0, false},
    FeatureSupport{Feature::nesting, Browser::ios_saf, 1114624, true},
    FeatureSupport{Feature::nesting, Browser::opera, 6946816, true},
    FeatureSupport{Feature::nesting, Browser::safari, 1114624, true},
    FeatureSupport{Feature::nesting, Browser::samsung, 0, false},

    FeatureSupport{Feature::oklab_colors, Browser::android, 7274496, true},
    FeatureSupport{Feature::oklab_colors, Browser::chrome, 7274496, true},
    FeatureSupport{Feature::oklab_colors, Browser::edge, 7274496, true},
    FeatureSupport{Feature::oklab_colors, Browser::firefox, 7405568, true},
    FeatureSupport{Feature::oklab_colors, Browser::ie, 0, false},
    FeatureSupport{Feature::oklab_colors, Browser::ios_saf, 984064, true},
    FeatureSupport{Feature::oklab_colors, Browser::opera, 4915200, true},
    FeatureSupport{Feature::oklab_colors, Browser::safari, 984064, true},
    FeatureSupport{Feature::oklab_colors, Browser::samsung, 1441792, true},

    FeatureSupport{Feature::webkit_fill_available_size, Browser::android, 263168, true},
    FeatureSupport{Feature::webkit_fill_available_size, Browser::chrome, 1638400, true},
    FeatureSupport{Feature::webkit_fill_available_size, Browser::edge, 5177344, true},
    FeatureSupport{Feature::webkit_fill_available_size, Browser::firefox, 0, false},
    FeatureSupport{Feature::webkit_fill_available_size, Browser::ie, 0, false},
    FeatureSupport{Feature::webkit_fill_available_size, Browser::ios_saf, 458752, true},
    FeatureSupport{Feature::webkit_fill_available_size, Browser::opera, 917504, true},
    FeatureSupport{Feature::webkit_fill_available_size, Browser::safari, 458752, true},
    FeatureSupport{Feature::webkit_fill_available_size, Browser::samsung, 327680, true},
};

constexpr const FeatureSupport* find_support(Feature feature, Browser browser) {
    for (const auto& row : FEATURE_SUPPORT_TABLE) {
        if (row.feature == feature && row.browser == browser) return &row;
    }
    return nullptr;
}

constexpr bool is_compatible(Feature feature, const BrowserTargets& targets) {
    for (std::size_t index{}; index < targets.versions.size(); ++index) {
        if (!targets.versions[index]) continue;
        const auto* support = find_support(feature, static_cast<Browser>(index));
        if (support == nullptr || !support->available ||
            *targets.versions[index] < support->minimum_version) {
            return false;
        }
    }
    return true;
}

struct Targets {
    BrowserTargets browsers;
    std::array<Feature, 6> include{};
    std::size_t include_count{};
    std::array<Feature, 6> exclude{};
    std::size_t exclude_count{};

    static constexpr bool contains(const std::array<Feature, 6>& features, std::size_t count,
                                   Feature feature) {
        for (std::size_t index{}; index < count; ++index) {
            if (features[index] == feature) return true;
        }
        return false;
    }

    constexpr bool should_compile(Feature feature) const {
        if (contains(include, include_count, feature)) return true;
        return !contains(exclude, exclude_count, feature) &&
            !lightningcss::is_compatible(feature, browsers);
    }

    constexpr bool is_compatible(Feature feature) const {
        return lightningcss::is_compatible(feature, browsers);
    }
};

}  // namespace mbun::css::lightningcss
