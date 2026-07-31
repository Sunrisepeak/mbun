// Real-socket integration vectors for mbun.runtime_server.http1_server.
// A raw non-blocking client writes literal HTTP/1.1 bytes to 127.0.0.1 while
// the server's event loop is pumped in the same thread (bounded pump, no
// sleeps). Scenarios mirror bun runtime/server/RequestContext.rs lifecycle:
// GET round-trip, Content-Length body, chunked body, keep-alive reuse +
// pipelining, Connection: close, 400/404/431 error paths, and a large
// response driven through EPOLLOUT write backpressure.
#if defined(__linux__)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

import std;
import mbun.event_loop;
import mbun.runtime_socket;
import mbun.runtime_server;

namespace {

using namespace mbun::runtime_server;

int checks { 0 };
int failures { 0 };

void check(bool condition, std::string_view name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL {}", name);
    }
}

#if defined(__linux__)

struct Fixture {
    mbun::event_loop::HostReadinessBackend epoll {};
    mbun::event_loop::EventLoop loop;
    Http1Server server;

    Fixture()
        : loop { epoll.seam() }
        , server { loop, epoll }
    {
    }

    // Bounded pump: a 5ms no-op timer caps each run_once wait, so conditions
    // are awaited, never slept for.
    bool pump_until(const std::function<bool()>& done, int maxIterations = 2000) {
        for (int i { 0 }; i < maxIterations; ++i) {
            if (done())
                return true;
            tick_();
        }
        return done();
    }

    void pump_iterations(int iterations) {
        for (int i { 0 }; i < iterations; ++i)
            tick_();
    }

private:
    void tick_() {
        const auto now { std::chrono::steady_clock::now() };
        static_cast<void>(loop.set_timeout(std::chrono::milliseconds { 5 }, [] {}, now));
        static_cast<void>(loop.run_once(std::chrono::steady_clock::now()));
    }
};

// Blocking connect (the backlog admits it immediately), then non-blocking
// send/recv interleaved with server loop pumps.
struct RawClient {
    int fd { -1 };
    bool eof { false };

    ~RawClient() {
        if (fd >= 0)
            ::close(fd);
    }

    bool connect_to(std::uint16_t port) {
        fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return false;
        ::sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1)
            return false;
        if (::connect(fd, reinterpret_cast<const ::sockaddr*>(&addr), sizeof(addr)) != 0)
            return false;
        return ::fcntl(fd, F_SETFL, O_NONBLOCK) == 0;
    }

    bool send_all(Fixture& fx, std::string_view data, int maxIterations = 20000) {
        std::size_t sent { 0 };
        for (int i { 0 }; i < maxIterations && sent < data.size(); ++i) {
            const ::ssize_t n { ::send(fd, data.data() + sent, data.size() - sent,
                                       MSG_NOSIGNAL) };
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                fx.pump_iterations(1);
                continue;
            }
            return false;
        }
        return sent == data.size();
    }

    // Recv into `out` (pumping the server between attempts) until `done(out)`
    // holds or the peer sends FIN.
    bool recv_until(Fixture& fx, std::string& out,
                    const std::function<bool(const std::string&)>& done,
                    int maxIterations = 20000) {
        std::array<char, 64 * 1024> chunk {};
        for (int i { 0 }; i < maxIterations; ++i) {
            if (done(out))
                return true;
            const ::ssize_t n { ::recv(fd, chunk.data(), chunk.size(), 0) };
            if (n > 0) {
                out.append(chunk.data(), static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0) {
                eof = true;
                return done(out);
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                fx.pump_iterations(1);
                continue;
            }
            return false;
        }
        return done(out);
    }

    // Bounded negative observation: pump, then confirm the peer FIN arrived.
    bool wait_eof(Fixture& fx, int maxIterations = 20000) {
        std::string sink {};
        static_cast<void>(recv_until(fx, sink, [this](const std::string&) { return eof; },
                                     maxIterations));
        return eof;
    }
};

// One full HTTP/1.1 response (Content-Length framing): returns the total byte
// length of the response starting at `s`, or nullopt while incomplete.
std::optional<std::size_t> response_span(std::string_view s) {
    const auto headEnd { s.find("\r\n\r\n") };
    if (headEnd == std::string_view::npos)
        return std::nullopt;
    const std::string_view head { s.substr(0, headEnd + 4) };
    std::size_t contentLength { 0 };
    std::size_t pos { head.find("\r\n") };
    while (pos != std::string_view::npos && pos + 2 < head.size()) {
        const std::size_t lineEnd { head.find("\r\n", pos + 2) };
        std::string_view line { head.substr(pos + 2, lineEnd - pos - 2) };
        std::string lower {};
        for (const char c : line)
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (lower.starts_with("content-length:")) {
            const std::string_view value { line.substr(std::string_view { "content-length:" }.size()) };
            std::size_t start { value.find_first_not_of(' ') };
            static_cast<void>(std::from_chars(value.data() + start, value.data() + value.size(),
                                              contentLength));
        }
        pos = lineEnd;
    }
    const std::size_t total { headEnd + 4 + contentLength };
    if (s.size() < total)
        return std::nullopt;
    return total;
}

bool responses_complete(std::string_view s, int count) {
    std::size_t offset { 0 };
    for (int i { 0 }; i < count; ++i) {
        const auto span { response_span(s.substr(offset)) };
        if (!span)
            return false;
        offset += *span;
    }
    return true;
}

std::string body_of(std::string_view response) {
    const auto headEnd { response.find("\r\n\r\n") };
    return headEnd == std::string_view::npos ? std::string {}
                                             : std::string { response.substr(headEnd + 4) };
}

std::uint16_t listen_ok(Fixture& fx, Http1Handler handler) {
    const auto port { fx.server.listen("127.0.0.1", 0, std::move(handler)) };
    check(port.has_value() && *port != 0, "server binds an ephemeral port");
    return port.value_or(0);
}

void test_get_round_trip() {
    Fixture fx {};
    int seen { 0 };
    const auto port { listen_ok(fx, [&](const Http1Request& req) {
        ++seen;
        check(req.method == "GET" && req.path == "/hello", "handler sees method and path");
        check(req.header("host").value_or("") == "test", "handler sees request headers");
        Http1Response res {};
        res.headers.emplace_back("X-Test", "1");
        res.body = "hello mbun";
        return res;
    }) };

    RawClient client {};
    check(client.connect_to(port), "GET client connects");
    check(client.send_all(fx, "GET /hello HTTP/1.1\r\nHost: test\r\n\r\n"), "GET request sent");

    std::string received {};
    check(client.recv_until(fx, received,
                            [](const std::string& s) { return responses_complete(s, 1); }),
          "GET response arrives");
    check(received.starts_with("HTTP/1.1 200 OK\r\n"), "GET status line is 200 OK");
    check(received.contains("X-Test: 1\r\n"), "handler headers are written");
    check(received.contains("Content-Length: 10\r\n"), "Content-Length is stamped");
    check(received.contains("Connection: keep-alive\r\n"), "HTTP/1.1 defaults to keep-alive");
    check(body_of(received) == "hello mbun", "GET body round-trips");
    check(seen == 1, "handler ran exactly once");
}

void test_post_content_length_echo() {
    Fixture fx {};
    const auto port { listen_ok(fx, [](const Http1Request& req) {
        Http1Response res {};
        res.body = req.method + ":" + req.body;
        return res;
    }) };

    RawClient client {};
    check(client.connect_to(port), "POST client connects");
    // Split head and body across writes to exercise incremental parsing.
    check(client.send_all(fx, "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 11\r\n\r\nhello"),
          "POST head + partial body sent");
    fx.pump_iterations(5);
    check(client.send_all(fx, " world"), "POST body tail sent");

    std::string received {};
    check(client.recv_until(fx, received,
                            [](const std::string& s) { return responses_complete(s, 1); }),
          "POST response arrives");
    check(body_of(received) == "POST:hello world", "Content-Length body is echoed");
}

void test_chunked_request_body() {
    Fixture fx {};
    const auto port { listen_ok(fx, [](const Http1Request& req) {
        Http1Response res {};
        res.body = req.body;
        return res;
    }) };

    RawClient client {};
    check(client.connect_to(port), "chunked client connects");
    check(client.send_all(fx, "POST /c HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n"
                              "4\r\nWiki\r\n"),
          "chunked head + first chunk sent");
    fx.pump_iterations(5);
    check(client.send_all(fx, "5\r\npedia\r\n0\r\n\r\n"), "chunked tail sent");

    std::string received {};
    check(client.recv_until(fx, received,
                            [](const std::string& s) { return responses_complete(s, 1); }),
          "chunked response arrives");
    check(body_of(received) == "Wikipedia", "chunked body is decoded and echoed");
}

void test_keep_alive_and_pipelining() {
    Fixture fx {};
    int seen { 0 };
    const auto port { listen_ok(fx, [&](const Http1Request& req) {
        ++seen;
        Http1Response res {};
        res.body = "r" + std::string { req.path };
        return res;
    }) };

    RawClient client {};
    check(client.connect_to(port), "keep-alive client connects");

    // Sequential reuse of one connection.
    check(client.send_all(fx, "GET /1 HTTP/1.1\r\nHost: t\r\n\r\n"), "first request sent");
    std::string received {};
    check(client.recv_until(fx, received,
                            [](const std::string& s) { return responses_complete(s, 1); }),
          "first response arrives");
    check(client.send_all(fx, "GET /2 HTTP/1.1\r\nHost: t\r\n\r\n"), "second request sent");
    check(client.recv_until(fx, received,
                            [](const std::string& s) { return responses_complete(s, 2); }),
          "second response arrives on the same connection");

    // Serial pipelining: two requests in one write, two responses back.
    check(client.send_all(fx, "GET /3 HTTP/1.1\r\nHost: t\r\n\r\n"
                              "GET /4 HTTP/1.1\r\nHost: t\r\n\r\n"),
          "pipelined pair sent");
    check(client.recv_until(fx, received,
                            [](const std::string& s) { return responses_complete(s, 4); }),
          "pipelined responses both arrive");
    check(seen == 4, "handler ran once per request");
    check(!client.eof, "connection stays open across keep-alive requests");
    check(received.contains("r/1") && received.contains("r/2") && received.contains("r/3")
              && received.contains("r/4"),
          "responses preserve request order and identity");
    check(fx.server.open_connections() == 1, "server tracks the single reused connection");
}

void test_connection_close_semantics() {
    Fixture fx {};
    const auto port { listen_ok(fx, [](const Http1Request&) {
        Http1Response res {};
        res.body = "bye";
        return res;
    }) };

    RawClient client {};
    check(client.connect_to(port), "close client connects");
    check(client.send_all(fx, "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n"),
          "close request sent");

    std::string received {};
    check(client.recv_until(fx, received,
                            [](const std::string& s) { return responses_complete(s, 1); }),
          "close response arrives");
    check(received.contains("Connection: close\r\n"), "response advertises close");
    check(body_of(received) == "bye", "close response body intact");
    check(client.wait_eof(fx), "server closes after Connection: close");
    check(fx.pump_until([&] { return fx.server.open_connections() == 0; }),
          "server drops the closed connection state");
}

void test_http10_defaults_to_close() {
    Fixture fx {};
    const auto port { listen_ok(fx, [](const Http1Request&) {
        Http1Response res {};
        res.body = "old";
        return res;
    }) };

    RawClient client {};
    check(client.connect_to(port), "HTTP/1.0 client connects");
    check(client.send_all(fx, "GET / HTTP/1.0\r\nHost: t\r\n\r\n"), "HTTP/1.0 request sent");
    std::string received {};
    check(client.recv_until(fx, received,
                            [](const std::string& s) { return responses_complete(s, 1); }),
          "HTTP/1.0 response arrives");
    check(received.contains("Connection: close\r\n"), "HTTP/1.0 without keep-alive closes");
    check(client.wait_eof(fx), "HTTP/1.0 connection is closed by the server");
}

void test_bad_request_gets_400() {
    Fixture fx {};
    const auto port { listen_ok(fx, [](const Http1Request&) { return Http1Response {}; }) };

    RawClient client {};
    check(client.connect_to(port), "400 client connects");
    check(client.send_all(fx, "\x01garbage bytes not http\r\n\r\n"), "garbage sent");
    std::string received {};
    check(client.recv_until(fx, received,
                            [](const std::string& s) { return responses_complete(s, 1); }),
          "400 response arrives");
    check(received.starts_with("HTTP/1.1 400 Bad Request\r\n"), "garbage yields 400");
    check(client.wait_eof(fx), "400 closes the connection");
}

void test_missing_route_gets_404() {
    Fixture fx {};
    fx.server.add_route("/hi", HttpMethod::get, [](const Http1Request&) {
        Http1Response res {};
        res.body = "route";
        return res;
    });
    const auto port { listen_ok(fx, Http1Handler {}) };  // no fallback handler

    RawClient client {};
    check(client.connect_to(port), "404 client connects");
    check(client.send_all(fx, "GET /nope HTTP/1.1\r\nHost: t\r\n\r\n"
                              "GET /hi HTTP/1.1\r\nHost: t\r\n\r\n"),
          "unrouted + routed requests sent");
    std::string received {};
    check(client.recv_until(fx, received,
                            [](const std::string& s) { return responses_complete(s, 2); }),
          "404 and routed responses arrive");
    check(received.starts_with("HTTP/1.1 404 Not Found\r\n"), "missing handler yields 404");
    check(received.contains("HTTP/1.1 200 OK\r\n") && received.contains("route"),
          "RequestDispatcher route still serves after a 404");
}

void test_oversized_headers_get_431() {
    Fixture fx {};
    const auto port { listen_ok(fx, [](const Http1Request&) { return Http1Response {}; }) };

    RawClient client {};
    check(client.connect_to(port), "431 client connects");
    // >64KB of header bytes with no terminating blank line.
    std::string huge { "GET / HTTP/1.1\r\nHost: t\r\nX-Filler: " };
    huge.append(70 * 1024, 'x');
    check(client.send_all(fx, huge), "oversized header block sent");
    std::string received {};
    check(client.recv_until(fx, received,
                            [](const std::string& s) { return responses_complete(s, 1); }),
          "431 response arrives");
    check(received.starts_with("HTTP/1.1 431 Request Header Fields Too Large\r\n"),
          "oversized headers yield 431");
    check(client.wait_eof(fx), "431 closes the connection");
}

void test_large_response_backpressure() {
    const std::size_t bodySize { 256 * 1024 };
    std::string expected {};
    expected.reserve(bodySize);
    for (std::size_t i { 0 }; i < bodySize; ++i)
        expected.push_back(static_cast<char>('a' + (i % 26)));

    Fixture fx {};
    const auto port { listen_ok(fx, [&](const Http1Request&) {
        Http1Response res {};
        res.body = expected;
        return res;
    }) };

    // Shrink both kernel buffers before the accept inherits them: the 256KB
    // response then reliably hits EAGAIN and rides the stream-buffer flush path.
    const auto listenerHandle { fx.server.listener_handle() };
    check(listenerHandle.has_value(), "server exposes the listener handle");
    const int smallBuf { 4 * 1024 };
    check(::setsockopt(static_cast<int>(*listenerHandle), SOL_SOCKET, SO_SNDBUF, &smallBuf,
                       sizeof(smallBuf)) == 0,
          "small SO_SNDBUF applied to the listener fd");

    RawClient client {};
    check(client.connect_to(port), "backpressure client connects");
    check(client.send_all(fx, "GET /big HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n"),
          "backpressure request sent");

    std::string received {};
    check(client.recv_until(fx, received,
                            [](const std::string& s) { return responses_complete(s, 1); },
                            200000),
          "large response fully received through backpressure");
    check(body_of(received) == expected, "large body arrives byte-exact");
    check(client.wait_eof(fx), "close deferred until the buffered response drained");
}

// Async mode: the handler resolves "later" (a queued request answered between
// pump iterations with raw pre-framed bytes), keep-alive recycles the
// connection, and a second pipelined request is parked until finish_raw.
void test_async_deferred_response_keep_alive() {
    Fixture fx {};
    std::vector<std::pair<Http1RequestId, Http1Request>> inbox {};
    fx.server.set_async_handler(
        [&](Http1RequestId id, Http1Request&& req) { inbox.emplace_back(id, std::move(req)); });
    const auto port { fx.server.listen("127.0.0.1", 0, {}) };
    check(port.has_value() && *port != 0, "async server binds an ephemeral port");

    RawClient client {};
    check(client.connect_to(*port), "async client connects");
    // Two pipelined requests in one write: the second must stay parked while
    // the first awaits its deferred response.
    check(client.send_all(fx, "GET /a HTTP/1.1\r\nHost: t\r\n\r\n"
                              "GET /b HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n"),
          "pipelined async requests sent");
    check(fx.pump_until([&] { return inbox.size() == 1; }), "first request dispatched");
    fx.pump_iterations(20);
    check(inbox.size() == 1, "second request parked while first awaits response");
    check(inbox[0].second.path == "/a", "async request carries the path");
    check(fx.server.pending_requests() == 1, "one request pending");

    const std::string res1 { "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                             "Connection: keep-alive\r\n\r\nok" };
    check(fx.server.write_raw(inbox[0].first, res1), "raw response bytes accepted");
    check(fx.server.finish_raw(inbox[0].first, true), "finish_raw recycles keep-alive");
    check(inbox.size() == 2 && inbox[1].second.path == "/b",
          "pipelined request dispatched after recycle");

    const std::string res2 { "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                             "Connection: close\r\n\r\nbb" };
    check(fx.server.write_raw(inbox[1].first, res2), "second raw response accepted");
    check(fx.server.finish_raw(inbox[1].first, false), "finish_raw close path");

    std::string received {};
    check(client.recv_until(fx, received,
                            [](const std::string& s) { return responses_complete(s, 2); }),
          "both async responses arrive");
    check(body_of(std::string_view { received }.substr(*response_span(received))) == "bb",
          "second async body round-trips");
    check(client.wait_eof(fx), "connection closes after Connection: close response");
    check(fx.server.pending_requests() == 0, "no pending requests remain");
}

// Client disconnect while a request awaits its response fires the abort
// handler, and late write_raw/finish_raw calls become clean no-ops.
void test_async_abort_on_client_disconnect() {
    Fixture fx {};
    std::optional<Http1RequestId> dispatched {};
    std::optional<Http1RequestId> aborted {};
    fx.server.set_async_handler(
        [&](Http1RequestId id, Http1Request&&) { dispatched = id; },
        [&](Http1RequestId id) { aborted = id; });
    const auto port { fx.server.listen("127.0.0.1", 0, {}) };
    check(port.has_value(), "abort-test server binds");

    {
        RawClient client {};
        check(client.connect_to(*port), "abort-test client connects");
        check(client.send_all(fx, "GET /slow HTTP/1.1\r\nHost: t\r\n\r\n"), "request sent");
        check(fx.pump_until([&] { return dispatched.has_value(); }), "request dispatched");
    }  // client fd closes here (FIN)
    check(fx.pump_until([&] { return aborted.has_value(); }), "abort handler fired");
    check(aborted == dispatched, "abort reports the pending request id");
    check(!fx.server.write_raw(*dispatched, "x"), "write_raw after abort is a no-op");
    check(!fx.server.finish_raw(*dispatched, true), "finish_raw after abort is a no-op");
}

// Streaming mode (dispatch-on-headers): the request arrives before its body,
// chunks stream as on_body events, the final event carries done=true, and a
// response sent before the body completes closes the connection.
void test_async_streaming_request_body() {
    Fixture fx {};
    std::vector<std::pair<Http1RequestId, std::string>> heads {};
    std::string body {};
    bool bodyDone { false };
    fx.server.set_async_handler(
        [&](Http1RequestId id, Http1Request&& req) { heads.emplace_back(id, req.path); },
        {},
        [&](Http1RequestId, std::string_view chunk, bool done) {
            body.append(chunk);
            if (done)
                bodyDone = true;
        });
    const auto port { fx.server.listen("127.0.0.1", 0, {}) };
    check(port.has_value(), "streaming server binds");

    RawClient client {};
    check(client.connect_to(*port), "streaming client connects");
    check(client.send_all(fx, "POST /up HTTP/1.1\r\nHost: t\r\nContent-Length: 10\r\n\r\nhello"),
          "head + half body sent");
    check(fx.pump_until([&] { return heads.size() == 1; }),
          "request dispatched at header-complete (body still inbound)");
    check(!bodyDone && body == "hello", "first body chunk streamed");
    check(client.send_all(fx, "world"), "body tail sent");
    check(fx.pump_until([&] { return bodyDone; }), "body completion event fired");
    check(body == "helloworld", "streamed body is byte-exact");

    const std::string res { "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                            "Connection: keep-alive\r\n\r\nok" };
    check(fx.server.write_raw(heads[0].first, res), "streamed request answers");
    check(fx.server.finish_raw(heads[0].first, true), "finish_raw after complete body");

    // Second request: respond BEFORE the body completes → connection closes.
    check(client.send_all(fx, "POST /early HTTP/1.1\r\nHost: t\r\nContent-Length: 99\r\n\r\nx"),
          "second head + partial body sent");
    check(fx.pump_until([&] { return heads.size() == 2; }), "second request dispatched early");
    check(fx.server.write_raw(heads[1].first, res), "early response accepted");
    check(fx.server.finish_raw(heads[1].first, true), "early finish_raw returns");
    std::string received {};
    check(client.recv_until(fx, received,
                            [](const std::string& s) { return responses_complete(s, 2); }),
          "both responses arrive");
    check(client.wait_eof(fx), "responding mid-body closes the connection");
}

// detach_raw (WebSocket takeover): after the upgrade response, inbound bytes
// tunnel through on_body verbatim (no HTTP framing), raw writes keep working,
// and a remote close fires the abort handler.
void test_detach_raw_tunnel() {
    Fixture fx {};
    std::vector<Http1RequestId> heads {};
    std::string tunneled {};
    std::optional<Http1RequestId> aborted {};
    fx.server.set_async_handler(
        [&](Http1RequestId id, Http1Request&&) { heads.push_back(id); },
        [&](Http1RequestId id) { aborted = id; },
        [&](Http1RequestId, std::string_view chunk, bool) { tunneled.append(chunk); });
    const auto port { fx.server.listen("127.0.0.1", 0, {}) };
    check(port.has_value(), "detach server binds");

    RawClient client {};
    check(client.connect_to(*port), "detach client connects");
    check(client.send_all(fx, "GET /ws HTTP/1.1\r\nHost: t\r\nUpgrade: websocket\r\n\r\n"),
          "upgrade request sent");
    check(fx.pump_until([&] { return heads.size() == 1; }), "upgrade request dispatched");
    const std::string handshake { "HTTP/1.1 101 Switching Protocols\r\n"
                                  "Upgrade: websocket\r\nConnection: Upgrade\r\n\r\n" };
    check(fx.server.write_raw(heads[0], handshake), "101 written raw");
    check(fx.server.detach_raw(heads[0]), "connection detaches to raw tunnel");

    std::string received {};
    check(client.recv_until(fx, received,
                            [&](const std::string& s) { return s.contains("\r\n\r\n"); }),
          "client sees the 101");
    check(client.send_all(fx, "\x81\x03hey"), "raw frame bytes sent");
    check(fx.pump_until([&] { return tunneled.size() >= 5; }), "tunnel forwards raw bytes");
    check(tunneled == "\x81\x03hey", "tunneled bytes are verbatim (no HTTP parse)");
    check(fx.server.write_raw(heads[0], "\x81\x02ok"), "raw write into the tunnel works");
    std::string received2 {};
    check(client.recv_until(fx, received2,
                            [](const std::string& s) { return s.size() >= 4; }),
          "client receives tunneled reply");
    check(received2 == "\x81\x02ok", "tunneled reply is verbatim");

    ::close(client.fd);  // FIN from client
    client.fd = -1;
    check(fx.pump_until([&] { return aborted.has_value(); }), "tunnel close fires abort");
    check(aborted == heads[0], "abort carries the tunnel's request id");
}

#else  // !__linux__

void test_deferred_stub() {
    mbun::event_loop::HostReadinessBackend epoll {};
    mbun::event_loop::EventLoop loop { epoll.seam() };
    Http1Server server { loop, epoll };
    const auto port { server.listen("127.0.0.1", 0,
                                    [](const Http1Request&) { return Http1Response {}; }) };
    check(!port.has_value(), "non-linux listen reports deferred");
    check(server.open_connections() == 0, "non-linux server tracks no connections");
}

#endif

}  // namespace

int main() {
#if defined(__linux__)
    test_get_round_trip();
    test_post_content_length_echo();
    test_chunked_request_body();
    test_keep_alive_and_pipelining();
    test_connection_close_semantics();
    test_http10_defaults_to_close();
    test_bad_request_gets_400();
    test_missing_route_gets_404();
    test_oversized_headers_get_431();
    test_large_response_backpressure();
    test_async_deferred_response_keep_alive();
    test_async_abort_on_client_disconnect();
    test_async_streaming_request_body();
    test_detach_raw_tunnel();
#else
    test_deferred_stub();
#endif
    std::println("runtime_server http1: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
