// mbun.css.property_tables — generated-property-shaped metadata.
//
// This is intentionally metadata-only. Bun's Rust and Zig implementations
// generate the complete PropertyId/PropertyIdTag union in
// properties/properties_generated.{rs,zig}. Typed values and the complete
// generated table are DEFERRED; this subset does not claim full coverage.
export module mbun.css.property_tables;

import std;

namespace mbun::css {

export enum class PropertyIdTag : unsigned short {
    Unknown,
    Custom,
    All,
    Color,
    Background,
    BackgroundColor,
    BackgroundImage,
    Display,
    Position,
    Width,
    Height,
    Margin,
    Padding,
    Border,
    BorderRadius,
    Font,
    Opacity,
    Transform,
    Transition,
    Animation,
};

export enum class PropertyKind : unsigned char {
    Unknown,
    Longhand,
    Shorthand,
    Custom,
};

export struct PropertyDescriptor {
    PropertyIdTag id{};
    std::string_view name{};
    PropertyKind kind{PropertyKind::Unknown};
};

export inline constexpr std::array PROPERTY_TABLE{
    PropertyDescriptor{PropertyIdTag::Color, "color", PropertyKind::Longhand},
    PropertyDescriptor{PropertyIdTag::Background, "background", PropertyKind::Shorthand},
    PropertyDescriptor{PropertyIdTag::BackgroundColor, "background-color", PropertyKind::Longhand},
    PropertyDescriptor{PropertyIdTag::BackgroundImage, "background-image", PropertyKind::Longhand},
    PropertyDescriptor{PropertyIdTag::Display, "display", PropertyKind::Longhand},
    PropertyDescriptor{PropertyIdTag::Position, "position", PropertyKind::Longhand},
    PropertyDescriptor{PropertyIdTag::Width, "width", PropertyKind::Longhand},
    PropertyDescriptor{PropertyIdTag::Height, "height", PropertyKind::Longhand},
    PropertyDescriptor{PropertyIdTag::Margin, "margin", PropertyKind::Shorthand},
    PropertyDescriptor{PropertyIdTag::Padding, "padding", PropertyKind::Shorthand},
    PropertyDescriptor{PropertyIdTag::Border, "border", PropertyKind::Shorthand},
    PropertyDescriptor{PropertyIdTag::BorderRadius, "border-radius", PropertyKind::Shorthand},
    PropertyDescriptor{PropertyIdTag::Font, "font", PropertyKind::Shorthand},
    PropertyDescriptor{PropertyIdTag::Opacity, "opacity", PropertyKind::Longhand},
    PropertyDescriptor{PropertyIdTag::Transform, "transform", PropertyKind::Longhand},
    PropertyDescriptor{PropertyIdTag::Transition, "transition", PropertyKind::Shorthand},
    PropertyDescriptor{PropertyIdTag::Animation, "animation", PropertyKind::Shorthand},
};

export constexpr bool is_custom_property(std::string_view name) {
    return name.size() >= 2 && name[0] == '-' && name[1] == '-';
}

constexpr char ascii_lower(char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

constexpr bool ascii_equal_ignore_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index{}; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) return false;
    }
    return true;
}

export constexpr const PropertyDescriptor* find_property(std::string_view name) {
    for (const auto& descriptor : PROPERTY_TABLE)
        if (ascii_equal_ignore_case(descriptor.name, name)) return &descriptor;
    return nullptr;
}

export constexpr PropertyIdTag property_id(std::string_view name) {
    if (is_custom_property(name)) return PropertyIdTag::Custom;
    if (ascii_equal_ignore_case(name, "all")) return PropertyIdTag::All;
    if (const auto* descriptor = find_property(name)) return descriptor->id;
    return PropertyIdTag::Unknown;
}

export constexpr PropertyKind property_kind(std::string_view name) {
    if (is_custom_property(name)) return PropertyKind::Custom;
    if (ascii_equal_ignore_case(name, "all")) return PropertyKind::Shorthand;
    if (const auto* descriptor = find_property(name)) return descriptor->kind;
    return PropertyKind::Unknown;
}

}  // namespace mbun::css
