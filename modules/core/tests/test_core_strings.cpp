// test_core_strings.cpp — T1.2 mbun.core.strings test suite.
//
// Vectors are extracted from bun's original test suite (assertion semantics
// preserved; see AGENTS.md TDD rules):
//   - .mbun/bun-ref/test/js/bun/util/toUTF16Alloc.test.ts   (UTF-8 -> UTF-16)
//   - .mbun/bun-ref/test/js/bun/util/stripANSI.test.ts      (strip_ansi)
//   - .mbun/bun-ref/test/js/bun/util/stringWidth.test.ts    (string_width)
//
// JS string inputs are code-unit sequences; each vector encodes the exact
// code points (WTF-8, lone surrogates preserved) via the harness `W(...)`
// helper, independent of the module under test.
//
// DEFERRED(S1) — need a runtime oracle or out of pure-logic scope:
//   - stringWidth.test.ts `toMatchNPMStringWidth` / `toMatchNPMStringWidthExcludeANSI`
//     cases: these assert equality with the npm `string-width` package rather
//     than an explicit value, so they need that oracle at S1. The
//     `describe("stringWidth extended")` + `describe("stringWidth SIMD fast paths")`
//     blocks assert explicit `.toBe(n)` and are extracted here.
//   - stripANSI.test.ts plain (non-tuple) entries compare against npm
//     `strip-ansi`; only the explicit `[input, expected]` tuples (bun's
//     intentional contract) plus unambiguous well-formed cases are extracted.
//   - stripANSI zero-copy identity / heapStats object-count assertions (JSC heap
//     introspection), non-string coercion (JS argument coercion).
import std;
import mbun.core.strings;

namespace strings = mbun::core::strings;

namespace {

int gChecks{0};
int gFailures{0};
constexpr int MAX_FAILURE_PRINTS{50};

void report_failure(std::string_view what) {
    ++gFailures;
    if (gFailures <= MAX_FAILURE_PRINTS) {
        std::println("  FAIL {}", what);
    }
}

// Independent WTF-8 encoder (does not use the module under test): build a byte
// string from an explicit list of code points. Surrogates (0xD800..DFFF) are
// encoded as their 3-byte WTF-8 form.
std::string W(std::initializer_list<char32_t> cps) {
    std::string out;
    for (char32_t cp : cps) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// UTF-16 code-unit sequence builder (independent of the module).
std::u16string U16(std::initializer_list<char16_t> units) {
    return std::u16string{units.begin(), units.end()};
}

void check_size(std::size_t actual, std::size_t expected, std::string what) {
    ++gChecks;
    if (actual != expected) {
        report_failure(std::format("{} = {}, expected {}", what, actual, expected));
    }
}

void check_bool(bool actual, bool expected, std::string what) {
    ++gChecks;
    if (actual != expected) {
        report_failure(std::format("{} = {}, expected {}", what, actual, expected));
    }
}

std::string hex_dump(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        out += std::format("{:02X} ", c);
    }
    return out;
}

void check_bytes(std::string_view actual, std::string_view expected, std::string what) {
    ++gChecks;
    if (actual != expected) {
        report_failure(std::format("{}\n    expect={}\n    actual={}", what, hex_dump(expected),
                                   hex_dump(actual)));
    }
}

std::string u16_hex(std::u16string_view s) {
    std::string out;
    for (char16_t c : s) {
        out += std::format("{:04X} ", static_cast<std::uint16_t>(c));
    }
    return out;
}

void check_u16(std::u16string_view actual, std::u16string_view expected, std::string what) {
    ++gChecks;
    if (actual != expected) {
        report_failure(std::format("{}\n    expect={}\n    actual={}", what, u16_hex(expected),
                                   u16_hex(actual)));
    }
}

// ---------------------------------------------------------------------------
// UTF-8 -> UTF-16 (toUTF16Alloc.test.ts)
// ---------------------------------------------------------------------------

void test_utf8_to_utf16() {
    // source: toUTF16Alloc.test.ts > describe("...toUTF16AllocForReal(sentinel=true)")
    check_u16(strings::utf8_to_utf16("abc"), U16({'a', 'b', 'c'}), "[toUTF16] pure ASCII");
    // café / アプリケーション (valid UTF-8 fast path)
    check_u16(strings::utf8_to_utf16(W({'c', 'a', 'f', 0xE9})), U16({'c', 'a', 'f', 0xE9}),
              "[toUTF16] café");
    check_u16(strings::utf8_to_utf16(W({0x30A2, 0x30D7, 0x30EA})), U16({0x30A2, 0x30D7, 0x30EA}),
              "[toUTF16] katakana");
    // lone continuation byte -> U+FFFD
    check_u16(strings::utf8_to_utf16(std::string{static_cast<char>(0x80)}), U16({0xFFFD}),
              "[toUTF16] lone continuation byte");
    // ASCII prefix then invalid byte -> "abc<FFFD>"
    check_u16(strings::utf8_to_utf16(std::string{'a', 'b', 'c', static_cast<char>(0x80)}),
              U16({'a', 'b', 'c', 0xFFFD}), "[toUTF16] abc + 0x80");
    // multiple invalid sequences ending in non-ASCII -> "<FFFD>a<FFFD>b<FFFD>"
    check_u16(strings::utf8_to_utf16(std::string{static_cast<char>(0x80), 'a', static_cast<char>(0x80),
                                                 'b', static_cast<char>(0x80)}),
              U16({0xFFFD, 'a', 0xFFFD, 'b', 0xFFFD}), "[toUTF16] 80 a 80 b 80");
    // invalid sequence followed by trailing ASCII -> "<FFFD>abc"
    check_u16(strings::utf8_to_utf16(std::string{static_cast<char>(0x80), 'a', 'b', 'c'}),
              U16({0xFFFD, 'a', 'b', 'c'}), "[toUTF16] 80 abc");
    // astral: 😀 (U+1F600) round-trips to a surrogate pair
    check_u16(strings::utf8_to_utf16(W({0x1F600})), U16({0xD83D, 0xDE00}), "[toUTF16] astral 😀");
    // lone surrogate encoded as WTF-8 passes through (bun fallback semantics)
    check_u16(strings::wtf8_to_utf16(W({0xD800})), U16({0xD800}), "[toUTF16] lone surrogate WTF-8");
}

// ---------------------------------------------------------------------------
// UTF-16 -> UTF-8 / WTF-8 round-trips
// ---------------------------------------------------------------------------

void test_utf16_to_utf8() {
    check_bytes(strings::utf16_to_utf8(U16({'a', 'b', 'c'})), "abc", "[utf16->utf8] ASCII");
    check_bytes(strings::utf16_to_utf8(U16({'c', 'a', 'f', 0xE9})), W({'c', 'a', 'f', 0xE9}),
                "[utf16->utf8] café");
    check_bytes(strings::utf16_to_utf8(U16({0xD83D, 0xDE00})), W({0x1F600}),
                "[utf16->utf8] astral 😀");
    // lone surrogate -> U+FFFD in strict UTF-8
    check_bytes(strings::utf16_to_utf8(U16({0xD800})), W({0xFFFD}),
                "[utf16->utf8] lone surrogate -> FFFD");
    // lone surrogate preserved in WTF-8 (3-byte encoding)
    check_bytes(strings::utf16_to_wtf8(U16({0xD800})), W({0xD800}),
                "[utf16->wtf8] lone surrogate preserved");
    // WTF-8 round-trip for a lone surrogate: 0xD800 <-> ED A0 80
    check_u16(strings::wtf8_to_utf16(strings::utf16_to_wtf8(U16({0xD800, 'x'}))), U16({0xD800, 'x'}),
              "[wtf8 round-trip] lone surrogate + x");
}

// ---------------------------------------------------------------------------
// Latin-1 / ASCII detection and conversion
// ---------------------------------------------------------------------------

void test_latin1_and_detection() {
    check_bool(strings::is_all_ascii("hello world"), true, "[is_all_ascii] hello world");
    check_bool(strings::is_all_ascii(W({'c', 'a', 'f', 0xE9})), false, "[is_all_ascii] café");
    check_bool(strings::first_non_ascii("abc").has_value(), false, "[first_non_ascii] abc none");
    check_size(strings::first_non_ascii(std::string{'a', 'b', static_cast<char>(0x80)}).value_or(999),
               2, "[first_non_ascii] ab80 -> 2");
    check_bool(strings::is_all_latin1(U16({'a', 0xFF})), true, "[is_all_latin1] a + 0xFF");
    check_bool(strings::is_all_latin1(U16({'a', 0x100})), false, "[is_all_latin1] a + 0x100");

    // Latin-1 (byte 0xE9 = é) -> UTF-8 two-byte C3 A9
    check_bytes(strings::latin1_to_utf8(std::string{'c', 'a', 'f', static_cast<char>(0xE9)}),
                W({'c', 'a', 'f', 0xE9}), "[latin1->utf8] café");
    check_u16(strings::latin1_to_utf16(std::string{static_cast<char>(0xE9)}), U16({0xE9}),
              "[latin1->utf16] é");
    check_size(strings::utf8_length_of_latin1(std::string{'a', static_cast<char>(0xE9), 'b'}), 4,
               "[utf8_length_of_latin1] a é b");
    check_size(strings::utf8_length_of_utf16(U16({'a', 0x30A2, 0xD83D, 0xDE00})), 1 + 3 + 4,
               "[utf8_length_of_utf16] a katakana astral");
    check_bool(strings::wtf8_sequence_length(0xC3) == 2, true, "[wtf8_seq_len] 0xC3 2-byte lead");
    check_bool(strings::wtf8_sequence_length(0xE9) == 3, true, "[wtf8_seq_len] 0xE9 3-byte lead");
    check_bool(strings::wtf8_sequence_length(0xF0) == 4, true, "[wtf8_seq_len] 0xF0 4-byte lead");
    check_bool(strings::wtf8_sequence_length(0x80) == 1, true, "[wtf8_seq_len] stray cont");

    // codepoint iteration
    std::vector<char32_t> cps;
    strings::for_each_codepoint(W({'a', 0x30A2, 0x1F600}), [&](char32_t c) { cps.push_back(c); });
    check_bool(cps.size() == 3 && cps[0] == 'a' && cps[1] == 0x30A2 && cps[2] == 0x1F600, true,
               "[for_each_codepoint] a katakana astral");
}

// ---------------------------------------------------------------------------
// strip_ansi (stripANSI.test.ts explicit [input, expected] tuples + unambiguous)
// ---------------------------------------------------------------------------

void ck_strip(std::string input, std::string expected, std::string what) {
    check_bytes(strings::strip_ansi(input), expected, what);
}

void test_strip_ansi() {
    // Unambiguous well-formed CSI/OSC around text (bun == strip-ansi).
    ck_strip(W({0x1b, '[', '3', '1', 'm', 'r', 'e', 'd', 0x1b, '[', '3', '9', 'm'}), "red",
             "[strip] CSI red");
    ck_strip(W({0x1b, '[', '1', ';', '3', '1', 'm', 'b', 'o', 'l', 'd', ' ', 'r', 'e', 'd', 0x1b,
                '[', '0', 'm'}),
             "bold red", "[strip] combined");
    ck_strip("plain text", "plain text", "[strip] plain text unchanged");
    ck_strip("", "", "[strip] empty");
    // source: explicit [input, expected] tuples
    ck_strip(W({0x1b, ']', '0', ';', 'w', 'i', 'n', 'd', 'o', 'w', ' ', 't', 'i', 't', 'l', 'e', 0x07,
                't', 'e', 'x', 't'}),
             "text", "[strip] OSC BEL");
    ck_strip(W({0x1b, ']', '0', ';', 'w', 'i', 'n', 'd', 'o', 'w', ' ', 't', 'i', 't', 'l', 'e', 0x1b,
                '\\', 't', 'e', 'x', 't'}),
             "text", "[strip] OSC ST");
    ck_strip(W({0x1b, '*', 'B', 't', 'e', 'x', 't'}), "text", "[strip] ESC * B two-byte");
    ck_strip(W({0x1b, '+', 'B', 't', 'e', 'x', 't'}), "text", "[strip] ESC + B two-byte");
    ck_strip(W({0x1b, '7', 't', 'e', 'x', 't'}), "text", "[strip] ESC 7");
    ck_strip(W({0x1b, '8', 't', 'e', 'x', 't'}), "text", "[strip] ESC 8");
    ck_strip(W({0x1b, '#', '8', 't', 'e', 'x', 't'}), "text", "[strip] ESC # 8");
    ck_strip(W({0x1b, '%', 'G', 't', 'e', 'x', 't'}), "text", "[strip] ESC % G");
    ck_strip(W({'t', 'e', 'x', 't', 0x1b}), "text", "[strip] trailing ESC");
    ck_strip(W({'t', 'e', 'x', 't', 0x1b, '['}), "text", "[strip] trailing CSI open");
    ck_strip(W({0x1b, ']', 'i', 'n', 'c', 'o', 'm', 'p', 'l', 'e', 't', 'e'}), "",
             "[strip] incomplete OSC consumes rest");
    ck_strip(W({0x1b, ']'}), "", "[strip] lone OSC open");
    // C1 CSI (0x9b) recognised; standalone C1 ST (0x9c) preserved
    ck_strip(W({0x9b, '3', '1', 'm', 't', 'e', 'x', 't', 0x9b, '3', '9', 'm'}), "text",
             "[strip] C1 CSI 0x9b");
    ck_strip(W({0x9c, 't', 'e', 'x', 't'}), W({0x9c, 't', 'e', 'x', 't'}),
             "[strip] standalone C1 ST 0x9c preserved");
    // OSC hyperlink keeps the link text
    ck_strip(W({0x1b, ']', '8', ';', ';', 'h', 't', 't', 'p', 0x07, 'l', 'i', 'n', 'k', 0x1b, ']',
                '8', ';', ';', 0x07}),
             "link", "[strip] OSC-8 hyperlink");
    // unicode content passes through
    ck_strip(W({0x1b, '[', '3', '1', 'm', 0x4F60, 0x597D, 0x1b, '[', '3', '9', 'm'}), W({0x4F60, 0x597D}),
             "[strip] CSI around 你好");
    ck_strip(W({0x1b, '[', '3', '2', 'm', 0x1F600, 0x1b, '[', '3', '9', 'm'}), W({0x1F600}),
             "[strip] CSI around 😀");
    // strip a single escape character
    ck_strip(W({0x1b}), "", "[strip] single ESC");
    // ESC + ordinary two-byte handling: ESC SP <x>
    ck_strip(W({0x1b, '[', '3', '1', 'm', 0x1b, ' ', 'i', 'n', ' ', 't', 'e', 'x', 't', 0x1b, '[',
                '3', '9', 'm'}),
             "n text", "[strip] ESC SP x mid-sequence");
}

// ---------------------------------------------------------------------------
// string_width (stringWidth.test.ts "extended" + "SIMD fast paths", explicit values)
// ---------------------------------------------------------------------------

void ckw(std::string input, std::size_t expected, std::string what,
         strings::StringWidthOptions opts = {}) {
    check_size(strings::string_width(input, opts), expected, what);
}

void ckw16(std::u16string input, std::size_t expected, std::string what,
           strings::StringWidthOptions opts = {}) {
    check_size(strings::string_width(std::span<const char16_t>{input}, opts), expected, what);
}

void ckw_latin1(std::vector<unsigned char> input, std::size_t expected, std::string what,
                strings::StringWidthOptions opts = {}) {
    check_size(strings::string_width(std::span<const unsigned char>{input}, opts), expected, what);
}

std::string repeat_w(std::initializer_list<char32_t> cps, int n) {
    std::string one{W(cps)};
    std::string out;
    for (int k{0}; k < n; ++k) {
        out += one;
    }
    return out;
}

void test_string_width_zero_width() {
    // source: stringWidth extended > zero-width characters
    ckw(W({0x00AD}), 0, "[width] soft hyphen");
    ckw(W({'a', 0x00AD, 'b'}), 2, "[width] a soft-hyphen b");
    ckw(W({0x2060}), 0, "[width] word joiner");
    ckw(W({'a', 0x2060, 'b'}), 2, "[width] a WJ b");
    ckw(W({0x200B}), 0, "[width] ZWSP");
    ckw(W({'a', 0x200B, 'b', 0x200C, 'c', 0x200D, 'd'}), 4, "[width] ZW joiners between letters");
    ckw(W({0x200E}), 0, "[width] LRM");
    ckw(W({0x202E, 'a', 'b', 'c', 0x202C}), 3, "[width] bidi override abc");
    ckw(W({0x2066, 'a', 'b', 'c', 0x2069}), 3, "[width] isolate abc");
    ckw(W({0x061C}), 0, "[width] Arabic letter mark");
    ckw(W({0xFEFF}), 0, "[width] BOM");
    ckw(W({0xFEFF, 'h', 'e', 'l', 'l', 'o'}), 5, "[width] BOM hello");
    ckw(W({0x0600}), 0, "[width] Arabic number sign");
    ckw(W({0x0600, 'h', 'e', 'l', 'l', 'o'}), 5, "[width] Arabic number sign hello");
    ckw(W({0xFE00}), 0, "[width] VS1");
    ckw(W({0xFE0F}), 0, "[width] VS16 alone");
    ckw(W({0x0300}), 0, "[width] combining grave");
    ckw(W({'e', 0x0301}), 1, "[width] e + combining acute");
    ckw(W({0xD800}), 0, "[width] lone high surrogate");
    ckw(W({0xDFFF}), 0, "[width] lone low surrogate");
    ckw(W({0x00}), 0, "[width] NUL");
    ckw(W({0x7F}), 0, "[width] DEL");
    ckw(W({0x80}), 0, "[width] C1 start");
    ckw(W({0x9F}), 0, "[width] C1 end");
    // source: edge cases > only zero-width
    ckw(W({0x200B, 0x200C, 0x200D}), 0, "[width] only zero-width");
}

void test_string_width_utf16_direct() {
    // source: bun stringWidth.cpp visibleUTF16Width and jsFunctionBunStringWidth
    ckw16(u"hello world", 11, "[width16] ASCII");
    ckw16(u"\u4E2D\u6587", 4, "[width16] CJK");
    ckw16(u"\U0001F469\u200D\U0001F4BB", 2, "[width16] emoji ZWJ");
    ckw16(u"e\u0301", 1, "[width16] combining acute");
    ckw16(u"\x1b[31mred\x1b[0m", 3, "[width16] ANSI excluded");
    ckw16(std::u16string{char16_t{0xD800}, u'a'}, 1, "[width16] lone lead skipped");
    ckw16(std::u16string{char16_t{0xDFFF}, u'a'}, 1, "[width16] lone trail skipped");
    ckw16(u"\u00A7", 2, "[width16] ambiguous wide",
          strings::StringWidthOptions{.count_ansi_escape_codes = false,
                                      .ambiguous_is_narrow = false});
}

void test_string_width_latin1_direct() {
    // source: bun stringWidth.cpp visibleLatin1Width / JSString 8-bit path
    ckw_latin1({'c', 'a', 'f', 0xE9}, 4, "[width-latin1] café");
    ckw_latin1({0x1B, '[', '3', '1', 'm', 'r', 'e', 'd', 0x1B, '[', '0', 'm'}, 3,
               "[width-latin1] ANSI excluded");
    ckw_latin1({0xA7}, 1, "[width-latin1] ambiguous option is bug-compatible narrow",
               strings::StringWidthOptions{.count_ansi_escape_codes = false,
                                           .ambiguous_is_narrow = false});
}

void test_string_width_csi_osc() {
    // source: stringWidth extended > CSI sequences / OSC sequences
    ckw(W({'a', 0x1b, '[', '5', 'A', 'b'}), 2, "[width] CSI cursor up");
    ckw(W({'a', 0x1b, '[', '1', '0', ';', '2', '0', 'H', 'b'}), 2, "[width] CSI cursor pos");
    ckw(W({'a', 0x1b, '[', '3', '8', ';', '5', ';', '1', '9', '6', 'm', 'b'}), 2,
        "[width] CSI 256-color");
    ckw(W({0x1b, '[', '3', '1', 'm', 0x1b, '[', '1', 'm', 'h', 'e', 'l', 'l', 'o', 0x1b, '[', '0',
           'm'}),
        5, "[width] multiple CSI + hello");
    // OSC-8 hyperlink with BEL / ST terminator
    ckw(W({0x1b, ']', '8', ';', ';', 'u', 'r', 'l', 0x07, 'l', 'i', 'n', 'k', 0x1b, ']', '8', ';',
           ';', 0x07}),
        4, "[width] OSC-8 BEL");
    ckw(W({0x1b, ']', '8', ';', ';', 'u', 'r', 'l', 0x1b, '\\', 'l', 'i', 'n', 'k', 0x1b, ']', '8',
           ';', ';', 0x1b, '\\'}),
        4, "[width] OSC-8 ST");
    // unterminated OSC in UTF-16 string: only the leading CJK counts
    {
        std::string in{W({0x4E2D, 0x1b, ']', '8', ';', ';'})};
        in += std::string(100, 'x');
        ckw(in, 2, "[width] unterminated OSC after 中");
    }
    // bare ESC followed by non-sequence char
    ckw(W({'a', 0x1b, 'X', 'b'}), 3, "[width] bare ESC X");
    // ESC ESC starts new sequence
    ckw(W({0x1b, 0x1b, '[', '3', '1', 'm', 'r', 'e', 'd', 0x1b, '[', '0', 'm'}), 3,
        "[width] ESC ESC CSI red");
    ckw(W({'a', 0x1b, 0x1b, 'b'}), 2, "[width] ESC ESC ordinary");
}

void test_string_width_emoji() {
    // source: stringWidth extended > emoji handling
    ckw(W({0x1F600}), 2, "[width] grinning face");
    ckw(W({0x2764, 0xFE0F}), 2, "[width] heart + VS16");
    ckw(W({0x1F1FA, 0x1F1F8}), 2, "[width] US flag (RI pair)");
    ckw(W({0x1F1E6}), 1, "[width] single regional indicator");
    ckw(W({0x1F44B, 0x1F3FD}), 2, "[width] wave + skin tone");
    ckw(W({0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466}), 2, "[width] family ZWJ");
    ckw(W({0x1F469, 0x200D, 0x1F4BB}), 2, "[width] woman technologist");
    ckw(W({'1', 0xFE0F, 0x20E3}), 2, "[width] keycap 1");
    ckw(W({0x2600, 0xFE0F}), 2, "[width] sun + VS16");
    ckw(W({0x2600, 0xFE0E}), 1, "[width] sun + VS15 (text)");
    ckw(W({0x2764, 0xFE0E}), 1, "[width] heart + VS15 (text)");
    ckw(W({'0', 0xFE0F}), 1, "[width] digit + VS16 stays narrow");
    ckw(W({'a', 0xFE0F}), 1, "[width] letter + VS16 stays narrow");
    ckw(W({0x00A9, 0xFE0F}), 2, "[width] copyright + VS16 -> emoji");
    ckw(W({0x00A9, 0xFE0E}), 1, "[width] copyright + VS15 -> text");
    // emoji in context: "Hello 👋 World"
    ckw(W({'H', 'e', 'l', 'l', 'o', ' ', 0x1F44B, ' ', 'W', 'o', 'r', 'l', 'd'}), 14,
        "[width] Hello wave World");
    ckw(W({0x1F3E0, 0x1F3E1, 0x1F3E2}), 6, "[width] three houses");
}

void test_string_width_east_asian() {
    // source: stringWidth extended > East Asian Width
    ckw(W({0x4E2D}), 2, "[width] 中");
    ckw(W({0x4E2D, 0x6587}), 4, "[width] 中文");
    ckw(W({0x65E5, 0x672C, 0x8A9E}), 6, "[width] 日本語");
    ckw(W({0xD55C, 0xAE00}), 4, "[width] 한글");
    ckw(W({0xFF21}), 2, "[width] fullwidth A");
    ckw(W({0xFF01}), 2, "[width] fullwidth !");
    ckw(W({0xFF71}), 1, "[width] halfwidth katakana A");
    ckw(W({'h', 'e', 'l', 'l', 'o', 0x4E16, 0x754C}), 9, "[width] hello 世界");
    // source: stringWidth.cpp static_asserts — U+00A7 section sign is East Asian
    // Ambiguous: narrow (1) by default, wide (2) when ambiguousIsNarrow=false.
    ckw(W({0x00A7}), 1, "[width] § ambiguous narrow (default)");
    ckw(W({0x00A7}), 2, "[width] § ambiguous wide",
        strings::StringWidthOptions{.count_ansi_escape_codes = false, .ambiguous_is_narrow = false});
}

void test_string_width_indic() {
    // source: stringWidth extended > Indic scripts / Devanagari conjuncts (GB9c)
    ckw(W({0x0915}), 1, "[width] Ka");
    ckw(W({0x0915, 0x094D}), 1, "[width] Ka + virama");
    ckw(W({0x0915, 0x093F}), 1, "[width] Ka + vowel sign i");
    ckw(W({0x0E01}), 1, "[width] Thai Ko Kai");
    ckw(W({0x0E01, 0x0E47}), 1, "[width] Thai with maitaikhu");
    ckw(W({0x0E32}), 1, "[width] Thai SARA AA (spacing)");
    ckw(W({0x0E01, 0x0E32}), 2, "[width] Ko + SARA AA");
    ckw(W({0x0E31}), 0, "[width] Thai MAI HAN-AKAT (combining)");
    ckw(W({0x0E01, 0x0E31}), 1, "[width] Ko + MAI HAN-AKAT");
    ckw(W({0x093D}), 1, "[width] Devanagari Avagraha (visible)");
    ckw(W({0x0915, 0x094D, 0x0937}), 2, "[width] Ka+Virama+Ssa conjunct");
    ckw(W({0x0915, 0x094D, 0x200D, 0x0937}), 2, "[width] Ka+Virama+ZWJ+Ssa conjunct");
    ckw(W({0x0915, 0x094D, 0x0915, 0x094D, 0x0915}), 3, "[width] three consonants joined");
}

void test_string_width_edge() {
    // source: stringWidth extended > edge cases / ANSI preserve grapheme state
    ckw("", 0, "[width] empty");
    ckw(std::string(10000, 'a'), 10000, "[width] 10000 a");
    ckw(repeat_w({0x1F600}, 1000), 2000, "[width] 1000 grinning");
    ckw(W({'H', 'e', 'l', 'l', 'o', 0x1b, '[', '3', '1', 'm', 0x4E16, 0x754C, 0x1b, '[', '0', 'm',
           0x1F44B}),
        11, "[width] mixed content");
    // "ANSI sequences preserve grapheme state"
    ckw(W({0xFE0F, '?'}), 1, "[width] VS16 orphan + ?");
    ckw(W({0x1b, '[', '1', 'm', 0xFE0F, '?'}), 1, "[width] SGR + VS16 + ?");
    ckw(W({'e', 0x1b, '[', '1', 'm', 0x0301}), 1, "[width] e + SGR + combining acute");
    ckw(W({0x1F469, 0x1b, '[', '1', 'm', 0x200D, 0x1b, '[', '2', '2', 'm', 0x1F4BB}), 2,
        "[width] woman + SGR + ZWJ + SGR + laptop");
    ckw(W({0xFE0F, 0x200D}), 0, "[width] orphan VS16 + ZWJ");
    // countAnsiEscapeCodes=false (default): SGR excluded
    ckw(W({'h', 'e', 'l', 'l', 'o', ' ', 0x1b, '[', '3', '1', 'm', 'r', 'e', 'd', 0x1b, '[', '3',
           '9', 'm', ' ', 'w', 'o', 'r', 'l', 'd'}),
        15, "[width] SGR excluded (default)");
    // countAnsiEscapeCodes=true: ANSI counted as visible
    ckw(W({'h', 'e', 'l', 'l', 'o', ' ', 0x1b, '[', '3', '1', 'm', 'r', 'e', 'd', 0x1b, '[', '3',
           '9', 'm', ' ', 'w', 'o', 'r', 'l', 'd'}),
        23, "[width] SGR counted",
        strings::StringWidthOptions{.count_ansi_escape_codes = true, .ambiguous_is_narrow = true});
    // consistency: stringWidth(s) == stringWidth(stripANSI(s))
    {
        std::string s{W({0x1b, '[', '1', 'm', 0xFE0F, '?'})};
        check_size(strings::string_width(s), strings::string_width(strings::strip_ansi(s)),
                   "[width] consistency with strip_ansi");
    }
}

void test_string_width_fuzz() {
    // source: stringWidth extended > fuzzer-like stress tests (explicit expectations)
    ckw(repeat_w({0x1b}, 10000), 0, "[width] 10000 bare ESC");
    ckw(repeat_w({'a', 0x00, 'b', 0x00, 'c', 0x00}, 3000), 9000, "[width] NUL interspersed");
    ckw(repeat_w({'a', 0x7F, 'b', 0x7F, 'c'}, 3000), 9000, "[width] DEL interspersed");
    ckw(repeat_w({'a', 0x00AD, 'b', 0x00AD, 'c', 0x00AD}, 3000), 9000, "[width] soft hyphen stress");
    {
        std::string in{"a"};
        for (int i{0}; i < 1000; ++i) {
            in += W({static_cast<char32_t>(0x0300 + (i % 112))});
        }
        ckw(in, 1, "[width] extremely long single grapheme");
    }
    ckw(repeat_w({0x5B57, 0xE0100}, 5000), 10000, "[width] VS supplement after wide char");
    ckw(repeat_w({0x1F1E6, 0x1F1E7, 0x1F1E8, 0x1F1E9}, 500), 2000,
        "[width] regional indicator pairs (even)");
    // "all CSI final bytes": 63 final bytes * 'a' * 100
    {
        std::string one;
        for (char32_t fb{0x40}; fb <= 0x7e; ++fb) {
            one += W({'a', 0x1b, '[', '1', fb});
        }
        std::string in;
        for (int k{0}; k < 100; ++k) {
            in += one;
        }
        ckw(in, 6300, "[width] all CSI final bytes");
    }
}

// ---------------------------------------------------------------------------
// SIMD-fast-path coverage (explicit values from "stringWidth SIMD fast paths")
// ---------------------------------------------------------------------------

void test_string_width_simd() {
    for (std::size_t n : {std::size_t{0}, 1uz, 2uz, 7uz, 15uz, 16uz, 17uz, 31uz, 32uz, 33uz, 63uz,
                          64uz, 65uz, 100uz, 127uz, 128uz, 129uz, 255uz, 256uz, 257uz, 1000uz,
                          4096uz}) {
        ckw(std::string(n, 'x'), n, std::format("[width simd] {} x", n));
        ckw(std::string(n, 'x'), n, std::format("[width simd] {} x count-ansi", n),
            strings::StringWidthOptions{.count_ansi_escape_codes = true});
    }
    // Latin-1 (8-bit, non-ASCII)
    ckw(W({'c', 'a', 'f', 0xE9}), 4, "[width simd] café");
    ckw(W({'n', 'a', 0xEF, 'v', 'e', ' ', 'f', 'a', 0xE7, 'a', 'd', 'e'}), 12, "[width simd] naïve façade");
    ckw(W({0x00A7, 0x00B6, 0x00B1}), 3, "[width simd] §¶±");
    ckw(W({'c', 'o', 0x00AD, 'o', 'p', 'e', 'r', 'a', 't', 'e'}), 9, "[width simd] co-operate");
    ckw(repeat_w({0xE9}, 100), 100, "[width simd] 100 é");
    // SGR mid Latin-1 / UTF-16 text
    ckw(W({'h', 'e', 'l', 'l', 'o', ' ', 0x1b, '[', '3', '1', 'm', 'r', 'e', 'd', 0x1b, '[', '3',
           '9', 'm', ' ', 'w', 'o', 'r', 'l', 'd'}),
        15, "[width simd] SGR mid Latin-1");
    ckw(W({0x5B89, 0x1b, '[', '3', '1', 'm', 0x5EB7, 0x1b, '[', '3', '9', 'm', '!'}), 5,
        "[width simd] SGR mid UTF-16");
    ckw(W({0x1F600, 0x1b, '[', '1', 'm', 'o', 'k', 0x1b, '[', '2', '2', 'm', 0x1F600}), 6,
        "[width simd] emoji + SGR + ok + emoji");
    ckw(W({0x4E2D, 0x6587}), 4, "[width simd] 中文");
    ckw(W({0x3053, 0x3093, 0x306B, 0x3061, 0x306F}), 10, "[width simd] こんにちは");
}

}  // namespace

int main() {
    test_utf8_to_utf16();
    test_utf16_to_utf8();
    test_latin1_and_detection();
    test_strip_ansi();
    test_string_width_zero_width();
    test_string_width_utf16_direct();
    test_string_width_latin1_direct();
    test_string_width_csi_osc();
    test_string_width_emoji();
    test_string_width_east_asian();
    test_string_width_indic();
    test_string_width_edge();
    test_string_width_fuzz();
    test_string_width_simd();

    std::println("core.strings: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
