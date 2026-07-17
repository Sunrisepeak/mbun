// ascii_only.cppm — ASCII-only output pass for `--target=bun` bundles.
//
// ref: bun-ref/src/js_printer/lib.rs:8019 — `is_bun_platform = ascii_only` for
//      printAst: target=bun turns the printer's ASCII_ONLY const generic on, so
//      every token printer escapes non-ASCII on the way out.
// ref: bun-ref/src/js_printer/lib.rs:6838 `print_identifier` → :6846
//      `print_identifier_ascii_only` — identifiers escape as `\u{...}` (lowercase
//      hex, unpadded: `format_args!("{:x}", …)` at :6875), with an all-ASCII fast
//      path at :6853.
// ref: bun-ref/src/js_printer/lib.rs:1152-1160 `write_pre_quoted_string_inner` —
//      string escapes are `\xHH` (cp <= 0xFF), `\uHHHH` (cp <= 0xFFFF), else a
//      `\uHHHH\uHHHH` surrogate pair; uppercase hex
//      (bun-ref/src/bun_core/string/mod.rs:2419 `bmp_escape`, :2427
//      `surrogate_pair_escape`, :1286 `encode_surrogate_pair` = [lead, trail]).
// ref: bun-ref/src/js_printer/lib.rs:792 `can_print_without_escape` — the
//      cp > LAST_ASCII arm is gated on `!ascii_only`.
//
// WHY a token pass and not an AST printer: mbun does not transpile by
// AST-rebuild. `mbun.js_parser::transpile` emits the ORIGINAL source bytes plus
// recorded edits (Arena::erase_slice — see the contract comment on transpile()),
// which is what lets the bundler pass through JavaScript that the AST subset
// does not model. There is therefore no AST→source printer anywhere in the
// bundler's path to hang bun's ASCII_ONLY const generic off of.
//
// This pass is the erasure-architecture equivalent of that const generic, and it
// is token-accurate rather than textual: it re-lexes the emitted bundle with the
// real mbun.js_lexer — the same tokenizer mbun.js_parser drives, including the
// regex-vs-division and template-continuation re-scan protocol mirrored from
// js_parser.cppm:284 `regex_allowed_after_` / :330-385 `pre_lex_` — and rewrites
// only the two token kinds that can carry non-ASCII with meaning-preserving
// escapes. Every other byte, trivia included, is copied through verbatim.
//
// Known gaps (deliberate — each would trade correctness for ASCII coverage):
//   - Template literals are passed through verbatim. Escaping a template's
//     cooked text rewrites its `.raw`, which is observable via String.raw and
//     tagged templates. bun escapes them anyway and pays for it: its own suite
//     skips "template literal raw property with unicode in an ascii-only build"
//     (bun-ref/test/regression/issue/14976/14976.test.ts). Passing through keeps
//     `.raw` exact at the cost of leaving non-ASCII in template text.
//   - Regex literal bodies are passed through verbatim: `\u{...}` inside a
//     pattern only means a code point under the `u`/`v` flag, so escaping an
//     unflagged pattern would silently change what it matches.
//   - Comment text is trivia and has no escape syntax at all; bun's printer
//     never re-emits comments, mbun's erasure transpiler preserves them.
export module mbun.bundler.ascii_only;

import std;
import mbun.js_lexer;

namespace mbun::bundler::ascii_detail {

using mbun::js_lexer::Lexer;
using mbun::js_lexer::Token;

inline constexpr char HEX_UPPER[]{"0123456789ABCDEF"};
inline constexpr char HEX_LOWER[]{"0123456789abcdef"};

// ref: bun-ref/src/js_printer/lib.rs:6853 `strings::is_all_ascii` fast path.
bool is_all_ascii(std::string_view text) {
    for (unsigned char c : text) {
        if (c >= 0x80) {
            return false;
        }
    }
    return true;
}

struct Decoded {
    char32_t cp{0};
    std::size_t width{1};
};

// Decode the UTF-8 sequence starting at `text[i]`. Malformed input decodes as the
// single raw byte so this pass degrades to "copy through" instead of corrupting.
Decoded decode_utf8(std::string_view text, std::size_t i) {
    const unsigned char b0{static_cast<unsigned char>(text[i])};
    if (b0 < 0x80) {
        return {b0, 1};
    }
    auto cont = [&](std::size_t k) -> int {
        if (i + k >= text.size()) {
            return -1;
        }
        const unsigned char b{static_cast<unsigned char>(text[i + k])};
        return (b & 0xC0) == 0x80 ? static_cast<int>(b & 0x3F) : -1;
    };
    if ((b0 & 0xE0) == 0xC0) {
        const int c1{cont(1)};
        if (c1 < 0) {
            return {b0, 1};
        }
        return {static_cast<char32_t>(((b0 & 0x1F) << 6) | c1), 2};
    }
    if ((b0 & 0xF0) == 0xE0) {
        const int c1{cont(1)};
        const int c2{cont(2)};
        if (c1 < 0 || c2 < 0) {
            return {b0, 1};
        }
        return {static_cast<char32_t>(((b0 & 0x0F) << 12) | (c1 << 6) | c2), 3};
    }
    if ((b0 & 0xF8) == 0xF0) {
        const int c1{cont(1)};
        const int c2{cont(2)};
        const int c3{cont(3)};
        if (c1 < 0 || c2 < 0 || c3 < 0) {
            return {b0, 1};
        }
        return {static_cast<char32_t>(((b0 & 0x07) << 18) | (c1 << 12) | (c2 << 6) | c3), 4};
    }
    return {b0, 1};
}

void append_hex4_upper(std::string& out, std::uint32_t v) {
    out.push_back(HEX_UPPER[(v >> 12) & 0xF]);
    out.push_back(HEX_UPPER[(v >> 8) & 0xF]);
    out.push_back(HEX_UPPER[(v >> 4) & 0xF]);
    out.push_back(HEX_UPPER[v & 0xF]);
}

// ref: bun-ref/src/js_printer/lib.rs:1152-1160 — `\xHH` / `\uHHHH` / surrogate
// pair, uppercase hex. Valid inside a string literal under any quote style.
void append_string_escape(std::string& out, char32_t cp) {
    const std::uint32_t c{static_cast<std::uint32_t>(cp)};
    if (c <= 0xFF) {
        out += "\\x";
        out.push_back(HEX_UPPER[(c >> 4) & 0xF]);
        out.push_back(HEX_UPPER[c & 0xF]);
        return;
    }
    if (c <= 0xFFFF) {
        out += "\\u";
        append_hex4_upper(out, c);
        return;
    }
    // ref: bun-ref/src/bun_core/lib.rs:1286 encode_surrogate_pair = [lead, trail].
    const std::uint32_t v{c - 0x10000};
    out += "\\u";
    append_hex4_upper(out, 0xD800 + (v >> 10));
    out += "\\u";
    append_hex4_upper(out, 0xDC00 + (v & 0x3FF));
}

// ref: bun-ref/src/js_printer/lib.rs:6871-6877 — `\u{` + lowercase unpadded hex
// + `}`. Legal in both the start and continue position of an identifier.
void append_identifier_escape(std::string& out, char32_t cp) {
    out += "\\u{";
    std::uint32_t c{static_cast<std::uint32_t>(cp)};
    char buf[8];
    int n{0};
    do {
        buf[n++] = HEX_LOWER[c & 0xF];
        c >>= 4;
    } while (c != 0);
    while (n > 0) {
        out.push_back(buf[--n]);
    }
    out.push_back('}');
}

// An identifier's only escape syntax is `\uHHHH` / `\u{...}`, both pure ASCII, so
// a non-ASCII byte in the raw text is always a literal code point — no escape
// state to track. ref: bun-ref/src/js_printer/lib.rs:6846.
void put_identifier(std::string& out, std::string_view raw) {
    if (is_all_ascii(raw)) {
        out += raw;
        return;
    }
    std::size_t i{0};
    while (i < raw.size()) {
        const unsigned char b{static_cast<unsigned char>(raw[i])};
        if (b < 0x80) {
            out.push_back(static_cast<char>(b));
            ++i;
            continue;
        }
        const Decoded d{decode_utf8(raw, i)};
        append_identifier_escape(out, d.cp);
        i += d.width;
    }
}

// Rewrite a string literal's raw text (quotes included). Existing escape
// sequences are already ASCII and are copied through byte-for-byte — this pass
// never decodes and re-quotes, so quote style and every `\xHH` / `\uHHHH` /
// `\u{...}` / octal escape survive exactly as written.
void put_string(std::string& out, std::string_view raw) {
    if (is_all_ascii(raw)) {
        out += raw;
        return;
    }
    std::size_t i{0};
    while (i < raw.size()) {
        const unsigned char b{static_cast<unsigned char>(raw[i])};
        if (b == '\\' && i + 1 < raw.size()) {
            const unsigned char e{static_cast<unsigned char>(raw[i + 1])};
            if (e < 0x80) {
                // An ASCII escape sequence: copy `\` + the selector byte and let
                // the remainder (hex digits, `{...}`, a line continuation's
                // newline) fall through as ordinary ASCII on later iterations.
                out.push_back('\\');
                out.push_back(static_cast<char>(e));
                i += 2;
                continue;
            }
            const Decoded d{decode_utf8(raw, i + 1)};
            // LineContinuation :: \ LineTerminatorSequence, and U+2028/U+2029 are
            // LineTerminators — the sequence contributes nothing to the string
            // value, so it must be dropped, not escaped.
            if (d.cp != 0x2028 && d.cp != 0x2029) {
                // Any other `\<non-ASCII>` is an identity escape denoting the code
                // point itself; the whole pair collapses to one escape.
                append_string_escape(out, d.cp);
            }
            i += 1 + d.width;
            continue;
        }
        if (b < 0x80) {
            out.push_back(static_cast<char>(b));
            ++i;
            continue;
        }
        const Decoded d{decode_utf8(raw, i)};
        append_string_escape(out, d.cp);
        i += d.width;
    }
}

// Whether a `/` here starts a regex rather than division.
// ref: mbun modules/js/src/js_parser.cppm:284 `regex_allowed_after_` (kept in
// sync with it; the parser's copy is private to its detail namespace).
bool regex_allowed_after(Token prev) {
    switch (prev) {
    case Token::Identifier:
    case Token::EscapedKeyword:
    case Token::PrivateIdentifier:
    case Token::NumericLiteral:
    case Token::BigIntegerLiteral:
    case Token::StringLiteral:
    case Token::RegExpLiteral:
    case Token::NoSubstitutionTemplateLiteral:
    case Token::TemplateTail:
    case Token::CloseParen:
    case Token::CloseBracket:
    case Token::This:
    case Token::Super:
    case Token::True:
    case Token::False:
    case Token::Null:
    case Token::PlusPlus:
    case Token::MinusMinus:
        return false;  // value → `/` is division
    default:
        return true;  // operator / keyword / punctuator / start → regex
    }
}

}  // namespace mbun::bundler::ascii_detail

export namespace mbun::bundler {

// Rewrite `js` so every byte is < 0x80, escaping non-ASCII identifiers and string
// literals. Meaning-preserving: the result parses to the same program.
//
// On a lex error the input is returned unchanged — a bundle we cannot tokenize is
// one we cannot safely rewrite, and emitting correct non-ASCII beats emitting
// corrupted ASCII.
std::string escape_ascii_only(std::string_view js) {
    using namespace mbun::bundler::ascii_detail;
    if (is_all_ascii(js)) {
        return std::string{js};
    }
    std::string out;
    out.reserve(js.size() + js.size() / 8 + 16);

    Lexer lexer{js};
    std::vector<char> braceStack;     // 't' = template substitution, '{' = block
    Token prevSig{Token::EndOfFile};  // start of input → a regex may begin here
    std::size_t last{0};

    while (true) {
        auto step{lexer.next()};
        if (!step) {
            return std::string{js};
        }
        Token t{lexer.token()};
        if (t == Token::EndOfFile) {
            break;
        }
        // Regex vs division, then template continuation — mirrors
        // js_parser.cppm:358-385 `pre_lex_`. Without the `}` re-scan a template's
        // tail would be mis-lexed as ordinary tokens and rewritten wrongly.
        if ((t == Token::Slash || t == Token::SlashEquals) && regex_allowed_after(prevSig)) {
            if (!lexer.scan_regexp()) {
                return std::string{js};
            }
            t = lexer.token();
        }
        if (t == Token::OpenBrace) {
            braceStack.push_back('{');
        } else if (t == Token::CloseBrace) {
            if (!braceStack.empty() && braceStack.back() == 't') {
                braceStack.pop_back();
                if (!lexer.rescan_close_brace_as_template_token()) {
                    return std::string{js};
                }
                t = lexer.token();
            } else if (!braceStack.empty()) {
                braceStack.pop_back();
            }
        }
        if (t == Token::TemplateHead || t == Token::TemplateMiddle) {
            braceStack.push_back('t');
        }

        const std::size_t start{lexer.start()};
        const std::size_t end{lexer.end()};
        out.append(js.substr(last, start - last));  // trivia, verbatim
        const std::string_view raw{js.substr(start, end - start)};
        switch (t) {
        case Token::Identifier:
        case Token::EscapedKeyword:
        case Token::PrivateIdentifier:
            put_identifier(out, raw);
            break;
        case Token::StringLiteral:
            put_string(out, raw);
            break;
        default:
            out.append(raw);
            break;
        }
        last = end;
        prevSig = t;
    }
    out.append(js.substr(last));
    return out;
}

}  // namespace mbun::bundler
