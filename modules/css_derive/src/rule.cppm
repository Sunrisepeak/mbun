export module mbun.css_derive.rule;

import std;

export namespace mbun::css_derive {

enum class RuleKind : std::uint8_t {
    qualified,
    at_rule,
    declaration,
};

struct RuleMetadata {
    std::string_view type_name {};
    std::string_view css_name {};
    RuleKind kind {RuleKind::at_rule};
    bool has_block {true};
    bool is_declaration_bearing {false};
};

[[nodiscard]] constexpr auto rule_is_at_rule(const RuleMetadata& metadata) noexcept -> bool {
    return metadata.kind == RuleKind::at_rule;
}

}
