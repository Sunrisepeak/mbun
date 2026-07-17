// test_js_lexer.cpp — T2.3 mbun.js_lexer (JS/TS lexer) test suite (S0 vectors).
//
// Test vectors are extracted from bun's original test suite (assertion
// semantics preserved, see AGENTS.md TDD rules). bun has no standalone lexer
// test file; lexical behavior is asserted through transpiler tests, so each
// vector cites the transpiler test it was derived from:
//   - .mbun/bun-ref/test/bundler/transpiler/transpiler.test.js
//   - .mbun/bun-ref/test/bundler/transpiler/runtime-transpiler.test.ts
//   - .mbun/bun-ref/test/bundler/transpiler/property.test.ts (+ fixture)
//   - .mbun/bun-ref/test/bundler/transpiler/fixtures/bun-pragma/*
// Vectors marked "lexer.rs semantics" are reference-derived from bun's lexer
// source (.mbun/refs/bun-lexer.rs, bun-ref @ f6e084f src/js_parser/lexer.rs):
// they pin behavior the transpiler tests only exercise indirectly (exact
// token kinds, numeric values, escape decoding). None weaken any bun test.
//
// DEFERRED(T2.4) — cases in the same bun test blocks that need the parser
// (js_parser) to judge; this list is the input checklist for T2.4:
//   - transpiler.test.js > TypeScript > "types" / "malformed enums" /
//     "rejects export clauses inside a non-declare namespace" / tuple-label
//     blocks: 'Unexpected {', 'Unexpected break', 'Unexpected "const"',
//     "Unexpected >", "Unexpected >>", "Unexpected >>>", "Unexpected >=",
//     "Unexpected >>=", "Unexpected >>>=", "Invalid assignment target" —
//     parser must re-split ">>"/">>>" via expect_greater_than-style rescans.
//   - transpiler.test.js > "identifier escapes":
//     'Expected identifier but found "var"' and
//     "Unexpected var" (TEscapedKeyword rejection sites),
//     'Expected identifier but found "in"' (escaped contextual keyword).
//   - transpiler.test.js > "private identifiers": "Unexpected #foo",
//     'Expected identifier but found "#foo"', 'Deleting the private name
//     "#foo" is forbidden' (lexer only produces TPrivateIdentifier).
//   - transpiler.test.js > TypeScript > "does not crash on an unterminated
//     template literal after type arguments": the "f<T>`" family is asserted
//     here lexically; the `new C<T>` TS type-argument context is T2.4.
//   - transpiler.test.js > "normalizes \r\n" (expectPrinted round-trip) and
//     all expectPrinted/expectPrintedMin vectors: printing is T2.5; only the
//     lexical decode half is asserted here.
//   - transpiler.test.js > "parser" > regexp printing round-trips (T2.5),
//     JSX tests, scan/scanImports, macros, DCE blocks: T2.4+.
//   - JSX scanning modes (next_inside_jsx_element / parse_jsx_string_literal
//     / expect_jsx_element_child in bun's lexer) are driven by the parser and
//     land with T2.4 JSX work; the token set here already covers them.
//
// SKIPPED(S1): runtime-transpiler.test.ts spawns bun subprocesses; only the
// pure-lexical "Unterminated string literal" expectation is vectorized here.
import std;
import mbun.js_lexer;

namespace {

using mbun::js_lexer::Lexer;
using mbun::js_lexer::Token;
using mbun::js_lexer::token_to_string;

int gChecks{0};
int gFailures{0};
constexpr int MAX_FAILURE_PRINTS{40};

void report_failure(std::string_view what) {
    ++gFailures;
    if (gFailures <= MAX_FAILURE_PRINTS) {
        std::println("  FAIL {}", what);
    }
}

std::string token_name(Token t) {
    return std::format("Token#{}({})", static_cast<int>(t), token_to_string(t));
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

// Lex the whole source; returns tokens (excluding EOF) or stops at first error.
struct LexResult {
    std::vector<Token> tokens;
    bool ok{true};
    std::string firstError;
};

LexResult lex_all(std::string_view src) {
    LexResult result;
    Lexer lexer{src};
    while (true) {
        auto step = lexer.next();
        if (!step) {
            result.ok = false;
            if (!lexer.diagnostics().empty()) {
                result.firstError = lexer.diagnostics().front().message;
            }
            return result;
        }
        if (lexer.token() == Token::EndOfFile) {
            return result;
        }
        result.tokens.push_back(lexer.token());
    }
}

void check_tokens(std::string_view src, std::initializer_list<Token> expected,
                  std::string_view srcRef) {
    ++gChecks;
    LexResult r{lex_all(src)};
    if (!r.ok) {
        report_failure(std::format("[{}] lex(\"{}\") unexpected error: {}", srcRef,
                                   printable(src), r.firstError));
        return;
    }
    if (!std::ranges::equal(r.tokens, expected)) {
        std::string got;
        for (Token t : r.tokens) {
            got += token_name(t) + " ";
        }
        report_failure(std::format("[{}] lex(\"{}\") tokens = {}", srcRef, printable(src), got));
    }
}

// First token must be `token` with identifier text `text` (identifier /
// escaped keyword / private identifier / bigint / hashbang contents).
void check_ident(std::string_view src, Token token, std::string_view text,
                 std::string_view srcRef) {
    ++gChecks;
    Lexer lexer{src};
    auto step = lexer.next();
    if (!step) {
        report_failure(std::format("[{}] lex(\"{}\") unexpected error", srcRef, printable(src)));
        return;
    }
    if (lexer.token() != token || lexer.identifier() != text) {
        report_failure(std::format("[{}] lex(\"{}\") = {} \"{}\", expected {} \"{}\"", srcRef,
                                   printable(src), token_name(lexer.token()),
                                   printable(lexer.identifier()), token_name(token),
                                   printable(text)));
    }
}

void check_number(std::string_view src, double expected, std::string_view srcRef) {
    ++gChecks;
    Lexer lexer{src};
    auto step = lexer.next();
    if (!step) {
        report_failure(std::format("[{}] number(\"{}\") unexpected error", srcRef,
                                   printable(src)));
        return;
    }
    if (lexer.token() != Token::NumericLiteral) {
        report_failure(std::format("[{}] number(\"{}\") token = {}", srcRef, printable(src),
                                   token_name(lexer.token())));
        return;
    }
    double actual{lexer.number()};
    // Exact equality: these are integer-representable or exactly-parsed values.
    if (!(actual == expected)) {
        report_failure(std::format("[{}] number(\"{}\") = {}, expected {}", srcRef,
                                   printable(src), actual, expected));
    }
}

void check_bigint(std::string_view src, std::string_view text, std::string_view srcRef) {
    ++gChecks;
    Lexer lexer{src};
    auto step = lexer.next();
    if (!step || lexer.token() != Token::BigIntegerLiteral || lexer.identifier() != text) {
        report_failure(std::format("[{}] bigint(\"{}\") = {} \"{}\", expected \"{}\"", srcRef,
                                   printable(src),
                                   step ? token_name(lexer.token()) : std::string{"<error>"},
                                   printable(step ? lexer.identifier() : ""), printable(text)));
    }
}

// First token must be a string/template token; decoded UTF-16 value must match.
void check_string_utf16(std::string_view src, std::u16string_view expected, Token expectedToken,
                        std::string_view srcRef) {
    ++gChecks;
    Lexer lexer{src};
    auto step = lexer.next();
    if (!step) {
        report_failure(std::format("[{}] string(\"{}\") unexpected error", srcRef,
                                   printable(src)));
        return;
    }
    if (lexer.token() != expectedToken) {
        report_failure(std::format("[{}] string(\"{}\") token = {}, expected {}", srcRef,
                                   printable(src), token_name(lexer.token()),
                                   token_name(expectedToken)));
        return;
    }
    auto decoded = lexer.string_literal_utf16();
    if (!decoded) {
        report_failure(std::format("[{}] string(\"{}\") decode failed", srcRef, printable(src)));
        return;
    }
    if (*decoded != expected) {
        std::string got;
        std::string want;
        for (char16_t c : *decoded) {
            got += std::format("{:04X} ", static_cast<unsigned>(c));
        }
        for (char16_t c : expected) {
            want += std::format("{:04X} ", static_cast<unsigned>(c));
        }
        report_failure(std::format("[{}] string(\"{}\") = [{}], expected [{}]", srcRef,
                                   printable(src), got, want));
    }
}

void check_string_utf16(std::string_view src, std::u16string_view expected,
                        std::string_view srcRef) {
    check_string_utf16(src, expected, Token::StringLiteral, srcRef);
}

// Lex until failure; the first diagnostic must contain `message`.
void check_error(std::string_view src, std::string_view message, std::string_view srcRef) {
    ++gChecks;
    LexResult r{lex_all(src)};
    if (r.ok) {
        report_failure(std::format("[{}] lex(\"{}\") expected error \"{}\", got none", srcRef,
                                   printable(src), message));
        return;
    }
    if (r.firstError.find(message) == std::string::npos) {
        report_failure(std::format("[{}] lex(\"{}\") error = \"{}\", expected to contain \"{}\"",
                                   srcRef, printable(src), r.firstError, message));
    }
}

// Lex fully; a non-fatal diagnostic containing `message` must have been logged.
void check_warn_diag(std::string_view src, std::string_view message, std::string_view srcRef) {
    ++gChecks;
    Lexer lexer{src};
    bool sawDiag{false};
    while (true) {
        auto step = lexer.next();
        if (!step) {
            break;
        }
        if (lexer.token() == Token::EndOfFile) {
            break;
        }
    }
    for (const auto& d : lexer.diagnostics()) {
        if (d.message.find(message) != std::string::npos) {
            sawDiag = true;
        }
    }
    if (!sawDiag) {
        report_failure(std::format("[{}] lex(\"{}\") expected diagnostic \"{}\"", srcRef,
                                   printable(src), message));
    }
}

// source: transpiler.test.js > describe("parser") > it("regexp")
void check_regex(std::string_view src, std::string_view expectedRaw,
                 std::string_view expectDiag, std::string_view srcRef) {
    ++gChecks;
    Lexer lexer{src};
    auto step = lexer.next();
    if (!step || lexer.token() != Token::Slash) {
        report_failure(std::format("[{}] regex(\"{}\") first token not '/'", srcRef,
                                   printable(src)));
        return;
    }
    auto scan = lexer.scan_regexp();
    if (!scan) {
        report_failure(std::format("[{}] regex(\"{}\") scan failed", srcRef, printable(src)));
        return;
    }
    if (lexer.raw() != expectedRaw) {
        report_failure(std::format("[{}] regex(\"{}\") raw = \"{}\", expected \"{}\"", srcRef,
                                   printable(src), printable(lexer.raw()),
                                   printable(expectedRaw)));
        return;
    }
    if (!expectDiag.empty()) {
        bool found{false};
        for (const auto& d : lexer.diagnostics()) {
            if (d.message.find(expectDiag) != std::string::npos) {
                found = true;
            }
        }
        if (!found) {
            report_failure(std::format("[{}] regex(\"{}\") expected diagnostic \"{}\"", srcRef,
                                       printable(src), expectDiag));
        }
    } else if (!lexer.diagnostics().empty()) {
        report_failure(std::format("[{}] regex(\"{}\") unexpected diagnostic \"{}\"", srcRef,
                                   printable(src), lexer.diagnostics().front().message));
    }
}

// ── vector groups ──────────────────────────────────────────────────────────

void test_empty_and_smoke() {
    // source: transpiler.test.js > it("scan on empty file does not segfault")
    check_tokens("", {}, "scan on empty file");
    check_tokens(" \t\n", {}, "scan on empty file");
}

void test_punctuation() {
    // Token inventory pinned by the ">"-splitting error family:
    // source: transpiler.test.js > TypeScript > it("types") —
    // err("f<x> >>>= g<y>;", ...) etc. distinguish every ">"-token.
    const char* SRC{"transpiler.test.js > TypeScript > types"};
    check_tokens(">", {Token::GreaterThan}, SRC);
    check_tokens(">=", {Token::GreaterThanEquals}, SRC);
    check_tokens(">>", {Token::GreaterThanGreaterThan}, SRC);
    check_tokens(">>=", {Token::GreaterThanGreaterThanEquals}, SRC);
    check_tokens(">>>", {Token::GreaterThanGreaterThanGreaterThan}, SRC);
    check_tokens(">>>=", {Token::GreaterThanGreaterThanGreaterThanEquals}, SRC);
    check_tokens("f<x> >>>= g<y>;",
                 {Token::Identifier, Token::LessThan, Token::Identifier, Token::GreaterThan,
                  Token::GreaterThanGreaterThanGreaterThanEquals, Token::Identifier,
                  Token::LessThan, Token::Identifier, Token::GreaterThan, Token::Semicolon},
                 SRC);

    // Full operator sweep. source: lexer.rs semantics (next() dispatch); the
    // token set is bun's `T` enum (src/ast/lexer_tables.rs @ f6e084f).
    const char* REF{"lexer.rs semantics > punctuation"};
    check_tokens("& && &= &&=",
                 {Token::Ampersand, Token::AmpersandAmpersand, Token::AmpersandEquals,
                  Token::AmpersandAmpersandEquals},
                 REF);
    check_tokens("| || |= ||=",
                 {Token::Bar, Token::BarBar, Token::BarEquals, Token::BarBarEquals}, REF);
    check_tokens("^ ^= ~ !", {Token::Caret, Token::CaretEquals, Token::Tilde,
                              Token::Exclamation}, REF);
    check_tokens("! != !==",
                 {Token::Exclamation, Token::ExclamationEquals, Token::ExclamationEqualsEquals},
                 REF);
    check_tokens("= == === =>", {Token::Equals, Token::EqualsEquals, Token::EqualsEqualsEquals,
                                 Token::EqualsGreaterThan}, REF);
    check_tokens("+ ++ += - -- -=",
                 {Token::Plus, Token::PlusPlus, Token::PlusEquals, Token::Minus,
                  Token::MinusMinus, Token::MinusEquals},
                 REF);
    check_tokens("* ** *= **=", {Token::Asterisk, Token::AsteriskAsterisk, Token::AsteriskEquals,
                                 Token::AsteriskAsteriskEquals}, REF);
    check_tokens("/ /=", {Token::Slash, Token::SlashEquals}, REF);
    check_tokens("% %=", {Token::Percent, Token::PercentEquals}, REF);
    check_tokens("< <= << <<=",
                 {Token::LessThan, Token::LessThanEquals, Token::LessThanLessThan,
                  Token::LessThanLessThanEquals},
                 REF);
    check_tokens("? ?? ??= ", {Token::Question, Token::QuestionQuestion,
                               Token::QuestionQuestionEquals}, REF);
    check_tokens("( ) [ ] { } , ; :",
                 {Token::OpenParen, Token::CloseParen, Token::OpenBracket, Token::CloseBracket,
                  Token::OpenBrace, Token::CloseBrace, Token::Comma, Token::Semicolon,
                  Token::Colon},
                 REF);
    check_tokens(". ...", {Token::Dot, Token::DotDotDot}, REF);
    // source: es-decorators.test.ts (decorator syntax requires TAt)
    check_tokens("@dec class {}", {Token::At, Token::Identifier, Token::Class,
                                   Token::OpenBrace, Token::CloseBrace},
                 "es-decorators.test.ts");

    // "a?.b" vs "a?.1:b" disambiguation. source: lexer.rs semantics
    // (next() 0x3F arm lookahead for 'a?.1:b').
    check_tokens("a?.b", {Token::Identifier, Token::QuestionDot, Token::Identifier}, REF);
    check_tokens("a?.1:b",
                 {Token::Identifier, Token::Question, Token::NumericLiteral, Token::Colon,
                  Token::Identifier},
                 REF);
}

void test_keywords() {
    // source: lexer.rs KEYWORDS table (src/ast/lexer_tables.rs @ f6e084f);
    // exercised throughout transpiler.test.js (e.g. tuple-label err vectors).
    struct KW {
        const char* text;
        Token token;
    };
    static constexpr KW KEYWORDS[] = {
        {"break", Token::Break},   {"case", Token::Case},     {"catch", Token::Catch},
        {"class", Token::Class},   {"const", Token::Const},   {"continue", Token::Continue},
        {"debugger", Token::Debugger}, {"default", Token::Default}, {"delete", Token::Delete},
        {"do", Token::Do},         {"else", Token::Else},     {"enum", Token::Enum},
        {"export", Token::Export}, {"extends", Token::Extends}, {"false", Token::False},
        {"finally", Token::Finally}, {"for", Token::For},     {"function", Token::Function},
        {"if", Token::If},         {"import", Token::Import}, {"in", Token::In},
        {"instanceof", Token::Instanceof}, {"new", Token::New}, {"null", Token::Null},
        {"return", Token::Return}, {"super", Token::Super},   {"switch", Token::Switch},
        {"this", Token::This},     {"throw", Token::Throw},   {"true", Token::True},
        {"try", Token::Try},       {"typeof", Token::Typeof}, {"var", Token::Var},
        {"void", Token::Void},     {"while", Token::While},   {"with", Token::With},
    };
    for (const auto& kw : KEYWORDS) {
        check_tokens(kw.text, {kw.token}, "lexer_tables.rs KEYWORDS");
    }
    // Contextual keywords are plain identifiers.
    // source: transpiler.test.js > TypeScript > "contextual keywords used as
    // plain identifiers keep their statements"
    for (const char* id : {"let", "yield", "await", "static", "async", "type", "namespace",
                           "interface", "declare", "satisfies", "abstract"}) {
        check_ident(id, Token::Identifier, id,
                    "transpiler.test.js > contextual keywords used as plain identifiers");
    }
}

void test_identifiers() {
    const char* ESC{"transpiler.test.js > parser > identifier escapes"};
    // source: transpiler.test.js > it("identifier escapes")
    check_ident("_\\u0076\\u0061\\u0072", Token::Identifier, "_var", ESC);
    // "var" decodes to keyword "var" => escaped keyword token
    check_ident("\\u0076\\u0061\\u0072", Token::EscapedKeyword, "var", ESC);

    // source: transpiler.test.js > TypeScript > "export default of an
    // identifier written with unicode escapes does not crash"
    const char* UESC{"transpiler.test.js > TypeScript > export default unicode escapes"};
    check_ident("\\u{66}", Token::Identifier, "f", UESC);
    check_ident("\\u0066", Token::Identifier, "f", UESC);
    check_ident("\\u{66}oo", Token::Identifier, "foo", UESC);
    check_tokens("export default \\u{66}", {Token::Export, Token::Default, Token::Identifier},
                 UESC);
    check_ident("\\u0066oo", Token::Identifier, "foo", UESC);

    // Unicode identifiers (Latin-1 Supplement + Myanmar).
    // source: property.test.ts > property-non-ascii-fixture.js ("código")
    // Adjacent-literal split stops the \xB3 hex escape from swallowing the 'd'.
    check_ident("c\xC3\xB3" "digo", Token::Identifier, "c\xC3\xB3" "digo",
                "property.test.ts > property-non-ascii-fixture.js");
    // U+1011 is ID_Start. source: transpiler.test.js > "string quote
    // selection" uses "ထ" — start property per lexer.rs unicode tables.
    check_ident("\xE1\x80\x91", Token::Identifier, "\xE1\x80\x91",
                "lexer.rs semantics > unicode ID_Start");

    // Escapes must still form a valid identifier.
    // source: lexer.rs scan_identifier_with_escapes ("Invalid identifier")
    check_error("\\u0031abc", "Invalid identifier", "lexer.rs semantics > invalid identifier");
    // Non-\u escape in identifier position is a syntax error.
    check_error("\\n", "Syntax Error", "lexer.rs semantics > invalid identifier escape");

    // Private identifiers.
    // source: transpiler.test.js > it("private identifiers") — parser errors
    // like "Unexpected #foo" are DEFERRED(T2.4); the token itself is lexical.
    const char* PRIV{"transpiler.test.js > private identifiers"};
    check_ident("#foo", Token::PrivateIdentifier, "#foo", PRIV);
    check_tokens("#foo in this", {Token::PrivateIdentifier, Token::In, Token::This}, PRIV);
    check_tokens("class Foo { #foo }",
                 {Token::Class, Token::Identifier, Token::OpenBrace, Token::PrivateIdentifier,
                  Token::CloseBrace},
                 PRIV);
    check_error("#3", "Syntax Error", "lexer.rs semantics > bad private identifier");
}

void test_numbers() {
    // source: transpiler.test.js > describe("simplification") >
    // it("constant folding") — expectPrinted("123", "123") etc.
    const char* FOLD{"transpiler.test.js > parser > constant folding"};
    check_number("123", 123.0, FOLD);
    check_tokens("-123", {Token::Minus, Token::NumericLiteral}, FOLD);
    check_tokens("123 .toString()",
                 {Token::NumericLiteral, Token::Dot, Token::Identifier, Token::OpenParen,
                  Token::CloseParen},
                 FOLD);

    // Bigint text semantics (identifier holds the digits-as-written, with
    // decimal underscores stripped but radix prefixes kept — this is exactly
    // why bun folds "123n === 1_2_3n" but not "0x00n == 0n").
    // source: transpiler.test.js > "type coercions" (0x00n == 0n stays) and
    // constant folding ("123n === 1_2_3n" -> "!0").
    const char* BIG{"transpiler.test.js > type coercions / constant folding bigint"};
    check_bigint("1n", "1", BIG);
    check_bigint("123n", "123", BIG);
    check_bigint("1234n", "1234", BIG);
    check_bigint("1_2_3n", "123", BIG);
    check_bigint("0x00n", "0x00", BIG);
    check_bigint("0n", "0", BIG);
    check_bigint("2n", "2", BIG);

    // Numeric literal value matrix. source: lexer.rs semantics
    // (parse_numeric_literal_or_dot); bun tests exercise via printing (T2.5).
    const char* NUM{"lexer.rs semantics > numeric literals"};
    check_number("0", 0.0, NUM);
    check_number("0x10", 16.0, NUM);
    check_number("0X10", 16.0, NUM);
    check_number("0xff", 255.0, NUM);
    check_number("0xFF", 255.0, NUM);
    check_number("0b101", 5.0, NUM);
    check_number("0B101", 5.0, NUM);
    check_number("0o17", 15.0, NUM);
    check_number("0O17", 15.0, NUM);
    check_number("017", 15.0, NUM);   // legacy octal
    check_number("08", 8.0, NUM);     // invalid legacy octal -> decimal
    check_number("09", 9.0, NUM);
    check_number("018", 18.0, NUM);   // 8 appears -> re-parsed as decimal
    check_number("0.5", 0.5, NUM);
    check_number(".5", 0.5, NUM);
    check_number("5.", 5.0, NUM);
    check_number("1e3", 1000.0, NUM);
    check_number("1E3", 1000.0, NUM);
    check_number("1e+3", 1000.0, NUM);
    check_number("1e-3", 0.001, NUM);
    check_number("1.5e2", 150.0, NUM);
    check_number("1_000", 1000.0, NUM);
    check_number("1_000.5", 1000.5, NUM);
    check_number("1_0e1_0", 100000000000.0, NUM);
    check_number("0x1_0", 16.0, NUM);
    check_number("0b1_1", 3.0, NUM);
    check_number("123456789", 123456789.0, NUM);
    check_number("1234567890123", 1234567890123.0, NUM);

    // Numeric error matrix. source: lexer.rs semantics (syntax_error paths).
    check_error("0x", "Syntax Error", NUM);
    check_error("0b", "Syntax Error", NUM);
    check_error("0o", "Syntax Error", NUM);
    check_error("0b2", "Syntax Error", NUM);
    check_error("0o8", "Syntax Error", NUM);
    check_error("0xG", "Syntax Error", NUM);
    check_error("1__2", "Syntax Error", NUM);
    check_error("1_", "Syntax Error", NUM);
    // "_1" starts with an identifier-start char, so it is an identifier, not a
    // number (corrected from a mis-extracted error vector — bun/JS lex this as
    // an identifier; asserting the correct behavior strengthens the test).
    check_ident("_1", Token::Identifier, "_1", NUM);
    check_error("1e", "Syntax Error", NUM);
    check_error("1e+", "Syntax Error", NUM);
    check_error("123abc", "Syntax Error", NUM);  // identifier right after number
    check_error("1.5n", "Syntax Error", NUM);    // bigint cannot have dot
    check_error("1e3n", "Syntax Error", NUM);    // ... or exponent
    check_error("010n", "Syntax Error", NUM);    // ... or legacy octal
    check_error("017.5", "Syntax Error", NUM);   // legacy octal cannot have dot... (see impl)
    check_error("0_1", "Syntax Error", NUM);     // legacy octal cannot have separators

    // "." and "..." are lexed by the number scanner.
    check_tokens(".", {Token::Dot}, NUM);
    check_tokens("...", {Token::DotDotDot}, NUM);
    check_tokens("a.b", {Token::Identifier, Token::Dot, Token::Identifier}, NUM);
}

void test_strings() {
    // source: transpiler.test.js > parser > "constant folding"
    // ('a' === '\x61' -> !0)
    const char* FOLD{"transpiler.test.js > parser > constant folding"};
    check_string_utf16("'\\x61'", u"a", FOLD);
    check_string_utf16("'a'", u"a", FOLD);
    check_string_utf16("'abc'", u"abc", FOLD);

    // source: transpiler.test.js > parser > "string quote selection"
    const char* QUOTE{"transpiler.test.js > parser > string quote selection"};
    check_string_utf16("\"\\n\"", u"\n", QUOTE);
    check_string_utf16("\"\\\"\"", u"\"", QUOTE);
    check_string_utf16("'\\''", u"'", QUOTE);
    check_string_utf16("\"\\u1011\"", u"ထ", QUOTE);
    check_string_utf16("\"\xE1\x80\x91\"", u"ထ", QUOTE);  // "ထ" passthrough

    // source: transpiler.test.js > parser > "unicode surrogates"
    const char* SURR{"transpiler.test.js > parser > unicode surrogates"};
    check_string_utf16("\"\\u{10334}\"", u"\U00010334", SURR);
    check_string_utf16("\"\\uD800\\uDF34\"", u"\U00010334", SURR);
    {
        // "\uDF34\uD800" keeps its (reversed, unpaired) order.
        std::u16string reversed;
        reversed.push_back(0xDF34);
        reversed.push_back(0xD800);
        check_string_utf16("\"\\uDF34\\uD800\"", reversed, SURR);
    }
    check_string_utf16("\"\xF0\x90\x8C\xB4\"", u"\U00010334", SURR);  // raw "𐌴"

    // source: transpiler.test.js > parser > "import with unicode"
    check_string_utf16("'mod\\u1011'", u"modထ",
                       "transpiler.test.js > parser > import with unicode");
    check_string_utf16("'mod\xE1\x80\x91'", u"modထ",
                       "transpiler.test.js > parser > import with unicode");
    // source: transpiler.test.js > parser > "import with quote"
    check_string_utf16("'\".ts'", u"\".ts", "transpiler.test.js > parser > import with quote");
    // source: transpiler.test.js > parser > "empty string as import/export
    // clause alias"
    check_string_utf16("\"\"", u"",
                       "transpiler.test.js > parser > empty string as clause alias");

    // source: transpiler.test.js > edge cases > it('`str` + "``"')
    check_string_utf16("\"``\"", u"``", "transpiler.test.js > edge cases > str + ``");
    check_string_utf16("\"`\"", u"`", "transpiler.test.js > edge cases > str + ``");

    // Escape decode matrix. source: lexer.rs decode_escape_sequences.
    const char* ESC{"lexer.rs semantics > string escapes"};
    check_string_utf16("'\\b\\f\\n\\r\\t\\v'", u"\b\f\n\r\t\v", ESC);
    check_string_utf16("'\\0'", std::u16string(1, u'\0'), ESC);
    check_string_utf16("'\\101'", u"A", ESC);   // legacy octal escape
    check_string_utf16("'\\x41'", u"A", ESC);
    check_string_utf16("'\\a\\q'", u"aq", ESC);  // identity escapes
    check_string_utf16("'a\\\nb'", u"ab", ESC);  // line continuation LF
    check_string_utf16("'a\\\r\nb'", u"ab", ESC);  // line continuation CRLF
    check_string_utf16("'\\u0041'", u"A", ESC);
    check_string_utf16("'\\u{41}'", u"A", ESC);
    // "\08": \0 followed by digit 8 -> "Invalid legacy octal literal"
    // (non-fatal range diagnostic; decoding continues).
    check_warn_diag("x = '\\08'", "Invalid legacy octal literal", ESC);
    check_error("'\\u{110000}'", "Unicode escape sequence is out of range", ESC);
    check_error("'\\u12'", "Syntax Error", ESC);
    check_error("'\\u{}'", "Syntax Error", ESC);
    check_error("'\\x4'", "Syntax Error", ESC);
    check_error("'\\xG1'", "Syntax Error", ESC);

    // Unterminated strings.
    // source: transpiler.test.js > TypeScript > "does not crash on an
    // unterminated template literal after type arguments"
    const char* UNTERM{"transpiler.test.js > TypeScript > unterminated template literal"};
    check_error("new C<T>\n`", "Unterminated string literal", UNTERM);
    check_error("new C<T>`", "Unterminated string literal", UNTERM);
    check_error("f<T>`", "Unterminated string literal", UNTERM);
    // source: runtime-transpiler.test.ts > "reports an unterminated string
    // literal at the end of a large js file" (1 MiB of 'a')
    {
        std::string big{"var s = \""};
        big.append(std::size_t{1} << 20, 'a');
        check_error(big, "Unterminated string literal",
                    "runtime-transpiler.test.ts > unterminated large js file");
    }
    // source: lexer.rs semantics — newline terminates ' and " strings.
    check_error("'abc\ndef'", "Unterminated string literal",
                "lexer.rs semantics > newline in string");
    check_error("\"abc", "Unterminated string literal",
                "lexer.rs semantics > EOF in string");
}

void test_templates() {
    // source: transpiler.test.js > it("normalizes \\r\\n") —
    // console.log(`\r\n\r\n\r\n`) prints as `\n\n\n` (TV normalization).
    check_string_utf16("`\r\n\r\n\r\n`", u"\n\n\n", Token::NoSubstitutionTemplateLiteral,
                       "transpiler.test.js > normalizes \\r\\n");
    check_string_utf16("``", u"", Token::NoSubstitutionTemplateLiteral,
                       "transpiler.test.js > edge cases > str + ``");
    check_string_utf16("`template`", u"template", Token::NoSubstitutionTemplateLiteral,
                       "transpiler.test.js > parser > constant folding template");

    // Template with substitutions: head/middle/tail via the parser-driven
    // rescan protocol. source: transpiler.test.js > "raw template literal
    // contents" (`...${"meow123"}...`), token protocol per lexer.rs.
    {
        ++gChecks;
        const char* SRC{"transpiler.test.js > raw template literal contents"};
        Lexer lexer{"`a${b}c${d}e`"};
        bool ok{true};
        auto expect_step = [&](Token expected) {
            if (!ok) {
                return;
            }
            if (lexer.token() != expected) {
                report_failure(std::format("[{}] template dance got {}, expected {}", SRC,
                                           token_name(lexer.token()), token_name(expected)));
                ok = false;
            }
        };
        if (!lexer.next()) {
            ok = false;
        }
        expect_step(Token::TemplateHead);
        if (ok && lexer.string_literal_utf16().value_or(u"?") != u"a") {
            report_failure(std::format("[{}] template head contents mismatch", SRC));
            ok = false;
        }
        if (ok && !lexer.next()) {
            ok = false;
        }
        if (ok) {
            expect_step(Token::Identifier);
        }
        if (ok && !lexer.next()) {
            ok = false;
        }
        if (ok) {
            expect_step(Token::CloseBrace);
        }
        if (ok && !lexer.rescan_close_brace_as_template_token()) {
            ok = false;
        }
        if (ok) {
            expect_step(Token::TemplateMiddle);
            if (lexer.string_literal_utf16().value_or(u"?") != u"c") {
                report_failure(std::format("[{}] template middle contents mismatch", SRC));
                ok = false;
            }
        }
        if (ok && !lexer.next()) {
            ok = false;
        }
        if (ok) {
            expect_step(Token::Identifier);
        }
        if (ok && !lexer.next()) {
            ok = false;
        }
        if (ok) {
            expect_step(Token::CloseBrace);
        }
        if (ok && !lexer.rescan_close_brace_as_template_token()) {
            ok = false;
        }
        if (ok) {
            expect_step(Token::TemplateTail);
            if (lexer.string_literal_utf16().value_or(u"?") != u"e") {
                report_failure(std::format("[{}] template tail contents mismatch", SRC));
                ok = false;
            }
        }
        if (!ok && gFailures == 0) {
            report_failure(std::format("[{}] template dance failed", SRC));
        }
    }

    // Raw template contents (TRV): \r and \r\n normalize to \n, escapes stay.
    // source: transpiler.test.js > it("raw template literal contents")
    {
        const char* SRC{"transpiler.test.js > raw template literal contents"};
        struct RawCase {
            std::string_view input;
            std::string_view raw;
        };
        static constexpr RawCase RAW_CASES[] = {
            {"`\r`", "\n"},
            {"`\r\n`", "\n"},
            {"`\n`", "\n"},
            {"`\r\r\r\r\r\n\r`", "\n\n\n\n\n\n"},
            {"`\n\r`", "\n\n"},
            {"`\\n`", "\\n"},  // TRV keeps the escape characters
        };
        for (const auto& c : RAW_CASES) {
            ++gChecks;
            Lexer lexer{c.input};
            auto step = lexer.next();
            if (!step || lexer.token() != Token::NoSubstitutionTemplateLiteral) {
                report_failure(std::format("[{}] raw(\"{}\") did not lex as template", SRC,
                                           printable(c.input)));
                continue;
            }
            if (lexer.raw_template_contents() != c.raw) {
                report_failure(std::format("[{}] raw(\"{}\") = \"{}\", expected \"{}\"", SRC,
                                           printable(c.input),
                                           printable(lexer.raw_template_contents()),
                                           printable(c.raw)));
            }
        }
    }

    // Escaped ` and ${ do not terminate/split. source: transpiler.test.js >
    // parser > "string quote selection" (`\`hi\``).
    check_string_utf16("`\\`hi\\``", u"`hi`", Token::NoSubstitutionTemplateLiteral,
                       "transpiler.test.js > parser > string quote selection");
    check_string_utf16("`a\\${b`", u"a${b", Token::NoSubstitutionTemplateLiteral,
                       "lexer.rs semantics > escaped template dollar");
    // Multi-line template is legal.
    check_string_utf16("`a\nb`", u"a\nb", Token::NoSubstitutionTemplateLiteral,
                       "lexer.rs semantics > multiline template");
}

void test_regexp() {
    // source: transpiler.test.js > parser > it("regexp")
    const char* SRC{"transpiler.test.js > parser > regexp"};
    check_regex("/x/g", "/x/g", "", SRC);
    check_regex("/x/i", "/x/i", "", SRC);
    check_regex("/x/m", "/x/m", "", SRC);
    check_regex("/x/s", "/x/s", "", SRC);
    check_regex("/x/u", "/x/u", "", SRC);
    check_regex("/x/y", "/x/y", "", SRC);
    check_regex("/gimme/g", "/gimme/g", "", SRC);
    check_regex("/gimgim/g", "/gimgim/g", "", SRC);
    check_regex("/x/msuygig", "/x/msuygig", "Duplicate flag \"g\" in regular expression", SRC);

    // source: lexer.rs scan_reg_exp semantics.
    const char* REF{"lexer.rs semantics > regexp"};
    check_regex("/a[/]b/", "/a[/]b/", "", REF);      // '/' inside class
    check_regex("/a\\/b/", "/a\\/b/", "", REF);      // escaped '/'
    check_regex("/x/dgimsuvy", "/x/dgimsuvy", "", REF);
    check_regex("/x/q", "/x/q", "Invalid flag \"q\" in regular expression", REF);
    {
        // Newline inside a regex is a syntax error.
        ++gChecks;
        Lexer lexer{"/a\nb/"};
        auto first = lexer.next();
        bool failed{false};
        if (first && lexer.token() == Token::Slash) {
            failed = !lexer.scan_regexp().has_value();
        }
        if (!failed) {
            report_failure(std::format("[{}] newline in regex should fail", REF));
        }
    }
}

void test_comments() {
    const char* REF{"lexer.rs semantics > comments"};
    check_tokens("a // comment\nb", {Token::Identifier, Token::Identifier}, REF);
    check_tokens("a /* comment */ b", {Token::Identifier, Token::Identifier}, REF);
    check_tokens("a /* multi\nline */ b", {Token::Identifier, Token::Identifier}, REF);
    check_tokens("// only a comment", {}, REF);
    check_tokens("/**/", {}, REF);
    check_error("/* unterminated", "Expected \"*/\" to terminate multi-line comment", REF);
    // Legacy HTML open comment is an explicit lexer error.
    check_error("<!-- foo", "Legacy HTML comments not implemented yet!", REF);
    // "-->" at line start = legacy close comment: consumes the rest of the line.
    check_tokens("a\n--> rest of line\nb", {Token::Identifier, Token::Identifier}, REF);

    // has_newline_before across comments (ASI input for T2.4).
    {
        ++gChecks;
        Lexer lexer{"a /* x\ny */ b"};
        bool ok{true};
        ok = ok && lexer.next().has_value();  // a
        ok = ok && lexer.next().has_value();  // b
        if (!ok || !lexer.has_newline_before()) {
            report_failure(std::format("[{}] multiline comment must set has_newline_before", REF));
        }
    }
    {
        ++gChecks;
        Lexer lexer{"a /* xy */ b"};
        bool ok{true};
        ok = ok && lexer.next().has_value();
        ok = ok && lexer.next().has_value();
        if (!ok || lexer.has_newline_before()) {
            report_failure(
                std::format("[{}] same-line comment must not set has_newline_before", REF));
        }
    }
}

void test_hashbang() {
    // source: fixtures/bun-pragma/fail/bun-pragma-before-hashbang.ts +
    // lexer.rs THashbang semantics (only at offset 0).
    const char* SRC{"bun-pragma fixtures + lexer.rs hashbang"};
    {
        ++gChecks;
        Lexer lexer{"#!/usr/bin/env bun\nexport const foo = 123;"};
        auto step = lexer.next();
        if (!step || lexer.token() != Token::Hashbang ||
            lexer.identifier() != "#!/usr/bin/env bun") {
            report_failure(std::format("[{}] hashbang token/text mismatch (got {} \"{}\")", SRC,
                                       step ? token_name(lexer.token()) : "<error>",
                                       printable(step ? lexer.identifier() : "")));
        } else {
            step = lexer.next();
            if (!step || lexer.token() != Token::Export) {
                report_failure(std::format("[{}] token after hashbang mismatch", SRC));
            }
        }
    }
    // "#!" not at offset 0 is not a hashbang ('!' can't start an identifier).
    check_error("a #! b", "Syntax Error", SRC);
}

void test_whitespace_and_newlines() {
    const char* REF{"lexer.rs semantics > whitespace"};
    // Unusual whitespace: NBSP, BOM/ZWNBSP, VT, FF, ideographic space.
    check_tokens("a\xC2\xA0" "b", {Token::Identifier, Token::Identifier}, REF);
    check_tokens("\xEF\xBB\xBFlet x", {Token::Identifier, Token::Identifier}, REF);
    check_tokens("a\x0B\x0C b", {Token::Identifier, Token::Identifier}, REF);
    check_tokens("a\xE3\x80\x80" "b", {Token::Identifier, Token::Identifier}, REF);
    // LS/PS count as newlines (has_newline_before).
    {
        ++gChecks;
        Lexer lexer{"a\xE2\x80\xA8m"};
        bool ok{true};
        ok = ok && lexer.next().has_value();
        ok = ok && lexer.next().has_value();
        if (!ok || lexer.token() != Token::Identifier || !lexer.has_newline_before()) {
            report_failure(std::format("[{}] U+2028 must set has_newline_before", REF));
        }
    }
    // Unknown character is a SyntaxError token (parser reports "Unexpected").
    {
        ++gChecks;
        Lexer lexer{"\xC2\xA1"};  // "¡" — not ID_Start
        auto step = lexer.next();
        if (!step || lexer.token() != Token::SyntaxError) {
            report_failure(std::format("[{}] non-identifier char must lex as SyntaxError token",
                                       REF));
        }
    }
}

void test_positions() {
    const char* REF{"lexer.rs semantics > positions"};
    {
        ++gChecks;
        Lexer lexer{"let x = 42"};
        bool ok{true};
        ok = ok && lexer.next().has_value();
        bool posOk{ok && lexer.start() == 0 && lexer.end() == 3 && lexer.raw() == "let"};
        ok = ok && lexer.next().has_value();
        posOk = posOk && ok && lexer.start() == 4 && lexer.end() == 5 && lexer.raw() == "x";
        ok = ok && lexer.next().has_value();
        ok = ok && lexer.next().has_value();
        posOk = posOk && ok && lexer.start() == 8 && lexer.end() == 10 && lexer.raw() == "42";
        if (!posOk) {
            report_failure(std::format("[{}] start/end/raw offsets wrong", REF));
        }
    }
    {
        ++gChecks;
        Lexer lexer{"a\nbb\n  cc"};
        auto lc0 = lexer.line_col(0);   // 'a'
        auto lc1 = lexer.line_col(2);   // 'bb'
        auto lc2 = lexer.line_col(7);   // 'cc'
        bool ok{lc0.line == 1 && lc0.column == 0 && lc1.line == 2 && lc1.column == 0 &&
                lc2.line == 3 && lc2.column == 2};
        if (!ok) {
            report_failure(std::format(
                "[{}] line_col mismatch: got ({},{}) ({},{}) ({},{})", REF, lc0.line, lc0.column,
                lc1.line, lc1.column, lc2.line, lc2.column));
        }
    }
}

}  // namespace

int main() {
    test_empty_and_smoke();
    test_punctuation();
    test_keywords();
    test_identifiers();
    test_numbers();
    test_strings();
    test_templates();
    test_regexp();
    test_comments();
    test_hashbang();
    test_whitespace_and_newlines();
    test_positions();

    std::println("test_js_lexer: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
