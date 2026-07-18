// src/js_parser/types.cppm — module mbun.js_parser.types
//
// TypeScript type syntax: parsing it for real (so its extent is known exactly)
// and recording the erasure, plus the speculation helpers that decide whether a
// `<` opens type arguments or is a less-than.
//
// Why this is a module of its own (AGENTS.md 规则 10 — split by 职责): TS types
// are a *leaf*. A type position contains only types — never a statement, never
// an expression — so measured against the rest of the parser this layer's only
// outbound calls are to the token cursor beneath it. Statement/expression/class
// parsing calls *into* it (parse_type_params_, parse_type_arguments_,
// skip_optional_type_annotation_, classify_follow_) and it never calls back.
// One-directional, so — like token_cursor — a plain base class suffices and no
// call site had to change.
//
// (Two token-level helpers, `is_keyword_` and `advance_true_`, were the only
// things that made this look like a cycle. They were sitting in the class- and
// expression-parsing regions purely by accident of layout; both are pure
// token predicates, so they moved down into token_cursor and the cycle went
// away. Nothing else was touched.)
//
// Everything here erases: the parse records arena edits over the type's span and
// builds no AST nodes, because the transpiler is an eraser (see js_parser.cppm).
export module mbun.js_parser.types;

import std;
import mbun.js_lexer;
import mbun.ast;
import mbun.js_parser.token_cursor;

export namespace mbun::js_parser::detail {

// TS type parsing/skipping, layered directly on the token cursor.
class TypeParser : public TokenCursor {
public:
    using TokenCursor::TokenCursor;

    // A binding in a skipped type position: `a`, `this`, `[a, ...b]`, `{ a, b = 1 }`.
    // The destructuring forms only have to be balanced here — nothing downstream reads
    // them, and the enclosing type is erased whole.
    // ref: bun src/js_parser/parse/parse_skip_typescript.rs:55-90 (skip_type_script_binding).
    bool skip_type_binding_() {
        Token k = curk_();
        if (k == Token::Identifier || k == Token::This || k == Token::EscapedKeyword ||
            is_keyword_(k)) {
            return advance_true_();
        }
        if (k == Token::OpenBracket || k == Token::OpenBrace) {
            return skip_balanced_();
        }
        return false;
    }

    // A function type's parameter list: `(a: T, b?, ...rest: U[])`. Each element is a
    // BINDING — which is what separates `(a: T) => R` from the parenthesized type
    // `((r: string) => number)`, whose contents are a type and not a binding.
    // ref: bun src/js_parser/parse/parse_skip_typescript.rs:152-200 (skip_typescript_fn_args).
    bool skip_type_fn_args_() {
        if (curk_() != Token::OpenParen) {
            return false;
        }
        advance_();  // `(`
        while (ok_ && curk_() != Token::CloseParen) {
            if (curk_() == Token::DotDotDot) {
                advance_();
            }
            if (!skip_type_binding_()) {
                return false;
            }
            if (curk_() == Token::Question) {
                advance_();
            }
            if (curk_() == Token::Colon) {
                advance_();
                parse_type_();
                if (!ok_) {
                    return false;
                }
            }
            if (curk_() != Token::Comma) {
                break;
            }
            advance_();
        }
        return ok_ && curk_() == Token::CloseParen && advance_true_();
    }

    // Speculative: do the parens at the cursor hold a function type's parameter list
    // followed by `=>`? Rewinds the cursor, the edit log and the error state.
    // ref: bun src/js_parser/parse/parse_skip_typescript.rs:1494-1501, :209
    // (skip_type_script_arrow_args_with_backtracking — fn args, then expect `=>`).
    bool try_skip_type_fn_args_() {
        Save save = save_();
        const bool savedOk = ok_;
        std::string savedMsg = errMsg_;
        const std::size_t savedOff = errOff_;
        const bool hit = skip_type_fn_args_() && curk_() == Token::EqualsGreaterThan;
        restore_(save);
        ok_ = savedOk;
        errMsg_ = std::move(savedMsg);
        errOff_ = savedOff;
        return hit;
    }

    // ── TypeScript type-argument speculation ─────────────────────────────────
    Follow classify_follow_() const {
        Token k = curk_();
        if (k == Token::OpenParen) {
            return Follow::Call;
        }
        if (k == Token::NoSubstitutionTemplateLiteral || k == Token::TemplateHead) {
            return Follow::Tagged;
        }
        // Ambiguous: `<`, any `>`-run, and unary +/- keep the relational reading.
        if (k == Token::LessThan || is_gt_family_(k) || k == Token::Plus || k == Token::Minus) {
            return Follow::No;
        }
        // `=` splits on the SOURCE token shape (bun transpiler.test.js:547/560):
        //  - the residual half of a split `>=`/`>>=`/`>>>=` (eat_gt_ peeled the
        //    `>` that closed a candidate list) keeps the relational reading —
        //    `p<0||p>=l` and `f<x>=g<y>` must not erase bogus type args;
        //  - a STANDALONE `=` token is an instantiation-expression assignment:
        //    `f<x> = g<y>;` erases to `f = g;`.
        if (k == Token::Equals) {
            return cur_().kind == Token::Equals ? Follow::Instantiation : Follow::No;
        }
        // If the token can begin a new expression, this was a comparison.
        if (can_start_expression_(k)) {
            return Follow::No;
        }
        return Follow::Instantiation;
    }

    static bool can_start_expression_(Token k) {
        switch (k) {
        case Token::Identifier:
        case Token::EscapedKeyword:
        case Token::PrivateIdentifier:
        case Token::NumericLiteral:
        case Token::BigIntegerLiteral:
        case Token::StringLiteral:
        case Token::NoSubstitutionTemplateLiteral:
        case Token::TemplateHead:
        case Token::True:
        case Token::False:
        case Token::Null:
        case Token::This:
        case Token::Super:
        case Token::Function:
        case Token::Class:
        case Token::New:
        case Token::Import:
        case Token::OpenParen:
        case Token::OpenBracket:
        case Token::OpenBrace:
        case Token::Exclamation:
        case Token::Tilde:
        case Token::Plus:
        case Token::Minus:
        case Token::PlusPlus:
        case Token::MinusMinus:
        case Token::Typeof:
        case Token::Void:
        case Token::Delete:
        case Token::At:
            return true;
        default:
            return false;
        }
    }

    // Speculatively skip `< ... >` balancing angle brackets. Returns true and
    // leaves the cursor just past the closing '>'; on failure the caller
    // restores. Does not mutate the token vector — only the (idx, gtOffset)
    // cursor moves.
    bool try_skip_type_arguments_() {
        advance_();  // consume '<'
        int depth = 1;
        while (depth > 0) {
            Token k = curk_();
            if (k == Token::EndOfFile) {
                return false;
            }
            if (k == Token::LessThan) {
                ++depth;
                advance_();
            } else if (is_gt_family_(k)) {
                eat_gt_();
                --depth;
            } else if (k == Token::OpenParen || k == Token::OpenBracket ||
                       k == Token::OpenBrace) {
                if (!skip_balanced_()) {
                    return false;
                }
            } else if (k == Token::Semicolon) {
                return false;
            } else {
                advance_();
            }
        }
        return true;
    }

    // Skip a balanced (), [] or {} group starting at the current open token.
    bool skip_balanced_() {
        Token open = curk_();
        Token close;
        if (open == Token::OpenParen) {
            close = Token::CloseParen;
        } else if (open == Token::OpenBracket) {
            close = Token::CloseBracket;
        } else if (open == Token::OpenBrace) {
            close = Token::CloseBrace;
        } else {
            return false;
        }
        advance_();
        int depth = 1;
        while (depth > 0) {
            Token k = curk_();
            if (k == Token::EndOfFile) {
                return false;
            }
            if (k == open) {
                ++depth;
            } else if (k == close) {
                --depth;
            } else if (k == Token::OpenParen || k == Token::OpenBracket ||
                       k == Token::OpenBrace) {
                if (!skip_balanced_()) {
                    return false;
                }
                continue;
            }
            advance_();
        }
        return true;
    }

    // ── TypeScript types ─────────────────────────────────────────────────────
    // Parse and ERASE a `<T, ...>` type-parameter list. `nested` is set when the
    // list appears inside another type (a generic function type), where the whole
    // enclosing type is already being erased by an outer edit — so we must NOT
    // record a second (overlapping) edit here.
    void parse_type_params_(bool allowEmpty, bool nested = false) {
        std::uint32_t start = cur_().start;
        advance_();  // '<'
        if (allowEmpty && curk_() == Token::GreaterThan) {
            eat_gt_();
            if (!nested) {
                arena_.add_edit(start, prev_end_());
            }
            return;
        }
        while (ok_) {
            parse_type_parameter_();
            if (!ok_) {
                return;
            }
            if (curk_() == Token::Comma) {
                advance_();
                if (is_gt_family_(curk_())) {
                    break;  // trailing comma
                }
                continue;
            }
            break;
        }
        if (!ok_) {
            return;
        }
        expect_gt_();
        if (ok_ && !nested) {
            arena_.add_edit(start, prev_end_());
        }
    }

    void parse_type_parameter_() {
        // Variance / const modifiers. `in` and `const` are reserved keywords so
        // they are always modifiers; `out` is contextual (a plain identifier) so
        // it is a modifier only when another name/modifier follows it. An escaped
        // spelling is never a modifier.
        bool seenIn = false;
        bool seenOut = false;
        bool seenConst = false;
        while (ok_) {
            Token k = curk_();
            const Tok& t = cur_();
            bool escaped = tok_has_escape_(t);
            int modType = -1;  // 0=in 1=out 2=const
            if (!escaped) {
                if (k == Token::In) {
                    modType = 0;
                } else if (k == Token::Const) {
                    modType = 2;
                } else if (k == Token::Identifier && std::string_view{t.ident} == "out") {
                    // `out` is a modifier only if a name/modifier follows.
                    Token nk = peek_kind_(1);
                    bool nextIsName =
                        nk == Token::Identifier || nk == Token::In || nk == Token::Const;
                    if (nextIsName) {
                        modType = 1;
                    }
                }
            }
            if (modType < 0) {
                break;
            }
            if (modType == 2) {
                if (seenConst || seenIn || seenOut) {
                    fail_("The modifier \"const\" is not valid here");
                    return;
                }
                seenConst = true;
            } else if (modType == 1) {
                if (seenOut) {
                    fail_("The modifier \"out\" is not valid here");
                    return;
                }
                seenOut = true;
            } else {
                if (seenIn || seenOut) {
                    fail_("The modifier \"in\" is not valid here");
                    return;
                }
                seenIn = true;
            }
            advance_();
        }
        // Parameter name.
        if (curk_() == Token::EscapedKeyword) {
            expected_identifier_();
            return;
        }
        if (curk_() != Token::Identifier) {
            expected_identifier_();
            return;
        }
        advance_();  // name
        // `extends` constraint and `=` default.
        if (curk_() == Token::Extends) {
            advance_();
            parse_type_();
            if (!ok_) {
                return;
            }
        }
        if (curk_() == Token::Equals) {
            advance_();
            parse_type_();
            if (!ok_) {
                return;
            }
        }
        // Must be followed by ',' or the closing '>'.
        if (curk_() != Token::Comma && !is_gt_family_(curk_())) {
            expected_(">");
        }
    }

    void parse_type_arguments_(bool nested = false) {
        std::uint32_t start = cur_().start;
        advance_();  // '<'
        while (ok_) {
            parse_type_();  // empty `<>` -> parse_type_ reports "Unexpected >"
            if (!ok_) {
                return;
            }
            if (curk_() == Token::Comma) {
                advance_();
                if (is_gt_family_(curk_())) {
                    break;
                }
                continue;
            }
            break;
        }
        if (!ok_) {
            return;
        }
        expect_gt_();
        if (ok_ && !nested) {
            arena_.add_edit(start, prev_end_());
        }
    }

    // Minimal but faithful TS type grammar: enough to parse the annotations and
    // type-argument lists the T2.4 vectors carry (refs with type args, tuples,
    // object/function types, unions/intersections, literal & template types).
    void parse_type_() {
        // TS type predicate: `asserts x [is T]` (assertion signature).
        if (ident_is_("asserts")) {
            advance_();
            if (curk_() == Token::Identifier || curk_() == Token::This) {
                advance_();
            }
            if (ident_is_("is")) {
                advance_();
                parse_type_union_();
            }
            return;
        }
        parse_type_union_();
        // `x is T` type-guard predicate (return-type position).
        if (ident_is_("is") && !cur_().newlineBefore) {
            advance_();
            parse_type_union_();
        }
    }

    void parse_type_union_() {
        if (curk_() == Token::Bar || curk_() == Token::Ampersand) {
            advance_();  // leading | or &
        }
        parse_type_intersection_();
        while (ok_ && curk_() == Token::Bar) {
            advance_();
            parse_type_intersection_();
        }
    }

    void parse_type_intersection_() {
        parse_type_postfix_();
        while (ok_ && curk_() == Token::Ampersand) {
            advance_();
            parse_type_postfix_();
        }
    }

    void parse_type_postfix_() {
        parse_type_primary_();
        if (!ok_) {
            return;
        }
        while (ok_) {
            if (curk_() == Token::OpenBracket) {
                // ref parse_skip_typescript.rs:915-917, verbatim:
                //     // "{ ['x']: string \n ['y']: string }" must not become a
                //     // single type
                //     if self.lexer.has_newline_before { return Ok(()); }
                // A `[` on a NEW LINE is not this type's suffix — it opens the
                // next member. Without this, `[key: number]: string \n [k2]: any`
                // reads `string[k2]` as an indexed-access type, swallows the next
                // member's key, and the class body then dies on its `:`. The
                // `extends` arm below already guards the same way (:941), which
                // is what makes the omission here an oversight rather than a
                // model difference.
                if (cur_().newlineBefore) {
                    return;
                }
                // Indexed access / array type: `T[]` or `T[K]`.
                advance_();
                if (curk_() == Token::CloseBracket) {
                    advance_();
                } else {
                    parse_type_();
                    if (!ok_) {
                        return;
                    }
                    if (!expect_(Token::CloseBracket)) {
                        return;
                    }
                }
            } else if ((curk_() == Token::Extends || ident_is_("extends")) &&
                       !cur_().newlineBefore) {
                // Conditional type: T extends U ? X : Y
                advance_();
                parse_type_();
                if (!ok_) {
                    return;
                }
                if (curk_() == Token::Question) {
                    advance_();
                    parse_type_();
                    if (!ok_) {
                        return;
                    }
                    if (!expect_(Token::Colon)) {
                        return;
                    }
                    parse_type_();
                    if (!ok_) {
                        return;
                    }
                }
            } else {
                break;
            }
        }
    }

    void parse_type_primary_() {
        // Prefix type operators (contextual keywords): `keyof T`, `readonly T[]`,
        // `unique symbol`, `infer X`.
        if (ident_is_("keyof") || ident_is_("readonly") || ident_is_("unique")) {
            advance_();
            parse_type_primary_();
            return;
        }
        // `abstract new (…) => T` (TS 4.2). `abstract` is a prefix operator ONLY when
        // `new` follows; standalone it is an ordinary type reference name.
        // ref: bun src/js_parser/parse/parse_skip_typescript.rs:494-501 (TsIdentKind::Abstract).
        if (ident_is_("abstract") && peek_kind_(1) == Token::New) {
            advance_();
            parse_type_primary_();
            return;
        }
        if (ident_is_("infer")) {
            advance_();
            if (curk_() == Token::Identifier) {
                advance_();  // inferred type-parameter name
            }
            return;
        }
        Token k = curk_();
        switch (k) {
        case Token::Identifier:
        case Token::Void:
        case Token::Null:
        case Token::This:
        case Token::True:
        case Token::False: {
            advance_();
            // Qualified name `A.B.C`.
            while (ok_ && curk_() == Token::Dot) {
                advance_();
                if (curk_() == Token::Identifier || is_keyword_(curk_())) {
                    advance_();
                } else {
                    expected_identifier_();
                    return;
                }
            }
            if (curk_() == Token::LessThan) {
                parse_type_arguments_(/*nested=*/true);
            }
            return;
        }
        case Token::Typeof: {
            advance_();
            parse_type_primary_();
            return;
        }
        case Token::Import: {
            // TS import type: `import("mod")[.Qualified][<Args>]`.
            advance_();  // import
            if (curk_() == Token::OpenParen) {
                if (!skip_balanced_()) {
                    fail_("Parse error");
                    return;
                }
            }
            while (ok_ && curk_() == Token::Dot) {
                advance_();
                if (curk_() == Token::Identifier || is_keyword_(curk_())) {
                    advance_();
                } else {
                    expected_identifier_();
                    return;
                }
            }
            if (curk_() == Token::LessThan) {
                parse_type_arguments_(/*nested=*/true);
            }
            return;
        }
        case Token::New: {
            // Constructor type `new (...) => T`.
            advance_();
            if (curk_() == Token::LessThan) {
                parse_type_params_(false, /*nested=*/true);
                if (!ok_) {
                    return;
                }
            }
            if (curk_() == Token::OpenParen) {
                if (!skip_balanced_()) {
                    fail_("Parse error");
                    return;
                }
            }
            if (curk_() == Token::EqualsGreaterThan) {
                advance_();
                parse_type_();
            }
            return;
        }
        case Token::NumericLiteral:
        case Token::BigIntegerLiteral:
        case Token::StringLiteral:
        case Token::NoSubstitutionTemplateLiteral: {
            advance_();
            return;
        }
        case Token::TemplateHead: {
            // Template literal type.
            advance_();
            while (ok_) {
                parse_type_();
                if (!ok_) {
                    return;
                }
                if (curk_() == Token::TemplateTail) {
                    advance_();
                    break;
                }
                if (curk_() == Token::TemplateMiddle) {
                    advance_();
                    continue;
                }
                expected_("}");
                return;
            }
            return;
        }
        case Token::Minus: {
            // Negative numeric literal type.
            advance_();
            if (curk_() == Token::NumericLiteral || curk_() == Token::BigIntegerLiteral) {
                advance_();
            } else {
                unexpected_();
            }
            return;
        }
        case Token::OpenBracket: {
            parse_tuple_type_();
            return;
        }
        case Token::OpenBrace: {
            // Object / mapped type — skip balanced.
            if (!skip_balanced_()) {
                fail_("Parse error");
            }
            return;
        }
        case Token::OpenParen: {
            // `(a: T) => R` (function type) or `(A | B)` (parenthesized type). Only the
            // function type's `=>` belongs to the type: taking any `=>` that follows a
            // balanced `(…)` steals the enclosing arrow function's arrow, so
            // `(x): ((r: string) => number) => body` swallowed `=> body` as the return
            // type and then failed at the real end of the annotation.
            // Tell them apart the way bun does — speculatively parse the parens as a
            // PARAMETER LIST (each element a binding, not a type) and require `=>`.
            // ref: bun src/js_parser/parse/parse_skip_typescript.rs:203-225
            // (skip_type_script_paren_or_fn_type).
            if (try_skip_type_fn_args_()) {
                skip_type_fn_args_();  // re-run to commit (the speculation rewound)
                if (!expect_(Token::EqualsGreaterThan)) {
                    return;
                }
                parse_type_();
                return;
            }
            advance_();  // `(`
            parse_type_();
            if (!ok_) {
                return;
            }
            expect_(Token::CloseParen);
            return;
        }
        case Token::LessThan: {
            // Generic function type `<T>(...) => R`.
            parse_type_params_(false, /*nested=*/true);
            if (!ok_) {
                return;
            }
            if (curk_() == Token::OpenParen) {
                if (!skip_balanced_()) {
                    fail_("Parse error");
                    return;
                }
            }
            if (curk_() == Token::EqualsGreaterThan) {
                advance_();
                parse_type_();
            }
            return;
        }
        default:
            if (is_keyword_(k) || k == Token::EscapedKeyword) {
                // Contextual keyword used as a type name (`keyof`, `infer`, …) —
                // treat as an identifier-like type.
                advance_();
                if (curk_() == Token::LessThan) {
                    parse_type_arguments_(/*nested=*/true);
                }
                return;
            }
            unexpected_();  // e.g. empty `Foo<>` -> "Unexpected >"
            return;
        }
    }

    void parse_tuple_type_() {
        advance_();  // '['
        while (ok_ && curk_() != Token::CloseBracket && curk_() != Token::EndOfFile) {
            if (curk_() == Token::DotDotDot) {
                advance_();
            }
            // A labelled tuple element `label: T` or `label?: T`. A label must be
            // an identifier or a contextual keyword; a reserved keyword label is
            // rejected. We detect the label form by a following ':' or '?:'.
            if (is_tuple_label_ahead_()) {
                Token lk = curk_();
                if (lk != Token::Identifier && lk != Token::EscapedKeyword) {
                    // Reserved keyword used as a tuple label.
                    unexpected_();
                    return;
                }
                advance_();  // label
                if (curk_() == Token::Question) {
                    advance_();
                }
                if (!expect_(Token::Colon)) {
                    return;
                }
            }
            parse_type_();
            if (!ok_) {
                return;
            }
            // Optional tuple element `[T?]` — the `?` trails the element TYPE and has
            // no label or `:` (the labelled form `[a?: T]` consumed its `?` above).
            // ref: bun src/js_parser/parse/parse_skip_typescript.rs:706-708.
            if (curk_() == Token::Question) {
                advance_();
            }
            if (curk_() == Token::Comma) {
                advance_();
                continue;
            }
            break;
        }
        expect_(Token::CloseBracket);
    }

    bool is_tuple_label_ahead_() const {
        // current token followed by ':' (or '?' ':') marks a labelled element.
        Token n1 = peek_kind_(1);
        if (n1 == Token::Colon) {
            return true;
        }
        if (n1 == Token::Question && tok_at_(idx_ + 2).kind == Token::Colon) {
            return true;
        }
        return false;
    }
};

}  // namespace mbun::js_parser::detail
