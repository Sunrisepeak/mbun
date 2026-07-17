export module mbun.bun_core.version;

import std;

namespace mbun::bun_core {

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
            const auto end { text.find('.', begin) };
            const auto token_end { end == std::string_view::npos ? text.size() : end };
            if (token_end == begin) {
                return std::nullopt;
            }
            std::uint64_t value { 0 };
            for (const char digit : text.substr(begin, token_end - begin)) {
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
    friend constexpr auto operator<=>(const Version&, const Version&) = default;
};

export inline constexpr Version RUNTIME_VERSION { 1, 3, 14 };

export constexpr Version runtime_version() { return RUNTIME_VERSION; }

} // namespace mbun::bun_core
