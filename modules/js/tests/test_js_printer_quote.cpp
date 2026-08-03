// test_js_printer_quote.cpp — CAP-BUILD-PRINTER shard 1/4 test suite.
//
// Covers the string / quote-selection / escaping layer ported from
// .mbun/bun-ref/src/js_printer/lib.rs:792-1197 (mbun.js_printer.{encoding,quote,
// whitespacer,flags,options,printer_core,printer}).
//
// ── Where the expectations come from ─────────────────────────────────────────
// NOT from reading the Rust. Every `bun_says` vector below is transcribed from
// the actual output of real bun 1.3.14, captured with:
//
//   bun -e 'const t = new Bun.Transpiler({loader:"ts"});
//           console.log(JSON.stringify(t.transformSync(<src>)))'
//
// (and the same with `{loader:"ts", target:"bun"}` for the ascii-only vectors).
// The comment on each group records the source it was fed and the bytes it
// printed. That is the spec — AGENTS.md 核心原则 1: bun's behavior, including the
// bug-compatible edges, IS the acceptance criterion.
//
// The layer under test is what real bun's printer bottoms out in for those
// cases: transformSync re-prints from the AST, so `'a\nb'` coming back as a
// backtick string with a raw LF is `best_quote_char_for_string` +
// `write_pre_quoted_string_inner` and nothing else.
import std;
import mbun.js_printer;

namespace {

using mbun::js_printer::best_quote_char_for_string;
using mbun::js_printer::bmp_escape;
using mbun::js_printer::can_print_without_escape;
using mbun::js_printer::Encoding;
using mbun::js_printer::estimate_length_for_utf8;
using mbun::js_printer::hex2_upper;
using mbun::js_printer::index_of_needs_escape_for_js_string;
using mbun::js_printer::index_of_needs_escape_scalar;
using mbun::js_printer::Options;
using mbun::js_printer::PrinterFlags;
using mbun::js_printer::quote_for_json;
using mbun::js_printer::StringSink;
using mbun::js_printer::surrogate_pair_escape;
using mbun::js_printer::write_json_string;
using mbun::js_printer::write_pre_quoted_string;
using mbun::js_printer::write_pre_quoted_string_inner;
using mbun::js_printer::ws;

int gChecks { 0 };
int gFailures { 0 };

std::string printable(std::string_view s) {
    std::string out;
    for (const unsigned char c : s) {
        if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20 || c >= 0x7F) {
            out += std::format("\\x{:02X}", c);
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view what) {
    gChecks += 1;
    if (actual != expected) {
        gFailures += 1;
        std::println("FAIL {}\n  expected: \"{}\"\n  actual:   \"{}\"", what,
            printable(expected), printable(actual));
    }
}

void check_true(bool cond, std::string_view what) {
    gChecks += 1;
    if (!cond) {
        gFailures += 1;
        std::println("FAIL {}", what);
    }
}

// Escape `text` the way the printer does, quotes included, so a vector reads as
// the literal real bun emits.
template <Encoding ENC>
std::string quoted(std::string_view text, std::uint8_t quote, bool asciiOnly, bool json = false) {
    std::string out;
    StringSink sink { out };
    out.push_back(static_cast<char>(quote));
    write_pre_quoted_string_inner<ENC>(text, sink, quote, asciiOnly, json);
    out.push_back(static_cast<char>(quote));
    return out;
}

std::uint8_t best_quote_u8(std::string_view s, bool allowBacktick) {
    return best_quote_char_for_string<char>(std::span<const char> { s.data(), s.size() }, allowBacktick);
}

std::uint8_t best_quote_u16(std::u16string_view s, bool allowBacktick) {
    return best_quote_char_for_string<char16_t>(
        std::span<const char16_t> { s.data(), s.size() }, allowBacktick);
}

// ─────────────────────────────────────────────────────────────────────────────
// can_print_without_escape — ref lib.rs:792
// ─────────────────────────────────────────────────────────────────────────────
void test_can_print_without_escape() {
    // Printable ASCII, minus the five the printer always handles itself.
    check_true(can_print_without_escape('a', false), "cpwe: 'a'");
    check_true(can_print_without_escape(' ', false), "cpwe: space (FIRST_ASCII)");
    check_true(can_print_without_escape('~', false), "cpwe: '~' (LAST_ASCII)");
    check_true(!can_print_without_escape(0x1F, false), "cpwe: 0x1F below FIRST_ASCII");
    check_true(!can_print_without_escape('\\', false), "cpwe: backslash");
    check_true(!can_print_without_escape('"', false), "cpwe: double quote");
    check_true(!can_print_without_escape('\'', false), "cpwe: single quote");
    check_true(!can_print_without_escape('`', false), "cpwe: backtick");
    // `$` is rejected for EVERY quote char, not just backtick — see the note in
    // quote.cppm about why that is consistent with the scan, which only stops on
    // `$` for backticks.
    check_true(!can_print_without_escape('$', false), "cpwe: dollar");

    // Non-ASCII: gated entirely on ascii_only ...
    check_true(can_print_without_escape(0x00E9, false), "cpwe: U+00E9 !ascii_only");
    check_true(!can_print_without_escape(0x00E9, true), "cpwe: U+00E9 ascii_only");
    check_true(can_print_without_escape(0x1F600, false), "cpwe: U+1F600 !ascii_only");
    check_true(!can_print_without_escape(0x1F600, true), "cpwe: U+1F600 ascii_only");

    // ... except the four that are never printable raw regardless: the BOM and
    // the two line terminators (they'd break the line structure) and surrogates.
    check_true(!can_print_without_escape(0xFEFF, false), "cpwe: BOM never raw");
    check_true(!can_print_without_escape(0x2028, false), "cpwe: U+2028 never raw");
    check_true(!can_print_without_escape(0x2029, false), "cpwe: U+2029 never raw");
    check_true(!can_print_without_escape(0xD800, false), "cpwe: lead surrogate never raw");
    check_true(!can_print_without_escape(0xDFFF, false), "cpwe: trail surrogate never raw");
    check_true(can_print_without_escape(0xD7FF, false), "cpwe: just below surrogates");
    check_true(can_print_without_escape(0xE000, false), "cpwe: just above surrogates");
}

// ─────────────────────────────────────────────────────────────────────────────
// best_quote_char_for_string — ref lib.rs:812
//
// Vectors from real bun 1.3.14, `new Bun.Transpiler({loader:"ts"})`:
//   in: const s = "abc";                  out: const s = "abc";
//   in: const s = 'has "double" quotes';  out: const s = 'has "double" quotes';
//   in: const s = "has 'single' quotes";  out: const s = "has 'single' quotes";
//   in: const s = 'has "d" and \'s\'';    out: const s = `has "d" and 's'`;
//   in: const s = 'a "" b \'';            out: const s = `a "" b '`;
//   in: const s = 'x"""y\'\'\'z';         out: const s = `x"""y'''z`;
//   in: const s = 'a\nb';                 out: const s = `a<LF>b`;
//   in: const s = 'a`b';                  out: const s = "a`b";
//   in: const s = 'a${b}c';               out: const s = "a${b}c";
// ─────────────────────────────────────────────────────────────────────────────
void test_best_quote_char() {
    // No quotes anywhere → double. backtick_cost(0) < min(0,0)=0 is FALSE, so
    // the backtick branch does not fire on a cost tie. That `<` (not `<=`) is
    // why plain strings print with `"` and not backticks.
    check_true(best_quote_u8("abc", true) == '"', "bqc: plain → \"");

    // double_cost=2, single_cost=0 → single wins.
    check_true(best_quote_u8("has \"double\" quotes", true) == '\'', "bqc: has \" → '");
    // single_cost=2, double_cost=0 → neither backtick (0 < min(2,0)=0 false) nor
    // single (2 < 0 false) → double.
    check_true(best_quote_u8("has 'single' quotes", true) == '"', "bqc: has ' → \"");
    // 2/2/0 → backtick (0 < min(2,2)=2).
    check_true(best_quote_u8("has \"d\" and 's'", true) == '`', "bqc: both → `");
    // 1/2/0 → backtick.
    check_true(best_quote_u8("a \"\" b '", true) == '`', "bqc: 1 single 2 double → `");
    // 3/3/0 → backtick.
    check_true(best_quote_u8("x\"\"\"y'''z", true) == '`', "bqc: 3/3 → `");

    // allow_backtick=false disables the whole branch: 2/2 ties → double.
    check_true(best_quote_u8("has \"d\" and 's'", false) == '"', "bqc: both, no backtick → \"");

    // A newline costs BOTH ' and " but not ` — this is the entire reason real
    // bun prints 'a\nb' as a backtick string with a literal newline in it.
    check_true(best_quote_u8("a\nb", true) == '`', "bqc: newline → `");
    check_true(best_quote_u8("a\nb", false) == '"', "bqc: newline, no backtick → \"");

    // A backslash consumes the NEXT unit (:831), so an escaped quote costs
    // nothing. `\"` → double_cost stays 0 → tie at 0/0 → double.
    check_true(best_quote_u8("a\\\"b", true) == '"', "bqc: backslash consumes next");

    // `$` only costs a backtick when followed by `{`.
    check_true(best_quote_u8("a$b", true) == '"', "bqc: lone $ costs nothing");
    // "'${'" → single=1, backtick=1. backtick(1) < min(1, 0)=0? no. single(1) <
    // double(0)? no → double. Matches real bun printing `"a${b}c"` with quotes.
    check_true(best_quote_u8("a${b}c", true) == '"', "bqc: ${ → not backtick here");
    // With a real reason to leave " and ': 2 doubles + 2 singles vs 1 `${`.
    check_true(best_quote_u8("\"\"''${", true) == '`', "bqc: ${ costs 1, still cheapest");

    // A backtick in the text costs the backtick quote.
    check_true(best_quote_u8("a`b", true) == '"', "bqc: backtick in text → \"");
    // 1 backtick vs 2+2 → backtick still cheapest.
    check_true(best_quote_u8("`\"\"''", true) == '`', "bqc: 1 backtick beats 2/2");

    // ── the 1024-unit cap (:820) — bug-compatible, do not "fix" ──
    // 1024 'a' then 500 '"'. The doubles are all past the cap, so they are never
    // costed and the result is the 0/0 tie → double, even though double is
    // objectively the worst choice for this string.
    {
        std::string s(1024, 'a');
        s.append(500, '"');
        check_true(best_quote_u8(s, true) == '"', "bqc: >1024 tail is not costed");
    }
    // Same string with the doubles inside the window → single.
    {
        std::string s(1000, 'a');
        s.append(24, '"');
        check_true(best_quote_u8(s, true) == '\'', "bqc: <1024 doubles are costed");
    }
    // The `$` arm bounds-checks against str.len(), NOT the 1024 cap (:835), so a
    // `$` at index 1023 does look at index 1024 and find the `{`.
    {
        std::string s(1023, 'a');
        s += "${";
        s.append(4, '"');
        s.append(4, '\'');
        // backtick_cost=1 (the ${ at 1023), single=0, double=0 (both past cap).
        // 1 < min(0,0)=0? no → double.
        check_true(best_quote_u8(s, true) == '"', "bqc: $ at 1023 reads index 1024");
    }

    // UTF-16 element type — the same function, ref :2600.
    check_true(best_quote_u16(u"has \"double\" quotes", true) == '\'', "bqc16: has \" → '");
    check_true(best_quote_u16(u"a\nb", true) == '`', "bqc16: newline → `");
    check_true(best_quote_u16(u"abc", true) == '"', "bqc16: plain → \"");
}

// ─────────────────────────────────────────────────────────────────────────────
// write_pre_quoted_string_inner — ref lib.rs:974
//
// Vectors from real bun 1.3.14, `new Bun.Transpiler({loader:"ts"})`. The `out`
// column is the exact string after `const s = `:
//   'a\tb'      → "a\tb"        (escaped)
//   '\x07'      → "\x07"
//   '\v'        → "\v"
//   '\b'        → "\b"
//   '\f'        → "\f"
//   '\r'        → "\r"
//   '\0'        → "\x00"        (NUL is NOT \0 — it widens to \x00)
//   'a\\b'      → "a\\b"
//   'a`b'       → "a`b"         (backtick raw inside a "-string)
//   'a${b}c'    → "a${b}c"      ($ raw inside a "-string)
//   '﻿'    → "﻿"      (uppercase hex)
//   ' '    → " "
//   ' '    → " "
//   '\uD800'    → "\uD800"      (lone surrogate survives as an escape)
//   'héllo'     → "héllo"       (raw — default target is NOT ascii-only)
//   '😀'        → "😀" (per-code-unit, NOT recombined)
// ─────────────────────────────────────────────────────────────────────────────
void test_escape_utf8() {
    // Control characters with dedicated escapes (:1067-1149).
    check_eq(quoted<Encoding::Utf8>("a\tb", '"', false), "\"a\\tb\"", "esc: tab");
    check_eq(quoted<Encoding::Utf8>("\x07", '"', false), "\"\\x07\"", "esc: bell");
    check_eq(quoted<Encoding::Utf8>("\x0B", '"', false), "\"\\v\"", "esc: vtab");
    check_eq(quoted<Encoding::Utf8>("\b", '"', false), "\"\\b\"", "esc: backspace");
    check_eq(quoted<Encoding::Utf8>("\f", '"', false), "\"\\f\"", "esc: formfeed");
    check_eq(quoted<Encoding::Utf8>("\r", '"', false), "\"\\r\"", "esc: cr");
    check_eq(quoted<Encoding::Utf8>("a\nb", '"', false), "\"a\\nb\"", "esc: newline in \"");
    // NUL has no dedicated arm, so it falls to `_` → \xHH. Real bun: "\x00".
    check_eq(quoted<Encoding::Utf8>(std::string_view { "\0", 1 }, '"', false), "\"\\x00\"", "esc: NUL → \\x00");
    check_eq(quoted<Encoding::Utf8>("a\\b", '"', false), "\"a\\\\b\"", "esc: backslash");

    // Quote chars: only the ACTIVE quote is escaped; the others go through raw.
    check_eq(quoted<Encoding::Utf8>("a\"b", '"', false), "\"a\\\"b\"", "esc: \" in \"-string");
    check_eq(quoted<Encoding::Utf8>("a\"b", '\'', false), "'a\"b'", "esc: \" in '-string raw");
    check_eq(quoted<Encoding::Utf8>("a'b", '\'', false), "'a\\'b'", "esc: ' in '-string");
    check_eq(quoted<Encoding::Utf8>("a'b", '"', false), "\"a'b\"", "esc: ' in \"-string raw");
    check_eq(quoted<Encoding::Utf8>("a`b", '`', false), "`a\\`b`", "esc: ` in `-string");
    check_eq(quoted<Encoding::Utf8>("a`b", '"', false), "\"a`b\"", "esc: ` in \"-string raw");

    // Backtick strings: LF and TAB go through RAW (that is the point of picking
    // a backtick), and only `${` needs escaping — a lone `$` does not.
    check_eq(quoted<Encoding::Utf8>("a\nb", '`', false), "`a\nb`", "esc: newline raw in `");
    check_eq(quoted<Encoding::Utf8>("a\tb", '`', false), "`a\tb`", "esc: tab raw in `");
    check_eq(quoted<Encoding::Utf8>("a$b", '`', false), "`a$b`", "esc: lone $ raw in `");
    check_eq(quoted<Encoding::Utf8>("a${b}", '`', false), "`a\\${b}`", "esc: ${ escaped in `");
    check_eq(quoted<Encoding::Utf8>("a${b}", '"', false), "\"a${b}\"", "esc: ${ raw in \"");
    // A `$` at the very end has no next unit → no escape.
    check_eq(quoted<Encoding::Utf8>("a$", '`', false), "`a$`", "esc: trailing $ in `");

    // Non-ASCII, ascii_only=false → raw UTF-8 passthrough.
    check_eq(quoted<Encoding::Utf8>("héllo", '"', false), "\"héllo\"", "esc: héllo raw");
    check_eq(quoted<Encoding::Utf8>("é", '"', false), "\"é\"", "esc: é raw");
    // ascii_only=true → \xHH for <=0xFF, \uHHHH above. Real bun target:"bun":
    //   'héllo' → "h\xE9llo" ; 'Ā' → "Ā" ; '😀' → "😀"
    check_eq(quoted<Encoding::Utf8>("héllo", '"', true), "\"h\\xE9llo\"", "esc: héllo ascii_only");
    check_eq(quoted<Encoding::Utf8>("\xC4\x80", '"', true), "\"\\u0100\"", "esc: U+0100 ascii_only");
    check_eq(quoted<Encoding::Utf8>("\U0001F600", '"', true), "\"\\uD83D\\uDE00\"",
        "esc: emoji ascii_only → surrogate pair");
    check_eq(quoted<Encoding::Utf8>("\U0001F600", '"', false), "\"\U0001F600\"",
        "esc: emoji !ascii_only raw (utf8 path)");

    // Never-raw code points, regardless of ascii_only. Uppercase hex. Spelled as
    // explicit bytes: a literal BOM/LS/PS in the source is invisible, and the
    // next editor to touch this file would silently normalise it away.
    check_eq(quoted<Encoding::Utf8>("\xEF\xBB\xBF", '"', false), "\"\\uFEFF\"", "esc: BOM");
    check_eq(quoted<Encoding::Utf8>("\xE2\x80\xA8", '"', false), "\"\\u2028\"", "esc: U+2028");
    check_eq(quoted<Encoding::Utf8>("\xE2\x80\xA9", '"', false), "\"\\u2029\"", "esc: U+2029");

    // Empty / all-printable fast path (the bulk-copy branch at :1046-1055).
    check_eq(quoted<Encoding::Utf8>("", '"', false), "\"\"", "esc: empty");
    check_eq(quoted<Encoding::Utf8>("abc", '"', false), "\"abc\"", "esc: plain");
    // Long enough to exercise several SWAR blocks plus a tail.
    {
        const std::string s(200, 'x');
        check_eq(quoted<Encoding::Utf8>(s, '"', false), "\"" + s + "\"", "esc: long plain");
    }
    // Escapes at block boundaries — the bulk-copy resume must land exactly.
    {
        std::string s(17, 'a');
        s += '"';
        s.append(17, 'b');
        check_eq(quoted<Encoding::Utf8>(s, '"', false),
            "\"" + std::string(17, 'a') + "\\\"" + std::string(17, 'b') + "\"",
            "esc: escape mid-block");
    }
}

void test_escape_utf16() {
    // The Utf16 path iterates CODE UNITS, so an astral code point escapes as two
    // independent surrogates rather than one \u{...}. bun flags this as a TODO at
    // :1035-1037 ("we could parse the whole codepoint... eg \u{10334} will
    // convert to 𐌴 without this") — it is a known deviation from ideal output
    // that real bun ships, so it is the spec. VERIFIED: real bun prints '😀' as
    // "😀" on every target, ascii_only or not, which is exactly what
    // per-code-unit iteration predicts and what the Utf8 path would NOT do.
    const auto u16bytes { [](std::u16string_view s) {
        return std::string_view { reinterpret_cast<const char*>(s.data()), s.size() * 2 };
    } };

    constexpr char16_t BOM16[] { 0xFEFF };

    check_eq(quoted<Encoding::Utf16>(u16bytes(u"abc"), '"', false), "\"abc\"", "esc16: plain");
    check_eq(quoted<Encoding::Utf16>(u16bytes(u"a\tb"), '"', false), "\"a\\tb\"", "esc16: tab");
    check_eq(quoted<Encoding::Utf16>(u16bytes(u"a\"b"), '"', false), "\"a\\\"b\"", "esc16: quote");

    // é: printable when !ascii_only → re-encoded to WTF-8 (:1057-1062).
    check_eq(quoted<Encoding::Utf16>(u16bytes(u"héllo"), '"', false), "\"héllo\"",
        "esc16: é re-encoded to utf8");
    check_eq(quoted<Encoding::Utf16>(u16bytes(u"héllo"), '"', true), "\"h\\xE9llo\"",
        "esc16: é ascii_only");

    // The headline case: 😀 is two code units and escapes as two.
    check_eq(quoted<Encoding::Utf16>(u16bytes(u"\U0001F600"), '"', false), "\"\\uD83D\\uDE00\"",
        "esc16: emoji → \\uD83D\\uDE00 (!ascii_only)");
    check_eq(quoted<Encoding::Utf16>(u16bytes(u"\U0001F600"), '"', true), "\"\\uD83D\\uDE00\"",
        "esc16: emoji → \\uD83D\\uDE00 (ascii_only)");

    // A lone (unpaired) surrogate survives as its own escape — real bun prints
    // '\uD800' as "\uD800". This is why the layer is WTF-16/WTF-8, not UTF.
    {
        const char16_t lone[] { 0xD800 };
        check_eq(quoted<Encoding::Utf16>(u16bytes(std::u16string_view { lone, 1 }), '"', false),
            "\"\\uD800\"", "esc16: lone surrogate");
    }
    check_eq(quoted<Encoding::Utf16>(u16bytes(std::u16string_view{BOM16, 1}), '"', false), "\"\\uFEFF\"", "esc16: BOM");
}

void test_escape_latin1_ascii() {
    // Latin1: every byte is its own code point, re-encoded to WTF-8 on the way
    // out. 0xE9 is 'é', not a UTF-8 lead byte — getting this wrong is the classic
    // Latin1/UTF-8 confusion bun's CLAUDE.md calls out explicitly.
    check_eq(quoted<Encoding::Latin1>("\xE9", '"', false), "\"é\"", "latin1: 0xE9 → é");
    check_eq(quoted<Encoding::Latin1>("\xE9", '"', true), "\"\\xE9\"", "latin1: 0xE9 ascii_only");
    check_eq(quoted<Encoding::Latin1>("abc", '"', false), "\"abc\"", "latin1: plain");
    check_eq(quoted<Encoding::Latin1>("a\tb", '"', false), "\"a\\tb\"", "latin1: tab");

    // Ascii: bun debug-asserts text[i] <= 0x7F (:1030); within contract it is the
    // byte-identity path.
    check_eq(quoted<Encoding::Ascii>("abc", '"', false), "\"abc\"", "ascii: plain");
    check_eq(quoted<Encoding::Ascii>("a\"b", '"', false), "\"a\\\"b\"", "ascii: quote");
    check_eq(quoted<Encoding::Ascii>("a\nb", '`', false), "`a\nb`", "ascii: newline raw in `");
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON — ref lib.rs:1167 / :1189
// ─────────────────────────────────────────────────────────────────────────────
void test_json() {
    // `json = true` suppresses \xHH (not valid JSON) and widens to \u00HH.
    {
        std::string out;
        quote_for_json(std::string_view { "\0", 1 }, out, false);
        check_eq(out, "\"\\u0000\"", "json: NUL → \\u0000 not \\x00");
    }
    {
        std::string out;
        quote_for_json("a\tb\nc", out, false);
        check_eq(out, "\"a\\tb\\nc\"", "json: tab + newline");
    }
    {
        std::string out;
        quote_for_json("say \"hi\"", out, false);
        check_eq(out, "\"say \\\"hi\\\"\"", "json: embedded quotes");
    }
    {
        // ascii_only=false → UTF-8 passthrough (JSON is transported as UTF-8).
        std::string out;
        quote_for_json("héllo", out, false);
        check_eq(out, "\"héllo\"", "json: é raw when !ascii_only");
    }
    {
        std::string out;
        quote_for_json("héllo", out, true);
        check_eq(out, "\"h\\u00E9llo\"", "json: é → \\u00E9 when ascii_only (not \\xE9)");
    }
    {
        // quote_for_json appends — it must not clobber what is already there.
        std::string out { "{\"k\":" };
        quote_for_json("v", out, false);
        check_eq(out, "{\"k\":\"v\"", "json: appends to existing buffer");
    }
    {
        // write_json_string hardcodes ascii_only=false (:1194).
        std::string out;
        StringSink sink { out };
        write_json_string<Encoding::Utf8>("a\"b", sink);
        check_eq(out, "\"a\\\"b\"", "json: write_json_string utf8");
    }
    {
        std::string out;
        StringSink sink { out };
        const char16_t units[] { u'a', 0xD83D, 0xDE00 };
        write_json_string<Encoding::Utf16>(
            std::string_view { reinterpret_cast<const char*>(units), sizeof(units) }, sink);
        check_eq(out, "\"a\\uD83D\\uDE00\"", "json: write_json_string utf16");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// write_pre_quoted_string — the const-generic facade, ref lib.rs:952
// ─────────────────────────────────────────────────────────────────────────────
void test_facade() {
    std::string out;
    StringSink sink { out };
    write_pre_quoted_string<'"', false, false, Encoding::Utf8>("a\"b", sink);
    check_eq(out, "a\\\"b", "facade: forwards to inner");

    std::string out2;
    StringSink sink2 { out2 };
    write_pre_quoted_string<'"', true, false, Encoding::Utf8>("é", sink2);
    check_eq(out2, "\\xE9", "facade: ASCII_ONLY=true reaches the loop");
}

// ─────────────────────────────────────────────────────────────────────────────
// The escape scan — SWAR vs the scalar oracle.
//
// The SWAR block scan must be a SUPERSET detector: false positives cost a
// re-scan, false negatives silently emit unescaped bytes. This fuzzes the two
// implementations against each other over inputs built from exactly the alphabet
// that stresses the predicate boundaries.
// ─────────────────────────────────────────────────────────────────────────────
void test_escape_scan() {
    // Every byte value, at every offset within a block and across the boundary.
    for (int q : { '"', '\'', '`' }) {
        const auto quote { static_cast<std::uint8_t>(q) };
        for (int b = 0; b < 256; ++b) {
            for (std::size_t pos = 0; pos < 20; ++pos) {
                std::string s(20, 'a');
                s[pos] = static_cast<char>(b);
                const std::size_t swar { index_of_needs_escape_for_js_string(s, quote) };
                const std::size_t ref { index_of_needs_escape_scalar(s, quote) };
                if (swar != ref) {
                    gChecks += 1;
                    gFailures += 1;
                    std::println("FAIL scan: quote='{}' byte=0x{:02X} pos={} swar={} scalar={}",
                        static_cast<char>(quote), b, pos, swar, ref);
                    return;
                }
            }
        }
    }
    gChecks += 1;

    // Pseudo-random strings over a boundary-heavy alphabet, all lengths 0..80.
    constexpr char ALPHABET[] { 'a', 'z', ' ', '\x00', '\x1F', '\x20', '\x7E', '\x7F',
        '\x80', '\xFF', '\\', '"', '\'', '`', '$', '{', '\n', '\t' };
    std::mt19937 rng { 0xC0FFEE };
    std::uniform_int_distribution<int> pick { 0, static_cast<int>(std::size(ALPHABET)) - 1 };
    for (int q : { '"', '\'', '`' }) {
        const auto quote { static_cast<std::uint8_t>(q) };
        for (std::size_t len = 0; len <= 80; ++len) {
            for (int iter = 0; iter < 40; ++iter) {
                std::string s;
                s.reserve(len);
                for (std::size_t i = 0; i < len; ++i) {
                    s.push_back(ALPHABET[pick(rng)]);
                }
                const std::size_t swar { index_of_needs_escape_for_js_string(s, quote) };
                const std::size_t ref { index_of_needs_escape_scalar(s, quote) };
                if (swar != ref) {
                    gChecks += 1;
                    gFailures += 1;
                    std::println("FAIL scan fuzz: quote='{}' len={} swar={} scalar={} s=\"{}\"",
                        static_cast<char>(quote), len, swar, ref, printable(s));
                    return;
                }
            }
        }
    }
    gChecks += 1;

    // The documented asymmetry: `$` stops the scan only for backticks.
    check_true(index_of_needs_escape_for_js_string("a$b", '"') == 3, "scan: $ ignored for \"");
    check_true(index_of_needs_escape_for_js_string("a$b", '`') == 1, "scan: $ found for `");
    // A non-active quote does not stop the scan.
    check_true(index_of_needs_escape_for_js_string("a'b", '"') == 3, "scan: ' ignored for \"");
    check_true(index_of_needs_escape_for_js_string("a'b", '\'') == 1, "scan: ' found for '");
    // Sentinel is the length, not npos.
    check_true(index_of_needs_escape_for_js_string("", '"') == 0, "scan: empty → 0");
    check_true(index_of_needs_escape_for_js_string("abc", '"') == 3, "scan: none → size()");
    // 0x7E printable, 0x7F not (the >= 127 boundary).
    check_true(index_of_needs_escape_for_js_string("\x7E", '"') == 1, "scan: 0x7E clean");
    check_true(index_of_needs_escape_for_js_string("\x7F", '"') == 0, "scan: 0x7F needs escape");
}

// ─────────────────────────────────────────────────────────────────────────────
// estimate_length_for_utf8 — ref lib.rs:909
// ─────────────────────────────────────────────────────────────────────────────
void test_estimate_length() {
    // +2 for the quotes.
    check_true(estimate_length_for_utf8("", false, '"') == 2, "est: empty → 2");
    check_true(estimate_length_for_utf8("abc", false, '"') == 5, "est: abc → 5");
    // é printable when !ascii_only → costs its 2 UTF-8 bytes.
    check_true(estimate_length_for_utf8("é", false, '"') == 4, "est: é !ascii_only → 4");
    // é not printable when ascii_only → costs 6 (the branch is `<= 0xFFFF → 6`,
    // which over-estimates the 4 bytes `\xE9` actually takes; it is an estimate,
    // and bun's own comment at :1175 explains it prefers to over-reserve).
    check_true(estimate_length_for_utf8("é", true, '"') == 8, "est: é ascii_only → 8");
    // Astral → 12.
    check_true(estimate_length_for_utf8("\U0001F600", true, '"') == 14, "est: emoji ascii_only → 14");
    // Truncated final sequence must not read past the end (Deviation 2). The
    // assertion here is just "returns and does not crash"; the value is whatever
    // the clamped decode yields.
    {
        const std::string truncated { "\xF0\x9F" };
        const std::size_t n { estimate_length_for_utf8(truncated, true, '"') };
        check_true(n > 0, "est: truncated utf8 does not read OOB");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// encoding primitives — ref bun_core/string/mod.rs:2419/:2427, fmt.rs:2920
// ─────────────────────────────────────────────────────────────────────────────
void test_encoding_primitives() {
    static_assert(hex2_upper(0x00) == std::array<char, 2> { '0', '0' });
    static_assert(hex2_upper(0xE9) == std::array<char, 2> { 'E', '9' });
    static_assert(hex2_upper(0xFF) == std::array<char, 2> { 'F', 'F' });
    static_assert(bmp_escape(0x2028) == std::array<char, 6> { '\\', 'u', '2', '0', '2', '8' });
    static_assert(bmp_escape(0xFEFF) == std::array<char, 6> { '\\', 'u', 'F', 'E', 'F', 'F' });
    // U+1F600 → lead D83D, trail DE00. Matches what real bun prints for '😀'.
    static_assert(surrogate_pair_escape(0x1F600)
        == std::array<char, 12> { '\\', 'u', 'D', '8', '3', 'D', '\\', 'u', 'D', 'E', '0', '0' });
    gChecks += 1;  // the static_asserts above are the check

    // The whitespacer: minify strips spaces, at compile time.
    static_assert(ws<" = ">.normal == " = ");
    static_assert(ws<" = ">.minify == "=");
    static_assert(ws<"} from ">.normal == "} from ");
    static_assert(ws<"} from ">.minify == "}from");
    gChecks += 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// Printer skeleton — instantiate it so the CRTP chain and the flag NTTP are
// actually type-checked, not just parsed.
// ─────────────────────────────────────────────────────────────────────────────
void test_printer_skeleton() {
    using mbun::js_printer::make_printer;
    constexpr PrinterFlags F {};

    {
        std::string out;
        auto p { make_printer<F>(out) };
        p.print("a");
        p.print('=');
        p.print_space();
        p.print("1");
        p.print_semicolon_after_statement();
        check_eq(out, "a= 1;\n", "printer: basic output");
    }
    {
        // minify_whitespace defers the semicolon instead of emitting it.
        std::string out;
        Options opts {};
        opts.minifyWhitespace = true;
        auto p { make_printer<F>(out, opts) };
        p.print("a");
        p.print_space();
        p.print_semicolon_after_statement();
        check_true(p.needsSemicolon, "printer: minify defers semicolon");
        p.print_semicolon_if_needed();
        check_eq(out, "a;", "printer: minify drops the space, emits deferred ;");
    }
    {
        std::string out;
        auto p { make_printer<F>(out) };
        p.indent();
        p.indent();
        p.print_indent();
        p.print("x");
        p.unindent();
        p.print_newline();
        p.print_indent();
        p.print("y");
        check_eq(out, "    x\n  y", "printer: indentation");
    }
    {
        // unindent at depth 0 saturates rather than wrapping (:1919).
        std::string out;
        auto p { make_printer<F>(out) };
        p.unindent();
        p.unindent();
        p.print_indent();
        p.print("x");
        check_eq(out, "x", "printer: unindent saturates at 0");
    }
    {
        // Tab indentation.
        std::string out;
        Options opts {};
        opts.indent.character = mbun::js_printer::IndentationCharacter::Tab;
        opts.indent.scalar = 1;
        auto p { make_printer<F>(out, opts) };
        p.indent();
        p.print_indent();
        p.print("x");
        check_eq(out, "\tx", "printer: tab indent");
    }
    {
        // An indent deeper than the 128-byte buffer must loop, not truncate.
        std::string out;
        auto p { make_printer<F>(out) };
        for (int i = 0; i < 100; ++i) {
            p.indent();
        }
        p.print_indent();
        check_eq(out, std::string(200, ' '), "printer: indent > 128 bytes loops");
    }
    {
        std::string out;
        auto p { make_printer<F>(out) };
        p.print_whitespacer(ws<" = ">);
        check_eq(out, " = ", "printer: whitespacer normal");
    }
    {
        std::string out;
        Options opts {};
        opts.minifyWhitespace = true;
        auto p { make_printer<F>(out, opts) };
        p.print_whitespacer(ws<" = ">);
        p.print_equals();
        check_eq(out, "==", "printer: whitespacer minify + print_equals");
    }
    {
        // The flag NTTP must reach the escape layer.
        std::string out;
        auto p { make_printer<F>(out) };
        p.print_string_characters_utf8("héllo", '"');
        check_eq(out, "héllo", "printer: !asciiOnly passthrough");
    }
    {
        constexpr PrinterFlags ASCII { .asciiOnly = true };
        std::string out;
        auto p { make_printer<ASCII>(out) };
        p.print_string_characters_utf8("héllo", '"');
        check_eq(out, "h\\xE9llo", "printer: asciiOnly escapes");
    }
    {
        constexpr PrinterFlags ASCII { .asciiOnly = true };
        std::string out;
        auto p { make_printer<ASCII>(out) };
        p.print_string_characters_utf16(std::u16string_view { u"\U0001F600" }, '"');
        check_eq(out, "\\uD83D\\uDE00", "printer: utf16 overload");
    }
}

}  // namespace

int main() {
    test_can_print_without_escape();
    test_best_quote_char();
    test_escape_utf8();
    test_escape_utf16();
    test_escape_latin1_ascii();
    test_json();
    test_facade();
    test_escape_scan();
    test_estimate_length();
    test_encoding_primitives();
    test_printer_skeleton();

    std::println("test_js_printer_quote: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
