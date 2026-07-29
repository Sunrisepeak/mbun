// ── Permission Model: propagate the sandbox to a child process ───────────────
// node's lib/child_process.js copyPermissionModelFlagsToEnv splices the parent's
// permission flags into the CHILD's NODE_OPTIONS, so `--allow-child-process`
// grants the ability to spawn — not the ability to spawn an UNGATED process.
// Without this, one allowed spawn is a complete escape from the sandbox.
//
// It is done here, at the fork boundary, rather than in the JS child_process
// shim: every spawn path in this runtime (node:child_process, Bun.spawn, the pty
// and "ex" variants) funnels through these helpers, and a JS-level splice would
// miss the ones that do not go through node:child_process at all.
//
// node's exceptions are kept: the child's OWN --permission (in argv, or already
// in the NODE_OPTIONS it was handed) wins and nothing is added.
extern "C" char** environ;

void permission_inject_child_env(std::vector<std::string>& env, bool& haveEnv,
                                 const std::vector<std::string>& argv) {
    if (!gPermission.enabled()) return;
    const auto& flags{gPermissionOptions.presentFlags};
    if (flags.empty()) return;
    for (const std::string& a : argv) {
        if (a == "--permission" || a == "--permission-audit") return;
    }

    // A child with no explicit env inherits ours; to add one variable we have to
    // materialise the inherited set rather than mutate the PARENT's environ
    // (the corpus asserts the parent's own process.env.NODE_OPTIONS stays unset).
    if (!haveEnv) {
        for (char** e = environ; e != nullptr && *e != nullptr; ++e) env.emplace_back(*e);
        haveEnv = true;
    }

    std::string extra{};
    for (const std::string& f : flags) {
        if (!extra.empty()) extra += ' ';
        extra += f;
    }

    for (std::string& entry : env) {
        if (!entry.starts_with("NODE_OPTIONS=")) continue;
        // Already carries a permission flag: node leaves it alone.
        if (entry.find("--permission") != std::string::npos) return;
        const bool empty{entry.size() == std::string_view{"NODE_OPTIONS="}.size()};
        entry += (empty ? "" : " ") + extra;
        return;
    }
    env.push_back("NODE_OPTIONS=" + extra);
}

JSValueRef cp_spawn_sync_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "spawnSync: not supported on Windows yet");
#else
    std::string file{fs_arg(ctx, argc, args, 0)};
    std::vector<std::string> argvStr{file};
    if (argc > 1 && JSValueIsArray(ctx, args[1])) {
        JSObjectRef arr{JSValueToObject(ctx, args[1], nullptr)};
        JSStringRef lenKey{JSStringCreateWithUTF8CString("length")};
        double len{JSValueToNumber(ctx, JSObjectGetProperty(ctx, arr, lenKey, nullptr), nullptr)};
        JSStringRelease(lenKey);
        for (unsigned i = 0; i < static_cast<unsigned>(len); ++i) {
            JSValueRef el{JSObjectGetPropertyAtIndex(ctx, arr, i, nullptr)};
            argvStr.push_back(val_to_string(ctx, el));
        }
    }
    // options.cwd / options.input / options.env / options.detached / options.stdio
    std::string cwd, input;
    std::vector<std::string> envStr;  // "KEY=VALUE" pairs; empty ⇒ inherit environ
    bool haveEnv{false};
    bool detached{false};
    double uid{-1}, gid{-1};
    // options.stdio, per slot: Pipe (capture, the default), Inherit (hand the
    // parent's own fd to the child), Ignore (/dev/null). node returns `null` for a
    // non-piped stdout/stderr, and `stdio: 'inherit'` is not a cosmetic detail: a
    // child that writes more than a pipe buffer to the fd we drain LAST deadlocks
    // against this synchronous reader, which is how `stdio: 'inherit'` spawns hung
    // instead of streaming (test/common/index.js's flag re-exec does exactly this).
    enum class Stdio { Pipe, Inherit, Ignore };
    Stdio stdioMode[3]{Stdio::Pipe, Stdio::Pipe, Stdio::Pipe};
    int stdioFd[3]{-1, -1, -1};  // numeric stdio entry: dup this parent fd
    // options.timeoutMs / options.killSignalNum: node's spawnSync(…, { timeout,
    // killSignal }) (src/spawn_sync.cc SyncProcessRunner::SetupTimeoutHandler).
    // The JS wrapper resolves the signal NAME to a number before it gets here so
    // this layer never carries a signal table. -1 ⇒ no timeout.
    double timeoutMs{-1};
    int killSigNum{SIGTERM};
    if (argc > 2 && JSValueIsObject(ctx, args[2])) {
        JSObjectRef opt{JSValueToObject(ctx, args[2], nullptr)};
        JSValueRef toV{get_prop(ctx, opt, "timeoutMs")};
        if (JSValueIsNumber(ctx, toV)) {
            const double v{JSValueToNumber(ctx, toV, nullptr)};
            if (std::isfinite(v) && v > 0) timeoutMs = v;
        }
        JSValueRef ksV{get_prop(ctx, opt, "killSignalNum")};
        if (JSValueIsNumber(ctx, ksV)) {
            const int s{static_cast<int>(JSValueToNumber(ctx, ksV, nullptr))};
            if (s > 0 && s < NSIG) killSigNum = s;
        }
        const auto readStdioSlot{[&](JSValueRef v, std::size_t slot) {
            if (JSValueIsNumber(ctx, v)) {
                stdioMode[slot] = Stdio::Inherit;
                stdioFd[slot] = static_cast<int>(JSValueToNumber(ctx, v, nullptr));
                return;
            }
            if (!JSValueIsString(ctx, v)) return;  // null/undefined/stream ⇒ pipe
            const std::string s{val_to_string(ctx, v)};
            if (s == "inherit") stdioMode[slot] = Stdio::Inherit;
            else if (s == "ignore") stdioMode[slot] = Stdio::Ignore;
            else stdioMode[slot] = Stdio::Pipe;
        }};
        JSValueRef stdioV{get_prop(ctx, opt, "stdio")};
        if (JSValueIsArray(ctx, stdioV)) {
            JSObjectRef arr{JSValueToObject(ctx, stdioV, nullptr)};
            for (unsigned s = 0; s < 3; ++s) {
                readStdioSlot(JSObjectGetPropertyAtIndex(ctx, arr, s, nullptr), s);
            }
        } else if (JSValueIsString(ctx, stdioV)) {
            for (std::size_t s = 0; s < 3; ++s) readStdioSlot(stdioV, s);
        }
        detached = JSValueToBoolean(ctx, get_prop(ctx, opt, "detached"));
        JSValueRef uidV{get_prop(ctx, opt, "uid")};
        if (JSValueIsNumber(ctx, uidV)) uid = JSValueToNumber(ctx, uidV, nullptr);
        JSValueRef gidV{get_prop(ctx, opt, "gid")};
        if (JSValueIsNumber(ctx, gidV)) gid = JSValueToNumber(ctx, gidV, nullptr);
        JSValueRef argv0V{get_prop(ctx, opt, "argv0")};
        if (JSValueIsString(ctx, argv0V)) {
            argvStr.front() = val_to_string(ctx, argv0V);
        }
        JSValueRef cwdV{get_prop(ctx, opt, "cwd")};
        if (JSValueIsString(ctx, cwdV)) cwd = val_to_string(ctx, cwdV);
        JSValueRef inV{get_prop(ctx, opt, "input")};
        if (JSValueIsString(ctx, inV)) input = val_to_string(ctx, inV);
        JSValueRef envV{get_prop(ctx, opt, "env")};
        if (JSValueIsObject(ctx, envV)) {
            haveEnv = true;
            JSObjectRef envObj{JSValueToObject(ctx, envV, nullptr)};
            JSPropertyNameArrayRef names{JSObjectCopyPropertyNames(ctx, envObj)};
            const std::size_t cnt{JSPropertyNameArrayGetCount(names)};
            for (std::size_t i = 0; i < cnt; ++i) {
                JSStringRef k{JSPropertyNameArrayGetNameAtIndex(names, i)};
                const std::size_t kb{JSStringGetMaximumUTF8CStringSize(k)};
                std::string key(kb, '\0');
                key.resize(JSStringGetUTF8CString(k, key.data(), kb));
                if (!key.empty() && key.back() == '\0') key.pop_back();
                JSValueRef vv{JSObjectGetProperty(ctx, envObj, k, nullptr)};
                if (!JSValueIsUndefined(ctx, vv) && !JSValueIsNull(ctx, vv)) {
                    envStr.push_back(key + "=" + val_to_string(ctx, vv));
                }
            }
            JSPropertyNameArrayRelease(names);
        }
    }

    // `input` forces a pipe on stdin whatever options.stdio asked for (node does
    // the same: there is nowhere else to write it).
    if (!input.empty()) stdioMode[0] = Stdio::Pipe;
    const bool pipeIn{stdioMode[0] == Stdio::Pipe};
    const bool pipeOut{stdioMode[1] == Stdio::Pipe};
    const bool pipeErr{stdioMode[2] == Stdio::Pipe};

    int outPipe[2]{-1, -1}, errPipe[2]{-1, -1}, inPipe[2]{-1, -1}, statPipe[2];
    // Permission Model: node enforces the ChildProcess scope at the fork
    // (src/spawn_sync.cc / process_wrap.cc), and so does this. `resource` is the
    // executable, which is what test-permission-child-process-cli asserts on.
    // permission_deny_spawn also handles mbun's worker-as-subprocess case.
    if (permission_deny_spawn(ctx, exc, file, envStr)) return JSValueMakeUndefined(ctx);
    permission_inject_child_env(envStr, haveEnv, argvStr);
    if ((pipeIn && pipe(inPipe) != 0) || (pipeOut && pipe(outPipe) != 0) ||
        (pipeErr && pipe(errPipe) != 0) || pipe(statPipe) != 0) {
        return fs_throw(ctx, exc, "spawnSync: pipe() failed");
    }
    fcntl(statPipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(statPipe[1], F_SETFD, FD_CLOEXEC);
    const pid_t pid{fork()};
    if (pid < 0) {
        return fs_throw(ctx, exc, "spawnSync: fork() failed");
    }
    if (pid == 0) {  // child
        close(statPipe[0]);
        const auto failStat{[&](int e) {
            const std::int32_t v{e};
            (void)!write(statPipe[1], &v, sizeof v);
            _exit(127);
        }};
        if (detached) (void)setsid();  // new session/process group (spawnSync detached)
        // Wire each standard fd per its stdio mode. "inherit" is the no-op case:
        // the parent's fd is already there (or is the numeric fd the caller named).
        const auto wire{[&](int slot, int childEnd) {
            switch (stdioMode[static_cast<std::size_t>(slot)]) {
            case Stdio::Pipe:
                if (childEnd >= 0) dup2(childEnd, slot);
                break;
            case Stdio::Inherit: {
                const int from{stdioFd[static_cast<std::size_t>(slot)]};
                if (from >= 0 && from != slot) dup2(from, slot);
                break;
            }
            case Stdio::Ignore: {
                const int devnull{open("/dev/null", slot == 0 ? O_RDONLY : O_WRONLY)};
                if (devnull >= 0) {
                    dup2(devnull, slot);
                    close(devnull);
                }
                break;
            }
            }
        }};
        wire(0, inPipe[0]);
        wire(1, outPipe[1]);
        wire(2, errPipe[1]);
        for (const int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1],
                             errPipe[0], errPipe[1]}) {
            if (fd >= 3) close(fd);  // never the freshly wired 0/1/2
        }
        if (!cwd.empty()) { if (chdir(cwd.c_str()) != 0) failStat(errno); }
        if (gid >= 0) {
            if (setgroups(0, nullptr) != 0 && errno == EPERM) failStat(EPERM);
            if (setgid(static_cast<gid_t>(gid)) != 0) failStat(errno);
        }
        if (uid >= 0 && setuid(static_cast<uid_t>(uid)) != 0) failStat(errno);
        std::vector<char*> cargv;
        for (auto& a : argvStr) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        if (haveEnv) {
            std::vector<char*> cenv;
            for (auto& e : envStr) cenv.push_back(const_cast<char*>(e.c_str()));
            cenv.push_back(nullptr);
            execvpe(file.c_str(), cargv.data(), cenv.data());
        } else {
            execvp(file.c_str(), cargv.data());
        }
        failStat(errno);  // exec failed (e.g. ENOENT)
    }
    // parent
    close(statPipe[1]);
    if (pipeIn) close(inPipe[0]);
    if (pipeOut) close(outPipe[1]);
    if (pipeErr) close(errPipe[1]);
    std::int32_t childErr{0};
    {
        ssize_t sn;
        do { sn = read(statPipe[0], &childErr, sizeof childErr); } while (sn < 0 && errno == EINTR);
        if (sn != static_cast<ssize_t>(sizeof childErr)) childErr = 0;
    }
    close(statPipe[0]);
    if (pipeIn) {
        if (!input.empty()) {
            ssize_t off = 0;
            while (off < static_cast<ssize_t>(input.size())) {
                ssize_t w{write(inPipe[1], input.data() + off, input.size() - off)};
                if (w <= 0) break;
                off += w;
            }
        }
        close(inPipe[1]);
    }
    // Drain both pipes concurrently under an optional deadline. Draining them in
    // sequence (stdout to EOF, then stderr) deadlocks a child that fills the
    // pipe we read LAST, and a blocking read cannot honour options.timeout at
    // all — a spawnSync({ timeout }) against a child that never exits hung this
    // process forever. poll() gives both properties at once.
    std::string outStr, errStr;
    bool timedOut{false};
    {
        const auto t0{std::chrono::steady_clock::now()};
        const auto elapsedMs{[&t0] {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }};
        int fds[2]{pipeOut ? outPipe[0] : -1, pipeErr ? errPipe[0] : -1};
        std::string* bufs[2]{&outStr, &errStr};
        for (int i = 0; i < 2; ++i) {
            if (fds[i] >= 0) fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL, 0) | O_NONBLOCK);
        }
        char buf[4096];
        // Both pipes at EOF means the child's output is complete and every
        // writer has closed — there is nothing left to wait FOR. Keeping
        // `(timeoutMs >= 0 && !timedOut)` as a third disjunct here made the loop
        // spin with zero fds, poll() a null set until the deadline, and then
        // "time out" and kill a child that had already exited: a spawnSync with
        // { timeout: 30000 } against a child that finished in 25ms returned
        // status=null after the full 30s. The timeout must bound the READING,
        // not add a mandatory wait. Measured: node returns status=3 in 25ms for
        // the same case, mbun took 20033ms before this and 25ms after.
        while (fds[0] >= 0 || fds[1] >= 0) {
            pollfd pf[2];
            int idx[2];
            int n{0};
            for (int i = 0; i < 2; ++i) {
                if (fds[i] < 0) continue;
                pf[n] = pollfd{.fd = fds[i], .events = POLLIN, .revents = 0};
                idx[n] = i;
                ++n;
            }
            int wait{-1};
            if (timeoutMs >= 0 && !timedOut) {
                const double left{timeoutMs - elapsedMs()};
                wait = left <= 0 ? 0 : static_cast<int>(left) + 1;
            }
            const int rc{poll(n ? pf : nullptr, static_cast<nfds_t>(n), wait)};
            if (rc < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (rc == 0) {
                if (timeoutMs >= 0 && !timedOut && elapsedMs() >= timeoutMs) {
                    // node kills the child and then keeps reading whatever it
                    // already produced, so the result still carries partial output.
                    timedOut = true;
                    kill(pid, killSigNum);
                }
                continue;
            }
            for (int k = 0; k < n; ++k) {
                if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                const int i{idx[k]};
                for (;;) {
                    const ssize_t r{read(fds[i], buf, sizeof buf)};
                    if (r > 0) { bufs[i]->append(buf, static_cast<std::size_t>(r)); continue; }
                    if (r < 0 && errno == EINTR) continue;
                    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    close(fds[i]);
                    fds[i] = -1;
                    break;  // EOF or a dead pipe
                }
            }
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i] >= 0) close(fds[i]);
        }
    }
    int status{0};
    waitpid(pid, &status, 0);
    int code{WIFEXITED(status) ? WEXITSTATUS(status) : -1};
    int sig{WIFSIGNALED(status) ? WTERMSIG(status) : 0};

    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    // Child-side pre-exec failure (setuid EPERM, chdir/exec ENOENT, …): node
    // surfaces it as result.error and leaves status/stdio null. `errnoCode` maps
    // the raw errno to its POSIX name in the JS wrapper.
    if (childErr != 0) {
        set_prop(ctx, r, "errno", JSValueMakeNumber(ctx, childErr));
        set_prop(ctx, r, "status", JSValueMakeNull(ctx));
        set_prop(ctx, r, "signal", JSValueMakeNull(ctx));
        set_prop(ctx, r, "pid", JSValueMakeNumber(ctx, static_cast<double>(pid)));
        set_prop(ctx, r, "stdout", JSValueMakeNull(ctx));
        set_prop(ctx, r, "stderr", JSValueMakeNull(ctx));
        return r;
    }
    // node: exactly one of status/signal is non-null. A child killed by a signal
    // has `status === null` (lib/child_process.js spawnSync), not -1 — callers
    // branch on `result.signal` first and then trust `result.status` as an exit
    // code, so a numeric -1 there reads as "exited with -1".
    set_prop(ctx, r, "status", sig ? JSValueMakeNull(ctx) : JSValueMakeNumber(ctx, code));
    set_prop(ctx, r, "signal",
             sig ? make_string(ctx, std::to_string(sig)) : JSValueMakeNull(ctx));
    set_prop(ctx, r, "pid", JSValueMakeNumber(ctx, static_cast<double>(pid)));
    set_prop(ctx, r, "timedOut", JSValueMakeBoolean(ctx, timedOut));
    // node reports a non-piped stdio slot as null, not "" — `stdio: 'inherit'`
    // went straight to the parent's fd and there is nothing to hand back.
    set_prop(ctx, r, "stdout", pipeOut ? make_string(ctx, outStr) : JSValueMakeNull(ctx));
    set_prop(ctx, r, "stderr", pipeErr ? make_string(ctx, errStr) : JSValueMakeNull(ctx));
    return r;
#endif
}
// ── binary-safe subprocess primitives (__mbunProcNative) ────────────────────
// Bytes cross the JS↔native boundary as base64 strings (the C API string path
// is UTF-8 + NUL-terminated, so raw bytes would be corrupted). Powers
// Bun.spawn with piped stdin/stdout, Bun.stdin, and binary process.stdout.write.

constexpr char kB64Alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const unsigned char* data, std::size_t n) {
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        const unsigned b0{data[i]};
        const unsigned b1{i + 1 < n ? data[i + 1] : 0u};
        const unsigned b2{i + 2 < n ? data[i + 2] : 0u};
        out.push_back(kB64Alpha[b0 >> 2]);
        out.push_back(kB64Alpha[((b0 & 0x3u) << 4) | (b1 >> 4)]);
        out.push_back(i + 1 < n ? kB64Alpha[((b1 & 0xfu) << 2) | (b2 >> 6)] : '=');
        out.push_back(i + 2 < n ? kB64Alpha[b2 & 0x3fu] : '=');
    }
    return out;
}

std::string b64_decode(std::string_view s) {
    static const auto table{[] {
        std::array<signed char, 256> t{};
        t.fill(-1);
        for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(kB64Alpha[i])] = static_cast<signed char>(i);
        return t;
    }()};
    std::string out;
    out.reserve((s.size() / 4) * 3);
    int acc{0}, bits{0};
    for (const char c : s) {
        const signed char v{table[static_cast<unsigned char>(c)]};
        if (v < 0) continue;  // '=', whitespace
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xff));
        }
    }
    return out;
}

#if !defined(_WIN32)
// Shared option parsing: {cwd, env} from an options object.
struct SpawnOpts {
    std::string cwd;
    std::vector<std::string> env;
    bool haveEnv{false};
};

SpawnOpts parse_spawn_opts(JSContextRef ctx, std::size_t argc, const JSValueRef args[]) {
    SpawnOpts o;
    if (argc > 2 && JSValueIsObject(ctx, args[2])) {
        JSObjectRef opt{JSValueToObject(ctx, args[2], nullptr)};
        JSValueRef cwdV{get_prop(ctx, opt, "cwd")};
        if (JSValueIsString(ctx, cwdV)) o.cwd = val_to_string(ctx, cwdV);
        JSValueRef envV{get_prop(ctx, opt, "env")};
        if (JSValueIsObject(ctx, envV)) {
            o.haveEnv = true;
            JSObjectRef envObj{JSValueToObject(ctx, envV, nullptr)};
            JSPropertyNameArrayRef names{JSObjectCopyPropertyNames(ctx, envObj)};
            const std::size_t cnt{JSPropertyNameArrayGetCount(names)};
            for (std::size_t i = 0; i < cnt; ++i) {
                JSStringRef k{JSPropertyNameArrayGetNameAtIndex(names, i)};
                const std::size_t kb{JSStringGetMaximumUTF8CStringSize(k)};
                std::string key(kb, '\0');
                key.resize(JSStringGetUTF8CString(k, key.data(), kb));
                if (!key.empty() && key.back() == '\0') key.pop_back();
                JSValueRef vv{JSObjectGetProperty(ctx, envObj, k, nullptr)};
                if (!JSValueIsUndefined(ctx, vv) && !JSValueIsNull(ctx, vv)) {
                    o.env.push_back(key + "=" + val_to_string(ctx, vv));
                }
            }
            JSPropertyNameArrayRelease(names);
        }
    }
    return o;
}

std::vector<std::string> parse_spawn_argv(JSContextRef ctx, std::size_t argc, const JSValueRef args[],
                                          const std::string& file) {
    std::vector<std::string> argvStr{file};
    if (argc > 1 && JSValueIsArray(ctx, args[1])) {
        JSObjectRef arr{JSValueToObject(ctx, args[1], nullptr)};
        JSStringRef lenKey{JSStringCreateWithUTF8CString("length")};
        double len{JSValueToNumber(ctx, JSObjectGetProperty(ctx, arr, lenKey, nullptr), nullptr)};
        JSStringRelease(lenKey);
        for (unsigned i = 0; i < static_cast<unsigned>(len); ++i) {
            argvStr.push_back(val_to_string(ctx, JSObjectGetPropertyAtIndex(ctx, arr, i, nullptr)));
        }
    }
    return argvStr;
}

// Resolve argv[0] against the CHILD's env.PATH (not the caller's). execvpe
// searches the *caller's* environ PATH, so a bare exe name resolved against a
// custom env.PATH would miss it. When file has no '/', walk each dir in the
// child env's PATH for an executable and return its absolute path; else "".
std::string resolve_in_env_path(const std::string& file, const std::vector<std::string>& env) {
    if (file.find('/') != std::string::npos) return "";
    std::string_view path;
    for (const auto& e : env) {
        if (e.size() >= 5 && e.compare(0, 5, "PATH=") == 0) { path = std::string_view{e}.substr(5); break; }
    }
    if (path.empty()) return "";
    std::size_t start{0};
    while (start <= path.size()) {
        std::size_t end{path.find(':', start)};
        if (end == std::string_view::npos) end = path.size();
        std::string_view dir{path.substr(start, end - start)};
        std::string cand{dir.empty() ? file : std::string{dir} + "/" + file};
        if (access(cand.c_str(), X_OK) == 0) return cand;
        start = end + 1;
    }
    return "";
}

[[noreturn]] void spawn_child_exec(const std::string& file, std::vector<std::string>& argvStr,
                                   const SpawnOpts& o) {
    if (!o.cwd.empty()) { if (chdir(o.cwd.c_str()) != 0) _exit(127); }
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
    _exit(127);
}
#endif

// proc.spawn(file, args, {cwd, env}) → { pid, in, out, err } with live pipe fds.
JSValueRef proc_spawn_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "proc.spawn: not supported on Windows yet");
#else
    std::string file{fs_arg(ctx, argc, args, 0)};
    std::vector<std::string> argvStr{parse_spawn_argv(ctx, argc, args, file)};
    SpawnOpts o{parse_spawn_opts(ctx, argc, args)};
    // Permission Model: node enforces the ChildProcess scope at the fork
    // (src/spawn_sync.cc / process_wrap.cc), and so does this. `resource` is the
    // executable, which is what test-permission-child-process-cli asserts on.
    // permission_deny_spawn also handles mbun's worker-as-subprocess case.
    if (permission_deny_spawn(ctx, exc, file, o.env)) return JSValueMakeUndefined(ctx);
    permission_inject_child_env(o.env, o.haveEnv, argvStr);
    int inPipe[2], outPipe[2], errPipe[2];
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        return fs_throw(ctx, exc, "proc.spawn: pipe() failed");
    }
    const pid_t pid{fork()};
    if (pid < 0) return fs_throw(ctx, exc, "proc.spawn: fork() failed");
    if (pid == 0) {
        dup2(inPipe[0], 0); dup2(outPipe[1], 1); dup2(errPipe[1], 2);
        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        spawn_child_exec(file, argvStr, o);
    }
    close(inPipe[0]); close(outPipe[1]); close(errPipe[1]);
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, r, "pid", JSValueMakeNumber(ctx, static_cast<double>(pid)));
    set_prop(ctx, r, "in", JSValueMakeNumber(ctx, inPipe[1]));
    set_prop(ctx, r, "out", JSValueMakeNumber(ctx, outPipe[0]));
    set_prop(ctx, r, "err", JSValueMakeNumber(ctx, errPipe[0]));
    return r;
#endif
}

// proc.write(fd, b64) → bytes written (full write loop).
JSValueRef proc_write_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "proc.write: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    std::string data{b64_decode(fs_arg(ctx, argc, args, 1))};
    std::size_t off{0};
    while (off < data.size()) {
        const ssize_t w{write(fd, data.data() + off, data.size() - off)};
        if (w <= 0) return fs_throw(ctx, exc, "proc.write: write() failed");
        off += static_cast<std::size_t>(w);
    }
    return JSValueMakeNumber(ctx, static_cast<double>(off));
#endif
}

// proc.read(fd, max) → base64 chunk (blocking; ≥1 byte) or null on EOF.
JSValueRef proc_read_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                        const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "proc.read: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    std::size_t max{argc > 1 ? static_cast<std::size_t>(JSValueToNumber(ctx, args[1], nullptr)) : 65536};
    if (max == 0 || max > (1u << 24)) max = 65536;
    std::string buf(max, '\0');
    ssize_t n;
    do { n = read(fd, buf.data(), max); } while (n < 0 && errno == EINTR);
    if (n < 0) return fs_throw(ctx, exc, "proc.read: read() failed");
    if (n == 0) return JSValueMakeNull(ctx);
    return make_string(ctx, b64_encode(reinterpret_cast<const unsigned char*>(buf.data()),
                                       static_cast<std::size_t>(n)));
#endif
}

JSValueRef proc_close_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef*) {
#if !defined(_WIN32)
    if (argc > 0) close(static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)));
#endif
    return JSValueMakeUndefined(ctx);
}

#if !defined(_WIN32)
// proc.setNonBlock(fd) → set O_NONBLOCK on a parent-side pipe fd (the async child
// driver uses this so a large stdin write never blocks the single-threaded loop).
JSValueRef proc_set_nonblock_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                const JSValueRef args[], JSValueRef*) {
    if (argc > 0) {
        const int fd{static_cast<int>(JSValueToNumber(ctx, args[0], nullptr))};
        const int fl{fcntl(fd, F_GETFL, 0)};
        if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    return JSValueMakeUndefined(ctx);
}

// proc.writeNB(fd, b64, offset) → bytes written this call (0 on EAGAIN, -1 on a
// broken pipe/error). Single non-blocking write; the driver tracks the offset.
JSValueRef proc_write_nb_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef*) {
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    std::string data{b64_decode(fs_arg(ctx, argc, args, 1))};
    const std::size_t off{argc > 2 ? static_cast<std::size_t>(JSValueToNumber(ctx, args[2], nullptr))
                                   : 0};
    if (off >= data.size()) return JSValueMakeNumber(ctx, 0);
    ssize_t w;
    do { w = write(fd, data.data() + off, data.size() - off); } while (w < 0 && errno == EINTR);
    if (w < 0) return JSValueMakeNumber(ctx, (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1);
    return JSValueMakeNumber(ctx, static_cast<double>(w));
}

// proc.readNB(fd, max) → base64 chunk, "" on EAGAIN (nothing ready), or null on EOF.
JSValueRef proc_read_nb_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef*) {
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    std::size_t max{argc > 1 ? static_cast<std::size_t>(JSValueToNumber(ctx, args[1], nullptr)) : 65536};
    if (max == 0 || max > (1u << 24)) max = 65536;
    std::string buf(max, '\0');
    ssize_t n;
    do { n = read(fd, buf.data(), max); } while (n < 0 && errno == EINTR);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? make_string(ctx, "")
                                                                : JSValueMakeNull(ctx);
    if (n == 0) return JSValueMakeNull(ctx);
    return make_string(ctx, b64_encode(reinterpret_cast<const unsigned char*>(buf.data()),
                                       static_cast<std::size_t>(n)));
}

// proc.setenv(name, value) → write the REAL process environment; value null or
// undefined unsets. node's process.env is a live view of environ, mbun's is a
// snapshot taken at startup, so a JS-level `delete process.env.X` is invisible
// to a child spawned afterwards. Callers that need the real thing (cluster's
// worker dropping NODE_UNIQUE_ID so grandchildren do not inherit it — node's
// test-cluster-basic asserts exactly that) go through here.
JSValueRef proc_setenv_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef*) {
    if (argc == 0) return JSValueMakeUndefined(ctx);
    const std::string name{fs_arg(ctx, argc, args, 0)};
    if (name.empty() || name.find('=') != std::string::npos) return JSValueMakeUndefined(ctx);
    if (argc < 2 || JSValueIsNull(ctx, args[1]) || JSValueIsUndefined(ctx, args[1])) {
        ::unsetenv(name.c_str());
        return JSValueMakeUndefined(ctx);
    }
    const std::string value{fs_arg(ctx, argc, args, 1)};
    ::setenv(name.c_str(), value.c_str(), 1);
    return JSValueMakeUndefined(ctx);
}

// proc.drainMicrotasks() — run JSC's microtask queue to exhaustion right now.
// node drains the process.nextTick queue BETWEEN callbacks (runNextTicks after
// every immediate / timer in lib/internal/timers.js), so a nextTick scheduled
// by one callback runs before the next callback starts. mbun's nextTick rides
// on queueMicrotask, and JSC only drains at a JSLock release — which never
// happens inside the timer drain, since the whole drain runs under one lock.
// The observable difference: `emitWarning(x); setImmediate(next)` delivered the
// warning AFTER `next` had run (test-process-warning). Exposing the VM's own
// drain lets the timer loop restore node's ordering without splitting the drain
// across pump iterations.
JSValueRef proc_drain_microtasks_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                                    const JSValueRef[], JSValueRef*) {
    if (JSC::JSGlobalObject* g = toJS(ctx); g != nullptr) JSC::getVM(g).drainMicrotasks();
    return JSValueMakeUndefined(ctx);
}

// proc.setTimeZone(name) → apply an IANA zone to the engine's date cache, the
// native half of node's "assigning process.env.TZ changes the local timezone"
// contract (test-process-env-tz). JSC caches the resolved zone per-VM, so a bare
// setenv("TZ") is invisible to Date.prototype.toString — the cache has to be
// invalidated explicitly. Same mechanism bun uses (JSEnvironmentVariableMap.cpp
// applyTZFromString): WTF::setTimeZoneOverride + DateCache::resetIfNecessarySlow.
// A missing/empty/null argument drops the override back to the system zone, so
// `delete process.env.TZ` restores the startup behaviour instead of freezing the
// last assigned zone. Returns true when the zone was accepted.
JSValueRef proc_set_timezone_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                const JSValueRef args[], JSValueRef*) {
    const bool clear{argc == 0 || JSValueIsNull(ctx, args[0]) || JSValueIsUndefined(ctx, args[0])};
    const std::string name{clear ? std::string{} : fs_arg(ctx, argc, args, 0)};
    // bun caps the accepted name at 32 chars; longer input is never a zone id and
    // WTF would scan it against the whole ICU table for nothing.
    if (!clear && (name.empty() || name.size() >= 32)) return JSValueMakeBoolean(ctx, false);
    if (clear) ::unsetenv("TZ"); else ::setenv("TZ", name.c_str(), 1);
    const WTF::String wtfName{WTF::String::fromUTF8(name)};
    if (!WTF::setTimeZoneOverride(WTF::StringView{wtfName})) return JSValueMakeBoolean(ctx, false);
    if (JSC::JSGlobalObject* g = toJS(ctx); g != nullptr)
        JSC::getVM(g).dateCache.resetIfNecessarySlow();
    return JSValueMakeBoolean(ctx, true);
}

// ── SCM_RIGHTS descriptor passing over the fork() IPC socketpair ─────────────
// node's NODE_HANDLE protocol hands a listening/accepted socket from one process
// to another; on POSIX that is an AF_UNIX ancillary message carrying the
// descriptor alongside the JSON frame. Both halves are exposed as non-blocking
// twins of writeNB/readNB so the JS IPC driver can keep one code path.
//
// Ordering contract that makes the JS side simple: Linux attaches ancillary data
// to the FIRST byte of the sendmsg that carried it and never merges a later
// segment past that boundary into an earlier recvmsg, so descriptors arrive in
// the same order as the frames that carry them, never after. The JS layer can
// therefore queue received fds and pair the Nth NODE_HANDLE frame with the Nth
// descriptor.

// proc.sendmsgFd(fd, b64, offset, sendFd) → bytes written (0 EAGAIN, -1 error).
// When sendFd >= 0 the descriptor rides along as SCM_RIGHTS on this write.
JSValueRef proc_sendmsg_fd_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                              const JSValueRef args[], JSValueRef*) {
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    std::string data{b64_decode(fs_arg(ctx, argc, args, 1))};
    const std::size_t off{argc > 2 ? static_cast<std::size_t>(JSValueToNumber(ctx, args[2], nullptr))
                                   : 0};
    const int sendFd{argc > 3 ? static_cast<int>(JSValueToNumber(ctx, args[3], nullptr)) : -1};
    if (off >= data.size()) return JSValueMakeNumber(ctx, 0);

    iovec iov{};
    iov.iov_base = data.data() + off;
    iov.iov_len = data.size() - off;
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    union {
        char buf[CMSG_SPACE(sizeof(int))];
        cmsghdr align;
    } control{};
    if (sendFd >= 0) {
        msg.msg_control = control.buf;
        msg.msg_controllen = sizeof(control.buf);
        cmsghdr* cm{CMSG_FIRSTHDR(&msg)};
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cm), &sendFd, sizeof(int));
    }

    ssize_t w;
    do { w = sendmsg(fd, &msg, MSG_NOSIGNAL); } while (w < 0 && errno == EINTR);
    if (w < 0) return JSValueMakeNumber(ctx, (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1);
    return JSValueMakeNumber(ctx, static_cast<double>(w));
}

// proc.recvmsgFd(fd, max) → { data: b64|"" , fds: [int], eof: bool }.
// data "" means EAGAIN (nothing ready); eof true means the peer closed.
JSValueRef proc_recvmsg_fd_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                              const JSValueRef args[], JSValueRef*) {
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    std::size_t max{argc > 1 ? static_cast<std::size_t>(JSValueToNumber(ctx, args[1], nullptr)) : 65536};
    if (max == 0 || max > (1u << 24)) max = 65536;
    std::string buf(max, '\0');

    iovec iov{};
    iov.iov_base = buf.data();
    iov.iov_len = max;
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    union {
        char buf[CMSG_SPACE(sizeof(int) * 8)];
        cmsghdr align;
    } control{};
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof(control.buf);

    ssize_t n;
    do { n = recvmsg(fd, &msg, 0); } while (n < 0 && errno == EINTR);

    JSObjectRef out{JSObjectMake(ctx, nullptr, nullptr)};
    JSObjectRef fds{JSObjectMakeArray(ctx, 0, nullptr, nullptr)};
    unsigned fdCount{0};
    if (n >= 0) {
        for (cmsghdr* cm{CMSG_FIRSTHDR(&msg)}; cm != nullptr; cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
            const std::size_t payload{cm->cmsg_len - CMSG_LEN(0)};
            for (std::size_t i = 0; i + sizeof(int) <= payload; i += sizeof(int)) {
                int got{-1};
                std::memcpy(&got, CMSG_DATA(cm) + i, sizeof(int));
                JSObjectSetPropertyAtIndex(ctx, fds, fdCount++, JSValueMakeNumber(ctx, got), nullptr);
            }
        }
    }
    set_prop(ctx, out, "fds", fds);
    if (n < 0) {
        const bool again{errno == EAGAIN || errno == EWOULDBLOCK};
        set_prop(ctx, out, "data", make_string(ctx, ""));
        set_prop(ctx, out, "eof", JSValueMakeBoolean(ctx, !again));
        return out;
    }
    if (n == 0 && fdCount == 0) {
        set_prop(ctx, out, "data", make_string(ctx, ""));
        set_prop(ctx, out, "eof", JSValueMakeBoolean(ctx, true));
        return out;
    }
    set_prop(ctx, out, "data",
             make_string(ctx, b64_encode(reinterpret_cast<const unsigned char*>(buf.data()),
                                         static_cast<std::size_t>(n))));
    set_prop(ctx, out, "eof", JSValueMakeBoolean(ctx, false));
    return out;
}
#endif

// proc.wait(pid, nohang) → { exited, code, signal } (signal name numeric).
JSValueRef proc_wait_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                        const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "proc.wait: not supported on Windows yet");
#else
    const pid_t pid{argc > 0 ? static_cast<pid_t>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const bool nohang{argc > 1 && JSValueToBoolean(ctx, args[1])};
    int status{0};
    const pid_t r{waitpid(pid, &status, nohang ? WNOHANG : 0)};
    JSObjectRef obj{JSObjectMake(ctx, nullptr, nullptr)};
    if (r <= 0) {
        set_prop(ctx, obj, "exited", JSValueMakeBoolean(ctx, false));
        return obj;
    }
    set_prop(ctx, obj, "exited", JSValueMakeBoolean(ctx, true));
    set_prop(ctx, obj, "code",
             JSValueMakeNumber(ctx, WIFEXITED(status) ? WEXITSTATUS(status) : -1));
    set_prop(ctx, obj, "signal",
             JSValueMakeNumber(ctx, WIFSIGNALED(status) ? WTERMSIG(status) : 0));
    return obj;
#endif
}

JSValueRef proc_kill_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                        const JSValueRef args[], JSValueRef*) {
#if !defined(_WIN32)
    if (argc > 0) {
        const pid_t pid{static_cast<pid_t>(JSValueToNumber(ctx, args[0], nullptr))};
        const int sig{argc > 1 ? static_cast<int>(JSValueToNumber(ctx, args[1], nullptr)) : SIGTERM};
        return JSValueMakeBoolean(ctx, kill(pid, sig) == 0);
    }
#endif
    return JSValueMakeBoolean(ctx, false);
}

// ── POSIX signal delivery to JS listeners ──────────────────────────────────
// node/bun install a real handler for every signal that has a `process.on(SIG…)`
// listener and drop back to SIG_DFL when the last one is removed. The handler
// itself must be async-signal-safe, so it only bumps a sig_atomic_t counter;
// the event-loop pump drains the counters and emits from JS. A *count* (not a
// flag) is required so a burst of N synchronous deliveries — e.g. a tight loop
// of process.kill(pid, "SIGINT") — fires the JS listener N times rather than
// coalescing to one.
#if !defined(_WIN32)
volatile std::sig_atomic_t gSignalPending[NSIG]{};

extern "C" void mbun_signal_handler(int sig) {
    if (sig > 0 && sig < NSIG) gSignalPending[sig] += 1;
}

// proc.signalWatch(signum, on) — install (on) / restore the default (off).
JSValueRef proc_signal_watch_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                const JSValueRef args[], JSValueRef*) {
    if (argc < 1) return JSValueMakeBoolean(ctx, false);
    const int sig{static_cast<int>(JSValueToNumber(ctx, args[0], nullptr))};
    if (sig <= 0 || sig >= NSIG) return JSValueMakeBoolean(ctx, false);
    const bool on{argc > 1 && JSValueToBoolean(ctx, args[1])};
    struct sigaction sa{};
    if (on) {
        sa.sa_handler = &mbun_signal_handler;
        sigemptyset(&sa.sa_mask);
        // SA_RESTART: a JS-observed signal must not turn every in-flight read()
        // into EINTR (node installs its handlers the same way).
        sa.sa_flags = SA_RESTART;
    } else {
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        gSignalPending[sig] = 0;
    }
    return JSValueMakeBoolean(ctx, ::sigaction(sig, &sa, nullptr) == 0);
}

// proc.signalDrain() → [signum, …] delivered since the last drain. A signal
// received N times appears N times, so the JS listener fires once per delivery.
JSValueRef proc_signal_drain_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                                const JSValueRef[], JSValueRef* exc) {
    std::vector<JSValueRef> out;
    for (int s = 1; s < NSIG; ++s) {
        // Subtract (not zero) so a delivery racing between the read and the
        // reset is carried to the next drain instead of being lost.
        std::sig_atomic_t c{gSignalPending[s]};
        if (c != 0) {
            gSignalPending[s] -= c;
            for (std::sig_atomic_t k{0}; k < c; ++k) out.push_back(JSValueMakeNumber(ctx, s));
        }
    }
    return JSObjectMakeArray(ctx, out.size(), out.empty() ? nullptr : out.data(), exc);
}
#endif

// proc.spawnSyncB64(file, args, {cwd, env, inputB64, maxBuffer, timeoutMs,
// killSignal}) → { status, signal, pid, stdoutB64, stderrB64,
// exitedDueToMaxBuffer, exitedDueToTimeout } — like spawnSync but bytes cross
// as base64 both ways.
//
// stdout and stderr are drained CONCURRENTLY through poll(2). The old
// "drain(out) then drain(err)" order deadlocked whenever a child filled the
// stderr pipe buffer before closing stdout, and — with no byte budget — a child
// that never stops writing (`yes`) grew `outStr` until the process was
// OOM-killed. maxBuffer/timeout are what bun uses to bound both cases
// (bun-ref js_bun_spawn_bindings.rs: max_buffer / exited_due_to_maxbuf).
JSValueRef proc_spawn_sync_b64_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                  const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "proc.spawnSyncB64: not supported on Windows yet");
#else
    std::string file{fs_arg(ctx, argc, args, 0)};
    std::vector<std::string> argvStr{parse_spawn_argv(ctx, argc, args, file)};
    SpawnOpts o{parse_spawn_opts(ctx, argc, args)};
    // Permission Model: node enforces the ChildProcess scope at the fork
    // (src/spawn_sync.cc / process_wrap.cc), and so does this. `resource` is the
    // executable, which is what test-permission-child-process-cli asserts on.
    // permission_deny_spawn also handles mbun's worker-as-subprocess case.
    if (permission_deny_spawn(ctx, exc, file, o.env)) return JSValueMakeUndefined(ctx);
    permission_inject_child_env(o.env, o.haveEnv, argvStr);
    std::string input;
    // No budget / no deadline unless the caller asks for one. `Infinity` on the
    // JS side is passed through as absent (bun treats it as "unlimited").
    double maxBuffer{-1};
    double timeoutMs{-1};
    int killSignal{SIGTERM};
    if (argc > 2 && JSValueIsObject(ctx, args[2])) {
        JSObjectRef opt{JSValueToObject(ctx, args[2], nullptr)};
        JSValueRef inV{get_prop(ctx, opt, "inputB64")};
        if (JSValueIsString(ctx, inV)) input = b64_decode(val_to_string(ctx, inV));
        JSValueRef mbV{get_prop(ctx, opt, "maxBuffer")};
        if (JSValueIsNumber(ctx, mbV)) {
            const double v{JSValueToNumber(ctx, mbV, nullptr)};
            if (std::isfinite(v) && v >= 0) maxBuffer = v;
        }
        JSValueRef toV{get_prop(ctx, opt, "timeoutMs")};
        if (JSValueIsNumber(ctx, toV)) {
            const double v{JSValueToNumber(ctx, toV, nullptr)};
            // 0 is a real deadline ("kill now"): an already-aborted AbortSignal
            // arrives as timeoutMs 0, not as "no timeout".
            if (std::isfinite(v) && v >= 0) timeoutMs = v;
        }
        JSValueRef ksV{get_prop(ctx, opt, "killSignal")};
        if (JSValueIsNumber(ctx, ksV)) killSignal = static_cast<int>(JSValueToNumber(ctx, ksV, nullptr));
    }
    int outPipe[2], errPipe[2], inPipe[2];
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0 || pipe(inPipe) != 0) {
        return fs_throw(ctx, exc, "proc.spawnSyncB64: pipe() failed");
    }
    const pid_t pid{fork()};
    if (pid < 0) return fs_throw(ctx, exc, "proc.spawnSyncB64: fork() failed");
    if (pid == 0) {
        dup2(inPipe[0], 0); dup2(outPipe[1], 1); dup2(errPipe[1], 2);
        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        spawn_child_exec(file, argvStr, o);
    }
    close(inPipe[0]); close(outPipe[1]); close(errPipe[1]);
    if (!input.empty()) {
        std::size_t off{0};
        while (off < input.size()) {
            const ssize_t w{write(inPipe[1], input.data() + off, input.size() - off)};
            if (w <= 0) break;
            off += static_cast<std::size_t>(w);
        }
    }
    close(inPipe[1]);

    std::string outStr, errStr;
    bool hitMaxBuffer{false}, hitTimeout{false};
    {
        // 64 KiB is node's stdio read size; it is also the overshoot bound the
        // corpus asserts (result <= maxBuffer + 64 KiB): the read that trips the
        // limit is still buffered, but no further read happens.
        constexpr std::size_t kChunk{64u * 1024u};
        std::vector<char> buf(kChunk);
        int fds[2]{outPipe[0], errPipe[0]};
        bool open_[2]{true, true};
        std::string* sink[2]{&outStr, &errStr};
        const auto start{std::chrono::steady_clock::now()};
        auto elapsedMs{[&start] {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        }};
        while (open_[0] || open_[1]) {
            pollfd pfd[2];
            int n{0};
            int slot[2]{-1, -1};
            for (int i = 0; i < 2; ++i) {
                if (!open_[i]) continue;
                slot[n] = i;
                pfd[n++] = pollfd{.fd = fds[i], .events = POLLIN, .revents = 0};
            }
            int waitMs{-1};
            if (timeoutMs >= 0) {
                const double left{timeoutMs - elapsedMs()};
                waitMs = left <= 0 ? 0 : static_cast<int>(left) + 1;
            }
            const int pr{poll(pfd, static_cast<nfds_t>(n), waitMs)};
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (timeoutMs >= 0 && !hitTimeout && elapsedMs() >= timeoutMs) {
                hitTimeout = true;
                if (killSignal != 0) kill(pid, killSignal);
                // Keep draining until EOF so the post-kill output is reported.
                timeoutMs = -1;
                continue;
            }
            if (pr == 0) continue;
            for (int k = 0; k < n; ++k) {
                if ((pfd[k].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;
                const int i{slot[k]};
                ssize_t rn;
                do { rn = read(fds[i], buf.data(), kChunk); } while (rn < 0 && errno == EINTR);
                if (rn > 0) {
                    sink[i]->append(buf.data(), static_cast<std::size_t>(rn));
                } else if (rn == 0) {
                    open_[i] = false;
                    close(fds[i]);
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    open_[i] = false;
                    close(fds[i]);
                }
            }
            if (maxBuffer >= 0 && !hitMaxBuffer &&
                static_cast<double>(outStr.size() + errStr.size()) > maxBuffer) {
                hitMaxBuffer = true;
                if (killSignal != 0) kill(pid, killSignal);
                // Stop reading entirely: closing our ends makes a child that
                // ignored (or was never sent) the signal die of EPIPE instead of
                // blocking forever on a full pipe.
                for (int i = 0; i < 2; ++i) {
                    if (open_[i]) { open_[i] = false; close(fds[i]); }
                }
            }
        }
    }
    int status{0};
    waitpid(pid, &status, 0);
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    const int sig{WIFSIGNALED(status) ? WTERMSIG(status) : 0};
    // A signalled child has NO exit code (bun/node report exitCode === null).
    set_prop(ctx, r, "status",
             sig != 0 ? JSValueMakeNull(ctx)
                      : JSValueMakeNumber(ctx, WIFEXITED(status) ? WEXITSTATUS(status) : -1));
    set_prop(ctx, r, "signal", sig ? make_string(ctx, std::to_string(sig)) : JSValueMakeNull(ctx));
    set_prop(ctx, r, "pid", JSValueMakeNumber(ctx, static_cast<double>(pid)));
    set_prop(ctx, r, "exitedDueToMaxBuffer", JSValueMakeBoolean(ctx, hitMaxBuffer));
    set_prop(ctx, r, "exitedDueToTimeout", JSValueMakeBoolean(ctx, hitTimeout));
    set_prop(ctx, r, "stdoutB64",
             make_string(ctx, b64_encode(reinterpret_cast<const unsigned char*>(outStr.data()), outStr.size())));
    set_prop(ctx, r, "stderrB64",
             make_string(ctx, b64_encode(reinterpret_cast<const unsigned char*>(errStr.data()), errStr.size())));
    return r;
#endif
}
