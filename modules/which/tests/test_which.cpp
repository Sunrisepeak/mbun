// Engine-level tests for mbun.which, off the JS engine. The semantics mirror
// bun's `src/which/lib.rs` (compat/bun/src/which/lib.rs): PATH iteration order,
// "absolute / slash-bearing names are never looked up in $PATH", the POSIX
// executable-bit probe and the Windows .exe/.cmd/.bat extension search order.
//
// The module takes its filesystem probes through the injected `Fs` struct, so
// these tests bind those probes to a REAL throwaway directory tree created
// under the system temp dir (unique per process, removed at exit). The Windows
// arm builds candidates with '\' separators; the probe adapter maps them back
// to '/' so the same on-disk tree drives both platform algorithms.
import std;
import mbun.which;

namespace {

using namespace mbun::which;

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

void check_some(const std::optional<std::string>& actual, std::string_view expected,
                std::string_view label) {
    if (!actual) {
        ++checks;
        ++failures;
        std::println("FAIL: {}\n  expected: {}\n  actual:   <none>", label, expected);
        return;
    }
    check_eq(*actual, expected, label);
}

void check_none(const std::optional<std::string>& actual, std::string_view label) {
    ++checks;
    if (actual) {
        ++failures;
        std::println("FAIL: {}\n  expected: <none>\n  actual:   {}", label, *actual);
    }
}

// ---------------------------------------------------------------------------
// Throwaway on-disk tree of fake executables.
// ---------------------------------------------------------------------------

namespace fs = std::filesystem;

struct TempTree {
    fs::path root;

    TempTree() {
        // Unique per run (random + clock) so parallel runs never collide.
        std::random_device rd;
        const auto stamp {
            static_cast<unsigned long long>(
                std::chrono::steady_clock::now().time_since_epoch().count())
        };
        root = fs::temp_directory_path() / std::format("mbun-which-{:x}-{:x}", rd(), stamp);
        fs::create_directories(root);
    }

    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    std::string dir(std::string_view rel) {
        fs::path p { root / rel };
        fs::create_directories(p);
        return p.string();
    }

    // Creates `rel` as a file; `executable` adds the exec bits (chmod +x).
    std::string file(std::string_view rel, bool executable) {
        fs::path p { root / rel };
        fs::create_directories(p.parent_path());
        {
            std::ofstream out { p };
            out << "#!/bin/sh\nexit 0\n";
        }
        fs::permissions(p,
                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        executable ? fs::perm_options::add : fs::perm_options::remove);
        return p.string();
    }
};

// POSIX probe: regular file (after symlink resolution) carrying an exec bit.
bool real_is_executable_file(std::string_view path) {
    std::error_code ec;
    fs::path p { std::string { path } };
    if (!fs::is_regular_file(p, ec) || ec) {
        return false;
    }
    const auto st { fs::status(p, ec) };
    if (ec) {
        return false;
    }
    const auto perms { st.permissions() };
    return (perms & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) !=
           fs::perms::none;
}

// Windows probe: "does this path exist as a file". The Windows arm builds
// candidates with '\', which is a legal filename byte on Linux, so translate
// back to '/' before hitting the real tree.
bool real_exists_os_path(std::string_view path) {
    std::string translated { path };
    for (char& c : translated) {
        if (c == '\\') {
            c = '/';
        }
    }
    std::error_code ec;
    const bool ok { fs::is_regular_file(fs::path { translated }, ec) };
    return ok && !ec;
}

Fs make_fs() {
    return Fs {
        .is_executable_file_path = real_is_executable_file,
        .exists_os_path = real_exists_os_path,
    };
}

// Expected shape of a path the Windows arm produced from the cwd branch: bun
// runs posix_to_platform_in_place over that result, so every separator is '\'.
std::string to_win(std::string_view path) {
    std::string out { path };
    for (char& c : out) {
        if (c == '/') {
            c = '\\';
        }
    }
    return out;
}

// Expected shape of a $PATH-branch result: bun does NOT normalize separators
// there, so the segment keeps the caller's bytes and only the joining
// separator is '\'.
std::string win_join(std::string_view dir, std::string_view name) {
    return std::format("{}\\{}", dir, name);
}

std::string join_path(std::initializer_list<std::string_view> segments) {
    std::string out;
    for (std::string_view s : segments) {
        if (!out.empty()) {
            out.push_back(':');
        }
        out.append(s);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_posix_path_lookup(TempTree& tree) {
    const std::string bin1 { tree.dir("bin1") };
    const std::string bin2 { tree.dir("bin2") };
    const std::string bin3 { tree.dir("bin3") };
    const std::string tool1 { tree.file("bin1/tool", true) };
    tree.file("bin2/tool", true);
    const std::string only3 { tree.file("bin3/only3", true) };

    const Fs fs { make_fs() };
    const std::string path { join_path({ bin1, bin2, bin3 }) };

    // 1. found in the FIRST PATH entry (earlier entries win).
    check_some(which(fs, path, "", "tool", Platform::Posix), tool1,
               "posix: bare name resolves to the first PATH entry");

    // 2. found in a LATER PATH entry.
    check_some(which(fs, path, "", "only3", Platform::Posix), only3,
               "posix: bare name resolves in a later PATH entry");

    // 3. not found anywhere -> none.
    check_none(which(fs, path, "", "definitely-not-here", Platform::Posix),
               "posix: missing bin returns none");

    // Empty bin is rejected up front (bun: `if bin.is_empty() { return None }`).
    check_none(which(fs, path, "", "", Platform::Posix), "posix: empty bin returns none");

    // Empty PATH segments are skipped, not joined as "/bin".
    const std::string paddedPath { std::format("::{}::{}::", bin1, bin3) };
    check_some(which(fs, paddedPath, "", "only3", Platform::Posix), only3,
               "posix: empty PATH segments are skipped");

    // Over-long bin is rejected before any probe.
    const std::string longBin(MAX_PATH_BYTES + 1, 'a');
    check_none(which(fs, path, "", longBin, Platform::Posix),
               "posix: bin longer than MAX_PATH_BYTES returns none");
}

void test_posix_non_executable_skipped(TempTree& tree) {
    const std::string bin1 { tree.dir("bin1") };
    const std::string bin2 { tree.dir("bin2") };
    const std::string nonexecDir { tree.dir("nonexec") };
    tree.file("bin1/shared", false);                                   // present, not +x
    const std::string shared2 { tree.file("bin2/shared", true) };      // present, +x
    tree.file("nonexec/plainfile", false);

    const Fs fs { make_fs() };

    // 4. a non-executable file in an earlier entry does not shadow the real one.
    check_some(which(fs, join_path({ bin1, bin2 }), "", "shared", Platform::Posix), shared2,
               "posix: non-executable file in an earlier PATH entry is skipped");

    // A file that is only ever non-executable is never resolved.
    check_none(which(fs, nonexecDir, "", "plainfile", Platform::Posix),
               "posix: non-executable file never resolves");

    // A directory with a matching name is not an executable either.
    tree.dir("bin1/dirname");
    check_none(which(fs, bin1, "", "dirname", Platform::Posix),
               "posix: directory with the bin's name is not resolved");
}

void test_posix_absolute_input(TempTree& tree) {
    const std::string absExec { tree.file("abs/real-tool", true) };
    const std::string absPlain { tree.file("abs/plain-tool", false) };
    const std::string absDir { tree.dir("abs") };
    const std::string bin1 { tree.dir("bin1") };

    const Fs fs { make_fs() };

    // 5. absolute path input: probed directly, returned verbatim.
    check_some(which(fs, "", "", absExec, Platform::Posix), absExec,
               "posix: absolute executable path resolves to itself");

    // Absolute but not executable -> none (and NOT retried in $PATH).
    check_none(which(fs, absDir, "", absPlain, Platform::Posix),
               "posix: absolute non-executable path returns none");

    // "Do not look absolute paths up in $PATH": /nope/tool must not find bin1/tool.
    tree.file("bin1/tool", true);
    check_none(which(fs, bin1, "", "/definitely/not/here/tool", Platform::Posix),
               "posix: absolute path is never looked up in $PATH");
}

void test_posix_relative_input(TempTree& tree) {
    const std::string cwd { tree.dir("cwd") };
    const std::string local { tree.file("cwd/local", true) };
    const std::string nested { tree.file("cwd/sub/nested", true) };
    const std::string bin1 { tree.dir("bin1") };
    tree.file("bin1/tool", true);

    const Fs fs { make_fs() };

    // 6. relative path input is resolved against cwd, with "./" stripped.
    check_some(which(fs, "", cwd, "./local", Platform::Posix), local,
               "posix: ./name resolves against cwd");
    check_some(which(fs, "", cwd, "sub/nested", Platform::Posix), nested,
               "posix: dir/name resolves against cwd");

    // Trailing separators on cwd are trimmed before joining.
    check_some(which(fs, "", cwd + "///", "./local", Platform::Posix), local,
               "posix: trailing slashes on cwd are trimmed");

    // A slash-bearing name is never looked up in $PATH.
    check_none(which(fs, bin1, cwd, "./tool", Platform::Posix),
               "posix: slash-bearing name is not looked up in $PATH");

    // No cwd -> nothing to resolve a relative slash-bearing name against.
    check_none(which(fs, bin1, "", "./tool", Platform::Posix),
               "posix: relative name with empty cwd returns none");
}

void test_posix_empty_path(TempTree& tree) {
    const std::string cwd { tree.dir("cwd") };
    tree.file("cwd/local", true);
    tree.file("bin1/tool", true);

    const Fs fs { make_fs() };

    // 7. empty PATH: bare names resolve nowhere, even with a populated cwd.
    check_none(which(fs, "", cwd, "tool", Platform::Posix),
               "posix: empty PATH returns none for a bare name");
    check_none(which(fs, "", cwd, "local", Platform::Posix),
               "posix: empty PATH does not fall back to cwd for a bare name");
    check_none(which(fs, ":::", cwd, "local", Platform::Posix),
               "posix: all-empty PATH segments return none");
}

void test_win_extension_probing(TempTree& tree) {
    const std::string winDir { tree.dir("win") };
    const std::string appExe { win_join(winDir, "app.exe") };
    tree.file("win/app.exe", true);
    tree.file("win/app.cmd", true);   // .exe must win the search order
    tree.file("win/only.cmd", true);
    tree.file("win/script.bat", true);
    tree.file("win/noext", true);

    const Fs fs { make_fs() };

    // 8. Windows extension probing: exe, then cmd, then bat.
    check_some(which(fs, winDir, "", "app", Platform::Win32), appExe,
               "win32: extensionless bin probes .exe first");
    check_some(which(fs, winDir, "", "only", Platform::Win32), win_join(winDir, "only.cmd"),
               "win32: extensionless bin falls back to .cmd");
    check_some(which(fs, winDir, "", "script", Platform::Win32), win_join(winDir, "script.bat"),
               "win32: extensionless bin falls back to .bat");

    // A file with no extension is not executable on Windows and is not probed bare.
    check_none(which(fs, winDir, "", "noext", Platform::Win32),
               "win32: extensionless file is not resolved");

    // A bin that already ends in a known extension is probed as-is only.
    check_some(which(fs, winDir, "", "app.exe", Platform::Win32), appExe,
               "win32: bin with .exe is probed verbatim");
    // The known-extension check is case-insensitive: an upper-case .EXE still
    // counts as "already has an extension" and is probed verbatim.
    tree.file("win/upper.EXE", true);
    check_some(which(fs, winDir, "", "upper.EXE", Platform::Win32), win_join(winDir, "upper.EXE"),
               "win32: known extension check is case-insensitive");
    check_none(which(fs, winDir, "", "missing.exe", Platform::Win32),
               "win32: bin with an extension is not extension-probed again");

    // PATH iteration uses ';' and skips empty segments.
    const std::string other { tree.dir("win2") };
    tree.file("win2/other.exe", true);
    const std::string winPath { std::format(";{};;{};", winDir, other) };
    check_some(which(fs, winPath, "", "other", Platform::Win32), win_join(other, "other.exe"),
               "win32: PATH is split on ';' with empty segments skipped");
    check_none(which(fs, winPath, "", "", Platform::Win32), "win32: empty bin returns none");
    check_none(which(fs, "", "", "app", Platform::Win32), "win32: empty PATH returns none");
}

void test_win_absolute_and_relative(TempTree& tree) {
    const std::string winDir { tree.dir("win") };
    tree.file("win/app.exe", true);
    tree.file("win/sub/deep.exe", true);

    const Fs fs { make_fs() };

    // Absolute bins are probed directly (with extension probing when bare).
    check_some(which(fs, "", "", winDir + "/app", Platform::Win32), winDir + "/app.exe",
               "win32: absolute extensionless bin gets extension probing");
    check_some(which(fs, "", "", winDir + "/app.exe", Platform::Win32), winDir + "/app.exe",
               "win32: absolute bin with extension resolves verbatim");
    check_none(which(fs, winDir, "", winDir + "/nope", Platform::Win32),
               "win32: missing absolute bin is not looked up in $PATH");

    // Slash-bearing relative names resolve against cwd and come back with '\'.
    check_some(which(fs, "", winDir, "./app", Platform::Win32), to_win(winDir + "/app.exe"),
               "win32: ./name resolves against cwd with backslashes");
    check_some(which(fs, "", winDir, "sub/deep", Platform::Win32), to_win(winDir + "/sub/deep.exe"),
               "win32: dir/name resolves against cwd");
    check_none(which(fs, winDir, "", "./app", Platform::Win32),
               "win32: slash-bearing name with empty cwd is not looked up in $PATH");
}

void test_which_for_spawn(TempTree& tree) {
    const std::string winDir { tree.dir("win") };
    const std::string otherDir { tree.dir("win2") };
    tree.file("win/app.exe", true);
    tree.file("win2/app.exe", true);
    const std::string cwd { tree.dir("cwd") };
    const std::string local { tree.file("cwd/local", true) };
    const std::string bin1 { tree.dir("bin1") };
    const std::string tool1 { tree.file("bin1/tool", true) };

    const Fs fs { make_fs() };

    // Windows spawn resolves a bare name against cwd BEFORE $PATH.
    check_some(which_for_spawn(fs, otherDir, winDir, "app", Platform::Win32),
               to_win(winDir + "/app.exe"),
               "win32 spawn: cwd is searched before $PATH");
    // ...unless the NoDefaultCurrentDirectoryInExePath opt-out is set.
    check_some(which_for_spawn(fs, otherDir, winDir, "app", Platform::Win32, true),
               win_join(otherDir, "app.exe"),
               "win32 spawn: noDefaultCwdInPath falls back to $PATH order");
    // With nothing in cwd it behaves like plain which().
    check_some(which_for_spawn(fs, otherDir, cwd, "app", Platform::Win32),
               win_join(otherDir, "app.exe"),
               "win32 spawn: falls through to $PATH when cwd has no match");

    // POSIX spawn is $PATH-only for bare names — cwd is never searched.
    check_none(which_for_spawn(fs, bin1, cwd, "local", Platform::Posix),
               "posix spawn: cwd is not searched for a bare name");
    check_some(which_for_spawn(fs, bin1, cwd, "tool", Platform::Posix), tool1,
               "posix spawn: bare name resolves from $PATH");
    check_eq(local, tree.root.string() + "/cwd/local", "temp tree layout is as expected");
}

void test_extension_predicates() {
    check(ends_with_extension("foo.exe"), "ends_with_extension: .exe");
    check(ends_with_extension("foo.CMD"), "ends_with_extension: .CMD (case-insensitive)");
    check(ends_with_extension("foo.bat"), "ends_with_extension: .bat");
    check(!ends_with_extension("foo.com"), "ends_with_extension: .com is not in the set");
    check(!ends_with_extension("foo"), "ends_with_extension: no extension");
    check(!ends_with_extension("exe"), "ends_with_extension: shorter than 4 bytes");
    // Bun's check is purely "byte -4 is '.' and the last 3 match", so a bare
    // ".exe" counts. Kept as a parity assertion, not an aspiration.
    check(ends_with_extension(".exe"), "ends_with_extension: bare '.exe' counts (bun parity)");

    check(is_batch_file("foo.cmd"), "is_batch_file: .cmd");
    check(is_batch_file("foo.BAT"), "is_batch_file: .BAT");
    check(is_batch_file("foo.cmd."), "is_batch_file: trailing period stripped (CVE-2024-43402)");
    check(is_batch_file("foo.cmd  "), "is_batch_file: trailing spaces stripped");
    check(!is_batch_file("foo.exe"), "is_batch_file: .exe is not a batch file");
    check(!is_batch_file("cmd"), "is_batch_file: bare name is not a batch file");

    check(batch_arg_has_cmd_metachars("a%PATH%"), "batch metachars: %");
    check(batch_arg_has_cmd_metachars("a&b"), "batch metachars: &");
    check(batch_arg_has_cmd_metachars("a\nb"), "batch metachars: newline");
    check(!batch_arg_has_cmd_metachars("plain-arg_1"), "batch metachars: clean arg");
}

}  // namespace

int main() {
    TempTree tree;
    test_posix_path_lookup(tree);
    test_posix_non_executable_skipped(tree);
    test_posix_absolute_input(tree);
    test_posix_relative_input(tree);
    test_posix_empty_path(tree);
    test_win_extension_probing(tree);
    test_win_absolute_and_relative(tree);
    test_which_for_spawn(tree);
    test_extension_predicates();
    std::println("test_which: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
