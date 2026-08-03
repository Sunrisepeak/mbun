// src/js_printer/quote.cppm — module mbun.js_printer.quote
//
// CAP-BUILD-PRINTER shard 1/4 — string / quote-selection / escaping layer.
//
// Mechanical port ("移植三段法") of the printer's string layer. This is the
// hottest loop in the printer and the one the elysia `Function.prototype
// .toString()` sniffing and `--target=bun` ascii-only both bottom out in.
//
// Blueprint (file:line are .mbun/bun-ref/src/js_printer/lib.rs unless noted):
//   :792   `can_print_without_escape`
//   :812   `best_quote_char_for_string`
//   :909   `estimate_length_for_utf8`
//   :952   `write_pre_quoted_string`        (const-generic facade)
//   :974   `write_pre_quoted_string_inner`  (the escaping loop)
//   :1167  `quote_for_json`
//   :1189  `write_json_string`
//   bun_core/string/immutable.rs:1623  `index_of_needs_escape_for_java_script_string`
//   src/jsc/bindings/highway_strings.cpp:666  the SIMD scan it dispatches to
//     (:711 is the scalar-remainder predicate — the normative definition)
//
// Verified against real bun 1.3.14 (`Bun.Transpiler#transformSync`), not just
// read off the Rust — see tests/test_js_printer_quote.cpp, whose expectations
// are transcribed from that binary's actual output.
//
// ── Deliberate deviations from the blueprint (each is a fix, not a shortcut) ──
//
// 1. Fallibility. bun threads `Result<(), bun_core::Error>` through the writers,
//    but every printer call site discards it (`let _ = ...` at :2641, :2654), and
//    the two JSON entry points only ever see an in-memory sink. mbun's sinks are
//    infallible (`std::string` append; OOM terminates), so these return `void`.
//    That is faithful to the *observable* behavior, and drops a dead error path
//    from the hot loop.
//
// 2. Out-of-bounds reads on truncated WTF-8. bun's `estimate_length_for_utf8`
//    (:919-926) and `write_pre_quoted_string_inner`'s Utf8 arm (:1020-1026) index
//    `remaining[1..3]` off a lead byte without clamping to the slice end. In Rust
//    that is a panic on a truncated final sequence; in C++ it would be an OOB
//    read. Callers guarantee validity (`debug_assert!(is_valid_wtf8(str))`,
//    :3052), so the inputs that would trip it are already contractually excluded
//    — we clamp and read 0 instead, which is byte-identical for every legal input
//    and safe for the illegal ones. (:1017 already clamps in the inner loop via
//    `clamped_width`; this just extends the same discipline to the decode.)
//
// 3. The SIMD scan. bun calls into Highway (a vendored C++ SIMD library) via FFI.
//    mbun cannot take a host dependency (AGENTS.md §Toolchain), and `<simd>` is
//    not in the LLVM-22 ∩ GCC-16 C++26 intersection, so the scan is SWAR over
//    `std::uint64_t` — portable, no intrinsics, ~8 bytes/iteration. Every SWAR
//    term is `has_zero`-based (exact, no borrow propagation) and a block hit is
//    re-verified scalar-wise against `needs_escape_byte`, which IS the normative
//    predicate. `index_of_needs_escape_scalar` is kept as the reference oracle and
//    the test differentially fuzzes the two against each other.
export module mbun.js_printer.quote;

import std;
import mbun.js_printer.encoding;

export namespace mbun::js_printer {

// ─────────────────────────────────────────────────────────────────────────────
// ByteSink — ref lib.rs:38 (`pub use bun_io::Write`)
//
// bun's escapers are generic over `W: Write + ?Sized`. The MC++ equivalent is a
// concept: static dispatch, no vtable, no `?Sized` erasure. `write_all` takes a
// byte range spelled as `string_view` because the printer's output *is* a
// `std::string` — this keeps the sink append a plain `memcpy` with no adapter.
// ─────────────────────────────────────────────────────────────────────────────
template <typename W>
concept ByteSink = requires(W& w, std::string_view bytes) {
    { w.write_all(bytes) };
};

// The only sink the printer needs today. Non-owning by design: the printer owns
// its output buffer and hands out a sink view for the duration of one escape.
class StringSink {
private:
    std::string* out_;

public:
    explicit StringSink(std::string& out)
        : out_ { &out } { }

    void write_all(std::string_view bytes) { out_->append(bytes); }

    std::string& buffer() { return *out_; }
};

static_assert(ByteSink<StringSink>);

// ─────────────────────────────────────────────────────────────────────────────
// can_print_without_escape — ref lib.rs:792
//
// `ascii_only` stays a *runtime* arg exactly as bun made it (see the PERF note at
// :786-790): the callers are the two large loops below, and keeping it runtime
// collapses them to one monomorphization each instead of one per
// (ascii_only × quote_char × json) combo.
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] constexpr bool can_print_without_escape(std::int32_t c, bool asciiOnly) {
    if (c <= static_cast<std::int32_t>(LAST_ASCII)) {
        return c >= static_cast<std::int32_t>(FIRST_ASCII) && c != static_cast<std::int32_t>('\\')
            && c != static_cast<std::int32_t>('"') && c != static_cast<std::int32_t>('\'')
            && c != static_cast<std::int32_t>('`') && c != static_cast<std::int32_t>('$');
    }
    return !asciiOnly && c != 0xFEFF && c != 0x2028 && c != 0x2029
        && (c < static_cast<std::int32_t>(FIRST_HIGH_SURROGATE)
            || c > static_cast<std::int32_t>(LAST_LOW_SURROGATE));
}

// ─────────────────────────────────────────────────────────────────────────────
// index_of_needs_escape_for_js_string
//
// ref bun_core/string/immutable.rs:1623 → highway_strings.cpp:666. Returns the
// index of the first byte needing escape, or `slice.size()` when there is none
// (bun's `Option<u32>`; a sentinel index avoids the optional in the hot loop and
// matches the C++ side's own `return text_len` at highway_strings.cpp:719).
//
// NOTE the asymmetry with `can_print_without_escape`, which is load-bearing and
// NOT a bug: this scan does not stop on `$` unless the quote is a backtick, and
// never stops on a non-active quote char, whereas `can_print_without_escape`
// rejects `"`, `'`, `` ` `` and `$` unconditionally. The escaping loop is
// correct either way — a `'` inside a `"`-quoted string is bulk-copied by the
// scan and would be written verbatim by the `0x27` arm anyway (:1109-1116).
// ─────────────────────────────────────────────────────────────────────────────

// The normative predicate — ref highway_strings.cpp:711 (scalar remainder).
[[nodiscard]] constexpr bool needs_escape_byte(std::uint8_t c, std::uint8_t quoteChar, bool isBacktick) {
    return c >= 127 || c < 0x20 || c == '\\' || c == quoteChar || (isBacktick && c == '$');
}

// Reference oracle. Kept exported so the test can differentially fuzz the SWAR
// path against it; also the tail path of the SWAR scan itself.
[[nodiscard]] constexpr std::size_t index_of_needs_escape_scalar(
    std::string_view slice, std::uint8_t quoteChar) {
    const bool isBacktick { quoteChar == '`' };
    for (std::size_t i { 0 }; i < slice.size(); ++i) {
        if (needs_escape_byte(static_cast<std::uint8_t>(slice[i]), quoteChar, isBacktick)) {
            return i;
        }
    }
    return slice.size();
}

namespace detail {

inline constexpr std::uint64_t SWAR_ONES { 0x0101'0101'0101'0101ull };
inline constexpr std::uint64_t SWAR_HIGH { 0x8080'8080'8080'8080ull };

// Classic `haszero`: sets 0x80 in every byte lane that is zero. Exact — the
// `& ~v` term kills borrow propagation, so there are no cross-lane artifacts.
[[nodiscard]] constexpr std::uint64_t swar_has_zero(std::uint64_t v) {
    return (v - SWAR_ONES) & ~v & SWAR_HIGH;
}

// `hasvalue`: lanes equal to `n`. Exact (haszero of the xor).
[[nodiscard]] constexpr std::uint64_t swar_has_value(std::uint64_t v, std::uint8_t n) {
    return swar_has_zero(v ^ (SWAR_ONES * n));
}

// Lanes with value < 0x20. Deliberately NOT the bithacks `hasless`, whose
// borrow chain can smear across lanes: `c < 0x20` ⟺ `(c & 0xE0) == 0`, so this
// reduces to an exact `has_zero` on a masked word.
[[nodiscard]] constexpr std::uint64_t swar_has_lt_0x20(std::uint64_t v) {
    return swar_has_zero(v & (SWAR_ONES * 0xE0));
}

}  // namespace detail

[[nodiscard]] inline std::size_t index_of_needs_escape_for_js_string(
    std::string_view slice, std::uint8_t quoteChar) {
    const bool isBacktick { quoteChar == '`' };
    const std::size_t n { slice.size() };
    std::size_t i { 0 };

    // SWAR block scan. Terms: <0x20, >=0x80, ==0x7F, =='\\', ==quote, [=='$'].
    // Union is a superset of `needs_escape_byte` on the block, never a subset,
    // so a hit is re-verified scalar-wise and a miss is provably clean.
    for (; i + 8 <= n; i += 8) {
        std::uint64_t v {};
        std::memcpy(&v, slice.data() + i, sizeof(v));
        std::uint64_t mask { detail::swar_has_lt_0x20(v) | (v & detail::SWAR_HIGH)
            | detail::swar_has_value(v, 0x7F) | detail::swar_has_value(v, '\\')
            | detail::swar_has_value(v, quoteChar) };
        if (isBacktick) {
            mask |= detail::swar_has_value(v, '$');
        }
        if (mask != 0) [[unlikely]] {
            for (std::size_t j { i }; j < i + 8; ++j) {
                if (needs_escape_byte(static_cast<std::uint8_t>(slice[j]), quoteChar, isBacktick)) {
                    return j;
                }
            }
        }
    }

    for (; i < n; ++i) {
        if (needs_escape_byte(static_cast<std::uint8_t>(slice[i]), quoteChar, isBacktick)) {
            return i;
        }
    }
    return n;
}

// ─────────────────────────────────────────────────────────────────────────────
// best_quote_char_for_string — ref lib.rs:812
//
// Ported 1:1 including two shapes that look like bugs and are NOT to be
// "fixed" — they are the spec, and real bun's output depends on them:
//   * `n = str.len().min(1024)` (:820) — only the first 1024 code units are
//     costed; a 2000-char string's tail cannot influence quote choice.
//   * the `$` arm (:835) bounds-checks against `str.len()`, NOT `n`, so a `$` at
//     index 1023 does look at index 1024. Reproduced exactly.
//   * the `\\` arm (:831) does `i += 1` and then falls into the shared `i += 1`
//     — i.e. a backslash consumes the following unit, so `\"` costs nothing.
//
// Real-bun cross-check (transformSync, loader:"ts"):
//   `'has "double" quotes'`  → `'has "double" quotes'`   (single=0 double=2 → ')
//   `"has 'single' quotes"`  → `"has 'single' quotes"`   (single=2 double=0 → ")
//   `'has "d" and \'s\''`    → `` `has "d" and 's'` ``   (2/2/0 → backtick)
//   `'a "" b \''`            → `` `a "" b '` ``          (1/2/0 → backtick)
//   `'a\nb'`                 → `` `a<LF>b` ``            (newline costs ' and ")
// ─────────────────────────────────────────────────────────────────────────────
template <typename T>
    requires std::convertible_to<T, std::uint32_t>
[[nodiscard]] constexpr std::uint8_t best_quote_char_for_string(
    std::span<const T> str, bool allowBacktick) {
    std::size_t singleCost { 0 };
    std::size_t doubleCost { 0 };
    std::size_t backtickCost { 0 };
    std::size_t i { 0 };
    const std::size_t n { std::min<std::size_t>(str.size(), 1024) };

    while (i < n) {
        switch (static_cast<std::uint32_t>(str[i])) {
        case 0x27:  // '
            singleCost += 1;
            break;
        case 0x22:  // "
            doubleCost += 1;
            break;
        case 0x60:  // `
            backtickCost += 1;
            break;
        case 0x0A:  // \n
            singleCost += 1;
            doubleCost += 1;
            break;
        case 0x5C:  // backslash — consumes the escaped unit
            i += 1;
            break;
        case 0x24:  // $ — only costs inside a template, and only before `{`
            if (i + 1 < str.size() && static_cast<std::uint32_t>(str[i + 1]) == std::uint32_t { '{' }) {
                backtickCost += 1;
            }
            break;
        default:
            break;
        }
        i += 1;
    }

    if (allowBacktick && backtickCost < std::min(singleCost, doubleCost)) {
        return '`';
    }
    if (singleCost < doubleCost) {
        return '\'';
    }
    return '"';
}

// ─────────────────────────────────────────────────────────────────────────────
// estimate_length_for_utf8 — ref lib.rs:909
//
// Sizing pass: the printed length of `input` quoted with `quote_char`. bun has
// no live caller today (`quote_for_json` deliberately uses a 12.5% heuristic
// instead — see the note at :1175-1181, it would double the scan work) but it is
// part of this shard's assigned surface, so it is ported and tested.
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] inline std::size_t estimate_length_for_utf8(
    std::string_view input, bool asciiOnly, std::uint8_t quoteChar) {
    std::string_view remaining { input };
    std::size_t len { 2 };  // for quotes

    for (;;) {
        const std::size_t i { index_of_needs_escape_for_js_string(remaining, quoteChar) };
        if (i == remaining.size()) {
            break;
        }
        len += i;
        remaining.remove_prefix(i);

        const std::uint8_t charLen { wtf8_byte_sequence_length(static_cast<std::uint8_t>(remaining[0])) };
        // Deviation 2 (see file header): clamp instead of reading past the end.
        const std::size_t readable { std::min<std::size_t>(charLen, remaining.size()) };
        std::array<std::uint8_t, 4> bytes {};
        for (std::size_t k { 0 }; k < readable; ++k) {
            bytes[k] = static_cast<std::uint8_t>(remaining[k]);
        }

        const std::int32_t c { decode_wtf8_rune(bytes, charLen, 0) };
        if (can_print_without_escape(c, asciiOnly)) {
            len += charLen;
        } else if (c <= 0xFFFF) {
            len += 6;
        } else {
            len += 12;
        }
        remaining.remove_prefix(std::min<std::size_t>(charLen, remaining.size()));
    }

    return len + remaining.size();
}

// ─────────────────────────────────────────────────────────────────────────────
// write_pre_quoted_string_inner — ref lib.rs:974
//
// THE escaping loop. `quote_char` / `ascii_only` / `json` are runtime args and
// `ENCODING` is the only compile-time parameter, exactly as bun settled on
// (:968-972): the encoding changes the code-unit *indexing structure* of the
// loop, so a copy per encoding is genuinely different code, while the other
// three are cheap, well-predicted branches whose monomorphization fan-out was a
// measurable slice of the crate's .text.
//
// `text` is raw bytes for every encoding, including Utf16 — bun passes
// `bytemuck::cast_slice(&[u16])` (:2652) and indexes little-endian pairs, so the
// caller keeps a flat byte buffer and this stays one signature.
//
// Real-bun cross-check for the Utf16 path (which is what the TODO at :1035-1037
// describes): `'😀'` prints as `"😀"` — the astral code point is NOT
// recombined, each surrogate code unit escapes independently. That holds
// regardless of ascii_only (verified on target:"bun" and target:"browser"),
// which is exactly what per-code-unit iteration predicts.
// ─────────────────────────────────────────────────────────────────────────────
template <Encoding ENCODING, ByteSink W>
void write_pre_quoted_string_inner(
    std::string_view text, W& writer, std::uint8_t quoteChar, bool asciiOnly, bool json) {
    // Precondition, ref :984 — `debug_assert!(!(json && quote_char != b'"'))`:
    // `json` implies `quote_char == '"'`. Not enforced here (no assert macro
    // under `import std`, and this is the hot loop); both entry points that pass
    // `json = true` — `quote_for_json` and `write_json_string` — hardcode '"'.

    std::size_t i { 0 };
    const std::size_t n { ENCODING == Encoding::Utf16 ? text.size() / 2 : text.size() };

    // ref :998 — `code_unit_at!`.
    const auto code_unit_at { [&](std::size_t idx) -> std::int32_t {
        if constexpr (ENCODING == Encoding::Utf16) {
            const std::uint8_t lo { static_cast<std::uint8_t>(text[idx * 2]) };
            const std::uint8_t hi { static_cast<std::uint8_t>(text[idx * 2 + 1]) };
            return static_cast<std::int32_t>(
                static_cast<std::uint16_t>(static_cast<std::uint16_t>(lo)
                    | static_cast<std::uint16_t>(static_cast<std::uint16_t>(hi) << 8)));
        } else {
            return static_cast<std::int32_t>(static_cast<std::uint8_t>(text[idx]));
        }
    } };

    while (i < n) {
        // ref :1012
        std::uint8_t width { 1 };
        if constexpr (ENCODING == Encoding::Utf8) {
            width = wtf8_byte_sequence_length(static_cast<std::uint8_t>(text[i]));
        }
        const std::size_t clampedWidth { std::min<std::size_t>(width, n - i) };

        // ref :1018
        std::int32_t c {};
        if constexpr (ENCODING == Encoding::Utf8) {
            // Deviation 2 (see file header): `clampedWidth` bounds the read.
            std::array<std::uint8_t, 4> bytes {};
            for (std::size_t k { 0 }; k < clampedWidth; ++k) {
                bytes[k] = static_cast<std::uint8_t>(text[i + k]);
            }
            c = decode_wtf8_rune(bytes, width, 0);
        } else {
            // Ascii (bun debug-asserts text[i] <= 0x7F at :1030), Latin1, Utf16.
            c = code_unit_at(i);
        }

        // ref :1042 — printable fast path.
        if (can_print_without_escape(c, asciiOnly)) {
            if constexpr (ENCODING == Encoding::Ascii || ENCODING == Encoding::Utf8) {
                // Bulk-copy this unit plus everything up to the next escape.
                const std::string_view remain { text.substr(i + clampedWidth) };
                const std::size_t j { index_of_needs_escape_for_js_string(remain, quoteChar) };
                if (j != remain.size()) {
                    writer.write_all(text.substr(i, clampedWidth + j));
                    i += clampedWidth + j;
                } else {
                    writer.write_all(text.substr(i));
                    break;
                }
            } else {
                // Latin1 / Utf16 — re-encode the code unit as WTF-8.
                std::array<char, 4> codepointBytes {};
                const std::size_t codepointLen { encode_wtf8_rune(
                    codepointBytes, static_cast<std::uint32_t>(c)) };
                writer.write_all(std::string_view { codepointBytes.data(), codepointLen });
                i += clampedWidth;
            }
            continue;
        }

        // ref :1066 — escape arms.
        switch (c) {
        case 0x07:
            writer.write_all("\\x07");
            i += 1;
            break;
        case 0x08:
            writer.write_all("\\b");
            i += 1;
            break;
        case 0x0C:
            writer.write_all("\\f");
            i += 1;
            break;
        case 0x0A:
            // A template literal may carry a raw newline; every other quote must
            // escape it. This is why `best_quote_char_for_string` charges `\n`
            // against ' and " but not ` — real bun prints `'a\nb'` as a backtick
            // string containing a literal LF.
            writer.write_all(quoteChar == '`' ? "\n" : "\\n");
            i += 1;
            break;
        case 0x0D:
            writer.write_all("\\r");
            i += 1;
            break;
        case 0x0B:
            writer.write_all("\\v");
            i += 1;
            break;
        case 0x5C:
            writer.write_all("\\\\");
            i += 1;
            break;
        case 0x22:
            writer.write_all(quoteChar == '"' ? "\\\"" : "\"");
            i += 1;
            break;
        case 0x27:
            writer.write_all(quoteChar == '\'' ? "\\'" : "'");
            i += 1;
            break;
        case 0x60:
            writer.write_all(quoteChar == '`' ? "\\`" : "`");
            i += 1;
            break;
        case 0x24:
            // Only `${` opens a substitution, so a lone `$` never needs escaping.
            if (quoteChar == '`' && i + clampedWidth < n
                && code_unit_at(i + clampedWidth) == static_cast<std::int32_t>('{')) {
                writer.write_all("\\$");
            } else {
                writer.write_all("$");
            }
            i += 1;
            break;
        case 0x09:
            writer.write_all(quoteChar == '`' ? "\t" : "\\t");
            i += 1;
            break;
        default: {
            // ref :1150 — note this advances by `width`, not `clampedWidth`.
            i += width;
            if (c <= 0xFF && !json) {
                // `\xHH` is not valid JSON, hence the gate — JSON widens to \u00HH.
                const std::array<char, 2> h { hex2_upper(static_cast<std::uint8_t>(c)) };
                const std::array<char, 4> out { '\\', 'x', h[0], h[1] };
                writer.write_all(std::string_view { out.data(), out.size() });
            } else if (c <= 0xFFFF) {
                const std::array<char, 6> out { bmp_escape(static_cast<std::uint32_t>(c)) };
                writer.write_all(std::string_view { out.data(), out.size() });
            } else {
                const std::array<char, 12> out { surrogate_pair_escape(static_cast<std::uint32_t>(c)) };
                writer.write_all(std::string_view { out.data(), out.size() });
            }
            break;
        }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// write_pre_quoted_string — ref lib.rs:952
//
// Const-generic facade over the loop above, kept for source-stable call sites.
// Forwards its compile-time quote/ascii/json to the runtime parameters so the
// loop is monomorphized once per (W, ENCODING) rather than once per
// (W, QUOTE_CHAR, ASCII_ONLY, JSON, ENCODING).
// ─────────────────────────────────────────────────────────────────────────────
template <std::uint8_t QUOTE_CHAR, bool ASCII_ONLY, bool JSON, Encoding ENCODING, ByteSink W>
void write_pre_quoted_string(std::string_view text, W& writer) {
    write_pre_quoted_string_inner<ENCODING, W>(text, writer, QUOTE_CHAR, ASCII_ONLY, JSON);
}

// ─────────────────────────────────────────────────────────────────────────────
// quote_for_json — ref lib.rs:1167
//
// bun reserves `len + len/8 + 8` rather than calling `estimate_length_for_utf8`:
// the estimate would repeat the whole scan + rune decode just to size the buffer.
// The 12.5% slack (not 6.25%) is deliberate — tab-indented JS runs ~9.4% escaped
// bytes, and undershooting forces a 2x doubling memcpy of the entire source.
// ─────────────────────────────────────────────────────────────────────────────
inline void quote_for_json(std::string_view text, std::string& bytes, bool asciiOnly) {
    bytes.reserve(bytes.size() + text.size() + (text.size() >> 3) + 8);
    bytes.push_back('"');
    StringSink sink { bytes };
    write_pre_quoted_string_inner<Encoding::Utf8>(text, sink, '"', asciiOnly, true);
    bytes.push_back('"');
}

// ─────────────────────────────────────────────────────────────────────────────
// write_json_string — ref lib.rs:1189. Note `ascii_only = false` is hardcoded:
// JSON is transported as UTF-8, so only the structurally-required escapes apply.
// ─────────────────────────────────────────────────────────────────────────────
template <Encoding ENCODING, ByteSink W>
void write_json_string(std::string_view input, W& writer) {
    writer.write_all("\"");
    write_pre_quoted_string_inner<ENCODING, W>(input, writer, '"', false, true);
    writer.write_all("\"");
}

}  // namespace mbun::js_printer
