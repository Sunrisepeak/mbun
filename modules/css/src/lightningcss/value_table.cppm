// Lightning CSS registered-property syntax metadata seam.
//
// Blueprint: bun Rust/Zig css/values/syntax.{rs,zig}
// SyntaxComponentKind::parse_string. Parsing typed payloads is intentionally
// outside this metadata-only checkpoint.
export module mbun.css.lightningcss.value_table;

import std;

export namespace mbun::css::lightningcss {

enum class ValueKind : std::uint8_t {
    unknown,
    length,
    number,
    percentage,
    length_percentage,
    color,
    image,
    url,
    integer,
    angle,
    time,
    resolution,
    transform_function,
    transform_list,
    custom_ident,
    css_wide_keyword,
};

struct ValueEntry {
    std::string_view name;
    ValueKind kind;
};

inline constexpr std::array VALUE_TABLE{
    ValueEntry{"length", ValueKind::length},
    ValueEntry{"number", ValueKind::number},
    ValueEntry{"percentage", ValueKind::percentage},
    ValueEntry{"length-percentage", ValueKind::length_percentage},
    ValueEntry{"color", ValueKind::color},
    ValueEntry{"image", ValueKind::image},
    ValueEntry{"url", ValueKind::url},
    ValueEntry{"integer", ValueKind::integer},
    ValueEntry{"angle", ValueKind::angle},
    ValueEntry{"time", ValueKind::time},
    ValueEntry{"resolution", ValueKind::resolution},
    ValueEntry{"transform-function", ValueKind::transform_function},
    ValueEntry{"transform-list", ValueKind::transform_list},
    ValueEntry{"custom-ident", ValueKind::custom_ident},
};

constexpr char syntax_ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

constexpr bool syntax_name_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index{}; index < left.size(); ++index) {
        if (syntax_ascii_lower(left[index]) != syntax_ascii_lower(right[index])) return false;
    }
    return true;
}

constexpr bool is_css_wide_keyword(std::string_view value) {
    return syntax_name_equal(value, "initial") || syntax_name_equal(value, "inherit") ||
        syntax_name_equal(value, "unset") || syntax_name_equal(value, "revert") ||
        syntax_name_equal(value, "revert-layer");
}

constexpr ValueKind lookup_value_kind(std::string_view value) {
    if (is_css_wide_keyword(value)) return ValueKind::css_wide_keyword;
    for (const auto& entry : VALUE_TABLE) {
        if (syntax_name_equal(entry.name, value)) return entry.kind;
    }
    return ValueKind::unknown;
}

}  // namespace mbun::css::lightningcss
