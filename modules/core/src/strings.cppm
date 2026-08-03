// strings.cppm — mbun.core.strings: UTF-8/16/WTF-8, Latin-1, codepoint iteration,
// ANSI stripping and terminal string width. Foundation string primitives for the
// js/css/http subsystems.
//
// Algorithms and Unicode data are ported from bun's real implementation
// (MIT-licensed) and re-expressed in MC++/C++26 (string_view zero-copy, no ICU
// dependency). References:
//   - ref: bun src/bun_core/string/immutable.rs (decode_wtf8_one,
//          to_utf16_alloc_for_real, to_utf8_alloc, first_non_ascii)
//   - ref: bun src/jsc/bindings/ANSIHelpers.h (consumeANSI / findEscapeCharacter)
//   - ref: bun src/jsc/bindings/stringWidth.cpp (visibleUTF16Width, GraphemeState,
//          fusedClassify, computeGraphemeBreakNoControl + kGraphemeBreakDecisions)
//          + stringWidthTables.h (the width / emoji / grapheme-break-class data).
//
// The width / emoji / grapheme-break-class data is stored as sorted range tables
// (Hangul LV/LVT computed algorithmically); at COMPILE TIME those ranges are
// expanded by `consteval` into a flat direct-index fused-classification table and
// a precomputed grapheme-break decision table, so the hot path classifies each
// codepoint and resolves each break in O(1) — no per-codepoint binary search, no
// ICU, no 40 KB packed blob. See the fused-classification section below.
//
// Behaviour is pinned by bun's original test suite; see
// tests/test_core_strings.cpp for the extracted vectors and their sources.
export module mbun.core.strings;

import std;

namespace mbun::core::strings {

// ---------------------------------------------------------------------------
// Unicode data tables (ported from bun stringWidthTables.h; see file header).
// ---------------------------------------------------------------------------
namespace {

struct Range {
    char32_t lo;
    char32_t hi;
};
struct GRange {
    char32_t lo;
    char32_t hi;
    std::uint8_t cls;
};

// Grapheme break classes. Ordinals MUST match bun's GraphemeBreakClass enum so
// the ported break algorithm is bit-identical.
enum GBC : std::uint8_t {
    Other = 0, Prepend = 1, RegionalIndicator = 2, SpacingMark = 3,
    L = 4, V = 5, T = 6, Lv = 7, Lvt = 8, Zwj = 9, Zwnj = 10,
    ExtendedPictographic = 11, EmojiModifierBase = 12, EmojiModifier = 13,
    IndicConjunctBreakExtend = 14, IndicConjunctBreakLinker = 15,
    IndicConjunctBreakConsonant = 16,
};

// width class 0 (zero-width): controls, combining marks, format chars, surrogates
constexpr Range kZeroWidth[]{
    {0x0,0x1F},{0x7F,0x9F},{0xAD,0xAD},{0x300,0x36F},{0x600,0x605},{0x61C,0x61C},{0x6DD,0x6DD},
    {0x70F,0x70F},{0x8E2,0x8E2},{0x900,0x902},{0x93A,0x93C},{0x93E,0x94D},{0x951,0x957},
    {0x962,0x963},{0x980,0x982},{0x9BA,0x9BC},{0x9BE,0x9CD},{0x9D1,0x9D7},{0x9E2,0x9E3},
    {0xA00,0xA02},{0xA3A,0xA3C},{0xA3E,0xA4D},{0xA51,0xA57},{0xA62,0xA63},{0xA80,0xA82},
    {0xABA,0xABC},{0xABE,0xACD},{0xAD1,0xAD7},{0xAE2,0xAE3},{0xB00,0xB02},{0xB3A,0xB3C},
    {0xB3E,0xB4D},{0xB51,0xB57},{0xB62,0xB63},{0xB80,0xB82},{0xBBA,0xBBC},{0xBBE,0xBCD},
    {0xBD1,0xBD7},{0xBE2,0xBE3},{0xC00,0xC02},{0xC3A,0xC3C},{0xC3E,0xC4D},{0xC51,0xC57},
    {0xC62,0xC63},{0xC80,0xC82},{0xCBA,0xCBC},{0xCBE,0xCCD},{0xCD1,0xCD7},{0xCE2,0xCE3},
    {0xD00,0xD02},{0xD3A,0xD3C},{0xD3E,0xD4D},{0xE31,0xE31},{0xE34,0xE3A},{0xE47,0xE4E},
    {0xEB1,0xEB1},{0xEB4,0xEBC},{0xEC8,0xECD},{0x180B,0x180F},{0x1AB0,0x1AFF},{0x1DC0,0x1DFF},
    {0x200B,0x200F},{0x202A,0x202E},{0x2060,0x206F},{0x20D0,0x20FF},{0xD800,0xDFFF},{0xFE00,0xFE0F},
    {0xFE20,0xFE2F},{0xFEFF,0xFEFF},{0xE0000,0xE007F},{0xE0100,0xE01EF},
};
// East Asian Wide + Fullwidth (width 2)
constexpr Range kWide[]{
    {0x1100,0x115F},{0x231A,0x231B},{0x2329,0x232A},{0x23E9,0x23EC},{0x23F0,0x23F0},{0x23F3,0x23F3},
    {0x25FD,0x25FE},{0x2614,0x2615},{0x2648,0x2653},{0x267F,0x267F},{0x2693,0x2693},{0x26A1,0x26A1},
    {0x26AA,0x26AB},{0x26BD,0x26BE},{0x26C4,0x26C5},{0x26CE,0x26CE},{0x26D4,0x26D4},{0x26EA,0x26EA},
    {0x26F2,0x26F3},{0x26F5,0x26F5},{0x26FA,0x26FA},{0x26FD,0x26FD},{0x2705,0x2705},{0x270A,0x270B},
    {0x2728,0x2728},{0x274C,0x274C},{0x274E,0x274E},{0x2753,0x2755},{0x2757,0x2757},{0x2795,0x2797},
    {0x27B0,0x27B0},{0x27BF,0x27BF},{0x2B1B,0x2B1C},{0x2B50,0x2B50},{0x2B55,0x2B55},{0x2E80,0x2E99},
    {0x2E9B,0x2EF3},{0x2F00,0x2FD5},{0x2FF0,0x303E},{0x3041,0x3096},{0x3099,0x30FF},{0x3105,0x312F},
    {0x3131,0x318E},{0x3190,0x31E3},{0x31EF,0x321E},{0x3220,0x3247},{0x3250,0x4DBF},{0x4E00,0xA48C},
    {0xA490,0xA4C6},{0xA960,0xA97C},{0xAC00,0xD7A3},{0xF900,0xFAFF},{0xFE10,0xFE19},{0xFE30,0xFE52},
    {0xFE54,0xFE66},{0xFE68,0xFE6B},{0xFF01,0xFF60},{0xFFE0,0xFFE6},{0x16FE0,0x16FE4},
    {0x16FF0,0x16FF1},{0x17000,0x187F7},{0x18800,0x18CD5},{0x18D00,0x18D08},{0x1AFF0,0x1AFF3},
    {0x1AFF5,0x1AFFB},{0x1AFFD,0x1AFFE},{0x1B000,0x1B122},{0x1B132,0x1B132},{0x1B150,0x1B152},
    {0x1B155,0x1B155},{0x1B164,0x1B167},{0x1B170,0x1B2FB},{0x1F004,0x1F004},{0x1F0CF,0x1F0CF},
    {0x1F18E,0x1F18E},{0x1F191,0x1F19A},{0x1F200,0x1F202},{0x1F210,0x1F23B},{0x1F240,0x1F248},
    {0x1F250,0x1F251},{0x1F260,0x1F265},{0x1F300,0x1F320},{0x1F32D,0x1F335},{0x1F337,0x1F37C},
    {0x1F37E,0x1F393},{0x1F3A0,0x1F3CA},{0x1F3CF,0x1F3D3},{0x1F3E0,0x1F3F0},{0x1F3F4,0x1F3F4},
    {0x1F3F8,0x1F43E},{0x1F440,0x1F440},{0x1F442,0x1F4FC},{0x1F4FF,0x1F53D},{0x1F54B,0x1F54E},
    {0x1F550,0x1F567},{0x1F57A,0x1F57A},{0x1F595,0x1F596},{0x1F5A4,0x1F5A4},{0x1F5FB,0x1F64F},
    {0x1F680,0x1F6C5},{0x1F6CC,0x1F6CC},{0x1F6D0,0x1F6D2},{0x1F6D5,0x1F6D7},{0x1F6DC,0x1F6DF},
    {0x1F6EB,0x1F6EC},{0x1F6F4,0x1F6FC},{0x1F7E0,0x1F7EB},{0x1F7F0,0x1F7F0},{0x1F90C,0x1F93A},
    {0x1F93C,0x1F945},{0x1F947,0x1F9FF},{0x1FA70,0x1FA7C},{0x1FA80,0x1FA88},{0x1FA90,0x1FABD},
    {0x1FABF,0x1FAC5},{0x1FACE,0x1FADB},{0x1FAE0,0x1FAE8},{0x1FAF0,0x1FAF8},{0x20000,0x2FFFD},
    {0x30000,0x3FFFD},
};
// East Asian Ambiguous (width 1 by default, 2 when ambiguousIsNarrow=false)
constexpr Range kAmbiguous[]{
    {0xA1,0xA1},{0xA4,0xA4},{0xA7,0xA8},{0xAA,0xAA},{0xAE,0xAE},{0xB0,0xB4},{0xB6,0xBA},{0xBC,0xBF},
    {0xC6,0xC6},{0xD0,0xD0},{0xD7,0xD8},{0xDE,0xE1},{0xE6,0xE6},{0xE8,0xEA},{0xEC,0xED},{0xF0,0xF0},
    {0xF2,0xF3},{0xF7,0xFA},{0xFC,0xFC},{0xFE,0xFE},{0x101,0x101},{0x111,0x111},{0x113,0x113},
    {0x11B,0x11B},{0x126,0x127},{0x12B,0x12B},{0x131,0x133},{0x138,0x138},{0x13F,0x142},
    {0x144,0x144},{0x148,0x14B},{0x14D,0x14D},{0x152,0x153},{0x166,0x167},{0x16B,0x16B},
    {0x1CE,0x1CE},{0x1D0,0x1D0},{0x1D2,0x1D2},{0x1D4,0x1D4},{0x1D6,0x1D6},{0x1D8,0x1D8},
    {0x1DA,0x1DA},{0x1DC,0x1DC},{0x251,0x251},{0x261,0x261},{0x2C4,0x2C4},{0x2C7,0x2C7},
    {0x2C9,0x2CB},{0x2CD,0x2CD},{0x2D0,0x2D0},{0x2D8,0x2DB},{0x2DD,0x2DD},{0x2DF,0x2DF},
    {0x391,0x3A1},{0x3A3,0x3A9},{0x3B1,0x3C1},{0x3C3,0x3C9},{0x401,0x401},{0x410,0x44F},
    {0x451,0x451},{0x2010,0x2010},{0x2013,0x2016},{0x2018,0x2019},{0x201C,0x201D},{0x2020,0x2022},
    {0x2024,0x2027},{0x2030,0x2030},{0x2032,0x2033},{0x2035,0x2035},{0x203B,0x203B},{0x203E,0x203E},
    {0x2074,0x2074},{0x207F,0x207F},{0x2081,0x2084},{0x20AC,0x20AC},{0x2103,0x2103},{0x2105,0x2105},
    {0x2109,0x2109},{0x2113,0x2113},{0x2116,0x2116},{0x2121,0x2122},{0x2126,0x2126},{0x212B,0x212B},
    {0x2153,0x2154},{0x215B,0x215E},{0x2160,0x216B},{0x2170,0x2179},{0x2189,0x2189},{0x2190,0x2199},
    {0x21B8,0x21B9},{0x21D2,0x21D2},{0x21D4,0x21D4},{0x21E7,0x21E7},{0x2200,0x2200},{0x2202,0x2203},
    {0x2207,0x2208},{0x220B,0x220B},{0x220F,0x220F},{0x2211,0x2211},{0x2215,0x2215},{0x221A,0x221A},
    {0x221D,0x2220},{0x2223,0x2223},{0x2225,0x2225},{0x2227,0x222C},{0x222E,0x222E},{0x2234,0x2237},
    {0x223C,0x223D},{0x2248,0x2248},{0x224C,0x224C},{0x2252,0x2252},{0x2260,0x2261},{0x2264,0x2267},
    {0x226A,0x226B},{0x226E,0x226F},{0x2282,0x2283},{0x2286,0x2287},{0x2295,0x2295},{0x2299,0x2299},
    {0x22A5,0x22A5},{0x22BF,0x22BF},{0x2312,0x2312},{0x2460,0x24E9},{0x24EB,0x254B},{0x2550,0x2573},
    {0x2580,0x258F},{0x2592,0x2595},{0x25A0,0x25A1},{0x25A3,0x25A9},{0x25B2,0x25B3},{0x25B6,0x25B7},
    {0x25BC,0x25BD},{0x25C0,0x25C1},{0x25C6,0x25C8},{0x25CB,0x25CB},{0x25CE,0x25D1},{0x25E2,0x25E5},
    {0x25EF,0x25EF},{0x2605,0x2606},{0x2609,0x2609},{0x260E,0x260F},{0x261C,0x261C},{0x261E,0x261E},
    {0x2640,0x2640},{0x2642,0x2642},{0x2660,0x2661},{0x2663,0x2665},{0x2667,0x266A},{0x266C,0x266D},
    {0x266F,0x266F},{0x269E,0x269F},{0x26BF,0x26BF},{0x26C6,0x26CD},{0x26CF,0x26D3},{0x26D5,0x26E1},
    {0x26E3,0x26E3},{0x26E8,0x26E9},{0x26EB,0x26F1},{0x26F4,0x26F4},{0x26F6,0x26F9},{0x26FB,0x26FC},
    {0x26FE,0x26FF},{0x273D,0x273D},{0x2776,0x277F},{0x2B56,0x2B59},{0x3248,0x324F},{0xE000,0xF8FF},
    {0xFFFD,0xFFFD},{0x1F100,0x1F10A},{0x1F110,0x1F12D},{0x1F130,0x1F169},{0x1F170,0x1F18D},
    {0x1F18F,0x1F190},{0x1F19B,0x1F1AC},{0xF0000,0xFFFFD},{0x100000,0x10FFFD},
};
// Unicode Emoji property (with bun's isEmojiPresentation early-outs baked in)
constexpr Range kEmoji[]{
    {0x203C,0x203C},{0x2049,0x2049},{0x2122,0x2122},{0x2139,0x2139},{0x2194,0x2199},{0x21A9,0x21AA},
    {0x231A,0x231B},{0x2328,0x2328},{0x23CF,0x23CF},{0x23E9,0x23F3},{0x23F8,0x23FA},{0x24C2,0x24C2},
    {0x25AA,0x25AB},{0x25B6,0x25B6},{0x25C0,0x25C0},{0x25FB,0x25FE},{0x2600,0x2604},{0x260E,0x260E},
    {0x2611,0x2611},{0x2614,0x2615},{0x2618,0x2618},{0x261D,0x261D},{0x2620,0x2620},{0x2622,0x2623},
    {0x2626,0x2626},{0x262A,0x262A},{0x262E,0x262F},{0x2638,0x263A},{0x2640,0x2640},{0x2642,0x2642},
    {0x2648,0x2653},{0x265F,0x2660},{0x2663,0x2663},{0x2665,0x2666},{0x2668,0x2668},{0x267B,0x267B},
    {0x267E,0x267F},{0x2692,0x2697},{0x2699,0x2699},{0x269B,0x269C},{0x26A0,0x26A1},{0x26A7,0x26A7},
    {0x26AA,0x26AB},{0x26B0,0x26B1},{0x26BD,0x26BE},{0x26C4,0x26C5},{0x26C8,0x26C8},{0x26CE,0x26CF},
    {0x26D1,0x26D1},{0x26D3,0x26D4},{0x26E9,0x26EA},{0x26F0,0x26F5},{0x26F7,0x26FA},{0x26FD,0x26FD},
    {0x2702,0x2702},{0x2705,0x2705},{0x2708,0x270D},{0x270F,0x270F},{0x2712,0x2712},{0x2714,0x2714},
    {0x2716,0x2716},{0x271D,0x271D},{0x2721,0x2721},{0x2728,0x2728},{0x2733,0x2734},{0x2744,0x2744},
    {0x2747,0x2747},{0x274C,0x274C},{0x274E,0x274E},{0x2753,0x2755},{0x2757,0x2757},{0x2763,0x2764},
    {0x2795,0x2797},{0x27A1,0x27A1},{0x27B0,0x27B0},{0x27BF,0x27BF},{0x2934,0x2935},{0x2B05,0x2B07},
    {0x2B1B,0x2B1C},{0x2B50,0x2B50},{0x2B55,0x2B55},{0x1F004,0x1F004},{0x1F0CF,0x1F0CF},
    {0x1F170,0x1F171},{0x1F17E,0x1F17F},{0x1F18E,0x1F18E},{0x1F191,0x1F19A},{0x1F1E6,0x1F1FF},
    {0x1F201,0x1F202},{0x1F21A,0x1F21A},{0x1F22F,0x1F22F},{0x1F232,0x1F23A},{0x1F250,0x1F251},
    {0x1F300,0x1F321},{0x1F324,0x1F393},{0x1F396,0x1F397},{0x1F399,0x1F39B},{0x1F39E,0x1F3F0},
    {0x1F3F3,0x1F3F5},{0x1F3F7,0x1F4FD},{0x1F4FF,0x1F53D},{0x1F549,0x1F54E},{0x1F550,0x1F567},
    {0x1F56F,0x1F570},{0x1F573,0x1F57A},{0x1F587,0x1F587},{0x1F58A,0x1F58D},{0x1F590,0x1F590},
    {0x1F595,0x1F596},{0x1F5A4,0x1F5A5},{0x1F5A8,0x1F5A8},{0x1F5B1,0x1F5B2},{0x1F5BC,0x1F5BC},
    {0x1F5C2,0x1F5C4},{0x1F5D1,0x1F5D3},{0x1F5DC,0x1F5DE},{0x1F5E1,0x1F5E1},{0x1F5E3,0x1F5E3},
    {0x1F5E8,0x1F5E8},{0x1F5EF,0x1F5EF},{0x1F5F3,0x1F5F3},{0x1F5FA,0x1F64F},{0x1F680,0x1F6C5},
    {0x1F6CB,0x1F6D2},{0x1F6D5,0x1F6D8},{0x1F6DC,0x1F6E5},{0x1F6E9,0x1F6E9},{0x1F6EB,0x1F6EC},
    {0x1F6F0,0x1F6F0},{0x1F6F3,0x1F6FC},{0x1F7E0,0x1F7EB},{0x1F7F0,0x1F7F0},{0x1F90C,0x1F93A},
    {0x1F93C,0x1F945},{0x1F947,0x1F9FF},{0x1FA70,0x1FA7C},{0x1FA80,0x1FA8A},{0x1FA8E,0x1FAC6},
    {0x1FAC8,0x1FAC8},{0x1FACD,0x1FADC},{0x1FADF,0x1FAEA},{0x1FAEF,0x1FAF8},
};
// Grapheme break class per codepoint (non-Other, non-Hangul-syllable). Hangul
// LV/LVT syllables are derived algorithmically in graphemeClass().
constexpr GRange kGrapheme[]{
    {0xA9,0xA9,11},{0xAE,0xAE,11},{0x300,0x36F,14},{0x483,0x489,14},{0x591,0x5BD,14},
    {0x5BF,0x5BF,14},{0x5C1,0x5C2,14},{0x5C4,0x5C5,14},{0x5C7,0x5C7,14},{0x600,0x605,1},
    {0x610,0x61A,14},{0x64B,0x65F,14},{0x670,0x670,14},{0x6D6,0x6DC,14},{0x6DD,0x6DD,1},
    {0x6DF,0x6E4,14},{0x6E7,0x6E8,14},{0x6EA,0x6ED,14},{0x70F,0x70F,1},{0x711,0x711,14},
    {0x730,0x74A,14},{0x7A6,0x7B0,14},{0x7EB,0x7F3,14},{0x7FD,0x7FD,14},{0x816,0x819,14},
    {0x81B,0x823,14},{0x825,0x827,14},{0x829,0x82D,14},{0x859,0x85B,14},{0x890,0x891,1},
    {0x897,0x89F,14},{0x8CA,0x8E1,14},{0x8E2,0x8E2,1},{0x8E3,0x902,14},{0x903,0x903,3},
    {0x915,0x939,16},{0x93A,0x93A,14},{0x93B,0x93B,3},{0x93C,0x93C,14},{0x93E,0x940,3},
    {0x941,0x948,14},{0x949,0x94C,3},{0x94D,0x94D,15},{0x94E,0x94F,3},{0x951,0x957,14},
    {0x958,0x95F,16},{0x962,0x963,14},{0x978,0x97F,16},{0x981,0x981,14},{0x982,0x983,3},
    {0x995,0x9A8,16},{0x9AA,0x9B0,16},{0x9B2,0x9B2,16},{0x9B6,0x9B9,16},{0x9BC,0x9BC,14},
    {0x9BE,0x9BE,14},{0x9BF,0x9C0,3},{0x9C1,0x9C4,14},{0x9C7,0x9C8,3},{0x9CB,0x9CC,3},
    {0x9CD,0x9CD,15},{0x9D7,0x9D7,14},{0x9DC,0x9DD,16},{0x9DF,0x9DF,16},{0x9E2,0x9E3,14},
    {0x9F0,0x9F1,16},{0x9FE,0x9FE,14},{0xA01,0xA02,14},{0xA03,0xA03,3},{0xA3C,0xA3C,14},
    {0xA3E,0xA40,3},{0xA41,0xA42,14},{0xA47,0xA48,14},{0xA4B,0xA4D,14},{0xA51,0xA51,14},
    {0xA70,0xA71,14},{0xA75,0xA75,14},{0xA81,0xA82,14},{0xA83,0xA83,3},{0xA95,0xAA8,16},
    {0xAAA,0xAB0,16},{0xAB2,0xAB3,16},{0xAB5,0xAB9,16},{0xABC,0xABC,14},{0xABE,0xAC0,3},
    {0xAC1,0xAC5,14},{0xAC7,0xAC8,14},{0xAC9,0xAC9,3},{0xACB,0xACC,3},{0xACD,0xACD,15},
    {0xAE2,0xAE3,14},{0xAF9,0xAF9,16},{0xAFA,0xAFF,14},{0xB01,0xB01,14},{0xB02,0xB03,3},
    {0xB15,0xB28,16},{0xB2A,0xB30,16},{0xB32,0xB33,16},{0xB35,0xB39,16},{0xB3C,0xB3C,14},
    {0xB3E,0xB3F,14},{0xB40,0xB40,3},{0xB41,0xB44,14},{0xB47,0xB48,3},{0xB4B,0xB4C,3},
    {0xB4D,0xB4D,15},{0xB55,0xB57,14},{0xB5C,0xB5D,16},{0xB5F,0xB5F,16},{0xB62,0xB63,14},
    {0xB71,0xB71,16},{0xB82,0xB82,14},{0xBBE,0xBBE,14},{0xBBF,0xBBF,3},{0xBC0,0xBC0,14},
    {0xBC1,0xBC2,3},{0xBC6,0xBC8,3},{0xBCA,0xBCC,3},{0xBCD,0xBCD,14},{0xBD7,0xBD7,14},
    {0xC00,0xC00,14},{0xC01,0xC03,3},{0xC04,0xC04,14},{0xC15,0xC28,16},{0xC2A,0xC39,16},
    {0xC3C,0xC3C,14},{0xC3E,0xC40,14},{0xC41,0xC44,3},{0xC46,0xC48,14},{0xC4A,0xC4C,14},
    {0xC4D,0xC4D,15},{0xC55,0xC56,14},{0xC58,0xC5A,16},{0xC62,0xC63,14},{0xC81,0xC81,14},
    {0xC82,0xC83,3},{0xCBC,0xCBC,14},{0xCBE,0xCBE,3},{0xCBF,0xCC0,14},{0xCC1,0xCC1,3},
    {0xCC2,0xCC2,14},{0xCC3,0xCC4,3},{0xCC6,0xCC8,14},{0xCCA,0xCCD,14},{0xCD5,0xCD6,14},
    {0xCE2,0xCE3,14},{0xCF3,0xCF3,3},{0xD00,0xD01,14},{0xD02,0xD03,3},{0xD15,0xD3A,16},
    {0xD3B,0xD3C,14},{0xD3E,0xD3E,14},{0xD3F,0xD40,3},{0xD41,0xD44,14},{0xD46,0xD48,3},
    {0xD4A,0xD4C,3},{0xD4D,0xD4D,15},{0xD4E,0xD4E,1},{0xD57,0xD57,14},{0xD62,0xD63,14},
    {0xD81,0xD81,14},{0xD82,0xD83,3},{0xDCA,0xDCA,14},{0xDCF,0xDCF,14},{0xDD0,0xDD1,3},
    {0xDD2,0xDD4,14},{0xDD6,0xDD6,14},{0xDD8,0xDDE,3},{0xDDF,0xDDF,14},{0xDF2,0xDF3,3},
    {0xE31,0xE31,14},{0xE33,0xE33,3},{0xE34,0xE3A,14},{0xE47,0xE4E,14},{0xEB1,0xEB1,14},
    {0xEB3,0xEB3,3},{0xEB4,0xEBC,14},{0xEC8,0xECE,14},{0xF18,0xF19,14},{0xF35,0xF35,14},
    {0xF37,0xF37,14},{0xF39,0xF39,14},{0xF3E,0xF3F,3},{0xF71,0xF7E,14},{0xF7F,0xF7F,3},
    {0xF80,0xF84,14},{0xF86,0xF87,14},{0xF8D,0xF97,14},{0xF99,0xFBC,14},{0xFC6,0xFC6,14},
    {0x102D,0x1030,14},{0x1031,0x1031,3},{0x1032,0x1037,14},{0x1039,0x103A,14},{0x103B,0x103C,3},
    {0x103D,0x103E,14},{0x1056,0x1057,3},{0x1058,0x1059,14},{0x105E,0x1060,14},{0x1071,0x1074,14},
    {0x1082,0x1082,14},{0x1084,0x1084,3},{0x1085,0x1086,14},{0x108D,0x108D,14},{0x109D,0x109D,14},
    {0x1100,0x115F,4},{0x1160,0x11A7,5},{0x11A8,0x11FF,6},{0x135D,0x135F,14},{0x1712,0x1715,14},
    {0x1732,0x1734,14},{0x1752,0x1753,14},{0x1772,0x1773,14},{0x17B4,0x17B5,14},{0x17B6,0x17B6,3},
    {0x17B7,0x17BD,14},{0x17BE,0x17C5,3},{0x17C6,0x17C6,14},{0x17C7,0x17C8,3},{0x17C9,0x17D3,14},
    {0x17DD,0x17DD,14},{0x180B,0x180D,14},{0x180F,0x180F,14},{0x1885,0x1886,14},{0x18A9,0x18A9,14},
    {0x1920,0x1922,14},{0x1923,0x1926,3},{0x1927,0x1928,14},{0x1929,0x192B,3},{0x1930,0x1931,3},
    {0x1932,0x1932,14},{0x1933,0x1938,3},{0x1939,0x193B,14},{0x1A17,0x1A18,14},{0x1A19,0x1A1A,3},
    {0x1A1B,0x1A1B,14},{0x1A55,0x1A55,3},{0x1A56,0x1A56,14},{0x1A57,0x1A57,3},{0x1A58,0x1A5E,14},
    {0x1A60,0x1A60,14},{0x1A62,0x1A62,14},{0x1A65,0x1A6C,14},{0x1A6D,0x1A72,3},{0x1A73,0x1A7C,14},
    {0x1A7F,0x1A7F,14},{0x1AB0,0x1ACE,14},{0x1B00,0x1B03,14},{0x1B04,0x1B04,3},{0x1B34,0x1B3D,14},
    {0x1B3E,0x1B41,3},{0x1B42,0x1B44,14},{0x1B6B,0x1B73,14},{0x1B80,0x1B81,14},{0x1B82,0x1B82,3},
    {0x1BA1,0x1BA1,3},{0x1BA2,0x1BA5,14},{0x1BA6,0x1BA7,3},{0x1BA8,0x1BAD,14},{0x1BE6,0x1BE6,14},
    {0x1BE7,0x1BE7,3},{0x1BE8,0x1BE9,14},{0x1BEA,0x1BEC,3},{0x1BED,0x1BED,14},{0x1BEE,0x1BEE,3},
    {0x1BEF,0x1BF3,14},{0x1C24,0x1C2B,3},{0x1C2C,0x1C33,14},{0x1C34,0x1C35,3},{0x1C36,0x1C37,14},
    {0x1CD0,0x1CD2,14},{0x1CD4,0x1CE0,14},{0x1CE1,0x1CE1,3},{0x1CE2,0x1CE8,14},{0x1CED,0x1CED,14},
    {0x1CF4,0x1CF4,14},{0x1CF7,0x1CF7,3},{0x1CF8,0x1CF9,14},{0x1DC0,0x1DFF,14},{0x200C,0x200C,10},
    {0x200D,0x200D,9},{0x203C,0x203C,11},{0x2049,0x2049,11},{0x20D0,0x20F0,14},{0x2122,0x2122,11},
    {0x2139,0x2139,11},{0x2194,0x2199,11},{0x21A9,0x21AA,11},{0x231A,0x231B,11},{0x2328,0x2328,11},
    {0x2388,0x2388,11},{0x23CF,0x23CF,11},{0x23E9,0x23F3,11},{0x23F8,0x23FA,11},{0x24C2,0x24C2,11},
    {0x25AA,0x25AB,11},{0x25B6,0x25B6,11},{0x25C0,0x25C0,11},{0x25FB,0x25FE,11},{0x2600,0x2605,11},
    {0x2607,0x2612,11},{0x2614,0x261C,11},{0x261D,0x261D,12},{0x261E,0x2685,11},{0x2690,0x26F8,11},
    {0x26F9,0x26F9,12},{0x26FA,0x2705,11},{0x2708,0x2709,11},{0x270A,0x270D,12},{0x270E,0x2712,11},
    {0x2714,0x2714,11},{0x2716,0x2716,11},{0x271D,0x271D,11},{0x2721,0x2721,11},{0x2728,0x2728,11},
    {0x2733,0x2734,11},{0x2744,0x2744,11},{0x2747,0x2747,11},{0x274C,0x274C,11},{0x274E,0x274E,11},
    {0x2753,0x2755,11},{0x2757,0x2757,11},{0x2763,0x2767,11},{0x2795,0x2797,11},{0x27A1,0x27A1,11},
    {0x27B0,0x27B0,11},{0x27BF,0x27BF,11},{0x2934,0x2935,11},{0x2B05,0x2B07,11},{0x2B1B,0x2B1C,11},
    {0x2B50,0x2B50,11},{0x2B55,0x2B55,11},{0x2CEF,0x2CF1,14},{0x2D7F,0x2D7F,14},{0x2DE0,0x2DFF,14},
    {0x302A,0x302F,14},{0x3030,0x3030,11},{0x303D,0x303D,11},{0x3099,0x309A,14},{0x3297,0x3297,11},
    {0x3299,0x3299,11},{0xA66F,0xA672,14},{0xA674,0xA67D,14},{0xA69E,0xA69F,14},{0xA6F0,0xA6F1,14},
    {0xA802,0xA802,14},{0xA806,0xA806,14},{0xA80B,0xA80B,14},{0xA823,0xA824,3},{0xA825,0xA826,14},
    {0xA827,0xA827,3},{0xA82C,0xA82C,14},{0xA880,0xA881,3},{0xA8B4,0xA8C3,3},{0xA8C4,0xA8C5,14},
    {0xA8E0,0xA8F1,14},{0xA8FF,0xA8FF,14},{0xA926,0xA92D,14},{0xA947,0xA951,14},{0xA952,0xA952,3},
    {0xA953,0xA953,14},{0xA960,0xA97C,4},{0xA980,0xA982,14},{0xA983,0xA983,3},{0xA9B3,0xA9B3,14},
    {0xA9B4,0xA9B5,3},{0xA9B6,0xA9B9,14},{0xA9BA,0xA9BB,3},{0xA9BC,0xA9BD,14},{0xA9BE,0xA9BF,3},
    {0xA9C0,0xA9C0,14},{0xA9E5,0xA9E5,14},{0xAA29,0xAA2E,14},{0xAA2F,0xAA30,3},{0xAA31,0xAA32,14},
    {0xAA33,0xAA34,3},{0xAA35,0xAA36,14},{0xAA43,0xAA43,14},{0xAA4C,0xAA4C,14},{0xAA4D,0xAA4D,3},
    {0xAA7C,0xAA7C,14},{0xAAB0,0xAAB0,14},{0xAAB2,0xAAB4,14},{0xAAB7,0xAAB8,14},{0xAABE,0xAABF,14},
    {0xAAC1,0xAAC1,14},{0xAAEB,0xAAEB,3},{0xAAEC,0xAAED,14},{0xAAEE,0xAAEF,3},{0xAAF5,0xAAF5,3},
    {0xAAF6,0xAAF6,14},{0xABE3,0xABE4,3},{0xABE5,0xABE5,14},{0xABE6,0xABE7,3},{0xABE8,0xABE8,14},
    {0xABE9,0xABEA,3},{0xABEC,0xABEC,3},{0xABED,0xABED,14},{0xD7B0,0xD7C6,5},{0xD7CB,0xD7FB,6},
    {0xFB1E,0xFB1E,14},{0xFE00,0xFE0F,14},{0xFE20,0xFE2F,14},{0xFF9E,0xFF9F,14},
    {0x101FD,0x101FD,14},{0x102E0,0x102E0,14},{0x10376,0x1037A,14},{0x10A01,0x10A03,14},
    {0x10A05,0x10A06,14},{0x10A0C,0x10A0F,14},{0x10A38,0x10A3A,14},{0x10A3F,0x10A3F,14},
    {0x10AE5,0x10AE6,14},{0x10D24,0x10D27,14},{0x10D69,0x10D6D,14},{0x10EAB,0x10EAC,14},
    {0x10EFC,0x10EFF,14},{0x10F46,0x10F50,14},{0x10F82,0x10F85,14},{0x11000,0x11000,3},
    {0x11001,0x11001,14},{0x11002,0x11002,3},{0x11038,0x11046,14},{0x11070,0x11070,14},
    {0x11073,0x11074,14},{0x1107F,0x11081,14},{0x11082,0x11082,3},{0x110B0,0x110B2,3},
    {0x110B3,0x110B6,14},{0x110B7,0x110B8,3},{0x110B9,0x110BA,14},{0x110BD,0x110BD,1},
    {0x110C2,0x110C2,14},{0x110CD,0x110CD,1},{0x11100,0x11102,14},{0x11127,0x1112B,14},
    {0x1112C,0x1112C,3},{0x1112D,0x11134,14},{0x11145,0x11146,3},{0x11173,0x11173,14},
    {0x11180,0x11181,14},{0x11182,0x11182,3},{0x111B3,0x111B5,3},{0x111B6,0x111BE,14},
    {0x111BF,0x111BF,3},{0x111C0,0x111C0,14},{0x111C2,0x111C3,1},{0x111C9,0x111CC,14},
    {0x111CE,0x111CE,3},{0x111CF,0x111CF,14},{0x1122C,0x1122E,3},{0x1122F,0x11231,14},
    {0x11232,0x11233,3},{0x11234,0x11237,14},{0x1123E,0x1123E,14},{0x11241,0x11241,14},
    {0x112DF,0x112DF,14},{0x112E0,0x112E2,3},{0x112E3,0x112EA,14},{0x11300,0x11301,14},
    {0x11302,0x11303,3},{0x1133B,0x1133C,14},{0x1133E,0x1133E,14},{0x1133F,0x1133F,3},
    {0x11340,0x11340,14},{0x11341,0x11344,3},{0x11347,0x11348,3},{0x1134B,0x1134C,3},
    {0x1134D,0x1134D,14},{0x11357,0x11357,14},{0x11362,0x11363,3},{0x11366,0x1136C,14},
    {0x11370,0x11374,14},{0x113B8,0x113B8,14},{0x113B9,0x113BA,3},{0x113BB,0x113C0,14},
    {0x113C2,0x113C2,14},{0x113C5,0x113C5,14},{0x113C7,0x113C9,14},{0x113CA,0x113CA,3},
    {0x113CC,0x113CD,3},{0x113CE,0x113D0,14},{0x113D1,0x113D1,1},{0x113D2,0x113D2,14},
    {0x113E1,0x113E2,14},{0x11435,0x11437,3},{0x11438,0x1143F,14},{0x11440,0x11441,3},
    {0x11442,0x11444,14},{0x11445,0x11445,3},{0x11446,0x11446,14},{0x1145E,0x1145E,14},
    {0x114B0,0x114B0,14},{0x114B1,0x114B2,3},{0x114B3,0x114B8,14},{0x114B9,0x114B9,3},
    {0x114BA,0x114BA,14},{0x114BB,0x114BC,3},{0x114BD,0x114BD,14},{0x114BE,0x114BE,3},
    {0x114BF,0x114C0,14},{0x114C1,0x114C1,3},{0x114C2,0x114C3,14},{0x115AF,0x115AF,14},
    {0x115B0,0x115B1,3},{0x115B2,0x115B5,14},{0x115B8,0x115BB,3},{0x115BC,0x115BD,14},
    {0x115BE,0x115BE,3},{0x115BF,0x115C0,14},{0x115DC,0x115DD,14},{0x11630,0x11632,3},
    {0x11633,0x1163A,14},{0x1163B,0x1163C,3},{0x1163D,0x1163D,14},{0x1163E,0x1163E,3},
    {0x1163F,0x11640,14},{0x116AB,0x116AB,14},{0x116AC,0x116AC,3},{0x116AD,0x116AD,14},
    {0x116AE,0x116AF,3},{0x116B0,0x116B7,14},{0x1171D,0x1171D,14},{0x1171E,0x1171E,3},
    {0x1171F,0x1171F,14},{0x11722,0x11725,14},{0x11726,0x11726,3},{0x11727,0x1172B,14},
    {0x1182C,0x1182E,3},{0x1182F,0x11837,14},{0x11838,0x11838,3},{0x11839,0x1183A,14},
    {0x11930,0x11930,14},{0x11931,0x11935,3},{0x11937,0x11938,3},{0x1193B,0x1193E,14},
    {0x1193F,0x1193F,1},{0x11940,0x11940,3},{0x11941,0x11941,1},{0x11942,0x11942,3},
    {0x11943,0x11943,14},{0x119D1,0x119D3,3},{0x119D4,0x119D7,14},{0x119DA,0x119DB,14},
    {0x119DC,0x119DF,3},{0x119E0,0x119E0,14},{0x119E4,0x119E4,3},{0x11A01,0x11A0A,14},
    {0x11A33,0x11A38,14},{0x11A39,0x11A39,3},{0x11A3A,0x11A3A,1},{0x11A3B,0x11A3E,14},
    {0x11A47,0x11A47,14},{0x11A51,0x11A56,14},{0x11A57,0x11A58,3},{0x11A59,0x11A5B,14},
    {0x11A84,0x11A89,1},{0x11A8A,0x11A96,14},{0x11A97,0x11A97,3},{0x11A98,0x11A99,14},
    {0x11C2F,0x11C2F,3},{0x11C30,0x11C36,14},{0x11C38,0x11C3D,14},{0x11C3E,0x11C3E,3},
    {0x11C3F,0x11C3F,14},{0x11C92,0x11CA7,14},{0x11CA9,0x11CA9,3},{0x11CAA,0x11CB0,14},
    {0x11CB1,0x11CB1,3},{0x11CB2,0x11CB3,14},{0x11CB4,0x11CB4,3},{0x11CB5,0x11CB6,14},
    {0x11D31,0x11D36,14},{0x11D3A,0x11D3A,14},{0x11D3C,0x11D3D,14},{0x11D3F,0x11D45,14},
    {0x11D46,0x11D46,1},{0x11D47,0x11D47,14},{0x11D8A,0x11D8E,3},{0x11D90,0x11D91,14},
    {0x11D93,0x11D94,3},{0x11D95,0x11D95,14},{0x11D96,0x11D96,3},{0x11D97,0x11D97,14},
    {0x11EF3,0x11EF4,14},{0x11EF5,0x11EF6,3},{0x11F00,0x11F01,14},{0x11F02,0x11F02,1},
    {0x11F03,0x11F03,3},{0x11F34,0x11F35,3},{0x11F36,0x11F3A,14},{0x11F3E,0x11F3F,3},
    {0x11F40,0x11F42,14},{0x11F5A,0x11F5A,14},{0x13440,0x13440,14},{0x13447,0x13455,14},
    {0x1611E,0x16129,14},{0x1612A,0x1612C,3},{0x1612D,0x1612F,14},{0x16AF0,0x16AF4,14},
    {0x16B30,0x16B36,14},{0x16D63,0x16D63,5},{0x16D67,0x16D6A,5},{0x16F4F,0x16F4F,14},
    {0x16F51,0x16F87,3},{0x16F8F,0x16F92,14},{0x16FE4,0x16FE4,14},{0x16FF0,0x16FF1,14},
    {0x1BC9D,0x1BC9E,14},{0x1CF00,0x1CF2D,14},{0x1CF30,0x1CF46,14},{0x1D165,0x1D169,14},
    {0x1D16D,0x1D172,14},{0x1D17B,0x1D182,14},{0x1D185,0x1D18B,14},{0x1D1AA,0x1D1AD,14},
    {0x1D242,0x1D244,14},{0x1DA00,0x1DA36,14},{0x1DA3B,0x1DA6C,14},{0x1DA75,0x1DA75,14},
    {0x1DA84,0x1DA84,14},{0x1DA9B,0x1DA9F,14},{0x1DAA1,0x1DAAF,14},{0x1E000,0x1E006,14},
    {0x1E008,0x1E018,14},{0x1E01B,0x1E021,14},{0x1E023,0x1E024,14},{0x1E026,0x1E02A,14},
    {0x1E08F,0x1E08F,14},{0x1E130,0x1E136,14},{0x1E2AE,0x1E2AE,14},{0x1E2EC,0x1E2EF,14},
    {0x1E4EC,0x1E4EF,14},{0x1E5EE,0x1E5EF,14},{0x1E8D0,0x1E8D6,14},{0x1E944,0x1E94A,14},
    {0x1F000,0x1F0FF,11},{0x1F10D,0x1F10F,11},{0x1F12F,0x1F12F,11},{0x1F16C,0x1F171,11},
    {0x1F17E,0x1F17F,11},{0x1F18E,0x1F18E,11},{0x1F191,0x1F19A,11},{0x1F1AD,0x1F1E5,11},
    {0x1F1E6,0x1F1FF,2},{0x1F201,0x1F20F,11},{0x1F21A,0x1F21A,11},{0x1F22F,0x1F22F,11},
    {0x1F232,0x1F23A,11},{0x1F23C,0x1F23F,11},{0x1F249,0x1F384,11},{0x1F385,0x1F385,12},
    {0x1F386,0x1F3C1,11},{0x1F3C2,0x1F3C4,12},{0x1F3C5,0x1F3C6,11},{0x1F3C7,0x1F3C7,12},
    {0x1F3C8,0x1F3C9,11},{0x1F3CA,0x1F3CC,12},{0x1F3CD,0x1F3FA,11},{0x1F3FB,0x1F3FF,13},
    {0x1F400,0x1F441,11},{0x1F442,0x1F443,12},{0x1F444,0x1F445,11},{0x1F446,0x1F450,12},
    {0x1F451,0x1F465,11},{0x1F466,0x1F478,12},{0x1F479,0x1F47B,11},{0x1F47C,0x1F47C,12},
    {0x1F47D,0x1F480,11},{0x1F481,0x1F483,12},{0x1F484,0x1F484,11},{0x1F485,0x1F487,12},
    {0x1F488,0x1F48E,11},{0x1F48F,0x1F48F,12},{0x1F490,0x1F490,11},{0x1F491,0x1F491,12},
    {0x1F492,0x1F4A9,11},{0x1F4AA,0x1F4AA,12},{0x1F4AB,0x1F53D,11},{0x1F546,0x1F573,11},
    {0x1F574,0x1F575,12},{0x1F576,0x1F579,11},{0x1F57A,0x1F57A,12},{0x1F57B,0x1F58F,11},
    {0x1F590,0x1F590,12},{0x1F591,0x1F594,11},{0x1F595,0x1F596,12},{0x1F597,0x1F644,11},
    {0x1F645,0x1F647,12},{0x1F648,0x1F64A,11},{0x1F64B,0x1F64F,12},{0x1F680,0x1F6A2,11},
    {0x1F6A3,0x1F6A3,12},{0x1F6A4,0x1F6B3,11},{0x1F6B4,0x1F6B6,12},{0x1F6B7,0x1F6BF,11},
    {0x1F6C0,0x1F6C0,12},{0x1F6C1,0x1F6CB,11},{0x1F6CC,0x1F6CC,12},{0x1F6CD,0x1F6FF,11},
    {0x1F774,0x1F77F,11},{0x1F7D5,0x1F7FF,11},{0x1F80C,0x1F80F,11},{0x1F848,0x1F84F,11},
    {0x1F85A,0x1F85F,11},{0x1F888,0x1F88F,11},{0x1F8AE,0x1F8FF,11},{0x1F90C,0x1F90C,12},
    {0x1F90D,0x1F90E,11},{0x1F90F,0x1F90F,12},{0x1F910,0x1F917,11},{0x1F918,0x1F91F,12},
    {0x1F920,0x1F925,11},{0x1F926,0x1F926,12},{0x1F927,0x1F92F,11},{0x1F930,0x1F939,12},
    {0x1F93A,0x1F93A,11},{0x1F93C,0x1F93E,12},{0x1F93F,0x1F945,11},{0x1F947,0x1F976,11},
    {0x1F977,0x1F977,12},{0x1F978,0x1F9B4,11},{0x1F9B5,0x1F9B6,12},{0x1F9B7,0x1F9B7,11},
    {0x1F9B8,0x1F9B9,12},{0x1F9BA,0x1F9BA,11},{0x1F9BB,0x1F9BB,12},{0x1F9BC,0x1F9CC,11},
    {0x1F9CD,0x1F9CF,12},{0x1F9D0,0x1F9D0,11},{0x1F9D1,0x1F9DD,12},{0x1F9DE,0x1FAC2,11},
    {0x1FAC3,0x1FAC5,12},{0x1FAC6,0x1FAEF,11},{0x1FAF0,0x1FAF8,12},{0x1FAF9,0x1FAFF,11},
    {0x1FC00,0x1FFFD,11},{0xE0020,0xE007F,14},{0xE0100,0xE01EF,14},
};

constexpr bool in_ranges(std::span<const Range> rs, char32_t cp) {
    std::size_t lo{0}, hi{rs.size()};
    while (lo < hi) {
        const std::size_t mid{lo + (hi - lo) / 2};
        if (cp < rs[mid].lo) {
            hi = mid;
        } else if (cp > rs[mid].hi) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

// Hangul syllable block: LBase..(LCount*VCount*TCount).
constexpr char32_t HANGUL_BASE{0xAC00};
constexpr char32_t HANGUL_COUNT{19u * 21u * 28u};  // 11172

// Grapheme break class from the sorted range table + Hangul LV/LVT (binary
// search; used only for the compile-time table build and the rare non-BMP tail).
constexpr std::uint8_t graphemeClassSlow(char32_t cp) {
    if (cp >= HANGUL_BASE && cp < HANGUL_BASE + HANGUL_COUNT) {
        return ((cp - HANGUL_BASE) % 28u == 0) ? GBC::Lv : GBC::Lvt;
    }
    std::size_t lo{0}, hi{std::size(kGrapheme)};
    while (lo < hi) {
        const std::size_t mid{lo + (hi - lo) / 2};
        if (cp < kGrapheme[mid].lo) {
            hi = mid;
        } else if (cp > kGrapheme[mid].hi) {
            lo = mid + 1;
        } else {
            return kGrapheme[mid].cls;
        }
    }
    return GBC::Other;
}

// ---------------------------------------------------------------------------
// Fused per-codepoint classification (grapheme-break class + width + emoji in
// one byte), mirroring bun stringWidth.cpp's `fusedClassify` structure:
//   bits 0-4  grapheme-break-class ordinal (GBC)
//   bits 5-6  width code (see below)
//   bit  7    Emoji property
// bun ships a 3-stage lookup table generated from ICU; here the same O(1) byte
// is produced at COMPILE TIME by expanding the sorted range tables above into a
// flat direct-index table (no binary search on the hot path, no ICU). A flat
// table over the whole 0..0x10FFFF space would exceed the constexpr array-size
// limit, so codepoints below FUSED_LIMIT (covers ASCII, all of the BMP, and the
// SMP emoji/symbol planes — every codepoint the width logic meets in practice)
// are direct-indexed, and the sparse, mostly-uniform tail is classified by the
// range binary search (graphemeClassSlow / in_ranges), which the table build
// shares as its single source of truth.
//
// The width code is chosen so NARROW == 0: the default (narrow, Other,
// non-emoji) byte is therefore 0, letting the value-initialised table start
// fully default with no fill pass (keeps the consteval build within libc++'s
// hardened constexpr step budget on Clang as well as GCC).
constexpr std::uint8_t FUSED_CLASS_MASK{0x1F};
constexpr std::uint8_t FUSED_WIDTH_SHIFT{5};
constexpr std::uint8_t FUSED_WIDTH_MASK{0x3};
constexpr std::uint8_t WIDTH_NARROW{0};  // -> 1 column
constexpr std::uint8_t WIDTH_ZERO{1};    // -> 0 columns
constexpr std::uint8_t WIDTH_WIDE{2};    // -> 2 columns
constexpr std::uint8_t WIDTH_AMBIG{3};   // East Asian ambiguous (1 or 2 columns)
constexpr std::uint8_t FUSED_EMOJI_BIT{0x80};
constexpr char32_t FUSED_LIMIT{0x20000};

// Slow (range binary search) classification — identical result to the direct
// table; used to build the table and for cp >= FUSED_LIMIT.
constexpr std::uint8_t classifySlow(char32_t cp) {
    std::uint8_t width{WIDTH_NARROW};
    if (in_ranges(kZeroWidth, cp)) {
        width = WIDTH_ZERO;
    } else if (in_ranges(kWide, cp)) {
        width = WIDTH_WIDE;
    } else if (in_ranges(kAmbiguous, cp)) {
        width = WIDTH_AMBIG;
    }
    std::uint8_t byte = static_cast<std::uint8_t>((width << FUSED_WIDTH_SHIFT) | graphemeClassSlow(cp));
    if (cp >= 0x80 && in_ranges(kEmoji, cp)) {
        byte |= FUSED_EMOJI_BIT;
    }
    return byte;
}

// Compile-time expansion of the range tables into a flat direct-index table for
// cp < FUSED_LIMIT. Built by filling ranges (touching only covered codepoints,
// via a raw pointer to skip libc++'s per-access hardening) rather than
// per-codepoint binary search, to stay within the constexpr step budget of both
// toolchains. Result is bit-identical to classifySlow. Width ranges are pairwise
// disjoint and applied first as plain writes; emoji/class layers are OR/merge so
// they preserve the width bits.
consteval std::array<std::uint8_t, FUSED_LIMIT> buildFusedTable() {
    std::array<std::uint8_t, FUSED_LIMIT> t{};  // 0 == narrow / Other / non-emoji
    std::uint8_t* const d{t.data()};
    const auto fillWidth = [&](std::span<const Range> rs, std::uint8_t width) {
        const std::uint8_t bits{static_cast<std::uint8_t>(width << FUSED_WIDTH_SHIFT)};
        for (const Range r : rs) {
            if (r.lo >= FUSED_LIMIT) {
                continue;
            }
            const char32_t hi{r.hi < FUSED_LIMIT ? r.hi : FUSED_LIMIT - 1};
            for (char32_t cp{r.lo}; cp <= hi; ++cp) {
                d[cp] = bits;
            }
        }
    };
    fillWidth(kZeroWidth, WIDTH_ZERO);
    fillWidth(kWide, WIDTH_WIDE);
    fillWidth(kAmbiguous, WIDTH_AMBIG);
    for (const Range r : kEmoji) {
        if (r.lo >= FUSED_LIMIT) {
            continue;
        }
        const char32_t hi{r.hi < FUSED_LIMIT ? r.hi : FUSED_LIMIT - 1};
        for (char32_t cp{r.lo}; cp <= hi; ++cp) {
            d[cp] |= FUSED_EMOJI_BIT;
        }
    }
    for (const GRange g : kGrapheme) {
        if (g.lo >= FUSED_LIMIT) {
            continue;
        }
        const char32_t hi{g.hi < FUSED_LIMIT ? g.hi : FUSED_LIMIT - 1};
        for (char32_t cp{g.lo}; cp <= hi; ++cp) {
            d[cp] = static_cast<std::uint8_t>((d[cp] & ~FUSED_CLASS_MASK) | g.cls);
        }
    }
    for (char32_t cp{HANGUL_BASE}; cp < HANGUL_BASE + HANGUL_COUNT; ++cp) {
        const std::uint8_t cls{static_cast<std::uint8_t>(((cp - HANGUL_BASE) % 28u == 0) ? GBC::Lv : GBC::Lvt)};
        d[cp] = static_cast<std::uint8_t>((d[cp] & ~FUSED_CLASS_MASK) | cls);
    }
    return t;
}

constexpr auto kFusedTable = buildFusedTable();

// Spot-checks: the direct table must agree with the range-search source.
static_assert(kFusedTable[U'A'] == classifySlow(U'A'));
static_assert(kFusedTable[0x1B] == classifySlow(0x1B));
static_assert(kFusedTable[0x4E2D] == classifySlow(0x4E2D));
static_assert(kFusedTable[0xFF21] == classifySlow(0xFF21));
static_assert(kFusedTable[0x1F600] == classifySlow(0x1F600));
static_assert(kFusedTable[0xAC00] == classifySlow(0xAC00));
static_assert(kFusedTable[0xAC01] == classifySlow(0xAC01));
static_assert(kFusedTable[0x1F1E6] == classifySlow(0x1F1E6));

inline std::uint8_t fusedClassify(char32_t cp) {
    return (cp < FUSED_LIMIT) ? kFusedTable[cp] : classifySlow(cp);
}

constexpr std::uint8_t widthFromFused(std::uint8_t packed, bool ambiguousAsWide) {
    const std::uint8_t width{static_cast<std::uint8_t>((packed >> FUSED_WIDTH_SHIFT) & FUSED_WIDTH_MASK)};
    if (width == WIDTH_AMBIG) {
        return ambiguousAsWide ? 2 : 1;
    }
    return width == WIDTH_WIDE ? 2 : (width == WIDTH_ZERO ? 0 : 1);
}

// Terminal column width of a single codepoint (0, 1 or 2).
inline std::uint8_t codepointWidth(char32_t cp, bool ambiguousAsWide) {
    return widthFromFused(fusedClassify(cp), ambiguousAsWide);
}

}  // namespace

// ===========================================================================
// WTF-8 codepoint primitives
// ===========================================================================

// Byte length of the WTF-8 sequence a lead byte begins (1..4). An invalid lead
// (continuation byte or > 0xF7) reports 1, matching bun's lenient decoder.
// ref: bun decode_wtf8_one
export constexpr std::uint8_t wtf8_sequence_length(std::uint8_t lead) {
    if (lead < 0x80) return 1;
    if (lead < 0xC0) return 1;  // stray continuation byte
    if (lead < 0xE0) return 2;
    if (lead < 0xF0) return 3;
    if (lead < 0xF8) return 4;
    return 1;
}

// Decode one WTF-8 sequence starting at s[i]; advances i past it. Invalid lead
// or truncated sequence yields U+FFFD and advances a single byte. Lone
// surrogates pass through unchanged (WTF-8). ref: bun decode_wtf8_one
export char32_t decode_wtf8(std::string_view s, std::size_t& i) {
    const std::size_t n{s.size()};
    const auto b0{static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[i]))};
    if (b0 < 0x80) {
        i += 1;
        return b0;
    }
    if (b0 < 0xC0 || i + 1 >= n) {
        i += 1;
        return 0xFFFD;
    }
    const auto b1{static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[i + 1]))};
    if (b0 < 0xE0) {
        i += 2;
        return ((b0 & 0x1F) << 6) | (b1 & 0x3F);
    }
    if (i + 2 >= n) {
        i += 1;
        return 0xFFFD;
    }
    const auto b2{static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[i + 2]))};
    if (b0 < 0xF0) {
        i += 3;
        return ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
    }
    if (i + 3 >= n) {
        i += 1;
        return 0xFFFD;
    }
    const auto b3{static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[i + 3]))};
    i += 4;
    return ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
}

// Append the WTF-8 encoding of cp to out (1..4 bytes). Surrogates (U+D800..DFFF)
// are encoded as their 3-byte form (WTF-8, not strict UTF-8).
export void encode_wtf8(std::string& out, char32_t cp) {
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

// Append the strict-UTF-8 encoding of cp to out; lone surrogates become U+FFFD.
export void encode_utf8(std::string& out, char32_t cp) {
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        cp = 0xFFFD;
    }
    encode_wtf8(out, cp);
}

// Invoke f(codepoint) for each WTF-8 codepoint of s (in order).
export template <class F>
void for_each_codepoint(std::string_view s, F&& f) {
    std::size_t i{0};
    while (i < s.size()) {
        f(decode_wtf8(s, i));
    }
}

// ===========================================================================
// Latin-1 / ASCII detection
// ===========================================================================

// Index of the first byte >= 0x80, or nullopt if all bytes are ASCII.
// ref: bun strings::first_non_ascii
export std::optional<std::size_t> first_non_ascii(std::string_view s) {
    for (std::size_t i{0}; i < s.size(); ++i) {
        if (static_cast<std::uint8_t>(s[i]) >= 0x80) {
            return i;
        }
    }
    return std::nullopt;
}

export bool is_all_ascii(std::string_view s) { return !first_non_ascii(s).has_value(); }

// True when every UTF-16 code unit fits in Latin-1 (<= 0xFF).
export bool is_all_latin1(std::u16string_view s) {
    for (const char16_t c : s) {
        if (c > 0xFF) {
            return false;
        }
    }
    return true;
}

// ===========================================================================
// Encoding conversions
// ===========================================================================

// UTF-8/WTF-8 -> UTF-16. Invalid bytes become U+FFFD; lone surrogates encoded
// as WTF-8 pass through as surrogate code units. Matches Bun's
// toUTF16AllocForReal fallback semantics. ref: bun to_utf16_alloc_for_real
export std::u16string wtf8_to_utf16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    std::size_t i{0};
    while (i < s.size()) {
        const char32_t cp{decode_wtf8(s, i)};
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<char16_t>(cp));
        } else {
            const char32_t v{cp - 0x10000};
            out.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
        }
    }
    return out;
}

// Alias: identical fallback behaviour (bun's decoder is WTF-8 lenient).
export std::u16string utf8_to_utf16(std::string_view s) { return wtf8_to_utf16(s); }

// UTF-16 -> WTF-8. Lone surrogates are preserved (encoded as 3-byte WTF-8).
// ref: bun to_utf8_alloc (encode_wtf8_rune)
export std::string utf16_to_wtf8(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    const std::size_t n{s.size()};
    for (std::size_t i{0}; i < n; ++i) {
        char32_t cp{s[i]};
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        encode_wtf8(out, cp);
    }
    return out;
}

// UTF-16 -> strict UTF-8. Lone surrogates become U+FFFD.
export std::string utf16_to_utf8(std::u16string_view s) {
    std::string out;
    out.reserve(s.size());
    const std::size_t n{s.size()};
    for (std::size_t i{0}; i < n; ++i) {
        char32_t cp{s[i]};
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        encode_utf8(out, cp);
    }
    return out;
}

// Latin-1 bytes -> UTF-8 (each byte is a codepoint 0x00..0xFF).
export std::string latin1_to_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        encode_wtf8(out, static_cast<std::uint8_t>(ch));
    }
    return out;
}

// Latin-1 bytes -> UTF-16.
export std::u16string latin1_to_utf16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    for (const char ch : s) {
        out.push_back(static_cast<char16_t>(static_cast<std::uint8_t>(ch)));
    }
    return out;
}

// UTF-8 byte length that Latin-1 input would occupy (high bytes take 2 bytes).
// ref: bun element_length_latin1_into_utf8
export std::size_t utf8_length_of_latin1(std::string_view s) {
    std::size_t len{0};
    for (const char ch : s) {
        len += (static_cast<std::uint8_t>(ch) >= 0x80) ? 2 : 1;
    }
    return len;
}

// WTF-8 byte length that UTF-16 input would occupy.
// ref: bun element_length_utf16_into_utf8
export std::size_t utf8_length_of_utf16(std::u16string_view s) {
    std::size_t len{0};
    const std::size_t n{s.size()};
    for (std::size_t i{0}; i < n; ++i) {
        const char32_t c{s[i]};
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            len += 4;
            ++i;
        } else if (c < 0x80) {
            len += 1;
        } else if (c < 0x800) {
            len += 2;
        } else {
            len += 3;
        }
    }
    return len;
}

// ===========================================================================
// ANSI escape-sequence handling. ref: bun ANSIHelpers.h
// ===========================================================================
namespace {

constexpr bool isEscapeIntroducer(char32_t c) {
    switch (c) {
    case 0x1b: case 0x9b: case 0x9d: case 0x90: case 0x98: case 0x9e: case 0x9f:
        return true;
    default:
        return false;
    }
}

// Index of the first escape-relevant codepoint at/after `from` (introducers plus
// standalone C1 ST 0x9c), or cps.size() if none.
std::size_t findEscape(std::span<const char32_t> cps, std::size_t from) {
    for (std::size_t i{from}; i < cps.size(); ++i) {
        if (isEscapeIntroducer(cps[i]) || cps[i] == 0x9c) {
            return i;
        }
    }
    return cps.size();
}

// Consume the ANSI escape sequence beginning at `start` (chaining consecutive
// sequences). Returns the index just past it; == start for a non-introducer
// (e.g. standalone 0x9c). ref: bun consumeANSI
std::size_t consumeANSI(std::span<const char32_t> cps, std::size_t start) {
    enum class St { start, gotEsc, ignoreNext, inCsi, inOsc, inOscEsc, needSt, needStEsc };
    St state{St::start};
    const std::size_t end{cps.size()};
    for (std::size_t it{start}; it < end; ++it) {
        const char32_t c{cps[it]};
        switch (state) {
        case St::start:
            switch (c) {
            case 0x1b: state = St::gotEsc; break;
            case 0x9b: state = St::inCsi; break;
            case 0x9d: state = St::inOsc; break;
            case 0x90: case 0x98: case 0x9e: case 0x9f: state = St::needSt; break;
            default: return it;
            }
            break;
        case St::gotEsc:
            switch (c) {
            case '[': state = St::inCsi; break;
            case ' ': case '#': case '%': case '(': case ')': case '*': case '+':
            case '.': case '/': state = St::ignoreNext; break;
            case ']': state = St::inOsc; break;
            case 'P': case 'X': case '^': case '_': state = St::needSt; break;
            default: state = St::start; break;
            }
            break;
        case St::ignoreNext:
            state = St::start;
            break;
        case St::inCsi:
            if (c >= 0x40 && c <= 0x7e) {
                state = St::start;
            }
            break;
        case St::inOsc:
            if (c == 0x07 || c == 0x9c) {
                state = St::start;
            } else if (c == 0x1b) {
                state = St::inOscEsc;
            }
            break;
        case St::inOscEsc:
            state = (c == '\\') ? St::start : St::inOsc;
            break;
        case St::needSt:
            if (c == 0x9c) {
                state = St::start;
            } else if (c == 0x1b) {
                state = St::needStEsc;
            }
            break;
        case St::needStEsc:
            state = (c == '\\') ? St::start : St::needSt;
            break;
        }
    }
    return end;
}

}  // namespace

// Remove ANSI escape sequences from s (WTF-8 in, WTF-8 out). ref: bun stripANSI
export std::string strip_ansi(std::string_view s) {
    std::vector<char32_t> cps;
    cps.reserve(s.size());
    for_each_codepoint(s, [&](char32_t cp) { cps.push_back(cp); });

    std::string out;
    out.reserve(s.size());
    std::size_t start{0};
    const std::size_t n{cps.size()};
    while (start < n) {
        const std::size_t esc{findEscape(cps, start)};
        for (std::size_t k{start}; k < esc; ++k) {
            encode_wtf8(out, cps[k]);
        }
        if (esc == n) {
            break;
        }
        const std::size_t next{consumeANSI(cps, esc)};
        if (next == esc) {
            encode_wtf8(out, cps[esc]);  // non-introducer (standalone 0x9c)
            start = esc + 1;
        } else {
            start = next;
        }
    }
    return out;
}

// ===========================================================================
// Terminal string width. ref: bun stringWidth.cpp visibleUTF16Width
// ===========================================================================

export struct StringWidthOptions {
    bool count_ansi_escape_codes{false};
    bool ambiguous_is_narrow{true};
};

namespace {

// Accumulates one grapheme cluster and decides its terminal width. Flags,
// keycaps, emoji-with-modifier/ZWJ and variation selectors override the plain
// codepoint-width sum. ref: bun GraphemeState
struct GraphemeState {
    char32_t firstCp{0};
    std::uint16_t nonEmojiWidth{0};
    std::uint8_t baseWidth{0};
    std::uint8_t count{0};
    bool emojiBase{false};
    bool keycap{false};
    bool regional{false};
    bool skinTone{false};
    bool zwj{false};
    bool vs15{false};
    bool vs16{false};

    static bool isRegional(char32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }
    static bool isSkinTone(char32_t cp) { return cp >= 0x1F3FB && cp <= 0x1F3FF; }

    void reset(char32_t cp, std::uint8_t packed, bool ambiguousAsWide) {
        *this = GraphemeState{};
        firstCp = cp;
        count = 1;
        // Fast path for ASCII: no emoji complexity, simple width. ref: bun.
        if (cp < 0x80) {
            const std::uint8_t w = (cp >= 0x20 && cp < 0x7F) ? 1 : 0;
            baseWidth = w;
            nonEmojiWidth = w;
            return;
        }
        baseWidth = widthFromFused(packed, ambiguousAsWide);
        nonEmojiWidth = baseWidth;
        emojiBase = (packed & FUSED_EMOJI_BIT) != 0;
        keycap = (cp == 0x20E3);
        regional = isRegional(cp);
        skinTone = isSkinTone(cp);
        zwj = (cp == 0x200D);
    }

    void add(char32_t cp, std::uint8_t packed, bool ambiguousAsWide) {
        if (count < 0xFF) {
            ++count;
        }
        keycap = keycap || (cp == 0x20E3);
        regional = regional || isRegional(cp);
        skinTone = skinTone || isSkinTone(cp);
        zwj = zwj || (cp == 0x200D);
        vs15 = vs15 || (cp == 0xFE0E);
        vs16 = vs16 || (cp == 0xFE0F);
        const std::uint32_t w{static_cast<std::uint32_t>(nonEmojiWidth) + widthFromFused(packed, ambiguousAsWide)};
        nonEmojiWidth = static_cast<std::uint16_t>(std::min<std::uint32_t>(w, 1023));
    }

    std::size_t width() const {
        if (count == 0) return 0;
        if (regional && count >= 2) return 2;
        if (keycap) return 2;
        if (regional) return 1;
        if (emojiBase && (skinTone || zwj)) return 2;
        if (vs15 || vs16) {
            if (baseWidth == 2) return 2;
            if (vs16) {
                if ((firstCp >= 0x30 && firstCp <= 0x39) || firstCp == 0x23 || firstCp == 0x2A) return 1;
                if (firstCp < 0x80) return 1;
                return 2;
            }
            return 1;
        }
        return nonEmojiWidth;
    }
};

// UAX #29 grapheme break between gb1 and gb2, carrying `state`. Returns true to
// break. ref: bun computeGraphemeBreakNoControl
enum class GBState : std::uint8_t { Default, Regional, ExtPict, IndicConsonant, IndicLinker };

constexpr bool gbcIsIndicExtend(std::uint8_t gb) {
    return gb == GBC::IndicConjunctBreakExtend || gb == GBC::Zwj;
}
constexpr bool gbcIsExtend(std::uint8_t gb) {
    return gb == GBC::Zwnj || gb == GBC::IndicConjunctBreakExtend || gb == GBC::IndicConjunctBreakLinker;
}
constexpr bool gbcIsExtPict(std::uint8_t gb) {
    return gb == GBC::ExtendedPictographic || gb == GBC::EmojiModifierBase;
}

constexpr bool graphemeBreakCompute(std::uint8_t gb1, std::uint8_t gb2, GBState& state) {
    switch (state) {
    case GBState::Regional:
        if (gb1 != GBC::RegionalIndicator || gb2 != GBC::RegionalIndicator) state = GBState::Default;
        break;
    case GBState::ExtPict: {
        const auto expected = [](std::uint8_t gb) {
            return gb == GBC::IndicConjunctBreakExtend || gb == GBC::IndicConjunctBreakLinker
                || gb == GBC::Zwnj || gb == GBC::Zwj || gb == GBC::ExtendedPictographic
                || gb == GBC::EmojiModifierBase || gb == GBC::EmojiModifier;
        };
        if (!expected(gb1) || !expected(gb2)) state = GBState::Default;
        break;
    }
    case GBState::IndicConsonant:
    case GBState::IndicLinker: {
        const auto expected = [](std::uint8_t gb) {
            return gb == GBC::IndicConjunctBreakConsonant || gb == GBC::IndicConjunctBreakLinker
                || gb == GBC::IndicConjunctBreakExtend || gb == GBC::Zwj;
        };
        if (!expected(gb1) || !expected(gb2)) state = GBState::Default;
        break;
    }
    case GBState::Default:
        break;
    }

    if (gb1 == GBC::L && (gb2 == GBC::L || gb2 == GBC::V || gb2 == GBC::Lv || gb2 == GBC::Lvt)) return false;
    if ((gb1 == GBC::Lv || gb1 == GBC::V) && (gb2 == GBC::V || gb2 == GBC::T)) return false;
    if ((gb1 == GBC::Lvt || gb1 == GBC::T) && gb2 == GBC::T) return false;
    if (gb2 == GBC::SpacingMark) return false;
    if (gb1 == GBC::Prepend) return false;

    if (gb1 == GBC::IndicConjunctBreakConsonant) {
        if (gbcIsIndicExtend(gb2)) { state = GBState::IndicConsonant; return false; }
        if (gb2 == GBC::IndicConjunctBreakLinker) { state = GBState::IndicLinker; return false; }
    } else if (state == GBState::IndicConsonant) {
        if (gb2 == GBC::IndicConjunctBreakLinker) { state = GBState::IndicLinker; return false; }
        if (gbcIsIndicExtend(gb2)) return false;
        state = GBState::Default;
    } else if (state == GBState::IndicLinker) {
        if (gb2 == GBC::IndicConjunctBreakLinker || gbcIsIndicExtend(gb2)) return false;
        if (gb2 == GBC::IndicConjunctBreakConsonant) { state = GBState::Default; return false; }
        state = GBState::Default;
    }

    if (gbcIsExtPict(gb1)) {
        if (gbcIsExtend(gb2) || gb2 == GBC::Zwj) { state = GBState::ExtPict; return false; }
        if (gb1 == GBC::EmojiModifierBase && gb2 == GBC::EmojiModifier) { state = GBState::ExtPict; return false; }
    } else if (state == GBState::ExtPict) {
        if ((gbcIsExtend(gb1) || gb1 == GBC::EmojiModifier) && (gbcIsExtend(gb2) || gb2 == GBC::Zwj)) return false;
        if (gb1 == GBC::Zwj && gbcIsExtPict(gb2)) { state = GBState::Default; return false; }
        state = GBState::Default;
    }

    if (gb1 == GBC::RegionalIndicator && gb2 == GBC::RegionalIndicator) {
        if (state == GBState::Default) { state = GBState::Regional; return false; }
        state = GBState::Default;
        return true;
    }

    if (gbcIsExtend(gb2) || gb2 == GBC::Zwj) return false;
    return true;
}

// Precomputed decision table over every (state, class1, class2) permutation, so
// the hot loop resolves a grapheme break with a single lookup instead of the
// branchy algorithm. ref: bun kGraphemeBreakDecisions. Key: state (3 bits) |
// gb1 << 3 | gb2 << 8. Value: shouldBreak (bit 0) | nextState << 1.
constexpr std::size_t GBC_COUNT{17};
constexpr std::size_t GBS_COUNT{5};

constexpr std::size_t graphemeBreakKey(std::uint8_t gb1, std::uint8_t gb2, GBState state) {
    return static_cast<std::size_t>(state) | (static_cast<std::size_t>(gb1) << 3)
        | (static_cast<std::size_t>(gb2) << 8);
}

consteval std::array<std::uint8_t, 1u << 13> buildGraphemeBreakTable() {
    std::array<std::uint8_t, 1u << 13> r{};
    for (std::size_t s = 0; s < GBS_COUNT; ++s) {
        for (std::size_t i1 = 0; i1 < GBC_COUNT; ++i1) {
            for (std::size_t i2 = 0; i2 < GBC_COUNT; ++i2) {
                GBState state{static_cast<GBState>(s)};
                const bool shouldBreak =
                    graphemeBreakCompute(static_cast<std::uint8_t>(i1), static_cast<std::uint8_t>(i2), state);
                const std::size_t key = graphemeBreakKey(static_cast<std::uint8_t>(i1),
                                                         static_cast<std::uint8_t>(i2),
                                                         static_cast<GBState>(s));
                r[key] = static_cast<std::uint8_t>((shouldBreak ? 1u : 0u)
                                                   | (static_cast<std::uint8_t>(state) << 1));
            }
        }
    }
    return r;
}

constexpr auto kGraphemeBreakTable = buildGraphemeBreakTable();

inline bool graphemeBreak(std::uint8_t gb1, std::uint8_t gb2, GBState& state) {
    const std::uint8_t value = kGraphemeBreakTable[graphemeBreakKey(gb1, gb2, state)];
    state = static_cast<GBState>(value >> 1);
    return (value & 1) != 0;
}

template <typename Codepoints>
std::size_t stringWidthImpl(Codepoints codepoints, StringWidthOptions opts) {
    // The 8-bit Latin-1 entry marks its Codepoints struct so the CSI parser can
    // match bun's per-encoding ANSI-termination quirk (see the sawCsi block).
    constexpr bool isLatin1 = requires { typename Codepoints::latin1_tag; };
    const bool excludeAnsi{!opts.count_ansi_escape_codes};
    const bool ambiguousAsWide{!opts.ambiguous_is_narrow};

    std::size_t len{0};
    bool hasPrev{false};
    std::uint8_t prevClass{GBC::Other};
    GBState breakState{GBState::Default};
    GraphemeState gs;
    bool saw1b{false}, sawCsi{false}, sawOsc{false}, oscEsc{false};

    char32_t cp{};
    while (true) {
        if constexpr (requires {
                          codepoints.printableAsciiRun();
                          codepoints.asciiAt(std::size_t{});
                          codepoints.advance(std::size_t{});
                      }) {
            if (!saw1b && !sawCsi && !sawOsc) {
                const std::size_t run{codepoints.printableAsciiRun()};
                if (run > 0) {
                    len += gs.width();
                    if (run > 1) len += run - 1;
                    const char32_t last{codepoints.asciiAt(run - 1)};
                    const std::uint8_t asciiPacked{fusedClassify(last)};
                    gs.reset(last, asciiPacked, ambiguousAsWide);
                    hasPrev = true;
                    prevClass = GBC::Other;
                    breakState = GBState::Default;
                    codepoints.advance(run);
                    continue;
                }
            }
        }
        if (!codepoints.next(cp)) break;

        if (sawOsc) {
            if (oscEsc) {
                oscEsc = false;
                // bun highway_strings.cpp:904-916: a non-`\` ESC inside an OSC
                // consumes only the ESC — the next byte is re-evaluated, so a
                // following BEL/ST still terminates and a second ESC re-arms.
                if (cp == '\\') { sawOsc = saw1b = false; continue; }
            }
            if (cp == 0x07 || cp == 0x9c) { sawOsc = saw1b = false; }
            else if (cp == 0x1b) { oscEsc = true; }
            continue;
        }
        if (sawCsi) {
            // A CSI ends on a final byte in [0x40,0x7e]. bun's Latin-1 kernel
            // (highway_strings.cpp:899-903) consumes high bytes 0x80-0xFF as
            // CSI params, whereas visibleUTF16Width (stringWidth.cpp:984-988)
            // ends the CSI on a non-ASCII codepoint — so gate the high-byte
            // termination to the non-Latin-1 paths for byte-exact parity.
            if (cp >= 0x40 && cp <= 0x7e) {
                sawCsi = saw1b = false;
            } else if (!isLatin1 && cp > 0x7F) {
                sawCsi = saw1b = false;  // abnormal end (UTF-16/WTF-8 only), cp discarded
            }
            continue;
        }
        if (saw1b) {
            if (cp == '[') { sawCsi = true; continue; }
            if (cp == ']') { sawOsc = true; continue; }
            if (cp == 0x1b) { continue; }
            len += codepointWidth(cp, ambiguousAsWide);  // ESC + ordinary char
            saw1b = false;
            continue;
        }
        if (excludeAnsi && cp == 0x1b) {
            saw1b = true;
            continue;
        }

        const std::uint8_t packed{fusedClassify(cp)};
        const std::uint8_t cls{static_cast<std::uint8_t>(packed & FUSED_CLASS_MASK)};
        if (hasPrev) {
            if (graphemeBreak(prevClass, cls, breakState)) {
                len += gs.width();
                gs.reset(cp, packed, ambiguousAsWide);
            } else {
                gs.add(cp, packed, ambiguousAsWide);
            }
        } else {
            gs.reset(cp, packed, ambiguousAsWide);
        }
        hasPrev = true;
        prevClass = cls;
    }

    len += gs.width();
    return len;
}

}  // namespace

// Terminal column width of s (WTF-8). ref: bun visibleUTF16Width / jsFunctionBunStringWidth
export std::size_t string_width(std::string_view s, StringWidthOptions opts = {}) {
    struct Codepoints {
        std::string_view s;
        std::size_t i{};

        bool next(char32_t& cp) {
            if (i >= s.size()) return false;
            cp = decode_wtf8(s, i);
            return true;
        }
    };
    return stringWidthImpl(Codepoints{.s = s}, opts);
}

// Direct Latin-1 entry for JSC's compact 8-bit strings. Non-ASCII bytes are
// code points U+0080..U+00FF, not UTF-8 lead bytes, so this path also removes
// the temporary UTF-8 allocation previously paid by every such call.
export std::size_t string_width(std::span<const unsigned char> s, StringWidthOptions opts = {}) {
    // bug-compatible with bun's visibleLatin1Width: the 8-bit fast path uses a
    // fixed per-byte table, so the ambiguousIsNarrow option is ignored — Latin-1
    // East-Asian-ambiguous bytes (e.g. 0xA7 §) always count as narrow.
    opts.ambiguous_is_narrow = true;
    struct Codepoints {
        using latin1_tag = int;  // marks the 8-bit path for the CSI parser (local-class type tag)
        std::span<const unsigned char> s;
        std::size_t i{};

        std::size_t printableAsciiRun() const {
            std::size_t end{i};
            while (end < s.size() && s[end] >= 0x20 && s[end] <= 0x7E) ++end;
            return end - i;
        }

        char32_t asciiAt(std::size_t offset) const { return s[i + offset]; }

        void advance(std::size_t count) { i += count; }

        bool next(char32_t& cp) {
            if (i >= s.size()) return false;
            cp = s[i++];
            return true;
        }
    };
    return stringWidthImpl(Codepoints{.s = s}, opts);
}

// Direct UTF-16 entry used by the JSC binding. This mirrors bun's
// visibleUTF16Width path and avoids allocating/transcoding a temporary UTF-8
// string for every Bun.stringWidth call. Lone surrogate units are skipped, as
// in bun's decodeUTF16Codepoint helper.
export std::size_t string_width(std::span<const char16_t> s, StringWidthOptions opts = {}) {
    struct Codepoints {
        std::span<const char16_t> s;
        std::size_t i{};

        std::size_t printableAsciiRun() const {
            std::size_t end{i};
            while (end < s.size() && s[end] >= 0x20 && s[end] <= 0x7E) ++end;
            return end - i;
        }

        char32_t asciiAt(std::size_t offset) const { return s[i + offset]; }

        void advance(std::size_t count) { i += count; }

        bool next(char32_t& cp) {
            while (i < s.size()) {
                const char16_t unit{s[i++]};
                if (unit >= 0xD800 && unit <= 0xDBFF) {
                    if (i < s.size() && s[i] >= 0xDC00 && s[i] <= 0xDFFF) {
                        const char16_t trail{s[i++]};
                        cp = 0x10000 + ((static_cast<char32_t>(unit) - 0xD800) << 10)
                            + (static_cast<char32_t>(trail) - 0xDC00);
                        return true;
                    }
                    continue;
                }
                if (unit >= 0xDC00 && unit <= 0xDFFF) continue;
                cp = unit;
                return true;
            }
            return false;
        }
    };
    return stringWidthImpl(Codepoints{.s = s}, opts);
}

}  // namespace mbun::core::strings
