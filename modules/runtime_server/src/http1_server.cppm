// HTTP/1.1 server tying the epoll socket stack to the runtime_server seams.
//
// PORT-SOURCE:
//   bun Rust src/runtime/server/{mod.rs,RequestContext.rs}: uWS parses the
//   request head, on_request builds a RequestContext, the body is buffered
//   into request_body_buf while is_waiting_for_request_body, the handler's
//   response is written via render_metadata + end(data, close_connection),
//   and uWS recycles the socket onto the next keep-alive request. Here the
//   uWS C++ half (HTTP/1 framing, keep-alive recycling, response metadata)
//   is done with mbun.http parse_request/decode_chunked over the
//   mbun.runtime_socket epoll backend; write backpressure rides the
//   EpollSocketBackend stream buffer (us_socket_stream_buffer_t equivalent),
//   mirroring RequestContext.on_writable_response_buffer's drain-then-finish.
//
// Platform: all fd/epoll specifics live in EpollSocketBackend, so this module
// is platform-neutral source; on non-Linux the backend's honest DEFERRED stub
// makes listen() fail cleanly and no connection ever surfaces.
export module mbun.runtime_server.http1_server;

import std;
import mbun.event_loop;
import mbun.runtime_socket;
import mbun.http;
import mbun.runtime_server.request_dispatch;
import mbun.runtime_server.response_backend;

export namespace mbun::runtime_server {

// Minimal ServerConfig subset: the handler sees the buffered request and
// returns a full response (streaming bodies are DEFERRED).
struct Http1Request {
    std::string method {};
    std::string path {};
    std::vector<std::pair<std::string, std::string>> headers {};
    std::string body {};
    unsigned minorVersion { 1 };
    // Accepted peer, captured once at accept and stamped onto every request the
    // connection carries (keep-alive included). ref: bun
    // src/runtime/server/RequestContext.rs — requestIP()/ws.remoteAddress read
    // the uWS socket's remote address, so pipelined/keep-alive requests on one
    // socket all report the same peer. nullopt = no peer (unix socket, or the
    // connection went away before we asked) → JS surfaces null, never a fake IP.
    std::optional<runtime_socket::PeerAddress> remote {};

    [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const {
        for (const auto& [key, value] : headers) {
            if (key.size() == name.size()
                && std::ranges::equal(key, name, [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a))
                              == std::tolower(static_cast<unsigned char>(b));
                   }))
                return value;
        }
        return std::nullopt;
    }
};

struct Http1Response {
    std::uint16_t status { 200 };
    std::vector<std::pair<std::string, std::string>> headers {};
    std::string body {};
};

using Http1Handler = std::function<Http1Response(const Http1Request&)>;

// Async (deferred-response) mode identifiers + callbacks. Blueprint: bun
// src/runtime/server/RequestContext.rs — a JS fetch handler returns a Promise,
// so the RequestContext outlives on_request and the response is rendered later
// via resolved-promise continuations; here the consumer (the JSC bridge) gets
// a request id and answers with raw pre-framed HTTP/1.1 bytes when ready.
using Http1RequestId = std::uint64_t;
using Http1AsyncHandler = std::function<void(Http1RequestId, Http1Request&&)>;
// Fired when the client disconnects while a request is still awaiting its
// response (RequestContext.on_abort — cancels streaming bodies in the handler).
using Http1AbortHandler = std::function<void(Http1RequestId)>;
// Streaming request bodies (bun: onRequest fires at header-complete and the
// body streams into request_body_buf while is_waiting_for_request_body):
// chunk deliveries end with a final (chunk="", done=true) call. When set, the
// async handler dispatches on headers with Http1Request::body empty.
using Http1BodyHandler = std::function<void(Http1RequestId, std::string_view, bool)>;

// ref: bun runtime/server/HTTPStatusText.rs (subset used by this server).
constexpr std::string_view http1_status_text(std::uint16_t status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

class Http1Server {
private:
    // uws AppResponse equivalent, corked: head and body are serialized into
    // the connection's outbox, which the server uncorks as a single write per
    // data event (uWS corks for the whole loop iteration, so a pipelined
    // burst flushes together). Besides saving syscalls, this is a correctness
    // matter for small responses — a head send followed by a separate tiny
    // body send parks that segment behind Nagle + delayed ACK for ~40ms.
    class CorkedResponseBackend_ final : public ResponseBackend {
    private:
        std::string& outbox_;

    public:
        explicit CorkedResponseBackend_(std::string& outbox)
            : outbox_ { outbox }
        {
        }

        bool write_head(const ResponseHead& head) override {
            outbox_ += "HTTP/1.1 ";
            outbox_ += std::to_string(head.status);
            outbox_ += ' ';
            outbox_ += http1_status_text(head.status);
            outbox_ += "\r\n";
            for (const auto& [name, value] : head.headers) {
                outbox_ += name;
                outbox_ += ": ";
                outbox_ += value;
                outbox_ += "\r\n";
            }
            outbox_ += "\r\n";
            return true;
        }

        bool write_body(std::string_view bytes) override {
            outbox_ += bytes;
            return true;
        }

        void end() override {}  // Uncork happens in Http1Server::flush_outbox_.
        void abort() override { outbox_.clear(); }
    };

    enum class Phase : std::uint8_t { headers, fixedBody, chunkedBody, awaitingResponse,
                                      rawTunnel };

    // RequestContext equivalent, reset in place on keep-alive reuse.
    struct Conn_ {
        std::string inbox {};
        std::string outbox {};  // corked response bytes, flushed per data event
        Phase phase { Phase::headers };
        Http1Request request {};
        std::size_t bodyRemaining { 0 };
        http::ChunkedDecoder chunkDecoder {};
        bool keepAlive { true };
        bool closeWhenDrained { false };
        Http1RequestId activeRequest { 0 };  // non-zero once dispatched (async)
        bool bodyStreaming { false };  // dispatched on headers, body still inbound
        // Captured at accept, not per request: getpeername on a half-closed fd
        // fails, and `conn.request` is reset per keep-alive request while this
        // is not.
        std::optional<runtime_socket::PeerAddress> peer {};
    };

    static constexpr std::size_t MAX_HEADERS { 100 };

    std::shared_ptr<runtime_socket::EpollSocketBackend> backend_ {};
    std::optional<runtime_socket::NativeHandle> listenerHandle_ {};
    Http1Handler fallback_ {};
    Http1AsyncHandler asyncHandler_ {};
    Http1AbortHandler abortHandler_ {};
    Http1BodyHandler bodyHandler_ {};
    RequestDispatcher dispatcher_ {};
    std::vector<Http1Handler> routeHandlers_ {};
    std::unordered_map<runtime_socket::NativeHandle, Conn_> conns_ {};
    std::unordered_map<Http1RequestId, runtime_socket::NativeHandle> pending_ {};
    Http1RequestId nextRequestId_ { 1 };

public:  // Big Five: callbacks capture `this`, so pin the object.
    Http1Server(event_loop::EventLoop& loop, event_loop::EpollBackend& epoll) {
        runtime_socket::SocketEvents events {};
        events.on_open = [this](runtime_socket::NativeHandle handle, bool isClient) {
            if (!isClient) {
                Conn_ conn {};
                conn.peer = backend_->remote_address(handle);
                conns_.emplace(handle, std::move(conn));
            }
        };
        events.on_data = [this](runtime_socket::NativeHandle handle,
                                std::span<const std::byte> bytes) {
            on_data_(handle, bytes);
        };
        events.on_writable = [this](runtime_socket::NativeHandle handle) {
            on_writable_(handle);
        };
        events.on_close = [this](runtime_socket::NativeHandle handle) { on_remote_close_(handle); };
        backend_ = std::make_shared<runtime_socket::EpollSocketBackend>(loop, epoll,
                                                                        std::move(events));
    }
    Http1Server(const Http1Server&) = delete;
    Http1Server& operator=(const Http1Server&) = delete;
    Http1Server(Http1Server&&) = delete;
    Http1Server& operator=(Http1Server&&) = delete;
    ~Http1Server() = default;  // backend_ teardown closes every live fd.

public:
    // ServerConfig.listen subset: bind + arm the fallback handler. Returns the
    // kernel-assigned port so tests can bind 127.0.0.1:0.
    std::expected<std::uint16_t, runtime_socket::BackendError>
    listen(std::string_view host, std::uint16_t port, Http1Handler handler) {
        fallback_ = std::move(handler);
        // A ':' in the host means a numeric IPv6 address ("::1", "::"), since
        // ipv4 hosts are dotted quads (DNS resolution happens upstream).
        const auto address { host.find(':') != std::string_view::npos
                                 ? runtime_socket::Address::ipv6(std::string { host }, port)
                                 : runtime_socket::Address::ipv4(std::string { host }, port) };
        auto bound { backend_->listen(address) };
        if (!bound)
            return std::unexpected { bound.error() };
        listenerHandle_ = *bound;
        return backend_->local_port(*bound).value_or(port);
    }

    // Arm async (deferred-response) mode: instead of calling a synchronous
    // handler inline, each parsed request is announced with an id and the
    // connection is parked (pipelined bytes wait) until finish_raw/abort_raw.
    void set_async_handler(Http1AsyncHandler onRequest, Http1AbortHandler onAbort = {},
                           Http1BodyHandler onBody = {}) {
        asyncHandler_ = std::move(onRequest);
        abortHandler_ = std::move(onAbort);
        bodyHandler_ = std::move(onBody);  // set → dispatch on headers, stream body
    }

    // Append pre-framed response bytes to the request's connection and uncork.
    // The caller owns HTTP framing (status line/headers/chunked encoding) —
    // uWS Response::write passthrough for the JS serializer.
    bool write_raw(Http1RequestId id, std::string_view bytes) {
        const auto p { pending_.find(id) };
        if (p == pending_.end())
            return false;
        const auto found { conns_.find(p->second) };
        if (found == conns_.end())
            return false;
        found->second.outbox += bytes;
        flush_outbox_(p->second, found->second);
        return true;
    }

    // Response complete: recycle onto the next keep-alive request (replaying
    // pipelined inbox bytes) or flush-then-close (RequestContext end tail).
    bool finish_raw(Http1RequestId id, bool keepAliveWanted) {
        const auto p { pending_.find(id) };
        if (p == pending_.end())
            return false;
        const auto handle { p->second };
        pending_.erase(p);
        const auto found { conns_.find(handle) };
        if (found == conns_.end())
            return false;
        Conn_& conn { found->second };
        conn.activeRequest = 0;
        // Responding before the request body finished arriving leaves framing
        // mid-stream — the connection cannot be recycled (bun closes it too).
        const bool unfinishedBody { conn.bodyStreaming };
        conn.bodyStreaming = false;
        if (unfinishedBody || !(keepAliveWanted && conn.keepAlive)) {
            finish_close_(handle);
            return true;
        }
        conn.phase = Phase::headers;
        conn.request = Http1Request {};
        conn.bodyRemaining = 0;
        conn.chunkDecoder = http::ChunkedDecoder {};
        conn.keepAlive = true;
        process_(handle);  // may dispatch the next pipelined request re-entrantly
        return true;
    }

    // Hard-abort: drop the connection without completing the response (the
    // JS layer's socket.destroy() on a streaming error).
    void abort_raw(Http1RequestId id) {
        const auto p { pending_.find(id) };
        if (p == pending_.end())
            return;
        const auto handle { p->second };
        pending_.erase(p);
        close_now_(handle);
    }

    // WebSocket upgrade takeover (uWS us_socket_context_adopt shape): after
    // the 101 response bytes went out via write_raw, the connection leaves
    // HTTP framing entirely — every subsequent inbound byte forwards through
    // the body handler (done stays false) under the same request id, and the
    // pending entry stays alive for raw frame writes until abort_raw or a
    // remote close (which fires the abort handler like any live request).
    bool detach_raw(Http1RequestId id) {
        const auto p { pending_.find(id) };
        if (p == pending_.end() || !bodyHandler_)
            return false;
        const auto found { conns_.find(p->second) };
        if (found == conns_.end())
            return false;
        Conn_& conn { found->second };
        conn.phase = Phase::rawTunnel;
        conn.bodyStreaming = false;
        conn.keepAlive = false;
        // Bytes already buffered past the request head belong to the tunnel
        // (a client may pipeline frames right behind the handshake).
        if (!conn.inbox.empty()) {
            std::string first { std::move(conn.inbox) };
            conn.inbox.clear();
            bodyHandler_(id, first, false);
        }
        return true;
    }

    // Bytes still buffered behind the request's connection (backpressure gauge
    // for the JS sink's desiredSize; us_socket_stream_buffer_t occupancy).
    [[nodiscard]] std::size_t response_backpressure(Http1RequestId id) const {
        const auto p { pending_.find(id) };
        if (p == pending_.end())
            return 0;
        return backend_->pending_write_bytes(p->second);
    }

    // Soft stop: refuse new connections, let in-flight requests finish.
    void stop_listening() {
        if (listenerHandle_) {
            backend_->close(*listenerHandle_);
            listenerHandle_.reset();
        }
    }

    // Close connections with no request awaiting a response. This is bun's
    // separate `server.closeIdleConnections()` JS API (ref: bun
    // src/runtime/server/server_body.rs:2483 close_idle_connections), NOT part
    // of Server.stop() -- a non-abrupt stop closes only the listen socket and
    // leaves established connections serving (mod.rs:1551).
    void close_idle() {
        std::vector<runtime_socket::NativeHandle> idle {};
        for (const auto& [handle, conn] : conns_) {
            if (conn.activeRequest == 0 && !conn.closeWhenDrained)
                idle.push_back(handle);
        }
        for (const auto handle : idle)
            close_now_(handle);
    }

    [[nodiscard]] std::size_t pending_requests() const { return pending_.size(); }

    // Established connections, in-flight or parked on keep-alive. A soft stop
    // keeps the server alive while any remain (bun defers teardown the same way:
    // deinit_if_we_can, src/runtime/server/mod.rs:1584).
    [[nodiscard]] std::size_t connection_count() const { return conns_.size(); }

    // Static routes compose through the existing RequestDispatcher; later
    // declarations override earlier ones (bun static route rule).
    void add_route(std::string path, HttpMethod method, Http1Handler handler) {
        dispatcher_.add_route(std::move(path), method, routeHandlers_.size());
        routeHandlers_.push_back(std::move(handler));
    }

    void close() {
        // Collect first: backend close mutates conns_ via no callback, but
        // keep iteration and erasure clearly separated anyway.
        std::vector<runtime_socket::NativeHandle> open {};
        open.reserve(conns_.size());
        for (const auto& [handle, conn] : conns_)
            open.push_back(handle);
        for (const auto handle : open)
            backend_->close(handle);
        conns_.clear();
        pending_.clear();
        if (listenerHandle_) {
            backend_->close(*listenerHandle_);
            listenerHandle_.reset();
        }
    }

    [[nodiscard]] const RequestDispatcher& dispatcher() const { return dispatcher_; }
    [[nodiscard]] std::optional<runtime_socket::NativeHandle> listener_handle() const {
        return listenerHandle_;
    }
    [[nodiscard]] std::size_t open_connections() const { return conns_.size(); }
    [[nodiscard]] runtime_socket::EpollSocketBackend& socket_backend() { return *backend_; }

private:
    static bool iequals_(std::string_view a, std::string_view b) {
        return a.size() == b.size()
               && std::ranges::equal(a, b, [](char x, char y) {
                      return std::tolower(static_cast<unsigned char>(x))
                             == std::tolower(static_cast<unsigned char>(y));
                  });
    }

    // Comma-list token scan for Connection / Transfer-Encoding values.
    static bool has_token_ci_(std::string_view value, std::string_view token) {
        std::size_t start { 0 };
        while (start <= value.size()) {
            std::size_t end { value.find(',', start) };
            if (end == std::string_view::npos)
                end = value.size();
            std::string_view item { value.substr(start, end - start) };
            while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
                item.remove_prefix(1);
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
                item.remove_suffix(1);
            if (iequals_(item, token))
                return true;
            start = end + 1;
        }
        return false;
    }

    static std::string_view trim_ows_(std::string_view v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
            v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
            v.remove_suffix(1);
        return v;
    }

    // Every value a field name carries, in header order. Http1Request::header
    // collapses repeats to the first, which hides exactly the conflicts that make a
    // message's framing ambiguous.
    static std::vector<std::string_view> header_values_(const Http1Request& request,
                                                        std::string_view name) {
        std::vector<std::string_view> out {};
        for (const auto& [key, value] : request.headers) {
            if (iequals_(key, name))
                out.emplace_back(trim_ows_(value));
        }
        return out;
    }

    // Is "chunked" the FINAL transfer coding of this Transfer-Encoding value?
    // A trailing coding of anything else (including an unrecognised token) means the
    // content length is undeterminable, which RFC 9112 §6.1 makes an error for a
    // request. Empty list items are tolerated — a comma-separated field value with a
    // trailing separator is still well-formed.
    static bool last_coding_is_chunked_(std::string_view value) {
        std::string_view last {};
        std::size_t start { 0 };
        while (start <= value.size()) {
            std::size_t end { value.find(',', start) };
            if (end == std::string_view::npos)
                end = value.size();
            std::string_view item { value.substr(start, end - start) };
            while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
                item.remove_prefix(1);
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t'))
                item.remove_suffix(1);
            if (!item.empty())
                last = item;
            start = end + 1;
        }
        return iequals_(last, "chunked");
    }

    // How many "chunked" codings the (possibly multi-line) Transfer-Encoding list
    // carries. More than one — "chunked, chunked" — is the double-chunked smuggling
    // vector: chunked is not applied twice, so a recipient that stops at the first
    // and one that stops at the last frame the message differently.
    static std::size_t chunked_coding_count_(std::string_view value) {
        std::size_t n { 0 };
        std::size_t start { 0 };
        while (start <= value.size()) {
            std::size_t end { value.find(',', start) };
            if (end == std::string_view::npos)
                end = value.size();
            if (iequals_(trim_ows_(value.substr(start, end - start)), "chunked"))
                ++n;
            start = end + 1;
        }
        return n;
    }

    void on_data_(runtime_socket::NativeHandle handle, std::span<const std::byte> bytes) {
        const auto found { conns_.find(handle) };
        if (found == conns_.end() || found->second.closeWhenDrained)
            return;  // Late bytes on a connection already marked for close.
        found->second.inbox.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        process_(handle);
    }

    // Deferred half of the close path: uWS's on_writable fires once the
    // stream buffer drains, which is exactly when a close-marked response
    // has fully reached the kernel.
    void on_writable_(runtime_socket::NativeHandle handle) {
        const auto found { conns_.find(handle) };
        if (found != conns_.end() && found->second.closeWhenDrained)
            close_now_(handle);
    }

    void close_now_(runtime_socket::NativeHandle handle) {
        scrub_pending_(handle);
        // Drain-close: half-close (FIN) and discard leftover inbound. A plain
        // close() with unread request bytes (the 431 oversized-header path)
        // makes the kernel RST, and the client observes ECONNRESET instead of
        // the clean EOF "connection: close" semantics promise.
        backend_->close_after_drain(handle);
        conns_.erase(handle);
    }

    // Drop the pending-request entry of a dying connection so later
    // write_raw/finish_raw calls become clean no-ops.
    void scrub_pending_(runtime_socket::NativeHandle handle) {
        const auto found { conns_.find(handle) };
        if (found != conns_.end() && found->second.activeRequest != 0) {
            pending_.erase(found->second.activeRequest);
            found->second.activeRequest = 0;
        }
    }

    // Remote FIN/reset: if a request was still awaiting its response, tell the
    // consumer (RequestContext.on_abort) so streaming handlers can cancel.
    void on_remote_close_(runtime_socket::NativeHandle handle) {
        const auto found { conns_.find(handle) };
        const Http1RequestId aborted {
            found != conns_.end() ? found->second.activeRequest : 0
        };
        scrub_pending_(handle);
        conns_.erase(handle);
        if (aborted != 0 && abortHandler_)
            abortHandler_(aborted);
    }

    // Uncork: hand the accumulated response bytes to the socket backend in
    // one write (the unsent tail rides its stream buffer).
    void flush_outbox_(runtime_socket::NativeHandle handle, Conn_& conn) {
        if (conn.outbox.empty())
            return;
        static_cast<void>(backend_->write(handle, std::as_bytes(std::span { conn.outbox })));
        conn.outbox.clear();
    }

    // end(data, close_connection) tail: uncork, then close immediately when
    // nothing stayed buffered, otherwise wait for the drain notification.
    void finish_close_(runtime_socket::NativeHandle handle) {
        const auto found { conns_.find(handle) };
        if (found != conns_.end())
            flush_outbox_(handle, found->second);
        if (backend_->pending_write_bytes(handle) == 0) {
            close_now_(handle);
            return;
        }
        if (found != conns_.end())
            found->second.closeWhenDrained = true;
    }

    // Serial request pump: headers -> body -> handler -> response, then loop
    // while the inbox still holds pipelined bytes (uWS HTTP/1 pipelining is
    // the same strictly-serial replay).
    void process_(runtime_socket::NativeHandle handle) {
        pump_requests_(handle);
        // Uncork whatever this event produced (close paths already flushed).
        const auto found { conns_.find(handle) };
        if (found != conns_.end())
            flush_outbox_(handle, found->second);
    }

    void pump_requests_(runtime_socket::NativeHandle handle) {
        while (true) {
            const auto found { conns_.find(handle) };
            if (found == conns_.end() || found->second.closeWhenDrained)
                return;
            Conn_& conn { found->second };

            if (conn.phase == Phase::awaitingResponse)
                return;  // parked: pipelined bytes wait for finish_raw
            if (conn.phase == Phase::rawTunnel) {
                // Upgraded (WebSocket) connection: raw passthrough to the
                // consumer; no HTTP framing ever again on this socket.
                if (!conn.inbox.empty() && bodyHandler_) {
                    std::string bytes { std::move(conn.inbox) };
                    conn.inbox.clear();
                    bodyHandler_(conn.activeRequest, bytes, false);
                }
                return;
            }
            if (conn.phase == Phase::headers) {
                if (!parse_headers_(handle, conn))
                    return;  // need more bytes, or the conn was failed+closed
                // parse_headers_ may complete a bodiless request in place.
                continue;
            }
            if (conn.phase == Phase::fixedBody) {
                const std::size_t take { std::min(conn.bodyRemaining, conn.inbox.size()) };
                if (conn.bodyStreaming) {
                    if (take > 0 && bodyHandler_)
                        bodyHandler_(conn.activeRequest,
                                     std::string_view { conn.inbox }.substr(0, take), false);
                } else {
                    conn.request.body.append(conn.inbox, 0, take);
                }
                conn.inbox.erase(0, take);
                conn.bodyRemaining -= take;
                if (conn.bodyRemaining > 0)
                    return;
                if (!body_complete_(handle, conn))
                    return;
                continue;
            }
            // Phase::chunkedBody
            auto decoded { http::decode_chunked(conn.chunkDecoder, conn.inbox) };
            if (conn.bodyStreaming) {
                if (!decoded.decoded.empty() && bodyHandler_)
                    bodyHandler_(conn.activeRequest, decoded.decoded, false);
            } else {
                conn.request.body += decoded.decoded;
            }
            if (decoded.status == http::ParseStatus::Invalid) {
                fail_request_(handle, 400, "invalid chunked body");
                return;
            }
            if (decoded.status == http::ParseStatus::Incomplete) {
                conn.inbox.clear();  // decode_chunked consumed everything
                return;
            }
            std::string trailing { decoded.trailing };  // copy before inbox dies
            conn.inbox = std::move(trailing);
            if (!body_complete_(handle, conn))
                return;
        }
    }

    // Body fully received: streaming mode signals the final body event and
    // parks the connection until finish_raw; buffered mode runs the handler.
    bool body_complete_(runtime_socket::NativeHandle handle, Conn_& conn) {
        if (conn.bodyStreaming) {
            conn.bodyStreaming = false;
            conn.phase = Phase::awaitingResponse;
            if (bodyHandler_)
                bodyHandler_(conn.activeRequest, {}, true);
            return false;  // parked until finish_raw
        }
        return finish_request_(handle, conn);
    }

    // Returns false when the caller must stop pumping (need data / closed).
    // On Ok the request head is copied out of the inbox and the body phase is
    // armed; bodiless requests are finished inline.
    bool parse_headers_(runtime_socket::NativeHandle handle, Conn_& conn) {
        std::array<http::Header, MAX_HEADERS> storage {};
        const auto res { http::parse_request(conn.inbox, storage) };
        // Read per request (not per server): node:http's maxHeaderSize setter
        // must also re-limit servers that are already listening.
        const auto maxHeaderBytes { http::max_http_header_size() };
        if (res.status == http::ParseStatus::Incomplete) {
            // Still short of the terminating CRLF CRLF: whatever is buffered is
            // all header bytes, so an over-limit inbox can never become legal.
            if (conn.inbox.size() > maxHeaderBytes)
                fail_request_(handle, 431, "request header block too large");
            return false;
        }
        if (res.status == http::ParseStatus::Invalid) {
            fail_request_(handle, 400, "malformed request");
            return false;
        }
        // Head arrived whole (one read, or the last read completed it): the
        // Incomplete branch above never saw it, so re-check the parsed size.
        if (res.bytes_read > maxHeaderBytes) {
            fail_request_(handle, 431, "request header block too large");
            return false;
        }

        conn.request = Http1Request {};
        conn.request.method.assign(res.method);
        conn.request.path.assign(res.path);
        conn.request.minorVersion = res.minor_version;
        conn.request.headers.reserve(res.num_headers);
        for (std::size_t i { 0 }; i < res.num_headers; ++i) {
            const auto& h { storage[i] };
            if (h.is_multiline()) {
                // obs-fold. RFC 9112 §5.2 lets a server either reject the message
                // or replace the fold with SP; node's llhttp rejects (400), and so
                // does this server. Folding it instead is the more dangerous half of
                // the choice: an intermediary that rejects and an origin that folds
                // (or vice versa) disagree about where the header block ends, which
                // is a request-smuggling differential.
                fail_request_(handle, 400, "obs-fold in header block");
                return false;
            }
            conn.request.headers.emplace_back(std::string { h.name }, std::string { h.value });
        }

        // HTTP/1.1 defaults to keep-alive; 1.0 must opt in (RFC 7230 §6.3).
        const auto connection { conn.request.header("connection") };
        if (res.minor_version >= 1)
            conn.keepAlive = !(connection && has_token_ci_(*connection, "close"));
        else
            conn.keepAlive = connection && has_token_ci_(*connection, "keep-alive");

        conn.inbox.erase(0, res.bytes_read);

        // ── Body framing ─────────────────────────────────────────────────────
        // RFC 9112 §6.1/§6.3 and node's llhttp both make an AMBIGUOUS framing an
        // unrecoverable error, and for one reason: whenever two HTTP
        // implementations on the same path can pick different answers, the
        // difference is a request-smuggling primitive. This server previously
        // resolved every ambiguity silently and kept the connection alive for
        // pipelining, which is the exact configuration the attack needs. Each
        // rejection below is measured against node:http, which answers 400 to all
        // of them; fail_request_ also closes the connection, so nothing that
        // followed the ambiguous framing is ever parsed as a request.
        const auto teValues { header_values_(conn.request, "transfer-encoding") };
        const auto clValues { header_values_(conn.request, "content-length") };

        // 1. Content-Length AND Transfer-Encoding (CL.TE / TE.CL smuggling).
        //    RFC 9112 §6.1: "A server MAY reject a request that contains both
        //    Content-Length and Transfer-Encoding". llhttp does, unconditionally.
        if (!teValues.empty() && !clValues.empty()) {
            fail_request_(handle, 400, "both Transfer-Encoding and Content-Length");
            return false;
        }
        // 2. CONFLICTING duplicate Content-Length (CL.CL smuggling). header() returns
        //    the FIRST match, so this server framed on "6" while the JS Headers object
        //    — and any front-end — could read "6, 0". RFC 9112 §6.3 permits repeats
        //    only when every copy carries the SAME value, which is the line bun's own
        //    request-smuggling suite draws ("accepts duplicate Content-Length headers
        //    with identical values" vs "rejects conflicting duplicate Content-Length
        //    headers").
        for (std::size_t i { 1 }; i < clValues.size(); ++i) {
            if (clValues[i] != clValues[0]) {
                fail_request_(handle, 400, "conflicting content-length");
                return false;
            }
        }
        // 3. Transfer-Encoding's LAST coding must be "chunked" (RFC 9112 §6.1: "If
        //    any transfer coding other than chunked is applied to a request's
        //    content, the sender MUST apply chunked as the final transfer coding").
        //    Repeated field lines are one list split across lines (RFC 9110 §5.3), so
        //    join them in order before looking at the last coding — "Transfer-Encoding:
        //    gzip" + "Transfer-Encoding: chunked" is legal and must still be served.
        //    What must NOT pass: "chunked, identity", a second "chunked", an empty
        //    value, or an unrecognised coding such as "xchunked" — each left this
        //    server with no framing at all, so it treated the body bytes as the NEXT
        //    pipelined request.
        bool chunked { false };
        if (!teValues.empty()) {
            std::string joined { teValues.front() };
            for (std::size_t i { 1 }; i < teValues.size(); ++i) {
                joined += ',';
                joined += teValues[i];
            }
            if (!last_coding_is_chunked_(joined) || chunked_coding_count_(joined) != 1) {
                fail_request_(handle, 400, "invalid transfer-encoding");
                return false;
            }
            chunked = true;
        }
        std::size_t length { 0 };
        if (!chunked) {
            if (const auto contentLength { conn.request.header("content-length") }) {
                const auto [end, err] { std::from_chars(
                    contentLength->data(), contentLength->data() + contentLength->size(),
                    length) };
                if (err != std::errc {} || end != contentLength->data() + contentLength->size()) {
                    fail_request_(handle, 400, "invalid content-length");
                    return false;
                }
                // uWS overloads remainingStreamingBytes with chunked-state flag bits
                // in the top 5 bits; a Content-Length must never reach them. Reject
                // CL > STATE_SIZE_MASK (2^59-1). ref: bun-uws HttpParser.h:953 +
                // ChunkedEncoding.h:40.
                if (length > 0x07FFFFFFFFFFFFFFULL) {
                    fail_request_(handle, 400, "invalid content-length");
                    return false;
                }
            }
        }

        // Streaming mode (bun RequestContext): dispatch at header-complete;
        // the body follows as on_body chunk events while the handler may
        // already be producing the response.
        if (asyncHandler_ && bodyHandler_) {
            const Http1RequestId id { nextRequestId_++ };
            pending_.emplace(id, handle);
            conn.activeRequest = id;
            conn.request.remote = conn.peer;
            Http1Request head { std::move(conn.request) };
            conn.request = Http1Request {};
            if (chunked) {
                conn.phase = Phase::chunkedBody;
                conn.chunkDecoder = http::ChunkedDecoder {};
                conn.bodyStreaming = true;
                asyncHandler_(id, std::move(head));
                return true;
            }
            if (length > 0) {
                conn.phase = Phase::fixedBody;
                conn.bodyRemaining = length;
                conn.bodyStreaming = true;
                asyncHandler_(id, std::move(head));
                return true;
            }
            conn.phase = Phase::awaitingResponse;
            asyncHandler_(id, std::move(head));
            bodyHandler_(id, {}, true);  // bodiless: body stream closes at once
            return false;  // parked until finish_raw
        }

        if (chunked) {
            conn.phase = Phase::chunkedBody;
            conn.chunkDecoder = http::ChunkedDecoder {};
            return true;
        }
        if (length > 0) {
            conn.phase = Phase::fixedBody;
            conn.bodyRemaining = length;
            return true;
        }
        return finish_request_(handle, conn);
    }

    // Handler dispatch + response write + keep-alive/close bookkeeping.
    // Returns false when the connection was closed (stop pumping).
    bool finish_request_(runtime_socket::NativeHandle handle, Conn_& conn) {
        // Async mode: park the connection and hand the request out by id; the
        // response arrives later through write_raw/finish_raw/abort_raw.
        if (asyncHandler_) {
            const Http1RequestId id { nextRequestId_++ };
            pending_.emplace(id, handle);
            conn.phase = Phase::awaitingResponse;
            conn.activeRequest = id;
            conn.request.remote = conn.peer;
            Http1Request request { std::move(conn.request) };
            conn.request = Http1Request {};
            asyncHandler_(id, std::move(request));
            return false;  // stop pumping this connection until finish_raw
        }

        const Http1Handler* handler { &fallback_ };
        const auto match { dispatcher_.dispatch(
            RequestView { conn.request.method, conn.request.path }) };
        if (match.found)
            handler = &routeHandlers_[dispatcher_.route_at(match.routeIndex)->handlerId];

        Http1Response response {};
        if (*handler) {
            response = (*handler)(conn.request);
        } else {
            response.status = 404;  // render_missing equivalent
            response.body = "Not Found";
        }

        const bool keepAlive { conn.keepAlive };
        send_response_(conn, response, keepAlive);
        if (!keepAlive) {
            finish_close_(handle);
            return false;
        }
        // Recycle the context for the next request on this connection.
        conn.phase = Phase::headers;
        conn.request = Http1Request {};
        conn.bodyRemaining = 0;
        conn.chunkDecoder = http::ChunkedDecoder {};
        conn.keepAlive = true;
        return true;
    }

    // render_default_error equivalent: honest status + close, never leave the
    // peer waiting on a connection whose framing is broken.
    void fail_request_(runtime_socket::NativeHandle handle, std::uint16_t status,
                       std::string_view reason) {
        const auto found { conns_.find(handle) };
        if (found == conns_.end())
            return;
        // A framing failure mid-stream kills a request the consumer already
        // holds (dispatch-on-headers) — notify it like a client abort.
        const Http1RequestId aborted { found->second.activeRequest };
        Http1Response response {};
        response.status = status;
        response.headers.emplace_back("Content-Type", "text/plain");
        response.body.assign(reason);
        send_response_(found->second, response, false);
        scrub_pending_(handle);  // late write_raw/finish_raw become no-ops
        finish_close_(handle);
        if (aborted != 0 && abortHandler_)
            abortHandler_(aborted);
    }

    // render_metadata + end: fill in Content-Length (or honor an explicit
    // chunked Transfer-Encoding), stamp the Connection header, cork the
    // serialized response into the connection outbox.
    void send_response_(Conn_& conn, const Http1Response& response, bool keepAlive) {
        ResponseHead head {};
        head.status = response.status;
        head.headers = response.headers;

        bool hasLength { false };
        bool chunked { false };
        bool hasConnection { false };
        for (const auto& [name, value] : head.headers) {
            if (iequals_(name, "content-length"))
                hasLength = true;
            else if (iequals_(name, "transfer-encoding"))
                chunked = has_token_ci_(value, "chunked");
            else if (iequals_(name, "connection"))
                hasConnection = true;
        }
        if (!hasLength && !chunked)
            head.headers.emplace_back("Content-Length", std::to_string(response.body.size()));
        if (!hasConnection)
            head.headers.emplace_back("Connection", keepAlive ? "keep-alive" : "close");

        CorkedResponseBackend_ sink { conn.outbox };
        ResponseWriter writer { sink };
        if (chunked) {
            std::string framed {};
            if (!response.body.empty()) {
                framed += std::format("{:x}\r\n", response.body.size());
                framed += response.body;
                framed += "\r\n";
            }
            framed += "0\r\n\r\n";
            static_cast<void>(writer.write(head, framed));
        } else {
            static_cast<void>(writer.write(head, response.body));
        }
        writer.end();
    }
};

}  // namespace mbun::runtime_server
