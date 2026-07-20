// src/js_parser/token_cursor.cppm — module mbun.js_parser.token_cursor
//
// The token layer the whole parser stands on: the pre-lexed token vector, the
// cursor over it, the ">"-run sub-cursor, the sticky error state, and the arena
// the erasure edits accumulate in.
//
// Why this is a module of its own (AGENTS.md 规则 10 — split by 职责): measured
// against the rest of the parser, this layer has **no outbound calls at all** —
// nothing in here reaches up into statement, expression, class or type parsing.
// That makes it a true base layer rather than one side of a cycle, so it is a
// plain base class, not a CRTP mixin: `Parser` inherits `cur_()`, `advance_()`,
// `expect_()` &c. and every existing call site keeps compiling untouched. (The
// CRTP shape js_printer uses is only needed where recursion is *mutual*, which
// is the statement/expression core — see js_parser.cppm.)
//
// Contents mirror the pipeline: pre_lex_ (the whole source → `toks_`, handing
// JSX off to mbun.js_parser.jsx_lower), the cursor + lookahead, the
// expect/fail diagnostics, save_/restore_ backtracking, and the pure token
// predicates (is_keyword_, regex_allowed_after_, effective_kind_, …).
export module mbun.js_parser.token_cursor;

import std;
import mbun.js_lexer;
import mbun.ast;
import mbun.js_parser.jsx_lower;

export namespace mbun::js_parser::detail {

using mbun::js_lexer::Token;
using mbun::ast::Arena;
using mbun::ast::Node;
using mbun::ast::NodeIndex;
using mbun::ast::NodeKind;
using mbun::ast::NONE;
using mbun::ast::VarKind;

// Result of the type-argument follow-token classification.
enum class Follow { No, Call, Tagged, Instantiation };

// A pre-lexed token snapshot.
struct Tok {
    Token kind{Token::EndOfFile};
    std::uint32_t start{0};
    std::uint32_t end{0};
    std::string_view raw;   // slice of the source
    std::string ident;      // decoded identifier / #name / bigint digits (if any)
    // A string literal's VALUE, i.e. its escapes already decoded: for `'\x41'`
    // the value is the single character `A`, not the four source bytes. Held as
    // a 1-biased index into `TokenCursor::strPool_` (0 = none) rather than
    // inline, so a token that is not a string costs 4 bytes instead of a whole
    // empty std::u16string. The lexer decodes this for us already; before this
    // field the value simply had nowhere to live and was dropped on the floor.
    std::uint32_t strIdx{0};
    bool newlineBefore{false};
    bool jsx{false};        // synthetic JSX element token; `ident` holds the lowered
                            // `React.createElement(...)` replacement to edit in.
};


// Runs a restore action on scope exit, so parser context saved around a
// sub-parse is put back even on the `return NONE` error paths.
template <typename F>
class ScopeGuard {
private:
    F onExit_;

public:
    explicit ScopeGuard(F onExit) : onExit_{std::move(onExit)} {}
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ~ScopeGuard() { onExit_(); }
};

// The token cursor + error state + edit arena. A base class, not a mixin: see
// the header — this layer never calls back up into the parser.
class TokenCursor {
protected:
    // ── state ────────────────────────────────────────────────────────────────
    std::string_view src_;
    JsxLowerer jsxLower_;  // JSX scanning/lowering — mbun.js_parser.jsx_lower
    std::vector<Tok> toks_;
    // Decoded string-literal values, referenced 1-biased by `Tok::strIdx`. Side
    // storage so the common (non-string) token pays only the 4-byte index.
    std::vector<std::u16string> strPool_;
    std::size_t idx_{0};
    unsigned gtOffset_{0};  // sub-cursor into a ">"-run at toks_[idx_]

    bool ok_{true};
    std::string errMsg_;
    std::size_t errOff_{0};

    Arena arena_;
    bool jsx_{false};          // TSX/JSX input — lower JSX elements to createElement

public:
    TokenCursor(std::string_view src, bool jsx, JsxOptions jsxOpts = {})
        : src_{src}, jsxLower_{src, std::move(jsxOpts)}, jsx_{jsx} {}

    // Which automatic-runtime symbols the lowering emitted, for the caller that
    // has to import them. Empty unless `jsx_` and the automatic runtime.
    [[nodiscard]] const JsxUsed& jsx_used() const { return jsxLower_.used(); }
    [[nodiscard]] const JsxOptions& jsx_options() const { return jsxLower_.options(); }

    struct Save {
        std::size_t idx;
        unsigned gt;
        std::size_t edits;
    };
    Save save_() const { return Save{idx_, gtOffset_, arena_.edit_count()}; }
    void restore_(Save s) {
        idx_ = s.idx;
        gtOffset_ = s.gt;
        arena_.truncate_edits(s.edits);
    }

    // Byte offset just past the previously-consumed token (end of the last real
    // token before the cursor). Used to bound an erasure span that ends at the
    // token before the current one.
    std::uint32_t prev_end_() const {
        return idx_ > 0 ? tok_at_(idx_ - 1).end : 0;
    }

    // Kind of the token before the cursor (EndOfFile at the start of input).
    Token prev_kind_() const {
        return idx_ > 0 ? tok_at_(idx_ - 1).kind : Token::EndOfFile;
    }

    void erase_current_token_() {
        arena_.add_edit(cur_().start, cur_().end);
        advance_();
    }

    // Whether a `/` at this position begins a regular-expression literal (rather
    // than division), based on the previous significant token: a regex may follow
    // anything that is NOT a value-producing token.
    static bool regex_allowed_after_(Token prev) {
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

    // ── pre-lexing ───────────────────────────────────────────────────────────
    bool pre_lex_() {
        mbun::js_lexer::Lexer lexer{src_};
        std::vector<char> braceStack;  // 't' = template substitution, '{' = block
        Token prevSig{Token::EndOfFile};  // previous significant token (start → regex ok)
        while (true) {
            auto step = lexer.next();
            if (!step) {
                errMsg_ = lexer.diagnostics().empty() ? std::string{"Parse error"}
                                                      : lexer.diagnostics().back().message;
                errOff_ = lexer.diagnostics().empty() ? lexer.start()
                                                      : lexer.diagnostics().back().offset;
                return false;
            }
            Token t = lexer.token();
            if (t == Token::EndOfFile) {
                break;
            }
            // JSX element in expression position (TSX/JSX). The JS tokenizer cannot
            // lex JSX (a `</` close tag and text runs would derail it), so when a
            // `<` appears where an expression may start we hand-scan the whole
            // element from the raw source, emit ONE synthetic token carrying the
            // lowered `React.createElement(...)`, and resume lexing past it.
            if (jsx_ && t == Token::LessThan && regex_allowed_after_(prevSig) &&
                jsxLower_.jsx_element_starts_at_(lexer.end())) {
                std::size_t jsxStart = lexer.start();
                std::size_t jsxEnd = jsxStart;
                std::string lowered;
                if (!jsxLower_.jsx_parse_element_(jsxEnd, lowered)) {
                    errMsg_ = "Unexpected token in JSX";
                    errOff_ = jsxStart;
                    return false;
                }
                Tok jt;
                jt.kind = Token::LessThan;
                jt.start = static_cast<std::uint32_t>(jsxStart);
                jt.end = static_cast<std::uint32_t>(jsxEnd);
                jt.raw = src_.substr(jsxStart, jsxEnd - jsxStart);
                jt.ident = std::move(lowered);
                jt.jsx = true;
                jt.newlineBefore = lexer.has_newline_before();
                toks_.push_back(std::move(jt));
                lexer.seek(jsxEnd);
                prevSig = Token::CloseParen;  // JSX is a value: `/` divides, `<` compares
                continue;
            }
            // Regex vs division: re-scan a `/`/`/=` as a regex literal when a regex
            // is allowed after the previous token.
            if ((t == Token::Slash || t == Token::SlashEquals) && regex_allowed_after_(prevSig)) {
                if (!lexer.scan_regexp()) {
                    errMsg_ = lexer.diagnostics().empty() ? std::string{"Parse error"}
                                                          : lexer.diagnostics().back().message;
                    errOff_ = lexer.start();
                    return false;
                }
                t = lexer.token();
            }
            if (t == Token::OpenBrace) {
                braceStack.push_back('{');
            } else if (t == Token::CloseBrace) {
                if (!braceStack.empty() && braceStack.back() == 't') {
                    braceStack.pop_back();
                    if (!lexer.rescan_close_brace_as_template_token()) {
                        errMsg_ = lexer.diagnostics().empty() ? std::string{"Parse error"}
                                                              : lexer.diagnostics().back().message;
                        errOff_ = lexer.start();
                        return false;
                    }
                    t = lexer.token();
                } else if (!braceStack.empty()) {
                    braceStack.pop_back();
                }
            }
            if (t == Token::TemplateHead || t == Token::TemplateMiddle) {
                braceStack.push_back('t');
            }
            push_tok_(lexer, t);
            prevSig = t;
        }
        Tok eof;
        eof.kind = Token::EndOfFile;
        eof.start = static_cast<std::uint32_t>(src_.size());
        eof.end = static_cast<std::uint32_t>(src_.size());
        eof.newlineBefore = lexer.has_newline_before();
        toks_.push_back(std::move(eof));
        return true;
    }

    void push_tok_(const mbun::js_lexer::Lexer& lexer, Token t) {
        Tok tok;
        tok.kind = t;
        tok.start = static_cast<std::uint32_t>(lexer.start());
        tok.end = static_cast<std::uint32_t>(lexer.end());
        tok.raw = lexer.raw();
        tok.newlineBefore = lexer.has_newline_before();
        switch (t) {
        case Token::Identifier:
        case Token::EscapedKeyword:
        case Token::PrivateIdentifier:
        case Token::BigIntegerLiteral:
        case Token::Hashbang:
            tok.ident.assign(lexer.identifier());
            break;
        case Token::StringLiteral:
            // The lexer already decoded every escape into UTF-16 while scanning
            // (js_lexer.cppm:1280 `decode_escape`); keep the result instead of
            // re-deriving it from `raw`, which cannot be done correctly anyway.
            if (auto v = lexer.string_literal_utf16()) {
                strPool_.push_back(std::move(*v));
                tok.strIdx = static_cast<std::uint32_t>(strPool_.size());  // 1-biased
            }
            break;
        default:
            break;
        }
        toks_.push_back(std::move(tok));
    }

    // ── cursor / gt-splitting ────────────────────────────────────────────────
    const Tok& tok_at_(std::size_t i) const {
        return i < toks_.size() ? toks_[i] : toks_.back();
    }
    const Tok& cur_() const { return tok_at_(idx_); }

    static Token effective_kind_(Token k, unsigned gt) {
        if (gt == 0) {
            return k;
        }
        switch (k) {
        case Token::GreaterThanGreaterThan:  // ">>"
            return Token::GreaterThan;
        case Token::GreaterThanGreaterThanGreaterThan:  // ">>>"
            return gt == 1 ? Token::GreaterThanGreaterThan : Token::GreaterThan;
        case Token::GreaterThanEquals:  // ">="
            return Token::Equals;
        case Token::GreaterThanGreaterThanEquals:  // ">>="
            return gt == 1 ? Token::GreaterThanEquals : Token::Equals;
        case Token::GreaterThanGreaterThanGreaterThanEquals:  // ">>>="
            return gt == 1   ? Token::GreaterThanGreaterThanEquals
                   : gt == 2 ? Token::GreaterThanEquals
                             : Token::Equals;
        default:
            return k;
        }
    }

    Token curk_() const { return effective_kind_(cur_().kind, gtOffset_); }
    Token peek_kind_(std::size_t ahead) const {
        // Only valid when the current token is not mid-gt-split (ahead>0 skips
        // whole tokens). Callers use this for simple lookahead.
        return tok_at_(idx_ + ahead).kind;
    }

    void advance_() {
        if (idx_ + 1 < toks_.size()) {
            ++idx_;
        }
        gtOffset_ = 0;
    }

    static bool is_gt_family_(Token k) {
        switch (k) {
        case Token::GreaterThan:
        case Token::GreaterThanGreaterThan:
        case Token::GreaterThanGreaterThanGreaterThan:
        case Token::GreaterThanEquals:
        case Token::GreaterThanGreaterThanEquals:
        case Token::GreaterThanGreaterThanGreaterThanEquals:
            return true;
        default:
            return false;
        }
    }

    // Consume exactly one '>' from the current ">"-run; returns false if the
    // current effective token does not start with '>'.
    bool eat_gt_() {
        Token e = curk_();
        if (e == Token::GreaterThan) {
            advance_();
            return true;
        }
        if (is_gt_family_(e)) {
            ++gtOffset_;
            return true;
        }
        return false;
    }

    // ── diagnostics ──────────────────────────────────────────────────────────
    void fail_(std::string msg) {
        if (ok_) {
            ok_ = false;
            errMsg_ = std::move(msg);
            errOff_ = cur_().start;
        }
    }

    // ── recursion budget ─────────────────────────────────────────────────────
    // The descent parser recurses once per nesting level, so source that nests
    // deeply enough runs the NATIVE stack out before it runs out of tokens —
    // a SIGSEGV that takes the whole process down (bun's
    // `fixtures/lots-of-for-loop.js` nests ~90k `for` statements; the fuzzer
    // shapes in the transpiler suite nest 100k brackets). bun caps the descent
    // and reports the overflow as a catchable
    // "Maximum call stack size exceeded" instead of crashing.
    // ref: compat/bun/test/bundler/transpiler/transpiler.test.js
    //      "runtime transpiler stack overflows" /
    //      "deeply nested expressions error instead of crashing the process"
    // The cap is a nesting count, not a byte budget: the worst-case chain
    // (statement → expression → … → primary) is a few hundred bytes of frame
    // per level, so 1000 levels stays ~1 MB deep — safe even on the smaller
    // stacks of JSC's worker threads — while being far past anything real
    // source nests.
    static constexpr int kMaxParseDepth{1000};
    int depth_{0};

    // RAII: `DepthGuard g{this}; if (!g.ok) return NONE;` at every recursion hub.
    // Unwinds on every exit path (including the early returns of a backtracking
    // speculative parse), so the counter always tracks the live stack.
    struct DepthGuard {
        TokenCursor* p;
        bool ok;
        explicit DepthGuard(TokenCursor* self) : p{self} {
            ok = ++p->depth_ <= kMaxParseDepth;
            if (!ok) {
                p->fail_("Maximum call stack size exceeded");
            }
        }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;
        ~DepthGuard() { --p->depth_; }
    };

    std::string_view token_text_() const {
        Token e = curk_();
        if (e == Token::EndOfFile) {
            return "end of file";
        }
        const Tok& t = cur_();
        switch (e) {
        case Token::Identifier:
        case Token::EscapedKeyword:
        case Token::PrivateIdentifier:
        case Token::NumericLiteral:
        case Token::BigIntegerLiteral:
        case Token::StringLiteral:
        case Token::NoSubstitutionTemplateLiteral:
        case Token::TemplateHead:
        case Token::TemplateMiddle:
        case Token::TemplateTail:
            return t.raw;
        default:
            return mbun::js_lexer::token_to_string(e);
        }
    }

    void unexpected_() { fail_(std::format("Unexpected {}", token_text_())); }
    void expected_identifier_() {
        fail_(std::format("Expected identifier but found \"{}\"", token_text_()));
    }
    void expected_(std::string_view what) {
        fail_(std::format("Expected \"{}\" but found \"{}\"", what, token_text_()));
    }

    bool expect_(Token k) {
        if (curk_() == k) {
            advance_();
            return true;
        }
        expected_(mbun::js_lexer::token_to_string(k));
        return false;
    }

    // Consume a single '>' that closes a type-argument/parameter list.
    bool expect_gt_() {
        if (eat_gt_()) {
            return true;
        }
        expected_(">");
        return false;
    }

    static bool tok_has_escape_(const Tok& t) {
        return t.raw.find('\\') != std::string_view::npos;
    }

    // The decoded value of a string-literal token, or nullptr if this token is
    // not a string (or the lexer could not decode it).
    const std::u16string* tok_string_value_(const Tok& t) const {
        if (t.strIdx == 0 || t.strIdx > strPool_.size()) {
            return nullptr;
        }
        return &strPool_[t.strIdx - 1];
    }

    bool ident_is_(std::string_view s) const {
        return curk_() == Token::Identifier && std::string_view{cur_().ident} == s &&
               !tok_has_escape_(cur_());
    }

    static bool is_keyword_(Token k) {
        return k >= Token::Break && k <= Token::With;
    }

    // Whether `k` can name an import/export specifier: any IdentifierName
    // (plain identifier, escaped keyword, or reserved word like `default`) or a
    // module-export string literal.
    static bool is_spec_name_(Token k) {
        return k == Token::Identifier || k == Token::EscapedKeyword ||
               k == Token::StringLiteral || is_keyword_(k);
    }

    bool advance_true_() {
        advance_();
        return true;
    }
};

}  // namespace mbun::js_parser::detail
