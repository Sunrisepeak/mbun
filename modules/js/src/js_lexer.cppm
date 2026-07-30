// src/js_lexer/js_lexer.cppm — module mbun.js_lexer
//
// JS/TS lexer (T2.3). Streaming, single-pass, byte-level scan with an ASCII
// fast path and a UTF-8 slow path. Not a mechanical port of bun's Zig/Rust
// lexer: the token model and error/diagnostic split match bun's observable
// behavior (the T2.3 test vectors are the spec), but the implementation is a
// self-contained C++26 design.
//
// Unicode identifier/whitespace classification is deliberately approximate
// (ASCII exact; a compact rule set for the non-ASCII ranges the test suite
// exercises) pending full Unicode tables alongside T1.2 (core.strings).
export module mbun.js_lexer;

import std;

export namespace mbun::js_lexer {

enum class Token : std::uint8_t {
    EndOfFile,
    SyntaxError,

    // literals / names
    Identifier,
    EscapedKeyword,
    PrivateIdentifier,
    NumericLiteral,
    BigIntegerLiteral,
    StringLiteral,
    RegExpLiteral,
    NoSubstitutionTemplateLiteral,
    TemplateHead,
    TemplateMiddle,
    TemplateTail,
    Hashbang,

    // punctuation
    Ampersand,
    AmpersandAmpersand,
    AmpersandAmpersandEquals,
    AmpersandEquals,
    Asterisk,
    AsteriskAsterisk,
    AsteriskAsteriskEquals,
    AsteriskEquals,
    At,
    Bar,
    BarBar,
    BarBarEquals,
    BarEquals,
    Caret,
    CaretEquals,
    CloseBrace,
    CloseBracket,
    CloseParen,
    Colon,
    Comma,
    Dot,
    DotDotDot,
    Equals,
    EqualsEquals,
    EqualsEqualsEquals,
    EqualsGreaterThan,
    Exclamation,
    ExclamationEquals,
    ExclamationEqualsEquals,
    GreaterThan,
    GreaterThanEquals,
    GreaterThanGreaterThan,
    GreaterThanGreaterThanEquals,
    GreaterThanGreaterThanGreaterThan,
    GreaterThanGreaterThanGreaterThanEquals,
    LessThan,
    LessThanEquals,
    LessThanLessThan,
    LessThanLessThanEquals,
    Minus,
    MinusMinus,
    MinusEquals,
    OpenBrace,
    OpenBracket,
    OpenParen,
    Percent,
    PercentEquals,
    Plus,
    PlusPlus,
    PlusEquals,
    Question,
    QuestionDot,
    QuestionQuestion,
    QuestionQuestionEquals,
    Semicolon,
    Slash,
    SlashEquals,
    Tilde,

    // keywords
    Break,
    Case,
    Catch,
    Class,
    Const,
    Continue,
    Debugger,
    Default,
    Delete,
    Do,
    Else,
    Enum,
    Export,
    Extends,
    False,
    Finally,
    For,
    Function,
    If,
    Import,
    In,
    Instanceof,
    New,
    Null,
    Return,
    Super,
    Switch,
    This,
    Throw,
    True,
    Try,
    Typeof,
    Var,
    Void,
    While,
    With,
};

std::string_view token_to_string(Token t) {
    switch (t) {
    case Token::EndOfFile: return "EndOfFile";
    case Token::SyntaxError: return "SyntaxError";
    case Token::Identifier: return "Identifier";
    case Token::EscapedKeyword: return "EscapedKeyword";
    case Token::PrivateIdentifier: return "PrivateIdentifier";
    case Token::NumericLiteral: return "NumericLiteral";
    case Token::BigIntegerLiteral: return "BigIntegerLiteral";
    case Token::StringLiteral: return "StringLiteral";
    case Token::NoSubstitutionTemplateLiteral: return "NoSubstitutionTemplateLiteral";
    case Token::TemplateHead: return "TemplateHead";
    case Token::TemplateMiddle: return "TemplateMiddle";
    case Token::TemplateTail: return "TemplateTail";
    case Token::Hashbang: return "Hashbang";
    case Token::Ampersand: return "&";
    case Token::AmpersandAmpersand: return "&&";
    case Token::AmpersandAmpersandEquals: return "&&=";
    case Token::AmpersandEquals: return "&=";
    case Token::Asterisk: return "*";
    case Token::AsteriskAsterisk: return "**";
    case Token::AsteriskAsteriskEquals: return "**=";
    case Token::AsteriskEquals: return "*=";
    case Token::At: return "@";
    case Token::Bar: return "|";
    case Token::BarBar: return "||";
    case Token::BarBarEquals: return "||=";
    case Token::BarEquals: return "|=";
    case Token::Caret: return "^";
    case Token::CaretEquals: return "^=";
    case Token::CloseBrace: return "}";
    case Token::CloseBracket: return "]";
    case Token::CloseParen: return ")";
    case Token::Colon: return ":";
    case Token::Comma: return ",";
    case Token::Dot: return ".";
    case Token::DotDotDot: return "...";
    case Token::Equals: return "=";
    case Token::EqualsEquals: return "==";
    case Token::EqualsEqualsEquals: return "===";
    case Token::EqualsGreaterThan: return "=>";
    case Token::Exclamation: return "!";
    case Token::ExclamationEquals: return "!=";
    case Token::ExclamationEqualsEquals: return "!==";
    case Token::GreaterThan: return ">";
    case Token::GreaterThanEquals: return ">=";
    case Token::GreaterThanGreaterThan: return ">>";
    case Token::GreaterThanGreaterThanEquals: return ">>=";
    case Token::GreaterThanGreaterThanGreaterThan: return ">>>";
    case Token::GreaterThanGreaterThanGreaterThanEquals: return ">>>=";
    case Token::LessThan: return "<";
    case Token::LessThanEquals: return "<=";
    case Token::LessThanLessThan: return "<<";
    case Token::LessThanLessThanEquals: return "<<=";
    case Token::Minus: return "-";
    case Token::MinusMinus: return "--";
    case Token::MinusEquals: return "-=";
    case Token::OpenBrace: return "{";
    case Token::OpenBracket: return "[";
    case Token::OpenParen: return "(";
    case Token::Percent: return "%";
    case Token::PercentEquals: return "%=";
    case Token::Plus: return "+";
    case Token::PlusPlus: return "++";
    case Token::PlusEquals: return "+=";
    case Token::Question: return "?";
    case Token::QuestionDot: return "?.";
    case Token::QuestionQuestion: return "??";
    case Token::QuestionQuestionEquals: return "?\?=";
    case Token::Semicolon: return ";";
    case Token::Slash: return "/";
    case Token::SlashEquals: return "/=";
    case Token::Tilde: return "~";
    case Token::Break: return "break";
    case Token::Case: return "case";
    case Token::Catch: return "catch";
    case Token::Class: return "class";
    case Token::Const: return "const";
    case Token::Continue: return "continue";
    case Token::Debugger: return "debugger";
    case Token::Default: return "default";
    case Token::Delete: return "delete";
    case Token::Do: return "do";
    case Token::Else: return "else";
    case Token::Enum: return "enum";
    case Token::Export: return "export";
    case Token::Extends: return "extends";
    case Token::False: return "false";
    case Token::Finally: return "finally";
    case Token::For: return "for";
    case Token::Function: return "function";
    case Token::If: return "if";
    case Token::Import: return "import";
    case Token::In: return "in";
    case Token::Instanceof: return "instanceof";
    case Token::New: return "new";
    case Token::Null: return "null";
    case Token::Return: return "return";
    case Token::Super: return "super";
    case Token::Switch: return "switch";
    case Token::This: return "this";
    case Token::Throw: return "throw";
    case Token::True: return "true";
    case Token::Try: return "try";
    case Token::Typeof: return "typeof";
    case Token::Var: return "var";
    case Token::Void: return "void";
    case Token::While: return "while";
    case Token::With: return "with";
    case Token::RegExpLiteral: return "regular expression";
    }
    return "?";
}

struct Diagnostic {
    std::string message;
    std::size_t offset{0};
    bool fatal{false};
};

struct LineCol {
    std::uint32_t line{1};
    std::uint32_t column{0};
};

class Lexer {
public:
    explicit Lexer(std::string_view src) : src_{src} {}

    // Advance to the next token. Returns the token on success, or unexpected on
    // a fatal lexical error (a diagnostic is also recorded in diagnostics()).
    // Non-fatal problems (e.g. legacy octal escape) record a diagnostic but
    // still return a token. An unrecognized character yields a SyntaxError
    // token (non-fatal) so the parser can report "Unexpected".
    std::expected<Token, Diagnostic> next() {
        newlineBefore_ = false;
        if (auto err = skip_trivia(); !err) {
            return std::unexpected(err.error());
        }
        start_ = pos_;
        if (pos_ >= src_.size()) {
            token_ = Token::EndOfFile;
            end_ = pos_;
            return token_;
        }
        return scan_token();
    }

    Token token() const { return token_; }
    std::size_t start() const { return start_; }
    std::size_t end() const { return end_; }

    // Reposition the scanning cursor to an absolute byte offset. Used by the
    // JSX-aware pre-lexer: after it hand-scans a whole JSX element from the raw
    // source (which the JS tokenizer cannot lex — JSX text and `</` would be
    // mis-scanned as identifiers / a regexp), it seeks the lexer just past the
    // element to resume normal tokenization.
    void seek(std::size_t p) {
        pos_ = p <= src_.size() ? p : src_.size();
        start_ = pos_;
        end_ = pos_;
    }

    bool has_newline_before() const { return newlineBefore_; }
    std::string_view raw() const { return src_.substr(start_, end_ - start_); }
    std::string_view identifier() const { return identBuf_; }
    double number() const { return number_; }
    const std::vector<Diagnostic>& diagnostics() const { return diags_; }

    std::optional<std::u16string> string_literal_utf16() const {
        if (!strValid_) {
            return std::nullopt;
        }
        return strBuf_;
    }

    std::string_view raw_template_contents() const { return rawTmplBuf_; }

    LineCol line_col(std::size_t offset) const {
        LineCol lc;
        lc.line = 1;
        std::size_t lineStart{0};
        std::size_t i{0};
        while (i < offset && i < src_.size()) {
            unsigned char c = static_cast<unsigned char>(src_[i]);
            if (c == '\n') {
                ++lc.line;
                ++i;
                lineStart = i;
            } else if (c == '\r') {
                ++lc.line;
                ++i;
                if (i < src_.size() && src_[i] == '\n') {
                    ++i;
                }
                lineStart = i;
            } else {
                ++i;
            }
        }
        // Column: count codepoints from the line start up to offset.
        std::uint32_t col{0};
        std::size_t j{lineStart};
        while (j < offset && j < src_.size()) {
            auto [cp, len] = decode_utf8(j);
            (void)cp;
            j += len;
            ++col;
        }
        lc.column = col;
        return lc;
    }

    // Re-scan the current '/' or '/=' token as a regular expression literal.
    // On success sets token()==... (kept as the caller expects via raw()), and
    // raw() spans the whole "/.../flags". Invalid/duplicate flags are non-fatal
    // diagnostics; a newline inside the body is fatal.
    std::expected<void, Diagnostic> scan_regexp() {
        // Rewind to just after the opening '/'.
        pos_ = start_ + 1;
        bool inClass{false};
        while (true) {
            if (pos_ >= src_.size()) {
                return fatal("Unterminated regular expression");
            }
            unsigned char c = static_cast<unsigned char>(src_[pos_]);
            if (c == '\n' || c == '\r') {
                return fatal("Unterminated regular expression");
            }
            if (c == '\\') {
                pos_ += 1;
                if (pos_ >= src_.size()) {
                    return fatal("Unterminated regular expression");
                }
                unsigned char n = static_cast<unsigned char>(src_[pos_]);
                if (n == '\n' || n == '\r') {
                    return fatal("Unterminated regular expression");
                }
                pos_ += 1;
                continue;
            }
            if (c == '[') {
                inClass = true;
            } else if (c == ']') {
                inClass = false;
            } else if (c == '/' && !inClass) {
                pos_ += 1;
                break;
            }
            pos_ += 1;
        }
        // Flags.
        bool seen[128]{};
        while (pos_ < src_.size()) {
            unsigned char c = static_cast<unsigned char>(src_[pos_]);
            if (!is_ascii_id_continue(c)) {
                break;
            }
            if (c >= 'a' && c <= 'z') {
                static constexpr std::string_view VALID{"dgimsuvy"};
                if (VALID.find(static_cast<char>(c)) == std::string_view::npos) {
                    warn(std::format("Invalid flag \"{}\" in regular expression",
                                     static_cast<char>(c)));
                } else if (seen[c]) {
                    warn(std::format("Duplicate flag \"{}\" in regular expression",
                                     static_cast<char>(c)));
                } else {
                    seen[c] = true;
                }
            } else {
                warn(std::format("Invalid flag \"{}\" in regular expression", static_cast<char>(c)));
            }
            pos_ += 1;
        }
        end_ = pos_;
        token_ = Token::RegExpLiteral;
        return {};
    }

    // After a CloseBrace token inside a template, re-scan it as the continuation
    // of the template literal (TemplateMiddle / TemplateTail).
    std::expected<void, Diagnostic> rescan_close_brace_as_template_token() {
        pos_ = start_;  // at the '}'
        auto r = scan_template('}');
        if (!r) {
            return std::unexpected(r.error());
        }
        return {};
    }

private:
    std::string_view src_;
    std::size_t pos_{0};
    std::size_t start_{0};
    std::size_t end_{0};
    Token token_{Token::EndOfFile};
    bool newlineBefore_{false};

    std::string identBuf_;
    std::u16string strBuf_;
    bool strValid_{false};
    std::string rawTmplBuf_;
    double number_{0.0};

    std::vector<Diagnostic> diags_;

    std::unexpected<Diagnostic> make_fatal(std::string msg) {
        Diagnostic d{std::move(msg), pos_, true};
        diags_.push_back(d);
        return std::unexpected(d);
    }
    std::expected<void, Diagnostic> fatal(std::string msg) {
        return make_fatal(std::move(msg));
    }
    std::expected<void, Diagnostic> fatal_at(std::string msg, std::size_t off) {
        Diagnostic d{std::move(msg), off, true};
        diags_.push_back(d);
        return std::unexpected(d);
    }
    void warn(std::string msg) { diags_.push_back(Diagnostic{std::move(msg), pos_, false}); }

    static bool is_ascii_id_start(unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
    }
    static bool is_ascii_id_continue(unsigned char c) {
        return is_ascii_id_start(c) || (c >= '0' && c <= '9');
    }

    // UTF-8 decode at byte i; returns (codepoint, byte-length). Invalid bytes
    // decode as U+FFFD with length 1.
    std::pair<char32_t, std::size_t> decode_utf8(std::size_t i) const {
        if (i >= src_.size()) {
            return {0xFFFD, 1};
        }
        unsigned char c0 = static_cast<unsigned char>(src_[i]);
        if (c0 < 0x80) {
            return {c0, 1};
        }
        auto cont = [&](std::size_t k) -> int {
            if (i + k >= src_.size()) {
                return -1;
            }
            unsigned char cc = static_cast<unsigned char>(src_[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return -1;
            }
            return cc & 0x3F;
        };
        if ((c0 & 0xE0) == 0xC0) {
            int b1 = cont(1);
            if (b1 < 0) {
                return {0xFFFD, 1};
            }
            char32_t cp = ((c0 & 0x1F) << 6) | b1;
            return {cp, 2};
        }
        if ((c0 & 0xF0) == 0xE0) {
            int b1 = cont(1), b2 = cont(2);
            if (b1 < 0 || b2 < 0) {
                return {0xFFFD, 1};
            }
            char32_t cp = ((c0 & 0x0F) << 12) | (b1 << 6) | b2;
            return {cp, 3};
        }
        if ((c0 & 0xF8) == 0xF0) {
            int b1 = cont(1), b2 = cont(2), b3 = cont(3);
            if (b1 < 0 || b2 < 0 || b3 < 0) {
                return {0xFFFD, 1};
            }
            char32_t cp = ((c0 & 0x07) << 18) | (b1 << 12) | (b2 << 6) | b3;
            return {cp, 4};
        }
        return {0xFFFD, 1};
    }

    static bool is_unicode_whitespace(char32_t cp) {
        switch (cp) {
        case 0x00A0:
        case 0x1680:
        case 0x2000:
        case 0x2001:
        case 0x2002:
        case 0x2003:
        case 0x2004:
        case 0x2005:
        case 0x2006:
        case 0x2007:
        case 0x2008:
        case 0x2009:
        case 0x200A:
        case 0x202F:
        case 0x205F:
        case 0x3000:
        case 0xFEFF:
            return true;
        default:
            return false;
        }
    }
    static bool is_unicode_newline(char32_t cp) { return cp == 0x2028 || cp == 0x2029; }

    // Approximate Unicode ID_Start for non-ASCII (see file header note).
    static bool is_unicode_id_start(char32_t cp) {
        if (cp < 0x80) {
            return is_ascii_id_start(static_cast<unsigned char>(cp));
        }
        if (is_unicode_whitespace(cp) || is_unicode_newline(cp)) {
            return false;
        }
        // C1 controls (U+0080..U+009F) are Cc, never ID_Start/ID_Continue. The
        // default-allow rule below let `class { W\u0081; }` lex as one
        // identifier and the invalid property name went unreported.
        // ref: regression 012039.
        if (cp <= 0x009F) {
            return false;
        }
        if (cp >= 0x00A1 && cp <= 0x00BF) {
            return cp == 0x00AA || cp == 0x00B5 || cp == 0x00BA;
        }
        if (cp == 0x00D7 || cp == 0x00F7) {
            return false;  // × ÷
        }
        return true;
    }
    static bool is_unicode_id_continue(char32_t cp) {
        if (cp < 0x80) {
            return is_ascii_id_continue(static_cast<unsigned char>(cp));
        }
        // Combining marks / ZWNJ / ZWJ commonly allowed as continue; approximated.
        if (cp == 0x200C || cp == 0x200D) {
            return true;
        }
        return is_unicode_id_start(cp);
    }

    // Skip whitespace and comments, tracking newlineBefore_. Fatal on an
    // unterminated block comment or legacy HTML open comment.
    std::expected<void, Diagnostic> skip_trivia() {
        while (pos_ < src_.size()) {
            unsigned char c = static_cast<unsigned char>(src_[pos_]);
            if (c == ' ' || c == '\t' || c == 0x0B || c == 0x0C) {
                pos_ += 1;
                continue;
            }
            if (c == '\n') {
                newlineBefore_ = true;
                pos_ += 1;
                continue;
            }
            if (c == '\r') {
                newlineBefore_ = true;
                pos_ += 1;
                if (pos_ < src_.size() && src_[pos_] == '\n') {
                    pos_ += 1;
                }
                continue;
            }
            if (c == '/' && pos_ + 1 < src_.size()) {
                unsigned char n = static_cast<unsigned char>(src_[pos_ + 1]);
                if (n == '/') {
                    pos_ += 2;
                    skip_line();
                    continue;
                }
                if (n == '*') {
                    pos_ += 2;
                    if (auto e = skip_block_comment(); !e) {
                        return e;
                    }
                    continue;
                }
            }
            if (c == '<' && starts_with(pos_, "<!--")) {
                return fatal_at("Legacy HTML comments not implemented yet!", pos_);
            }
            if (c == '-' && starts_with(pos_, "-->") && (newlineBefore_ || pos_ == 0)) {
                pos_ += 3;
                skip_line();
                continue;
            }
            if (c >= 0x80) {
                auto [cp, len] = decode_utf8(pos_);
                if (is_unicode_whitespace(cp)) {
                    pos_ += len;
                    continue;
                }
                if (is_unicode_newline(cp)) {
                    newlineBefore_ = true;
                    pos_ += len;
                    continue;
                }
            }
            break;
        }
        return {};
    }

    bool starts_with(std::size_t at, std::string_view s) const {
        return src_.substr(at).starts_with(s);
    }
    void skip_line() {
        while (pos_ < src_.size()) {
            unsigned char c = static_cast<unsigned char>(src_[pos_]);
            if (c == '\n' || c == '\r') {
                return;
            }
            if (c >= 0x80) {
                auto [cp, len] = decode_utf8(pos_);
                if (is_unicode_newline(cp)) {
                    return;
                }
                pos_ += len;
                continue;
            }
            pos_ += 1;
        }
    }
    std::expected<void, Diagnostic> skip_block_comment() {
        while (pos_ < src_.size()) {
            unsigned char c = static_cast<unsigned char>(src_[pos_]);
            if (c == '*' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '/') {
                pos_ += 2;
                return {};
            }
            if (c == '\n') {
                newlineBefore_ = true;
            } else if (c == '\r') {
                newlineBefore_ = true;
            } else if (c >= 0x80) {
                auto [cp, len] = decode_utf8(pos_);
                if (is_unicode_newline(cp)) {
                    newlineBefore_ = true;
                }
                pos_ += len;
                continue;
            }
            pos_ += 1;
        }
        return fatal("Expected \"*/\" to terminate multi-line comment");
    }

    // ── token scanning ──────────────────────────────────────────────────────

    std::expected<Token, Diagnostic> scan_token() {
        unsigned char c = static_cast<unsigned char>(src_[pos_]);

        // Hashbang only at offset 0.
        if (c == '#' && pos_ == 0 && pos_ + 1 < src_.size() && src_[pos_ + 1] == '!') {
            skip_line();
            end_ = pos_;
            identBuf_.assign(src_.substr(start_, end_ - start_));
            return set(Token::Hashbang);
        }
        if (c == '#') {
            return scan_private_identifier();
        }

        if (is_ascii_id_start(c)) {
            return scan_identifier();
        }
        if (c == '\\') {
            return scan_identifier();  // \u escape identifier
        }
        if (c >= '0' && c <= '9') {
            return scan_number();
        }
        if (c == '.') {
            if (pos_ + 1 < src_.size() && src_[pos_ + 1] >= '0' && src_[pos_ + 1] <= '9') {
                return scan_number();
            }
            if (starts_with(pos_, "...")) {
                return punct(3, Token::DotDotDot);
            }
            return punct(1, Token::Dot);
        }
        if (c == '"' || c == '\'') {
            return scan_string(c);
        }
        if (c == '`') {
            return scan_template('`');
        }

        if (c >= 0x80) {
            auto [cp, len] = decode_utf8(pos_);
            if (is_unicode_id_start(cp)) {
                return scan_identifier();
            }
            // Unknown character → non-fatal SyntaxError token.
            pos_ += len;
            end_ = pos_;
            return set(Token::SyntaxError);
        }

        return scan_punctuation(c);
    }

    std::expected<Token, Diagnostic> punct(std::size_t n, Token t) {
        pos_ += n;
        end_ = pos_;
        return set(t);
    }
    Token set(Token t) {
        token_ = t;
        return t;
    }

    char peek(std::size_t k) const {
        return (pos_ + k < src_.size()) ? src_[pos_ + k] : '\0';
    }

    std::expected<Token, Diagnostic> scan_punctuation(unsigned char c) {
        switch (c) {
        case '(': return punct(1, Token::OpenParen);
        case ')': return punct(1, Token::CloseParen);
        case '[': return punct(1, Token::OpenBracket);
        case ']': return punct(1, Token::CloseBracket);
        case '{': return punct(1, Token::OpenBrace);
        case '}': return punct(1, Token::CloseBrace);
        case ',': return punct(1, Token::Comma);
        case ';': return punct(1, Token::Semicolon);
        case ':': return punct(1, Token::Colon);
        case '~': return punct(1, Token::Tilde);
        case '@': return punct(1, Token::At);
        case '?':
            if (peek(1) == '?') {
                return peek(2) == '=' ? punct(3, Token::QuestionQuestionEquals)
                                      : punct(2, Token::QuestionQuestion);
            }
            if (peek(1) == '.' && !(peek(2) >= '0' && peek(2) <= '9')) {
                return punct(2, Token::QuestionDot);
            }
            return punct(1, Token::Question);
        case '&':
            if (peek(1) == '&') {
                return peek(2) == '=' ? punct(3, Token::AmpersandAmpersandEquals)
                                      : punct(2, Token::AmpersandAmpersand);
            }
            return peek(1) == '=' ? punct(2, Token::AmpersandEquals) : punct(1, Token::Ampersand);
        case '|':
            if (peek(1) == '|') {
                return peek(2) == '=' ? punct(3, Token::BarBarEquals) : punct(2, Token::BarBar);
            }
            return peek(1) == '=' ? punct(2, Token::BarEquals) : punct(1, Token::Bar);
        case '^':
            return peek(1) == '=' ? punct(2, Token::CaretEquals) : punct(1, Token::Caret);
        case '+':
            if (peek(1) == '+') {
                return punct(2, Token::PlusPlus);
            }
            return peek(1) == '=' ? punct(2, Token::PlusEquals) : punct(1, Token::Plus);
        case '-':
            if (peek(1) == '-') {
                return punct(2, Token::MinusMinus);
            }
            return peek(1) == '=' ? punct(2, Token::MinusEquals) : punct(1, Token::Minus);
        case '*':
            if (peek(1) == '*') {
                return peek(2) == '=' ? punct(3, Token::AsteriskAsteriskEquals)
                                      : punct(2, Token::AsteriskAsterisk);
            }
            return peek(1) == '=' ? punct(2, Token::AsteriskEquals) : punct(1, Token::Asterisk);
        case '/':
            return peek(1) == '=' ? punct(2, Token::SlashEquals) : punct(1, Token::Slash);
        case '%':
            return peek(1) == '=' ? punct(2, Token::PercentEquals) : punct(1, Token::Percent);
        case '=':
            if (peek(1) == '=') {
                return peek(2) == '=' ? punct(3, Token::EqualsEqualsEquals)
                                      : punct(2, Token::EqualsEquals);
            }
            return peek(1) == '>' ? punct(2, Token::EqualsGreaterThan) : punct(1, Token::Equals);
        case '!':
            if (peek(1) == '=') {
                return peek(2) == '=' ? punct(3, Token::ExclamationEqualsEquals)
                                      : punct(2, Token::ExclamationEquals);
            }
            return punct(1, Token::Exclamation);
        case '<':
            if (peek(1) == '<') {
                return peek(2) == '=' ? punct(3, Token::LessThanLessThanEquals)
                                      : punct(2, Token::LessThanLessThan);
            }
            return peek(1) == '=' ? punct(2, Token::LessThanEquals) : punct(1, Token::LessThan);
        case '>':
            if (peek(1) == '>') {
                if (peek(2) == '>') {
                    return peek(3) == '=' ? punct(4, Token::GreaterThanGreaterThanGreaterThanEquals)
                                          : punct(3, Token::GreaterThanGreaterThanGreaterThan);
                }
                return peek(2) == '=' ? punct(3, Token::GreaterThanGreaterThanEquals)
                                      : punct(2, Token::GreaterThanGreaterThan);
            }
            return peek(1) == '=' ? punct(2, Token::GreaterThanEquals) : punct(1, Token::GreaterThan);
        default:
            pos_ += 1;
            end_ = pos_;
            return set(Token::SyntaxError);
        }
    }

    // ── identifiers ─────────────────────────────────────────────────────────

    std::expected<Token, Diagnostic> scan_identifier() {
        identBuf_.clear();
        bool sawEscape{false};
        bool first{true};
        while (pos_ < src_.size()) {
            unsigned char c = static_cast<unsigned char>(src_[pos_]);
            if (c == '\\') {
                sawEscape = true;
                if (peek(1) != 'u') {
                    return make_fatal("Syntax Error: expected 'u' in identifier escape");
                }
                pos_ += 2;  // consume "\u"
                auto cp = read_unicode_escape_after_u();
                if (!cp) {
                    return std::unexpected(cp.error());
                }
                char32_t v = *cp;
                bool ok = first ? is_unicode_id_start(v) : is_unicode_id_continue(v);
                if (!ok) {
                    return make_fatal("Invalid identifier");
                }
                append_utf8(identBuf_, v);
                first = false;
                continue;
            }
            if (c < 0x80) {
                bool ok = first ? is_ascii_id_start(c) : is_ascii_id_continue(c);
                if (!ok) {
                    break;
                }
                identBuf_.push_back(static_cast<char>(c));
                pos_ += 1;
                first = false;
                continue;
            }
            auto [cp, len] = decode_utf8(pos_);
            bool ok = first ? is_unicode_id_start(cp) : is_unicode_id_continue(cp);
            if (!ok) {
                // A non-ASCII codepoint that is neither an identifier character
                // nor a separator cannot begin the NEXT token either, so merely
                // ending the identifier here reported the failure in the wrong
                // place ("Expected identifier but found SyntaxError"). Name the
                // offending run instead, the way esbuild/bun do.
                // Whitespace/newline codepoints (NBSP, U+2028, ...) legitimately
                // terminate an identifier and must keep breaking out.
                // ref: regression 012039.
                if (!first && !is_unicode_whitespace(cp) && !is_unicode_newline(cp)) {
                    std::string msg{"Unexpected \""};
                    msg += identBuf_;
                    msg += src_.substr(pos_, len);
                    msg += '"';
                    return make_fatal(msg);
                }
                break;
            }
            identBuf_.append(src_.substr(pos_, len));
            pos_ += len;
            first = false;
        }
        end_ = pos_;
        if (first) {
            // No identifier characters consumed at all (e.g. lone bad escape).
            return make_fatal("Syntax Error");
        }
        Token kw = keyword_token(identBuf_);
        if (kw != Token::Identifier) {
            return set(sawEscape ? Token::EscapedKeyword : kw);
        }
        return set(Token::Identifier);
    }

    std::expected<Token, Diagnostic> scan_private_identifier() {
        // at '#'
        identBuf_.assign("#");
        pos_ += 1;
        if (pos_ >= src_.size()) {
            return make_fatal("Syntax Error: unexpected end after '#'");
        }
        unsigned char c = static_cast<unsigned char>(src_[pos_]);
        bool startOk;
        if (c == '\\') {
            startOk = true;  // \u escape validated in loop
        } else if (c < 0x80) {
            startOk = is_ascii_id_start(c);
        } else {
            auto [cp, len] = decode_utf8(pos_);
            (void)len;
            startOk = is_unicode_id_start(cp);
        }
        if (!startOk) {
            return make_fatal("Syntax Error: expected identifier after '#'");
        }
        bool first{true};
        while (pos_ < src_.size()) {
            unsigned char ch = static_cast<unsigned char>(src_[pos_]);
            if (ch == '\\') {
                if (peek(1) != 'u') {
                    return make_fatal("Syntax Error");
                }
                pos_ += 2;
                auto cp = read_unicode_escape_after_u();
                if (!cp) {
                    return std::unexpected(cp.error());
                }
                char32_t v = *cp;
                bool ok = first ? is_unicode_id_start(v) : is_unicode_id_continue(v);
                if (!ok) {
                    return make_fatal("Invalid identifier");
                }
                append_utf8(identBuf_, v);
                first = false;
                continue;
            }
            if (ch < 0x80) {
                bool ok = first ? is_ascii_id_start(ch) : is_ascii_id_continue(ch);
                if (!ok) {
                    break;
                }
                identBuf_.push_back(static_cast<char>(ch));
                pos_ += 1;
                first = false;
                continue;
            }
            auto [cp, len] = decode_utf8(pos_);
            bool ok = first ? is_unicode_id_start(cp) : is_unicode_id_continue(cp);
            if (!ok) {
                break;
            }
            identBuf_.append(src_.substr(pos_, len));
            pos_ += len;
            first = false;
        }
        end_ = pos_;
        return set(Token::PrivateIdentifier);
    }

    static Token keyword_token(std::string_view s) {
        struct Kw {
            std::string_view text;
            Token token;
        };
        static constexpr Kw TABLE[] = {
            {"break", Token::Break},         {"case", Token::Case},
            {"catch", Token::Catch},         {"class", Token::Class},
            {"const", Token::Const},         {"continue", Token::Continue},
            {"debugger", Token::Debugger},   {"default", Token::Default},
            {"delete", Token::Delete},       {"do", Token::Do},
            {"else", Token::Else},           {"enum", Token::Enum},
            {"export", Token::Export},       {"extends", Token::Extends},
            {"false", Token::False},         {"finally", Token::Finally},
            {"for", Token::For},             {"function", Token::Function},
            {"if", Token::If},               {"import", Token::Import},
            {"in", Token::In},               {"instanceof", Token::Instanceof},
            {"new", Token::New},             {"null", Token::Null},
            {"return", Token::Return},       {"super", Token::Super},
            {"switch", Token::Switch},       {"this", Token::This},
            {"throw", Token::Throw},         {"true", Token::True},
            {"try", Token::Try},             {"typeof", Token::Typeof},
            {"var", Token::Var},             {"void", Token::Void},
            {"while", Token::While},         {"with", Token::With},
        };
        for (const auto& kw : TABLE) {
            if (kw.text == s) {
                return kw.token;
            }
        }
        return Token::Identifier;
    }

    // ── numbers ─────────────────────────────────────────────────────────────

    std::expected<Token, Diagnostic> scan_number() {
        std::size_t begin = pos_;
        unsigned char c0 = static_cast<unsigned char>(src_[pos_]);

        // Leading '.' float (e.g. ".5").
        if (c0 == '.') {
            return scan_decimal(begin, /*sawDot=*/true);
        }

        if (c0 == '0' && pos_ + 1 < src_.size()) {
            unsigned char n = static_cast<unsigned char>(src_[pos_ + 1]);
            if (n == 'x' || n == 'X') {
                return scan_radix(begin, 16, "Syntax Error");
            }
            if (n == 'b' || n == 'B') {
                return scan_radix(begin, 2, "Syntax Error");
            }
            if (n == 'o' || n == 'O') {
                return scan_radix(begin, 8, "Syntax Error");
            }
            if (n >= '0' && n <= '9') {
                return scan_legacy_octal(begin);
            }
            if (n == '_') {
                return make_fatal("Syntax Error");  // separator after leading 0
            }
            if (n == 'n') {
                pos_ += 2;
                return finish_bigint(begin);
            }
            // "0" followed by '.', 'e', etc. falls through to decimal.
        }
        return scan_decimal(begin, /*sawDot=*/false);
    }

    // Hex / binary / octal with the 0x/0b/0o prefix and '_' separators.
    std::expected<Token, Diagnostic> scan_radix(std::size_t begin, int radix,
                                                std::string_view err) {
        pos_ += 2;  // consume prefix
        std::string digits;
        bool lastUnderscore{false};
        bool any{false};
        while (pos_ < src_.size()) {
            unsigned char c = static_cast<unsigned char>(src_[pos_]);
            if (c == '_') {
                if (!any || lastUnderscore) {
                    return make_fatal(std::string{err});
                }
                lastUnderscore = true;
                pos_ += 1;
                continue;
            }
            int d = digit_value(c);
            if (d < 0 || d >= radix) {
                break;
            }
            digits.push_back(static_cast<char>(c));
            any = true;
            lastUnderscore = false;
            pos_ += 1;
        }
        if (!any || lastUnderscore) {
            return make_fatal(std::string{err});
        }
        // BigInt suffix?
        if (pos_ < src_.size() && src_[pos_] == 'n') {
            pos_ += 1;
            return finish_bigint(begin);
        }
        if (auto e = ensure_no_trailing_ident(err); !e) {
            return std::unexpected(e.error());
        }
        double v{0.0};
        for (char ch : digits) {
            v = v * radix + digit_value(static_cast<unsigned char>(ch));
        }
        number_ = v;
        end_ = pos_;
        return set(Token::NumericLiteral);
    }

    // Legacy octal (leading 0 followed by digits). If any 8/9 appears the whole
    // thing is a NonOctalDecimalIntegerLiteral. No '.', exponent, 'n', or
    // separators allowed.
    std::expected<Token, Diagnostic> scan_legacy_octal(std::size_t begin) {
        pos_ += 1;  // consume '0'
        bool hasEightNine{false};
        std::string digits{"0"};
        while (pos_ < src_.size()) {
            unsigned char c = static_cast<unsigned char>(src_[pos_]);
            if (c == '_') {
                return make_fatal("Syntax Error");  // separators not allowed
            }
            if (c >= '0' && c <= '9') {
                if (c == '8' || c == '9') {
                    hasEightNine = true;
                }
                digits.push_back(static_cast<char>(c));
                pos_ += 1;
                continue;
            }
            break;
        }
        if (pos_ < src_.size()) {
            unsigned char c = static_cast<unsigned char>(src_[pos_]);
            if (c == '.' || c == 'e' || c == 'E' || c == 'n') {
                return make_fatal("Syntax Error");
            }
        }
        if (auto e = ensure_no_trailing_ident("Syntax Error"); !e) {
            return std::unexpected(e.error());
        }
        double v{0.0};
        int radix = hasEightNine ? 10 : 8;
        for (char ch : digits) {
            v = v * radix + digit_value(static_cast<unsigned char>(ch));
        }
        number_ = v;
        end_ = pos_;
        (void)begin;
        return set(Token::NumericLiteral);
    }

    std::expected<Token, Diagnostic> scan_decimal(std::size_t begin, bool sawDot) {
        std::string clean;  // digits with underscores removed
        auto read_digits = [&]() -> std::expected<bool, Diagnostic> {
            bool any{false};
            bool lastUnderscore{false};
            while (pos_ < src_.size()) {
                unsigned char c = static_cast<unsigned char>(src_[pos_]);
                if (c == '_') {
                    if (!any || lastUnderscore) {
                        return std::unexpected(make_fatal("Syntax Error").error());
                    }
                    lastUnderscore = true;
                    pos_ += 1;
                    continue;
                }
                if (c >= '0' && c <= '9') {
                    clean.push_back(static_cast<char>(c));
                    any = true;
                    lastUnderscore = false;
                    pos_ += 1;
                    continue;
                }
                break;
            }
            if (lastUnderscore) {
                return std::unexpected(make_fatal("Syntax Error").error());
            }
            return any;
        };

        if (sawDot) {
            clean.push_back('.');
            pos_ += 1;  // consume '.'
            auto d = read_digits();
            if (!d) {
                return std::unexpected(d.error());
            }
        } else {
            auto d = read_digits();
            if (!d) {
                return std::unexpected(d.error());
            }
            bool isInteger{true};
            if (pos_ < src_.size() && src_[pos_] == '.') {
                isInteger = false;
                clean.push_back('.');
                pos_ += 1;
                auto f = read_digits();
                if (!f) {
                    return std::unexpected(f.error());
                }
            }
            // BigInt (integer only, no dot/exponent).
            if (isInteger && pos_ < src_.size() && src_[pos_] == 'n') {
                pos_ += 1;
                return finish_bigint(begin);
            }
        }

        // Exponent.
        bool sawExp{false};
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            sawExp = true;
            clean.push_back('e');
            pos_ += 1;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) {
                clean.push_back(src_[pos_]);
                pos_ += 1;
            }
            auto d = read_digits();
            if (!d) {
                return std::unexpected(d.error());
            }
            if (!*d) {
                return make_fatal("Syntax Error");  // exponent with no digits
            }
        }
        // No bigint after dot/exponent.
        if ((sawDot || sawExp) && pos_ < src_.size() && src_[pos_] == 'n') {
            return make_fatal("Syntax Error");
        }
        // A dot-only (no digits at all) case shouldn't reach here.
        if (auto e = ensure_no_trailing_ident("Syntax Error"); !e) {
            return std::unexpected(e.error());
        }
        double v{0.0};
        auto res = std::from_chars(clean.data(), clean.data() + clean.size(), v);
        if (res.ec == std::errc::result_out_of_range) {
            // JS numeric literals never overflow: `1e999` is Infinity, `1e-999`
            // rounds to 0 (strtod gives IEEE semantics; from_chars flags a range
            // error and leaves the value implementation-defined).
            v = std::strtod(clean.c_str(), nullptr);
        } else if (res.ec != std::errc{}) {
            // e.g. ".e3" style — treat as syntax error.
            return make_fatal("Syntax Error");
        }
        number_ = v;
        end_ = pos_;
        return set(Token::NumericLiteral);
    }

    std::expected<Token, Diagnostic> finish_bigint(std::size_t begin) {
        // pos_ now just after 'n'. Raw slice is [begin, pos_-1) plus 'n'.
        std::string_view digits = src_.substr(begin, (pos_ - 1) - begin);
        identBuf_.clear();
        for (char ch : digits) {
            if (ch != '_') {
                identBuf_.push_back(ch);
            }
        }
        if (auto e = ensure_no_trailing_ident("Syntax Error"); !e) {
            return std::unexpected(e.error());
        }
        end_ = pos_;
        return set(Token::BigIntegerLiteral);
    }

    std::expected<void, Diagnostic> ensure_no_trailing_ident(std::string_view err) {
        if (pos_ >= src_.size()) {
            return {};
        }
        unsigned char c = static_cast<unsigned char>(src_[pos_]);
        if (c == '\\') {
            return fatal(std::string{err});
        }
        if (c < 0x80) {
            if (is_ascii_id_start(c)) {
                return fatal(std::string{err});
            }
            return {};
        }
        auto [cp, len] = decode_utf8(pos_);
        (void)len;
        if (is_unicode_id_start(cp)) {
            return fatal(std::string{err});
        }
        return {};
    }

    static int digit_value(unsigned char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    // ── strings ─────────────────────────────────────────────────────────────

    std::expected<Token, Diagnostic> scan_string(unsigned char quote) {
        pos_ += 1;  // opening quote
        strBuf_.clear();
        strValid_ = false;
        while (true) {
            if (pos_ >= src_.size()) {
                return make_fatal("Unterminated string literal");
            }
            unsigned char c = static_cast<unsigned char>(src_[pos_]);
            if (c == quote) {
                pos_ += 1;
                break;
            }
            if (c == '\n' || c == '\r') {
                return make_fatal("Unterminated string literal");
            }
            if (c == '\\') {
                if (auto e = decode_escape(strBuf_); !e) {
                    return std::unexpected(e.error());
                }
                continue;
            }
            if (c < 0x80) {
                append_utf16(strBuf_, c);
                pos_ += 1;
                continue;
            }
            auto [cp, len] = decode_utf8(pos_);
            append_utf16(strBuf_, cp);
            pos_ += len;
        }
        strValid_ = true;
        end_ = pos_;
        return set(Token::StringLiteral);
    }

    // Decode a backslash escape at pos_ into `out` (UTF-16). Advances pos_.
    std::expected<void, Diagnostic> decode_escape(std::u16string& out) {
        // pos_ at '\\'
        pos_ += 1;
        if (pos_ >= src_.size()) {
            return fatal("Unterminated string literal");
        }
        unsigned char c = static_cast<unsigned char>(src_[pos_]);
        switch (c) {
        case 'b': out.push_back(u'\b'); pos_ += 1; return {};
        case 'f': out.push_back(u'\f'); pos_ += 1; return {};
        case 'n': out.push_back(u'\n'); pos_ += 1; return {};
        case 'r': out.push_back(u'\r'); pos_ += 1; return {};
        case 't': out.push_back(u'\t'); pos_ += 1; return {};
        case 'v': out.push_back(u'\v'); pos_ += 1; return {};
        case '\n': pos_ += 1; return {};  // line continuation LF
        case '\r':
            pos_ += 1;
            if (pos_ < src_.size() && src_[pos_] == '\n') {
                pos_ += 1;
            }
            return {};  // line continuation CRLF
        case 'x': {
            pos_ += 1;
            int hi = pos_ < src_.size() ? digit_value(static_cast<unsigned char>(src_[pos_])) : -1;
            int lo = pos_ + 1 < src_.size()
                         ? digit_value(static_cast<unsigned char>(src_[pos_ + 1]))
                         : -1;
            if (hi < 0 || hi > 15 || lo < 0 || lo > 15) {
                return fatal("Syntax Error");
            }
            out.push_back(static_cast<char16_t>(hi * 16 + lo));
            pos_ += 2;
            return {};
        }
        case 'u': {
            pos_ += 1;
            auto cp = read_unicode_escape_after_u();
            if (!cp) {
                return std::unexpected(cp.error());
            }
            append_utf16(out, *cp);
            return {};
        }
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7': {
            // Legacy octal escape (up to 3 octal digits). "\0" followed by a
            // digit is a non-fatal "Invalid legacy octal literal".
            if (c == '0') {
                unsigned char n = static_cast<unsigned char>(peek(1));
                if (n < '0' || n > '9') {
                    out.push_back(u'\0');
                    pos_ += 1;
                    return {};
                }
                warn("Invalid legacy octal literal");
            } else {
                warn("Invalid legacy octal literal");
            }
            int value{0};
            int count{0};
            int maxDigits = (c <= '3') ? 3 : 2;
            while (count < maxDigits && pos_ < src_.size()) {
                unsigned char d = static_cast<unsigned char>(src_[pos_]);
                if (d < '0' || d > '7') {
                    break;
                }
                value = value * 8 + (d - '0');
                pos_ += 1;
                ++count;
            }
            out.push_back(static_cast<char16_t>(value));
            return {};
        }
        default:
            // Identity escape: the char itself (decode as UTF-8 codepoint).
            if (c < 0x80) {
                append_utf16(out, c);
                pos_ += 1;
            } else {
                auto [cp, len] = decode_utf8(pos_);
                append_utf16(out, cp);
                pos_ += len;
            }
            return {};
        }
    }

    // pos_ points just after "\u". Reads \uHHHH or \u{...}. Returns codepoint.
    std::expected<char32_t, Diagnostic> read_unicode_escape_after_u() {
        if (pos_ < src_.size() && src_[pos_] == '{') {
            pos_ += 1;
            char32_t v{0};
            int count{0};
            while (pos_ < src_.size() && src_[pos_] != '}') {
                int d = digit_value(static_cast<unsigned char>(src_[pos_]));
                if (d < 0 || d > 15) {
                    return std::unexpected(make_fatal("Syntax Error").error());
                }
                v = v * 16 + d;
                if (v > 0x10FFFF) {
                    return std::unexpected(
                        make_fatal("Unicode escape sequence is out of range").error());
                }
                ++count;
                pos_ += 1;
            }
            if (pos_ >= src_.size() || src_[pos_] != '}' || count == 0) {
                return std::unexpected(make_fatal("Syntax Error").error());
            }
            pos_ += 1;  // consume '}'
            return v;
        }
        // Exactly 4 hex digits.
        char32_t v{0};
        for (int i = 0; i < 4; ++i) {
            if (pos_ >= src_.size()) {
                return std::unexpected(make_fatal("Syntax Error").error());
            }
            int d = digit_value(static_cast<unsigned char>(src_[pos_]));
            if (d < 0 || d > 15) {
                return std::unexpected(make_fatal("Syntax Error").error());
            }
            v = v * 16 + d;
            pos_ += 1;
        }
        return v;
    }

    // ── templates ───────────────────────────────────────────────────────────

    // opener == '`' for head/no-substitution; '}' for middle/tail.
    std::expected<Token, Diagnostic> scan_template(unsigned char opener) {
        pos_ += 1;  // consume opener
        strBuf_.clear();
        strValid_ = false;
        rawTmplBuf_.clear();
        bool endedWithDollar{false};
        while (true) {
            if (pos_ >= src_.size()) {
                return make_fatal("Unterminated string literal");
            }
            unsigned char c = static_cast<unsigned char>(src_[pos_]);
            if (c == '`') {
                pos_ += 1;
                break;
            }
            if (c == '$' && peek(1) == '{') {
                pos_ += 2;
                endedWithDollar = true;
                break;
            }
            if (c == '\\') {
                // Raw keeps the escape verbatim; cooked decodes it.
                std::size_t escStart = pos_;
                if (auto e = decode_escape(strBuf_); !e) {
                    return std::unexpected(e.error());
                }
                rawTmplBuf_.append(src_.substr(escStart, pos_ - escStart));
                continue;
            }
            if (c == '\r') {
                // TV/TRV: normalize \r and \r\n to \n.
                strBuf_.push_back(u'\n');
                rawTmplBuf_.push_back('\n');
                pos_ += 1;
                if (pos_ < src_.size() && src_[pos_] == '\n') {
                    pos_ += 1;
                }
                continue;
            }
            if (c < 0x80) {
                append_utf16(strBuf_, c);
                rawTmplBuf_.push_back(static_cast<char>(c));
                pos_ += 1;
                continue;
            }
            auto [cp, len] = decode_utf8(pos_);
            append_utf16(strBuf_, cp);
            rawTmplBuf_.append(src_.substr(pos_, len));
            pos_ += len;
        }
        strValid_ = true;
        end_ = pos_;
        if (opener == '`') {
            return set(endedWithDollar ? Token::TemplateHead : Token::NoSubstitutionTemplateLiteral);
        }
        return set(endedWithDollar ? Token::TemplateMiddle : Token::TemplateTail);
    }

    // ── utf-8 / utf-16 helpers ──────────────────────────────────────────────

    static void append_utf8(std::string& out, char32_t cp) {
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

    static void append_utf16(std::u16string& out, char32_t cp) {
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<char16_t>(cp));
        } else {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        }
    }
};

}  // namespace mbun::js_lexer
