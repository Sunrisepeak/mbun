// mbun.css.tokenizer — zero-copy CSS tokenizer (CSS Syntax Level 3).
//
// Ref: Bun Rust src/css/css_parser.rs `Tokenizer` (which is a port of
// lightningcss / cssparser), Zig cross-check src/css/css_parser.zig. Tokens
// carry zero-copy [start,end) source spans (no per-token heap); the whole
// stylesheet is tokenized in a single forward pass.
//
// This is the single public token producer for the module: the transform
// engine (mbun.css) consumes `tokenize()` directly rather than carrying its own
// scanner. Numeric payloads (integer vs number, sign), IdHash vs
// UnrestrictedHash, and source locations remain DEFERRED (see PORT_STATUS.md).
export module mbun.css.tokenizer;

import std;

namespace mbun::css {

using std::size_t;

export enum class CssTokenKind : unsigned char {
    Ident,
    Function,   // text includes the trailing '(' (e.g. "calc(")
    AtKeyword,  // text includes leading '@'
    Hash,       // text includes leading '#'
    String,     // text includes quotes
    BadString,
    Url,  // whole url(...) with unquoted body
    BadUrl,
    Delim,
    Number,
    Percentage,
    Dimension,
    Whitespace,
    Comment,  // text includes /* */
    Cdo,      // <!--
    Cdc,      // -->
    IncludeMatch,    // ~=
    DashMatch,       // |=
    PrefixMatch,     // ^=
    SuffixMatch,     // $=
    SubstringMatch,  // *=
    Colon,
    Semicolon,
    Comma,
    OpenSquare,
    CloseSquare,
    OpenParen,
    CloseParen,
    OpenCurly,
    CloseCurly,
};

export struct TokenSpan {
    std::size_t start{};
    std::size_t end{};

    constexpr bool empty() const { return start == end; }
    constexpr std::size_t size() const { return end - start; }
    constexpr std::string_view view(std::string_view source) const {
        return source.substr(start, size());
    }
};

export struct CssToken {
    CssTokenKind kind{};
    TokenSpan span{};
    char delim{};  // valid when kind == Delim

    constexpr bool is_whitespace() const {
        return kind == CssTokenKind::Whitespace || kind == CssTokenKind::Comment;
    }

    constexpr bool is_block_open() const {
        return kind == CssTokenKind::Function || kind == CssTokenKind::OpenParen ||
               kind == CssTokenKind::OpenSquare || kind == CssTokenKind::OpenCurly;
    }

    constexpr bool is_block_close() const {
        return kind == CssTokenKind::CloseParen || kind == CssTokenKind::CloseSquare ||
               kind == CssTokenKind::CloseCurly;
    }

    constexpr std::string_view text(std::string_view source) const { return span.view(source); }
};

// ─── Character predicates (ASCII fast paths; non-ASCII is name-continuation) ─
export constexpr bool css_is_whitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}
export constexpr bool css_is_digit(char c) { return c >= '0' && c <= '9'; }
constexpr bool is_hex(char c) {
    return css_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
constexpr bool is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
constexpr bool is_name_start(unsigned char c) {
    return is_letter(static_cast<char>(c)) || c == '_' || c >= 0x80;
}
constexpr bool is_name(unsigned char c) {
    return is_name_start(c) || css_is_digit(static_cast<char>(c)) || c == '-';
}

// ─── Tokenizer state machine ─────────────────────────────────────────────────
class Tokenizer {
public:
    explicit Tokenizer(std::string_view src) : src_{src} {}

    std::vector<CssToken> tokenize() {
        std::vector<CssToken> out;
        out.reserve(src_.size() / 4 + 8);
        while (pos_ < src_.size()) out.push_back(next_());
        return out;
    }

private:
    std::string_view src_;
    size_t           pos_{0};

    char at(size_t i) const { return i < src_.size() ? src_[i] : '\0'; }
    char cur() const { return at(pos_); }
    char peek(size_t n = 1) const { return at(pos_ + n); }

    bool valid_escape(size_t i) const {
        return at(i) == '\\' && at(i + 1) != '\n' && at(i + 1) != '\0';
    }
    // Is `+d`, `.d`, or `d` a number at position i?
    bool starts_number(size_t i) const {
        char c0 = at(i);
        if (c0 == '+' || c0 == '-') {
            if (css_is_digit(at(i + 1))) return true;
            return at(i + 1) == '.' && css_is_digit(at(i + 2));
        }
        if (c0 == '.') return css_is_digit(at(i + 1));
        return css_is_digit(c0);
    }
    bool starts_ident(size_t i) const {
        char c0 = at(i);
        if (c0 == '-') {
            char c1 = at(i + 1);
            if (is_name_start(static_cast<unsigned char>(c1)) || c1 == '-') return true;
            return valid_escape(i + 1);
        }
        if (is_name_start(static_cast<unsigned char>(c0))) return true;
        return valid_escape(i);
    }

    CssToken make(CssTokenKind k, size_t start) const {
        return CssToken{k, TokenSpan{start, pos_}, 0};
    }
    CssToken delim(size_t start, char d) {
        pos_++;
        return CssToken{CssTokenKind::Delim, TokenSpan{start, pos_}, d};
    }

    void consume_escape() {
        // caller ensures a backslash at pos_
        pos_++;  // backslash
        if (is_hex(cur())) {
            int n = 0;
            while (n < 6 && is_hex(cur())) { pos_++; n++; }
            if (css_is_whitespace(cur())) pos_++;  // one trailing ws consumed
        } else if (cur() != '\0') {
            pos_++;
        }
    }

    void consume_name() {
        while (pos_ < src_.size()) {
            if (is_name(static_cast<unsigned char>(cur()))) {
                pos_++;
            } else if (valid_escape(pos_)) {
                consume_escape();
            } else {
                break;
            }
        }
    }

    CssToken consume_ident_like(size_t start) {
        consume_name();
        // url( ... ) special form
        if (cur() == '(') {
            std::string_view name = src_.substr(start, pos_ - start);
            pos_++;  // (
            if (name.size() == 3 && (name[0] == 'u' || name[0] == 'U') &&
                (name[1] == 'r' || name[1] == 'R') && (name[2] == 'l' || name[2] == 'L')) {
                // peek past whitespace: if quote → normal function, else unquoted url
                size_t p = pos_;
                while (p < src_.size() && css_is_whitespace(at(p))) p++;
                if (at(p) != '"' && at(p) != '\'') return consume_url(start);
            }
            return make(CssTokenKind::Function, start);
        }
        return make(CssTokenKind::Ident, start);
    }

    CssToken consume_url(size_t start) {
        // pos_ is just after '('
        while (css_is_whitespace(cur())) pos_++;
        while (pos_ < src_.size()) {
            char c = cur();
            if (c == ')') {
                pos_++;
                return make(CssTokenKind::Url, start);
            }
            if (css_is_whitespace(c)) {
                while (css_is_whitespace(cur())) pos_++;
                if (cur() == ')') {
                    pos_++;
                    return make(CssTokenKind::Url, start);
                }
                return consume_bad_url(start);
            }
            if (c == '"' || c == '\'' || c == '(') return consume_bad_url(start);
            if (c == '\\') {
                if (valid_escape(pos_)) {
                    consume_escape();
                    continue;
                }
                return consume_bad_url(start);
            }
            pos_++;
        }
        return make(CssTokenKind::Url, start);
    }

    CssToken consume_bad_url(size_t start) {
        while (pos_ < src_.size()) {
            char c = cur();
            if (c == ')') {
                pos_++;
                break;
            }
            if (valid_escape(pos_)) {
                consume_escape();
                continue;
            }
            pos_++;
        }
        return make(CssTokenKind::BadUrl, start);
    }

    CssToken consume_string(size_t start, char quote) {
        pos_++;  // opening quote
        while (pos_ < src_.size()) {
            char c = cur();
            if (c == quote) {
                pos_++;
                return make(CssTokenKind::String, start);
            }
            if (c == '\n') return make(CssTokenKind::BadString, start);
            if (c == '\\') {
                if (at(pos_ + 1) == '\n') {
                    pos_ += 2;
                    continue;
                }
                if (valid_escape(pos_)) {
                    consume_escape();
                    continue;
                }
                pos_++;
                continue;
            }
            pos_++;
        }
        return make(CssTokenKind::String, start);
    }

    CssToken consume_numeric(size_t start) {
        if (cur() == '+' || cur() == '-') pos_++;
        while (css_is_digit(cur())) pos_++;
        if (cur() == '.' && css_is_digit(peek())) {
            pos_ += 2;
            while (css_is_digit(cur())) pos_++;
        }
        if (cur() == 'e' || cur() == 'E') {
            size_t p = pos_ + 1;
            if (at(p) == '+' || at(p) == '-') p++;
            if (css_is_digit(at(p))) {
                pos_ = p;
                while (css_is_digit(cur())) pos_++;
            }
        }
        if (cur() == '%') {
            pos_++;
            return make(CssTokenKind::Percentage, start);
        }
        if (starts_ident(pos_)) {
            consume_name();
            return make(CssTokenKind::Dimension, start);
        }
        return make(CssTokenKind::Number, start);
    }

    CssToken next_() {
        size_t start = pos_;
        char   c     = cur();

        if (css_is_whitespace(c)) {
            while (css_is_whitespace(cur())) pos_++;
            return make(CssTokenKind::Whitespace, start);
        }
        if (c == '/' && peek() == '*') {
            pos_ += 2;
            while (pos_ < src_.size() && !(cur() == '*' && peek() == '/')) pos_++;
            if (pos_ < src_.size()) pos_ += 2;
            return make(CssTokenKind::Comment, start);
        }
        if (c == '"' || c == '\'') return consume_string(start, c);
        if (c == '#') {
            if (is_name(static_cast<unsigned char>(peek())) || valid_escape(pos_ + 1)) {
                pos_++;
                consume_name();
                return make(CssTokenKind::Hash, start);
            }
            return delim(start, '#');
        }
        if (c == '(') { pos_++; return make(CssTokenKind::OpenParen, start); }
        if (c == ')') { pos_++; return make(CssTokenKind::CloseParen, start); }
        if (c == '[') { pos_++; return make(CssTokenKind::OpenSquare, start); }
        if (c == ']') { pos_++; return make(CssTokenKind::CloseSquare, start); }
        if (c == '{') { pos_++; return make(CssTokenKind::OpenCurly, start); }
        if (c == '}') { pos_++; return make(CssTokenKind::CloseCurly, start); }
        if (c == ',') { pos_++; return make(CssTokenKind::Comma, start); }
        if (c == ':') { pos_++; return make(CssTokenKind::Colon, start); }
        if (c == ';') { pos_++; return make(CssTokenKind::Semicolon, start); }
        if (c == '+' || c == '.') {
            if (starts_number(pos_)) return consume_numeric(start);
            return delim(start, c);
        }
        if (c == '-') {
            if (starts_number(pos_)) return consume_numeric(start);
            if (peek() == '-' && peek(2) == '>') {
                pos_ += 3;
                return make(CssTokenKind::Cdc, start);
            }
            if (starts_ident(pos_)) return consume_ident_like(start);
            return delim(start, '-');
        }
        if (c == '<') {
            if (peek() == '!' && peek(2) == '-' && peek(3) == '-') {
                pos_ += 4;
                return make(CssTokenKind::Cdo, start);
            }
            return delim(start, '<');
        }
        if (c == '@') {
            if (starts_ident(pos_ + 1)) {
                pos_++;
                consume_name();
                return make(CssTokenKind::AtKeyword, start);
            }
            return delim(start, '@');
        }
        if (c == '\\') {
            if (valid_escape(pos_)) return consume_ident_like(start);
            return delim(start, '\\');
        }
        if (css_is_digit(c)) return consume_numeric(start);
        if (is_name_start(static_cast<unsigned char>(c))) return consume_ident_like(start);

        // match operators (only when directly followed by '=')
        auto op = [&](char second, CssTokenKind k) -> std::optional<CssToken> {
            if (peek() == '=' && c == second) {
                pos_ += 2;
                return make(k, start);
            }
            return std::nullopt;
        };
        if (c == '~') { if (auto t = op('~', CssTokenKind::IncludeMatch)) return *t; }
        if (c == '|') { if (auto t = op('|', CssTokenKind::DashMatch)) return *t; }
        if (c == '^') { if (auto t = op('^', CssTokenKind::PrefixMatch)) return *t; }
        if (c == '$') { if (auto t = op('$', CssTokenKind::SuffixMatch)) return *t; }
        if (c == '*') { if (auto t = op('*', CssTokenKind::SubstringMatch)) return *t; }

        return delim(start, c);
    }
};

// ─── Public API ──────────────────────────────────────────────────────────────
// Tokenize `source` into a flat vector of zero-copy tokens spanning the whole
// input (whitespace/comments preserved as tokens). This is the shared scanner
// consumed by the transform engine and available to any consumer that needs
// token-level access to CSS.
export std::vector<CssToken> tokenize(std::string_view source) {
    return Tokenizer{source}.tokenize();
}

}  // namespace mbun::css
