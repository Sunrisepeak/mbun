// src/js_printer/encoding.cppm — module mbun.js_printer.encoding
//
// CAP-BUILD-PRINTER shard 1/4 — encoding primitives for the JS printer.
//
// Mechanical port ("移植三段法" stage 1+2+3) of the escape/encode primitives the
// printer's string layer stands on. bun keeps these in `bun_core` rather than in
// the printer crate; mbun has no bun_core-equivalent that owns them, so they live
// here next to their only consumer (mbun.js_printer.quote).
//
// Blueprint (file:line are .mbun/bun-ref/src/...):
//   js_printer/lib.rs:28                 `enum Encoding`
//   bun_core/string/mod.rs:2410-2413     FIRST_ASCII / LAST_ASCII /
//                                        FIRST_HIGH_SURROGATE / LAST_LOW_SURROGATE
//   bun_core/string/mod.rs:2419          `bmp_escape`
//   bun_core/string/mod.rs:2427          `surrogate_pair_escape`
//   bun_core/fmt.rs:2920 / :2961         `hex_byte_upper` / `hex_u16`
//   bun_core/lib.rs:1272 / :1279 / :1286 `u16_lead` / `u16_trail` /
//                                        `encode_surrogate_pair`
//   bun_core/lib.rs:1534                 `encode_wtf8_rune`
//   bun_core/lib.rs:2219 / :2232         `wtf8_byte_sequence_length{,_with_invalid}`
//   bun_core/string/immutable/unicode.rs:103 / :1252
//                                        `decode_wtf8_rune_t{,_multibyte}`
//
// Everything here is `constexpr` and allocation-free: these sit on the hottest
// loop in the printer, so the fan-out that Rust pays with a `CodePointZero`
// trait collapses to plain `std::int32_t` arithmetic (mbun has exactly one
// instantiation — the printer always decodes into `i32`, see lib.rs:927/:1027).
export module mbun.js_printer.encoding;

import std;

export namespace mbun::js_printer {

// ─────────────────────────────────────────────────────────────────────────────
// Encoding — ref lib.rs:28
//
// In bun this is a `ConstParamTy` stand-in for `bun_core::strings::Encoding` so
// it can be a const-generic parameter. In MC++ a scoped enum is already a valid
// non-type template parameter, so no stand-in is needed: this IS the type used
// as `template <Encoding ENC>` in mbun.js_printer.quote.
// ─────────────────────────────────────────────────────────────────────────────
enum class Encoding : std::uint8_t {
    Ascii,
    Utf8,
    Latin1,
    Utf16,
};

// ── ASCII / surrogate bounds — ref bun_core/string/mod.rs:2410-2413 ──────────
inline constexpr std::uint32_t FIRST_ASCII { 0x20 };
inline constexpr std::uint32_t LAST_ASCII { 0x7E };
inline constexpr std::uint32_t FIRST_HIGH_SURROGATE { 0xD800 };
inline constexpr std::uint32_t LAST_LOW_SURROGATE { 0xDFFF };

// ── hex — ref bun_core/fmt.rs:2920 (`hex_byte_upper`), :2961 (`hex_u16`) ─────
inline constexpr char UPPER_HEX_TABLE[17] { "0123456789ABCDEF" };

// `hex2_upper` — ref bun_core/fmt.rs:2940 (alias of `hex_byte_upper`, :2920).
constexpr std::array<char, 2> hex2_upper(std::uint8_t b) {
    return { UPPER_HEX_TABLE[(b >> 4) & 0x0F], UPPER_HEX_TABLE[b & 0x0F] };
}

// `hex4_upper` — ref bun_core/fmt.rs:2951 (alias of `hex_u16<false>`, :2961).
constexpr std::array<char, 4> hex4_upper(std::uint16_t v) {
    return {
        UPPER_HEX_TABLE[(v >> 12) & 0x0F],
        UPPER_HEX_TABLE[(v >> 8) & 0x0F],
        UPPER_HEX_TABLE[(v >> 4) & 0x0F],
        UPPER_HEX_TABLE[v & 0x0F],
    };
}

// ── UTF-16 surrogate primitives (ICU utf16.h) ────────────────────────────────

// `u16_lead` — ref bun_core/lib.rs:1272. Precondition 0x10000 <= cp <= 0x10FFFF.
constexpr std::uint16_t u16_lead(std::uint32_t supplementary) {
    return static_cast<std::uint16_t>((supplementary >> 10) + 0xD7C0);
}

// `u16_trail` — ref bun_core/lib.rs:1279. Same precondition as u16_lead.
constexpr std::uint16_t u16_trail(std::uint32_t supplementary) {
    return static_cast<std::uint16_t>((supplementary & 0x3FF) | 0xDC00);
}

// `encode_surrogate_pair` — ref bun_core/lib.rs:1286. `[lead, trail]`.
constexpr std::array<std::uint16_t, 2> encode_surrogate_pair(std::uint32_t supplementary) {
    return { u16_lead(supplementary), u16_trail(supplementary) };
}

// ── escape sequences — ref bun_core/string/mod.rs:2419 / :2427 ───────────────

// `bmp_escape` — `\uHHHH` (uppercase hex) for a BMP code unit, lone surrogates
// included. ref bun_core/string/mod.rs:2419.
constexpr std::array<char, 6> bmp_escape(std::uint32_t c) {
    const std::array<char, 4> h { hex4_upper(static_cast<std::uint16_t>(c)) };
    return { '\\', 'u', h[0], h[1], h[2], h[3] };
}

// `surrogate_pair_escape` — `\uHHHH\uHHHH` for a supplementary code point
// (c > 0xFFFF). ref bun_core/string/mod.rs:2427.
constexpr std::array<char, 12> surrogate_pair_escape(std::uint32_t c) {
    const std::array<std::uint16_t, 2> pair { encode_surrogate_pair(c) };
    const std::array<char, 4> l { hex4_upper(pair[0]) };
    const std::array<char, 4> h { hex4_upper(pair[1]) };
    return { '\\', 'u', l[0], l[1], l[2], l[3], '\\', 'u', h[0], h[1], h[2], h[3] };
}

// ── WTF-8 ────────────────────────────────────────────────────────────────────

// `wtf8_byte_sequence_length` — ref bun_core/lib.rs:2219. Note the deliberate
// non-validating shape: an unexpected lead byte (0x80..=0xBF continuation,
// 0xF8..=0xFF) reports 1 so the caller always advances. bun aliases this as
// `wtf8_byte_sequence_length_with_invalid` (:2232) at the printer's call sites
// (lib.rs:918, :1014) purely for spec-faithful naming — same function.
constexpr std::uint8_t wtf8_byte_sequence_length(std::uint8_t firstByte) {
    if (firstByte <= 0x7F) {
        return 1;
    }
    if (firstByte >= 0xC0 && firstByte <= 0xDF) {
        return 2;
    }
    if (firstByte >= 0xE0 && firstByte <= 0xEF) {
        return 3;
    }
    if (firstByte >= 0xF0 && firstByte <= 0xF7) {
        return 4;
    }
    return 1;
}

// `decode_wtf8_rune_t_multibyte` — ref
// bun_core/string/immutable/unicode.rs:1252. Asserts len > 1. Clone of
// esbuild's decodeWTF8Rune (itself a WTF-8 fork of Go's utf8.DecodeRune):
// every ill-formed sequence — bad continuation, overlong, out of range —
// decodes to `zero` rather than erroring, which is what lets the printer escape
// invalid input byte-wise instead of rejecting it.
constexpr std::int32_t decode_wtf8_rune_multibyte(
    const std::array<std::uint8_t, 4>& p, std::uint8_t len, std::int32_t zero) {
    const std::uint8_t s1 { p[1] };
    if ((s1 & 0xC0) != 0x80) {
        return zero;
    }
    if (len == 2) {
        const std::int32_t cp { (static_cast<std::int32_t>(p[0] & 0x1F) << 6)
            | static_cast<std::int32_t>(s1 & 0x3F) };
        return cp < 0x80 ? zero : cp;
    }

    const std::uint8_t s2 { p[2] };
    if ((s2 & 0xC0) != 0x80) {
        return zero;
    }
    if (len == 3) {
        const std::int32_t cp { (static_cast<std::int32_t>(p[0] & 0x0F) << 12)
            | (static_cast<std::int32_t>(s1 & 0x3F) << 6)
            | static_cast<std::int32_t>(s2 & 0x3F) };
        return cp < 0x800 ? zero : cp;
    }

    const std::uint8_t s3 { p[3] };
    if ((s3 & 0xC0) != 0x80) {
        return zero;
    }
    const std::int32_t cp { (static_cast<std::int32_t>(p[0] & 0x07) << 18)
        | (static_cast<std::int32_t>(s1 & 0x3F) << 12)
        | (static_cast<std::int32_t>(s2 & 0x3F) << 6)
        | static_cast<std::int32_t>(s3 & 0x3F) };
    if (cp < 0x10000 || cp > 0x10FFFF) {
        return zero;
    }
    return cp;
}

// `decode_wtf8_rune_t::<i32>` — ref bun_core/string/immutable/unicode.rs:103.
// The printer is the only caller and always instantiates `T = i32` with
// `zero = 0` (lib.rs:927, :1027), so the generic collapses.
constexpr std::int32_t decode_wtf8_rune(
    const std::array<std::uint8_t, 4>& p, std::uint8_t len, std::int32_t zero) {
    if (len == 0) {
        return zero;
    }
    if (len == 1) {
        return static_cast<std::int32_t>(p[0]);
    }
    return decode_wtf8_rune_multibyte(p, len, zero);
}

// `encode_wtf8_rune` — ref bun_core/lib.rs:1534. WTF-8: unpaired surrogates
// encode as their own 3-byte sequence rather than being replaced. Returns the
// number of bytes written (1..=4).
constexpr std::size_t encode_wtf8_rune(std::array<char, 4>& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
}

}  // namespace mbun::js_printer
