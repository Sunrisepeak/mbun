// test_async_http.cpp — mbun.install.async_http (concurrent executor) tests.
//
// Covers src/async_http/ — the event-loop-driven HTTP path that mirrors bun's
// dedicated "HTTP Client" thread (HTTPThread.rs:1247) multiplexing every
// in-flight request over one uSockets loop.
//
// Covers:
//   - the host resolver (src/async_http/resolver.cppm) — numeric address
//     rendering plus the resolve-once-per-host cache that stands in for what bun
//     gets from uSockets + its keep-alive pool (HTTPContext.rs:701);
//   - the connection state machine + engine end-to-end against an in-process
//     127.0.0.1 server (no external network): framing, redirects, transport
//     failures, and — the whole point — that requests actually overlap.

#if defined(__linux__)
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

import std;
import mbun.install.async_http;
// The engine reports results as http_executor::ExecuteResult so both network
// paths feed the identical classify_* gate; the tests name that type directly.
import mbun.install.http_executor;
import mbun.install.network_task;
import mbun.runtime_socket;
import mbun.tls;

namespace ah = mbun::install::async_http;
namespace hx = mbun::install::http_executor;
namespace nt = mbun::install::network_task;
namespace rs = mbun::runtime_socket;

namespace {

int gChecks{0};
int gFailures{0};

void check_true(bool cond, std::string_view what) {
    ++gChecks;
    if (!cond) {
        ++gFailures;
        std::println("  FAIL {}", what);
    }
}

template <class A, class B>
void check_eq(const A& a, const B& b, std::string_view what) {
    ++gChecks;
    if (!(a == b)) {
        ++gFailures;
        if constexpr (std::is_convertible_v<A, std::string_view> &&
                      std::is_convertible_v<B, std::string_view>) {
            std::println("  FAIL {}: got \"{}\", expected \"{}\"", what, std::string_view{a},
                         std::string_view{b});
        } else {
            std::println("  FAIL {}", what);
        }
    }
}

// A numeric literal must round-trip to exactly one address carrying the port.
// The epoll backend feeds Address::host() to inet_pton, so a name must never
// survive resolution.
void test_resolver_numeric_literal() {
    auto resolved{ah::HostResolver::resolve("127.0.0.1", 8080)};
    check_true(resolved.has_value(), "resolver: 127.0.0.1 resolves");
    if (!resolved) {
        std::println("  (error: {})", resolved.error().message);
        return;
    }
    check_eq(resolved->size(), std::size_t{1}, "resolver: one row for a v4 literal");
    if (resolved->empty()) {
        return;
    }
    const rs::Address& address{resolved->front()};
    check_true(address.family() == rs::AddressFamily::ipv4, "resolver: family is ipv4");
    check_eq(std::string{address.host()}, std::string{"127.0.0.1"},
             "resolver: host is numeric text");
    check_eq(address.port(), std::uint16_t{8080}, "resolver: port carried through");
}

void test_resolver_ipv6_literal() {
    auto resolved{ah::HostResolver::resolve("::1", 443)};
    check_true(resolved.has_value(), "resolver: ::1 resolves");
    if (!resolved || resolved->empty()) {
        return;
    }
    const rs::Address& address{resolved->front()};
    check_true(address.family() == rs::AddressFamily::ipv6, "resolver: family is ipv6");
    check_eq(std::string{address.host()}, std::string{"::1"}, "resolver: v6 numeric text");
    check_eq(address.port(), std::uint16_t{443}, "resolver: v6 port carried through");
}

// The whole point of the cache: however many requests an install issues against
// one host, the resolver is consulted once.
void test_resolver_caches_per_host() {
    ah::ResolverStats before{ah::HostResolver::stats()};
    for (int i{0}; i < 4; ++i) {
        auto resolved{ah::HostResolver::resolve("127.0.0.2", 9001)};
        check_true(resolved.has_value(), "resolver-cache: resolve succeeded");
    }
    ah::ResolverStats after{ah::HostResolver::stats()};
    check_eq(after.lookups - before.lookups, std::uint64_t{1},
             "resolver-cache: four resolutions of one host issue one getaddrinfo");
    check_eq(after.hits - before.hits, std::uint64_t{3},
             "resolver-cache: the rest are served from the cache");
}

// Distinct ports are distinct cache keys: a pooled connection is only reusable
// for the same port (HTTPContext.rs:701 matches port before hostname).
void test_resolver_port_is_part_of_key() {
    ah::ResolverStats before{ah::HostResolver::stats()};
    check_true(ah::HostResolver::resolve("127.0.0.3", 1111).has_value(),
               "resolver-key: port 1111 resolves");
    check_true(ah::HostResolver::resolve("127.0.0.3", 2222).has_value(),
               "resolver-key: port 2222 resolves");
    ah::ResolverStats after{ah::HostResolver::stats()};
    check_eq(after.lookups - before.lookups, std::uint64_t{2},
             "resolver-key: same host on two ports is two cache entries");
}

// A resolver failure must stay retryable, so it is never cached: bun treats a
// resolve failure as `metadata.is_none()` and retries it (runTasks.rs:381).
void test_resolver_does_not_cache_failure() {
    ah::ResolverStats before{ah::HostResolver::stats()};
    auto first{ah::HostResolver::resolve("invalid.host.mbun.test.invalid", 80)};
    auto second{ah::HostResolver::resolve("invalid.host.mbun.test.invalid", 80)};
    ah::ResolverStats after{ah::HostResolver::stats()};
    check_true(!first.has_value(), "resolver-fail: unresolvable host reports an error");
    check_true(!second.has_value(), "resolver-fail: still an error on the retry");
    check_eq(after.lookups - before.lookups, std::uint64_t{2},
             "resolver-fail: failures are re-resolved, not cached");
    check_eq(after.hits - before.hits, std::uint64_t{0}, "resolver-fail: no cache hit served");
}

#if defined(__linux__)

// ── in-process HTTP server ──────────────────────────────────────────────────
// Unlike test_http_executor's TestServer (which serves one connection at a
// time), this one handles every accepted connection on its own thread and can
// hold each response back by `delay`. That is what makes overlap observable: N
// concurrent requests against a `delay`-per-response server finish in ~delay if
// they are multiplexed, and in ~N×delay if they are serialized.
class ConcurrentTestServer {
private:
    int listenFd_{-1};
    std::uint16_t port_{0};
    std::string response_{};
    std::chrono::milliseconds delay_{0};
    std::atomic<int> accepted_{0};
    std::atomic<bool> stop_{false};
    std::thread acceptor_{};
    std::vector<std::thread> workers_{};
    std::mutex mutex_{};

public:
    ConcurrentTestServer(std::string response, std::chrono::milliseconds delay)
        : response_{std::move(response)}, delay_{delay} {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one{1};
        (void)::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        ::sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listenFd_, reinterpret_cast<::sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(listenFd_, 64) != 0) {
            ::close(listenFd_);
            listenFd_ = -1;
            return;
        }
        ::socklen_t len{sizeof addr};
        (void)::getsockname(listenFd_, reinterpret_cast<::sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::timeval tv{5, 0};  // bounded accept so a broken test cannot hang
        (void)::setsockopt(listenFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        acceptor_ = std::thread{[this] { accept_loop_(); }};
    }

    ConcurrentTestServer(const ConcurrentTestServer&) = delete;
    ConcurrentTestServer& operator=(const ConcurrentTestServer&) = delete;

    ~ConcurrentTestServer() {
        stop_.store(true);
        // Break the acceptor out of accept() instead of waiting for its receive
        // timeout, and let any delaying worker notice stop_ — otherwise teardown
        // would cost seconds per server and dominate the suite's runtime.
        if (listenFd_ >= 0) {
            ::shutdown(listenFd_, SHUT_RDWR);
        }
        if (acceptor_.joinable()) {
            acceptor_.join();
        }
        std::scoped_lock lock{mutex_};
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        if (listenFd_ >= 0) {
            ::close(listenFd_);
        }
    }

    bool ok() const { return listenFd_ >= 0; }
    std::uint16_t port() const { return port_; }
    int accepted() const { return accepted_.load(); }
    std::string url(std::string_view path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string{path};
    }

private:
    void accept_loop_() {
        while (!stop_.load()) {
            const int fd{::accept(listenFd_, nullptr, nullptr)};
            if (fd < 0) {
                return;  // accept timeout / shutdown
            }
            accepted_.fetch_add(1);
            std::scoped_lock lock{mutex_};
            workers_.emplace_back([this, fd] { serve_(fd); });
        }
    }

    void serve_(int fd) {
        std::string request;
        char buf[8192];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const ::ssize_t n{::recv(fd, buf, sizeof buf, 0)};
            if (n <= 0) {
                ::close(fd);
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }
        // Interruptible: a long stall (the idle-timeout test holds a response
        // for 30s) must not keep teardown waiting once the test is done.
        for (std::chrono::milliseconds waited{0}; waited < delay_ && !stop_.load();
             waited += std::chrono::milliseconds{10}) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        if (stop_.load()) {
            ::close(fd);
            return;
        }
        std::size_t sent{0};
        while (sent < response_.size()) {
            const ::ssize_t n{::send(fd, response_.data() + sent, response_.size() - sent, 0)};
            if (n <= 0) {
                break;
            }
            sent += static_cast<std::size_t>(n);
        }
        ::close(fd);
    }
};

nt::RequestDescriptor make_request(std::string url) {
    nt::RequestDescriptor d{};
    d.kind = nt::RequestKind::PackageManifest;
    d.url = std::move(url);
    d.headers.push_back({"Accept", std::string{nt::ACCEPT_HEADER_VALUE}});
    return d;
}

std::string ok_response(std::string_view body) {
    return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
           std::string{body};
}

// Drive one request to completion on the engine.
std::optional<hx::ExecuteResult> fetch_one(ah::Engine& engine,
                                                                     std::string url) {
    std::optional<hx::ExecuteResult> out{};
    engine.fetch(make_request(std::move(url)), ah::ConnectionOptions{},
                 [&out](hx::ExecuteResult r) { out = std::move(r); });
    engine.run();
    return out;
}

std::string body_str(const hx::HttpResponse& r) {
    return std::string{reinterpret_cast<const char*>(r.body.data()), r.body.size()};
}

// ── engine e2e ──────────────────────────────────────────────────────────────

void test_engine_content_length() {
    ConcurrentTestServer server{ok_response("{\"name\":\"left-pad\"}"), std::chrono::milliseconds{0}};
    check_true(server.ok(), "engine-cl: server started");
    ah::Engine engine{};
    auto result{fetch_one(engine, server.url("/left-pad"))};
    check_true(result.has_value(), "engine-cl: completion fired");
    if (!result || !result->has_value()) {
        if (result && !result->has_value()) {
            std::println("  (error: {})", result->error().message);
        }
        return;
    }
    check_eq((*result)->status, 200, "engine-cl: status");
    check_eq(body_str(**result), std::string{"{\"name\":\"left-pad\"}"}, "engine-cl: body");
}

void test_engine_chunked() {
    ConcurrentTestServer server{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                                "5\r\nhello\r\n7\r\n chunky\r\n0\r\n\r\n",
                                std::chrono::milliseconds{0}};
    check_true(server.ok(), "engine-chunked: server started");
    ah::Engine engine{};
    auto result{fetch_one(engine, server.url("/pkg"))};
    check_true(result.has_value() && result->has_value(), "engine-chunked: succeeded");
    if (!result || !result->has_value()) {
        return;
    }
    check_eq(body_str(**result), std::string{"hello chunky"}, "engine-chunked: decoded body");
}

// A 4xx must arrive as a *response*, not a transport error: classify_* treats it
// as terminal only because it has metadata (runTasks.rs:467-475).
void test_engine_404_has_metadata() {
    ConcurrentTestServer server{"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n",
                                std::chrono::milliseconds{0}};
    check_true(server.ok(), "engine-404: server started");
    ah::Engine engine{};
    auto result{fetch_one(engine, server.url("/nope"))};
    check_true(result.has_value() && result->has_value(),
               "engine-404: 4xx surfaces as a response, not an error");
    if (result && result->has_value()) {
        check_eq((*result)->status, 404, "engine-404: status");
    }
}

// No listener on the port → the connect must fail cleanly (and be reported as a
// transport error, i.e. metadata.is_none(), so the retry gate can see it).
void test_engine_connect_refused() {
    ah::Engine engine{};
    // Port 1 on loopback: reserved, never listening.
    auto result{fetch_one(engine, "http://127.0.0.1:1/pkg")};
    check_true(result.has_value(), "engine-refused: completion fired");
    if (!result) {
        return;
    }
    check_true(!result->has_value(), "engine-refused: reported as a transport error");
    if (!result->has_value()) {
        check_true(result->error().code == nt::NetworkErrorCode::ConnectFailed,
                   "engine-refused: ConnectFailed");
    }
}

void test_engine_redirect() {
    ConcurrentTestServer target{ok_response("tarball"), std::chrono::milliseconds{0}};
    check_true(target.ok(), "engine-redirect: target started");
    ConcurrentTestServer source{"HTTP/1.1 302 Found\r\nLocation: " + target.url("/final") +
                                    "\r\nContent-Length: 0\r\n\r\n",
                                std::chrono::milliseconds{0}};
    check_true(source.ok(), "engine-redirect: source started");
    ah::Engine engine{};
    auto result{fetch_one(engine, source.url("/start"))};
    check_true(result.has_value() && result->has_value(), "engine-redirect: succeeded");
    if (!result || !result->has_value()) {
        return;
    }
    check_eq((*result)->status, 200, "engine-redirect: followed to final status");
    check_eq(body_str(**result), std::string{"tarball"}, "engine-redirect: final body");
}

// The reason this whole subsystem exists. The blocking executor issues requests
// strictly end-to-end (measured: 12 connects, none overlapping, ~1.8s each). On
// one loop, N requests against a server that holds every response for `delay`
// must finish in about one `delay`, not N of them.
void test_engine_requests_overlap() {
    constexpr int REQUESTS{8};
    constexpr std::chrono::milliseconds DELAY{200};
    ConcurrentTestServer server{ok_response("payload"), DELAY};
    check_true(server.ok(), "engine-overlap: server started");

    ah::Engine engine{};
    int completed{0};
    int okCount{0};
    for (int i{0}; i < REQUESTS; ++i) {
        engine.fetch(make_request(server.url("/pkg" + std::to_string(i))), ah::ConnectionOptions{},
                     [&completed, &okCount](hx::ExecuteResult r) {
                         ++completed;
                         if (r.has_value() && r->status == 200) {
                             ++okCount;
                         }
                     });
    }
    // Every request is in flight before the loop turns: they were all handed to
    // the engine up front, exactly as bun schedules a whole batch at once
    // (schedule_tasks, runTasks.rs:1655-1678).
    check_eq(engine.inflight_count(), std::size_t{REQUESTS},
             "engine-overlap: all requests are in flight together");

    const auto start{std::chrono::steady_clock::now()};
    engine.run();
    const auto elapsed{std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start)};

    check_eq(completed, REQUESTS, "engine-overlap: every request completed");
    check_eq(okCount, REQUESTS, "engine-overlap: every request returned 200");
    check_eq(server.accepted(), REQUESTS, "engine-overlap: server saw every connection");
    // Serial would be 8×200ms = 1600ms. Concurrent is ~200ms; allow generous
    // slack for scheduling so this asserts "overlapped", not a stopwatch.
    check_true(elapsed < std::chrono::milliseconds{1000},
               "engine-overlap: wall clock is far below the serial 8x200ms");
    std::println("  (engine-overlap: {} requests in {}ms; serial would be {}ms)", REQUESTS,
                 elapsed.count(), REQUESTS * DELAY.count());
}

// ── keep-alive pool ─────────────────────────────────────────────────────────
// Serves many requests on one connection. Every other server here closes after
// responding, against which the pool can only ever evict — so reuse is not
// observable without a peer that actually honours keep-alive.
class KeepAliveTestServer {
private:
    int listenFd_{-1};
    std::uint16_t port_{0};
    std::string response_{};
    std::atomic<int> accepted_{0};
    std::atomic<int> served_{0};
    std::atomic<bool> stop_{false};
    std::thread acceptor_{};
    std::vector<std::thread> workers_{};
    std::mutex mutex_{};

public:
    explicit KeepAliveTestServer(std::string response) : response_{std::move(response)} {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one{1};
        (void)::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        ::sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listenFd_, reinterpret_cast<::sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(listenFd_, 64) != 0) {
            ::close(listenFd_);
            listenFd_ = -1;
            return;
        }
        ::socklen_t len{sizeof addr};
        (void)::getsockname(listenFd_, reinterpret_cast<::sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::timeval tv{5, 0};
        (void)::setsockopt(listenFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        acceptor_ = std::thread{[this] { accept_loop_(); }};
    }

    KeepAliveTestServer(const KeepAliveTestServer&) = delete;
    KeepAliveTestServer& operator=(const KeepAliveTestServer&) = delete;

    ~KeepAliveTestServer() {
        stop_.store(true);
        if (listenFd_ >= 0) {
            ::shutdown(listenFd_, SHUT_RDWR);
        }
        if (acceptor_.joinable()) {
            acceptor_.join();
        }
        std::scoped_lock lock{mutex_};
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        if (listenFd_ >= 0) {
            ::close(listenFd_);
        }
    }

    bool ok() const { return listenFd_ >= 0; }
    int accepted() const { return accepted_.load(); }
    int served() const { return served_.load(); }
    std::string url(std::string_view path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string{path};
    }

private:
    void accept_loop_() {
        while (!stop_.load()) {
            const int fd{::accept(listenFd_, nullptr, nullptr)};
            if (fd < 0) {
                return;
            }
            accepted_.fetch_add(1);
            std::scoped_lock lock{mutex_};
            workers_.emplace_back([this, fd] { serve_(fd); });
        }
    }

    // Read request / write response, repeatedly, until the peer goes away.
    void serve_(int fd) {
        // Bounded so a worker cannot outlive the test if the client simply
        // parks the socket and never speaks again.
        ::timeval tv{2, 0};
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        std::string buffer;
        char buf[8192];
        for (;;) {
            while (buffer.find("\r\n\r\n") == std::string::npos) {
                const ::ssize_t n{::recv(fd, buf, sizeof buf, 0)};
                if (n == 0 || stop_.load()) {
                    ::close(fd);  // peer hung up, or teardown
                    return;
                }
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        continue;  // idle: the client is holding this open
                    }
                    ::close(fd);
                    return;
                }
                buffer.append(buf, static_cast<std::size_t>(n));
            }
            buffer.erase(0, buffer.find("\r\n\r\n") + 4);
            served_.fetch_add(1);
            std::size_t sent{0};
            while (sent < response_.size()) {
                const ::ssize_t n{::send(fd, response_.data() + sent, response_.size() - sent, 0)};
                if (n <= 0) {
                    ::close(fd);
                    return;
                }
                sent += static_cast<std::size_t>(n);
            }
        }
    }
};

// The pool's whole reason to exist: a later request to the same host:port speaks
// on the earlier one's socket instead of dialling again (bun: existing_socket,
// HTTPContext.rs:701).
//
// Sequential on purpose. A socket becomes reusable only once released, so this
// is the shape that exercises reuse; concurrent requests each dial, which is
// correct and is what test_engine_requests_overlap covers.
void test_pool_reuses_connection() {
    KeepAliveTestServer server{ok_response("payload")};
    check_true(server.ok(), "pool-reuse: server started");
    if (!server.ok()) {
        return;
    }
    ah::Engine engine{};
    constexpr int REQUESTS{4};
    int completed{0};
    int okCount{0};
    for (int i{0}; i < REQUESTS; ++i) {
        engine.fetch(make_request(server.url("/pkg")), ah::ConnectionOptions{},
                     [&](hx::ExecuteResult result) {
                         ++completed;
                         if (result && result->status == 200) {
                             ++okCount;
                         }
                     });
        engine.run();
    }

    check_eq(completed, REQUESTS, "pool-reuse: every request completed");
    check_eq(okCount, REQUESTS, "pool-reuse: every request returned 200");
    check_eq(server.served(), REQUESTS, "pool-reuse: the server answered every request");
    // The assertion that matters: four requests, one TCP connection.
    check_eq(server.accepted(), 1, "pool-reuse: four requests cost one connection");
    const ah::PoolStats& stats{engine.http_pool_stats()};
    check_eq(stats.reused, std::uint64_t{REQUESTS - 1},
             "pool-reuse: every request after the first reused a parked socket");
    check_eq(engine.pooled_count(), std::size_t{1},
             "pool-reuse: the socket stays parked for the next request");
    std::println("  (pool-reuse: {} requests over {} connection(s); {} reused)", REQUESTS,
                 server.accepted(), stats.reused);
}

// A peer that closes after responding must not poison the next request. This is
// the ordinary case against ConcurrentTestServer, and the reason the pool probes
// liveness at both ends instead of trusting a slot (HTTPContext.rs:788-798).
void test_pool_does_not_reuse_a_closed_socket() {
    ConcurrentTestServer server{ok_response("payload"), std::chrono::milliseconds{0}};
    check_true(server.ok(), "pool-dead: server started");
    if (!server.ok()) {
        return;
    }
    ah::Engine engine{};
    constexpr int REQUESTS{3};
    int okCount{0};
    for (int i{0}; i < REQUESTS; ++i) {
        engine.fetch(make_request(server.url("/pkg")), ah::ConnectionOptions{},
                     [&](hx::ExecuteResult result) {
                         if (result && result->status == 200) {
                             ++okCount;
                         }
                     });
        engine.run();
    }
    // Each request must succeed on its own fresh connection: a socket the peer
    // closed is never handed to a later request.
    check_eq(okCount, REQUESTS, "pool-dead: every request still succeeded");
    check_eq(server.accepted(), REQUESTS, "pool-dead: a closing peer forces a new connection");
    check_eq(engine.http_pool_stats().reused, std::uint64_t{0},
             "pool-dead: nothing was ever reused from a peer that closes");
}

// Port and hostname are match dimensions (HTTPContext.rs:701 checks port before
// hostname), so a socket parked for one origin must never serve another.
void test_pool_does_not_cross_origins() {
    KeepAliveTestServer first{ok_response("first")};
    KeepAliveTestServer second{ok_response("second")};
    check_true(first.ok() && second.ok(), "pool-origin: both servers started");
    if (!first.ok() || !second.ok()) {
        return;
    }
    ah::Engine engine{};
    std::vector<std::string> bodies{};
    auto get{[&](const std::string& url) {
        engine.fetch(make_request(url), ah::ConnectionOptions{},
                     [&](hx::ExecuteResult result) {
                         if (result) {
                             bodies.emplace_back(reinterpret_cast<const char*>(result->body.data()),
                                                 result->body.size());
                         }
                     });
        engine.run();
    }};
    get(first.url("/a"));
    get(second.url("/a"));  // different port: must not take the parked socket
    get(first.url("/b"));   // back to the first: may reuse

    check_eq(bodies.size(), std::size_t{3}, "pool-origin: three responses");
    if (bodies.size() == 3) {
        check_eq(bodies[0], std::string{"first"}, "pool-origin: first server answered");
        check_eq(bodies[1], std::string{"second"}, "pool-origin: second server answered itself");
        check_eq(bodies[2], std::string{"first"}, "pool-origin: reuse went back to the right host");
    }
    check_eq(first.accepted(), 1, "pool-origin: the first host was dialled once");
    check_eq(second.accepted(), 1, "pool-origin: the second host was dialled once");
    check_eq(engine.pooled_count(), std::size_t{2}, "pool-origin: both sockets are parked");
}

// ── https ───────────────────────────────────────────────────────────────────
// Server-role TlsChannel over one accepted connection, serving a canned response
// on the encrypted app edge. Proves the *async* client handshake: the same
// memory-BIO shuttle as the blocking executor, but fed by on_data instead of
// blocking recv.
class TlsTestServer {
private:
    int listenFd_{-1};
    std::uint16_t port_{0};
    std::string cert_;
    std::string key_;
    std::string response_;
    std::string request_;
    std::mutex mutex_;
    std::thread thread_;

public:
    TlsTestServer(std::string cert, std::string key, std::string response)
        : cert_{std::move(cert)}, key_{std::move(key)}, response_{std::move(response)} {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one{1};
        (void)::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        ::sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listenFd_, reinterpret_cast<::sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(listenFd_, 8) != 0) {
            ::close(listenFd_);
            listenFd_ = -1;
            return;
        }
        ::socklen_t len{sizeof addr};
        (void)::getsockname(listenFd_, reinterpret_cast<::sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::timeval tv{10, 0};
        (void)::setsockopt(listenFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        thread_ = std::thread{[this] { serve_(); }};
    }

    TlsTestServer(const TlsTestServer&) = delete;
    TlsTestServer& operator=(const TlsTestServer&) = delete;

    ~TlsTestServer() {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listenFd_ >= 0) {
            ::close(listenFd_);
        }
    }

    bool ok() const { return listenFd_ >= 0; }
    std::uint16_t port() const { return port_; }
    std::string request() {
        std::scoped_lock lock{mutex_};
        return request_;
    }

private:
    static bool flush_(int fd, mbun::tls::TlsChannel& ch) {
        while (ch.has_encrypted()) {
            std::vector<std::uint8_t> enc{ch.take_encrypted()};
            std::size_t sent{0};
            while (sent < enc.size()) {
                const ::ssize_t n{::send(fd, enc.data() + sent, enc.size() - sent, 0)};
                if (n <= 0) {
                    return false;
                }
                sent += static_cast<std::size_t>(n);
            }
        }
        return true;
    }

    void serve_() {
        const int fd{::accept(listenFd_, nullptr, nullptr)};
        if (fd < 0) {
            return;
        }
        ::timeval tv{10, 0};
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

        mbun::tls::Config cfg{};
        cfg.certificate = cert_;
        cfg.key = key_;
        cfg.verify = mbun::tls::VerifyMode::disabled;
        mbun::tls::TlsChannel ch{mbun::tls::TlsRole::server, cfg};

        char tmp[16 * 1024];
        while (!ch.established()) {
            ch.handshake();
            if (!flush_(fd, ch)) {
                ::close(fd);
                return;
            }
            if (ch.established()) {
                break;
            }
            if (ch.handshake_state() == mbun::tls::HandshakeState::failed) {
                ::close(fd);
                return;
            }
            const ::ssize_t n{::recv(fd, tmp, sizeof tmp, 0)};
            if (n <= 0) {
                ::close(fd);
                return;
            }
            ch.feed_encrypted(
                {reinterpret_cast<const std::uint8_t*>(tmp), static_cast<std::size_t>(n)});
        }

        std::string req;
        while (req.find("\r\n\r\n") == std::string::npos) {
            std::vector<std::uint8_t> dec{ch.read()};
            if (!dec.empty()) {
                req.append(reinterpret_cast<const char*>(dec.data()), dec.size());
                continue;
            }
            if (ch.want() != mbun::tls::IoWant::read) {
                break;
            }
            const ::ssize_t n{::recv(fd, tmp, sizeof tmp, 0)};
            if (n <= 0) {
                break;
            }
            ch.feed_encrypted(
                {reinterpret_cast<const std::uint8_t*>(tmp), static_cast<std::size_t>(n)});
        }
        {
            std::scoped_lock lock{mutex_};
            request_ = std::move(req);
        }

        ch.write({reinterpret_cast<const std::uint8_t*>(response_.data()), response_.size()});
        (void)flush_(fd, ch);
        ch.shutdown();
        (void)flush_(fd, ch);
        ::close(fd);
    }
};

void test_engine_https_get() {
    auto pem{mbun::tls::make_self_signed("localhost")};
    check_true(pem.has_value(), "engine-https: self-signed cert generated");
    if (!pem) {
        return;
    }
    const std::string body{R"({"name":"is-number","dist-tags":{"latest":"7.0.0"}})"};
    TlsTestServer server{pem->cert, pem->key,
                         "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                             "\r\n\r\n" + body};
    check_true(server.ok(), "engine-https: server started");

    ah::Engine engine{};
    ah::ConnectionOptions options{};
    options.tlsVerifyPeer = false;  // accept the self-signed cert without a CA

    std::optional<hx::ExecuteResult> out{};
    engine.fetch(make_request("https://127.0.0.1:" + std::to_string(server.port()) + "/is-number"),
                 options, [&out](hx::ExecuteResult r) { out = std::move(r); });
    engine.run();

    check_true(out.has_value() && out->has_value(), "engine-https: GET succeeded over TLS");
    if (!out || !out->has_value()) {
        if (out && !out->has_value()) {
            std::println("  (error: {})", out->error().message);
        }
        return;
    }
    check_eq((*out)->status, 200, "engine-https: status");
    check_eq(body_str(**out), body, "engine-https: body decrypted intact");
    check_true(server.request().starts_with("GET /is-number HTTP/1.1\r\n"),
               "engine-https: server saw the plaintext GET");
    // bun sends keep-alive on install requests (lib.rs:2189); the blocking
    // executor still sends Connection: close.
    check_true(server.request().find("Connection: keep-alive\r\n") != std::string::npos,
               "engine-https: request advertises keep-alive");
}

// ── admission budget ────────────────────────────────────────────────────────

// The values are bun's, copied not invented (PackageManager.rs:335-336,
// AsyncHTTP.rs:86, PackageManagerOptions.rs:131).
// normalize_idle_timeout_seconds, ported from src/http/lib.rs:296-304. The
// rounding exists for uSockets' minute-granularity long timer; mbun's timer has
// no such granularity, but BUN_CONFIG_HTTP_IDLE_TIMEOUT is an observable knob,
// so the arithmetic must still agree with bun's.
void test_idle_timeout_normalization() {
    check_eq(ah::IDLE_TIMEOUT_SECONDS, std::uint32_t{300}, "idle: the default is 300s");
    // 0 disables the timer and must survive normalization intact.
    check_eq(ah::normalize_idle_timeout_seconds(0), std::uint32_t{0}, "idle: 0 stays 0");
    // <= 240 is served by the short timer: passed through exactly.
    check_eq(ah::normalize_idle_timeout_seconds(3), std::uint32_t{3}, "idle: 3s is exact");
    check_eq(ah::normalize_idle_timeout_seconds(240), std::uint32_t{240},
             "idle: 240s is the last exact value");
    // > 240 rounds UP to a whole minute — never down, which would fire early.
    check_eq(ah::normalize_idle_timeout_seconds(241), std::uint32_t{300},
             "idle: 241s rounds up to a whole minute");
    check_eq(ah::normalize_idle_timeout_seconds(300), std::uint32_t{300},
             "idle: the 300s default is already whole-minute");
    check_eq(ah::normalize_idle_timeout_seconds(301), std::uint32_t{360},
             "idle: 301s rounds up, not down");
    // The long-timeout counter wraps % 240 minutes, so 239 min is the ceiling.
    check_eq(ah::normalize_idle_timeout_seconds(239 * 60), std::uint32_t{239 * 60},
             "idle: 239 minutes is the ceiling");
    check_eq(ah::normalize_idle_timeout_seconds(1'000'000), std::uint32_t{239 * 60},
             "idle: anything larger clamps to 239 minutes");
}

void test_scheduler_constants() {
    check_eq(ah::DEFAULT_MAX_SIMULTANEOUS_REQUESTS_FOR_BUN_INSTALL, std::size_t{64},
             "budget: bun install runs at 64");
    check_eq(ah::DEFAULT_MAX_SIMULTANEOUS_REQUESTS_FOR_BUN_INSTALL_FOR_PROXIES, std::size_t{64},
             "budget: the proxy default is the same 64");
    check_eq(ah::MAX_SIMULTANEOUS_REQUESTS, std::size_t{256},
             "budget: the generic HTTP default is 256");
    check_eq(ah::MIN_SIMULTANEOUS_REQUESTS, std::size_t{4}, "budget: the halving floor is 4");
    // A fresh engine must run at bun install's value, not the generic default.
    ah::Engine engine{};
    check_eq(engine.max_simultaneous_requests(), std::size_t{64},
             "budget: a default engine runs at 64");
}

// Precedence is CLI → proxy → default (PackageManager.rs:2200-2211).
void test_scheduler_precedence() {
    check_eq(ah::resolve_max_simultaneous_requests(std::nullopt, false, std::nullopt),
             std::size_t{64}, "precedence: bare default is 64");
    check_eq(ah::resolve_max_simultaneous_requests(std::nullopt, true, std::nullopt),
             std::size_t{64}, "precedence: proxy default is 64");
    check_eq(ah::resolve_max_simultaneous_requests(std::size_t{7}, false, std::nullopt),
             std::size_t{7}, "precedence: --network-concurrency wins");
    check_eq(ah::resolve_max_simultaneous_requests(std::size_t{7}, true, std::size_t{9}),
             std::size_t{7}, "precedence: CLI outranks both proxy and env");
    check_eq(ah::resolve_max_simultaneous_requests(std::nullopt, false, std::size_t{9}),
             std::size_t{9}, "precedence: env applies when no CLI value");
    // max(n, 1) — PackageManager.rs:2202.
    check_eq(ah::resolve_max_simultaneous_requests(std::size_t{0}, false, std::nullopt),
             std::size_t{1}, "precedence: --network-concurrency=0 clamps to 1");
}

// BUN_CONFIG_MAX_HTTP_REQUESTS is u16 1..65535; 0 and junk warn and are ignored
// rather than failing the install (AsyncHTTP.rs:229-257).
void test_scheduler_env_parse() {
    check_eq(ah::parse_max_http_requests_env("32").value_or(0), std::size_t{32},
             "env: parses a plain number");
    check_true(!ah::parse_max_http_requests_env("0").has_value(), "env: 0 is ignored");
    check_true(!ah::parse_max_http_requests_env("").has_value(), "env: empty is unset");
    check_true(!ah::parse_max_http_requests_env("abc").has_value(), "env: junk is ignored");
    check_true(!ah::parse_max_http_requests_env("70000").has_value(),
               "env: above u16 range is ignored");
    check_eq(ah::parse_max_http_requests_env("65535").value_or(0), std::size_t{65535},
             "env: u16 max is accepted");
}

// Deferred-before-queued (HTTPThread.rs:113): a request parked at capacity must
// not be starved by later arrivals.
void test_scheduler_admission_order() {
    ah::AdmissionQueue<int> queue{2};
    for (int i{1}; i <= 4; ++i) {
        queue.push(i);
    }
    check_eq(queue.admit().value_or(-1), 1, "admit: first");
    check_eq(queue.admit().value_or(-1), 2, "admit: second");
    check_true(!queue.admit().has_value(), "admit: capacity 2 blocks the third");
    check_eq(queue.active(), std::size_t{2}, "admit: two active");
    check_eq(queue.pending(), std::size_t{2}, "admit: two parked, none dropped");
    queue.on_finished();
    // 3 and 4 were parked as deferred; they must come back in FIFO order.
    check_eq(queue.admit().value_or(-1), 3, "admit: deferred drains in FIFO order");
    queue.push(9);
    queue.on_finished();
    check_eq(queue.admit().value_or(-1), 4,
             "admit: deferred outranks a newly queued request");
}

// First transport error of a drain halves toward the floor; later errors in the
// same drain do not compound (runTasks.rs:370-378).
void test_scheduler_halves_once_per_drain() {
    ah::AdmissionQueue<int> queue{64};
    queue.note_network_error();
    check_eq(queue.max_simultaneous_requests(), std::size_t{32}, "halve: 64 → 32");
    queue.note_network_error();
    check_eq(queue.max_simultaneous_requests(), std::size_t{32},
             "halve: a second error in the same drain does not compound");
    queue.end_drain();
    queue.note_network_error();
    check_eq(queue.max_simultaneous_requests(), std::size_t{16}, "halve: next drain → 16");
    for (int i{0}; i < 5; ++i) {
        queue.end_drain();
        queue.note_network_error();
    }
    check_eq(queue.max_simultaneous_requests(), std::size_t{4},
             "halve: floors at min_simultaneous_requests, never below");
}

// The budget must actually meter the loop: with a cap of 2, a server that holds
// responses can never see more than 2 connections at once.
void test_engine_budget_is_enforced() {
    constexpr int REQUESTS{6};
    ConcurrentTestServer server{ok_response("payload"), std::chrono::milliseconds{60}};
    check_true(server.ok(), "budget-enforced: server started");

    ah::Engine engine{2};
    int completed{0};
    std::size_t peakActive{0};
    for (int i{0}; i < REQUESTS; ++i) {
        engine.fetch(make_request(server.url("/pkg" + std::to_string(i))), ah::ConnectionOptions{},
                     [&completed](hx::ExecuteResult r) {
                         if (r.has_value() && r->status == 200) {
                             ++completed;
                         }
                     });
    }
    check_eq(engine.inflight_count(), std::size_t{REQUESTS},
             "budget-enforced: all requests accepted, none dropped");
    check_true(engine.active_count() <= 2, "budget-enforced: only 2 started up front");
    while (engine.has_work()) {
        engine.run_once();
        peakActive = std::max(peakActive, engine.active_count());
    }
    check_eq(completed, REQUESTS, "budget-enforced: every request still completed");
    check_true(peakActive <= 2, "budget-enforced: never exceeded the cap of 2");
    check_true(server.accepted() == REQUESTS, "budget-enforced: server saw every request");
}

// The idle bound is the only per-request timeout bun has (lib.rs:283). A peer
// that accepts and then says nothing must fail as Timeout -- on the
// metadata.is_none() retry path -- in bounded time.
void test_engine_idle_timeout() {
    // Never responds: the worker blocks reading a request that satisfies it, and
    // we never let it answer within the bound.
    ConcurrentTestServer server{ok_response("too late"), std::chrono::seconds{30}};
    check_true(server.ok(), "engine-idle: server started");
    ah::Engine engine{};
    ah::ConnectionOptions options{};
    options.idleTimeout = std::chrono::milliseconds{300};

    std::optional<hx::ExecuteResult> out{};
    engine.fetch(make_request(server.url("/stall")), options,
                 [&out](hx::ExecuteResult r) { out = std::move(r); });
    const auto start{std::chrono::steady_clock::now()};
    engine.run();
    const auto elapsed{std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start)};

    check_true(out.has_value(), "engine-idle: completion fired");
    if (!out) {
        return;
    }
    check_true(!out->has_value(), "engine-idle: reported as a transport error");
    if (!out->has_value()) {
        check_true(out->error().code == nt::NetworkErrorCode::Timeout,
                   "engine-idle: Timeout, not TlsHandshakeFailed or ProtocolError");
    }
    check_true(elapsed < std::chrono::seconds{5}, "engine-idle: failed in bounded time");
}

#endif  // __linux__

}  // namespace

int main() {
#if defined(__linux__)
    test_resolver_numeric_literal();
    test_resolver_ipv6_literal();
    test_resolver_caches_per_host();
    test_resolver_port_is_part_of_key();
    test_resolver_does_not_cache_failure();
    test_engine_content_length();
    test_engine_chunked();
    test_engine_404_has_metadata();
    test_engine_connect_refused();
    test_engine_redirect();
    test_engine_https_get();
    test_idle_timeout_normalization();
    test_scheduler_constants();
    test_scheduler_precedence();
    test_scheduler_env_parse();
    test_scheduler_admission_order();
    test_scheduler_halves_once_per_drain();
    test_engine_budget_is_enforced();
    test_engine_requests_overlap();
    test_pool_reuses_connection();
    test_pool_does_not_reuse_a_closed_socket();
    test_pool_does_not_cross_origins();
    test_engine_idle_timeout();
#else
    // DEFERRED off-Linux: the resolver reports a structured error rather than
    // pretending to resolve (same stance as epoll_socket_backend.cppm).
    auto resolved{ah::HostResolver::resolve("127.0.0.1", 80)};
    check_true(!resolved.has_value(), "non-linux: resolver is DEFERRED");
#endif

    std::println("test_async_http: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
