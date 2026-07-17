// http_executor.cppm — mbun.install.http_executor
//
// Blocking HTTP/1.1 GET executor for the install network layer. This is the
// synchronous stand-in for bun's `AsyncHTTP::init(GET, url, headers, ...,
// FetchRedirect::Follow) + schedule + notify` pipeline (NetworkTask.rs
// `for_manifest`/`for_tarball` schedule their request on the HTTP thread and
// receive an `HTTPClientResult` in `notify`; runTasks.rs then classifies it).
// Here the whole request runs inline on the caller's thread and the result
// feeds the very same classification logic
// (`network_task::classify_manifest_response` / `classify_tarball_response`).
//
// Semantic correspondence with the reference (ref: .mbun/bun-ref/src/install/
// NetworkTask.rs + PackageManager/runTasks.rs):
//   * method is always GET; the header set comes verbatim from the
//     RequestDescriptor built by `network_task::for_manifest`/`for_tarball`;
//   * redirects are followed (AsyncHTTP uses FetchRedirect::Follow); on a
//     cross-origin hop the Authorization / npm-auth-type headers are dropped
//     (bun's http client strips credentials when the origin changes);
//   * a transport failure (resolve/connect/send/recv/timeout/framing) maps to
//     `response.metadata.is_none()` → `classify_*(has_metadata=false, ...)`;
//   * retry is an immediate re-enqueue with `retried += 1` and no backoff,
//     exactly like runTasks.rs (`if task.retried < max_retry_count { task.
//     retried += 1; enqueue_network_task(...) }`) — `execute_with_retry`
//     collapses that queue round-trip into a loop.
//
// TLS (https://): after the TCP connect the executor drives a client-role
//   mbun::tls::TlsChannel (memory-BIO handshake loop: want()→recv/send shuttling
//   ciphertext through feed_encrypted/take_encrypted), then application bytes go
//   through TlsChannel read/write while the same parse_response/ChunkedDecoder
//   framing runs on top. Certificate chains verify against the platform CA store
//   by default (ExecutorOptions::tlsCaBundle overrides it for self-signed test
//   registries; tlsVerifyPeer=false disables verification). A handshake/verify
//   failure surfaces as NetworkErrorCode::TlsHandshakeFailed.
//
// Documented deviations / DEFERRED:
//   * blocking, one-connection-per-request (Connection: close). bun's HTTP
//     thread multiplexes async sockets with keep-alive; the async executor and
//     the concurrent-request budget (`reduce_max_simultaneous_requests`) hook
//     in when the event-loop layer lands (DEFERRED).
//   * a process-wide DNS cache sits in front of getaddrinfo (detail::
//     resolve_cached). bun has no cache at this layer because uSockets resolves
//     once per *connection* and the keep-alive pool makes connections rare; with
//     one connection per request we would otherwise re-resolve the same registry
//     host on every request. It folds away once keep-alive lands.
//   * `Accept-Encoding: identity` is sent instead of bun's gzip/deflate —
//     transparent response decompression is DEFERRED until the shared inflate
//     path is wired here (tarballs are .tgz payloads either way).
//   * Windows (winsock) is DEFERRED: returns PlatformUnsupported, same honest
//     stub stance as modules/event_loop/src/epoll_backend.cppm.
module;

#if !defined(_WIN32)
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

export module mbun.install.http_executor;

import std;
import mbun.http.message;
import mbun.install.http_framing;
import mbun.install.network_task;
import mbun.tls;

namespace mbun::install::http_executor {

namespace nt = mbun::install::network_task;
namespace hf = mbun::install::http_framing;

// ── result / options types ──────────────────────────────────────────────────

// The blocking analogue of the metadata+body half of bun's HTTPClientResult:
// status/headers mirror `metadata.response`, body mirrors `response_buffer`.
export struct HttpResponse {
    int status{0};
    std::string reason;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::uint8_t> body;
};

export struct ExecutorOptions {
    // SO_SNDTIMEO also bounds connect() on Linux; SO_RCVTIMEO bounds each recv.
    // These are wall/idle bounds, not total-transfer bounds: ioTimeout limits the
    // gap between bytes (a steady large download never trips it), connectTimeout
    // bounds the TCP connect *and* the TLS handshake. They are deliberately far
    // larger than a healthy registry needs (connect/first-byte < 1s) yet small
    // enough that an unreachable/half-open host fails fast instead of stalling
    // the whole install — a stalling TLS peer now costs one connectTimeout, not
    // ioTimeout × the retry budget.
    std::chrono::milliseconds connectTimeout{std::chrono::seconds{10}};
    std::chrono::milliseconds ioTimeout{std::chrono::seconds{30}};
    std::size_t maxRedirects{10};
    std::size_t maxResponseBytes{std::size_t{1} << 30};  // 1 GiB guard

    // TLS (https) knobs. An empty caBundle verifies the server chain against the
    // platform default trust store (registry.npmjs.org); a PEM bundle here
    // overrides it for a self-signed test registry. tlsVerifyPeer=false disables
    // chain/host verification (bun's --no-verify / rejectUnauthorized=false).
    std::string tlsCaBundle{};
    bool tlsVerifyPeer{true};
};

export using ExecuteResult = std::expected<HttpResponse, nt::NetworkError>;

// Resolver accounting for the process-wide DNS cache (see detail::resolve_cached).
// `lookups` counts getaddrinfo calls actually issued, `hits` counts connections
// served from the cache — a whole install against one registry should show
// lookups == 1 no matter how many packages it fetches.
export struct ResolverStats {
    std::uint64_t lookups{0};
    std::uint64_t hits{0};
};

// Case-insensitive response-header lookup (headers keep their wire casing).
export const std::string* find_header(const HttpResponse& response, std::string_view name) {
    for (const auto& [key, value] : response.headers) {
        if (nt::UrlParts::ascii_ieq(key, name)) {
            return &value;
        }
    }
    return nullptr;
}

// Terminal errors must not burn retry attempts: retrying cannot fix a bad URL
// or a missing TLS backend. Everything else models `metadata.is_none()` and
// goes through the classify_* retry gate like any bun network error.
//
// TlsHandshakeFailed is deliberately NOT here. It used to be, on the theory that
// retrying a dead endpoint multiplies the deadline into a hang -- but bun retries
// a stalled TLS handshake like any other network failure, and has a regression
// test for it (test/cli/install/bun-install-stalled-tls.test.ts, whose comment
// reads "Don't spin through 5 retries (each its own timeout)"). Treating it as
// terminal turned a transient handshake blip into a fatal install: a real
// install of elysia died on one EAGAIN after 44 packages. The hang that
// motivated it comes from our sequential executor, not from retrying; the fix
// for that is the concurrent path plus an idle timeout, which is what bun bounds
// requests with.
export constexpr bool is_terminal_error(nt::NetworkErrorCode code) {
    switch (code) {
        case nt::NetworkErrorCode::InvalidURL:
        case nt::NetworkErrorCode::OutOfMemory:
        case nt::NetworkErrorCode::TlsNotWired:
        case nt::NetworkErrorCode::PlatformUnsupported:
        case nt::NetworkErrorCode::ResponseTooLarge:
            return true;
        default:
            return false;
    }
}

// ── implementation (POSIX) ──────────────────────────────────────────────────

namespace detail {

inline std::unexpected<nt::NetworkError> fail(nt::NetworkErrorCode code, std::string message) {
    return std::unexpected(nt::NetworkError{code, std::move(message)});
}

#if !defined(_WIN32)

struct FdGuard {
    int fd{-1};
    FdGuard() = default;
    explicit FdGuard(int f) : fd{f} {}
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
    ~FdGuard() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

inline ::timeval to_timeval(std::chrono::milliseconds ms) {
    ::timeval tv{};
    tv.tv_sec = static_cast<time_t>(ms.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((ms.count() % 1000) * 1000);
    return tv;
}

// Bound the next blocking recv on `fd` so the TLS handshake/read loops cannot
// wait past their deadline (a floor of 1ms keeps SO_RCVTIMEO ≠ "block forever").
inline void set_recv_timeout(int fd, std::chrono::milliseconds ms) {
    if (ms.count() < 1) {
        ms = std::chrono::milliseconds{1};
    }
    ::timeval tv{to_timeval(ms)};
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

// connect(2) with EINTR completion via poll + SO_ERROR (a connect interrupted
// by a signal keeps completing in the background; re-calling it is wrong).
inline int connect_eintr(int fd, const ::sockaddr* addr, ::socklen_t len,
                         std::chrono::milliseconds timeout) {
    if (::connect(fd, addr, len) == 0) {
        return 0;
    }
    if (errno != EINTR) {
        return -1;
    }
    ::pollfd pfd{fd, POLLOUT, 0};
    for (;;) {
        int r{::poll(&pfd, 1, static_cast<int>(timeout.count()))};
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            errno = r == 0 ? ETIMEDOUT : errno;
            return -1;
        }
        break;
    }
    int soErr{0};
    ::socklen_t soLen{sizeof soErr};
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &soLen) != 0) {
        return -1;
    }
    if (soErr != 0) {
        errno = soErr;
        return -1;
    }
    return 0;
}

inline bool send_all(int fd, std::string_view data) {
    std::size_t sent{0};
    while (sent < data.size()) {
        ::ssize_t n{::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL)};
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

struct RecvResult {
    ::ssize_t n{0};   // >0 bytes, 0 EOF, <0 error
    int err{0};       // errno when n < 0
    bool timed_out() const {
        return n < 0 && (err == EAGAIN || err == EWOULDBLOCK);
    }
};

inline RecvResult recv_some(int fd, char* buf, std::size_t cap) {
    for (;;) {
        ::ssize_t n{::recv(fd, buf, cap, 0)};
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return {n, n < 0 ? errno : 0};
    }
}

// One getaddrinfo row, deep-copied so it outlives freeaddrinfo(). Storing the
// `addrinfo*` itself would dangle: the list is owned by the resolver.
struct ResolvedAddr {
    ::sockaddr_storage storage{};
    ::socklen_t length{0};
    int family{0};
    int socktype{0};
    int protocol{0};
};

// Process-wide host:port → address-list cache.
//
// bun has no DNS cache in its HTTP client itself — resolution lives below it in
// uSockets, which resolves a host once per connection attempt and lets the
// keep-alive pool (HTTPContext.rs:701 `existing_socket`) keep that connection
// hot, so a whole install touches the resolver a handful of times. mbun's
// executor opens one connection per request (Connection: close), so without a
// cache the same host is resolved once per request: a 6-package install measured
// 12 getaddrinfo calls (12 requests × 1) for a single hostname, each fanning out
// into RFC3484 address-sorting probes. This cache restores the reference's
// *effective* behaviour (resolve a host once) on top of our connection model.
//
// No TTL / no invalidation: `bun install` is a one-shot CLI process, and bun
// likewise never re-resolves a host it already has a pooled connection to for
// the life of the process. Failures are deliberately not cached — a transient
// resolver error must stay retryable through the classify_* gate.
struct ResolverCache {
    std::mutex mutex{};
    std::unordered_map<std::string, std::vector<ResolvedAddr>> entries{};
    ResolverStats stats{};
};

inline ResolverCache& resolver_cache() {
    static ResolverCache cache{};
    return cache;
}

inline std::vector<ResolvedAddr>* resolve_cached(const std::string& hostZ,
                                                 const std::string& portZ,
                                                 int& gaiOut) {
    ResolverCache& cache{resolver_cache()};

    std::string key{hostZ + ":" + portZ};
    std::lock_guard<std::mutex> lock{cache.mutex};
    if (auto hit{cache.entries.find(key)}; hit != cache.entries.end()) {
        ++cache.stats.hits;
        gaiOut = 0;
        return &hit->second;
    }
    ++cache.stats.lookups;

    ::addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ::addrinfo* list{nullptr};
    gaiOut = ::getaddrinfo(hostZ.c_str(), portZ.c_str(), &hints, &list);
    if (gaiOut != 0 || list == nullptr) {
        if (list != nullptr) {
            ::freeaddrinfo(list);
        }
        return nullptr;
    }

    // Deep-copy in resolver order: getaddrinfo already applied RFC3484 sorting,
    // and the connect loop below tries the rows in order like bun's connector.
    std::vector<ResolvedAddr> addrs{};
    for (::addrinfo* ai{list}; ai != nullptr; ai = ai->ai_next) {
        if (ai->ai_addrlen > sizeof(::sockaddr_storage)) {
            continue;
        }
        ResolvedAddr row{};
        std::memcpy(&row.storage, ai->ai_addr, ai->ai_addrlen);
        row.length = ai->ai_addrlen;
        row.family = ai->ai_family;
        row.socktype = ai->ai_socktype;
        row.protocol = ai->ai_protocol;
        addrs.push_back(row);
    }
    ::freeaddrinfo(list);
    if (addrs.empty()) {
        gaiOut = EAI_NONAME;
        return nullptr;
    }
    return &cache.entries.emplace(std::move(key), std::move(addrs)).first->second;
}

// Resolve + connect. Tries every resolved row (v4/v6) like bun's connector.
inline std::expected<int, nt::NetworkError> open_connection(std::string_view host,
                                                            std::string_view port,
                                                            const ExecutorOptions& options) {
    std::string hostZ{host};
    std::string portZ{port.empty() ? std::string_view{"80"} : port};
    int gai{0};
    std::vector<ResolvedAddr>* addrs{resolve_cached(hostZ, portZ, gai)};
    if (addrs == nullptr) {
        return std::unexpected(nt::NetworkError{
            nt::NetworkErrorCode::ResolveFailed,
            "getaddrinfo failed for \"" + hostZ + "\": " + ::gai_strerror(gai)});
    }

    int lastErr{0};
    for (const ResolvedAddr& ai : *addrs) {
        FdGuard sock{::socket(ai.family, ai.socktype, ai.protocol)};
        if (sock.fd < 0) {
            lastErr = errno;
            continue;
        }
        ::timeval sndTv{to_timeval(options.connectTimeout)};
        ::timeval rcvTv{to_timeval(options.ioTimeout)};
        (void)::setsockopt(sock.fd, SOL_SOCKET, SO_SNDTIMEO, &sndTv, sizeof sndTv);
        (void)::setsockopt(sock.fd, SOL_SOCKET, SO_RCVTIMEO, &rcvTv, sizeof rcvTv);
        if (connect_eintr(sock.fd, reinterpret_cast<const ::sockaddr*>(&ai.storage), ai.length,
                          options.connectTimeout) == 0) {
            // Switch the send timeout from connect budget to io budget.
            (void)::setsockopt(sock.fd, SOL_SOCKET, SO_SNDTIMEO, &rcvTv, sizeof rcvTv);
            int fd{sock.fd};
            sock.fd = -1;  // ownership transferred to the caller
            return fd;
        }
        lastErr = errno;
    }
    return std::unexpected(nt::NetworkError{
        nt::NetworkErrorCode::ConnectFailed,
        "connect to " + hostZ + ":" + portZ + " failed: " +
            std::string{std::strerror(lastErr)}});
}

// ── body framing ────────────────────────────────────────────────────────────
// Head parsing + RFC7230 §3.3.3 body-length rules live in mbun.install
// .http_framing, shared verbatim with the concurrent executor in src/async_http/.

using Framing = hf::Framing;
using HeadInfo = hf::HeadInfo;
using hf::parse_head;

// ── transports (plaintext fd / TLS over fd) ─────────────────────────────────
// Both expose the same byte interface so perform_exchange runs unchanged over
// either: write_all pushes the whole buffer, read_some fills up to `cap` bytes
// (>0 data, 0 EOF, <0 error+errno) mirroring recv(2).

struct PlainTransport {
    int fd{-1};
    bool write_all(std::string_view data) { return send_all(fd, data); }
    RecvResult read_some(char* out, std::size_t cap) { return recv_some(fd, out, cap); }
};

// TlsChannel over `fd`: the app edge (read/write) drives the memory BIOs while
// this shuttles ciphertext to/from the socket. Decrypted plaintext is buffered
// between read_some calls because SSL_read hands back a whole record at a time.
struct TlsTransport {
    int fd{-1};
    mbun::tls::TlsChannel channel;
    std::vector<std::uint8_t> plain{};
    std::size_t plainPos{0};
    // Max idle time a single read_some may spend shuttling ciphertext without
    // surfacing any application bytes — bounds a stalled/half-open peer.
    std::chrono::milliseconds ioTimeout{std::chrono::seconds{60}};

    // Flush ciphertext the channel produced (handshake or app records) to the
    // socket. Preserves errno so callers can classify a timeout.
    bool flush() {
        while (channel.has_encrypted()) {
            std::vector<std::uint8_t> enc{channel.take_encrypted()};
            if (enc.empty()) {
                break;
            }
            if (!send_all(fd, std::string_view{reinterpret_cast<const char*>(enc.data()),
                                               enc.size()})) {
                return false;
            }
        }
        return true;
    }

    bool write_all(std::string_view data) {
        std::size_t off{0};
        while (off < data.size()) {
            std::size_t n{channel.write(
                {reinterpret_cast<const std::uint8_t*>(data.data()) + off, data.size() - off})};
            if (n == 0) {
                return false;  // memory-BIO SSL_write never partial-blocks; a 0 is fatal
            }
            off += n;
            if (!flush()) {
                return false;
            }
        }
        return true;
    }

    RecvResult read_some(char* out, std::size_t cap) {
        // Idle budget: reset once per call. Progress (returning app bytes) ends
        // the call, so a live download keeps re-arming it; only a peer that
        // never yields plaintext trips it, failing in bounded time.
        std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::now() +
                                                        ioTimeout};
        for (;;) {
            if (plainPos < plain.size()) {
                std::size_t n{std::min(cap, plain.size() - plainPos)};
                std::memcpy(out, plain.data() + plainPos, n);
                plainPos += n;
                return {static_cast<::ssize_t>(n), 0};
            }
            plain = channel.read();
            plainPos = 0;
            if (!plain.empty()) {
                continue;
            }
            // No plaintext ready: either the peer sent close_notify (clean EOF,
            // want()!=read) or we need more ciphertext off the socket.
            if (channel.want() != mbun::tls::IoWant::read) {
                return {0, 0};
            }
            if (!flush()) {
                return {-1, errno};
            }
            std::chrono::steady_clock::time_point now{std::chrono::steady_clock::now()};
            if (now >= deadline) {
                return {-1, EWOULDBLOCK};  // idle too long → Timeout at the caller
            }
            set_recv_timeout(
                fd, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            char tmp[16 * 1024];
            RecvResult r{recv_some(fd, tmp, sizeof tmp)};
            if (r.n <= 0) {
                return r;  // socket error/EOF/timeout propagates with errno intact
            }
            channel.feed_encrypted(
                {reinterpret_cast<const std::uint8_t*>(tmp), static_cast<std::size_t>(r.n)});
        }
    }
};

// Blocking client TLS handshake over the socket: emit ClientHello, then loop
// flush→read→feed until established or a fatal alert/verify failure. The whole
// handshake is bounded by connectTimeout (a wall-clock deadline plus a matching
// per-recv SO_RCVTIMEO), so a silent or half-open peer that completes the TCP
// connect but never speaks TLS fails in bounded time instead of hanging. Every
// handshake failure — timeout, EOF, alert, non-convergence — is reported as the
// terminal TlsHandshakeFailed so it is not retried (retrying a dead endpoint
// would multiply the deadline into a hang across the retry budget).
inline std::optional<nt::NetworkError> tls_handshake(TlsTransport& tr, std::string_view rawUrl,
                                                     const ExecutorOptions& options) {
    if (!tr.channel.valid()) {
        return nt::NetworkError{nt::NetworkErrorCode::TlsHandshakeFailed,
                                "TLS setup failed for " + std::string{rawUrl} + ": " +
                                    std::string{tr.channel.last_error()}};
    }
    std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::now() +
                                                   options.connectTimeout};
    tr.channel.handshake();
    for (int guard{0}; guard < 100000; ++guard) {
        if (!tr.flush()) {
            return nt::NetworkError{nt::NetworkErrorCode::TlsHandshakeFailed,
                                    "TLS handshake write failed for " + std::string{rawUrl} +
                                        ": " + std::strerror(errno)};
        }
        if (tr.channel.established()) {
            return std::nullopt;
        }
        if (tr.channel.handshake_state() == mbun::tls::HandshakeState::failed) {
            return nt::NetworkError{nt::NetworkErrorCode::TlsHandshakeFailed,
                                    "TLS handshake failed for " + std::string{rawUrl} + ": " +
                                        std::string{tr.channel.last_error()}};
        }
        std::chrono::steady_clock::time_point now{std::chrono::steady_clock::now()};
        if (now >= deadline) {
            return nt::NetworkError{nt::NetworkErrorCode::TlsHandshakeFailed,
                                    "TLS handshake timed out for " + std::string{rawUrl}};
        }
        set_recv_timeout(tr.fd,
                         std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
        char tmp[16 * 1024];
        RecvResult r{recv_some(tr.fd, tmp, sizeof tmp)};
        if (r.n < 0) {
            return nt::NetworkError{nt::NetworkErrorCode::TlsHandshakeFailed,
                                    "TLS handshake read failed/timed out for " +
                                        std::string{rawUrl} + ": " + std::strerror(r.err)};
        }
        if (r.n == 0) {
            return nt::NetworkError{nt::NetworkErrorCode::TlsHandshakeFailed,
                                    "connection closed during TLS handshake for " +
                                        std::string{rawUrl}};
        }
        tr.channel.feed_encrypted(
            {reinterpret_cast<const std::uint8_t*>(tmp), static_cast<std::size_t>(r.n)});
        tr.channel.handshake();
    }
    return nt::NetworkError{nt::NetworkErrorCode::TlsHandshakeFailed,
                            "TLS handshake did not converge for " + std::string{rawUrl}};
}

// GET request text. Host carries the explicit port only when the URL had one
// (matching what bun's HTTPClient sends for non-default ports).
inline std::string build_request(const nt::UrlParts& url, const std::vector<nt::Header>& headers) {
    std::string request;
    request.reserve(256);
    request.append("GET ").append(url.pathname.empty() ? "/" : url.pathname)
        .append(" HTTP/1.1\r\nHost: ").append(url.hostname);
    if (!url.port.empty()) {
        request.push_back(':');
        request.append(url.port);
    }
    request.append("\r\n");
    for (const nt::Header& h : headers) {
        request.append(h.name).append(": ").append(h.value).append("\r\n");
    }
    request.append("Connection: close\r\nAccept-Encoding: identity\r\n\r\n");
    return request;
}

// Send the request and read/frame the response over an established transport.
template <class Transport>
inline ExecuteResult perform_exchange(Transport& tr, std::string_view request,
                                      std::string_view rawUrl, const ExecutorOptions& options) {
    if (!tr.write_all(request)) {
        bool timedOut{errno == EAGAIN || errno == EWOULDBLOCK};
        return fail(timedOut ? nt::NetworkErrorCode::Timeout : nt::NetworkErrorCode::SendFailed,
                    "send to " + std::string{rawUrl} + " failed: " + std::strerror(errno));
    }

    // Read until the header block parses.
    std::string data;
    HeadInfo head{};
    char buf[16 * 1024];
    for (;;) {
        RecvResult r{tr.read_some(buf, sizeof buf)};
        if (r.n < 0) {
            return fail(r.timed_out() ? nt::NetworkErrorCode::Timeout
                                      : nt::NetworkErrorCode::RecvFailed,
                        "recv from " + std::string{rawUrl} + " failed: " + std::strerror(r.err));
        }
        if (r.n == 0) {
            return fail(nt::NetworkErrorCode::ProtocolError,
                        "connection closed before response headers from " + std::string{rawUrl});
        }
        data.append(buf, static_cast<std::size_t>(r.n));
        if (data.size() > options.maxResponseBytes) {
            return fail(nt::NetworkErrorCode::ResponseTooLarge,
                        "response header block exceeds limit from " + std::string{rawUrl});
        }
        std::optional<mbun::http::ParseStatus> st{parse_head(data, head)};
        if (st == mbun::http::ParseStatus::Ok) {
            break;
        }
        if (st == mbun::http::ParseStatus::Invalid) {
            return fail(nt::NetworkErrorCode::ProtocolError,
                        "malformed HTTP response from " + std::string{rawUrl});
        }
    }

    // Body per framing.
    std::string body;
    std::string_view initial{std::string_view{data}.substr(head.bytesRead)};
    switch (head.framing) {
        case Framing::NoBody:
            break;
        case Framing::ContentLength: {
            body.assign(initial.substr(0, std::min(initial.size(), head.contentLength)));
            if (head.contentLength > options.maxResponseBytes) {
                return fail(nt::NetworkErrorCode::ResponseTooLarge,
                            "Content-Length exceeds limit from " + std::string{rawUrl});
            }
            while (body.size() < head.contentLength) {
                RecvResult r{tr.read_some(buf, sizeof buf)};
                if (r.n < 0) {
                    return fail(r.timed_out() ? nt::NetworkErrorCode::Timeout
                                              : nt::NetworkErrorCode::RecvFailed,
                                "recv from " + std::string{rawUrl} + " failed: " +
                                    std::strerror(r.err));
                }
                if (r.n == 0) {
                    return fail(nt::NetworkErrorCode::ProtocolError,
                                "response body truncated from " + std::string{rawUrl});
                }
                std::size_t want{head.contentLength - body.size()};
                body.append(buf, std::min(static_cast<std::size_t>(r.n), want));
            }
            break;
        }
        case Framing::Chunked: {
            mbun::http::ChunkedDecoder dec{};
            dec.consume_trailer = true;
            mbun::http::ChunkedResult r{mbun::http::decode_chunked(dec, initial)};
            body.append(r.decoded);
            while (r.status == mbun::http::ParseStatus::Incomplete) {
                RecvResult rr{tr.read_some(buf, sizeof buf)};
                if (rr.n < 0) {
                    return fail(rr.timed_out() ? nt::NetworkErrorCode::Timeout
                                               : nt::NetworkErrorCode::RecvFailed,
                                "recv from " + std::string{rawUrl} + " failed: " +
                                    std::strerror(rr.err));
                }
                if (rr.n == 0) {
                    return fail(nt::NetworkErrorCode::ProtocolError,
                                "chunked body truncated from " + std::string{rawUrl});
                }
                r = mbun::http::decode_chunked(dec, {buf, static_cast<std::size_t>(rr.n)});
                body.append(r.decoded);
                if (body.size() > options.maxResponseBytes) {
                    return fail(nt::NetworkErrorCode::ResponseTooLarge,
                                "chunked body exceeds limit from " + std::string{rawUrl});
                }
            }
            if (r.status == mbun::http::ParseStatus::Invalid) {
                return fail(nt::NetworkErrorCode::ProtocolError,
                            "malformed chunked framing from " + std::string{rawUrl});
            }
            break;
        }
        case Framing::UntilClose: {
            body.assign(initial);
            for (;;) {
                RecvResult r{tr.read_some(buf, sizeof buf)};
                if (r.n < 0) {
                    return fail(r.timed_out() ? nt::NetworkErrorCode::Timeout
                                              : nt::NetworkErrorCode::RecvFailed,
                                "recv from " + std::string{rawUrl} + " failed: " +
                                    std::strerror(r.err));
                }
                if (r.n == 0) {
                    break;  // close-delimited body ends at EOF
                }
                body.append(buf, static_cast<std::size_t>(r.n));
                if (body.size() > options.maxResponseBytes) {
                    return fail(nt::NetworkErrorCode::ResponseTooLarge,
                                "response body exceeds limit from " + std::string{rawUrl});
                }
            }
            break;
        }
    }

    HttpResponse out{};
    out.status = head.status;
    out.reason = std::move(head.reason);
    out.headers = std::move(head.headers);
    out.body.assign(body.begin(), body.end());
    return out;
}

// One GET on one connection; no redirect handling (execute() loops those). For
// https the TCP connection is wrapped in a client-role TlsChannel and the
// exchange runs over the decrypted app edge; http goes over the bare fd.
inline ExecuteResult perform_once(const nt::UrlParts& url, std::string_view rawUrl,
                                  const std::vector<nt::Header>& headers,
                                  const ExecutorOptions& options) {
    bool isHttps{nt::UrlParts::ascii_ieq(url.protocol, "https")};
    std::string port{url.port.empty() ? std::string{isHttps ? "443" : "80"}
                                      : std::string{url.port}};
    auto conn{open_connection(url.hostname, port, options)};
    if (!conn) {
        return std::unexpected(std::move(conn).error());
    }
    FdGuard sock{*conn};

    std::string request{build_request(url, headers)};

    if (isHttps) {
        mbun::tls::Config cfg{};
        cfg.serverName = std::string{url.hostname};
        cfg.ca = options.tlsCaBundle;
        cfg.verify = options.tlsVerifyPeer ? mbun::tls::VerifyMode::required
                                           : mbun::tls::VerifyMode::disabled;
        TlsTransport tr{sock.fd, mbun::tls::TlsChannel{mbun::tls::TlsRole::client, std::move(cfg)},
                        {}, 0, options.ioTimeout};
        if (auto err{tls_handshake(tr, rawUrl, options)}) {
            return std::unexpected(std::move(*err));
        }
        // Handshake tightened SO_RCVTIMEO toward its deadline; restore the io
        // budget for the response (read_some re-arms it per call thereafter).
        set_recv_timeout(sock.fd, options.ioTimeout);
        return perform_exchange(tr, request, rawUrl, options);
    }

    PlainTransport tr{sock.fd};
    return perform_exchange(tr, request, rawUrl, options);
}

#endif  // !defined(_WIN32)

inline bool is_redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

}  // namespace detail

// ── public API ──────────────────────────────────────────────────────────────

// Snapshot of the process-wide resolver cache accounting.
export ResolverStats resolver_stats() {
#if defined(_WIN32)
    return ResolverStats{};  // DEFERRED: no winsock resolver path yet.
#else
    detail::ResolverCache& cache{detail::resolver_cache()};
    std::lock_guard<std::mutex> lock{cache.mutex};
    return cache.stats;
#endif
}

// Blocking GET of `request.url` with `request.headers`, following redirects
// (FetchRedirect::Follow semantics). Transport failures map to error codes the
// caller feeds into classify_* with has_metadata=false.
export ExecuteResult execute(const nt::RequestDescriptor& request,
                             const ExecutorOptions& options = {}) {
#if defined(_WIN32)
    (void)options;
    return detail::fail(nt::NetworkErrorCode::PlatformUnsupported,
                        "http_executor: winsock backend is DEFERRED; cannot fetch " + request.url);
#else
    std::string currentUrl{request.url};
    nt::UrlParts origin{nt::parse_url(request.url)};
    for (std::size_t hop{0}; hop <= options.maxRedirects; ++hop) {
        nt::UrlParts url{nt::parse_url(currentUrl)};
        bool isHttp{nt::UrlParts::ascii_ieq(url.protocol, "http")};
        bool isHttps{nt::UrlParts::ascii_ieq(url.protocol, "https")};
        if ((!isHttp && !isHttps) || url.hostname.empty()) {
            return detail::fail(nt::NetworkErrorCode::InvalidURL,
                                "expected an http(s):// URL, got \"" + currentUrl + "\"");
        }

        // Drop credentials once a redirect leaves the original origin.
        bool sameOrigin{nt::UrlParts::ascii_ieq(url.protocol, origin.protocol) &&
                        nt::UrlParts::ascii_ieq(url.hostname, origin.hostname) &&
                        url.effective_port() == origin.effective_port()};
        std::vector<nt::Header> headers;
        headers.reserve(request.headers.size());
        for (const nt::Header& h : request.headers) {
            if (!sameOrigin && (nt::UrlParts::ascii_ieq(h.name, "Authorization") ||
                                nt::UrlParts::ascii_ieq(h.name, "npm-auth-type"))) {
                continue;
            }
            headers.push_back(h);
        }

        ExecuteResult result{detail::perform_once(url, currentUrl, headers, options)};
        if (!result || !detail::is_redirect_status(result->status)) {
            return result;
        }
        const std::string* location{find_header(*result, "Location")};
        if (location == nullptr || location->empty()) {
            return result;  // redirect without Location: surface it as-is
        }
        currentUrl = nt::url_join(currentUrl, *location);
        if (currentUrl.empty()) {
            return detail::fail(nt::NetworkErrorCode::InvalidURL,
                                "invalid redirect Location \"" + *location + "\"");
        }
    }
    return detail::fail(nt::NetworkErrorCode::ProtocolError,
                        "too many redirects fetching " + request.url);
#endif
}

// ── retry loop (runTasks.rs semantics) ──────────────────────────────────────

export struct RetryOutcome {
    ExecuteResult result{HttpResponse{}};
    std::uint16_t retried{0};
    nt::Classification classification{};
};

// Wraps execute() in the exact retry gate runTasks.rs applies per response:
// classify_*(has_metadata, status, retried, policy); verdict Retry bumps
// `retried` and immediately re-issues the request (no backoff, mirroring
// `enqueue_network_task` re-enqueue). Terminal executor errors (bad URL /
// TLS not wired / unsupported platform) skip the retry gate: metadata will
// never appear no matter how often we retry.
export RetryOutcome execute_with_retry(const nt::RequestDescriptor& request,
                                       const nt::RetryPolicy& policy,
                                       const ExecutorOptions& options = {}) {
    RetryOutcome out{};
    for (;;) {
        out.result = execute(request, options);
        if (!out.result && is_terminal_error(out.result.error().code)) {
            out.classification = {nt::ResponseVerdict::Fail, "HTTPError"};
            return out;
        }
        bool hasMetadata{out.result.has_value()};
        int status{hasMetadata ? out.result->status : 0};
        out.classification =
            request.kind == nt::RequestKind::Extract
                ? nt::classify_tarball_response(hasMetadata, status, out.retried, policy)
                : nt::classify_manifest_response(hasMetadata, status, out.retried, policy);
        if (out.classification.verdict != nt::ResponseVerdict::Retry) {
            return out;
        }
        ++out.retried;
    }
}

}  // namespace mbun::install::http_executor
