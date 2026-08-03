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
// `Ipc` is node's fork() channel slot: a bidirectional AF_UNIX SOCK_STREAM
// socketpair rather than a half-duplex pipe (node lib/internal/child_process.js
// setupChannel + src/spawn_sync.cc UV_CREATE_PIPE|UV_READABLE|UV_WRITABLE). The
// parent end comes back in `fds`; the child end is dup2'd into the slot index
// and advertised to the child through NODE_CHANNEL_FD (added by the JS layer).
struct StdioSlot {
    enum class Kind : std::uint8_t { Pipe, Ignore, Inherit, Fd, Ipc } kind{Kind::Pipe};
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
    // Permission Model: node enforces the ChildProcess scope at the fork
    // (src/spawn_sync.cc / process_wrap.cc), and so does this. `resource` is the
    // executable, which is what test-permission-child-process-cli asserts on.
    // permission_deny_spawn also handles mbun's worker-as-subprocess case.
    if (permission_deny_spawn(ctx, exc, file, o.env)) return JSValueMakeUndefined(ctx);
    permission_inject_child_env(o.env, o.haveEnv, argvStr);
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
                    int rawFd{static_cast<int>(JSValueToNumber(ctx, el, nullptr))};
                    // A descriptor that came from fs.openSync is one of mbun's
                    // VIRTUAL fds (numbered from 1000); handing that number to
                    // dup2 in the child is EBADF. Translate it to the real OS
                    // descriptor the same way every other fd binding does
                    // (test-fs-syncwritestream spawns a child with
                    // stdio: ['inherit', fd, fd]).
                    if (auto it{fd_table().find(rawFd)};
                        it != fd_table().end() && it->second.is_open()) {
                        rawFd = it->second.native_fd();
                    }
                    s = {StdioSlot::Kind::Fd, rawFd};
                } else {
                    const std::string k{val_to_string(ctx, el)};
                    s.kind = k == "ignore"    ? StdioSlot::Kind::Ignore
                             : k == "inherit" ? StdioSlot::Kind::Inherit
                             : k == "ipc"     ? StdioSlot::Kind::Ipc
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
        if (slots[i].kind == StdioSlot::Kind::Ipc) {
            // [0] = parent end, [1] = child end (both directions on each).
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pipes[i].data()) != 0) {
                return fs_throw(ctx, exc, "spawnEx: socketpair() failed");
            }
            continue;
        }
        if (slots[i].kind != StdioSlot::Kind::Pipe) continue;
        // Slots 0/1/2 are half-duplex by nature (stdin reads, stdout/stderr
        // write), so a pipe() is both sufficient and cheaper. A 'pipe' slot
        // ABOVE stderr has no such direction: node backs every extra slot with a
        // uv_pipe_t (a socketpair) and hands the parent a duplex net.Socket, which
        // is what `cluster.setupPrimary({ stdio: [..., 'ipc', 'pipe'] })` then
        // writes to and reads from over the same descriptor
        // (test-cluster-fork-stdio). A half-duplex pipe gave the parent a
        // read-only end, so channel.write() had nothing to write to.
        if (i >= 3) {
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pipes[i].data()) != 0) {
                return fs_throw(ctx, exc, "spawnEx: socketpair() failed");
            }
            continue;
        }
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
            case StdioSlot::Kind::Ipc:
                src = pipes[i][1];
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
            mbun::platform::process::exec_path_env(file.c_str(), cargv.data(), cenv.data());
        } else {
            execvp(file.c_str(), cargv.data());
        }
        failFd(errno);
    }
    // ── parent ──
    close(errPipe[1]);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].kind == StdioSlot::Kind::Ipc) { close(pipes[i][1]); continue; }
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
            if (slots[i].kind == StdioSlot::Kind::Ipc) { close(pipes[i][0]); continue; }
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
                    : slots[i].kind == StdioSlot::Kind::Ipc ? pipes[i][0]
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

// Current-process credential mutations backing process.set{e,}{u,g}id(),
// setgroups(), and initgroups().  Keep name resolution and the POSIX syscall
// together: JavaScript validates the public Node argument contract, while this
// seam reports either the libc errno or which passwd/group lookup failed.
JSObjectRef proc_credential_result(JSContextRef ctx, int err = 0,
                                   const char* unknown_kind = nullptr,
                                   const std::string& unknown_name = {}) {
    JSObjectRef result{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, result, "errno", JSValueMakeNumber(ctx, err));
    if (unknown_kind != nullptr) {
        set_prop(ctx, result, "unknownKind", make_string(ctx, unknown_kind));
        set_prop(ctx, result, "unknownName", make_string(ctx, unknown_name));
    }
    return result;
}

bool proc_credential_uid(JSContextRef ctx, JSValueRef value, uid_t& out,
                         std::string& missing) {
    if (JSValueIsString(ctx, value)) {
        const std::string name{val_to_string(ctx, value)};
        if (struct passwd* pw{getpwnam(name.c_str())}; pw != nullptr) {
            out = pw->pw_uid;
            return true;
        }
        missing = name;
        return false;
    }
    const double raw{JSValueToNumber(ctx, value, nullptr)};
    if (!std::isfinite(raw) || raw < 0 || raw > static_cast<double>(std::numeric_limits<uid_t>::max())) {
        errno = EINVAL;
        return false;
    }
    out = static_cast<uid_t>(raw);
    return true;
}

bool proc_credential_gid(JSContextRef ctx, JSValueRef value, gid_t& out,
                         std::string& missing) {
    if (JSValueIsString(ctx, value)) {
        const std::string name{val_to_string(ctx, value)};
        if (struct group* gr{getgrnam(name.c_str())}; gr != nullptr) {
            out = gr->gr_gid;
            return true;
        }
        missing = name;
        return false;
    }
    const double raw{JSValueToNumber(ctx, value, nullptr)};
    if (!std::isfinite(raw) || raw < 0 || raw > static_cast<double>(std::numeric_limits<gid_t>::max())) {
        errno = EINVAL;
        return false;
    }
    out = static_cast<gid_t>(raw);
    return true;
}

JSValueRef proc_set_credential_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                  std::size_t argc, const JSValueRef args[], JSValueRef*) {
    const int which{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const JSValueRef value{argc > 1 ? args[1] : JSValueMakeUndefined(ctx)};
    std::string missing;
    if (which == 0 || which == 1) {
        uid_t id{};
        if (!proc_credential_uid(ctx, value, id, missing)) {
            return proc_credential_result(ctx, missing.empty() ? errno : 0,
                                          missing.empty() ? nullptr : "user", missing);
        }
        const int rc{which == 0 ? setuid(id) : seteuid(id)};
        return proc_credential_result(ctx, rc == 0 ? 0 : errno);
    }
    if (which == 2 || which == 3) {
        gid_t id{};
        if (!proc_credential_gid(ctx, value, id, missing)) {
            return proc_credential_result(ctx, missing.empty() ? errno : 0,
                                          missing.empty() ? nullptr : "group", missing);
        }
        const int rc{which == 2 ? setgid(id) : setegid(id)};
        return proc_credential_result(ctx, rc == 0 ? 0 : errno);
    }
    return proc_credential_result(ctx, EINVAL);
}

JSValueRef proc_setgroups_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                             std::size_t argc, const JSValueRef args[], JSValueRef*) {
    JSObjectRef groups{argc > 0 ? JSValueToObject(ctx, args[0], nullptr) : nullptr};
    if (groups == nullptr) return proc_credential_result(ctx, EINVAL);
    const std::size_t size{static_cast<std::size_t>(JSValueToNumber(ctx, get_prop(ctx, groups, "length"), nullptr))};
    std::vector<gid_t> ids;
    ids.reserve(size);
    for (std::size_t i{}; i < size; ++i) {
        std::string missing;
        gid_t id{};
        if (!proc_credential_gid(ctx, JSObjectGetPropertyAtIndex(ctx, groups, static_cast<unsigned>(i), nullptr), id, missing)) {
            return proc_credential_result(ctx, missing.empty() ? errno : 0,
                                          missing.empty() ? nullptr : "group", missing);
        }
        ids.push_back(id);
    }
    const int rc{setgroups(ids.size(), ids.data())};
    return proc_credential_result(ctx, rc == 0 ? 0 : errno);
}

JSValueRef proc_initgroups_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                              std::size_t argc, const JSValueRef args[], JSValueRef*) {
    const JSValueRef user{argc > 0 ? args[0] : JSValueMakeUndefined(ctx)};
    const JSValueRef group_value{argc > 1 ? args[1] : JSValueMakeUndefined(ctx)};
    // Node resolves the supplementary/extra group before the user; preserve
    // that observable error order when both names are unknown.
    std::string group_missing;
    gid_t group{};
    if (!proc_credential_gid(ctx, group_value, group, group_missing)) {
        return proc_credential_result(ctx, group_missing.empty() ? errno : 0,
                                      group_missing.empty() ? nullptr : "group", group_missing);
    }
    std::string user_missing;
    uid_t uid{};
    if (!proc_credential_uid(ctx, user, uid, user_missing)) {
        return proc_credential_result(ctx, user_missing.empty() ? errno : 0,
                                      user_missing.empty() ? nullptr : "user", user_missing);
    }
    struct passwd* pw{getpwuid(uid)};
    if (pw == nullptr || pw->pw_name == nullptr) {
        return proc_credential_result(ctx, 0, "user", val_to_string(ctx, user));
    }
    const int rc{initgroups(pw->pw_name, group)};
    return proc_credential_result(ctx, rc == 0 ? 0 : errno);
}

JSValueRef proc_groups_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                          const JSValueRef[], JSValueRef*) {
    const int count{getgroups(0, nullptr)};
    if (count < 0) return JSObjectMakeArray(ctx, 0, nullptr, nullptr);
    std::vector<gid_t> groups(static_cast<std::size_t>(count));
    if (count > 0 && getgroups(count, groups.data()) < 0) return JSObjectMakeArray(ctx, 0, nullptr, nullptr);
    std::vector<JSValueRef> values;
    values.reserve(groups.size());
    for (const gid_t group : groups) values.push_back(JSValueMakeNumber(ctx, static_cast<double>(group)));
    return JSObjectMakeArray(ctx, values.size(), values.data(), nullptr);
}

// ── proc.spawnPty(file, argv, { ..., slave }): fork a child onto an EXISTING
// pseudo-terminal and return { pid } (or { errno }). The pty itself is created
// by proc.openPty below and owned by a Bun.Terminal, which is the only shape
// bun has: `Bun.spawn({terminal})` either borrows a live Terminal or builds one
// from an options object first, so there is exactly one pty implementation and
// one owner of the fds. `slave` stays open in the parent (the Terminal holds
// it) so the pty survives this child.
// The child setsid()s, claims the slave as its controlling terminal with
// TIOCSCTTY and dup2s it onto 0/1/2; exec failures cross a CLOEXEC status pipe
// exactly like spawnEx. Ref: bun-ref src/runtime/api/bun/Terminal.rs.
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
    // Permission Model: node enforces the ChildProcess scope at the fork
    // (src/spawn_sync.cc / process_wrap.cc), and so does this. `resource` is the
    // executable, which is what test-permission-child-process-cli asserts on.
    // permission_deny_spawn also handles mbun's worker-as-subprocess case.
    if (permission_deny_spawn(ctx, exc, file, o.env)) return JSValueMakeUndefined(ctx);
    permission_inject_child_env(o.env, o.haveEnv, argvStr);
    // `slave` — the descriptor of the pty this child is being attached to. It
    // belongs to a Bun.Terminal (bun js_bun_spawn_bindings.rs:823
    // `existing_terminal`), which also owns the window size, so nothing about
    // the pty is configured here.
    int slave{-1};
    if (argc > 2 && JSValueIsObject(ctx, args[2])) {
        JSObjectRef opt{JSValueToObject(ctx, args[2], nullptr)};
        JSValueRef sv{get_prop(ctx, opt, "slave")};
        if (JSValueIsNumber(ctx, sv)) { double d{JSValueToNumber(ctx, sv, nullptr)}; if (d >= 0) slave = static_cast<int>(d); }
    }
    if (slave < 0) return fs_throw(ctx, exc, "spawnPty: no pty slave descriptor (opts.slave)");

    int errPipe[2];
    if (pipe(errPipe) != 0) return fs_throw(ctx, exc, "spawnPty: pipe() failed");
    fcntl(errPipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(errPipe[1], F_SETFD, FD_CLOEXEC);

    const pid_t pid{fork()};
    if (pid < 0) { close(errPipe[0]); close(errPipe[1]); return fs_throw(ctx, exc, "spawnPty: fork() failed"); }
    if (pid == 0) {  // ── child ──
        close(errPipe[0]);
        { sigset_t ss; sigemptyset(&ss); sigprocmask(SIG_SETMASK, &ss, nullptr);
          for (int s = 1; s < NSIG; ++s) signal(s, SIG_DFL); }
        const auto failFd{[&](int e) {
            const std::int32_t v{e};
            (void)!write(errPipe[1], &v, sizeof v);
            _exit(127);
        }};
        // setsid + TIOCSCTTY + dup2 onto 0/1/2 is the platform layer's job:
        // it is async-signal-safe there too, and Windows has no equivalent at
        // all. Non-zero is the errno of the first step that failed.
        if (const int ptyErr{mbun::platform::pty::attach_controlling_terminal(slave)}; ptyErr != 0)
            failFd(ptyErr);
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
            mbun::platform::process::exec_path_env(file.c_str(), cargv.data(), cenv.data());
        } else {
            execvp(file.c_str(), cargv.data());
        }
        failFd(errno);
    }
    // ── parent ──
    close(errPipe[1]);
    // The slave is NOT closed here: it belongs to the Bun.Terminal that owns
    // this pty, which keeps it open so the pty survives the child and can host
    // the next one.
    std::int32_t childErr{0};
    ssize_t n;
    do { n = read(errPipe[0], &childErr, sizeof childErr); } while (n < 0 && errno == EINTR);
    close(errPipe[0]);
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    if (n == static_cast<ssize_t>(sizeof childErr)) {  // pre-exec/exec failure
        int status{0};
        waitpid(pid, &status, 0);
        set_prop(ctx, r, "errno", JSValueMakeNumber(ctx, childErr));
        return r;
    }
    set_prop(ctx, r, "pid", JSValueMakeNumber(ctx, static_cast<double>(pid)));
    return r;
}

// ── Bun.Terminal's PTY primitives ───────────────────────────────────────────
// spawnPty above creates a pty AND forks in one step, which is all
// `Bun.spawn({terminal: {...}})` needs. `new Bun.Terminal({...})` is the other
// half of the same API: a pty that exists on its own, before (and after) any
// child is attached to it. These three entry points are that half — creation,
// termios access, and raw mode — so the JS class stays a thin wrapper over the
// syscalls rather than re-deriving terminal semantics.
//
// PORT-SOURCE: compat/bun/src/runtime/api/bun/Terminal.rs create_pty_posix
// (:922) for the cooked-mode defaults, get_termios/set_termios (:1302) for the
// flag accessors, and bun_core::tty::State for raw mode.

// proc.openPty(cols, rows) → { master, write, slave }.
// `master` is the read side and the ioctl/termios handle; `write` is a second
// descriptor onto the same master so the read and write sides can be closed
// independently (same reason spawnPty dups it); `slave` stays open in this
// process so the pty does not collapse when an attached child exits.
JSValueRef proc_open_pty_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
    unsigned short cols{80}, rows{24};
    if (argc > 0 && JSValueIsNumber(ctx, args[0])) {
        const double d{JSValueToNumber(ctx, args[0], nullptr)};
        if (d > 0 && d < 65535) cols = static_cast<unsigned short>(d);
    }
    if (argc > 1 && JSValueIsNumber(ctx, args[1])) {
        const double d{JSValueToNumber(ctx, args[1], nullptr)};
        if (d > 0 && d < 65535) rows = static_cast<unsigned short>(d);
    }
    // Creation, the cooked-mode defaults and the CLOEXEC discipline all live in
    // mbun::platform::pty::open_pty. What stays here is the JS-facing half: the
    // message text for each failure stage, and the shape of the result object.
    const auto pty{mbun::platform::pty::open_pty({cols, rows})};
    if (!pty.ok()) {
        using Stage = mbun::platform::pty::OpenStage;
        switch (pty.stage) {
            case Stage::OpenMaster:   return fs_throw(ctx, exc, "openPty: posix_openpt() failed");
            case Stage::Unlock:       return fs_throw(ctx, exc, "openPty: grantpt/unlockpt failed");
            case Stage::ResolveSlave: return fs_throw(ctx, exc, "openPty: ptsname failed");
            case Stage::OpenSlave:    return fs_throw(ctx, exc, "openPty: open(pts) failed");
            default:                  return fs_throw(ctx, exc, "openPty: not supported on this platform");
        }
    }
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, r, "master", JSValueMakeNumber(ctx, static_cast<double>(pty.descriptors.master)));
    set_prop(ctx, r, "write", JSValueMakeNumber(ctx, static_cast<double>(pty.descriptors.write)));
    set_prop(ctx, r, "slave", JSValueMakeNumber(ctx, static_cast<double>(pty.descriptors.slave)));
    return r;
}

// proc.ptyTermios(fd) → { iflag, oflag, cflag, lflag } | null when fd is dead.
JSValueRef proc_pty_termios_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef*) {
    if (argc < 1 || !JSValueIsNumber(ctx, args[0])) return JSValueMakeNull(ctx);
    const int fd{static_cast<int>(JSValueToNumber(ctx, args[0], nullptr))};
    const auto flags{mbun::platform::pty::termios_flags(fd)};
    if (!flags) return JSValueMakeNull(ctx);
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, r, "iflag", JSValueMakeNumber(ctx, static_cast<double>(flags->input)));
    set_prop(ctx, r, "oflag", JSValueMakeNumber(ctx, static_cast<double>(flags->output)));
    set_prop(ctx, r, "cflag", JSValueMakeNumber(ctx, static_cast<double>(flags->control)));
    set_prop(ctx, r, "lflag", JSValueMakeNumber(ctx, static_cast<double>(flags->local)));
    return r;
}

// proc.ptySetTermios(fd, which, bits) — which: 0=iflag 1=oflag 2=cflag 3=lflag.
// Read-modify-write so the other three fields and c_cc survive, exactly like
// bun's set_termios_flag.
JSValueRef proc_pty_set_termios_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                   const JSValueRef args[], JSValueRef*) {
    if (argc < 3) return JSValueMakeBoolean(ctx, false);
    const int fd{static_cast<int>(JSValueToNumber(ctx, args[0], nullptr))};
    const int which{static_cast<int>(JSValueToNumber(ctx, args[1], nullptr))};
    const double raw{JSValueToNumber(ctx, args[2], nullptr)};
    if (fd < 0 || !std::isfinite(raw) || which < 0 || which > 3) return JSValueMakeBoolean(ctx, false);
    // The 32-bit narrowing is the JS contract, not a platform detail: the JS
    // side clamps flags to uint32 before calling, and widening it here would
    // start honouring bits that never reached the terminal before.
    const auto bits{static_cast<std::uint32_t>(static_cast<std::int64_t>(raw))};
    const auto field{static_cast<mbun::platform::pty::TermiosField>(which)};
    return JSValueMakeBoolean(ctx, mbun::platform::pty::set_termios_field(fd, field, bits));
}

// proc.ptySetRaw(fd, enable) → cfmakeraw() on, cooked defaults back off.
// bun keeps the pre-raw termios in the Terminal so a restore is exact; the
// saved copy lives on the JS object here for the same reason (`saved` is the
// object ptyTermios returned before raw mode was entered).
JSValueRef proc_pty_set_raw_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef*) {
    if (argc < 2) return JSValueMakeBoolean(ctx, false);
    const int fd{static_cast<int>(JSValueToNumber(ctx, args[0], nullptr))};
    const bool enable{JSValueToBoolean(ctx, args[1])};
    return JSValueMakeBoolean(ctx, mbun::platform::pty::set_raw_mode(fd, enable));
}

// proc.ptyResize(fd, cols, rows) → push a new window size to the pty master.
JSValueRef proc_pty_resize_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                              const JSValueRef args[], JSValueRef*) {
    if (argc >= 3) {
        const int fd{static_cast<int>(JSValueToNumber(ctx, args[0], nullptr))};
        mbun::platform::pty::resize(
            fd, {static_cast<std::uint16_t>(JSValueToNumber(ctx, args[1], nullptr)),
                 static_cast<std::uint16_t>(JSValueToNumber(ctx, args[2], nullptr))});
    }
    return JSValueMakeUndefined(ctx);
}

// ── proc.execve: replace THIS process image ──────────────────────────────────
// node src/node_process_methods.cc Execve, driven by
// lib/internal/process/per_thread.js execve(). The JS half does node's argument
// validation; this half is the syscall plus the two things node inherits from
// libuv and its permission model for free:
//
//   * the ChildProcess permission scope (node's THROW_IF_INSUFFICIENT_PERMISSIONS
//     with PermissionScope::kChildProcess), so `--permission` without
//     `--allow-child-process` refuses the exec with an ERR_ACCESS_DENIED that
//     carries permission/resource — test-process-execve-permission-fail asserts
//     all three.
//
//   * every inherited descriptor above stdio becoming CLOEXEC. node's fds are
//     CLOEXEC because libuv opens them that way, so after a successful execve the
//     listening socket is gone and the replacement image can bind the SAME port
//     (test-process-execve-socket does exactly that). mbun's sockets are not
//     CLOEXEC, so they are marked here.
//
//     Marked, never closed, and only after the target has been proven
//     executable: execve can FAIL, and test-process-execve-throws requires the
//     process to be fully intact afterwards — stdout writable, nextTick and
//     setImmediate still firing. FD_CLOEXEC has no effect until an exec actually
//     succeeds, so a failed execve leaves every descriptor usable.
void execve_mark_cloexec_above_stdio() {
#if defined(__linux__) && defined(SYS_close_range)
#  if !defined(CLOSE_RANGE_CLOEXEC)
#    define CLOSE_RANGE_CLOEXEC (1U << 2)
#  endif
    if (::syscall(SYS_close_range, 3u, ~0u, CLOSE_RANGE_CLOEXEC) == 0) return;
#endif
    // Fallback: walk the open descriptors. Deliberately NOT close_range_compat —
    // its own fallback CLOSES fds, which would maim a process whose execve fails.
    const long openMax{sysconf(_SC_OPEN_MAX)};
    const int limit{static_cast<int>(openMax > 0 ? std::min<long>(openMax, 65536) : 1024)};
    for (int fd{3}; fd < limit; ++fd) {
        const int flags{fcntl(fd, F_GETFD)};
        if (flags >= 0 && (flags & FD_CLOEXEC) == 0) (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

// node's ErrnoException(errno, "execve", nullptr, execPath): `errno` is the raw
// POSITIVE errno here, not libuv's negated form (test-process-execve-throws
// asserts `err.errno > 0`), alongside code/syscall/path.
JSValueRef execve_throw_errno(JSContextRef ctx, JSValueRef* exc, int e, const std::string& path) {
    const std::string code{fd_errno_code(e)};
    // node ErrnoException(isolate, errno, syscall, msg, path) formats as
    // "<CODE>, <strerror> '<path>'" -- NOT the libuv "CODE: msg, syscall 'path'"
    // shape used by the fs bindings. execve reaches JS through ErrnoException
    // (node_process_methods.cc Execve -> ThrowErrnoException), and bun's
    // BunProcess.cpp builds the identical string, so both corpora read this one.
    const std::string msg{(code.empty() ? "E" + std::to_string(e) : code) + ", "
                          + fd_errno_message(e) + " '" + path + "'"};
    JSValueRef m{make_string(ctx, msg)};
    JSObjectRef err{JSObjectMakeError(ctx, 1, &m, nullptr)};
    if (!code.empty()) set_prop(ctx, err, "code", make_string(ctx, code));
    set_prop(ctx, err, "errno", JSValueMakeNumber(ctx, e));
    set_prop(ctx, err, "syscall", make_string(ctx, "execve"));
    set_prop(ctx, err, "path", make_string(ctx, path));
    if (exc != nullptr) *exc = err;
    return JSValueMakeUndefined(ctx);
}

// proc.execve(execPath, argv, env) — argv is the FULL argv (including argv[0]);
// env is an array of "KEY=VALUE" strings, as node's JS layer builds it. Returns
// only on failure.
JSValueRef proc_execve_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
    const std::string path{fs_arg(ctx, argc, args, 0)};
    if (permission_deny_scope(ctx, exc, mbun::permission::Scope::ChildProcess, path)) {
        return JSValueMakeUndefined(ctx);
    }
    const auto readList{[&](std::size_t idx, std::vector<std::string>& out) {
        if (argc <= idx || !JSValueIsArray(ctx, args[idx])) return;
        JSObjectRef arr{JSValueToObject(ctx, args[idx], nullptr)};
        JSStringRef lenKey{JSStringCreateWithUTF8CString("length")};
        const double len{JSValueToNumber(ctx, JSObjectGetProperty(ctx, arr, lenKey, nullptr), nullptr)};
        JSStringRelease(lenKey);
        for (unsigned i = 0; i < static_cast<unsigned>(len); ++i) {
            out.push_back(val_to_string(ctx, JSObjectGetPropertyAtIndex(ctx, arr, i, nullptr)));
        }
    }};
    std::vector<std::string> argvStr;
    readList(1, argvStr);
    std::vector<std::string> envStr;
    readList(2, envStr);
    // Refuse BEFORE touching descriptors, so a doomed execve leaves the process
    // whole. access(2) covers the cases the corpus exercises (missing file,
    // non-executable, permission) and reports the errno execve itself would.
    if (::access(path.c_str(), X_OK) != 0) return execve_throw_errno(ctx, exc, errno, path);
    // node flushes stdio before handing the image over; buffered bytes would
    // otherwise die with the old image.
    std::fflush(nullptr);
    execve_mark_cloexec_above_stdio();
    std::vector<char*> cargv;
    for (auto& a : argvStr) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    std::vector<char*> cenv;
    for (auto& e : envStr) cenv.push_back(const_cast<char*>(e.c_str()));
    cenv.push_back(nullptr);
    ::execve(path.c_str(), cargv.data(), cenv.data());
    return execve_throw_errno(ctx, exc, errno, path);
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
    // node's schemeless block list (lib/internal/bootstrap/realm.js
    // `schemelessBlockList`): four builtins are requirable ONLY with the
    // `node:` scheme, so a bare `require("test")` must miss the builtin table
    // entirely and fall through to the ordinary node_modules resolver — that is
    // what lets a userland package named `test` shadow the runner. mbun
    // registered both keys, so bare `test` returned the builtin `nodeTest`
    // function and `node_modules/test/index.js` was unreachable. The `node:`-
    // prefixed spec is a different string and is unaffected; the
    // BuiltinModule shim in this file already models the same list for
    // `module.isBuiltin` / `builtinModules`.
    if (mod == "test" || mod == "test/reporters" || mod == "sqlite" || mod == "quic") {
        return std::nullopt;
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
// The fact and its node spelling both belong to mbun.platform.capability, which
// resolves them once at compile time; there is no conditional left here.
constexpr const char* host_platform() {
    return mbun::platform::platform_name(mbun::platform::HOST_PLATFORM);
}
constexpr const char* host_arch() {
    return mbun::platform::architecture_name(mbun::platform::HOST_ARCHITECTURE);
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
    const std::string dest{val_to_string(ctx, args[0])};
    // node throws a real uvException here, not a string: an Error carrying
    // .code/.errno/.syscall/.path (the cwd we failed to leave) and .dest (the
    // requested directory), with the two-path message shape
    // "ENOENT: no such file or directory, chdir '<cwd>' -> '<dest>'".
    // test-process-chdir-errormessage asserts every one of those fields; a
    // thrown string satisfied none of them. fs_make_error parses that exact
    // layout (io_bindings.inc), so reuse it rather than rebuilding the error.
    if (::chdir(dest.c_str()) != 0) {
        const int e{errno};
        if (exc != nullptr) {
            std::error_code cwdEc;
            const std::string cwd{std::filesystem::current_path(cwdEc).string()};
            std::string code{fd_errno_code(e)};
            if (code.empty()) code = "UNKNOWN";
            *exc = fs_make_error(ctx, code + ": " + fd_errno_message(e) + ", chdir '" + cwd +
                                          "' -> '" + dest + "'");
        }
    }
    return JSValueMakeUndefined(ctx);
}

// process.exit([code]) — node lib/internal/process/per_thread.js: settle the
// exit code, emit 'exit' synchronously, then leave. It used to std::exit()
// straight away, so no 'exit' listener ever ran (and node's own test harness
// verifies every common.mustCall from inside one). The whole sequence lives in
// JS (__mbun_run_exit, js_builtins :node_process_lifecycle) and hands back the
// status to leave with — process.exitCode assigned by a listener wins.
int run_exit_sequence(JSContextRef ctx, JSValueRef codeArg) {
    JSObjectRef global{JSContextGetGlobalObject(ctx)};
    JSStringRef name{JSStringCreateWithUTF8CString("__mbun_run_exit")};
    JSValueRef fnVal{JSObjectGetProperty(ctx, global, name, nullptr)};
    JSStringRelease(name);
    const JSValueRef fallback{codeArg};
    const auto asStatus{[&](JSValueRef v) {
        if (v == nullptr || JSValueIsUndefined(ctx, v) || JSValueIsNull(ctx, v)) return 0;
        const double d{JSValueToNumber(ctx, v, nullptr)};
        return std::isnan(d) ? 0 : static_cast<int>(d);
    }};
    if (fnVal == nullptr || !JSValueIsObject(ctx, fnVal)) return asStatus(fallback);
    JSObjectRef fn{JSValueToObject(ctx, fnVal, nullptr)};
    if (fn == nullptr || !JSObjectIsFunction(ctx, fn)) return asStatus(fallback);
    const JSValueRef callArgs[1]{codeArg != nullptr ? codeArg : JSValueMakeUndefined(ctx)};
    JSValueRef exc{nullptr};
    JSValueRef result{JSObjectCallAsFunction(ctx, fn, nullptr, 1, callArgs, &exc)};
    if (exc != nullptr) return 1;
    return asStatus(result);
}

JSValueRef process_exit_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef*) {
    std::exit(run_exit_sequence(ctx, argc > 0 ? args[0] : nullptr));
}

// process.reallyExit(code) — node's raw exit: no 'exit' emission, no handlers.
JSValueRef process_really_exit_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
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
            // Permission Model: loading a module is an fs READ of that file, and
            // node gates it the same way (an unlisted require() fails with
            // ERR_ACCESS_DENIED, not MODULE_NOT_FOUND — test-permission-fs-require).
            // Gating HERE, in the loader's own read hook, means the bytes are
            // never even fetched; the require path turns the recorded denial into
            // the right error.
            if (gPermission.enabled() && !gPermission.warning_only() &&
                !gPermission.is_granted(mbun::permission::Scope::FileSystemRead, p)) {
                gPermissionDeniedModuleRead = std::string{p};
                return std::nullopt;
            }
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
JSValueRef module_cache_get_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef* exc);
JSValueRef resolve_native_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef* exc);
JSValueRef transpile_native_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef* exc);
// CAP-NAPI: process.dlopen trampoline (defined in engine.inc, backed by
// runtime/napi_objects.inc mbun_napi_process_dlopen).
JSValueRef napi_dlopen_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc);
