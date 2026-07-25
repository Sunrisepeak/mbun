// Engine-level tests for mbun.permission, off the JS engine.
//
// Every expectation here is lifted from node's own permission corpus so the
// model is scored against the same vectors the runtime will be:
//
//   test/parallel/test-permission-fs-wildcard.js       the two allow-lists and
//                                                     all 14 has() assertions
//   test/parallel/test-permission-drop-fs-read.js      "path drop is a no-op
//                                                     when * was granted"
//   test/parallel/test-permission-drop-fs-granted-path.js  the five drop cases
//   test/parallel/test-permission-drop-fs-scope.js     drop('fs') hits both trees
//   test/parallel/test-permission-has.js               unknown label -> false,
//                                                     has('fs') needs BOTH
//   test/parallel/test-permission-fs-traversal-path.js `..` must not escape
//
// The filesystem is faked: `isDir` answers from an explicit set, so the
// WildcardIfDir behaviour is tested deterministically instead of depending on
// what happens to exist on the build machine.
import std;
import mbun.permission;

namespace {

using namespace mbun::permission;

int checks{0};
int failures{0};

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
        std::println("FAIL: {} — got \"{}\", want \"{}\"", label, actual, expected);
    }
}

// Fix a model's cwd and directory probe so the tests are hermetic. Model owns a
// radix tree whose nodes live in its own arena, so it is deliberately neither
// copyable nor movable — configure it in place.
void init_model(Model& m, std::string cwd, std::set<std::string> dirs = {}) {
    m.set_cwd_provider([cwd] { return cwd; });
    m.set_is_dir_probe([dirs](const std::string& p) { return dirs.contains(p); });
}

void grant(Model& m, Scope s, std::initializer_list<std::string> paths) {
    std::vector<std::string> v{paths};
    m.apply(s, v);
}

// ── the scope table ─────────────────────────────────────────────────────────
void test_scope_table() {
    check(scope_from_label("fs") == Scope::FileSystem, "label fs");
    check(scope_from_label("fs.read") == Scope::FileSystemRead, "label fs.read");
    check(scope_from_label("fs.write") == Scope::FileSystemWrite, "label fs.write");
    check(scope_from_label("child") == Scope::ChildProcess, "label child");
    check(scope_from_label("worker") == Scope::WorkerThreads, "label worker");
    check(scope_from_label("wasi") == Scope::Wasi, "label wasi");
    check(scope_from_label("net") == Scope::Net, "label net");
    check(scope_from_label("addon") == Scope::Addon, "label addon");
    check(scope_from_label("ffi") == Scope::Ffi, "label ffi");
    check(scope_from_label("inspector") == Scope::Inspector, "label inspector");

    // test-permission-has: an unknown key and the *Name* form are both "not a
    // scope" — has('FileSystemWrite', ...) must answer false, not true.
    check(scope_from_label("invalid-key") == Scope::Root, "unknown label -> Root");
    check(scope_from_label("FileSystemWrite") == Scope::Root, "Name form is not a label");

    check_eq(scope_name(Scope::FileSystemRead), "FileSystemRead", "name fs.read");
    check_eq(scope_name(Scope::Wasi), "WASI", "name wasi");
    check_eq(scope_name(Scope::WorkerThreads), "WorkerThreads", "name worker");
    check_eq(scope_name(Scope::Ffi), "FFI", "name ffi");

    check_eq(scope_flag(Scope::FileSystemWrite), "--allow-fs-write", "flag fs.write");
    check_eq(scope_flag(Scope::FileSystem), "", "no --allow-fs exists");

    // The exact strings test-permission-child-process-cli / -wasi assert on.
    check_eq(access_denied_message(Scope::ChildProcess),
             "Access to this API has been restricted. Use --allow-child-process to manage permissions.",
             "child denial message");
    check_eq(access_denied_message(Scope::Wasi),
             "Access to this API has been restricted. Use --allow-wasi to manage permissions.",
             "wasi denial message");
    check_eq(access_denied_message(Scope::FileSystem),
             "Access to this API has been restricted.", "fs-root denial message");
}

// ── path.resolve / normalizeString ──────────────────────────────────────────
void test_path_resolve() {
    check_eq(path_resolve("/cwd", "/tmp/"), "/tmp", "trailing slash dropped");
    check_eq(path_resolve("/cwd", "."), "/cwd", "'.' is the cwd");
    check_eq(path_resolve("/cwd", "a/b"), "/cwd/a/b", "relative joins cwd");
    check_eq(path_resolve("/cwd", "../x"), "/x", "parent of cwd");
    check_eq(path_resolve("/a/b", "/a/b/c/../../d"), "/a/d", "traversal collapses");
    check_eq(path_resolve("/a/b", "/a/b/./c//d"), "/a/b/c/d", "dots and dup slashes");
    check_eq(path_resolve("/a", "/"), "/", "root stays root");
    check_eq(path_resolve("/a", "/tmp/*"), "/tmp/*", "'*' is an ordinary char here");
    check_eq(path_resolve("/a", "/x/../../../y"), "/y", "cannot escape root");
}

// ── test-permission-fs-wildcard.js, first allow-list ────────────────────────
void test_wildcard_primary() {
    Model m{};
    init_model(m, "/cwd");
    m.enable();
    grant(m, Scope::FileSystemRead,
          {"/tmp/*", "/example/foo*", "/example/bar*", "/folder/*", "/show", "/slower",
           "/slown", "/home/foo/*", "/files/index.js", "/files/index.json", "/files/i"});

    check(!m.is_granted(Scope::FileSystemRead, "/slow"), "wildcard: !/slow");
    check(!m.is_granted(Scope::FileSystemRead, "/slows"), "wildcard: !/slows");
    check(m.is_granted(Scope::FileSystemRead, "/slown"), "wildcard: /slown");
    check(m.is_granted(Scope::FileSystemRead, "/home/foo"), "wildcard: /home/foo");
    check(m.is_granted(Scope::FileSystemRead, "/home/foo/"), "wildcard: /home/foo/");
    check(!m.is_granted(Scope::FileSystemRead, "/home/fo"), "wildcard: !/home/fo");
    check(m.is_granted(Scope::FileSystemRead, "/files/index.js"), "wildcard: /files/index.js");
    check(m.is_granted(Scope::FileSystemRead, "/files/index.json"), "wildcard: /files/index.json");
    check(!m.is_granted(Scope::FileSystemRead, "/files/index.j"), "wildcard: !/files/index.j");
    check(m.is_granted(Scope::FileSystemRead, "/files/i"), "wildcard: /files/i");

    // Subtree coverage of an explicit "/*" grant.
    check(m.is_granted(Scope::FileSystemRead, "/tmp/anything/deep"), "wildcard: /tmp subtree");
    // A read grant says nothing about writes.
    check(!m.is_granted(Scope::FileSystemWrite, "/tmp/anything"), "read grant is not a write grant");
}

// ── test-permission-fs-wildcard.js, second allow-list ───────────────────────
void test_wildcard_secondary() {
    Model m{};
    init_model(m, "/cwd");
    m.enable();
    grant(m, Scope::FileSystemRead,
          {"/a/b/*", "/a/b/d", "/etc/passwd.*", "/home/*.js"});

    check(m.is_granted(Scope::FileSystemRead, "/a/b/c"), "wildcard2: /a/b/c");
    check(!m.is_granted(Scope::FileSystemRead, "/a/c/c"), "wildcard2: !/a/c/c");
    check(!m.is_granted(Scope::FileSystemRead, "/etc/passwd"), "wildcard2: !/etc/passwd");
    // A trailing "*" is greedy: "/home/*.js" grants all of /home/.
    check(m.is_granted(Scope::FileSystemRead, "/home/another-file.md"),
          "wildcard2: /home/another-file.md");
}

// ── directory grants (WildcardIfDir) and traversal ──────────────────────────
void test_directory_grant_and_traversal() {
    Model m{};
    init_model(m, "/cwd", {"/tmp/sub"});
    m.enable();
    grant(m, Scope::FileSystemRead, {"/tmp/sub"});

    check(m.is_granted(Scope::FileSystemRead, "/tmp/sub/file"), "dir grant covers children");
    check(m.is_granted(Scope::FileSystemRead, "/tmp/sub"), "dir grant covers itself");
    check(!m.is_granted(Scope::FileSystemRead, "/tmp/other"), "dir grant is scoped");
    // test-permission-fs-traversal-path: `..` is normalised BEFORE the lookup,
    // so a path that walks out of the granted subtree is denied.
    check(!m.is_granted(Scope::FileSystemRead, "/tmp/sub/../secret"), "traversal denied");
    check(!m.is_granted(Scope::FileSystemRead, "/tmp/sub/deep/../../secret"),
          "deep traversal denied");
}

// ── wildcard-everything and has() with no reference ─────────────────────────
void test_allow_all() {
    Model m{};
    init_model(m, "/cwd");
    m.enable();
    grant(m, Scope::FileSystemRead, {"*"});

    check(m.is_granted(Scope::FileSystemRead), "has('fs.read') with *");
    check(m.is_granted(Scope::FileSystemRead, "/anything"), "* grants everything");
    check(!m.is_granted(Scope::FileSystemWrite), "has('fs.write') without the flag");
    // test-permission-has: has('fs') requires BOTH halves to be *.
    check(!m.is_granted(Scope::FileSystem), "has('fs') needs read AND write");

    grant(m, Scope::FileSystemWrite, {"*"});
    check(m.is_granted(Scope::FileSystem), "has('fs') once both are *");
}

// test-permission-has's exact flag combination: --allow-fs-read=* --allow-fs-write=.
void test_has_matrix() {
    Model m{};
    init_model(m, "/cwd", {"/cwd"});
    m.enable();
    grant(m, Scope::FileSystemRead, {"*"});
    grant(m, Scope::FileSystemWrite, {"."});
    // Every scope with no --allow flag is denied.
    m.apply(Scope::ChildProcess, std::vector<std::string>{"*"});
    m.apply(Scope::WorkerThreads, std::vector<std::string>{"*"});
    m.apply(Scope::Wasi, std::vector<std::string>{"*"});
    m.apply(Scope::Inspector, std::vector<std::string>{"*"});
    m.apply(Scope::Addon, std::vector<std::string>{"*"});
    m.apply(Scope::Ffi, std::vector<std::string>{"*"});

    check(!m.is_granted(Scope::FileSystem), "has-matrix: !fs");
    check(m.is_granted(Scope::FileSystemRead), "has-matrix: fs.read");
    // `--allow-fs-write=.` is a path grant, not "*", so the reference-less query
    // (allow_all_out_) is false — exactly what node reports.
    check(!m.is_granted(Scope::FileSystemWrite), "has-matrix: !fs.write");
    check(m.is_granted(Scope::FileSystemWrite, "/cwd/file"), "has-matrix: cwd is writable");
    check(!m.is_granted(Scope::Wasi), "has-matrix: !wasi");
    check(!m.is_granted(Scope::WorkerThreads), "has-matrix: !worker");
    check(!m.is_granted(Scope::Inspector), "has-matrix: !inspector");
    check(!m.is_granted(Scope::Net), "has-matrix: !net");
    check(!m.is_granted(Scope::Addon), "has-matrix: !addon");
    check(!m.is_granted(Scope::Ffi), "has-matrix: !ffi");
    check(!m.is_granted(Scope::ChildProcess), "has-matrix: !child");
    check(!m.is_granted(Scope::Root, "x"), "has-matrix: unknown scope denied");
}

// ── the single-state scopes' inverted Apply ─────────────────────────────────
void test_single_state_scopes() {
    Model m{};
    init_model(m, "/cwd");
    m.enable();
    // Nothing applied yet: node's deny_all_ starts false, i.e. granted.
    check(m.is_granted(Scope::ChildProcess), "child granted before Apply");
    m.apply(Scope::ChildProcess, std::vector<std::string>{"*"});
    check(!m.is_granted(Scope::ChildProcess), "Apply DENIES child");

    // Net is inverted: Apply grants it.
    check(!m.is_granted(Scope::Net), "net denied before Apply");
    m.apply(Scope::Net, std::vector<std::string>{"*"});
    check(m.is_granted(Scope::Net), "Apply GRANTS net");
    m.drop(Scope::Net);
    check(!m.is_granted(Scope::Net), "drop revokes net");

    // Once denied, always denied — a second Apply cannot re-grant.
    m.apply(Scope::WorkerThreads, std::vector<std::string>{"*"});
    m.apply(Scope::WorkerThreads, std::vector<std::string>{"*"});
    check(!m.is_granted(Scope::WorkerThreads), "worker stays denied");
}

// ── test-permission-drop-fs-read / -write / -scope ──────────────────────────
void test_drop_wildcard_scopes() {
    {
        Model m{};
    init_model(m, "/cwd");
        m.enable();
        grant(m, Scope::FileSystemRead, {"*"});
        grant(m, Scope::FileSystemWrite, {"*"});

        // A specific-path drop against a "*" grant is a no-op.
        m.drop(Scope::FileSystemRead, "/tmp/some-path");
        check(m.is_granted(Scope::FileSystemRead), "drop path under * is a no-op");
        check(m.is_granted(Scope::FileSystemRead, "/cwd/file"), "…still reads");

        m.drop(Scope::FileSystemRead);
        check(!m.is_granted(Scope::FileSystemRead), "drop('fs.read') revokes all");
        check(!m.is_granted(Scope::FileSystemRead, "/cwd/file"), "…including by path");
        // The write half is untouched.
        check(m.is_granted(Scope::FileSystemWrite), "write survives a read drop");
    }
    {
        // drop('fs') hits both halves (test-permission-drop-fs-scope).
        Model m{};
    init_model(m, "/cwd");
        m.enable();
        grant(m, Scope::FileSystemRead, {"*"});
        grant(m, Scope::FileSystemWrite, {"*"});
        m.drop(Scope::FileSystem, "/tmp/some-path");
        check(m.is_granted(Scope::FileSystemRead), "drop('fs', path) is a no-op under *");
        check(m.is_granted(Scope::FileSystemWrite), "…for writes too");
        m.drop(Scope::FileSystem);
        check(!m.is_granted(Scope::FileSystemRead), "drop('fs') revokes reads");
        check(!m.is_granted(Scope::FileSystemWrite), "drop('fs') revokes writes");
    }
}

// ── test-permission-drop-fs-granted-path.js, all five cases ─────────────────
void test_drop_granted_paths() {
    const std::string dir{"/tmp/granted-dir"};
    const std::string dir2{"/tmp/other-dir"};

    // Case 1: drop a file inside a granted directory — no-op.
    {
        Model m{};
    init_model(m, "/cwd", {dir});
        m.enable();
        grant(m, Scope::FileSystemRead, {dir});
        check(m.is_granted(Scope::FileSystemRead, dir + "/item1.txt"), "case1: granted");
        m.drop(Scope::FileSystemRead, dir + "/item1.txt");
        check(m.is_granted(Scope::FileSystemRead, dir + "/item1.txt"),
              "case1: file drop inside a dir grant is a no-op");
        check(m.is_granted(Scope::FileSystemRead, dir + "/item2.txt"), "case1: sibling still read");
    }
    // Case 2: drop the granted directory itself — revokes the subtree.
    {
        Model m{};
    init_model(m, "/cwd", {dir});
        m.enable();
        grant(m, Scope::FileSystemRead, {dir});
        m.drop(Scope::FileSystemRead, dir);
        check(!m.is_granted(Scope::FileSystemRead, dir + "/item1.txt"), "case2: subtree revoked");
        check(!m.is_granted(Scope::FileSystemRead, dir + "/item2.txt"), "case2: fully revoked");
    }
    // Case 3: two grants, drop one — the other survives.
    {
        Model m{};
    init_model(m, "/cwd", {dir, dir2});
        m.enable();
        grant(m, Scope::FileSystemRead, {dir, dir2});
        m.drop(Scope::FileSystemRead, dir);
        check(!m.is_granted(Scope::FileSystemRead, dir + "/item1.txt"), "case3: dropped one");
        check(m.is_granted(Scope::FileSystemRead, dir2 + "/other.txt"), "case3: kept the other");
    }
    // Case 4: dir + file granted separately, drop the file — dir grant still covers it.
    {
        Model m{};
    init_model(m, "/cwd", {dir});
        m.enable();
        grant(m, Scope::FileSystemRead, {dir, dir + "/item1.txt"});
        m.drop(Scope::FileSystemRead, dir + "/item1.txt");
        check(m.is_granted(Scope::FileSystemRead, dir + "/item1.txt"),
              "case4: the directory grant still covers the file");
    }
    // Case 5: drop the whole scope.
    {
        Model m{};
    init_model(m, "/cwd", {dir});
        m.enable();
        grant(m, Scope::FileSystemRead, {dir});
        m.drop(Scope::FileSystemRead);
        check(!m.is_granted(Scope::FileSystemRead, dir + "/item1.txt"), "case5: revoked by path");
        check(!m.is_granted(Scope::FileSystemRead), "case5: revoked as a scope");
    }
}

// A repeated grant must not double-insert (test-permission-fs-repeat-path).
void test_repeat_path() {
    Model m{};
    init_model(m, "/cwd");
    m.enable();
    const std::string f{"/fixtures/regular-file.md"};
    grant(m, Scope::FileSystemWrite, {f, f});
    grant(m, Scope::FileSystemRead, {f, f});
    check(m.is_granted(Scope::FileSystemWrite, f), "repeat: write granted");
    check(m.is_granted(Scope::FileSystemRead, f), "repeat: read granted");
    check(!m.is_granted(Scope::FileSystemRead, "/fixtures/protected-file.md"), "repeat: !read other");
    check(!m.is_granted(Scope::FileSystemWrite, "/fixtures/protected-file.md"),
          "repeat: !write other");
    // Dropping once must remove it (a duplicate entry would leave it granted).
    m.drop(Scope::FileSystemRead, f);
    check(!m.is_granted(Scope::FileSystemRead, f), "repeat: one drop is enough");
}

// Relative CLI paths resolve against the cwd (test-permission-fs-relative-path).
void test_relative_grant() {
    Model m{};
    init_model(m, "/work/test/parallel");
    m.enable();
    grant(m, Scope::FileSystemWrite, {"../fixtures/permission/deny/regular-file.md"});
    check(m.is_granted(Scope::FileSystemWrite,
                       "/work/test/fixtures/permission/deny/regular-file.md"),
          "relative grant resolves against cwd");
    check(!m.is_granted(Scope::FileSystemWrite,
                        "/work/test/fixtures/permission/deny/protected-file.md"),
          "relative grant does not leak to a sibling");
}

// The model must be inert until it is enabled: nothing may consult it otherwise,
// and enabling is a one-way door.
void test_enable_state() {
    Model m{};
    init_model(m, "/cwd");
    check(!m.enabled(), "disabled by default");
    check(!m.warning_only(), "not audit by default");
    m.enable();
    check(m.enabled(), "enable() sticks");
    m.enable_warning_only();
    check(m.warning_only(), "audit mode sticks");
}

// ── the CLI surface ─────────────────────────────────────────────────────────
Options opts(std::initializer_list<std::string> tokens) {
    std::vector<std::string> v{tokens};
    return parse_options(v);
}

void test_parse_options() {
    // Nothing at all: the model must stay off. This is the case that matters
    // most — every ordinary `mbun script.js` run takes it.
    {
        const Options o{opts({"--version", "script.js"})};
        check(!o.enabled(), "no flags -> model off");
        check(!o.any_allow_flag(), "no flags -> no allow flags");
        check(o.presentFlags.empty(), "no flags -> nothing to propagate");
    }
    // The `=` form.
    {
        const Options o{opts({"--permission", "--allow-fs-read=*", "--allow-fs-write=/tmp"})};
        check(o.permission && o.enabled(), "--permission");
        check(!o.audit, "not audit");
        check(o.allowFsRead.size() == 1 && o.allowFsRead[0] == "*", "read=* parsed");
        check(o.allowFsWrite.size() == 1 && o.allowFsWrite[0] == "/tmp", "write=/tmp parsed");
    }
    // The SPACE form, which node's own corpus uses (test-permission-fs-wildcard
    // passes `--allow-fs-read /tmp/*`); mis-parsing it would treat the path as
    // the script to run.
    {
        const Options o{opts({"--permission", "--allow-fs-read", "*", "--allow-fs-write",
                              "../fixtures/x.md", "-e", "0"})};
        check(o.allowFsRead.size() == 1 && o.allowFsRead[0] == "*", "space-form read");
        check(o.allowFsWrite.size() == 1 && o.allowFsWrite[0] == "../fixtures/x.md",
              "space-form write");
        // Propagation normalises to the `=` form so a child sees one token.
        check(std::ranges::find(o.presentFlags, std::string{"--allow-fs-read=*"}) != o.presentFlags.end(),
              "space form propagates as =");
    }
    // Repeated flags accumulate (test-permission-fs-repeat-path / -symlink).
    {
        const Options o{opts({"--permission", "--allow-fs-read=/a", "--allow-fs-read=/b"})};
        check(o.allowFsRead.size() == 2, "repeated read flags accumulate");
    }
    // Booleans.
    {
        const Options o{opts({"--permission-audit", "--allow-child-process", "--allow-worker",
                              "--allow-net", "--allow-addons", "--allow-wasi",
                              "--allow-inspector", "--allow-ffi"})};
        check(o.audit && o.enabled(), "--permission-audit enables");
        check(!o.permission, "audit is not --permission");
        check(o.allowChildProcess, "allow child");
        check(o.allowWorker, "allow worker");
        check(o.allowNet, "allow net");
        check(o.allowAddons, "allow addons");
        check(o.allowWasi, "allow wasi");
        check(o.allowInspector, "allow inspector");
        check(o.allowFfi, "allow ffi");
        check(o.any_allow_flag(), "any_allow_flag");
    }
    // A dangling value-taking flag must not read past the end.
    {
        const Options o{opts({"--permission", "--allow-fs-read"})};
        check(o.allowFsRead.empty(), "dangling flag grants nothing");
    }
    // Allow flags WITHOUT --permission are still recorded: node throws
    // ERR_MISSING_OPTION for exactly this combination, so the caller has to be
    // able to see it.
    {
        const Options o{opts({"--allow-fs-read=*", "script.js"})};
        check(!o.enabled(), "allow flag alone does not enable");
        check(o.any_allow_flag(), "…but is visible to the ERR_MISSING_OPTION check");
    }
}

// node env.cc's bootstrap order, including the implicit entry-point grant.
void test_apply_options() {
    // --permission with nothing else: every scope denied, except that the entry
    // point itself stays readable.
    {
        Model m{};
        init_model(m, "/work");
        const Options o{opts({"--permission"})};
        apply_options(m, o, false, "script.js", {});
        check(m.enabled(), "model enabled");
        check(!m.is_granted(Scope::ChildProcess), "bare --permission denies child");
        check(!m.is_granted(Scope::WorkerThreads), "…worker");
        check(!m.is_granted(Scope::Wasi), "…wasi");
        check(!m.is_granted(Scope::Inspector), "…inspector");
        check(!m.is_granted(Scope::Addon), "…addons");
        check(!m.is_granted(Scope::Ffi), "…ffi");
        check(!m.is_granted(Scope::Net), "…net");
        check(m.is_granted(Scope::FileSystemRead, "/work/script.js"),
              "entry point is implicitly readable");
        check(!m.is_granted(Scope::FileSystemRead, "/work/other.js"), "…but nothing else is");
        check(!m.is_granted(Scope::FileSystemWrite, "/work/script.js"), "…and not writable");
    }
    // --allow-* flips exactly its own scope.
    {
        Model m{};
        init_model(m, "/work");
        const Options o{opts({"--permission", "--allow-child-process"})};
        apply_options(m, o, false, "script.js", {});
        check(m.is_granted(Scope::ChildProcess), "--allow-child-process grants child");
        check(!m.is_granted(Scope::WorkerThreads), "…and nothing else");
    }
    // -e: no entry point exists, so nothing is implicitly readable
    // (test-permission-child-process-inherit-flags reads has('fs.read') === false
    // in a child started with `-e`).
    {
        Model m{};
        init_model(m, "/work");
        const Options o{opts({"--permission", "-e", "0"})};
        apply_options(m, o, /*hasEvalString=*/true, "", {});
        check(!m.is_granted(Scope::FileSystemRead), "-e grants no read");
        check(!m.is_granted(Scope::FileSystemRead, "/work/script.js"), "-e grants no path");
    }
    // Preloaded modules (-r) get the same implicit read grant as the entry
    // (test-permission-fs-read-entrypoint spawns `-r <loader> --permission <file>`).
    {
        Model m{};
        init_model(m, "/work");
        const std::vector<std::string> preloads{"/work/loader.js"};
        const Options o{opts({"--permission"})};
        apply_options(m, o, false, "hello.js", preloads);
        check(m.is_granted(Scope::FileSystemRead, "/work/loader.js"), "preload is readable");
        check(m.is_granted(Scope::FileSystemRead, "/work/hello.js"), "entry is readable");
    }
    // The `inspect` subcommand is not an entry point.
    {
        Model m{};
        init_model(m, "/work");
        apply_options(m, opts({"--permission"}), false, "inspect", {});
        check(!m.is_granted(Scope::FileSystemRead, "/work/inspect"), "'inspect' is not granted");
    }
    // Disabled model: apply_options must be a complete no-op, so an ordinary run
    // can never be gated by accident.
    {
        Model m{};
        init_model(m, "/work");
        apply_options(m, opts({"--allow-fs-read=/x", "script.js"}), false, "script.js", {});
        check(!m.enabled(), "no --permission -> model stays off");
        check(!m.is_granted(Scope::FileSystemRead, "/x"), "…and nothing was applied");
    }
    // NODE_OPTIONS first, command line second: the child's own --permission
    // decides (test-permission-child-process-inherit-flags case 2/3).
    {
        Model m{};
        init_model(m, "/work");
        const std::vector<std::string> tokens{"--permission", "--allow-fs-read=*", "-e", "0"};
        const Options inherited{parse_options(tokens)};
        check(inherited.allowFsRead.size() == 1, "inherited read flag parsed");
        apply_options(m, inherited, true, "", {});
        check(m.is_granted(Scope::FileSystemRead), "inherited read=* applies");
    }
}

}  // namespace

int main() {
    test_scope_table();
    test_path_resolve();
    test_wildcard_primary();
    test_wildcard_secondary();
    test_directory_grant_and_traversal();
    test_allow_all();
    test_has_matrix();
    test_single_state_scopes();
    test_drop_wildcard_scopes();
    test_drop_granted_paths();
    test_repeat_path();
    test_relative_grant();
    test_enable_state();
    test_parse_options();
    test_apply_options();

    std::println("mbun.permission: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
