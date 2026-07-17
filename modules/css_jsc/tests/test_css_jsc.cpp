import std;
import mbun.css_jsc;

namespace {

int checks { 0 };
int failures { 0 };

void check(bool condition, std::string_view name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL: {}", name);
    }
}

void test_value_conversion() {
    using namespace mbun::css_jsc;

    auto text { css_text_from_value(JsValue::string(".a { color: red; }"), "source") };
    check(text && *text == ".a { color: red; }", "string source conversion");

    auto missing { css_text_from_value(JsValue::undefined(), "source") };
    check(!missing && missing.error().kind == ValueErrorKind::Missing, "undefined source error");

    auto rgb { rgba_from_value(JsValue::array({ 255.0, 128.0, 0.0 })) };
    check(rgb && rgb->red == 255 && rgb->green == 128 && rgb->blue == 0 && rgb->alpha == 255,
        "rgb array conversion");

    auto object { rgba_from_value(JsValue::object({ { "r", 1.0 }, { "g", 2.0 }, { "b", 3.0 } })) };
    check(object && object->red == 1 && object->green == 2 && object->blue == 3,
        "rgb object conversion");
}

void test_bun_color_numeric_boundaries() {
    using namespace mbun::css_jsc;

    auto packed24 { rgba_from_value(JsValue::number(0x123456)) };
    check(packed24 && packed24->red == 0x12 && packed24->green == 0x34 && packed24->blue == 0x56
            && packed24->alpha == 255,
        "24-bit packed color is opaque");

    auto packed32 { rgba_from_value(JsValue::number(0x80123456)) };
    check(packed32 && packed32->red == 0x12 && packed32->green == 0x34 && packed32->blue == 0x56
            && packed32->alpha == 0x80,
        "32-bit packed color keeps alpha");

    auto packedNegative { rgba_from_value(JsValue::number(-1.0)) };
    check(packedNegative && packedNegative->red == 255 && packedNegative->green == 255
            && packedNegative->blue == 255 && packedNegative->alpha == 255,
        "negative packed color uses low 32 bits");

    auto packedHugePositive { rgba_from_value(JsValue::number(std::numeric_limits<double>::max())) };
    check(packedHugePositive && packedHugePositive->red == 255 && packedHugePositive->green == 255
            && packedHugePositive->blue == 255 && packedHugePositive->alpha == 255,
        "huge positive packed color saturates before low bits");

    auto packedHugeNegative { rgba_from_value(JsValue::number(-std::numeric_limits<double>::max())) };
    check(packedHugeNegative && packedHugeNegative->red == 0 && packedHugeNegative->green == 0
            && packedHugeNegative->blue == 0 && packedHugeNegative->alpha == 255,
        "huge negative packed color saturates before low bits");

    auto packedNan { rgba_from_value(JsValue::number(std::numeric_limits<double>::quiet_NaN())) };
    check(packedNan && packedNan->red == 0 && packedNan->green == 0 && packedNan->blue == 0
            && packedNan->alpha == 255,
        "NaN packed color becomes opaque black");

    auto packedPositiveInfinity { rgba_from_value(JsValue::number(std::numeric_limits<double>::infinity())) };
    check(packedPositiveInfinity && packedPositiveInfinity->red == 255 && packedPositiveInfinity->green == 255
            && packedPositiveInfinity->blue == 255 && packedPositiveInfinity->alpha == 255,
        "positive infinity packed color saturates");

    auto packedNegativeInfinity { rgba_from_value(JsValue::number(-std::numeric_limits<double>::infinity())) };
    check(packedNegativeInfinity && packedNegativeInfinity->red == 0 && packedNegativeInfinity->green == 0
            && packedNegativeInfinity->blue == 0 && packedNegativeInfinity->alpha == 255,
        "negative infinity packed color saturates");

    auto alphaModulo { rgba_from_value(
        JsValue::object({ { "r", 1.0 }, { "g", 2.0 }, { "b", 3.0 }, { "a", 2.0 } })) };
    check(alphaModulo && alphaModulo->alpha == 254, "object alpha uses Bun modulo semantics");

    auto alphaNegativeModulo { rgba_from_value(
        JsValue::object({ { "r", 1.0 }, { "g", 2.0 }, { "b", 3.0 }, { "a", -2.0 } })) };
    check(alphaNegativeModulo && alphaNegativeModulo->alpha == 2, "negative object alpha uses Euclidean modulo");

    auto alphaNan { rgba_from_value(JsValue::object(
        { { "r", 1.0 }, { "g", 2.0 }, { "b", 3.0 }, { "a", std::numeric_limits<double>::quiet_NaN() } })) };
    check(alphaNan && alphaNan->alpha == 0, "NaN object alpha becomes zero");

    auto alphaPositiveInfinity { rgba_from_value(JsValue::object(
        { { "r", 1.0 }, { "g", 2.0 }, { "b", 3.0 }, { "a", std::numeric_limits<double>::infinity() } })) };
    check(alphaPositiveInfinity && alphaPositiveInfinity->alpha == 255, "positive infinity object alpha saturates");

    auto alphaNegativeInfinity { rgba_from_value(JsValue::object(
        { { "r", 1.0 }, { "g", 2.0 }, { "b", 3.0 }, { "a", -std::numeric_limits<double>::infinity() } })) };
    check(alphaNegativeInfinity && alphaNegativeInfinity->alpha == 0, "negative infinity object alpha saturates");

    auto nonFiniteChannels { rgba_from_value(JsValue::array({ std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::max() })) };
    check(nonFiniteChannels && nonFiniteChannels->red == 0 && nonFiniteChannels->green == 255
            && nonFiniteChannels->blue == 0 && nonFiniteChannels->alpha == 255,
        "color channels use Bun saturating conversion");
}

void test_stylesheet_backend_seam() {
    using namespace mbun::css_jsc;
    StyleSheetBackend backend {
        .parse = [](std::string_view source) -> std::expected<std::vector<std::string>, BackendError> {
            return std::vector<std::string> { std::string { source } };
        },
        .serialize = [](std::span<const std::string> rules) -> std::expected<std::string, BackendError> {
            std::string result;
            for (const auto& rule : rules) result += rule;
            return result;
        },
    };
    StyleSheetObject sheet { backend };
    check(sheet.replace_sync("a{color:red}").has_value(), "stylesheet parse");
    check(sheet.length() == 1 && sheet.css_text() == "a{color:red}", "stylesheet serialization");
    check(sheet.insert_rule(1, "b{color:blue}").has_value(), "stylesheet insert");
    check(sheet.length() == 2 && sheet.css_text() == "a{color:red}b{color:blue}", "inserted rule visible");
    check(sheet.delete_rule(0).has_value() && sheet.css_text() == "b{color:blue}", "stylesheet delete");
}

}  // namespace

int main() {
    test_value_conversion();
    test_bun_color_numeric_boundaries();
    test_stylesheet_backend_seam();
    std::println("test_css_jsc: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
