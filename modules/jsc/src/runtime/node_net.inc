// ── __mbunDgramNative: non-blocking POSIX UDP primitives (node:dgram) ────────
// Companion to net.inc's TCP layer. Same single-threaded, O_NONBLOCK design:
// every datagram socket is non-blocking and the JS dgram reactor (node_net.cppm)
// drains readable datagrams from the event-loop pump via __mbunNetDrain. Bytes
// cross the JS boundary as base64 (the JSC C string path is UTF-8). POSIX only;
// Windows is DEFERRED honestly (the JS layer throws when the native is absent).
#if !defined(_WIN32)

// Parse host+port into a sockaddr_storage for the requested family. "" / "0.0.0.0"
// / "::" map to the wildcard. Returns false on anything that is not a literal of
// that family — the JS layer resolves every hostname through dns.lookup before it
// gets here, exactly as node's dgram does, and node's UDP handle rejects a
// non-literal (uv_ip4_addr fails): _createSocketHandle("localhost", 0, "udp4")
// must report an errno, not quietly bind to 127.0.0.1.
inline bool dgram_parse_addr(const std::string& host, std::uint16_t port, bool v6,
                             sockaddr_storage& ss, socklen_t& len) {
    std::memset(&ss, 0, sizeof ss);
    if (v6) {
        auto* a6{reinterpret_cast<sockaddr_in6*>(&ss)};
        a6->sin6_family = AF_INET6;
        a6->sin6_port = htons(port);
        len = sizeof(sockaddr_in6);
        if (host.empty() || host == "::" || host == "::0" || host == "*") {
            a6->sin6_addr = in6addr_any;
            return true;
        }
        std::string h{host};
        const auto pct{h.find('%')};  // strip a scope suffix (fe80::1%lo)
        std::string scope;
        if (pct != std::string::npos) { scope = h.substr(pct + 1); h = h.substr(0, pct); }
        if (inet_pton(AF_INET6, h.c_str(), &a6->sin6_addr) != 1) return false;
        if (!scope.empty()) {
            unsigned idx{if_nametoindex(scope.c_str())};
            if (idx == 0) idx = static_cast<unsigned>(std::strtoul(scope.c_str(), nullptr, 10));
            a6->sin6_scope_id = idx;
        }
        return true;
    }
    auto* a4{reinterpret_cast<sockaddr_in*>(&ss)};
    a4->sin_family = AF_INET;
    a4->sin_port = htons(port);
    len = sizeof(sockaddr_in);
    if (host.empty() || host == "0.0.0.0" || host == "*") { a4->sin_addr.s_addr = htonl(INADDR_ANY); return true; }
    return inet_pton(AF_INET, host.c_str(), &a4->sin_addr) == 1;
}

// errno → the POSIX name libuv reports it under (node's `err.code`). The JS
// layer turns the name into the negative UV_E* number node's handle methods
// return, via util.getSystemErrorMap(); anything not listed maps to UNKNOWN,
// which is exactly what libuv does for an errno it has no code for.
inline const char* dgram_errno_name(int e) {
    switch (e) {
        case EACCES: return "EACCES";
        case EADDRINUSE: return "EADDRINUSE";
        case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
        case EAFNOSUPPORT: return "EAFNOSUPPORT";
        case EAGAIN: return "EAGAIN";
        case EBADF: return "EBADF";
        case ECONNREFUSED: return "ECONNREFUSED";
        case ECONNRESET: return "ECONNRESET";
        case EDESTADDRREQ: return "EDESTADDRREQ";
        case EFAULT: return "EFAULT";
        case EHOSTUNREACH: return "EHOSTUNREACH";
        case EINVAL: return "EINVAL";
        case EISCONN: return "EISCONN";
        case EMSGSIZE: return "EMSGSIZE";
        case ENETDOWN: return "ENETDOWN";
        case ENETUNREACH: return "ENETUNREACH";
        case ENOBUFS: return "ENOBUFS";
        case ENODEV: return "ENODEV";
        case ENOENT: return "ENOENT";
        case ENOMEM: return "ENOMEM";
        case ENOPROTOOPT: return "ENOPROTOOPT";
        case ENOTCONN: return "ENOTCONN";
        case ENOTSOCK: return "ENOTSOCK";
        case EPERM: return "EPERM";
        case EPIPE: return "EPIPE";
        case EPROTONOSUPPORT: return "EPROTONOSUPPORT";
        default: return "UNKNOWN";
    }
}

// Failure result for every dgram native: the errno NAME as a JS string. Success
// results are always a number / object / null, so the JS side distinguishes the
// two with `typeof r === "string"` — the same split node gets from a negative
// return value. Nothing here throws: node's UDP handle methods report errors by
// return value, and the dgram JS layer wraps them into ErrnoException /
// ExceptionWithHostPort with the syscall name it knows.
inline JSValueRef dgram_err(JSContextRef ctx, int e) {
    return make_string(ctx, dgram_errno_name(e));
}

// Format a sockaddr into { address, port, family } for rinfo / address().
inline JSObjectRef dgram_addr_info(JSContextRef ctx, const sockaddr_storage& ss) {
    char buf[INET6_ADDRSTRLEN]{};
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    if (ss.ss_family == AF_INET6) {
        const auto* a6{reinterpret_cast<const sockaddr_in6*>(&ss)};
        inet_ntop(AF_INET6, &a6->sin6_addr, buf, sizeof buf);
        set_prop(ctx, r, "address", make_string(ctx, buf));
        set_prop(ctx, r, "port", JSValueMakeNumber(ctx, ntohs(a6->sin6_port)));
        set_prop(ctx, r, "family", make_string(ctx, "IPv6"));
    } else {
        const auto* a4{reinterpret_cast<const sockaddr_in*>(&ss)};
        inet_ntop(AF_INET, &a4->sin_addr, buf, sizeof buf);
        set_prop(ctx, r, "address", make_string(ctx, buf));
        set_prop(ctx, r, "port", JSValueMakeNumber(ctx, ntohs(a4->sin_port)));
        set_prop(ctx, r, "family", make_string(ctx, "IPv4"));
    }
    return r;
}
#endif

// dgram.create(type, reuseAddr?, reusePort?) → fd (type "udp4"|"udp6") or an
// errno name. Socket is O_NONBLOCK.
JSValueRef dgramn_create_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "dgram: not supported on Windows yet");
#else
    const std::string type{fs_arg(ctx, argc, args, 0)};
    const bool v6{type == "udp6"};
    const int fd{socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0)};
    if (fd < 0) return dgram_err(ctx, errno);
    // SO_REUSEADDR ONLY when dgram.createSocket({ reuseAddr: true }) asked for
    // it. Setting it unconditionally lets two processes bind the same UDP port,
    // which masks the EADDRINUSE node reports and breaks addMembership's
    // in-use detection (issue 24157 — bun had the same bug).
    if (argc > 1 && JSValueToBoolean(ctx, args[1])) {
        const int one{1};
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    }
#if defined(SO_REUSEPORT)
    if (argc > 2 && JSValueToBoolean(ctx, args[2])) {
        const int one{1};
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    }
#endif
    net_set_nonblock(fd);
    // Register in the shared poll set net.inc's wait() blocks on. The pump parks
    // in poll(POLLIN) over net_fds() for up to 60s at a time
    // (__mbun_pump_idle_ms), so a datagram arriving on an unregistered fd is only
    // noticed when some unrelated timer happens to cut the park short — which is
    // exactly how far apart "receive works" and "receive works reliably" are:
    // every dgram receive was riding luck about pump ordering.
    net_fds().insert(fd);
    return JSValueMakeNumber(ctx, fd);
#endif
}

// dgram.open(fd) → 0, or an errno name. Adopts an existing descriptor as this
// handle's socket (node UDP::Open / dgram bind({ fd })). EINVAL when the fd is
// not a datagram socket — the JS layer turns a non-UDP fd into
// ERR_INVALID_FD_TYPE before it gets here, this is the defence in depth.
JSValueRef dgramn_open_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "dgram: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    int type{0};
    socklen_t tlen{sizeof type};
    if (fd < 0 || getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tlen) != 0) return dgram_err(ctx, EBADF);
    if (type != SOCK_DGRAM) return dgram_err(ctx, EINVAL);
    net_set_nonblock(fd);
    net_fds().insert(fd);
    return JSValueMakeNumber(ctx, 0);
#endif
}

// dgram.handletype(fd) → node guessHandleType()'s string for an fd.
JSValueRef dgramn_handletype_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                const JSValueRef args[], JSValueRef*) {
#if defined(_WIN32)
    return make_string(ctx, "UNKNOWN");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    if (fd < 0) return make_string(ctx, "UNKNOWN");
    if (isatty(fd) == 1) return make_string(ctx, "TTY");
    int type{0};
    socklen_t tlen{sizeof type};
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tlen) == 0) {
        sockaddr_storage ss{};
        socklen_t slen{sizeof ss};
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &slen) == 0 && ss.ss_family == AF_UNIX) {
            return make_string(ctx, "PIPE");
        }
        return make_string(ctx, type == SOCK_DGRAM ? "UDP" : "TCP");
    }
    struct stat st{};
    if (fstat(fd, &st) == 0) {
        if (S_ISFIFO(st.st_mode)) return make_string(ctx, "PIPE");
        if (S_ISREG(st.st_mode)) return make_string(ctx, "FILE");
    }
    return make_string(ctx, "UNKNOWN");
#endif
}

// dgram.bind(fd, host, port, ipv6Only?) → { address, port, family }.
JSValueRef dgramn_bind_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "dgram: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const std::string host{fs_arg(ctx, argc, args, 1)};
    const auto port{static_cast<std::uint16_t>(argc > 2 ? JSValueToNumber(ctx, args[2], nullptr) : 0)};
    sockaddr_storage ss{};
    socklen_t len{};
    const bool v6{argc > 3 && JSValueToBoolean(ctx, args[3])};
    if (!dgram_parse_addr(host, port, v6, ss, len)) return dgram_err(ctx, EINVAL);
    if (v6 && argc > 4 && JSValueToBoolean(ctx, args[4])) {
        const int on{1};
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof on);
    }
    if (bind(fd, reinterpret_cast<sockaddr*>(&ss), len) != 0) return dgram_err(ctx, errno);
    sockaddr_storage bound{};
    socklen_t blen{sizeof bound};
    getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen);
    return dgram_addr_info(ctx, bound);
#endif
}

// dgram.send(fd, b64, port, host, v6?, connected?) → bytes sent, or an errno
// name. `connected` uses send() rather than sendto(): a connected UDP socket
// must reject an explicit destination (node ERR_SOCKET_DGRAM_IS_CONNECTED is the
// JS guard; here it simply means "no address supplied").
JSValueRef dgramn_send_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "dgram: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const std::string data{b64_decode(fs_arg(ctx, argc, args, 1))};
    const auto port{static_cast<std::uint16_t>(argc > 2 ? JSValueToNumber(ctx, args[2], nullptr) : 0)};
    const std::string host{fs_arg(ctx, argc, args, 3)};
    const bool v6{argc > 4 && JSValueToBoolean(ctx, args[4])};
    const bool connected{argc > 5 && JSValueToBoolean(ctx, args[5])};
    ssize_t n;
    if (connected) {
        do { n = send(fd, data.data(), data.size(), MSG_NOSIGNAL); }
        while (n < 0 && errno == EINTR);
    } else {
        sockaddr_storage ss{};
        socklen_t len{};
        if (!dgram_parse_addr(host.empty() ? (v6 ? "::1" : "127.0.0.1") : host, port, v6, ss, len)) {
            return dgram_err(ctx, EINVAL);
        }
        do { n = sendto(fd, data.data(), data.size(), MSG_NOSIGNAL,
                        reinterpret_cast<sockaddr*>(&ss), len); }
        while (n < 0 && errno == EINTR);
    }
    if (n < 0) return dgram_err(ctx, errno);
    return JSValueMakeNumber(ctx, static_cast<double>(n));
#endif
}

// dgram.recv(fd) → { data(b64), address, port, family } | null (would block) |
// errno name.
JSValueRef dgramn_recv_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return JSValueMakeNull(ctx);
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    char buf[65536];
    sockaddr_storage from{};
    socklen_t flen{sizeof from};
    ssize_t n;
    do { n = recvfrom(fd, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &flen); }
    while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return JSValueMakeNull(ctx);
        return dgram_err(ctx, errno);
    }
    JSObjectRef r{dgram_addr_info(ctx, from)};
    set_prop(ctx, r, "data", make_string(ctx, b64_encode(reinterpret_cast<const unsigned char*>(buf),
                                                         static_cast<std::size_t>(n))));
    set_prop(ctx, r, "size", JSValueMakeNumber(ctx, static_cast<double>(n)));
    return r;
#endif
}

// dgram.address(fd) → { address, port, family } | errno name.
JSValueRef dgramn_address_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "dgram: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    sockaddr_storage ss{};
    socklen_t len{sizeof ss};
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return dgram_err(ctx, errno);
    return dgram_addr_info(ctx, ss);
#endif
}

// dgram.peername(fd) → { address, port, family } | errno name (getpeername; the
// backing call for Socket.prototype.remoteAddress()).
JSValueRef dgramn_peername_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                              const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "dgram: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    sockaddr_storage ss{};
    socklen_t len{sizeof ss};
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return dgram_err(ctx, errno);
    return dgram_addr_info(ctx, ss);
#endif
}

// dgram.connect(fd, host, port, v6) → 0 | errno name. A connected UDP socket
// fixes the default destination and starts filtering inbound datagrams.
JSValueRef dgramn_connect_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "dgram: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const std::string host{fs_arg(ctx, argc, args, 1)};
    const auto port{static_cast<std::uint16_t>(argc > 2 ? JSValueToNumber(ctx, args[2], nullptr) : 0)};
    const bool v6{argc > 3 && JSValueToBoolean(ctx, args[3])};
    sockaddr_storage ss{};
    socklen_t len{};
    if (!dgram_parse_addr(host.empty() ? (v6 ? "::1" : "127.0.0.1") : host, port, v6, ss, len)) {
        return dgram_err(ctx, EINVAL);
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&ss), len) != 0) return dgram_err(ctx, errno);
    return JSValueMakeNumber(ctx, 0);
#endif
}

// dgram.disconnect(fd) → 0 | errno name. connect() to AF_UNSPEC is the POSIX
// way to dissolve a UDP association (what uv_udp_connect(NULL) does).
JSValueRef dgramn_disconnect_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "dgram: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    sockaddr_storage ss{};
    std::memset(&ss, 0, sizeof ss);
    ss.ss_family = AF_UNSPEC;
    if (connect(fd, reinterpret_cast<sockaddr*>(&ss), sizeof ss) != 0 && errno != EAFNOSUPPORT) {
        return dgram_err(ctx, errno);
    }
    return JSValueMakeNumber(ctx, 0);
#endif
}

// dgram.bufsize(fd, size, recv) → the effective buffer size | errno name.
// size 0 reads, non-zero sets then reads back (node UDPWrap::BufferSize).
JSValueRef dgramn_bufsize_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "dgram: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const int size{argc > 1 ? static_cast<int>(JSValueToNumber(ctx, args[1], nullptr)) : 0};
    const bool recvBuf{argc > 2 && JSValueToBoolean(ctx, args[2])};
    const int opt{recvBuf ? SO_RCVBUF : SO_SNDBUF};
    if (size != 0 && setsockopt(fd, SOL_SOCKET, opt, &size, sizeof size) != 0) {
        return dgram_err(ctx, errno);
    }
    int cur{0};
    socklen_t clen{sizeof cur};
    if (getsockopt(fd, SOL_SOCKET, opt, &cur, &clen) != 0) return dgram_err(ctx, errno);
    return JSValueMakeNumber(ctx, cur);
#endif
}

// dgram.mcastiface(fd, iface, v6) → 0 | errno name (IP_MULTICAST_IF /
// IPV6_MULTICAST_IF). v4 takes an interface ADDRESS, v6 an interface NAME/index.
JSValueRef dgramn_mcastiface_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "dgram: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    std::string iface{fs_arg(ctx, argc, args, 1)};
    const bool v6{argc > 2 && JSValueToBoolean(ctx, args[2])};
    if (v6) {
        // node/libuv: an IPv6 multicast interface is a name or index. An
        // address+scope form ("::%lo0") whose scope does not resolve falls back to
        // index 0 (default selection); a BARE name that does not resolve — and the
        // empty string — is EINVAL (test-dgram-multicast-set-interface asserts
        // both halves).
        const auto pct{iface.find('%')};
        const bool scoped{pct != std::string::npos};
        if (scoped) iface = iface.substr(pct + 1);
        if (iface.empty()) {
            if (!scoped) return dgram_err(ctx, EINVAL);
        }
        unsigned idx{iface.empty() ? 0u : if_nametoindex(iface.c_str())};
        if (idx == 0 && !iface.empty()) {
            idx = static_cast<unsigned>(std::strtoul(iface.c_str(), nullptr, 10));
            if (idx == 0 && !scoped) return dgram_err(ctx, EINVAL);
        }
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx, sizeof idx) != 0) {
            return dgram_err(ctx, errno);
        }
        return JSValueMakeNumber(ctx, 0);
    }
    in_addr addr{};
    if (iface.empty()) addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, iface.c_str(), &addr) != 1) return dgram_err(ctx, EINVAL);
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &addr, sizeof addr) != 0) {
        return dgram_err(ctx, errno);
    }
    return JSValueMakeNumber(ctx, 0);
#endif
}

// dgram.srcmembership(fd, add, source, group, iface, v6) → 0 | errno name.
// Source-specific multicast: IP_ADD_SOURCE_MEMBERSHIP for v4,
// MCAST_JOIN_SOURCE_GROUP for v6 (which has no ipv6_mreq_source).
JSValueRef dgramn_srcmembership_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                   const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "dgram: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const bool add{argc > 1 && JSValueToBoolean(ctx, args[1])};
    const std::string source{fs_arg(ctx, argc, args, 2)};
    const std::string group{fs_arg(ctx, argc, args, 3)};
    const std::string iface{fs_arg(ctx, argc, args, 4)};
    const bool v6{argc > 5 && JSValueToBoolean(ctx, args[5])};
#if defined(MCAST_JOIN_SOURCE_GROUP)
    if (v6) {
        group_source_req req{};
        auto* grp{reinterpret_cast<sockaddr_in6*>(&req.gsr_group)};
        auto* src{reinterpret_cast<sockaddr_in6*>(&req.gsr_source)};
        grp->sin6_family = AF_INET6;
        src->sin6_family = AF_INET6;
        if (inet_pton(AF_INET6, group.c_str(), &grp->sin6_addr) != 1) return dgram_err(ctx, EINVAL);
        if (inet_pton(AF_INET6, source.c_str(), &src->sin6_addr) != 1) return dgram_err(ctx, EINVAL);
        if (!iface.empty()) {
            std::string h{iface};
            const auto pct{h.find('%')};
            if (pct != std::string::npos) h = h.substr(pct + 1);
            unsigned idx{if_nametoindex(h.c_str())};
            if (idx == 0) idx = static_cast<unsigned>(std::strtoul(h.c_str(), nullptr, 10));
            req.gsr_interface = idx;
        }
        if (setsockopt(fd, IPPROTO_IPV6,
                       add ? MCAST_JOIN_SOURCE_GROUP : MCAST_LEAVE_SOURCE_GROUP,
                       &req, sizeof req) != 0) {
            return dgram_err(ctx, errno);
        }
        return JSValueMakeNumber(ctx, 0);
    }
#else
    if (v6) return dgram_err(ctx, ENOPROTOOPT);
#endif
#if defined(IP_ADD_SOURCE_MEMBERSHIP)
    ip_mreq_source mreq{};
    if (inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr) != 1) return dgram_err(ctx, EINVAL);
    if (inet_pton(AF_INET, source.c_str(), &mreq.imr_sourceaddr) != 1) return dgram_err(ctx, EINVAL);
    if (!iface.empty()) {
        if (inet_pton(AF_INET, iface.c_str(), &mreq.imr_interface) != 1) return dgram_err(ctx, EINVAL);
    } else {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    }
    if (setsockopt(fd, IPPROTO_IP, add ? IP_ADD_SOURCE_MEMBERSHIP : IP_DROP_SOURCE_MEMBERSHIP,
                   &mreq, sizeof mreq) != 0) {
        return dgram_err(ctx, errno);
    }
    return JSValueMakeNumber(ctx, 0);
#else
    return dgram_err(ctx, ENOPROTOOPT);
#endif
#endif
}

JSValueRef dgramn_close_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef*) {
#if !defined(_WIN32)
    if (argc > 0) {
        const int fd{static_cast<int>(JSValueToNumber(ctx, args[0], nullptr))};
        net_fds().erase(fd);
        close(fd);
    }
#endif
    return JSValueMakeUndefined(ctx);
}

// dgram.setBroadcast(fd, flag) / setTTL / setMulticastTTL / setMulticastLoopback
// → the value | errno name.
JSValueRef dgramn_setopt_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "dgram: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const std::string opt{fs_arg(ctx, argc, args, 1)};
    const int val{argc > 2 ? static_cast<int>(JSValueToNumber(ctx, args[2], nullptr)) : 0};
    const bool v6{argc > 3 && JSValueToBoolean(ctx, args[3])};
    int rc{0};
    if (opt == "broadcast") { rc = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &val, sizeof val); }
    else if (opt == "ttl") {
        rc = v6 ? setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &val, sizeof val)
                : setsockopt(fd, IPPROTO_IP, IP_TTL, &val, sizeof val);
    } else if (opt == "multicastTTL") {
        rc = v6 ? setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &val, sizeof val)
                : setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &val, sizeof val);
    } else if (opt == "multicastLoopback") {
        rc = v6 ? setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &val, sizeof val)
                : setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &val, sizeof val);
    } else {
        return dgram_err(ctx, ENOPROTOOPT);
    }
    if (rc != 0) return dgram_err(ctx, errno);
    return JSValueMakeNumber(ctx, val);
#endif
}

// dgram.membership(fd, add?, group, iface, v6?) → 0 | errno name.
JSValueRef dgramn_membership_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "dgram: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    const bool add{argc > 1 && JSValueToBoolean(ctx, args[1])};
    const std::string group{fs_arg(ctx, argc, args, 2)};
    const std::string iface{fs_arg(ctx, argc, args, 3)};
    const bool v6{argc > 4 && JSValueToBoolean(ctx, args[4])};
    int rc{0};
    if (v6) {
        ipv6_mreq mreq{};
        if (inet_pton(AF_INET6, group.c_str(), &mreq.ipv6mr_multiaddr) != 1) {
            return dgram_err(ctx, EINVAL);
        }
        unsigned idx{0};
        if (!iface.empty()) {
            std::string h{iface};
            const auto pct{h.find('%')};  // "::%lo" → interface "lo"
            if (pct != std::string::npos) h = h.substr(pct + 1);
            idx = if_nametoindex(h.c_str());
            if (idx == 0) idx = static_cast<unsigned>(std::strtoul(h.c_str(), nullptr, 10));
        }
        mreq.ipv6mr_interface = idx;
        rc = setsockopt(fd, IPPROTO_IPV6, add ? IPV6_ADD_MEMBERSHIP : IPV6_DROP_MEMBERSHIP,
                        &mreq, sizeof mreq);
    } else {
        ip_mreq mreq{};
        if (inet_pton(AF_INET, group.c_str(), &mreq.imr_multiaddr) != 1) {
            return dgram_err(ctx, EINVAL);
        }
        if (!iface.empty()) {
            if (inet_pton(AF_INET, iface.c_str(), &mreq.imr_interface) != 1) {
                return dgram_err(ctx, EINVAL);
            }
        } else {
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        }
        rc = setsockopt(fd, IPPROTO_IP, add ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                        &mreq, sizeof mreq);
    }
    if (rc != 0) return dgram_err(ctx, errno);
    return JSValueMakeNumber(ctx, 0);
#endif
}

// install_dgram_native(ctx, global): shape __mbunDgramNative.
inline void install_dgram_native(JSContextRef ctx, JSObjectRef global) {
    JSObjectRef dn{JSObjectMake(ctx, nullptr, nullptr)};
    set_fn(ctx, dn, "create", dgramn_create_cb);
    set_fn(ctx, dn, "bind", dgramn_bind_cb);
    set_fn(ctx, dn, "send", dgramn_send_cb);
    set_fn(ctx, dn, "recv", dgramn_recv_cb);
    set_fn(ctx, dn, "address", dgramn_address_cb);
    set_fn(ctx, dn, "close", dgramn_close_cb);
    set_fn(ctx, dn, "setopt", dgramn_setopt_cb);
    set_fn(ctx, dn, "membership", dgramn_membership_cb);
    set_fn(ctx, dn, "open", dgramn_open_cb);
    set_fn(ctx, dn, "handletype", dgramn_handletype_cb);
    set_fn(ctx, dn, "peername", dgramn_peername_cb);
    set_fn(ctx, dn, "connect", dgramn_connect_cb);
    set_fn(ctx, dn, "disconnect", dgramn_disconnect_cb);
    set_fn(ctx, dn, "bufsize", dgramn_bufsize_cb);
    set_fn(ctx, dn, "mcastiface", dgramn_mcastiface_cb);
    set_fn(ctx, dn, "srcmembership", dgramn_srcmembership_cb);
    set_prop(ctx, global, "__mbunDgramNative", dn);
}
