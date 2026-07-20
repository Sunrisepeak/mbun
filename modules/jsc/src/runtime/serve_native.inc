// ── __mbunServeNative: Bun.serve over the native epoll event-loop stack ─────
// T-LOOP.2 bridge: HTTP/1.1 parsing/framing/keep-alive/backpressure run in
// mbun.runtime_server.Http1Server (async deferred-response mode) on the shared
// mbun.event_loop epoll backend; the JS layer (js_net.cppm Bun.serve) receives
// parsed requests by id, runs the fetch handler, and answers with raw
// pre-framed response bytes (its serializer owns Content-Length/chunked/
// Connection semantics, mirroring bun's RequestContext render path).
//
// Blueprint: bun src/runtime/server/{mod.rs,RequestContext.rs} — uWS parses on
// the loop, on_request hands a context to JS, the Promise resolution renders
// later; src/uws_sys/Loop.rs drives everything from one epoll loop. All calls
// here originate on the JS thread (the engine pump), so no locking is needed.
// Bytes cross the JS boundary as base64 (same rationale as __mbunNetNative).
// Linux-only: install_serve_native is a no-op elsewhere, so the JS layer's
// `__mbunServeNative` feature-detect falls back to the portable poll reactor.
#if defined(__linux__)

struct ServeNativeEvent {
    enum class Kind : std::uint8_t { request, body, aborted };
    Kind kind{Kind::request};
    int serverId{0};
    std::uint64_t requestId{0};
    mbun::runtime_server::Http1Request request{};  // Kind::request only
    std::string chunk{};                           // Kind::body only
    bool done{false};                              // Kind::body only
};

struct ServeNativeState {
    mbun::event_loop::EpollBackend epoll{};
    mbun::event_loop::EventLoop loop;
    std::unordered_map<int, std::unique_ptr<mbun::runtime_server::Http1Server>> servers{};
    std::deque<ServeNativeEvent> events{};
    int nextServerId{1};
    ServeNativeState() : loop{epoll.seam()} {}
};

// Main-thread-only singleton (JSC C API contract; matches net_fds()).
ServeNativeState& serve_native_state() {
    static ServeNativeState gState;
    // Join the reactor's epoll fd to the shared poll set (net.inc net_fds):
    // it reads as ready while events are queued, so the pump's idle poll
    // (NN.wait) wakes instantly for Bun.serve traffic and the idle tick can
    // stay long without costing request latency.
    static const bool joinedPollSet{[] {
        if (gState.epoll.backend_fd() >= 0) net_fds().insert(gState.epoll.backend_fd());
        return true;
    }()};
    (void)joinedPollSet;
    return gState;
}

mbun::runtime_server::Http1Server* serve_native_server(JSContextRef ctx, std::size_t argc,
                                                       const JSValueRef args[]) {
    const int id{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : 0};
    auto& st{serve_native_state()};
    const auto found{st.servers.find(id)};
    return found == st.servers.end() ? nullptr : found->second.get();
}

// serve.listen(host, port) → { id, port }. Hostname normalization mirrors
// net_parse_addr: loopback aliases → 127.0.0.1, any aliases → 0.0.0.0
// (Address::ipv4 wants a numeric address for inet_pton).
JSValueRef serven_listen_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef* exc) {
    std::string host{fs_arg(ctx, argc, args, 0)};
    const auto port{static_cast<std::uint16_t>(
        argc > 1 ? JSValueToNumber(ctx, args[1], nullptr) : 0)};
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);  // "[::1]" → "::1"
    }
    if (host.empty() || host == "localhost") host = "127.0.0.1";
    if (host == "*") host = "0.0.0.0";
    // IPv6 hosts ("::", "::1", …) pass through — Http1Server::listen binds
    // AF_INET6 for any host containing ':' (Linux dual-stack covers v4 peers).
    auto& st{serve_native_state()};
    const int id{st.nextServerId++};
    auto server{std::make_unique<mbun::runtime_server::Http1Server>(st.loop, st.epoll)};
    server->set_async_handler(
        [&st, id](mbun::runtime_server::Http1RequestId rid,
                  mbun::runtime_server::Http1Request&& req) {
            st.events.push_back(ServeNativeEvent{.kind = ServeNativeEvent::Kind::request,
                                                 .serverId = id,
                                                 .requestId = rid,
                                                 .request = std::move(req)});
        },
        [&st, id](mbun::runtime_server::Http1RequestId rid) {
            st.events.push_back(ServeNativeEvent{.kind = ServeNativeEvent::Kind::aborted,
                                                 .serverId = id,
                                                 .requestId = rid});
        },
        // Streaming request bodies (dispatch-on-headers): chunk events follow
        // the request event; (empty, done=true) closes the JS body stream.
        [&st, id](mbun::runtime_server::Http1RequestId rid, std::string_view chunk,
                  bool done) {
            st.events.push_back(ServeNativeEvent{.kind = ServeNativeEvent::Kind::body,
                                                 .serverId = id,
                                                 .requestId = rid,
                                                 .chunk = std::string{chunk},
                                                 .done = done});
        });
    auto bound{server->listen(host, port, {})};
    if (!bound) {
        return fs_throw(ctx, exc,
                        "serve.listen: " + bound.error().message + " (" + host + ":" +
                            std::to_string(port) + ")");
    }
    st.servers.emplace(id, std::move(server));
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, r, "id", JSValueMakeNumber(ctx, id));
    set_prop(ctx, r, "port", JSValueMakeNumber(ctx, *bound));
    return r;
}

// serve.tick(ms) → queued-event count. One epoll pass on the shared loop; a
// throwaway timer caps the wait so an idle loop never blocks the JS pump.
JSValueRef serven_tick_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef*) {
    int ms{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : 0};
    if (ms < 0) ms = 0;
    if (ms > 50) ms = 50;
    auto& st{serve_native_state()};
    const auto now{std::chrono::steady_clock::now()};
    static_cast<void>(st.loop.set_timeout(std::chrono::milliseconds{ms}, [] {}, now));
    static_cast<void>(st.loop.run_once(std::chrono::steady_clock::now()));
    return JSValueMakeNumber(ctx, static_cast<double>(st.events.size()));
}

// serve.next() → next queued event or null.
//   { type: "request", server, id, method, path, minor, headers: [k,v,...], body: b64 }
//   { type: "aborted", server, id }
JSValueRef serven_next_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t,
                          const JSValueRef[], JSValueRef*) {
    auto& st{serve_native_state()};
    if (st.events.empty()) return JSValueMakeNull(ctx);
    ServeNativeEvent ev{std::move(st.events.front())};
    st.events.pop_front();
    JSObjectRef r{JSObjectMake(ctx, nullptr, nullptr)};
    set_prop(ctx, r, "server", JSValueMakeNumber(ctx, ev.serverId));
    set_prop(ctx, r, "id", JSValueMakeNumber(ctx, static_cast<double>(ev.requestId)));
    if (ev.kind == ServeNativeEvent::Kind::aborted) {
        set_prop(ctx, r, "type", make_string(ctx, "aborted"));
        return r;
    }
    if (ev.kind == ServeNativeEvent::Kind::body) {
        set_prop(ctx, r, "type", make_string(ctx, "body"));
        set_prop(ctx, r, "done", JSValueMakeBoolean(ctx, ev.done));
        set_prop(ctx, r, "chunk",
                 make_string(ctx, ev.chunk.empty()
                                      ? std::string{}
                                      : b64_encode(reinterpret_cast<const unsigned char*>(
                                                       ev.chunk.data()),
                                                   ev.chunk.size())));
        return r;
    }
    set_prop(ctx, r, "type", make_string(ctx, "request"));
    set_prop(ctx, r, "method", make_string(ctx, ev.request.method));
    // Keep the raw path bytes as latin1: the JS route matcher's decodeParam
    // percent-decodes then does a lossy UTF-8 decode itself (js_net.cppm:1058) —
    // make_string would UTF-8-decode first and double-decode multibyte params.
    set_prop(ctx, r, "path", make_latin1_string(ctx, ev.request.path));
    set_prop(ctx, r, "minor", JSValueMakeNumber(ctx, ev.request.minorVersion));
    // Peer endpoint for requestIP()/ws.remoteAddress. Absent (not null) when the
    // socket has no reportable peer, so the JS side's `?? null` yields bun's
    // null rather than a fabricated 127.0.0.1. ref: bun RequestContext's
    // getRemoteSocketInfo → us_socket_remote_address.
    if (ev.request.remote) {
        set_prop(ctx, r, "remoteAddress", make_string(ctx, ev.request.remote->address));
        set_prop(ctx, r, "remotePort", JSValueMakeNumber(ctx, ev.request.remote->port));
        set_prop(ctx, r, "remoteFamily",
                 make_string(ctx, ev.request.remote->ipv6 ? "IPv6" : "IPv4"));
    }
    std::vector<JSValueRef> flat;
    flat.reserve(ev.request.headers.size() * 2);
    for (const auto& [k, v] : ev.request.headers) {
        // header names/values are ByteStrings (Latin-1) — bytes ≥ 0x80 must
        // survive, not corrupt the string via UTF-8 decode (regression 08893).
        flat.push_back(make_latin1_string(ctx, k));
        flat.push_back(make_latin1_string(ctx, v));
    }
    set_prop(ctx, r, "headers",
             JSObjectMakeArray(ctx, flat.size(), flat.empty() ? nullptr : flat.data(), nullptr));
    set_prop(ctx, r, "body",
             make_string(ctx, ev.request.body.empty()
                                  ? std::string{}
                                  : b64_encode(reinterpret_cast<const unsigned char*>(
                                                   ev.request.body.data()),
                                               ev.request.body.size())));
    return r;
}

// serve.write(server, reqId, b64) → bool (raw pre-framed response bytes).
JSValueRef serven_write_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef*) {
    auto* server{serve_native_server(ctx, argc, args)};
    if (server == nullptr || argc < 3) return JSValueMakeBoolean(ctx, false);
    const auto rid{static_cast<std::uint64_t>(JSValueToNumber(ctx, args[1], nullptr))};
    // A Uint8Array body is read in place; base64 costs ~6x the payload in
    // transient JS strings per response (a 36 MB body served 1000x by
    // fetch-leak was OOM-killed). A string argument stays base64.
    const std::uint8_t* vp{nullptr};
    std::size_t vn{0};
    if (arg_bytes_view(ctx, argc, args, 2, vp, vn)) {
        std::string bytes;
        if (vn != 0) bytes.assign(reinterpret_cast<const char*>(vp), vn);
        return JSValueMakeBoolean(ctx, server->write_raw(rid, bytes));
    }
    const std::string data{b64_decode(fs_arg(ctx, argc, args, 2))};
    return JSValueMakeBoolean(ctx, server->write_raw(rid, data));
}

// serve.finish(server, reqId, keepAlive) → bool (recycle or flush+close).
JSValueRef serven_finish_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef*) {
    auto* server{serve_native_server(ctx, argc, args)};
    if (server == nullptr || argc < 2) return JSValueMakeBoolean(ctx, false);
    const auto rid{static_cast<std::uint64_t>(JSValueToNumber(ctx, args[1], nullptr))};
    const bool keepAlive{argc > 2 && JSValueToBoolean(ctx, args[2])};
    return JSValueMakeBoolean(ctx, server->finish_raw(rid, keepAlive));
}

// serve.detach(server, reqId) → bool: WebSocket upgrade takeover — the conn
// leaves HTTP framing; inbound bytes keep flowing as body events, outbound
// frames go through serve.write, teardown through serve.abort.
JSValueRef serven_detach_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                            const JSValueRef args[], JSValueRef*) {
    auto* server{serve_native_server(ctx, argc, args)};
    if (server == nullptr || argc < 2) return JSValueMakeBoolean(ctx, false);
    const auto rid{static_cast<std::uint64_t>(JSValueToNumber(ctx, args[1], nullptr))};
    return JSValueMakeBoolean(ctx, server->detach_raw(rid));
}

// serve.abort(server, reqId): hard-close the request's connection.
JSValueRef serven_abort_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                           const JSValueRef args[], JSValueRef*) {
    auto* server{serve_native_server(ctx, argc, args)};
    if (server != nullptr && argc >= 2) {
        server->abort_raw(static_cast<std::uint64_t>(JSValueToNumber(ctx, args[1], nullptr)));
    }
    return JSValueMakeUndefined(ctx);
}

// serve.backpressure(server, reqId) → buffered response bytes (desiredSize).
JSValueRef serven_backpressure_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                  const JSValueRef args[], JSValueRef*) {
    auto* server{serve_native_server(ctx, argc, args)};
    if (server == nullptr || argc < 2) return JSValueMakeNumber(ctx, 0);
    const auto rid{static_cast<std::uint64_t>(JSValueToNumber(ctx, args[1], nullptr))};
    return JSValueMakeNumber(ctx, static_cast<double>(server->response_backpressure(rid)));
}

// serve.stop(server, force): force closes everything and frees the server;
// soft stops the listener and reaps idle keep-alive connections (in-flight
// requests keep their server alive until they finish).
// Every server shares one `st.loop`, and a server's socket backend both holds
// epoll registrations and posts closures that capture it. Destroying it with
// work still queued leaves those closures pointing at freed memory; they then
// fire on some LATER tick, so the crash lands far from its cause (a ~1-in-6
// SIGSEGV in js/web/fetch/fetch-redirect once the shared reactor stopped
// ticking continuously). Run the loop to quiescence first -- bounded, because
// a peer that keeps sending must not turn stop() into an unbounded drain.
void drain_before_destroy_(ServeNativeState& st) {
    for (int round{0}; round < 8; ++round) {
        // A zero-length timer bounds the wait, exactly as serven_tick_cb does:
        // run_once() blocks until the loop has something to do, and a server
        // with registered-but-idle watches has nothing — without this the drain
        // turned stop() into a hang (js/web/fetch/fetch-redirect, issue 22353).
        const auto now{std::chrono::steady_clock::now()};
        static_cast<void>(st.loop.set_timeout(std::chrono::milliseconds{0}, [] {}, now));
        if (st.loop.run_once(std::chrono::steady_clock::now()) == 0) return;
    }
}

JSValueRef serven_stop_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                          const JSValueRef args[], JSValueRef*) {
    const int id{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : 0};
    const bool force{argc > 1 && JSValueToBoolean(ctx, args[1])};
    auto& st{serve_native_state()};
    const auto found{st.servers.find(id)};
    if (found == st.servers.end()) return JSValueMakeUndefined(ctx);
    if (force) {
        found->second->close();
        drain_before_destroy_(st);
        st.servers.erase(found);
    } else {
        // ref bun src/runtime/server/mod.rs:1551 — a NON-abrupt stop closes only
        // the listen socket; established connections keep being served (only the
        // abrupt branch at :1560 closes the app, i.e. every connection). Closing
        // idle sockets here would be `server.closeIdleConnections()`, which bun
        // exposes as a SEPARATE method (server_body.rs:2483), and it is exactly
        // what `stop(false)` must not do: a client holding a pooled keep-alive
        // connection can still reach the server afterwards.
        found->second->stop_listening();
        if (found->second->pending_requests() == 0 && found->second->connection_count() == 0) {
            drain_before_destroy_(st);
            st.servers.erase(found);
        }
    }
    return JSValueMakeUndefined(ctx);
}

// serve.connections(id) → established connection count (in-flight or parked on
// keep-alive), so the JS layer can keep a soft-stopped server's dispatch entry
// registered while any client can still reach it.
JSValueRef serven_connections_cb(JSContextRef ctx, JSObjectRef, JSObjectRef, std::size_t argc,
                                 const JSValueRef args[], JSValueRef*) {
    const int id{argc > 0 ? static_cast<int>(JSValueToNumber(ctx, args[0], nullptr)) : 0};
    auto& st{serve_native_state()};
    const auto found{st.servers.find(id)};
    if (found == st.servers.end()) return JSValueMakeNumber(ctx, 0);
    return JSValueMakeNumber(ctx, static_cast<double>(found->second->connection_count()));
}

void install_serve_native(JSContextRef ctx, JSObjectRef global) {
    JSObjectRef sn{JSObjectMake(ctx, nullptr, nullptr)};
    set_fn(ctx, sn, "listen", serven_listen_cb);
    set_fn(ctx, sn, "tick", serven_tick_cb);
    set_fn(ctx, sn, "next", serven_next_cb);
    set_fn(ctx, sn, "write", serven_write_cb);
    set_fn(ctx, sn, "finish", serven_finish_cb);
    set_fn(ctx, sn, "detach", serven_detach_cb);
    set_fn(ctx, sn, "abort", serven_abort_cb);
    set_fn(ctx, sn, "backpressure", serven_backpressure_cb);
    set_fn(ctx, sn, "stop", serven_stop_cb);
    set_fn(ctx, sn, "connections", serven_connections_cb);
    set_prop(ctx, global, "__mbunServeNative", sn);
}

#else  // !__linux__

// DEFERRED honestly: no __mbunServeNative — Bun.serve keeps the portable
// poll-reactor path (js_net.cppm feature-detects the global).
void install_serve_native(JSContextRef, JSObjectRef) {}

#endif
