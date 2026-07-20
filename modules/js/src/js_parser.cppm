// src/js_parser.cppm — module mbun.js_parser
//
// Recursive-descent + Pratt parser for the JS/TS/JSX subset the transpiler
// tests exercise (T2.4). Built on mbun.js_lexer (tokens) and mbun.ast (nodes).
//
// Design notes (not a mechanical port of bun's Zig parser):
//   * The lexer is a streaming scanner, but a Pratt parser needs bounded
//     lookahead and — crucially for TypeScript — speculative backtracking over
//     type-argument lists. So we pre-lex the whole source into a flat token
//     vector once, and the parser is a cursor over it. Backtracking is just a
//     save/restore of (index, gtOffset).
//   * ">>"/">>>"/">="/">>="/">>>=" are single lexer tokens, but TypeScript type
//     syntax needs to peel one ">" at a time to close nested type-argument
//     lists (Array<Array<number>>). Rather than mutate the token stream we keep
//     a `gtOffset_` sub-cursor into the current ">"-run; eat_gt_() consumes one
//     ">" and the "effective" token kind is recomputed from the remainder. This
//     is the "expect_greater_than-style rescan" the T2.4 checklist calls for.
//   * Errors use std::expected-free propagation: the first error sets a sticky
//     flag+message (matching bun's AggregateError.errors[0] semantics) and every
//     parse step bails. No exceptions on the hot path.
//   * The TS instantiation-expression disambiguation (f<x> vs f < x) follows the
//     TypeScript/esbuild rule: speculatively skip the type arguments, then decide
//     from the FOLLOWING token whether it was really type args.
export module mbun.js_parser;

import std;
import mbun.js_lexer;
import mbun.ast;
import mbun.js_parser.cjs_runtime;
import mbun.js_parser.token_cursor;
import mbun.js_parser.types;
// `export import`, not `import`: TranspileOptions now HAS a JsxOptions member, so
// every caller that names one (the module loader, the CLI's flag wiring) needs
// the type — re-exporting it here keeps them on the single `mbun.js_parser`
// import they already have. ref AGENTS.md 规则 10 (薄聚合模块 export import).
export import mbun.js_parser.jsx_lower;
import mbun.js_parser.jsx_pragma;
// MEASUREMENT-ONLY (CAP-BUILD-PRINTER wiring probe): the AST-rebuild printer,
// reachable from transpile() ONLY behind TranspileOptions::printer /
// MBUN_TRANSPILE_PRINTER=1. Default-off; see transpile()'s note.
import mbun.js_printer;
import mbun.js_parser.subst;
import mbun.js_parser.module_scope;
import mbun.js_parser.trim_imports;

export namespace mbun::js_parser {

using mbun::js_lexer::Token;

struct ParseResult {
    bool ok{true};
    std::string error;
    std::size_t error_offset{0};
    mbun::ast::Arena arena;
    mbun::ast::NodeIndex program{mbun::ast::NONE};
    bool cjs_esm_module{false};  // (CJS mode) input used ESM syntax → wrapper stays strict
    bool cjs_esm_export{false};  // (CJS mode) module used ESM export → needs __esModule marker
    bool cjs_live_export{false};  // (CJS mode) an export lowered to __mbun_X → needs the helper
    // (CJS mode) every live export as (key literal, local name), for the
    // end-of-module re-push. Insertion-ordered; duplicates are harmless.
    std::vector<std::pair<std::string, std::string>> cjs_live_names;
    bool top_level_await{false};  // module awaits at top level (needs an async wrapper in
                                  // the script-mode JSC runtime, which has no native TLA)
};

namespace detail {

using mbun::ast::Arena;
using mbun::ast::Node;
using mbun::ast::NodeIndex;
using mbun::ast::NodeKind;
using mbun::ast::NONE;
using mbun::ast::VarKind;
namespace bflags = mbun::ast::bflags;  // Binding* node flag bits (ast.cppm:205)
// ⚠️ `propFlags`, not `pflags`: parse_binding_object_ already has a LOCAL
// `std::uint8_t pflags` (:721) holding bflags bits, which would shadow a
// namespace alias of that name.
namespace propFlags = mbun::ast::pflags;  // Property flag bits (ast.cppm:263)
namespace mflags = mbun::ast::mflags;  // Import/ExportDecl flag bits (ast.cppm mflags)
// `fnFlags`/`exprFlags` follow propFlags' spelling for the same shadowing reason
// — a lowercase alias is one local variable away from being shadowed.
namespace fnFlags = mbun::ast::fnflags;  // Arrow/Function* flag bits (ast.cppm)
namespace exprFlags = mbun::ast::eflags;  // Object/ArrayLiteral flag bits (ast.cppm)
using mbun::ast::PropertyKind;            // ref bun G::PropertyKind (g.rs:245)

// A supra-BMP (astral) code point is stored as a UTF-16 surrogate pair; its
// source `\u{…}` form must be re-serialized so decorator metadata and the
// rewritten key echo the code point the way bun's js_printer does (each
// surrogate code unit escaped independently to `\uXXXX`).
inline bool key_has_supra_bmp_(const std::u16string& v) {
    for (char16_t u : v) {
        if (u >= 0xD800 && u <= 0xDBFF) return true;  // high surrogate ⇒ astral
    }
    return false;
}
// Quote a decoded UTF-16 string as a JS double-quoted literal, escaping each
// surrogate code unit to `\uXXXX` (ref js_printer Utf16 escaper, quote.cppm:333).
inline std::string quote_astral_key_(const std::u16string& v) {
    std::string out;
    out.push_back('"');
    mbun::js_printer::StringSink sink{out};
    std::string_view bytes{reinterpret_cast<const char*>(v.data()), v.size() * sizeof(char16_t)};
    mbun::js_printer::write_pre_quoted_string_inner<mbun::js_printer::Encoding::Utf16>(
        bytes, sink, static_cast<std::uint8_t>('"'), /*asciiOnly=*/false, /*json=*/false);
    out.push_back('"');
    return out;
}

class Parser : public TypeParser {
public:
    explicit Parser(std::string_view src, bool cjs = false, bool jsx = false,
                    bool legacyDecorators = false, JsxOptions jsxOpts = {},
                    bool trimUnusedImports = false)
        : TypeParser{src, jsx, std::move(jsxOpts)}, cjs_{cjs},
          legacyDecorators_{legacyDecorators} {
        trim_.set_enabled(trimUnusedImports);
    }

    ParseResult run() {
        ParseResult result;
        if (!pre_lex_()) {
            result.ok = false;
            result.error = errMsg_;
            result.error_offset = errOff_;
            return result;
        }
        NodeIndex prog = parse_program_();
        // Post-parse, because the decision is order-independent: `export {a}`
        // may precede the `import {a}` it keeps alive. Records erasure edits
        // only — a no-op unless trim is on. Never on a failed parse: the edits
        // are keyed to spans a broken token stream may never have closed.
        if (ok_) {
            // The JSX lowering rewrites SOURCE, so the names it emits into its
            // replacement text (`<Foo a={v}/>` -> `jsxDEV(Foo, {a: v})`) never
            // reach the identifier arm. Drain them here — after parse_program_,
            // so the imports they refer to are already registered.
            for (const std::string& n : jsxLower_.jsx_value_refs()) {
                trim_.mark_used(n);
            }
            trim_.apply(arena_);
        }
        inject_jsx_runtime_import_();
        result.arena = std::move(arena_);
        result.program = prog;
        result.ok = ok_;
        result.error = errMsg_;
        result.error_offset = errOff_;
        result.cjs_esm_module = cjsEsmModule_;
        result.cjs_esm_export = cjsEsmExport_;
        result.cjs_live_export = cjsLiveExport_;
        result.cjs_live_names = cjsLiveNames_;
        result.top_level_await = topLevelAwait_;
        return result;
    }

private:
    // ── automatic-runtime import injection ───────────────────────────────────
    // The automatic runtime's whole point is that the file does NOT have to
    // import anything itself, so the lowering owes it the import: `jsxDEV(...)`
    // is a free variable until one exists. ref js_parser/parser.rs:719-746
    // (`runtime_import_names` — the same "import exactly the symbols the
    // lowering referenced" contract) and jsx.rs:246-256 (`set_import_source` —
    // the module is `package_name` + `/jsx-dev-runtime`|`/jsx-runtime`).
    //
    // A zero-width edit at offset 0 puts it before everything. That is safe
    // against a hashbang for a reason worth stating: parse_program_ ERASES `#!`
    // outright (see its note), so there is no `#!` left at offset 0 to displace.
    //
    // ⚠️ Deliberately NOT emitted by the Bun.Transpiler path — see the caller in
    // transpile_(). bun's transformSync prints the `jsxDEV_…` CALL but no import
    // (verified: the oracle's whole output for `const a = <div/>;` is the one
    // `const` line), because import injection belongs to the linker, which
    // transformSync does not run.
    void inject_jsx_runtime_import_() {
        const JsxUsed& u = jsx_used();
        if (!u.any() || !jsx_options().inject_import) {
            return;
        }
        std::vector<NamedSpec> named;
        if (u.jsx_dev) {
            named.push_back({"jsxDEV", std::string{kJsxDev}});
        }
        if (u.jsx) {
            named.push_back({"jsx", std::string{kJsx}});
        }
        if (u.jsxs) {
            named.push_back({"jsxs", std::string{kJsxs}});
        }
        if (u.fragment) {
            named.push_back({"Fragment", std::string{kFragment}});
        }
        const std::string spec{"\"" + jsx_options().import_module() + "\""};
        if (cjs_) {
            // `src_.size() + 1` as the uniqueness seed, not 0: build_cjs_import_
            // names its temp `__mbun_i<seed>` from the import's START OFFSET, and
            // a real `import` written at offset 0 — the overwhelmingly common
            // shape of a JSX file — would take `__mbun_i0` too and the two
            // `const` declarations would collide. One past the end of the source
            // is the one seed no token can hold. (Same trick as the module-scope
            // `using` frame's envId; see parse_program_.)
            arena_.add_edit(0, 0,
                            build_cjs_import_(static_cast<std::uint32_t>(src_.size() + 1), "", "",
                                              named, spec, false));
            return;
        }
        std::string s{"import { "};
        for (std::size_t i{0}; i < named.size(); ++i) {
            if (i != 0) {
                s += ", ";
            }
            s += named[i].name + " as " + named[i].alias;
        }
        s += " } from " + spec + ";";
        arena_.add_edit(0, 0, std::move(s));
    }

    // ── state ────────────────────────────────────────────────────────────────
    bool insideClass_{false};
    bool classHasSuper_{false};  // enclosing class has an `extends` clause (for param-property init placement)
    bool allowPrivateBrand_{false};
    bool cjs_{false};          // ESM → CommonJS lowering mode
    bool legacyDecorators_{false};  // TS experimentalDecorators: erase, no stage-3 lowering
    // TS unused-import elision (bun `trim_unused_imports`). OFF unless the
    // caller asks: Bun.Transpiler leaves it off, the runtime loader turns it on.
    // See js_parser/trim_imports.cppm for the three measured defaults.
    detail::ImportTrimmer trim_;
    bool cjsEsmModule_{false};  // saw a top-level ESM import/export in CJS mode
    bool cjsEsmExport_{false};  // saw an ESM `export` → module needs __esModule marker
    bool cjsLiveExport_{false};  // emitted a __mbun_X call → prelude must define the helper
    std::vector<std::pair<std::string, std::string>> cjsLiveNames_;  // (key, local) per __mbun_X
    int fnDepth_{0};            // nesting depth of function/method/arrow bodies
    bool topLevelAwait_{false};  // saw `await` at module top level (fnDepth_ == 0)
    // `yield` is a contextual keyword: a YieldExpression only inside a generator
    // body, a plain identifier anywhere else (`const yield = 1` is legal sloppy
    // JS). Mirrors bun's `fn_or_arrow_data_parse.allow_yield`
    // (AwaitOrYield::AllowExpr vs AllowIdent, src/js_parser/parser.rs:1529-1544).
    // Saved/restored around every function-ish body; arrows inherit it.
    bool inGenerator_{false};

    // Per-block frame for `using`/`await using` lowering (explicit resource mgmt).
    struct UsingFrame {
        std::uint32_t envId{0};
        std::uint32_t bodyStart{0};  // byte offset just after the block's `{`
        bool hasUsing{false};
        bool hasAsync{false};
    };
    std::vector<UsingFrame> usingFrames_;

    // ── module-level `using` lowering ────────────────────────────────────────
    // ref: bun src/js_parser/p.rs LowerUsingDeclarationsContext::finalize. A
    // top-level `using` moves the whole module body into a try/catch/finally,
    // so every statement that may not appear inside a block (import, re-export)
    // has to be hoisted back out and exported bindings republished through a
    // trailing `export {}` clause. The form tells emit_module_using_wrapper_
    // which of those each top-level `export` needs.
    //
    // This IS ast::ExportForm, not a copy of it: the same tag now also rides in
    // `ExportDecl::aux` for the printer, and two enums with the same eight cases
    // would drift the first time anyone added a ninth. Aliased rather than
    // replaced so the ~15 `ExForm::` spellings below keep reading as the local
    // concept they always were.
    using ExForm = mbun::ast::ExportForm;
    // One top-level statement, recorded by parse_program_ for the wrapper.
    struct TopStmt {
        std::uint32_t start{0};  // statement start (the `export`/`import` keyword)
        std::uint32_t end{0};    // one past the statement
        std::uint32_t pivot{0};  // Decl: start of the declaration; DefaultExpr: end of `default`
        NodeKind kind{NodeKind::Missing};
        ExForm form{ExForm::None};
        NodeIndex inner{NONE};  // Decl: the declaration node (for collect_export_names_)
    };
    std::vector<TopStmt> topStmts_;
    // Written by parse_export_ just before it returns, read by parse_program_
    // only when the statement it just parsed is itself an ExportDecl. An outer
    // `export` (e.g. `export namespace N { export const x = 1 }`) writes after
    // the nested one has returned, so the outermost value always wins.
    ExForm lastExForm_{ExForm::None};
    std::uint32_t lastExPivot_{0};

    // Stack of enclosing TS namespace names; when non-empty an `export` inside
    // targets the namespace object (`N.x = x`) instead of module exports.
    std::vector<std::string> nsTarget_;

    // ── ES stage-3 decorator lowering (TC39 decorators, esbuild-style) ──────
    // A decorator expression's source span (excluding the leading '@').
    struct DecSpan {
        std::uint32_t start{0};
        std::uint32_t end{0};
    };
    // One class member the lowering must touch: a decorated member, an
    // `accessor` member (JSC has no native support), or a computed-key member
    // of a decorated class (its key is hoisted to preserve evaluation order).
    struct DecMember {
        int kind{0};  // 0 method, 1 getter, 2 setter, 3 field, 4 accessor
        bool isStatic{false};
        bool isPrivate{false};
        bool isComputed{false};
        bool isAccessor{false};
        int elem{-1};    // element index in the runtime context (decorated only)
        int keyIdx{-1};  // hoisted computed-key index
        std::string nameArg;   // emission-ready `._m` name argument (public keys)
        std::string keyLiteral;          // normalized `"\uXXXX…"` for a supra-BMP string key
        bool keyNeedsRewrite{false};     // rewrite the class-body key span with keyLiteral
        std::string privName;  // private name without '#'
        std::vector<DecSpan> decs;             // decorator exprs, source order
        std::uint32_t declStart{0};            // member start after decorators
        std::uint32_t nameStart{0}, nameEnd{0};        // key span (incl. [] if computed)
        std::uint32_t keyExprStart{0}, keyExprEnd{0};  // computed key expr span
        bool hasInit{false};
        std::uint32_t initStart{0}, initEnd{0};  // field initializer expr span
        std::uint32_t noInitPos{0};              // where `= …` goes when no initializer
        std::uint32_t memberEnd{0};              // just past the member (delegators)
        std::uint32_t accKwStart{0}, accKwEnd{0};  // `accessor` keyword span
        bool hasGetSetKw{false};
        std::uint32_t gsStart{0}, gsEnd{0};  // `get`/`set` keyword span
        // TS legacy: a decorated field removed from the class body (set-semantics).
        // `dropInitText`/`dropKeyText` hold the erasure-applied source captured
        // before the member's own edits are rolled back, ready for re-emission as
        // the relocated `<target> = (<init>)` assignment.
        bool legacyDrop{false};
        std::string dropInitText;
        std::string dropKeyText;
        // TS legacy parameter decorators: (param index, decorator exprs).
        std::vector<std::pair<int, std::vector<DecSpan>>> params;
    };
    struct ClassPlan {
        int site{0};  // unique lowering site id (names the runtime slot)
        std::vector<DecSpan> classDecs;
        std::string nameHint;   // NamedEvaluation hint for anonymous expressions
        std::string className;  // '' when anonymous
        std::uint32_t decoStart{0};   // first class-decorator '@' (or class kw)
        std::uint32_t classStart{0};  // `class` keyword
        bool hasExtends{false};
        std::uint32_t extStart{0}, extEnd{0};  // heritage expression span
        std::uint32_t bodyOpen{0};             // class body '{'
        bool exprForm{false};                  // class expression / anonymous default
        bool hasCtor{false};                   // an explicit constructor was parsed
        NodeIndex ctorBody{NONE};              // its body block (for super-call lookup)
        std::uint32_t ctorBraceStart{0};       // its body '{'
        std::vector<DecMember> members;        // source order
        // TS legacy constructor parameter decorators: (index, decorator exprs).
        std::vector<std::pair<int, std::vector<DecSpan>>> ctorParams;
        bool anyDec{false};
        bool anyAccessor{false};
    };
    std::vector<ClassPlan> planStack_;  // innermost class being parsed = back()
    int decSite_{0};
    std::vector<DecSpan> pendingClassDecs_;  // decorators awaiting their class
    std::uint32_t pendingDecoStart_{0};
    std::string pendingClassNameHint_;
    // NamedEvaluation hint: `nameHint_` applies iff the decorated class
    // expression starts exactly at token index `nameHintTok_`.
    std::string nameHint_;
    std::size_t nameHintTok_{static_cast<std::size_t>(-1)};
    bool pendingDefaultExportClass_{false};  // `export default class …` → expr-form lowering

    // Separator to emit before the `exports.x = x;` / `N.x = x;` run that an
    // `export <decl>` lowering appends after the declaration's last token.
    //
    // A `var`/`let`/`const` declaration may be terminated by ASI rather than an
    // explicit `;` (`export const v = 42`). Since the lowering is an erasure —
    // original bytes are kept and the appends spliced on — the newline ASI
    // relied on sits *after* the insert point, so the appends would fuse onto
    // the initializer: `const v = 42 exports.v = v;`. Emit the semicolon ASI
    // implied. Block-terminated declarations (function/class) need no
    // terminator, and the lowered enum/namespace forms already end in `;`.
    std::string_view export_append_sep_(NodeIndex inner) const {
        if (inner == NONE || arena_.at(inner).kind != NodeKind::VarDecl) {
            return "";
        }
        return prev_kind_() == Token::Semicolon ? "" : ";";
    }

    // The binding name of an `export default` function/class *declaration*, or ""
    // when the default export is anonymous (`export default class {}`) or an
    // expression. The cursor sits on `class` / `function` / `async` / `abstract`.
    // Unlike an anonymous default export, a named one declares that name in module
    // scope, so the CJS lowering must keep it a declaration.
    std::string default_decl_name_() const {
        std::size_t k{0};
        if ((ident_is_("async") && peek_kind_(1) == Token::Function) ||
            (ident_is_("abstract") && peek_kind_(1) == Token::Class)) {
            k = 1;  // `async function …` / `abstract class …`
        }
        if (peek_kind_(k) != Token::Function && peek_kind_(k) != Token::Class) {
            return {};
        }
        ++k;  // past `function` / `class`
        if (peek_kind_(k) == Token::Asterisk) {
            ++k;  // generator: `function* name`
        }
        // Anything else here is anonymous: `class {`, `class extends B`, `function (`.
        return peek_kind_(k) == Token::Identifier ? std::string{tok_at_(idx_ + k).raw}
                                                  : std::string{};
    }

    // Record a deletion of the raw span of the current token, then advance.
    // ── program / statements ─────────────────────────────────────────────────
    NodeIndex parse_program_() {
        std::vector<NodeIndex> stmts;
        // ref: bun src/js_parser/parse/parse_entry.rs — a leading hashbang is
        // consumed before statement parsing, never becoming a statement. Erase it
        // rather than just advancing: transpile() passes source through verbatim
        // plus edits, and the CJS preamble/async IIFE wrapper would push `#!` off
        // offset 0, where it is a SyntaxError. The lexer stops the token before
        // the newline, so erasing it keeps every line number intact.
        std::uint32_t bodyStart = 0;
        if (curk_() == Token::Hashbang) {
            arena_.add_edit(cur_().start, cur_().end);
            bodyStart = static_cast<std::uint32_t>(cur_().end);
            advance_();
        }
        // Module-scope frame, so a top-level `using` lowers against it instead of
        // referencing an env that no wrapper ever declares. envId is src_.size():
        // every block frame keys off its `{` offset, which is always in bounds, so
        // a one-past-the-end id can never collide with one. ref: bun
        // src/js_parser/parse/parse_entry.rs:945 (will_wrap_module_in_try_catch_for_using).
        usingFrames_.push_back(
            UsingFrame{static_cast<std::uint32_t>(src_.size()), bodyStart, false, false});
        while (ok_ && curk_() != Token::EndOfFile) {
            const std::uint32_t sStart = static_cast<std::uint32_t>(cur_().start);
            lastExForm_ = ExForm::None;
            NodeIndex s = parse_statement_();
            if (!ok_) {
                break;
            }
            if (s != NONE) {
                stmts.push_back(s);
                const bool isExport = arena_.at(s).kind == NodeKind::ExportDecl;
                topStmts_.push_back(TopStmt{sStart, prev_end_(),
                                            isExport ? lastExPivot_ : 0, arena_.at(s).kind,
                                            isExport ? lastExForm_ : ExForm::None,
                                            isExport ? arena_.at(s).a : NONE});
            }
        }
        const UsingFrame moduleFrame = usingFrames_.back();
        usingFrames_.pop_back();
        if (ok_) {
            emit_module_using_wrapper_(moduleFrame);
        }
        NodeIndex prog = arena_.make(NodeKind::Program, 0, src_.size());
        arena_.at(prog).listStart = arena_.commit_list(stmts);
        arena_.at(prog).listCount = static_cast<std::uint32_t>(stmts.size());
        return prog;
    }

    NodeIndex parse_statement_() {
        if (!ok_) {
            return NONE;
        }
        DepthGuard depth{this};  // nested blocks/loops recurse here (see kMaxParseDepth)
        if (!depth.ok) {
            return NONE;
        }
        // Leading decorators on a class declaration (`@dec class C {}` and the
        // `@dec export class C {}` form): capture the decorator expressions and
        // hand them to parse_class_, which lowers TC39 stage-3 decorator
        // semantics to plain JS (see emit_decorated_class_). The `@…` spans are
        // erased from the source; the captured text re-appears in the lowering.
        if (curk_() == Token::At) {
            std::uint32_t decoStart = cur_().start;
            std::vector<DecSpan> decs;
            while (ok_ && curk_() == Token::At) {
                decs.push_back(parse_one_decorator_());
            }
            if (!ok_) {
                return NONE;
            }
            pendingClassDecs_ = std::move(decs);
            pendingDecoStart_ = decoStart;
            NodeIndex s = parse_statement_();
            pendingClassDecs_.clear();  // safety: never leak onto a later class
            return s;
        }
        Token k = curk_();
        switch (k) {
        case Token::Semicolon: {
            NodeIndex n = arena_.make(NodeKind::EmptyStmt, cur_().start, cur_().end);
            advance_();
            return n;
        }
        case Token::OpenBrace:
            return parse_block_();
        case Token::Var:
            return parse_var_decl_(VarKind::Var, /*needSemi=*/true, /*allowIn=*/true);
        case Token::Const:
            // `const enum` is still an enum (erase from the `const` keyword).
            if (peek_kind_(1) == Token::Enum) {
                std::uint32_t constStart = cur_().start;
                advance_();
                return parse_enum_(constStart);
            }
            return parse_var_decl_(VarKind::Const, true, true);
        case Token::Function:
            return parse_function_(/*isDecl=*/true);
        case Token::Class:
            return parse_class_(/*isDecl=*/true);
        case Token::Enum:
            return parse_enum_();
        case Token::Return:
            return parse_return_();
        case Token::If:
            return parse_if_();
        case Token::For:
            return parse_for_();
        case Token::Throw:
            return parse_throw_();
        case Token::Try:
            return parse_try_();
        case Token::While:
            return parse_while_();
        case Token::Do:
            return parse_do_while_();
        case Token::Switch:
            return parse_switch_();
        case Token::Break:
        case Token::Continue:
            return parse_break_continue_();
        case Token::With:
            return parse_with_();
        case Token::Import:
            // `import(...)` / `import.meta` are expressions, not declarations.
            if (peek_kind_(1) == Token::OpenParen || peek_kind_(1) == Token::Dot) {
                break;
            }
            return parse_import_();
        case Token::Export:
            return parse_export_();
        default:
            break;
        }
        // Labeled statement: `label: stmt` (an identifier immediately followed by
        // a colon at statement position).
        if (curk_() == Token::Identifier && peek_kind_(1) == Token::Colon) {
            std::size_t start = cur_().start;
            std::string_view label = cur_().raw;
            advance_();  // label
            advance_();  // :
            NodeIndex body = parse_statement_();
            if (!ok_) {
                return NONE;
            }
            NodeIndex n = arena_.make(NodeKind::LabeledStmt, start, cur_().start);
            arena_.at(n).text = label;
            arena_.at(n).a = body;
            return n;
        }
        // Contextual-keyword-led statements.
        if (ident_is_("let") && is_binding_start_(peek_kind_(1))) {
            return parse_var_decl_(VarKind::Let, true, true);
        }
        // `using x = …` / `await using x = …` (explicit resource management).
        if (ident_is_("using") && peek_kind_(1) == Token::Identifier &&
            !tok_at_(idx_ + 1).newlineBefore) {
            return parse_using_decl_(/*isAsync=*/false, cur_().start);
        }
        if (ident_is_("await") && !tok_at_(idx_ + 1).newlineBefore &&
            tok_at_(idx_ + 1).kind == Token::Identifier &&
            std::string_view{tok_at_(idx_ + 1).ident} == "using" &&
            tok_at_(idx_ + 2).kind == Token::Identifier) {
            return parse_using_decl_(/*isAsync=*/true, cur_().start);
        }
        if (ident_is_("type") && peek_kind_(1) == Token::Identifier &&
            !tok_at_(idx_ + 1).newlineBefore) {
            return parse_type_alias_();
        }
        if ((ident_is_("namespace") || ident_is_("module")) &&
            (peek_kind_(1) == Token::Identifier || peek_kind_(1) == Token::StringLiteral)) {
            return parse_namespace_();
        }
        if (ident_is_("interface") && peek_kind_(1) == Token::Identifier) {
            return parse_interface_();
        }
        if (ident_is_("declare")) {
            // `declare …` is type-only: parse the following statement to find its
            // extent, then erase the whole thing from the output.
            std::uint32_t declStart = cur_().start;
            std::size_t editMark = arena_.edit_count();
            advance_();
            // `declare global { … }` — consume and erase the braced body whole
            // (bun parse_stmt.rs:1816). Parsing it as a statement would keep the
            // block (and its hoisting `var`s) in the output, because `global`
            // alone parses as an expression statement via ASI and the block
            // survives as a separate statement.
            if (ident_is_("global") && peek_kind_(1) == Token::OpenBrace) {
                advance_();  // global
                advance_();  // {
                int depth = 1;
                while (depth > 0 && curk_() != Token::EndOfFile) {
                    if (curk_() == Token::OpenBrace) ++depth;
                    else if (curk_() == Token::CloseBrace) --depth;
                    advance_();
                }
                arena_.truncate_edits(editMark);
                arena_.add_edit(declStart, prev_end_());
                return arena_.make(NodeKind::TypeScriptStmt, declStart, prev_end_());
            }
            NodeIndex inner = parse_statement_();
            if (!ok_) {
                return NONE;
            }
            // Drop any edits recorded inside; one outer deletion subsumes them.
            arena_.truncate_edits(editMark);
            arena_.add_edit(declStart, prev_end_());
            // ⚠️ Returns a TypeScriptStmt, NOT `inner` — ref parse/parse_stmt.rs:1882
            // `return Ok(Some(p.s(S::TypeScript {}, loc)))`. bun parses the inner
            // statement for its EXTENT ONLY and then discards it; the same is true
            // of `declare global` (:1816) and a declare'd namespace/enum
            // (parse/parse_typescript.rs:352-358).
            //
            // Returning `inner` was harmless while erasure was the only consumer —
            // the edit above already deletes the bytes, and that path never reads a
            // node. It stopped being harmless the moment a printer rebuilt output
            // FROM the tree: `declare namespace N{}` handed it a NamespaceDecl
            // indistinguishable from a real one, and `declare enum E{}` an EnumDecl.
            // Both must emit nothing (verified on real bun 1.4.0: each transforms to
            // ""), so the tree has to carry the erasure too, not just the edit list.
            //
            // ⚠️ NOT ported: bun's one exception at :1853 — `export declare var` at
            // NAMESPACE scope becomes a real `export var` (the bindings turn into
            // namespace exports). It is unreachable here because mbun does not lower
            // namespaces at all; it belongs with the SNamespace port, not this line.
            const std::uint32_t declEnd = prev_end_();
            NodeIndex ts = arena_.make(NodeKind::TypeScriptStmt, declStart, declEnd);
            (void)inner;
            return ts;
        }
        if (ident_is_("abstract") && peek_kind_(1) == Token::Class) {
            erase_current_token_();  // erase `abstract` class modifier
            return parse_class_(true);
        }
        return parse_expression_statement_();
    }

    static bool is_binding_start_(Token k) {
        return k == Token::Identifier || k == Token::OpenBracket || k == Token::OpenBrace;
    }

    NodeIndex parse_block_() {
        std::size_t start = cur_().start;
        advance_();  // '{'
        usingFrames_.push_back(UsingFrame{static_cast<std::uint32_t>(start),
                                          static_cast<std::uint32_t>(start) + 1, false, false});
        std::vector<NodeIndex> stmts;
        while (ok_ && curk_() != Token::CloseBrace && curk_() != Token::EndOfFile) {
            NodeIndex s = parse_statement_();
            if (!ok_) {
                usingFrames_.pop_back();
                return NONE;
            }
            if (s != NONE) {
                stmts.push_back(s);
            }
        }
        const UsingFrame frame = usingFrames_.back();
        usingFrames_.pop_back();
        emit_using_wrapper_(frame, static_cast<std::uint32_t>(cur_().start));  // '}' offset
        if (!expect_(Token::CloseBrace)) {
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::Block, start, cur_().end);
        arena_.at(n).listStart = arena_.commit_list(stmts);
        arena_.at(n).listCount = static_cast<std::uint32_t>(stmts.size());
        return n;
    }

    NodeIndex parse_expression_statement_() {
        std::size_t start = cur_().start;
        NodeIndex e = parse_expression_(/*allowIn=*/true);
        if (!ok_) {
            return NONE;
        }
        consume_semicolon_();
        NodeIndex n = arena_.make(NodeKind::ExpressionStmt, start, cur_().start);
        arena_.at(n).a = e;
        return n;
    }

    // Lenient ASI: consume an explicit ';' if present, otherwise accept.
    // When ASI applies right after an erased span (e.g. `x = {} as any` before a
    // `(...)` statement), the erasure can merge the two statements into a call /
    // index expression in the OUTPUT even though the parser separated them.
    // Make the parser's ASI decision explicit with a ';' edit in that case.
    void consume_semicolon_() {
        if (curk_() == Token::Semicolon) {
            advance_();
            return;
        }
        Token k = curk_();
        if (k != Token::OpenParen && k != Token::OpenBracket) {
            return;
        }
        const std::uint32_t here = prev_end_();
        for (const auto& e : arena_.edits()) {
            if (e.end == here && e.start < e.end) {
                arena_.add_edit(here, here, ";");
                return;
            }
        }
    }

    NodeIndex parse_var_decl_(VarKind kind, bool needSemi, bool allowIn) {
        std::size_t start = cur_().start;
        advance_();  // var/let/const
        std::vector<NodeIndex> decls;
        while (ok_) {
            NodeIndex name = parse_binding_name_();
            if (!ok_) {
                return NONE;
            }
            // TS definite-assignment assertion on a declarator: `let x!: T`.
            // Erase the `!` so only the JS binding survives.
            if (curk_() == Token::Exclamation) {
                erase_current_token_();
            }
            skip_optional_type_annotation_();
            if (!ok_) {
                return NONE;
            }
            NodeIndex init = NONE;
            if (curk_() == Token::Equals) {
                advance_();
                if (!arena_.at(name).text.empty()) {  // NamedEvaluation hint
                    nameHint_ = arena_.at(name).text;
                    nameHintTok_ = idx_;
                }
                init = parse_assign_(allowIn);
                if (!ok_) {
                    return NONE;
                }
            }
            NodeIndex d = arena_.make(NodeKind::VarDeclarator, start, cur_().start);
            arena_.at(d).a = name;
            arena_.at(d).b = init;
            decls.push_back(d);
            if (curk_() == Token::Comma) {
                advance_();
                continue;
            }
            break;
        }
        if (needSemi) {
            consume_semicolon_();
        }
        NodeIndex n = arena_.make(NodeKind::VarDecl, start, cur_().start);
        arena_.at(n).aux = static_cast<std::uint32_t>(kind);
        arena_.at(n).listStart = arena_.commit_list(decls);
        arena_.at(n).listCount = static_cast<std::uint32_t>(decls.size());
        return n;
    }

    // Hint text for the binding target parse_binding_name_ consumed most
    // recently, or empty when that target was a *pattern* rather than a plain
    // name. This reproduces, exactly, the `lastIdent` of the token scan this
    // recursive descent replaced: there, every non-identifier token cleared it,
    // so at a `=` it held a name iff the immediately preceding token was an
    // identifier — i.e. iff the target is a plain binding identifier. `[x = …]`
    // and `{a: b = …}` hint (`x` / `b`); `{a: {b} = …}` does not, because the
    // token before `=` is `}`. Keeping that rule byte-for-byte is what keeps the
    // NamedEvaluation lowering of `const [Foo = @dec class {}] = []` firing on
    // the same token as before.
    std::string lastBindingIdent_;

    // `= <expr>` after a binding target — ref lib.rs:5280
    // (`maybe_print_default_binding_value`), b.rs `default_value`.
    //
    // Defaults are parsed as real expressions so TS erasure and decorated-class
    // lowering inside them still apply (`const [Foo = @dec class {}] = []`). That
    // also keeps them out of the bound-name set for free: parse_assign_ swallows
    // the default, so its identifiers are never binding targets.
    NodeIndex parse_binding_default_() {
        if (curk_() != Token::Equals) {
            return NONE;
        }
        advance_();  // =
        if (!lastBindingIdent_.empty()) {  // NamedEvaluation hint
            nameHint_ = lastBindingIdent_;
            nameHintTok_ = idx_;
        }
        return parse_assign_(true);
    }

    // An object pattern's key: `{a: t}` / `{"a-b": t}` / `{0: t}` / `{default: t}`.
    // Computed keys (`{[k]: t}`) are handled by the caller — they are expressions.
    // Mirrors the key acceptance of the object-literal parser (:4048).
    NodeIndex parse_binding_key_() {
        const Token k = curk_();
        NodeKind kind{NodeKind::Identifier};
        if (k == Token::StringLiteral) {
            kind = NodeKind::StringLiteral;
        } else if (k == Token::NumericLiteral) {
            kind = NodeKind::NumberLiteral;
        } else if (k != Token::Identifier && k != Token::EscapedKeyword && !is_keyword_(k)) {
            expected_identifier_();
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::Identifier, cur_().start, cur_().end);
        arena_.at(n).kind = kind;
        // Identifier-like keys carry their name so the binding tree can be read
        // without the source buffer; literal keys keep bun's model, where the key
        // is an expression the printer re-prints from its own node (lib.rs:5163).
        if (kind == NodeKind::Identifier) {
            arena_.at(n).text = cur_().raw;
        }
        // A shorthand `{Foo = @dec class {}}` never reaches parse_binding_name_
        // (its target is synthesised below), so the key token is what the old
        // scan would have had in `lastIdent` at the `=`. Only a *plain*
        // identifier counted there — a string/number/keyword key cleared it — so
        // reproduce that split exactly or the hint fires where it never used to.
        if (k == Token::Identifier) {
            lastBindingIdent_ = cur_().ident.empty() ? std::string{cur_().raw} : cur_().ident;
        } else {
            lastBindingIdent_.clear();
        }
        advance_();
        return n;
    }

    // `[a, [b], ...rest]` — ref b.rs:84 `b::Array{items, has_spread}`. bun keeps
    // `has_spread` at the ARRAY level and asks `has_spread && i == len-1` at use
    // (binding.rs:230); the same asymmetry is kept here so the two models stay 1:1.
    NodeIndex parse_binding_array_() {
        const std::uint32_t start = cur_().start;
        advance_();  // [
        // ref parse/mod.rs:1002-1071 — `is_single_line` is sampled at three
        // points and only ever cleared: before the first element, around every
        // comma, and before the `]`. It affects whitespace only (b.rs:121).
        bool isSingleLine{!cur_().newlineBefore};
        std::vector<NodeIndex> items;
        bool hasSpread{false};
        while (ok_ && curk_() != Token::CloseBracket && curk_() != Token::EndOfFile) {
            if (curk_() == Token::Comma) {  // elision: `[, x]` — ref b.rs B::BMissing
                items.push_back(arena_.make(NodeKind::BindingMissing, cur_().start, cur_().start));
            } else {
                if (curk_() == Token::DotDotDot) {
                    hasSpread = true;
                    advance_();
                }
                const std::uint32_t elStart = cur_().start;
                NodeIndex target = parse_binding_name_();
                if (!ok_) {
                    return NONE;
                }
                // DEVIATION(ref mod.rs:1030 `if !has_spread && token == TEquals`):
                // bun refuses to parse a default after a rest element, so
                // `[...r = 1]` is a syntax error there. The token scan this
                // replaced accepted it, and accepting it is unobservable on valid
                // input, so the laxer form is kept — tightening it would be a
                // behaviour change smuggled in under a refactor.
                NodeIndex def = parse_binding_default_();
                if (!ok_) {
                    return NONE;
                }
                NodeIndex el = arena_.make(NodeKind::BindingElement, elStart, prev_end_());
                arena_.at(el).a = target;
                arena_.at(el).b = def;
                items.push_back(el);
            }
            if (curk_() != Token::Comma) {
                break;
            }
            if (cur_().newlineBefore) {
                isSingleLine = false;
            }
            advance_();  // ,
            if (cur_().newlineBefore) {
                isSingleLine = false;
            }
        }
        if (cur_().newlineBefore) {
            isSingleLine = false;
        }
        if (!expect_(Token::CloseBracket)) {
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::BindingArray, start, prev_end_());
        if (hasSpread) {
            arena_.at(n).flags |= bflags::HasSpread;
        }
        if (isSingleLine) {
            arena_.at(n).flags |= bflags::IsSingleLine;
        }
        if (!items.empty()) {
            arena_.at(n).listStart = arena_.commit_list(items);
            arena_.at(n).listCount = static_cast<std::uint32_t>(items.size());
        }
        lastBindingIdent_.clear();  // target was a pattern: no NamedEvaluation hint
        return n;
    }

    // `{a, b: c, [k]: v, d = 1, ...rest}` — ref b.rs `b::Object{properties}` and
    // `b::Property{flags, key, value, default}` (b.rs:68). Object rest is tagged
    // PER PROPERTY (`flags::Property::IsSpread`, binding.rs:258), unlike arrays.
    NodeIndex parse_binding_object_() {
        const std::uint32_t start = cur_().start;
        advance_();  // {
        // ref parse/mod.rs:1088-1126 — same three sample points as the array.
        bool isSingleLine{!cur_().newlineBefore};
        std::vector<NodeIndex> props;
        while (ok_ && curk_() != Token::CloseBrace && curk_() != Token::EndOfFile) {
            const std::uint32_t pstart = cur_().start;
            std::uint8_t pflags{0};
            NodeIndex key{NONE};
            NodeIndex value{NONE};
            if (curk_() == Token::DotDotDot) {  // `{...rest}`
                pflags |= bflags::IsSpread;
                advance_();
                value = parse_binding_name_();
            } else {
                if (curk_() == Token::OpenBracket) {
                    // `{[k]: v}` — the key is an EXPRESSION (`k` is read, not bound).
                    pflags |= bflags::IsComputed;
                    advance_();
                    key = parse_assign_(true);
                    if (!ok_) {
                        return NONE;
                    }
                    if (!expect_(Token::CloseBracket)) {
                        return NONE;
                    }
                } else {
                    key = parse_binding_key_();
                }
                if (!ok_) {
                    return NONE;
                }
                if (curk_() == Token::Colon) {
                    // `{a: b}` RENAMES — it is never a type annotation. `b` is the
                    // binding target and the only name this property contributes.
                    advance_();
                    value = parse_binding_name_();
                } else {
                    // shorthand `{a}` == `{a: a}` — ref lib.rs:5175, where the
                    // printer folds it back when key and target names match.
                    pflags |= bflags::WasShorthand;
                    // Read the key's fields out before `make` — it can reallocate
                    // the node vector and dangle the reference. `text` is a view
                    // into the SOURCE (not the arena), so copying the view is safe;
                    // materialising a std::string here would dangle instead.
                    const std::uint32_t ks{arena_.at(key).start};
                    const std::uint32_t ke{arena_.at(key).end};
                    const std::string_view kt{arena_.at(key).text};
                    value = arena_.make(NodeKind::BindingIdentifier, ks, ke);
                    arena_.at(value).text = kt;
                }
            }
            if (!ok_) {
                return NONE;
            }
            NodeIndex def = parse_binding_default_();
            if (!ok_) {
                return NONE;
            }
            NodeIndex p = arena_.make(NodeKind::BindingProperty, pstart, prev_end_());
            arena_.at(p).a = key;
            arena_.at(p).b = value;
            arena_.at(p).c = def;
            arena_.at(p).flags = pflags;
            props.push_back(p);
            if (curk_() != Token::Comma) {
                break;
            }
            if (cur_().newlineBefore) {
                isSingleLine = false;
            }
            advance_();  // ,
            if (cur_().newlineBefore) {
                isSingleLine = false;
            }
        }
        if (cur_().newlineBefore) {
            isSingleLine = false;
        }
        if (!expect_(Token::CloseBrace)) {
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::BindingObject, start, prev_end_());
        if (isSingleLine) {
            arena_.at(n).flags |= bflags::IsSingleLine;
        }
        if (!props.empty()) {
            arena_.at(n).listStart = arena_.commit_list(props);
            arena_.at(n).listCount = static_cast<std::uint32_t>(props.size());
        }
        lastBindingIdent_.clear();  // target was a pattern: no NamedEvaluation hint
        return n;
    }

    // A binding identifier, or a destructuring pattern.
    //
    // A binding is a TREE, not a token soup: `{a: b}` renames, `{[k]: v}` reads
    // `k` and binds only `v`, `[, x]` has a hole. bun models it as the `B` union
    // (src/ast/b.rs:35) reached from `G::Decl.binding` (src/ast/g.rs:20), and its
    // printer walks that tree (js_printer/lib.rs:5052 `print_binding`). This is
    // the recursive-descent image of it; the loose token scan it replaced could
    // only ever yield the flat SET of bound names, which is all the erasure
    // transpiler ever needed — the printer is the consumer that needs structure.
    //
    // ⚠️ The name set this tree yields must stay identical: collect_export_names_
    // (:1110) walks it to publish one `__mbun_X` getter per bound name, so that
    // `export const {a, b: c} = o` publishes `a` and `c` — exactly as bun does
    // (runtime.js:129-137 gives *every* export a getter, and `export var {request,
    // get, …} = http` — src/node-fallbacks/http.js:2 — is how bun's own node
    // shims are written).
    NodeIndex parse_binding_name_() {
        const Token k = curk_();
        if (k == Token::OpenBracket) {
            return parse_binding_array_();
        }
        if (k == Token::OpenBrace) {
            return parse_binding_object_();
        }
        if (k == Token::EscapedKeyword) {
            expected_identifier_();
            return NONE;
        }
        if (k != Token::Identifier) {
            expected_identifier_();
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::BindingIdentifier, cur_().start, cur_().end);
        arena_.at(n).text = cur_().raw;
        lastBindingIdent_ = cur_().ident.empty() ? std::string{cur_().raw} : cur_().ident;
        advance_();
        return n;
    }

    // Parse and ERASE a `: Type` annotation (variable / parameter / property /
    // return type). Records one deletion edit spanning the colon through the end
    // of the type so the erasure printer strips it from the original source.
    void skip_optional_type_annotation_() {
        if (curk_() == Token::Colon) {
            std::uint32_t start = cur_().start;
            advance_();
            parse_type_();
            if (!ok_) {
                return;
            }
            arena_.add_edit(start, prev_end_());
        }
    }

    // ── declarations (TS) ────────────────────────────────────────────────────
    NodeIndex parse_enum_() { return parse_enum_(cur_().start); }

    // Lower a TS `enum` to a JS IIFE via a source replacement edit. `eraseFrom`
    // is where the erased span begins (the `enum` keyword, or the preceding
    // `const` for `const enum`). Produces:
    //   var E; (function (E) { E[E["A"] = 0] = "A"; E["B"] = "x"; })(E || (E = {}));
    // Reverse mapping is emitted for numeric members (matching tsc); string
    // members get a forward-only assignment. Auto-increment tracks a running
    // integer counter that resets on each numeric-literal initializer.
    NodeIndex parse_enum_(std::uint32_t eraseFrom) {
        std::size_t start = cur_().start;
        advance_();  // enum
        if (curk_() != Token::Identifier) {
            expected_identifier_();
            return NONE;
        }
        std::string enumName{cur_().raw};
        NodeIndex name = arena_.make(NodeKind::Identifier, cur_().start, cur_().end);
        arena_.at(name).text = cur_().raw;
        advance_();
        if (!expect_(Token::OpenBrace)) {
            return NONE;
        }
        std::vector<NodeIndex> members;
        std::string body;
        long long counter = 0;
        bool counterValid = true;
        // Identifier-keyed member names seen so far. A later member's
        // initializer that is a bare reference to one of these (e.g.
        // `bbb = Français`) must resolve to the enum object slot, not a local
        // — inside the lowered IIFE the member lives at Enum["Français"], so a
        // raw `Français` throws ReferenceError (ref tsc enum emit: member refs
        // become `Enum.Name`). GH issue #11963.
        std::unordered_set<std::string> seenMembers;
        while (ok_ && curk_() != Token::CloseBrace && curk_() != Token::EndOfFile) {
            Token k = curk_();
            if (k != Token::Identifier && k != Token::StringLiteral) {
                expected_identifier_();
                return NONE;
            }
            // Property key as a JS string literal: identifiers get quoted; a
            // string-literal key already carries its quotes.
            std::string jsKey =
                (k == Token::StringLiteral) ? std::string{cur_().raw}
                                            : ('"' + std::string{cur_().raw} + '"');
            const std::string keyName{k == Token::Identifier ? std::string{cur_().raw}
                                                             : std::string{}};
            NodeIndex key = arena_.make(NodeKind::Identifier, cur_().start, cur_().end);
            advance_();
            NodeIndex init = NONE;
            std::string valueExpr;
            bool reverseMap = true;
            if (curk_() == Token::Equals) {
                advance_();
                std::uint32_t initStart = cur_().start;
                init = parse_assign_(true);
                if (!ok_) {
                    return NONE;
                }
                std::string_view initText =
                    src_.substr(initStart, prev_end_() - initStart);
                Token it = tok_at_(idx_ - 1).kind;  // last consumed initializer token
                bool singleTok = (tok_at_(idx_ - 1).start == initStart);
                if (singleTok && it == Token::StringLiteral) {
                    valueExpr = std::string{initText};
                    reverseMap = false;      // string enum member: no reverse map
                    counterValid = false;
                } else if (singleTok && it == Token::NumericLiteral) {
                    valueExpr = std::string{initText};
                    reverseMap = true;
                    counter = parse_int_literal_(initText) + 1;
                    counterValid = true;
                } else if (singleTok && it == Token::Identifier &&
                           seenMembers.count(std::string{initText}) != 0) {
                    // Reference to a prior member: resolve to the enum slot.
                    valueExpr = enumName + "[\"" + std::string{initText} + "\"]";
                    reverseMap = true;       // member values are numeric here
                    counterValid = false;
                } else {
                    valueExpr = '(' + std::string{initText} + ')';
                    reverseMap = true;       // assume numeric (tsc default)
                    counterValid = false;
                }
            } else if (counterValid) {
                valueExpr = std::to_string(counter);
                ++counter;
            } else {
                // Non-const predecessor with no initializer: not valid TS, but
                // avoid emitting broken JS — forward-assign undefined.
                valueExpr = "undefined";
                reverseMap = false;
            }
            if (reverseMap) {
                body += enumName + '[' + enumName + '[' + jsKey + "] = " + valueExpr +
                        "] = " + jsKey + "; ";
            } else {
                body += enumName + '[' + jsKey + "] = " + valueExpr + "; ";
            }
            NodeIndex m = arena_.make(NodeKind::EnumMember, cur_().start, cur_().end);
            arena_.at(m).a = key;
            arena_.at(m).b = init;
            members.push_back(m);
            if (!keyName.empty()) seenMembers.insert(keyName);
            if (curk_() == Token::Comma) {
                advance_();
                continue;
            }
            break;
        }
        if (!expect_(Token::CloseBrace)) {
            return NONE;
        }
        std::string lowered = "var " + enumName + "; (function (" + enumName + ") { " + body +
                              "})(" + enumName + " || (" + enumName + " = {}));";
        arena_.add_edit(eraseFrom, prev_end_(), std::move(lowered));
        NodeIndex n = arena_.make(NodeKind::EnumDecl, start, cur_().start);
        arena_.at(n).a = name;
        arena_.at(n).listStart = arena_.commit_list(members);
        arena_.at(n).listCount = static_cast<std::uint32_t>(members.size());
        return n;
    }

    // Parse a decimal / hex / octal / binary integer literal to long long for
    // enum auto-increment. Non-integer (float) falls back to 0 continuation.
    static long long parse_int_literal_(std::string_view t) {
        std::string s;
        s.reserve(t.size());
        for (char c : t) {
            if (c != '_') {
                s.push_back(c);
            }
        }
        try {
            std::size_t pos = 0;
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                return std::stoll(s.substr(2), &pos, 16);
            }
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
                return std::stoll(s.substr(2), &pos, 8);
            }
            if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
                return std::stoll(s.substr(2), &pos, 2);
            }
            if (s.find('.') != std::string::npos || s.find('e') != std::string::npos ||
                s.find('E') != std::string::npos) {
                return static_cast<long long>(std::stod(s));
            }
            return std::stoll(s, &pos, 10);
        } catch (...) {
            return 0;
        }
    }

    NodeIndex parse_type_alias_() {
        std::size_t start = cur_().start;
        advance_();  // type
        NodeIndex name = arena_.make(NodeKind::Identifier, cur_().start, cur_().end);
        advance_();
        if (curk_() == Token::LessThan) {
            parse_type_params_(/*allowEmpty=*/true, /*nested=*/true);
            if (!ok_) {
                return NONE;
            }
        }
        if (!expect_(Token::Equals)) {
            return NONE;
        }
        parse_type_();
        if (!ok_) {
            return NONE;
        }
        consume_semicolon_();
        NodeIndex n = arena_.make(NodeKind::TypeAliasDecl, start, cur_().start);
        arena_.at(n).a = name;
        arena_.add_edit(static_cast<std::uint32_t>(start), prev_end_());  // erase whole `type X = …;`
        return n;
    }

    NodeIndex parse_interface_() {
        std::size_t start = cur_().start;
        advance_();  // interface
        NodeIndex name = arena_.make(NodeKind::Identifier, cur_().start, cur_().end);
        advance_();
        if (curk_() == Token::LessThan) {
            parse_type_params_(/*allowEmpty=*/true, /*nested=*/true);
            if (!ok_) {
                return NONE;
            }
        }
        if (ident_is_("extends") || curk_() == Token::Extends) {
            advance_();
            parse_type_();
            while (ok_ && curk_() == Token::Comma) {
                advance_();
                parse_type_();
            }
            if (!ok_) {
                return NONE;
            }
        }
        if (curk_() == Token::OpenBrace) {
            if (!skip_balanced_()) {
                fail_("Parse error");
            }
        }
        NodeIndex n = arena_.make(NodeKind::InterfaceDecl, start, cur_().start);
        arena_.at(n).a = name;
        arena_.add_edit(static_cast<std::uint32_t>(start), prev_end_());  // erase whole interface
        return n;
    }

    // Lower a TS namespace/module to an IIFE:
    //   namespace N { export function f(){} }
    //   → var N; (function (N) { function f(){} N.f = f; })(N || (N = {}));
    // Exports inside the body target N (see parse_export_'s nsTarget_ branch).
    NodeIndex parse_namespace_() {
        std::uint32_t start = cur_().start;
        advance_();  // namespace/module
        std::string nsName{cur_().raw};
        NodeIndex name = arena_.make(NodeKind::Identifier, cur_().start, cur_().end);
        arena_.at(name).text = cur_().raw;
        advance_();
        // Qualified `namespace A.B.C {}` — lower only the outer name (rare; the
        // dotted tail is left inside the erased span for the common single-name case).
        while (ok_ && curk_() == Token::Dot) {
            advance_();
            if (curk_() == Token::Identifier) {
                advance_();
            }
        }
        if (curk_() == Token::OpenBrace) {
            const std::uint32_t bracePos = static_cast<std::uint32_t>(cur_().start);
            advance_();  // '{'
            arena_.add_edit(start, bracePos + 1,
                            "var " + nsName + "; (function (" + nsName + ") {");
            nsTarget_.push_back(nsName);
            usingFrames_.push_back(UsingFrame{bracePos, bracePos + 1, false, false});
            while (ok_ && curk_() != Token::CloseBrace && curk_() != Token::EndOfFile) {
                if (parse_statement_() == NONE && !ok_) {
                    break;
                }
            }
            const UsingFrame frame = usingFrames_.back();
            usingFrames_.pop_back();
            emit_using_wrapper_(frame, static_cast<std::uint32_t>(cur_().start));
            nsTarget_.pop_back();
            const std::uint32_t closePos = static_cast<std::uint32_t>(cur_().start);  // '}'
            if (ok_) {
                arena_.add_edit(closePos, closePos + 1,
                                "})(" + nsName + " || (" + nsName + " = {}));");
            }
            expect_(Token::CloseBrace);
        }
        NodeIndex n = arena_.make(NodeKind::NamespaceDecl, start, cur_().start);
        arena_.at(n).a = name;
        return n;
    }

    // True when the current `type` token introduces a TS type-only clause rather
    // than being the imported/exported binding literally named `type`.
    bool type_keyword_here_() const {
        if (!ident_is_("type")) {
            return false;
        }
        Token n1 = peek_kind_(1);
        if (n1 == Token::OpenBrace || n1 == Token::Asterisk) {
            return true;  // `type { … }` / `type * as …`
        }
        // `type Foo …` is type-only unless Foo is the contextual `from`/`as`
        // (which makes `type` the actual binding name, e.g. `import type from …`).
        if (n1 == Token::Identifier) {
            std::string_view w{tok_at_(idx_ + 1).ident};
            return w != "from" && w != "as";
        }
        return false;
    }

    // `import …` declaration. Runtime imports are kept verbatim (ESM; the module
    // loader handles linking). TS type-only imports are erased; inline `type`
    // specifiers are stripped. `import X = require("m")` lowers to `const`.
    NodeIndex parse_import_() {
        std::uint32_t start = cur_().start;
        if (cjs_ && nsTarget_.empty()) cjsEsmModule_ = true;
        advance_();  // import

        // `import X = require("m");` / `import X = A.B;`  → `const X = …;`
        if (curk_() == Token::Identifier && peek_kind_(1) == Token::Equals) {
            arena_.add_edit(start, prev_end_(), "const");  // replace `import` keyword
            // consume `X = <expr>;`
            advance_();       // X
            advance_();       // =
            parse_assign_(true);
            consume_semicolon_();
            NodeIndex eq = arena_.make(NodeKind::ImportDecl, start, cur_().start);
            // Not an import any more — the edit above already turned it into a
            // `const`. See ast.cppm mflags::IsTsImportEquals for why the printer
            // has to be told rather than left to infer it from the empty payload.
            arena_.at(eq).flags = mflags::IsTsImportEquals;
            return eq;
        }

        bool typeOnly = type_keyword_here_();
        if (typeOnly) {
            advance_();  // type
        }
        // ref p.rs:4067/4161/4242 — register this statement's local bindings in
        // the name-keyed map as they are parsed. A no-op unless trim is on.
        const std::uint32_t trimStmt = trim_.enabled() ? trim_.begin_stmt() : 0u;

        std::string defName, nsName, specRaw;
        std::vector<NamedSpec> named;
        bool sideEffect = false;
        // ── the AST half (ast.cppm ImportDecl) ───────────────────────────────
        // Everything below already parsed the import into C++ LOCALS and spent
        // them on build_cjs_import_; these mirror the same tokens into arena
        // nodes so the printer can rebuild the statement. Nothing here emits an
        // Edit, so the erasure path is bit-identical either way.
        NodeIndex defNode = NONE, nsNode = NONE;
        std::string_view specView;  // ImportDecl::text — raw slice, quotes included
        std::vector<NodeIndex> items;
        bool singleLine = true;

        if (curk_() == Token::StringLiteral) {
            // bare `import "mod";`
            specRaw = std::string{cur_().raw};
            specView = cur_().raw;
            sideEffect = true;
            advance_();
        } else {
            // default binding
            if (curk_() == Token::Identifier) {
                defName = std::string{cur_().raw};
                // Whole-statement-only: a default binding has no span that can be
                // erased on its own without rewriting the clause.
                trim_.add_binding(trimStmt, cur_().raw);
                defNode = make_spec_name_();
                advance_();
                if (curk_() == Token::Comma) {
                    advance_();
                }
            }
            if (curk_() == Token::Asterisk) {
                advance_();  // *
                if (ident_is_("as")) {
                    advance_();
                }
                if (curk_() == Token::Identifier) {
                    nsName = std::string{cur_().raw};
                    trim_.add_binding(trimStmt, cur_().raw);
                    nsNode = make_spec_name_();
                    advance_();
                }
            } else if (curk_() == Token::OpenBrace) {
                parse_import_export_specifiers_(cjs_ ? &named : nullptr, &items, &singleLine);
                // Register each clause item's LOCAL name. For an import that is
                // the `as` side when present (`import {a as b}` binds `b`) — the
                // mirror of the export clause, whose local is the L side. The
                // ClauseItem's own span (trailing comma included) is what a
                // partial trim erases, exactly as the inline `{type a}` arm does.
                for (const NodeIndex ci : items) {
                    const Node& item = arena_.at(ci);
                    const NodeIndex localN = item.b != NONE ? item.b : item.a;
                    if (localN != NONE) {
                        trim_.add_binding(trimStmt, arena_.at(localN).text, item.start, item.end);
                    }
                }
            }
            if (ident_is_("from")) {
                advance_();
                if (curk_() == Token::StringLiteral) {
                    specRaw = std::string{cur_().raw};
                    specView = cur_().raw;
                    advance_();
                }
            }
        }
        // Import attributes / assertions: `with { type: "json" }` (or legacy
        // `assert { … }`). Consume the clause so it is not mis-parsed as a
        // `with` statement, capturing `type` — it selects the loader, so it is
        // the one attribute that changes what the import evaluates to.
        std::string typeAttr;
        if (curk_() == Token::With || ident_is_("assert")) {
            advance_();  // with / assert
            if (curk_() == Token::OpenBrace && !parse_import_attributes_(&typeAttr)) {
                fail_("Parse error");
                return NONE;
            }
        }
        consume_semicolon_();
        // The trimmable span is the one the type-only arm erases just below. A
        // type-only import is already erased whole here, so it is not trimmable —
        // there is nothing left to trim.
        //
        // CJS *is* trimmable, and must be: this is the RUNTIME's configuration
        // (module_loader passes cjs=true), so excluding it made the whole feature
        // a no-op exactly where it is supposed to fire — `bun run x.ts` would
        // still require() a module reached only through a type. The lowering
        // recorded just below claims this same span; the trimmer retracts it at
        // apply() time via Arena::blank_edit rather than recording a second edit
        // that erase_slice would drop as an overlap.
        trim_.finish_stmt(trimStmt, start, prev_end_(), !typeOnly);
        if (typeOnly) {
            arena_.add_edit(start, prev_end_());  // erase whole type-only import
        } else if (cjs_) {
            arena_.add_edit(start, prev_end_(),
                            build_cjs_import_(start, defName, nsName, named, specRaw, sideEffect,
                                              typeAttr));
        }
        NodeIndex n = arena_.make(NodeKind::ImportDecl, start, cur_().start);
        Node& in = arena_.at(n);
        in.text = specView;
        in.a = defNode;
        in.b = nsNode;
        in.listStart = arena_.commit_list(items);
        in.listCount = static_cast<std::uint32_t>(items.size());
        in.flags = static_cast<std::uint8_t>((singleLine ? mflags::IsSingleLine : 0u) |
                                             (typeOnly ? mflags::IsTypeOnly : 0u));
        return n;
    }

    // The `{ type: "text", … }` body of an import-attributes / import-assertions
    // clause. Consumes exactly the balanced braces (so the caller's contract is
    // unchanged from the skip_balanced_() this replaced) and writes the `type`
    // value — unquoted — through `outType`.
    //
    // Only `type` is captured because only `type` is load-bearing: it selects the
    // loader, and the loader decides what the module evaluates to
    // (.mbun/bun-ref/src/bundler/options.rs:600-604 — a `type` attribute replaces
    // the extension-derived loader). Every other attribute is inert for this
    // transpiler, so they are consumed and dropped.
    //
    // Grammar is deliberately permissive rather than validating: an attribute key
    // is an IdentifierName *or* a string literal and the value must be a string
    // literal (tc39/proposal-import-attributes). Anything that does not fit that
    // shape is skipped rather than rejected — this is an *erasure* transpiler, so
    // rejecting a clause bun accepts would be strictly worse than ignoring an
    // attribute it does not implement. Returns false only on an unbalanced clause,
    // which is the one case the caller must report (it means the token stream is
    // not an attributes clause at all).
    //
    // A value containing an escape is skipped: `raw` is the source slice with the
    // quotes still on, so an escaped value would need decoding, and no real loader
    // name ("text"/"json"/"file"/…) contains one.
    bool parse_import_attributes_(std::string* outType) {
        advance_();  // {
        int depth = 1;
        while (depth > 0) {
            const Token k = curk_();
            if (k == Token::EndOfFile) {
                return false;
            }
            if (k == Token::OpenBrace) {
                ++depth;
                advance_();
                continue;
            }
            if (k == Token::CloseBrace) {
                --depth;
                advance_();
                continue;
            }
            // `key : "value"` at the top level of the clause.
            const bool keyIsIdent = (k == Token::Identifier || is_keyword_(k) ||
                                     k == Token::EscapedKeyword);
            const bool keyIsString = (k == Token::StringLiteral);
            if (depth == 1 && (keyIsIdent || keyIsString) && peek_kind_(1) == Token::Colon) {
                const bool isType =
                    keyIsIdent ? (std::string_view{cur_().ident} == "type")
                               : (std::string_view{cur_().raw} == "\"type\"" ||
                                  std::string_view{cur_().raw} == "'type'");
                advance_();  // key
                advance_();  // :
                if (curk_() == Token::StringLiteral) {
                    const std::string_view raw{cur_().raw};
                    if (isType && outType != nullptr && raw.size() >= 2 &&
                        raw.find('\\') == std::string_view::npos) {
                        *outType = std::string{raw.substr(1, raw.size() - 2)};
                    }
                    advance_();  // value
                }
                continue;
            }
            advance_();
        }
        return true;
    }

    // A `{ a, b as c, type D }` clause shared by import and export. Erases inline
    // `type X` / `type X as Y` specifiers (including a trailing comma). When `out`
    // is non-null, each value (non-type) specifier is captured as {name, alias}
    // for CJS codegen.
    //
    // `items`/`singleLine` are the AST-rebuild printer's half (ast.cppm ClauseItem):
    // each value specifier also becomes a ClauseItem node, and `*singleLine` reports
    // whether the braces held a newline. Both are pure ADDITIONS — they write nodes
    // and a bool, never an Edit — so the erasure path cannot see them. `out` stays
    // separate rather than being derived from `items` because the two disagree on
    // purpose: CJS codegen wants owning std::strings it can concatenate, the arena
    // wants non-owning views, and `out` is populated ONLY in CJS mode while `items`
    // is populated always.
    void parse_import_export_specifiers_(std::vector<NamedSpec>* out = nullptr,
                                         std::vector<NodeIndex>* items = nullptr,
                                         bool* singleLine = nullptr,
                                         bool* hadTypeOnly = nullptr) {
        advance_();  // {
        // ref s.rs:207 `is_single_line`. Only newlines INSIDE the braces count, so
        // the scan starts at the token AFTER `{` and includes the `}` itself:
        // `import\n{a}` is single-line but `import {a\n}` is not (both verified
        // against bun 1.4.0 — see ast.cppm mflags::IsSingleLine).
        bool multiline = false;
        while (ok_ && curk_() != Token::CloseBrace && curk_() != Token::EndOfFile) {
            std::size_t loopStart = idx_;
            std::uint32_t specStart = cur_().start;
            multiline = multiline || cur_().newlineBefore;
            bool specType = type_keyword_here_();
            if (specType) {
                advance_();  // type
            }
            // An import/export specifier name is any IdentifierName — including
            // reserved words such as `default` (`import { default as x }`) — or a
            // module-export string. Accept keyword tokens so the loop advances.
            std::string name, alias;
            NodeIndex lNode = NONE, rNode = NONE;
            if (is_spec_name_(curk_())) {
                name = std::string{cur_().raw};
                lNode = make_spec_name_();
                advance_();
            }
            alias = name;
            if (ident_is_("as")) {
                advance_();
                if (is_spec_name_(curk_())) {
                    alias = std::string{cur_().raw};
                    rNode = make_spec_name_();
                    advance_();
                }
            }
            std::uint32_t specEnd = prev_end_();
            if (curk_() == Token::Comma) {
                advance_();
                specEnd = prev_end_();
            }
            if (specType) {
                // ref parse_import_export.rs:334 `had_type_only_exports = true`.
                // Only the INLINE `{type a}` form sets it; `export type {a}` is the
                // caller's `typeReexport`, a different (whole-statement) erasure.
                if (hadTypeOnly != nullptr) {
                    *hadTypeOnly = true;
                }
                arena_.add_edit(specStart, specEnd);
            } else {
                if (out != nullptr && !name.empty()) {
                    out->push_back(NamedSpec{std::move(name), std::move(alias)});
                }
                if (items != nullptr && lNode != NONE) {
                    NodeIndex ci = arena_.make(NodeKind::ClauseItem, specStart, specEnd);
                    arena_.at(ci).a = lNode;
                    arena_.at(ci).b = rNode;  // NONE when the source wrote no `as`
                    items->push_back(ci);
                }
            }
            if (idx_ == loopStart) {
                // No token consumed — bail rather than spin forever.
                fail_("Parse error");
                return;
            }
        }
        multiline = multiline || (curk_() == Token::CloseBrace && cur_().newlineBefore);
        if (singleLine != nullptr) {
            *singleLine = !multiline;
        }
        expect_(Token::CloseBrace);
    }

    // The current token as a clause/namespace NAME node. mbun splits what bun's
    // `ArenaStr` alias covers across two kinds, exactly as an object key is split
    // (property.cppm:353-357): an IdentifierName is an Identifier, a module-export
    // string (`import { "a-b" as c }`) is a StringLiteral whose text is the source
    // slice WITH its quotes. Caller advances.
    NodeIndex make_spec_name_() {
        NodeIndex n = arena_.make(curk_() == Token::StringLiteral ? NodeKind::StringLiteral
                                                                  : NodeKind::Identifier,
                                  cur_().start, cur_().end);
        arena_.at(n).text = cur_().raw;
        return n;
    }

    // An import/export specifier name is an IdentifierName *or* a module-export
    // string (`import { "a-b" as x }`), whose captured raw keeps its quotes. The
    // two forms need different property syntax.
    // Collect the simple exported binding names of a declaration (for CJS
    // `exports.x = x`). Destructuring patterns (non-Identifier names) are skipped.
    void collect_export_names_(NodeIndex decl, std::vector<std::string>& names) {
        if (decl == NONE) {
            return;
        }
        const Node& n = arena_.at(decl);
        // Every name a declaration binds, in source order. For a destructuring
        // pattern that means walking the binding TREE parse_binding_name_ builds
        // (:775) and taking its leaves, so `export const {a, b: c} = o` publishes
        // `a` and `c` — one getter each, as bun does (runtime.js:129-137). The set
        // is exactly what the flat name list this replaced carried: keys are not
        // names (`{a: b}` binds only `b`), computed keys are expressions
        // (`{[k]: v}` binds only `v`), and defaults were swallowed by parse_assign_
        // so they never appear here at all. A pattern that binds nothing
        // (`export const {} = o`) contributes nothing.
        //
        // FunctionDecl/ClassDecl/EnumDecl/NamespaceDecl names are plain
        // Identifier nodes (not bindings), so both kinds are accepted.
        auto push_name = [&](this auto&& self, NodeIndex nameNode) -> void {
            if (nameNode == NONE) {
                return;
            }
            const NodeKind k = arena_.at(nameNode).kind;
            if (k == NodeKind::Identifier || k == NodeKind::BindingIdentifier) {
                const std::string_view t = arena_.at(nameNode).text;
                if (!t.empty()) {
                    names.emplace_back(t);
                }
                return;
            }
            // Nothing here mutates the arena, so each child span stays valid.
            if (k == NodeKind::BindingArray) {
                for (NodeIndex el : arena_.list_of(arena_.at(nameNode))) {
                    // BindingElement.a is the target; a `[, x]` hole is a bare
                    // BindingMissing whose `.a` is NONE and so binds nothing.
                    self(arena_.at(el).a);
                }
                return;
            }
            if (k == NodeKind::BindingObject) {
                for (NodeIndex p : arena_.list_of(arena_.at(nameNode))) {
                    self(arena_.at(p).b);  // BindingProperty.b — the target, not the key
                }
            }
        };
        switch (n.kind) {
        case NodeKind::VarDecl:
            for (NodeIndex d : arena_.list_of(n)) {
                push_name(arena_.at(d).a);
            }
            break;
        case NodeKind::FunctionDecl:
        case NodeKind::ClassDecl:
        case NodeKind::EnumDecl:
        case NodeKind::NamespaceDecl:
            push_name(n.a);
            break;
        default:
            break;
        }
    }

    // The ExportDecl node for one `export …`, tagged with its form (ast.cppm
    // ExportForm). Every `return` in parse_export_ goes through here so the tag
    // can never be forgotten — an untagged node defaults to ExportForm::None,
    // which the printer reads as "type-only, print nothing", and a silently
    // unprinted export is exactly the failure this slice exists to end.
    NodeIndex make_export_(std::uint32_t start, ExForm form, NodeIndex value = NONE) {
        NodeIndex n = arena_.make(NodeKind::ExportDecl, start, cur_().start);
        arena_.at(n).aux = static_cast<std::uint32_t>(form);
        arena_.at(n).a = value;
        return n;
    }

    // `export …`. In ESM-preserve mode the `export` keyword is kept (value decls)
    // or erased (type-only). In CJS mode every form lowers to `exports.x = …` /
    // `require()` re-exports. Declarations delegate to parse_statement_ so nested
    // type erasure / enum lowering still applies underneath.
    NodeIndex parse_export_() {
        std::uint32_t start = cur_().start;
        std::size_t editMark = arena_.edit_count();
        if (cjs_ && nsTarget_.empty()) cjsEsmModule_ = true;
        advance_();  // export

        // export = expr;  (TS export-assignment) → lower to `module.exports = expr`
        if (curk_() == Token::Equals) {
            arena_.add_edit(start, cur_().end, "module.exports =");
            advance_();  // =
            NodeIndex value = parse_assign_(true);
            consume_semicolon_();
            lastExForm_ = ExForm::Assign;
            return make_export_(start, ExForm::Assign, value);
        }

        // export default …
        if (curk_() == Token::Default) {
            advance_();  // default
            const std::uint32_t defEnd = prev_end_();  // end of `default`
            lastExForm_ = ExForm::DefaultExpr;
            // The node's own tag. NOT read back off lastExForm_ at the return:
            // that member is set BEFORE the inner declaration is parsed (unlike the
            // `export <decl>` arm, which sets it after), so anything nested that
            // called parse_export_ would have overwritten it. No such nesting is
            // legal inside a `export default` declaration today — this is a local
            // because relying on that is a footgun, not because it is currently a
            // bug.
            ExForm form = ExForm::DefaultExpr;
            lastExPivot_ = defEnd;
            const std::uint32_t declStart = cur_().start;
            NodeIndex inner = NONE;
            // A *named* `export default class C {}` / `function f() {}` binds `C`/`f`
            // in module scope on top of the default export, so a later `export { C }`
            // — or any other reference to it in the module — resolves. Anonymous
            // forms bind nothing.
            std::string defName{default_decl_name_()};
            // `abstract class` only reaches declaration form when named: erasing the
            // modifier leaves `class {}`, which is not a legal declaration anonymous.
            if (curk_() == Token::Function || curk_() == Token::Class ||
                (ident_is_("async") && peek_kind_(1) == Token::Function) ||
                (ident_is_("abstract") && peek_kind_(1) == Token::Class && !defName.empty())) {
                lastExForm_ = ExForm::DefaultDecl;
                form = ExForm::DefaultDecl;
                // An *anonymous* decorated/`accessor` default-export class must lower
                // in expression form: the position is an expression in both ESM
                // (`export default <expr>`) and CJS (`exports.default = <expr>`). A
                // named one stays a declaration so its binding survives — the
                // decorator lowering emits declaration form for it exactly as it
                // does for a non-default `export class C`.
                pendingDefaultExportClass_ = curk_() == Token::Class && defName.empty();
                if (pendingDefaultExportClass_) {
                    pendingClassNameHint_ = "default";  // NamedEvaluation
                }
                inner = parse_statement_or_async_function_();
                pendingDefaultExportClass_ = false;
            } else {
                defName.clear();  // an expression declares no binding
                nameHint_ = "default";  // e.g. `export default (@dec class {})`
                nameHintTok_ = idx_;
                inner = parse_assign_(true);
                consume_semicolon_();
            }
            if (cjs_) {
                cjsEsmExport_ = true;
                if (defName.empty()) {
                    // `export default <expr>` → `exports.default = <expr>`.
                    arena_.add_edit(start, defEnd, "exports.default =");
                } else {
                    // `export default class C {}` → `class C {}` + `exports.default = C;`.
                    // Keeping the *declaration* is what binds `C` in module scope:
                    // lowering to `exports.default = class C {}` scopes the name to the
                    // class body alone, so a later `export { C }` threw ReferenceError.
                    arena_.add_edit(start, declStart);  // erase `export default `
                    const std::uint32_t at = prev_end_();
                    arena_.add_edit(at, at, std::string{export_append_sep_(inner)} +
                                                " exports.default = " + defName + ";");
                }
            }
            return make_export_(start, form, inner);
        }

        // export * [as ns] from "m";
        if (curk_() == Token::Asterisk) {
            advance_();  // *
            std::string nsName;
            NodeIndex nsNode = NONE;
            std::string_view specView;
            if (ident_is_("as")) {
                advance_();
                // ⚠️ Identifier only, exactly as before. `export * as "a-b" from "y"`
                // is legal (bun 1.4.0 prints it back verbatim) but this parser has
                // never consumed the string form, so `from` is not found and the
                // statement mis-parses. Widening the token test here would also
                // change the CJS lowering's `exports.<nsName>` — a second bug, in a
                // path this slice is not allowed to disturb. Left as-is and recorded:
                // the AST models what the parser accepts, not more.
                if (curk_() == Token::Identifier) {
                    nsName = std::string{cur_().raw};
                    nsNode = make_spec_name_();
                    advance_();
                }
            }
            std::string specRaw;
            if (ident_is_("from")) {
                advance_();
                if (curk_() == Token::StringLiteral) {
                    specRaw = std::string{cur_().raw};
                    specView = cur_().raw;
                    advance_();
                }
            }
            consume_semicolon_();
            if (cjs_) {
                cjsEsmExport_ = true;
                std::string repl{
                    nsName.empty()
                        ? std::string{kObjectAlias} + ".assign(exports, require(" + specRaw + "));"
                        : "exports." + nsName + " = require(" + specRaw + ");"};
                arena_.add_edit(start, prev_end_(), repl);
            }
            lastExForm_ = ExForm::Star;
            NodeIndex n = make_export_(start, ExForm::Star);
            arena_.at(n).b = nsNode;
            arena_.at(n).text = specView;
            return n;
        }

        // export type { … } [from "m"];  → whole type-only re-export erased
        bool typeReexport = ident_is_("type") && peek_kind_(1) == Token::OpenBrace;

        // export { … } [from "m"];
        if (curk_() == Token::OpenBrace || typeReexport) {
            if (typeReexport) {
                advance_();  // type
            }
            std::vector<NamedSpec> specs;
            std::vector<NodeIndex> items;
            bool singleLine = true;
            bool hadTypeOnly = false;
            parse_import_export_specifiers_(cjs_ ? &specs : nullptr, &items, &singleLine,
                                            &hadTypeOnly);
            std::string specRaw;
            std::string_view specView;
            bool hasFrom = false;
            if (ident_is_("from")) {
                advance_();
                if (curk_() == Token::StringLiteral) {
                    specRaw = std::string{cur_().raw};
                    specView = cur_().raw;
                    advance_();
                }
                hasFrom = true;
            }
            consume_semicolon_();
            // `export {a}` (no `from`) is a VALUE reference to the local `a`, so
            // it keeps an import alive — verified on the oracle:
            //   import {a} from 'y'; export {a}  =>  import { a } from "y"; …
            // `export {a} FROM 'y'` is exempt: those names are the other module's,
            // not local refs (module_scope.cppm:20-22 draws the same line).
            // Order-independent, hence a post-parse decision.
            if (!hasFrom && !typeReexport) {
                for (const NodeIndex ci : items) {
                    const Node& item = arena_.at(ci);
                    if (item.a != NONE) {
                        trim_.mark_used(arena_.at(item.a).text);
                    }
                }
            }
            if (typeReexport) {
                arena_.truncate_edits(editMark);
                arena_.add_edit(start, prev_end_());
            } else if (cjs_) {
                cjsEsmExport_ = true;
                std::string repl;
                if (hasFrom) {
                    const std::string g{"__mbun_e" + std::to_string(start)};
                    repl = "const " + g + " = require(" + specRaw + ");";
                    for (const NamedSpec& s : specs) {
                        repl += " exports." + s.alias + " = " + g + "." + s.name + ";";
                    }
                } else if (!specs.empty()) {
                    // `export { X as Y }` (no `from`) exports a *live binding* to
                    // the module-scope X; `exports.Y = X` here would snapshot it.
                    // That is wrong twice over: X may still be in its TDZ (the
                    // clause only names the binding, so a `const X` further down is
                    // legal — hono's utils/mime.ts does exactly this), and a later
                    // write to X never reaches exports. __mbun_X answers both with
                    // a getter, the way bun does — see kLiveExportAlias.
                    cjsLiveExport_ = true;
                    for (const NamedSpec& s : specs) {
                        repl += " " + std::string{kLiveExportAlias} + "(" + key_literal_(s.alias) +
                                ", () => " + s.name + ");";
                        cjsLiveNames_.emplace_back(key_literal_(s.alias), s.name);
                    }
                }
                arena_.truncate_edits(editMark);
                arena_.add_edit(start, prev_end_(), repl);
            }
            // A type-only re-export is fully erased, so it needs no hoisting.
            lastExForm_ = typeReexport ? ExForm::None
                          : hasFrom    ? ExForm::ClauseFrom
                                       : ExForm::Clause;
            NodeIndex n = make_export_(start, lastExForm_);

            // `export {type a}` — every specifier inline-`type`, nothing left — is
            // erased WHOLE, not printed as `export {};`. bun spells the rule TWICE,
            // once per branch, and both must be honoured (parse_stmt.rs:1261 for the
            // `from` form, :1322 for the bare one; identical bodies):
            //     if export_clause.clauses.is_empty() && export_clause.had_type_only_exports {
            //         return Ok(p.s(S::TypeScript {}, loc));
            //     }
            // So `hasFrom` is NOT a discriminator here — an earlier draft of this
            // gated on `!hasFrom` and was wrong; :1261 fires before the S::ExportFrom
            // return at :1303. That is a clause that merely FILTERS down to nothing,
            // which still prints `export {};` to mark the file an ES module. All
            // four verified against 1.4.0:
            //     export {type a}          => ""                      (this override)
            //     export {type a} from 'y' => ""                      (this override)
            //     export {a}               => "export {};"            (module_scope filter)
            //     export {}                => "export {};"            (empty in source)
            //     export {} from 'y'       => "export {  } from \"y\";"  (!!! two spaces —
            //                                 S::ExportFrom has no empty-clause arm)
            //
            // ⚠️ Written onto the NODE only, deliberately NOT onto `lastExForm_`.
            // lastExForm_ is the ERASURE path's copy: topStmts_ (:305) carries it
            // into the top-level-await/`using` hoisting, whose switch (:3059) sends
            // ExForm::Clause to `hoist(ts, after)` and ExForm::None to `default:`
            // (i.e. nowhere). Retagging it would leave a `using`-wrapped module's
            // `export { }` INSIDE the try block — illegal, and a default-path output
            // change besides. The printer reads `aux`; the erasure path reads
            // lastExForm_. Splitting them keeps this AST-only, which is what the
            // 0-diff gate requires.
            if (items.empty() && hadTypeOnly) {
                arena_.at(n).aux = static_cast<std::uint32_t>(ExForm::None);
            }
            arena_.at(n).text = specView;
            arena_.at(n).listStart = arena_.commit_list(items);
            arena_.at(n).listCount = static_cast<std::uint32_t>(items.size());
            if (singleLine) {
                arena_.at(n).flags = mflags::IsSingleLine;
            }
            return n;
        }

        // export <declaration>. Type-only declarations erase the `export` keyword
        // too; value declarations keep it (ESM mode) or lower to `exports.x = x`.
        bool typeOnlyDecl = ident_is_("interface") ||
                            (ident_is_("type") && peek_kind_(1) == Token::Identifier) ||
                            ident_is_("declare");
        const std::uint32_t declStart = cur_().start;
        NodeIndex inner = parse_statement_or_async_function_();
        if (!ok_) {
            return NONE;
        }
        if (typeOnlyDecl) {
            arena_.truncate_edits(editMark);
            arena_.add_edit(start, prev_end_());
        } else if (!nsTarget_.empty()) {
            // Inside a TS namespace: `export <decl>` → `<decl>` + `N.x = x` (the
            // namespace object), regardless of module mode.
            arena_.add_edit(start, declStart);  // erase `export `
            std::vector<std::string> names;
            collect_export_names_(inner, names);
            if (!names.empty()) {
                std::string appends{export_append_sep_(inner)};
                for (const std::string& nm : names) {
                    appends += " " + nsTarget_.back() + "." + nm + " = " + nm + ";";
                }
                const std::uint32_t at = prev_end_();
                arena_.add_edit(at, at, appends);
            }
        } else if (cjs_) {
            cjsEsmExport_ = true;
            arena_.add_edit(start, declStart);  // erase `export `
            std::vector<std::string> names;
            collect_export_names_(inner, names);
            if (!names.empty()) {
                // Like the clause form above, an `export var/let/const/function/class`
                // is a *live binding*, not a snapshot. `exports.x = x` here freezes
                // the value the declaration happened to have: `export var NS;`
                // published `undefined` forever, so the TS `namespace` pattern
                // (`(function (N) { … })(NS || (NS = {}))`, which fills NS on the
                // *next* statement) left importers with undefined — typebox's
                // TypeSystemPolicy is exactly this. `export let count = 0` had the
                // same hole for any later mutation.
                //
                // So publish a getter, which is what bun does for every export
                // form (see kLiveExportAlias). Unlike the clause form this cannot
                // hit a TDZ — the binding is declared right here — so the only
                // change in behaviour is that reads now see writes.
                cjsLiveExport_ = true;
                std::string appends{export_append_sep_(inner)};
                for (const std::string& nm : names) {
                    appends += " " + std::string{kLiveExportAlias} + "(" + key_literal_(nm) +
                               ", () => " + nm + ");";
                    cjsLiveNames_.emplace_back(key_literal_(nm), nm);
                }
                const std::uint32_t at = prev_end_();
                arena_.add_edit(at, at, appends);  // zero-width insert after the decl
            }
        }
        // A type-only declaration is fully erased, so it needs no hoisting.
        lastExForm_ = typeOnlyDecl ? ExForm::None : ExForm::Decl;
        lastExPivot_ = declStart;
        // `.a = inner` was the ONE field any import/export node in this arena
        // carried before this slice (the stmt.cppm header called it out by name);
        // make_export_ sets it, and now also the form tag that tells the printer
        // an erased type-only declaration from a real one.
        return make_export_(start, lastExForm_, inner);
    }

    // parse_statement_, but also recognises a top-level `async function …`
    // declaration (which the plain statement dispatcher treats as an expression).
    NodeIndex parse_statement_or_async_function_() {
        if (ident_is_("async") && peek_kind_(1) == Token::Function &&
            !tok_at_(idx_ + 1).newlineBefore) {
            advance_();  // async
            return parse_function_(/*isDecl=*/true, /*isAsync=*/true);
        }
        return parse_statement_();
    }

    // `isAsync` is the caller's, not this function's, to know: every `async`
    // entry point has already consumed the `async` token by the time it gets
    // here (it has to — `async` is not reserved, so telling `async function(){}`
    // from `async(function(){})` is the CALLER's lookahead problem, and all four
    // call sites do it before dispatching). ref g.rs:285 `flags: FunctionSet` —
    // bun carries the same bit on the same struct, read back at lib.rs:5322.
    NodeIndex parse_function_(bool isDecl, bool isAsync = false) {
        std::size_t start = cur_().start;
        advance_();  // function
        bool isGenerator = curk_() == Token::Asterisk;
        if (isGenerator) {
            advance_();  // generator
        }
        // A function introduces its own `yield` context (a plain `function` nested in
        // a generator may use `yield` as an identifier again), so this is a set, not
        // a push. Generator FormalParameters are [+Yield] per spec, hence set before
        // the params rather than only around the body.
        bool savedGenerator = inGenerator_;
        inGenerator_ = isGenerator;
        ScopeGuard restoreGenerator{[&] { inGenerator_ = savedGenerator; }};
        NodeIndex name = NONE;
        if (curk_() == Token::Identifier) {
            name = arena_.make(NodeKind::Identifier, cur_().start, cur_().end);
            arena_.at(name).text = cur_().raw;
            advance_();
        }
        if (curk_() == Token::LessThan) {
            parse_type_params_(/*allowEmpty=*/false);
            if (!ok_) {
                return NONE;
            }
        }
        std::vector<NodeIndex> params;
        bool hasRestArg = false;
        parse_params_(params, nullptr, nullptr, &hasRestArg);
        if (!ok_) {
            return NONE;
        }
        skip_optional_type_annotation_();  // return type
        if (!ok_) {
            return NONE;
        }
        NodeIndex body = NONE;
        if (curk_() == Token::OpenBrace) {
            bool savedBrand = allowPrivateBrand_;
            ++fnDepth_;
            body = parse_block_();
            --fnDepth_;
            allowPrivateBrand_ = savedBrand;
        } else {
            consume_semicolon_();
        }
        NodeIndex n = arena_.make(isDecl ? NodeKind::FunctionDecl : NodeKind::FunctionExpr, start,
                                  cur_().start);
        arena_.at(n).a = name;
        arena_.at(n).b = body;
        // ref g.rs:285 / ast/lib.rs:3292. `IsExport` is NOT set here: bun sets it
        // on the S::Function when the `export` prefix is parsed, and mbun erases
        // ESM syntax rather than modelling it, so no node reaches the printer
        // with it. See fnflags::IsExport in ast.cppm.
        std::uint8_t fnf = 0;
        if (isAsync) {
            fnf |= fnFlags::IsAsync;
        }
        if (isGenerator) {
            fnf |= fnFlags::IsGenerator;
        }
        if (hasRestArg) {
            fnf |= fnFlags::HasRestArg;
        }
        arena_.at(n).flags = fnf;
        arena_.at(n).listStart = arena_.commit_list(params);
        arena_.at(n).listCount = static_cast<std::uint32_t>(params.size());
        return n;
    }

    // Parse and ERASE a single decorator `@expr` / `@expr(...)` / `@expr.member`
    // (a leading-`@` LeftHandSideExpression). The span is deleted from the
    // output; the returned DecSpan covers the decorator *expression* (excluding
    // the '@') so class lowering can re-emit its text with inner TS-erasure
    // edits applied (see emit_decorated_class_). Callers that support stage-3
    // semantics (class statements/expressions and class members) collect the
    // spans; parameter decorators (TS legacy only) simply stay erased.
    DecSpan parse_one_decorator_() {
        std::uint32_t start = cur_().start;  // '@'
        advance_();                          // @
        std::uint32_t exprStart = cur_().start;
        // A decorator expression is restricted (per the decorators grammar): a
        // dotted identifier chain with an optional trailing call, or a fully
        // parenthesized expression. Critically it does NOT include computed
        // `[...]` indexing — otherwise `@dec [key]: T` swallows the class-member
        // key. Parse the restricted form rather than a full LHS expression.
        if (curk_() == Token::OpenParen) {
            advance_();  // (
            parse_expression_(true);
            if (!ok_ || !expect_(Token::CloseParen)) {
                return {};
            }
        } else if (curk_() == Token::Identifier || is_keyword_(curk_()) ||
                   curk_() == Token::EscapedKeyword) {
            // A VALUE reference, but a raw token advance rather than
            // parse_primary_ — so the identifier arm's mark_used never sees it.
            // The head of a `@A.B.C` chain is the reference. Without this,
            // `import {D} from 'y'; @D class C {}` trimmed the import while the
            // lowering still emitted `D` => ReferenceError. Oracle keeps it.
            trim_.mark_used(cur_().raw);
            advance_();  // identifier reference
        } else {
            expected_identifier_();
            return {};
        }
        // `.member` chain (dotted names only — no computed indexing).
        while (ok_ && curk_() == Token::Dot) {
            advance_();
            if (curk_() == Token::Identifier || is_keyword_(curk_()) ||
                curk_() == Token::EscapedKeyword || curk_() == Token::PrivateIdentifier) {
                advance_();
            } else {
                expected_identifier_();
                return {};
            }
        }
        // Optional call `@deco<Type>(...)` (type arguments then arguments).
        if (curk_() == Token::LessThan) {
            parse_type_arguments_();
            if (!ok_) {
                return {};
            }
        }
        if (curk_() == Token::OpenParen) {
            std::vector<NodeIndex> args;
            parse_arguments_(args);
            if (!ok_) {
                return {};
            }
        }
        std::uint32_t exprEnd = prev_end_();
        arena_.add_edit(start, exprEnd);  // erase `@...` (subsumes any inner type edits)
        return DecSpan{exprStart, exprEnd};
    }

    // When `propNames` is non-null (a constructor's parameter list) the bound name
    // of every parameter carrying a TS access modifier is captured so the caller
    // can synthesize `this.<name> = <name>;` in the constructor body.
    // Builds one `Arg` node (ast.cppm, ref g.rs:332) per parameter into `out`.
    //
    // `hasRestArg` is FUNCTION-level, not per-arg, because bun's is: `G::Fn` has
    // `flags::Function::HasRestArg` (ast/lib.rs:3295) and `print_fn_args` applies
    // the `...` to the LAST arg only (`has_rest_arg && i + 1 == args.len()`,
    // lib.rs:2510). Same array-level/property-level asymmetry that bflags
    // documents for BindingArray::HasSpread vs BindingProperty::IsSpread — kept
    // rather than "fixed" so the two models stay 1:1 and neither side needs a
    // translation table. (A `...` in a non-final position is a syntax error, so
    // the two encodings agree on every program that parses.)
    void parse_params_(std::vector<NodeIndex>& out, std::vector<std::string>* propNames = nullptr,
                       std::vector<std::pair<int, std::vector<DecSpan>>>* paramDecs = nullptr,
                       bool* hasRestArg = nullptr) {
        if (!expect_(Token::OpenParen)) {
            return;
        }
        int paramIdx = -1;
        while (ok_ && curk_() != Token::CloseParen && curk_() != Token::EndOfFile) {
            ++paramIdx;
            // Parameter decorators (TS legacy): erased; the spans feed the legacy
            // lowering (emit_legacy_decorated_class_) when a collector is given.
            std::vector<DecSpan> pdecs;
            while (ok_ && curk_() == Token::At) {
                pdecs.push_back(parse_one_decorator_());
            }
            if (!ok_) {
                return;
            }
            if (!pdecs.empty() && paramDecs != nullptr) {
                paramDecs->emplace_back(paramIdx, std::move(pdecs));
            }
            // TS `this` parameter (`function f(this: T, ...)`) is type-only: erase
            // the whole `this: T` and any trailing comma — JS has no `this` param.
            if (curk_() == Token::This) {
                erase_current_token_();  // `this`
                skip_optional_type_annotation_();
                if (!ok_) {
                    return;
                }
                if (curk_() == Token::Comma) {
                    erase_current_token_();
                    continue;
                }
                break;
            }
            // TS parameter-property modifiers are erased. For a constructor, capture
            // the bound name so `constructor(private x)` also synthesizes `this.x = x`.
            bool hadModifier = false;
            while (ident_is_("public") || ident_is_("private") || ident_is_("protected") ||
                   ident_is_("readonly") || ident_is_("override")) {
                hadModifier = true;
                erase_current_token_();
            }
            const std::size_t argStart = cur_().start;
            if (curk_() == Token::DotDotDot) {
                advance_();
                if (hasRestArg != nullptr) {
                    *hasRestArg = true;
                }
            }
            NodeIndex p = parse_binding_name_();
            if (!ok_) {
                return;
            }
            // A TS parameter property (`constructor(private x)`) only assigns
            // `this.x` when the parameter is a plain name — `constructor(private
            // {x})` is a TS error, so a pattern target contributes nothing here.
            if (hadModifier && propNames != nullptr &&
                arena_.at(p).kind == NodeKind::BindingIdentifier &&
                !arena_.at(p).text.empty()) {
                propNames->emplace_back(arena_.at(p).text);
            }
            if (curk_() == Token::Question) {
                erase_current_token_();  // optional-parameter marker
            }
            skip_optional_type_annotation_();
            if (!ok_) {
                return;
            }
            // ref g.rs:335 `pub default: Option<ExprNodeIndex>` — was parsed and
            // discarded; `print_fn_args` (lib.rs:2517) needs it to print `= 1`.
            NodeIndex def = NONE;
            if (curk_() == Token::Equals) {
                advance_();
                def = parse_assign_(true);
                if (!ok_) {
                    return;
                }
            }
            // ref g.rs:332 — the params list holds `G::Arg`, not bare bindings.
            NodeIndex arg = arena_.make(NodeKind::Arg, argStart, prev_end_());
            arena_.at(arg).a = p;
            arena_.at(arg).b = def;
            out.push_back(arg);
            if (curk_() == Token::Comma) {
                advance_();
                continue;
            }
            break;
        }
        expect_(Token::CloseParen);
    }

    NodeIndex parse_class_(bool isDecl) {
        // Set up the decorator-lowering plan for this class (consumes any
        // pending class decorators captured by the caller); the plan is
        // populated by parse_class_member_ via planStack_.back().
        std::size_t planIdx = planStack_.size();
        planStack_.emplace_back();
        {
            ClassPlan& plan = planStack_.back();
            plan.site = ++decSite_;
            plan.classDecs = std::move(pendingClassDecs_);
            pendingClassDecs_.clear();
            plan.decoStart = plan.classDecs.empty() ? static_cast<std::uint32_t>(cur_().start)
                                                    : pendingDecoStart_;
            plan.classStart = cur_().start;
            plan.nameHint = std::move(pendingClassNameHint_);
            pendingClassNameHint_.clear();
        }
        const bool exprForm = !isDecl || pendingDefaultExportClass_;
        pendingDefaultExportClass_ = false;
        planStack_[planIdx].exprForm = exprForm;
        NodeIndex n = parse_class_inner_(isDecl, planIdx);
        if (ok_ && n != NONE) {
            emit_decorated_class_(planStack_[planIdx], /*isDecl=*/!exprForm, prev_end_());
        }
        planStack_.resize(planIdx);
        return n;
    }

    NodeIndex parse_class_inner_(bool isDecl, std::size_t planIdx) {
        std::size_t start = cur_().start;
        advance_();  // class
        NodeIndex name = NONE;
        if (curk_() == Token::Identifier) {
            name = arena_.make(NodeKind::Identifier, cur_().start, cur_().end);
            arena_.at(name).text = cur_().raw;
            planStack_[planIdx].className = cur_().raw;
            advance_();
        }
        if (curk_() == Token::LessThan) {
            parse_type_params_(/*allowEmpty=*/false);
            if (!ok_) {
                return NONE;
            }
        }
        NodeIndex superClass = NONE;
        if (curk_() == Token::Extends) {
            advance_();
            planStack_[planIdx].hasExtends = true;
            planStack_[planIdx].extStart = cur_().start;
            superClass = parse_lhs_expression_();
            if (!ok_) {
                return NONE;
            }
            // Heritage clause type arguments are unambiguous: `extends B<T>`.
            if (curk_() == Token::LessThan) {
                parse_type_arguments_();
                if (!ok_) {
                    return NONE;
                }
            }
            planStack_[planIdx].extEnd = prev_end_();
        }
        if (ident_is_("implements")) {
            std::uint32_t implStart = cur_().start;
            advance_();
            parse_type_();
            while (ok_ && curk_() == Token::Comma) {
                advance_();
                parse_type_();
            }
            if (!ok_) {
                return NONE;
            }
            arena_.add_edit(implStart, prev_end_());  // erase `implements I, J`
        }
        planStack_[planIdx].bodyOpen = cur_().start;  // '{'
        std::vector<NodeIndex> members;
        bool savedHasSuper = classHasSuper_;
        classHasSuper_ = (superClass != NONE);
        parse_class_body_(members);
        classHasSuper_ = savedHasSuper;
        if (!ok_) {
            return NONE;
        }
        NodeIndex n = arena_.make(isDecl ? NodeKind::ClassDecl : NodeKind::ClassExpr, start,
                                  cur_().start);
        arena_.at(n).a = name;
        arena_.at(n).b = superClass;
        arena_.at(n).listStart = arena_.commit_list(members);
        arena_.at(n).listCount = static_cast<std::uint32_t>(members.size());
        return n;
    }

    // ── ES stage-3 decorator lowering emission ──────────────────────────────
    // Lower a decorated class (or a class using `accessor`) to plain JS via
    // source edits, esbuild-style but against the __mbun_dc* runtime helpers
    // installed by the jsc runtime prelude (runtime.cppm). Shape for a class
    // statement `@A class C extends E { @d m() {} @f x = 1 }`:
    //
    //   __mbun_dcF(N,"C",[(A)])._h((E))._m(0,0,"m",[(d)])._m(1,3,"x",[(f)]);
    //   class C extends __mbun_dcH(N) {
    //     static { __mbun_dcA(N,this); }        // apply decorators (spec order)
    //     #__mbun_iiN = __mbun_dcII(N,this);    // instance extra initializers
    //     m() {}
    //     x = __mbun_dcFld(N,1,this,(1));       // field initializer chain
    //   }
    //   C = __mbun_dcFin(N,C);                  // class replacement + extras
    //
    // A class expression is the same chain ending `._c(class … { … })`.
    // Private members are decorated by moving the implementation to a mangled
    // static private method plus a delegating private accessor; `accessor`
    // members lower to a private storage field plus get/set delegators.
    void emit_decorated_class_(ClassPlan& p, bool isDecl, std::uint32_t classEnd) {
        const std::string S = std::to_string(p.site);
        auto text = [&](std::uint32_t a, std::uint32_t b) {
            return arena_.erase_slice(a, b, src_);
        };
        if (legacyDecorators_) {
            // TS legacy mode (tsconfig experimentalDecorators): `accessor`
            // members still lower, then the class runs through the TS-legacy
            // application helper (__mbun_ld) instead of the stage-3 lowering.
            for (DecMember& m : p.members) {
                if (m.isAccessor) {
                    emit_accessor_lowering_(m, S, /*machinery=*/false);
                }
            }
            emit_legacy_decorated_class_(p, isDecl, classEnd);
            return;
        }
        const bool machinery = p.anyDec || !p.classDecs.empty();
        if (!machinery && !p.anyAccessor) {
            return;
        }
        if (!machinery) {
            // `accessor` members only: lower each to storage + plain get/set.
            for (DecMember& m : p.members) {
                if (m.isAccessor) {
                    emit_accessor_lowering_(m, S, /*machinery=*/false);
                }
            }
            return;
        }
        // Assign runtime element/key indices.
        int nextElem = 0;
        int nextKey = 0;
        for (DecMember& m : p.members) {
            if (!m.decs.empty()) {
                m.elem = nextElem++;
            }
            if (m.isComputed) {
                m.keyIdx = nextKey++;
            }
        }
        // Prelude chain: class decorators, heritage, then per-member decorator
        // lists and hoisted computed keys — exactly the spec evaluation order.
        std::string nm = !p.className.empty() ? p.className : p.nameHint;
        std::string pre = "__mbun_dcF(" + S + ",\"" + nm + "\",[";
        for (std::size_t i = 0; i < p.classDecs.size(); ++i) {
            if (i) {
                pre += ",";
            }
            pre += "(" + text(p.classDecs[i].start, p.classDecs[i].end) + ")";
        }
        pre += "])";
        if (p.hasExtends) {
            pre += "._h((" + text(p.extStart, p.extEnd) + "))";
        }
        std::string memberChain;
        for (DecMember& m : p.members) {
            if (m.elem >= 0) {
                int flags = m.kind | (m.isStatic ? 8 : 0) | (m.isPrivate ? 16 : 0) |
                            (m.isComputed ? 32 : 0);
                std::string nameArg = m.isComputed  ? std::to_string(m.keyIdx)
                                      : m.isPrivate ? "\"#" + m.privName + "\""
                                                    : m.nameArg;
                memberChain += "._m(" + std::to_string(m.elem) + "," + std::to_string(flags) +
                               "," + nameArg + ",[";
                for (std::size_t i = 0; i < m.decs.size(); ++i) {
                    if (i) {
                        memberChain += ",";
                    }
                    memberChain += "(" + text(m.decs[i].start, m.decs[i].end) + ")";
                }
                memberChain += "])";
            }
            if (m.keyIdx >= 0) {
                memberChain += "._k(" + std::to_string(m.keyIdx) + ",(" +
                               text(m.keyExprStart, m.keyExprEnd) + "))";
            }
        }
        // Class decorators + heritage evaluate in the OUTER environment (the
        // prelude); member decorator lists and hoisted computed keys evaluate
        // inside the class body via a synthetic first member's computed key
        // (below), which gives them the spec environment: the classEnv name
        // binding (TDZ until initialized) and the class's private names.
        if (isDecl) {
            arena_.add_edit(p.decoStart, p.decoStart, pre + "; ");
        } else {
            // Expression form: the chain replaces the (erased) decorators right
            // before `class`. Inserting at classStart (not decoStart) keeps
            // `export default @dec class {}` valid — the erased span between
            // `default` and `class` may contain non-expression syntax.
            arena_.add_edit(p.classStart, p.classStart, pre + "._c(");
            arena_.add_edit(classEnd, classEnd, ")");
        }
        if (p.hasExtends) {
            arena_.add_edit(p.extStart, p.extEnd, "__mbun_dcH(" + S + ")");
        }
        // Synthetic first member: its computed key evaluates the member
        // decorator lists + hoisted keys at class creation, inside classEnv and
        // the class's private environment (spec evaluation environment). The
        // throwaway symbol-keyed static field is deleted again in __mbun_dcA.
        // Then the injected first static block registers private
        // implementations/access and applies all decorators; plus the
        // instance-extra-initializer field.
        std::string inj;
        if (!memberChain.empty()) {
            inj += " static [(__mbun_dcC(" + S + ")" + memberChain + ",__mbun_dcT(" + S +
                   "))];";
        }
        inj += " static { ";
        for (DecMember& m : p.members) {
            if (m.elem < 0 || !m.isPrivate) {
                continue;
            }
            const std::string K = std::to_string(m.elem);
            const std::string name = "#" + m.privName;
            const std::string stor = "#__mbun_" + std::string{m.kind == 4 ? "a" : "p"} + S + "_" + K;
            std::string impl;
            std::string acc;
            switch (m.kind) {
            case 0:  // method
            case 1:  // getter (implementation moved to a static method)
                impl = "this." + stor;
                acc = "{get:(o)=>o." + name + ",has:(o)=>" + name + " in o}";
                break;
            case 2:  // setter
                impl = "this." + stor;
                acc = "{set:(o,v)=>{o." + name + "=v;},has:(o)=>" + name + " in o}";
                break;
            case 3:  // field
                impl = "void 0";
                acc = "{get:(o)=>o." + name + ",set:(o,v)=>{o." + name + "=v;},has:(o)=>" +
                      name + " in o}";
                break;
            default:  // 4 accessor: synthesized target over the storage field
                impl = "{get(){return this." + stor + ";},set(v){this." + stor + "=v;}}";
                acc = "{get:(o)=>o." + name + ",set:(o,v)=>{o." + name + "=v;},has:(o)=>" +
                      name + " in o}";
                break;
            }
            inj += "__mbun_dcP(" + S + "," + K + "," + impl + "," + acc + "); ";
        }
        inj += "__mbun_dcA(" + S + ",this); } ";
        bool needII = false;
        for (const DecMember& m : p.members) {
            if (m.elem >= 0 && !m.isStatic && m.kind <= 2) {
                needII = true;
            }
        }
        if (needII) {
            inj += "#__mbun_ii" + S + " = __mbun_dcII(" + S + ",this); ";
        }
        arena_.add_edit(p.bodyOpen + 1, p.bodyOpen + 1, inj);
        if (isDecl && !p.className.empty()) {
            arena_.add_edit(classEnd, classEnd,
                            " " + p.className + " = __mbun_dcFin(" + S + "," + p.className + ");");
        }
        // Per-member rewrites.
        for (DecMember& m : p.members) {
            if (m.isAccessor) {
                emit_accessor_lowering_(m, S, /*machinery=*/true);
                continue;
            }
            const std::string K = std::to_string(m.elem);
            if (m.elem >= 0 && m.kind == 3) {
                // Decorated field: run the initializer chain + extra inits.
                if (m.hasInit) {
                    arena_.add_edit(m.initStart, m.initStart,
                                    "__mbun_dcFld(" + S + "," + K + ",this,(");
                    arena_.add_edit(m.initEnd, m.initEnd, "))");
                } else {
                    arena_.add_edit(m.noInitPos, m.noInitPos,
                                    " = __mbun_dcFld(" + S + "," + K + ",this)");
                }
            } else if (m.elem >= 0 && m.kind <= 2 && m.isPrivate) {
                // Decorated private method/getter/setter: move implementation to
                // a mangled *static* private method and delegate through the
                // original name so `this.#m` sees the decorated value.
                const std::string stor = "#__mbun_p" + S + "_" + K;
                if (!m.isStatic) {
                    arena_.add_edit(m.declStart, m.declStart, "static ");
                }
                if (m.hasGetSetKw) {
                    arena_.add_edit(m.gsStart, m.gsEnd, "");
                }
                arena_.add_edit(m.nameStart, m.nameEnd, stor);
                const std::string name = "#" + m.privName;
                const std::string st = m.isStatic ? "static " : "";
                std::string d;
                if (m.kind == 0) {
                    d = st + "get " + name + "() { return __mbun_dcPV(" + S + ",this," + K +
                        "); }";
                } else if (m.kind == 1) {
                    d = st + "get " + name + "() { return __mbun_dcPV(" + S + ",this," + K +
                        ").call(this); }";
                } else {
                    d = st + "set " + name + "(v) { __mbun_dcPV(" + S + ",this," + K +
                        ").call(this,v); }";
                }
                arena_.add_edit(m.memberEnd, m.memberEnd, "; " + d);
            }
            if (m.keyNeedsRewrite) {
                // Echo the class-body string key the same normalized way as its
                // `._m` metadata argument (both must be the surrogate-escape form).
                arena_.add_edit(m.nameStart, m.nameEnd, m.keyLiteral);
            }
            if (m.keyIdx >= 0) {
                // Hoisted computed key: read the evaluated value back.
                arena_.add_edit(m.keyExprStart, m.keyExprEnd,
                                "__mbun_dcK(" + S + "," + std::to_string(m.keyIdx) + ")");
            }
        }
    }

    // TS legacy (experimentalDecorators) lowering: decorators are erased in the
    // class and re-applied AFTER the class evaluates (tsc's __decorate timing)
    // through the __mbun_ld runtime helper:
    //
    //   C = __mbun_ld(C, [[flags,key,[decs],[[idx,[decs]]…]], …],
    //                    [[classDecs],[[idx,[decs]]…]]);
    //
    // Elements in declaration order; per element the helper applies parameter
    // decorators (descending index) then member decorators bottom-up, matching
    // tsc's reversed __decorate array. Class decorators + constructor parameter
    // decorators form the tail (params first, then class decorators bottom-up).
    void emit_legacy_decorated_class_(ClassPlan& p, bool isDecl, std::uint32_t classEnd) {
        auto text = [&](std::uint32_t a, std::uint32_t b) {
            return arena_.erase_slice(a, b, src_);
        };
        // Legacy set-semantics for decorated fields. A property decorator installs
        // a prototype get/set accessor (via __mbun_ld); a native class field uses
        // [[Define]], which SHADOWS that accessor. The field was already REMOVED
        // from the class body at parse time (see parse_class_member_'s legacyDrop);
        // here an initialized one is re-emitted as a [[Set]] assignment: instance →
        // constructor after `super(...)` (synthesizing a constructor if absent);
        // static → after the class, before __mbun_ld applies decorators.
        // ref src/js_parser/p.rs lower_class 6957-7016 (removal + assignment build)
        // and 7026-7053 (insert after super / start of constructor).
        {
            std::vector<std::string> instAssigns;
            std::string staticAssigns;
            for (DecMember& m : p.members) {
                if (!m.legacyDrop || !m.hasInit) {
                    continue;  // uninitialized: dropped with no assignment
                }
                const std::string keyRef =
                    m.isComputed ? "[" + m.dropKeyText + "]" : "[" + m.nameArg + "]";
                if (m.isStatic) {
                    staticAssigns += p.className + keyRef + " = (" + m.dropInitText + "); ";
                } else {
                    instAssigns.push_back("this" + keyRef + " = (" + m.dropInitText + ");");
                }
            }
            if (!instAssigns.empty()) {
                std::string inits;
                for (const std::string& a : instAssigns) {
                    inits += a + " ";
                }
                if (p.hasCtor) {
                    std::uint32_t superEnd = p.hasExtends ? find_super_call_end_(p.ctorBody) : 0;
                    if (superEnd != 0) {
                        arena_.add_edit(superEnd, superEnd, " " + inits);  // after super(...)
                    } else {
                        arena_.add_edit(p.ctorBraceStart + 1, p.ctorBraceStart + 1, " " + inits);
                    }
                } else {
                    const std::string ctor =
                        p.hasExtends ? "constructor(...args) { super(...args); " + inits + "} "
                                     : "constructor() { " + inits + "} ";
                    arena_.add_edit(p.bodyOpen + 1, p.bodyOpen + 1, ctor);
                }
            }
            if (!staticAssigns.empty()) {
                // Recorded before the __mbun_ld edit below (both at classEnd), so
                // the field values are set before the decorators are applied.
                arena_.add_edit(classEnd, classEnd, " " + staticAssigns);
            }
        }
        auto decList = [&](const std::vector<DecSpan>& ds) {
            std::string out = "[";
            for (std::size_t i = 0; i < ds.size(); ++i) {
                if (i) {
                    out += ",";
                }
                out += "(" + text(ds[i].start, ds[i].end) + ")";
            }
            return out + "]";
        };
        auto paramList = [&](const std::vector<std::pair<int, std::vector<DecSpan>>>& ps) {
            std::string out = "[";
            for (std::size_t i = 0; i < ps.size(); ++i) {
                if (i) {
                    out += ",";
                }
                out += "[" + std::to_string(ps[i].first) + "," + decList(ps[i].second) + "]";
            }
            return out + "]";
        };
        bool any = !p.classDecs.empty() || !p.ctorParams.empty();
        std::string els = "[";
        bool first = true;
        for (DecMember& m : p.members) {
            if (m.isPrivate || m.kind > 3 || (m.decs.empty() && m.params.empty())) {
                continue;  // TS legacy cannot decorate private/accessor members
            }
            any = true;
            if (!first) {
                els += ",";
            }
            first = false;
            const int flags = m.kind | (m.isStatic ? 8 : 0);
            const std::string key =
                m.isComputed ? "(" + text(m.keyExprStart, m.keyExprEnd) + ")" : m.nameArg;
            // Thunked so decorator factories evaluate per element, right before
            // that element is decorated (tsc evaluates each __decorate's
            // decorator array separately).
            els += "()=>[" + std::to_string(flags) + "," + key + "," + decList(m.decs) + "," +
                   paramList(m.params) + "]";
        }
        els += "]";
        if (!any) {
            return;
        }
        const std::string tail =
            "()=>[" + decList(p.classDecs) + "," + paramList(p.ctorParams) + "]";
        if (isDecl) {
            if (p.className.empty()) {
                return;  // no binding to re-apply through (anonymous declaration)
            }
            arena_.add_edit(classEnd, classEnd, " " + p.className + " = __mbun_ld(" +
                                                    p.className + "," + els + "," + tail + ");");
        } else {
            arena_.add_edit(p.classStart, p.classStart, "__mbun_ld(");
            arena_.add_edit(classEnd, classEnd, "," + els + "," + tail + ")");
        }
    }

    // Lower one `accessor` member: the member itself becomes the private
    // storage field; a get/set pair over the storage is appended after it.
    void emit_accessor_lowering_(DecMember& m, const std::string& S, bool machinery) {
        auto text = [&](std::uint32_t a, std::uint32_t b) {
            return arena_.erase_slice(a, b, src_);
        };
        const std::string K = std::to_string(m.elem);
        const std::string stor =
            "#__mbun_a" + S + "_" + (m.elem >= 0 ? K : "u" + std::to_string(m.nameStart));
        arena_.add_edit(m.accKwStart, m.accKwEnd, "");  // drop `accessor`
        // Capture the key text for the delegators *before* the storage rename
        // (only used in the no-machinery computed case).
        std::string rawKey;
        if (!m.isPrivate) {
            if (m.isComputed) {
                rawKey = machinery ? "[__mbun_dcK(" + S + "," + std::to_string(m.keyIdx) + ")]"
                                   : "[" + text(m.keyExprStart, m.keyExprEnd) + "]";
            } else {
                rawKey = text(m.nameStart, m.nameEnd);
            }
        }
        arena_.add_edit(m.nameStart, m.nameEnd, stor);  // key → storage field name
        if (m.elem >= 0) {
            if (m.hasInit) {
                arena_.add_edit(m.initStart, m.initStart,
                                "__mbun_dcFld(" + S + "," + K + ",this,(");
                arena_.add_edit(m.initEnd, m.initEnd, "))");
            } else {
                arena_.add_edit(m.noInitPos, m.noInitPos,
                                " = __mbun_dcFld(" + S + "," + K + ",this)");
            }
        }
        const std::string st = m.isStatic ? "static " : "";
        std::string g;
        std::string s;
        if (m.isPrivate) {
            const std::string name = "#" + m.privName;
            if (m.elem >= 0) {
                g = st + "get " + name + "() { return __mbun_dcPV(" + S + ",this," + K +
                    ").get.call(this); }";
                s = st + "set " + name + "(v) { __mbun_dcPV(" + S + ",this," + K +
                    ").set.call(this,v); }";
            } else {
                g = st + "get " + name + "() { return this." + stor + "; }";
                s = st + "set " + name + "(v) { this." + stor + " = v; }";
            }
        } else {
            g = st + "get " + rawKey + "() { return this." + stor + "; }";
            s = st + "set " + rawKey + "(v) { this." + stor + " = v; }";
        }
        arena_.add_edit(m.memberEnd, m.memberEnd, "; " + g + " " + s);
    }

    // Synthesize the `this.<name> = <name>;` assignments for TS constructor
    // parameter properties. In a derived class they must run after `super(...)`
    // (where `this` first becomes valid), so inject just past the top-level super
    // call; otherwise inject at the very top of the constructor body.
    void emit_param_property_inits_(NodeIndex body, std::uint32_t braceStart,
                                    const std::vector<std::string>& names) {
        std::string inits;
        for (const std::string& nm : names) {
            inits += "this." + nm + " = " + nm + "; ";
        }
        std::uint32_t superEnd = classHasSuper_ ? find_super_call_end_(body) : 0;
        if (superEnd != 0) {
            arena_.add_edit(superEnd, superEnd, inits);  // after `super(...);`
        } else {
            arena_.add_edit(braceStart + 1, braceStart + 1, " " + inits);  // after `{`
        }
    }

    // Byte offset just past a top-level `super(...)` call statement in `block`, or
    // 0 when the block has no such statement.
    std::uint32_t find_super_call_end_(NodeIndex block) {
        if (block == NONE) {
            return 0;
        }
        const Node& b = arena_.at(block);
        for (NodeIndex s : arena_.list_of(b)) {
            const Node& st = arena_.at(s);
            if (st.kind == NodeKind::ExpressionStmt && st.a != NONE) {
                const Node& e = arena_.at(st.a);
                if (e.kind == NodeKind::Call && e.a != NONE &&
                    arena_.at(e.a).kind == NodeKind::SuperExpr) {
                    return st.end;
                }
            }
        }
        return 0;
    }

    void parse_class_body_(std::vector<NodeIndex>& out) {
        if (!expect_(Token::OpenBrace)) {
            return;
        }
        bool savedInClass = insideClass_;
        insideClass_ = true;
        while (ok_ && curk_() != Token::CloseBrace && curk_() != Token::EndOfFile) {
            if (curk_() == Token::Semicolon) {
                advance_();
                continue;
            }
            NodeIndex m = parse_class_member_();
            if (!ok_) {
                insideClass_ = savedInClass;
                return;
            }
            if (m != NONE) {
                out.push_back(m);
            }
        }
        insideClass_ = savedInClass;
        expect_(Token::CloseBrace);
    }

    // Whether the token FOLLOWING a contextual modifier keyword looks like a member
    // key — i.e. the keyword is a modifier rather than the key itself. Anything else
    // (`:` `=` `(` `;` `}` `<` `?` `!` `\n`…) means the keyword names the member:
    // `class C { get: string }`, `class C { readonly?: T }`, `class C { get<T>() {} }`.
    // ref: bun src/js_parser/parse/parse_property.rs:350-358 (could_be_modifier_keyword).
    static bool key_follows_(Token k) {
        return k == Token::Identifier || k == Token::EscapedKeyword || is_keyword_(k) ||
               k == Token::OpenBracket || k == Token::NumericLiteral ||
               k == Token::StringLiteral || k == Token::Asterisk ||
               k == Token::PrivateIdentifier;
    }

    NodeIndex parse_class_member_() {
        std::size_t start = cur_().start;
        // Member decorators: parsed + erased; the spans feed the stage-3
        // lowering plan of the enclosing class (emit_decorated_class_).
        DecMember rec;
        while (ok_ && curk_() == Token::At) {
            rec.decs.push_back(parse_one_decorator_());
        }
        if (!ok_) {
            return NONE;
        }
        rec.declStart = cur_().start;
        if (!rec.decs.empty()) {
            // Erasing the decorators can merge this member into the previous
            // field's initializer (ASI hazard: `x = 1` + `[k] = 2` → `1[k] = 2`).
            // A leading `;` (an empty ClassElement) keeps them separate.
            arena_.add_edit(rec.declStart, rec.declStart, ";");
        }
        // Mark: a body-less member is dropped whole (see the `IsForwardDeclaration`
        // branch below), and the edits recorded while parsing it — which start at the
        // very same offset and would therefore win the overlap tie-break in
        // Arena::erase_slice — must be rolled back first, not merely subsumed.
        const std::size_t editMark = arena_.edit_count();
        // Static initializer block: `static { … }` (ES2022). Not a keyed member —
        // keep `static` and parse the body as a statement block so TS inside it is
        // still erased. (Without this, `static` is consumed and `{` fails as a key.)
        if (ident_is_("static") && peek_kind_(1) == Token::OpenBrace) {
            advance_();  // keep `static`
            bool savedBrand = allowPrivateBrand_;
            allowPrivateBrand_ = true;  // `#x in o` is valid inside a static block
            NodeIndex blk = parse_block_();
            allowPrivateBrand_ = savedBrand;
            return blk;
        }
        bool isCtor = false;  // member key is the literal `constructor`
        // ref bun G::Property (g.rs:143). `rec.kind` next to this is a DIFFERENT
        // enum — it is the TC39 decorator runtime's element kind, emitted as
        // `m.kind | (isStatic ? 8 : 0)` (:1929, :2126) — so the two are tracked
        // separately rather than one being cast to the other.
        PropertyKind pkind{PropertyKind::Normal};
        std::uint8_t propflags{0};
        // Modifiers. `static` and `accessor` are real JS and are kept; the TS-only
        // access/mutation modifiers are erased from the source.
        while (ident_is_("static") || ident_is_("public") || ident_is_("private") ||
               ident_is_("protected") || ident_is_("readonly") || ident_is_("abstract") ||
               ident_is_("declare") || ident_is_("accessor") || ident_is_("override")) {
            // Do not consume a modifier that is actually the member name.
            if (!key_follows_(peek_kind_(1))) {
                break;
            }
            bool tsOnly = ident_is_("public") || ident_is_("private") || ident_is_("protected") ||
                          ident_is_("readonly") || ident_is_("abstract") || ident_is_("declare") ||
                          ident_is_("override");
            if (tsOnly) {
                erase_current_token_();
            } else {
                if (ident_is_("static")) {
                    rec.isStatic = true;
                    propflags |= propFlags::IsStatic;  // ref lib.rs:4812
                } else {  // `accessor` — lowered (JSC has no native support)
                    rec.isAccessor = true;
                    rec.kind = 4;
                    pkind = PropertyKind::AutoAccessor;  // ref g.rs:253 / lib.rs:4828
                    rec.accKwStart = cur_().start;
                    rec.accKwEnd = cur_().end;
                }
                advance_();  // keep `static` / `accessor` (accessor edited later)
            }
        }
        // `async` method modifier (only when a member name follows on this line).
        bool isAsync = false;
        if (ident_is_("async") && key_follows_(peek_kind_(1)) && !tok_at_(idx_ + 1).newlineBefore) {
            isAsync = true;
            advance_();
        }
        // `get`/`set` accessor keyword — never after `async` (bun: `!opts.is_async`).
        if (!isAsync && (ident_is_("get") || ident_is_("set")) && key_follows_(peek_kind_(1))) {
            pkind = ident_is_("get") ? PropertyKind::Get : PropertyKind::Set;  // g.rs:247-248
            rec.kind = ident_is_("get") ? 1 : 2;
            rec.hasGetSetKw = true;
            rec.gsStart = cur_().start;
            rec.gsEnd = cur_().end;
            advance_();
        }
        bool isGeneratorMethod = curk_() == Token::Asterisk;
        if (isGeneratorMethod) {
            advance_();  // generator method
        }
        // Member key.
        //
        // ref g.rs:143 `G::Property::key` — every arm below now BUILDS it. It used
        // to build nothing at all: this function parked the key's text in the
        // decorator-lowering record (`rec.nameArg`) and advanced past it, so the
        // Property it returned was an ERASURE MARKER with a == NONE and b == NONE.
        // print_property's `key == NONE` guard then made every class member print
        // as nothing, which is why `class C { m(){} }` printed `class C { ; }`.
        //
        // The key's KIND follows the key TOKEN, exactly as parse_object_property_
        // and parse_binding_key_ already do — see the note there: building an
        // Identifier for every key makes `{"a-b": 1}` claim to be an identifier
        // NAMED `"a-b"`, and a printer that asks `is_identifier` of that
        // (lib.rs:4911) re-quotes an already-quoted key into `'"a-b"'`.
        bool isPrivate = false;
        std::string privateName;
        std::string keyPlain;  // plain identifier key (NamedEvaluation hint)
        NodeIndex key = NONE;
        rec.nameStart = cur_().start;
        if (curk_() == Token::PrivateIdentifier) {
            isPrivate = true;
            privateName = cur_().ident;
            rec.isPrivate = true;
            rec.privName = privateName.starts_with('#') ? privateName.substr(1) : privateName;
            // ref :4899-4904 — bun's private key is an `E::PrivateIdentifier` NODE
            // (resolved through the symbol table at :4904 `print_symbol`), not a
            // flag. mbun's PrivateName node images it and carries the `#name` text;
            // pflags::IsPrivate stays because mbun has no symbol table to resolve
            // through (ast.cppm's note) and print_property needs to know without
            // one.
            key = arena_.make(NodeKind::PrivateName, cur_().start, cur_().end);
            arena_.at(key).text = cur_().raw;
            rec.nameEnd = cur_().end;
            advance_();
        } else if (curk_() == Token::OpenBracket) {
            // Computed key — or, in a class body, a TS INDEX SIGNATURE
            // (`[key: string]: any`), which is not a key at all.
            rec.isComputed = true;
            propflags |= propFlags::IsComputed;  // ref lib.rs:4869
            advance_();
            // ref parse_property.rs:298 — captured BEFORE the expression is
            // parsed, because it asks what the FIRST token of the `[…]` was.
            // `[k]` and `[key: string]` both start with an identifier; only the
            // `:` that follows the expression tells them apart.
            const bool wasIdentifier = curk_() == Token::Identifier;
            rec.keyExprStart = cur_().start;
            NodeIndex keyExpr = parse_assign_(true);
            if (!ok_) {
                return NONE;
            }
            // ─────────────────────────────────────────────────────────────────
            // TS index signature — ref parse_property.rs:300-318 ("Handle index
            // signatures"). All four of bun's conditions, in bun's order:
            //   :303  token == TColon && was_identifier && opts.is_class
            //   :304  expr.data is E::Identifier  (so `[a.b]`/`["x"]` are keys)
            //
            // `opts.is_class` needs no test here: this function IS the class
            // path. The object-literal computed key has its own arm in
            // parse_object_property_ and is deliberately untouched — that is
            // exactly what bun's is_class guard buys, and `({[k]: v})` must keep
            // working.
            //
            // ⚠️ bun gates the whole block on IS_TYPESCRIPT_ENABLED (:301).
            // mbun has no such switch — this parser always erases TS, for .js
            // input too — so an index signature in a .js file is accepted here
            // where real bun rejects it. That stance is the whole parser's and
            // predates this branch; narrowing it is not this change's to make.
            if (curk_() == Token::Colon && wasIdentifier && keyExpr != NONE &&
                arena_.at(keyExpr).kind == NodeKind::Identifier) {
                advance_();     // `:`   — ref :306
                parse_type_();  // the KEY type (`string`/`number`/`symbol`) :307
                if (!ok_) {
                    return NONE;
                }
                if (!expect_(Token::CloseBracket)) {  // ref :308
                    return NONE;
                }
                if (!expect_(Token::Colon)) {  // ref :309
                    return NONE;
                }
                parse_type_();  // the VALUE type — ref :310
                if (!ok_) {
                    return NONE;
                }
                consume_semicolon_();  // ref :311 expect_or_insert_semicolon
                // ref :314 `return Ok(None)` — "Skip this property entirely". An
                // index signature is PURE TYPE: it declares nothing at runtime,
                // so the member is erased whole rather than emitted.
                //
                // The truncate is not optional, and it is the same rollback the
                // body-less-member arm below does for the same reason (see the
                // editMark note at the top of this function): the modifier loop
                // may already have erased a `readonly`/`declare` at an offset
                // INSIDE the span about to be deleted, and two overlapping edits
                // starting at the same offset would fight over Arena::erase_slice's
                // tie-break. Roll those back, then delete the span once.
                arena_.truncate_edits(editMark);
                arena_.add_edit(rec.declStart, prev_end_());
                return NONE;
            }
            rec.keyExprEnd = prev_end_();
            key = keyExpr;  // ref :3924 — a computed key IS the expression
            if (!expect_(Token::CloseBracket)) {
                return NONE;
            }
            rec.nameEnd = prev_end_();  // past ']'
        } else if (curk_() == Token::Identifier || curk_() == Token::StringLiteral ||
                   curk_() == Token::NumericLiteral || is_keyword_(curk_()) ||
                   curk_() == Token::EscapedKeyword) {
            isCtor = curk_() == Token::Identifier &&
                     std::string_view{cur_().ident} == "constructor" && !tok_has_escape_(cur_());
            if (curk_() == Token::StringLiteral) {
                // A supra-BMP key stored as UTF-16 must be re-serialized; its raw
                // `\u{…}` source form would be echoed verbatim otherwise, so it
                // would match neither the literal astral chars nor `\uXXXX`.
                const std::u16string* v = tok_string_value_(cur_());
                if (v && key_has_supra_bmp_(*v)) {
                    rec.keyLiteral = quote_astral_key_(*v);
                    rec.keyNeedsRewrite = true;
                    rec.nameArg = rec.keyLiteral;
                } else {
                    rec.nameArg = std::string{cur_().raw};  // literal text = expression
                }
            } else if (curk_() == Token::NumericLiteral) {
                rec.nameArg = std::string{cur_().raw};  // literal text = expression
            } else {
                rec.nameArg = "\"" +
                              (cur_().ident.empty() ? std::string{cur_().raw} : cur_().ident) +
                              "\"";
            }
            if (curk_() == Token::Identifier) {
                keyPlain = cur_().ident.empty() ? std::string{cur_().raw} : cur_().ident;
            }
            // Same kind-follows-the-token split as parse_object_property_.
            NodeKind keyKind{NodeKind::Identifier};
            if (curk_() == Token::StringLiteral) {
                keyKind = NodeKind::StringLiteral;
            } else if (curk_() == Token::NumericLiteral) {
                keyKind = NodeKind::NumberLiteral;
            }
            key = arena_.make(keyKind, cur_().start, cur_().end);
            // Identifier-like keys carry their name so the tree reads without the
            // source buffer; literal keys keep bun's model, where the key is an
            // expression the printer re-prints from its own node.
            if (keyKind == NodeKind::Identifier) {
                arena_.at(key).text = cur_().raw;
            }
            rec.nameEnd = cur_().end;
            advance_();
        } else {
            expected_identifier_();
            return NONE;
        }
        NodeIndex node = arena_.make(NodeKind::Property, start, cur_().start);
        arena_.at(node).a = key;  // ref g.rs:143 — G::Property::key
        // The bare `isPrivate ? 1 : 0` this replaces was write-only — nothing read
        // a Property's flags — and bit 0 is bun's IsComputed. Now that the bits are
        // bun's (ast.cppm:263), "private" moves to the mbun-only IsPrivate bit; see
        // the note there for why bun needs no such flag.
        if (isPrivate) {
            propflags |= propFlags::IsPrivate;
        }
        arena_.at(node).flags = propflags;
        arena_.at(node).aux = static_cast<std::uint32_t>(pkind);
        if (isPrivate) {
            arena_.at(node).text = privateName;
        }
        // Record the member on the enclosing class's lowering plan when it needs
        // attention (decorated / `accessor` / computed key hoisting).
        auto record = [&]() {
            if (planStack_.empty() || isCtor) {
                return;
            }
            if (rec.decs.empty() && !rec.isAccessor && !rec.isComputed && rec.params.empty()) {
                return;
            }
            ClassPlan& pl = planStack_.back();
            if (!rec.decs.empty()) {
                pl.anyDec = true;
            }
            if (rec.isAccessor) {
                pl.anyAccessor = true;
            }
            pl.members.push_back(rec);
        };
        // The optional `?` / definite-assignment `!` marker trails the KEY, so it
        // precedes the type parameters and the `(` that would make this a method:
        // `foo?: number`, `foo!: number`, and also `foo?(): void` / `foo?<T>(): T`.
        // ref: bun src/js_parser/parse/parse_property.rs:603-627 — consumed before
        // skip_type_script_type_parameters and before the field/method split.
        if (curk_() == Token::Question || curk_() == Token::Exclamation) {
            erase_current_token_();
        }
        if (curk_() == Token::LessThan) {
            parse_type_params_(/*allowEmpty=*/false);
            if (!ok_) {
                return NONE;
            }
        }
        if (curk_() == Token::OpenParen) {
            // Method. ref g.rs:276 — the value is an `E::Function` wrapping a
            // `G::Fn`, and print_property reaches THROUGH it for everything a
            // method needs: `async`/`*` off func.flags (lib.rs:4843-4855), the
            // params + body via print_func (:4880, :5017). Built the same way, and
            // for the same reason, as the object-literal method arm above.
            arena_.at(node).flags |= propFlags::IsMethod;
            std::vector<NodeIndex> params;
            std::vector<std::string> propNames;
            std::vector<std::pair<int, std::vector<DecSpan>>> paramDecs;
            bool hasRestArg = false;
            parse_params_(params, isCtor ? &propNames : nullptr, &paramDecs, &hasRestArg);
            if (!ok_) {
                return NONE;
            }
            if (isCtor && !planStack_.empty()) {
                planStack_.back().ctorParams = std::move(paramDecs);
            } else {
                rec.params = std::move(paramDecs);
            }
            skip_optional_type_annotation_();  // method return type
            if (!ok_) {
                return NONE;
            }
            if (curk_() == Token::OpenBrace) {
                std::uint32_t braceStart = static_cast<std::uint32_t>(cur_().start);
                bool savedBrand = allowPrivateBrand_;
                bool savedGenerator = inGenerator_;
                allowPrivateBrand_ = true;
                inGenerator_ = isGeneratorMethod;  // `*m(){ yield }` / `async *m(){ yield }`
                ++fnDepth_;
                NodeIndex body = parse_block_();
                --fnDepth_;
                inGenerator_ = savedGenerator;
                allowPrivateBrand_ = savedBrand;
                if (isCtor && !planStack_.empty()) {
                    // Record the constructor so the legacy decorator lowering can
                    // relocate decorated-field initializers into it (after super).
                    ClassPlan& pl = planStack_.back();
                    pl.hasCtor = true;
                    pl.ctorBody = body;
                    pl.ctorBraceStart = braceStart;
                }
                if (isCtor && !propNames.empty()) {
                    emit_param_property_inits_(body, braceStart, propNames);
                }
                // ref g.rs:276 — the value function. Its span starts at the member
                // key (a method has no `function` keyword of its own), which is
                // what bun's `E::Function` loc covers too.
                NodeIndex value = arena_.make(NodeKind::FunctionExpr, start, prev_end_());
                arena_.at(value).b = body;
                std::uint8_t fnf = 0;
                if (isAsync) {
                    fnf |= fnFlags::IsAsync;
                }
                if (isGeneratorMethod) {
                    fnf |= fnFlags::IsGenerator;
                }
                if (hasRestArg) {
                    fnf |= fnFlags::HasRestArg;
                }
                arena_.at(value).flags = fnf;
                arena_.at(value).listStart = arena_.commit_list(params);
                arena_.at(value).listCount = static_cast<std::uint32_t>(params.size());
                arena_.at(node).b = value;
            } else {
                // No body: a TS overload signature (`foo(): void;`) or an `abstract`
                // method. Type-only — erase the member so the emitted class body stays
                // valid JS. ref: bun src/js_parser/parse/parse_property.rs:123-127
                // (`IsForwardDeclaration` → "Skip this property entirely", Ok(None)).
                consume_semicolon_();
                arena_.truncate_edits(editMark);
                arena_.add_edit(rec.declStart, prev_end_());
                return NONE;
            }
            rec.memberEnd = prev_end_();
            record();
            return node;
        }
        // Field.
        if (rec.kind != 4) {
            rec.kind = 3;
        }
        skip_optional_type_annotation_();
        if (!ok_) {
            return NONE;
        }
        if (curk_() == Token::Equals) {
            advance_();
            if (!keyPlain.empty()) {  // NamedEvaluation hint for `Foo = @dec class {}`
                nameHint_ = keyPlain;
                nameHintTok_ = idx_;
            }
            rec.hasInit = true;
            rec.initStart = cur_().start;
            bool savedBrand = allowPrivateBrand_;
            allowPrivateBrand_ = true;
            // ref g.rs:143-151 — "It's also used for class fields: class Foo { a = 1 }".
            NodeIndex fieldInit = parse_assign_(true);  // was: parsed and discarded
            allowPrivateBrand_ = savedBrand;
            if (!ok_) {
                return NONE;
            }
            // Re-index rather than reusing a reference: parse_assign_ appends to
            // the node vector and can reallocate it.
            arena_.at(node).c = fieldInit;
            rec.initEnd = prev_end_();
        } else {
            rec.noInitPos = prev_end_();
            // An initializer-less field loses its type annotation to erasure, so the
            // key can end up bare on its line (`get!: H<E, "get">` → `get`). In a
            // class body a bare `get`/`set`/`async`/`static` then absorbs the NEXT
            // field's key as an accessor/modifier: hono hono-base.ts:104-106 stacks
            // `get!` / `put!` / `post!` and dies with "Unexpected identifier 'put'.
            // Expected a parameter list for getter definition". ASI does not save it
            // — `get put` is a valid continuation, so no newline is inserted.
            // bun prints `get;` for exactly this input, so terminate the field the
            // same way whenever the source has no `;` of its own.
            //
            // Not for a decorated or `accessor` field: both lower to an injected
            // ` = __mbun_dcFld(…)` at this very position (see noInitPos above), so a
            // `;` here would emit `x;= …`, and the initializer already makes the key
            // unambiguous.
            if (curk_() != Token::Semicolon && rec.decs.empty() && !rec.isAccessor) {
                const std::uint32_t at = prev_end_();
                arena_.add_edit(at, at, ";");
            }
        }
        consume_semicolon_();
        rec.memberEnd = prev_end_();
        // TS legacy (experimentalDecorators): a decorated field is REMOVED from the
        // class body — a native `[[Define]]` field would shadow the prototype
        // get/set accessor its decorator installs (via __mbun_ld). With an
        // initializer it is relocated as a `[[Set]]` assignment by
        // emit_legacy_decorated_class_. Capture the erasure-applied init/key text
        // now, then roll the member's own edits back (truncate) so the whole-field
        // deletion is not defeated by the leading modifier/type erasures that share
        // its start offset. ref bun src/js_parser/p.rs lower_class 6957-7016.
        if (legacyDecorators_ && !planStack_.empty() && rec.kind == 3 && !rec.isPrivate &&
            !rec.decs.empty()) {
            const ClassPlan& pl = planStack_.back();
            const bool canStatic = !pl.exprForm && !pl.className.empty();
            if (!rec.isStatic || canStatic) {
                if (rec.hasInit) {
                    rec.dropInitText = arena_.erase_slice(rec.initStart, rec.initEnd, src_);
                }
                if (rec.isComputed) {
                    rec.dropKeyText = arena_.erase_slice(rec.keyExprStart, rec.keyExprEnd, src_);
                }
                rec.legacyDrop = true;
                arena_.truncate_edits(editMark);
                arena_.add_edit(rec.declStart, rec.memberEnd);  // drop the whole field
            }
        }
        record();
        return node;
    }

    NodeIndex parse_return_() {
        std::size_t start = cur_().start;
        advance_();  // return
        NodeIndex arg = NONE;
        if (curk_() != Token::Semicolon && curk_() != Token::CloseBrace &&
            curk_() != Token::EndOfFile && !cur_().newlineBefore) {
            arg = parse_expression_(true);
            if (!ok_) {
                return NONE;
            }
        }
        consume_semicolon_();
        NodeIndex n = arena_.make(NodeKind::ReturnStmt, start, cur_().start);
        arena_.at(n).a = arg;
        return n;
    }

    NodeIndex parse_throw_() {
        std::size_t start = cur_().start;
        advance_();  // throw
        NodeIndex arg = parse_expression_(true);
        if (!ok_) {
            return NONE;
        }
        consume_semicolon_();
        NodeIndex n = arena_.make(NodeKind::ThrowStmt, start, cur_().start);
        arena_.at(n).a = arg;
        return n;
    }

    // Lower `using x = e` / `await using x = e` to `const x = __mbun_using(env, e
    // [, true])` and mark the enclosing block so it wraps the body in try/finally.
    NodeIndex parse_using_decl_(bool isAsync, std::uint32_t declStart) {
        if (isAsync) {
            if (fnDepth_ == 0) {
                topLevelAwait_ = true;
            }
            advance_();  // await
        }
        advance_();  // using

        std::uint32_t envId = declStart;
        if (!usingFrames_.empty()) {
            usingFrames_.back().hasUsing = true;
            usingFrames_.back().hasAsync = usingFrames_.back().hasAsync || isAsync;
            envId = usingFrames_.back().envId;
        }
        const std::string id{std::to_string(envId)};

        // Replace the `using ` / `await using ` keyword span with `const ` once;
        // each comma-separated declarator then wraps its own initializer.
        NodeIndex firstNameNode = NONE;
        bool first = true;
        while (ok_) {
            std::string name{cur_().raw};
            std::uint32_t nameStart = cur_().start;
            NodeIndex nameNode = arena_.make(NodeKind::Identifier, cur_().start, cur_().end);
            arena_.at(nameNode).text = cur_().raw;
            advance_();  // binding
            skip_optional_type_annotation_();
            if (!ok_ || !expect_(Token::Equals)) {
                return NONE;
            }
            std::uint32_t initStart = cur_().start;
            parse_assign_(true);
            if (!ok_) {
                return NONE;
            }
            std::uint32_t initEnd = prev_end_();

            if (first) {
                // `using`/`await using` keyword → `const`, except at module scope,
                // where the body moves into a try/catch and the binding still has
                // to be reachable from the trailing `export {}` clause.
                // ref: bun src/js_parser/p.rs:9287-9293.
                arena_.add_edit(declStart, nameStart, at_module_scope_() ? "var " : "const ");
                firstNameNode = nameNode;
                first = false;
            }
            // `<name> [: T] =` → `<name> = __mbun_using(__mbun_envID, `
            arena_.add_edit(nameStart, initStart,
                            name + " = __mbun_using(__mbun_env" + id + ", ");
            // close the __mbun_using call after the initializer
            arena_.add_edit(initEnd, initEnd, isAsync ? ", true)" : ", false)");

            if (curk_() == Token::Comma) {
                advance_();
                continue;
            }
            break;
        }
        consume_semicolon_();

        NodeIndex n = arena_.make(NodeKind::VarDecl, declStart, cur_().start);
        arena_.at(n).a = firstNameNode;
        return n;
    }

    // Wrap a block body in the resource-management try/finally when it declared a
    // `using`. `bodyStart` is the byte just after `{`, `closePos` the `}` offset.
    void emit_using_wrapper_(const UsingFrame& frame, std::uint32_t closePos) {
        if (!frame.hasUsing) {
            return;
        }
        const std::string id{std::to_string(frame.envId)};
        arena_.add_edit(frame.bodyStart, frame.bodyStart,
                        " const __mbun_env" + id +
                            " = { stack: [], error: void 0, hasError: false }; try {");
        arena_.add_edit(closePos, closePos,
                        " } catch (__mbun_e" + id + ") { __mbun_env" + id + ".error = __mbun_e" +
                            id + "; __mbun_env" + id + ".hasError = true; } finally { " +
                            (frame.hasAsync ? "await __mbun_disposeAsync(__mbun_env"
                                            : "__mbun_dispose(__mbun_env") +
                            id + "); } ");
    }

    // The module frame is the bottom of usingFrames_ (pushed by parse_program_);
    // every block/namespace body pushes one on top of it.
    bool at_module_scope_() const { return usingFrames_.size() == 1 && fnDepth_ == 0; }

    // `const`/`let` → `var` for a top-level declaration whose keyword starts at
    // `at`. The module body moves into a try/catch, and a lexical declaration
    // would not survive that move: the trailing `export {}` clause, which stays
    // outside, has to still see the binding.
    // ref: bun src/js_parser/p.rs:6175 select_local_kind.
    void force_var_kind_(std::uint32_t at) {
        const auto kw = [&](std::string_view k) {
            return src_.compare(at, k.size(), k) == 0;
        };
        if (kw("const")) {
            arena_.add_edit(at, at + 5, "var");
        } else if (kw("let")) {
            arena_.add_edit(at, at + 3, "var");
        }
    }

    // Wrap the module body in the resource-management try/finally when a
    // top-level `using` was declared. Statements that are illegal inside a block
    // (import, re-export) are moved back out around the try, and exported
    // bindings are republished through a trailing `export {}` clause.
    // ref: bun src/js_parser/p.rs:9297 LowerUsingDeclarationsContext::finalize.
    void emit_module_using_wrapper_(const UsingFrame& frame) {
        if (!frame.hasUsing) {
            return;
        }
        const std::string id{std::to_string(frame.envId)};
        std::string before;                 // hoisted out, re-emitted ahead of the try
        std::string after;                  // re-emitted behind the try
        std::vector<std::string> exported;  // names for the trailing `export {}`

        // `hoist` renders a statement with the edits recorded inside it already
        // applied, then deletes it in place. erase_slice must run before the
        // deletion is recorded, or it would subsume the statement it renders.
        const auto hoist = [&](const TopStmt& ts, std::string& into) {
            into += arena_.erase_slice(ts.start, ts.end, src_);
            into += "\n";
            arena_.add_edit(ts.start, ts.end);
        };

        // CJS lowering has already rewritten every import/export into plain
        // statements (`const x = require(…)` / `exports.x = …`), so nothing is
        // block-illegal and the body can be wrapped as-is.
        if (!cjs_) {
            for (const TopStmt& ts : topStmts_) {
                if (ts.kind == NodeKind::ImportDecl) {
                    hoist(ts, before);
                    continue;
                }
                if (ts.kind != NodeKind::ExportDecl) {
                    if (ts.kind == NodeKind::VarDecl) {
                        force_var_kind_(ts.start);
                    } else if (ts.kind == NodeKind::FunctionDecl) {
                        // A function declaration is block-scoped in strict mode, so
                        // leaving it in the try would hide it from the rest of the
                        // module. ref: bun visit/mod.rs:1482 (should_hoist_fns is
                        // `parent_is_none`, i.e. true exactly at module scope).
                        hoist(ts, before);
                    }
                    continue;
                }
                switch (ts.form) {
                case ExForm::Star:
                case ExForm::ClauseFrom:
                case ExForm::DefaultDecl:
                    hoist(ts, before);
                    break;
                case ExForm::Clause:
                    hoist(ts, after);
                    break;
                case ExForm::DefaultExpr: {
                    // `export default <expr>` → `var __mbun_defID = <expr>` in the
                    // try + `export { __mbun_defID as default }` after it, so the
                    // initializer still observes the `using` bindings.
                    // ref: bun visit/visit_stmt.rs:511-529.
                    const std::string name{"__mbun_def" + id};
                    arena_.add_edit(ts.start, ts.pivot, "var " + name + " =");
                    after += "export { " + name + " as default };\n";
                    break;
                }
                case ExForm::Decl: {
                    std::vector<std::string> names;
                    collect_export_names_(ts.inner, names);
                    const NodeKind ik = ts.inner == NONE ? NodeKind::Missing
                                                         : arena_.at(ts.inner).kind;
                    if (ik == NodeKind::FunctionDecl || ik == NodeKind::ClassDecl) {
                        // ref: bun p.rs:9317 (exported class) / 9343 (function).
                        hoist(ts, before);
                        break;
                    }
                    // `export const x = …` → `var x = …` in the try, name
                    // republished after it. ref: bun p.rs:9350-9375.
                    arena_.add_edit(ts.start, ts.pivot);  // erase `export `
                    force_var_kind_(ts.pivot);
                    for (const std::string& nm : names) {
                        exported.push_back(nm);
                    }
                    break;
                }
                default:
                    break;
                }
            }
        }

        if (!exported.empty()) {
            std::string clause{"export {"};
            for (std::size_t i = 0; i < exported.size(); ++i) {
                clause += (i ? ", " : " ") + exported[i];
            }
            after += clause + " };\n";
        }

        arena_.add_edit(frame.bodyStart, frame.bodyStart,
                        before + " const __mbun_env" + id +
                            " = { stack: [], error: void 0, hasError: false }; try {");
        const std::uint32_t end = static_cast<std::uint32_t>(src_.size());
        arena_.add_edit(end, end,
                        " } catch (__mbun_e" + id + ") { __mbun_env" + id + ".error = __mbun_e" +
                            id + "; __mbun_env" + id + ".hasError = true; } finally { " +
                            (frame.hasAsync ? "await __mbun_disposeAsync(__mbun_env"
                                            : "__mbun_dispose(__mbun_env") +
                            id + "); }\n" + after);
    }

    // try { … } catch (e[: T]) { … } finally { … }  (bun src/ast/s.rs:173 S::Try,
    // src/ast/nodes.rs:727 Catch / :734 Finally). Bodies recurse for nested erasure.
    // TryStmt borrows the Block children of each clause: bun stores three separate
    // StmtLists, and a flat-arena node owns exactly one list span, so the two inner
    // clauses are their own nodes (as they are in bun) and the try body keeps the
    // node's own list.
    NodeIndex parse_try_() {
        std::size_t start = cur_().start;
        advance_();  // try
        NodeIndex body = parse_block_();
        if (!ok_) {
            return NONE;
        }
        NodeIndex catchClause = NONE;
        NodeIndex finallyClause = NONE;
        if (curk_() == Token::Catch) {
            std::size_t catchStart = cur_().start;
            advance_();  // catch
            NodeIndex binding = NONE;
            if (curk_() == Token::OpenParen) {
                advance_();  // (
                if (curk_() != Token::CloseParen) {
                    binding = parse_binding_name_();
                    skip_optional_type_annotation_();  // catch (e: unknown) → erase type
                }
                if (!expect_(Token::CloseParen)) {
                    return NONE;
                }
            }
            NodeIndex catchBody = parse_block_();
            if (!ok_) {
                return NONE;
            }
            catchClause = arena_.make(NodeKind::CatchClause, catchStart, cur_().start);
            arena_.at(catchClause).a = binding;
            copy_block_list_(catchClause, catchBody);
        }
        if (curk_() == Token::Finally) {
            std::size_t finallyStart = cur_().start;
            advance_();  // finally
            NodeIndex finallyBody = parse_block_();
            if (!ok_) {
                return NONE;
            }
            finallyClause = arena_.make(NodeKind::FinallyClause, finallyStart, cur_().start);
            copy_block_list_(finallyClause, finallyBody);
        }
        NodeIndex n = arena_.make(NodeKind::TryStmt, start, cur_().start);
        copy_block_list_(n, body);
        arena_.at(n).b = catchClause;
        arena_.at(n).c = finallyClause;
        return n;
    }

    // Re-point `dst`'s child list at the statements `block` already committed, so
    // a clause node exposes its body directly instead of through a Block wrapper.
    // The pool span is shared, not copied — nothing mutates it after commit.
    void copy_block_list_(NodeIndex dst, NodeIndex block) {
        if (block == NONE) {
            return;
        }
        arena_.at(dst).listStart = arena_.at(block).listStart;
        arena_.at(dst).listCount = arena_.at(block).listCount;
    }

    // while (test) body  (bun src/ast/s.rs:162 S::While{test_, body})
    NodeIndex parse_while_() {
        std::size_t start = cur_().start;
        advance_();  // while
        if (!expect_(Token::OpenParen)) {
            return NONE;
        }
        NodeIndex test = parse_expression_(true);
        if (!ok_ || !expect_(Token::CloseParen)) {
            return NONE;
        }
        NodeIndex body = parse_statement_();
        if (!ok_) {
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::WhileStmt, start, cur_().start);
        arena_.at(n).a = test;
        arena_.at(n).b = body;
        return n;
    }

    // do body while (test);  (bun src/ast/s.rs:157 S::DoWhile{body, test_} — note
    // the slot order follows bun: a=body, b=test.)
    NodeIndex parse_do_while_() {
        std::size_t start = cur_().start;
        advance_();  // do
        NodeIndex body = parse_statement_();
        if (!ok_ || !expect_(Token::While) || !expect_(Token::OpenParen)) {
            return NONE;
        }
        NodeIndex test = parse_expression_(true);
        if (!ok_ || !expect_(Token::CloseParen)) {
            return NONE;
        }
        consume_semicolon_();
        NodeIndex n = arena_.make(NodeKind::DoWhileStmt, start, cur_().start);
        arena_.at(n).a = body;
        arena_.at(n).b = test;
        return n;
    }

    // switch (test) { case v: … default: … }  (bun src/ast/s.rs:181 S::Switch,
    // src/ast/nodes.rs:739 Case{value, body}). A `default:` clause is a Case with
    // no value, exactly as in bun (`value: Option<ExprNodeIndex>`).
    NodeIndex parse_switch_() {
        std::size_t start = cur_().start;
        advance_();  // switch
        if (!expect_(Token::OpenParen)) {
            return NONE;
        }
        NodeIndex test = parse_expression_(true);
        if (!ok_ || !expect_(Token::CloseParen) || !expect_(Token::OpenBrace)) {
            return NONE;
        }
        std::vector<NodeIndex> cases;
        while (ok_ && curk_() != Token::CloseBrace && curk_() != Token::EndOfFile) {
            std::size_t caseStart = cur_().start;
            NodeIndex value = NONE;
            if (curk_() == Token::Case) {
                advance_();
                value = parse_expression_(true);
                if (!ok_ || !expect_(Token::Colon)) {
                    return NONE;
                }
            } else if (curk_() == Token::Default) {
                advance_();
                if (!expect_(Token::Colon)) {
                    return NONE;
                }
            } else {
                unexpected_();
                return NONE;
            }
            // A case body is a statement *list*, not a block: it runs to the next
            // `case`/`default`/`}` with no scope of its own.
            std::vector<NodeIndex> body;
            while (ok_ && curk_() != Token::Case && curk_() != Token::Default &&
                   curk_() != Token::CloseBrace && curk_() != Token::EndOfFile) {
                NodeIndex s = parse_statement_();
                if (!ok_) {
                    return NONE;
                }
                if (s != NONE) {
                    body.push_back(s);
                }
            }
            NodeIndex c = arena_.make(NodeKind::SwitchCase, caseStart, cur_().start);
            arena_.at(c).a = value;
            arena_.at(c).listStart = arena_.commit_list(body);
            arena_.at(c).listCount = static_cast<std::uint32_t>(body.size());
            cases.push_back(c);
        }
        if (!expect_(Token::CloseBrace)) {
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::SwitchStmt, start, cur_().start);
        arena_.at(n).a = test;
        arena_.at(n).listStart = arena_.commit_list(cases);
        arena_.at(n).listCount = static_cast<std::uint32_t>(cases.size());
        return n;
    }

    // break/continue [label]  (bun src/ast/s.rs:304 S::Break / :309 S::Continue —
    // `label: Option<LocRef>`; an empty `text` here is that `None`.)
    NodeIndex parse_break_continue_() {
        std::size_t start = cur_().start;
        const bool isBreak = curk_() == Token::Break;
        advance_();  // break / continue
        // optional label on the same line (no ASI-inserted semicolon before it)
        std::string_view label;
        if (curk_() == Token::Identifier && !cur_().newlineBefore) {
            label = cur_().raw;
            advance_();
        }
        consume_semicolon_();
        NodeIndex n = arena_.make(isBreak ? NodeKind::BreakStmt : NodeKind::ContinueStmt, start,
                                  cur_().start);
        arena_.at(n).text = label;
        return n;
    }

    // with (value) body  (bun src/ast/s.rs:167 S::With{value, body}) — sloppy mode.
    NodeIndex parse_with_() {
        std::size_t start = cur_().start;
        advance_();  // with
        if (!expect_(Token::OpenParen)) {
            return NONE;
        }
        NodeIndex value = parse_expression_(true);
        if (!ok_ || !expect_(Token::CloseParen)) {
            return NONE;
        }
        NodeIndex body = parse_statement_();
        if (!ok_) {
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::WithStmt, start, cur_().start);
        arena_.at(n).a = value;
        arena_.at(n).b = body;
        return n;
    }

    NodeIndex parse_if_() {
        std::size_t start = cur_().start;
        advance_();  // if
        if (!expect_(Token::OpenParen)) {
            return NONE;
        }
        NodeIndex test = parse_expression_(true);
        if (!ok_) {
            return NONE;
        }
        if (!expect_(Token::CloseParen)) {
            return NONE;
        }
        NodeIndex cons = parse_statement_();
        if (!ok_) {
            return NONE;
        }
        NodeIndex alt = NONE;
        if (curk_() == Token::Else) {
            advance_();
            alt = parse_statement_();
            if (!ok_) {
                return NONE;
            }
        }
        NodeIndex n = arena_.make(NodeKind::IfStmt, start, cur_().start);
        arena_.at(n).a = test;
        arena_.at(n).b = cons;
        arena_.at(n).c = alt;
        return n;
    }

    // `for (using x of y)` / `for (await using x of y)`: rewrite the leading
    // `using` / `await using` to `const` and parse the binding, leaving the cursor
    // at `of`. DEFERRED(for-of using disposal): per-iteration `Symbol.dispose` /
    // `Symbol.asyncDispose` is not lowered here — the binding behaves as a plain
    // `const` (valid JS, no SyntaxError) rather than emitting broken disposal code.
    NodeIndex parse_for_using_head_() {
        std::uint32_t start = cur_().start;
        if (ident_is_("await")) {
            advance_();  // await
        }
        std::uint32_t usingEnd = static_cast<std::uint32_t>(cur_().end);
        advance_();  // using
        arena_.add_edit(start, usingEnd, "const");  // `using`/`await using` → `const`
        NodeIndex nameNode = parse_binding_name_();
        if (!ok_) {
            return NONE;
        }
        skip_optional_type_annotation_();
        if (!ok_) {
            return NONE;
        }
        NodeIndex d = arena_.make(NodeKind::VarDeclarator, start, cur_().start);
        arena_.at(d).a = nameNode;
        NodeIndex n = arena_.make(NodeKind::VarDecl, start, cur_().start);
        arena_.at(n).aux = static_cast<std::uint32_t>(VarKind::Const);
        std::array<NodeIndex, 1> arr{d};
        arena_.at(n).listStart = arena_.commit_list(arr);
        arena_.at(n).listCount = 1;
        return n;
    }

    NodeIndex parse_for_() {
        std::size_t start = cur_().start;
        advance_();  // for
        if (ident_is_("await")) {
            advance_();
        }
        if (!expect_(Token::OpenParen)) {
            return NONE;
        }
        // The for-init is a no-in, no-private-brand context.
        bool savedBrand = allowPrivateBrand_;
        allowPrivateBrand_ = false;
        NodeIndex init = NONE;
        if (curk_() == Token::Semicolon) {
            // no init
        } else if (curk_() == Token::Var || curk_() == Token::Const ||
                   (ident_is_("let") && is_binding_start_(peek_kind_(1)))) {
            VarKind vk = curk_() == Token::Var    ? VarKind::Var
                         : curk_() == Token::Const ? VarKind::Const
                                                   : VarKind::Let;
            init = parse_var_decl_(vk, /*needSemi=*/false, /*allowIn=*/false);
        } else if ((ident_is_("using") && peek_kind_(1) == Token::Identifier &&
                    !tok_at_(idx_ + 1).newlineBefore) ||
                   (ident_is_("await") && tok_at_(idx_ + 1).kind == Token::Identifier &&
                    std::string_view{tok_at_(idx_ + 1).ident} == "using" &&
                    !tok_at_(idx_ + 1).newlineBefore &&
                    tok_at_(idx_ + 2).kind == Token::Identifier)) {
            init = parse_for_using_head_();
        } else {
            init = parse_expression_(/*allowIn=*/false);
        }
        if (!ok_) {
            allowPrivateBrand_ = savedBrand;
            return NONE;
        }
        NodeIndex n;
        if (curk_() == Token::In || ident_is_("of")) {
            bool isOf = ident_is_("of");
            advance_();
            NodeIndex rhs = parse_expression_(true);
            if (!ok_) {
                allowPrivateBrand_ = savedBrand;
                return NONE;
            }
            if (!expect_(Token::CloseParen)) {
                allowPrivateBrand_ = savedBrand;
                return NONE;
            }
            allowPrivateBrand_ = savedBrand;
            NodeIndex body = parse_statement_();
            if (!ok_) {
                return NONE;
            }
            n = arena_.make(NodeKind::ForInStmt, start, cur_().start);
            arena_.at(n).a = init;
            arena_.at(n).b = rhs;
            arena_.at(n).flags = isOf ? 1 : 0;
            std::array<NodeIndex, 1> bodyArr{body};
            arena_.at(n).listStart = arena_.commit_list(bodyArr);
            arena_.at(n).listCount = 1;
            return n;
        }
        if (!expect_(Token::Semicolon)) {
            allowPrivateBrand_ = savedBrand;
            return NONE;
        }
        NodeIndex test = NONE;
        if (curk_() != Token::Semicolon) {
            test = parse_expression_(true);
        }
        if (!ok_ || !expect_(Token::Semicolon)) {
            allowPrivateBrand_ = savedBrand;
            return NONE;
        }
        NodeIndex update = NONE;
        if (curk_() != Token::CloseParen) {
            update = parse_expression_(true);
        }
        if (!ok_ || !expect_(Token::CloseParen)) {
            allowPrivateBrand_ = savedBrand;
            return NONE;
        }
        allowPrivateBrand_ = savedBrand;
        NodeIndex body = parse_statement_();
        if (!ok_) {
            return NONE;
        }
        n = arena_.make(NodeKind::ForStmt, start, cur_().start);
        arena_.at(n).a = init;
        arena_.at(n).b = test;
        arena_.at(n).c = update;
        std::array<NodeIndex, 1> bodyArr{body};
        arena_.at(n).listStart = arena_.commit_list(bodyArr);
        arena_.at(n).listCount = 1;
        return n;
    }

    // ── expressions ──────────────────────────────────────────────────────────
    NodeIndex parse_expression_(bool allowIn) {
        NodeIndex e = parse_assign_(allowIn);
        if (!ok_) {
            return NONE;
        }
        if (curk_() == Token::Comma) {
            std::vector<NodeIndex> parts{e};
            while (ok_ && curk_() == Token::Comma) {
                advance_();
                NodeIndex nxt = parse_assign_(allowIn);
                if (!ok_) {
                    return NONE;
                }
                parts.push_back(nxt);
            }
            NodeIndex n = arena_.make(NodeKind::Sequence, 0, cur_().start);
            arena_.at(n).listStart = arena_.commit_list(parts);
            arena_.at(n).listCount = static_cast<std::uint32_t>(parts.size());
            return n;
        }
        return e;
    }

    // `yield` / `yield *` — only reached inside a generator body, where `yield` is
    // always a keyword. Mechanical port of bun's `parse_yield_expr`
    // (src/js_parser/parse/mod.rs:102-131): a newline may not separate `yield` from
    // `*`; the operand is absent before a closing/separator token, and ASI
    // suppresses it after a newline unless this is a `yield*` delegation. The
    // operand binds at Level::Yield (just under Comma), i.e. an AssignmentExpression
    // — so `yield a, b` is `(yield a), b` and `yield a = 1` is `yield (a = 1)`.
    NodeIndex parse_yield_expr_() {
        std::size_t start = cur_().start;
        advance_();  // yield
        bool isStar = curk_() == Token::Asterisk;
        if (isStar) {
            if (cur_().newlineBefore) {
                fail_("Unexpected newline before \"*\"");
                return NONE;
            }
            advance_();  // *
        }
        NodeIndex value = NONE;
        switch (curk_()) {
        case Token::CloseBrace:
        case Token::CloseParen:
        case Token::CloseBracket:
        case Token::Colon:
        case Token::Comma:
        case Token::Semicolon:
            break;  // `yield` with no operand
        default:
            if (isStar || !cur_().newlineBefore) {
                value = parse_assign_(true);
                if (!ok_) {
                    return NONE;
                }
            }
            break;
        }
        NodeIndex n = arena_.make(NodeKind::Yield, start, cur_().start);
        arena_.at(n).a = value;
        arena_.at(n).aux = isStar ? 1u : 0u;
        return n;
    }

    NodeIndex parse_assign_(bool allowIn) {
        DepthGuard depth{this};  // nested elements/arguments/conditionals recurse here
        if (!depth.ok) {
            return NONE;
        }
        if (inGenerator_ && ident_is_("yield")) {
            return parse_yield_expr_();
        }
        NodeIndex left = parse_conditional_(allowIn);
        if (!ok_) {
            return NONE;
        }
        Token k = curk_();
        if (is_assign_op_(k)) {
            std::size_t start = arena_.at(left).start;
            if (!is_assign_target_(left)) {
                fail_("Invalid assignment target");
                return NONE;
            }
            advance_();
            if (k == Token::Equals && arena_.at(left).kind == NodeKind::Identifier &&
                !arena_.at(left).text.empty()) {  // NamedEvaluation hint
                nameHint_ = arena_.at(left).text;
                nameHintTok_ = idx_;
            }
            NodeIndex right = parse_assign_(allowIn);
            if (!ok_) {
                return NONE;
            }
            NodeIndex n = arena_.make(NodeKind::Assignment, start, cur_().start);
            arena_.at(n).aux = static_cast<std::uint32_t>(k);
            arena_.at(n).a = left;
            arena_.at(n).b = right;
            return n;
        }
        return left;
    }

    static bool is_assign_op_(Token k) {
        switch (k) {
        case Token::Equals:
        case Token::PlusEquals:
        case Token::MinusEquals:
        case Token::AsteriskEquals:
        case Token::SlashEquals:
        case Token::PercentEquals:
        case Token::AsteriskAsteriskEquals:
        case Token::LessThanLessThanEquals:
        case Token::GreaterThanGreaterThanEquals:
        case Token::GreaterThanGreaterThanGreaterThanEquals:
        case Token::AmpersandEquals:
        case Token::BarEquals:
        case Token::CaretEquals:
        case Token::AmpersandAmpersandEquals:
        case Token::BarBarEquals:
        case Token::QuestionQuestionEquals:
            return true;
        default:
            return false;
        }
    }

    // ref bun `is_valid_assignment_target` (p.rs:5643-5654).
    bool is_assign_target_(NodeIndex n) const {
        switch (arena_.at(n).kind) {
        case NodeKind::Identifier:
            return true;
        // ref p.rs:5648-5649 — `EDot(e) => e.optional_chain.is_none()` and the
        // same for `EIndex`. A link anywhere in an optional chain cannot be
        // assigned to, because there is no way to short-circuit a store: both
        // `a?.b = c` and `a?.b.c = d` are SyntaxErrors. Note the test is
        // `is_none()`, i.e. Continuation is rejected too, not just Start — which
        // is also why `(a?.b).c = d` stays LEGAL: the parens end the chain, so
        // the assigned link's state is None. Verified against Bun.Transpiler.
        case NodeKind::Member:
        case NodeKind::PrivateMember:
        case NodeKind::Index:
            return (arena_.at(n).flags & ast::ocflags::IsOptionalAny) == 0;
        case NodeKind::ArrayLiteral:
        case NodeKind::ObjectLiteral:
            return true;
        case NodeKind::Paren:
            return arena_.at(n).a != NONE && is_assign_target_(arena_.at(n).a);
        default:
            return false;
        }
    }

    NodeIndex parse_conditional_(bool allowIn) {
        NodeIndex test = parse_binary_(0, allowIn);
        if (!ok_) {
            return NONE;
        }
        if (curk_() == Token::Question) {
            std::size_t start = arena_.at(test).start;
            advance_();
            NodeIndex cons = parse_assign_(true);
            if (!ok_) {
                return NONE;
            }
            if (!expect_(Token::Colon)) {
                return NONE;
            }
            NodeIndex alt = parse_assign_(allowIn);
            if (!ok_) {
                return NONE;
            }
            NodeIndex n = arena_.make(NodeKind::Conditional, start, cur_().start);
            arena_.at(n).a = test;
            arena_.at(n).b = cons;
            arena_.at(n).c = alt;
            return n;
        }
        return test;
    }

    int binary_lbp_(Token k, bool allowIn) const {
        switch (k) {
        case Token::QuestionQuestion:
            return 3;
        case Token::BarBar:
            return 4;
        case Token::AmpersandAmpersand:
            return 5;
        case Token::Bar:
            return 6;
        case Token::Caret:
            return 7;
        case Token::Ampersand:
            return 8;
        case Token::EqualsEquals:
        case Token::ExclamationEquals:
        case Token::EqualsEqualsEquals:
        case Token::ExclamationEqualsEquals:
            return 9;
        case Token::LessThan:
        case Token::LessThanEquals:
        case Token::GreaterThan:
        case Token::GreaterThanEquals:
        case Token::Instanceof:
            return 10;
        case Token::In:
            return allowIn ? 10 : 0;
        case Token::LessThanLessThan:
        case Token::GreaterThanGreaterThan:
        case Token::GreaterThanGreaterThanGreaterThan:
            return 11;
        case Token::Plus:
        case Token::Minus:
            return 12;
        case Token::Asterisk:
        case Token::Slash:
        case Token::Percent:
            return 13;
        case Token::AsteriskAsterisk:
            return 14;
        default:
            return 0;
        }
    }

    NodeIndex parse_binary_(int minBp, bool allowIn) {
        NodeIndex left = parse_unary_();
        if (!ok_) {
            return NONE;
        }
        while (ok_) {
            // TS `as` / `satisfies` type-assertion suffix (same line only).
            if ((ident_is_("as") || ident_is_("satisfies")) && !cur_().newlineBefore) {
                // Erase from the end of the left operand so the space before `as`
                // is absorbed too (`obj as Foo` → `obj`, not `obj `).
                std::uint32_t asStart = prev_end_();
                advance_();
                if (curk_() == Token::Const) {
                    advance_();
                } else {
                    parse_type_();
                }
                if (!ok_) {
                    return NONE;
                }
                arena_.add_edit(asStart, prev_end_());  // erase ` as T` / ` satisfies T`
                continue;
            }
            Token op = curk_();
            int lbp = binary_lbp_(op, allowIn);
            if (lbp == 0 || lbp <= minBp) {
                break;
            }
            std::size_t start = arena_.at(left).start;
            advance_();
            int rightMin = (op == Token::AsteriskAsterisk) ? lbp - 1 : lbp;
            NodeIndex right = parse_binary_(rightMin, allowIn);
            if (!ok_) {
                return NONE;
            }
            NodeKind kind = (op == Token::AmpersandAmpersand || op == Token::BarBar ||
                             op == Token::QuestionQuestion)
                                ? NodeKind::Logical
                                : NodeKind::Binary;
            NodeIndex n = arena_.make(kind, start, cur_().start);
            arena_.at(n).aux = static_cast<std::uint32_t>(op);
            arena_.at(n).a = left;
            arena_.at(n).b = right;
            left = n;
        }
        return left;
    }

    // Whether `t` can begin a unary expression (used to disambiguate `await` /
    // `yield` as operators from their identifier uses).
    static bool token_starts_expr_(Token t) {
        switch (t) {
        case Token::Identifier:
        case Token::EscapedKeyword:
        case Token::PrivateIdentifier:
        case Token::NumericLiteral:
        case Token::BigIntegerLiteral:
        case Token::StringLiteral:
        case Token::RegExpLiteral:
        case Token::NoSubstitutionTemplateLiteral:
        case Token::TemplateHead:
        case Token::True:
        case Token::False:
        case Token::Null:
        case Token::This:
        case Token::Super:
        case Token::OpenParen:
        case Token::OpenBracket:
        case Token::OpenBrace:
        case Token::New:
        case Token::Import:
        case Token::Function:
        case Token::Class:
        case Token::Typeof:
        case Token::Void:
        case Token::Delete:
        case Token::Exclamation:
        case Token::Tilde:
        case Token::Plus:
        case Token::Minus:
        case Token::PlusPlus:
        case Token::MinusMinus:
            return true;
        default:
            return false;
        }
    }

    NodeIndex parse_unary_() {
        DepthGuard depth{this};  // `- - - …1` / `void void …` chains recurse here
        if (!depth.ok) {
            return NONE;
        }
        // `await <expr>` (contextual keyword): treat as a unary operator when an
        // operand follows, otherwise it is a plain identifier.
        if (ident_is_("await") && token_starts_expr_(peek_kind_(1))) {
            if (fnDepth_ == 0) {
                topLevelAwait_ = true;
            }
            std::size_t start = cur_().start;
            advance_();  // await
            NodeIndex operand = parse_unary_();
            if (!ok_) {
                return NONE;
            }
            NodeIndex n = arena_.make(NodeKind::Unary, start, cur_().start);
            arena_.at(n).a = operand;
            return n;
        }
        Token k = curk_();
        switch (k) {
        case Token::Exclamation:
        case Token::Tilde:
        case Token::Plus:
        case Token::Minus:
        case Token::Typeof:
        case Token::Void:
        case Token::Delete: {
            std::size_t start = cur_().start;
            advance_();
            NodeIndex operand = parse_unary_();
            if (!ok_) {
                return NONE;
            }
            if (k == Token::Delete && arena_.at(operand).kind == NodeKind::PrivateMember) {
                fail_(std::format("Deleting the private name \"{}\" is forbidden",
                                  arena_.at(operand).text));
                return NONE;
            }
            NodeIndex n = arena_.make(NodeKind::Unary, start, cur_().start);
            arena_.at(n).aux = static_cast<std::uint32_t>(k);
            arena_.at(n).a = operand;
            return n;
        }
        case Token::PlusPlus:
        case Token::MinusMinus: {
            std::size_t start = cur_().start;
            advance_();
            NodeIndex operand = parse_unary_();
            if (!ok_) {
                return NONE;
            }
            NodeIndex n = arena_.make(NodeKind::Update, start, cur_().start);
            arena_.at(n).aux = static_cast<std::uint32_t>(k);
            arena_.at(n).a = operand;
            arena_.at(n).flags = 1;  // prefix
            return n;
        }
        default:
            break;
        }
        NodeIndex e = parse_postfix_();
        if (!ok_) {
            return NONE;
        }
        // Postfix ++/-- (same line).
        if ((curk_() == Token::PlusPlus || curk_() == Token::MinusMinus) && !cur_().newlineBefore) {
            Token op = curk_();
            std::size_t start = arena_.at(e).start;
            advance_();
            NodeIndex n = arena_.make(NodeKind::Update, start, cur_().start);
            arena_.at(n).aux = static_cast<std::uint32_t>(op);
            arena_.at(n).a = e;
            return n;
        }
        return e;
    }

    NodeIndex parse_lhs_expression_() {
        // Left-hand-side expression: primary + member/call chain, no binary ops.
        return parse_postfix_();
    }

    NodeIndex parse_postfix_() {
        NodeIndex e = parse_primary_();
        if (!ok_) {
            return NONE;
        }
        // ref parse_suffix.rs:1440 — the running optional-chain state for THIS
        // suffix loop. Zero == bun's `None`; otherwise one of the two `ocflags`
        // bits. It lives here, and not on the node, because it is what carries
        // "we are still inside the chain `?.` opened" from one link to the next.
        //
        // A fresh loop starts at `None`, which is exactly why `(a?.b).c` differs
        // from `a?.b.c`: the parenthesised form is parsed by parse_primary_ ->
        // parse_paren_, a SEPARATE parse_postfix_ frame, so the outer `.c` is a
        // brand-new loop whose state begins at zero. That is the whole mechanism
        // by which parens cut a chain; there is no explicit check for it.
        std::uint8_t optionalChain = 0;
        while (ok_) {
            Token k = curk_();
            // ref parse_suffix.rs:1483-1485 — "Reset the optional chain flag by
            // default. That way we won't accidentally treat `c.d` as
            // OptionalChainContinue in `a?.b + c.d`." Each arm that stays in the
            // chain re-publishes `oldOptionalChain` into `optionalChain`; every
            // arm that does not (and every `break`) therefore ends the chain by
            // simply doing nothing.
            const std::uint8_t oldOptionalChain = optionalChain;
            optionalChain = 0;
            if (k == Token::Exclamation && !cur_().newlineBefore) {
                // TS non-null assertion postfix `x!` — erase the `!`, keep chaining.
                erase_current_token_();
                // ref parse_suffix.rs:486 — `!` builds no node, so without this the
                // chain would die on the token: `a?.b!.c` must stay `a?.b.c`, not
                // decay to `(a?.b).c`. Verified against Bun.Transpiler.
                optionalChain = oldOptionalChain;
            } else if (k == Token::Dot) {
                bool objIsSuper = arena_.at(e).kind == NodeKind::SuperExpr;
                advance_();
                // ref parse_suffix.rs:63-119 (`sfx_t_dot`) — a plain `.` link
                // INHERITS the incoming state (:89/:112) and republishes it (:118).
                e = parse_member_after_dot_(e, objIsSuper, oldOptionalChain);
                if (!ok_) {
                    return NONE;
                }
                optionalChain = oldOptionalChain;
            } else if (k == Token::QuestionDot) {
                bool objIsSuper = arena_.at(e).kind == NodeKind::SuperExpr;
                advance_();
                // ref parse_suffix.rs:122-265 (`sfx_t_question_dot`) — `?.` STARTS a
                // chain (:129) regardless of what came before, so this link is
                // always Start and never Continuation.
                const std::uint8_t optionalStart = ast::ocflags::IsOptionalStart;
                if (curk_() == Token::OpenParen) {
                    e = parse_call_(e, optionalStart);  // ref :166 "a?.()"
                } else if (curk_() == Token::OpenBracket) {
                    e = parse_index_(e, optionalStart);  // ref :140 "a?.[b]"
                } else {
                    e = parse_member_after_dot_(e, objIsSuper, optionalStart);
                }
                if (!ok_) {
                    return NONE;
                }
                // ref parse_suffix.rs:259-262 — "Only continue if we have started."
                optionalChain = ast::ocflags::IsOptionalContinuation;
            } else if (k == Token::OpenParen) {
                // ref parse_suffix.rs:365-368 (`sfx_t_open_paren`) — `a?.b()` keeps
                // the chain: the call itself is Continuation.
                e = parse_call_(e, oldOptionalChain);
                if (!ok_) {
                    return NONE;
                }
                optionalChain = oldOptionalChain;
            } else if (k == Token::OpenBracket) {
                // ref parse_suffix.rs:355-368 (`sfx_t_open_bracket`).
                e = parse_index_(e, oldOptionalChain);
                if (!ok_) {
                    return NONE;
                }
                optionalChain = oldOptionalChain;
            } else if (k == Token::NoSubstitutionTemplateLiteral || k == Token::TemplateHead) {
                // ref parse_suffix.rs:273-279 / :304-310 — a tagged template may not
                // be a link in an optional chain, because there is no syntax that
                // could short-circuit the tag call. Both template arms raise this.
                if (oldOptionalChain != 0) {
                    fail_("Template literals cannot have an optional chain as a tag");
                    return NONE;
                }
                e = parse_tagged_template_(e);
                if (!ok_) {
                    return NONE;
                }
            } else if (k == Token::LessThan) {
                // TypeScript type arguments / instantiation expression.
                std::uint32_t ltStart = cur_().start;
                Save save = save_();
                if (try_skip_type_arguments_()) {
                    Follow f = classify_follow_();
                    if (f == Follow::Call) {
                        arena_.add_edit(ltStart, prev_end_());  // erase `<...>` before call
                        // ref parse_suffix.rs:951-959 (`sfx_t_less_than`) — `a?.b<T>()`
                        // is still one chain; the type arguments are not a link.
                        e = parse_call_(e, oldOptionalChain);
                        if (!ok_) {
                            return NONE;
                        }
                        optionalChain = oldOptionalChain;
                    } else if (f == Follow::Tagged) {
                        arena_.add_edit(ltStart, prev_end_());
                        // Same rule as the bare tagged-template arm above
                        // (parse_suffix.rs:273-279): a chain cannot tag a template.
                        if (oldOptionalChain != 0) {
                            fail_("Template literals cannot have an optional chain as a tag");
                            return NONE;
                        }
                        e = parse_tagged_template_(e);
                        if (!ok_) {
                            return NONE;
                        }
                    } else if (f == Follow::Instantiation) {
                        // Keep `e`; type arguments are erased. Cursor is past ">".
                        arena_.add_edit(ltStart, prev_end_());
                        // No node is built, so — as with `!` above — the chain has
                        // to be carried across by hand or `a?.b<T>.c` would decay.
                        optionalChain = oldOptionalChain;
                        continue;
                    } else {
                        restore_(save);
                        break;
                    }
                } else {
                    restore_(save);
                    break;
                }
            } else {
                break;
            }
        }
        return e;
    }

    // `oc` is an `ast::ocflags` bit (or 0 == bun's `optional_chain: None`), passed
    // down rather than recomputed: only the suffix loop knows whether this link is
    // Start, a Continuation of a chain opened earlier, or outside one entirely.
    NodeIndex parse_member_after_dot_(NodeIndex obj, bool objIsSuper, std::uint8_t oc) {
        Token k = curk_();
        if (k == Token::PrivateIdentifier) {
            if (objIsSuper || !insideClass_) {
                expected_identifier_();
                return NONE;
            }
            NodeIndex n = arena_.make(NodeKind::PrivateMember, arena_.at(obj).start, cur_().end);
            arena_.at(n).a = obj;
            arena_.at(n).text = cur_().ident;
            arena_.at(n).flags = oc;
            advance_();
            return n;
        }
        if (k == Token::Identifier || k == Token::EscapedKeyword || is_keyword_(k) ||
            k == Token::True || k == Token::False || k == Token::Null || k == Token::This) {
            NodeIndex n = arena_.make(NodeKind::Member, arena_.at(obj).start, cur_().end);
            arena_.at(n).a = obj;
            arena_.at(n).text = cur_().raw;
            arena_.at(n).flags = oc;
            advance_();
            return n;
        }
        expected_identifier_();
        return NONE;
    }

    NodeIndex parse_index_(NodeIndex obj, std::uint8_t oc = 0) {
        advance_();  // '['
        NodeIndex idx = parse_expression_(true);
        if (!ok_) {
            return NONE;
        }
        if (!expect_(Token::CloseBracket)) {
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::Index, arena_.at(obj).start, cur_().start);
        arena_.at(n).a = obj;
        arena_.at(n).b = idx;
        arena_.at(n).flags = oc;
        return n;
    }

    NodeIndex parse_call_(NodeIndex callee, std::uint8_t oc = 0) {
        std::vector<NodeIndex> args;
        parse_arguments_(args);
        if (!ok_) {
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::Call, arena_.at(callee).start, cur_().start);
        arena_.at(n).a = callee;
        arena_.at(n).listStart = arena_.commit_list(args);
        arena_.at(n).listCount = static_cast<std::uint32_t>(args.size());
        arena_.at(n).flags = oc;
        return n;
    }

    void parse_arguments_(std::vector<NodeIndex>& out) {
        advance_();  // '('
        while (ok_ && curk_() != Token::CloseParen && curk_() != Token::EndOfFile) {
            // ref :737-753 — the `...` marker becomes an `E::Spread` node wrapping
            // the value, exactly as the array-literal path does (parse_array_
            // literal_'s DotDotDot arm). This arm used to `advance_()` past the
            // dots and drop them, turning `f(...args)` into `f(args)` and
            // `new J(...k)` into `new J(k)` — a silent arity change, and the same
            // bug the array path's comment records ("Dropping it turned `[...a]`
            // into `[a]`"). It was unobservable while the printer raw-echoed the
            // enclosing function bodies.
            if (curk_() == Token::DotDotDot) {
                const std::size_t dotsStart = cur_().start;
                advance_();
                NodeIndex value = parse_assign_(true);
                if (!ok_) {
                    return;
                }
                NodeIndex sp = arena_.make(NodeKind::SpreadElement, dotsStart, cur_().start);
                arena_.at(sp).a = value;
                out.push_back(sp);
                if (curk_() == Token::Comma) {
                    advance_();
                    continue;
                }
                break;
            }
            NodeIndex a = parse_assign_(true);
            if (!ok_) {
                return;
            }
            out.push_back(a);
            if (curk_() == Token::Comma) {
                advance_();
                continue;
            }
            break;
        }
        expect_(Token::CloseParen);
    }

    NodeIndex parse_tagged_template_(NodeIndex tag) {
        std::size_t start = arena_.at(tag).start;
        NodeIndex tmpl = parse_template_literal_();
        if (!ok_) {
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::TaggedTemplate, start, cur_().start);
        arena_.at(n).a = tag;
        arena_.at(n).b = tmpl;
        return n;
    }

    NodeIndex parse_template_literal_() {
        std::size_t start = cur_().start;
        if (curk_() == Token::NoSubstitutionTemplateLiteral) {
            NodeIndex n = arena_.make(NodeKind::TemplateLiteral, start, cur_().end);
            advance_();
            return n;
        }
        // TemplateHead ... (expr TemplateMiddle)* expr TemplateTail
        advance_();  // head
        while (ok_) {
            parse_expression_(true);
            if (!ok_) {
                return NONE;
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
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::TemplateLiteral, start, cur_().start);
        return n;
    }

    // ── JSX lowering (TSX/JSX → React.createElement) ─────────────────────────
    // A JSX element is scanned straight from the raw source bytes (see pre_lex_'s
    // interception): the JS tokenizer cannot lex JSX because text runs and a `</`
    // close tag would be mis-scanned as identifiers / a regexp. pre_lex_ therefore
    // hand-scans the whole element, stores the generated
    //   React.createElement(tag, props, ...children)
    // string on the synthetic token, and here we simply record one replacement
    // edit over the element's span (classic runtime — needs a `React` in scope, no
    // import injected). Correct SUBSET handled: elements, `<>…</>` fragments,
    // self-closing tags, attributes (name="s" / name={expr} / boolean / {...spread}),
    // and children (text with the standard JSX whitespace trim, {expr}, and nested
    // elements). DEFERRED (kept literal / no crash): JSX entity decoding (`&amp;`),
    // and namespaced/hyphenated attribute+tag names are emitted as quoted string
    // keys rather than special-cased.
    NodeIndex parse_jsx_token_() {
        std::uint32_t s = cur_().start;
        std::uint32_t e = cur_().end;
        arena_.add_edit(s, e, cur_().ident);
        advance_();
        // The lowered form is a call expression; a non-assign-target placeholder
        // node is enough for the surrounding postfix/binary machinery.
        return arena_.make(NodeKind::Call, s, e);
    }

    NodeIndex parse_primary_() {
        DepthGuard depth{this};  // `[[[…]]]` / `f(f(f(…)))` bottom out here
        if (!depth.ok) {
            return NONE;
        }
        Token k = curk_();
        const Tok& t = cur_();
        switch (k) {
        case Token::Identifier: {
            // `async` function expression / arrow (async is not a reserved word, so
            // it is only special when immediately followed by function/params/param
            // on the same line; otherwise it is a plain identifier or a call).
            if (ident_is_("async") && !tok_at_(idx_ + 1).newlineBefore) {
                const Token n1 = peek_kind_(1);
                if (n1 == Token::Function) {
                    advance_();  // async
                    return parse_function_(/*isDecl=*/false, /*isAsync=*/true);
                }
                // async x => …
                if (n1 == Token::Identifier && peek_kind_(2) == Token::EqualsGreaterThan) {
                    advance_();  // async
                    return parse_arrow_from_ident_(/*isAsync=*/true);
                }
                // async (params) => …  — speculate: only an arrow if `=>` follows.
                if (n1 == Token::OpenParen) {
                    Save save = save_();
                    advance_();  // async
                    const bool isArrow = try_parse_arrow_paren_();
                    restore_(save);
                    if (isArrow) {
                        advance_();  // async
                        return parse_paren_or_arrow_(/*isAsync=*/true);
                    }
                }
                // async <T>(params) => …  — a *generic* async arrow. The `<` is
                // ambiguous with a comparison (`async < x`), a call with explicit
                // type arguments (`async<T>(x)`), and — in TSX — a JSX element, so
                // speculate exactly as the non-async `<T>(…)` head below does and
                // commit only once a `(`-headed arrow head follows. A JSX-lowered
                // `<` is never type parameters.
                // ref: bun src/js_parser/parse/mod.rs:1639-1665 (parse_async_prefix_expr's
                // TLessThan arm: try_skip_type_script_type_parameters_then_open_paren_
                // with_backtracking, then parse_paren_expr with is_async).
                if (n1 == Token::LessThan && !tok_at_(idx_ + 1).jsx) {
                    Save save = save_();
                    advance_();  // async
                    const bool isArrow = try_skip_type_arguments_() &&
                                         curk_() == Token::OpenParen && try_parse_arrow_paren_();
                    restore_(save);
                    if (isArrow) {
                        const std::size_t start = cur_().start;  // the `async` token
                        advance_();                              // async
                        parse_type_params_(/*allowEmpty=*/false);  // erases `<T>`
                        if (!ok_) {
                            return NONE;
                        }
                        std::vector<NodeIndex> params;
                        bool hasRestArg = false;
                        parse_params_(params, /*propNames=*/nullptr, /*paramDecs=*/nullptr,
                                      &hasRestArg);
                        if (!ok_) {
                            return NONE;
                        }
                        skip_optional_type_annotation_();  // arrow return type
                        if (!ok_) {
                            return NONE;
                        }
                        if (!expect_(Token::EqualsGreaterThan)) {
                            return NONE;
                        }
                        return finish_arrow_(start, params, /*isAsync=*/true, hasRestArg);
                    }
                }
            }
            // Arrow with single identifier param: `x => ...`.
            if (peek_kind_(1) == Token::EqualsGreaterThan) {
                return parse_arrow_from_ident_();
            }
            // ref p.rs:5022 `store_name_in_ref` — the single VALUE-position
            // identifier arm. A name in a type annotation never reaches here
            // (types.cppm builds no nodes), which is exactly why `let x: T`
            // leaves T unused.
            trim_.mark_used(t.raw);
            NodeIndex n = arena_.make(NodeKind::Identifier, t.start, t.end);
            arena_.at(n).text = t.raw;
            advance_();
            return n;
        }
        case Token::NumericLiteral: {
            NodeIndex n = arena_.make(NodeKind::NumberLiteral, t.start, t.end);
            advance_();
            return n;
        }
        case Token::BigIntegerLiteral: {
            NodeIndex n = arena_.make(NodeKind::BigIntLiteral, t.start, t.end);
            advance_();
            return n;
        }
        case Token::StringLiteral: {
            NodeIndex n = arena_.make(NodeKind::StringLiteral, t.start, t.end);
            // Record the DECODED value (ref e.rs — bun's `E::String` carries the
            // decoded string, never the source slice). Without this the node's
            // only string is its raw span, and a consumer that re-quotes it
            // escapes the backslash of `\x41` a second time.
            if (const std::u16string* v = tok_string_value_(t)) {
                arena_.at(n).aux = arena_.add_string(*v);
            }
            advance_();
            return n;
        }
        case Token::RegExpLiteral: {
            NodeIndex n = arena_.make(NodeKind::RegExpLiteral, t.start, t.end);
            advance_();
            return n;
        }
        case Token::NoSubstitutionTemplateLiteral:
        case Token::TemplateHead:
            return parse_template_literal_();
        case Token::True:
        case Token::False: {
            NodeIndex n = arena_.make(NodeKind::BooleanLiteral, t.start, t.end);
            advance_();
            return n;
        }
        case Token::Null: {
            NodeIndex n = arena_.make(NodeKind::NullLiteral, t.start, t.end);
            advance_();
            return n;
        }
        case Token::This: {
            NodeIndex n = arena_.make(NodeKind::ThisExpr, t.start, t.end);
            advance_();
            return n;
        }
        case Token::Super: {
            NodeIndex n = arena_.make(NodeKind::SuperExpr, t.start, t.end);
            advance_();
            return n;
        }
        case Token::OpenParen:
            return parse_paren_or_arrow_();
        case Token::OpenBracket:
            return parse_array_literal_();
        case Token::OpenBrace:
            return parse_object_literal_();
        case Token::Function:
            return parse_function_(/*isDecl=*/false);
        case Token::Class:
            return parse_class_(/*isDecl=*/false);
        case Token::At: {
            // Decorated class expression: `(@dec class { … })`. Capture the
            // decorators and the NamedEvaluation hint (if this expression is
            // directly the RHS of `x = …` / `x: …` / `const x = …`).
            if (nameHintTok_ == idx_) {
                pendingClassNameHint_ = nameHint_;
            }
            std::uint32_t decoStart = cur_().start;
            std::vector<DecSpan> decs;
            while (ok_ && curk_() == Token::At) {
                decs.push_back(parse_one_decorator_());
            }
            if (!ok_) {
                return NONE;
            }
            if (curk_() != Token::Class) {
                fail_("Expected \"class\" after decorators");
                return NONE;
            }
            pendingClassDecs_ = std::move(decs);
            pendingDecoStart_ = decoStart;
            return parse_class_(/*isDecl=*/false);
        }
        case Token::New:
            return parse_new_();
        case Token::Import:
            return parse_import_expr_();
        case Token::PrivateIdentifier:
            return parse_private_primary_();
        case Token::LessThan: {
            // A JSX element in expression position (TSX/JSX input): the pre-lexer
            // already scanned + lowered it onto this synthetic token.
            if (cur_().jsx) {
                return parse_jsx_token_();
            }
            // A `<…>` in expression-start position (plain TS, not JSX) is either a
            // generic arrow's type PARAMETERS (`<T>(x) => x`, `<T extends U>`, `<T,>`)
            // or an old-style cast, whose `<…>` holds an arbitrary TYPE (`<T[]>[]`,
            // `<Foo<T>[]>x`). Neither a type-parameter nor a type reading subsumes the
            // other, so skim the `<…>` and the head that follows it first, then commit.
            // ref: bun src/js_parser/parse/parse_prefix.rs:965-990 — speculate
            // try_skip_type_script_type_parameters_then_open_paren_with_backtracking,
            // else `next()` + skip_type_script_type + expect_greater_than + parse_prefix.
            std::size_t start = cur_().start;
            Save save = save_();
            // Skimmers only (they never call fail_), so `ok_` stays clean either way.
            const bool isArrow = try_skip_type_arguments_() && curk_() == Token::OpenParen &&
                                 try_parse_arrow_paren_();
            restore_(save);
            if (isArrow) {
                parse_type_params_(/*allowEmpty=*/false);  // erases `<T>`
                if (!ok_) {
                    return NONE;
                }
                std::vector<NodeIndex> params;
                bool hasRestArg = false;
                parse_params_(params, /*propNames=*/nullptr, /*paramDecs=*/nullptr, &hasRestArg);
                if (!ok_) {
                    return NONE;
                }
                skip_optional_type_annotation_();  // arrow return type
                if (!ok_) {
                    return NONE;
                }
                if (!expect_(Token::EqualsGreaterThan)) {
                    return NONE;
                }
                // A `<T>(…) => …` head is never async — the async form is handled by
                // the `async <T>(…)` branch above, which passes isAsync=true.
                return finish_arrow_(start, params, /*isAsync=*/false, hasRestArg);
            }
            // Old-style cast `<Type>expr` — erase the `<…>` and return the operand.
            advance_();  // `<`
            parse_type_();
            if (!ok_) {
                return NONE;
            }
            if (!expect_gt_()) {
                return NONE;
            }
            arena_.add_edit(static_cast<std::uint32_t>(start), prev_end_());
            return parse_unary_();
        }
        case Token::EscapedKeyword:
            unexpected_();
            return NONE;
        default:
            unexpected_();
            return NONE;
        }
    }

    NodeIndex parse_private_primary_() {
        // `#foo in obj` is a brand check, valid only inside a class body value
        // context. Anywhere else (top level, for-init) it is unexpected.
        if (!allowPrivateBrand_) {
            unexpected_();  // "Unexpected #foo"
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::PrivateName, cur_().start, cur_().end);
        arena_.at(n).text = cur_().ident;
        advance_();
        if (curk_() != Token::In) {
            expected_("in");
            return NONE;
        }
        // Leave `in` for the binary layer to consume.
        return n;
    }

    // `isAsync` is the caller's, because only the caller can know: `async` is not
    // a reserved word, so the token is only the modifier when the speculation
    // above committed to an arrow head (see the `ident_is_("async")` block).
    NodeIndex parse_arrow_from_ident_(bool isAsync = false) {
        std::size_t start = cur_().start;
        std::vector<NodeIndex> params;
        // ⚠️ An Arrow's param list holds `G::Arg`, NEVER a bare binding — the
        // invariant ast.cppm:82-85 spells out, because print_fn_args
        // (js_printer/func.cppm, ref lib.rs:2515/:2517) reads `arg.a` (binding)
        // and `arg.b` (default) off every element. This path used to push the
        // Identifier node itself, so `arg.a` was NONE and the parameter printed
        // as NOTHING: `x => x` came out as `() => x`, changing arity. It was
        // invisible while Arrow bodies were raw-echoed by the printer; the
        // paren-headed path (parse_params_) always built the Arg correctly, which
        // is why only the bare-identifier head was wrong.
        NodeIndex b = arena_.make(NodeKind::BindingIdentifier, cur_().start, cur_().end);
        arena_.at(b).text = cur_().raw;
        NodeIndex arg = arena_.make(NodeKind::Arg, cur_().start, cur_().end);
        arena_.at(arg).a = b;
        arena_.at(arg).b = NONE;  // a bare-identifier head can have no default
        params.push_back(arg);
        advance_();          // ident
        advance_();          // =>
        // A single-identifier head (`x => …`) cannot carry a rest parameter —
        // `...x => …` is not a valid arrow head, it needs parens.
        return finish_arrow_(start, params, isAsync, /*hasRestArg=*/false);
    }

    // ⚠️ `isAsync` / `hasRestArg` are the ONLY things that record `async x => …`
    // and `(...r) => …` in the AST. Every Arrow funnels through here, so an arrow
    // built without them silently loses the keyword and the spread: the printer
    // reads `fnflags::IsAsync` / `HasRestArg` (js_printer/arrow.cppm, ref
    // lib.rs:3796/:3805) and cannot invent what the parser did not record.
    // Dropping either is a semantic change, not a formatting one — an `async`
    // arrow that loses its keyword makes every `await` in its body a
    // SyntaxError, and `(r) => r` has a different arity from `(...r) => r`.
    NodeIndex finish_arrow_(std::size_t start, std::vector<NodeIndex>& params,
                            bool isAsync = false, bool hasRestArg = false) {
        NodeIndex body;
        ++fnDepth_;
        if (curk_() == Token::OpenBrace) {
            bool savedBrand = allowPrivateBrand_;
            body = parse_block_();
            allowPrivateBrand_ = savedBrand;
        } else {
            body = parse_assign_(true);
        }
        --fnDepth_;
        if (!ok_) {
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::Arrow, start, cur_().start);
        arena_.at(n).b = body;
        arena_.at(n).listStart = arena_.commit_list(params);
        arena_.at(n).listCount = static_cast<std::uint32_t>(params.size());
        // ref bun ast/e.rs `E::Arrow { is_async, has_rest_arg }` — bun keeps these
        // two as struct fields on EArrow; mbun carries them on the shared fnflags
        // byte (ast.cppm:252-254), which is where the printer's Function arms read
        // them from too.
        std::uint8_t f = 0;
        if (isAsync) {
            f |= fnFlags::IsAsync;
        }
        if (hasRestArg) {
            f |= fnFlags::HasRestArg;
        }
        arena_.at(n).flags = f;
        return n;
    }

    // `isAsync`: the caller has already consumed the `async` token and proven an
    // arrow head follows (`async (a) => …`). A parenthesised EXPRESSION can never
    // be async, so the flag only ever reaches the finish_arrow_ paths below.
    NodeIndex parse_paren_or_arrow_(bool isAsync = false) {
        std::size_t start = cur_().start;
        if (nameHintTok_ == idx_) {
            ++nameHintTok_;  // NamedEvaluation pierces parens: `x = (@dec class {})`
        }
        // Empty `()` must be an arrow head.
        if (peek_kind_(1) == Token::CloseParen) {
            advance_();  // (
            advance_();  // )
            // Optional return type annotation `): T =>`.
            skip_optional_type_annotation_();
            if (!ok_) {
                return NONE;
            }
            if (curk_() != Token::EqualsGreaterThan) {
                expected_("=>");
                return NONE;
            }
            advance_();  // =>
            std::vector<NodeIndex> params;
            // `()` — no params at all, so no rest arg.
            return finish_arrow_(start, params, isAsync, /*hasRestArg=*/false);
        }
        // Speculatively try to parse an arrow parameter list.
        Save save = save_();
        if (try_parse_arrow_paren_()) {
            // Confirmed an arrow head. Rewind and re-parse the parameter list
            // properly (the speculation used skip_balanced_, which does NOT erase
            // TS annotations) so `(a: number): R => …` loses its types.
            restore_(save);
            std::vector<NodeIndex> params;
            bool hasRestArg = false;
            parse_params_(params, /*propNames=*/nullptr, /*paramDecs=*/nullptr, &hasRestArg);
            if (!ok_) {
                return NONE;
            }
            skip_optional_type_annotation_();  // arrow return type `): T`
            if (!ok_) {
                return NONE;
            }
            if (!expect_(Token::EqualsGreaterThan)) {
                return NONE;
            }
            return finish_arrow_(start, params, isAsync, hasRestArg);
        }
        restore_(save);
        advance_();  // (
        NodeIndex inner = parse_expression_(true);
        if (!ok_) {
            return NONE;
        }
        if (!expect_(Token::CloseParen)) {
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::Paren, start, cur_().start);
        arena_.at(n).a = inner;
        return n;
    }

    // Speculative: does `( ... )` form an arrow head? Skips balanced parens and
    // checks for a following `=>` (allowing a `: type` return annotation).
    bool try_parse_arrow_paren_() {
        if (curk_() != Token::OpenParen) {
            return false;
        }
        if (!skip_balanced_()) {
            return false;
        }
        if (curk_() == Token::Colon) {
            advance_();
            return try_type_then_arrow_();
        }
        return curk_() == Token::EqualsGreaterThan && advance_true_();
    }

    // Speculative: does a return type followed by `=>` start here? Parses the type
    // for real, then rewinds (edits and error state included).
    //
    // Scanning tokens for the next `=>` instead cannot find the type's END, so it
    // latches onto any later arrow in the file. `x ? (a as T)` + newline + `: y`
    // then reads as an arrow's parameter list plus return type, and the commit to
    // that reading fails on the `as` — which is why this only bit sources that omit
    // semicolons (the scan's only stop token) between the ternary and the next `=>`.
    // ref: bun src/js_parser/parse/parse_prefix.rs:965-990 — the arrow speculation
    // skips the type with the real skip_type_script_type under backtracking.
    bool try_type_then_arrow_() {
        Save save = save_();
        const bool savedOk = ok_;
        std::string savedMsg = errMsg_;
        const std::size_t savedOff = errOff_;
        parse_type_();
        const bool hit = ok_ && curk_() == Token::EqualsGreaterThan;
        restore_(save);
        ok_ = savedOk;
        errMsg_ = std::move(savedMsg);
        errOff_ = savedOff;
        return hit;
    }


    NodeIndex parse_array_literal_() {
        std::size_t start = cur_().start;
        advance_();  // [
        std::vector<NodeIndex> elems;
        // ref parse_prefix.rs:729-774 `pfx_t_open_bracket`. The loop shape is
        // load-bearing and is copied exactly: each arm only PUSHES, and the
        // single shared tail below consumes the separating comma. An arm that
        // `continue`d past that tail would make an elision eat its own comma,
        // which is precisely how holes used to vanish.
        while (ok_ && curk_() != Token::CloseBracket && curk_() != Token::EndOfFile) {
            if (curk_() == Token::Comma) {
                // ref :731-736 — a hole is a real element (`E::Missing`), not a
                // skip: `[, x]` has length 2 and `[x]` has length 1.
                elems.push_back(arena_.make(NodeKind::Missing, cur_().start, cur_().start));
            } else if (curk_() == Token::DotDotDot) {
                // ref :737-753 — the `...` marker becomes an `E::Spread` node
                // wrapping the value. Dropping it turned `[...a]` into `[a]`.
                std::size_t dotsStart = cur_().start;
                advance_();
                NodeIndex value = parse_assign_(true);
                if (!ok_) {
                    return NONE;
                }
                NodeIndex sp = arena_.make(NodeKind::SpreadElement, dotsStart, cur_().start);
                arena_.at(sp).a = value;
                elems.push_back(sp);
            } else {
                // ref :754-758
                NodeIndex el = parse_assign_(true);
                if (!ok_) {
                    return NONE;
                }
                elems.push_back(el);
            }
            // ref :761-769 — the one place a comma is consumed. A trailing comma
            // therefore exits via the loop condition WITHOUT pushing a hole, so
            // `[a,]` stays length 1 while `[a,,]` is length 2.
            if (curk_() != Token::Comma) {
                break;
            }
            advance_();
        }
        if (!expect_(Token::CloseBracket)) {
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::ArrayLiteral, start, cur_().start);
        arena_.at(n).listStart = arena_.commit_list(elems);
        arena_.at(n).listCount = static_cast<std::uint32_t>(elems.size());
        return n;
    }

    NodeIndex parse_object_literal_() {
        std::size_t start = cur_().start;
        advance_();  // {
        // ref e.rs:1233 `E::Object::is_single_line` — read back by the printer at
        // lib.rs:3941-3971 to pick the WHOLE layout (`{ a, b }` vs one property
        // per indented line). bun preserves the author's choice here; it is not
        // cosmetic drift. Recorded exactly the way parse_binding_object_ already
        // records bflags::IsSingleLine (:726 and its loop below) — same rule,
        // same three probe points, so the two sides cannot drift.
        bool isSingleLine{!cur_().newlineBefore};
        std::vector<NodeIndex> props;
        while (ok_ && curk_() != Token::CloseBrace && curk_() != Token::EndOfFile) {
            if (curk_() == Token::DotDotDot) {
                std::size_t sstart = cur_().start;
                advance_();
                NodeIndex sp = parse_assign_(true);
                if (!ok_) {
                    return NONE;
                }
                // ref parse_prefix.rs:820-829 — `{...x}` is a PROPERTY whose kind
                // is Spread and whose value is the expression, NOT a bare
                // expression in the member list:
                //     G::Property{ kind: Spread, value: Some(v), ..Default() }
                // so `key` stays NONE (g.rs:158 "Key is optional for spread") and
                // flags stay EMPTY — print_property switches on `kind` (:4759),
                // never on flags::Property::IsSpread, which is the BINDING side's
                // spelling of rest (parse/mod.rs:1167). Pushing the bare value, as
                // this did before, loses the `...` entirely: a printer walking the
                // list cannot tell `{...a}` from `{a}`.
                NodeIndex p = arena_.make(NodeKind::Property, sstart, prev_end_());
                arena_.at(p).aux = static_cast<std::uint32_t>(PropertyKind::Spread);
                arena_.at(p).b = sp;
                props.push_back(p);
            } else {
                NodeIndex p = parse_object_property_();
                if (!ok_) {
                    return NONE;
                }
                props.push_back(p);
            }
            // The three newline probes are parse_binding_object_'s (:788-801),
            // in its order: before the `,`, after the `,`, and before the `}`.
            // `{a,\n b}` and `{a\n, b}` are both multi-line; only the last probe
            // catches `{a\n}`.
            if (curk_() != Token::Comma) {
                break;
            }
            if (cur_().newlineBefore) {
                isSingleLine = false;
            }
            advance_();  // ,
            if (cur_().newlineBefore) {
                isSingleLine = false;
            }
        }
        if (cur_().newlineBefore) {
            isSingleLine = false;
        }
        if (!expect_(Token::CloseBrace)) {
            return NONE;
        }
        NodeIndex n = arena_.make(NodeKind::ObjectLiteral, start, cur_().start);
        if (isSingleLine) {
            arena_.at(n).flags |= exprFlags::IsSingleLine;
        }
        arena_.at(n).listStart = arena_.commit_list(props);
        arena_.at(n).listCount = static_cast<std::uint32_t>(props.size());
        return n;
    }

    NodeIndex parse_object_property_() {
        std::size_t start = cur_().start;
        // ref bun G::Property (g.rs:143). The accessor/computed/method/shorthand
        // facts below were already being PARSED here and then dropped on the
        // floor, which made `{get a(){}}` and `{a}` build identical nodes; they
        // are now recorded on the node bun keeps them on.
        PropertyKind kind{PropertyKind::Normal};
        std::uint8_t flags{0};
        bool isAsyncMethod = false;
        // Optional get/set/async prefixes (only if a key follows). The consume
        // CONDITION is unchanged — only the recording is new.
        if (ident_is_("get") || ident_is_("set") || ident_is_("async")) {
            Token nk = peek_kind_(1);
            if (nk != Token::Colon && nk != Token::Comma && nk != Token::CloseBrace &&
                nk != Token::OpenParen) {
                // ⚠️ `async` is STILL not a field of bun's G::Property, and there
                // is still no Property-level IsAsync bit — bun puts it on the
                // VALUE function (`G::FnFlags::IsAsync`) and print_property reads
                // it back OFF THE VALUE at lib.rs:4844. What changed is that the
                // method arm below now builds that value function, so the flag
                // finally has the place bun keeps it. Recording it here on the
                // Property would be the model bun does not have.
                if (ident_is_("get")) {
                    kind = PropertyKind::Get;
                } else if (ident_is_("set")) {
                    kind = PropertyKind::Set;
                } else {
                    isAsyncMethod = true;
                }
                advance_();
            }
        }
        bool isGeneratorMethod = curk_() == Token::Asterisk;
        if (isGeneratorMethod) {
            advance_();
        }
        Token k = curk_();
        if (k == Token::PrivateIdentifier) {
            expected_identifier_();
            return NONE;
        }
        NodeIndex key = NONE;
        std::string keyIdent;
        if (k == Token::OpenBracket) {
            flags |= propFlags::IsComputed;  // ref lib.rs:4869 — `{[k]: v}`
            advance_();
            key = parse_assign_(true);
            if (!ok_) {
                return NONE;
            }
            if (!expect_(Token::CloseBracket)) {
                return NONE;
            }
        } else if (k == Token::Identifier || k == Token::StringLiteral ||
                   k == Token::NumericLiteral || is_keyword_(k) || k == Token::EscapedKeyword) {
            if (k == Token::Identifier || is_keyword_(k)) {  // NamedEvaluation hint source
                keyIdent = cur_().ident.empty() ? std::string{cur_().raw} : cur_().ident;
            }
            // The key node's KIND must follow the key token, exactly as the
            // binding side's parse_binding_key_ (:~700) already does. This used to
            // build an Identifier for every key, so `{"a-b": 1}` and `{1: y}` both
            // claimed to be identifiers named `"a-b"` / `1` — and a printer that
            // asks `is_identifier(name)` of that (lib.rs:4911) answers "no" and
            // quotes an ALREADY-quoted key into `'"a-b"'`. bun's key is an
            // expression: E::String for a string key, E::Number for a numeric one
            // (parse_property.rs), which is what these kinds image.
            NodeKind keyKind{NodeKind::Identifier};
            if (k == Token::StringLiteral) {
                keyKind = NodeKind::StringLiteral;
            } else if (k == Token::NumericLiteral) {
                keyKind = NodeKind::NumberLiteral;
            }
            key = arena_.make(keyKind, cur_().start, cur_().end);
            // Identifier-like keys carry their name so the tree can be read
            // without the source buffer; literal keys keep bun's model, where the
            // key is an expression the printer re-prints from its own node. Same
            // split, same words, as parse_binding_key_.
            if (keyKind == NodeKind::Identifier) {
                arena_.at(key).text = cur_().raw;
            }
            advance_();
        } else {
            expected_identifier_();
            return NONE;
        }
        NodeIndex value = NONE;
        NodeIndex init = NONE;  // `= <expr>` — bun's G::Property::initializer
        if (curk_() == Token::Colon) {
            advance_();
            if (!keyIdent.empty()) {  // NamedEvaluation hint for `{ Foo: @dec class {} }`
                nameHint_ = keyIdent;
                nameHintTok_ = idx_;
            }
            value = parse_assign_(true);
            if (!ok_) {
                return NONE;
            }
        } else if (curk_() == Token::OpenParen || curk_() == Token::LessThan) {
            // Method shorthand — `{m(){}}`, `{async m(){}}`, `{*g(){}}`,
            // `{get a(){}}`.
            //
            // ref parse_property.rs / g.rs:276 — bun's `{m(){}}` is
            // `Property{flags: IsMethod, value: Some(E::Function{func})}`, and
            // print_property reaches THROUGH that value for everything a method
            // needs: `async` / `*` come off func.flags (lib.rs:4843-4855) and the
            // parameter list + body are printed by `print_func` (:4880, :5017).
            // The value is now built, so those arms can fire.
            //
            // The node is a FunctionExpr rather than a new kind because that IS
            // bun's model: `E::Function` wraps a `G::Fn` (e.rs), the same `G::Fn`
            // an `S::Function` declaration wraps — one function type, two
            // wrappers. print_property's guard is literally
            // `if let ExprData::EFunction(func)` (:4843), which is why
            // property.cppm's try_print_method_ checks for this kind.
            //
            // ⚠️ The value's span starts at the KEY, not at the `(`: a method
            // shorthand has no `function` keyword of its own, so there is no
            // narrower honest span, and print_func never prints from it anyway
            // (it prints args + block from the child nodes). It matters only to
            // expr.cppm's deferred-echo fallback, which a method value never
            // reaches — print_property calls print_func directly.
            flags |= propFlags::IsMethod;
            if (curk_() == Token::LessThan) {
                parse_type_params_(false);
                if (!ok_) {
                    return NONE;
                }
            }
            std::vector<NodeIndex> params;
            bool hasRestArg = false;
            parse_params_(params, nullptr, nullptr, &hasRestArg);
            if (!ok_) {
                return NONE;
            }
            skip_optional_type_annotation_();  // object method return type
            if (!ok_) {
                return NONE;
            }
            NodeIndex body = NONE;
            if (curk_() == Token::OpenBrace) {
                bool savedGenerator = inGenerator_;
                inGenerator_ = isGeneratorMethod;  // `{ *g(){ yield 1 } }`
                ++fnDepth_;
                body = parse_block_();
                --fnDepth_;
                inGenerator_ = savedGenerator;
                if (!ok_) {
                    return NONE;
                }
            }
            value = arena_.make(NodeKind::FunctionExpr, start, prev_end_());
            arena_.at(value).b = body;
            // ref g.rs:285 — async/generator live HERE, on the function, which is
            // where lib.rs:4844/:4852 read them back from.
            std::uint8_t fnf = 0;
            if (isAsyncMethod) {
                fnf |= fnFlags::IsAsync;
            }
            if (isGeneratorMethod) {
                fnf |= fnFlags::IsGenerator;
            }
            if (hasRestArg) {
                fnf |= fnFlags::HasRestArg;
            }
            arena_.at(value).flags = fnf;
            arena_.at(value).listStart = arena_.commit_list(params);
            arena_.at(value).listCount = static_cast<std::uint32_t>(params.size());
        } else if (curk_() == Token::Equals) {
            // `{a = 1}` — only legal as a destructuring-assignment target
            // (`({a = 1} = o)`). `{a: b = 1}` is NOT this arm: its `= 1` is parsed
            // by parse_assign_ above as part of the value expression. So this arm
            // is always shorthand-with-default, which is why it sets WasShorthand.
            // ref g.rs:143-151, whose doc comment names this exact syntax as what
            // `initializer` is for.
            flags |= propFlags::WasShorthand;
            // Shorthand-with-default `{a = 1}` reads `a` too.
            trim_.mark_used(keyIdent);
            advance_();
            if (!keyIdent.empty()) {  // NamedEvaluation hint for `{ Foo = @dec class {} } = …`
                nameHint_ = keyIdent;
                nameHintTok_ = idx_;
            }
            init = parse_assign_(true);  // was: parsed and discarded
            if (!ok_) {
                return NONE;
            }
        } else {
            // ref lib.rs:5175 — shorthand `{a}` == `{a: a}`. bun's printer folds it
            // back by comparing the key text against the value's RESOLVED symbol
            // name rather than trusting this flag (a renamer must be able to
            // unfold `{a}` into `{a: a$1}`), so the flag records the SOURCE form.
            //
            // DEFERRED: bun's shorthand also carries `value: Some(E::Identifier)`
            // (the read of `a`); mbun leaves value NONE here, so the fold at :4921
            // has nothing to compare and print_property falls through to printing
            // the bare key — which happens to be the right bytes for `{a}`.
            //
            // That missing value node is a real VALUE READ, and the trimmer is the
            // first thing to need it: with no node, nothing reaches the identifier
            // arm, so `import {a} from 'y'; const o = {a}` trimmed the import and
            // left `{a}` behind — a ReferenceError, and the shape bun's own
            // fetch-h3.ts (`serve: { tls, http3: true }`) hits. Marking the key
            // here is the read, recorded where the AST does not yet model it.
            trim_.mark_used(keyIdent);
            flags |= propFlags::WasShorthand;
        }
        NodeIndex n = arena_.make(NodeKind::Property, start, cur_().start);
        arena_.at(n).a = key;
        arena_.at(n).b = value;
        arena_.at(n).c = init;
        arena_.at(n).aux = static_cast<std::uint32_t>(kind);
        arena_.at(n).flags = flags;
        return n;
    }

    NodeIndex parse_new_() {
        std::size_t start = cur_().start;
        advance_();  // new
        if (curk_() == Token::Dot) {
            advance_();  // .
            if (curk_() == Token::Identifier) {
                advance_();  // target
            }
            NodeIndex n = arena_.make(NodeKind::New, start, cur_().start);
            return n;
        }
        NodeIndex callee = parse_primary_();
        if (!ok_) {
            return NONE;
        }
        // member chain (no call).
        while (ok_) {
            if (curk_() == Token::Dot) {
                bool objIsSuper = arena_.at(callee).kind == NodeKind::SuperExpr;
                advance_();
                // `new` parses its callee with a member chain that cannot contain
                // `?.` (the loop below never accepts QuestionDot), so every link
                // here is unconditionally outside an optional chain.
                callee = parse_member_after_dot_(callee, objIsSuper, /*oc=*/0);
                if (!ok_) {
                    return NONE;
                }
            } else if (curk_() == Token::OpenBracket) {
                callee = parse_index_(callee);
                if (!ok_) {
                    return NONE;
                }
            } else if (curk_() == Token::LessThan) {
                std::uint32_t ltStart = cur_().start;
                Save save = save_();
                if (try_skip_type_arguments_()) {
                    Follow f = classify_follow_();
                    if (f == Follow::Call || f == Follow::Tagged ||
                        f == Follow::Instantiation) {
                        // keep going; type args erased for new
                        arena_.add_edit(ltStart, prev_end_());
                        break;
                    }
                    restore_(save);
                    break;
                }
                restore_(save);
                break;
            } else {
                break;
            }
        }
        std::vector<NodeIndex> args;
        if (curk_() == Token::OpenParen) {
            parse_arguments_(args);
            if (!ok_) {
                return NONE;
            }
        }
        NodeIndex n = arena_.make(NodeKind::New, start, cur_().start);
        arena_.at(n).a = callee;
        arena_.at(n).listStart = arena_.commit_list(args);
        arena_.at(n).listCount = static_cast<std::uint32_t>(args.size());
        // A `new X\`tmpl\`` tag: handled by the postfix loop of the caller.
        return n;
    }

    NodeIndex parse_import_expr_() {
        std::size_t start = cur_().start;
        advance_();  // import
        if (curk_() == Token::Dot) {
            advance_();
            if (curk_() == Token::Identifier) {
                advance_();  // meta
            }
            // In CJS mode `import.meta` is a syntax error under script-mode eval, so
            // lower it to `__mbunImportMeta`: a module-local the CJS wrapper binds to
            // this module's file/dir, falling back to the global at the entry scope.
            if (cjs_) {
                arena_.add_edit(static_cast<std::uint32_t>(start), prev_end_(),
                                "__mbunImportMeta");
            }
            return arena_.make(NodeKind::ImportMeta, start, cur_().start);
        }
        if (curk_() == Token::OpenParen) {
            // `import(` in cjs mode: script-mode JSC's default loader can't fetch or
            // transpile TS, so lower to the module-scoped require via a Promise
            // helper. Rewrite `import(` → `globalThis.__mbun_dyn_import(require,`.
            std::uint32_t parenEnd{cur_().start + 1};
            std::vector<NodeIndex> args;
            parse_arguments_(args);
            if (!ok_) {
                return NONE;
            }
            if (cjs_) {
                arena_.add_edit(static_cast<std::uint32_t>(start), parenEnd,
                                "globalThis.__mbun_dyn_import(require,");
            }
            NodeIndex n = arena_.make(NodeKind::ImportCall, start, cur_().start);
            arena_.at(n).listStart = arena_.commit_list(args);
            arena_.at(n).listCount = static_cast<std::uint32_t>(args.size());
            return n;
        }
        unexpected_();
        return NONE;
    }

};

}  // namespace detail

ParseResult parse(std::string_view src) {
    detail::Parser p{src};
    return p.run();
}

// Result of transpiling TypeScript → runnable JavaScript by erasure.
struct TranspileResult {
    bool ok{true};
    std::string error;
    std::size_t error_offset{0};
    std::string code;  // erased/lowered JS (equals `src` when input is plain JS)
    bool cjs_esm_module{false};  // CJS output originated from an ESM module and must run strict
    bool top_level_await{false};  // module awaits at top level (script-mode JSC
                                  // needs the async-IIFE wrapper + event-loop pump)
};

// Transpile options. `cjs` lowers ESM import/export to CommonJS require/exports
// (for the script-mode JSC runtime, which cannot evaluate ES modules); when off,
// import/export are kept verbatim (type-only forms still erased).
struct TranspileOptions {
    bool cjs{false};
    bool jsx{false};  // TSX/JSX input — lower JSX elements (see jsx_options)
    bool legacy_decorators{false};  // tsconfig experimentalDecorators: keep the TS
                                    // legacy behavior (decorators erased, no stage-3
                                    // lowering; `accessor` members still lower)

    // The JSX runtime/factory/import-source configuration, before this file's own
    // `@jsx*` comment pragmas are applied on top (transpile_ does that).
    // Defaults to bun's default Pragma: AUTOMATIC runtime, development, "react".
    // ref options_types/jsx.rs:186-197.
    // Its `inject_import` is the one field a caller MUST think about: the module
    // loader sets it, Bun.Transpiler must not — see the note on JsxOptions.
    detail::JsxOptions jsx_options{};
    // MEASUREMENT-ONLY. Emit via the AST-rebuild printer (mbun.js_printer)
    // instead of erasure. DEFAULT OFF and must stay off: the printer cannot yet
    // round-trip a program (ImportDecl/ExportDecl/TS decls are silently dropped
    // by print_stmt's default arm; Arrow/FunctionExpr/ClassExpr echo RAW source
    // bytes with no erasure edits applied). This switch exists so the cost of
    // wiring can be measured on the corpus, not so it can be turned on.
    bool printer{false};

    // bun's `minify_syntax` (options.rs:1974). Currently only drives the
    // single-use symbol substitution in js_parser/subst.cppm. Requires `printer`
    // — the erasure path emits original bytes, so an AST rewrite cannot reach
    // the output.
    //
    // DEFAULT OFF, and NOT implied by `printer`, because bun has TWO transpiler
    // configurations and mbun currently has one entry point serving both:
    //
    //   Bun.Transpiler / transformSync  -> minify_syntax OFF -> `const c = ctx;
    //                                      f(c)` is preserved verbatim
    //   the RUNTIME loader (bun run /   -> target.is_bun() -> tree_shaking ->
    //   bun test)                          inlining -> minify_syntax ON, so
    //                                      `f(ctx)` is what the module (and
    //                                      hence Function.prototype.toString)
    //                                      actually sees
    //
    // Both verified against bun 1.4.0 (`.mbun/bin/bun-rust`) on the same source:
    // transformSync keeps the alias, `bun run` + toString() shows it inlined —
    // and `minify: true` renames but still does NOT inline it. So a caller that
    // implements the Bun.Transpiler surface must leave this off, while the
    // module loader must turn it on. Wiring it to a real `target` belongs with
    // whoever promotes the printer to production; until then it is opt-in so it
    // cannot silently move the corpus metric, which is scored against
    // Bun.Transpiler (i.e. against minify_syntax OFF).
    bool minify_syntax{false};

    // bun's `trim_unused_imports` (js_parser/parser.rs:239): drop a TS import
    // whose every local binding is unused. See js_parser/trim_imports.cppm for
    // the mechanism and for the THREE measured per-path defaults — they
    // disagree, so this cannot be inferred from any one of them:
    //
    //   Bun.Transpiler/transformSync  OFF  (JSTranspiler.rs:645 — .unwrap_or(
    //                                       tree_shaking), tree_shaking=false)
    //   the RUNTIME loader (bun run)  ON   (bundler/transpiler.rs:1606-1609 —
    //                                       .unwrap_or_else(|| loader.is_typescript()))
    //   the BUNDLER (bun build)       ON   (bundler/ParseTask.rs:2435-2436)
    //
    // DEFAULT OFF for the same reason minify_syntax above is: one entry point
    // serves both bun configurations, and the corpus metric is scored against
    // Bun.Transpiler — i.e. against trim OFF. Verified on bun 1.4.0:
    //     new Bun.Transpiler({loader:"ts"}).transformSync("import {T} from 'y';let x: T")
    //       ->  import { T } from "y";\nlet x;      // the import SURVIVES
    // Whoever wires the module loader turns it on; nothing else may.
    //
    // Unlike minify_syntax this does NOT require `printer`: it drives the
    // erasure path, which is the one the runtime actually uses.
    bool trim_unused_imports{false};
};

// MEASUREMENT-ONLY escape hatch for the corpus harness, so the printer path can
// be exercised through callers that do not thread TranspileOptions. Read once.
// Absent/"0" → erasure, i.e. the default path is untouched.
[[nodiscard]] inline bool printer_path_enabled_() {
    static const bool on = [] {
        const char* v = std::getenv("MBUN_TRANSPILE_PRINTER");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}

// Companion escape hatch for TranspileOptions::minify_syntax — see the note
// there for why this is a SEPARATE switch and not implied by the printer.
[[nodiscard]] inline bool minify_syntax_enabled_() {
    static const bool on = [] {
        const char* v = std::getenv("MBUN_TRANSPILE_MINIFY_SYNTAX");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}

// Companion escape hatch for TranspileOptions::trim_unused_imports, so the
// corpus harness can measure the runtime path's configuration without a caller
// that threads TranspileOptions. Absent/"0" -> no trimming, i.e. the default
// path is untouched.
[[nodiscard]] inline bool trim_unused_imports_enabled_() {
    static const bool on = [] {
        const char* v = std::getenv("MBUN_TRANSPILE_TRIM_UNUSED_IMPORTS");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}

// Transpile TS/TSX source to JavaScript by **erasure**: parse the source
// (recording deletions of type-only spans and lowering enums), then emit the
// original bytes with only those edits applied. Unlike an AST-rebuild printer
// this leaves all JavaScript formatting/semantics byte-for-byte intact, so the
// output is safe to hand to a JS engine even for constructs the AST subset does
// not model. On a parse error the original source is returned unchanged so the
// caller still sees the engine's own diagnostic.
//
// `use_printer` is resolved ONCE by the `transpile()` wrapper below (from
// opts.printer or the env escape hatch) and threaded explicitly, so the CJS
// second pass can force erasure without the env var re-enabling the printer
// underneath it — see the two-pass note in the printer branch.
TranspileResult transpile_(std::string_view src, const TranspileOptions& opts, bool use_printer) {
    // The printer rebuilds the statement from the AST and never applies the
    // arena's edits, so asking the parser for the CJS lowering here would only
    // build strings nothing reads — the second pass below re-derives them from
    // the printed bytes instead. Parse pass 1 in ESM mode.
    // This file's own `@jsxRuntime` / `@jsx` / `@jsxFrag` / `@jsxImportSource`
    // comment pragmas override the caller's options. A WHOLE-FILE pre-pass, not
    // a lex-time hook, because bun honors a pragma wherever it appears —
    // including in a `//` comment AFTER the last element (verified on the
    // oracle) — while mbun lowers JSX in source order inside the pre-lexer, so
    // by then it would be too late. ref js_parser/lexer.rs:2589-2656.
    detail::JsxOptions jsxOpts{opts.jsx_options};
    if (opts.jsx) {
        detail::JsxPragmaResult pragma{detail::scan_jsx_pragmas(src, jsxOpts)};
        if (!pragma.ok) {
            // ref oracle: `/** @jsxRuntime bogus */` is a hard error
            // (`Unsupported JSX runtime: "bogus"`), not a shrug.
            TranspileResult bad;
            bad.ok = false;
            bad.error = std::move(pragma.error);
            bad.error_offset = 0;
            bad.code = std::string{src};
            return bad;
        }
    }
    detail::Parser p{src, use_printer ? false : opts.cjs, opts.jsx, opts.legacy_decorators,
                     std::move(jsxOpts), opts.trim_unused_imports};
    ParseResult r = p.run();
    TranspileResult out;
    out.ok = r.ok;
    out.error = r.error;
    out.error_offset = r.error_offset;
    out.cjs_esm_module = r.cjs_esm_module;
    out.top_level_await = r.top_level_await;
    if (!r.ok) {
        out.code = std::string{src};
        return out;
    }
    // MEASUREMENT-ONLY branch — see TranspileOptions::printer. When off (always,
    // by default) this is exactly the erasure line it has always been.
    if (use_printer) {
        // bun's `minify_syntax` single-use substitution (`const c = ctx; f(c)` ->
        // `f(ctx)`), reached in bun via options.rs:1970 `target.is_bun()` ->
        // tree_shaking -> inlining -> minify_syntax. ON for every file bun's own
        // RUNTIME transpiles, which is why Function.prototype.toString() under
        // `bun test` reports the inlined form — and OFF for Bun.Transpiler.
        // Hence the separate opt-in; see TranspileOptions::minify_syntax.
        if (opts.minify_syntax || minify_syntax_enabled_())
            detail::substitute_single_use_symbols(r.arena, r.program);
        // ref js_parser/visit/visit_stmt.rs:168 `s_export_clause`. Drops every
        // `export {…}` (no `from`) specifier whose local name is not a module-scope
        // value binding, exactly as bun's visit pass does — `export {a}` with no
        // `a` in scope becomes `export {};`. UNCONDITIONAL (unlike the minify
        // pass above): it is not an optimisation but a correctness precondition
        // for printing the clause at all, since printing it verbatim would emit
        // references to bindings that TS erasure removed. Printer branch only, so
        // the erasure path never sees it — see module_scope.cppm's header.
        detail::filter_export_clauses(r.arena, r.program);
        std::string printed;
        using mbun::js_printer::IsTopLevel;
        using mbun::js_printer::Options;
        using mbun::js_printer::Printer;
        using mbun::js_printer::PrinterFlags;
        using mbun::js_printer::StringSink;
        using mbun::js_printer::TopLevel;
        Printer<PrinterFlags{}, StringSink> pr{StringSink{printed}, Options{}};
        pr.bind_arena(r.arena);
        pr.source = src;
        pr.print_stmt(r.program, TopLevel::init(IsTopLevel::Yes));
        // ── the printer is now allowed to say no ─────────────────────────────
        // `out.ok` above is `r.ok`, i.e. the PARSE result, and for the printer
        // path that was never the whole truth: print_stmt's `default:` dropped
        // every kind it had no port for (TS enum/namespace, `export {a}`, and —
        // until b9c11ca6 — Arrow/FunctionExpr/ClassExpr echoed raw source), and
        // the caller got `ok == true` over output that was missing statements or
        // was not valid JS. A dropped `import` is not a cosmetic diff: the
        // module simply does not have its dependencies any more.
        //
        // The printer records those kinds now (js_printer/unsupported.cppm), so
        // this is where the record becomes an answer. Reporting `ok == false`
        // over incomplete output is strictly more honest than reporting success,
        // and it is what makes the gap COUNTABLE on the corpus instead of
        // showing up as a silent byte-diff nobody attributes.
        //
        // ⚠️ This can only fire when `use_printer` is true, i.e. behind the
        // MEASUREMENT-ONLY `TranspileOptions::printer` flag. The default erasure
        // path never constructs a Printer and is untouched.
        if (pr.has_unsupported()) {
            out.ok = false;
            out.error = pr.unsupported_message();
            out.error_offset = pr.first_unsupported().offset;
            out.code = std::string { src };
            return out;
        }
        // ── two-pass: printer emits ESM, erasure lowers it to CJS ────────────
        // The printer prints the AST, and EVERY lowering this transpiler owns is
        // an arena Edit (78 add_edit sites: TS erasure, enum, JSX, decorators,
        // `using`, ESM->CJS, import.meta, dynamic import) — none of which the
        // printer path applies. For a `cjs` caller that is not cosmetic: the
        // script-mode runtime evaluates via the C API's JSEvaluateScript, which
        // has NO ESM linker (JSBase.h declares no JSEvaluateModule — see
        // jsc/module_loader.cppm:83), so JSC parses a printed `import { x } from
        // "m"` as a dynamic `import(` CALL and dies with
        //   SyntaxError: Unexpected token '{'. import call expects one or two arguments.
        // i.e. the printer's ESM was being fed to a *script* entry point.
        //
        // Rather than reimplement the lowering against the AST (a second
        // implementation of the same semantics, free to drift from the erasure
        // one that the default path — and every test — actually exercises), run
        // the printed bytes back through erasure in cjs mode. The printer's
        // output is ordinary JS, so this is exactly the transformation the
        // default path already applies to the original source, reusing all 78
        // edits and the __esModule / __mbun_link machinery verbatim.
        //
        // Cost: a second parse per module. Acceptable while this is an opt-in
        // measurement flag; if the printer is ever promoted to the default the
        // lowering should move onto the AST (bun's own `rewrite_esm_to_cjs` is
        // vestigial in 1.4.0 — js_printer/lib.rs:8156, comment only — because
        // bun's runtime links real ESM through JSC::JSModuleLoader instead).
        //
        // NOTE this pass CANNOT rescue a statement the printer DROPS: `export
        // {a}` (ExportForm::Clause, js_printer/stmt.cppm:940) and the TS decls
        // (EnumDecl/NamespaceDecl -> stmt.cppm's `default:`) are printed as
        // nothing, so pass 2 never sees them. Those are printer AST-coverage
        // gaps, tracked separately — they are NOT what this fixes.
        if (opts.cjs) {
            TranspileOptions second{opts};
            second.printer = false;
            // Already applied to the AST above; erasure emits source bytes, so an
            // AST rewrite could not reach pass 2's output anyway.
            second.minify_syntax = false;
            // Pass 1 already recorded the trim; pass 2 lowers ESM->CJS, where
            // every import statement's bytes are replaced by the require()
            // lowering anyway (finish_stmt marks those untrimmable).
            second.trim_unused_imports = false;
            TranspileResult lowered{transpile_(printed, second, /*use_printer=*/false)};
            // A parse failure here is a PRINTER bug (it emitted invalid JS), not
            // the user's. Surface it loudly rather than silently shipping the
            // un-lowered ESM to an engine that cannot evaluate it. `error_offset`
            // then indexes the printed text, not `src` — the honest coordinate
            // for a diagnostic about printed bytes.
            return lowered;
        }
        out.code = std::move(printed);
    } else {
        out.code = r.arena.erase_slice(0, static_cast<std::uint32_t>(src.size()), src);
    }
    // A CJS-lowered module that used ESM `export` gets an __esModule marker so a
    // default import interops correctly (import def → module.exports.default).
    //
    // Hygiene: the marker (and the `export *` / live-binding lowerings below) must
    // reach the real `Object`, but this prelude is spliced into the user's own
    // module scope — an `export function Object(x){}` hoists over any bare
    // `Object` reference here and the marker dies with "Object.defineProperty is
    // not a function". Bun avoids this by keeping its helpers in a separate
    // runtime module and capturing `var __defProp = Object.defineProperty` there
    // (bun-ref src/runtime.js:10), so a user symbol in a nested module scope can
    // never shadow it; its bundler additionally renames colliding user symbols.
    // This erasure transpiler has neither an outer scope of its own nor a
    // renamer, so it captures the same value *without naming any global*:
    // `({}).constructor` is the real `Object` and no user declaration can
    // intercept it. Kept self-contained (rather than an engine-installed
    // `globalThis.__mbun_*` helper) because the bundler emits standalone bundles
    // that run outside mbun — see bundler/vertical_slice.cppm:785.
    //
    // `var`, not `const`: an entry module is eval'd at global scope (the runtime
    // only wraps *required* modules in a function — engine.inc transform_module_),
    // so two CJS-lowered files reaching the same context would make a second
    // `const __mbun_O` a "cannot declare a const twice" SyntaxError. `var`
    // redeclaration is legal and idempotent here, which is also the form bun's
    // own runtime uses (bun-ref src/runtime.js:10 `var __defProp = …`).
    if (opts.cjs && r.cjs_esm_export) {
        const std::string O{detail::kObjectAlias};
        std::string prelude{"var " + O + " = ({}).constructor; " + O +
                            ".defineProperty(exports, \"__esModule\", { value: true });"};
        // `__mbun_X(k, g)` publishes `k` as a live getter on `exports`. Only when
        // some export actually lowered to one: a module whose exports are all
        // `export default <expr>` / `export … from "m"` never calls it, and every
        // module pays for this prelude.
        //
        // The `__mbun_subs` branch pushes the value through a cyclic importer's
        // __mbun_link accessor (engine.inc) instead of redefining over it, which
        // would strand that importer's binding at undefined. The setter degrades
        // the slot back to a plain data property, so an importer or a re-exporter
        // assigning to it wins, as it would have against the old `exports.k = k`.
        // `var` for the same reason kObjectAlias uses it (see below).
        if (r.cjs_live_export) {
            prelude += " var " + std::string{detail::kLiveExportAlias} +
                       " = (k, g) => { const d = " + O +
                       ".getOwnPropertyDescriptor(exports, k);"
                       " if (d && d.get && d.get.__mbun_subs) { exports[k] = g(); return; } " +
                       O + ".defineProperty(exports, k, { get: g, set: (v) => " + O +
                       ".defineProperty(exports, k, { value: v, writable: true,"
                       " enumerable: true, configurable: true }), enumerable: true,"
                       " configurable: true }); };";
            prelude += " var " + std::string{detail::kLiveRepushAlias} +
                       " = (k, g) => { const d = " + O +
                       ".getOwnPropertyDescriptor(exports, k);"
                       " if (d && d.get && d.get.__mbun_subs) { exports[k] = g(); } };";
        }
        out.code.insert(0, prelude + "\n");
        // Re-push every live export now the body has run and each local is final;
        // see kLiveRepushAlias. A no-op unless a cyclic importer linked the slot.
        if (!r.cjs_live_names.empty()) {
            std::string tail{"\n"};
            for (const auto& [key, local] : r.cjs_live_names) {
                tail += std::string{detail::kLiveRepushAlias} + "(" + key + ", () => " + local + ");";
            }
            out.code += tail;
        }
    }
    return out;
}

// Public entry point. Resolves the printer switch ONCE — opts.printer or the
// MBUN_TRANSPILE_PRINTER escape hatch — and threads the answer down, so the
// CJS second pass can demand erasure without the env var overriding it.
TranspileResult transpile(std::string_view src, TranspileOptions opts = {}) {
    // Fold the env hatch in HERE, once. `opts` is by value, so the CJS second
    // pass inherits the resolved answer instead of re-reading the environment
    // underneath itself — the same reason `use_printer` is threaded explicitly.
    opts.trim_unused_imports = opts.trim_unused_imports || trim_unused_imports_enabled_();
    return transpile_(src, opts, opts.printer || printer_path_enabled_());
}

}  // namespace mbun::js_parser
