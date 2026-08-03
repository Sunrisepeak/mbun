// T5.4 Unicode/uucode seam tests.
//
// Vectors are authoritative values taken from the Unicode 16.0.0 UCD
// (DerivedGeneralCategory, DerivedEastAsianWidth, UnicodeData, CaseFolding) —
// the same data bun's uucode library is generated from. They are not guessed.
//
// Full (multi-codepoint) case folding / SpecialCasing, grapheme-cluster
// segmentation, and NFC/NFD normalization remain DEFERRED.
import std;
import mbun.unicode;

int main() {
    using namespace mbun::unicode;
    int failures { 0 };
    auto check = [&](bool value, std::string_view label) {
        if (!value) {
            ++failures;
            std::println("FAIL {}", label);
        }
    };

    // ---- codepoint primitives ----
    check(is_valid_codepoint(0x10FFFF), "max codepoint");
    check(!is_valid_codepoint(0x110000), "out of range codepoint");
    check(!is_valid_codepoint(0xD800), "surrogate is not scalar");

    auto decoded { decode_utf8("\xF0\x9F\x8C\x8D") };
    check(decoded.has_value() && decoded->value == 0x1F30D && decoded->bytes == 4,
          "UTF-8 codepoint decode");
    check(!decode_utf8("\xC0\xAF").has_value(), "overlong UTF-8 rejected");

    // ---- general category (Unicode 16.0.0) ----
    check(category('A') == GeneralCategory::letter_uppercase, "U+0041 Lu");
    check(category('7') == GeneralCategory::number_decimal_digit, "U+0037 Nd");
    check(category(' ') == GeneralCategory::separator_space, "U+0020 Zs");
    check(category(0x03A3) == GeneralCategory::letter_uppercase, "U+03A3 Lu");
    check(category(0x00E9) == GeneralCategory::letter_lowercase, "U+00E9 Ll");
    check(category(0x05D0) == GeneralCategory::letter_other, "U+05D0 Lo (Hebrew)");
    check(category(0x4E2D) == GeneralCategory::letter_other, "U+4E2D Lo (CJK)");
    check(category(0x0301) == GeneralCategory::mark_nonspacing, "U+0301 Mn");
    check(category(0x0BC0) == GeneralCategory::mark_nonspacing, "U+0BC0 Mn (Tamil)");
    check(category(0x0660) == GeneralCategory::number_decimal_digit, "U+0660 Nd (Arabic)");
    check(category(0x1F600) == GeneralCategory::symbol_other, "U+1F600 So (emoji)");
    check(category(0x1F3FB) == GeneralCategory::symbol_modifier, "U+1F3FB Sk (skin tone)");
    check(category(0x00AD) == GeneralCategory::other_format, "U+00AD Cf (soft hyphen)");
    check(category(0x200D) == GeneralCategory::other_format, "U+200D Cf (ZWJ)");
    check(category(0xD800) == GeneralCategory::other_surrogate, "U+D800 Cs");
    check(category(0x0378) == GeneralCategory::other_not_assigned, "U+0378 Cn (unassigned)");

    // ---- width (East Asian Width + tailoring) ----
    check(width(0x41) == Width::narrow, "U+0041 narrow");
    check(width(0x4E2D) == Width::wide, "U+4E2D wide (W)");
    check(width(0x3000) == Width::wide, "U+3000 wide (F, ideographic space)");
    check(width(0x00A1) == Width::ambiguous, "U+00A1 ambiguous (A)");
    check(width(0x200D) == Width::zero, "U+200D zero (Cf/ZWJ)");
    check(width(0x0301) == Width::zero, "U+0301 zero (Mn)");
    check(width(0x00AD) == Width::narrow, "U+00AD soft hyphen narrow (tailored)");
    check(width(0x1F1E6) == Width::wide, "U+1F1E6 regional indicator wide (tailored)");
    check(wcwidth(0x41) == 1, "U+0041 wcwidth 1");
    check(wcwidth(0x4E2D) == 2, "U+4E2D wcwidth 2");
    check(wcwidth(0x1F600) == 2, "U+1F600 emoji wcwidth 2");
    check(wcwidth(0x0301) == 0, "U+0301 combining wcwidth 0");
    check(wcwidth(0x00A1) == 1, "U+00A1 ambiguous wcwidth 1");

    // ---- case mappings (simple, Unicode 16.0.0) ----
    check(to_lower('A') == 'a', "U+0041 lower");
    check(to_upper('z') == 'Z', "U+007A upper");
    check(to_lower(0x03A3) == 0x03C3, "U+03A3 lower (Sigma)");
    check(to_upper(0x03C3) == 0x03A3, "U+03C3 upper (sigma)");
    check(to_lower(0x00C5) == 0x00E5, "U+00C5 lower (A-ring)");
    check(to_upper(0x00E5) == 0x00C5, "U+00E5 upper (a-ring)");
    check(to_upper(0x1E9E) == 0x1E9E, "U+1E9E capital sharp s has no simple upper");
    check(to_lower(0x1E9E) == 0x00DF, "U+1E9E lower to sharp s");
    check(to_upper('a') == to_title('a'), "U+0061 title == upper (A)");
    check(to_lower('%') == '%', "non-letter lower identity");

    // ---- case folding (CaseFolding.txt C+S) ----
    check(case_fold('A') == 'a', "U+0041 fold");
    check(case_fold(0x03A3) == 0x03C3, "U+03A3 fold (Sigma)");
    check(case_fold(0x00B5) == 0x03BC, "U+00B5 micro folds to mu");
    check(case_fold(0x1E9E) == 0x00DF, "U+1E9E folds to sharp s");
    check(case_fold(0x0130) == 0x0130, "U+0130 dotted I has no simple fold (identity)");

    std::println("test_unicode: {} failures", failures);
    return failures == 0 ? 0 : 1;
}
