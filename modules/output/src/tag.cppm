// tag.cppm — Bun's compact <tag> to ANSI table.
//
// The table intentionally stays a linear constexpr scan. Bun only reaches it
// while rendering diagnostic markers, so keeping one canonical table is more
// important than introducing a runtime map or duplicated escape strings.
// ref: .mbun/bun-ref/src/bun_output_tags/lib.rs
export module mbun.output.tag;

import std;

export namespace mbun::output {

namespace ansi {
inline constexpr std::string_view reset{"\x1b[0m"};
inline constexpr std::string_view bold{"\x1b[1m"};
inline constexpr std::string_view dim{"\x1b[2m"};
inline constexpr std::string_view italic{"\x1b[3m"};
inline constexpr std::string_view underline{"\x1b[4m"};
inline constexpr std::string_view invert{"\x1b[7m"};
inline constexpr std::string_view strikethrough{"\x1b[9m"};
inline constexpr std::string_view black{"\x1b[30m"};
inline constexpr std::string_view red{"\x1b[31m"};
inline constexpr std::string_view green{"\x1b[32m"};
inline constexpr std::string_view yellow{"\x1b[33m"};
inline constexpr std::string_view blue{"\x1b[34m"};
inline constexpr std::string_view magenta{"\x1b[35m"};
inline constexpr std::string_view cyan{"\x1b[36m"};
inline constexpr std::string_view white{"\x1b[37m"};
inline constexpr std::string_view bright_white{"\x1b[97m"};
inline constexpr std::string_view bg_red{"\x1b[41m"};
inline constexpr std::string_view bg_green{"\x1b[42m"};
}  // namespace ansi

struct TagEscape {
    std::string_view name;
    std::string_view escape;
};

inline constexpr std::array<TagEscape, 14> tag_escapes {{
    {"b", ansi::bold},       {"d", ansi::dim},       {"i", ansi::italic},
    {"u", ansi::underline},  {"black", ansi::black},  {"red", ansi::red},
    {"green", ansi::green},  {"yellow", ansi::yellow},{"blue", ansi::blue},
    {"magenta", ansi::magenta}, {"cyan", ansi::cyan}, {"white", ansi::white},
    {"bgred", ansi::bg_red},  {"bggreen", ansi::bg_green},
}};

[[nodiscard]] constexpr std::optional<std::string_view> color_for(std::string_view name) noexcept {
    for (const auto& entry : tag_escapes) {
        if (entry.name == name) {
            return entry.escape;
        }
    }
    return std::nullopt;
}

[[nodiscard]] constexpr std::optional<std::string_view> color_for_bytes(
    std::span<const std::byte> name) noexcept {
    for (const auto& entry : tag_escapes) {
        if (entry.name.size() != name.size()) {
            continue;
        }
        bool equal{true};
        for (std::size_t i{0}; i < name.size(); ++i) {
            if (static_cast<unsigned char>(entry.name[i]) != std::to_integer<unsigned char>(name[i])) {
                equal = false;
                break;
            }
        }
        if (equal) {
            return entry.escape;
        }
    }
    return std::nullopt;
}

}  // namespace mbun::output
