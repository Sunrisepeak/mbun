// connection.cppm — mbun.install.async_http.connection
//
// One request's state machine, driven by event-loop callbacks:
//
//   connecting → [tls_handshake] → sending → reading_head → reading_body → done
//
// PORT-SOURCE: bun's HTTPClient (src/http/lib.rs) driven by the socket callbacks
// its HTTPContext installs (src/http/HTTPContext.rs: onOpen / onData / onWritable
// / onClose / onTimeout). Every in-flight request is a non-blocking socket on the
// single "HTTP Client" loop; nothing here may block that loop.
//
// This is a *control inversion* of the blocking executor in
// src/http_executor.cppm, not a second implementation:
//   * the TLS handshake is the same memory-BIO shuttle (feed_encrypted →
//     handshake() → take_encrypted → socket), with the blocking recv replaced by
//     on_data delivery. want()/established()/handshake_state() semantics are
//     unchanged.
//   * response framing is mbun.install.http_framing (parse_head + RFC7230
//     §3.3.3), shared verbatim with the blocking path; bodies use the same
//     mbun::http::ChunkedDecoder.
//   * the result type is http_executor::HttpResponse / ExecuteResult, so both
//     paths feed the identical classify_* retry gate in network_task.cppm.
//
// Timeouts: bun bounds a request with a single idle timer, not a connect/read
// split — IDLE_TIMEOUT_SECONDS = 300 (src/http/lib.rs:283), overridable via
// BUN_CONFIG_HTTP_IDLE_TIMEOUT (bun-core/env_var.rs:56, wired at
// HTTPThread.rs:1265-1272). bun arms it in onOpen (the fix for issue #30325);
// we arm it at connect() as well, so a connect that never completes is bounded
// too — uSockets bounds that below bun's client, a layer we do not have. Any
// byte of progress re-arms it.
//
// Keep-alive: `Connection: keep-alive` is sent (bun: is_keep_alive_possible,
// src/http/lib.rs:2189; enable_keepalive default true, feature_flags.zig:30).
// Socket reuse itself is the pool's job (HTTPContext.rs:19 POOL_SIZE=64);
// until it lands, connection_reusable() reports whether a socket *could* be
// pooled and the owner simply closes it.
//
// DEFERRED (documented, not silently missing):
//   * `Accept-Encoding: gzip, deflate` is sent and decoded transparently
//     (build_request_ / finish_ → decode_body_). bun advertises "gzip, deflate,
//     br, zstd" (http/lib.rs:980); brotli and zstd are absent here only because
//     mbun.core.compress has no decoder for them yet — the header must never
//     advertise more than decode_body_ can decode.
//   * HTTP/2 is deliberately absent: bun never offers h2 ALPN on the install
//     path (can_offer_h2, src/http/lib.rs:1880-1910, requires force_http2 or
//     experimental_http2_client_from_cli, neither of which src/install/ sets),
//     so alpn_offer is always .h1 there.
//   * Linux only, matching event_loop/runtime_socket's epoll backends.
export module mbun.install.async_http.connection;

import std;
import mbun.core.compress;
import mbun.event_loop;
import mbun.http.message;
import mbun.install.http_executor;
import mbun.install.http_framing;
import mbun.install.network_task;
import mbun.runtime_socket;
import mbun.tls;

export namespace mbun::install::async_http {

namespace nt = mbun::install::network_task;
namespace hf = mbun::install::http_framing;
namespace hx = mbun::install::http_executor;

// bun's only timeout knob on this path: IDLE_TIMEOUT_SECONDS = 300
// (src/http/lib.rs:283), env BUN_CONFIG_HTTP_IDLE_TIMEOUT (bun-core/
// env_var.rs:56, default 300), normalized once at startup
// (HTTPThread.rs:1265-1272). 0 disables the timer.
inline constexpr std::uint32_t IDLE_TIMEOUT_SECONDS{300};

// Ported verbatim from normalize_idle_timeout_seconds (src/http/lib.rs:296-304):
// uSockets' long-timeout counter wraps % 240 minutes, so clamp to 239 min; and
// values above 240s are served by the minute-granularity long timer, so round
// them UP to a whole minute — rounding down would fire earlier than asked.
//
// mbun's event_loop timer has no such granularity, so this math is not load
// bearing here. It is copied anyway: BUN_CONFIG_HTTP_IDLE_TIMEOUT is an
// observable knob, and a value that behaves differently than bun's would be a
// deviation even where the underlying reason no longer applies.
inline constexpr std::uint32_t normalize_idle_timeout_seconds(std::uint64_t raw) {
    raw = std::min<std::uint64_t>(raw, 239ULL * 60ULL);
    if (raw > 240) {
        raw = ((raw + 59) / 60) * 60;  // div_ceil(60) * 60
    }
    return static_cast<std::uint32_t>(raw);
}

// Per-request knobs. Deliberately *not* the blocking executor's
// connectTimeout/ioTimeout pair: that split is an mbun invention, while bun has
// exactly one per-request idle bound (lib.rs:283).
struct ConnectionOptions {
    std::chrono::milliseconds idleTimeout{std::chrono::seconds{IDLE_TIMEOUT_SECONDS}};
    std::size_t maxResponseBytes{std::size_t{1} << 30};  // 1 GiB guard
    std::string tlsCaBundle{};
    bool tlsVerifyPeer{true};
};

enum class ConnState : std::uint8_t {
    idle,
    connecting,
    tls_handshake,
    sending,
    reading_head,
    reading_body,
    done,
    failed,
};

class HttpConnection {
public:
    using Completion = std::function<void(hx::ExecuteResult)>;

private:
    event_loop::EventLoop& loop_;
    runtime_socket::SocketBackend& backend_;
    ConnectionOptions options_{};

    ConnState state_{ConnState::idle};
    runtime_socket::NativeHandle handle_{-1};
    bool handleOpen_{false};

    std::string url_{};
    std::string request_{};
    bool https_{false};
    std::string hostname_{};

    std::optional<mbun::tls::TlsChannel> tls_{};

    std::string buffer_{};  // undecrypted-plaintext accumulation (head + body)
    hf::HeadInfo head_{};
    std::string body_{};
    std::optional<mbun::http::ChunkedDecoder> chunked_{};

    event_loop::TimerId idleTimer_{};
    bool idleArmed_{false};

    Completion completion_{};
    bool completed_{false};

public:  // Big Five: owns a live fd + a loop timer; non-copyable/non-movable
         // because the owner routes callbacks to a stable address.
    HttpConnection(event_loop::EventLoop& loop, runtime_socket::SocketBackend& backend,
                   ConnectionOptions options)
        : loop_{loop}
        , backend_{backend}
        , options_{std::move(options)} {}
    HttpConnection(const HttpConnection&) = delete;
    HttpConnection& operator=(const HttpConnection&) = delete;
    HttpConnection(HttpConnection&&) = delete;
    HttpConnection& operator=(HttpConnection&&) = delete;
    ~HttpConnection() { disarm_idle_(); }

public:  // introspection
    [[nodiscard]] ConnState state() const { return state_; }
    [[nodiscard]] runtime_socket::NativeHandle handle() const { return handle_; }
    [[nodiscard]] bool finished() const {
        return state_ == ConnState::done || state_ == ConnState::failed;
    }
    // Whether the peer left this socket reusable (bun: is_keep_alive_possible,
    // lib.rs:2189). The pool consults this before parking the socket.
    [[nodiscard]] bool connection_reusable() const {
        if (state_ != ConnState::done || head_.framing == hf::Framing::UntilClose) {
            return false;  // close-delimited bodies end *by* closing the socket
        }
        for (const auto& [name, value] : head_.headers) {
            if (nt::UrlParts::ascii_ieq(name, "Connection")) {
                return !hf::token_list_contains(value, "close");
            }
        }
        return true;  // HTTP/1.1 default is persistent
    }

public:
    // Kick off the request: non-blocking connect, then everything else happens
    // on loop callbacks (us_socket_context_connect → onOpen in bun's context).
    // `address` must be numeric — the epoll backend feeds it to inet_pton, and
    // resolution already happened in HostResolver.
    void start(const runtime_socket::Address& address, const nt::UrlParts& url,
               std::string_view rawUrl, const std::vector<nt::Header>& headers,
               Completion completion) {
        completion_ = std::move(completion);
        url_ = std::string{rawUrl};
        hostname_ = url.hostname;
        https_ = nt::UrlParts::ascii_ieq(url.protocol, "https");
        request_ = build_request_(url, headers);

        auto connected{backend_.connect(address)};
        if (!connected) {
            fail_(nt::NetworkErrorCode::ConnectFailed,
                  "connect to " + std::string{url.hostname} + " failed: " +
                      connected.error().message);
            return;
        }
        handle_ = *connected;
        handleOpen_ = true;
        state_ = ConnState::connecting;
        arm_idle_();
    }

    // Speak on a socket the pool already connected (and, for https, already
    // handshook). There is no connect and no onOpen: the socket is live, so the
    // request goes out now and the machine starts at `sending`. bun's
    // equivalent is existing_socket() returning a socket whose HTTPClient then
    // skips straight to writing the request.
    void start_pooled(runtime_socket::NativeHandle handle,
                      std::optional<mbun::tls::TlsChannel> tls, const nt::UrlParts& url,
                      std::string_view rawUrl, const std::vector<nt::Header>& headers,
                      Completion completion) {
        completion_ = std::move(completion);
        url_ = std::string{rawUrl};
        hostname_ = url.hostname;
        https_ = nt::UrlParts::ascii_ieq(url.protocol, "https");
        request_ = build_request_(url, headers);
        handle_ = handle;
        handleOpen_ = true;
        tls_ = std::move(tls);
        state_ = ConnState::sending;
        arm_idle_();
        if (tls_) {
            send_tls_(request_);
            return;
        }
        send_plain_(request_);
    }

    // Give up the socket without closing it, so the pool can park it. The
    // established TLS session travels with the fd — it *is* the connection.
    // After this the object owns nothing and must not be driven again.
    struct DetachedSocket {
        runtime_socket::NativeHandle handle{-1};
        std::optional<mbun::tls::TlsChannel> tls{};
    };

    DetachedSocket detach() {
        disarm_idle_();
        DetachedSocket out{handle_, std::move(tls_)};
        tls_.reset();
        handle_ = -1;
        handleOpen_ = false;
        state_ = ConnState::done;
        return out;
    }

    // us_socket_open / onOpen. Note the backend defers this to the next tick even
    // when a loopback connect succeeds immediately (epoll_socket_backend
    // .cppm:145-151), so it always arrives here and never before start() returns.
    void on_open() {
        if (finished() || state_ != ConnState::connecting) {
            return;
        }
        rearm_idle_();
        if (!https_) {
            state_ = ConnState::sending;
            send_plain_(request_);
            return;
        }
        mbun::tls::Config cfg{};
        cfg.serverName = hostname_;
        cfg.ca = options_.tlsCaBundle;
        cfg.verify = options_.tlsVerifyPeer ? mbun::tls::VerifyMode::required
                                            : mbun::tls::VerifyMode::disabled;
        tls_.emplace(mbun::tls::TlsRole::client, std::move(cfg));
        if (!tls_->valid()) {
            fail_(nt::NetworkErrorCode::TlsHandshakeFailed,
                  "TLS setup failed for " + url_ + ": " + std::string{tls_->last_error()});
            return;
        }
        state_ = ConnState::tls_handshake;
        tls_->handshake();  // emit ClientHello into the memory BIO
        flush_tls_();
    }

    // onData. Ciphertext when TLS is on, application bytes otherwise.
    void on_data(std::span<const std::byte> bytes) {
        if (finished() || bytes.empty()) {
            return;
        }
        rearm_idle_();
        if (!tls_) {
            feed_plaintext_({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
            return;
        }
        tls_->feed_encrypted(
            {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
        if (state_ == ConnState::tls_handshake) {
            drive_handshake_();
            if (finished() || state_ == ConnState::tls_handshake) {
                return;
            }
        }
        drain_tls_plaintext_();
    }

    // onClose: peer FIN, or a failed connect (the backend reports SO_ERROR != 0
    // on a connecting socket through this same path — epoll_socket_backend
    // .cppm:455-461). The fd is already gone; do not close it again.
    void on_close() {
        handleOpen_ = false;
        if (finished()) {
            return;
        }
        switch (state_) {
            case ConnState::connecting:
                fail_(nt::NetworkErrorCode::ConnectFailed, "connect to " + url_ + " failed");
                return;
            case ConnState::tls_handshake:
                fail_(nt::NetworkErrorCode::TlsHandshakeFailed,
                      "connection closed during TLS handshake for " + url_);
                return;
            case ConnState::reading_body:
                // A close-delimited body is *terminated* by the close.
                if (head_.framing == hf::Framing::UntilClose) {
                    finish_();
                    return;
                }
                fail_(nt::NetworkErrorCode::ProtocolError,
                      "response body truncated from " + url_);
                return;
            default:
                fail_(nt::NetworkErrorCode::ProtocolError,
                      "connection closed before response headers from " + url_);
                return;
        }
    }

    // onTimeout: the single idle bound (bun lib.rs:283). Reported as Timeout —
    // never as TlsHandshakeFailed, even when it fires mid-handshake — so it
    // lands on classify_*'s metadata.is_none() retry path like any bun timeout.
    void on_idle_timeout() {
        idleArmed_ = false;
        if (finished()) {
            return;
        }
        fail_(nt::NetworkErrorCode::Timeout, "request to " + url_ + " timed out (idle)");
    }

private:
    // GET request text. Mirrors http_executor::build_request except for
    // Connection: keep-alive (bun lib.rs:2189) instead of close.
    std::string build_request_(const nt::UrlParts& url, const std::vector<nt::Header>& headers) {
        std::string request;
        request.reserve(256);
        request.append("GET ")
            .append(url.pathname.empty() ? "/" : url.pathname)
            .append(" HTTP/1.1\r\nHost: ")
            .append(url.hostname);
        if (!url.port.empty()) {
            request.push_back(':');
            request.append(url.port);
        }
        request.append("\r\n");
        for (const nt::Header& h : headers) {
            request.append(h.name).append(": ").append(h.value).append("\r\n");
        }
        // bun sends Accept-Encoding on every request
        // (ACCEPT_ENCODING_HEADER, http/lib.rs:991 → :980 "gzip, deflate, br,
        // zstd"). Advertising `identity` instead is not a neutral simplification:
        // npm serves packuments as raw JSON, and the abbreviated packument for a
        // package with many versions is huge. MEASURED against
        // registry.npmjs.org for @typescript-eslint/typescript-estree:
        //   identity            → 8,804,113 bytes on the wire
        //   gzip/deflate/br/... → 2,429,754 bytes  (3.6x less)
        // On a real install that is the difference between spending ~100s in one
        // manifest GET and ~2s, and it is the single largest cost mbun's install
        // was paying versus bun.
        //
        // DEVIATION (deliberate): bun's list is "gzip, deflate, br, zstd"; this
        // advertises only what mbun can actually decode today —
        // mbun.core.compress ships gzip/zlib/raw-inflate but no brotli or zstd.
        // Advertising a codec we cannot decode would be a correctness bug (the
        // server is free to pick it), so the list grows when the decoders land,
        // not before.
        request.append("Connection: keep-alive\r\nAccept-Encoding: gzip, deflate\r\n\r\n");
        return request;
    }

    void send_plain_(std::string_view data) {
        // The backend buffers any unsent tail and drains it on EPOLLOUT
        // (us_socket_stream_buffer_t semantics), so a short write is not ours
        // to handle — but a hard failure is.
        auto written{backend_.write(
            handle_, {reinterpret_cast<const std::byte*>(data.data()), data.size()})};
        if (!written) {
            fail_(nt::NetworkErrorCode::SendFailed,
                  "send to " + url_ + " failed: " + written.error().message);
            return;
        }
        state_ = ConnState::reading_head;
    }

    // Push whatever ciphertext the channel has queued out to the socket.
    bool flush_tls_() {
        while (tls_->has_encrypted()) {
            std::vector<std::uint8_t> enc{tls_->take_encrypted()};
            if (enc.empty()) {
                break;
            }
            auto written{backend_.write(
                handle_, {reinterpret_cast<const std::byte*>(enc.data()), enc.size()})};
            if (!written) {
                fail_(nt::NetworkErrorCode::SendFailed,
                      "TLS write to " + url_ + " failed: " + written.error().message);
                return false;
            }
        }
        return true;
    }

    // Same convergence loop as the blocking tls_handshake(), minus the recv:
    // bytes arrive via on_data, so this returns to the loop when it needs more.
    void drive_handshake_() {
        tls_->handshake();
        if (!flush_tls_()) {
            return;
        }
        if (tls_->handshake_state() == mbun::tls::HandshakeState::failed) {
            fail_(nt::NetworkErrorCode::TlsHandshakeFailed,
                  "TLS handshake failed for " + url_ + ": " + std::string{tls_->last_error()});
            return;
        }
        if (!tls_->established()) {
            return;  // need more ciphertext; wait for the next on_data
        }
        state_ = ConnState::sending;
        send_tls_(request_);
    }

    void send_tls_(std::string_view data) {
        std::size_t off{0};
        while (off < data.size()) {
            std::size_t n{tls_->write(
                {reinterpret_cast<const std::uint8_t*>(data.data()) + off, data.size() - off})};
            if (n == 0) {
                // A memory-BIO SSL_write never partial-blocks; 0 is fatal.
                fail_(nt::NetworkErrorCode::SendFailed, "TLS write to " + url_ + " failed");
                return;
            }
            off += n;
            if (!flush_tls_()) {
                return;
            }
        }
        state_ = ConnState::reading_head;
    }

    // Pull every decrypted record the channel can produce and frame it.
    void drain_tls_plaintext_() {
        for (;;) {
            std::vector<std::uint8_t> plain{tls_->read()};
            if (plain.empty()) {
                break;
            }
            feed_plaintext_({reinterpret_cast<const char*>(plain.data()), plain.size()});
            if (finished()) {
                return;
            }
        }
        if (!flush_tls_()) {
            return;
        }
        // want() != read after a drain means the peer sent close_notify: a clean
        // TLS EOF, which the framing layer must see as end-of-stream.
        if (!finished() && tls_->want() != mbun::tls::IoWant::read) {
            on_close();
        }
    }

    // The framing edge: identical decisions to the blocking executor, reached by
    // accumulation instead of by looping on recv.
    void feed_plaintext_(std::string_view bytes) {
        if (state_ == ConnState::reading_head) {
            buffer_.append(bytes);
            if (buffer_.size() > options_.maxResponseBytes) {
                fail_(nt::NetworkErrorCode::ResponseTooLarge,
                      "response header block exceeds limit from " + url_);
                return;
            }
            std::optional<mbun::http::ParseStatus> st{hf::parse_head(buffer_, head_)};
            if (st == mbun::http::ParseStatus::Invalid) {
                fail_(nt::NetworkErrorCode::ProtocolError, "malformed HTTP response from " + url_);
                return;
            }
            if (st != mbun::http::ParseStatus::Ok) {
                return;  // Incomplete: more bytes, please
            }
            state_ = ConnState::reading_body;
            if (head_.framing == hf::Framing::Chunked) {
                chunked_.emplace();
                chunked_->consume_trailer = true;
            }
            if (head_.framing == hf::Framing::ContentLength &&
                head_.contentLength > options_.maxResponseBytes) {
                fail_(nt::NetworkErrorCode::ResponseTooLarge,
                      "Content-Length exceeds limit from " + url_);
                return;
            }
            // Bytes past the header block are the first body bytes.
            std::string rest{buffer_.substr(head_.bytesRead)};
            buffer_.clear();
            if (head_.framing == hf::Framing::NoBody) {
                finish_();
                return;
            }
            consume_body_(rest);
            return;
        }
        if (state_ == ConnState::reading_body) {
            consume_body_(bytes);
        }
    }

    void consume_body_(std::string_view bytes) {
        switch (head_.framing) {
            case hf::Framing::NoBody:
                finish_();
                return;
            case hf::Framing::ContentLength: {
                std::size_t want{head_.contentLength - body_.size()};
                body_.append(bytes.substr(0, std::min(bytes.size(), want)));
                if (body_.size() >= head_.contentLength) {
                    finish_();
                }
                return;
            }
            case hf::Framing::Chunked: {
                mbun::http::ChunkedResult r{mbun::http::decode_chunked(*chunked_, bytes)};
                body_.append(r.decoded);
                if (body_.size() > options_.maxResponseBytes) {
                    fail_(nt::NetworkErrorCode::ResponseTooLarge,
                          "chunked body exceeds limit from " + url_);
                    return;
                }
                if (r.status == mbun::http::ParseStatus::Invalid) {
                    fail_(nt::NetworkErrorCode::ProtocolError,
                          "malformed chunked framing from " + url_);
                    return;
                }
                if (r.status != mbun::http::ParseStatus::Incomplete) {
                    finish_();
                }
                return;
            }
            case hf::Framing::UntilClose:
                body_.append(bytes);
                if (body_.size() > options_.maxResponseBytes) {
                    fail_(nt::NetworkErrorCode::ResponseTooLarge,
                          "response body exceeds limit from " + url_);
                }
                return;  // ends at on_close()
        }
    }

    // Content-Encoding → plaintext. Applied to the fully-framed body, so it
    // composes with both length- and chunked-delimited responses: Transfer-
    // Encoding is the framing, Content-Encoding is the payload, and the framing
    // is already undone by the time this runs. Content-Length likewise describes
    // the *encoded* body, so the framing above is right to count it before this.
    //
    // Unknown/undecodable codings are an error rather than a pass-through: the
    // body would be handed to the SRI check and the tar reader as compressed
    // bytes, which fails later and further from the cause. We only ever advertise
    // gzip/deflate, so a server picking anything else is already misbehaving.
    std::expected<std::string, std::string> decode_body_(std::string_view coding,
                                                         const std::string& body) const {
        namespace cz = mbun::core::compress;
        // mbun.core.compress speaks span<const uint8_t> / vector<uint8_t>; its
        // `Bytes`/`ByteView` aliases are internal to that module, so spell the
        // std types out rather than depend on names it does not export.
        const auto bytes{[&body] {
            return std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(body.data()), body.size()};
        }};
        const auto to_string{[](const std::vector<std::uint8_t>& b) {
            return std::string{reinterpret_cast<const char*>(b.data()), b.size()};
        }};
        if (nt::UrlParts::ascii_ieq(coding, "gzip") || nt::UrlParts::ascii_ieq(coding, "x-gzip")) {
            auto out{cz::gzip_decompress(bytes())};
            if (!out) {
                return std::unexpected(out.error());
            }
            return to_string(*out);
        }
        if (nt::UrlParts::ascii_ieq(coding, "deflate")) {
            // HTTP "deflate" is historically ambiguous: RFC 9110 says zlib-
            // wrapped, but some servers send a raw deflate stream. Try the
            // spec-correct form first and fall back, which is what every real
            // client does.
            if (auto zlib{cz::zlib_decompress(bytes())}) {
                return to_string(*zlib);
            }
            auto raw{cz::inflate_raw(bytes())};
            if (!raw) {
                return std::unexpected(raw.error());
            }
            return to_string(*raw);
        }
        return std::unexpected("unsupported Content-Encoding \"" + std::string{coding} + "\"");
    }

    void finish_() {
        if (completed_) {
            return;
        }
        hx::HttpResponse out{};
        out.status = head_.status;
        out.reason = std::move(head_.reason);
        out.headers = head_.headers;

        // `identity`, or no header at all, means the body is already plaintext.
        const std::string* coding{hx::find_header(out, "Content-Encoding")};
        if (coding != nullptr && !coding->empty() &&
            !nt::UrlParts::ascii_ieq(*coding, "identity")) {
            auto decoded{decode_body_(*coding, body_)};
            if (!decoded) {
                fail_(nt::NetworkErrorCode::ProtocolError,
                      "failed to decode " + *coding + " response from " + url_ + ": " +
                          decoded.error());
                return;
            }
            body_ = std::move(*decoded);
        }
        state_ = ConnState::done;
        out.body.assign(body_.begin(), body_.end());
        complete_(hx::ExecuteResult{std::move(out)});
    }

    void fail_(nt::NetworkErrorCode code, std::string message) {
        if (completed_) {
            return;
        }
        state_ = ConnState::failed;
        complete_(hx::ExecuteResult{
            std::unexpected(nt::NetworkError{code, std::move(message)})});
    }

    void complete_(hx::ExecuteResult result) {
        completed_ = true;
        disarm_idle_();
        // The owner decides the socket's fate (pool it or close it); it may also
        // destroy this object from inside the callback, so touch nothing after.
        if (completion_) {
            Completion callback{std::move(completion_)};
            completion_ = {};
            callback(std::move(result));
        }
    }

    void arm_idle_() {
        if (options_.idleTimeout.count() <= 0) {
            return;  // 0 disables the timer (bun: disable_timeout, lib.rs:277)
        }
        idleTimer_ = loop_.set_timeout(options_.idleTimeout, [this] { on_idle_timeout(); },
                                       std::chrono::steady_clock::now());
        idleArmed_ = true;
    }

    // Any progress re-arms the bound, so a healthy transfer never trips it and
    // only a stalled peer does (bun re-arms per socket activity).
    void rearm_idle_() {
        disarm_idle_();
        arm_idle_();
    }

    void disarm_idle_() {
        if (idleArmed_) {
            static_cast<void>(loop_.clear_timer(idleTimer_));
            idleArmed_ = false;
        }
    }
};

}  // namespace mbun::install::async_http
