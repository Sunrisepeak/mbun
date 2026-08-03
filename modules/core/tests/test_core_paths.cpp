// test_core_paths.cpp — T1.4 mbun.core.paths (node:path pure-logic core) test suite.
//
// Test vectors are extracted verbatim from bun's original test suite
// (.mbun/bun-ref/test/js/node/path/*, assertion semantics preserved, see AGENTS.md
// TDD rules):
//   basename.test.js, dirname.test.js, extname.test.js, is-absolute.test.js,
//   join.test.js, normalize.test.js, relative.test.js, resolve.test.js,
//   parse-format.test.js, zero-length-strings.test.js, 15704.test.js,
//   path.test.js (sep/delimiter), browserify.test.js
//
// Adaptations (assertion semantics preserved):
//   - process.cwd()-dependent vectors: the C++ API is pure logic, resolve/relative
//     take an explicit cwd argument. The given cwd is POSIX_CWD / WIN32_CWD below;
//     expected values are derived with the tests' own formulas for that cwd
//     (parentIsRoot() === false for both levels, cwd !== "/").
//   - "platform" (path.xxx) vectors are extracted as posix: bun runs them on linux
//     where path === path.posix (path.test.js > "path.delimiter" asserts exactly that).
//   - basename(path, undefined) maps to the single-argument C++ overload.
//   - parse-format.test.js > describe("path.format") subprocess test runs in-process
//     (the subprocess existed only for crash isolation; its assertions are pure).
//   - JS typeof-string checks in checkParseFormat are statically guaranteed by the
//     C++ ParsedPath struct type.
//
// SKIPPED(S1) — needs bun:test runtime, or out of T1.4 pure-logic scope:
//   - __filename-based vectors in basename.test.js / dirname.test.js /
//     extname.test.js / browserify.test.js (runtime file path of the test itself)
//   - path.test.js > "errors", parse-format.test.js "errors"/format-throws vectors,
//     resolve.test.js > "undefined argument are ignored ..." (JS ERR_INVALID_ARG_TYPE
//     argument-type semantics; the C++ API is statically typed)
//   - path.test.js > "Bun.which skips PATH segments ..." (Bun.spawn + Bun.which)
//   - posix-relative-on-windows.test.js (windows-host process.cwd regex invariant)
//   - resolve-long-cwd.test.ts (Bun.spawn + real filesystem + process.cwd)
//   - posix-exists.test.js / win32-exists.test.js (require() module identity)
//   - to-namespaced-path.test.js (toNamespacedPath is not part of T1.4 scope)
//   - matches-glob.test.ts (path.matchesGlob → T2.9 mbun.glob)
import std;
import mbun.core.paths;

namespace {

namespace posix = mbun::core::paths::posix;
namespace win32 = mbun::core::paths::win32;
using mbun::core::paths::ParsedPath;

int gChecks{0};
int gFailures{0};
constexpr int MAX_FAILURE_PRINTS{40};

// Explicit cwd stand-ins for process.cwd() (see header note on adaptations).
constexpr std::string_view POSIX_CWD{"/home/user/mbun"};
constexpr std::string_view WIN32_CWD{"C:\\Users\\user\\mbun"};

void report_failure(std::string_view what) {
    ++gFailures;
    if (gFailures <= MAX_FAILURE_PRINTS) {
        std::println("  FAIL {}", what);
    }
}

void check_str(std::string_view actual, std::string_view expected, std::string what) {
    ++gChecks;
    if (actual != expected) {
        report_failure(std::format("{}\n    expect=\"{}\"\n    actual=\"{}\"", what,
                                   expected.size() > 120 ? expected.substr(0, 120) : expected,
                                   actual.size() > 120 ? actual.substr(0, 120) : actual));
    }
}

void check_bool(bool actual, bool expected, std::string what) {
    ++gChecks;
    if (actual != expected) {
        report_failure(std::format("{} = {}, expected {}", what, actual, expected));
    }
}

std::string with_slashes(std::string_view s, char from, char to) {
    std::string out{s};
    for (char& c : out) {
        if (c == from) {
            c = to;
        }
    }
    return out;
}

std::string render_parts(std::span<const std::string_view> parts) {
    std::string out;
    for (std::size_t i{0}; i < parts.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += "\"";
        out += parts[i].size() > 40 ? parts[i].substr(0, 40) : parts[i];
        out += "\"";
    }
    return out;
}

// ---------------------------------------------------------------------------
// sep / delimiter
// ---------------------------------------------------------------------------

void test_sep_delimiter() {
    // source: path.test.js > describe("path") > test("path.sep") / test("path.delimiter")
    check_str(win32::SEP, "\\", "win32.SEP");
    check_str(posix::SEP, "/", "posix.SEP");
    check_str(win32::DELIMITER, ";", "win32.DELIMITER");
    check_str(posix::DELIMITER, ":", "posix.DELIMITER");
}

// ---------------------------------------------------------------------------
// is_absolute
// ---------------------------------------------------------------------------

struct BoolCase {
    std::string_view path;
    bool expected;
};

void test_is_absolute() {
    // source: is-absolute.test.js > describe("path.isAbsolute") > test("win32")
    constexpr BoolCase kWin[]{
        {"/", true},
        {"//", true},
        {"//server", true},
        {"//server/file", true},
        {"\\\\server\\file", true},
        {"\\\\server", true},
        {"\\\\", true},
        {"c", false},
        {"c:", false},
        {"c:\\", true},
        {"c:/", true},
        {"c://", true},
        {"C:/Users/", true},
        {"C:\\Users\\", true},
        {"C:cwd/another", false},
        {"C:cwd\\another", false},
        {"directory/directory", false},
        {"directory\\directory", false},
    };
    for (const auto& c : kWin) {
        check_bool(win32::is_absolute(c.path), c.expected,
                   std::format("[is-absolute.test.js>win32] win32.is_absolute(\"{}\")", c.path));
    }

    // source: is-absolute.test.js > describe("path.isAbsolute") > test("posix")
    constexpr BoolCase kPosix[]{
        {"/home/foo", true},
        {"/home/foo/..", true},
        {"bar/", false},
        {"./baz", false},
    };
    for (const auto& c : kPosix) {
        check_bool(posix::is_absolute(c.path), c.expected,
                   std::format("[is-absolute.test.js>posix] posix.is_absolute(\"{}\")", c.path));
    }

    // source: browserify.test.js > describe("isAbsolute")
    check_bool(win32::is_absolute("/foo/bar"), true, "[browserify>isAbsolute] win32 /foo/bar");
    check_bool(posix::is_absolute("/foo/bar"), true, "[browserify>isAbsolute] posix /foo/bar");
    check_bool(win32::is_absolute("\\hello\\world"), true,
               "[browserify>isAbsolute] win32 \\hello\\world");
    check_bool(posix::is_absolute("\\hello\\world"), false,
               "[browserify>isAbsolute] posix \\hello\\world");
    check_bool(win32::is_absolute("C:\\hello\\world"), true,
               "[browserify>isAbsolute] win32 C:\\hello\\world");
    check_bool(posix::is_absolute("C:\\hello\\world"), false,
               "[browserify>isAbsolute] posix C:\\hello\\world");

    // source: zero-length-strings.test.js > "zero length strings" (isAbsolute part)
    check_bool(posix::is_absolute(""), false, "[zero-length] posix.is_absolute(\"\")");
    check_bool(win32::is_absolute(""), false, "[zero-length] win32.is_absolute(\"\")");
}

// ---------------------------------------------------------------------------
// basename
// ---------------------------------------------------------------------------

struct BasenameCase {
    std::string_view path;
    std::string_view suffix;  // empty => single-argument call
    std::string_view expected;
};

void run_basename(std::span<const BasenameCase> cases, bool isWin, std::string_view src) {
    for (const auto& c : cases) {
        std::string actual;
        if (c.suffix.empty()) {
            actual = isWin ? win32::basename(c.path) : posix::basename(c.path);
        } else {
            actual = isWin ? win32::basename(c.path, c.suffix) : posix::basename(c.path, c.suffix);
        }
        check_str(actual, c.expected,
                  std::format("[{}] {}.basename(\"{}\"{}{}{})", src, isWin ? "win32" : "posix",
                              c.path, c.suffix.empty() ? "" : ", \"", c.suffix,
                              c.suffix.empty() ? "" : "\""));
    }
}

void test_basename() {
    // source: basename.test.js > describe("path.dirname") > test("platform")
    //         browserify.test.js > it("path.basename") (platform part)
    // platform === posix on linux; __filename vectors SKIPPED (see header)
    constexpr BasenameCase kPlatform[]{
        {".js", ".js", ""},
        {"js", ".js", "js"},
        {"file.js", ".ts", "file.js"},
        {"file", ".js", "file"},
        {"file.js.old", ".js.old", "file"},
        {"", "", ""},
        {"/dir/basename.ext", "", "basename.ext"},
        {"/basename.ext", "", "basename.ext"},
        {"basename.ext", "", "basename.ext"},
        {"basename.ext/", "", "basename.ext"},
        {"basename.ext//", "", "basename.ext"},
        {"aaa/bbb", "/bbb", "bbb"},
        {"aaa/bbb", "a/bbb", "bbb"},
        {"aaa/bbb", "bbb", "bbb"},
        {"aaa/bbb//", "bbb", "bbb"},
        {"aaa/bbb", "bb", "b"},
        {"aaa/bbb", "b", "bb"},
        {"/aaa/bbb", "/bbb", "bbb"},
        {"/aaa/bbb", "a/bbb", "bbb"},
        {"/aaa/bbb", "bbb", "bbb"},
        {"/aaa/bbb//", "bbb", "bbb"},
        {"/aaa/bbb", "bb", "b"},
        {"/aaa/bbb", "b", "bb"},
        {"/aaa/bbb", "", "bbb"},
        {"/aaa/", "", "aaa"},
        {"/aaa/b", "", "b"},
        {"/a/b", "", "b"},
        {"//a", "", "a"},
        {"a", "a", ""},
    };
    run_basename(kPlatform, false, "basename.test.js>platform");

    // source: basename.test.js > test("win32"); browserify.test.js > it("path.basename")
    // ("foo", undefined) maps to the single-argument overload (see header)
    constexpr BasenameCase kWin[]{
        {"\\dir\\basename.ext", "", "basename.ext"},
        {"\\basename.ext", "", "basename.ext"},
        {"basename.ext", "", "basename.ext"},
        {"basename.ext\\", "", "basename.ext"},
        {"basename.ext\\\\", "", "basename.ext"},
        {"foo", "", "foo"},
        {"foo", "", "foo"},  // ("foo", undefined)
        {"aaa\\bbb", "\\bbb", "bbb"},
        {"aaa\\bbb", "a\\bbb", "bbb"},
        {"aaa\\bbb", "bbb", "bbb"},
        {"aaa\\bbb\\\\\\\\", "bbb", "bbb"},
        {"aaa\\bbb", "bb", "b"},
        {"aaa\\bbb", "b", "bb"},
        {"C:", "", ""},
        {"C:.", "", "."},
        {"C:\\", "", ""},
        {"C:\\dir\\base.ext", "", "base.ext"},
        {"C:\\basename.ext", "", "basename.ext"},
        {"C:basename.ext", "", "basename.ext"},
        {"C:basename.ext\\", "", "basename.ext"},
        {"C:basename.ext\\\\", "", "basename.ext"},
        {"C:foo", "", "foo"},
        {"file:stream", "", "file:stream"},
        {"a", "a", ""},
    };
    run_basename(kWin, true, "basename.test.js>win32");

    // source: basename.test.js > test("posix"); browserify.test.js > it("path.basename")
    constexpr BasenameCase kPosix[]{
        {"\\dir\\basename.ext", "", "\\dir\\basename.ext"},
        {"\\basename.ext", "", "\\basename.ext"},
        {"basename.ext", "", "basename.ext"},
        {"basename.ext\\", "", "basename.ext\\"},
        {"basename.ext\\\\", "", "basename.ext\\\\"},
        {"foo", "", "foo"},
        {"foo", "", "foo"},  // ("foo", undefined)
    };
    run_basename(kPosix, false, "basename.test.js>posix");

    // source: basename.test.js > test("posix with control characters")
    //         browserify.test.js > it("path.basename") (control char part)
    check_str(posix::basename("/a/b/Icon\r"), "Icon\r",
              "[basename.test.js>control-chars] posix.basename(\"/a/b/Icon\\r\")");
}

// ---------------------------------------------------------------------------
// dirname
// ---------------------------------------------------------------------------

struct PairCase {
    std::string_view input;
    std::string_view expected;
};

void run_dirname(std::span<const PairCase> cases, bool isWin, std::string_view src) {
    for (const auto& c : cases) {
        std::string actual{isWin ? win32::dirname(c.input) : posix::dirname(c.input)};
        check_str(actual, c.expected,
                  std::format("[{}] {}.dirname(\"{}\")", src, isWin ? "win32" : "posix", c.input));
    }
}

void test_dirname() {
    // source: dirname.test.js > describe("path.dirname") > test("win32")
    constexpr PairCase kWin[]{
        {"c:\\", "c:\\"},
        {"c:\\foo", "c:\\"},
        {"c:\\foo\\", "c:\\"},
        {"c:\\foo\\bar", "c:\\foo"},
        {"c:\\foo\\bar\\", "c:\\foo"},
        {"c:\\foo\\bar\\baz", "c:\\foo\\bar"},
        {"c:\\foo bar\\baz", "c:\\foo bar"},
        {"\\", "\\"},
        {"\\foo", "\\"},
        {"\\foo\\", "\\"},
        {"\\foo\\bar", "\\foo"},
        {"\\foo\\bar\\", "\\foo"},
        {"\\foo\\bar\\baz", "\\foo\\bar"},
        {"\\foo bar\\baz", "\\foo bar"},
        {"c:", "c:"},
        {"c:foo", "c:"},
        {"c:foo\\", "c:"},
        {"c:foo\\bar", "c:foo"},
        {"c:foo\\bar\\", "c:foo"},
        {"c:foo\\bar\\baz", "c:foo\\bar"},
        {"c:foo bar\\baz", "c:foo bar"},
        {"file:stream", "."},
        {"dir\\file:stream", "dir"},
        {"\\\\unc\\share", "\\\\unc\\share"},
        {"\\\\unc\\share\\foo", "\\\\unc\\share\\"},
        {"\\\\unc\\share\\foo\\", "\\\\unc\\share\\"},
        {"\\\\unc\\share\\foo\\bar", "\\\\unc\\share\\foo"},
        {"\\\\unc\\share\\foo\\bar\\", "\\\\unc\\share\\foo"},
        {"\\\\unc\\share\\foo\\bar\\baz", "\\\\unc\\share\\foo\\bar"},
        {"/a/b/", "/a"},
        {"/a/b", "/a"},
        {"/a", "/"},
        {"", "."},
        {"/", "/"},
        {"////", "/"},
        {"foo", "."},
    };
    run_dirname(kWin, true, "dirname.test.js>win32");

    // source: dirname.test.js > test("posix")
    constexpr PairCase kPosix[]{
        {"/a/b/", "/a"}, {"/a/b", "/a"}, {"/a", "/"},   {"", "."},
        {"/", "/"},      {"////", "/"},  {"//a", "//"}, {"foo", "."},
    };
    run_dirname(kPosix, false, "dirname.test.js>posix");

    // source: browserify.test.js > describe("dirname") > it("path.dirname")
    // (posix + platform duplicate; platform === posix on linux)
    constexpr PairCase kBrowserifyFixtures[]{
        {"yo", "."},
        {"/yo", "/"},
        {"/yo/", "/"},
        {"/yo/123", "/yo"},
        {".", "."},
        {"../", "."},
        {"../../", ".."},
        {"../../foo", "../.."},
        {"../../foo/../", "../../foo"},
        {"/foo/../", "/foo"},
        {"../../foo/../bar", "../../foo/.."},
    };
    run_dirname(kBrowserifyFixtures, false, "browserify>dirname>path.dirname");

    // source: browserify.test.js > describe("dirname") > it("path.posix.dirname")
    constexpr PairCase kBrowserifyPosix[]{
        {"/a/b/", "/a"},
        {"/a/b", "/a"},
        {"/a", "/"},
        {"/a/", "/"},
        {"", "."},
        {"/", "/"},
        {"//", "/"},
        {"///", "/"},
        {"////", "/"},
        {"//a", "//"},
        {"//ab", "//"},
        {"///a", "//"},
        {"////a", "///"},
        {"/////a", "////"},
        {"foo", "."},
        {"foo/", "."},
        {"a/b", "a"},
        {"a/", "."},
        {"a///b", "a//"},
        {"a//b", "a/"},
        {"\\", "."},
        {"\\a", "."},
        {"a", "."},
        {"/a/b//c", "/a/b/"},
        {"/文檔", "/"},
        {"/文檔/", "/"},
        {"/文檔/新建文件夾", "/文檔"},
        {"/文檔/新建文件夾/", "/文檔"},
        {"//新建文件夾", "//"},
        {"///新建文件夾", "//"},
        {"////新建文件夾", "///"},
        {"/////新建文件夾", "////"},
        {"新建文件夾", "."},
        {"新建文件夾/", "."},
        {"文檔/新建文件夾", "文檔"},
        {"文檔/", "."},
        {"文檔///新建文件夾", "文檔//"},
        {"文檔//新建文件夾", "文檔/"},
    };
    run_dirname(kBrowserifyPosix, false, "browserify>dirname>path.posix.dirname");

    // source: browserify.test.js > describe("dirname") > it("path.win32.dirname")
    constexpr PairCase kBrowserifyWin[]{
        {"c:\\", "c:\\"},
        {"c:\\foo", "c:\\"},
        {"c:\\foo\\", "c:\\"},
        {"c:\\foo\\bar", "c:\\foo"},
        {"c:\\foo\\bar\\", "c:\\foo"},
        {"c:\\foo\\bar\\baz", "c:\\foo\\bar"},
        {"c:\\foo bar\\baz", "c:\\foo bar"},
        {"c:\\\\foo", "c:\\"},
        {"\\", "\\"},
        {"\\foo", "\\"},
        {"\\foo\\", "\\"},
        {"\\foo\\bar", "\\foo"},
        {"\\foo\\bar\\", "\\foo"},
        {"\\foo\\bar\\baz", "\\foo\\bar"},
        {"\\foo bar\\baz", "\\foo bar"},
        {"c:", "c:"},
        {"c:foo", "c:"},
        {"c:foo\\", "c:"},
        {"c:foo\\bar", "c:foo"},
        {"c:foo\\bar\\", "c:foo"},
        {"c:foo\\bar\\baz", "c:foo\\bar"},
        {"c:foo bar\\baz", "c:foo bar"},
        {"file:stream", "."},
        {"dir\\file:stream", "dir"},
        {"\\\\unc\\share", "\\\\unc\\share"},
        {"\\\\unc\\share\\foo", "\\\\unc\\share\\"},
        {"\\\\unc\\share\\foo\\", "\\\\unc\\share\\"},
        {"\\\\unc\\share\\foo\\bar", "\\\\unc\\share\\foo"},
        {"\\\\unc\\share\\foo\\bar\\", "\\\\unc\\share\\foo"},
        {"\\\\unc\\share\\foo\\bar\\baz", "\\\\unc\\share\\foo\\bar"},
        {"/a/b/", "/a"},
        {"/a/b", "/a"},
        {"/a", "/"},
        {"", "."},
        {"/", "/"},
        {"////", "/"},
        {"foo", "."},
        {"c:\\文檔", "c:\\"},
        {"c:\\文檔\\", "c:\\"},
        {"c:\\文檔\\新建文件夾", "c:\\文檔"},
        {"c:\\文檔\\新建文件夾\\", "c:\\文檔"},
        {"c:\\文檔\\新建文件夾\\baz", "c:\\文檔\\新建文件夾"},
        {"c:\\文檔 1\\新建文件夾", "c:\\文檔 1"},
        {"c:\\\\文檔", "c:\\"},
        {"\\文檔", "\\"},
        {"\\文檔\\", "\\"},
        {"\\文檔\\新建文件夾", "\\文檔"},
        {"\\文檔\\新建文件夾\\", "\\文檔"},
        {"\\文檔\\新建文件夾\\baz", "\\文檔\\新建文件夾"},
        {"\\文檔 1\\baz", "\\文檔 1"},
        {"c:文檔", "c:"},
        {"c:文檔\\", "c:"},
        {"c:文檔\\新建文件夾", "c:文檔"},
        {"c:文檔\\新建文件夾\\", "c:文檔"},
        {"c:文檔\\新建文件夾\\baz", "c:文檔\\新建文件夾"},
        {"c:文檔 1\\baz", "c:文檔 1"},
        {"/文檔/新建文件夾/", "/文檔"},
        {"/文檔/新建文件夾", "/文檔"},
        {"/文檔", "/"},
        {"新建文件夾", "."},
    };
    run_dirname(kBrowserifyWin, true, "browserify>dirname>path.win32.dirname");
}

// ---------------------------------------------------------------------------
// extname
// ---------------------------------------------------------------------------

void test_extname() {
    // source: extname.test.js > describe("path.extname") > test("general")
    // Each row is checked as posix, win32 (slashes → backslashes) and win32 with a
    // "C:" prefix, exactly like the JS loop. The __filename row is SKIPPED.
    constexpr PairCase kGeneral[]{
        {"", ""},
        {"/path/to/file", ""},
        {"/path/to/file.ext", ".ext"},
        {"/path.to/file.ext", ".ext"},
        {"/path.to/file", ""},
        {"/path.to/.file", ""},
        {"/path.to/.file.ext", ".ext"},
        {"/path/to/f.ext", ".ext"},
        {"/path/to/..ext", ".ext"},
        {"/path/to/..", ""},
        {"file", ""},
        {"file.ext", ".ext"},
        {".file", ""},
        {".file.ext", ".ext"},
        {"/file", ""},
        {"/file.ext", ".ext"},
        {"/.file", ""},
        {"/.file.ext", ".ext"},
        {".path/file.ext", ".ext"},
        {"file.ext.ext", ".ext"},
        {"file.", "."},
        {".", ""},
        {"./", ""},
        {".file.ext", ".ext"},
        {".file", ""},
        {".file.", "."},
        {".file..", "."},
        {"..", ""},
        {"../", ""},
        {"..file.ext", ".ext"},
        {"..file", ".file"},
        {"..file.", "."},
        {"..file..", "."},
        {"...", "."},
        {"...ext", ".ext"},
        {"....", "."},
        {"file.ext/", ".ext"},
        {"file.ext//", ".ext"},
        {"file/", ""},
        {"file//", ""},
        {"file./", "."},
        {"file.//", "."},
    };
    for (const auto& c : kGeneral) {
        check_str(posix::extname(c.input), c.expected,
                  std::format("[extname.test.js>general] posix.extname(\"{}\")", c.input));
        std::string winInput{with_slashes(c.input, '/', '\\')};
        check_str(win32::extname(winInput), c.expected,
                  std::format("[extname.test.js>general] win32.extname(\"{}\")", winInput));
        std::string winDrive{"C:" + winInput};
        check_str(win32::extname(winDrive), c.expected,
                  std::format("[extname.test.js>general] win32.extname(\"{}\")", winDrive));
    }

    // source: extname.test.js > test("win32")
    constexpr PairCase kWin[]{
        {".\\", ""},    {"..\\", ""},     {"file.ext\\", ".ext"}, {"file.ext\\\\", ".ext"},
        {"file\\", ""}, {"file\\\\", ""}, {"file.\\", "."},       {"file.\\\\", "."},
    };
    for (const auto& c : kWin) {
        check_str(win32::extname(c.input), c.expected,
                  std::format("[extname.test.js>win32] win32.extname(\"{}\")", c.input));
    }

    // source: extname.test.js > test("posix")
    constexpr PairCase kPosix[]{
        {".\\", ""},    {"..\\", ".\\"},  {"file.ext\\", ".ext\\"}, {"file.ext\\\\", ".ext\\\\"},
        {"file\\", ""}, {"file\\\\", ""}, {"file.\\", ".\\"},       {"file.\\\\", ".\\\\"},
    };
    for (const auto& c : kPosix) {
        check_str(posix::extname(c.input), c.expected,
                  std::format("[extname.test.js>posix] posix.extname(\"{}\")", c.input));
    }

    // source: browserify.test.js > it("path.extname") (platform === posix on linux)
    check_str(posix::extname("index.js"), ".js", "[browserify>extname] posix.extname(index.js)");
    check_str(posix::extname("make_plot.🔥"), ".🔥",
              "[browserify>extname] posix.extname(make_plot.🔥)");
}

// ---------------------------------------------------------------------------
// join
// ---------------------------------------------------------------------------

struct JoinCase {
    std::vector<std::string_view> parts;
    std::string_view expected;
};

// Shared vector table for path.posix.join and path.win32.join.
// source: join.test.js > describe("path.join") > test("general")
//         browserify.test.js > it("path.join") (identical table)
const std::vector<JoinCase> kJoinCommon{
    {{".", "x/b", "..", "/b/c.js"}, "x/b/c.js"},
    {{}, "."},
    {{"/.", "x/b", "..", "/b/c.js"}, "/x/b/c.js"},
    {{"/foo", "../../../bar"}, "/bar"},
    {{"foo", "../../../bar"}, "../../bar"},
    {{"foo/", "../../../bar"}, "../../bar"},
    {{"foo/x", "../../../bar"}, "../bar"},
    {{"foo/x", "./bar"}, "foo/x/bar"},
    {{"foo/x/", "./bar"}, "foo/x/bar"},
    {{"foo/x/", ".", "bar"}, "foo/x/bar"},
    {{"./"}, "./"},
    {{".", "./"}, "./"},
    {{".", ".", "."}, "."},
    {{".", "./", "."}, "."},
    {{".", "/./", "."}, "."},
    {{".", "/////./", "."}, "."},
    {{"."}, "."},
    {{"", "."}, "."},
    {{"", "foo"}, "foo"},
    {{"foo", "/bar"}, "foo/bar"},
    {{"", "/foo"}, "/foo"},
    {{"", "", "/foo"}, "/foo"},
    {{"", "", "foo"}, "foo"},
    {{"foo", ""}, "foo"},
    {{"foo/", ""}, "foo/"},
    {{"foo", "", "/bar"}, "foo/bar"},
    {{"./", "..", "/foo"}, "../foo"},
    {{"./", "..", "..", "/foo"}, "../../foo"},
    {{".", "..", "..", "/foo"}, "../../foo"},
    {{"", "..", "..", "/foo"}, "../../foo"},
    {{"/"}, "/"},
    {{"/", "."}, "/"},
    {{"/", ".."}, "/"},
    {{"/", "..", ".."}, "/"},
    {{""}, "."},
    {{"", ""}, "."},
    {{" /foo"}, " /foo"},
    {{" ", "foo"}, " /foo"},
    {{" ", "."}, " "},
    {{" ", "/"}, " /"},
    {{" ", ""}, " "},
    {{"/", "foo"}, "/foo"},
    {{"/", "/foo"}, "/foo"},
    {{"/", "//foo"}, "/foo"},
    {{"/", "", "/foo"}, "/foo"},
    {{"", "/", "foo"}, "/foo"},
    {{"", "/", "/foo"}, "/foo"},
};

// Windows-specific join vectors.
// source: join.test.js > test("general") (win32 additions)
//         browserify.test.js > it("path.join") (identical additions)
const std::vector<JoinCase> kJoinWin32{
    // UNC path expected
    {{"//foo/bar"}, "\\\\foo\\bar\\"},
    {{"\\/foo/bar"}, "\\\\foo\\bar\\"},
    {{"\\\\foo/bar"}, "\\\\foo\\bar\\"},
    // UNC path expected - server and share separate
    {{"//foo", "bar"}, "\\\\foo\\bar\\"},
    {{"//foo/", "bar"}, "\\\\foo\\bar\\"},
    {{"//foo", "/bar"}, "\\\\foo\\bar\\"},
    // UNC path expected - questionable
    {{"//foo", "", "bar"}, "\\\\foo\\bar\\"},
    {{"//foo/", "", "bar"}, "\\\\foo\\bar\\"},
    {{"//foo/", "", "/bar"}, "\\\\foo\\bar\\"},
    // UNC path expected - even more questionable
    {{"", "//foo", "bar"}, "\\\\foo\\bar\\"},
    {{"", "//foo/", "bar"}, "\\\\foo\\bar\\"},
    {{"", "//foo/", "/bar"}, "\\\\foo\\bar\\"},
    // No UNC path expected (no double slash in first component)
    {{"\\", "foo/bar"}, "\\foo\\bar"},
    {{"\\", "/foo/bar"}, "\\foo\\bar"},
    {{"", "/", "/foo/bar"}, "\\foo\\bar"},
    // No UNC path expected (no non-slashes in first component - questionable)
    {{"//", "foo/bar"}, "\\foo\\bar"},
    {{"//", "/foo/bar"}, "\\foo\\bar"},
    {{"\\\\", "/", "/foo/bar"}, "\\foo\\bar"},
    {{"//"}, "\\"},
    // No UNC path expected (share name missing - questionable).
    {{"//foo"}, "\\foo"},
    {{"//foo/"}, "\\foo\\"},
    {{"//foo", "/"}, "\\foo\\"},
    {{"//foo", "", "/"}, "\\foo\\"},
    // No UNC path expected (too many leading slashes - questionable)
    {{"///foo/bar"}, "\\foo\\bar"},
    {{"////foo", "bar"}, "\\foo\\bar"},
    {{"\\\\\\/foo/bar"}, "\\foo\\bar"},
    // Drive-relative vs drive-absolute paths (status quo behavior)
    {{"c:"}, "c:."},
    {{"c:."}, "c:."},
    {{"c:", ""}, "c:."},
    {{"", "c:"}, "c:."},
    {{"c:.", "/"}, "c:.\\"},
    {{"c:.", "file"}, "c:file"},
    {{"c:", "/"}, "c:\\"},
    {{"c:", "file"}, "c:\\file"},
};

void check_join_win32(const JoinCase& c, std::string_view src) {
    ++gChecks;
    std::string actual{win32::join(c.parts)};
    // Same dual acceptance as the JS test: win32 output is also accepted with
    // backslashes replaced by forward slashes (expected values of the shared
    // table are written with forward slashes).
    std::string alt{with_slashes(actual, '\\', '/')};
    if (actual != c.expected && alt != c.expected) {
        report_failure(std::format("[{}] win32.join({})\n    expect=\"{}\"\n    actual=\"{}\"", src,
                                   render_parts(c.parts), c.expected, actual));
    }
}

void test_join() {
    for (const auto& c : kJoinCommon) {
        check_str(posix::join(c.parts), c.expected,
                  std::format("[join.test.js>general] posix.join({})", render_parts(c.parts)));
        check_join_win32(c, "join.test.js>general");
    }
    for (const auto& c : kJoinWin32) {
        check_join_win32(c, "join.test.js>general(win32)");
    }

    // source: zero-length-strings.test.js > "zero length strings" (join part)
    // path.join(pwd) vectors use the explicit given cwd (see header)
    check_str(posix::join({""}), ".", "[zero-length] posix.join(\"\")");
    check_str(posix::join({"", ""}), ".", "[zero-length] posix.join(\"\",\"\")");
    check_str(win32::join({""}), ".", "[zero-length] win32.join(\"\")");
    check_str(win32::join({"", ""}), ".", "[zero-length] win32.join(\"\",\"\")");
    check_str(posix::join({POSIX_CWD}), POSIX_CWD, "[zero-length] posix.join(cwd)");
    check_str(posix::join({POSIX_CWD, ""}), POSIX_CWD, "[zero-length] posix.join(cwd,\"\")");

    // source: normalize.test.js > "first segment of exactly 4 chars ..." (join part)
    check_str(posix::join({"bb..", "..", "x"}), "x", "[normalize.test.js>4char] posix.join");
    check_str(win32::join({"bb..", "..", "x"}), "x", "[normalize.test.js>4char] win32.join");
}

void test_join_long() {
    // source: 15704.test.js > "too-long path names do not crash when joined"
    {
        const std::string longName(4096, 'b');
        check_str(posix::join({longName}), longName, "[15704] posix.join(b*4096)");
        check_str(win32::join({longName}), longName, "[15704] win32.join(b*4096)");
        // path.join (platform) duplicate → posix on linux
        check_str(posix::join({longName}), longName, "[15704] path.join(b*4096)");
    }

    // source: browserify.test.js > describe("path.join #5769") (platform sep === "/")
    for (std::size_t len : {4096uz, 4095uz, 4097uz, 65432uz, 65431uz, 65433uz}) {
        const std::string longName(len, 'b');
        check_str(posix::join({longName}), longName,
                  std::format("[browserify>join#5769] length {}", len));

        std::vector<std::string_view> parts(len, "b");
        std::string expected;
        expected.reserve(2 * len - 1);
        for (std::size_t i{0}; i < len; ++i) {
            if (i != 0) {
                expected += '/';
            }
            expected += 'b';
        }
        check_str(posix::join(parts), expected,
                  std::format("[browserify>join#5769] length {} joined", len));
    }
}

// ---------------------------------------------------------------------------
// normalize
// ---------------------------------------------------------------------------

void test_normalize() {
    // source: normalize.test.js > describe("path.normalize") > test("win32")
    //         browserify.test.js > it("path.normalize") (identical vectors)
    constexpr PairCase kWin[]{
        {"./fixtures///b/../b/c.js", "fixtures\\b\\c.js"},
        {"/foo/../../../bar", "\\bar"},
        {"a//b//../b", "a\\b"},
        {"a//b//./c", "a\\b\\c"},
        {"a//b//.", "a\\b"},
        {"//server/share/dir/file.ext", "\\\\server\\share\\dir\\file.ext"},
        {"/a/b/c/../../../x/y/z", "\\x\\y\\z"},
        {"C:", "C:."},
        {"C:..\\abc", "C:..\\abc"},
        {"C:..\\..\\abc\\..\\def", "C:..\\..\\def"},
        {"C:\\.", "C:\\"},
        {"file:stream", "file:stream"},
        {"bar\\foo..\\..\\", "bar\\"},
        {"bar\\foo..\\..", "bar"},
        {"bar\\foo..\\..\\baz", "bar\\baz"},
        {"bar\\foo..\\", "bar\\foo..\\"},
        {"bar\\foo..", "bar\\foo.."},
        {"..\\foo..\\..\\..\\bar", "..\\..\\bar"},
        {"..\\...\\..\\.\\...\\..\\..\\bar", "..\\..\\bar"},
        {"../../../foo/../../../bar", "..\\..\\..\\..\\..\\bar"},
        {"../../../foo/../../../bar/../../", "..\\..\\..\\..\\..\\..\\"},
        {"../foobar/barfoo/foo/../../../bar/../../", "..\\..\\"},
        {"../.../../foobar/../../../bar/../../baz", "..\\..\\..\\..\\baz"},
        {"foo/bar\\baz", "foo\\bar\\baz"},
    };
    for (const auto& c : kWin) {
        check_str(win32::normalize(c.input), c.expected,
                  std::format("[normalize.test.js>win32] win32.normalize(\"{}\")", c.input));
    }

    // source: normalize.test.js > test("posix"); browserify.test.js > it("path.normalize")
    constexpr PairCase kPosix[]{
        {"./fixtures///b/../b/c.js", "fixtures/b/c.js"},
        {"/foo/../../../bar", "/bar"},
        {"a//b//../b", "a/b"},
        {"a//b//./c", "a/b/c"},
        {"a//b//.", "a/b"},
        {"/a/b/c/../../../x/y/z", "/x/y/z"},
        {"///..//./foo/.//bar", "/foo/bar"},
        {"bar/foo../../", "bar/"},
        {"bar/foo../..", "bar"},
        {"bar/foo../../baz", "bar/baz"},
        {"bar/foo../", "bar/foo../"},
        {"bar/foo..", "bar/foo.."},
        {"../foo../../../bar", "../../bar"},
        {"../.../.././.../../../bar", "../../bar"},
        {"../../../foo/../../../bar", "../../../../../bar"},
        {"../../../foo/../../../bar/../../", "../../../../../../"},
        {"../foobar/barfoo/foo/../../../bar/../../", "../../"},
        {"../.../../foobar/../../../bar/../../baz", "../../../../baz"},
        {"foo/bar\\baz", "foo/bar\\baz"},
        {"", "."},  // browserify.test.js only
    };
    for (const auto& c : kPosix) {
        check_str(posix::normalize(c.input), c.expected,
                  std::format("[normalize.test.js>posix] posix.normalize(\"{}\")", c.input));
    }

    // source: normalize.test.js > test("first segment of exactly 4 chars ending in
    // '..' followed by '..'") (join vectors covered in test_join)
    check_str(posix::normalize("bb../../x"), "x", "[normalize>4char] posix bb../../x");
    check_str(win32::normalize("bb..\\..\\x"), "x", "[normalize>4char] win32 bb..\\..\\x");
    check_str(win32::normalize("bb../../x"), "x", "[normalize>4char] win32 bb../../x");
    check_str(posix::normalize("..../../x"), "x", "[normalize>4char] posix ..../../x");
    check_str(posix::normalize("bb../.."), ".", "[normalize>4char] posix bb../..");
    check_str(posix::normalize("bb../../"), "./", "[normalize>4char] posix bb../../");
    check_str(posix::normalize("b../../x"), "x", "[normalize>4char] posix b../../x");
    check_str(posix::normalize("abc../../x"), "x", "[normalize>4char] posix abc../../x");
    check_str(posix::normalize("abcd/../x"), "x", "[normalize>4char] posix abcd/../x");
    check_str(posix::normalize("../../x"), "../../x", "[normalize>4char] posix ../../x");
    check_str(posix::normalize("/bb../../x"), "/x", "[normalize>4char] posix /bb../../x");

    // source: zero-length-strings.test.js (normalize part; platform === posix)
    check_str(posix::normalize(""), ".", "[zero-length] posix.normalize(\"\")");
    check_str(win32::normalize(""), ".", "[zero-length] win32.normalize(\"\")");
    check_str(posix::normalize(POSIX_CWD), POSIX_CWD, "[zero-length] posix.normalize(cwd)");
}

void test_normalize_long() {
    // source: normalize.test.js > test("very long paths") (platform === posix)
    for (std::size_t len : {4096uz, 10000uz, 50000uz, 98340uz, 100000uz}) {
        const std::string longPath(len, 'a');
        std::string actual{posix::normalize(longPath)};
        check_str(actual, longPath, std::format("[normalize>long] a*{}", len));
        check_bool(actual.size() == len, true, std::format("[normalize>long] a*{} length", len));
    }
}

// ---------------------------------------------------------------------------
// resolve
// ---------------------------------------------------------------------------

struct ResolveCase {
    std::vector<std::string_view> parts;
    std::string_view expected;
};

void test_resolve() {
    // source: resolve.test.js > describe("path.resolve") > test("general") (win32)
    //         browserify.test.js > it("path.resolve") (identical vectors)
    // [["."], process.cwd()] uses the explicit given cwd (see header)
    const std::vector<ResolveCase> kWin{
        {{"c:/blah\\blah", "d:/games", "c:../a"}, "c:\\blah\\a"},
        {{"c:/ignore", "d:\\a/b\\c/d", "\\e.exe"}, "d:\\e.exe"},
        {{"c:/ignore", "c:/some/file"}, "c:\\some\\file"},
        {{"d:/ignore", "d:some/dir//"}, "d:\\ignore\\some\\dir"},
        {{"."}, WIN32_CWD},
        {{"//server/share", "..", "relative\\"}, "\\\\server\\share\\relative"},
        {{"c:/", "//"}, "c:\\"},
        {{"c:/", "//dir"}, "c:\\dir"},
        {{"c:/", "//server/share"}, "\\\\server\\share\\"},
        {{"c:/", "//server//share"}, "\\\\server\\share\\"},
        {{"c:/", "///some//dir"}, "c:\\some\\dir"},
        {{"C:\\foo\\tmp.3\\", "..\\tmp.3\\cycles\\root.js"}, "C:\\foo\\tmp.3\\cycles\\root.js"},
    };
    for (const auto& c : kWin) {
        check_str(win32::resolve(c.parts, WIN32_CWD), c.expected,
                  std::format("[resolve.test.js>win32] win32.resolve({})", render_parts(c.parts)));
    }

    // source: resolve.test.js > test("general") (posix); browserify.test.js
    const std::vector<ResolveCase> kPosix{
        {{"/var/lib", "../", "file/"}, "/var/file"},
        {{"/var/lib", "/../", "file/"}, "/file"},
        {{"a/b/c/", "../../.."}, POSIX_CWD},
        {{"."}, POSIX_CWD},
        {{"/some/dir", ".", "/absolute/"}, "/absolute"},
        {{"/foo/tmp.3/", "../tmp.3/cycles/root.js"}, "/foo/tmp.3/cycles/root.js"},
    };
    for (const auto& c : kPosix) {
        check_str(posix::resolve(c.parts, POSIX_CWD), c.expected,
                  std::format("[resolve.test.js>posix] posix.resolve({})", render_parts(c.parts)));
    }

    // source: resolve.test.js > test("UNC path before drive-relative path does not
    // corrupt resolvedDevice")
    check_str(win32::resolve({"C:/base", "//server/share", "C:relative"}, WIN32_CWD),
              "C:\\base\\relative", "[resolve>UNC-corrupt] C:/base //server/share C:relative");
    check_str(win32::resolve({"C:/base", "//a/b", "//c/d", "C:foo"}, WIN32_CWD), "C:\\base\\foo",
              "[resolve>UNC-corrupt] C:/base //a/b //c/d C:foo");
    check_str(win32::resolve({"//server/share", "C:relative"}, WIN32_CWD),
              win32::resolve({"C:relative"}, WIN32_CWD),
              "[resolve>UNC-corrupt] UNC arg is a no-op (C:relative)");
    check_str(win32::resolve({"//server/share", "C:"}, WIN32_CWD),
              win32::resolve({"C:"}, WIN32_CWD), "[resolve>UNC-corrupt] UNC arg is a no-op (C:)");
    check_str(win32::resolve({"//a/b", "//c/d", "C:foo"}, WIN32_CWD),
              win32::resolve({"C:foo"}, WIN32_CWD),
              "[resolve>UNC-corrupt] UNC args are a no-op (C:foo)");

    // source: zero-length-strings.test.js (resolve part; platform === posix)
    check_str(posix::resolve({""}, POSIX_CWD), POSIX_CWD, "[zero-length] resolve(\"\")");
    check_str(posix::resolve({"", ""}, POSIX_CWD), POSIX_CWD, "[zero-length] resolve(\"\",\"\")");
}

void test_resolve_long() {
    // source: resolve.test.js > test("very long paths") (platform === posix on linux)
    for (std::size_t len : {4096uz, 10000uz, 50000uz, 98340uz, 100000uz}) {
        std::string longPath{"/"};
        longPath.append(len, 'a');
        std::string result{posix::resolve({longPath}, POSIX_CWD)};
        check_bool(result.contains('a'), true, std::format("[resolve>long] a*{} contains a", len));
        check_bool(posix::is_absolute(result), true,
                   std::format("[resolve>long] a*{} isAbsolute", len));
        check_bool(result.size() == 1 + len, true,
                   std::format("[resolve>long] a*{} length {} == {}", len, result.size(), 1 + len));
    }
    const std::string longSegment(50000, 'b');
    std::string result{posix::resolve({"/", longSegment, "c"}, POSIX_CWD)};
    check_bool(result.contains('b'), true, "[resolve>long] multi contains b");
    check_bool(result.ends_with("/c"), true, "[resolve>long] multi ends with /c");
}

// ---------------------------------------------------------------------------
// relative
// ---------------------------------------------------------------------------

struct RelativeCase {
    std::string_view from;
    std::string_view to;
    std::string_view expected;
};

void run_relative(std::span<const RelativeCase> cases, bool isWin, std::string_view src) {
    for (const auto& c : cases) {
        std::string actual{isWin ? win32::relative(c.from, c.to, WIN32_CWD)
                                 : posix::relative(c.from, c.to, POSIX_CWD)};
        check_str(actual, c.expected,
                  std::format("[{}] {}.relative(\"{}\",\"{}\")", src, isWin ? "win32" : "posix",
                              c.from, c.to));
    }
}

void test_relative() {
    // source: relative.test.js > describe("path.relative") > test("general") (win32)
    //         browserify.test.js > it("path.relative") (superset, last entry only there)
    constexpr RelativeCase kWin[]{
        {"c:/blah\\blah", "d:/games", "d:\\games"},
        {"c:/aaaa/bbbb", "c:/aaaa", ".."},
        {"c:/aaaa/bbbb", "c:/cccc", "..\\..\\cccc"},
        {"c:/aaaa/bbbb", "c:/aaaa/bbbb", ""},
        {"c:/aaaa/bbbb", "c:/aaaa/cccc", "..\\cccc"},
        {"c:/aaaa/", "c:/aaaa/cccc", "cccc"},
        {"c:/", "c:\\aaaa\\bbbb", "aaaa\\bbbb"},
        {"c:/aaaa/bbbb", "d:\\", "d:\\"},
        {"c:/AaAa/bbbb", "c:/aaaa/bbbb", ""},
        {"c:/aaaaa/", "c:/aaaa/cccc", "..\\aaaa\\cccc"},
        {"C:\\foo\\bar\\baz\\quux", "C:\\", "..\\..\\..\\.."},
        {"C:\\foo\\test", "C:\\foo\\test\\bar\\package.json", "bar\\package.json"},
        {"C:\\foo\\bar\\baz-quux", "C:\\foo\\bar\\baz", "..\\baz"},
        {"C:\\foo\\bar\\baz", "C:\\foo\\bar\\baz-quux", "..\\baz-quux"},
        {"\\\\foo\\bar", "\\\\foo\\bar\\baz", "baz"},
        {"\\\\foo\\bar\\baz", "\\\\foo\\bar", ".."},
        {"\\\\foo\\bar\\baz-quux", "\\\\foo\\bar\\baz", "..\\baz"},
        {"\\\\foo\\bar\\baz", "\\\\foo\\bar\\baz-quux", "..\\baz-quux"},
        {"C:\\baz-quux", "C:\\baz", "..\\baz"},
        {"C:\\baz", "C:\\baz-quux", "..\\baz-quux"},
        {"\\\\foo\\baz-quux", "\\\\foo\\baz", "..\\baz"},
        {"\\\\foo\\baz", "\\\\foo\\baz-quux", "..\\baz-quux"},
        {"C:\\baz", "\\\\foo\\bar\\baz", "\\\\foo\\bar\\baz"},
        {"\\\\foo\\bar\\baz", "C:\\baz", "C:\\baz"},
        {"C:\\dev\\test", "C:\\dev\\test\\hello.test.ts", "hello.test.ts"},  // browserify only
    };
    run_relative(kWin, true, "relative.test.js>win32");

    // source: relative.test.js > test("general") (posix)
    constexpr RelativeCase kPosix[]{
        {"/var/lib", "/var", ".."},
        {"/var/lib", "/bin", "../../bin"},
        {"/var/lib", "/var/lib", ""},
        {"/var/lib", "/var/apache", "../apache"},
        {"/var/", "/var/lib", "lib"},
        {"/", "/var/lib", "var/lib"},
        {"/foo/test", "/foo/test/bar/package.json", "bar/package.json"},
        {"/Users/a/web/b/test/mails", "/Users/a/web/b", "../.."},
        {"/foo/bar/baz-quux", "/foo/bar/baz", "../baz"},
        {"/foo/bar/baz", "/foo/bar/baz-quux", "../baz-quux"},
        {"/baz-quux", "/baz", "../baz"},
        {"/baz", "/baz-quux", "../baz-quux"},
        {"/page1/page2/foo", "/", "../../.."},
    };
    run_relative(kPosix, false, "relative.test.js>posix");

    // source: browserify.test.js > it("path.relative") (posix additions)
    // cwd-dependent expectations derived with the test's own formulas for the
    // explicit given cwd POSIX_CWD = "/home/user/mbun" (see header):
    //   posix.resolve(".") === cwd, cwdParent === "/home/user",
    //   parentIsRoot() === false, parentIsRoot(2) === false
    constexpr RelativeCase kBrowserifyPosix[]{
        {POSIX_CWD, "foo", "foo"},  // [path.posix.resolve("."), "foo", "foo"]
        {"/webpack", "/webpack", ""},
        {"/webpack/", "/webpack", ""},
        {"/webpack", "/webpack/", ""},
        {"/webpack/", "/webpack/", ""},
        {"/webpack-hot-middleware", "/webpack/buildin/module.js", "../webpack/buildin/module.js"},
        {"/webp4ck-hot-middleware", "/webpack/buildin/module.js", "../webpack/buildin/module.js"},
        {"/webpack-hot-middleware", "/webp4ck/buildin/module.js", "../webp4ck/buildin/module.js"},
        {"/var/webpack-hot-middleware", "/var/webpack/buildin/module.js",
         "../webpack/buildin/module.js"},
        // "../../.." + posix.resolve("../") + "/static"
        {"/app/node_modules/pkg", "../static", "../../../home/user/static"},
        // "../../.." + posix.resolve("../../") + "/static"
        {"/app/node_modules/pkg", "../../static", "../../../home/static"},
        // ".." + posix.resolve("../") + "/static"
        {"/app", "../static", "../home/user/static"},
        {".", "../static", "../static"},  // cwd !== "/"
        // (posix.resolve("../") + "/static").slice(1)
        {"/", "../static", "home/user/static"},
        {"../", "../", ""},
        {"../", "../../", ".."},
        {"../../", "../", "user"},  // path.basename(cwdParent)
        {"../../", "../../", ""},
    };
    run_relative(kBrowserifyPosix, false, "browserify>relative(posix)");

    // ["/app", "../".repeat(64) + "static", "../static"]
    {
        std::string to;
        for (int i{0}; i < 64; ++i) {
            to += "../";
        }
        to += "static";
        check_str(posix::relative("/app", to, POSIX_CWD), "../static",
                  "[browserify>relative(posix)] /app vs ../ *64 + static");
    }

    // source: zero-length-strings.test.js (relative part; platform === posix)
    check_str(posix::relative("", POSIX_CWD, POSIX_CWD), "", "[zero-length] relative(\"\",cwd)");
    check_str(posix::relative(POSIX_CWD, "", POSIX_CWD), "", "[zero-length] relative(cwd,\"\")");
    check_str(posix::relative(POSIX_CWD, POSIX_CWD, POSIX_CWD), "",
              "[zero-length] relative(cwd,cwd)");
}

void test_relative_long() {
    // source: relative.test.js > test("very long paths") (platform === posix)
    std::string longPath1{"/home/"};
    longPath1.append(50000, 'a');
    std::string longPath2{"/home/"};
    longPath2.append(50000, 'b');
    std::string result{posix::relative(longPath1, longPath2, POSIX_CWD)};
    check_bool(result.starts_with(".."), true, "[relative>long] starts with ..");
    check_bool(result.contains('b'), true, "[relative>long] contains b");
}

// ---------------------------------------------------------------------------
// parse / format
// ---------------------------------------------------------------------------

struct Api {
    std::string_view osName;
    ParsedPath (*parse)(std::string_view);
    std::string (*format)(const ParsedPath&);
    std::string (*dirname)(std::string_view);
    std::string (*basename)(std::string_view);
    std::string (*extname)(std::string_view);
};

const Api kPosixApi{
    "posix",
    &posix::parse,
    &posix::format,
    &posix::dirname,
    [](std::string_view p) { return posix::basename(p); },
    &posix::extname,
};

const Api kWin32Api{
    "win32",
    &win32::parse,
    &win32::format,
    &win32::dirname,
    [](std::string_view p) { return win32::basename(p); },
    &win32::extname,
};

// source: parse-format.test.js > checkParseFormat() invariants
// (typeof checks are statically guaranteed by ParsedPath)
void check_parse_format(const Api& api, std::string_view element, std::string_view root) {
    ParsedPath output{api.parse(element)};
    std::string what{std::format("{}.parse(\"{}\")", api.osName, element)};
    check_str(api.format(output), element, std::format("[parse-format] format({})", what));
    check_str(output.root, root, std::format("[parse-format] {}.root", what));
    check_bool(output.dir.starts_with(output.root), true,
               std::format("[parse-format] {}.dir startsWith root", what));
    check_str(output.dir, output.dir.empty() ? std::string{} : api.dirname(element),
              std::format("[parse-format] {}.dir == dirname", what));
    check_str(output.base, api.basename(element), std::format("[parse-format] {}.base", what));
    check_str(output.ext, api.extname(element), std::format("[parse-format] {}.ext", what));
}

void check_parsed(const ParsedPath& actual, const ParsedPath& expected, std::string what) {
    check_str(actual.root, expected.root, what + ".root");
    check_str(actual.dir, expected.dir, what + ".dir");
    check_str(actual.base, expected.base, what + ".base");
    check_str(actual.ext, expected.ext, what + ".ext");
    check_str(actual.name, expected.name, what + ".name");
}

void test_parse_format_invariants() {
    // source: parse-format.test.js > describe("path.parse") > test("general") winPaths
    constexpr PairCase kWinPaths[]{
        {"C:\\path\\dir\\index.html", "C:\\"},
        {"C:\\another_path\\DIR\\1\\2\\33\\\\index", "C:\\"},
        {"another_path\\DIR with spaces\\1\\2\\33\\index", ""},
        {"\\", "\\"},
        {"\\foo\\C:", "\\"},
        {"file", ""},
        {"file:stream", ""},
        {".\\file", ""},
        {"C:", "C:"},
        {"C:.", "C:"},
        {"C:..", "C:"},
        {"C:abc", "C:"},
        {"C:\\", "C:\\"},
        {"C:\\abc", "C:\\"},
        {"", ""},
        // unc
        {"\\\\server\\share\\file_path", "\\\\server\\share\\"},
        {"\\\\server two\\shared folder\\file path.zip", "\\\\server two\\shared folder\\"},
        {"\\\\teela\\admin$\\system32", "\\\\teela\\admin$\\"},
        {"\\\\?\\UNC\\server\\share", "\\\\?\\UNC\\"},
    };
    for (const auto& c : kWinPaths) {
        check_parse_format(kWin32Api, c.input, c.expected);
    }

    // source: parse-format.test.js > test("general") unixPaths
    constexpr PairCase kUnixPaths[]{
        {"/home/user/dir/file.txt", "/"},
        {"/home/user/a dir/another File.zip", "/"},
        {"/home/user/a dir//another&File.", "/"},
        {"/home/user/a$$$dir//another File.zip", "/"},
        {"user/dir/another File.zip", ""},
        {"file", ""},
        {".\\file", ""},
        {"./file", ""},
        {"C:\\foo", ""},
        {"/", "/"},
        {"", ""},
        {".", ""},
        {"..", ""},
        {"/foo", "/"},
        {"/foo.", "/"},
        {"/foo.bar", "/"},
        {"/.", "/"},
        {"/.foo", "/"},
        {"/.foo.bar", "/"},
        {"/foo/bar.baz", "/"},
    };
    for (const auto& c : kUnixPaths) {
        check_parse_format(kPosixApi, c.input, c.expected);
    }
}

void test_parse_special() {
    // source: parse-format.test.js > winSpecialCaseParseTests
    check_parsed(win32::parse("t"),
                 ParsedPath{.root = "", .dir = "", .base = "t", .ext = "", .name = "t"},
                 "[parse-format>win-special] win32.parse(\"t\")");
    check_parsed(win32::parse("/foo/bar"),
                 ParsedPath{.root = "/", .dir = "/foo", .base = "bar", .ext = "", .name = "bar"},
                 "[parse-format>win-special] win32.parse(\"/foo/bar\")");

    // source: parse-format.test.js > "Test removal of trailing path separators"
    struct TrailingCase {
        std::string_view input;
        ParsedPath expected;
    };
    const TrailingCase kWinTrailing[]{
        {".\\", {.root = "", .dir = "", .base = ".", .ext = "", .name = "."}},
        {"\\\\", {.root = "\\", .dir = "\\", .base = "", .ext = "", .name = ""}},
        {"\\\\", {.root = "\\", .dir = "\\", .base = "", .ext = "", .name = ""}},
        {"c:\\foo\\\\\\", {.root = "c:\\", .dir = "c:\\", .base = "foo", .ext = "", .name = "foo"}},
        {"D:\\foo\\\\\\bar.baz",
         {.root = "D:\\", .dir = "D:\\foo\\\\", .base = "bar.baz", .ext = ".baz", .name = "bar"}},
    };
    for (const auto& c : kWinTrailing) {
        check_parsed(win32::parse(c.input), c.expected,
                     std::format("[parse-format>trailing] win32.parse(\"{}\")", c.input));
    }
    const TrailingCase kPosixTrailing[]{
        {"./", {.root = "", .dir = "", .base = ".", .ext = "", .name = "."}},
        {"//", {.root = "/", .dir = "/", .base = "", .ext = "", .name = ""}},
        {"///", {.root = "/", .dir = "/", .base = "", .ext = "", .name = ""}},
        {"/foo///", {.root = "/", .dir = "/", .base = "foo", .ext = "", .name = "foo"}},
        {"/foo///bar.baz",
         {.root = "/", .dir = "/foo//", .base = "bar.baz", .ext = ".baz", .name = "bar"}},
    };
    for (const auto& c : kPosixTrailing) {
        check_parsed(posix::parse(c.input), c.expected,
                     std::format("[parse-format>trailing] posix.parse(\"{}\")", c.input));
    }
}

void test_format_special() {
    // source: parse-format.test.js > winSpecialCaseFormatTests
    check_str(win32::format({.dir = "some\\dir"}), "some\\dir\\", "[format>win] {dir}");
    check_str(win32::format({.base = "index.html"}), "index.html", "[format>win] {base}");
    check_str(win32::format({.root = "C:\\"}), "C:\\", "[format>win] {root}");
    check_str(win32::format({.ext = ".html", .name = "index"}), "index.html",
              "[format>win] {name,ext}");
    check_str(win32::format({.dir = "some\\dir", .ext = ".html", .name = "index"}),
              "some\\dir\\index.html", "[format>win] {dir,name,ext}");
    check_str(win32::format({.root = "C:\\", .ext = ".html", .name = "index"}), "C:\\index.html",
              "[format>win] {root,name,ext}");
    check_str(win32::format({}), "", "[format>win] {}");

    // source: parse-format.test.js > unixSpecialCaseFormatTests
    check_str(posix::format({.dir = "some/dir"}), "some/dir/", "[format>unix] {dir}");
    check_str(posix::format({.base = "index.html"}), "index.html", "[format>unix] {base}");
    check_str(posix::format({.root = "/"}), "/", "[format>unix] {root}");
    check_str(posix::format({.ext = ".html", .name = "index"}), "index.html",
              "[format>unix] {name,ext}");
    check_str(posix::format({.dir = "some/dir", .ext = ".html", .name = "index"}),
              "some/dir/index.html", "[format>unix] {dir,name,ext}");
    check_str(posix::format({.root = "/", .ext = ".html", .name = "index"}), "/index.html",
              "[format>unix] {root,name,ext}");
    check_str(posix::format({}), "", "[format>unix] {}");

    // source: parse-format.test.js > nodejs/node#44343 (platform === posix)
    check_str(posix::format({.ext = "png", .name = "x"}), "x.png", "[format>44343] ext w/o dot");
    check_str(posix::format({.ext = ".png", .name = "x"}), "x.png", "[format>44343] ext w/ dot");

    // source: parse-format.test.js > describe("path.format") > "formats { dir, name,
    // ext } with a dot-less ext when the result exceeds the platform path limit"
    // (run in-process; the subprocess existed only for crash isolation)
    {
        std::string dir{"/"};
        dir.append(70000, 'd');
        std::string name(70000, 'n');
        std::string posixExpected{dir + "/" + name + ".txt"};
        std::string winExpected{dir + "\\" + name + ".txt"};
        check_str(posix::format({.dir = dir, .ext = "txt", .name = name}), posixExpected,
                  "[format>huge] posix dot-less ext");
        check_str(win32::format({.dir = dir, .ext = "txt", .name = name}), winExpected,
                  "[format>huge] win32 dot-less ext");
        check_str(posix::format({.dir = "/tmp", .ext = "txt", .name = "file"}), "/tmp/file.txt",
                  "[format>huge] small case");
    }

    // source: browserify.test.js > "empty string arguments, issue #4005"
    check_str(posix::format({.root = "", .dir = "", .base = "", .ext = ".ts", .name = "foo"}),
              "foo.ts", "[format>#4005] explicit empty strings");
    check_str(posix::format({.ext = ".ts", .name = "foo"}), "foo.ts", "[format>#4005] name+ext");

    // source: browserify.test.js > test("path.format works for vite's example")
    // (platform === posix; base: undefined maps to empty base)
    check_str(posix::format({.root = "", .dir = "", .ext = ".css", .name = "index"}), "index.css",
              "[format>vite] {name,ext}");
}

void test_parse_name_root() {
    // source: browserify.test.js > it("path.parse().name") (platform === posix on
    // linux; path.parse(file).name SKIPPED — __filename)
    constexpr PairCase kPosixNames[]{
        {".js", ".js"},
        {"..js", "."},
        {"", ""},
        {".", "."},
        {"dir/name.ext", "name"},
        {"/dir/name.ext", "name"},
        {"/name.ext", "name"},
        {"name.ext", "name"},
        {"name.ext/", "name"},
        {"name.ext//", "name"},
        {"aaa/bbb", "bbb"},
        {"aaa/bbb/", "bbb"},
        {"aaa/bbb//", "bbb"},
        {"/aaa/bbb", "bbb"},
        {"/aaa/bbb/", "bbb"},
        {"/aaa/bbb//", "bbb"},
        {"///aaa", "aaa"},
        {"//aaa", "aaa"},
        {"/aaa", "aaa"},
        {"aaa.", "aaa"},
        // Windows parses these as UNC roots; posix keeps the name
        {"//aaa/bbb", "bbb"},
        {"//aaa/bbb/", "bbb"},
        {"//aaa/bbb//", "bbb"},
        // On unix a backslash is just another character
        {"\\dir\\name.ext", "\\dir\\name"},
        {"\\name.ext", "\\name"},
        {"name.ext", "name"},
        {"name.ext\\", "name"},
        {"name.ext\\\\", "name"},
    };
    for (const auto& c : kPosixNames) {
        check_str(posix::parse(c.input).name, c.expected,
                  std::format("[browserify>parse().name] posix.parse(\"{}\").name", c.input));
    }
    check_str(win32::parse("//aaa/bbb").name, "",
              "[browserify>parse().name] win32.parse(\"//aaa/bbb\").name");
    check_str(win32::parse("//aaa/bbb/").name, "",
              "[browserify>parse().name] win32.parse(\"//aaa/bbb/\").name");
    check_str(win32::parse("//aaa/bbb//").name, "",
              "[browserify>parse().name] win32.parse(\"//aaa/bbb//\").name");

    // source: browserify.test.js > it("path.parse() windows edition")
    constexpr PairCase kWinNames[]{
        {"\\dir\\name.ext", "name"}, {"\\name.ext", "name"},         {"name.ext", "name"},
        {"name.ext\\", "name"},      {"name.ext\\\\", "name"},       {"name", "name"},
        {".name", ".name"},          {"file:stream", "file:stream"},
    };
    for (const auto& c : kWinNames) {
        check_str(win32::parse(c.input).name, c.expected,
                  std::format("[browserify>parse()win] win32.parse(\"{}\").name", c.input));
    }

    // source: browserify.test.js > it("path.parse() windows edition - drive letter")
    constexpr PairCase kWinDriveNames[]{
        {"C:", ""},
        {"C:.", "."},
        {"C:\\", ""},
        {"C:\\.", "."},
        {"C:\\.ext", ".ext"},
        {"C:\\dir\\name.ext", "name"},
        {"C:name.ext", "name"},
        {"C:name.ext\\", "name"},
        {"C:name.ext\\\\", "name"},
        {"C:foo", "foo"},
        {"C:.foo", ".foo"},
    };
    for (const auto& c : kWinDriveNames) {
        check_str(win32::parse(c.input).name, c.expected,
                  std::format("[browserify>parse()drive] win32.parse(\"{}\").name", c.input));
    }

    // source: browserify.test.js > it("path.parse() windows edition - .root")
    constexpr PairCase kWinRoots[]{
        {"C:", "C:"},         {"C:.", "C:"},          {"C:\\", "C:\\"},
        {"C:\\.", "C:\\"},    {"C:\\.ext", "C:\\"},   {"C:\\dir\\name.ext", "C:\\"},
        {"C:name.ext", "C:"}, {"C:name.ext\\", "C:"}, {"C:name.ext\\\\", "C:"},
        {"C:foo", "C:"},      {"C:.foo", "C:"},       {"/:.foo", "/"},
    };
    for (const auto& c : kWinRoots) {
        check_str(win32::parse(c.input).root, c.expected,
                  std::format("[browserify>parse().root] win32.parse(\"{}\").root", c.input));
    }
}

void test_posix_parse_format_cases() {
    // source: browserify.test.js > describe("path.posix.parse and path.posix.format")
    struct Case {
        std::string_view input;
        ParsedPath expected;
    };
    const Case kCases[]{
        {"/tmp/test.txt",
         {.root = "/", .dir = "/tmp", .base = "test.txt", .ext = ".txt", .name = "test"}},
        {"/tmp/test/file.txt",
         {.root = "/", .dir = "/tmp/test", .base = "file.txt", .ext = ".txt", .name = "file"}},
        {"/tmp/test/dir",
         {.root = "/", .dir = "/tmp/test", .base = "dir", .ext = "", .name = "dir"}},
        {"/tmp/test/dir/",
         {.root = "/", .dir = "/tmp/test", .base = "dir", .ext = "", .name = "dir"}},
        {".", {.root = "", .dir = "", .base = ".", .ext = "", .name = "."}},
        {"./", {.root = "", .dir = "", .base = ".", .ext = "", .name = "."}},
        {"/.", {.root = "/", .dir = "/", .base = ".", .ext = "", .name = "."}},
        {"/../", {.root = "/", .dir = "/", .base = "..", .ext = ".", .name = "."}},
        {"./file.txt", {.root = "", .dir = ".", .base = "file.txt", .ext = ".txt", .name = "file"}},
        {"../file.txt",
         {.root = "", .dir = "..", .base = "file.txt", .ext = ".txt", .name = "file"}},
        {"../test/file.txt",
         {.root = "", .dir = "../test", .base = "file.txt", .ext = ".txt", .name = "file"}},
        {"test/file.txt",
         {.root = "", .dir = "test", .base = "file.txt", .ext = ".txt", .name = "file"}},
        {"test/dir", {.root = "", .dir = "test", .base = "dir", .ext = "", .name = "dir"}},
        {"test/dir/another_dir",
         {.root = "", .dir = "test/dir", .base = "another_dir", .ext = "", .name = "another_dir"}},
        {"./dir", {.root = "", .dir = ".", .base = "dir", .ext = "", .name = "dir"}},
        {"../dir", {.root = "", .dir = "..", .base = "dir", .ext = "", .name = "dir"}},
        {"../dir/another_dir",
         {.root = "", .dir = "../dir", .base = "another_dir", .ext = "", .name = "another_dir"}},
        // oven-sh/bun#4954
        {"/test/Ł.txt", {.root = "/", .dir = "/test", .base = "Ł.txt", .ext = ".txt", .name = "Ł"}},
        // oven-sh/bun#8090
        {".prettierrc",
         {.root = "", .dir = "", .base = ".prettierrc", .ext = "", .name = ".prettierrc"}},
    };
    for (const auto& c : kCases) {
        ParsedPath parsed{posix::parse(c.input)};
        check_parsed(parsed, c.expected,
                     std::format("[browserify>posix-parse] posix.parse(\"{}\")", c.input));
        std::string expectedFormat{c.input};
        if (expectedFormat.ends_with('/')) {
            expectedFormat.pop_back();
        }
        check_str(posix::format(parsed), expectedFormat,
                  std::format("[browserify>posix-parse] format(parse(\"{}\"))", c.input));
    }
}

}  // namespace

int main() {
    test_sep_delimiter();
    test_is_absolute();
    test_basename();
    test_dirname();
    test_extname();
    test_join();
    test_join_long();
    test_normalize();
    test_normalize_long();
    test_resolve();
    test_resolve_long();
    test_relative();
    test_relative_long();
    test_parse_format_invariants();
    test_parse_special();
    test_format_special();
    test_parse_name_root();
    test_posix_parse_format_cases();

    if (gFailures > MAX_FAILURE_PRINTS) {
        std::println("  ... {} more failures not shown", gFailures - MAX_FAILURE_PRINTS);
    }
    std::println("test_core_paths: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
