// test_http_executor.cpp — mbun.install blocking HTTP/1.1 executor tests.
//
// Covers src/http_executor.cppm against a minimal in-process HTTP server
// (socket/bind/listen/accept on 127.0.0.1, canned responses, one connection
// per response, no external network):
//   - 200 + Content-Length framing, request-line/Host/descriptor headers,
//   - chunked transfer decoding (multi-chunk + trailer),
//   - close-delimited (no framing header) bodies,
//   - 404 classification passthrough (PackageManifestHTTP404),
//   - connection refused → retry until the RetryPolicy budget is exhausted,
//   - https:// TLS handshake + encrypted GET against a local self-signed
//     server (verify disabled; verify+CA bundle; untrusted → TlsHandshakeFailed),
//   - retry loop: 500 then 200 (runTasks.rs re-enqueue semantics),
//   - redirect following (302 + Location),
//   - the process-wide DNS cache collapsing repeat lookups of one host.
//
// Reference semantics: .mbun/bun-ref/src/install/NetworkTask.rs (AsyncHTTP GET
// + FetchRedirect::Follow) and PackageManager/runTasks.rs (retry gate).

#if !defined(_WIN32)
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

import std;
import mbun.install.http_executor;
import mbun.install.network_task;
import mbun.tls;

namespace nt = mbun::install::network_task;
namespace hx = mbun::install::http_executor;

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

nt::RequestDescriptor make_request(std::string url,
                                   nt::RequestKind kind = nt::RequestKind::PackageManifest) {
    nt::RequestDescriptor d{};
    d.kind = kind;
    d.url = std::move(url);
    d.headers.push_back({"Accept", std::string{nt::ACCEPT_HEADER_VALUE}});
    return d;
}

hx::ExecutorOptions fast_options() {
    hx::ExecutorOptions o{};
    o.connectTimeout = std::chrono::seconds{5};
    o.ioTimeout = std::chrono::seconds{5};
    return o;
}

#if !defined(_WIN32)

// ── minimal blocking HTTP server ────────────────────────────────────────────
// Serves the canned responses in order, one accepted connection per response
// (the executor always sends Connection: close). Records each request's
// header block for assertions.
class TestServer {
private:
    int listenFd_{-1};
    std::uint16_t port_{0};
    std::vector<std::string> responses_;
    std::vector<std::string> requests_;
    std::mutex mutex_;
    std::thread thread_;

public:
    explicit TestServer(std::vector<std::string> responses) : responses_{std::move(responses)} {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one{1};
        (void)::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        ::sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // ephemeral
        if (::bind(listenFd_, reinterpret_cast<::sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(listenFd_, 8) != 0) {
            ::close(listenFd_);
            listenFd_ = -1;
            return;
        }
        ::socklen_t len{sizeof addr};
        (void)::getsockname(listenFd_, reinterpret_cast<::sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        // Bound accept so a failing test cannot hang the suite.
        ::timeval tv{10, 0};
        (void)::setsockopt(listenFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        thread_ = std::thread{[this] { serve_(); }};
    }

    TestServer(const TestServer&) = delete;
    TestServer& operator=(const TestServer&) = delete;

    ~TestServer() {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listenFd_ >= 0) {
            ::close(listenFd_);
        }
    }

    bool ok() const { return listenFd_ >= 0; }
    std::uint16_t port() const { return port_; }

    std::string url(std::string_view path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string{path};
    }

    std::vector<std::string> requests() {
        std::scoped_lock lock{mutex_};
        return requests_;
    }

private:
    void serve_() {
        for (const std::string& response : responses_) {
            int fd{::accept(listenFd_, nullptr, nullptr)};
            if (fd < 0) {
                return;  // accept timeout — bail out instead of hanging
            }
            std::string request;
            char buf[8192];
            while (request.find("\r\n\r\n") == std::string::npos) {
                ::ssize_t n{::recv(fd, buf, sizeof buf, 0)};
                if (n <= 0) {
                    break;
                }
                request.append(buf, static_cast<std::size_t>(n));
            }
            {
                std::scoped_lock lock{mutex_};
                requests_.push_back(std::move(request));
            }
            std::size_t sent{0};
            while (sent < response.size()) {
                ::ssize_t n{::send(fd, response.data() + sent, response.size() - sent, 0)};
                if (n <= 0) {
                    break;
                }
                sent += static_cast<std::size_t>(n);
            }
            ::close(fd);
        }
    }
};

std::string body_str(const hx::HttpResponse& r) {
    return std::string{reinterpret_cast<const char*>(r.body.data()), r.body.size()};
}

// ── tests ───────────────────────────────────────────────────────────────────

void test_content_length_200() {
    std::string body{R"({"name":"left-pad","versions":{}})"};
    TestServer server{{"HTTP/1.1 200 OK\r\n"
                       "Content-Type: application/json\r\n"
                       "ETag: \"abc123\"\r\n"
                       "Content-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n" + body}};
    check_true(server.ok(), "cl200: server started");

    auto result{hx::execute(make_request(server.url("/left-pad")), fast_options())};
    check_true(result.has_value(), "cl200: execute succeeded");
    if (!result) {
        std::println("  (error: {})", result.error().message);
        return;
    }
    check_eq(result->status, 200, "cl200: status");
    check_eq(body_str(*result), body, "cl200: body");
    const std::string* etag{hx::find_header(*result, "etag")};
    check_true(etag != nullptr && *etag == "\"abc123\"", "cl200: case-insensitive header lookup");

    auto requests{server.requests()};
    check_eq(requests.size(), std::size_t{1}, "cl200: one request");
    if (!requests.empty()) {
        const std::string& req{requests[0]};
        check_true(req.starts_with("GET /left-pad HTTP/1.1\r\n"), "cl200: request line");
        check_true(req.find("Host: 127.0.0.1:" + std::to_string(server.port()) + "\r\n") !=
                       std::string::npos,
                   "cl200: Host header with explicit port");
        check_true(req.find("Accept: " + std::string{nt::ACCEPT_HEADER_VALUE} + "\r\n") !=
                       std::string::npos,
                   "cl200: descriptor Accept header forwarded");
        check_true(req.find("Connection: close\r\n") != std::string::npos,
                   "cl200: Connection: close");
    }
}

void test_chunked() {
    TestServer server{{"HTTP/1.1 200 OK\r\n"
                       "Transfer-Encoding: chunked\r\n\r\n"
                       "6\r\nhello \r\n"
                       "9;ext=1\r\nchunked w\r\n"
                       "4\r\norld\r\n"
                       "0\r\nX-Trailer: 1\r\n\r\n"}};
    check_true(server.ok(), "chunked: server started");

    auto result{hx::execute(make_request(server.url("/pkg")), fast_options())};
    check_true(result.has_value(), "chunked: execute succeeded");
    if (!result) {
        std::println("  (error: {})", result.error().message);
        return;
    }
    check_eq(result->status, 200, "chunked: status");
    check_eq(body_str(*result), std::string{"hello chunked world"}, "chunked: decoded body");
}

void test_until_close_body() {
    // No Content-Length, no Transfer-Encoding: body is delimited by close.
    TestServer server{{"HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/plain\r\n\r\n"
                       "close-delimited body"}};
    check_true(server.ok(), "eof: server started");

    auto result{hx::execute(make_request(server.url("/pkg")), fast_options())};
    check_true(result.has_value(), "eof: execute succeeded");
    if (result) {
        check_eq(body_str(*result), std::string{"close-delimited body"}, "eof: body");
    }
}

void test_404_classification() {
    TestServer server{{"HTTP/1.1 404 Not Found\r\n"
                       "Content-Length: 9\r\n\r\n"
                       "not found"}};
    check_true(server.ok(), "404: server started");

    nt::RetryPolicy policy{};
    auto outcome{hx::execute_with_retry(make_request(server.url("/nope")), policy,
                                        fast_options())};
    check_true(outcome.result.has_value(), "404: metadata present");
    if (outcome.result) {
        check_eq(outcome.result->status, 404, "404: status");
    }
    check_eq(outcome.retried, std::uint16_t{0}, "404: 4xx is not retried");
    check_true(outcome.classification.verdict == nt::ResponseVerdict::Fail, "404: verdict Fail");
    check_eq(outcome.classification.error_name, std::string_view{"PackageManifestHTTP404"},
             "404: error name");
}

void test_connection_refused_retries() {
    // Grab an ephemeral port, then close it: connecting is refused.
    int fd{::socket(AF_INET, SOCK_STREAM, 0)};
    ::sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)::bind(fd, reinterpret_cast<::sockaddr*>(&addr), sizeof addr);
    ::socklen_t len{sizeof addr};
    (void)::getsockname(fd, reinterpret_cast<::sockaddr*>(&addr), &len);
    std::uint16_t port{ntohs(addr.sin_port)};
    ::close(fd);

    std::string url{"http://127.0.0.1:" + std::to_string(port) + "/pkg"};
    auto single{hx::execute(make_request(url), fast_options())};
    check_true(!single.has_value(), "refused: execute fails");
    if (!single) {
        check_true(single.error().code == nt::NetworkErrorCode::ConnectFailed,
                   "refused: ConnectFailed code");
    }

    nt::RetryPolicy policy{.max_retry_count = 2};
    auto outcome{hx::execute_with_retry(make_request(url, nt::RequestKind::Extract), policy,
                                        fast_options())};
    check_true(!outcome.result.has_value(), "refused: retry outcome still fails");
    check_eq(outcome.retried, std::uint16_t{2}, "refused: retried == max_retry_count");
    check_true(outcome.classification.verdict == nt::ResponseVerdict::Fail,
               "refused: verdict Fail after budget");
    check_eq(outcome.classification.error_name, std::string_view{"HTTPError"},
             "refused: HTTPError name (runTasks.rs fallback)");
}

void test_retry_500_then_200() {
    TestServer server{{"HTTP/1.1 500 Internal Server Error\r\n"
                       "Content-Length: 5\r\n\r\noops!",
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Length: 2\r\n\r\nok"}};
    check_true(server.ok(), "retry: server started");

    nt::RetryPolicy policy{};
    auto outcome{hx::execute_with_retry(make_request(server.url("/pkg")), policy,
                                        fast_options())};
    check_true(outcome.result.has_value(), "retry: final result ok");
    if (outcome.result) {
        check_eq(outcome.result->status, 200, "retry: final status 200");
        check_eq(body_str(*outcome.result), std::string{"ok"}, "retry: final body");
    }
    check_eq(outcome.retried, std::uint16_t{1}, "retry: exactly one retry");
    check_true(outcome.classification.verdict == nt::ResponseVerdict::Success,
               "retry: verdict Success");
    check_eq(server.requests().size(), std::size_t{2}, "retry: two requests hit the server");
}

void test_redirect_follow() {
    // First connection answers 302, second serves the real body. The Location
    // is same-origin relative, so descriptor headers survive the hop.
    TestServer server{{"HTTP/1.1 302 Found\r\n"
                       "Location: /real.tgz\r\n"
                       "Content-Length: 0\r\n\r\n",
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Length: 7\r\n\r\ntarball"}};
    check_true(server.ok(), "redirect: server started");

    auto result{hx::execute(make_request(server.url("/moved.tgz"), nt::RequestKind::Extract),
                            fast_options())};
    check_true(result.has_value(), "redirect: execute succeeded");
    if (result) {
        check_eq(result->status, 200, "redirect: final status");
        check_eq(body_str(*result), std::string{"tarball"}, "redirect: final body");
    }
    auto requests{server.requests()};
    check_eq(requests.size(), std::size_t{2}, "redirect: two hops");
    if (requests.size() == 2) {
        check_true(requests[1].starts_with("GET /real.tgz HTTP/1.1\r\n"),
                   "redirect: second request targets Location");
    }
}

// ── minimal blocking HTTPS server (server-role TlsChannel) ──────────────────
// Wraps one accepted connection in a server-side TlsChannel driven off the raw
// socket, then serves one canned response over the encrypted app edge. Proves
// the executor's client handshake + TLS read/write against a self-signed cert.
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
                ::ssize_t n{::send(fd, enc.data() + sent, enc.size() - sent, 0)};
                if (n <= 0) {
                    return false;
                }
                sent += static_cast<std::size_t>(n);
            }
        }
        return true;
    }

    void serve_() {
        int fd{::accept(listenFd_, nullptr, nullptr)};
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
            ::ssize_t n{::recv(fd, tmp, sizeof tmp, 0)};
            if (n <= 0) {
                ::close(fd);
                return;
            }
            ch.feed_encrypted({reinterpret_cast<const std::uint8_t*>(tmp),
                               static_cast<std::size_t>(n)});
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
            ::ssize_t n{::recv(fd, tmp, sizeof tmp, 0)};
            if (n <= 0) {
                break;
            }
            ch.feed_encrypted({reinterpret_cast<const std::uint8_t*>(tmp),
                               static_cast<std::size_t>(n)});
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

void test_https_handshake_get() {
    auto pem{mbun::tls::make_self_signed("localhost")};
    check_true(pem.has_value(), "https: self-signed cert generated");
    if (!pem) {
        return;
    }
    std::string body{R"({"name":"is-number","dist-tags":{"latest":"7.0.0"}})"};
    TlsTestServer server{pem->cert, pem->key,
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                             std::to_string(body.size()) + "\r\n\r\n" + body};
    check_true(server.ok(), "https: server started");

    hx::ExecutorOptions o{fast_options()};
    o.tlsVerifyPeer = false;  // accept the self-signed cert without a CA
    std::string url{"https://127.0.0.1:" + std::to_string(server.port()) + "/is-number"};
    auto result{hx::execute(make_request(url), o)};
    check_true(result.has_value(), "https: execute succeeded over TLS");
    if (!result) {
        std::println("  (error: {})", result.error().message);
        return;
    }
    check_eq(result->status, 200, "https: status");
    check_eq(body_str(*result), body, "https: body decrypted intact");
    check_true(server.request().starts_with("GET /is-number HTTP/1.1\r\n"),
               "https: server saw plaintext GET");
}

void test_https_verify_with_ca() {
    // CN=localhost cert, trusted via tlsCaBundle, connected to https://localhost.
    auto pem{mbun::tls::make_self_signed("localhost")};
    check_true(pem.has_value(), "https-ca: cert generated");
    if (!pem) {
        return;
    }
    TlsTestServer server{pem->cert, pem->key, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi"};
    check_true(server.ok(), "https-ca: server started");

    hx::ExecutorOptions o{fast_options()};
    o.tlsVerifyPeer = true;
    o.tlsCaBundle = pem->cert;  // trust our self-signed cert as the CA
    std::string url{"https://localhost:" + std::to_string(server.port()) + "/pkg"};
    auto result{hx::execute(make_request(url), o)};
    check_true(result.has_value(), "https-ca: verified handshake succeeded");
    if (result) {
        check_eq(body_str(*result), std::string{"hi"}, "https-ca: body");
    } else {
        std::println("  (error: {})", result.error().message);
    }
}

void test_https_verify_rejects_untrusted() {
    // No CA bundle + verify required → self-signed cert is untrusted → fail.
    auto pem{mbun::tls::make_self_signed("localhost")};
    if (!pem) {
        return;
    }
    TlsTestServer server{pem->cert, pem->key, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"};
    check_true(server.ok(), "https-untrusted: server started");

    hx::ExecutorOptions o{fast_options()};
    o.tlsVerifyPeer = true;  // platform store, does not trust our self-signed cert
    std::string url{"https://localhost:" + std::to_string(server.port()) + "/pkg"};
    auto result{hx::execute(make_request(url), o)};
    check_true(!result.has_value(), "https-untrusted: handshake rejected");
    if (!result) {
        check_true(result.error().code == nt::NetworkErrorCode::TlsHandshakeFailed,
                   "https-untrusted: TlsHandshakeFailed code");
    }

    // A TLS handshake failure goes through the retry gate like any other network
    // failure -- bun retries a handshake that never completes (see
    // test/cli/install/bun-install-stalled-tls.test.ts, "Don't spin through 5
    // retries"). mbun reports cert-verify and stalled-handshake under the same
    // TlsHandshakeFailed code, so both retry. Retrying an untrusted cert cannot
    // succeed, but it costs a bounded budget and keeps us on bun's shape rather
    // than inventing a split bun does not make.
    auto pem2{mbun::tls::make_self_signed("localhost")};
    TlsTestServer server2{pem2->cert, pem2->key, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"};
    std::string url2{"https://localhost:" + std::to_string(server2.port()) + "/pkg"};
    nt::RetryPolicy policy{};
    auto outcome{hx::execute_with_retry(make_request(url2), policy, o)};
    check_true(!outcome.result.has_value(), "https-untrusted: still fails after the retry budget");
    check_true(outcome.retried > 0, "https-untrusted: retried (not classified terminal)");
    check_true(outcome.classification.verdict == nt::ResponseVerdict::Fail,
               "https-untrusted: verdict Fail");
}

// A peer that completes the TCP connect but never speaks TLS (accepts and holds
// the socket silent). The handshake must fail in bounded time, not hang.
class SilentPeer {
private:
    int listenFd_{-1};
    std::uint16_t port_{0};
    std::thread thread_;
    std::atomic<bool> stop_{false};

public:
    SilentPeer() {
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
        ::timeval tv{1, 0};
        (void)::setsockopt(listenFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        thread_ = std::thread{[this] {
            std::vector<int> held;
            while (!stop_.load()) {
                int fd{::accept(listenFd_, nullptr, nullptr)};
                if (fd >= 0) {
                    held.push_back(fd);  // hold open, send nothing
                }
            }
            for (int fd : held) {
                ::close(fd);
            }
        }};
    }

    SilentPeer(const SilentPeer&) = delete;
    SilentPeer& operator=(const SilentPeer&) = delete;

    ~SilentPeer() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listenFd_ >= 0) {
            ::close(listenFd_);
        }
    }

    bool ok() const { return listenFd_ >= 0; }
    std::uint16_t port() const { return port_; }
};

void test_https_silent_peer_times_out() {
    SilentPeer peer{};
    check_true(peer.ok(), "silent: peer started");

    hx::ExecutorOptions o{fast_options()};
    o.connectTimeout = std::chrono::milliseconds{800};  // bound the handshake tightly
    o.ioTimeout = std::chrono::milliseconds{800};
    o.tlsVerifyPeer = false;
    std::string url{"https://127.0.0.1:" + std::to_string(peer.port()) + "/pkg"};

    auto start{std::chrono::steady_clock::now()};
    nt::RetryPolicy policy{};
    auto outcome{hx::execute_with_retry(make_request(url), policy, o)};
    auto elapsed{std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start)};

    check_true(!outcome.result.has_value(), "silent: fails (does not hang)");
    if (!outcome.result) {
        check_true(outcome.result.error().code == nt::NetworkErrorCode::TlsHandshakeFailed,
                   "silent: TlsHandshakeFailed code");
    }
    // Retries, like bun: a peer that completes the TCP connect but never speaks
    // TLS is exactly the transient case bun's stalled-TLS regression test covers.
    // What keeps this bounded is the per-attempt connectTimeout (and, above it,
    // the install-wide budget) -- not refusing to retry, which is what turned one
    // EAGAIN into a dead install of a real dependency tree.
    check_true(outcome.retried > 0, "silent: retried (not classified terminal)");
    // Bounded: one connectTimeout window (0.8s), comfortably under a few seconds
    // even with scheduling slack — never the multi-minute hang.
    check_true(elapsed < std::chrono::seconds{5}, "silent: bounded time to failure");
}

#endif  // !defined(_WIN32)

#if !defined(_WIN32)
// The process-wide resolver cache must collapse repeat lookups of one host to a
// single getaddrinfo, however many requests the install issues against it. This
// restores what bun gets for free from the uSockets keep-alive pool (a host is
// resolved once per connection, and connections are reused —
// HTTPContext.rs:701 `existing_socket`) on top of our connection-per-request
// model, which measured 12 getaddrinfo calls for a 6-package install.
void test_dns_cache_resolves_host_once() {
    std::string body{"ok"};
    std::string response{"HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                         "\r\n\r\n" + body};
    TestServer server{{response, response, response}};
    check_true(server.ok(), "dns: server started");

    hx::ResolverStats before{hx::resolver_stats()};
    for (int i{0}; i < 3; ++i) {
        auto result{hx::execute(make_request(server.url("/pkg")), fast_options())};
        check_true(result.has_value(), "dns: request succeeded");
    }
    hx::ResolverStats after{hx::resolver_stats()};

    check_eq(after.lookups - before.lookups, std::uint64_t{1},
             "dns: three requests to one host issue exactly one getaddrinfo");
    check_eq(after.hits - before.hits, std::uint64_t{2},
             "dns: the other two connections are served from the cache");
    check_eq(server.requests().size(), std::size_t{3},
             "dns: all three requests still reached the server");
}
#endif

void test_invalid_url() {
    auto result{hx::execute(make_request("ftp://example.com/pkg"), fast_options())};
    check_true(!result.has_value() &&
                   result.error().code == nt::NetworkErrorCode::InvalidURL,
               "url: non-http scheme is InvalidURL");
}

}  // namespace

int main() {
#if defined(_WIN32)
    // Winsock backend is DEFERRED: the executor must return the structured
    // PlatformUnsupported error rather than pretending to fetch.
    auto result{hx::execute(make_request("http://127.0.0.1:1/pkg"), fast_options())};
    check_true(!result.has_value() &&
                   result.error().code == nt::NetworkErrorCode::PlatformUnsupported,
               "win32: PlatformUnsupported structured error");
#else
    test_content_length_200();
    test_chunked();
    test_until_close_body();
    test_404_classification();
    test_connection_refused_retries();
    test_retry_500_then_200();
    test_redirect_follow();
    test_https_handshake_get();
    test_https_verify_with_ca();
    test_https_verify_rejects_untrusted();
    test_https_silent_peer_times_out();
    test_dns_cache_resolves_host_once();
#endif
    test_invalid_url();

    std::println("test_http_executor: {} checks, {} failures", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
