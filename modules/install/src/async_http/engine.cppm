// engine.cppm — mbun.install.async_http.engine
//
// Owns the loop and multiplexes every in-flight request on it.
//
// PORT-SOURCE: bun's HTTPThread (src/http/HTTPThread.rs) — one loop
// (MiniEventLoop::init_global at :1281) running `drain_events(); uws_loop.tick();`
// (:1373-1381), with the socket context's callbacks routed to the HTTPClient that
// owns each socket (src/http/HTTPContext.rs). mbun's equivalent is
// event_loop::HostReadinessBackend + EventLoop + runtime_socket::EpollSocketBackend.
//
// Routing: runtime_socket installs ONE SocketEvents callback set per backend and
// routes by fd (epoll_socket_backend.cppm:44-50), so the fd→connection map lives
// here — the same shape as bun, where the socket's user data points at its
// HTTPClient.
//
// Reaping: a completion fires from inside a socket callback, i.e. from inside the
// connection's own frame, so the connection must NOT be destroyed there. Finished
// requests are marked and reaped between loop ticks — the same reason bun defers
// task cleanup to the tick boundary rather than freeing inside a callback.
//
// Redirects: bun uses FetchRedirect::Follow for install requests, and its client
// drops credentials when the origin changes. Handled here (at the reap boundary)
// rather than inside the state machine, so a hop is simply a fresh connection —
// which is also what makes each hop poolable.
//
// Keep-alive: a finished request's socket goes back to its scheme's pool instead
// of being closed (release_socket, HTTPContext.rs:581) and the next request for
// the same host:port starts on it (existing_socket, :701). Because a parked
// socket is still registered with the loop, its events route here too — an idle
// socket that closes or speaks is dropped from the pool rather than handed to a
// later request. That, plus the liveness probe the pool runs at both ends, is
// what makes reuse safe against a peer that quietly hangs up.
export module mbun.install.async_http.engine;

import std;
import mbun.event_loop;
import mbun.install.async_http.connection;
import mbun.install.async_http.pool;
import mbun.install.async_http.resolver;
import mbun.install.async_http.scheduler;
import mbun.install.http_executor;
import mbun.install.network_task;
import mbun.runtime_socket;

export namespace mbun::install::async_http {

namespace nt = mbun::install::network_task;
namespace hx = mbun::install::http_executor;

class Engine {
public:
    using Completion = std::function<void(hx::ExecuteResult)>;

private:
    struct InFlight {
        std::unique_ptr<HttpConnection> conn{};
        nt::RequestDescriptor request{};
        ConnectionOptions options{};
        nt::UrlParts origin{};
        std::string currentUrl{};
        std::size_t hop{0};
        Completion done{};

        bool settled{false};              // connection reported a result
        hx::ExecuteResult result{hx::HttpResponse{}};
        runtime_socket::NativeHandle handle{-1};

        // What this hop's socket would be pooled under, captured at connect
        // time because a redirect rewrites currentUrl before it is released.
        std::string poolHost{};
        std::uint16_t poolPort{0};
        bool https{false};
    };

    event_loop::HostReadinessBackend epoll_{};
    event_loop::EventLoop loop_;
    std::unique_ptr<runtime_socket::EpollSocketBackend> backend_{};
    std::unordered_map<runtime_socket::NativeHandle, InFlight*> byHandle_{};
    // Idle sockets the pool holds, so the loop's events for them route to the
    // pool rather than falling on the floor. `true` = the https pool.
    std::unordered_map<runtime_socket::NativeHandle, bool> pooledByHandle_{};
    std::vector<std::unique_ptr<InFlight>> inflight_{};
    AdmissionQueue<InFlight*> admission_{};
    // One pool per scheme, as bun keeps one HTTPContext per scheme
    // (HTTPContext.rs:19) — so ssl-ness is implied by which pool a socket is in
    // and is never a match dimension.
    ConnectionPool httpPool_{};
    ConnectionPool httpsPool_{};
    std::size_t maxRedirects_{10};

public:  // Big Five: owns an epoll fd, a loop and live sockets.
    explicit Engine(std::size_t maxSimultaneousRequests =
                        DEFAULT_MAX_SIMULTANEOUS_REQUESTS_FOR_BUN_INSTALL)
        : loop_{epoll_.seam()}, admission_{maxSimultaneousRequests} {
        runtime_socket::SocketEvents events{};
        events.on_open = [this](runtime_socket::NativeHandle h, bool) {
            if (InFlight* f{lookup_(h)}) {
                f->conn->on_open();
            }
        };
        events.on_data = [this](runtime_socket::NativeHandle h,
                                std::span<const std::byte> bytes) {
            if (InFlight* f{lookup_(h)}) {
                f->conn->on_data(bytes);
                return;
            }
            // Bytes on a socket nobody is using: a parked connection that the
            // peer decided to talk on (close_notify, a late chunk, garbage). It
            // can no longer be trusted to start a clean request.
            drop_pooled_(h, /*alreadyClosed=*/false);
        };
        events.on_writable = [](runtime_socket::NativeHandle) {
            // The backend drains its own write buffer before reporting writable
            // (epoll_socket_backend.cppm:471-494); the state machine hands it
            // whole buffers and never partial-writes, so there is nothing to do.
        };
        events.on_close = [this](runtime_socket::NativeHandle h) {
            if (InFlight* f{lookup_(h)}) {
                // The backend already deregistered and closed the fd.
                f->handle = -1;
                byHandle_.erase(h);
                f->conn->on_close();
                return;
            }
            // A parked socket the peer dropped. This is the common case against
            // a server that does not honour keep-alive, and it is why the pool
            // never has to trust that a slot is still good.
            drop_pooled_(h, /*alreadyClosed=*/true);
        };
        backend_ = std::make_unique<runtime_socket::EpollSocketBackend>(loop_, epoll_,
                                                                        std::move(events));
    }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;
    ~Engine() {
        // Parked sockets are live fds that nothing else owns; the backend is
        // still alive at this point, so hand them back before it goes.
        auto reap{[this](PooledSocket&& parked) { close_pooled_(std::move(parked)); }};
        httpPool_.drain(reap);
        httpsPool_.drain(reap);
    }

public:
    // Parked sockets are idle, not work: an install is finished when no request
    // is outstanding, even though the pool may still hold open connections.
    [[nodiscard]] bool has_work() const { return !inflight_.empty(); }
    [[nodiscard]] const PoolStats& http_pool_stats() const { return httpPool_.stats(); }
    [[nodiscard]] const PoolStats& https_pool_stats() const { return httpsPool_.stats(); }
    [[nodiscard]] std::size_t pooled_count() const { return httpPool_.size() + httpsPool_.size(); }
    // Requests handed to the engine and not yet completed (queued + started).
    [[nodiscard]] std::size_t inflight_count() const { return inflight_.size(); }
    // Requests with a live socket right now — never exceeds the budget.
    [[nodiscard]] std::size_t active_count() const { return admission_.active(); }
    [[nodiscard]] std::size_t max_simultaneous_requests() const {
        return admission_.max_simultaneous_requests();
    }
    [[nodiscard]] event_loop::EventLoop& loop() { return loop_; }

    // Hand a request to the loop. `done` fires on the loop thread once the
    // request reaches a terminal state (response, transport failure, or the idle
    // bound). Transport failures arrive as NetworkError so the caller can feed
    // classify_* with has_metadata=false, exactly like the blocking executor.
    void fetch(nt::RequestDescriptor request, ConnectionOptions options, Completion done) {
        auto entry{std::make_unique<InFlight>()};
        entry->request = std::move(request);
        entry->options = std::move(options);
        entry->currentUrl = entry->request.url;
        entry->origin = nt::parse_url(entry->request.url);
        entry->done = std::move(done);
        InFlight* raw{entry.get()};
        inflight_.push_back(std::move(entry));
        admission_.push(raw);
        pump_();
    }

    // One loop tick: admit what the budget allows, advance every started
    // request, then reap the finished ones (bun: drain_events + uws_loop.tick,
    // HTTPThread.rs:1373-1381).
    void run_once() {
        pump_();
        static_cast<void>(loop_.run_once(std::chrono::steady_clock::now()));
        reap_();
        admission_.end_drain();  // the next drain may halve the budget again
        pump_();                 // completions freed capacity
    }

    // Drive until every in-flight request has settled.
    void run() {
        while (has_work()) {
            run_once();
        }
    }

private:
    InFlight* lookup_(runtime_socket::NativeHandle handle) {
        const auto found{byHandle_.find(handle)};
        return found == byHandle_.end() ? nullptr : found->second;
    }

    // Start every request the budget currently allows (bun: the admission loop
    // in drain_events, HTTPThread.rs:856). Requests beyond it wait in the
    // admission queue; nothing is dropped.
    void pump_() {
        while (std::optional<InFlight*> next{admission_.admit()}) {
            begin_hop_(**next);
        }
    }

    // Resolve + start one hop. Credentials are dropped once a redirect leaves
    // the original origin (bun's client strips them on origin change).
    void begin_hop_(InFlight& entry) {
        nt::UrlParts url{nt::parse_url(entry.currentUrl)};
        const bool isHttp{nt::UrlParts::ascii_ieq(url.protocol, "http")};
        const bool isHttps{nt::UrlParts::ascii_ieq(url.protocol, "https")};
        if ((!isHttp && !isHttps) || url.hostname.empty()) {
            settle_(entry, std::unexpected(nt::NetworkError{
                               nt::NetworkErrorCode::InvalidURL,
                               "expected an http(s):// URL, got \"" + entry.currentUrl + "\""}));
            return;
        }

        const std::uint16_t port{static_cast<std::uint16_t>(
            url.port.empty() ? (isHttps ? 443 : 80) : std::stoi(std::string{url.port}))};

        const bool sameOrigin{nt::UrlParts::ascii_ieq(url.protocol, entry.origin.protocol) &&
                              nt::UrlParts::ascii_ieq(url.hostname, entry.origin.hostname) &&
                              url.effective_port() == entry.origin.effective_port()};
        std::vector<nt::Header> headers{};
        headers.reserve(entry.request.headers.size());
        for (const nt::Header& h : entry.request.headers) {
            if (!sameOrigin && (nt::UrlParts::ascii_ieq(h.name, "Authorization") ||
                                nt::UrlParts::ascii_ieq(h.name, "npm-auth-type"))) {
                continue;
            }
            headers.push_back(h);
        }

        entry.poolHost = url.hostname;
        entry.poolPort = port;
        entry.https = isHttps;
        entry.conn = std::make_unique<HttpConnection>(loop_, *backend_, entry.options);
        InFlight* raw{&entry};
        auto completion{[this, raw](hx::ExecuteResult result) {
            settle_(*raw, std::move(result));
        }};

        // A pooled socket is already connected and, for https, already
        // handshook — the whole cost this saves (bun: existing_socket,
        // HTTPContext.rs:701).
        ConnectionPool& pool{isHttps ? httpsPool_ : httpPool_};
        if (auto reused{pool.acquire(url.hostname, port, entry.options.tlsVerifyPeer,
                                     [this](PooledSocket&& dead) {
                                         close_pooled_(std::move(dead));
                                     })}) {
            pooledByHandle_.erase(reused->handle);
            entry.handle = reused->handle;
            byHandle_[entry.handle] = &entry;
            entry.conn->start_pooled(reused->handle, std::move(reused->tls), url,
                                     entry.currentUrl, headers, completion);
            return;
        }

        // Resolution only matters when actually dialling; a pool hit skips it.
        auto resolved{HostResolver::resolve(url.hostname, port)};
        if (!resolved) {
            settle_(entry, std::unexpected(nt::NetworkError{
                               nt::NetworkErrorCode::ResolveFailed, resolved.error().message}));
            return;
        }
        entry.conn->start(resolved->front(), url, entry.currentUrl, headers, completion);
        // start() may have settled synchronously (e.g. connect refused outright),
        // in which case there is no live fd to route.
        if (!entry.settled && entry.conn->handle() >= 0) {
            entry.handle = entry.conn->handle();
            byHandle_[entry.handle] = &entry;
        }
    }

    // Record a terminal result. Never destroys anything: this runs inside the
    // connection's own callback frame.
    void settle_(InFlight& entry, hx::ExecuteResult result) {
        if (entry.settled) {
            return;
        }
        entry.settled = true;
        entry.result = std::move(result);
        // Wake the loop so this tick ends and reap_() can run.
        //
        // EventLoop::run_once fires due timers BEFORE it computes its poll
        // timeout, so a result produced by a timer (an idle timeout — the one
        // case where nothing else is about to arrive on the socket) would leave
        // next_deadline() empty and send poll_once into an indefinite block: the
        // completion would not surface until some unrelated event happened to
        // wake it. post() bumps the backend's wakeup counter, which makes that
        // poll return immediately (epoll_backend.cppm: pending_wakeups → 0ms).
        static_cast<void>(loop_.post([] {}));
    }

    static bool is_redirect_status_(int status) {
        return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    }

    // Between ticks: follow redirects, deliver results, drop finished entries.
    void reap_() {
        for (std::size_t i{0}; i < inflight_.size();) {
            InFlight& entry{*inflight_[i]};
            if (!entry.settled) {
                ++i;
                continue;
            }
            if (try_redirect_(entry)) {
                ++i;  // re-armed for another hop; keep it in the list
                continue;
            }
            release_socket_(entry);
            // Halve the budget on the first *transport* failure of this drain.
            // bun gates this on `metadata.is_none()` alone (runTasks.rs:371) —
            // note that a 5xx is retried (:381-390) but does NOT throttle, so
            // this must not key off the retry decision.
            if (!entry.result) {
                admission_.note_network_error();
            }
            admission_.on_finished();
            Completion done{std::move(entry.done)};
            hx::ExecuteResult result{std::move(entry.result)};
            // Erase before the callback: it may enqueue more work, and it must
            // not observe a half-dead entry.
            inflight_.erase(inflight_.begin() + static_cast<std::ptrdiff_t>(i));
            if (done) {
                done(std::move(result));
            }
        }
    }

    // Returns true when `entry` was re-armed for another hop.
    bool try_redirect_(InFlight& entry) {
        if (!entry.result || !is_redirect_status_(entry.result->status)) {
            return false;
        }
        const std::string* location{hx::find_header(*entry.result, "Location")};
        if (location == nullptr || location->empty()) {
            return false;  // redirect without Location: surface it as-is
        }
        if (entry.hop >= maxRedirects_) {
            entry.result = std::unexpected(nt::NetworkError{
                nt::NetworkErrorCode::ProtocolError,
                "too many redirects fetching " + entry.request.url});
            return false;
        }
        std::string next{nt::url_join(entry.currentUrl, *location)};
        if (next.empty()) {
            entry.result = std::unexpected(
                nt::NetworkError{nt::NetworkErrorCode::InvalidURL,
                                 "invalid redirect Location \"" + *location + "\""});
            return false;
        }
        release_socket_(entry);
        ++entry.hop;
        entry.currentUrl = std::move(next);
        entry.settled = false;
        entry.result = hx::HttpResponse{};
        begin_hop_(entry);
        return true;
    }

    // Park the socket if the peer left it reusable, close it otherwise (bun:
    // release_socket, HTTPContext.rs:581 — which likewise falls through to
    // close_socket when the socket cannot be parked, :688-694).
    void release_socket_(InFlight& entry) {
        if (entry.handle < 0) {
            entry.conn.reset();
            return;
        }
        byHandle_.erase(entry.handle);
        if (entry.conn && entry.conn->connection_reusable()) {
            HttpConnection::DetachedSocket detached{entry.conn->detach()};
            if (pool_for_(entry.https)
                    .release(detached.handle, entry.poolHost, entry.poolPort,
                             entry.options.tlsVerifyPeer, std::move(detached.tls))) {
                pooledByHandle_[detached.handle] = entry.https;
                entry.handle = -1;
                entry.conn.reset();
                return;
            }
            // Refused (pool full, hostname too long, socket already bad).
            backend_->close(detached.handle);
            entry.handle = -1;
            entry.conn.reset();
            return;
        }
        backend_->close(entry.handle);
        entry.handle = -1;
        entry.conn.reset();
    }

    ConnectionPool& pool_for_(bool https) { return https ? httpsPool_ : httpPool_; }

    // Give up a parked socket. `alreadyClosed` distinguishes the loop telling us
    // the fd is gone (on_close: the backend already closed and deregistered it)
    // from us deciding to drop it (on_data: still ours to close).
    void drop_pooled_(runtime_socket::NativeHandle handle, bool alreadyClosed) {
        const auto found{pooledByHandle_.find(handle)};
        if (found == pooledByHandle_.end()) {
            return;
        }
        const bool https{found->second};
        pooledByHandle_.erase(found);
        if (auto dead{pool_for_(https).evict(handle)}; dead && !alreadyClosed) {
            backend_->close(dead->handle);
        }
    }

    void close_pooled_(PooledSocket&& dead) {
        pooledByHandle_.erase(dead.handle);
        backend_->close(dead.handle);
    }
};

}  // namespace mbun::install::async_http
