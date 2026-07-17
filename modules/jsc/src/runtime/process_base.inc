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
    // options.cwd / options.input / options.env / options.detached
    std::string cwd, input;
    std::vector<std::string> envStr;  // "KEY=VALUE" pairs; empty ⇒ inherit environ
    bool haveEnv{false};
    bool detached{false};
    double uid{-1}, gid{-1};
    if (argc > 2 && JSValueIsObject(ctx, args[2])) {
        JSObjectRef opt{JSValueToObject(ctx, args[2], nullptr)};
        detached = JSValueToBoolean(ctx, get_prop(ctx, opt, "detached"));
        JSValueRef uidV{get_prop(ctx, opt, "uid")};
        if (JSValueIsNumber(ctx, uidV)) uid = JSValueToNumber(ctx, uidV, nullptr);
        JSValueRef gidV{get_prop(ctx, opt, "gid")};
        if (JSValueIsNumber(ctx, gidV)) gid = JSValueToNumber(ctx, gidV, nullptr);
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

    int outPipe[2], errPipe[2], inPipe[2], statPipe[2];
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0 || pipe(inPipe) != 0 || pipe(statPipe) != 0) {
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
        dup2(inPipe[0], 0);
        dup2(outPipe[1], 1);
        dup2(errPipe[1], 2);
        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
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
    close(inPipe[0]);
    close(outPipe[1]);
    close(errPipe[1]);
    std::int32_t childErr{0};
    {
        ssize_t sn;
        do { sn = read(statPipe[0], &childErr, sizeof childErr); } while (sn < 0 && errno == EINTR);
        if (sn != static_cast<ssize_t>(sizeof childErr)) childErr = 0;
    }
    close(statPipe[0]);
    if (!input.empty()) {
        ssize_t off = 0;
        while (off < static_cast<ssize_t>(input.size())) {
            ssize_t w{write(inPipe[1], input.data() + off, input.size() - off)};
            if (w <= 0) break;
            off += w;
        }
    }
    close(inPipe[1]);
    auto drain{[](int fd) {
        std::string out;
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0) out.append(buf, static_cast<std::size_t>(n));
        close(fd);
        return out;
    }};
    std::string outStr{drain(outPipe[0])};
    std::string errStr{drain(errPipe[0])};
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
    set_prop(ctx, r, "status", JSValueMakeNumber(ctx, code));
    set_prop(ctx, r, "signal",
             sig ? make_string(ctx, std::to_string(sig)) : JSValueMakeNull(ctx));
    set_prop(ctx, r, "pid", JSValueMakeNumber(ctx, static_cast<double>(pid)));
    set_prop(ctx, r, "stdout", make_string(ctx, outStr));
    set_prop(ctx, r, "stderr", make_string(ctx, errStr));
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

// proc.spawnSyncB64(file, args, {cwd, env, inputB64}) → { status, signal, pid,
// stdoutB64, stderrB64 } — like spawnSync but bytes cross as base64 both ways.
JSValueRef proc_spawn_sync_b64_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                  const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "proc.spawnSyncB64: not supported on Windows yet");
#else
    std::string file{fs_arg(ctx, argc, args, 0)};
    std::vector<std::string> argvStr{parse_spawn_argv(ctx, argc, args, file)};
    SpawnOpts o{parse_spawn_opts(ctx, argc, args)};
    std::string input;
    if (argc > 2 && JSValueIsObject(ctx, args[2])) {
        JSObjectRef opt{JSValueToObject(ctx, args[2], nullptr)};
        JSValueRef inV{get_prop(ctx, opt, "inputB64")};
        if (JSValueIsString(ctx, inV)) input = b64_decode(val_to_string(ctx, inV));
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
    auto drain{[](int fd) {
        std::string out;
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0) out.append(buf, static_cast<std::size_t>(n));
        close(fd);
        return out;
    }};
    const std::string outStr{drain(outPipe[0])};
    const std::string errStr{drain(errPipe[0])};
    int status{0};
    waitpid(pid, &status, 0);
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, r, "status",
             JSValueMakeNumber(ctx, WIFEXITED(status) ? WEXITSTATUS(status) : -1));
    const int sig{WIFSIGNALED(status) ? WTERMSIG(status) : 0};
    set_prop(ctx, r, "signal", sig ? make_string(ctx, std::to_string(sig)) : JSValueMakeNull(ctx));
    set_prop(ctx, r, "pid", JSValueMakeNumber(ctx, static_cast<double>(pid)));
    set_prop(ctx, r, "stdoutB64",
             make_string(ctx, b64_encode(reinterpret_cast<const unsigned char*>(outStr.data()), outStr.size())));
    set_prop(ctx, r, "stderrB64",
             make_string(ctx, b64_encode(reinterpret_cast<const unsigned char*>(errStr.data()), errStr.size())));
    return r;
#endif
}
