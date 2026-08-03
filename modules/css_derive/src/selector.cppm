export module mbun.css_derive.selector;

import std;

export namespace mbun::css_derive {

enum class SelectorComponentKind : std::uint8_t {
    type,
    class_name,
    id,
    attribute,
    pseudo_class,
    pseudo_element,
    combinator,
};

struct SelectorComponentMetadata {
    std::string_view type_name {};
    std::string_view css_name {};
    SelectorComponentKind kind {SelectorComponentKind::type};
    std::uint8_t specificity_weight {};
};

struct SelectorMetadata {
    std::string_view type_name {};
    std::span<const SelectorComponentMetadata> components {};
    bool supports_nested {false};
    bool stores_right_to_left {true};
};

[[nodiscard]] constexpr auto selector_specificity_weight(
    const SelectorComponentMetadata& metadata) noexcept -> std::uint8_t {
    return metadata.specificity_weight;
}

}
