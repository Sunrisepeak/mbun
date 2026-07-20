// Engine-level tests for mbun.dotenv, off the JS engine. The expected values
// mirror bun's .env suite (compat/bun/test/cli/run/env.test.ts) and bun's
// reference parser (compat/bun/src/dotenv/env_loader.rs) where they translate
// 1:1 — the runtime-level `process.env.X` assertions there become direct Map
// lookups here.
import std;
import mbun.dotenv;

namespace {

using namespace mbun::dotenv;

int checks { 0 };
int failures { 0 };

void check(bool value, std::string_view label) {
    ++checks;
    if (!value) {
        ++failures;
        std::println("FAIL: {}", label);
    }
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view label) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::println("FAIL: {}\n  expected: {}\n  actual:   {}", label, expected, actual);
    }
}

// Lookup helper: a distinctive sentinel keeps a missing key from silently
// comparing equal to an empty expected value.
std::string_view get(const Map& map, std::string_view key) {
    if (auto value { map.get(key) }) {
        return *value;
    }
    return "<missing>";
}

// -------------------------------------------------------------------------
// 1. Bare (unquoted) values
// -------------------------------------------------------------------------
void test_bare_values() {
    auto map { parse("FOO=bar") };
    check_eq(get(map, "FOO"), "bar", "bare value");
    check(map.count() == 1, "bare value: single entry");

    // bun ".env space edgecase (issue #411)": inner spaces are kept.
    check_eq(get(parse("VARNAME=A B"), "VARNAME"), "A B", "bare value keeps inner space");

    // Whitespace around `=` and at end of line is trimmed.
    check_eq(get(parse("FOO =   bar   \n"), "FOO"), "bar", "bare value trimmed");

    // Empty value, with and without a trailing newline.
    check_eq(get(parse("EMPTY=\n"), "EMPTY"), "", "empty value before newline");
    check_eq(get(parse("EMPTY="), "EMPTY"), "", "empty value at EOF");

    // Key charset is [A-Za-z0-9_-.] (env_loader.rs parse_key).
    check_eq(get(parse("A.B-C_1=x"), "A.B-C_1"), "x", "key charset _ - .");

    // Multiple entries keep insertion order.
    auto multi { parse("A=1\nB=2\nC=3\n") };
    check(multi.count() == 3, "three entries");
    check_eq(multi.key_at(0), "A", "order: first key");
    check_eq(multi.key_at(2), "C", "order: last key");
    check_eq(multi.value_at(1), "2", "order: middle value");
}

// -------------------------------------------------------------------------
// 2. Double-quoted values
// -------------------------------------------------------------------------
void test_double_quoted() {
    check_eq(get(parse("A=\"a b\""), "A"), "a b", "double-quoted value");
    check_eq(get(parse("E=\"\""), "E"), "", "double-quoted empty value");

    // bun "#3911": \n inside double quotes becomes a real newline.
    check_eq(get(parse("KEY=\"a\\nb\""), "KEY"), "a\nb", "\\n escape in double quotes");
    check_eq(get(parse("KEY=\"a\\rb\""), "KEY"), "a\rb", "\\r escape in double quotes");

    // Only \n and \r are unescaped; any other escape keeps both bytes verbatim
    // (env_loader.rs parse_quoted default arm).
    check_eq(get(parse("KEY=\"a\\qb\""), "KEY"), "a\\qb", "unknown escape kept verbatim");

    // `#` inside quotes is data, not a comment.
    check_eq(get(parse("A=\"x # y\""), "A"), "x # y", "# inside double quotes is literal");

    // Single quotes inside double quotes are plain data.
    check_eq(get(parse("A=\"it's\""), "A"), "it's", "apostrophe inside double quotes");
}

// -------------------------------------------------------------------------
// 3. Single-quoted values
// -------------------------------------------------------------------------
void test_single_quoted() {
    check_eq(get(parse("A='a b'"), "A"), "a b", "single-quoted value");

    // bun ".env with zero length strings": FOO='' is an empty string, not missing.
    auto zero { parse("FOO=''\n") };
    check(zero.get("FOO").has_value(), "FOO='' is present");
    check_eq(get(zero, "FOO"), "", "FOO='' is zero length");

    // No escape processing inside single quotes: the backslash is kept and the
    // following byte is emitted as-is (parse_quoted non-`"` arm).
    check_eq(get(parse("A='a\\nb'"), "A"), "a\\nb", "no \\n escape in single quotes");

    check_eq(get(parse("A='has \"double\" inside'"), "A"), "has \"double\" inside",
             "double quotes inside single quotes");

    // Expansion still runs on single-quoted values — bun expands every parsed
    // value in a file regardless of quote style (_parse EXPAND branch).
    check_eq(get(parse("FOO=foo\nBAR='$FOO'"), "BAR"), "foo", "single quotes still expand");
}

// -------------------------------------------------------------------------
// 4. Backtick-quoted values
// -------------------------------------------------------------------------
void test_backtick_quoted() {
    check_eq(get(parse("B=`hello world`"), "B"), "hello world", "backtick-quoted value");

    // bun ".env special characters 1 (issue #2823)": A="a$t" -> "a" ($t unset),
    // C=`c\$v` -> "c$v" (the backslash survives quoting, then escapes the $).
    auto special { parse("A=\"a$t\"\nC=`c\\$v`") };
    check_eq(get(special, "A"), "a", "issue #2823: \"a$t\" -> a");
    check_eq(get(special, "C"), "c$v", "issue #2823: `c\\$v` -> c$v");
}

// -------------------------------------------------------------------------
// 5. `export ` prefix and `KEY: value` form
// -------------------------------------------------------------------------
void test_export_and_colon() {
    // bun ".env export assign".
    auto exported { parse("export FOO = foo\nexport = bar") };
    check_eq(get(exported, "FOO"), "foo", "export prefix");
    check_eq(get(exported, "export"), "bar", "bare `export = bar` assigns key `export`");

    // `export` without a separating space is just part of the key.
    check_eq(get(parse("exportFOO=1"), "exportFOO"), "1", "exportFOO is one key");

    // bun ".env colon assign": `KEY: value` needs whitespace after the colon.
    check_eq(get(parse("FOO: foo"), "FOO"), "foo", "colon assign");
    check(!parse("FOO:foo").get("FOO").has_value(), "colon without whitespace is not an assign");
}

// -------------------------------------------------------------------------
// 6. `#` comments
// -------------------------------------------------------------------------
void test_comments() {
    // bun ".env comments".
    auto commented { parse("#FOZ\nFOO = foo#FAIL\nBAR='bar' #BAZ") };
    check_eq(get(commented, "FOO"), "foo", "inline comment after bare value");
    check_eq(get(commented, "BAR"), "bar", "comment after quoted value");
    check(!commented.get("FOZ").has_value(), "full-line comment ignored");
    check(commented.count() == 2, "comment lines produce no entries");

    check_eq(get(parse("A=1 # trailing"), "A"), "1", "spaced trailing comment");
    check_eq(get(parse("A=#nothing"), "A"), "", "value that is only a comment is empty");
}

// -------------------------------------------------------------------------
// 7. Empty lines, whitespace-only lines and junk lines
// -------------------------------------------------------------------------
void test_empty_and_whitespace() {
    check(parse("").count() == 0, "empty input yields no entries");
    check(parse("\n\n  \n\t\n").count() == 0, "whitespace-only input yields no entries");

    auto blanks { parse("\n\n  \n\t\nFOO=bar\n\n") };
    check(blanks.count() == 1, "blank lines around an entry");
    check_eq(get(blanks, "FOO"), "bar", "entry after blank lines");

    check_eq(get(parse("    FOO=bar"), "FOO"), "bar", "leading whitespace before key");

    // A line that is not an assignment is skipped whole.
    auto junk { parse("garbage line here\nFOO=bar\n") };
    check(junk.count() == 1, "junk line skipped");
    check_eq(get(junk, "FOO"), "bar", "entry after junk line");
}

// -------------------------------------------------------------------------
// 8. ${VAR} interpolation
// -------------------------------------------------------------------------
void test_interpolation() {
    // bun ".env value expansion".
    auto expanded { parse("FOO=foo\nBAR=$FOO bar\nMOO=${FOO} ${BAR:-fail} ${MOZ:-moo}") };
    check_eq(get(expanded, "FOO"), "foo", "expansion: plain value");
    check_eq(get(expanded, "BAR"), "foo bar", "expansion: $VAR");
    check_eq(get(expanded, "MOO"), "foo foo bar moo", "expansion: ${VAR} and ${VAR:-default}");

    // Unset variables expand to the empty string.
    check_eq(get(parse("A=[${NOPE}]"), "A"), "[]", "unset ${VAR} expands to empty");
    check_eq(get(parse("A=${NOPE:-fallback}"), "A"), "fallback", "default used when unset");

    // A set variable wins over the default.
    check_eq(get(parse("X=set\nA=${X:-fallback}"), "A"), "set", "set value beats default");

    // Multiple refs in one value, and a ref that resolves to another expansion.
    check_eq(get(parse("A=1\nB=2\nC=$A-$B"), "C"), "1-2", "two refs in one value");
    check_eq(get(parse("A=a\nB=${A}b\nC=${B}c"), "C"), "abc", "chained expansion");
}

// -------------------------------------------------------------------------
// 9. Multiline values
// -------------------------------------------------------------------------
void test_multiline() {
    // A real newline inside quotes is kept; the value spans lines.
    check_eq(get(parse("KEY=\"line1\nline2\"\nNEXT=n"), "KEY"), "line1\nline2",
             "multiline double-quoted value");
    check_eq(get(parse("KEY=\"line1\nline2\"\nNEXT=n"), "NEXT"), "n",
             "parsing resumes after a multiline value");

    check_eq(get(parse("KEY='a\nb\nc'"), "KEY"), "a\nb\nc", "multiline single-quoted value");
    check_eq(get(parse("KEY=`a\nb`"), "KEY"), "a\nb", "multiline backtick value");

    // bun ".env Windows-style newline (issue #3042)": lone \r inside a quoted
    // value normalizes to \n, and \r\n collapses to \n.
    auto crlf { parse("FOO=\rBAR='bar\r\rbaz'\r\nMOO=moo\r") };
    check_eq(get(crlf, "FOO"), "", "issue #3042: FOO empty");
    check_eq(get(crlf, "BAR"), "bar\n\nbaz", "issue #3042: lone \\r normalized to \\n");
    check_eq(get(crlf, "MOO"), "moo", "issue #3042: MOO after CRLF");
}

// -------------------------------------------------------------------------
// 10. Escaped characters
// -------------------------------------------------------------------------
void test_escapes() {
    // bun ".env escaped dollar sign": \$FOO is not expanded and the backslash
    // is consumed by the expander.
    auto dollar { parse("FOO=foo\nBAR=\\$FOO") };
    check_eq(get(dollar, "FOO"), "foo", "escaped dollar: FOO");
    check_eq(get(dollar, "BAR"), "$FOO", "escaped dollar: \\$FOO -> $FOO");

    // An escaped quote does not close the value; bun keeps the backslash in the
    // result (parse_quoted only rewrites \n and \r).
    check_eq(get(parse("Q=\"he said \\\"hi\\\"\""), "Q"), "he said \\\"hi\\\"",
             "escaped double quote keeps its backslash");

    // A backslash in an unquoted value is a plain byte.
    check_eq(get(parse("A=a\\b"), "A"), "a\\b", "backslash in unquoted value");

    // An unterminated quote is not a quoted value at all: the parser falls back
    // to unquoted scanning, so the opening quote stays in the value. This
    // matches bun's `KEY="a\n` boundary case.
    check_eq(get(parse("A=\"unterminated\n"), "A"), "\"unterminated", "unterminated quote falls back");
}

// -------------------------------------------------------------------------
// 11. Override rules
// -------------------------------------------------------------------------
void test_override() {
    // bun issue #1262: a later key in the same source overrides an earlier one.
    check_eq(get(parse("FOO=1\nFOO=2\nFOO=3"), "FOO"), "3", "later key wins within one source");
    check(parse("FOO=1\nFOO=2").count() == 1, "duplicate key keeps one slot");

    // Keys already present before the parse are kept unless override_existing.
    Map keep;
    keep.put("FOO", "old");
    parse_into(keep, "FOO=new");
    check_eq(get(keep, "FOO"), "old", "pre-existing key not overridden by default");

    Map replace;
    replace.put("FOO", "old");
    parse_into(replace, "FOO=new", ParseOptions { .override_existing = true });
    check_eq(get(replace, "FOO"), "new", "override_existing replaces pre-existing key");

    // Expansion only rewrites entries added by this parse, so a pre-existing
    // value keeps its literal `$REF` (env_loader.rs starts the expand loop at
    // the pre-parse count).
    Map mixed;
    mixed.put("A", "$B");
    parse_into(mixed, "B=b\n");
    check_eq(get(mixed, "A"), "$B", "pre-existing values are not re-expanded");
    check_eq(get(mixed, "B"), "b", "new key parsed alongside pre-existing one");
}

// -------------------------------------------------------------------------
// 12. ParseOptions: expand / is_process
// -------------------------------------------------------------------------
void test_parse_options() {
    // expand = false leaves references untouched.
    auto raw { parse("FOO=foo\nBAR=$FOO bar", ParseOptions { .expand = false }) };
    check_eq(get(raw, "BAR"), "$FOO bar", "expand=false keeps $VAR literal");

    // is_process keeps the quote bytes and skips expansion (bun's IS_PROCESS).
    auto process { parse("A=\"x\"\nB=$A", ParseOptions { .is_process = true }) };
    check_eq(get(process, "A"), "\"x\"", "is_process keeps surrounding quotes");
    check_eq(get(process, "B"), "$A", "is_process skips expansion");
}

// -------------------------------------------------------------------------
// 13. Map container semantics
// -------------------------------------------------------------------------
void test_map() {
    Map map;
    check(map.count() == 0, "fresh map is empty");
    check(!map.get("NOPE").has_value(), "missing key returns nullopt");

    map.put("A", "1");
    map.put("B", "2");
    check(map.count() == 2, "two puts, two entries");
    check_eq(map.key_at(0), "A", "insertion order preserved");
    check_eq(get(map, "B"), "2", "lookup by key");

    // put on an existing key replaces in place, keeping the slot.
    map.put("A", "9");
    check(map.count() == 2, "re-put keeps the entry count");
    check_eq(map.key_at(0), "A", "re-put keeps the slot position");
    check_eq(get(map, "A"), "9", "re-put replaces the value");

    auto existing { map.get_or_put("A") };
    check(existing.found_existing, "get_or_put reports an existing key");
    check(existing.index == 0, "get_or_put returns the existing index");

    auto fresh { map.get_or_put("C") };
    check(!fresh.found_existing, "get_or_put reports a new key");
    check(fresh.index == 2, "get_or_put appends at the end");
    check_eq(get(map, "C"), "", "new get_or_put slot starts empty");

    map.set_value(fresh.index, "3");
    check_eq(get(map, "C"), "3", "set_value writes the slot");
    check_eq(map.value_at(2), "3", "value_at reads the slot");
}

}  // namespace

int main() {
    test_bare_values();
    test_double_quoted();
    test_single_quoted();
    test_backtick_quoted();
    test_export_and_colon();
    test_comments();
    test_empty_and_whitespace();
    test_interpolation();
    test_multiline();
    test_escapes();
    test_override();
    test_parse_options();
    test_map();
    std::println("test_dotenv: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
