// test_defines.cpp — mbun.bundler.defines: compile-time identifier substitution.
//
// Sources (assertion semantics preserved):
//   test/bundler/bundler_env.test.ts > describe("bundler/cli") >
//     "env/inline", "env/disable", "env/pattern-matching", "nested-refs"
//   — those four are end-to-end `bun build --env <mode>` runs; the table below
//   pins the pure substitution they rest on (which `process.env.X` occurrences
//   are replaced, and which must be left alone) so a regression is caught here
//   instead of only in the corpus.
//
// Blueprint for the behaviour under test: bun src/bundler/defines.rs
// `copy_env_for_define` :105 (synthesises the key `process.env.` ++ NAME with the
// value stored as a quoted string) and src/bundler/options.rs `create_defines`.

import std;
import mbun.bundler.defines;

namespace {

int gChecks{0};
int gFailures{0};

void check(bool condition, std::string_view message) {
    ++gChecks;
    if (!condition) {
        ++gFailures;
        std::println("FAIL: {}", message);
    }
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view message) {
    ++gChecks;
    if (actual != expected) {
        ++gFailures;
        std::println("FAIL: {}\n  expected: {}\n  actual:   {}", message, expected, actual);
    }
}

mbun::bundler::DefineTable env_table() {
    mbun::bundler::DefineTable table{};
    table.insert("process.env.FOO", mbun::bundler::quote_js_string("bar"));
    table.insert("process.env.BAZ", mbun::bundler::quote_js_string("123"));
    return table;
}

// `--env inline` replaces every read of an inlined variable, and only those.
void test_env_inline_replaces_reads() {
    const mbun::bundler::DefineTable table{env_table()};
    check_eq(mbun::bundler::apply_defines("console.log(process.env.FOO);", table),
             "console.log(\"bar\");", "process.env.FOO is replaced by its quoted value");
    check_eq(mbun::bundler::apply_defines("process.env.FOO + process.env.BAZ", table),
             "\"bar\" + \"123\"", "every occurrence is replaced");
    // A variable that is not in the table keeps its runtime lookup — that is what
    // makes `--env PUBLIC_*` leave PRIVATE_SECRET undefined at run time.
    check_eq(mbun::bundler::apply_defines("console.log(process.env.OTHER);", table),
             "console.log(process.env.OTHER);", "an un-inlined variable is left alone");
}

// The substitution is token-driven, so a match inside a string, comment,
// template literal or regex is impossible.
void test_no_substitution_inside_literals() {
    const mbun::bundler::DefineTable table{env_table()};
    check_eq(mbun::bundler::apply_defines("const s = \"process.env.FOO\";", table),
             "const s = \"process.env.FOO\";", "a string literal is never rewritten");
    check_eq(mbun::bundler::apply_defines("// process.env.FOO\nx;", table),
             "// process.env.FOO\nx;", "a comment is never rewritten");
    check_eq(mbun::bundler::apply_defines("const t = `process.env.FOO`;", table),
             "const t = `process.env.FOO`;", "a template literal is never rewritten");
    check_eq(mbun::bundler::apply_defines("const r = /process.env.FOO/;", table),
             "const r = /process.env.FOO/;", "a regex literal is never rewritten");
    // ...but a `${}` substitution is real code and IS rewritten.
    check_eq(mbun::bundler::apply_defines("const t = `v=${process.env.FOO}`;", table),
             "const t = `v=${\"bar\"}`;", "a template substitution is ordinary code");
}

// Writes and bindings must survive untouched: replacing them would change what
// the program declares rather than what it reads.
void test_write_positions_are_not_substituted() {
    const mbun::bundler::DefineTable table{env_table()};
    check_eq(mbun::bundler::apply_defines("process.env.FOO = 1;", table), "process.env.FOO = 1;",
             "an assignment target is left alone");
    check_eq(mbun::bundler::apply_defines("process.env.FOO++;", table), "process.env.FOO++;",
             "an update target is left alone");
    check_eq(mbun::bundler::apply_defines("x.process.env.FOO;", table), "x.process.env.FOO;",
             "a chain reached through a member access does not match");
}

// The table matches the longest key, and a plain identifier key works too.
void test_longest_match_and_bare_identifier() {
    mbun::bundler::DefineTable table{};
    table.insert("process.env", "({})");
    table.insert("process.env.FOO", "\"bar\"");
    table.insert("DEBUG", "false");
    check_eq(mbun::bundler::apply_defines("process.env.FOO;", table), "\"bar\";",
             "the more specific key wins");
    check_eq(mbun::bundler::apply_defines("process.env;", table), "({});",
             "the shorter key still matches on its own");
    check_eq(mbun::bundler::apply_defines("if (DEBUG) f();", table), "if (false) f();",
             "a bare identifier key is substituted");
    check(!table.insert("a..b", "1"), "a key with an empty segment is rejected");
}

// An empty table is the common case and must be byte-for-byte identity.
void test_empty_table_is_identity() {
    const mbun::bundler::DefineTable table{};
    check_eq(mbun::bundler::apply_defines("process.env.FOO;", table), "process.env.FOO;",
             "an empty table changes nothing");
}

// The value is stored as a JS string literal, so a value that itself looks like
// code ("process.env.BASE_URL", "$BASE_URL") is emitted as text, not re-expanded.
// ref: bundler_env.test.ts > "nested-refs".
void test_quote_js_string() {
    check_eq(mbun::bundler::quote_js_string("process.env.BASE_URL"), "\"process.env.BASE_URL\"",
             "a code-looking value is quoted");
    check_eq(mbun::bundler::quote_js_string("$BASE_URL"), "\"$BASE_URL\"",
             "a $-prefixed value is quoted verbatim");
    check_eq(mbun::bundler::quote_js_string("a\"b\\c\nd"), "\"a\\\"b\\\\c\\nd\"",
             "quotes, backslashes and newlines are escaped");

    mbun::bundler::DefineTable table{};
    table.insert("process.env.SHOULD_PRINT_BASE_URL",
                 mbun::bundler::quote_js_string("process.env.BASE_URL"));
    check_eq(mbun::bundler::apply_defines("console.log(process.env.SHOULD_PRINT_BASE_URL);", table),
             "console.log(\"process.env.BASE_URL\");",
             "the inserted text is not rescanned for further defines");
}

}  // namespace

int main() {
    test_env_inline_replaces_reads();
    test_no_substitution_inside_literals();
    test_write_positions_are_not_substituted();
    test_longest_match_and_bare_identifier();
    test_empty_table_is_identity();
    test_quote_js_string();

    if (gFailures != 0) {
        std::println("bundler.defines: {}/{} checks failed", gFailures, gChecks);
        return 1;
    }
    std::println("bundler.defines: {} checks passed", gChecks);
    return 0;
}
