// test_js_parser.cpp — T2.4 mbun.js_parser + mbun.ast (JS/TS parser) test suite.
//
// Vectors are extracted from bun's original transpiler test suite (assertion
// semantics preserved, see AGENTS.md TDD rules). Primary source file:
//   - .mbun/bun-ref/test/bundler/transpiler/transpiler.test.js
// Each `err(...)`/`ok(...)` cites the bun `it()` block it derives from. bun's
// `expectParseError(code, msg)` asserts `err.message === msg` (exact), so these
// assert the message verbatim.
//
// This suite consumes the DEFERRED(T2.4) checklist recorded in the header of
// test_js_lexer.cpp — every "needs the parser to judge" case listed there is
// realised below (or explicitly re-deferred with a reason). Coverage map:
//   [T2.4 ✓] private-identifier errors, escaped-keyword rejection, malformed
//            enums, empty type parameters, type-parameter variance modifiers,
//            tuple-label reserved-keyword rejection, the ">>"/">>>" instantiation
//            re-split family (Unexpected >, >>, >>>, >=, >>=, >>>= and Invalid
//            assignment target), unterminated template after type arguments.
//
// DEFERRED(T2.5) — printer round-trips. Every `expectPrinted*` vector in the bun
// blocks below needs mbun.js_printer (T2.5) to judge the *output* string; here we
// only assert those inputs PARSE without error (the parse half of the contract).
// The exact printed form is verified in the T2.5 suite. This covers, e.g., the
// instantiation-expression cases that are valid (`f<x> = g<y>` -> "f = g"), the
// empty type params allowed for type/interface (`type X<> = never`), and the
// nested-generic ">>" re-split in type position (`Array<Array<number>>`).
//
// DEFERRED(T2.4, re-deferred with reason):
//   - transpiler.test.js > "rejects export clauses inside a non-declare
//     namespace" ("Unexpected {" / "Unexpected *"): needs import/export-clause
//     parsing inside a namespace body (module-record semantics), landing with the
//     import/export work; the tuple-label & instantiation halves of the checklist
//     are covered here.
//   - the single quoted tuple-label vector `type _const = [const: string]` ->
//     'Unexpected "const"' (bun quotes only this keyword in that position; the
//     other 20+ tuple-label keywords are unquoted and ARE covered). The quoting
//     rule is bun-idiosyncratic and is pinned in the T2.5 pass.
import std;
import mbun.js_parser;
import mbun.ast;

namespace {

using mbun::js_parser::parse;
using mbun::js_parser::ParseResult;

int gChecks{0};
int gFailures{0};
constexpr int MAX_FAILURE_PRINTS{60};

void report_failure(std::string_view what) {
    ++gFailures;
    if (gFailures <= MAX_FAILURE_PRINTS) {
        std::println("  FAIL {}", what);
    }
}

std::string printable(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
        } else {
            out += std::format("\\x{:02X}", c);
        }
    }
    return out;
}

// The input must fail to parse with exactly `message` (bun expectParseError).
void err(std::string_view src, std::string_view message, std::string_view ref) {
    ++gChecks;
    ParseResult r = parse(src);
    if (r.ok) {
        report_failure(std::format("[{}] parse(\"{}\") expected error \"{}\", got none", ref,
                                   printable(src), printable(message)));
        return;
    }
    if (r.error != message) {
        report_failure(std::format("[{}] parse(\"{}\") error = \"{}\", expected \"{}\"", ref,
                                   printable(src), printable(r.error), printable(message)));
    }
}

// The input must parse without any error (DEFERRED(T2.5) for the printed form).
void ok(std::string_view src, std::string_view ref) {
    ++gChecks;
    ParseResult r = parse(src);
    if (!r.ok) {
        report_failure(std::format("[{}] parse(\"{}\") unexpected error \"{}\"", ref,
                                   printable(src), printable(r.error)));
    }
}

// The top-level program must have exactly `count` statements, the first of kind
// `kind` (exercises the AST shape).
void ok_program(std::string_view src, std::size_t count, mbun::ast::NodeKind firstKind,
                std::string_view ref) {
    ++gChecks;
    ParseResult r = parse(src);
    if (!r.ok) {
        report_failure(std::format("[{}] parse(\"{}\") unexpected error \"{}\"", ref,
                                   printable(src), printable(r.error)));
        return;
    }
    const auto& prog = r.arena.at(r.program);
    auto stmts = r.arena.list_of(prog);
    if (stmts.size() != count) {
        report_failure(std::format("[{}] parse(\"{}\") stmt count = {}, expected {}", ref,
                                   printable(src), stmts.size(), count));
        return;
    }
    if (count > 0 && r.arena.at(stmts[0]).kind != firstKind) {
        report_failure(std::format("[{}] parse(\"{}\") first stmt = {}, expected {}", ref,
                                   printable(src),
                                   mbun::ast::node_kind_name(r.arena.at(stmts[0]).kind),
                                   mbun::ast::node_kind_name(firstKind)));
    }
}

// Assert `idx` names a node of `kind`; returns false (having reported) if not,
// so a caller can stop walking a shape that is already wrong.
bool has_kind(const ParseResult& r, mbun::ast::NodeIndex idx, mbun::ast::NodeKind kind,
              std::string_view what) {
    ++gChecks;
    if (idx == mbun::ast::NONE) {
        report_failure(std::format("{}: slot is NONE, expected {}", what,
                                   mbun::ast::node_kind_name(kind)));
        return false;
    }
    if (r.arena.at(idx).kind != kind) {
        report_failure(std::format("{}: kind = {}, expected {}", what,
                                   mbun::ast::node_kind_name(r.arena.at(idx).kind),
                                   mbun::ast::node_kind_name(kind)));
        return false;
    }
    return true;
}

void check(bool cond, std::string_view what) {
    ++gChecks;
    if (!cond) {
        report_failure(what);
    }
}

// Parse `src` and hand back its single top-level statement.
mbun::ast::NodeIndex only_stmt(const ParseResult& r) {
    if (!r.ok || r.program == mbun::ast::NONE) {
        return mbun::ast::NONE;
    }
    auto stmts = r.arena.list_of(r.arena.at(r.program));
    return stmts.size() == 1 ? stmts[0] : mbun::ast::NONE;
}

// ── vector groups ────────────────────────────────────────────────────────────

void test_private_identifiers() {
    // source: transpiler.test.js > it("private identifiers")
    const char* SRC{"transpiler.test.js > private identifiers"};
    err("#foo", "Unexpected #foo", SRC);
    err("#foo in this", "Unexpected #foo", SRC);
    err("this.#foo", "Expected identifier but found \"#foo\"", SRC);
    err("this?.#foo", "Expected identifier but found \"#foo\"", SRC);
    err("({ #foo: 1 })", "Expected identifier but found \"#foo\"", SRC);
    err("class Foo { x = { #foo: 1 } }", "Expected identifier but found \"#foo\"", SRC);
    err("class Foo { x = #foo }", "Expected \"in\" but found \"}\"", SRC);
    err("class Foo { #foo; foo() { delete this.#foo } }",
        "Deleting the private name \"#foo\" is forbidden", SRC);
    err("class Foo { #foo; foo() { delete this?.#foo } }",
        "Deleting the private name \"#foo\" is forbidden", SRC);
    err("class Foo extends Bar { #foo; foo() { super.#foo } }",
        "Expected identifier but found \"#foo\"", SRC);
    err("class Foo { #foo = () => { for (#foo in this) ; } }", "Unexpected #foo", SRC);
    err("class Foo { #foo = () => { for (x = #foo in this) ; } }", "Unexpected #foo", SRC);

    // Valid private-name uses (DEFERRED(T2.5) printed form): parse only.
    ok("class Foo { #foo }", SRC);
    ok("class Foo { #foo = 1 }", SRC);
    ok("class Foo { #foo = #foo in this }", SRC);
    ok("class Foo { #foo; foo() { return this.#foo } }", SRC);
}

void test_identifier_escapes() {
    // source: transpiler.test.js > it("identifier escapes")
    // C++ note: "\\u0076" is written with a doubled backslash so the SOURCE text
    // is the six characters v (an escape), not the UCN for 'v'.
    const char* SRC{"transpiler.test.js > identifier escapes"};
    err("var \\u0076\\u0061\\u0072", "Expected identifier but found \"\\u0076\\u0061\\u0072\"",
        SRC);
    err("\\u0076\\u0061\\u0072 foo", "Unexpected \\u0076\\u0061\\u0072", SRC);
    // Escaped identifiers that are NOT reserved words are fine (parse only).
    ok("var _\\u0076\\u0061\\u0072", SRC);
    ok("foo._\\u0076\\u0061\\u0072", SRC);
    ok("foo.\\u0076\\u0061\\u0072", SRC);
}

void test_malformed_enums() {
    // source: transpiler.test.js > it("malformed enums")
    const char* SRC{"transpiler.test.js > malformed enums"};
    err("enum Foo { [2]: 'hi' }", "Expected identifier but found \"[\"", SRC);
    err("enum [] { a }", "Expected identifier but found \"[\"", SRC);
    // Valid enum parses (printed form DEFERRED(T2.5)).
    ok("enum Foo { A, B, C }", SRC);
    ok("enum Foo { A = 1, B = 2 }", SRC);
    ok("const enum Foo { A }", SRC);
}

void test_empty_type_parameters() {
    // source: transpiler.test.js > it("should parse empty type parameters")
    const char* SRC{"transpiler.test.js > should parse empty type parameters"};
    err("class Foo<> {}", "Expected identifier but found \">\"", SRC);
    err("function foo<>(): void {}", "Expected identifier but found \">\"", SRC);
    err("const x: Foo<> = {}", "Unexpected >", SRC);
    // Empty type params ARE allowed for type/interface (parse only; DEFERRED(T2.5)).
    ok("type X<> = never;var x: X", SRC);
    ok("interface X<> {};var x: X", SRC);
    ok("class Foo<T> {}", SRC);
    ok("function foo<T>(): void {}", SRC);
}

void test_type_parameter_modifiers() {
    // source: transpiler.test.js > TypeScript > type-parameter variance blocks
    const char* SRC{"transpiler.test.js > TypeScript > type parameter modifiers"};
    err("type Foo<i\\u006E T> = T", "Expected identifier but found \"i\\u006E\"", SRC);
    err("type Foo<ou\\u0074 T> = T", "Expected \">\" but found \"T\"", SRC);
    err("type Foo<in in> = T", "The modifier \"in\" is not valid here", SRC);
    err("type Foo<out in> = T", "The modifier \"in\" is not valid here", SRC);
    err("type Foo<out in T> = T", "The modifier \"in\" is not valid here", SRC);
    // Valid modifier forms (parse only; printed form is empty, DEFERRED(T2.5)).
    ok("type Foo<in T> = T", SRC);
    ok("type Foo<out T> = T", SRC);
    ok("type Foo<in out T> = T", SRC);
    ok("type Foo<const T> = T", SRC);
    ok("type Foo<out out> = T", SRC);
    ok("type Foo<in X, out Y> = [X, Y]", SRC);
}

void test_tuple_labels() {
    // source: transpiler.test.js > TypeScript > tuple-label blocks (~795-821).
    // Reserved keywords are invalid tuple labels -> "Unexpected <keyword>".
    // (`type _const = [const: string]` -> 'Unexpected "const"' is re-deferred; it
    // is the only quoted one — see the file header.)
    const char* SRC{"transpiler.test.js > TypeScript > tuple labels"};
    err("type _break = [break: string]", "Unexpected break", SRC);
    err("type _case = [case: string]", "Unexpected case", SRC);
    err("type _catch = [catch: string]", "Unexpected catch", SRC);
    err("type _class = [class: string]", "Unexpected class", SRC);
    err("type _continue = [continue: string]", "Unexpected continue", SRC);
    err("type _debugger = [debugger: string]", "Unexpected debugger", SRC);
    err("type _default = [default: string]", "Unexpected default", SRC);
    err("type _delete = [delete: string]", "Unexpected delete", SRC);
    err("type _do = [do: string]", "Unexpected do", SRC);
    err("type _else = [else: string]", "Unexpected else", SRC);
    err("type _enum = [enum: string]", "Unexpected enum", SRC);
    err("type _export = [export: string]", "Unexpected export", SRC);
    err("type _extends = [extends: string]", "Unexpected extends", SRC);
    err("type _finally = [finally: string]", "Unexpected finally", SRC);
    err("type _for = [for: string]", "Unexpected for", SRC);
    err("type _if = [if: string]", "Unexpected if", SRC);
    err("type _in = [in: string]", "Unexpected in", SRC);
    err("type _instanceof = [instanceof: string]", "Unexpected instanceof", SRC);
    err("type _return = [return: string]", "Unexpected return", SRC);
    err("type _super = [super: string]", "Unexpected super", SRC);
    err("type _switch = [switch: string]", "Unexpected switch", SRC);
    err("type _throw = [throw: string]", "Unexpected throw", SRC);
    err("type _try = [try: string]", "Unexpected try", SRC);
    err("type _var = [var: string]", "Unexpected var", SRC);
    err("type _while = [while: string]", "Unexpected while", SRC);
    err("type _with = [with: string]", "Unexpected with", SRC);
}

void test_instantiation_resplit() {
    // source: transpiler.test.js > TypeScript > it("types") (~471-482).
    // The ">>"/">>>" re-split family: after an attempted type-argument list that
    // is NOT a valid instantiation, "<" is relational and the following ">"-run
    // lands in operand position -> "Unexpected <op>".
    const char* SRC{"transpiler.test.js > TypeScript > types (>> re-split)"};
    err("f<x> > g<y>;", "Unexpected >", SRC);
    err("f<x> >> g<y>;", "Unexpected >>", SRC);
    err("f<x> >>> g<y>;", "Unexpected >>>", SRC);
    err("f<x> >= g<y>;", "Unexpected >=", SRC);
    err("f<x> >>= g<y>;", "Unexpected >>=", SRC);
    err("f<x> >>>= g<y>;", "Unexpected >>>=", SRC);
    // No-space forms bind ">>="/">>>=" as assignment to a non-target -> invalid.
    err("f<x>>=g<y>;", "Invalid assignment target", SRC);
    err("f<x>>>=g<y>;", "Invalid assignment target", SRC);

    // Instantiation / relational forms that are VALID (printed form DEFERRED(T2.5)).
    ok("f<x>>g<y>;", SRC);       // "f < x >> g"
    ok("f<x>>>g<y>;", SRC);      // "f < x >>> g"
    ok("f<x> = g<y>;", SRC);     // "f = g"
    ok("f<x> * g<y>;", SRC);     // "f * g"
    ok("f<x> == g<y>;", SRC);    // "f == g"
    ok("f<x> ?? g<y>;", SRC);    // "f ?? g"
    ok("f<x> in g<y>;", SRC);    // "f in g"
    ok("f<x> instanceof g<y>;", SRC);
    ok("f<x> ? g<y> : h<z>;", SRC);
    ok("f<x>, g<y>;", SRC);
    ok("a([f<x>]);", SRC);
    ok("f<number>?.();", SRC);
    ok("f<x> + g<y>;", SRC);     // relational: "f < x > +g"
    ok("f<x> - g<y>;", SRC);
}

void test_generic_gt_resplit_in_types() {
    // The "expect_greater_than-style rescan" peels one ">" per nested list.
    // source: transpiler.test.js > TypeScript > "types" (Array<Array<number>>).
    const char* SRC{"transpiler.test.js > TypeScript > nested generic re-split"};
    ok("let x: Array<Array<number>>", SRC);
    ok("let x: Array<Array<Array<number>>>", SRC);
    ok("let x: Map<string, Array<number>> = y", SRC);
    ok("let x: Array<Array<number>> = y", SRC);
    ok("function f<T extends Array<Array<number>>>(): void {}", SRC);
}

void test_unterminated_template_after_type_args() {
    // source: transpiler.test.js > it("does not crash on an unterminated template
    // literal after type arguments"). The unterminated backtick surfaces the
    // lexical error through the parser's pre-lex.
    const char* SRC{"transpiler.test.js > unterminated template after type arguments"};
    err("new C<T>\n`", "Unterminated string literal", SRC);
    err("new C<T>`", "Unterminated string literal", SRC);
    err("f<T>`", "Unterminated string literal", SRC);
    // The terminated forms parse (printed form DEFERRED(T2.5)).
    ok("new C<T>`ok`", SRC);
    ok("f<T>`ok`", SRC);
}

// ── G::Property: kind / flags / initializer ──────────────────────────────────
//
// Blueprint: bun `G::Property` (src/ast/g.rs:143) + `G::PropertyKind` (g.rs:245)
// + `flags::Property` (src/ast/lib.rs:3283). These pin that each SYNTACTIC FORM
// of an object/class member builds a DISTINGUISHABLE node — before this, the
// parser recognised `get`/`set`/computed/method/shorthand and then discarded all
// of it, so `{get a(){}}` and `{a: f}` built byte-identical nodes and
// print_property (lib.rs:4743) had no input it could switch on.
//
// The forms below are the ones print_property branches on, each cited to the arm
// that consumes it.
void test_object_property_kinds_and_flags() {
    using K = mbun::ast::NodeKind;
    using mbun::ast::NodeIndex;
    using mbun::ast::PropertyKind;
    namespace pf = mbun::ast::pflags;
    const char* SRC{"ast/g.rs:143 G::Property + js_printer/lib.rs:4743 print_property"};

    // `({...})` — a `{` in statement position is a Block, so the object literal
    // is reached through a Paren, exactly as bun's tests write it.
    auto props = [](const ParseResult& r) -> std::vector<NodeIndex> {
        NodeIndex st = only_stmt(r);
        if (st == mbun::ast::NONE || r.arena.at(st).kind != K::ExpressionStmt) {
            return {};
        }
        NodeIndex paren = r.arena.at(st).a;
        if (paren == mbun::ast::NONE || r.arena.at(paren).kind != K::Paren) {
            return {};
        }
        NodeIndex obj = r.arena.at(paren).a;
        if (obj == mbun::ast::NONE || r.arena.at(obj).kind != K::ObjectLiteral) {
            return {};
        }
        auto sp = r.arena.list_of(r.arena.at(obj));
        return {sp.begin(), sp.end()};
    };
    auto kind_of = [](const ParseResult& r, NodeIndex p) {
        return static_cast<PropertyKind>(r.arena.at(p).aux);
    };

    // ── the acceptance vector: every form at once, each distinguishable ──────
    {
        ParseResult r = parse("({get a(){}, set a(v){}, async b(){}, [k]: v, ...rest, c, d: e})");
        check(r.ok, "all-forms: parses");
        std::vector<NodeIndex> p = props(r);
        check(p.size() == 7, std::format("all-forms: 7 properties, got {}", p.size()));
        if (p.size() == 7) {
            // every one of them is a Property node — including the spread, which
            // used to be pushed as a BARE EXPRESSION (losing the `...` entirely).
            for (std::size_t i = 0; i < p.size(); ++i) {
                check(r.arena.at(p[i]).kind == K::Property,
                      std::format("all-forms: [{}] is a Property", i));
            }
            // `get a(){}` — ref lib.rs:4816
            check(kind_of(r, p[0]) == PropertyKind::Get, "all-forms: [0] get -> PropertyKind::Get");
            check((r.arena.at(p[0]).flags & pf::IsMethod) != 0, "all-forms: [0] get is a method");
            // `set a(v){}` — ref lib.rs:4822
            check(kind_of(r, p[1]) == PropertyKind::Set, "all-forms: [1] set -> PropertyKind::Set");
            // `async b(){}` — ref lib.rs:4843. See the DEFERRED note below: async
            // is NOT a G::Property field, so this is Normal+IsMethod.
            check(kind_of(r, p[2]) == PropertyKind::Normal, "all-forms: [2] async -> Normal");
            check((r.arena.at(p[2]).flags & pf::IsMethod) != 0, "all-forms: [2] async is a method");
            // `[k]: v` — ref lib.rs:4869
            check((r.arena.at(p[3]).flags & pf::IsComputed) != 0, "all-forms: [3] [k] is computed");
            check(kind_of(r, p[3]) == PropertyKind::Normal, "all-forms: [3] [k]: v -> Normal");
            // `...rest` — ref lib.rs:4759 / parse_prefix.rs:820-829
            check(kind_of(r, p[4]) == PropertyKind::Spread, "all-forms: [4] ... -> Spread");
            check(r.arena.at(p[4]).a == mbun::ast::NONE,
                  "all-forms: [4] spread has NO key (g.rs:158)");
            check(r.arena.at(p[4]).b != mbun::ast::NONE, "all-forms: [4] spread has a value");
            check(r.arena.at(p[4]).flags == 0,
                  "all-forms: [4] spread sets NO flags (bun uses kind, not IsSpread)");
            // `c` — ref lib.rs:4921
            check((r.arena.at(p[5]).flags & pf::WasShorthand) != 0, "all-forms: [5] c is shorthand");
            check(kind_of(r, p[5]) == PropertyKind::Normal, "all-forms: [5] c -> Normal");
            // `d: e` — the plain case: no kind, no flags at all
            check(kind_of(r, p[6]) == PropertyKind::Normal, "all-forms: [6] d: e -> Normal");
            check(r.arena.at(p[6]).flags == 0, "all-forms: [6] d: e has no flags");
            check(r.arena.at(p[6]).a != mbun::ast::NONE && r.arena.at(p[6]).b != mbun::ast::NONE,
                  "all-forms: [6] d: e has key AND value");
        }
    }

    // ── THE REGRESSION THIS EXISTS FOR ──────────────────────────────────────
    // A getter and a plain property used to build byte-identical nodes. Compare
    // the two directly so a future edit that drops `kind` fails here.
    {
        ParseResult rg = parse("({get a(){}})");
        ParseResult rp = parse("({a: f})");
        std::vector<NodeIndex> g = props(rg);
        std::vector<NodeIndex> q = props(rp);
        check(g.size() == 1 && q.size() == 1, "getter-vs-plain: one property each");
        if (g.size() == 1 && q.size() == 1) {
            check(kind_of(rg, g[0]) != kind_of(rp, q[0]),
                  "getter-vs-plain: kinds DIFFER (was identical before g.rs:245 landed)");
            check(kind_of(rg, g[0]) == PropertyKind::Get, "getter-vs-plain: getter is Get");
            check(kind_of(rp, q[0]) == PropertyKind::Normal, "getter-vs-plain: plain is Normal");
        }
    }

    // ── `{a = 1}`: the initializer slot (g.rs:143-151 names this exact syntax) ─
    {
        ParseResult r = parse("({a = 1} = o)");
        check(r.ok, "shorthand-default: parses");
        // reached through the assignment's LHS rather than a Paren
        NodeIndex st = only_stmt(r);
        NodeIndex asn = st != mbun::ast::NONE ? r.arena.at(st).a : mbun::ast::NONE;
        if (asn != mbun::ast::NONE && r.arena.at(asn).kind == K::Assignment) {
            NodeIndex lhs = r.arena.at(asn).a;
            if (lhs != mbun::ast::NONE && r.arena.at(lhs).kind == K::Paren) {
                lhs = r.arena.at(lhs).a;
            }
            if (has_kind(r, lhs, K::ObjectLiteral, "shorthand-default: LHS")) {
                auto sp = r.arena.list_of(r.arena.at(lhs));
                check(sp.size() == 1, "shorthand-default: one property");
                if (sp.size() == 1) {
                    check(r.arena.at(sp[0]).c != mbun::ast::NONE,
                          "shorthand-default: initializer recorded (was discarded)");
                    check((r.arena.at(sp[0]).flags & pf::WasShorthand) != 0,
                          "shorthand-default: is shorthand");
                }
            }
        }
    }

    // ── class members ───────────────────────────────────────────────────────
    // The class path builds Property nodes as erasure MARKERS: it never builds a
    // key or a value node (js_parser.cppm parse_class_member_), so only kind and
    // flags are assertable here. That gap is recorded in the port's header.
    auto members = [](const ParseResult& r) -> std::vector<NodeIndex> {
        NodeIndex st = only_stmt(r);
        if (st == mbun::ast::NONE || r.arena.at(st).kind != K::ClassDecl) {
            return {};
        }
        auto sp = r.arena.list_of(r.arena.at(st));
        return {sp.begin(), sp.end()};
    };
    {
        ParseResult r = parse("class C { static m(){} }");
        std::vector<NodeIndex> m = members(r);
        check(m.size() == 1, "class: static method -> one member");
        if (m.size() == 1) {
            check((r.arena.at(m[0]).flags & pf::IsStatic) != 0, "class: static -> IsStatic (:4812)");
            check((r.arena.at(m[0]).flags & pf::IsMethod) != 0, "class: m(){} -> IsMethod");
        }
    }
    {
        ParseResult r = parse("class C { get a(){} }");
        std::vector<NodeIndex> m = members(r);
        if (m.size() == 1) {
            check(static_cast<PropertyKind>(r.arena.at(m[0]).aux) == PropertyKind::Get,
                  "class: get a(){} -> PropertyKind::Get");
        }
    }
    {
        ParseResult r = parse("class C { #x = 1 }");
        std::vector<NodeIndex> m = members(r);
        check(m.size() == 1, "class: private field -> one member");
        if (m.size() == 1) {
            // mbun-only bit: bun expresses private via an EPrivateIdentifier KEY,
            // which this parser does not build (ast.cppm pflags::IsPrivate).
            check((r.arena.at(m[0]).flags & pf::IsPrivate) != 0, "class: #x -> IsPrivate");
            check((r.arena.at(m[0]).flags & pf::IsComputed) == 0,
                  "class: #x is NOT computed (bit 0 used to mean 'private')");
            check(r.arena.at(m[0]).c != mbun::ast::NONE, "class: #x = 1 records the initializer");
        }
    }
    {
        ParseResult r = parse("class C { accessor x = 1 }");
        std::vector<NodeIndex> m = members(r);
        if (m.size() == 1) {
            check(static_cast<PropertyKind>(r.arena.at(m[0]).aux) == PropertyKind::AutoAccessor,
                  "class: accessor -> PropertyKind::AutoAccessor (g.rs:253)");
        }
    }
    {
        ParseResult r = parse("class C { [k] = 1 }");
        std::vector<NodeIndex> m = members(r);
        if (m.size() == 1) {
            check((r.arena.at(m[0]).flags & pf::IsComputed) != 0, "class: [k] -> IsComputed");
        }
    }
    ok("({get a(){}, set a(v){}, async b(){}, [k]: v, ...rest, c, d: e})", SRC);
}

void test_general_parsing_smoke() {
    // General expression/statement coverage exercising the Pratt table and the
    // AST shape (source: transpiler.test.js parser subset — constant folding,
    // member/call chains, etc.; printed form DEFERRED(T2.5)).
    const char* SRC{"transpiler.test.js > parser > general"};
    ok_program("var x = 1;", 1, mbun::ast::NodeKind::VarDecl, SRC);
    ok_program("let y = 2;", 1, mbun::ast::NodeKind::VarDecl, SRC);
    ok_program("const z = 3;", 1, mbun::ast::NodeKind::VarDecl, SRC);
    ok_program("1 + 2 * 3;", 1, mbun::ast::NodeKind::ExpressionStmt, SRC);
    ok_program("function f() {}", 1, mbun::ast::NodeKind::FunctionDecl, SRC);
    ok_program("class C {}", 1, mbun::ast::NodeKind::ClassDecl, SRC);
    ok_program("if (a) b; else c;", 1, mbun::ast::NodeKind::IfStmt, SRC);
    ok_program("{ a; b; }", 1, mbun::ast::NodeKind::Block, SRC);

    // Expressions.
    ok("a.b.c", SRC);
    ok("a?.b?.c", SRC);
    ok("f(1, 2, 3)", SRC);
    ok("a[b][c]", SRC);
    ok("x ? y : z", SRC);
    ok("a = b = c", SRC);
    ok("a += 1", SRC);
    ok("!a && b || c", SRC);
    ok("a ?? b", SRC);
    ok("-a + +b", SRC);
    ok("++a", SRC);
    ok("a--", SRC);
    ok("2 ** 3 ** 4", SRC);
    ok("new Foo(1, 2)", SRC);
    ok("new Foo", SRC);
    ok("[1, 2, 3]", SRC);
    ok("({ a: 1, b: 2, c })", SRC);
    ok("`a${b}c${d}e`", SRC);
    ok("typeof x === 'string'", SRC);
    ok("(a, b, c)", SRC);
    ok("() => 1", SRC);
    ok("(a, b) => a + b", SRC);
    ok("x => x * 2", SRC);
    ok("async function f() {}", SRC);
    ok("for (let i = 0; i < 10; i++) {}", SRC);
    ok("for (const x of xs) {}", SRC);
    ok("for (const k in obj) {}", SRC);
    ok("return", SRC);
    ok("throw new Error('x')", SRC);

    // TypeScript smoke (annotations erased by the parser subset).
    ok("let x: number = 1", SRC);
    ok("function f(a: number, b: string): void {}", SRC);
    ok("type T = { a: number; b: string }", SRC);
    ok("type U = A | B | C", SRC);
    ok("interface I { a: number; b(): void }", SRC);
    ok("namespace N { const x = 1; }", SRC);
    ok("enum E { A, B }", SRC);
    ok("class C<T> extends B<T> implements I<T> { m<U>(): void {} }", SRC);
    ok("declare const x: number", SRC);
    ok("abstract class C {}", SRC);
    ok("const f = <T>(x: T): T => x", SRC);
}

void test_empty_and_smoke() {
    // source: transpiler.test.js > it("scan on empty file does not segfault")
    const char* SRC{"transpiler.test.js > scan on empty file"};
    ok("", SRC);
    ok(" \t\n", SRC);
    ok("// just a comment", SRC);
    ok(";;;", SRC);
}

// Control-flow statements and their child wiring. Before these nodes existed the
// parser walked this syntax correctly but recorded a bare `Block`/`EmptyStmt`
// placeholder and dropped every child, so an AST consumer (the printer) could not
// tell `while` from `with` from `switch`. Shapes follow bun src/ast/s.rs.
void test_control_flow_statements() {
    const char* SRC{"ast: control-flow statements (bun src/ast/s.rs)"};
    using K = mbun::ast::NodeKind;

    // while (test) body — s.rs:162 S::While{test_, body}
    {
        ParseResult r = parse("while (x) y();");
        mbun::ast::NodeIndex n = only_stmt(r);
        if (has_kind(r, n, K::WhileStmt, "while: stmt")) {
            has_kind(r, r.arena.at(n).a, K::Identifier, "while: a=test");
            has_kind(r, r.arena.at(n).b, K::ExpressionStmt, "while: b=body");
        }
    }
    // do body while (test) — s.rs:157 S::DoWhile{body, test_}: a=body, b=test.
    // Pinned because the slot order is the reverse of While and is easy to flip.
    {
        ParseResult r = parse("do y(); while (x);");
        mbun::ast::NodeIndex n = only_stmt(r);
        if (has_kind(r, n, K::DoWhileStmt, "do-while: stmt")) {
            has_kind(r, r.arena.at(n).a, K::ExpressionStmt, "do-while: a=body");
            has_kind(r, r.arena.at(n).b, K::Identifier, "do-while: b=test");
        }
    }
    // switch — s.rs:181 S::Switch{test_, cases}; nodes.rs:739 Case{value, body}.
    // A `default:` clause is a Case with no value (bun: `value: Option<..>`).
    {
        ParseResult r = parse("switch (x) { case 1: a(); break; default: b(); }");
        mbun::ast::NodeIndex n = only_stmt(r);
        if (has_kind(r, n, K::SwitchStmt, "switch: stmt")) {
            has_kind(r, r.arena.at(n).a, K::Identifier, "switch: a=test");
            auto cases = r.arena.list_of(r.arena.at(n));
            check(cases.size() == 2, "switch: 2 cases");
            if (cases.size() == 2) {
                if (has_kind(r, cases[0], K::SwitchCase, "switch: case[0]")) {
                    has_kind(r, r.arena.at(cases[0]).a, K::NumberLiteral, "switch: case[0].a=1");
                    auto body = r.arena.list_of(r.arena.at(cases[0]));
                    check(body.size() == 2, "switch: case[0] body has 2 stmts");
                    if (body.size() == 2) {
                        has_kind(r, body[1], K::BreakStmt, "switch: case[0] body[1]=break");
                    }
                }
                if (has_kind(r, cases[1], K::SwitchCase, "switch: case[1]")) {
                    check(r.arena.at(cases[1]).a == mbun::ast::NONE,
                          "switch: `default:` case has no value");
                    check(r.arena.list_of(r.arena.at(cases[1])).size() == 1,
                          "switch: default body has 1 stmt");
                }
            }
        }
    }
    // try/catch/finally — s.rs:173 S::Try; nodes.rs:727 Catch / :734 Finally.
    {
        ParseResult r = parse("try { a(); } catch (e) { b(); } finally { c(); }");
        mbun::ast::NodeIndex n = only_stmt(r);
        if (has_kind(r, n, K::TryStmt, "try: stmt")) {
            check(r.arena.list_of(r.arena.at(n)).size() == 1, "try: body has 1 stmt");
            if (has_kind(r, r.arena.at(n).b, K::CatchClause, "try: b=catch")) {
                const auto& c = r.arena.at(r.arena.at(n).b);
                // `catch (e)` binds — bun's Catch.binding is a `Binding`, not an
                // expression (nodes.rs:727), so the parser builds the B-tree image.
                has_kind(r, c.a, K::BindingIdentifier, "catch: a=binding");
                check(c.listCount == 1, "catch: body has 1 stmt");
            }
            if (has_kind(r, r.arena.at(n).c, K::FinallyClause, "try: c=finally")) {
                check(r.arena.at(r.arena.at(n).c).listCount == 1, "finally: body has 1 stmt");
            }
        }
    }
    // Optional clauses: catch-less and binding-less forms must leave NONE, not a
    // stale index — bun models both as `Option` (s.rs:177, nodes.rs:729).
    {
        ParseResult r = parse("try { a(); } finally { b(); }");
        mbun::ast::NodeIndex n = only_stmt(r);
        if (has_kind(r, n, K::TryStmt, "try/finally: stmt")) {
            check(r.arena.at(n).b == mbun::ast::NONE, "try/finally: no catch clause");
            has_kind(r, r.arena.at(n).c, K::FinallyClause, "try/finally: c=finally");
        }
    }
    {
        ParseResult r = parse("try { a(); } catch { b(); }");
        mbun::ast::NodeIndex n = only_stmt(r);
        if (has_kind(r, n, K::TryStmt, "try/catch-no-binding: stmt")) {
            check(r.arena.at(n).c == mbun::ast::NONE, "optional catch binding: no finally");
            if (has_kind(r, r.arena.at(n).b, K::CatchClause, "optional catch binding: b=catch")) {
                check(r.arena.at(r.arena.at(n).b).a == mbun::ast::NONE,
                      "optional catch binding: `catch {}` has no binding");
            }
        }
    }
    // label / break / continue — s.rs:67 Label, :304 Break, :309 Continue. An
    // empty `text` is bun's `label: None`.
    {
        ParseResult r = parse("foo: bar();");
        mbun::ast::NodeIndex n = only_stmt(r);
        if (has_kind(r, n, K::LabeledStmt, "label: stmt")) {
            check(r.arena.at(n).text == "foo", "label: text=foo");
            has_kind(r, r.arena.at(n).a, K::ExpressionStmt, "label: a=stmt");
        }
    }
    {
        ParseResult r = parse("outer: while (x) { break outer; continue outer; }");
        mbun::ast::NodeIndex n = only_stmt(r);
        if (has_kind(r, n, K::LabeledStmt, "labelled break/continue: stmt")) {
            check(r.arena.at(n).text == "outer", "labelled break/continue: label text");
            mbun::ast::NodeIndex w = r.arena.at(n).a;
            if (has_kind(r, w, K::WhileStmt, "labelled break/continue: a=while")) {
                auto body = r.arena.list_of(r.arena.at(r.arena.at(w).b));
                check(body.size() == 2, "labelled break/continue: 2 stmts in body");
                if (body.size() == 2) {
                    if (has_kind(r, body[0], K::BreakStmt, "break outer")) {
                        check(r.arena.at(body[0]).text == "outer", "break: text=outer");
                    }
                    if (has_kind(r, body[1], K::ContinueStmt, "continue outer")) {
                        check(r.arena.at(body[1]).text == "outer", "continue: text=outer");
                    }
                }
            }
        }
    }
    {
        ParseResult r = parse("while (x) { break; }");
        mbun::ast::NodeIndex n = only_stmt(r);
        if (has_kind(r, n, K::WhileStmt, "unlabelled break: stmt")) {
            auto body = r.arena.list_of(r.arena.at(r.arena.at(n).b));
            if (body.size() == 1 && has_kind(r, body[0], K::BreakStmt, "unlabelled break")) {
                check(r.arena.at(body[0]).text.empty(), "unlabelled break: empty label");
            }
        }
    }
    // with (value) body — s.rs:167 S::With (sloppy mode)
    {
        ParseResult r = parse("with (o) { x; }");
        mbun::ast::NodeIndex n = only_stmt(r);
        if (has_kind(r, n, K::WithStmt, "with: stmt")) {
            has_kind(r, r.arena.at(n).a, K::Identifier, "with: a=value");
            has_kind(r, r.arena.at(n).b, K::Block, "with: b=body");
        }
    }
    // These all still parse clean (the pre-existing contract).
    ok("switch (x) {}", SRC);
    ok("try { a(); } catch (e: unknown) { b(); }", SRC);  // TS catch annotation
    ok("do { a(); } while (b);", SRC);
    ok("outer: for (;;) { break outer; }", SRC);
}

}  // namespace

// ── binding patterns (destructuring) ─────────────────────────────────────────
//
// The declarator LHS is a binding TREE, not an expression: bun's `B` union
// (src/ast/b.rs:35) reached from `G::Decl.binding` (src/ast/g.rs:20), which the
// printer walks at js_printer/lib.rs:5052 (`print_binding`). These pin the shape
// the parser builds for every form, because the two consumers disagree about
// what they need and both must keep working: the erasure transpiler wants only
// the SET of bound names (collect_export_names_), the printer wants structure.
//
// The flag asymmetry is bun's, deliberately mirrored (ast.cppm:198): an array
// carries `has_spread` at the ARRAY level (b.rs:84, asked as `has_spread && i ==
// len-1` at binding.rs:230), an object tags spread PER PROPERTY (binding.rs:258).
void test_binding_patterns() {
    namespace bf = mbun::ast::bflags;
    using K = mbun::ast::NodeKind;
    using mbun::ast::NodeIndex;
    using mbun::ast::NONE;

    // The binding of `const <pat> = <init>;`.
    auto binding_of = [](const ParseResult& r) -> NodeIndex {
        NodeIndex s = only_stmt(r);
        if (s == NONE || r.arena.at(s).kind != K::VarDecl) {
            return NONE;
        }
        auto decls = r.arena.list_of(r.arena.at(s));
        return decls.size() == 1 ? r.arena.at(decls[0]).a : NONE;
    };
    auto items = [](const ParseResult& r, NodeIndex n) {
        return r.arena.list_of(r.arena.at(n));
    };
    auto text_is = [](const ParseResult& r, NodeIndex n, std::string_view want,
                      std::string_view what) {
        check(n != NONE && r.arena.at(n).text == want,
              std::format("{}: text = \"{}\", expected \"{}\"", what,
                          n == NONE ? "<NONE>" : r.arena.at(n).text, want));
    };

    // A plain name is a BindingIdentifier — ref b.rs B::BIdentifier.
    {
        ParseResult r = parse("const x = 1;");
        NodeIndex b = binding_of(r);
        if (has_kind(r, b, K::BindingIdentifier, "const x: binding")) {
            text_is(r, b, "x", "const x: name");
        }
    }
    // `const {a, b: c, d = 1, ...rest} = o` — every object form at once.
    {
        ParseResult r = parse("const {a, b: c, d = 1, ...rest} = o;");
        NodeIndex b = binding_of(r);
        if (has_kind(r, b, K::BindingObject, "object: binding")) {
            auto ps = items(r, b);
            check(ps.size() == 4, "object: 4 properties");
            if (ps.size() == 4) {
                // `a` — shorthand: key and target are both `a`.
                if (has_kind(r, ps[0], K::BindingProperty, "object[0]: property")) {
                    check((r.arena.at(ps[0]).flags & bf::WasShorthand) != 0,
                          "object[0]: WasShorthand");
                    text_is(r, r.arena.at(ps[0]).a, "a", "object[0]: key");
                    if (has_kind(r, r.arena.at(ps[0]).b, K::BindingIdentifier,
                                 "object[0]: target")) {
                        text_is(r, r.arena.at(ps[0]).b, "a", "object[0]: target name");
                    }
                    check(r.arena.at(ps[0]).c == NONE, "object[0]: no default");
                }
                // `b: c` — a RENAME. `b` is the key, `c` is the only bound name.
                // This is never a type annotation, even in a .ts file.
                if (has_kind(r, ps[1], K::BindingProperty, "object[1]: property")) {
                    check((r.arena.at(ps[1]).flags & bf::WasShorthand) == 0,
                          "object[1]: not shorthand");
                    text_is(r, r.arena.at(ps[1]).a, "b", "object[1]: key");
                    if (has_kind(r, r.arena.at(ps[1]).b, K::BindingIdentifier,
                                 "object[1]: target")) {
                        text_is(r, r.arena.at(ps[1]).b, "c", "object[1]: target name");
                    }
                }
                // `d = 1` — shorthand plus a default. ref b.rs `default_value`.
                if (has_kind(r, ps[2], K::BindingProperty, "object[2]: property")) {
                    check((r.arena.at(ps[2]).flags & bf::WasShorthand) != 0,
                          "object[2]: WasShorthand");
                    text_is(r, r.arena.at(ps[2]).b, "d", "object[2]: target name");
                    has_kind(r, r.arena.at(ps[2]).c, K::NumberLiteral, "object[2]: default");
                }
                // `...rest` — object rest is PER-PROPERTY (binding.rs:258).
                if (has_kind(r, ps[3], K::BindingProperty, "object[3]: property")) {
                    check((r.arena.at(ps[3]).flags & bf::IsSpread) != 0, "object[3]: IsSpread");
                    check(r.arena.at(ps[3]).a == NONE, "object[3]: rest has no key");
                    text_is(r, r.arena.at(ps[3]).b, "rest", "object[3]: target name");
                }
            }
        }
    }
    // `const [x, [y], ...zs] = a` — nesting and array-level spread.
    {
        ParseResult r = parse("const [x, [y], ...zs] = a;");
        NodeIndex b = binding_of(r);
        if (has_kind(r, b, K::BindingArray, "array: binding")) {
            check((r.arena.at(b).flags & bf::HasSpread) != 0, "array: HasSpread");
            auto els = items(r, b);
            check(els.size() == 3, "array: 3 elements");
            if (els.size() == 3) {
                if (has_kind(r, els[0], K::BindingElement, "array[0]: element")) {
                    text_is(r, r.arena.at(els[0]).a, "x", "array[0]: target name");
                    check(r.arena.at(els[0]).b == NONE, "array[0]: no default");
                }
                // A nested pattern binds its leaves, not an intermediate name.
                if (has_kind(r, els[1], K::BindingElement, "array[1]: element")) {
                    NodeIndex inner = r.arena.at(els[1]).a;
                    if (has_kind(r, inner, K::BindingArray, "array[1]: nested array")) {
                        auto ies = items(r, inner);
                        check(ies.size() == 1, "array[1]: nested has 1 element");
                        if (ies.size() == 1) {
                            text_is(r, r.arena.at(ies[0]).a, "y", "array[1]: nested name");
                        }
                    }
                }
                // `has_spread` is array-level; the rest is the LAST element
                // (b.rs:84 / binding.rs:230) — there is no per-element flag.
                if (has_kind(r, els[2], K::BindingElement, "array[2]: element")) {
                    text_is(r, r.arena.at(els[2]).a, "zs", "array[2]: rest name");
                }
            }
        }
    }
    // An array HOLE is B::BMissing (b.rs), not a missing element: `[, x]` binds
    // `x` at index 1.
    {
        ParseResult r = parse("const [, x] = a;");
        NodeIndex b = binding_of(r);
        if (has_kind(r, b, K::BindingArray, "hole: binding")) {
            auto els = items(r, b);
            check(els.size() == 2, "hole: 2 elements");
            if (els.size() == 2) {
                has_kind(r, els[0], K::BindingMissing, "hole[0]: BindingMissing");
                if (has_kind(r, els[1], K::BindingElement, "hole[1]: element")) {
                    text_is(r, r.arena.at(els[1]).a, "x", "hole[1]: target name");
                }
            }
        }
    }
    // A computed key is an EXPRESSION: `k` is read, only `v` binds. ref
    // lib.rs:5148, where the printer re-prints it via print_expr.
    {
        ParseResult r = parse("const {[k]: v} = o;");
        NodeIndex b = binding_of(r);
        if (has_kind(r, b, K::BindingObject, "computed: binding")) {
            auto ps = items(r, b);
            check(ps.size() == 1, "computed: 1 property");
            if (ps.size() == 1) {
                check((r.arena.at(ps[0]).flags & bf::IsComputed) != 0, "computed: IsComputed");
                has_kind(r, r.arena.at(ps[0]).a, K::Identifier, "computed: key is an expr");
                text_is(r, r.arena.at(ps[0]).b, "v", "computed: target name");
            }
        }
    }
    // Literal keys keep bun's model — the key stays an expression node
    // (lib.rs:5163 prints EString/ENumber keys), and `default` is a keyword that
    // is still a perfectly good key.
    {
        ParseResult r = parse("const {\"a-b\": c, 0: d, default: e} = o;");
        NodeIndex b = binding_of(r);
        if (has_kind(r, b, K::BindingObject, "literal keys: binding")) {
            auto ps = items(r, b);
            check(ps.size() == 3, "literal keys: 3 properties");
            if (ps.size() == 3) {
                has_kind(r, r.arena.at(ps[0]).a, K::StringLiteral, "literal keys[0]: string key");
                text_is(r, r.arena.at(ps[0]).b, "c", "literal keys[0]: target");
                has_kind(r, r.arena.at(ps[1]).a, K::NumberLiteral, "literal keys[1]: number key");
                text_is(r, r.arena.at(ps[1]).b, "d", "literal keys[1]: target");
                text_is(r, r.arena.at(ps[2]).a, "default", "literal keys[2]: keyword key");
                text_is(r, r.arena.at(ps[2]).b, "e", "literal keys[2]: target");
            }
        }
    }
    // Deep mixed nesting with a default on a nested pattern — the form whose
    // NamedEvaluation hint must NOT fire (the token before `=` is `}`).
    {
        ParseResult r = parse("const {a: {b} = {}, c: [d = 1]} = o;");
        NodeIndex b = binding_of(r);
        if (has_kind(r, b, K::BindingObject, "deep: binding")) {
            auto ps = items(r, b);
            check(ps.size() == 2, "deep: 2 properties");
            if (ps.size() == 2) {
                has_kind(r, r.arena.at(ps[0]).b, K::BindingObject, "deep[0]: nested object");
                has_kind(r, r.arena.at(ps[0]).c, K::ObjectLiteral,
                         "deep[0]: default on the pattern");
                NodeIndex arr = r.arena.at(ps[1]).b;
                if (has_kind(r, arr, K::BindingArray, "deep[1]: nested array")) {
                    auto els = items(r, arr);
                    check(els.size() == 1, "deep[1]: 1 element");
                    if (els.size() == 1) {
                        text_is(r, r.arena.at(els[0]).a, "d", "deep[1]: target name");
                        has_kind(r, r.arena.at(els[0]).b, K::NumberLiteral, "deep[1]: default");
                    }
                }
            }
        }
    }
    // A binding is a binding wherever it appears: catch clauses and for-of heads
    // build the same tree (bun: Catch.binding is a `Binding`, nodes.rs:727).
    {
        ParseResult r = parse("try { a(); } catch ({message: m}) { b(); }");
        NodeIndex n = only_stmt(r);
        if (has_kind(r, n, K::TryStmt, "catch pattern: stmt")) {
            NodeIndex c = r.arena.at(n).b;
            if (has_kind(r, c, K::CatchClause, "catch pattern: clause")) {
                NodeIndex b = r.arena.at(c).a;
                if (has_kind(r, b, K::BindingObject, "catch pattern: binding")) {
                    auto ps = items(r, b);
                    check(ps.size() == 1, "catch pattern: 1 property");
                    if (ps.size() == 1) {
                        text_is(r, r.arena.at(ps[0]).b, "m", "catch pattern: target name");
                    }
                }
            }
        }
    }
    {
        ParseResult r = parse("for (const [k, v] of m) { f(k, v); }");
        NodeIndex n = only_stmt(r);
        if (has_kind(r, n, K::ForInStmt, "for-of: stmt")) {
            NodeIndex d = r.arena.at(n).a;
            if (has_kind(r, d, K::VarDecl, "for-of: a=decl")) {
                auto decls = r.arena.list_of(r.arena.at(d));
                check(decls.size() == 1, "for-of: 1 declarator");
                if (decls.size() == 1) {
                    NodeIndex b = r.arena.at(decls[0]).a;
                    if (has_kind(r, b, K::BindingArray, "for-of: binding")) {
                        check(items(r, b).size() == 2, "for-of: 2 elements");
                    }
                }
            }
        }
    }
    // An empty pattern is legal and binds nothing.
    {
        ParseResult r = parse("const {} = o;");
        NodeIndex b = binding_of(r);
        if (has_kind(r, b, K::BindingObject, "empty: binding")) {
            check(items(r, b).empty(), "empty: no properties");
        }
    }
}

int main() {
    test_empty_and_smoke();
    test_binding_patterns();
    test_object_property_kinds_and_flags();
    test_control_flow_statements();
    test_private_identifiers();
    test_identifier_escapes();
    test_malformed_enums();
    test_empty_type_parameters();
    test_type_parameter_modifiers();
    test_tuple_labels();
    test_instantiation_resplit();
    test_generic_gt_resplit_in_types();
    test_unterminated_template_after_type_args();
    test_general_parsing_smoke();

    std::println("test_js_parser: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
