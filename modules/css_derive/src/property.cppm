export module mbun.css_derive.property;

import std;

export namespace mbun::css_derive {

// Mirrors bun_css_derive's DefineEnumProperty contract: a unit enum is a
// closed set of CSS keywords, with an optional spelling override.
struct KeywordMetadata {
    std::string_view variant {};
    std::string_view css_name {};
};

enum class PropertyKind : std::uint8_t {
    enum_property,
    payload_enum,
    shorthand,
};

struct PropertyMetadata {
    std::string_view type_name {};
    PropertyKind kind {PropertyKind::enum_property};
    std::span<const KeywordMetadata> keywords {};
};

[[nodiscard]] constexpr auto ascii_lower(char value) noexcept -> char {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] constexpr auto ascii_equal_insensitive(std::string_view lhs,
                                                     std::string_view rhs) noexcept -> bool {
    if (lhs.size() != rhs.size())
        return false;
    for (std::size_t i {}; i < lhs.size(); ++i) {
        if (ascii_lower(lhs[i]) != ascii_lower(rhs[i]))
            return false;
    }
    return true;
}

[[nodiscard]] constexpr auto find_keyword(const PropertyMetadata& metadata,
                                          std::string_view input) noexcept
    -> std::optional<std::size_t> {
    for (std::size_t i {}; i < metadata.keywords.size(); ++i) {
        if (ascii_equal_insensitive(metadata.keywords[i].css_name, input))
            return i;
    }
    return std::nullopt;
}

[[nodiscard]] constexpr auto keyword_name(const PropertyMetadata& metadata,
                                          std::size_t index) noexcept
    -> std::optional<std::string_view> {
    if (index >= metadata.keywords.size())
        return std::nullopt;
    return metadata.keywords[index].css_name;
}

}
