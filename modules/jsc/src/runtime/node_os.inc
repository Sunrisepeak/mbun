// node_os.inc — native OS-info bridge for node:os and node:tty.
//
// Pure system information over POSIX libc only (uname/sysinfo/getpriority/
// getpwuid/getifaddrs/sysconf/gethostname/ioctl/isatty/tcget-setattr). No third
// party host library is touched — these are OS system calls. Exposed as
// globalThis.__mbunOsNative; the JS shaping (module objects, Symbol.toPrimitive,
// tmpdir env logic, getColorDepth) lives in builtins/node_os.cppm.
//
// Blueprint: node lib/os.js + libuv src/unix (uv_os_*), and bun's
// src/js/node/os.ts createNodeOsBinding / ProcessBindingTTYWrap.

#if !defined(_WIN32)

static int osn_arg_int(JSContextRef ctx, std::size_t argc, const JSValueRef args[],
                       std::size_t i, int def) {
    if (i >= argc || !JSValueIsNumber(ctx, args[i])) return def;
    return static_cast<int>(JSValueToNumber(ctx, args[i], nullptr));
}

// uname() → {sysname, nodename, release, version, machine}
JSValueRef osn_uname_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                        const JSValueRef[], JSValueRef*) {
    JSObjectRef o{JSObjectMake(ctx, nullptr, nullptr)};
    struct utsname u {};
    if (::uname(&u) == 0) {
        set_prop(ctx, o, "sysname", make_string(ctx, u.sysname));
        set_prop(ctx, o, "nodename", make_string(ctx, u.nodename));
        set_prop(ctx, o, "release", make_string(ctx, u.release));
        set_prop(ctx, o, "version", make_string(ctx, u.version));
        set_prop(ctx, o, "machine", make_string(ctx, u.machine));
    }
    return o;
}

// endianness() → "LE" | "BE"
JSValueRef osn_endianness_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                             const JSValueRef[], JSValueRef*) {
    const std::uint16_t one{1};
    const bool le{*reinterpret_cast<const unsigned char*>(&one) == 1};
    return make_string(ctx, le ? "LE" : "BE");
}

// totalmem() / freemem() / uptime() / loadavg() over Linux sysinfo(2).
JSValueRef osn_totalmem_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                           const JSValueRef[], JSValueRef*) {
    double v{0};
#if defined(__linux__)
    struct sysinfo si {};
    if (::sysinfo(&si) == 0) v = static_cast<double>(si.totalram) * si.mem_unit;
#else
    const long pages{::sysconf(_SC_PHYS_PAGES)}, ps{::sysconf(_SC_PAGE_SIZE)};
    if (pages > 0 && ps > 0) v = static_cast<double>(pages) * static_cast<double>(ps);
#endif
    return JSValueMakeNumber(ctx, v);
}

JSValueRef osn_freemem_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                          const JSValueRef[], JSValueRef*) {
    double v{0};
#if defined(__linux__)
    struct sysinfo si {};
    {
    bool got{false};
    if (std::ifstream f{"/proc/meminfo"}) {
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("MemAvailable:", 0) == 0) {
                std::size_t pos{line.find_first_of("0123456789", 13)};
                if (pos != std::string::npos)
                    v = static_cast<double>(std::strtoull(line.c_str() + pos, nullptr, 10)) * 1024.0, got = true;
                break;
            }
        }
    }
    if (!got) { if (::sysinfo(&si) == 0) v = static_cast<double>(si.freeram) * si.mem_unit; }
}
#else
    const long pages{::sysconf(_SC_AVPHYS_PAGES)}, ps{::sysconf(_SC_PAGE_SIZE)};
    if (pages > 0 && ps > 0) v = static_cast<double>(pages) * static_cast<double>(ps);
#endif
    return JSValueMakeNumber(ctx, v);
}

JSValueRef osn_uptime_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                         const JSValueRef[], JSValueRef*) {
    double v{0};
#if defined(__linux__)
    struct sysinfo si {};
    if (::sysinfo(&si) == 0) v = static_cast<double>(si.uptime);
#endif
    return JSValueMakeNumber(ctx, v);
}

// loadavg() → [1min, 5min, 15min]
JSValueRef osn_loadavg_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                          const JSValueRef[], JSValueRef*) {
    double a[3]{0, 0, 0};
#if defined(__linux__)
    struct sysinfo si {};
    if (::sysinfo(&si) == 0) {
        constexpr double kScale{65536.0};  // 1 << SI_LOAD_SHIFT
        a[0] = static_cast<double>(si.loads[0]) / kScale;
        a[1] = static_cast<double>(si.loads[1]) / kScale;
        a[2] = static_cast<double>(si.loads[2]) / kScale;
    }
#endif
    JSValueRef items[3]{JSValueMakeNumber(ctx, a[0]), JSValueMakeNumber(ctx, a[1]),
                        JSValueMakeNumber(ctx, a[2])};
    return JSObjectMakeArray(ctx, 3, items, nullptr);
}

// availableParallelism() → online CPU count.
JSValueRef osn_nprocs_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                         const JSValueRef[], JSValueRef*) {
    long n{::sysconf(_SC_NPROCESSORS_ONLN)};
    if (n < 1) n = 1;
    return JSValueMakeNumber(ctx, static_cast<double>(n));
}

// hostname() → gethostname
JSValueRef osn_hostname_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                           const JSValueRef[], JSValueRef*) {
    char buf[256];
    buf[0] = '\0';
    if (::gethostname(buf, sizeof buf) != 0) return make_string(ctx, "unknown");
    buf[sizeof buf - 1] = '\0';
    return make_string(ctx, buf);
}

// homedir() → $HOME, else the passwd entry's directory.
JSValueRef osn_homedir_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                          const JSValueRef[], JSValueRef*) {
    if (const char* h{std::getenv("HOME")}; h != nullptr && h[0] != '\0')
        return make_string(ctx, h);
    if (struct passwd* pw{::getpwuid(::getuid())}; pw != nullptr && pw->pw_dir != nullptr)
        return make_string(ctx, pw->pw_dir);
    return make_string(ctx, "unknown");
}

// userInfo() → {username, uid, gid, shell, homedir} from getpwuid.
JSValueRef osn_userinfo_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                           const JSValueRef[], JSValueRef*) {
    JSObjectRef o{JSObjectMake(ctx, nullptr, nullptr)};
    const uid_t uid{::getuid()};
    struct passwd* pw{::getpwuid(uid)};
    set_prop(ctx, o, "uid", JSValueMakeNumber(ctx, static_cast<double>(uid)));
    // shell mirrors bun: process.env.SHELL, or "unknown" when unset.
    const char* env_shell{::getenv("SHELL")};
    const char* shell{env_shell != nullptr && env_shell[0] ? env_shell : "unknown"};
    if (pw != nullptr) {
        set_prop(ctx, o, "username", make_string(ctx, pw->pw_name ? pw->pw_name : ""));
        set_prop(ctx, o, "gid", JSValueMakeNumber(ctx, static_cast<double>(pw->pw_gid)));
        set_prop(ctx, o, "shell", make_string(ctx, shell));
        set_prop(ctx, o, "homedir", make_string(ctx, pw->pw_dir ? pw->pw_dir : ""));
    } else {
        set_prop(ctx, o, "username", make_string(ctx, ""));
        set_prop(ctx, o, "gid", JSValueMakeNumber(ctx, static_cast<double>(::getgid())));
        set_prop(ctx, o, "shell", make_string(ctx, shell));
        set_prop(ctx, o, "homedir", make_string(ctx, ""));
    }
    return o;
}

// cpus() → [{model, speed, times:{user,nice,sys,idle,irq}}] from /proc.
JSValueRef osn_cpus_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                       const JSValueRef[], JSValueRef*) {
    std::vector<JSValueRef> out;
    const long hz{::sysconf(_SC_CLK_TCK) > 0 ? ::sysconf(_SC_CLK_TCK) : 100};
    const double toMs{1000.0 / static_cast<double>(hz)};

    // Per-cpu model / MHz from /proc/cpuinfo (index-ordered).
    std::vector<std::string> models;
    std::vector<double> speeds;
    if (auto info{read_file("/proc/cpuinfo")}) {
        std::istringstream in{*info};
        std::string line, model;
        double mhz{0};
        for (; std::getline(in, line);) {
            if (line.empty()) {  // blank line ends a processor block
                if (!model.empty() || mhz != 0) { models.push_back(model); speeds.push_back(mhz); }
                model.clear();
                mhz = 0;
                continue;
            }
            const auto colon{line.find(':')};
            if (colon == std::string::npos) continue;
            std::string key{line.substr(0, colon)};
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
            std::string val{line.substr(colon + 1)};
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
            if (key == "model name" || key == "Hardware" || key == "cpu model") model = val;
            else if (key == "cpu MHz" || key == "clock") { try { mhz = std::stod(val); } catch (...) {} }
        }
        if (!model.empty() || mhz != 0) { models.push_back(model); speeds.push_back(mhz); }
    }

    // Per-cpu jiffies from /proc/stat ("cpuN user nice system idle iowait irq ...").
    if (auto stat{read_file("/proc/stat")}) {
        std::istringstream in{*stat};
        std::string line;
        std::size_t idx{0};
        for (; std::getline(in, line);) {
            if (line.rfind("cpu", 0) != 0 || line.size() < 4 ||
                !(line[3] >= '0' && line[3] <= '9'))
                continue;
            std::istringstream ls{line};
            std::string tag;
            unsigned long long user{0}, nice{0}, sys{0}, idle{0}, iowait{0}, irq{0};
            ls >> tag >> user >> nice >> sys >> idle >> iowait >> irq;
            JSObjectRef times{JSObjectMake(ctx, nullptr, nullptr)};
            set_prop(ctx, times, "user", JSValueMakeNumber(ctx, static_cast<double>(user) * toMs));
            set_prop(ctx, times, "nice", JSValueMakeNumber(ctx, static_cast<double>(nice) * toMs));
            set_prop(ctx, times, "sys", JSValueMakeNumber(ctx, static_cast<double>(sys) * toMs));
            set_prop(ctx, times, "idle", JSValueMakeNumber(ctx, static_cast<double>(idle) * toMs));
            set_prop(ctx, times, "irq", JSValueMakeNumber(ctx, static_cast<double>(irq) * toMs));
            JSObjectRef cpu{JSObjectMake(ctx, nullptr, nullptr)};
            set_prop(ctx, cpu, "model",
                     make_string(ctx, idx < models.size() && !models[idx].empty() ? models[idx]
                                                                                  : std::string{"unknown"}));
            set_prop(ctx, cpu, "speed",
                     JSValueMakeNumber(ctx, idx < speeds.size() ? speeds[idx] : 0.0));
            set_prop(ctx, cpu, "times", times);
            out.push_back(cpu);
            ++idx;
        }
    }

    // Fallback: at least one entry per online CPU so callers never see [].
    if (out.empty()) {
        long n{::sysconf(_SC_NPROCESSORS_ONLN)};
        if (n < 1) n = 1;
        for (long i{0}; i < n; ++i) {
            JSObjectRef times{JSObjectMake(ctx, nullptr, nullptr)};
            for (const char* k : {"user", "nice", "sys", "idle", "irq"})
                set_prop(ctx, times, k, JSValueMakeNumber(ctx, 0));
            JSObjectRef cpu{JSObjectMake(ctx, nullptr, nullptr)};
            set_prop(ctx, cpu, "model", make_string(ctx, "unknown"));
            set_prop(ctx, cpu, "speed", JSValueMakeNumber(ctx, 0));
            set_prop(ctx, cpu, "times", times);
            out.push_back(cpu);
        }
    }
    return JSObjectMakeArray(ctx, out.size(), out.data(), nullptr);
}

// Count the leading set bits of a network mask → CIDR prefix length.
static int osn_prefix4(std::uint32_t mask_be) {
    std::uint32_t m{ntohl(mask_be)};
    int n{0};
    while (m & 0x80000000u) { ++n; m <<= 1; }
    return n;
}
static int osn_prefix6(const unsigned char* m) {
    int n{0};
    for (int i{0}; i < 16; ++i) {
        unsigned char b{m[i]};
        while (b & 0x80) { ++n; b = static_cast<unsigned char>(b << 1); }
        if (m[i] != 0xff) break;
    }
    return n;
}

// networkInterfaces() → { ifname: [ {address, netmask, family, mac, internal,
// cidr, scopeid?} ] } from getifaddrs.
JSValueRef osn_netif_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                        const JSValueRef[], JSValueRef*) {
    JSObjectRef result{JSObjectMake(ctx, nullptr, nullptr)};
    struct ifaddrs* head{nullptr};
    if (::getifaddrs(&head) != 0 || head == nullptr) return result;

    // Pass 1: interface name → MAC (from AF_PACKET link-layer addresses).
    std::map<std::string, std::string> macs;
#if defined(__linux__)
    for (struct ifaddrs* p{head}; p != nullptr; p = p->ifa_next) {
        if (p->ifa_addr == nullptr || p->ifa_addr->sa_family != AF_PACKET) continue;
        auto* ll{reinterpret_cast<struct sockaddr_ll*>(p->ifa_addr)};
        if (ll->sll_halen != 6) { macs[p->ifa_name] = "00:00:00:00:00:00"; continue; }
        char mac[18];
        std::snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", ll->sll_addr[0],
                      ll->sll_addr[1], ll->sll_addr[2], ll->sll_addr[3], ll->sll_addr[4],
                      ll->sll_addr[5]);
        macs[p->ifa_name] = mac;
    }
#endif

    // Pass 2: IPv4 / IPv6 addresses grouped per interface name.
    for (struct ifaddrs* p{head}; p != nullptr; p = p->ifa_next) {
        if (p->ifa_addr == nullptr) continue;
        const int fam{p->ifa_addr->sa_family};
        if (fam != AF_INET && fam != AF_INET6) continue;

        char addr[INET6_ADDRSTRLEN]{0};
        char mask[INET6_ADDRSTRLEN]{0};
        int prefix{0};
        unsigned scopeid{0};
        const char* familyName{fam == AF_INET ? "IPv4" : "IPv6"};

        if (fam == AF_INET) {
            auto* sin{reinterpret_cast<struct sockaddr_in*>(p->ifa_addr)};
            ::inet_ntop(AF_INET, &sin->sin_addr, addr, sizeof addr);
            if (p->ifa_netmask != nullptr) {
                auto* nm{reinterpret_cast<struct sockaddr_in*>(p->ifa_netmask)};
                ::inet_ntop(AF_INET, &nm->sin_addr, mask, sizeof mask);
                prefix = osn_prefix4(nm->sin_addr.s_addr);
            }
        } else {
            auto* sin6{reinterpret_cast<struct sockaddr_in6*>(p->ifa_addr)};
            ::inet_ntop(AF_INET6, &sin6->sin6_addr, addr, sizeof addr);
            scopeid = sin6->sin6_scope_id;
            if (p->ifa_netmask != nullptr) {
                auto* nm{reinterpret_cast<struct sockaddr_in6*>(p->ifa_netmask)};
                ::inet_ntop(AF_INET6, &nm->sin6_addr, mask, sizeof mask);
                prefix = osn_prefix6(nm->sin6_addr.s6_addr);
            }
        }

        JSObjectRef e{JSObjectMake(ctx, nullptr, nullptr)};
        set_prop(ctx, e, "address", make_string(ctx, addr));
        set_prop(ctx, e, "netmask", make_string(ctx, mask));
        set_prop(ctx, e, "family", make_string(ctx, familyName));
        auto it{macs.find(p->ifa_name)};
        set_prop(ctx, e, "mac",
                 make_string(ctx, it != macs.end() ? it->second : std::string{"00:00:00:00:00:00"}));
        set_prop(ctx, e, "internal",
                 JSValueMakeBoolean(ctx, (p->ifa_flags & IFF_LOOPBACK) != 0));
        std::string cidr{std::string{addr} + "/" + std::to_string(prefix)};
        set_prop(ctx, e, "cidr", make_string(ctx, cidr));
        if (fam == AF_INET6)
            set_prop(ctx, e, "scopeid", JSValueMakeNumber(ctx, static_cast<double>(scopeid)));

        JSStringRef key{JSStringCreateWithUTF8CString(p->ifa_name)};
        JSValueRef existing{JSObjectGetProperty(ctx, result, key, nullptr)};
        JSObjectRef arr;
        if (JSValueIsObject(ctx, existing)) {
            arr = JSValueToObject(ctx, existing, nullptr);
            JSValueRef lenV{get_prop(ctx, arr, "length")};
            const unsigned len{static_cast<unsigned>(JSValueToNumber(ctx, lenV, nullptr))};
            JSObjectSetPropertyAtIndex(ctx, arr, len, e, nullptr);
        } else {
            JSValueRef one[1]{e};
            arr = JSObjectMakeArray(ctx, 1, one, nullptr);
            JSObjectSetProperty(ctx, result, key, arr, kJSPropertyAttributeNone, nullptr);
        }
        JSStringRelease(key);
    }

    ::freeifaddrs(head);
    return result;
}

// getPriority(pid) → {ok, value, errno}. errno is the positive libc errno on
// failure (getpriority legitimately returns -1, so errno is pre-cleared).
JSValueRef osn_getprio_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef*) {
    const int pid{osn_arg_int(ctx, argc, args, 0, 0)};
    errno = 0;
    const int r{::getpriority(PRIO_PROCESS, static_cast<id_t>(pid))};
    JSObjectRef o{JSObjectMake(ctx, nullptr, nullptr)};
    if (r == -1 && errno != 0) {
        set_prop(ctx, o, "ok", JSValueMakeBoolean(ctx, false));
        set_prop(ctx, o, "errno", JSValueMakeNumber(ctx, static_cast<double>(errno)));
    } else {
        set_prop(ctx, o, "ok", JSValueMakeBoolean(ctx, true));
        set_prop(ctx, o, "value", JSValueMakeNumber(ctx, static_cast<double>(r)));
    }
    return o;
}

// setPriority(pid, prio) → {ok, errno}
JSValueRef osn_setprio_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef*) {
    const int pid{osn_arg_int(ctx, argc, args, 0, 0)};
    const int prio{osn_arg_int(ctx, argc, args, 1, 0)};
    errno = 0;
    const int r{::setpriority(PRIO_PROCESS, static_cast<id_t>(pid), prio)};
    JSObjectRef o{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, o, "ok", JSValueMakeBoolean(ctx, r == 0));
    if (r != 0) set_prop(ctx, o, "errno", JSValueMakeNumber(ctx, static_cast<double>(errno)));
    return o;
}

// Every tty entry point below takes a descriptor that JS obtained from
// node:fs, and those are mbun VIRTUAL fds (fd_table(), numbered from 1000) —
// not descriptors the kernel knows. `new tty.ReadStream(fs.openSync("/dev/tty"))`
// therefore reported isTTY false and setRawMode failed with EBADF(9), which is
// the whole "read piped stdin, then reopen /dev/tty" TUI pattern. Resolve the
// virtual fd to its real one first, exactly as fsn_fstat_cb does, and fall back
// to the number itself for stdio (0/1/2) and inherited descriptors that mbun
// never handed out. ref: regression tty-reopen-after-stdin-eof.
std::unordered_map<int, mbun::core::io::File>& fd_table();
inline int osn_real_fd(int vfd) {
    if (vfd < 0) return -1;
    auto& tbl{fd_table()};
    auto it{tbl.find(vfd)};
    if (it != tbl.end() && it->second.is_open()) return it->second.native_fd();
    return vfd;
}

// isatty(fd) → bool
JSValueRef osn_isatty_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef*) {
    const int fd{osn_real_fd(osn_arg_int(ctx, argc, args, 0, -1))};
    return JSValueMakeBoolean(ctx, fd >= 0 && ::isatty(fd) == 1);
}

// ttySize(fd) → [cols, rows] | null (ioctl TIOCGWINSZ)
JSValueRef osn_ttysize_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef*) {
    const int fd{osn_real_fd(osn_arg_int(ctx, argc, args, 0, -1))};
    struct winsize ws {};
    if (fd < 0 || ::ioctl(fd, TIOCGWINSZ, &ws) != 0) return JSValueMakeNull(ctx);
    JSValueRef items[2]{JSValueMakeNumber(ctx, ws.ws_col), JSValueMakeNumber(ctx, ws.ws_row)};
    return JSObjectMakeArray(ctx, 2, items, nullptr);
}

// ── stdin termios restore at exit (libuv uv_tty_reset_mode) ─────────────────
// A process that leaves a terminal in raw mode leaves the USER's shell in raw
// mode: libuv snapshots the descriptor's termios the first time a mode change
// is made and restores it from an atexit hook. node inherits that for free;
// mbun did not, so a `bun -e 'process.stdin.setRawMode(true)'` under a pty left
// ICANON/ECHO cleared for whoever owned the pty afterwards -- which is exactly
// what "child that called setRawMode restores termios on exit" observes from
// the parent side, and what makes an interactive TUI wreck the terminal it
// crashed out of.
//
// The snapshot is taken at INSTALL time when stdin is a terminal, not at the
// first setRawMode: the "interactive wrapper" case restores a cooked state the
// process itself never set (the mode was changed out from under it), which only
// a startup snapshot can produce. PORT-SOURCE: libuv src/unix/tty.c
// uv_tty_reset_mode + bun's bun_restore_stdio.
struct TtyStartupMode {
    struct termios saved {};
    bool valid{false};
};
inline TtyStartupMode& tty_startup_mode() {
    static TtyStartupMode m;
    return m;
}
inline void tty_restore_startup_mode() {
    TtyStartupMode& m{tty_startup_mode()};
    if (!m.valid) return;
    m.valid = false;  // restore exactly once, even if exit runs twice
    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &m.saved);
}
inline void tty_capture_startup_mode() {
    TtyStartupMode& m{tty_startup_mode()};
    // Gated on stdout being a terminal too. A pipeline PRODUCER (stdout is a
    // pipe, stdin happens to share a tty device with an interactive program)
    // must NOT write a stale cooked snapshot back on the way out: that is the
    // `less` case, where the exiting producer would clobber the raw mode the
    // live foreground program set. bun draws the same line in bun_restore_stdio,
    // and "pipeline producer exit does not clobber raw mode on shared tty
    // device" is the regression test for it.
    if (m.valid || ::isatty(STDIN_FILENO) != 1 || ::isatty(STDOUT_FILENO) != 1) return;
    if (::tcgetattr(STDIN_FILENO, &m.saved) != 0) return;
    m.valid = true;
    static bool registered{false};
    if (!registered) { registered = true; std::atexit(tty_restore_startup_mode); }
}

// setRawMode(fd, flag) → 0 on success, else the positive errno. termios cbreak
// mirror of libuv uv_tty_set_mode(UV_TTY_MODE_RAW).
JSValueRef osn_setrawmode_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef*) {
    const int fd{osn_real_fd(osn_arg_int(ctx, argc, args, 0, -1))};
    const bool raw{argc > 1 && JSValueToBoolean(ctx, args[1])};
    struct termios t {};
    if (fd < 0 || ::tcgetattr(fd, &t) != 0) return JSValueMakeNumber(ctx, errno);
    if (raw) {
        t.c_iflag &= ~(ICRNL | INPCK | ISTRIP | IXON | BRKINT);
        t.c_lflag &= ~(ICANON | ECHO | IEXTEN | ISIG);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
    } else {
        t.c_iflag |= (ICRNL | BRKINT);
        t.c_lflag |= (ICANON | ECHO | IEXTEN | ISIG);
    }
    if (::tcsetattr(fd, TCSANOW, &t) != 0) return JSValueMakeNumber(ctx, errno);
    return JSValueMakeNumber(ctx, 0);
}

void install_node_os_native(JSContextRef ctx, JSObjectRef global) {
    JSObjectRef on{JSObjectMake(ctx, nullptr, nullptr)};
    set_fn(ctx, on, "uname", osn_uname_cb);
    set_fn(ctx, on, "endianness", osn_endianness_cb);
    set_fn(ctx, on, "totalmem", osn_totalmem_cb);
    set_fn(ctx, on, "freemem", osn_freemem_cb);
    set_fn(ctx, on, "uptime", osn_uptime_cb);
    set_fn(ctx, on, "loadavg", osn_loadavg_cb);
    set_fn(ctx, on, "nprocs", osn_nprocs_cb);
    set_fn(ctx, on, "hostname", osn_hostname_cb);
    set_fn(ctx, on, "homedir", osn_homedir_cb);
    set_fn(ctx, on, "userInfo", osn_userinfo_cb);
    set_fn(ctx, on, "cpus", osn_cpus_cb);
    set_fn(ctx, on, "networkInterfaces", osn_netif_cb);
    set_fn(ctx, on, "getPriority", osn_getprio_cb);
    set_fn(ctx, on, "setPriority", osn_setprio_cb);
    set_fn(ctx, on, "isatty", osn_isatty_cb);
    set_fn(ctx, on, "ttySize", osn_ttysize_cb);
    set_fn(ctx, on, "setRawMode", osn_setrawmode_cb);
    tty_capture_startup_mode();
    set_prop(ctx, global, "__mbunOsNative", on);
}

#else  // _WIN32: node:os/tty fall back to the pure-JS bootstrap stubs.

void install_node_os_native(JSContextRef, JSObjectRef) {}

#endif  // !_WIN32
