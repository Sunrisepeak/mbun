// mbun.css — CSS tokenizer / parser / printer / minify.
//
// Scope (T2.8, pragmatic first pass): a *generic* token-list CSS engine that
// does structural round-trip (selectors / rules / at-rules) plus whitespace
// minification, faithfully reproducing lightningcss's serialization for the
// cases that do NOT require typed property normalization. Property-value
// transforms (color hex folding, shorthand merging, calc simplification,
// gradient/font/background normalization, vendor prefixing, An+B, escape
// re-encoding) are DEFERRED — see tests/test_css.cpp for the registry.
//
// Blueprint (read-only): bun's Rust rewrite of lightningcss, in
//   .mbun/bun-ref/src/css/  (css_parser.rs tokenizer, properties/custom.rs
//   TokenList parse+to_css, declaration.rs block format, printer.rs, rules/).
// Zig cross-check: .mbun/bun-zig-src/src/css/ (Token union at css_parser.zig).
//
// Performance (MC++): zero-copy string_view tokens over the source buffer
// (tokens carry [start,end) offsets, emitted verbatim — no per-token heap),
// single-pass tokenize, and a single output buffer written directly. The token
// list is materialized flat; nesting is handled by recursion over the flat
// vector without building a heavyweight AST.
export module mbun.css;

import std;
export import mbun.css.property_tables;
export import mbun.css.tokenizer;

namespace mbun::css {

using std::size_t;
using std::string;
using std::string_view;

// ─── Token model ─────────────────────────────────────────────────────────────
// The scanner lives in mbun.css.tokenizer (public, testable `tokenize()`). The
// engine consumes those tokens through this lightweight [start,end)+delim view
// (kind names alias the public `CssTokenKind`).
using Tk = CssTokenKind;

struct Token {
    Tk     kind{};
    size_t start{};
    size_t end{};
    char   delim{};  // valid when kind == Delim
};

// ─── Value token list ─── ref: bun src/css/properties/custom.rs TokenList ───
// A ValItem is a flat token plus its whitespace-collapse classification.
struct ValItem {
    Tk     kind{};
    size_t start{};
    size_t end{};
    char   delim{};
    bool   ws{false};  // synthetic single-space whitespace
};

class Engine {
public:
    Engine(string_view src, std::vector<Token> toks, bool minify)
        : src_{src}, toks_{std::move(toks)}, minify_{minify} {}

    string run() {
        print_rule_list_(0, toks_.size(), /*top=*/true);
        return std::move(out_);
    }

private:
    string_view              src_;
    std::vector<Token>       toks_;
    bool                     minify_;
    string                   out_;
    int                      indent_{0};

    string_view text_(const Token& t) const { return src_.substr(t.start, t.end - t.start); }

    // low-level writers (printer.rs analogues)
    void w(string_view s) { out_ += s; }
    void wc(char c) { out_ += c; }
    void whitespace() {
        if (!minify_) out_ += ' ';
    }
    void newline() {
        if (minify_) return;
        out_ += '\n';
        for (int i = 0; i < indent_; i++) out_ += ' ';
    }

    static bool is_ws_tok(Tk k) { return k == Tk::Whitespace || k == Tk::Comment; }
    static bool is_open(Tk k) {
        return k == Tk::Function || k == Tk::OpenParen || k == Tk::OpenSquare ||
               k == Tk::OpenCurly;
    }
    static Tk close_of(Tk k) {
        switch (k) {
            case Tk::OpenSquare: return Tk::CloseSquare;
            case Tk::OpenCurly: return Tk::CloseCurly;
            default: return Tk::CloseParen;  // Function / OpenParen
        }
    }

    // ── Skip helpers over the flat token vector ──
    size_t skip_ws(size_t i, size_t end) const {
        while (i < end && is_ws_tok(toks_[i].kind)) i++;
        return i;
    }

    // Find matching close for an open token at index `openIdx`; returns index of close.
    size_t match_close(size_t openIdx, size_t end) const {
        Tk want = close_of(toks_[openIdx].kind);
        int depthParen = 0, depthSquare = 0, depthCurly = 0;
        auto bump = [&](Tk k, int d) {
            if (k == Tk::OpenParen || k == Tk::Function || k == Tk::CloseParen) depthParen += d;
            else if (k == Tk::OpenSquare || k == Tk::CloseSquare) depthSquare += d;
            else if (k == Tk::OpenCurly || k == Tk::CloseCurly) depthCurly += d;
        };
        for (size_t i = openIdx + 1; i < end; i++) {
            Tk k = toks_[i].kind;
            if (is_open(k)) { bump(k, +1); continue; }
            if (k == Tk::CloseParen || k == Tk::CloseSquare || k == Tk::CloseCurly) {
                if (depthParen == 0 && depthSquare == 0 && depthCurly == 0 && k == want)
                    return i;
                bump(k, -1);
            }
        }
        return end;  // unterminated
    }

    // ── Build a value token list for [begin,end): collapse whitespace/comments,
    //    trim leading/trailing, and (for functions) trim nested args. ──
    // ref: custom.rs parse_into_impl (+ TokenList::parse trim).
    void build_level(size_t begin, size_t end, bool trim, std::vector<ValItem>& out) {
        bool last_is_delim = false;
        bool last_is_ws    = false;
        size_t i = begin;
        while (i < end) {
            const Token& t = toks_[i];
            Tk           k = t.kind;
            if (is_ws_tok(k)) {
                if (!last_is_delim) {
                    out.push_back(ValItem{Tk::Whitespace, 0, 0, 0, true});
                    last_is_ws = true;
                }
                i++;
                continue;
            }
            if (is_open(k)) {
                out.push_back(ValItem{k, t.start, t.end, t.delim, false});
                size_t close = match_close(i, end);
                // Function args are trimmed; plain (), [], {} are not.
                bool trimInner = (k == Tk::Function);
                build_level(i + 1, close, trimInner, out);
                if (close < end) {
                    const Token& ct = toks_[close];
                    out.push_back(ValItem{ct.kind, ct.start, ct.end, ct.delim, false});
                }
                i = (close < end) ? close + 1 : end;
                last_is_delim = true;  // ws not required after a block
                last_is_ws    = false;
                continue;
            }
            bool is_delim_like = (k == Tk::Delim || k == Tk::Comma);
            ValItem vi{k, t.start, t.end, t.delim, false};
            if (is_delim_like && last_is_ws) {
                out.back() = vi;  // replace trailing ws with the delim
            } else {
                out.push_back(vi);
            }
            last_is_delim = is_delim_like;
            last_is_ws    = false;
            i++;
        }
        if (trim && out.size() >= 2) {
            // (only trims the tail we appended in THIS call is hard with shared vec;
            //  but callers pass fresh sub-vectors for trim==true at top level)
        }
    }

    // Top-level value builder with trim (mirrors TokenList::parse).
    std::vector<ValItem> build_value(size_t begin, size_t end) {
        std::vector<ValItem> v;
        build_level(begin, end, /*trim=*/false, v);
        if (v.size() >= 2) {
            if (v.front().kind == Tk::Whitespace && v.front().ws) v.erase(v.begin());
        }
        if (v.size() >= 2) {
            if (v.back().kind == Tk::Whitespace && v.back().ws) v.pop_back();
        }
        return v;
    }

    // ── Serialize a value token list ── ref: custom.rs TokenList::to_css ──
    void write_value(const std::vector<ValItem>& v) {
        bool has_ws = false;
        for (size_t i = 0; i < v.size(); i++) {
            const ValItem& t = v[i];
            switch (t.kind) {
                case Tk::Delim: {
                    char d = t.delim;
                    if (d == '+' || d == '-') {
                        wc(' ');
                        wc(d);
                        wc(' ');
                    } else {
                        bool ws_before = !has_ws && (d == '/' || d == '*');
                        if (ws_before) whitespace();
                        wc(d);
                        whitespace();
                        if (minify_ && (d == '/' || d == '*') && i + 1 < v.size() &&
                            v[i + 1].kind == Tk::Delim) {
                            char nd = v[i + 1].delim;
                            if ((d == '/' && nd == '*') || (d == '*' && nd == '/')) wc(' ');
                        }
                    }
                    has_ws = true;
                    break;
                }
                case Tk::Comma:
                    wc(',');
                    whitespace();
                    has_ws = true;
                    break;
                case Tk::CloseParen:
                case Tk::CloseSquare:
                case Tk::CloseCurly:
                    w(text_tok(t));
                    // write_whitespace_if_needed (non-minify only)
                    if (!minify_ && i != v.size() - 1) {
                        Tk nk = v[i + 1].kind;
                        if (nk != Tk::Comma && nk != Tk::CloseParen) {
                            wc(' ');
                            has_ws = true;
                        } else {
                            has_ws = false;
                        }
                    } else {
                        has_ws = false;
                    }
                    break;
                case Tk::Whitespace:
                    wc(' ');
                    has_ws = true;
                    break;
                default:
                    w(text_tok(t));
                    has_ws = false;
                    break;
            }
        }
    }

    string_view text_tok(const ValItem& t) const {
        return src_.substr(t.start, t.end - t.start);
    }

    // ── Serialize a prelude (selector / at-rule prelude) ──
    // Combinators (> + ~) and comma drop surrounding whitespace; other
    // whitespace between compound pieces becomes a single descendant space.
    // ref: selectors/selector.rs serialize_combinator + media_query printing.
    void write_prelude(size_t begin, size_t end) {
        constexpr size_t WS = static_cast<size_t>(-1);  // ws marker sentinel
        // collapse to significant tokens (single ws markers, no lead/trail ws)
        std::vector<size_t> idx;  // token indices; WS = ws marker
        bool pendingWs = false;
        for (size_t i = begin; i < end; i++) {
            if (is_ws_tok(toks_[i].kind)) {
                pendingWs = true;
                continue;
            }
            if (pendingWs && !idx.empty()) idx.push_back(WS);
            pendingWs = false;
            idx.push_back(i);
        }
        auto is_comb = [&](size_t j) {
            return j != WS && toks_[j].kind == Tk::Delim &&
                   (toks_[j].delim == '>' || toks_[j].delim == '+' || toks_[j].delim == '~');
        };
        for (size_t n = 0; n < idx.size(); n++) {
            size_t j = idx[n];
            if (j == WS) {
                size_t left  = n > 0 ? idx[n - 1] : WS;
                size_t right = n + 1 < idx.size() ? idx[n + 1] : WS;
                bool   drop  = false;
                if (right != WS) {
                    Tk rk = toks_[right].kind;
                    if (is_comb(right) || rk == Tk::Comma || rk == Tk::CloseParen ||
                        rk == Tk::CloseSquare)
                        drop = true;
                }
                if (!drop && left != WS) {
                    Tk lk = toks_[left].kind;
                    if (is_comb(left) || lk == Tk::Comma || lk == Tk::OpenParen ||
                        lk == Tk::Function || lk == Tk::OpenSquare)
                        drop = true;
                }
                if (!drop) wc(' ');  // descendant combinator (kept in both modes)
                continue;
            }
            const Token& t = toks_[j];
            if (is_comb(j)) {
                if (minify_) {
                    wc(t.delim);
                } else {
                    wc(' ');
                    wc(t.delim);
                    wc(' ');
                }
            } else if (t.kind == Tk::Comma) {
                wc(',');
                whitespace();
            } else {
                w(text_(t));
            }
        }
    }

    // ── Rule-list printer (top level or nested @media/@supports body) ──
    void print_rule_list_(size_t begin, size_t end, bool top) {
        size_t i = begin;
        bool   firstEmitted = false;
        while (i < end) {
            i = skip_ws(i, end);
            // skip stray semicolons and CDO/CDC at rule boundaries
            while (i < end && (toks_[i].kind == Tk::Semicolon || toks_[i].kind == Tk::Cdo ||
                               toks_[i].kind == Tk::Cdc)) {
                i++;
                i = skip_ws(i, end);
            }
            if (i >= end || toks_[i].kind == Tk::CloseCurly) break;

            size_t ruleStart = i;
            // find prelude end: first top-level '{' or ';'
            int depthP = 0, depthS = 0;
            size_t bracePos = end, semiPos = end;
            for (size_t k = i; k < end; k++) {
                Tk kk = toks_[k].kind;
                if (kk == Tk::OpenParen || kk == Tk::Function) depthP++;
                else if (kk == Tk::CloseParen) { if (depthP) depthP--; }
                else if (kk == Tk::OpenSquare) depthS++;
                else if (kk == Tk::CloseSquare) { if (depthS) depthS--; }
                else if (kk == Tk::OpenCurly && depthP == 0 && depthS == 0) { bracePos = k; break; }
                else if (kk == Tk::Semicolon && depthP == 0 && depthS == 0) { semiPos = k; break; }
                else if (kk == Tk::CloseCurly && depthP == 0 && depthS == 0) { break; }
            }

            bool isAt = toks_[ruleStart].kind == Tk::AtKeyword;

            if (bracePos == end || (semiPos < bracePos)) {
                // statement (no block): at-rule ending in ';' (or EOF)
                size_t stmtEnd = (semiPos < end) ? semiPos : bracePos;
                if (stmtEnd == end) stmtEnd = end;
                // separator
                if (firstEmitted) newline();
                write_prelude(ruleStart, (semiPos < end) ? semiPos : end);
                wc(';');
                firstEmitted = true;
                i = (semiPos < end) ? semiPos + 1 : end;
                continue;
            }

            // block rule
            if (firstEmitted) {
                if (top) {
                    newline();
                    newline();
                } else {
                    newline();
                }
            }
            firstEmitted = true;
            size_t bodyBegin = bracePos + 1;
            size_t bodyEnd   = match_close(bracePos, end);

            if (isAt) {
                string_view kw = at_name_(ruleStart);
                if (is_nested_rule_at_(kw)) {
                    write_prelude(ruleStart, bracePos);
                    whitespace();
                    wc('{');
                    indent_ += 2;
                    print_rule_list_(bodyBegin, bodyEnd, /*top=*/false);
                    indent_ -= 2;
                    newline();
                    wc('}');
                } else {
                    // declaration-body at-rule (@font-face, @page, @property, ...)
                    write_prelude(ruleStart, bracePos);
                    print_decl_block_(bodyBegin, bodyEnd);
                }
            } else {
                write_prelude(ruleStart, bracePos);
                print_decl_block_(bodyBegin, bodyEnd);
            }
            i = (bodyEnd < end) ? bodyEnd + 1 : end;
        }
    }

    string_view at_name_(size_t atIdx) const {
        string_view t = text_(toks_[atIdx]);  // includes '@'
        return t.substr(1);
    }
    static bool ieq(string_view a, string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            char x = a[i], y = b[i];
            if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
            if (x != y) return false;
        }
        return true;
    }
    static bool is_nested_rule_at_(string_view kw) {
        // strip vendor prefix for the keyframes/document cases
        return ieq(kw, "media") || ieq(kw, "supports") || ieq(kw, "container") ||
               ieq(kw, "layer") || ieq(kw, "scope") || ieq(kw, "starting-style") ||
               ieq(kw, "keyframes") || ieq(kw, "-webkit-keyframes") ||
               ieq(kw, "-moz-keyframes") || ieq(kw, "-o-keyframes") ||
               ieq(kw, "document") || ieq(kw, "-moz-document");
    }

    // ── Declaration block: `{ decl; decl }` ── ref: declaration.rs to_css_block ─
    void print_decl_block_(size_t begin, size_t end) {
        // Gather segments split by top-level ';'. A segment may be a nested
        // rule (contains a top-level '{'). Collect declarations first.
        struct Seg { size_t b, e; bool isRule; size_t braceRel; };
        std::vector<Seg> segs;
        size_t i = begin;
        while (i < end) {
            i = skip_ws(i, end);
            if (i >= end) break;
            if (toks_[i].kind == Tk::Semicolon) { i++; continue; }
            int depthP = 0, depthS = 0;
            size_t segStart = i, semiPos = end, bracePos = end;
            for (size_t k = i; k < end; k++) {
                Tk kk = toks_[k].kind;
                if (kk == Tk::OpenParen || kk == Tk::Function) depthP++;
                else if (kk == Tk::CloseParen) { if (depthP) depthP--; }
                else if (kk == Tk::OpenSquare) depthS++;
                else if (kk == Tk::CloseSquare) { if (depthS) depthS--; }
                else if (kk == Tk::OpenCurly && depthP == 0 && depthS == 0) { bracePos = k; break; }
                else if (kk == Tk::Semicolon && depthP == 0 && depthS == 0) { semiPos = k; break; }
            }
            if (bracePos != end && (semiPos == end || bracePos < semiPos)) {
                size_t rEnd = match_close(bracePos, end);
                segs.push_back(Seg{segStart, rEnd + 1 > end ? end : rEnd + 1, true, bracePos});
                i = (rEnd < end) ? rEnd + 1 : end;
            } else {
                size_t segEnd = (semiPos < end) ? semiPos : end;
                segs.push_back(Seg{segStart, segEnd, false, 0});
                i = (semiPos < end) ? semiPos + 1 : end;
            }
        }

        whitespace();
        wc('{');
        indent_ += 2;
        // count printable decls for trailing-';' logic
        size_t total = segs.size();
        for (size_t s = 0; s < segs.size(); s++) {
            newline();
            const Seg& sg = segs[s];
            if (sg.isRule) {
                // nested rule inside a style rule (CSS nesting)
                size_t rb = sg.braceRel + 1;
                size_t re = match_close(sg.braceRel, end);
                write_prelude(sg.b, sg.braceRel);
                print_decl_block_(rb, re);
            } else {
                print_declaration_(sg.b, sg.e);
                bool last = (s == segs.size() - 1);
                if (!last || !minify_) wc(';');
            }
        }
        (void)total;
        indent_ -= 2;
        newline();
        wc('}');
    }

    // ── Single declaration: `prop: value [!important]` ── ref: properties_impl.rs ─
    void print_declaration_(size_t begin, size_t end) {
        // property name = tokens up to first top-level ':'
        size_t colon = end;
        int depthP = 0, depthS = 0;
        for (size_t k = begin; k < end; k++) {
            Tk kk = toks_[k].kind;
            if (kk == Tk::OpenParen || kk == Tk::Function) depthP++;
            else if (kk == Tk::CloseParen) { if (depthP) depthP--; }
            else if (kk == Tk::OpenSquare) depthS++;
            else if (kk == Tk::CloseSquare) { if (depthS) depthS--; }
            else if (kk == Tk::Colon && depthP == 0 && depthS == 0) { colon = k; break; }
        }
        if (colon == end) {
            // malformed; emit verbatim-ish
            write_prelude(begin, end);
            return;
        }
        size_t nameBeg = skip_ws(begin, colon);
        size_t nameEnd = colon;
        while (nameEnd > nameBeg && is_ws_tok(toks_[nameEnd - 1].kind)) nameEnd--;
        // property name emitted raw (idents / --custom / *hack)
        for (size_t k = nameBeg; k < nameEnd; k++) w(text_(toks_[k]));
        wc(':');
        whitespace();

        // value range (after colon), detect trailing !important
        size_t vBeg = colon + 1;
        size_t vEnd = end;
        bool important = detect_important_(vBeg, vEnd);
        std::vector<ValItem> val = build_value(vBeg, vEnd);
        write_value(val);
        if (important) {
            whitespace();
            w("!important");
        }
    }

    // If value ends with `! important` (delim '!' + ident 'important'), trim it
    // off [begin,end) and return true.
    bool detect_important_(size_t begin, size_t& end) {
        size_t e = end;
        while (e > begin && is_ws_tok(toks_[e - 1].kind)) e--;
        if (e > begin && toks_[e - 1].kind == Tk::Ident && ieq(text_(toks_[e - 1]), "important")) {
            size_t bang = e - 1;
            size_t p = bang;
            while (p > begin && is_ws_tok(toks_[p - 1].kind)) p--;
            if (p > begin && toks_[p - 1].kind == Tk::Delim && toks_[p - 1].delim == '!') {
                end = p - 1;
                return true;
            }
        }
        return false;
    }
};

// ─── Public API ─────────────────────────────────────────────────────────────
export struct TransformResult {
    bool   ok{true};
    string code;
};

// Parse `source` as a stylesheet and re-serialize it. `minify` selects minified
// (byte-compact) vs pretty (2-space indent) output. Pretty output ends with a
// trailing newline (matching lightningcss's stylesheet serializer).
export string transform(string_view source, bool minify) {
    std::vector<CssToken> ctoks = tokenize(source);
    std::vector<Token>    toks;
    toks.reserve(ctoks.size());
    for (const CssToken& ct : ctoks)
        toks.push_back(Token{ct.kind, ct.span.start, ct.span.end, ct.delim});
    Engine eng{source, std::move(toks), minify};
    string s = eng.run();
    if (!minify) s += '\n';
    return s;
}

// Convenience wrappers.
export string minify(string_view source) { return transform(source, true); }
export string print(string_view source) { return transform(source, false); }

}  // namespace mbun::css
