// src/js_printer/legacy.cppm — module mbun.js_printer.legacy
//
// ⚠️ SUPERSEDED, kept because it is still the spec for tests/test_js_printer.cpp.
//
// This is the pre-CAP-BUILD-PRINTER printer: a 648-line hand-written subset that
// covers 4 statement kinds and falls back to `out_ += slice_(n)` (i.e. echoes
// the original source bytes) for everything else. It is imported by ZERO
// production code — `mbun::js_parser::transpile` never used it; it transpiles by
// erasure (original bytes + Arena::erase_slice edits) instead.
//
// It is being replaced, function by function, by the 1:1 port of bun's real
// printer in the sibling modules of this directory (mbun.js_printer.{encoding,
// quote,whitespacer,options,flags,printer_core,printer}), per AGENTS.md
// 「移植三段法」. Do not add features here. When the port covers a construct this
// file handles, delete that arm here — and when test_js_printer.cpp passes
// against the port, delete this file.
//
// Original header follows.
// ─────────────────────────────────────────────────────────────────────────────
//
// AST → source printer (T2.5). Walks the flat mbun.ast arena produced by
// mbun.js_parser and re-emits JavaScript source directly into one output buffer
// (no intermediate string tree): every node appends to `out_`, and the only
// structural bookkeeping is a numeric precedence passed down so parentheses are
// inserted minimally (bun's canonical printing). TypeScript constructs are
// already erased by the parser (type arguments, `as`, `satisfies`, annotations),
// so the printer just prints the surviving JS shape.
//
// Not a mechanical port of bun's Zig printer: it is a compact self-contained
// walker whose *output* is verified byte-for-byte against bun's expectPrinted
// expectations (the T2.5 test vectors are the spec). Literals are normalised
// where bun normalises them — string quote selection (prefer `"`, fall back to
// `'` when the text contains `"` but no `'`) and identifier/number faithful
// re-emission — while folding/minification is a later pass (PrintOptions.minify
// is wired but performs no syntax folding yet; see the test-suite header).
export module mbun.js_printer.legacy;

import std;
import mbun.ast;
import mbun.js_lexer;

export namespace mbun::js_printer {

struct PrintOptions {
    bool minify{false};
};

namespace detail {

using mbun::ast::Arena;
using mbun::ast::Node;
using mbun::ast::NodeIndex;
using mbun::ast::NodeKind;
using mbun::ast::NONE;
using mbun::ast::VarKind;
using mbun::js_lexer::Token;

// Expression precedence levels (higher binds tighter). Binary/logical operators
// occupy 10 + left-binding-power so they all sit above conditional/assignment.
inline constexpr int P_LOWEST{0};
inline constexpr int P_COMMA{1};
inline constexpr int P_ASSIGN{2};
inline constexpr int P_COND{3};
inline constexpr int P_UNARY{30};
inline constexpr int P_POSTFIX{35};
inline constexpr int P_PRIMARY{40};

class Printer {
public:
    Printer(const Arena& arena, std::string_view source, PrintOptions opts)
        : arena_{arena}, src_{source}, opts_{opts} {}

    std::string run(NodeIndex program) {
        if (program == NONE) {
            return std::move(out_);
        }
        const Node& prog = arena_.at(program);
        for (NodeIndex s : arena_.list_of(prog)) {
            print_stmt_(s);
        }
        return std::move(out_);
    }

private:
    const Arena& arena_;
    std::string_view src_;
    PrintOptions opts_;
    std::string out_;

    const Node& at_(NodeIndex i) const { return arena_.at(i); }

    std::string_view slice_(const Node& n) const {
        if (n.start <= n.end && n.end <= src_.size()) {
            return src_.substr(n.start, n.end - n.start);
        }
        return {};
    }

    // ── binding power (mirrors the parser's binary_lbp_, allowIn folded in) ────
    static int binary_bp_(Token k) {
        switch (k) {
        case Token::QuestionQuestion: return 3;
        case Token::BarBar: return 4;
        case Token::AmpersandAmpersand: return 5;
        case Token::Bar: return 6;
        case Token::Caret: return 7;
        case Token::Ampersand: return 8;
        case Token::EqualsEquals:
        case Token::ExclamationEquals:
        case Token::EqualsEqualsEquals:
        case Token::ExclamationEqualsEquals: return 9;
        case Token::LessThan:
        case Token::LessThanEquals:
        case Token::GreaterThan:
        case Token::GreaterThanEquals:
        case Token::Instanceof:
        case Token::In: return 10;
        case Token::LessThanLessThan:
        case Token::GreaterThanGreaterThan:
        case Token::GreaterThanGreaterThanGreaterThan: return 11;
        case Token::Plus:
        case Token::Minus: return 12;
        case Token::Asterisk:
        case Token::Slash:
        case Token::Percent: return 13;
        case Token::AsteriskAsterisk: return 14;
        default: return 0;
        }
    }

    int prec_(NodeIndex idx) const {
        const Node& n = at_(idx);
        switch (n.kind) {
        case NodeKind::Sequence: return P_COMMA;
        case NodeKind::Assignment:
        case NodeKind::Arrow: return P_ASSIGN;
        case NodeKind::Conditional: return P_COND;
        case NodeKind::Binary:
        case NodeKind::Logical: return 10 + binary_bp_(static_cast<Token>(n.aux));
        case NodeKind::Unary: return P_UNARY;
        case NodeKind::Update: return (n.flags & 1u) ? P_UNARY : P_POSTFIX;
        case NodeKind::Call:
        case NodeKind::New:
        case NodeKind::Member:
        case NodeKind::PrivateMember:
        case NodeKind::Index:
        case NodeKind::TaggedTemplate: return P_POSTFIX;
        case NodeKind::Paren: return n.a != NONE ? prec_(n.a) : P_PRIMARY;
        default: return P_PRIMARY;
        }
    }

    // Emit `idx`, wrapping in parentheses when its precedence is below `minPrec`.
    void emit_(NodeIndex idx, int minPrec) {
        bool paren = prec_(idx) < minPrec;
        if (paren) {
            out_.push_back('(');
        }
        print_node_(idx);
        if (paren) {
            out_.push_back(')');
        }
    }

    // ── expressions ────────────────────────────────────────────────────────────
    void print_node_(NodeIndex idx) {
        const Node& n = at_(idx);
        switch (n.kind) {
        case NodeKind::Identifier:
            out_ += n.text.empty() ? slice_(n) : n.text;
            return;
        case NodeKind::NumberLiteral:
        case NodeKind::BigIntLiteral:
        case NodeKind::BooleanLiteral:
        case NodeKind::NullLiteral:
            out_ += slice_(n);
            return;
        case NodeKind::StringLiteral:
            out_ += requote_(slice_(n));
            return;
        case NodeKind::TemplateLiteral:
            out_ += slice_(n);
            return;
        case NodeKind::ThisExpr:
            out_ += "this";
            return;
        case NodeKind::SuperExpr:
            out_ += "super";
            return;
        case NodeKind::ArrayLiteral:
            print_array_(n);
            return;
        case NodeKind::ObjectLiteral:
            print_object_(n);
            return;
        case NodeKind::Property:
            print_property_(n);
            return;
        case NodeKind::SpreadElement:
            out_ += "...";
            emit_(n.a, P_ASSIGN);
            return;
        case NodeKind::Unary:
            print_unary_(n);
            return;
        case NodeKind::Update:
            print_update_(n);
            return;
        case NodeKind::Binary:
        case NodeKind::Logical:
            print_binary_(n);
            return;
        case NodeKind::Assignment:
            emit_(n.a, P_COND);
            out_ += ' ';
            out_ += mbun::js_lexer::token_to_string(static_cast<Token>(n.aux));
            out_ += ' ';
            emit_(n.b, P_ASSIGN);
            return;
        case NodeKind::Conditional:
            emit_(n.a, P_COND + 1);
            out_ += " ? ";
            emit_(n.b, P_ASSIGN);
            out_ += " : ";
            emit_(n.c, P_ASSIGN);
            return;
        case NodeKind::Sequence:
            print_sequence_(n);
            return;
        case NodeKind::Call:
            emit_(n.a, P_POSTFIX);
            print_args_(n);
            return;
        case NodeKind::New:
            out_ += "new ";
            if (n.a != NONE) {
                emit_(n.a, P_POSTFIX);
            }
            print_args_(n);
            return;
        case NodeKind::Member:
            emit_(n.a, P_POSTFIX);
            out_ += (n.flags & 2u) ? "?." : ".";
            out_ += n.text;
            return;
        case NodeKind::PrivateMember:
            emit_(n.a, P_POSTFIX);
            out_ += (n.flags & 2u) ? "?." : ".";
            out_ += n.text;
            return;
        case NodeKind::Index:
            emit_(n.a, P_POSTFIX);
            out_.push_back('[');
            emit_(n.b, P_LOWEST);
            out_.push_back(']');
            return;
        case NodeKind::Paren:
            if (n.a != NONE) {
                print_node_(n.a);
            }
            return;
        case NodeKind::Arrow:
            print_arrow_(n);
            return;
        default:
            // Constructs outside the T2.5 subset: re-emit the source slice.
            out_ += slice_(n);
            return;
        }
    }

    void print_array_(const Node& n) {
        out_.push_back('[');
        bool first = true;
        for (NodeIndex el : arena_.list_of(n)) {
            if (!first) {
                out_ += ", ";
            }
            first = false;
            emit_(el, P_ASSIGN);
        }
        out_.push_back(']');
    }

    void print_object_(const Node& n) {
        auto props = arena_.list_of(n);
        if (props.empty()) {
            out_ += "{}";
            return;
        }
        out_ += "{ ";
        bool first = true;
        for (NodeIndex p : props) {
            if (!first) {
                out_ += ", ";
            }
            first = false;
            print_node_(p);
        }
        out_ += " }";
    }

    void print_property_(const Node& n) {
        if (n.kind == NodeKind::SpreadElement) {
            out_ += "...";
            emit_(n.a, P_ASSIGN);
            return;
        }
        // Key: an Identifier key carries no `text`, so slice it from source.
        if (n.a != NONE) {
            const Node& key = at_(n.a);
            if (key.kind == NodeKind::Identifier) {
                out_ += key.text.empty() ? slice_(key) : key.text;
            } else {
                print_node_(n.a);
            }
        }
        if (n.b != NONE) {
            out_ += ": ";
            emit_(n.b, P_ASSIGN);
        }
    }

    void print_unary_(const Node& n) {
        Token op = static_cast<Token>(n.aux);
        std::string_view s = mbun::js_lexer::token_to_string(op);
        out_ += s;
        if (op == Token::Typeof || op == Token::Void || op == Token::Delete) {
            out_.push_back(' ');
        }
        emit_(n.a, P_UNARY);
    }

    void print_update_(const Node& n) {
        std::string_view s = mbun::js_lexer::token_to_string(static_cast<Token>(n.aux));
        if (n.flags & 1u) {  // prefix
            out_ += s;
            emit_(n.a, P_UNARY);
        } else {  // postfix
            emit_(n.a, P_POSTFIX);
            out_ += s;
        }
    }

    void print_binary_(const Node& n) {
        Token op = static_cast<Token>(n.aux);
        int p = 10 + binary_bp_(op);
        bool leftAssoc = op != Token::AsteriskAsterisk;
        emit_(n.a, leftAssoc ? p : p + 1);
        out_.push_back(' ');
        out_ += mbun::js_lexer::token_to_string(op);
        out_.push_back(' ');
        emit_(n.b, leftAssoc ? p + 1 : p);
    }

    void print_sequence_(const Node& n) {
        bool first = true;
        for (NodeIndex el : arena_.list_of(n)) {
            if (!first) {
                out_ += ", ";
            }
            first = false;
            emit_(el, P_ASSIGN);
        }
    }

    void print_args_(const Node& n) {
        out_.push_back('(');
        bool first = true;
        for (NodeIndex a : arena_.list_of(n)) {
            if (!first) {
                out_ += ", ";
            }
            first = false;
            emit_(a, P_ASSIGN);
        }
        out_.push_back(')');
    }

    void print_arrow_(const Node& n) {
        out_.push_back('(');
        bool first = true;
        for (NodeIndex p : arena_.list_of(n)) {
            if (!first) {
                out_ += ", ";
            }
            first = false;
            const Node& pn = at_(p);
            out_ += pn.text.empty() ? slice_(pn) : pn.text;
        }
        out_ += ") => ";
        if (n.b != NONE) {
            const Node& body = at_(n.b);
            if (body.kind == NodeKind::Block) {
                print_block_(n.b);
            } else {
                emit_(n.b, P_ASSIGN);
            }
        }
    }

    // ── statements ─────────────────────────────────────────────────────────────
    void print_stmt_(NodeIndex idx) {
        const Node& n = at_(idx);
        switch (n.kind) {
        case NodeKind::ExpressionStmt:
            emit_(n.a, P_LOWEST);
            out_ += ";\n";
            return;
        case NodeKind::VarDecl:
            print_var_decl_(n);
            out_ += ";\n";
            return;
        case NodeKind::EmptyStmt:
            return;
        case NodeKind::Block:
            print_block_(idx);
            out_.push_back('\n');
            return;
        default:
            // Statements outside the T2.5 subset: re-emit the source slice.
            out_ += slice_(n);
            out_.push_back('\n');
            return;
        }
    }

    void print_var_decl_(const Node& n) {
        switch (static_cast<VarKind>(n.aux)) {
        case VarKind::Var: out_ += "var "; break;
        case VarKind::Let: out_ += "let "; break;
        case VarKind::Const: out_ += "const "; break;
        }
        bool first = true;
        for (NodeIndex d : arena_.list_of(n)) {
            if (!first) {
                out_ += ", ";
            }
            first = false;
            const Node& decl = at_(d);
            if (decl.a != NONE) {
                const Node& name = at_(decl.a);
                out_ += name.text.empty() ? slice_(name) : name.text;
            }
            if (decl.b != NONE) {
                out_ += " = ";
                emit_(decl.b, P_ASSIGN);
            }
        }
    }

    void print_block_(NodeIndex idx) {
        const Node& n = at_(idx);
        auto stmts = arena_.list_of(n);
        if (stmts.empty()) {
            out_ += "{\n}";
            return;
        }
        out_ += "{\n";
        for (NodeIndex s : stmts) {
            out_ += "  ";
            print_stmt_(s);
        }
        out_.push_back('}');
    }

    // ── string literal re-quoting ──────────────────────────────────────────────
    static void append_utf8_(std::string& out, char32_t cp) {
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

    static int hex_val_(unsigned char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // Decode a source string-literal slice (including its surrounding quotes) into
    // code points, then re-emit with bun's quote selection: prefer '"', fall back
    // to '\'' only when the text contains '"' but no '\''.
    std::string requote_(std::string_view lit) const {
        std::vector<char32_t> cps;
        bool hasSingle = false;
        bool hasDouble = false;
        std::size_t i = (lit.size() >= 2) ? 1 : lit.size();
        std::size_t end = (lit.size() >= 2) ? lit.size() - 1 : lit.size();
        auto push = [&](char32_t cp) {
            if (cp == '\'') hasSingle = true;
            if (cp == '"') hasDouble = true;
            cps.push_back(cp);
        };
        while (i < end) {
            unsigned char c = static_cast<unsigned char>(lit[i]);
            if (c == '\\' && i + 1 < end) {
                unsigned char e = static_cast<unsigned char>(lit[i + 1]);
                switch (e) {
                case 'n': push(0x0A); i += 2; continue;
                case 't': push(0x09); i += 2; continue;
                case 'r': push(0x0D); i += 2; continue;
                case 'b': push(0x08); i += 2; continue;
                case 'f': push(0x0C); i += 2; continue;
                case 'v': push(0x0B); i += 2; continue;
                case '0': push(0x00); i += 2; continue;
                case '\\': push('\\'); i += 2; continue;
                case '\'': push('\''); i += 2; continue;
                case '"': push('"'); i += 2; continue;
                case '`': push('`'); i += 2; continue;
                case 'x': {
                    int hi = (i + 3 < end) ? hex_val_(static_cast<unsigned char>(lit[i + 2])) : -1;
                    int lo = (i + 3 < end) ? hex_val_(static_cast<unsigned char>(lit[i + 3])) : -1;
                    if (hi >= 0 && lo >= 0) {
                        push(static_cast<char32_t>(hi * 16 + lo));
                        i += 4;
                        continue;
                    }
                    push('\\');
                    ++i;
                    continue;
                }
                case 'u': {
                    // \u{...} or \uHHHH.
                    if (i + 2 < end && lit[i + 2] == '{') {
                        std::size_t j = i + 3;
                        char32_t v = 0;
                        bool any = false;
                        while (j < end && lit[j] != '}') {
                            int d = hex_val_(static_cast<unsigned char>(lit[j]));
                            if (d < 0) break;
                            v = v * 16 + static_cast<char32_t>(d);
                            any = true;
                            ++j;
                        }
                        if (any && j < end && lit[j] == '}') {
                            push(v);
                            i = j + 1;
                            continue;
                        }
                    } else if (i + 5 < end || (i + 5 == end)) {
                        char32_t v = 0;
                        bool ok = true;
                        for (int k = 0; k < 4; ++k) {
                            int d = hex_val_(static_cast<unsigned char>(lit[i + 2 + k]));
                            if (d < 0) {
                                ok = false;
                                break;
                            }
                            v = v * 16 + static_cast<char32_t>(d);
                        }
                        if (ok) {
                            push(v);
                            i += 6;
                            continue;
                        }
                    }
                    push('\\');
                    ++i;
                    continue;
                }
                default:
                    push(e);
                    i += 2;
                    continue;
                }
            }
            if (c < 0x80) {
                push(c);
                ++i;
                continue;
            }
            // UTF-8 multibyte passthrough.
            auto [cp, len] = decode_utf8_(lit, i);
            push(cp);
            i += len;
        }
        char quote = (hasDouble && !hasSingle) ? '\'' : '"';
        std::string out;
        out.push_back(quote);
        for (char32_t cp : cps) {
            if (cp == static_cast<char32_t>(quote)) {
                out.push_back('\\');
                out.push_back(quote);
            } else if (cp == '\\') {
                out += "\\\\";
            } else if (cp == 0x0A) {
                out += "\\n";
            } else if (cp == 0x09) {
                out += "\\t";
            } else if (cp == 0x0D) {
                out += "\\r";
            } else if (cp == 0x08) {
                out += "\\b";
            } else if (cp == 0x0C) {
                out += "\\f";
            } else if (cp == 0x0B) {
                out += "\\v";
            } else if (cp == 0x00) {
                out += "\\0";
            } else if (cp < 0x20) {
                out += std::format("\\x{:02X}", static_cast<unsigned>(cp));
            } else {
                append_utf8_(out, cp);
            }
        }
        out.push_back(quote);
        return out;
    }

    static std::pair<char32_t, std::size_t> decode_utf8_(std::string_view s, std::size_t i) {
        if (i >= s.size()) {
            return {0xFFFD, 1};
        }
        unsigned char c0 = static_cast<unsigned char>(s[i]);
        if (c0 < 0x80) {
            return {c0, 1};
        }
        auto cont = [&](std::size_t k) -> int {
            if (i + k >= s.size()) return -1;
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return -1;
            return cc & 0x3F;
        };
        if ((c0 & 0xE0) == 0xC0) {
            int b1 = cont(1);
            if (b1 < 0) return {0xFFFD, 1};
            return {static_cast<char32_t>(((c0 & 0x1F) << 6) | b1), 2};
        }
        if ((c0 & 0xF0) == 0xE0) {
            int b1 = cont(1), b2 = cont(2);
            if (b1 < 0 || b2 < 0) return {0xFFFD, 1};
            return {static_cast<char32_t>(((c0 & 0x0F) << 12) | (b1 << 6) | b2), 3};
        }
        if ((c0 & 0xF8) == 0xF0) {
            int b1 = cont(1), b2 = cont(2), b3 = cont(3);
            if (b1 < 0 || b2 < 0 || b3 < 0) return {0xFFFD, 1};
            return {static_cast<char32_t>(((c0 & 0x07) << 18) | (b1 << 12) | (b2 << 6) | b3), 4};
        }
        return {0xFFFD, 1};
    }
};

}  // namespace detail

// Print the AST rooted at `program` (a Program node in `arena`) back to source.
// `source` is the original text the arena's byte offsets refer to.
std::string print(const mbun::ast::Arena& arena, mbun::ast::NodeIndex program,
                  std::string_view source, PrintOptions options = {}) {
    detail::Printer p{arena, source, options};
    return p.run(program);
}

}  // namespace mbun::js_printer
