// test_package_json_editor.cpp — the package.json round-trip `mbun add` depends on.
//
// Covers mbun.install.npm.json's printer (stringify / guess_indentation /
// Value::singleLine) and mbun.install.package_json_editor's dependency-list edit.
//
// The blueprint for every expectation below:
//   - js_printer/lib.rs:974-1166  `write_pre_quoted_string_inner` (escape table)
//   - js_printer/lib.rs:4651-4691 `print_object_json` (layout / is_single_line)
//   - parsers/json.rs:257-300     `guess_indentation`
//   - parsers/json_stage2.rs:638-689 → json.rs:1000 (is_single_line from source)
//   - PackageManager/PackageJSONEditor.rs:887-1066 (reuse-or-create + rebuild)
//   - ast/e.rs:1622-1635, :1724-1741 (alphabetize = stable bytewise key sort)
import std;
import mbun.install.npm.json;
import mbun.install.package_json_editor;

namespace {

namespace json = mbun::install::npm::json;
namespace editor = mbun::install::package_json_editor;

int gChecks{0};
int gFailures{0};

void check(bool cond, std::string_view what) {
    ++gChecks;
    if (!cond) {
        ++gFailures;
        std::println("  FAIL {}", what);
    }
}

void check_eq(std::string_view got, std::string_view want, std::string_view what) {
    ++gChecks;
    if (got != want) {
        ++gFailures;
        std::println("  FAIL {}\n    got:  {}\n    want: {}", what, got, want);
    }
}

// Parse → print with no edits.
std::string roundtrip(std::string_view source) {
    auto doc{json::parse(source)};
    if (!doc || !doc->root) return "<parse failed>";
    return json::stringify(*doc->root, json::guess_indentation(source));
}

// ── the printer ─────────────────────────────────────────────────────────────

void test_escapes() {
    // The heart of regression/issue/00631: a decoded `\a<LF>\b\` must re-escape
    // to exactly `\\a\n\\b\\`. ref: lib.rs:1094 (0x5C → \\), :1077 (0x0A → \n).
    check_eq(roundtrip(R"({"testRegex":"\\a\n\\b\\"})"), R"({"testRegex": "\\a\n\\b\\"})",
             "escaped backslashes + newline survive the round-trip");

    // ref: lib.rs:1071-1099 — the named single-char escapes.
    check_eq(roundtrip(R"({"a":"\b\f\n\r\t\""})"), R"({"a": "\b\f\n\r\t\""})",
             "named control escapes");

    // ref: lib.rs:1101-1146 — `'`, backtick and `$` are NOT escaped when the
    // quote char is '"', even though can_print_without_escape excludes them.
    check_eq(roundtrip(R"({"a":"'`$"})"), R"({"a": "'`$"})", "quote/backtick/dollar stay raw");

    // ref: lib.rs:1155-1157 — for json, a sub-0x20 byte with no named escape
    // takes the \uXXXX branch (the \xHH branch is `!json` only). The input must
    // arrive escaped: a raw control byte inside a JSON string is invalid.
    check_eq(roundtrip(R"({"a":"\u0001\u001f"})"), R"({"a": "\u0001\u001f"})",
             "unnamed control chars decode then re-escape as \\uXXXX");

    // ref: lib.rs:1041-1057 — ascii_only=false, so non-ASCII is re-emitted
    // verbatim rather than escaped.
    check_eq(roundtrip("{\"a\":\"café 你好\"}"), "{\"a\": \"café 你好\"}",
             "non-ASCII passes through unescaped");

    // Keys go through the same quoter (lib.rs:4674-4675).
    check_eq(roundtrip(R"({"a\\b":1})"), R"({"a\\b": 1})", "keys are escaped too");
}

void test_layout() {
    // ref: json_stage2.rs:638 — a source with no newline inside the braces is
    // single-line, and lib.rs:4666-4669 prints json single-line members with a
    // space only *between* them (never after `{`).
    check_eq(roundtrip(R"({"a":1,"b":2})"), R"({"a": 1, "b": 2})", "single-line object layout");
    check_eq(roundtrip("{\n  \"a\": 1\n}"), "{\n  \"a\": 1\n}", "multi-line object layout");
    check_eq(roundtrip(R"({})"), R"({})", "empty object");
    check_eq(roundtrip(R"([])"), R"([])", "empty array");
    check_eq(roundtrip(R"([1,2])"), R"([1, 2])", "single-line array");
    check_eq(roundtrip("[\n  1,\n  2\n]"), "[\n  1,\n  2\n]", "multi-line array");

    // A nested multi-line value puts a newline in the parent's span, so the
    // parent is multi-line too (json_stage2.rs:681-689).
    check_eq(roundtrip("{\"a\": {\n  \"b\": 1\n}}"), "{\n  \"a\": {\n    \"b\": 1\n  }\n}",
             "a multi-line child forces the parent multi-line");

    check_eq(roundtrip(R"({"a":null,"b":true,"c":false,"d":1.5,"e":-3})"),
             R"({"a": null, "b": true, "c": false, "d": 1.5, "e": -3})", "scalar kinds");
}

void test_guess_indentation() {
    // ref: parsers/json.rs:2194-2197 + :1519-1525 (bun's own cases).
    check(json::guess_indentation("{\n    \"a\": 1\n}").count == 4, "guesses 4 spaces");
    check(json::guess_indentation("{\n\t\"a\": 1\n}").character == '\t', "guesses tab");
    check(json::guess_indentation("{\"a\": 1}").count == 2, "defaults to 2 spaces");
    // A newline *inside a string* must not be mistaken for indentation.
    check(json::guess_indentation(R"({"a": "/*",  "b": 2})").count == 2,
          "string contents do not drive the guess");

    check_eq(roundtrip("{\n    \"a\": {\n        \"b\": 1\n    }\n}"),
             "{\n    \"a\": {\n        \"b\": 1\n    }\n}", "4-space indent is replayed");
}

// ── the editor ──────────────────────────────────────────────────────────────

std::string edit_and_print(std::string_view source, editor::List list,
                           std::vector<editor::NewDependency> deps) {
    auto doc{json::parse(source)};
    if (!doc || !doc->root) return "<parse failed>";
    editor::edit(*doc, list, deps);
    return json::stringify(*doc->root, json::guess_indentation(source));
}

void test_edit() {
    // The exact 00631 expectation (test/regression/issue/00631.test.ts).
    check_eq(edit_and_print(R"({"testRegex":"\\a\n\\b\\"})", editor::List::Dependencies,
                            {{"left-pad", "^1.3.0"}}),
             "{\n  \"testRegex\": \"\\\\a\\n\\\\b\\\\\",\n  \"dependencies\": {\n"
             "    \"left-pad\": \"^1.3.0\"\n  }\n}",
             "issue/00631 — adding a dep rebuilds the root multi-line");

    // ref: PackageJSONEditor.rs:1024-1030 — appending a *new* dependency list
    // rebuilds the root with `..Default::default()`, resetting is_single_line.
    check_eq(edit_and_print(R"({"name":"x"})", editor::List::Dependencies, {{"a", "^1.0.0"}}),
             "{\n  \"name\": \"x\",\n  \"dependencies\": {\n    \"a\": \"^1.0.0\"\n  }\n}",
             "a new dependency list forces the root multi-line");

    // ref: the `needs_new_dependency_list` guards at :996/:1031 — when the list
    // already exists nothing is rebuilt, so both nodes keep their source layout.
    check_eq(edit_and_print(R"({"dependencies":{"a":"1.0.0"}})", editor::List::Dependencies,
                            {{"b", "^2.0.0"}}),
             R"({"dependencies": {"a": "1.0.0", "b": "^2.0.0"}})",
             "an existing single-line list keeps the root single-line");

    // ref: :906-911 → ast/e.rs:1622-1635 — alphabetize once len > 1, bytewise
    // ('@'=0x40 < 'l'=0x6C < 'z'=0x7A).
    check_eq(edit_and_print("{\n  \"name\": \"x\"\n}", editor::List::Dependencies,
                            {{"zoo", "^1.0.0"}, {"left-pad", "^1.3.0"}, {"@types/bun", "^1.0.0"}}),
             "{\n  \"name\": \"x\",\n  \"dependencies\": {\n    \"@types/bun\": \"^1.0.0\",\n"
             "    \"left-pad\": \"^1.3.0\",\n    \"zoo\": \"^1.0.0\"\n  }\n}",
             "dependencies are alphabetized bytewise");

    // Re-adding an existing name replaces the value in place (the phase-2
    // rewrite `add` performs: "latest" → "^1.3.0").
    check_eq(edit_and_print("{\n  \"dependencies\": {\n    \"a\": \"latest\"\n  }\n}",
                            editor::List::Dependencies, {{"a", "^1.3.0"}}),
             "{\n  \"dependencies\": {\n    \"a\": \"^1.3.0\"\n  }\n}",
             "re-adding a dep overwrites its version");

    // ref: CommandLineArguments.rs:1308-1313 — the flag → list mapping.
    check_eq(edit_and_print(R"({"name":"x"})", editor::List::DevDependencies, {{"a", "^1.0.0"}}),
             "{\n  \"name\": \"x\",\n  \"devDependencies\": {\n    \"a\": \"^1.0.0\"\n  }\n}",
             "-d targets devDependencies");
    check(editor::list_name(editor::List::OptionalDependencies) == "optionalDependencies",
          "--optional list name");
    check(editor::list_name(editor::List::PeerDependencies) == "peerDependencies",
          "--peer list name");

    // ref: :1063-1092 — an empty-object root is replaced by one holding just
    // the new list.
    check_eq(edit_and_print(R"({})", editor::List::Dependencies, {{"a", "^1.0.0"}}),
             "{\n  \"dependencies\": {\n    \"a\": \"^1.0.0\"\n  }\n}", "empty root is rebuilt");

    // A non-object `dependencies` is not reusable → replaced (:889-891).
    check_eq(edit_and_print(R"({"dependencies":"oops"})", editor::List::Dependencies,
                            {{"a", "^1.0.0"}}),
             "{\n  \"dependencies\": \"oops\",\n  \"dependencies\": {\n    \"a\": \"^1.0.0\"\n  }\n}",
             "a non-object dependencies field is not reused");

    // No requests → no edit at all.
    check_eq(edit_and_print(R"({"name":"x"})", editor::List::Dependencies, {}),
             R"({"name": "x"})", "an empty request list leaves the document untouched");
}

}  // namespace

int main() {
    test_escapes();
    test_layout();
    test_guess_indentation();
    test_edit();
    std::println("test_package_json_editor: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
