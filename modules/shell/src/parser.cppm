// parser.cppm — mbun.shell.parser: Bun shell lexer, AST, and parser.
//
// Re-implementation (NOT a line translation) of bun's shell parser, with its
// algorithm/data-structures/boundaries as the blueprint:
//   ref: bun src/shell_parser/parse.rs   (Lexer, Parser, ast, TokenTag)
//   ref: bun src/shell_parser/json_fmt.rs (AST JSON shape asserted by tests)
//   ref: compat/bun/test/js/bun/shell/{lex,parse}.test.ts (acceptance vectors)
//
// Scope (T2.10): lexing (words / quotes / vars / operators / redirects / glob
// metachars / brace-expansion / command-substitution / subshell) and syntax
// (command -> pipeline -> list; redirects; assignment prefixes; if-clause;
// cond-expr) producing an AST plus parse-error messages. Interpreter execution,
// builtins and real IO are DEFERRED(S1) -> T3.7.
//
// Design notes (MC++ / performance):
//   - single lexical pass over the ASCII source; words are copied into a stable
//     strpool once and tokens carry [start,end) ranges (zero re-copy on read).
//   - atoms hold std::string_view slices into that strpool (zero-copy).
//   - Unicode/WTF-8 source and the full JS-string range expansion path remain
//     DEFERRED. Template context analysis recognizes Bun's \x08__bunstr_N marker
//     in the real recursive lexer, while JS object refs \x08__bun_N remain
//     supported for redirect/pipe-target vectors.
//
// Output is canonical JSON matching json_fmt.rs field order, so the C++ test
// compares serialized strings directly (bun's tests JSON.parse + toEqual, which
// is order-independent; we pin the reference order on both sides).

export module mbun.shell.parser;

import std;

export namespace mbun::shell {

// ───────────────────────────── tokens ─────────────────────────────

enum class Tok : std::uint8_t {
    Pipe, DoublePipe, Ampersand, DoubleAmpersand, Redirect, Dollar, Asterisk,
    DoubleAsterisk, Eq, Semicolon, Newline, BraceBegin, Comma, BraceEnd,
    CmdSubstBegin, CmdSubstQuoted, CmdSubstEnd, OpenParen, CloseParen, Var,
    VarArgv, Text, SingleQuotedText, DoubleQuotedText, JSObjRef,
    DoubleBracketOpen, DoubleBracketClose, Delimit, Eof,
};

// ref: parse.rs ast::RedirectFlags
namespace rf {
constexpr std::uint8_t STDIN = 1 << 0;
constexpr std::uint8_t STDOUT = 1 << 1;
constexpr std::uint8_t STDERR = 1 << 2;
constexpr std::uint8_t APPEND = 1 << 3;
constexpr std::uint8_t DUPLICATE_OUT = 1 << 4;
}  // namespace rf

struct TextRange {
    std::uint32_t start{0};
    std::uint32_t end{0};
};

struct Token {
    Tok tag;
    TextRange range{};       // Var / Text / SingleQuotedText / DoubleQuotedText
    std::uint8_t flags{0};   // Redirect
    std::uint32_t num{0};    // VarArgv / JSObjRef
};

// The PascalCase tag name emitted in lex JSON (matches TokenTag variant names).
constexpr std::string_view tok_name(Tok t) {
    switch (t) {
        case Tok::Pipe: return "Pipe";
        case Tok::DoublePipe: return "DoublePipe";
        case Tok::Ampersand: return "Ampersand";
        case Tok::DoubleAmpersand: return "DoubleAmpersand";
        case Tok::Redirect: return "Redirect";
        case Tok::Dollar: return "Dollar";
        case Tok::Asterisk: return "Asterisk";
        case Tok::DoubleAsterisk: return "DoubleAsterisk";
        case Tok::Eq: return "Eq";
        case Tok::Semicolon: return "Semicolon";
        case Tok::Newline: return "Newline";
        case Tok::BraceBegin: return "BraceBegin";
        case Tok::Comma: return "Comma";
        case Tok::BraceEnd: return "BraceEnd";
        case Tok::CmdSubstBegin: return "CmdSubstBegin";
        case Tok::CmdSubstQuoted: return "CmdSubstQuoted";
        case Tok::CmdSubstEnd: return "CmdSubstEnd";
        case Tok::OpenParen: return "OpenParen";
        case Tok::CloseParen: return "CloseParen";
        case Tok::Var: return "Var";
        case Tok::VarArgv: return "VarArgv";
        case Tok::Text: return "Text";
        case Tok::SingleQuotedText: return "SingleQuotedText";
        case Tok::DoubleQuotedText: return "DoubleQuotedText";
        case Tok::JSObjRef: return "JSObjRef";
        case Tok::DoubleBracketOpen: return "DoubleBracketOpen";
        case Tok::DoubleBracketClose: return "DoubleBracketClose";
        case Tok::Delimit: return "Delimit";
        case Tok::Eof: return "Eof";
    }
    return "?";
}

// Human-readable token spelling used inside parser error messages.
// ref: parse.rs Token::as_human_readable + TokenTag strum names
constexpr std::string_view tok_human(Tok t) {
    switch (t) {
        case Tok::Pipe: return "`|`";
        case Tok::DoublePipe: return "`||`";
        case Tok::Ampersand: return "`&`";
        case Tok::DoubleAmpersand: return "`&&`";
        case Tok::Redirect: return "`>`";
        case Tok::Dollar: return "`$`";
        case Tok::Asterisk: return "`*`";
        case Tok::DoubleAsterisk: return "`**`";
        case Tok::Eq: return "`=`";
        case Tok::Semicolon: return "`;`";
        case Tok::Newline: return "`\\n`";
        case Tok::BraceBegin: return "`{`";
        case Tok::Comma: return "`,`";
        case Tok::BraceEnd: return "`}`";
        case Tok::CmdSubstBegin: return "`$(`";
        case Tok::CmdSubstEnd: return "`)`";
        case Tok::OpenParen: return "`(`";
        case Tok::CloseParen: return "`)";
        case Tok::JSObjRef: return "JSObjRef";
        case Tok::DoubleBracketOpen: return "[[";
        case Tok::DoubleBracketClose: return "]]";
        case Tok::Delimit: return "Delimit";
        case Tok::Eof: return "EOF";
        default: return "?";
    }
}

// The snake_case ExprTag name used in "expected ... but got: <tag>" errors.
enum class ExprTag { Assign, Binary, Pipeline, Cmd, Subshell, If, CondExpr, Async };
constexpr std::string_view expr_tag_name(ExprTag t) {
    switch (t) {
        case ExprTag::Assign: return "assign";
        case ExprTag::Binary: return "binary";
        case ExprTag::Pipeline: return "pipeline";
        case ExprTag::Cmd: return "cmd";
        case ExprTag::Subshell: return "subshell";
        case ExprTag::If: return "if";
        case ExprTag::CondExpr: return "condexpr";
        case ExprTag::Async: return "async";
    }
    return "?";
}

// ───────────────────────────── AST ─────────────────────────────
// Owning value tree; recursive positions use shared_ptr. string_views slice the
// lexer strpool (kept alive by the driver that owns Lexer through emit).

enum class SAKind {
    Var, VarArgv, Text, QuotedEmpty, Asterisk, DoubleAsterisk, BraceBegin,
    BraceEnd, Comma, Tilde, CmdSubst,
};

// Expansion categories mirror bun's shell_parser::ast::SimpleAtom. Keeping
// the classification explicit lets the future expander dispatch on a stable
// tag without re-parsing source text.
enum class ExpansionKind : std::uint8_t {
    None, Variable, Positional, Tilde, Glob, Brace, CommandSubstitution,
};

constexpr ExpansionKind expansion_kind(SAKind kind) {
    switch (kind) {
        case SAKind::Var: return ExpansionKind::Variable;
        case SAKind::VarArgv: return ExpansionKind::Positional;
        case SAKind::Tilde: return ExpansionKind::Tilde;
        case SAKind::Asterisk:
        case SAKind::DoubleAsterisk: return ExpansionKind::Glob;
        case SAKind::BraceBegin:
        case SAKind::BraceEnd:
        case SAKind::Comma: return ExpansionKind::Brace;
        case SAKind::CmdSubst: return ExpansionKind::CommandSubstitution;
        case SAKind::Text:
        case SAKind::QuotedEmpty: return ExpansionKind::None;
    }
    return ExpansionKind::None;
}

struct Script;

struct SimpleAtom {
    SAKind kind{SAKind::Text};
    std::string_view text{};                  // Var, Text
    std::uint8_t varargv{0};                   // VarArgv
    std::shared_ptr<Script> cmdsubst_script{}; // CmdSubst
    bool cmdsubst_quoted{false};
};

struct CompoundAtom {
    std::vector<SimpleAtom> atoms;
    bool brace_expansion_hint{false};
    bool glob_hint{false};
};

enum class AtomKind { Simple, Compound };
struct Atom {
    AtomKind kind{AtomKind::Simple};
    SimpleAtom simple{};
    CompoundAtom compound{};

    bool has_glob_expansion() const {
        if (kind == AtomKind::Simple)
            return expansion_kind(simple.kind) == ExpansionKind::Glob;
        return compound.glob_hint;
    }

    bool has_brace_expansion() const {
        return kind == AtomKind::Compound && compound.brace_expansion_hint;
    }

    bool has_tilde_expansion() const {
        if (kind == AtomKind::Simple)
            return simple.kind == SAKind::Tilde;
        return !compound.atoms.empty() && compound.atoms.front().kind == SAKind::Tilde;
    }

    bool has_expansions() const {
        return has_glob_expansion() || has_brace_expansion() || has_tilde_expansion();
    }
};

// Semantic parser boundary names; aliases keep the existing AST representation
// and JSON shape unchanged.
using Word = Atom;

enum class RedirKind { Atom, JsBuf };
struct Redirect {
    RedirKind kind{RedirKind::Atom};
    Atom atom{};
    std::uint32_t jsbuf_idx{0};
};

struct Assign {
    std::string_view label{};
    Atom value{};
};

struct Cmd {
    std::vector<Assign> assigns;
    std::vector<Atom> name_and_args;
    std::uint8_t redirect{0};
    bool has_redirect_file{false};
    Redirect redirect_file{};
};

using Command = Cmd;

struct Binary;
struct Pipeline;
struct Subshell;
struct If;
struct CondExpr;

enum class ExprKind { Assign, Binary, Pipeline, Cmd, Subshell, If, CondExpr, Async };
struct Expr {
    ExprKind kind{ExprKind::Cmd};
    std::vector<Assign> assigns;             // Assign
    std::shared_ptr<Binary> binary;
    std::shared_ptr<Pipeline> pipeline;
    std::shared_ptr<Cmd> cmd;
    std::shared_ptr<Subshell> subshell;
    std::shared_ptr<If> ifc;
    std::shared_ptr<CondExpr> condexpr;
    std::shared_ptr<Expr> async;
    ExprTag tag() const {
        switch (kind) {
            case ExprKind::Assign: return ExprTag::Assign;
            case ExprKind::Binary: return ExprTag::Binary;
            case ExprKind::Pipeline: return ExprTag::Pipeline;
            case ExprKind::Cmd: return ExprTag::Cmd;
            case ExprKind::Subshell: return ExprTag::Subshell;
            case ExprKind::If: return ExprTag::If;
            case ExprKind::CondExpr: return ExprTag::CondExpr;
            case ExprKind::Async: return ExprTag::Async;
        }
        return ExprTag::Cmd;
    }
};

struct Stmt {
    std::vector<Expr> exprs;
};
struct Script {
    std::vector<Stmt> stmts;
};

enum class BinaryOp { And, Or };
struct Binary {
    BinaryOp op{BinaryOp::And};
    Expr left;
    Expr right;
};

enum class PIKind { Cmd, Assigns, Subshell, If, CondExpr };
struct PipelineItem {
    PIKind kind{PIKind::Cmd};
    std::shared_ptr<Cmd> cmd;
    std::vector<Assign> assigns;
    std::shared_ptr<Subshell> subshell;
    std::shared_ptr<If> ifc;
    std::shared_ptr<CondExpr> condexpr;
};
struct Pipeline {
    std::vector<PipelineItem> items;
};

struct Subshell {
    Script script;
    bool has_redirect{false};
    Redirect redirect{};
    std::uint8_t redirect_flags{0};
};

struct If {
    std::vector<Stmt> cond;
    std::vector<Stmt> then_;
    std::vector<std::vector<Stmt>> else_parts;
};

struct CondExpr {
    std::string op;          // serialized op name, e.g. "-f", "=="
    std::vector<Atom> args;
};

// ───────────────────────────── lexer ─────────────────────────────

enum class CharState { Normal, Single, Double };
enum class SubKind { None, Normal, Backtick, Dollar };

enum class TemplateQuoteContext : std::uint8_t { Unquoted, SingleQuoted, DoubleQuoted };

struct InputChar {
    std::uint32_t ch{0};
    bool escaped{false};
};

// ref: parse.rs SPECIAL_CHARS
constexpr std::array<bool, 256> make_special_table() {
    std::array<bool, 256> t{};
    for (unsigned char c : std::string_view("~[]#;\n\t\r*?{,}`$=()0123456789|><&'\" \\"))
        t[c] = true;
    t[8] = true;  // SPECIAL_JS_CHAR
    return t;
}
constexpr std::array<bool, 256> SPECIAL_CHARS_TABLE = make_special_table();

constexpr std::uint8_t SPECIAL_JS_CHAR = 8;
constexpr std::uint32_t MAX_SUBSHELL_DEPTH = 128;

// ref: parse.rs needs_escape_utf8_ascii_latin1 — an empty string needs escaping
// too, so `$.escape("")` yields `""` rather than a word that vanishes.
constexpr bool needs_escape(std::string_view text) {
    if (text.empty()) return true;
    for (unsigned char c : text)
        if (SPECIAL_CHARS_TABLE[c]) return true;
    return false;
}

// ref: parse.rs escape_8bit<ADD_QUOTES, LATIN1=false> — the input here is already
// UTF-8 (JS hands us a UTF-8 conversion), so bytes >= 0x80 are copied verbatim.
// Only the four chars that keep their meaning inside double quotes are
// backslashed; the JS-object marker keeps bun's `\x08""` spelling.
inline std::string escape_string(std::string_view text, bool addQuotes = true) {
    constexpr std::string_view BACKSLASHABLE_CHARS{"$`\"\\"};
    std::string out;
    out.reserve(text.size() + (addQuotes ? 2 : 0));
    if (addQuotes) out.push_back('"');
    for (char c : text) {
        if (BACKSLASHABLE_CHARS.find(c) != std::string_view::npos) {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) == SPECIAL_JS_CHAR) {
            out.push_back(static_cast<char>(SPECIAL_JS_CHAR));
            out.append("\"\"");
        } else {
            out.push_back(c);
        }
    }
    if (addQuotes) out.push_back('"');
    return out;
}
constexpr std::string_view JS_OBJREF_BODY = "__bun_";  // prefix after \x08
constexpr std::string_view JS_STRREF_BODY = "__bunstr_";

class Lexer {
public:
    Lexer(std::string_view src, std::uint32_t jsobjs_len,
          std::size_t template_marker_count = 0)
        : src_(src), jsobjs_len_(jsobjs_len),
          template_contexts_(template_marker_count),
          template_markers_seen_(template_marker_count) {}

    void run() {
        lex_loop();
        for (std::size_t index{0}; index < template_markers_seen_.size(); ++index) {
            if (!template_markers_seen_[index]) {
                add_error(std::format("Missing JS string ref {}", index));
            }
        }
    }

    const std::vector<Token>& tokens() const { return tokens_; }
    const std::string& strpool() const { return strpool_; }
    const std::vector<std::string>& errors() const { return errors_; }
    const std::vector<TemplateQuoteContext>& template_contexts() const {
        return template_contexts_;
    }

private:
    std::string_view src_;
    std::size_t i_{0};
    CharState state_{CharState::Normal};
    std::optional<InputChar> prev_{};
    std::optional<InputChar> current_{};
    std::string strpool_;
    std::vector<Token> tokens_;
    std::vector<std::string> errors_;
    std::uint32_t word_start_{0};
    SubKind in_subshell_{SubKind::None};
    std::uint32_t subshell_depth_{0};
    std::uint32_t jsobjs_len_{0};
    std::vector<TemplateQuoteContext> template_contexts_;
    std::vector<bool> template_markers_seen_;
    bool aborted_{false};

    std::uint32_t j() const { return static_cast<std::uint32_t>(strpool_.size()); }

    void add_error(std::string_view msg) { errors_.emplace_back(msg); }

    void push(Tok t) { tokens_.push_back(Token{t}); }
    void push_range(Tok t, TextRange r) { tokens_.push_back(Token{t, r}); }
    void push_redirect(std::uint8_t flags) {
        Token tk{Tok::Redirect};
        tk.flags = flags;
        tokens_.push_back(tk);
    }

    std::optional<Tok> last_tag() const {
        if (tokens_.empty()) return std::nullopt;
        return tokens_.back().tag;
    }

    // ── char source (byte-transparent) with backslash escaping ──
    // ref: parse.rs ShellCharIter::read_char
    //
    // Bytes are passed through unmasked. This used to be `& 0x7F` ("ASCII"),
    // which silently cleared the high bit of every UTF-8 continuation/lead
    // byte: `$`echo ${"Í"}`` emitted C3 8D as 43 0D ("C\r") and "€" as
    // 62 02 2C. No shell metacharacter is >= 0x80, so a raw byte can never be
    // confused with one, and append_char already truncates to a byte.
    // ref: regression 17244.
    std::optional<InputChar> read_char() {
        if (i_ >= src_.size()) return std::nullopt;
        std::uint32_t c = static_cast<unsigned char>(src_[i_]);
        if (c != '\\' || state_ == CharState::Single)
            return InputChar{c, false};
        // backslash
        if (i_ + 1 >= src_.size()) return std::nullopt;
        std::uint32_t nxt = static_cast<unsigned char>(src_[i_ + 1]);
        if (state_ == CharState::Normal) {
            return InputChar{nxt, true};
        }
        // Double: backslash only escapes these
        if (nxt == '$' || nxt == '`' || nxt == '"' || nxt == '\\' || nxt == '\n' ||
            nxt == '#') {
            return InputChar{nxt, true};
        }
        return InputChar{c, false};  // literal backslash
    }

    std::optional<InputChar> eat() {
        auto r = read_char();
        if (!r) return std::nullopt;
        prev_ = current_;
        current_ = r;
        i_ += 1 + (r->escaped ? 1 : 0);
        return r;
    }
    std::optional<InputChar> peek() { return read_char(); }

    void append_char(std::uint32_t c) { strpool_.push_back(static_cast<char>(c & 0xFF)); }

    static bool is_whitespace(InputChar c) {
        return c.ch == '\t' || c.ch == '\r' || c.ch == '\n' || c.ch == ' ';
    }

    // ref: parse.rs Lexer::is_immediately_escaped_quote
    bool immediately_escaped_quote() const {
        if (state_ == CharState::Double)
            return current_ && !current_->escaped && current_->ch == '"' && prev_ &&
                   !prev_->escaped && prev_->ch == '"';
        if (state_ == CharState::Single)
            return current_ && !current_->escaped && current_->ch == '\'' && prev_ &&
                   !prev_->escaped && prev_->ch == '\'';
        return false;
    }

    static bool in_delimit_set(Tok t) {
        switch (t) {
            case Tok::Var:
            case Tok::VarArgv:
            case Tok::Text:
            case Tok::SingleQuotedText:
            case Tok::DoubleQuotedText:
            case Tok::BraceBegin:
            case Tok::Comma:
            case Tok::BraceEnd:
            case Tok::CmdSubstEnd:
            case Tok::Asterisk:
                return true;
            default:
                return false;
        }
    }

    void break_word(bool add_delimiter) { break_word_impl(add_delimiter, false, false); }
    void break_word_operator() { break_word_impl(true, false, true); }

    // ref: parse.rs Lexer::break_word_impl
    void break_word_impl(bool add_delimiter, bool in_normal_space, bool in_operator) {
        std::uint32_t start = word_start_;
        std::uint32_t end = j();
        if (start != end || immediately_escaped_quote()) {
            Tok t = state_ == CharState::Normal   ? Tok::Text
                    : state_ == CharState::Single ? Tok::SingleQuotedText
                                                  : Tok::DoubleQuotedText;
            push_range(t, TextRange{start, end});
            if (add_delimiter) push(Tok::Delimit);
        } else if ((in_normal_space || in_operator) && !tokens_.empty() &&
                   in_delimit_set(tokens_.back().tag)) {
            push(Tok::Delimit);
        }
        word_start_ = j();
    }

    enum class RedirDir { Out, In };

    // Returns true if operator doubled (>> or <<). ref: eat_simple_redirect_operator
    bool eat_simple_redirect_operator(RedirDir dir) {
        auto p = peek();
        if (!p || p->escaped) return false;
        if (p->ch == '>') {
            if (dir == RedirDir::Out) { eat(); return true; }
            return false;
        }
        if (p->ch == '<') {
            if (dir == RedirDir::In) { eat(); return true; }
            return false;
        }
        return false;
    }

    std::uint8_t eat_simple_redirect(RedirDir dir) {
        bool dbl = eat_simple_redirect_operator(dir);
        if (dbl)
            return dir == RedirDir::Out ? (rf::APPEND | rf::STDOUT) : (rf::STDIN | rf::APPEND);
        return dir == RedirDir::Out ? rf::STDOUT : rf::STDIN;
    }

    // ref: parse.rs Lexer::eat_redirect  (leading fd digit 0/1/2)
    std::optional<std::uint8_t> eat_redirect(InputChar first) {
        std::uint8_t flags = 0;
        if (first.ch == '0') flags |= rf::STDIN;
        else if (first.ch == '1') flags |= rf::STDOUT;
        else if (first.ch == '2') flags |= rf::STDERR;
        else return std::nullopt;

        auto in = peek();
        if (!in || in->escaped) return std::nullopt;
        if (in->ch == '>') {
            eat();
            if (eat_simple_redirect_operator(RedirDir::Out)) flags |= rf::APPEND;
            auto p = peek();
            if (p && !p->escaped && p->ch == '&') {
                eat();
                auto p2 = peek();
                if (p2) {
                    if (p2->ch == '1') {
                        eat();
                        if (!(flags & rf::STDOUT) && (flags & rf::STDERR)) {
                            flags |= rf::DUPLICATE_OUT;
                            flags |= rf::STDOUT;
                            flags &= ~rf::STDERR;
                        } else return std::nullopt;
                    } else if (p2->ch == '2') {
                        eat();
                        if (!(flags & rf::STDERR) && (flags & rf::STDOUT)) {
                            flags |= rf::DUPLICATE_OUT;
                            flags |= rf::STDERR;
                            flags &= ~rf::STDOUT;
                        } else return std::nullopt;
                    } else return std::nullopt;
                }
            }
            return flags;
        }
        if (in->ch == '<') {
            if (eat_simple_redirect_operator(RedirDir::In)) flags |= rf::APPEND;
            return flags;
        }
        return std::nullopt;
    }

    // ref: parse.rs Lexer::eat_var
    TextRange eat_var() {
        std::uint32_t start = j();
        int idx = 0;
        bool is_int = false;
        while (auto pr = peek()) {
            std::uint32_t c = pr->ch;
            bool escaped = pr->escaped;
            if (idx == 0) {
                if (c == '=') return TextRange{start, j()};
                if (c >= '0' && c <= '9') {
                    is_int = true;
                    eat();
                    append_char(c);
                    ++idx;
                    continue;
                }
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'))
                    return TextRange{start, j()};
            }
            ++idx;
            if (is_int) return TextRange{start, j()};
            if (c == '{' || c == '}' || c == ';' || c == '\'' || c == '"' || c == ' ' ||
                c == '|' || c == '&' || c == '>' || c == ',' || c == '$')
                return TextRange{start, j()};
            if (!escaped &&
                ((in_subshell_ == SubKind::Dollar && c == ')') ||
                 (in_subshell_ == SubKind::Backtick && c == '`') ||
                 (in_subshell_ == SubKind::Normal && c == ')')))
                return TextRange{start, j()};
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                c == '_') {
                eat();
                append_char(c);
            } else {
                return TextRange{start, j()};
            }
        }
        return TextRange{start, j()};
    }

    void eat_comment() {
        while (auto p = eat()) {
            if (p->escaped) continue;
            if (p->ch == '\n') break;
        }
    }

    // ── JS object reference (\x08__bun_N) ──
    bool looks_like_js_obj_ref() const {
        std::string_view rest = src_.substr(std::min(i_, src_.size()));
        return rest.size() >= JS_OBJREF_BODY.size() &&
               rest.substr(0, JS_OBJREF_BODY.size()) == JS_OBJREF_BODY;
    }

    bool looks_like_js_string_ref() const {
        const std::string_view rest{src_.substr(std::min(i_, src_.size()))};
        return rest.size() >= JS_STRREF_BODY.size() &&
               rest.substr(0, JS_STRREF_BODY.size()) == JS_STRREF_BODY;
    }

    std::optional<std::uint32_t> eat_js_string_ref() {
        const std::string_view rest{src_.substr(std::min(i_, src_.size()))};
        std::size_t cursor{JS_STRREF_BODY.size()};
        std::uint32_t index{};
        bool hasDigit{false};
        while (cursor < rest.size() && rest[cursor] >= '0' && rest[cursor] <= '9') {
            hasDigit = true;
            const auto digit{static_cast<std::uint32_t>(rest[cursor] - '0')};
            if (index > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) {
                add_error("Invalid JS string ref (number too high)");
                return std::nullopt;
            }
            index = index * 10 + digit;
            ++cursor;
        }
        if (!hasDigit) {
            add_error("Invalid JS string ref (no idx)");
            return std::nullopt;
        }
        if (index >= template_contexts_.size()) {
            add_error("Invalid JS string ref (out of bounds)");
            return std::nullopt;
        }
        i_ += cursor;
        current_ = InputChar{static_cast<std::uint32_t>(rest[cursor - 1]), false};
        return index;
    }

    TemplateQuoteContext template_quote_context() const {
        switch (state_) {
            case CharState::Normal: return TemplateQuoteContext::Unquoted;
            case CharState::Single: return TemplateQuoteContext::SingleQuoted;
            case CharState::Double: return TemplateQuoteContext::DoubleQuoted;
        }
        return TemplateQuoteContext::Unquoted;
    }

    std::optional<Token> eat_js_obj_ref() {
        std::string_view rest = src_.substr(std::min(i_, src_.size()));
        std::size_t k = JS_OBJREF_BODY.size();
        std::string digits;
        while (k < rest.size() && rest[k] >= '0' && rest[k] <= '9') {
            digits.push_back(rest[k]);
            ++k;
        }
        if (digits.empty()) {
            add_error("Invalid JS object ref (no idx)");
            return std::nullopt;
        }
        std::uint32_t idx = 0;
        for (char d : digits) idx = idx * 10 + static_cast<std::uint32_t>(d - '0');
        if (idx >= jsobjs_len_) {
            add_error("Invalid JS object ref (out of bounds)");
            return std::nullopt;
        }
        i_ += k;  // advance past __bun_<digits>
        current_ = InputChar{static_cast<std::uint32_t>(digits.back()), false};
        Token tk{Tok::JSObjRef};
        tk.num = idx;
        return tk;
    }

    // ref: parse.rs Lexer::eat_subshell
    void eat_subshell(SubKind kind) {
        if (subshell_depth_ >= MAX_SUBSHELL_DEPTH) {
            add_error("Subshell nesting depth exceeded");
            aborted_ = true;
            return;
        }
        if (kind == SubKind::Dollar) eat();  // eat '('
        if (kind == SubKind::Dollar || kind == SubKind::Backtick) {
            push(Tok::CmdSubstBegin);
            if (state_ == CharState::Double) push(Tok::CmdSubstQuoted);
        } else {
            push(Tok::OpenParen);
        }
        CharState prev_state = state_;
        SubKind saved_in = in_subshell_;
        std::uint32_t saved_depth = subshell_depth_;
        in_subshell_ = kind;
        subshell_depth_ = saved_depth + 1;
        state_ = CharState::Normal;
        lex_loop();
        in_subshell_ = saved_in;
        subshell_depth_ = saved_depth;
        state_ = prev_state;
    }

    // Main lexing loop. Returns when a subshell/cmdsubst closer is hit (its
    // token is pushed), on abort, or at EOF (pushes Eof at top level).
    // ref: parse.rs Lexer::lex
    void lex_loop() {
        while (!aborted_) {
            auto ino = eat();
            if (!ino) {
                // EOF
                break_word(true);
                break;
            }
            InputChar in = *ino;
            std::uint32_t c = in.ch;
            bool esc = in.escaped;

            if (c == SPECIAL_JS_CHAR) {
                if (looks_like_js_string_ref()) {
                    if (const auto index{eat_js_string_ref()}) {
                        if (template_markers_seen_[*index]) {
                            add_error("Duplicate JS string ref");
                            return;
                        }
                        template_markers_seen_[*index] = true;
                        template_contexts_[*index] = template_quote_context();
                        // Keep grammar validation faithful without exposing the
                        // interpolated bytes as shell syntax.
                        append_char('x');
                        continue;
                    }
                } else if (looks_like_js_obj_ref()) {
                    if (auto tok = eat_js_obj_ref()) {
                        if (state_ == CharState::Double) {
                            add_error("JS object reference not allowed in double quotes");
                            return;
                        }
                        break_word(false);
                        tokens_.push_back(*tok);
                        continue;
                    }
                }
                // fall through: append the 0x08 byte
            } else if (!esc) {
                bool consumed = false;
                bool ret = false;
                if (dispatch_special(c, consumed, ret)) {
                    if (ret) return;
                    if (consumed) continue;
                    // else: fall through to append_char below
                }
            } else if (c == '\n') {
                if (state_ != CharState::Double) break_word_impl(true, true, false);
                continue;
            }

            append_char(c);
        }

        if (in_subshell_ != SubKind::None) {
            if (in_subshell_ == SubKind::Dollar || in_subshell_ == SubKind::Backtick)
                add_error("Unclosed command substitution");
            else
                add_error("Unclosed subshell");
            return;
        }
        push(Tok::Eof);
    }

    // Returns true if `c` is a recognized special char (handled or explicitly
    // left to fall through as text). `consumed` => continue loop; `ret` => the
    // caller must return from lex_loop. Returns false => not special here.
    bool dispatch_special(std::uint32_t c, bool& consumed, bool& ret) {
        const bool in_quote = state_ == CharState::Single || state_ == CharState::Double;
        switch (c) {
            case '[':
            case ']':
                if (in_quote) return true;  // literal in quotes
                handle_bracket(c, consumed);
                return true;
            case '#': {
                if (in_quote) return true;
                bool ws_preceding = !prev_ || is_whitespace(*prev_);
                if (!ws_preceding) return true;  // literal '#'
                break_word(true);
                eat_comment();
                consumed = true;
                return true;
            }
            case ';':
                if (in_quote) return true;
                break_word(true);
                push(Tok::Semicolon);
                consumed = true;
                return true;
            case '\n':
                if (in_quote) return true;
                break_word_impl(true, true, false);
                push(Tok::Newline);
                consumed = true;
                return true;
            case '*': {
                if (in_quote) return true;
                auto nx = peek();
                if (nx && !nx->escaped && nx->ch == '*') {
                    eat();
                    break_word(false);
                    push(Tok::DoubleAsterisk);
                    consumed = true;
                    return true;
                }
                break_word(false);
                push(Tok::Asterisk);
                consumed = true;
                return true;
            }
            case '{':
                if (in_quote) return true;
                break_word(false);
                push(Tok::BraceBegin);
                consumed = true;
                return true;
            case ',':
                if (in_quote) return true;
                break_word(false);
                push(Tok::Comma);
                consumed = true;
                return true;
            case '}':
                if (in_quote) return true;
                break_word(false);
                push(Tok::BraceEnd);
                consumed = true;
                return true;
            case '`':
                if (state_ == CharState::Single) return true;
                if (in_subshell_ == SubKind::Backtick) {
                    break_word_operator();
                    if (auto lt = last_tag(); lt && *lt != Tok::Delimit) push(Tok::Delimit);
                    push(Tok::CmdSubstEnd);
                    ret = true;
                    return true;
                }
                eat_subshell(SubKind::Backtick);
                consumed = true;
                return true;
            case '$': {
                if (state_ == CharState::Single) return true;
                auto pk = peek();
                if (pk && !pk->escaped && pk->ch == '(') {
                    break_word(false);
                    eat_subshell(SubKind::Dollar);
                    consumed = true;
                    return true;
                }
                break_word(false);
                TextRange vr = eat_var();
                std::uint32_t len = vr.end - vr.start;
                if (len == 0) {
                    append_char('$');
                    break_word(false);
                } else if (len == 1) {
                    char ch0 = strpool_[vr.start];
                    if (ch0 >= '0' && ch0 <= '9') {
                        Token tk{Tok::VarArgv};
                        tk.num = static_cast<std::uint32_t>(ch0 - '0');
                        tokens_.push_back(tk);
                    } else {
                        push_range(Tok::Var, vr);
                    }
                } else {
                    push_range(Tok::Var, vr);
                }
                word_start_ = j();
                consumed = true;
                return true;
            }
            case '(':
                if (in_quote) return true;
                break_word(true);
                eat_subshell(SubKind::Normal);
                consumed = true;
                return true;
            case ')':
                if (in_quote) return true;
                if (in_subshell_ != SubKind::Dollar && in_subshell_ != SubKind::Normal) {
                    add_error("Unexpected ')'");
                    consumed = true;
                    return true;
                }
                break_word(true);
                if (in_subshell_ == SubKind::Dollar) {
                    if (auto lt = last_tag()) {
                        switch (*lt) {
                            case Tok::Delimit:
                            case Tok::Semicolon:
                            case Tok::Eof:
                            case Tok::Newline:
                                break;
                            default:
                                push(Tok::Delimit);
                        }
                    }
                    push(Tok::CmdSubstEnd);
                } else {
                    push(Tok::CloseParen);
                }
                ret = true;
                return true;
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9': {
                if (state_ != CharState::Normal) return true;
                std::size_t snap_i = i_;
                auto snap_prev = prev_;
                auto snap_cur = current_;
                if (auto redir = eat_redirect(InputChar{c, false})) {
                    break_word(true);
                    push_redirect(*redir);
                    consumed = true;
                    return true;
                }
                i_ = snap_i;
                prev_ = snap_prev;
                current_ = snap_cur;
                return true;  // append the digit as text
            }
            case '|': {
                if (in_quote) return true;
                break_word_operator();
                auto nx = peek();
                if (!nx) {
                    add_error("Unexpected EOF");
                    ret = true;
                    return true;
                }
                if (!nx->escaped && nx->ch == '&') {
                    add_error(
                        "Piping stdout and stderr (`|&`) is not supported yet. Please file "
                        "an issue on GitHub.");
                    ret = true;
                    return true;
                }
                if (nx->escaped || nx->ch != '|') {
                    push(Tok::Pipe);
                } else {
                    eat();
                    push(Tok::DoublePipe);
                }
                consumed = true;
                return true;
            }
            case '>':
                if (in_quote) return true;
                break_word_operator();
                push_redirect(eat_simple_redirect(RedirDir::Out));
                consumed = true;
                return true;
            case '<':
                if (in_quote) return true;
                break_word_operator();
                push_redirect(eat_simple_redirect(RedirDir::In));
                consumed = true;
                return true;
            case '&': {
                if (in_quote) return true;
                break_word_operator();
                auto nx = peek();
                if (!nx) {
                    push(Tok::Ampersand);
                    consumed = true;
                    return true;
                }
                if (nx->ch == '>' && !nx->escaped) {
                    eat();
                    std::uint8_t inner = eat_simple_redirect_operator(RedirDir::Out)
                                             ? (rf::APPEND | rf::STDOUT | rf::STDERR)
                                             : (rf::STDOUT | rf::STDERR);
                    push_redirect(inner);
                } else if (nx->escaped || nx->ch != '&') {
                    push(Tok::Ampersand);
                } else {
                    eat();
                    push(Tok::DoubleAmpersand);
                }
                consumed = true;
                return true;
            }
            case '\'':
                if (state_ == CharState::Single) {
                    break_word(false);
                    state_ = CharState::Normal;
                    consumed = true;
                } else if (state_ == CharState::Normal) {
                    break_word(false);
                    state_ = CharState::Single;
                    consumed = true;
                }
                return true;
            case '"':
                if (state_ == CharState::Single) return true;
                if (state_ == CharState::Normal) {
                    break_word(false);
                    state_ = CharState::Double;
                } else {
                    break_word(false);
                    state_ = CharState::Normal;
                }
                consumed = true;
                return true;
            case ' ':
                if (state_ == CharState::Normal) {
                    break_word_impl(true, true, false);
                    consumed = true;
                }
                return true;
            default:
                return false;
        }
    }

    // ref: parse.rs '[' / ']' double-bracket detection
    void handle_bracket(std::uint32_t c, bool& consumed) {
        auto p = peek();
        if (!p || p->escaped || p->ch != c) return;  // literal single bracket
        std::size_t snap_i = i_;
        auto snap_prev = prev_;
        auto snap_cur = current_;
        eat();  // consume the second bracket
        auto p2 = peek();
        if (!p2) {
            break_word(true);
            push(c == '[' ? Tok::DoubleBracketOpen : Tok::DoubleBracketClose);
            consumed = true;
            return;
        }
        if (!p2->escaped) {
            if (c == '[') {
                if (p2->ch == ' ' || p2->ch == '\r' || p2->ch == '\n' || p2->ch == '\t') {
                    break_word(true);
                    push(Tok::DoubleBracketOpen);
                    consumed = true;
                    return;
                }
            } else {
                std::uint32_t d = p2->ch;
                if (d == ' ' || d == '\r' || d == '\n' || d == '\t' || d == ';' || d == '&' ||
                    d == '|' || d == '>') {
                    break_word(true);
                    push(Tok::DoubleBracketClose);
                    consumed = true;
                    return;
                }
            }
        }
        i_ = snap_i;
        prev_ = snap_prev;
        current_ = snap_cur;
    }
};

// ───────────────────────────── parser ─────────────────────────────

bool is_all_ascii_var_name(std::string_view name) {
    // ref: parse.rs is_valid_var_name_ascii
    if (name.empty()) return false;
    unsigned char f = static_cast<unsigned char>(name[0]);
    if (f == '=' || (f >= '0' && f <= '9')) return false;
    if (!((f >= 'a' && f <= 'z') || (f >= 'A' && f <= 'Z') || f == '_')) return false;
    for (unsigned char c : name)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '_'))
            return false;
    return true;
}

std::optional<std::uint32_t> has_eq_sign(std::string_view s) {
    auto pos = s.find('=');
    if (pos == std::string_view::npos) return std::nullopt;
    return static_cast<std::uint32_t>(pos);
}

// if/then/elif/else/fi keyword classification. ref: parse.rs IfClauseTok
enum class IfTok { If, Else, Elif, Then, Fi };
std::optional<IfTok> if_tok_from_text(std::string_view t) {
    if (t == "if") return IfTok::If;
    if (t == "else") return IfTok::Else;
    if (t == "elif") return IfTok::Elif;
    if (t == "then") return IfTok::Then;
    if (t == "fi") return IfTok::Fi;
    return std::nullopt;
}

// ref: parse.rs ast::CondExprOp supported/serialize tables
struct CondOpEntry {
    std::string_view name;
    bool supported;
};
constexpr std::array<CondOpEntry, 26> COND_SINGLE_ARG = {{
    {"-a", false}, {"-b", false}, {"-c", true}, {"-d", true}, {"-e", false}, {"-f", true},
    {"-g", false}, {"-h", false}, {"-k", false}, {"-p", false}, {"-r", false}, {"-s", false},
    {"-t", false}, {"-u", false}, {"-w", false}, {"-x", false}, {"-G", false}, {"-L", false},
    {"-N", false}, {"-O", false}, {"-S", false}, {"-o", false}, {"-v", false}, {"-R", false},
    {"-z", true}, {"-n", true},
}};
constexpr std::array<CondOpEntry, 13> COND_BINARY = {{
    {"-ef", false}, {"-nt", false}, {"-ot", false}, {"==", true}, {"!=", true}, {"<", false},
    {">", false}, {"-eq", false}, {"-ne", false}, {"-lt", false}, {"-le", false}, {"-gt", false},
    {"-ge", false},
}};

// Parse error signalling: we accumulate messages and unwind via a bool return.
class ParseAbort {};

enum class SubshellKind { None, CmdSubst, Normal };

class Parser {
public:
    Parser(const std::vector<Token>& tokens, const std::string& strpool)
        : tokens_(tokens), strpool_(strpool) {}

    // Returns the parsed script; on error, errors() is populated and the script
    // is partial. ref: parse.rs Parser::parse_impl (top-level)
    Script parse() {
        try {
            return parse_script(SubshellKind::None);
        } catch (const ParseAbort&) {
            return Script{};
        }
    }

    const std::vector<std::string>& errors() const { return errors_; }

private:
    const std::vector<Token>& tokens_;
    const std::string& strpool_;
    std::uint32_t current_{0};
    std::vector<std::string> errors_;

    [[noreturn]] void fail(std::string msg) {
        errors_.push_back(std::move(msg));
        throw ParseAbort{};
    }

    const Token& peek() const { return tokens_[current_]; }
    Tok peek_tag() const { return tokens_[current_].tag; }
    Tok peek_n_tag(std::uint32_t n) const {
        std::size_t idx = static_cast<std::size_t>(current_) + n;
        if (idx >= tokens_.size()) return tokens_.back().tag;
        return tokens_[idx].tag;
    }
    std::string_view text(TextRange r) const {
        return std::string_view(strpool_).substr(r.start, r.end - r.start);
    }
    std::string_view cur_text() const { return text(peek().range); }

    void advance() {
        if (peek_tag() != Tok::Eof) ++current_;
    }
    bool matchTok(Tok t) {
        if (peek_tag() == t) {
            advance();
            return true;
        }
        return false;
    }

    bool is_terminator(Tok t, SubshellKind sub) const {
        if (t == Tok::Semicolon || t == Tok::Newline || t == Tok::Eof) return true;
        if (sub == SubshellKind::CmdSubst && t == Tok::CmdSubstEnd) return true;
        if (sub == SubshellKind::Normal && t == Tok::CloseParen) return true;
        return false;
    }

    bool delimits(Tok t, SubshellKind sub) const {
        if (t == Tok::Delimit || t == Tok::Semicolon || t == Tok::Eof || t == Tok::Newline)
            return true;
        if (sub == SubshellKind::CmdSubst && t == Tok::CmdSubstEnd) return true;
        if (sub == SubshellKind::Normal && t == Tok::CloseParen) return true;
        return false;
    }

    // if-keyword only counts when the next token delimits it.
    std::optional<IfTok> if_tok_here(SubshellKind sub) const {
        if (peek_tag() != Tok::Text) return std::nullopt;
        if (!delimits(peek_n_tag(1), sub)) return std::nullopt;
        return if_tok_from_text(cur_text());
    }

    void skip_newlines() {
        while (matchTok(Tok::Newline)) {}
    }

    // ref: parse.rs Parser::parse_impl
    Script parse_script(SubshellKind sub) {
        Script script;
        if (tokens_.empty() || (tokens_.size() == 1 && tokens_[0].tag == Tok::Eof))
            return script;
        while (!(peek_tag() == Tok::Eof ||
                 (sub == SubshellKind::CmdSubst && peek_tag() == Tok::CmdSubstEnd) ||
                 (sub == SubshellKind::Normal && peek_tag() == Tok::CloseParen))) {
            skip_newlines();
            if (peek_tag() == Tok::Eof ||
                (sub == SubshellKind::CmdSubst && peek_tag() == Tok::CmdSubstEnd) ||
                (sub == SubshellKind::Normal && peek_tag() == Tok::CloseParen))
                break;
            script.stmts.push_back(parse_stmt(sub));
            skip_newlines();
        }
        return script;
    }

    // ref: parse.rs Parser::parse_stmt
    Stmt parse_stmt(SubshellKind sub) {
        Stmt stmt;
        while (!is_terminator(peek_tag(), sub)) {
            Expr expr = parse_binary(sub);
            if (matchTok(Tok::Ampersand))
                fail("Background commands \"&\" are not supported yet.");
            stmt.exprs.push_back(std::move(expr));
        }
        // consume a single ; or newline separator (closers/Eof stay)
        if (peek_tag() == Tok::Semicolon || peek_tag() == Tok::Newline) advance();
        return stmt;
    }

    // ref: parse.rs Parser::parse_binary
    Expr parse_binary(SubshellKind sub) {
        Expr left = parse_pipeline(sub);
        while (peek_tag() == Tok::DoubleAmpersand || peek_tag() == Tok::DoublePipe) {
            BinaryOp op = peek_tag() == Tok::DoubleAmpersand ? BinaryOp::And : BinaryOp::Or;
            advance();
            Expr right = parse_pipeline(sub);
            auto bin = std::make_shared<Binary>();
            bin->op = op;
            bin->left = std::move(left);
            bin->right = std::move(right);
            Expr e;
            e.kind = ExprKind::Binary;
            e.binary = bin;
            left = std::move(e);
        }
        return left;
    }

    static std::optional<PipelineItem> as_pipeline_item(const Expr& e) {
        PipelineItem item;
        switch (e.kind) {
            case ExprKind::Assign:
                item.kind = PIKind::Assigns;
                item.assigns = e.assigns;
                return item;
            case ExprKind::Cmd:
                item.kind = PIKind::Cmd;
                item.cmd = e.cmd;
                return item;
            case ExprKind::Subshell:
                item.kind = PIKind::Subshell;
                item.subshell = e.subshell;
                return item;
            case ExprKind::If:
                item.kind = PIKind::If;
                item.ifc = e.ifc;
                return item;
            case ExprKind::CondExpr:
                item.kind = PIKind::CondExpr;
                item.condexpr = e.condexpr;
                return item;
            default:
                return std::nullopt;
        }
    }

    // ref: parse.rs Parser::parse_pipeline
    Expr parse_pipeline(SubshellKind sub) {
        Expr expr = parse_compound_cmd(sub);
        if (peek_tag() == Tok::Pipe) {
            auto pl = std::make_shared<Pipeline>();
            auto item = as_pipeline_item(expr);
            if (!item)
                fail(std::string("Expected a command, assignment, or subshell but got: ") +
                     std::string(expr_tag_name(expr.tag())));
            pl->items.push_back(*item);
            while (matchTok(Tok::Pipe)) {
                expr = parse_compound_cmd(sub);
                auto it2 = as_pipeline_item(expr);
                if (!it2)
                    fail(std::string("Expected a command, assignment, or subshell but got: ") +
                         std::string(expr_tag_name(expr.tag())));
                pl->items.push_back(*it2);
            }
            Expr e;
            e.kind = ExprKind::Pipeline;
            e.pipeline = pl;
            return e;
        }
        return expr;
    }

    // ref: parse.rs Parser::parse_compound_cmd
    Expr parse_compound_cmd(SubshellKind sub) {
        if (peek_tag() == Tok::OpenParen) {
            auto ss = std::make_shared<Subshell>(parse_subshell());
            if (ss->redirect_flags != 0)
                fail("Subshells with redirections are currently not supported. Please open a "
                     "GitHub issue.");
            Expr e;
            e.kind = ExprKind::Subshell;
            e.subshell = ss;
            return e;
        }
        if (if_tok_here(sub) == IfTok::If) {
            auto ifc = std::make_shared<If>(parse_if_clause(sub));
            Expr e;
            e.kind = ExprKind::If;
            e.ifc = ifc;
            return e;
        }
        if (peek_tag() == Tok::DoubleBracketOpen) {
            auto ce = std::make_shared<CondExpr>(parse_cond_expr(sub));
            Expr e;
            e.kind = ExprKind::CondExpr;
            e.condexpr = ce;
            return e;
        }
        return parse_simple_cmd(sub);
    }

    // ref: parse.rs Parser::parse_subshell
    Subshell parse_subshell() {
        advance();  // OpenParen
        Subshell ss;
        ss.script = parse_script(SubshellKind::Normal);
        if (peek_tag() == Tok::CloseParen) advance();
        auto pr = parse_redirect();
        ss.redirect_flags = pr.flags;
        ss.has_redirect = pr.has_file;
        ss.redirect = pr.redirect;
        return ss;
    }

    struct ParsedRedirect {
        std::uint8_t flags{0};
        bool has_file{false};
        Redirect redirect{};
    };

    // ref: parse.rs Parser::parse_redirect
    ParsedRedirect parse_redirect() {
        ParsedRedirect pr;
        if (peek_tag() != Tok::Redirect) return pr;
        pr.flags = peek().flags;
        advance();
        if (peek_tag() == Tok::JSObjRef) {
            pr.has_file = true;
            pr.redirect.kind = RedirKind::JsBuf;
            pr.redirect.jsbuf_idx = peek().num;
            advance();
            return pr;
        }
        auto file = parse_atom(SubshellKind::None);
        if (!file) {
            if (pr.flags & rf::DUPLICATE_OUT) return pr;
            fail("Redirection with no file");
        }
        pr.has_file = true;
        pr.redirect.kind = RedirKind::Atom;
        pr.redirect.atom = *file;
        return pr;
    }

    // ref: parse.rs Parser::parse_simple_cmd
    Expr parse_simple_cmd(SubshellKind sub) {
        std::vector<Assign> assigns;
        while (!is_terminator(peek_tag(), sub)) {
            if (auto a = parse_assign(sub))
                assigns.push_back(std::move(*a));
            else
                break;
        }
        if (is_terminator(peek_tag(), sub)) {
            if (assigns.empty()) fail("expected a command or assignment");
            Expr e;
            e.kind = ExprKind::Assign;
            e.assigns = std::move(assigns);
            return e;
        }
        auto name = parse_atom(sub);
        if (!name) {
            if (assigns.empty())
                fail(std::string("expected a command or assignment but got: \"") +
                     std::string(tok_name(peek_tag())) + "\"");
            Expr e;
            e.kind = ExprKind::Assign;
            e.assigns = std::move(assigns);
            return e;
        }
        auto cmd = std::make_shared<Cmd>();
        cmd->assigns = std::move(assigns);
        cmd->name_and_args.push_back(std::move(*name));
        while (auto arg = parse_atom(sub)) cmd->name_and_args.push_back(std::move(*arg));
        auto pr = parse_redirect();
        cmd->redirect = pr.flags;
        cmd->has_redirect_file = pr.has_file;
        cmd->redirect_file = pr.redirect;
        Expr e;
        e.kind = ExprKind::Cmd;
        e.cmd = cmd;
        return e;
    }

    // ref: parse.rs Parser::parse_assign
    std::optional<Assign> parse_assign(SubshellKind sub) {
        if (peek_tag() != Tok::Text) return std::nullopt;
        std::uint32_t start_idx = current_;
        TextRange range = peek().range;
        advance();
        std::string_view txt = text(range);
        auto eqp = has_eq_sign(txt);
        if (eqp) {
            std::uint32_t eq_idx = *eqp;
            if (eq_idx != 0 && is_all_ascii_var_name(txt.substr(0, eq_idx))) {
                std::string_view label = txt.substr(0, eq_idx);
                if (eq_idx == txt.size() - 1) {
                    if (delimits(peek_tag(), sub)) {
                        advance();  // expect_delimit
                        Assign a;
                        a.label = label;
                        a.value.kind = AtomKind::Simple;
                        a.value.simple = SimpleAtom{SAKind::Text, std::string_view("")};
                        return a;
                    }
                    auto atom = parse_atom(sub);
                    if (!atom) fail("Expected an atom");
                    Assign a;
                    a.label = label;
                    a.value = *atom;
                    return a;
                }
                std::string_view txt_value = txt.substr(eq_idx + 1);
                if (delimits(peek_tag(), sub)) {
                    advance();
                    Assign a;
                    a.label = label;
                    a.value.kind = AtomKind::Simple;
                    a.value.simple = SimpleAtom{SAKind::Text, txt_value};
                    return a;
                }
                auto right = parse_atom(sub);
                if (!right) fail("Expected an atom");
                Atom left;
                left.kind = AtomKind::Simple;
                left.simple = SimpleAtom{SAKind::Text, txt_value};
                Assign a;
                a.label = label;
                a.value = merge_atoms(left, *right);
                return a;
            }
        }
        current_ = start_idx;  // rollback
        return std::nullopt;
    }

    // ref: parse.rs Atom::merge (only the shapes reachable from parse_assign)
    static Atom merge_atoms(const Atom& l, const Atom& r) {
        auto to_simple_list = [](const Atom& a, std::vector<SimpleAtom>& out) {
            if (a.kind == AtomKind::Simple)
                out.push_back(a.simple);
            else
                out.insert(out.end(), a.compound.atoms.begin(), a.compound.atoms.end());
        };
        auto brace = [](const SimpleAtom& s) {
            return s.kind == SAKind::BraceBegin || s.kind == SAKind::BraceEnd;
        };
        auto glob = [](const SimpleAtom& s) {
            return s.kind == SAKind::Asterisk || s.kind == SAKind::DoubleAsterisk;
        };
        Atom out;
        out.kind = AtomKind::Compound;
        to_simple_list(l, out.compound.atoms);
        to_simple_list(r, out.compound.atoms);
        bool bh = false, gh = false;
        auto scan = [&](const Atom& a) {
            if (a.kind == AtomKind::Simple) {
                bh = bh || brace(a.simple);
                gh = gh || glob(a.simple);
            } else {
                bh = bh || a.compound.brace_expansion_hint;
                gh = gh || a.compound.glob_hint;
            }
        };
        scan(l);
        scan(r);
        out.compound.brace_expansion_hint = bh;
        out.compound.glob_hint = gh;
        return out;
    }

    // ref: parse.rs Parser::parse_atom
    std::optional<Atom> parse_atom(SubshellKind sub) {
        std::vector<SimpleAtom> atoms;
        bool has_brace_open = false, has_brace_close = false, has_comma = false;
        bool has_glob = false;
        while (true) {
            Tok pk = peek_tag();
            if (pk == Tok::Delimit) {
                advance();
                break;
            }
            if (pk == Tok::Eof || pk == Tok::Semicolon || pk == Tok::Newline) break;
            if (sub == SubshellKind::CmdSubst && pk == Tok::CmdSubstEnd) break;
            if (sub == SubshellKind::Normal && pk == Tok::CloseParen) break;

            bool next_delimits = delimits(peek_n_tag(1), sub);
            switch (pk) {
                case Tok::Asterisk:
                    has_glob = true;
                    advance();
                    atoms.push_back(SimpleAtom{SAKind::Asterisk});
                    if (next_delimits) { matchTok(Tok::Delimit); goto done; }
                    break;
                case Tok::DoubleAsterisk:
                    has_glob = true;
                    advance();
                    atoms.push_back(SimpleAtom{SAKind::DoubleAsterisk});
                    if (next_delimits) { matchTok(Tok::Delimit); goto done; }
                    break;
                case Tok::BraceBegin:
                    has_brace_open = true;
                    advance();
                    atoms.push_back(SimpleAtom{SAKind::BraceBegin});
                    if (next_delimits) { matchTok(Tok::Delimit); goto done; }
                    break;
                case Tok::BraceEnd:
                    has_brace_close = true;
                    advance();
                    atoms.push_back(SimpleAtom{SAKind::BraceEnd});
                    if (next_delimits) { matchTok(Tok::Delimit); goto done; }
                    break;
                case Tok::Comma:
                    has_comma = true;
                    advance();
                    atoms.push_back(SimpleAtom{SAKind::Comma});
                    if (next_delimits) { matchTok(Tok::Delimit); goto done; }
                    break;
                case Tok::CmdSubstBegin: {
                    advance();
                    bool quoted = matchTok(Tok::CmdSubstQuoted);
                    Script script = parse_script(SubshellKind::CmdSubst);
                    if (peek_tag() == Tok::CmdSubstEnd) advance();
                    SimpleAtom sa;
                    sa.kind = SAKind::CmdSubst;
                    sa.cmdsubst_script = std::make_shared<Script>(std::move(script));
                    sa.cmdsubst_quoted = quoted;
                    atoms.push_back(std::move(sa));
                    if (delimits(peek_tag(), sub)) { matchTok(Tok::Delimit); goto done; }
                    break;
                }
                case Tok::Text:
                case Tok::SingleQuotedText:
                case Tok::DoubleQuotedText: {
                    TextRange r = peek().range;
                    advance();
                    std::string_view txt = text(r);
                    if (pk == Tok::Text && !txt.empty() && txt[0] == '~') {
                        txt = txt.substr(1);
                        atoms.push_back(SimpleAtom{SAKind::Tilde});
                        if (!txt.empty()) atoms.push_back(SimpleAtom{SAKind::Text, txt});
                    } else if (txt.empty() &&
                               (pk == Tok::SingleQuotedText || pk == Tok::DoubleQuotedText)) {
                        atoms.push_back(SimpleAtom{SAKind::QuotedEmpty});
                    } else {
                        atoms.push_back(SimpleAtom{SAKind::Text, txt});
                    }
                    if (next_delimits) { matchTok(Tok::Delimit); goto done; }
                    break;
                }
                case Tok::Var: {
                    std::string_view txt = text(peek().range);
                    advance();
                    atoms.push_back(SimpleAtom{SAKind::Var, txt});
                    if (next_delimits) { matchTok(Tok::Delimit); goto done; }
                    break;
                }
                case Tok::VarArgv: {
                    std::uint8_t n = static_cast<std::uint8_t>(peek().num);
                    advance();
                    SimpleAtom sa;
                    sa.kind = SAKind::VarArgv;
                    sa.varargv = n;
                    atoms.push_back(sa);
                    if (next_delimits) { matchTok(Tok::Delimit); goto done; }
                    break;
                }
                case Tok::OpenParen:
                    fail("Unexpected token: `(`");
                case Tok::CloseParen:
                    fail("Unexpected token: `)`");
                default:
                    return std::nullopt;
            }
        }
    done:
        if (atoms.empty()) return std::nullopt;
        if (atoms.size() == 1) {
            Atom a;
            a.kind = AtomKind::Simple;
            a.simple = std::move(atoms[0]);
            return a;
        }
        Atom a;
        a.kind = AtomKind::Compound;
        a.compound.atoms = std::move(atoms);
        a.compound.brace_expansion_hint = has_brace_open && has_brace_close && has_comma;
        a.compound.glob_hint = has_glob;
        return a;
    }

    // ref: parse.rs Parser::parse_if_body
    std::vector<Stmt> parse_if_body(std::span<const IfTok> until, SubshellKind sub) {
        std::vector<Stmt> ret;
        auto stop = [&]() {
            if (auto it = if_tok_here(sub)) {
                for (IfTok u : until)
                    if (u == *it) return true;
            }
            if (peek_tag() == Tok::Eof) return true;
            if (sub == SubshellKind::CmdSubst && peek_tag() == Tok::CmdSubstEnd) return true;
            if (sub == SubshellKind::Normal && peek_tag() == Tok::CloseParen) return true;
            return false;
        };
        while (!stop()) {
            skip_newlines();
            if (stop()) break;
            ret.push_back(parse_stmt(sub));
            skip_newlines();
        }
        return ret;
    }

    bool match_if_tok(IfTok tok, SubshellKind sub) {
        if (peek_tag() == Tok::Text && delimits(peek_n_tag(1), sub) &&
            if_tok_from_text(cur_text()) == tok) {
            advance();
            advance();  // expect_delimit
            return true;
        }
        return false;
    }

    // ref: parse.rs Parser::parse_if_clause
    If parse_if_clause(SubshellKind sub) {
        match_if_tok(IfTok::If, sub);
        If node;
        {
            std::array<IfTok, 1> until{IfTok::Then};
            node.cond = parse_if_body(until, sub);
        }
        if (!match_if_tok(IfTok::Then, sub))
            fail(std::string("Expected \"then\" but got: ") + std::string(tok_human(peek_tag())));
        {
            std::array<IfTok, 3> until{IfTok::Else, IfTok::Elif, IfTok::Fi};
            node.then_ = parse_if_body(until, sub);
        }

        auto here = if_tok_here(sub);
        if (!here)
            fail(std::string("Expected \"else\", \"elif\", or \"fi\" but got: ") +
                 std::string(tok_human(peek_tag())));

        if (*here == IfTok::If || *here == IfTok::Then) {
            fail(std::string("Expected \"else\", \"elif\", or \"fi\" but got: ") +
                 std::string(tok_human(peek_tag())));
        } else if (*here == IfTok::Else) {
            match_if_tok(IfTok::Else, sub);
            std::array<IfTok, 1> until{IfTok::Fi};
            auto else_ = parse_if_body(until, sub);
            if (!match_if_tok(IfTok::Fi, sub))
                fail(std::string("Expected \"fi\" but got: ") + std::string(tok_human(peek_tag())));
            node.else_parts.push_back(std::move(else_));
        } else if (*here == IfTok::Elif) {
            while (true) {
                match_if_tok(IfTok::Elif, sub);
                std::array<IfTok, 1> then_until{IfTok::Then};
                auto elif_cond = parse_if_body(then_until, sub);
                if (!match_if_tok(IfTok::Then, sub))
                    fail(std::string("Expected \"then\" but got: ") +
                         std::string(tok_human(peek_tag())));
                std::array<IfTok, 3> body_until{IfTok::Elif, IfTok::Else, IfTok::Fi};
                auto then_part = parse_if_body(body_until, sub);
                node.else_parts.push_back(std::move(elif_cond));
                node.else_parts.push_back(std::move(then_part));
                auto nxt = if_tok_here(sub);
                if (!nxt) break;
                if (*nxt == IfTok::Elif) continue;
                if (*nxt == IfTok::Else) {
                    match_if_tok(IfTok::Else, sub);
                    std::array<IfTok, 1> until{IfTok::Fi};
                    node.else_parts.push_back(parse_if_body(until, sub));
                    break;
                }
                break;
            }
            if (!match_if_tok(IfTok::Fi, sub))
                fail(std::string("Expected \"fi\" but got: ") + std::string(tok_human(peek_tag())));
        } else {  // Fi
            match_if_tok(IfTok::Fi, sub);
        }
        return node;
    }

    // ref: parse.rs Parser::parse_cond_expr
    CondExpr parse_cond_expr(SubshellKind sub) {
        advance();  // DoubleBracketOpen
        if (peek_tag() == Tok::Text) {
            std::string_view txt = cur_text();
            if (!txt.empty() && txt[0] == '-') {
                for (const auto& e : COND_SINGLE_ARG) {
                    if (txt == e.name) {
                        if (!e.supported)
                            fail(std::string("Conditional expression operation: ") +
                                 std::string(e.name) +
                                 ", is not supported right now. Please open a GitHub issue if "
                                 "you would like it to be supported.");
                        advance();  // Text op
                        if (!matchTok(Tok::Delimit)) fail("Expected a single, simple word");
                        auto arg = parse_atom(sub);
                        if (!arg)
                            fail(std::string("Expected a word, but got: ") +
                                 std::string(tok_human(peek_tag())));
                        if (!matchTok(Tok::DoubleBracketClose))
                            fail(std::string("Expected \"]]\" but got: ") +
                                 std::string(tok_human(peek_tag())));
                        CondExpr ce;
                        ce.op = std::string(e.name);
                        ce.args.push_back(*arg);
                        return ce;
                    }
                }
                fail(std::string("Unknown conditional expression operation: ") + std::string(txt));
            }
        }
        auto arg1 = parse_atom(sub);
        if (!arg1)
            fail(std::string("Expected a conditional expression operand, but got: ") +
                 std::string(tok_human(peek_tag())));
        if (peek_tag() != Tok::Text)
            fail(std::string("Expected a conditional expression operator, but got: ") +
                 std::string(tok_human(peek_tag())));
        std::string_view op = cur_text();
        advance();
        if (!matchTok(Tok::Delimit)) fail("Expected a single, simple word");
        for (const auto& e : COND_BINARY) {
            if (op == e.name) {
                if (!e.supported)
                    fail(std::string("Conditional expression operation: ") + std::string(e.name) +
                         ", is not supported right now. Please open a GitHub issue if you would "
                         "like it to be supported.");
                auto arg2 = parse_atom(sub);
                if (!arg2)
                    fail(std::string("Expected a word, but got: ") +
                         std::string(tok_human(peek_tag())));
                if (!matchTok(Tok::DoubleBracketClose))
                    fail(std::string("Expected \"]]\" but got: ") +
                         std::string(tok_human(peek_tag())));
                CondExpr ce;
                ce.op = std::string(e.name);
                ce.args.push_back(*arg1);
                ce.args.push_back(*arg2);
                return ce;
            }
        }
        fail(std::string("Unknown conditional expression operation: ") + std::string(op));
    }
};

}  // namespace mbun::shell
