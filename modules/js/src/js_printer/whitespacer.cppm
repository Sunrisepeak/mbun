// src/js_printer/whitespacer.cppm — module mbun.js_printer.whitespacer
//
// CAP-BUILD-PRINTER shard 1/4 — the minify-whitespace token pair.
//
// Blueprint (.mbun/bun-ref/src/js_printer/lib.rs):
//   :809/:810  INDENTATION_SPACE_BUF / INDENTATION_TAB_BUF
//   :854       `struct Whitespacer`
//   :866/:879  `_ws_minify_len` / `_ws_minify`
//   :895       `macro_rules! ws!`
//
// A `Whitespacer` is a token that carries both spellings of a fixed piece of
// syntax — `" = "` normally, `"="` when minifying — so the printer picks one
// with a single predictable branch (`print_whitespacer`, :2603) instead of
// interleaving `print_space()` calls.
//
// ── Note on the missing `append` ─────────────────────────────────────────────
// bun's `Whitespacer` has no `append`, and :859-863 explains why: Rust `const
// fn` cannot concatenate two `&'static [u8]` without `const_format::concatcp!`
// at the call site, and a runtime stub would silently emit wrong bytes — so
// bun's callers hand-inline the concatenation (see SExportStar). The hazard does
// not exist here: `ws<"...">` is a variable template over a string literal, so a
// caller that wants the concatenation just spells the joined literal
// (`ws<"} from ">`) and gets the same compile-time object. No `append` needed,
// and nothing to get silently wrong.
export module mbun.js_printer.whitespacer;

import std;

export namespace mbun::js_printer {

// ref lib.rs:809/:810 — pre-sized indentation runs so `print_indent` emits a
// level in one `write_all` instead of a loop.
inline constexpr std::array<char, 128> INDENTATION_SPACE_BUF { []() {
    std::array<char, 128> buf {};
    buf.fill(' ');
    return buf;
}() };

inline constexpr std::array<char, 128> INDENTATION_TAB_BUF { []() {
    std::array<char, 128> buf {};
    buf.fill('\t');
    return buf;
}() };

// ref lib.rs:854. Both views point at objects with static storage duration.
struct Whitespacer {
    std::string_view normal;
    std::string_view minify;
};

namespace detail {

// Structural type so a string literal can be a template argument. `N` includes
// the NUL terminator, matching the `const char (&)[N]` deduction.
template <std::size_t N>
struct WsLiteral {
    std::array<char, N> data {};

    consteval WsLiteral(const char (&s)[N]) {
        for (std::size_t i { 0 }; i < N; ++i) {
            data[i] = s[i];
        }
    }

    [[nodiscard]] constexpr std::string_view view() const { return { data.data(), N - 1 }; }
};

template <std::size_t N>
WsLiteral(const char (&)[N]) -> WsLiteral<N>;

// ref lib.rs:866 — `_ws_minify_len`: the minified spelling is the source with
// every space removed. (Only spaces; tabs/newlines in a Whitespacer would
// survive, which is why no caller puts them there.)
constexpr std::size_t ws_minify_len(std::string_view s) {
    std::size_t n { 0 };
    for (const char c : s) {
        if (c != ' ') {
            n += 1;
        }
    }
    return n;
}

// ref lib.rs:879 — `_ws_minify`.
template <std::size_t K>
constexpr std::array<char, K> ws_minify(std::string_view s) {
    std::array<char, K> out {};
    std::size_t j { 0 };
    for (const char c : s) {
        if (c != ' ') {
            out[j] = c;
            j += 1;
        }
    }
    return out;
}

// Static storage for each distinct minified spelling. One object per literal.
template <WsLiteral S>
inline constexpr auto WS_MINIFIED { ws_minify<ws_minify_len(S.view())>(S.view()) };

}  // namespace detail

// ref lib.rs:895 — the `ws!` macro, as a variable template. `ws<" = ">` yields
// `{ normal: " = ", minify: "=" }`, both computed at compile time.
template <detail::WsLiteral S>
inline constexpr Whitespacer ws {
    S.view(),
    std::string_view { detail::WS_MINIFIED<S>.data(), detail::WS_MINIFIED<S>.size() },
};

}  // namespace mbun::js_printer
