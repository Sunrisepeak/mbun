// Real-fd integration vectors for mbun.runtime_socket.epoll_socket_backend.
// Scenarios mirror bun uws_sys/{us_socket_t.rs,socket.rs} semantics over
// 127.0.0.1: accept dispatch, echo round-trip, EPOLLOUT backpressure flush,
// peer-close -> on_close, and pause/resume gating of reads. The Connection /
// Listener state machines are driven with the epoll backend injected.
#if !defined(_WIN32)
#include <sys/socket.h>
#endif

import std;
import mbun.event_loop;
import mbun.runtime_socket;

namespace {

using namespace mbun::runtime_socket;
int checks{0};
int failures{0};

void check(bool condition, std::string_view name) {
    ++checks;
    if (!condition) {
        ++failures;
        std::println("FAIL {}", name);
    }
}

#if !defined(_WIN32)

// Bundles the loop + epoll backend + socket backend wiring every test needs.
struct Fixture {
    mbun::event_loop::HostReadinessBackend epoll{};
    mbun::event_loop::EventLoop loop;
    std::shared_ptr<EpollSocketBackend> backend{};

    explicit Fixture(SocketEvents events)
        : loop{epoll.seam()}
        , backend{std::make_shared<EpollSocketBackend>(loop, epoll, std::move(events))} {}

    // Bounded pump: each iteration blocks at most 5ms (a no-op timer caps the
    // run_once wait), so predicates are awaited, never slept for.
    bool pump_until(const std::function<bool()>& done, int maxIterations = 2000) {
        for (int i{0}; i < maxIterations; ++i) {
            if (done()) {
                return true;
            }
            tick_();
        }
        return done();
    }

    // Fixed negative-observation window for "X does not happen" assertions.
    void pump_iterations(int iterations) {
        for (int i{0}; i < iterations; ++i) {
            tick_();
        }
    }

private:
    void tick_() {
        const auto now{std::chrono::steady_clock::now()};
        static_cast<void>(loop.set_timeout(std::chrono::milliseconds{5}, [] {}, now));
        static_cast<void>(loop.run_once(std::chrono::steady_clock::now()));
    }
};

std::vector<std::byte> make_payload(std::string_view text) {
    std::vector<std::byte> bytes{};
    bytes.reserve(text.size());
    for (const char c : text) {
        bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
}

std::string to_text(std::span<const std::byte> bytes) {
    std::string text{};
    text.reserve(bytes.size());
    for (const std::byte b : bytes) {
        text.push_back(static_cast<char>(b));
    }
    return text;
}

// Listen on 127.0.0.1:0 through the Listener state machine and return the
// kernel-assigned port (us_socket_local_port equivalent).
std::uint16_t listen_ephemeral(Fixture& fx, Listener& listener) {
    const auto bound{listener.listen(Address::ipv4("127.0.0.1", 0))};
    check(bound.has_value(), "listener binds 127.0.0.1:0");
    check(listener.state() == ListenerState::listening, "listener enters listening state");
    const auto handle{listener.native_handle()};
    check(handle.has_value(), "listener exposes a native handle");
    const auto port{fx.backend->local_port(*handle)};
    check(port.has_value() && *port != 0, "listener reports an ephemeral port");
    return port.value_or(0);
}

void test_listener_accepts_connection() {
    std::optional<NativeHandle> serverSide{};
    bool clientOpened{false};
    SocketEvents events{};
    events.on_open = [&](NativeHandle handle, bool isClient) {
        if (isClient) {
            clientOpened = true;
        } else {
            serverSide = handle;
        }
    };
    Fixture fx{std::move(events)};

    Listener listener{fx.backend};
    const std::uint16_t port{listen_ephemeral(fx, listener)};

    Connection client{fx.backend};
    check(client.connect(Address::ipv4("127.0.0.1", port)).has_value(),
          "client connect() succeeds");
    check(client.state() == ConnectionState::open, "connection state machine reports open");

    check(fx.pump_until([&] { return serverSide.has_value() && clientOpened; }),
          "on_open fires for both accepted and client sockets");
    if (serverSide) {
        listener.add_connection();
        check(listener.active_connections() == 1, "listener tracks the accepted connection");
        fx.backend->close(*serverSide);
    }
}

void test_echo_round_trip() {
    std::optional<NativeHandle> serverSide{};
    std::string serverReceived{};
    std::string clientReceived{};
    std::optional<NativeHandle> clientHandle{};
    SocketEvents events{};
    Fixture* fixture{nullptr};
    events.on_open = [&](NativeHandle handle, bool isClient) {
        if (!isClient) {
            serverSide = handle;
        }
    };
    events.on_data = [&](NativeHandle handle, std::span<const std::byte> bytes) {
        if (serverSide && handle == *serverSide) {
            serverReceived += to_text(bytes);
            // Echo straight back from the data callback (uws on_data pattern).
            static_cast<void>(fixture->backend->write(handle, bytes));
        } else {
            clientReceived += to_text(bytes);
        }
    };
    Fixture fx{std::move(events)};
    fixture = &fx;

    Listener listener{fx.backend};
    const std::uint16_t port{listen_ephemeral(fx, listener)};

    Connection client{fx.backend};
    check(client.connect(Address::ipv4("127.0.0.1", port)).has_value(),
          "echo client connects");
    clientHandle = client.native_handle();
    check(fx.pump_until([&] { return serverSide.has_value(); }), "echo server accepts");

    const std::string message{"ping over epoll"};
    const auto payload{make_payload(message)};
    const auto written{client.write(payload)};
    check(written.has_value() && *written == payload.size(),
          "client write() accepts the full payload");
    check(client.bytes_written() == payload.size(), "connection tracks bytes written");

    check(fx.pump_until([&] { return clientReceived == message; }),
          "client receives the echoed payload");
    check(serverReceived == message, "server on_data saw the exact payload");
}

void test_backpressure_flush_and_writable() {
    std::optional<NativeHandle> serverSide{};
    std::size_t serverReceivedBytes{0};
    int writableEvents{0};
    SocketEvents events{};
    events.on_open = [&](NativeHandle handle, bool isClient) {
        if (!isClient) {
            serverSide = handle;
        }
    };
    events.on_data = [&](NativeHandle handle, std::span<const std::byte> bytes) {
        if (serverSide && handle == *serverSide) {
            serverReceivedBytes += bytes.size();
        }
    };
    events.on_writable = [&](NativeHandle) { ++writableEvents; };
    Fixture fx{std::move(events)};

    Listener listener{fx.backend};
    const std::uint16_t port{listen_ephemeral(fx, listener)};
    // Cap the receive window before any accept: accepted sockets inherit the
    // listener's SO_RCVBUF, so loopback autotuning cannot swallow the payload.
    const int rcvBuf{4 * 1024};
    check(::setsockopt(static_cast<int>(*listener.native_handle()), SOL_SOCKET, SO_RCVBUF,
                       &rcvBuf, sizeof(rcvBuf)) == 0,
          "small SO_RCVBUF applied to the listener fd");

    Connection client{fx.backend};
    check(client.connect(Address::ipv4("127.0.0.1", port)).has_value(),
          "backpressure client connects");
    const auto clientHandle{client.native_handle()};
    check(clientHandle.has_value(), "backpressure client has a handle");

    // Shrink the send buffer so a large write hits EAGAIN deterministically,
    // and pause the server so the peer window fills instead of draining.
    const int sndBuf{4 * 1024};
    check(::setsockopt(static_cast<int>(*clientHandle), SOL_SOCKET, SO_SNDBUF, &sndBuf,
                       sizeof(sndBuf)) == 0,
          "small SO_SNDBUF applied to the client fd");
    check(fx.pump_until([&] { return serverSide.has_value(); }), "backpressure server accepts");
    check(fx.backend->pause(*serverSide), "server side pauses reads");

    const std::size_t payloadSize{256 * 1024};
    const std::vector<std::byte> payload(payloadSize, std::byte{0x5a});
    const auto written{client.write(payload)};
    check(written.has_value() && *written == payloadSize,
          "large write is fully accepted (tail buffered)");
    fx.pump_iterations(10);
    check(fx.backend->pending_write_bytes(*clientHandle) > 0,
          "EAGAIN left bytes pending in the stream buffer");
    check(writableEvents == 0, "on_writable is withheld while the buffer is non-empty");

    check(fx.backend->resume(*serverSide), "server side resumes reads");
    check(fx.pump_until([&] { return serverReceivedBytes == payloadSize; }),
          "EPOLLOUT flush delivers every buffered byte to the peer");
    check(fx.pump_until([&] { return writableEvents == 1; }),
          "on_writable fires exactly once after the buffer drains");
    check(fx.backend->pending_write_bytes(*clientHandle) == 0,
          "stream buffer is empty after the flush");
}

void test_peer_close_reports_on_close() {
    std::optional<NativeHandle> serverSide{};
    std::vector<NativeHandle> closed{};
    SocketEvents events{};
    events.on_open = [&](NativeHandle handle, bool isClient) {
        if (!isClient) {
            serverSide = handle;
        }
    };
    events.on_close = [&](NativeHandle handle) { closed.push_back(handle); };
    Fixture fx{std::move(events)};

    Listener listener{fx.backend};
    const std::uint16_t port{listen_ephemeral(fx, listener)};

    Connection client{fx.backend};
    check(client.connect(Address::ipv4("127.0.0.1", port)).has_value(), "close client connects");
    const auto clientHandle{client.native_handle()};
    check(fx.pump_until([&] { return serverSide.has_value(); }), "close server accepts");

    const std::size_t before{fx.backend->open_sockets()};
    client.close();
    check(client.state() == ConnectionState::closed, "connection state machine reports closed");
    check(fx.backend->open_sockets() == before - 1, "local close releases the fd entry");

    check(fx.pump_until([&] { return !closed.empty(); }), "peer FIN raises on_close");
    check(closed.size() == 1 && serverSide && closed.front() == *serverSide,
          "on_close names the server-side socket");
    check(fx.backend->pending_write_bytes(*serverSide) == 0, "closed entry is fully dropped");
    static_cast<void>(clientHandle);
}

void test_pause_blocks_data_and_resume_recovers() {
    std::optional<NativeHandle> serverSide{};
    std::string serverReceived{};
    SocketEvents events{};
    events.on_open = [&](NativeHandle handle, bool isClient) {
        if (!isClient) {
            serverSide = handle;
        }
    };
    events.on_data = [&](NativeHandle handle, std::span<const std::byte> bytes) {
        if (serverSide && handle == *serverSide) {
            serverReceived += to_text(bytes);
        }
    };
    Fixture fx{std::move(events)};

    Listener listener{fx.backend};
    const std::uint16_t port{listen_ephemeral(fx, listener)};

    Connection client{fx.backend};
    check(client.connect(Address::ipv4("127.0.0.1", port)).has_value(), "pause client connects");
    check(fx.pump_until([&] { return serverSide.has_value(); }), "pause server accepts");

    check(fx.backend->pause(*serverSide), "pause() detaches read interest");
    const auto payload{make_payload("held back")};
    check(client.write(payload).has_value(), "client writes while server is paused");
    fx.pump_iterations(20);
    check(serverReceived.empty(), "paused socket delivers no on_data");

    check(fx.backend->resume(*serverSide), "resume() re-arms read interest");
    check(fx.pump_until([&] { return serverReceived == "held back"; }),
          "resume delivers the withheld bytes");
}

#else  // !__linux__

void test_deferred_stub() {
    mbun::event_loop::HostReadinessBackend epoll{};
    mbun::event_loop::EventLoop loop{epoll.seam()};
    auto backend{std::make_shared<EpollSocketBackend>(loop, epoll, SocketEvents{})};
    check(!backend->listen(Address::ipv4("127.0.0.1", 0)).has_value(),
          "non-linux listen reports deferred");
    check(!backend->connect(Address::ipv4("127.0.0.1", 1)).has_value(),
          "non-linux connect reports deferred");
    check(!backend->pause(1) && !backend->resume(1), "non-linux pause/resume refuse");
    check(backend->open_sockets() == 0, "non-linux backend tracks no sockets");
}

#endif

}  // namespace

int main() {
#if !defined(_WIN32)
    test_listener_accepts_connection();
    test_echo_round_trip();
    test_backpressure_flush_and_writable();
    test_peer_close_reports_on_close();
    test_pause_blocks_data_and_resume_recovers();
#else
    test_deferred_stub();
#endif
    std::println("runtime_socket epoll backend: {} checks, {} failures", checks, failures);
    return failures == 0 ? 0 : 1;
}
