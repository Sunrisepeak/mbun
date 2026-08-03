export module mbun.meta.version;

import std;

namespace mbun::meta {

// Bun Rust/Zig metadata sources are unavailable in this checkout. This seam
// keeps version data independent from bun_core until those sources are present.
export struct Version {
    std::uint32_t major {};
    std::uint32_t minor {};
    std::uint32_t patch {};

    [[nodiscard]] static std::optional<Version> parse(std::string_view text) {
        Version result {};
        std::uint32_t* components[] { &result.major, &result.minor, &result.patch };
        std::size_t component { 0 };
        std::size_t begin { 0 };
        while (begin < text.size() && component < 3) {
            const std::size_t end { text.find('.', begin) };
            const std::size_t tokenEnd { end == std::string_view::npos ? text.size() : end };
            if (tokenEnd == begin) {
                return std::nullopt;
            }

            std::uint64_t value { 0 };
            for (const char digit : text.substr(begin, tokenEnd - begin)) {
                if (digit < '0' || digit > '9') {
                    return std::nullopt;
                }
                value = value * 10 + static_cast<std::uint64_t>(digit - '0');
                if (value > std::numeric_limits<std::uint32_t>::max()) {
                    return std::nullopt;
                }
            }
            *components[component++] = static_cast<std::uint32_t>(value);
            if (end == std::string_view::npos) {
                begin = text.size();
            } else {
                begin = end + 1;
            }
        }
        if (component != 3 || begin != text.size()) {
            return std::nullopt;
        }
        return result;
    }

    [[nodiscard]] std::string to_string() const {
        return std::format("{}.{}.{}", major, minor, patch);
    }

    friend constexpr bool operator==(const Version& left, const Version& right) {
        return left.major == right.major && left.minor == right.minor && left.patch == right.patch;
    }
};

} // namespace mbun::meta
