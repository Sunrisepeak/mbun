#if !defined(_WIN32)
// ── proc.spawnEx: full-fidelity async spawn for node:child_process ──────────
// spawnEx(file, argv, opts) → { pid, fds } | { errno }
//   argv    full argv including argv[0] (argv0 override supported by caller).
//   opts    { cwd, env, stdio: ("pipe"|"ignore"|"inherit"|fd)[], detached,
//             uid, gid } — stdio defaults to ["pipe","pipe","pipe"].
//   fds     parent-side pipe fd per slot (-1 for non-pipe slots). Slot 0 is the
//           write end (child stdin); other pipe slots are read ends.
// Exec/pre-exec failures (ENOENT, EPERM from setuid, EBADF from bad stdio fd…)
// are reported through a CLOEXEC status pipe exactly like libuv: the parent
// blocks on it until exec succeeds (EOF) or the child writes errno.
struct StdioSlot {
    enum class Kind : std::uint8_t { Pipe, Ignore, Inherit, Fd } kind{Kind::Pipe};
    int fd{-1};  // Fd: source fd number
};

#if defined(__linux__)
// glibc declares close_range() behind feature-test macros, while musl does not
// expose the same libc symbol on all supported headers. Use the kernel syscall
// directly and retain a bounded fallback for older kernels.
int close_range_compat(unsigned first, unsigned last, unsigned flags) {
#  if defined(SYS_close_range)
    const long rc{::syscall(SYS_close_range, first, last, flags)};
    if (rc == 0) return 0;
    if (errno != ENOSYS && errno != EINVAL) return -1;
#  endif
    const long openMax{sysconf(_SC_OPEN_MAX)};
    const unsigned limit{static_cast<unsigned>(openMax > 0 ? std::min<long>(openMax, 65536) : 1024)};
    const unsigned end{std::min(last, limit == 0 ? 0u : limit - 1)};
    for (unsigned fd{first}; fd <= end; ++fd) close(static_cast<int>(fd));
    return 0;
}
#endif

JSValueRef proc_spawn_ex_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
    std::string file{fs_arg(ctx, argc, args, 0)};
    // argv: args[1] is the FULL argv (including argv[0]).
    std::vector<std::string> argvStr;
    if (argc > 1 && JSValueIsArray(ctx, args[1])) {
        JSObjectRef arr{JSValueToObject(ctx, args[1], nullptr)};
        JSStringRef lenKey{JSStringCreateWithUTF8CString("length")};
        double len{JSValueToNumber(ctx, JSObjectGetProperty(ctx, arr, lenKey, nullptr), nullptr)};
        JSStringRelease(lenKey);
        for (unsigned i = 0; i < static_cast<unsigned>(len); ++i) {
            argvStr.push_back(val_to_string(ctx, JSObjectGetPropertyAtIndex(ctx, arr, i, nullptr)));
        }
    }
    if (argvStr.empty()) argvStr.push_back(file);
    SpawnOpts o{parse_spawn_opts(ctx, argc, args)};
    bool detached{false};
    double uid{-1}, gid{-1};
    std::vector<StdioSlot> slots{{StdioSlot::Kind::Pipe, -1},
                                 {StdioSlot::Kind::Pipe, -1},
                                 {StdioSlot::Kind::Pipe, -1}};
    if (argc > 2 && JSValueIsObject(ctx, args[2])) {
        JSObjectRef opt{JSValueToObject(ctx, args[2], nullptr)};
        detached = JSValueToBoolean(ctx, get_prop(ctx, opt, "detached"));
        JSValueRef uidV{get_prop(ctx, opt, "uid")};
        if (JSValueIsNumber(ctx, uidV)) uid = JSValueToNumber(ctx, uidV, nullptr);
        JSValueRef gidV{get_prop(ctx, opt, "gid")};
        if (JSValueIsNumber(ctx, gidV)) gid = JSValueToNumber(ctx, gidV, nullptr);
        JSValueRef stV{get_prop(ctx, opt, "stdio")};
        if (JSValueIsArray(ctx, stV)) {
            slots.clear();
            JSObjectRef arr{JSValueToObject(ctx, stV, nullptr)};
            JSStringRef lenKey{JSStringCreateWithUTF8CString("length")};
            double len{JSValueToNumber(ctx, JSObjectGetProperty(ctx, arr, lenKey, nullptr), nullptr)};
            JSStringRelease(lenKey);
            for (unsigned i = 0; i < static_cast<unsigned>(len); ++i) {
                JSValueRef el{JSObjectGetPropertyAtIndex(ctx, arr, i, nullptr)};
                StdioSlot s;
                if (JSValueIsNumber(ctx, el)) {
                    s = {StdioSlot::Kind::Fd, static_cast<int>(JSValueToNumber(ctx, el, nullptr))};
                } else {
                    const std::string k{val_to_string(ctx, el)};
                    s.kind = k == "ignore"    ? StdioSlot::Kind::Ignore
                             : k == "inherit" ? StdioSlot::Kind::Inherit
                                              : StdioSlot::Kind::Pipe;
                }
                slots.push_back(s);
            }
            while (slots.size() < 3) slots.push_back({StdioSlot::Kind::Ignore, -1});
        }
    }
    const int nSlots{static_cast<int>(slots.size())};

    // Parent/child pipe ends per slot (only Pipe slots use them).
    std::vector<std::array<int, 2>> pipes(slots.size(), {-1, -1});
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].kind != StdioSlot::Kind::Pipe) continue;
        if (pipe(pipes[i].data()) != 0) return fs_throw(ctx, exc, "spawnEx: pipe() failed");
    }
    int errPipe[2];
    if (pipe(errPipe) != 0) return fs_throw(ctx, exc, "spawnEx: pipe() failed");
    fcntl(errPipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(errPipe[1], F_SETFD, FD_CLOEXEC);

    const pid_t pid{fork()};
    if (pid < 0) return fs_throw(ctx, exc, "spawnEx: fork() failed");
    if (pid == 0) {  // ── child ──
        close(errPipe[0]);
        // Reset signal dispositions + mask to default so the runtime's inherited
        // SIG_IGN SIGPIPE never leaks to the child (else a broken-pipe write in a
        // spawned shell prints "I/O error" instead of dying silently).
        // ref: bun spawn_process.rs:652,732 (POSIX_SPAWN_SETSIGDEF + reset_signals).
        { sigset_t ss; sigemptyset(&ss); sigprocmask(SIG_SETMASK, &ss, nullptr);
          for (int s = 1; s < NSIG; ++s) signal(s, SIG_DFL); }
        const auto fail{[&](int e) {
            const std::int32_t v{e};
            (void)!write(errPipe[1], &v, sizeof v);
            _exit(127);
        }};
        if (detached) (void)setsid();
        // Move the status pipe well above the stdio slot range, keep CLOEXEC.
        const int errFd{fcntl(errPipe[1], F_DUPFD_CLOEXEC, nSlots + 256)};
        if (errFd >= 0) close(errPipe[1]);
        const int reportFd{errFd >= 0 ? errFd : errPipe[1]};
        const auto failFd{[&](int e) {
            const std::int32_t v{e};
            (void)!write(reportFd, &v, sizeof v);
            _exit(127);
        }};
        // Phase 1: stage every slot's source fd above the slot range so dup2s
        // cannot stomp sources (classic libuv two-phase remap).
        std::vector<int> staged(slots.size(), -1);
        for (std::size_t i = 0; i < slots.size(); ++i) {
            int src{-1};
            switch (slots[i].kind) {
            case StdioSlot::Kind::Pipe:
                src = i == 0 ? pipes[i][0] : pipes[i][1];
                break;
            case StdioSlot::Kind::Inherit:
                src = static_cast<int>(i);
                break;
            case StdioSlot::Kind::Fd:
                src = slots[i].fd;
                break;
            case StdioSlot::Kind::Ignore:
                if (i < 3) src = open("/dev/null", i == 0 ? O_RDONLY : O_WRONLY);
                break;  // ≥3: leave the slot closed (node parity)
            }
            if (slots[i].kind == StdioSlot::Kind::Ignore && i >= 3) continue;
            if (src < 0) failFd(errno != 0 ? errno : EBADF);
            staged[i] = fcntl(src, F_DUPFD, nSlots);
            if (staged[i] < 0) failFd(errno);  // EBADF: fd number is not open
        }
        // Phase 2: place staged fds into their slots; close unstaged slots ≥3.
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (staged[i] >= 0) {
                if (dup2(staged[i], static_cast<int>(i)) < 0) failFd(errno);
            } else if (i >= 3) {
                close(static_cast<int>(i));
            }
        }
        // Close every inherited fd above the slot range except the status pipe
        // (runtime-internal fds are not CLOEXEC; node children must not see them).
#if defined(__linux__)
        if (reportFd > nSlots) (void)close_range_compat(static_cast<unsigned>(nSlots),
                                                        static_cast<unsigned>(reportFd - 1), 0);
        (void)close_range_compat(static_cast<unsigned>(reportFd + 1), ~0u, 0);
#else
        const int maxFd{static_cast<int>(sysconf(_SC_OPEN_MAX) > 0
                                             ? std::min<long>(sysconf(_SC_OPEN_MAX), 65536)
                                             : 1024)};
        for (int f = nSlots; f < maxFd; ++f) {
            if (f != reportFd) close(f);
        }
#endif
        if (!o.cwd.empty() && chdir(o.cwd.c_str()) != 0) failFd(errno);
        if (gid >= 0) {
            if (setgroups(0, nullptr) != 0 && errno == EPERM) failFd(EPERM);
            if (setgid(static_cast<gid_t>(gid)) != 0) failFd(errno);
        }
        if (uid >= 0 && setuid(static_cast<uid_t>(uid)) != 0) failFd(errno);
        std::vector<char*> cargv;
        for (auto& a : argvStr) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        if (o.haveEnv) {
            std::vector<char*> cenv;
            for (auto& e : o.env) cenv.push_back(const_cast<char*>(e.c_str()));
            cenv.push_back(nullptr);
            // Resolve a bare exe against the CHILD's env.PATH, not the caller's
            // (execvpe searches the caller's environ PATH).
            const std::string resolved{resolve_in_env_path(file, o.env)};
            if (!resolved.empty()) execve(resolved.c_str(), cargv.data(), cenv.data());
            execvpe(file.c_str(), cargv.data(), cenv.data());
        } else {
            execvp(file.c_str(), cargv.data());
        }
        failFd(errno);
    }
    // ── parent ──
    close(errPipe[1]);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].kind != StdioSlot::Kind::Pipe) continue;
        close(i == 0 ? pipes[i][0] : pipes[i][1]);
    }
    std::int32_t childErr{0};
    ssize_t n;
    do { n = read(errPipe[0], &childErr, sizeof childErr); } while (n < 0 && errno == EINTR);
    close(errPipe[0]);
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    if (n == static_cast<ssize_t>(sizeof childErr)) {  // pre-exec/exec failure
        int status{0};
        waitpid(pid, &status, 0);
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].kind != StdioSlot::Kind::Pipe) continue;
            close(i == 0 ? pipes[i][1] : pipes[i][0]);
        }
        set_prop(ctx, r, "errno", JSValueMakeNumber(ctx, childErr));
        return r;
    }
    set_prop(ctx, r, "pid", JSValueMakeNumber(ctx, static_cast<double>(pid)));
    JSValueRef fdVals[64];
    const std::size_t nf{std::min(slots.size(), std::size_t{64})};
    for (std::size_t i = 0; i < nf; ++i) {
        const int f{slots[i].kind == StdioSlot::Kind::Pipe ? (i == 0 ? pipes[i][1] : pipes[i][0])
                                                           : -1};
        fdVals[i] = JSValueMakeNumber(ctx, f);
    }
    set_prop(ctx, r, "fds", JSObjectMakeArray(ctx, nf, fdVals, nullptr));
    return r;
}

// proc.poll(fds, timeoutMs) → per-fd readiness (1 readable/EOF/error, 0 not).
// An empty fd list degrades to a plain sleep(timeoutMs) — the async child poll
// loop uses that to wait for exit without burning CPU when nothing is piped.
JSValueRef proc_poll_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                        const JSValueRef args[], JSValueRef*) {
    int timeout{argc > 1 ? static_cast<int>(JSValueToNumber(ctx, args[1], nullptr)) : 0};
    std::vector<pollfd> pfds;
    if (argc > 0 && JSValueIsArray(ctx, args[0])) {
        JSObjectRef arr{JSValueToObject(ctx, args[0], nullptr)};
        JSStringRef lenKey{JSStringCreateWithUTF8CString("length")};
        double len{JSValueToNumber(ctx, JSObjectGetProperty(ctx, arr, lenKey, nullptr), nullptr)};
        JSStringRelease(lenKey);
        for (unsigned i = 0; i < static_cast<unsigned>(len); ++i) {
            const int fd{static_cast<int>(
                JSValueToNumber(ctx, JSObjectGetPropertyAtIndex(ctx, arr, i, nullptr), nullptr))};
            pfds.push_back(pollfd{.fd = fd, .events = POLLIN, .revents = 0});
        }
    }
    int rc;
    do { rc = poll(pfds.empty() ? nullptr : pfds.data(),
                   static_cast<nfds_t>(pfds.size()), timeout); } while (rc < 0 && errno == EINTR);
    JSValueRef vals[64];
    const std::size_t nf{std::min(pfds.size(), std::size_t{64})};
    for (std::size_t i = 0; i < nf; ++i) {
        const bool ready{rc > 0 &&
                         (pfds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0};
        vals[i] = JSValueMakeNumber(ctx, ready ? 1 : 0);
    }
    return JSObjectMakeArray(ctx, nf, vals, nullptr);
}

// proc.ids() → real pid/ppid/uid/gid family (process.pid/getuid… are built on it).
JSValueRef proc_ids_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                       const JSValueRef[], JSValueRef*) {
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, r, "pid", JSValueMakeNumber(ctx, getpid()));
    set_prop(ctx, r, "ppid", JSValueMakeNumber(ctx, getppid()));
    set_prop(ctx, r, "uid", JSValueMakeNumber(ctx, getuid()));
    set_prop(ctx, r, "euid", JSValueMakeNumber(ctx, geteuid()));
    set_prop(ctx, r, "gid", JSValueMakeNumber(ctx, getgid()));
    set_prop(ctx, r, "egid", JSValueMakeNumber(ctx, getegid()));
    return r;
}

// ── proc.spawnPty: fork a child under a fresh pseudo-terminal (Bun.spawn's
// `terminal` option / Bun.Terminal). Returns { pid, master, write } — `master`
// is the read end of the pty master, `write` a dup of it for the terminal.write
// queue (independent close). Ref: bun-ref src/runtime/api/bun/Terminal.rs
// create_pty_posix (openpty → dup master into read/write fds; cooked-mode
// termios defaults on the slave; child dup2s the slave onto 0/1/2 after
// setsid+TIOCSCTTY). We use POSIX posix_openpt/grantpt/unlockpt/ptsname (all in
// libc, no libutil dlopen) instead of openpty. Exec failures cross a CLOEXEC
// status pipe exactly like spawnEx.
JSValueRef proc_spawn_pty_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
    std::string file{fs_arg(ctx, argc, args, 0)};
    std::vector<std::string> argvStr;
    if (argc > 1 && JSValueIsArray(ctx, args[1])) {
        JSObjectRef arr{JSValueToObject(ctx, args[1], nullptr)};
        JSStringRef lenKey{JSStringCreateWithUTF8CString("length")};
        double len{JSValueToNumber(ctx, JSObjectGetProperty(ctx, arr, lenKey, nullptr), nullptr)};
        JSStringRelease(lenKey);
        for (unsigned i = 0; i < static_cast<unsigned>(len); ++i)
            argvStr.push_back(val_to_string(ctx, JSObjectGetPropertyAtIndex(ctx, arr, i, nullptr)));
    }
    if (argvStr.empty()) argvStr.push_back(file);
    SpawnOpts o{parse_spawn_opts(ctx, argc, args)};
    unsigned short cols{80}, rows{24};
    if (argc > 2 && JSValueIsObject(ctx, args[2])) {
        JSObjectRef opt{JSValueToObject(ctx, args[2], nullptr)};
        JSValueRef c{get_prop(ctx, opt, "cols")};
        if (JSValueIsNumber(ctx, c)) { double d{JSValueToNumber(ctx, c, nullptr)}; if (d > 0 && d < 65535) cols = static_cast<unsigned short>(d); }
        JSValueRef rw{get_prop(ctx, opt, "rows")};
        if (JSValueIsNumber(ctx, rw)) { double d{JSValueToNumber(ctx, rw, nullptr)}; if (d > 0 && d < 65535) rows = static_cast<unsigned short>(d); }
    }

    const int master{posix_openpt(O_RDWR | O_NOCTTY)};
    if (master < 0) return fs_throw(ctx, exc, "spawnPty: posix_openpt() failed");
    if (grantpt(master) != 0 || unlockpt(master) != 0) { close(master); return fs_throw(ctx, exc, "spawnPty: grantpt/unlockpt failed"); }
    char sname[256];
    if (ptsname_r(master, sname, sizeof sname) != 0) { close(master); return fs_throw(ctx, exc, "spawnPty: ptsname failed"); }
    const int slave{open(sname, O_RDWR | O_NOCTTY)};
    if (slave < 0) { close(master); return fs_throw(ctx, exc, "spawnPty: open(pts) failed"); }
    { struct winsize ws{}; ws.ws_col = cols; ws.ws_row = rows; ioctl(master, TIOCSWINSZ, &ws); }

    int errPipe[2];
    if (pipe(errPipe) != 0) { close(master); close(slave); return fs_throw(ctx, exc, "spawnPty: pipe() failed"); }
    fcntl(errPipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(errPipe[1], F_SETFD, FD_CLOEXEC);

    const pid_t pid{fork()};
    if (pid < 0) { close(master); close(slave); close(errPipe[0]); close(errPipe[1]); return fs_throw(ctx, exc, "spawnPty: fork() failed"); }
    if (pid == 0) {  // ── child ──
        close(errPipe[0]);
        close(master);
        { sigset_t ss; sigemptyset(&ss); sigprocmask(SIG_SETMASK, &ss, nullptr);
          for (int s = 1; s < NSIG; ++s) signal(s, SIG_DFL); }
        const auto failFd{[&](int e) {
            const std::int32_t v{e};
            (void)!write(errPipe[1], &v, sizeof v);
            _exit(127);
        }};
        if (setsid() < 0) failFd(errno);
        if (ioctl(slave, TIOCSCTTY, 0) < 0) failFd(errno);
        if (dup2(slave, 0) < 0 || dup2(slave, 1) < 0 || dup2(slave, 2) < 0) failFd(errno);
        if (slave > 2) close(slave);
        if (!o.cwd.empty() && chdir(o.cwd.c_str()) != 0) failFd(errno);
        std::vector<char*> cargv;
        for (auto& a : argvStr) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        if (o.haveEnv) {
            std::vector<char*> cenv;
            for (auto& e : o.env) cenv.push_back(const_cast<char*>(e.c_str()));
            cenv.push_back(nullptr);
            const std::string resolved{resolve_in_env_path(file, o.env)};
            if (!resolved.empty()) execve(resolved.c_str(), cargv.data(), cenv.data());
            execvpe(file.c_str(), cargv.data(), cenv.data());
        } else {
            execvp(file.c_str(), cargv.data());
        }
        failFd(errno);
    }
    // ── parent ──
    close(errPipe[1]);
    close(slave);  // child owns the slave; the parent only reads/writes the master
    std::int32_t childErr{0};
    ssize_t n;
    do { n = read(errPipe[0], &childErr, sizeof childErr); } while (n < 0 && errno == EINTR);
    close(errPipe[0]);
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    if (n == static_cast<ssize_t>(sizeof childErr)) {  // pre-exec/exec failure
        int status{0};
        waitpid(pid, &status, 0);
        close(master);
        set_prop(ctx, r, "errno", JSValueMakeNumber(ctx, childErr));
        return r;
    }
    // A second fd onto the same master lets the read side (drainOut) and the
    // write side (terminal.write queue) be closed independently — no double
    // close of one fd number that the OS may have recycled.
    const int writeFd{fcntl(master, F_DUPFD, 3)};
    set_prop(ctx, r, "pid", JSValueMakeNumber(ctx, static_cast<double>(pid)));
    set_prop(ctx, r, "master", JSValueMakeNumber(ctx, master));
    set_prop(ctx, r, "write", JSValueMakeNumber(ctx, writeFd >= 0 ? writeFd : master));
    return r;
}

// proc.ptyResize(fd, cols, rows) → push a new window size to the pty master.
JSValueRef proc_pty_resize_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                              const JSValueRef args[], JSValueRef*) {
    if (argc >= 3) {
        const int fd{static_cast<int>(JSValueToNumber(ctx, args[0], nullptr))};
        struct winsize ws{};
        ws.ws_col = static_cast<unsigned short>(JSValueToNumber(ctx, args[1], nullptr));
        ws.ws_row = static_cast<unsigned short>(JSValueToNumber(ctx, args[2], nullptr));
        ioctl(fd, TIOCSWINSZ, &ws);
    }
    return JSValueMakeUndefined(ctx);
}
#endif

// Pure-JS implementations of the common node built-in modules, registered under
// globalThis.__mbunNativeModules (keyed by both "node:x" and "x"). Evaluated once
// at runtime init. Kept close to node semantics for the subset bun tests use.
// (kNodeBuiltinsJS moved to module mbun.jsc.js_builtins — see js_builtins.cppm)

// A built-in (node:/bun:) module object, or nullopt if `mod` is not built in.
// The require() implementation short-circuits these before hitting the resolver.
// bun:test is injected by the test runner (globalThis.__mbunBT), so it is served
// from there when present; `bun` resolves to the Bun global; the pure-JS node
// builtins live under globalThis.__mbunNativeModules (installed at init).
std::optional<JSValueRef> builtin_module(JSContextRef ctx, const std::string& mod) {
    JSObjectRef global{JSContextGetGlobalObject(ctx)};
    if (mod == "bun") {
        return get_prop(ctx, global, "Bun");
    }
    // `vitest` and `@jest/globals` are hardcoded aliases of `bun:test` — bun maps
    // them in its alias table rather than resolving the real packages, so a suite
    // written against either runs unmodified (ref: bun-ref/src/resolve_builtins/
    // HardcodedModule.rs:773-791 `BUN_TEST_EXTRA_ALIAS_KVS`). The aliases live in
    // BUN_TEST_ALIASES only (not BUN_ALIASES), i.e. they apply under `bun test`
    // and nowhere else; the `__mbunBT` presence check below reproduces exactly
    // that gate, since the harness only defines it while the test runner is up.
    // Outside `mbun test` these fall through to the resolver and hit the real
    // package in node_modules, which is what bun does too.
    if (mod == "bun:test" || mod == "vitest" || mod == "@jest/globals") {
        JSValueRef bt{get_prop(ctx, global, "__mbunBT")};
        if (!JSValueIsUndefined(ctx, bt) && !JSValueIsNull(ctx, bt)) {
            return bt;
        }
    }
    // pure-JS node builtins: globalThis.__mbunNativeModules[mod]
    JSValueRef reg{get_prop(ctx, global, "__mbunNativeModules")};
    if (JSValueIsObject(ctx, reg)) {
        JSObjectRef regObj{JSValueToObject(ctx, reg, nullptr)};
        JSStringRef key{JSStringCreateWithUTF8CString(mod.c_str())};
        JSValueRef m{JSObjectGetProperty(ctx, regObj, key, nullptr)};
        JSStringRelease(key);
        if (!JSValueIsUndefined(ctx, m) && !JSValueIsNull(ctx, m)) {
            return m;
        }
    }
    return std::nullopt;
}

JSValueRef require_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                      const JSValueRef args[], JSValueRef* exc) {
    std::string mod{arg_string(ctx, argc, args, 0)};
    if (auto b{builtin_module(ctx, mod)}) return *b;
    if (exc != nullptr) *exc = make_string(ctx, "Cannot find module '" + mod + "'");
    return JSValueMakeUndefined(ctx);
}

// ── process.* ────────────────────────────────────────────────────────────────

#if defined(_WIN32)
extern "C" char** _environ;
#define MBUN_ENVIRON _environ
#else
extern "C" char** environ;
#define MBUN_ENVIRON environ
#endif

// The host platform / arch as node's process.platform / process.arch strings.
constexpr const char* host_platform() {
#if defined(__APPLE__)
    return "darwin";
#elif defined(_WIN32)
    return "win32";
#elif defined(__FreeBSD__)
    return "freebsd";
#else
    return "linux";
#endif
}
constexpr const char* host_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x64";
#else
    return "x64";
#endif
}

// Absolute path to the running executable (process.execPath / bunExe()). Tests
// spawn [bunExe(), file] as a drop-in `bun`; a bare "bun" would exit 127, so this
// must be the real path. /proc/self/exe on linux; argv[0] canonicalised otherwise.
std::string exe_path() {
    std::error_code ec;
#if defined(__linux__)
    auto p{std::filesystem::read_symlink("/proc/self/exe", ec)};
    if (!ec && !p.empty()) return p.string();
#endif
    if (!gArgv.empty() && !gArgv[0].empty()) {
        auto c{std::filesystem::weakly_canonical(std::filesystem::path{gArgv[0]}, ec)};
        if (!ec) return c.string();
        return gArgv[0];
    }
    return "mbun";
}

JSValueRef process_cwd_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                          const JSValueRef[], JSValueRef*) {
    std::error_code ec;
    return make_string(ctx, std::filesystem::current_path(ec).string());
}

JSValueRef process_chdir_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
    if (argc < 1) return JSValueMakeUndefined(ctx);
    std::error_code ec;
    std::filesystem::current_path(std::filesystem::path{val_to_string(ctx, args[0])}, ec);
    if (ec && exc != nullptr) {
        *exc = make_string(ctx, "ENOENT: no such file or directory, chdir '" +
                                    val_to_string(ctx, args[0]) + "'");
    }
    return JSValueMakeUndefined(ctx);
}

JSValueRef process_exit_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef*) {
    std::exit(argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : 0);
}

// process.stdout.write / process.stderr.write — raw (no trailing newline).
JSValueRef stdout_write_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef*) {
    std::cout << arg_string(ctx, argc, args, 0);
    std::cout.flush();
    return JSValueMakeBoolean(ctx, true);
}
JSValueRef stderr_write_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef*) {
    std::cerr << arg_string(ctx, argc, args, 0);
    return JSValueMakeBoolean(ctx, true);
}

// Sanitize a byte string to well-formed UTF-8, replacing every invalid byte /
// truncated sequence with U+FFFD (WHATWG "replace" — the same result bun's
// lossy env decode produces). A property key derived straight from invalid
// UTF-8 crashes JSC when JSObjectSetProperty atomizes it (garbage-env test:
// an env var whose NAME is non-UTF-8 bytes SEGV'd the whole process at
// startup). Values survive JSValueMakeString untouched, but keys must be
// well-formed before they become identifiers.
std::string sanitize_utf8_(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    const auto repl = [&] { out.append("\xEF\xBF\xBD"); };
    while (i < n) {
        const unsigned char c = p[i];
        if (c < 0x80) { out.push_back(static_cast<char>(c)); ++i; continue; }
        int len = 0;
        unsigned int lo = 0, hi = 0;
        if ((c & 0xE0) == 0xC0 && c >= 0xC2) { len = 2; lo = 0x80; hi = 0xBF; }
        else if (c == 0xE0) { len = 3; lo = 0xA0; hi = 0xBF; }
        else if (c == 0xED) { len = 3; lo = 0x80; hi = 0x9F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; lo = 0x80; hi = 0xBF; }
        else if (c == 0xF0) { len = 4; lo = 0x90; hi = 0xBF; }
        else if (c == 0xF4) { len = 4; lo = 0x80; hi = 0x8F; }
        else if ((c & 0xF8) == 0xF0 && c <= 0xF3) { len = 4; lo = 0x80; hi = 0xBF; }
        else { repl(); ++i; continue; }  // invalid lead byte
        if (i + static_cast<std::size_t>(len) > n) { repl(); ++i; continue; }
        bool ok = true;
        for (int k = 1; k < len; ++k) {
            const unsigned char cc = p[i + static_cast<std::size_t>(k)];
            const unsigned int lb = (k == 1) ? lo : 0x80u;
            const unsigned int hb = (k == 1) ? hi : 0xBFu;
            if (cc < lb || cc > hb) { ok = false; break; }
        }
        if (!ok) { repl(); ++i; continue; }
        out.append(s.substr(i, static_cast<std::size_t>(len)));
        i += static_cast<std::size_t>(len);
    }
    return out;
}

// process.env from the real environment (key=value pairs).
JSObjectRef build_process_env(JSContextRef ctx) {
    JSObjectRef env{JSObjectMake(ctx, nullptr, nullptr)};
    for (char** e = MBUN_ENVIRON; e != nullptr && *e != nullptr; ++e) {
        std::string_view kv{*e};
        const auto eq = kv.find('=');
        if (eq == std::string_view::npos) continue;
        // Key must be well-formed UTF-8 before it becomes a JS property key
        // (invalid bytes crash JSObjectSetProperty's atomization).
        const std::string key{sanitize_utf8_(kv.substr(0, eq))};
        if (key.empty()) continue;  // an all-invalid key sanitizes to "" — bun drops it
        set_prop(ctx, env, key.c_str(), make_string(ctx, kv.substr(eq + 1)));
    }
    return env;
}

// An OS-backed FileSystem for the resolver / module loader (real fs access).
mbun::resolver::FileSystem os_fs() {
    return mbun::resolver::FileSystem{
        .file_exists = [](std::string_view p) {
            std::error_code ec;
            return std::filesystem::is_regular_file(std::filesystem::path{p}, ec);
        },
        .dir_exists = [](std::string_view p) {
            std::error_code ec;
            return std::filesystem::is_directory(std::filesystem::path{p}, ec);
        },
        .read_file = [](std::string_view p) -> std::optional<std::string> {
            return read_file(std::string{p});
        },
        // node preserveSymlinks=false: bare-specifier walk-up starts at the
        // importer's canonical dir (isolated-linker layouts need this).
        .real_path = [](std::string_view p) -> std::optional<std::string> {
            std::error_code ec;
            auto c{std::filesystem::canonical(std::filesystem::path{p}, ec)};
            if (ec) return std::nullopt;
            return c.string();
        },
    };
}

// The directory containing `path` (for a module's __dirname / relative require).
std::string parent_dir(const std::string& path) {
    std::error_code ec;
    return std::filesystem::path{path}.parent_path().string();
}

// require() implemented on the Runtime (defined below); the C callback trampolines
// here. Declared early so install_bindings_ can register it.
JSValueRef require_native_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef* exc);
JSValueRef evict_module_cache_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                 const JSValueRef args[], JSValueRef* exc);
JSValueRef resolve_native_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef* exc);
JSValueRef transpile_native_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef* exc);
// CAP-NAPI: process.dlopen trampoline (defined in engine.inc, backed by
// runtime/napi_objects.inc mbun_napi_process_dlopen).
JSValueRef napi_dlopen_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc);
