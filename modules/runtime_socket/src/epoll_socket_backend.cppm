// Linux epoll-backed SocketBackend driving Connection/Listener over real fds.
//
// PORT-SOURCE:
//   bun Rust src/uws_sys/us_socket_t.rs (open/pause/resume/shutdown/close and
//   us_socket_stream_buffer_t: unsent bytes buffered with a cursor, drained on
//   writable) and src/uws_sys/{socket.rs,ListenSocket.rs} callback model:
//   on_open / on_data / on_writable / on_close dispatched from the event loop.
//   Backpressure follows uws semantics: a partial/blocked send arms EPOLLOUT,
//   the writable event flushes the buffer, and draining it disarms EPOLLOUT
//   before on_writable fires.
//
// Deviation from us_socket_write (which returns the kernel-written count and
// leaves the remainder to the caller): write() accepts the full span — the
// unsent tail is buffered internally (us_socket_stream_buffer_t equivalent),
// so the return value is bytes accepted, not bytes on the wire.
//
// Non-Linux builds keep the honest DEFERRED stub style of
// modules/event_loop/src/epoll_backend.cppm: every operation fails cleanly.
module;

#if defined(__linux__)
#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>  // offsetof for sockaddr_un length
#include <netinet/in.h>
#include <netinet/tcp.h>  // TCP_NODELAY
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

export module mbun.runtime_socket.epoll_socket_backend;

import std;
import mbun.event_loop;
import mbun.runtime_socket.address;
import mbun.runtime_socket.backend;
import mbun.runtime_socket.buffer;

export namespace mbun::runtime_socket {

// SocketContext-style callback set (ref: uws_sys socket dispatch). All hooks
// fire on the event-loop thread via EventLoop token dispatch; on_open's bool
// is is_client (us_socket_open's is_client flag).
struct SocketEvents {
    std::function<void(NativeHandle, bool)> on_open {};
    std::function<void(NativeHandle, std::span<const std::byte>)> on_data {};
    std::function<void(NativeHandle)> on_writable {};
    std::function<void(NativeHandle)> on_close {};
};

class EpollSocketBackend final : public SocketBackend {
private:
    enum class Role : std::uint8_t { listener, connecting, open };

    struct Entry {
        std::uint64_t token { 0 };
        Role role { Role::open };
        bool readEnabled { true };
        bool writePending { false };
        bool inEpoll { false };
        // close_after_drain: write side already shut, inbound is discarded
        // until the peer's FIN/error, then the fd is reaped (no on_data).
        bool draining { false };
        StreamBuffer writeBuffer {};
    };

    event_loop::EventLoop& loop_;
    event_loop::HostReadinessBackend& epoll_;
    SocketEvents events_ {};
    std::unordered_map<int, Entry> entries_ {};
    // Process-global token counter: several backends may share one EventLoop
    // (e.g. multiple Bun.serve instances on the runtime loop) and its watch
    // table routes by token, so per-instance counters would collide.
    inline static std::uint64_t gNextToken { 1 };

public:  // Big Five: references + live fd table, non-copyable/non-movable.
    EpollSocketBackend(event_loop::EventLoop& loop, event_loop::HostReadinessBackend& epoll,
                       SocketEvents events)
        : loop_ { loop }
        , epoll_ { epoll }
        , events_ { std::move(events) }
    {
    }
    EpollSocketBackend(const EpollSocketBackend&) = delete;
    EpollSocketBackend& operator=(const EpollSocketBackend&) = delete;
    EpollSocketBackend(EpollSocketBackend&&) = delete;
    EpollSocketBackend& operator=(EpollSocketBackend&&) = delete;
    ~EpollSocketBackend() override {
#if defined(__linux__)
        for (const auto& [fd, entry] : entries_) {
            static_cast<void>(loop_.remove_watch(entry.token));
            if (entry.inEpoll)
                static_cast<void>(epoll_.remove(fd));
            ::close(fd);
        }
#endif
    }

public:  // SocketBackend
    // us_socket_context_listen: socket / SO_REUSEADDR / bind / listen, then
    // register read interest so readiness means "accept queue non-empty".
    std::expected<NativeHandle, BackendError> listen(const Address& address) override {
#if defined(__linux__)
        auto fd { open_socket_(address) };
        if (!fd)
            return std::unexpected { fd.error() };
        const int enable { 1 };
        if (::setsockopt(*fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0)
            return fail_and_close_(*fd, "setsockopt(SO_REUSEADDR) failed");
#if defined(IPV6_V6ONLY)
        // ref: bun packages/bun-usockets/src/bsd.c:1124-1131 — uSockets sets
        // IPV6_V6ONLY *explicitly* on every AF_INET6 listener (0 unless
        // LIBUS_SOCKET_IPV6_ONLY) rather than inheriting net.ipv6.bindv6only,
        // so a "::" listener accepts v4 peers (reported v4-mapped) on hosts
        // where that sysctl is 1. Bun.serve's defaulted host binds "::", so
        // without this the default listener would silently refuse v4 clients
        // on such a host.
        if (address.family() == AddressFamily::ipv6) {
            const int v6only { 0 };
            if (::setsockopt(*fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) != 0)
                return fail_and_close_(*fd, "setsockopt(IPV6_V6ONLY) failed");
        }
#endif
        ::sockaddr_storage storage {};
        ::socklen_t length { 0 };
        if (auto built { build_sockaddr_(address, storage, length) }; !built)
            return fail_and_close_with_(*fd, built.error());
        if (::bind(*fd, reinterpret_cast<const ::sockaddr*>(&storage), length) != 0)
            return fail_and_close_(*fd, "bind failed");
        if (::listen(*fd, SOMAXCONN) != 0)
            return fail_and_close_(*fd, "listen failed");
        if (auto registered { register_(*fd, Role::listener) }; !registered)
            return fail_and_close_with_(*fd, registered.error());
        return NativeHandle { *fd };
#else
        static_cast<void>(address);
        return std::unexpected { deferred_error_() };  // DEFERRED: Linux-only backend.
#endif
    }

    // PORT-SOURCE: bun packages/bun-usockets/src/bsd.c:768-775 (bsd_remote_addr)
    // + bsd.c:743-757 (internal_finalize_bsd_addr). Read off the fd on demand
    // rather than cached from accept4 (which discards the sockaddr here), same
    // as uSockets. The v4-mapped form of an AF_INET6 peer is deliberately kept
    // verbatim — see PeerAddress in backend.cppm.
    std::optional<PeerAddress> remote_address(NativeHandle handle) override {
#if defined(__linux__)
        ::sockaddr_storage storage {};
        ::socklen_t length { sizeof(storage) };
        if (::getpeername(static_cast<int>(handle), reinterpret_cast<::sockaddr*>(&storage),
                          &length)
            != 0)
            return std::nullopt;
        char text[INET6_ADDRSTRLEN] {};
        if (storage.ss_family == AF_INET6) {
            const auto& v6 { reinterpret_cast<const ::sockaddr_in6&>(storage) };
            if (::inet_ntop(AF_INET6, &v6.sin6_addr, text, sizeof(text)) == nullptr)
                return std::nullopt;
            return PeerAddress { .address = text, .port = ntohs(v6.sin6_port), .ipv6 = true };
        }
        if (storage.ss_family == AF_INET) {
            const auto& v4 { reinterpret_cast<const ::sockaddr_in&>(storage) };
            if (::inet_ntop(AF_INET, &v4.sin_addr, text, sizeof(text)) == nullptr)
                return std::nullopt;
            return PeerAddress { .address = text, .port = ntohs(v4.sin_port), .ipv6 = false };
        }
        // AF_UNIX and friends: bsd.c reports ip_length 0 / port -1; the JS layer
        // treats "no peer" as null, so report nothing rather than a fake IP.
        return std::nullopt;
#else
        static_cast<void>(handle);
        return std::nullopt;  // DEFERRED: Linux-only backend.
#endif
    }

    // Non-blocking connect; EINPROGRESS arms EPOLLOUT and completion is
    // detected via SO_ERROR in the write-readiness event (us_socket_t open).
    std::expected<NativeHandle, BackendError> connect(const Address& address) override {
#if defined(__linux__)
        auto fd { open_socket_(address) };
        if (!fd)
            return std::unexpected { fd.error() };
        ::sockaddr_storage storage {};
        ::socklen_t length { 0 };
        if (auto built { build_sockaddr_(address, storage, length) }; !built)
            return fail_and_close_with_(*fd, built.error());
        int rc { -1 };
        do {
            rc = ::connect(*fd, reinterpret_cast<const ::sockaddr*>(&storage), length);
        } while (rc != 0 && errno == EINTR);
        if (rc != 0 && errno != EINPROGRESS)
            return fail_and_close_(*fd, "connect failed");
        const Role role { rc == 0 ? Role::open : Role::connecting };
        if (auto registered { register_(*fd, role) }; !registered)
            return fail_and_close_with_(*fd, registered.error());
        if (rc == 0) {
            // Immediate loopback success: defer on_open to the next tick so it
            // still arrives via the loop, matching the EINPROGRESS ordering.
            const int connectedFd { *fd };
            static_cast<void>(loop_.post([this, connectedFd] {
                if (entries_.contains(connectedFd) && events_.on_open)
                    events_.on_open(NativeHandle { connectedFd }, true);
            }));
        }
        return NativeHandle { *fd };
#else
        static_cast<void>(address);
        return std::unexpected { deferred_error_() };  // DEFERRED
#endif
    }

    // Direct send first; on EAGAIN (or while a flush is already pending) the
    // tail goes into the stream buffer and EPOLLOUT is armed (uws semantics).
    std::expected<std::size_t, BackendError> write(NativeHandle handle,
                                                   std::span<const std::byte> bytes) override {
#if defined(__linux__)
        const int fd { static_cast<int>(handle) };
        const auto found { entries_.find(fd) };
        if (found == entries_.end() || found->second.role == Role::listener)
            return std::unexpected { BackendError { 0, "write on unknown socket" } };
        Entry& entry { found->second };
        if (entry.role == Role::connecting || entry.writePending) {
            entry.writeBuffer.append(bytes);
            entry.writePending = true;
            update_interest_(fd, entry);
            return bytes.size();
        }
        std::size_t sent { 0 };
        while (sent < bytes.size()) {
            const ::ssize_t n { ::send(fd, bytes.data() + sent, bytes.size() - sent,
                                       MSG_NOSIGNAL) };
            if (n > 0) {
                sent += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            return std::unexpected { BackendError { errno, "send failed" } };
        }
        if (sent < bytes.size()) {
            entry.writeBuffer.append(bytes.subspan(sent));
            entry.writePending = true;
            update_interest_(fd, entry);
        }
        return bytes.size();
#else
        static_cast<void>(handle);
        static_cast<void>(bytes);
        return std::unexpected { deferred_error_() };  // DEFERRED
#endif
    }

    // us_socket_pause/resume: drop or restore EPOLLIN via epoll_ctl.
    bool pause(NativeHandle handle) override {
#if defined(__linux__)
        return set_read_enabled_(static_cast<int>(handle), false);
#else
        static_cast<void>(handle);
        return false;  // DEFERRED
#endif
    }
    bool resume(NativeHandle handle) override {
#if defined(__linux__)
        return set_read_enabled_(static_cast<int>(handle), true);
#else
        static_cast<void>(handle);
        return false;  // DEFERRED
#endif
    }

    // us_socket_shutdown: FIN the write side, keep reading until peer close.
    bool shutdown(NativeHandle handle) override {
#if defined(__linux__)
        const int fd { static_cast<int>(handle) };
        return entries_.contains(fd) && ::shutdown(fd, SHUT_WR) == 0;
#else
        static_cast<void>(handle);
        return false;  // DEFERRED
#endif
    }

    // Local close: deregister and release the fd. on_close is reserved for
    // remote-initiated close (recv == 0 / socket error), matching the tests'
    // "peer closed" contract; a caller-driven close already knows it closed.
    void close(NativeHandle handle) override {
#if defined(__linux__)
        cleanup_(static_cast<int>(handle));
#else
        static_cast<void>(handle);
#endif
    }

    void close_after_drain(NativeHandle handle) override {
#if defined(__linux__)
        const int fd { static_cast<int>(handle) };
        const auto found { entries_.find(fd) };
        if (found == entries_.end()) {
            return;
        }
        ::shutdown(fd, SHUT_WR);  // FIN now; the peer reads EOF, not ECONNRESET
        found->second.draining = true;
        found->second.readEnabled = true;  // keep EPOLLIN so the drain progresses
        drain_now_(fd);
#else
        close(handle);
#endif
    }

public:  // Introspection (us_socket_local_port equivalents; used by tests).
    [[nodiscard]] std::optional<std::uint16_t> local_port(NativeHandle handle) const {
#if defined(__linux__)
        ::sockaddr_storage storage {};
        ::socklen_t length { sizeof(storage) };
        if (::getsockname(static_cast<int>(handle),
                          reinterpret_cast<::sockaddr*>(&storage), &length) != 0)
            return std::nullopt;
        if (storage.ss_family == AF_INET)
            return ntohs(reinterpret_cast<const ::sockaddr_in&>(storage).sin_port);
        if (storage.ss_family == AF_INET6)
            return ntohs(reinterpret_cast<const ::sockaddr_in6&>(storage).sin6_port);
        return std::nullopt;
#else
        static_cast<void>(handle);
        return std::nullopt;  // DEFERRED
#endif
    }

    [[nodiscard]] std::size_t pending_write_bytes(NativeHandle handle) const {
        const auto found { entries_.find(static_cast<int>(handle)) };
        return found == entries_.end() ? 0 : found->second.writeBuffer.size();
    }

    [[nodiscard]] std::size_t open_sockets() const { return entries_.size(); }

private:
    static BackendError deferred_error_() {
        return BackendError { 0, "DEFERRED: epoll socket backend is Linux-only" };
    }

#if defined(__linux__)
private:
    static constexpr std::size_t READ_CHUNK { 64 * 1024 };

    static BackendError errno_error_(std::string message) {
        return BackendError { errno, std::move(message) };
    }

    std::unexpected<BackendError> fail_and_close_(int fd, std::string message) {
        BackendError error { errno_error_(std::move(message)) };
        ::close(fd);
        return std::unexpected { std::move(error) };
    }
    std::unexpected<BackendError> fail_and_close_with_(int fd, BackendError error) {
        ::close(fd);
        return std::unexpected { std::move(error) };
    }

    // ref bun packages/bun-usockets/src/bsd.c:394 bsd_socket_nodelay, applied
    // unconditionally to every socket uSockets owns: loop.c:509 ("We always use
    // nodelay") on each accepted fd, context.c:504/677/793 on each connect.
    //
    // Not an optimisation -- a correctness-grade latency fix. A response is
    // written as head then body (two sends). With Nagle on, the small second
    // write is held until the first is ACKed, and the peer's delayed ACK sits on
    // it for ~40ms. The first request on a fresh connection escapes (nothing is
    // unacked yet, so head+body coalesce into one segment), which is why this
    // only shows up from the second keep-alive request onward -- exactly what
    // fetch's connection pool turned from a rare case into the common one.
    // Fails harmlessly with ENOPROTOOPT on AF_UNIX, as in uSockets.
    static void set_nodelay_(int fd) {
        const int one { 1 };
        static_cast<void>(::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one));
    }

    std::expected<int, BackendError> open_socket_(const Address& address) {
        int domain { AF_INET };
        switch (address.family()) {
            case AddressFamily::ipv4: domain = AF_INET; break;
            case AddressFamily::ipv6: domain = AF_INET6; break;
            case AddressFamily::unix: domain = AF_UNIX; break;
        }
        const int fd { ::socket(domain, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0) };
        if (fd < 0)
            return std::unexpected { errno_error_("socket() failed") };
        set_nodelay_(fd);
        return fd;
    }

    static std::expected<void, BackendError> build_sockaddr_(const Address& address,
                                                             ::sockaddr_storage& storage,
                                                             ::socklen_t& length) {
        const std::string host { address.host() };
        switch (address.family()) {
            case AddressFamily::ipv4: {
                auto& in4 { reinterpret_cast<::sockaddr_in&>(storage) };
                in4.sin_family = AF_INET;
                in4.sin_port = htons(address.port());
                if (::inet_pton(AF_INET, host.c_str(), &in4.sin_addr) != 1)
                    return std::unexpected { BackendError { 0, "invalid ipv4 host" } };
                length = sizeof(::sockaddr_in);
                return {};
            }
            case AddressFamily::ipv6: {
                auto& in6 { reinterpret_cast<::sockaddr_in6&>(storage) };
                in6.sin6_family = AF_INET6;
                in6.sin6_port = htons(address.port());
                in6.sin6_scope_id = address.scope_id();
                if (::inet_pton(AF_INET6, host.c_str(), &in6.sin6_addr) != 1)
                    return std::unexpected { BackendError { 0, "invalid ipv6 host" } };
                length = sizeof(::sockaddr_in6);
                return {};
            }
            case AddressFamily::unix: {
                auto& un { reinterpret_cast<::sockaddr_un&>(storage) };
                un.sun_family = AF_UNIX;
                if (host.size() >= sizeof(un.sun_path))
                    return std::unexpected { BackendError { 0, "unix path too long" } };
                std::ranges::copy(host, un.sun_path);
                un.sun_path[host.size()] = '\0';
                length = static_cast<::socklen_t>(offsetof(::sockaddr_un, sun_path)
                                                  + host.size() + 1);
                return {};
            }
        }
        return std::unexpected { BackendError { 0, "unknown address family" } };
    }

    // Register the fd with the epoll backend + route its token through the
    // EventLoop watch table (ready_polls dispatch in bun uws_sys/Loop.rs).
    std::expected<void, BackendError> register_(int fd, Role role) {
        Entry entry {};
        entry.token = gNextToken++;
        entry.role = role;
        const event_loop::PollInterest interest {
            role == Role::connecting ? event_loop::PollInterest::write
                                     : event_loop::PollInterest::read
        };
        if (!epoll_.add(fd, interest, entry.token))
            return std::unexpected { BackendError { errno, "epoll add failed" } };
        entry.inEpoll = true;
        if (!loop_.add_watch(entry.token,
                             [this, fd](const event_loop::BackendEvent&) { handle_event_(fd); })) {
            static_cast<void>(epoll_.remove(fd));
            return std::unexpected { BackendError { 0, "duplicate watch token" } };
        }
        entries_.emplace(fd, std::move(entry));
        return {};
    }

    // Desired interest = (readEnabled ? EPOLLIN : 0) | (writePending ? EPOLLOUT : 0);
    // an empty set deregisters the fd (level-triggered epoll would spin otherwise).
    void update_interest_(int fd, Entry& entry) {
        std::optional<event_loop::PollInterest> interest {};
        const bool wantRead { entry.readEnabled && entry.role != Role::connecting };
        if (wantRead && entry.writePending)
            interest = event_loop::PollInterest::read_write;
        else if (wantRead)
            interest = event_loop::PollInterest::read;
        else if (entry.writePending || entry.role == Role::connecting)
            interest = event_loop::PollInterest::write;
        if (!interest) {
            if (entry.inEpoll && epoll_.remove(fd))
                entry.inEpoll = false;
            return;
        }
        if (entry.inEpoll)
            static_cast<void>(epoll_.modify(fd, *interest, entry.token));
        else if (epoll_.add(fd, *interest, entry.token))
            entry.inEpoll = true;
    }

    bool set_read_enabled_(int fd, bool enabled) {
        const auto found { entries_.find(fd) };
        if (found == entries_.end() || found->second.role != Role::open)
            return false;
        found->second.readEnabled = enabled;
        update_interest_(fd, found->second);
        return true;
    }

    void cleanup_(int fd) {
        const auto found { entries_.find(fd) };
        if (found == entries_.end())
            return;
        static_cast<void>(loop_.remove_watch(found->second.token));
        if (found->second.inEpoll)
            static_cast<void>(epoll_.remove(fd));
        ::close(fd);
        entries_.erase(found);
    }

    void remote_close_(int fd) {
        // Deregister BEFORE the callback so a re-entrant backend->close() from
        // the user's on_close (e.g. Connection teardown) is a clean no-op.
        cleanup_(fd);
        if (events_.on_close)
            events_.on_close(NativeHandle { fd });
    }

    void handle_event_(int fd) {
        const auto found { entries_.find(fd) };
        if (found == entries_.end())
            return;
        switch (found->second.role) {
            case Role::listener: accept_ready_(fd); return;
            case Role::connecting: finish_connect_(fd); return;
            case Role::open: break;
        }
        flush_(fd);
        // flush_ may have detected an error and closed the socket; on_data
        // callbacks may pause or close too — re-validate before reading.
        const auto still { entries_.find(fd) };
        if (still != entries_.end() && still->second.readEnabled)
            read_ready_(fd);
    }

    // Listener readiness: drain the accept queue (accept4 NONBLOCK until
    // EAGAIN), register each connection for reads, announce via on_open.
    void accept_ready_(int fd) {
        while (true) {
            const int accepted { ::accept4(fd, nullptr, nullptr,
                                           SOCK_NONBLOCK | SOCK_CLOEXEC) };
            if (accepted < 0) {
                if (errno == EINTR)
                    continue;
                return;  // EAGAIN: queue drained; other errors retried next tick.
            }
            set_nodelay_(accepted);  // uSockets loop.c:510, on every accepted fd
            if (!register_(accepted, Role::open)) {
                ::close(accepted);
                continue;
            }
            if (events_.on_open)
                events_.on_open(NativeHandle { accepted }, false);
        }
    }

    // EPOLLOUT on a connecting socket: SO_ERROR == 0 completes the connect
    // (switch to read interest, us_socket_open), non-zero reports failure.
    void finish_connect_(int fd) {
        int soError { 0 };
        ::socklen_t length { sizeof(soError) };
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &length) != 0 || soError != 0) {
            remote_close_(fd);
            return;
        }
        Entry& entry { entries_.at(fd) };
        entry.role = Role::open;
        update_interest_(fd, entry);
        if (events_.on_open)
            events_.on_open(NativeHandle { fd }, true);
    }

    // Writable: drain the stream buffer; once empty disarm EPOLLOUT and fire
    // on_writable (uws only reports writable after prior backpressure).
    void flush_(int fd) {
        const auto found { entries_.find(fd) };
        if (found == entries_.end() || !found->second.writePending)
            return;
        Entry& entry { found->second };
        while (!entry.writeBuffer.empty()) {
            const auto readable { entry.writeBuffer.readable() };
            const ::ssize_t n { ::send(fd, readable.data(), readable.size(), MSG_NOSIGNAL) };
            if (n > 0) {
                static_cast<void>(entry.writeBuffer.consume(static_cast<std::size_t>(n)));
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;  // Still blocked; EPOLLOUT stays armed.
            remote_close_(fd);  // EPIPE/ECONNRESET etc.: fatal write error.
            return;
        }
        entry.writePending = false;
        update_interest_(fd, entry);
        if (events_.on_writable)
            events_.on_writable(NativeHandle { fd });
    }

    // Readable: recv until EAGAIN feeding on_data; recv == 0 is peer FIN.
    // Discard inbound on a draining fd until the peer closes; reap on FIN or
    // any hard error. Returns on EAGAIN (more readable events will follow).
    void drain_now_(int fd) {
        std::array<std::byte, READ_CHUNK> chunk {};
        while (true) {
            const ::ssize_t n { ::recv(fd, chunk.data(), chunk.size(), 0) };
            if (n > 0) {
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            cleanup_(fd);  // FIN (0) or hard error: the peer saw our FIN, reap
            return;
        }
    }

    void read_ready_(int fd) {
        if (const auto found { entries_.find(fd) };
            found != entries_.end() && found->second.draining) {
            drain_now_(fd);
            return;
        }
        std::array<std::byte, READ_CHUNK> chunk {};
        while (true) {
            const ::ssize_t n { ::recv(fd, chunk.data(), chunk.size(), 0) };
            if (n > 0) {
                if (events_.on_data)
                    events_.on_data(NativeHandle { fd },
                                    std::span<const std::byte> { chunk.data(),
                                                                 static_cast<std::size_t>(n) });
                // on_data may close or pause the socket; re-validate.
                const auto still { entries_.find(fd) };
                if (still == entries_.end() || !still->second.readEnabled)
                    return;
                continue;
            }
            if (n == 0) {
                remote_close_(fd);
                return;
            }
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            remote_close_(fd);
            return;
        }
    }
#endif
};

}  // namespace mbun::runtime_socket
