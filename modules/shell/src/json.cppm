// json.cppm — canonical Bun shell lexer/AST JSON adapter.
export module mbun.shell.json;

import std;
import mbun.shell.parser;

namespace mbun::shell {

// ───────────────────────────── JSON emit ─────────────────────────────
// Field order mirrors json_fmt.rs exactly.

void json_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    std::format_to(std::back_inserter(out), "\\u{:04x}", c);
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void emit_bool(std::string& o, bool b) { o += b ? "true" : "false"; }

void emit_redirect_flags(std::string& o, std::uint8_t r) {
    o += "{\"stdin\":";
    emit_bool(o, r & rf::STDIN);
    o += ",\"stdout\":";
    emit_bool(o, r & rf::STDOUT);
    o += ",\"stderr\":";
    emit_bool(o, r & rf::STDERR);
    o += ",\"append\":";
    emit_bool(o, r & rf::APPEND);
    o += ",\"duplicate_out\":";
    emit_bool(o, r & rf::DUPLICATE_OUT);
    o += ",\"__unused\":0}";
}

void emit_script(std::string& o, const Script& s);
void emit_atom(std::string& o, const Atom& a);

void emit_simple_atom(std::string& o, const SimpleAtom& s) {
    switch (s.kind) {
        case SAKind::Var:
            o += "{\"Var\":";
            json_string(o, s.text);
            break;
        case SAKind::VarArgv:
            std::format_to(std::back_inserter(o), "{{\"VarArgv\":{}", s.varargv);
            break;
        case SAKind::Text:
            o += "{\"Text\":";
            json_string(o, s.text);
            break;
        case SAKind::QuotedEmpty: o += "{\"quoted_empty\":{}"; break;
        case SAKind::Asterisk: o += "{\"asterisk\":{}"; break;
        case SAKind::DoubleAsterisk: o += "{\"double_asterisk\":{}"; break;
        case SAKind::BraceBegin: o += "{\"brace_begin\":{}"; break;
        case SAKind::BraceEnd: o += "{\"brace_end\":{}"; break;
        case SAKind::Comma: o += "{\"comma\":{}"; break;
        case SAKind::Tilde: o += "{\"tilde\":{}"; break;
        case SAKind::CmdSubst:
            o += "{\"cmd_subst\":{\"script\":";
            emit_script(o, *s.cmdsubst_script);
            o += ",\"quoted\":";
            emit_bool(o, s.cmdsubst_quoted);
            o += "}";
            break;
    }
    o += "}";
}

void emit_compound_atom(std::string& o, const CompoundAtom& c) {
    o += "{\"atoms\":[";
    for (std::size_t i = 0; i < c.atoms.size(); ++i) {
        if (i) o += ",";
        emit_simple_atom(o, c.atoms[i]);
    }
    o += "],\"brace_expansion_hint\":";
    emit_bool(o, c.brace_expansion_hint);
    o += ",\"glob_hint\":";
    emit_bool(o, c.glob_hint);
    o += "}";
}

void emit_atom(std::string& o, const Atom& a) {
    if (a.kind == AtomKind::Simple) {
        o += "{\"simple\":";
        emit_simple_atom(o, a.simple);
    } else {
        o += "{\"compound\":";
        emit_compound_atom(o, a.compound);
    }
    o += "}";
}

void emit_redirect(std::string& o, const Redirect& r) {
    if (r.kind == RedirKind::Atom) {
        o += "{\"atom\":";
        emit_atom(o, r.atom);
    } else {
        std::format_to(std::back_inserter(o), "{{\"jsbuf\":{{\"idx\":{}}}", r.jsbuf_idx);
    }
    o += "}";
}

void emit_assign(std::string& o, const Assign& a) {
    o += "{\"label\":";
    json_string(o, a.label);
    o += ",\"value\":";
    emit_atom(o, a.value);
    o += "}";
}

void emit_cmd(std::string& o, const Cmd& c) {
    o += "{\"assigns\":[";
    for (std::size_t i = 0; i < c.assigns.size(); ++i) {
        if (i) o += ",";
        emit_assign(o, c.assigns[i]);
    }
    o += "],\"name_and_args\":[";
    for (std::size_t i = 0; i < c.name_and_args.size(); ++i) {
        if (i) o += ",";
        emit_atom(o, c.name_and_args[i]);
    }
    o += "],\"redirect\":";
    emit_redirect_flags(o, c.redirect);
    o += ",\"redirect_file\":";
    if (c.has_redirect_file)
        emit_redirect(o, c.redirect_file);
    else
        o += "null";
    o += "}";
}

void emit_stmt_list(std::string& o, const std::vector<Stmt>& stmts);

void emit_subshell(std::string& o, const Subshell& s) {
    o += "{\"script\":";
    emit_script(o, s.script);
    o += ",\"redirect\":";
    if (s.has_redirect)
        emit_redirect(o, s.redirect);
    else
        o += "null";
    o += ",\"redirect_flags\":";
    emit_redirect_flags(o, s.redirect_flags);
    o += "}";
}

void emit_if(std::string& o, const If& i) {
    o += "{\"cond\":";
    emit_stmt_list(o, i.cond);
    o += ",\"then\":";
    emit_stmt_list(o, i.then_);
    o += ",\"else_parts\":[";
    for (std::size_t k = 0; k < i.else_parts.size(); ++k) {
        if (k) o += ",";
        emit_stmt_list(o, i.else_parts[k]);
    }
    o += "]}";
}

void emit_condexpr(std::string& o, const CondExpr& c) {
    o += "{\"op\":";
    json_string(o, c.op);
    o += ",\"args\":[";
    for (std::size_t k = 0; k < c.args.size(); ++k) {
        if (k) o += ",";
        emit_atom(o, c.args[k]);
    }
    o += "]}";
}

void emit_pipeline_item(std::string& o, const PipelineItem& p) {
    switch (p.kind) {
        case PIKind::Cmd:
            o += "{\"cmd\":";
            emit_cmd(o, *p.cmd);
            break;
        case PIKind::Assigns:
            o += "{\"assigns\":[";
            for (std::size_t i = 0; i < p.assigns.size(); ++i) {
                if (i) o += ",";
                emit_assign(o, p.assigns[i]);
            }
            o += "]";
            break;
        case PIKind::Subshell:
            o += "{\"subshell\":";
            emit_subshell(o, *p.subshell);
            break;
        case PIKind::If:
            o += "{\"if\":";
            emit_if(o, *p.ifc);
            break;
        case PIKind::CondExpr:
            o += "{\"condexpr\":";
            emit_condexpr(o, *p.condexpr);
            break;
    }
    o += "}";
}

void emit_expr(std::string& o, const Expr& e) {
    switch (e.kind) {
        case ExprKind::Assign:
            o += "{\"assign\":[";
            for (std::size_t i = 0; i < e.assigns.size(); ++i) {
                if (i) o += ",";
                emit_assign(o, e.assigns[i]);
            }
            o += "]";
            break;
        case ExprKind::Binary:
            o += "{\"binary\":{\"op\":";
            json_string(o, e.binary->op == BinaryOp::And ? "And" : "Or");
            o += ",\"left\":";
            emit_expr(o, e.binary->left);
            o += ",\"right\":";
            emit_expr(o, e.binary->right);
            o += "}";
            break;
        case ExprKind::Pipeline:
            o += "{\"pipeline\":{\"items\":[";
            for (std::size_t i = 0; i < e.pipeline->items.size(); ++i) {
                if (i) o += ",";
                emit_pipeline_item(o, e.pipeline->items[i]);
            }
            o += "]}";
            break;
        case ExprKind::Cmd:
            o += "{\"cmd\":";
            emit_cmd(o, *e.cmd);
            break;
        case ExprKind::Subshell:
            o += "{\"subshell\":";
            emit_subshell(o, *e.subshell);
            break;
        case ExprKind::If:
            o += "{\"if\":";
            emit_if(o, *e.ifc);
            break;
        case ExprKind::CondExpr:
            o += "{\"condexpr\":";
            emit_condexpr(o, *e.condexpr);
            break;
        case ExprKind::Async:
            o += "{\"async\":";
            emit_expr(o, *e.async);
            break;
    }
    o += "}";
}

void emit_stmt(std::string& o, const Stmt& s) {
    o += "{\"exprs\":[";
    for (std::size_t i = 0; i < s.exprs.size(); ++i) {
        if (i) o += ",";
        emit_expr(o, s.exprs[i]);
    }
    o += "]}";
}

void emit_stmt_list(std::string& o, const std::vector<Stmt>& stmts) {
    o += "[";
    for (std::size_t i = 0; i < stmts.size(); ++i) {
        if (i) o += ",";
        emit_stmt(o, stmts[i]);
    }
    o += "]";
}

void emit_script(std::string& o, const Script& s) {
    o += "{\"stmts\":";
    emit_stmt_list(o, s.stmts);
    o += "}";
}

std::string join_errors(const std::vector<std::string>& errs) {
    std::string out;
    for (std::size_t i = 0; i < errs.size(); ++i) {
        if (i) out += "\n";
        out += errs[i];
    }
    return out;
}

// ───────────────────────────── public API ─────────────────────────────

// Serialize the token stream as JSON, matching shellInternals.lex output.
export std::string lex_json(std::string_view src, std::uint32_t jsobjs_len = 0) {
    Lexer lx(src, jsobjs_len);
    lx.run();
    const auto& toks = lx.tokens();
    const std::string& pool = lx.strpool();
    std::string o = "[";
    for (std::size_t i = 0; i < toks.size(); ++i) {
        if (i) o += ",";
        const Token& t = toks[i];
        switch (t.tag) {
            case Tok::Var:
            case Tok::Text:
            case Tok::SingleQuotedText:
            case Tok::DoubleQuotedText:
                o += "{\"";
                o += tok_name(t.tag);
                o += "\":";
                json_string(o, std::string_view(pool).substr(t.range.start,
                                                             t.range.end - t.range.start));
                o += "}";
                break;
            case Tok::VarArgv:
            case Tok::JSObjRef:
                std::format_to(std::back_inserter(o), "{{\"{}\":{}}}", tok_name(t.tag), t.num);
                break;
            case Tok::Redirect:
                o += "{\"Redirect\":";
                emit_redirect_flags(o, t.flags);
                o += "}";
                break;
            default:
                o += "{\"";
                o += tok_name(t.tag);
                o += "\":{}}";
                break;
        }
    }
    o += "]";
    return o;
}

// Serialize the parsed AST as JSON, matching shellInternals.parse output.
export std::string parse_json(std::string_view src, std::uint32_t jsobjs_len = 0) {
    Lexer lx(src, jsobjs_len);
    lx.run();
    Parser p(lx.tokens(), lx.strpool());
    Script script = p.parse();
    std::string o;
    emit_script(o, script);
    return o;
}

// Combined lexer error message (newline-separated), or "" if none.
export std::string lex_errors(std::string_view src, std::uint32_t jsobjs_len = 0) {
    Lexer lx(src, jsobjs_len);
    lx.run();
    return join_errors(lx.errors());
}

// Full-pipeline error message: lexer errors if any, else parser errors, "" if none.
export std::string parse_errors(std::string_view src, std::uint32_t jsobjs_len = 0) {
    Lexer lx(src, jsobjs_len);
    lx.run();
    if (!lx.errors().empty()) return join_errors(lx.errors());
    Parser p(lx.tokens(), lx.strpool());
    p.parse();
    return join_errors(p.errors());
}

export struct TemplateMarkerAnalysis {
    std::vector<TemplateQuoteContext> contexts;
    std::string errors;
};

// Analyze Bun JS-string markers through the same recursive lexer/parser used by
// shell execution. In particular, command substitutions reset quote state.
// `jsobjsLen` bounds the \x08__bun_N object refs the source may carry; the lexer
// rejects an index at or past it, so a caller interpolating JS objects must pass
// its own count (bun tracks the same bound as out_jsobjs.len(), shell_body.rs:768).
export TemplateMarkerAnalysis analyze_template_markers(std::string_view src,
                                                        std::size_t markerCount,
                                                        std::uint32_t jsobjsLen = 0) {
    Lexer lexer{src, jsobjsLen, markerCount};
    lexer.run();
    if (!lexer.errors().empty()) {
        return {{}, join_errors(lexer.errors())};
    }
    Parser parser{lexer.tokens(), lexer.strpool()};
    parser.parse();
    return {lexer.template_contexts(), join_errors(parser.errors())};
}

}  // namespace mbun::shell
