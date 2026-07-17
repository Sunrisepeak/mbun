import std;
import mbun.css;

namespace {

int gChecks{};
int gFailures{};

void check(bool condition, std::string_view label) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::println("FAIL: {}", label);
    }
}

void test_property_tables() {
    using namespace mbun::css;

    check(property_id("color") == PropertyIdTag::Color, "lowercase property");
    check(property_id("COLOR") == PropertyIdTag::Color, "ASCII-case-insensitive property");
    check(property_kind("Background") == PropertyKind::Shorthand,
          "ASCII-case-insensitive shorthand");
    check(property_id("ALL") == PropertyIdTag::All, "ASCII-case-insensitive all property");
    check(property_kind("all") == PropertyKind::Shorthand, "all is a shorthand property");
    check(property_id("--Theme") == PropertyIdTag::Custom, "custom property identity");
    check(property_id("unknown-property") == PropertyIdTag::Unknown, "deferred property");
}

void test_token_model() {
    using namespace mbun::css;

    constexpr std::string_view source{"color: red"};
    constexpr CssToken ident{CssTokenKind::Ident, TokenSpan{0, 5}, 0};
    constexpr CssToken comment{CssTokenKind::Comment, TokenSpan{5, 5}, 0};
    constexpr CssToken open{CssTokenKind::Function, TokenSpan{0, 0}, 0};

    check(ident.text(source) == "color", "zero-copy token text");
    check(!ident.is_whitespace(), "identifier is not whitespace");
    check(comment.is_whitespace(), "comments collapse as whitespace in current engine");
    check(open.is_block_open(), "function opens a block");
}

}  // namespace

int main() {
    test_property_tables();
    test_token_model();
    std::println("test_css_tables: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
