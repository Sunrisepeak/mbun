// ── __mbunNetNative: non-blocking POSIX TCP primitives ──────────────────────
// Powers Bun.serve, the real network fetch(), and node:net/node:http in the JS
// layer (js_builtins.cppm). Design: NO background threads — every socket is
// O_NONBLOCK and all progress is made from the main JS thread by the event-loop
// pump calling globalThis.__mbunNetDrain() (which loops these primitives). This
// mirrors bun's single-threaded uSockets loop shape while keeping the JSC C API
// single-threaded contract trivially satisfied. Bytes cross the JS boundary as
// base64 (same rationale as __mbunProcNative: the C API string path is UTF-8).
// POSIX only; Windows (WSA) is DEFERRED honestly (throws).
#if !defined(_WIN32)

// All live fds created by this layer (listeners + connections), so wait() can
// poll readiness across everything with one syscall. Main-thread-only access.
std::unordered_set<int>& net_fds() {
    static std::unordered_set<int> gFds;
    return gFds;
}
void net_set_nonblock(int fd) {
    const int fl{fcntl(fd, F_GETFL, 0)};
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

// Per-fd TLS channel (T-TLS.3): memory-BIO engine from mbun.tls over the
// vendored mbun.openssl ecosystem package (no host libssl). Once a fd is
// wrapped, ALL its socket IO goes through the tls* callbacks below — the JS
// layer must not mix NN.read/NN.write on a wrapped fd (bytes would be stolen
// from the record stream). Main-thread-only, like net_fds().
struct NetTlsState {
    mbun::tls::TlsChannel channel;
    std::string pendingOut{};  // ciphertext the socket refused (EAGAIN tail)
    bool sockEof{false};
    bool failed{false};
    std::string error{};
    explicit NetTlsState(mbun::tls::TlsRole role, mbun::tls::Config cfg)
        : channel{role, std::move(cfg)} {}
};

std::unordered_map<int, std::unique_ptr<NetTlsState>>& net_tls() {
    static std::unordered_map<int, std::unique_ptr<NetTlsState>> gTls;
    return gTls;
}

// "localhost"/"" → loopback; otherwise a dotted-quad via inet_pton. (DNS for
// arbitrary hostnames is DEFERRED — tests bind/connect loopback.)
bool net_parse_addr(const std::string& host, std::uint16_t port, sockaddr_in& out) {
    std::memset(&out, 0, sizeof out);
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (host.empty() || host == "localhost" || host == "127.0.0.1") {
        out.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return true;
    }
    if (host == "0.0.0.0" || host == "::" || host == "::0" || host == "*") {
        out.sin_addr.s_addr = htonl(INADDR_ANY);
        return true;
    }
    return inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1;
}
#endif

// net.listen(host, port) → { fd, port } (port resolved when 0).
// errno → its POSIX NAME. The JS net layer classifies native failures by
// scanning the thrown message for an errno name (js_net.cppm ERRNO_RE), so a
// strerror-only message ("Permission denied") is indistinguishable from any
// other and falls back to EADDRINUSE.
inline const char* net_errno_name(int e) {
    switch (e) {
    case EACCES: return "EACCES";
    case EPERM: return "EPERM";
    case EADDRINUSE: return "EADDRINUSE";
    case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
    case EAFNOSUPPORT: return "EAFNOSUPPORT";
    case EINVAL: return "EINVAL";
    case ELOOP: return "ELOOP";
    case ENAMETOOLONG: return "ENAMETOOLONG";
    case ENOENT: return "ENOENT";
    case ENOTDIR: return "ENOTDIR";
    case EMFILE: return "EMFILE";
    case ENFILE: return "ENFILE";
    case ENOTSOCK: return "ENOTSOCK";
    // connect(2) side. A unix-socket connect to a path that exists but is not a
    // socket is ECONNREFUSED on Linux, and one whose mode denies access EACCES;
    // both used to be reported as a flat ENOENT.
    case ECONNREFUSED: return "ECONNREFUSED";
    case ECONNRESET: return "ECONNRESET";
    case ETIMEDOUT: return "ETIMEDOUT";
    case EHOSTUNREACH: return "EHOSTUNREACH";
    case ENETUNREACH: return "ENETUNREACH";
    case ENETDOWN: return "ENETDOWN";
    case EPIPE: return "EPIPE";
    case EISDIR: return "EISDIR";
    default: return "UNKNOWN";
    }
}

JSValueRef netn_listen_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "net.listen: not supported on Windows yet");
#else
    const std::string host{fs_arg(ctx, argc, args, 0)};
    // Permission Model: node gates the Net scope in tcp_wrap.cc / pipe_wrap.cc /
    // udp_wrap.cc — i.e. at the wrap's Bind/Connect/Listen, which is the syscall.
    // Same place here. `--allow-net` is all-or-nothing in node; the resource is
    // the address or socket path so the error names what was refused.
    if (permission_deny_scope(ctx, exc, mbun::permission::Scope::Net, host))
        return JSValueMakeUndefined(ctx);
    const auto port{static_cast<std::uint16_t>(
        argc > 1 ? JSValueToNumber(ctx, args[1], nullptr) : 0)};
    sockaddr_in addr{};
    if (!net_parse_addr(host, port, addr)) {
        return fs_throw(ctx, exc, "net.listen: invalid address '" + host + "'");
    }
    const int fd{socket(AF_INET, SOCK_STREAM, 0)};
    if (fd < 0) return fs_throw(ctx, exc, "net.listen: socket() failed");
    const int one{1};
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    // net.Server.listen({ reusePort: true }): several sockets may hold the same
    // address and the kernel load-balances accepts between them. Without it a
    // second listen on the port fails EADDRINUSE (test-cluster-net-reuseport,
    // test-net-server-reuseport).
    if (argc > 2 && JSValueToBoolean(ctx, args[2])) {
#ifdef SO_REUSEPORT
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) != 0) {
            const int saved{errno};
            close(fd);
            return fs_throw(ctx, exc, std::string{"net.listen: SO_REUSEPORT failed ("} +
                                          net_errno_name(saved) + ": " + std::strerror(saved) + ")");
        }
#else
        close(fd);
        return fs_throw(ctx, exc, "net.listen: SO_REUSEPORT failed (ENOTSUP: not supported)");
#endif
    }
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        ::listen(fd, 128) != 0) {
        const int saved{errno};
        close(fd);
        // The errno NAME, not just strerror: the JS layer classifies a bind
        // failure by scanning this message, so "Permission denied" alone was
        // reported as EADDRINUSE (test-cluster-bind-privileged-port wants EACCES).
        return fs_throw(ctx, exc, std::string{"net.listen: bind/listen failed ("} +
                                      net_errno_name(saved) + ": " + std::strerror(saved) + ")");
    }
    sockaddr_in bound{};
    socklen_t blen{sizeof bound};
    getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen);
    net_set_nonblock(fd);
    net_fds().insert(fd);
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, r, "fd", JSValueMakeNumber(ctx, fd));
    set_prop(ctx, r, "port", JSValueMakeNumber(ctx, ntohs(bound.sin_port)));
    return r;
#endif
}

// net.listenUnix(path) → { fd, path }: AF_UNIX stream listener for
// Bun.listen({unix}). Only the bind differs from netn_listen_cb — accept/read/
// write/wait/close are family-agnostic and shared with the AF_INET path above.
// Blueprint: bun src/runtime/socket/Listener.rs (listen → UnixOrHost::Unix).
JSValueRef netn_listen_unix_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "net.listenUnix: not supported on Windows yet");
#else
    const std::string path{fs_arg(ctx, argc, args, 0)};
    // Permission Model: node gates the Net scope in tcp_wrap.cc / pipe_wrap.cc /
    // udp_wrap.cc — i.e. at the wrap's Bind/Connect/Listen, which is the syscall.
    // Same place here. `--allow-net` is all-or-nothing in node; the resource is
    // the address or socket path so the error names what was refused.
    if (permission_deny_scope(ctx, exc, mbun::permission::Scope::Net, path))
        return JSValueMakeUndefined(ctx);
    if (path.empty()) return fs_throw(ctx, exc, "net.listenUnix: empty socket path");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    // sun_path is a fixed 108-byte field. Refuse rather than truncate: a silently
    // shortened path would bind a socket the caller never asked for.
    if (path.size() >= sizeof addr.sun_path) {
        return fs_throw(ctx, exc, "net.listenUnix: socket path too long (max " +
                                      std::to_string(sizeof addr.sun_path - 1) + " bytes)");
    }
    std::memcpy(addr.sun_path, path.data(), path.size());
    // bind() does NOT pre-unlink: Node/libuv (and bun) leave an existing socket
    // file alone so a stale/live path surfaces EADDRINUSE rather than silently
    // stealing it. close()/stop() unlinks the file it created (JS layer).
    const int fd{socket(AF_UNIX, SOCK_STREAM, 0)};
    if (fd < 0) return fs_throw(ctx, exc, "net.listenUnix: socket() failed");
    const auto alen{static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1)};
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), alen) != 0 || ::listen(fd, 128) != 0) {
        close(fd);
        return fs_throw(ctx, exc, std::string{"net.listenUnix: bind/listen failed ("} +
                                      std::strerror(errno) + ")");
    }
    net_set_nonblock(fd);
    net_fds().insert(fd);
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, r, "fd", JSValueMakeNumber(ctx, fd));
    set_prop(ctx, r, "path", make_string(ctx, path));
    return r;
#endif
}

// net.accept(listenFd) → connection fd, or -1 when none is pending.
JSValueRef netn_accept_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "net.accept: not supported on Windows yet");
#else
    (void)exc;
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const int conn{accept(fd, nullptr, nullptr)};
    if (conn < 0) return JSValueMakeNumber(ctx, -1);
    const int one{1};
    setsockopt(conn, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    net_set_nonblock(conn);
    net_fds().insert(conn);
    return JSValueMakeNumber(ctx, conn);
#endif
}

// net.connect(host, port) → connection fd (loopback connect completes fast; the
// socket then goes non-blocking for all reads/writes). Throws ECONNREFUSED.
JSValueRef netn_connect_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "net.connect: not supported on Windows yet");
#else
    const std::string host{fs_arg(ctx, argc, args, 0)};
    // Permission Model: node gates the Net scope in tcp_wrap.cc / pipe_wrap.cc /
    // udp_wrap.cc — i.e. at the wrap's Bind/Connect/Listen, which is the syscall.
    // Same place here. `--allow-net` is all-or-nothing in node; the resource is
    // the address or socket path so the error names what was refused.
    if (permission_deny_scope(ctx, exc, mbun::permission::Scope::Net, host))
        return JSValueMakeUndefined(ctx);
    const auto port{static_cast<std::uint16_t>(
        argc > 1 ? JSValueToNumber(ctx, args[1], nullptr) : 0)};
    sockaddr_in addr{};
    if (!net_parse_addr(host, port, addr)) {
        return fs_throw(ctx, exc, "net.connect: invalid address '" + host + "'");
    }
    const int fd{socket(AF_INET, SOCK_STREAM, 0)};
    if (fd < 0) return fs_throw(ctx, exc, "net.connect: socket() failed");
    // net.connect(host, port, localAddress, localPort): node binds the socket
    // to options.localAddress/localPort BEFORE connect(2) (tcp_wrap.cc Bind
    // then Connect), and a local address this host does not own fails the
    // connection with EADDRNOTAVAIL rather than silently dialling from the
    // default source address (test-http-localaddress-bind-error).
    const std::string laddr{argc > 2 && JSValueIsString(ctx, args[2]) ? fs_arg(ctx, argc, args, 2)
                                                                      : std::string{}};
    const auto lport{static_cast<std::uint16_t>(
        argc > 3 && JSValueIsNumber(ctx, args[3]) ? JSValueToNumber(ctx, args[3], nullptr) : 0)};
    if (!laddr.empty() || lport != 0) {
        sockaddr_in local{};
        if (!net_parse_addr(laddr.empty() ? std::string{"0.0.0.0"} : laddr, lport, local)) {
            close(fd);
            return fs_throw(ctx, exc, "EADDRNOTAVAIL: bind to " + laddr + " failed (invalid address)");
        }
        if (lport != 0) {
            const int reuse{1};
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
        }
        if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof local) != 0) {
            const int saved{errno};
            close(fd);
            return fs_throw(ctx, exc, std::string{net_errno_name(saved)} + ": bind to " + laddr + ":" +
                                          std::to_string(lport) + " failed (" + std::strerror(saved) + ")");
        }
    }
    int rc;
    do { rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr); }
    while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        close(fd);
        return fs_throw(ctx, exc, std::string{"ECONNREFUSED: connect to "} + host + ":" +
                                      std::to_string(port) + " failed (" + std::strerror(errno) + ")");
    }
    const int one{1};
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    net_set_nonblock(fd);
    net_fds().insert(fd);
    return JSValueMakeNumber(ctx, fd);
#endif
}

// net.sockname(fd) → "address:port" of the LOCAL end (getsockname), or null.
// node publishes this as socket.localAddress/localPort; without it a client
// socket reported localPort 0 forever, which is what an agent-reuse test reads
// to tell two connections apart.
JSValueRef netn_sockname_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "net.sockname: not supported on Windows yet");
#else
    (void)exc;
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    if (fd < 0) return JSValueMakeNull(ctx);
    sockaddr_in addr{};
    socklen_t len{sizeof addr};
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0 || addr.sin_family != AF_INET) {
        return JSValueMakeNull(ctx);
    }
    char buf[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof buf) == nullptr) return JSValueMakeNull(ctx);
    const std::string out{std::string{buf} + ":" + std::to_string(ntohs(addr.sin_port))};
    return make_string(ctx, out);
#endif
}

// net.peername(fd) → "address:port" of the REMOTE end (getpeername), or null.
// The accepted-socket path had no way to learn it, so every server-side
// socket.remotePort read 0 — which agreed with a client localPort that was also
// 0 until getsockname landed (test-net-socket-local-address compares the two).
JSValueRef netn_peername_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "net.peername: not supported on Windows yet");
#else
    (void)exc;
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    if (fd < 0) return JSValueMakeNull(ctx);
    sockaddr_in addr{};
    socklen_t len{sizeof addr};
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0 || addr.sin_family != AF_INET) {
        return JSValueMakeNull(ctx);
    }
    char buf[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof buf) == nullptr) return JSValueMakeNull(ctx);
    return make_string(ctx, std::string{buf} + ":" + std::to_string(ntohs(addr.sin_port)));
#endif
}

// net.connectUnix(path) → connection fd: AF_UNIX stream connect for
// Bun.connect({unix}). Only the address differs from netn_connect_cb — the fd is
// then non-blocking and shares read/write/wait/close with the AF_INET path.
// Blueprint: bun src/runtime/socket/Listener.rs (connect → UnixOrHost::Unix).
JSValueRef netn_connect_unix_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "net.connectUnix: not supported on Windows yet");
#else
    const std::string path{fs_arg(ctx, argc, args, 0)};
    // Permission Model: node gates the Net scope in tcp_wrap.cc / pipe_wrap.cc /
    // udp_wrap.cc — i.e. at the wrap's Bind/Connect/Listen, which is the syscall.
    // Same place here. `--allow-net` is all-or-nothing in node; the resource is
    // the address or socket path so the error names what was refused.
    if (permission_deny_scope(ctx, exc, mbun::permission::Scope::Net, path))
        return JSValueMakeUndefined(ctx);
    if (path.empty()) return fs_throw(ctx, exc, "net.connectUnix: empty socket path");
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) {
        return fs_throw(ctx, exc, "net.connectUnix: socket path too long (max " +
                                      std::to_string(sizeof addr.sun_path - 1) + " bytes)");
    }
    std::memcpy(addr.sun_path, path.data(), path.size());
    const int fd{socket(AF_UNIX, SOCK_STREAM, 0)};
    if (fd < 0) return fs_throw(ctx, exc, "net.connectUnix: socket() failed");
    const auto alen{static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1)};
    int rc;
    do { rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), alen); }
    while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        // The errno NAME, not a hard-coded ENOENT: connecting to a path that
        // exists but is not a socket is ECONNREFUSED (or ENOTSOCK), and one
        // whose mode denies access is EACCES. The JS layer classifies the
        // failure by scanning this message, so a fixed "ENOENT" reported every
        // pipe connect failure as a missing file (test-net-pipe-connect-errors).
        const int saved{errno};
        close(fd);
        return fs_throw(ctx, exc, std::string{net_errno_name(saved)} + ": connect to unix:" + path +
                                      " failed (" + std::strerror(saved) + ")");
    }
    net_set_nonblock(fd);
    net_fds().insert(fd);
    return JSValueMakeNumber(ctx, fd);
#endif
}

// net.read(fd) → base64 chunk | "" (would block) | null (EOF). Throws on a hard
// socket error with the errno name in the message (ECONNRESET surfaced to JS).
JSValueRef netn_read_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                        const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "net.read: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    char buf[65536];
    ssize_t n;
    do { n = recv(fd, buf, sizeof buf, 0); } while (n < 0 && errno == EINTR);
    if (n == 0) return JSValueMakeNull(ctx);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return make_string(ctx, "");
        return fs_throw(ctx, exc, errno == ECONNRESET ? "ECONNRESET: socket read failed"
                                                      : std::string{"net.read: "} + std::strerror(errno));
    }
    return make_string(ctx, b64_encode(reinterpret_cast<const unsigned char*>(buf),
                                       static_cast<std::size_t>(n)));
#endif
}

// net.readFd(fd) → base64 chunk | "" (would block) | null (EOF). The non-socket
// twin of net.read: process.stdin's fd 0 is a pipe/file/tty, and recv(2) on a
// non-socket fails outright with ENOTSOCK, so this uses read(2). The first touch
// flips the fd non-blocking and joins it to the reactor's poll set, so an idle
// stdin pump parks in netn_wait_cb's poll() instead of busy-spinning the CPU.
// Registering lazily (rather than at startup) keeps fd 0 blocking for programs
// that never pump stdin, so spawned children inherit it unmodified.
JSValueRef netn_read_fd_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "net.readFd: not supported on Windows yet");
#else
    (void)exc;
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    if (fd < 0) return JSValueMakeNull(ctx);
    if (!net_fds().contains(fd)) {
        net_set_nonblock(fd);
        net_fds().insert(fd);
    }
    char buf[65536];
    ssize_t n;
    do { n = read(fd, buf, sizeof buf); } while (n < 0 && errno == EINTR);
    if (n == 0) return JSValueMakeNull(ctx);  // EOF — the writer closed its end
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return make_string(ctx, "");
        // EBADF/EIO (closed stdin, or a tty gone away in the background) is not
        // recoverable for a read pump: report it as EOF rather than throwing
        // into the reactor, which would drop the item mid-drain.
        return JSValueMakeNull(ctx);
    }
    return make_string(ctx, b64_encode(reinterpret_cast<const unsigned char*>(buf),
                                       static_cast<std::size_t>(n)));
#endif
}

// net.readByteBlocking(fd) → byte (0-255) | -1 (EOF/error). The BLOCKING one-byte
// twin of readFd, used by the WHATWG simple dialogs (alert/confirm/prompt): they
// pause the whole program until the user answers, reading a line a byte at a time
// (bun src/runtime/webcore/prompt.rs BufferedStdin.take_byte). fd 0 is normally
// blocking until the stdin pump lazily flips it non-blocking; poll() on EAGAIN
// covers the flipped case so a byte is still returned once input arrives.
JSValueRef netn_read_byte_blocking_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                      const JSValueRef args[], JSValueRef*) {
#if defined(_WIN32)
    return JSValueMakeNumber(ctx, -1);
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    if (fd < 0) return JSValueMakeNumber(ctx, -1);
    for (;;) {
        unsigned char b;
        const ssize_t n{read(fd, &b, 1)};
        if (n == 1) return JSValueMakeNumber(ctx, static_cast<double>(b));
        if (n == 0) return JSValueMakeNumber(ctx, -1);  // EOF
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            pollfd pfd{fd, POLLIN, 0};
            poll(&pfd, 1, -1);  // block until readable (or hangup)
            continue;
        }
        return JSValueMakeNumber(ctx, -1);
    }
#endif
}

// net.write(fd, b64) → bytes accepted (may be a partial write; 0 = would block).
JSValueRef netn_write_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "net.write: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const std::string data{b64_decode(fs_arg(ctx, argc, args, 1))};
    std::size_t off{0};
    while (off < data.size()) {
        const ssize_t w{send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL)};
        if (w > 0) { off += static_cast<std::size_t>(w); continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return fs_throw(ctx, exc, errno == EPIPE || errno == ECONNRESET
                                      ? "ECONNRESET: socket write failed"
                                      : std::string{"net.write: "} + std::strerror(errno));
    }
    return JSValueMakeNumber(ctx, static_cast<double>(off));
#endif
}

// net.shutdown(fd): half-close the write side (FIN) — node's socket.end().
JSValueRef netn_shutdown_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef*) {
#if !defined(_WIN32)
    if (argc > 0) shutdown(static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)), SHUT_WR);
#endif
    return JSValueMakeUndefined(ctx);
}

// net.track(fd) → join an EXTERNALLY acquired descriptor to the reactor's poll
// set (and make it non-blocking). Descriptors that arrive over SCM_RIGHTS —
// node's NODE_HANDLE protocol, i.e. every socket cluster hands to a worker —
// never passed through accept()/connect(), so they were absent from net_fds()
// and netn_wait_cb never polled them: the pump parked for up to 60s while a
// readable byte or a FIN sat unnoticed on the socket, which is a hang for any
// test with a 15s budget (test-cluster-worker-wait-server-close,
// test-cluster-disconnect). Idempotent, so the accept()/connect() paths that
// already registered their fd can call it too.
JSValueRef netn_track_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef*) {
#if !defined(_WIN32)
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    if (fd >= 0 && !net_fds().contains(fd)) {
        net_set_nonblock(fd);
        net_fds().insert(fd);
    }
#endif
    return JSValueMakeUndefined(ctx);
}

JSValueRef netn_close_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef*) {
#if !defined(_WIN32)
    if (argc > 0) {
        const int fd{static_cast<int>(JSValueToNumber(ctx, args[0], nullptr))};
        net_fds().erase(fd);
        net_tls().erase(fd);  // drop any TLS channel riding this fd
        close(fd);
    }
#endif
    return JSValueMakeUndefined(ctx);
}

// setSockBuf(fd, which, size): which 1=SO_SNDBUF, 2=SO_RCVBUF. Backs
// bun:internal-for-testing setSocketOptions (socket_body.rs js_set_socket_options).
JSValueRef netn_set_sockbuf_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef*) {
#if !defined(_WIN32)
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const int which{argc > 1 ? static_cast<int>(JSValueToNumber(ctx, args[1], nullptr)) : 0};
    const int size{argc > 2 ? static_cast<int>(JSValueToNumber(ctx, args[2], nullptr)) : 0};
    if (fd >= 0 && which == 1) { (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof size); }
    else if (fd >= 0 && which == 2) { (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof size); }
    // which == 3: SO_KEEPALIVE, with `size` = TCP_KEEPIDLE seconds (0 = leave
    // the kernel default). node/undici arm both on every pooled client socket;
    // Linux only reports the keepalive timer in /proc/net/tcp once SO_KEEPALIVE
    // is on. Blueprint: bun src/runtime/socket (us_socket_keepalive).
    else if (fd >= 0 && which == 3) {
        const int on{size > 0 ? 1 : 0};
        (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
#if defined(TCP_KEEPIDLE)
        if (size > 0) (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &size, sizeof size);
#endif
    }
#endif
    return JSValueMakeUndefined(ctx);
}

// net.wait(ms) → number of fds ready to read. Blocks the pump for at most `ms`
// so a drain cycle with no immediate progress doesn't busy-spin the CPU.
JSValueRef netn_wait_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                        const JSValueRef args[], JSValueRef*) {
#if defined(_WIN32)
    return JSValueMakeNumber(ctx, 0);
#else
    int ms{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : 1};
    if (ms < 0) ms = 0;
    // Long parks are safe: every wake source is an fd in the poll set (net
    // sockets/listeners, the serve reactor's epoll fd) or bounds `ms` itself
    // (__mbun_pump_idle_ms returns the next timer deadline).
    if (ms > 60000) ms = 60000;
    std::vector<pollfd> pfds;
    pfds.reserve(net_fds().size());
    for (const int fd : net_fds()) pfds.push_back(pollfd{fd, POLLIN, 0});
    // No sockets: a zero-fd poll() is a plain bounded sleep — still pace the
    // pump instead of returning immediately (busy-spin).
    const int n{poll(pfds.empty() ? nullptr : pfds.data(),
                     static_cast<nfds_t>(pfds.size()), ms)};
    return JSValueMakeNumber(ctx, n < 0 ? 0 : n);
#endif
}

// ── TLS over the non-blocking reactor fds (T-TLS.3) ─────────────────────────
// Per-fd mbun::tls::TlsChannel (memory-BIO engine, mbun.openssl ecosystem
// package — no host libssl). All progress is single-pass and non-blocking:
// the JS reactor calls tlsStep/tlsRead/tlsWrite from its poll loop, shuttling
// ciphertext between the socket and the channel's BIOs (bun uws_sys ssl.zig
// shape). Backs Bun.serve({tls}), fetch(https://), and wss.
#if !defined(_WIN32)
// Move ciphertext: socket → read-BIO (bounded per pass), then write-BIO (plus
// any EAGAIN tail) → socket. Returns false on a fatal socket error.
bool net_tls_shuttle(int fd, NetTlsState& st) {
    char buf[65536];
    for (int pass = 0; pass < 16; ++pass) {
        ssize_t n;
        do { n = recv(fd, buf, sizeof buf, 0); } while (n < 0 && errno == EINTR);
        if (n > 0) {
            st.channel.feed_encrypted(std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(buf), static_cast<std::size_t>(n)});
            continue;
        }
        if (n == 0) { st.sockEof = true; break; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        st.failed = true;
        st.error = std::string{"socket read: "} + std::strerror(errno);
        return false;
    }
    if (!st.channel.established()) st.channel.handshake();
    if (st.channel.has_encrypted()) {
        std::vector<std::uint8_t> enc{st.channel.take_encrypted()};
        st.pendingOut.append(reinterpret_cast<const char*>(enc.data()), enc.size());
    }
    while (!st.pendingOut.empty()) {
        const ssize_t w{send(fd, st.pendingOut.data(), st.pendingOut.size(), MSG_NOSIGNAL)};
        if (w > 0) { st.pendingOut.erase(0, static_cast<std::size_t>(w)); continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        st.failed = true;
        st.error = std::string{"socket write: "} + std::strerror(errno);
        return false;
    }
    return true;
}
#endif

// net.tlsWrap(fd, isServer, cert, key, servername, verify) → bool. verify:
// 0 = disabled (rejectUnauthorized:false / NODE_TLS_REJECT_UNAUTHORIZED=0),
// 1 = required. `cert`/`key` are PEM strings (server side); `servername` is
// the client SNI. ca (optional PEM) rides arg 6.
JSValueRef netn_tls_wrap_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "net.tlsWrap: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const bool isServer{argc > 1 && JSValueToBoolean(ctx, args[1])};
    mbun::tls::Config cfg{};
    cfg.certificate = fs_arg(ctx, argc, args, 2);
    cfg.key = fs_arg(ctx, argc, args, 3);
    cfg.serverName = fs_arg(ctx, argc, args, 4);
    // verify: 0 = disabled (rejectUnauthorized:false / no requestCert),
    // 1 = required (fail on bad/absent peer cert), 2 = optional (request a peer
    // cert but do not fail — server requestCert:true + rejectUnauthorized:false).
    const int verify{argc > 5 ? static_cast<int>(JSValueToNumber(ctx, args[5], nullptr)) : 0};
    cfg.verify = verify == 2 ? mbun::tls::VerifyMode::optional
                 : verify >= 1 ? mbun::tls::VerifyMode::required
                               : mbun::tls::VerifyMode::disabled;
    cfg.ca = fs_arg(ctx, argc, args, 6);
    // arg 7: ALPN protocol names, comma-joined (client offers / server selects).
    if (argc > 7) {
        const std::string alpn{fs_arg(ctx, argc, args, 7)};
        std::size_t pos{0};
        while (pos < alpn.size()) {
            const std::size_t comma{alpn.find(',', pos)};
            const std::string one{alpn.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos)};
            if (!one.empty()) cfg.alpnProtocols.push_back(one);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    // args 8/9: minVersion / maxVersion names ("TLSv1.2"), mapped to OpenSSL
    // version constants. Empty/unknown leaves the backend default in place.
    // The name "none" is the JS layer's explicit "unpinned" marker: node passes
    // min=max=0 to SecureContext::Init when the caller supplied `secureProtocol`,
    // so the SSL_METHOD's own range applies rather than node's TLS 1.2 floor.
    // ref js_tls_live.cppm resolveVersions.
    const auto version_arg{[&](std::size_t i) {
        const std::string name{fs_arg(ctx, argc, args, i)};
        if (name == "none") return -1;
        return mbun::tls::tls_version_from_name(name);
    }};
    if (argc > 8) { cfg.minVersion = version_arg(8); }
    if (argc > 9) { cfg.maxVersion = version_arg(9); }
    // arg 10: node's options.ciphers, an OpenSSL cipher list string. Empty leaves
    // the context default; a list OpenSSL matches nothing to fails the wrap
    // rather than quietly widening the negotiated set.
    if (argc > 10) { cfg.ciphers = fs_arg(ctx, argc, args, 10); }
    // arg 11: hostCheck. node performs the peer-NAME check in JS
    // (tls.checkServerIdentity) and lets a caller replace it; mbun folds the
    // default check into OpenSSL via SSL_set1_host, which is stricter but cannot
    // be replaced. `false` here means "a caller-supplied checkServerIdentity will
    // run in JS instead", so the native check must stand aside — the CHAIN
    // verification is untouched either way. Absent argument = check enabled.
    if (argc > 11 && JSValueIsBoolean(ctx, args[11])) {
        cfg.hostCheck = JSValueToBoolean(ctx, args[11]);
    }
    // arg 12: the TLS 1.3 half of node's `ciphers` option. OpenSSL keeps the 1.3
    // suites behind SSL_CTX_set_ciphersuites, a different slot from the cipher
    // list in arg 10, and node splits the caller's string across the two
    // (lib/internal/tls/secure-context.js processCiphers). Empty leaves the
    // engine's own TLS 1.3 suite selection in place.
    if (argc > 12) { cfg.cipherSuites = fs_arg(ctx, argc, args, 12); }
    // arg 13: caIsComplete. `true` means arg 6 is the caller's ENTIRE trust
    // store — tls.setDefaultCACertificates() replaced node's addRootCerts() —
    // so the platform default store must not be consulted, not even when the
    // supplied store is empty ("trust nothing" is a legitimate instruction).
    if (argc > 13 && JSValueIsBoolean(ctx, args[13])) {
        cfg.caIsComplete = JSValueToBoolean(ctx, args[13]);
    }
    // arg 14: base64 of a previously serialised SSL_SESSION to resume with
    // (node's tls.connect({ session }) / socket.setSession()). Client only.
    if (argc > 14) { cfg.sessionDer = b64_decode(fs_arg(ctx, argc, args, 14)); }
    // arg 15: base64 of the server's 48-byte session-ticket key (node's
    // tls.createServer({ ticketKeys }) / server.setTicketKeys()). This is what
    // lets a ticket issued by one accepted connection be resumed by the next —
    // every connection here owns its own SSL_CTX, so without a stable key the
    // ticket is undecryptable by design.
    if (argc > 15) { cfg.ticketKeys = b64_decode(fs_arg(ctx, argc, args, 15)); }
    auto st{std::make_unique<NetTlsState>(
        isServer ? mbun::tls::TlsRole::server : mbun::tls::TlsRole::client, std::move(cfg))};
    if (!st->channel.valid()) {
        return fs_throw(ctx, exc,
                        "tlsWrap: " + std::string{st->channel.last_error().empty()
                                                      ? "TLS context setup failed"
                                                      : st->channel.last_error()});
    }
    st->channel.handshake();  // client: emit ClientHello into the write BIO
    net_tls()[fd] = std::move(st);
    return JSValueMakeBoolean(ctx, true);
#endif
}

// net.tlsStep(fd) → 1 established | 0 in progress | -1 failed | -2 no channel.
JSValueRef netn_tls_step_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef*) {
#if defined(_WIN32)
    return JSValueMakeNumber(ctx, -2);
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const auto found{net_tls().find(fd)};
    if (found == net_tls().end()) return JSValueMakeNumber(ctx, -2);
    NetTlsState& st{*found->second};
    if (!st.failed) net_tls_shuttle(fd, st);
    if (st.failed || st.channel.handshake_state() == mbun::tls::HandshakeState::failed) {
        return JSValueMakeNumber(ctx, -1);
    }
    if (st.channel.established()) return JSValueMakeNumber(ctx, 1);
    if (st.sockEof) return JSValueMakeNumber(ctx, -1);  // peer died mid-handshake
    return JSValueMakeNumber(ctx, 0);
#endif
}

// net.tlsError(fd) → { code, message }. The failure classification for a
// handshake that tlsStep reported as -1: a fatal OpenSSL reason surfaces its
// node-style code (e.g. ERR_SSL_NO_SUPPORTED_VERSIONS_ENABLED) so the JS
// TLSSocket error carries the same .code node would; a peer FIN before the
// handshake completes is ECONNRESET; anything else stays ERR_TLS_HANDSHAKE.
// Blueprint: bun-zig-src src/runtime/socket/tls_socket_functions.zig (handshake
// failure maps the SSL error to a code, EOF maps to ECONNRESET).
JSValueRef netn_tls_error_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef*) {
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
#if !defined(_WIN32)
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const auto found{net_tls().find(fd)};
    if (found != net_tls().end()) {
        NetTlsState& st{*found->second};
        const std::string_view sslCode{st.channel.error_code()};
        if (!sslCode.empty()) {
            set_prop(ctx, r, "code", make_string(ctx, std::string{sslCode}));
            set_prop(ctx, r, "message", make_string(ctx, std::string{st.channel.last_error()}));
            return r;
        }
        if (st.sockEof && !st.channel.established()) {
            set_prop(ctx, r, "code", make_string(ctx, "ECONNRESET"));
            set_prop(ctx, r, "message", make_string(ctx, "read ECONNRESET"));
            return r;
        }
        if (st.failed) {
            set_prop(ctx, r, "code", make_string(ctx, "ECONNRESET"));
            set_prop(ctx, r, "message", make_string(ctx, st.error));
            return r;
        }
    }
#endif
    set_prop(ctx, r, "code", make_string(ctx, "ERR_TLS_HANDSHAKE"));
    set_prop(ctx, r, "message", make_string(ctx, "TLS handshake failed"));
    return r;
}

// net.tlsRead(fd) → plaintext b64 | "" (nothing yet) | null (clean EOF).
JSValueRef netn_tls_read_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "net.tlsRead: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const auto found{net_tls().find(fd)};
    // Channel already scrubbed (fd closed/reused): report clean EOF rather than
    // throwing — a socket mid-teardown must not surface a spurious read error.
    if (found == net_tls().end()) return JSValueMakeNull(ctx);
    NetTlsState& st{*found->second};
    if (!st.failed) net_tls_shuttle(fd, st);
    if (st.failed) return fs_throw(ctx, exc, "ECONNRESET: " + st.error);
    std::vector<std::uint8_t> plain{st.channel.read()};
    if (!plain.empty()) {
        return make_string(ctx, b64_encode(plain.data(), plain.size()));
    }
    if (st.sockEof || st.channel.shutdown_complete()) return JSValueMakeNull(ctx);
    return make_string(ctx, "");
#endif
}

// net.tlsWrite(fd, b64) → plaintext bytes accepted (ciphertext is flushed to
// the socket as far as the kernel allows; the tail rides pendingOut).
JSValueRef netn_tls_write_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "net.tlsWrite: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const auto found{net_tls().find(fd)};
    if (found == net_tls().end()) return fs_throw(ctx, exc, "tlsWrite: no TLS channel on fd");
    NetTlsState& st{*found->second};
    if (st.failed) return fs_throw(ctx, exc, "ECONNRESET: " + st.error);
    if (!st.channel.established()) return JSValueMakeNumber(ctx, 0);  // pre-handshake: retry later
    const std::string data{b64_decode(fs_arg(ctx, argc, args, 1))};
    const std::size_t accepted{st.channel.write(std::span<const std::uint8_t>{
        reinterpret_cast<const std::uint8_t*>(data.data()), data.size()})};
    if (!net_tls_shuttle(fd, st)) return fs_throw(ctx, exc, "ECONNRESET: " + st.error);
    return JSValueMakeNumber(ctx, static_cast<double>(accepted));
#endif
}

// net.tlsClose(fd): send close_notify (best-effort) but KEEP the channel — the
// socket may keep polling tlsRead until it observes the peer's own close_notify
// / FIN, and a graceful half-close (node:tls / Bun socket end()) leaves the
// socket in NET.items after this call. The channel is scrubbed for good by
// net.close (netn_close_cb). Erasing here made a still-polling socket hit
// "tlsRead: no TLS channel on fd" once, surfacing as a spurious error mid-close
// (observed under many concurrent graceful closes).
JSValueRef netn_tls_close_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef*) {
#if !defined(_WIN32)
    if (argc > 0) {
        const int fd{static_cast<int>(JSValueToNumber(ctx, args[0], nullptr))};
        const auto found{net_tls().find(fd)};
        if (found != net_tls().end()) {
            NetTlsState& st{*found->second};
            if (!st.failed) {
                st.channel.shutdown();
                net_tls_shuttle(fd, st);
            }
        }
    }
#endif
    return JSValueMakeUndefined(ctx);
}

// net.tlsInfo(fd) → { established, protocol, cipher, servername, authorized,
// peerCert? } | null. Negotiated parameters plus the peer's leaf certificate as
// PEM, feeding the JS TLSSocket surface (getProtocol/getCipher/getPeerCertificate
// and socket.authorized). Blueprint: bun-zig-src src/runtime/socket/
// tls_socket_functions.zig (getPeerCertificate/getProtocol/getCipher read the
// SSL* after handshake); the PEM is parsed into node's cert object in JS via the
// existing __mbunCryptoAsymNative.x509parse bridge.
JSValueRef netn_tls_info_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef*) {
#if defined(_WIN32)
    return JSValueMakeNull(ctx);
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const auto found{net_tls().find(fd)};
    if (found == net_tls().end()) return JSValueMakeNull(ctx);
    NetTlsState& st{*found->second};
    const bool est{st.channel.established()};
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, r, "established", JSValueMakeBoolean(ctx, est));
    set_prop(ctx, r, "protocol", make_string(ctx, est ? st.channel.tls_version() : std::string{}));
    set_prop(ctx, r, "cipher", make_string(ctx, est ? st.channel.cipher() : std::string{}));
    // getCipher().standardName — the IANA name, which is not derivable from the
    // OpenSSL name ("AES256-SHA256" -> "TLS_RSA_WITH_AES_256_CBC_SHA256").
    set_prop(ctx, r, "cipherStandardName",
             make_string(ctx, est ? st.channel.cipher_standard_name() : std::string{}));
    set_prop(ctx, r, "servername", make_string(ctx, st.channel.peer_server_name()));
    set_prop(ctx, r, "authorized", JSValueMakeBoolean(ctx, est && st.channel.verify_ok()));
    set_prop(ctx, r, "alpnProtocol", make_string(ctx, est ? st.channel.alpn_protocol() : std::string{}));
    // node's TLSSocket.isSessionReused() — SSL_session_reused. Meaningful on
    // both roles: the client learns its offered session was accepted, the
    // server learns the peer arrived with a valid ticket.
    set_prop(ctx, r, "sessionReused", JSValueMakeBoolean(ctx, est && st.channel.session_reused()));
    if (est) {
        const std::string peer{st.channel.peer_certificate_pem()};
        if (!peer.empty()) set_prop(ctx, r, "peerCert", make_string(ctx, peer));
        // The verified chain, leaf first — node's getPeerCertificate(true) walks
        // it through `issuerCertificate`.
        std::vector<std::string> chain{st.channel.peer_certificate_chain_pem()};
        if (!chain.empty()) {
            std::vector<JSValueRef> items;
            items.reserve(chain.size());
            for (const std::string& pem : chain) items.push_back(make_string(ctx, pem));
            set_prop(ctx, r, "peerChain",
                     JSObjectMakeArray(ctx, items.size(), items.data(), nullptr));
        }
        // getFinished()/getPeerFinished(): the handshake's Finished messages.
        const std::vector<std::uint8_t> fin{st.channel.finished()};
        if (!fin.empty()) set_prop(ctx, r, "finished", make_string(ctx, b64_encode(fin.data(), fin.size())));
        const std::vector<std::uint8_t> pfin{st.channel.peer_finished()};
        if (!pfin.empty()) set_prop(ctx, r, "peerFinished", make_string(ctx, b64_encode(pfin.data(), pfin.size())));
    }
    return r;
#endif
}

// net.tlsKeylog(fd) → array of NSS keylog lines produced since the last call
// (empty array when there is nothing, or no channel on this fd). Draining is
// destructive so a JS-side poll cannot re-emit a line twice. Feeds node's
// TLSSocket/Server 'keylog' event (node src/crypto/crypto_context.cc
// SecureContext::KeylogCallback -> tls/wrap.js onkeylog); nothing else reads it,
// so a process with no keylog listener never surfaces key material.
JSValueRef netn_tls_keylog_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                              const JSValueRef args[], JSValueRef*) {
    std::vector<JSValueRef> items;
#if !defined(_WIN32)
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const auto found{net_tls().find(fd)};
    if (found != net_tls().end()) {
        for (const std::string& line : found->second->channel.take_keylog()) {
            items.push_back(make_string(ctx, line));
        }
    }
#else
    (void)argc;
    (void)args;
#endif
    return JSObjectMakeArray(ctx, items.size(), items.empty() ? nullptr : items.data(), nullptr);
}

// net.tlsNewSessions(fd) → array of base64 SSL_SESSION DER blobs delivered by
// OpenSSL since the last call (empty when there is nothing, or no channel).
// Destructive, like tlsKeylog: a polling reactor must not emit the same session
// twice. Feeds node's TLSSocket 'session' event, which for TLS 1.3 fires only
// once the post-handshake NewSessionTicket has been read — which is why this is
// polled from the read path rather than read once at handshake completion.
JSValueRef netn_tls_new_sessions_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                    const JSValueRef args[], JSValueRef*) {
    std::vector<JSValueRef> items;
#if !defined(_WIN32)
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const auto found{net_tls().find(fd)};
    if (found != net_tls().end()) {
        for (const std::vector<std::uint8_t>& der : found->second->channel.take_new_sessions()) {
            items.push_back(make_string(ctx, b64_encode(der.data(), der.size())));
        }
    }
#else
    (void)argc;
    (void)args;
#endif
    return JSObjectMakeArray(ctx, items.size(), items.empty() ? nullptr : items.data(), nullptr);
}

// net.tlsSession(fd) → base64 of the CURRENT session (node's getSession()), or
// "" when there is none. For TLS 1.3 this is the unresumable placeholder node
// documents right after the handshake; the resumable one arrives through
// tlsNewSessions.
JSValueRef netn_tls_session_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                               const JSValueRef args[], JSValueRef*) {
#if !defined(_WIN32)
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const auto found{net_tls().find(fd)};
    if (found != net_tls().end()) {
        const std::vector<std::uint8_t> der{found->second->channel.session_der()};
        if (!der.empty()) return make_string(ctx, b64_encode(der.data(), der.size()));
    }
#else
    (void)argc;
    (void)args;
#endif
    return make_string(ctx, std::string{});
}

// net.tlsTicket(fd) → base64 of the current session's ticket (node's
// getTLSTicket()), or "" when the session carries none.
JSValueRef netn_tls_ticket_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                              const JSValueRef args[], JSValueRef*) {
#if !defined(_WIN32)
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const auto found{net_tls().find(fd)};
    if (found != net_tls().end()) {
        const std::vector<std::uint8_t> tick{found->second->channel.tls_ticket()};
        if (!tick.empty()) return make_string(ctx, b64_encode(tick.data(), tick.size()));
    }
#else
    (void)argc;
    (void)args;
#endif
    return make_string(ctx, std::string{});
}

// net.tlsExportKeyingMaterial(fd, length, label, contextB64|null) → base64 of
// the RFC 5705 exporter output, or null when the export is not possible (no
// channel / handshake incomplete). node's TLSSocket.exportKeyingMaterial().
// A null context argument means "no context" (RFC 5705 use_context = 0), which
// differs from an empty context — the two produce different key material.
JSValueRef netn_tls_export_keying_material_cb(JSContextRef ctx, JSObjectRef, JSObjectRef,
                                              std::size_t argc, const JSValueRef args[],
                                              JSValueRef*) {
#if defined(_WIN32)
    return JSValueMakeNull(ctx);
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const auto found{net_tls().find(fd)};
    if (found == net_tls().end()) return JSValueMakeNull(ctx);
    const auto length{static_cast<std::size_t>(
        argc > 1 ? JSValueToNumber(ctx, args[1], nullptr) : 0)};
    const std::string label{fs_arg(ctx, argc, args, 2)};
    const bool useContext{argc > 3 && !JSValueIsNull(ctx, args[3])
                          && !JSValueIsUndefined(ctx, args[3])};
    std::string raw{};
    if (useContext) raw = b64_decode(fs_arg(ctx, argc, args, 3));
    const std::vector<std::uint8_t> out{found->second->channel.export_keying_material(
        length, label,
        std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(raw.data()),
                                      raw.size()},
        useContext)};
    if (out.empty()) return JSValueMakeNull(ctx);
    return make_string(ctx, b64_encode(out.data(), out.size()));
#endif
}

// net.tlsSelfSigned(cn) → { cert, key } PEM pair (ephemeral, for local https
// servers in tests / dev — mirrors bun's self-signed dev path).
JSValueRef netn_tls_self_signed_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                   const JSValueRef args[], JSValueRef* exc) {
    const std::string cn{argc > 0 ? fs_arg(ctx, argc, args, 0) : std::string{"localhost"}};
    auto pair{mbun::tls::make_self_signed(cn.empty() ? "localhost" : cn)};
    if (!pair) return fs_throw(ctx, exc, "tlsSelfSigned: certificate generation failed");
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, r, "cert", make_string(ctx, pair->cert));
    set_prop(ctx, r, "key", make_string(ctx, pair->key));
    return r;
}

// net.sleep(ms):真 blocking sleep for Bun.sleepSync (nanosleep; EINTR-resumed,
// clamped to 1s per call — the JS wrapper loops for longer spans).
JSValueRef netn_sleep_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                         const JSValueRef args[], JSValueRef*) {
#if !defined(_WIN32)
    double ms{argc > 0 ? JSValueToNumber(ctx, args[0], nullptr) : 0};
    if (!(ms > 0)) return JSValueMakeUndefined(ctx);
    if (ms > 1000) ms = 1000;
    timespec ts{static_cast<time_t>(ms / 1000),
                static_cast<long>((ms - static_cast<long>(ms / 1000) * 1000) * 1e6)};
    timespec rem{};
    while (nanosleep(&ts, &rem) != 0 && errno == EINTR) ts = rem;
#endif
    return JSValueMakeUndefined(ctx);
}
