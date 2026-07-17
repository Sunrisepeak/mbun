import std;
import mbun.css_derive;

int main() {
    const std::array keywords {
        mbun::css_derive::KeywordMetadata {.variant = "Preserve3d", .css_name = "preserve-3d"},
        mbun::css_derive::KeywordMetadata {.variant = "Auto", .css_name = "auto"},
    };
    const mbun::css_derive::PropertyMetadata property {
        .type_name = "TransformStyle",
        .kind = mbun::css_derive::PropertyKind::enum_property,
        .keywords = keywords,
    };

    auto preserve = mbun::css_derive::find_keyword(property, "PRESERVE-3D");
    auto missing = mbun::css_derive::find_keyword(property, "inherit");
    if (!preserve || *preserve != 0 || missing)
        return 1;
    if (mbun::css_derive::keyword_name(property, 1) != std::optional<std::string_view> {"auto"})
        return 2;
    if (mbun::css_derive::keyword_name(property, 2))
        return 3;

    const mbun::css_derive::RuleMetadata rule {
        .type_name = "MediaRule",
        .css_name = "@media",
        .kind = mbun::css_derive::RuleKind::at_rule,
        .has_block = true,
        .is_declaration_bearing = false,
    };
    if (!mbun::css_derive::rule_is_at_rule(rule))
        return 4;

    const mbun::css_derive::SelectorComponentMetadata component {
        .type_name = "PseudoClass",
        .css_name = ":is",
        .kind = mbun::css_derive::SelectorComponentKind::pseudo_class,
        .specificity_weight = 1,
    };
    const std::array components {component};
    const mbun::css_derive::SelectorMetadata selector {
        .type_name = "Selector",
        .components = components,
        .supports_nested = true,
        .stores_right_to_left = true,
    };
    if (selector.components.size() != 1 ||
        mbun::css_derive::selector_specificity_weight(selector.components.front()) != 1)
        return 5;

    std::println("test_css_derive: 5 checks, 0 failures");
    return 0;
}
