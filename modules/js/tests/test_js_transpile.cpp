// test_js_transpile.cpp — T2.4b mbun.js_parser::transpile (TS → JS by erasure).
//
// transpile() is the runtime path that unlocks broad S1 coverage: bun-native
// test files are TS, and JSC evals JavaScript, so every construct that is
// TypeScript-only must be erased (or lowered) before eval or the file fails with
// a SyntaxError before any API is even reached. The strategy mirrors bun/esbuild:
// keep the original JavaScript bytes byte-for-byte and cut ONLY the type-only
// spans (annotations, `interface`/`type`/`declare`, `as`/`satisfies`, non-null
// `!`, type-only imports/exports), lowering `enum` to a JS IIFE.
//
// Each `xp(src, expected)` pins the exact transpiled bytes (whitespace included,
// since erasure does not normalise formatting). `ok(src)` asserts a parse-clean
// transpile without pinning bytes (used for the DEFERRED namespace lowering).
import std;
import mbun.js_parser;

namespace {

using mbun::js_parser::transpile;

int gChecks{0};
int gFailures{0};

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

// The input must transpile without error to exactly `expected`.
void xp(std::string_view src, std::string_view expected) {
    ++gChecks;
    auto r = transpile(src);
    if (!r.ok) {
        ++gFailures;
        std::println("  FAIL transpile(\"{}\") unexpected error \"{}\"", printable(src),
                     printable(r.error));
        return;
    }
    if (r.code != expected) {
        ++gFailures;
        std::println("  FAIL transpile(\"{}\")\n       got: [{}]\n       exp: [{}]", printable(src),
                     printable(r.code), printable(expected));
    }
}

// The input must transpile (CJS mode) without error to exactly `expected`.
void xpc(std::string_view src, std::string_view expected) {
    ++gChecks;
    auto r = transpile(src, {.cjs = true});
    if (!r.ok) {
        ++gFailures;
        std::println("  FAIL cjs transpile(\"{}\") unexpected error \"{}\"", printable(src),
                     printable(r.error));
        return;
    }
    if (r.code != expected) {
        ++gFailures;
        std::println("  FAIL cjs transpile(\"{}\")\n       got: [{}]\n       exp: [{}]",
                     printable(src), printable(r.code), printable(expected));
    }
}

void expect_cjs_esm_mode(std::string_view src, bool expected) {
    ++gChecks;
    const auto r{transpile(src, {.cjs = true})};
    if (!r.ok || r.cjs_esm_module != expected) {
        ++gFailures;
        std::println("  FAIL cjs ESM mode for \"{}\": got {}, expected {}", printable(src),
                     r.cjs_esm_module, expected);
    }
}

// The input (TSX/JSX) must transpile without error to exactly `expected`, under
// `opts` (defaulting to bun's own default JSX config: automatic + development).
void xpx_with(std::string_view src, std::string_view expected,
              mbun::js_parser::detail::JsxOptions opts) {
    ++gChecks;
    auto r = transpile(src, {.jsx = true, .jsx_options = std::move(opts)});
    if (!r.ok) {
        ++gFailures;
        std::println("  FAIL jsx transpile(\"{}\") unexpected error \"{}\"", printable(src),
                     printable(r.error));
        return;
    }
    if (r.code != expected) {
        ++gFailures;
        std::println("  FAIL jsx transpile(\"{}\")\n       got: [{}]\n       exp: [{}]",
                     printable(src), printable(r.code), printable(expected));
    }
}

// Default runtime (automatic + development) — what bun does with no configuration.
void xpx(std::string_view src, std::string_view expected) {
    xpx_with(src, expected, {});
}

// The CLASSIC runtime, selected the way `--jsx-runtime classic` / bunfig's
// `jsx = "react"` select it. The JSX *scanner* is runtime-agnostic, so the
// scanning-semantics cases (whitespace fold, nesting, `<` disambiguation,
// attribute forms) stay on classic where their expectations are most legible —
// the automatic runtime's own shape is covered by test_jsx_automatic.
void xpx_classic(std::string_view src, std::string_view expected) {
    mbun::js_parser::detail::JsxOptions o;
    o.runtime = mbun::js_parser::detail::JsxRuntime::Classic;
    xpx_with(src, expected, std::move(o));
}

// The input must FAIL to transpile with exactly `expected` as the error.
void xpx_err(std::string_view src, std::string_view expected) {
    ++gChecks;
    auto r = transpile(src, {.jsx = true});
    if (r.ok) {
        ++gFailures;
        std::println("  FAIL jsx transpile(\"{}\") expected error \"{}\", got success",
                     printable(src), printable(expected));
        return;
    }
    if (r.error != expected) {
        ++gFailures;
        std::println("  FAIL jsx transpile(\"{}\")\n       got err: [{}]\n       exp err: [{}]",
                     printable(src), printable(r.error), printable(expected));
    }
}

// The input must transpile parse-clean (byte form not pinned).
void ok(std::string_view src) {
    ++gChecks;
    auto r = transpile(src);
    if (!r.ok) {
        ++gFailures;
        std::println("  FAIL transpile(\"{}\") unexpected error \"{}\"", printable(src),
                     printable(r.error));
    }
}

// Transpile (ESM mode) and hand back the code; used by the checks below, which
// pin the placement of individual fragments rather than the whole byte string
// (the module-`using` wrapper embeds source offsets that would make a full-byte
// expectation unreadable).
std::string tr_(std::string_view src) {
    ++gChecks;
    auto r = transpile(src);
    if (!r.ok) {
        ++gFailures;
        std::println("  FAIL transpile(\"{}\") unexpected error \"{}\"", printable(src),
                     printable(r.error));
        return {};
    }
    return r.code;
}

void expect_contains(std::string_view hay, std::string_view needle) {
    ++gChecks;
    if (hay.find(needle) == std::string_view::npos) {
        ++gFailures;
        std::println("  FAIL expected [{}] in [{}]", printable(needle), printable(hay));
    }
}

// `a` must appear, and appear before `b`.
void expect_before(std::string_view hay, std::string_view a, std::string_view b) {
    ++gChecks;
    const auto pa = hay.find(a);
    const auto pb = hay.find(b);
    if (pa == std::string_view::npos || pb == std::string_view::npos || pa >= pb) {
        ++gFailures;
        std::println("  FAIL expected [{}] before [{}] in [{}]", printable(a), printable(b),
                     printable(hay));
    }
}

void expect_eq_count(std::string_view hay, std::string_view needle, int want) {
    ++gChecks;
    int got = 0;
    for (std::size_t p = hay.find(needle); p != std::string_view::npos;
         p = hay.find(needle, p + needle.size())) {
        ++got;
    }
    if (got != want) {
        ++gFailures;
        std::println("  FAIL expected [{}] x{} (got x{}) in [{}]", printable(needle), want, got,
                     printable(hay));
    }
}

void test_annotation_erasure() {
    xp("const x: number = 1;", "const x = 1;");
    xp("let y: string;", "let y;");
    xp("function f(a: number, b: string): boolean { return true; }",
       "function f(a, b) { return true; }");
    xp("const g = (a: number, b: string): number => a + b;", "const g = (a, b) => a + b;");
    xp("class C { x: number = 1; method(a: string): void {} }", "class C { x = 1; method(a) {} }");
    xp("let y = x!;", "let y = x;");
    xp("const v = obj as Foo;", "const v = obj;");
    xp("const w = obj satisfies Bar;", "const w = obj;");
    // definite-assignment assertion on a declarator: `let x!: T`
    xp("let x!: T;", "let x;");
    xp("let a!: A, b!: B;", "let a, b;");
    xp("var socketToClose!: Socket;", "var socketToClose;");
    // TS `this` parameter is type-only — erased entirely from the emitted JS
    xp("function f(this: any, x: number) { return x; }", "function f( x) { return x; }");
    xp("function g(this: unknown) {}", "function g() {}");
    xp("const o = { m(this: unknown, a: number) { return a; } };",
       "const o = { m( a) { return a; } };");
    // plain JS is passed through byte-for-byte
    xp("(a, b) => a + b", "(a, b) => a + b");
    xp("const p = 1 + 2 * 3;", "const p = 1 + 2 * 3;");
}

// A contextual modifier keyword (`get`/`set`/`async`/`static`/`readonly`/…) is only
// a modifier when a member key follows it; otherwise it names the member itself.
// ref: bun src/js_parser/parse/parse_property.rs:350-358.
void test_class_member_named_like_modifier() {
    // `get`/`set` as annotated field names — NOT accessors (elysia hits this)
    xp("class C { get: string; }", "class C { get; }");
    xp("class C { set: string; }", "class C { set; }");
    xp("class C { get: string = \"x\"; }", "class C { get = \"x\"; }");
    xp("class C { get?: string; }", "class C { get; }");
    xp("class C { get!: string; }", "class C { get; }");
    // An initializer-less field is terminated with `;` even when the source has
    // none, matching bun (`class C { get }` → `class C { get; }`, verified against
    // bun 1.3.14). Without it the bare key absorbs whatever follows: stacking
    // annotated fields named `get`/`put` (hono hono-base.ts:104-106) parsed as a
    // getter definition and threw "Expected a parameter list for getter definition".
    xp("class C { get }", "class C { get; }");
    xp("class C { get!: H<E, \"get\">\n  put!: H<E, \"put\"> }",
       "class C { get;\n  put; }");
    xp("class C { get = 1; }", "class C { get = 1; }");
    xp("class C { get(): void {} }", "class C { get() {} }");
    // …and the same names as modifier-shaped fields
    xp("class C { static: number = 1; }", "class C { static = 1; }");
    xp("class C { readonly?: boolean; }", "class C { readonly; }");
    xp("class C { async: number = 1; }", "class C { async = 1; }");
    xp("class C { declare: string; }", "class C { declare; }");
    xp("class C { abstract: string; }", "class C { abstract; }");
    xp("class C { accessor: string; }", "class C { accessor; }");
    // real accessors / modifiers still parse as such
    xp("class C { get x(): number { return 1; } }", "class C { get x() { return 1; } }");
    xp("class C { set x(v: number) {} }", "class C { set x(v) {} }");
    xp("class C { get [k](): number { return 1; } }", "class C { get [k]() { return 1; } }");
    xp("class C { get #p(): number { return 1; } }", "class C { get #p() { return 1; } }");
    xp("class C { static get x(): number { return 1; } }",
       "class C { static get x() { return 1; } }");
    xp("class C { readonly x: number = 1; }", "class C {  x = 1; }");
    xp("class C { async m(): Promise<void> {} }", "class C { async m() {} }");
    xp("class C { async *m(): AsyncGenerator {} }", "class C { async *m() {} }");
    // `get<T>()` is a method named `get` with type parameters, not a getter
    xp("class C { get<T>(k: T): T { return k; } }", "class C { get(k) { return k; } }");
    // the `?`/`!` marker trails the KEY — it precedes type params and the method `(`
    xp("class C { m?(): void {} }", "class C { m() {} }");
    xp("class C { m?<T>(k: T): T { return k; } }", "class C { m(k) { return k; } }");
    xp("class C { protected _h?(...a: unknown[]): unknown; n() {} }", "class C {  n() {} }");
    xp("class C { x?: number = 1; }", "class C { x = 1; }");
    xp("class C { y!: number; }", "class C { y; }");
}

// A TS index signature in a class body (`[key: string]: any`) declares a TYPE, not
// a member: it has no runtime existence, so the whole thing is erased.
// ref: bun src/js_parser/parse/parse_property.rs:294-318 — the TOpenBracket arm's
// "Handle index signatures" block, whose four guards (:303-304) are `token ==
// TColon && was_identifier && opts.is_class` plus `expr.data is EIdentifier`, and
// which ends in `return Ok(None)` (:314) — "Skip this property entirely".
//
// ⚠️ Before this landed, mbun took the plain computed-key path and died on the
// `:` with `Expected "]" but found ":"`, which killed the WHOLE file — this is a
// parse error, so nothing in the module evaluated. That is why one missing branch
// cost itty-router 10 of its 11 files.
//
// Expectations checked against real bun 1.3.14 (`new Bun.Transpiler({loader:"ts"})
// .transformSync(src)`) and read against the 1.4.0 blueprint above; the two AGREE
// on every vector here (the arm is byte-identical in both trees). bun's Transpiler
// re-prints from the AST, so its output is reindented (`class B {\n  status = 2;\n}`)
// where mbun's erasure keeps the original bytes — same erasure decision, and it is
// the decision, not the whitespace, that these vectors pin (see the file header).
void test_class_index_signature() {
    // The itty-router repro, exactly. bun 1.3.14: `export class B {\n  status = 2;\n}`.
    xp("export class B { [key: string]: any\n status: number = 2 }",
       "export class B { \n status = 2 }");
    // ref :311 `expect_or_insert_semicolon` — a trailing `;` belongs to the erased
    // signature, not to the next member.
    xp("class C { [key: string]: any; }", "class C {  }");
    xp("class C { [key: number]: string\n x = 1 }", "class C { \n x = 1 }");
    // A modifier in front is erased with it: `readonly` is consumed by the modifier
    // loop, whose edit is rolled back by truncate_edits and subsumed by the one
    // span deletion. bun 1.3.14: `class C {\n  x = 1;\n}`.
    xp("class C { readonly [k: string]: T\n x = 1 }", "class C { \n x = 1 }");

    // ── the boundaries: a real computed key must NOT be mistaken for one ──────
    // ref :303 `was_identifier` + :304 `EIdentifier` + :317 — every one of these
    // falls through to `p.lexer.expect(TCloseBracket); key = expr`.
    // `[k]: string = 1` — a computed FIELD with a type annotation. The `:` here
    // follows the `]`, not the key, so :303's `token == TColon` is false.
    xp("class C { [k]: string = 1 }", "class C { [k] = 1 }");
    // Not an identifier at all (:303 was_identifier false).
    xp("class C { [\"computed\"]: number = 3 }", "class C { [\"computed\"] = 3 }");
    // Starts with an identifier, but the expression is a Member, not an Identifier
    // — this is :304's `_ => {}` arm. Also the reason the check cannot just be
    // "the first token was an identifier".
    xp("class C { [Symbol.iterator]() {} }", "class C { [Symbol.iterator]() {} }");
    // ref :303 `opts.is_class` — an OBJECT literal never takes this branch. mbun
    // gets this structurally: parse_object_property_ is a different function.
    xp("const o = {[k]: v};", "const o = {[k]: v};");
    // Mixed: signature erased, computed key next to it survives.
    xp("class C { [key: number]: string\n [k2]: any = 1 }", "class C { \n [k2] = 1 }");
}

// A class member with no body is a TS overload signature or an `abstract` method —
// type-only, so the whole member is erased.
// ref: bun src/js_parser/parse/parse_property.rs:123-127 (IsForwardDeclaration).
void test_class_member_without_body() {
    xp("class C { m(): void; m(x: number): void; m(x?: number): void {} }",
       "class C {   m(x) {} }");
    xp("class C { abstract m(): void; }", "class C {  }");
    xp("class C { abstract m(): void; n() {} }", "class C {  n() {} }");
    xp("class C { public static m(a: string): void; m(a: any) {} }", "class C {  m(a) {} }");
    xp("class C { constructor(x: string); constructor(x: number); constructor(x: any) {} }",
       "class C {   constructor(x) {} }");
    xp("class C { abstract get x(): number; }", "class C {  }");
    // ASI form: no trailing `;` on the signature
    xp("class C { m(): void\n m() {} }", "class C { \n m() {} }");
    // a body-less member does not swallow the member after it
    xp("class C { abstract m(): void; y: number = 1; }", "class C {  y = 1; }");
}


void test_async_functions() {
    // async arrows (async is not reserved — only special before fn/params/param)
    xp("const f = async () => 1;", "const f = async () => 1;");
    xp("test('x', async () => { await y; });", "test('x', async () => { await y; });");
    xp("const g = async (a: number): Promise<void> => { await a; };",
       "const g = async (a) => { await a; };");
    xp("const h = async x => x + 1;", "const h = async x => x + 1;");
    xp("const e = async function () { return 1; };", "const e = async function () { return 1; };");
    // `async` used as a plain call / identifier must NOT be read as an arrow
    xp("foo(async(1));", "foo(async(1));");
    xp("const a = async;", "const a = async;");
    xp("async(x);", "async(x);");
}

void test_statements() {
    // control-flow statements the transpiler must parse (were unsupported → files
    // fell back to raw ESM and failed under script-mode eval)
    xp("try { f(); } catch (e) { g(); } finally { h(); }",
       "try { f(); } catch (e) { g(); } finally { h(); }");
    xp("try { f(); } catch (e: unknown) { g(); }", "try { f(); } catch (e) { g(); }");
    xp("try { x(); } catch {}", "try { x(); } catch {}");
    xp("while (x) { y(); }", "while (x) { y(); }");
    xp("do { y(); } while (x);", "do { y(); } while (x);");
    xp("switch (x) { case 1: a(); break; default: b(); }",
       "switch (x) { case 1: a(); break; default: b(); }");
    xp("for (;;) { if (x) break; else continue; }", "for (;;) { if (x) break; else continue; }");
    xp("outer: for (const x of y) { break outer; }", "outer: for (const x of y) { break outer; }");
    // type erasure still applies inside these bodies
    xp("while (x) { const a: number = 1; }", "while (x) { const a = 1; }");
}

void test_await() {
    // await as a unary operator (was only accidentally OK at statement level;
    // failed inside call args / as an operand)
    xp("expect(await proc.exited).toBe(0);", "expect(await proc.exited).toBe(0);");
    xp("const r = await Bun.file(p).text();", "const r = await Bun.file(p).text();");
    xp("await import(dir + '/x.json');", "await import(dir + '/x.json');");
    xp("const b = await new C(src).resize(w, h);", "const b = await new C(src).resize(w, h);");
    xp("foo(await bar(), baz);", "foo(await bar(), baz);");
    // await used as an identifier / member name must NOT be treated as operator
    xp("const await = 1;", "const await = 1;");
    xp("obj.await;", "obj.await;");
}

void test_using_lowering() {
    // `using`/`await using` → __mbun_using + try/finally __mbun_dispose (the
    // prebuilt JSC lacks native support). Exact lowering for a single-using block:
    xp("{ using r = open(); }",
       "{ const __mbun_env0 = { stack: [], error: void 0, hasError: false }; try { "
       "const r = __mbun_using(__mbun_env0, open(), false);  } catch (__mbun_e0) { "
       "__mbun_env0.error = __mbun_e0; __mbun_env0.hasError = true; } finally { "
       "__mbun_dispose(__mbun_env0); } }");
    // type annotation erased; async dispose uses await; multiple usings share env
    ok("{ using r: Res = open(); }");
    ok("async () => { await using p = spawn(); await p.exited; }");
    ok("{ using a = x(); using b = y(); f(a, b); }");
    // comma-separated declarators — each wraps its own initializer. A top-level
    // `using` wraps the whole module body (env id is one past the last source
    // byte) and binds with `var`, so the trailing `export {}` clause a module can
    // carry still sees it. ref: bun src/js_parser/p.rs:9287-9293.
    xp("using u1 = a1, u2 = a2;",
       " const __mbun_env23 = { stack: [], error: void 0, hasError: false }; try {"
       "var u1 = __mbun_using(__mbun_env23, a1, false), "
       "u2 = __mbun_using(__mbun_env23, a2, false); } catch (__mbun_e23) { "
       "__mbun_env23.error = __mbun_e23; __mbun_env23.hasError = true; } finally { "
       "__mbun_dispose(__mbun_env23); }\n");
    xp("await using v1 = a1, v2 = a2;",
       " const __mbun_env29 = { stack: [], error: void 0, hasError: false }; try {"
       "var v1 = __mbun_using(__mbun_env29, a1, true), "
       "v2 = __mbun_using(__mbun_env29, a2, true); } catch (__mbun_e29) { "
       "__mbun_env29.error = __mbun_e29; __mbun_env29.hasError = true; } finally { "
       "await __mbun_disposeAsync(__mbun_env29); }\n");
    // `using` as a plain identifier is untouched
    xp("const using = 1;", "const using = 1;");
    xp("using();", "using();");
}

// A top-level `using` moves the module body into a try/catch, so statements that
// may not appear inside a block have to be hoisted back out around it and
// exported bindings republished after it.
// ref: bun src/js_parser/p.rs:9297 LowerUsingDeclarationsContext::finalize.
void test_top_level_using_esm() {
    // `import` is illegal inside a block → hoisted ahead of the try, and must not
    // be left behind at its original offset.
    const std::string imp{tr_("import x from \"m\";\nusing r = open();")};
    expect_contains(imp, "import x from \"m\";");
    expect_before(imp, "import x from \"m\";", "try {");
    expect_eq_count(imp, "import x", 1);

    // Re-exports are equally block-illegal.
    const std::string star{tr_("export * from \"m\";\nusing r = open();")};
    expect_before(star, "export * from \"m\";", "try {");
    const std::string reexp{tr_("export { a } from \"m\";\nusing r = open();")};
    expect_before(reexp, "export { a } from \"m\";", "try {");

    // `export const` → `var` inside the try + republished after it, so the
    // binding survives the move into the block. ref: bun p.rs:6175 / p.rs:9350.
    const std::string ec{tr_("using r = open();\nexport const a = 1, b = 2;")};
    expect_contains(ec, "var a = 1, b = 2;");
    expect_contains(ec, "export { a, b };");
    expect_before(ec, "finally", "export { a, b };");

    // A plain `export {}` clause moves behind the try; the binding it names is a
    // top-level `const`, which must have been demoted to `var` to stay visible.
    const std::string cl{tr_("using r = open();\nconst y = 1;\nexport { y };")};
    expect_contains(cl, "var y = 1;");
    expect_before(cl, "finally", "export { y };");

    // A function declaration is block-scoped in strict mode → hoisted out.
    // ref: bun visit/mod.rs:1482 (should_hoist_fns == parent_is_none).
    const std::string fn{tr_("using r = open();\nfunction f() {}\nf();")};
    expect_before(fn, "function f() {}", "try {");

    // `export default <expr>` keeps its initializer inside the try (it may read a
    // `using` binding) and republishes through the clause.
    // ref: bun visit/visit_stmt.rs:511-529.
    const std::string def{tr_("using r = open();\nexport default r.value;")};
    expect_contains(def, "= r.value;");
    expect_contains(def, " as default };");
    expect_before(def, "finally", " as default };");

    // A `using` in a nested block still lowers against that block, leaving the
    // module body alone.
    const std::string nested{tr_("import x from \"m\";\n{ using r = open(); }")};
    expect_eq_count(nested, "try {", 1);
    expect_before(nested, "import x from \"m\";", "try {");
}

void test_regex() {
    // regex literals (were mis-lexed as division → `\d` broke the lexer)
    xp("const re = /\\d+/g;", "const re = /\\d+/g;");
    xp("x.replace(/\\w/, '');", "x.replace(/\\w/, '');");
    xp("if (/\\d/.test(x)) {}", "if (/\\d/.test(x)) {}");
    xp("const b = y ? /\\d/ : /\\w/;", "const b = y ? /\\d/ : /\\w/;");
    xp("return /\\d/;", "return /\\d/;");
    xp("foo(/[a-z]+/gi, x);", "foo(/[a-z]+/gi, x);");
    xp("const re2 = /a\\/b/;", "const re2 = /a\\/b/;");  // escaped slash in body
    // division must still parse as division, not regex
    xp("const d = a / b / c;", "const d = a / b / c;");
    xp("const e = (a + b) / 2;", "const e = (a + b) / 2;");
    xp("foo(/[a-z]/g, x / y);", "foo(/[a-z]/g, x / y);");  // regex arg then division
}

void test_ts_type_features() {
    // TS import-type query
    xp("function f(e: typeof import(\"bun:test\").expect) {}", "function f(e) {}");
    xp("let x: import(\"m\").Foo<number>;", "let x;");
    // type predicate / assertion signature
    xp("const f = (s): s is string => s !== null;", "const f = (s) => s !== null;");
    xp("function g(x): asserts x is Foo {}", "function g(x) {}");
    xp("function h(x): asserts x {}", "function h(x) {}");
    // type operators keyof / readonly / infer / unique
    xp("function b(k: keyof typeof obj) {}", "function b(k) {}");
    xp("let r: readonly string[];", "let r;");
    xp("type E<T> = T extends (infer U)[] ? U : never;", "");
    xp("let s: unique symbol;", "let s;");
    // constructor type `new (…) => T`, and its `abstract` form (TS 4.2)
    xp("let c: new () => Foo;", "let c;");
    xp("let a: abstract new (x: number) => Foo;", "let a;");
    xp("type F = abstract new <T>(x: T) => T;", "");
    // `abstract` with no `new` following is an ordinary type reference name
    xp("let n: abstract;", "let n;");
    xp("let u: abstract | null;", "let u;");
    // old-style angle-bracket type assertion `<Type>expr` (non-JSX)
    xp("const q = <Foo>bar;", "const q = bar;");
    xp("const p = <URL>await f();", "const p = await f();");
    xp("const n = <number>x + 1;", "const n = x + 1;");
    xp("const m = <Foo>bar.baz;", "const m = bar.baz;");
    // the cast's `<…>` holds an arbitrary TYPE, not just a type name
    xp("const a = <T[]>[];", "const a = [];");
    xp("const b = <(number | undefined)[]>[];", "const b = [];");
    xp("const c = <Foo<Bar>[]>[];", "const c = [];");
    xp("const d = <readonly string[]>x;", "const d = x;");
    xp("const e = <{ a: number }>x;", "const e = x;");
    xp("const g = <Foo>(bar);", "const g = (bar);");
    // A parenthesized `as` in a ternary must not be mistaken for an arrow's parameter
    // list: the `:` arm looks like a return annotation, and without a `;` to stop it a
    // token scan reaches an unrelated `=>` further on and commits to the arrow.
    xp("const a = c ? (g as any) : (h as any)\nconst f = () => 1\n",
       "const a = c ? (g) : (h)\nconst f = () => 1\n");
    xp("const b =\n\tcond\n\t\t? (v as Foo<any>)\n\t\t: make(v, { s: () => 1 })\n",
       "const b =\n\tcond\n\t\t? (v)\n\t\t: make(v, { s: () => 1 })\n");
    xp("const d = typeof (g as any).next === 'f' ? (g as It<unknown>) : (g as any)[S]()\nconst z = "
       "() => 2\n",
       "const d = typeof (g).next === 'f' ? (g) : (g)[S]()\nconst z = () => 2\n");
    // real arrows with return annotations still parse as arrows
    xp("const e = (x: number): Foo<Bar> => x;", "const e = (x) => x;");
    xp("const h2 = (x): { a: number } => x;", "const h2 = (x) => x;");
    xp("const i = async (x: T): Promise<void> => {};", "const i = async (x) => {};");
    // A return type that is itself a function type: only the FUNCTION TYPE's `=>` is
    // part of the type. A parenthesized type must not consume the arrow that follows
    // it — that arrow belongs to the arrow function being annotated.
    xp("const a = (x: number): ((r: string) => number) => 1;", "const a = (x) => 1;");
    xp("const b = (x: number): ((r: string) => number) => {\n\treturn (r) => 1\n}",
       "const b = (x) => {\n\treturn (r) => 1\n}");
    xp("const c = (): (() => void) => () => {};", "const c = () => () => {};");
    xp("const d2 = (): Array<() => void> => [];", "const d2 = () => [];");
    xp("const e2 = (): ((a: A, b?: B) => C) => f;", "const e2 = () => f;");
    xp("const f3 = (): ((...r: T[]) => void) => f;", "const f3 = () => f;");
    xp("const g3 = (): (([a, b]: T, { c }: U) => void) => f;", "const g3 = () => f;");
    // an unparenthesized function-type return annotation keeps its own `=>`
    xp("let h3: (r: string) => number;", "let h3;");
    xp("let i3: () => void;", "let i3;");
    xp("let j3: (a: (b: C) => D) => E;", "let j3;");
    // a parenthesized (non-function) type is still just a type
    xp("let k: (number | string);", "let k;");
    xp("let l: (A);", "let l;");
    // tuple elements: plain, optional, labelled, optional-labelled, rest
    xp("let m: [A?];", "let m;");
    xp("let n2: [A, B?];", "let n2;");
    xp("let o2: [a: A, b?: B];", "let o2;");
    xp("let p2: [A, ...(B extends C ? [D?] : [])];", "let p2;");
    xp("let q2: [...A[]];", "let q2;");
    // must NOT be confused with generic arrow functions (same `<...>` start)
    xp("const f1 = <T>(x: T): T => x;", "const f1 = (x) => x;");
    xp("const f2 = <T extends U>(x: T) => x;", "const f2 = (x) => x;");
    xp("const f3 = <T,>(x: T) => x;", "const f3 = (x) => x;");
    xp("const f4 = <T, K>(x: T, y: K) => x;", "const f4 = (x, y) => x;");
    // …and the *async* generic arrow, which takes the same `<…>` head after
    // `async`. (hono utils/concurrent.ts:29 `const run = async <T>(fn: () => T,
    // …): Promise<T> => {…}`.) ref: bun parse/mod.rs:1639-1665.
    xp("const g1 = async <T>(x: T): Promise<T> => x;", "const g1 = async (x) => x;");
    xp("const g2 = async <T extends U>(x: T) => x;", "const g2 = async (x) => x;");
    xp("const g3 = async <T,>(x: T) => x;", "const g3 = async (x) => x;");
    xp("const g4 = async <T, K>(x: T, y: K) => x;", "const g4 = async (x, y) => x;");
    xp("const g5 = async <T>(fn: () => T, p?: P<T>): Promise<T> => fn();",
       "const g5 = async (fn, p) => fn();");
    // `async<T>(x)` with no `=>` is a *call* with explicit type arguments, not an
    // arrow — the speculation must not consume it.
    xp("const c1 = async<number>(x);", "const c1 = async(x);");
    // …and a bare `async` followed by `<` is a comparison.
    xp("const c2 = async < b;", "const c2 = async < b;");
}

void test_type_only_declarations() {
    xp("interface Foo { a: number; }", "");
    xp("type T = number | string;", "");
    xp("declare const z: number;", "");
    xp("export interface I { a: number; }", "");
    xp("export type Alias = number;", "");
}

void test_enum_lowering() {
    xp("enum E { A, B, C }",
       "var E; (function (E) { E[E[\"A\"] = 0] = \"A\"; E[E[\"B\"] = 1] = \"B\"; "
       "E[E[\"C\"] = 2] = \"C\"; })(E || (E = {}));");
    xp("enum Dir { Up = 1, Down, Left = 10, Right }",
       "var Dir; (function (Dir) { Dir[Dir[\"Up\"] = 1] = \"Up\"; Dir[Dir[\"Down\"] = 2] = \"Down\"; "
       "Dir[Dir[\"Left\"] = 10] = \"Left\"; Dir[Dir[\"Right\"] = 11] = \"Right\"; })(Dir || (Dir = {}));");
    xp("enum S { A = \"a\", B = \"b\" }",
       "var S; (function (S) { S[\"A\"] = \"a\"; S[\"B\"] = \"b\"; })(S || (S = {}));");
    xp("const enum CE { X, Y }",
       "var CE; (function (CE) { CE[CE[\"X\"] = 0] = \"X\"; CE[CE[\"Y\"] = 1] = \"Y\"; })(CE || (CE = {}));");

    // A TS enum MERGES with another enum, but redeclaring one over a
    // function/class/let/const/var binding of the same name is an error rather
    // than two silently-emitted IIFEs for one binding.
    // ref: compat/bun/test/bundler/transpiler/ts-enum-redecl-panic.test.ts
    {
        auto err = [](std::string_view src) {
            ++gChecks;
            auto r = transpile(src, {});
            if (r.ok || !r.error.contains("has already been declared")) {
                ++gFailures;
                std::println("  FAIL enum redecl(\"{}\") expected \"has already been declared\", "
                             "got ok={} err=[{}]",
                             printable(src), r.ok, printable(r.error));
            }
        };
        err("function X() {}\nenum X {}\nenum X {}\n");
        err("class X {}\nenum X {}\nenum X {}\n");
        err("let X = 1;\nenum X {}\nenum X {}\n");
        err("const X = 1;\nenum X {}\nenum X {}\n");
        err("function X() {}\nenum X {}\nenum X {}\nenum X {}\n");
        err("function Reflect() {} // only)\r\nenum Reflect {} // collision\r\n"
            "enum Reflect {} // collision\r\n");
    }
    // enum-over-enum merges, and a nested binding of the same name is a
    // DIFFERENT scope, so neither is an error.
    ok("enum M { A }\nenum M { B }\n");
    ok("enum N { A }\nfunction f() { class N {} }\n");
}

void test_imports() {
    // runtime imports kept verbatim (module linking is a separate step)
    xp("import { test } from \"bun:test\";", "import { test } from \"bun:test\";");
    xp("import { test, expect, describe } from \"bun:test\";",
       "import { test, expect, describe } from \"bun:test\";");
    xp("import Foo from \"./foo\";", "import Foo from \"./foo\";");
    xp("import * as ns from \"./m\";", "import * as ns from \"./m\";");
    xp("import \"./side-effect\";", "import \"./side-effect\";");
    // type-only imports erased; inline `type` specifiers stripped
    xp("import type { Foo } from \"./foo\";", "");
    xp("import { type A, B } from \"./m\";", "import {  B } from \"./m\";");
    // `import X = require(...)` lowers to const
    xp("import fs = require(\"fs\", undefined, 1);", "const fs = require(\"fs\", undefined, 1);");
    // import attributes / assertions kept verbatim in ESM mode
    xp("import x from \"m\" with { type: \"text\" };",
       "import x from \"m\" with { type: \"text\" };");
    xp("import \"m\" with { type: \"json\" };", "import \"m\" with { type: \"json\" };");
    xp("import cfg from \"./c.json\" assert { type: \"json\" };",
       "import cfg from \"./c.json\" assert { type: \"json\" };");
    // `default` (a reserved word) is a valid imported/exported specifier name
    xp("import { default as d, fn } from \"./m\";", "import { default as d, fn } from \"./m\";");
    xp("export { foo as default };", "export { foo as default };");
}

void test_exports() {
    xp("export function h() {}", "export function h() {}");
    xp("export const k: number = 2;", "export const k = 2;");
    xp("export default function () { return 1; }", "export default function () { return 1; }");
    xp("export default 42;", "export default 42;");
    xp("export { a, b };", "export { a, b };");
    xp("export { a, b } from \"./m\";", "export { a, b } from \"./m\";");
    xp("export * from \"./m\";", "export * from \"./m\";");
    // type-only exports erased; inline `type` specifiers stripped
    xp("export { type A, B };", "export {  B };");
    xp("export type { Foo };", "");
    xp("export type { Foo } from \"./m\";", "");
    xp("export async function af() {}", "export async function af() {}");
}

// Every ESM export of a module-scope binding — the `export { X as Y }` clause and
// the `export var/let/const/function/class` declaration alike — lowers to a live
// getter per spec: a not-yet-initialised X is read on access rather than in its
// TDZ, and a later write to X is still observed. One `__mbun_X` per module, in
// the prelude, and only when some export actually lowered to a call.
static const std::string kLiveHelper{
    " var __mbun_X = (k, g) => { const d = __mbun_O.getOwnPropertyDescriptor(exports, k);"
    " if (d && d.get && d.get.__mbun_subs) { exports[k] = g(); return; } "
    "__mbun_O.defineProperty(exports, k, { get: g, set: (v) => __mbun_O.defineProperty("
    "exports, k, { value: v, writable: true, enumerable: true, configurable: true }),"
    " enumerable: true, configurable: true }); };"
    " var __mbun_XP = (k, g) => { const d = __mbun_O.getOwnPropertyDescriptor(exports, k);"
    " if (d && d.get && d.get.__mbun_subs) { exports[k] = g(); } };"};

// Every live export is re-pushed once the module body has run, so a cyclic
// importer's pushed binding ends up with the final value rather than whatever
// the declaration held. `pairs` is "key, local" per export, in emission order.
static std::string tail(std::initializer_list<std::pair<const char*, const char*>> pairs) {
    std::string t{"\n"};
    for (const auto& [k, v] : pairs) {
        t += std::string{"__mbun_XP(\""} + k + "\", () => " + v + ");";
    }
    return t;
}

void test_namespace_lowering() {
    // TS namespace → IIFE; exported members become `N.x = x`.
    xp("namespace N { export function f() { return 1; } export const x = 2; }",
       "var N; (function (N) { function f() { return 1; } N.f = f; const x = 2; N.x = x; })"
       "(N || (N = {}));");
    // non-exported members stay local to the IIFE
    xp("namespace N { const priv = 1; export const pub = priv; }",
       "var N; (function (N) { const priv = 1; const pub = priv; N.pub = pub; })(N || (N = {}));");
    // ASI inside a namespace: `N.x = x` must not fuse onto the initializer
    xp("namespace N { export const x = 1 }",
       "var N; (function (N) { const x = 1; N.x = x; })(N || (N = {}));");
    // nested namespaces
    ok("namespace A { export namespace B { export const v = 1; } }");
    // `export namespace` also exports the namespace object at module level (cjs)
    // The namespace object is filled by the IIFE that runs *after* the `var S`
    // the getter closes over, which is the whole point of the live binding.
    xpc("export namespace S { export const x = 1; }",
        "var __mbun_O = ({}).constructor; __mbun_O.defineProperty(exports, \"__esModule\", { value: true });" + kLiveHelper + "\n"
        "var S; (function (S) { const x = 1; S.x = x; })(S || (S = {})); __mbun_X(\"S\", () => S);" +
            tail({{"S", "S"}}));
}

// CJS lowering (transpile with {.cjs=true}) — ESM import/export → require/exports.
void test_cjs_imports() {
    expect_cjs_esm_mode("import { test } from \"bun:test\";", true);
    expect_cjs_esm_mode("export type T = number;", true);
    expect_cjs_esm_mode("module.exports = 1;", false);
    expect_cjs_esm_mode("const p = import(\"./m\");", false);
    // Named imports are ESM *live bindings*: `let` + a __mbun_link subscription,
    // not a `const {…}` destructure, so a cyclic import observes the exporter's
    // later `exports.x = …` instead of snapshotting undefined forever.
    xpc("import { test, expect } from \"bun:test\";",
        "const __mbun_i0 = require(\"bun:test\", undefined, 1); let test = __mbun_i0.test, expect = "
        "__mbun_i0.expect; __mbun_link(__mbun_i0, \"test\", (__mbun_v) => test = __mbun_v); "
        "__mbun_link(__mbun_i0, \"expect\", (__mbun_v) => expect = __mbun_v);");
    xpc("import foo from \"./foo\";",
        "const __mbun_i0 = require(\"./foo\", undefined, 1); const foo = __mbun_i0 && __mbun_i0.__esModule && \"default\" in __mbun_i0 ? "
        "__mbun_i0.default : __mbun_i0;");
    // `import * as ns from "<cjs>"` exposes the CJS `default` binding, i.e.
    // module.exports itself, matching node/bun ESM-CJS interop. It is defined
    // non-enumerably (no `enumerable: true` below), so require()'s view,
    // Object.keys and the __esModule interop are all unchanged -- and it is
    // skipped when the module already has a `default` or is non-extensible.
    // Behaviour change landed in 7332af2 and measured at bun js/bun 0/17 -> 2/17
    // green; this golden was left stale by that commit, which is why
    // `mcpp test -p modules/js` had been failing.
    xpc("import * as ns from \"./m\";",
        "const __mbun_i0 = require(\"./m\", undefined, 1); const ns = __mbun_i0 && "
        "(typeof __mbun_i0 === \"object\" || typeof __mbun_i0 === \"function\") && "
        "Object.isExtensible(__mbun_i0) && !(\"default\" in __mbun_i0) ? "
        "(Object.defineProperty(__mbun_i0, \"default\", { value: __mbun_i0, writable: true, "
        "configurable: true }), __mbun_i0) : __mbun_i0;");
    xpc("import def, { a, b as c } from \"./m\";",
        "const __mbun_i0 = require(\"./m\", undefined, 1); const def = __mbun_i0 && __mbun_i0.__esModule && \"default\" in __mbun_i0 ? "
        "__mbun_i0.default : __mbun_i0; let a = __mbun_i0.a, c = __mbun_i0.b; "
        "__mbun_link(__mbun_i0, \"a\", (__mbun_v) => a = __mbun_v); __mbun_link(__mbun_i0, \"b\", (__mbun_v) => c = __mbun_v);");
    xpc("import \"./side\";", "require(\"./side\", undefined, 1);");
    // inline type specifier stripped from the CJS lowering
    xpc("import { type T, keep } from \"./m\";",
        "const __mbun_i0 = require(\"./m\", undefined, 1); let keep = __mbun_i0.keep; "
        "__mbun_link(__mbun_i0, \"keep\", (__mbun_v) => keep = __mbun_v);");
    // type-only import fully erased
    xpc("import type { X } from \"./m\";", "");
    // import.meta lowers to the injected global (script-mode eval has no meta)
    xpc("const u = import.meta.url;", "const u = __mbunImportMeta.url;");
    xpc("if (import.meta.main) foo();", "if (__mbunImportMeta.main) foo();");
    // dynamic import() is left as-is (valid in script mode)
    xpc("const m = globalThis.__mbun_dyn_import(require,\"./x\");", "const m = globalThis.__mbun_dyn_import(require,\"./x\");");
    // The import attributes clause is consumed, and the `type` attribute is
    // CARRIED INTO the lowered require() — it selects the loader, so dropping it
    // silently changed what the import evaluates to (`import css from "./ui.css"
    // with { type: "text" }` handed the CSS to the JS lexer: "Invalid character:
    // '@'"). ref .mbun/bun-ref/src/bundler/options.rs:600-604 — a `type` attribute
    // replaces the extension-derived loader.
    xpc("import x from \"m\" with { type: \"text\" };",
        "const __mbun_i0 = require(\"m\", { type: \"text\" }, 1); const x = __mbun_i0 && "
        "__mbun_i0.__esModule && \"default\" in __mbun_i0 ? __mbun_i0.default : __mbun_i0;");
    xpc("import \"m\" with { type: \"json\" };", "require(\"m\", { type: \"json\" }, 1);");
    // The legacy `assert { … }` spelling carries the attribute the same way (it is
    // what openauth's src/ui/base.tsx uses).
    xpc("import css from \"./ui.css\" assert { type: \"text\" };",
        "const __mbun_i0 = require(\"./ui.css\", { type: \"text\" }, 1); const css = __mbun_i0 && "
        "__mbun_i0.__esModule && \"default\" in __mbun_i0 ? __mbun_i0.default : __mbun_i0;");
    // A non-`type` attribute is inert: consumed, never forwarded.
    xpc("import x from \"m\" with { foo: \"bar\" };",
        "const __mbun_i0 = require(\"m\", undefined, 1); const x = __mbun_i0 && __mbun_i0.__esModule && \"default\" in __mbun_i0 ? "
        "__mbun_i0.default : __mbun_i0;");
    // reserved-word specifier name is a valid member/key on both sides
    xpc("import { default as d, fn } from \"./m\";",
        "const __mbun_i0 = require(\"./m\", undefined, 1); let d = __mbun_i0.default, fn = __mbun_i0.fn; "
        "__mbun_link(__mbun_i0, \"default\", (__mbun_v) => d = __mbun_v); "
        "__mbun_link(__mbun_i0, \"fn\", (__mbun_v) => fn = __mbun_v);");
    // a module-export *string* name can't use dot access on either side
    xpc("import { \"a-b\" as ab } from \"./m\";",
        "const __mbun_i0 = require(\"./m\", undefined, 1); let ab = __mbun_i0[\"a-b\"]; "
        "__mbun_link(__mbun_i0, \"a-b\", (__mbun_v) => ab = __mbun_v);");
}


void test_cjs_exports() {
    const std::string base{"var __mbun_O = ({}).constructor; __mbun_O.defineProperty(exports, \"__esModule\", { value: true });"};
    const std::string esm{base + "\n"};                 // no live export in the module
    const std::string esmX{base + kLiveHelper + "\n"};  // …plus the __mbun_X helper
    xpc("export const x = 1;", esmX + "const x = 1; __mbun_X(\"x\", () => x);" + tail({{"x", "x"}}));
    xpc("export const a = 1, b = 2;",
        esmX + "const a = 1, b = 2; __mbun_X(\"a\", () => a); __mbun_X(\"b\", () => b);" +
            tail({{"a", "a"}, {"b", "b"}}));
    xpc("export function f() {}",
        esmX + "function f() {} __mbun_X(\"f\", () => f);" + tail({{"f", "f"}}));
    xpc("export class C {}", esmX + "class C {} __mbun_X(\"C\", () => C);" + tail({{"C", "C"}}));
    xpc("export { a, b as c };",
        esmX + " __mbun_X(\"a\", () => a); __mbun_X(\"c\", () => b);" +
            tail({{"a", "a"}, {"c", "b"}}));
    // A clause names a *binding*, so it may legally precede that binding's `const`
    // declaration. Assigning here would read it in its TDZ; the getter defers the
    // read to access time. (hono utils/mime.ts: `export { baseMimes }` at :28,
    // `const baseMimes` at :94.) Verified against bun 1.3.14.
    xpc("export { later as alias };\nconst later = 1;",
        esmX + " __mbun_X(\"alias\", () => later);\nconst later = 1;" + tail({{"alias", "later"}}));
    // A module-export string keeps its quotes and stays a property key.
    xpc("export { a as \"a-b\" };", esmX + " __mbun_X(\"a-b\", () => a);" + tail({{"a-b", "a"}}));
    // `export {}` declares nothing, so it lowers to nothing (no stray helper).
    xpc("export {};", esm);
    xpc("export { a } from \"./m\";", esm + "const __mbun_e0 = require(\"./m\"); exports.a = __mbun_e0.a;");
    xpc("export default 42;", esm + "exports.default = 42;");
    // A *named* default-exported function/class declaration also binds that name in
    // module scope (ESM spec: the HoistableDeclaration/ClassDeclaration keeps its
    // BindingIdentifier), so it must stay a declaration — lowering it to
    // `exports.default = function g() {}` scopes `g` to the expression itself and a
    // later `export { g }` throws ReferenceError. Verified against bun 1.3.14 and
    // node: `export default function g(){}; export { g }` → `default === g`.
    xpc("export default function g() {}", esm + "function g() {} exports.default = g;");
    xpc("export default class C {}", esm + "class C {} exports.default = C;");
    xpc("export default async function g() {}", esm + "async function g() {} exports.default = g;");
    xpc("export default function* g() {}", esm + "function* g() {} exports.default = g;");
    // (erasing the `abstract` modifier leaves its separating space — erasure keeps
    // the surrounding bytes, and the gap is semantically irrelevant)
    xpc("export default abstract class C {}", esm + " class C {} exports.default = C;");
    xpc("export default class C extends B {}", esm + "class C extends B {} exports.default = C;");
    // …but an *anonymous* default export declares nothing, so it stays an expression
    // (`class {}` / `function () {}` are not legal declarations).
    xpc("export default function () {}", esm + "exports.default = function () {}");
    xpc("export default class {}", esm + "exports.default = class {}");
    xpc("export default class extends B {}", esm + "exports.default = class extends B {}");
    // the module-scope binding a named default export declares is referenceable, so
    // `export { g }` resolves it rather than throwing (the elysia `export default
    // class Elysia` + `export { Elysia }` shape).
    xpc("export default class C {}\nexport { C }",
        esmX + "class C {} exports.default = C;\n __mbun_X(\"C\", () => C);" + tail({{"C", "C"}}));
    xpc("export * from \"./m\";", esm + "__mbun_O.assign(exports, require(\"./m\"));");
    // enum lowering composes with export
    xpc("export enum E { A, B }",
        esmX + "var E; (function (E) { E[E[\"A\"] = 0] = \"A\"; E[E[\"B\"] = 1] = \"B\"; })(E || (E = {})); __mbun_X(\"E\", () => E);" +
            tail({{"E", "E"}}));
    // type-only export erased (no __esModule marker, no export emitted)
    xpc("export interface I { a: number; }", "");
    xpc("export type T = number;", "");
    // ASI-terminated declarations: the appended `exports.x = x;` must not fuse
    // onto the initializer (`const v = 42 exports.v = v;` is a syntax error).
    xpc("export const v = 42", esmX + "const v = 42; __mbun_X(\"v\", () => v);" + tail({{"v", "v"}}));
    xpc("export let w = 1\n", esmX + "let w = 1; __mbun_X(\"w\", () => w);\n" + tail({{"w", "w"}}));
    xpc("export var z = 1\n", esmX + "var z = 1; __mbun_X(\"z\", () => z);\n" + tail({{"z", "z"}}));
    xpc("export const a = 1\nexport const b = 2\n",
        esmX + "const a = 1; __mbun_X(\"a\", () => a);\nconst b = 2; __mbun_X(\"b\", () => b);\n" +
            tail({{"a", "a"}, {"b", "b"}}));
    xpc("export const o = {}\n", esmX + "const o = {}; __mbun_X(\"o\", () => o);\n" + tail({{"o", "o"}}));
    xpc("export const p = 1, q = 2\n",
        esmX + "const p = 1, q = 2; __mbun_X(\"p\", () => p); __mbun_X(\"q\", () => q);\n" +
            tail({{"p", "p"}, {"q", "q"}}));
    // block-terminated declarations need no inserted terminator
    xpc("export function f2() {}\n",
        esmX + "function f2() {} __mbun_X(\"f2\", () => f2);\n" + tail({{"f2", "f2"}}));
    xpc("export class C2 {}\n",
        esmX + "class C2 {} __mbun_X(\"C2\", () => C2);\n" + tail({{"C2", "C2"}}));

    // `export var NS;` publishes a *live binding*, not the `undefined` the
    // declaration happens to hold: the TS `namespace` emit fills NS on the next
    // statement, and `exports.NS = NS` froze it at undefined forever (typebox's
    // TypeSystemPolicy is exactly this shape). Verified against bun 1.3.14:
    // `export var NS; (function (N) { N.x = 1 })(NS || (NS = {}))` -> NS.x === 1.
    xpc("export var NS;", esmX + "var NS; __mbun_X(\"NS\", () => NS);" + tail({{"NS", "NS"}}));
    // A destructuring pattern publishes every name it binds, one getter each —
    // bun gives *every* export a getter (runtime.js:129-137), and its own node
    // shims are written this way (`export var { request, get, … } = http`,
    // src/node-fallbacks/http.js:2). Verified against bun 1.3.14: for
    // `const o = {a:7,b:8,z:9}; export const {a} = o; export const {b: c} = o;`
    // an importer sees a === 7 and c === 8.
    xpc("export const { a } = o;",
        esmX + "const { a } = o; __mbun_X(\"a\", () => a);" + tail({{"a", "a"}}));
    // `b` is the key and `c` the binding — only `c` is exported.
    xpc("export const { a, b: c } = o;",
        esmX + "const { a, b: c } = o; __mbun_X(\"a\", () => a); __mbun_X(\"c\", () => c);" +
            tail({{"a", "a"}, {"c", "c"}}));
    // A default does not make the name any less bound; `...rest` binds too.
    xpc("export const { a = 1, ...rest } = o;",
        esmX + "const { a = 1, ...rest } = o; __mbun_X(\"a\", () => a); "
               "__mbun_X(\"rest\", () => rest);" + tail({{"a", "a"}, {"rest", "rest"}}));
    xpc("export const [ x, y ] = arr;",
        esmX + "const [ x, y ] = arr; __mbun_X(\"x\", () => x); __mbun_X(\"y\", () => y);" +
            tail({{"x", "x"}, {"y", "y"}}));
    xpc("export const [ x, ...ys ] = arr;",
        esmX + "const [ x, ...ys ] = arr; __mbun_X(\"x\", () => x); "
               "__mbun_X(\"ys\", () => ys);" + tail({{"x", "x"}, {"ys", "ys"}}));
    // A computed key is an expression, not a binding: `k` is read, only `v` binds.
    xpc("export const { [k]: v } = o;",
        esmX + "const { [k]: v } = o; __mbun_X(\"v\", () => v);" + tail({{"v", "v"}}));
    // Nested patterns bind their leaves, not the intermediate keys.
    xpc("export const { a: { b }, c: [ d ] } = o;",
        esmX + "const { a: { b }, c: [ d ] } = o; __mbun_X(\"b\", () => b); "
               "__mbun_X(\"d\", () => d);" + tail({{"b", "b"}, {"d", "d"}}));
    // A destructured `let` mutated later reads through the getter, as in bun.
    xpc("export let { a: mut } = o;\nexport function bump() { mut = 99; }",
        esmX + "let { a: mut } = o; __mbun_X(\"mut\", () => mut);\n"
               "function bump() { mut = 99; } __mbun_X(\"bump\", () => bump);" +
            tail({{"mut", "mut"}, {"bump", "bump"}}));
    // A binding mutated by a later export'd function still reads through.
    xpc("export let n = 0;\nexport function inc() { n++; }",
        esmX + "let n = 0; __mbun_X(\"n\", () => n);\nfunction inc() { n++; } "
               "__mbun_X(\"inc\", () => inc);" + tail({{"n", "n"}, {"inc", "inc"}}));
}


void test_parameter_properties() {
    // TS constructor parameter properties: modifier erased AND `this.x = x`
    // synthesized at the top of the constructor body.
    xp("class C { constructor(private x: number) {} }",
       "class C { constructor( x) { this.x = x; } }");
    xp("class C { constructor(private a, private b) {} }",
       "class C { constructor( a,  b) { this.a = a; this.b = b; } }");
    xp("class C { constructor(public readonly x) {} }",
       "class C { constructor(  x) { this.x = x; } }");
    // a plain (non-property) parameter contributes no assignment
    xp("class C { constructor(x, private y) { this.z = 1; } }",
       "class C { constructor(x,  y) { this.y = y;  this.z = 1; } }");
    xp("class C { constructor(x: number) {} }", "class C { constructor(x) {} }");
    // derived class: assignments must run AFTER super(...) (this is valid only then)
    xp("class C extends B { constructor(private x) { super(); } }",
       "class C extends B { constructor( x) { super(); this.x = x; } }");
    xp("class C extends B { constructor(private x) { super(x); log(); } }",
       "class C extends B { constructor( x) { super(x); this.x = x; log(); } }");
}

void test_for_using() {
    // `for (using x of y)` / `for (await using x of y)` → `for (const x of y)`.
    // DEFERRED(for-of using disposal): per-iteration Symbol.dispose not lowered.
    xp("for (using x of y) f(x);", "for (const x of y) f(x);");
    xp("for (await using x of y) f(x);", "for (const x of y) f(x);");
    ok("async () => { for (await using r of gen()) use(r); }");
    // regular for-of remains byte-for-byte
    xp("for (const x of y) g(x);", "for (const x of y) g(x);");
}

// TS legacy decorator mode (tsconfig experimentalDecorators). The input must
// transpile to exactly `expected`.
void xpl(std::string_view src, std::string_view expected) {
    ++gChecks;
    auto r = transpile(src, {.legacy_decorators = true});
    if (!r.ok) {
        ++gFailures;
        std::println("  FAIL legacy transpile(\"{}\") unexpected error \"{}\"", printable(src),
                     printable(r.error));
        return;
    }
    if (r.code != expected) {
        ++gFailures;
        std::println("  FAIL legacy transpile(\"{}\")\n       got: [{}]\n       exp: [{}]",
                     printable(src), printable(r.code), printable(expected));
    }
}

// Stage-3 decorator lowering: the transpiled output must contain `needle` (the
// exact lowering shape is an implementation detail of the __mbun_dc* runtime).
void xps(std::string_view src, std::string_view needle) {
    ++gChecks;
    auto r = transpile(src);
    if (!r.ok) {
        ++gFailures;
        std::println("  FAIL transpile(\"{}\") unexpected error \"{}\"", printable(src),
                     printable(r.error));
        return;
    }
    if (r.code.find(needle) == std::string::npos) {
        ++gFailures;
        std::println("  FAIL transpile(\"{}\")\n       got: [{}]\n       missing: [{}]",
                     printable(src), printable(r.code), printable(needle));
    }
}

// The transpiled output must NOT contain `needle`.
void xps_not(std::string_view src, std::string_view needle) {
    ++gChecks;
    auto r = transpile(src);
    if (!r.ok) {
        ++gFailures;
        std::println("  FAIL transpile(\"{}\") unexpected error \"{}\"", printable(src),
                     printable(r.error));
        return;
    }
    if (r.code.find(needle) != std::string::npos) {
        ++gFailures;
        std::println("  FAIL transpile(\"{}\")\n       got: [{}]\n       must not contain: [{}]",
                     printable(src), printable(r.code), printable(needle));
    }
}

void test_decorators() {
    // TS legacy mode (experimentalDecorators): decorators are erased from the
    // class and re-applied after it via __mbun_ld (tsc __decorate timing).
    xpl("@sealed class C {}", " class C {} C = __mbun_ld(C,[],()=>[[(sealed)],[]]);");
    xpl("@foo.bar class C {}", " class C {} C = __mbun_ld(C,[],()=>[[(foo.bar)],[]]);");
    xpl("@foo() class C {}", " class C {} C = __mbun_ld(C,[],()=>[[(foo())],[]]);");
    xpl("class C { @dec method() {} }",
        "class C {  ;method() {} } C = __mbun_ld(C,[()=>[0,\"method\",[(dec)],[]]],()=>[[],[]]);");
    xpl("class C { @dec prop = 1; }",
        "class C {constructor() { this[\"prop\"] = (1); }   ; } C = __mbun_ld(C,[()=>[3,\"prop\",[(dec)],[]]],()=>[[],[]]);");
    xpl("class C { constructor(@Inject() x) {} }",
        "class C { constructor( x) {} } C = __mbun_ld(C,[],()=>[[],[[0,[(Inject())]]]]);");
    xpl("@a @b class C { @c m(@d p) {} }",
        "  class C {  ;m( p) {} } C = __mbun_ld(C,[()=>[0,\"m\",[(c)],[[0,[(d)]]]]],()=>[[(a),(b)],[]]);");
    // decorator on a computed member key: `[k]` must not be swallowed as indexing
    xpl("class C { @dec [k]: string = \"y\"; }",
        "class C {constructor() { this[k] = (\"y\"); }   ; } C = __mbun_ld(C,[()=>[3,(k),[(dec)],[]]],()=>[[],[]]);");
    xpl("class C { @a.b.c [k] = 1; }",
        "class C {constructor() { this[k] = (1); }   ; } C = __mbun_ld(C,[()=>[3,(k),[(a.b.c)],[]]],()=>[[],[]]);");
    // parenthesized decorator expression
    xpl("class C { @(deco) m() {} }",
        "class C {  ;m() {} } C = __mbun_ld(C,[()=>[0,\"m\",[((deco))],[]]],()=>[[],[]]);");
    // composes with cjs export lowering
    // A class decorator may *replace* the class: `C = __mbun_dcFin(1,C)` rebinds C,
    // and the live getter reports the replacement rather than the original.
    xpc("export @sealed class C {}",
        "var __mbun_O = ({}).constructor; __mbun_O.defineProperty(exports, \"__esModule\", { value: true });" + kLiveHelper + "\n"
        "__mbun_dcF(1,\"C\",[(sealed)]);  class C { static { __mbun_dcA(1,this); } } "
        "C = __mbun_dcFin(1,C); __mbun_X(\"C\", () => C);" + tail({{"C", "C"}}));
    // Default (stage-3, TC39) mode lowers to the __mbun_dc* runtime helpers:
    // class decorators evaluate in the prelude, member decorators inside the
    // class body (classEnv/private env), fields through the initializer chain.
    xps("@sealed class C {}", "__mbun_dcF(1,\"C\",[(sealed)]); ");
    xps("@sealed class C {}", " C = __mbun_dcFin(1,C);");
    xps("class C { @dec method() {} }", "._m(0,0,\"method\",[(dec)])");
    xps("class C { @dec prop = 1; }", "prop = __mbun_dcFld(1,0,this,(1));");
    xps("class C { @dec [k] = 1; }", "._k(0,(k))");
    xps("class C { accessor x = 1; }", "get x()");
    // An initializer-less DECORATED field takes its ` = __mbun_dcFld(…)` at the very
    // position the field terminator would go, so it must not also get a `;` there —
    // `prop;= __mbun_dcFld(…)` is a syntax error (caught on bun-ref's
    // test/bundler/transpiler/esbuild-decorator-tests.ts).
    xps("class C { @dec prop; }", "prop = __mbun_dcFld(1,0,this);");
    xps_not("class C { @dec prop; }", ";=");
    xps_not("class C { accessor x; }", ";=");
}

void test_jsx() {
    // ── the CLASSIC runtime + the runtime-agnostic scanner semantics ─────────
    // Classic is no longer the default (see test_jsx_automatic), so these now
    // select it explicitly — the assertions themselves are unchanged.
    // Lowercase tag → string; Uppercase / dotted → identifier; `<>` → Fragment.
    xpx_classic("const a = <div className=\"x\">hi</div>;",
                "const a = React.createElement(\"div\", { className: \"x\" }, \"hi\");");
    xpx_classic("const d = <img src={x} />;",
                "const d = React.createElement(\"img\", { src: x });");
    xpx_classic("const e = <br/>;", "const e = React.createElement(\"br\", null);");
    xpx_classic("const f = <Foo/>;", "const f = React.createElement(Foo, null);");
    xpx_classic("const m = <Foo.Bar/>;", "const m = React.createElement(Foo.Bar, null);");
    // Attributes: string / {expr} / boolean / {...spread} (object-spread props).
    xpx_classic("const b = <Foo a={1} {...rest}>{child}</Foo>;",
                "const b = React.createElement(Foo, { a: 1, ...rest }, child);");
    xpx_classic("const g = <input disabled />;",
                "const g = React.createElement(\"input\", { disabled: true });");
    xpx_classic("const h = <div data-x=\"1\" />;",
                "const h = React.createElement(\"div\", { \"data-x\": \"1\" });");
    // Fragment + text/expr children.
    xpx_classic("const c = <>a{x}b</>;",
                "const c = React.createElement(React.Fragment, null, \"a\", x, \"b\");");
    // Nested elements and `{expr}` children (with nested JSX in a callback).
    xpx_classic("const n = <A><B/><C>text</C></A>;",
                "const n = React.createElement(A, null, React.createElement(B, null), "
                "React.createElement(C, null, \"text\"));");
    xpx_classic("const e2 = <ul>{items.map(i => <li key={i}>{i}</li>)}</ul>;",
                "const e2 = React.createElement(\"ul\", null, items.map(i => "
                "React.createElement(\"li\", { key: i }, i)));");
    // JSX in conditional / arrow positions; comparison inside {expr} stays intact.
    xpx_classic(
        "const h2 = cond ? <a/> : <b/>;",
        "const h2 = cond ? React.createElement(\"a\", null) : React.createElement(\"b\", null);");
    xpx_classic("const j = <p>{a < b ? 1 : 2}</p>;",
                "const j = React.createElement(\"p\", null, a < b ? 1 : 2);");
    // Standard JSX whitespace fold: indentation/newlines collapse to single spaces.
    xpx_classic("const i = <div>\n  hello\n  world\n</div>;",
                "const i = React.createElement(\"div\", null, \"hello world\");");
    // Empty / comment-only expression children fold away.
    xpx_classic("const k = <div>{/* note */}x</div>;",
                "const k = React.createElement(\"div\", null, \"x\");");
    // With jsx enabled, plain TS erasure and real `<` comparison are unaffected.
    // (No element, so the runtime does not matter — left on the default.)
    xpx("const t: number = 1;", "const t = 1;");
    xpx("const c2 = a < b;", "const c2 = a < b;");
    xpx("const fn = foo<Bar>(x);", "const fn = foo(x);");
    // `@jsx` / `@jsxFrag` retarget the classic factory and fragment.
    // ref js_parser/lexer.rs:2589-2646; oracle `/** @jsxRuntime classic @jsx h */`
    // -> `h("div", null, "x")`.
    xpx("/** @jsxRuntime classic @jsx h */\nconst a = <div>x</div>;",
        "/** @jsxRuntime classic @jsx h */\nconst a = h(\"div\", null, \"x\");");
    xpx("/** @jsxRuntime classic @jsxFrag Fr */\nconst a = <>x</>;",
        "/** @jsxRuntime classic @jsxFrag Fr */\nconst a = React.createElement(Fr, null, \"x\");");
    // A dotted factory stays a member expression.
    xpx("/** @jsxRuntime classic @jsx a.b.c */\nconst a = <div/>;",
        "/** @jsxRuntime classic @jsx a.b.c */\nconst a = a.b.c(\"div\", null);");
}

// The AUTOMATIC runtime — bun's DEFAULT, and the one the classic expectations
// above used to be silently standing in for. Every `exp` here is bun 1.4.0's own
// output for the same input, read off `.mbun/bin/bun-rust`'s
// Bun.Transpiler.transformSync and whitespace-normalised (bun's printer breaks
// the props object across lines; mbun's erasure path emits it inline — the call
// shape, argument order and arity are what these pin).
void test_jsx_automatic() {
    // 6-arg dev form: jsxDEV(tag, props, key, isStaticChildren, undefined, this).
    // ref ast/e.rs:614-628.
    xpx("const a = <div x=\"1\" />;",
        "const a = jsxDEV_7x81h0kn(\"div\", { x: \"1\" }, undefined, false, undefined, this);");
    // No props is `{}`, not `null` — that is classic's spelling.
    xpx("const e = <br/>;",
        "const e = jsxDEV_7x81h0kn(\"br\", {}, undefined, false, undefined, this);");
    // children folds INTO the props object; one child is the value itself…
    xpx("const a = <div>hi</div>;",
        "const a = jsxDEV_7x81h0kn(\"div\", { children: \"hi\" }, undefined, false, undefined, "
        "this);");
    // …two or more become an array, and isStaticChildren flips to true. In DEV
    // bun never emits `jsxs` — the flag carries what `jsxs` carries in prod
    // (ref bun_core/feature_flags.rs:38).
    xpx("const a = <div>a{b}c</div>;",
        "const a = jsxDEV_7x81h0kn(\"div\", { children: [ \"a\", b, \"c\" ] }, undefined, true, "
        "undefined, this);");
    // `key` is LIFTED out of props into the 3rd argument.
    xpx("const a = <div key=\"k\" x=\"1\" />;",
        "const a = jsxDEV_7x81h0kn(\"div\", { x: \"1\" }, \"k\", false, undefined, this);");
    xpx("const a = <div key={x} />;",
        "const a = jsxDEV_7x81h0kn(\"div\", {}, x, false, undefined, this);");
    // …but `key` AFTER a spread cannot be lifted without changing which one wins,
    // so bun demotes that element to classic. ref oracle: `"key" prop after a
    // {...spread} is deprecated in JSX. Falling back to classic runtime.`
    xpx("const a = <div {...p} key=\"k\" />;",
        "const a = React.createElement(\"div\", { ...p, key: \"k\" });");
    // A spread with `key` BEFORE it is still liftable.
    xpx("const a = <div key=\"k\" {...p} />;",
        "const a = jsxDEV_7x81h0kn(\"div\", { ...p }, \"k\", false, undefined, this);");
    // `<>` imports Fragment rather than naming React.Fragment.
    xpx("const a = <>x</>;",
        "const a = jsxDEV_7x81h0kn(Fragment_8vg9x3sq, { children: \"x\" }, undefined, false, "
        "undefined, this);");
    // Components and member tags are identifiers, exactly as in classic.
    xpx("const a = <Foo x=\"1\">hi</Foo>;",
        "const a = jsxDEV_7x81h0kn(Foo, { x: \"1\", children: \"hi\" }, undefined, false, "
        "undefined, this);");
}

// The `@jsx*` comment pragmas. ref js_parser/lexer.rs:2589-2656.
void test_jsx_pragma() {
    // `@jsxRuntime classic` switches the whole file off the automatic default.
    xpx("/** @jsxRuntime classic */\nconst a = <div/>;",
        "/** @jsxRuntime classic */\nconst a = React.createElement(\"div\", null);");
    // A `//` line comment carries a pragma too.
    xpx("// @jsxRuntime classic\nconst a = <div/>;",
        "// @jsxRuntime classic\nconst a = React.createElement(\"div\", null);");
    // Position is irrelevant: a pragma BELOW the element still applies to it.
    // This is why pragma scanning is a whole-file pre-pass — verified on bun.
    xpx("const a = <div/>;\n/** @jsxRuntime classic */",
        "const a = React.createElement(\"div\", null);\n/** @jsxRuntime classic */");
    // …but only inside a COMMENT. The same text in a string is inert.
    xpx("const s = \"@jsxRuntime classic\";\nconst a = <div/>;",
        "const s = \"@jsxRuntime classic\";\nconst a = jsxDEV_7x81h0kn(\"div\", {}, undefined, "
        "false, undefined, this);");
    // …and a word boundary is required (`jsxRuntimeXclassic` is not `jsxRuntime`).
    xpx("/** @jsxRuntimeXclassic */\nconst a = <div/>;",
        "/** @jsxRuntimeXclassic */\nconst a = jsxDEV_7x81h0kn(\"div\", {}, undefined, false, "
        "undefined, this);");
    // An unknown runtime is a hard error, not a shrug. ref oracle.
    xpx_err("/** @jsxRuntime bogus */\nconst a = <div/>;", "Unsupported JSX runtime: \"bogus\"");

    // Deeply nested elements bound the JSX scanner's recursion instead of
    // running the native stack out (a bare SIGSEGV on the guard page). Each
    // `() => <div>` nests one more element — the `() => ` runs between them are
    // JSX text — so this reaches the lowerer's depth cap.
    // ref: compat/bun/test/bundler/transpiler/jsx-deep-nesting-stack-overflow.test.ts
    {
        std::string deep;
        for (int i = 0; i < 5000; ++i) {
            deep += "() => <div>";
        }
        xpx_err(deep, "Maximum call stack size exceeded");
    }
    // `@jsxImportSource` only moves the IMPORT, which transformSync does not emit
    // (see TranspileOptions::jsx_options) — so the call is unchanged here. The
    // import it selects is covered by test_jsx_import_injection.
    xpx("/** @jsxImportSource preact */\nconst a = <div/>;",
        "/** @jsxImportSource preact */\nconst a = jsxDEV_7x81h0kn(\"div\", {}, undefined, false, "
        "undefined, this);");
}

// The automatic runtime's import — the half transformSync leaves out and the
// module loader must not. ref js_parser/parser.rs:719-746 + jsx.rs:246-256.
void test_jsx_import_injection() {
    using mbun::js_parser::detail::JsxOptions;
    auto opts = [](std::string source, bool dev = true) {
        JsxOptions o;
        o.inject_import = true;
        o.import_source = std::move(source);
        o.development = dev;
        return o;
    };
    // ESM: dev pulls `jsxDEV` from `<source>/jsx-dev-runtime`.
    xpx_with("const a = <div/>;",
             "import { jsxDEV as jsxDEV_7x81h0kn } from \"react/jsx-dev-runtime\";const a = "
             "jsxDEV_7x81h0kn(\"div\", {}, undefined, false, undefined, this);",
             opts("react"));
    // `@jsxImportSource`/`--jsx-import-source` moves the module, suffix and all.
    xpx_with("const a = <div/>;",
             "import { jsxDEV as jsxDEV_7x81h0kn } from \"preact/jsx-dev-runtime\";const a = "
             "jsxDEV_7x81h0kn(\"div\", {}, undefined, false, undefined, this);",
             opts("preact"));
    // A RELATIVE import source is passed through as written, so it resolves
    // against the importing file — this is exactly hono's
    // `--jsx-import-source ../../src/jsx`.
    xpx_with("const a = <div/>;",
             "import { jsxDEV as jsxDEV_7x81h0kn } from \"../../src/jsx/jsx-dev-runtime\";const a "
             "= jsxDEV_7x81h0kn(\"div\", {}, undefined, false, undefined, this);",
             opts("../../src/jsx"));
    // Fragment is imported only when a fragment is actually used, and lands
    // alongside jsxDEV in one clause.
    xpx_with("const a = <>x</>;",
             "import { jsxDEV as jsxDEV_7x81h0kn, Fragment as Fragment_8vg9x3sq } from "
             "\"react/jsx-dev-runtime\";const a = jsxDEV_7x81h0kn(Fragment_8vg9x3sq, { children: "
             "\"x\" }, undefined, false, undefined, this);",
             opts("react"));
    // Production drops to `/jsx-runtime` and the 2-arg `jsx` / `jsxs` split.
    xpx_with("const a = <div/>;",
             "import { jsx as jsx_7x81h0kn } from \"react/jsx-runtime\";const a = "
             "jsx_7x81h0kn(\"div\", {});",
             opts("react", /*dev=*/false));
    xpx_with("const a = <div>a{b}c</div>;",
             "import { jsxs as jsxs_7x81h0kn } from \"react/jsx-runtime\";const a = "
             "jsxs_7x81h0kn(\"div\", { children: [ \"a\", b, \"c\" ] });",
             opts("react", /*dev=*/false));
    // A file with NO JSX must not acquire an import it does not need.
    xpx_with("const t: number = 1;", "const t = 1;", opts("react"));
    // Classic never imports: the factory is the user's own free variable.
    {
        JsxOptions o;
        o.inject_import = true;
        o.runtime = mbun::js_parser::detail::JsxRuntime::Classic;
        xpx_with("const a = <div/>;", "const a = React.createElement(\"div\", null);",
                 std::move(o));
    }
}

}  // namespace

int main() {
    test_annotation_erasure();
    test_class_member_named_like_modifier();
    test_class_index_signature();
    test_class_member_without_body();
    test_async_functions();
    test_statements();
    test_await();
    test_using_lowering();
    test_top_level_using_esm();
    test_regex();
    test_ts_type_features();
    test_type_only_declarations();
    test_enum_lowering();
    test_imports();
    test_exports();
    test_cjs_imports();
    test_cjs_exports();
    test_namespace_lowering();
    test_parameter_properties();
    test_for_using();
    test_decorators();
    test_jsx();
    test_jsx_automatic();
    test_jsx_pragma();
    test_jsx_import_injection();

    std::println("test_js_transpile: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
