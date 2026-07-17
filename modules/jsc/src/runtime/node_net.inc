// ── __mbunDgramNative: non-blocking POSIX UDP primitives (node:dgram) ────────
// Companion to net.inc's TCP layer. Same single-threaded, O_NONBLOCK design:
// every datagram socket is non-blocking and the JS dgram reactor (node_net.cppm)
// drains readable datagrams from the event-loop pump via __mbunNetDrain. Bytes
// cross the JS boundary as base64 (the JSC C string path is UTF-8). POSIX only;
// Windows is DEFERRED honestly (the JS layer throws when the native is absent).
#if !defined(_WIN32)

// Parse host+port into a sockaddr_storage for the requested family. "" / "0.0.0.0"
// / "::" map to the wildcard; "localhost" maps to loopback. Returns false on a
// malformed literal (the JS layer already validated hostnames).
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
        if (host == "localhost") { a6->sin6_addr = in6addr_loopback; return true; }
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
    if (host == "localhost") { a4->sin_addr.s_addr = htonl(INADDR_LOOPBACK); return true; }
    return inet_pton(AF_INET, host.c_str(), &a4->sin_addr) == 1;
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

// dgram.create(type) → fd (type "udp4"|"udp6"). Socket is O_NONBLOCK.
JSValueRef dgramn_create_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "dgram: not supported on Windows yet");
#else
    const std::string type{fs_arg(ctx, argc, args, 0)};
    const bool v6{type == "udp6"};
    const int fd{socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0)};
    if (fd < 0) return fs_throw(ctx, exc, std::string{"dgram: socket() failed ("} + std::strerror(errno) + ")");
    const int one{1};
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    net_set_nonblock(fd);
    return JSValueMakeNumber(ctx, fd);
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
    if (!dgram_parse_addr(host, port, v6, ss, len)) {
        return fs_throw(ctx, exc, "dgram.bind: invalid address '" + host + "'");
    }
    if (v6 && argc > 4 && JSValueToBoolean(ctx, args[4])) {
        const int on{1};
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof on);
    }
    if (bind(fd, reinterpret_cast<sockaddr*>(&ss), len) != 0) {
        return fs_throw(ctx, exc, std::string{"dgram.bind: "} + std::strerror(errno));
    }
    sockaddr_storage bound{};
    socklen_t blen{sizeof bound};
    getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen);
    return dgram_addr_info(ctx, bound);
#endif
}

// dgram.send(fd, b64, port, host, v6?) → bytes sent.
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
    sockaddr_storage ss{};
    socklen_t len{};
    if (!dgram_parse_addr(host.empty() ? (v6 ? "::1" : "127.0.0.1") : host, port, v6, ss, len)) {
        return fs_throw(ctx, exc, "dgram.send: invalid address '" + host + "'");
    }
    ssize_t n;
    do { n = sendto(fd, data.data(), data.size(), MSG_NOSIGNAL,
                    reinterpret_cast<sockaddr*>(&ss), len); }
    while (n < 0 && errno == EINTR);
    if (n < 0) return fs_throw(ctx, exc, std::string{"dgram.send: "} + std::strerror(errno));
    return JSValueMakeNumber(ctx, static_cast<double>(n));
#endif
}

// dgram.recv(fd) → { data(b64), address, port, family } | null (would block).
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
        return fs_throw(ctx, exc, std::string{"dgram.recv: "} + std::strerror(errno));
    }
    JSObjectRef r{dgram_addr_info(ctx, from)};
    set_prop(ctx, r, "data", make_string(ctx, b64_encode(reinterpret_cast<const unsigned char*>(buf),
                                                         static_cast<std::size_t>(n))));
    set_prop(ctx, r, "size", JSValueMakeNumber(ctx, static_cast<double>(n)));
    return r;
#endif
}

// dgram.address(fd) → { address, port, family }.
JSValueRef dgramn_address_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                             const JSValueRef args[], JSValueRef* exc) {
#if defined(_WIN32)
    return fs_throw(ctx, exc, "dgram: not supported on Windows yet");
#else
    const int fd{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : -1};
    sockaddr_storage ss{};
    socklen_t len{sizeof ss};
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
        return fs_throw(ctx, exc, std::string{"dgram.address: "} + std::strerror(errno));
    }
    return dgram_addr_info(ctx, ss);
#endif
}

JSValueRef dgramn_close_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef*) {
#if !defined(_WIN32)
    if (argc > 0) close(static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)));
#endif
    return JSValueMakeUndefined(ctx);
}

// dgram.setBroadcast(fd, flag) / setTTL / setMulticastTTL / setMulticastLoopback.
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
    }
    if (rc != 0) return fs_throw(ctx, exc, std::string{"dgram.setopt: "} + std::strerror(errno));
    return JSValueMakeNumber(ctx, val);
#endif
}

// dgram.membership(fd, add?, group, iface, v6?) → throws on invalid group/iface.
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
            return fs_throw(ctx, exc, "dgram: invalid multicast address '" + group + "'");
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
            return fs_throw(ctx, exc, "dgram: invalid multicast address '" + group + "'");
        }
        if (!iface.empty()) inet_pton(AF_INET, iface.c_str(), &mreq.imr_interface);
        else mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        rc = setsockopt(fd, IPPROTO_IP, add ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                        &mreq, sizeof mreq);
    }
    if (rc != 0) return fs_throw(ctx, exc, std::string{"dgram.membership: "} + std::strerror(errno));
    return JSValueMakeUndefined(ctx);
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
    set_prop(ctx, global, "__mbunDgramNative", dn);
}
