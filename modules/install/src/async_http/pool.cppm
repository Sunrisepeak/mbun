// pool.cppm — mbun.install.async_http.pool
//
// The keep-alive socket pool: where a finished request's socket goes instead of
// being closed, and where the next request for the same host looks first.
//
// PORT-SOURCE: bun's HTTPContext (src/http/HTTPContext.rs).
//   * POOL_SIZE = 64 (:19) — a fixed slab, not a growable host→list map.
//   * MAX_KEEPALIVE_HOSTNAME = 128 (:20) — the hostname is stored inline
//     (hostname_buf: [u8;128] + hostname_len: u8, :164-165); a longer hostname
//     is simply not poolable.
//   * release_socket (:581) parks a socket; existing_socket (:701) scans the
//     `used` bitmap linearly and matches port → ssl_config → reject_unauthorized
//     → ALPN → hostname.
//   * There is no idle-TTL sweeper: dead sockets are discarded lazily when the
//     scan trips over them (:788-798). bun does arm a 5-minute uSockets timer on
//     a parked socket (:628 set_timeout_minutes(5)); mbun's equivalent bound is
//     the connection's own idle timer, which is disarmed while parked — so a
//     pooled socket here lives until the peer drops it or the process exits.
//     DEFERRED: a parked-socket TTL. It costs an idle fd per host, and install
//     is a short-lived process against one or two hosts.
//
// bun keeps ONE pool per scheme (a http_context and a https_context, each with
// its own 64 slots) rather than one keyed pool, so the engine holds two of these
// and the ssl-ness of a slot is implied by which pool it is in — never matched
// at lookup.
//
// Matching dimensions ported: port, hostname, and verify mode. bun also matches
// ssl_config pointer identity, ALPN/h2 session, proxy tunnel, and proxy auth
// hash (:711-770). Those are DEFERRED here because the install path cannot
// produce them: it never offers h2 (can_offer_h2 requires flags src/install/
// never sets — lib.rs:1880-1910), never tunnels, and takes its TLS settings from
// one ExecutorOptions for the whole run rather than per-request SSL configs.
// Verify mode is matched because it is the one dimension install *can* vary
// (tlsVerifyPeer) and reusing a leniently-established socket for a strict caller
// is precisely the confusion bun blocks at :770-781.
//
// MC++ shape: std::array<Slot,64> + std::bitset<64>, scanned by bit rather than
// by walking all 64 slots, and the hostname inline so a park costs no
// allocation — structurally the same as bun's HiveArray + used bitmap.
module;

#if defined(__linux__)
#include <cerrno>
#include <sys/socket.h>
#endif

export module mbun.install.async_http.pool;

import std;
import mbun.runtime_socket;
import mbun.tls;

export namespace mbun::install::async_http {

// HTTPContext.rs:19 — the slab is this size and does not grow.
inline constexpr std::size_t POOL_SIZE{64};

// HTTPContext.rs:20 — hostnames longer than this are not poolable at all
// (existing_socket returns None before scanning, :702-704).
inline constexpr std::size_t MAX_KEEPALIVE_HOSTNAME{128};

// A socket handed back to the pool, with everything needed to decide whether a
// later request may speak on it. For https the established TlsChannel travels
// with the socket — the session is the socket, and re-handshaking a pooled
// connection would defeat the point of pooling it.
struct PooledSocket {
    runtime_socket::NativeHandle handle{-1};
    std::array<char, MAX_KEEPALIVE_HOSTNAME> hostnameBuf{};
    std::uint8_t hostnameLen{0};
    std::uint16_t port{0};
    bool verifyPeer{true};
    std::optional<mbun::tls::TlsChannel> tls{};

    [[nodiscard]] std::string_view hostname() const {
        return std::string_view{hostnameBuf.data(), hostnameLen};
    }
};

// Whether a parked fd is still usable, standing in for bun's
// is_closed()/is_shutdown()/get_error() checks (HTTPContext.rs:788-798, and the
// same guard before parking at :606-608).
//
// A zero-length peek means the peer sent FIN. Any *readable* byte on a socket
// that should be idle is equally disqualifying: a well-behaved server says
// nothing between responses, so unread bytes mean either a close_notify or a
// framing desync, and speaking a new request onto it would misparse. Only
// EAGAIN — nothing to say — proves the socket is alive and clean.
inline bool pooled_socket_alive(runtime_socket::NativeHandle handle) {
#if defined(__linux__)
    char probe{};
    const ::ssize_t n{::recv(static_cast<int>(handle), &probe, 1, MSG_PEEK | MSG_DONTWAIT)};
    if (n > 0) {
        return false;  // unexpected bytes on an idle socket
    }
    if (n == 0) {
        return false;  // FIN
    }
    return errno == EAGAIN || errno == EWOULDBLOCK;
#else
    static_cast<void>(handle);
    return false;
#endif
}

// Accounting so a test can assert reuse actually happened rather than infer it.
struct PoolStats {
    std::uint64_t parked{0};    // sockets accepted into the pool
    std::uint64_t reused{0};    // acquire() hits
    std::uint64_t misses{0};    // acquire() with no usable match
    std::uint64_t evicted{0};   // parked sockets found dead / dropped
    std::uint64_t rejected{0};  // release() refused (full, or hostname too long)
};

// One scheme's slab. Not copyable: it owns live fds, and the engine is the only
// thing allowed to decide their fate.
class ConnectionPool {
private:
    std::array<PooledSocket, POOL_SIZE> slots_{};
    std::bitset<POOL_SIZE> used_{};
    PoolStats stats_{};

public:
    ConnectionPool() = default;
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;
    ConnectionPool(ConnectionPool&&) = delete;
    ConnectionPool& operator=(ConnectionPool&&) = delete;
    ~ConnectionPool() = default;

    [[nodiscard]] std::size_t size() const { return used_.count(); }
    [[nodiscard]] bool empty() const { return used_.none(); }
    [[nodiscard]] const PoolStats& stats() const { return stats_; }

    // Park a socket. Returns the slot index on success; nullopt means the caller
    // still owns the fd and must close it — the pool being full or the hostname
    // being too long are ordinary outcomes, not errors (bun falls through to
    // close_socket at :688-694).
    std::optional<std::size_t> release(runtime_socket::NativeHandle handle,
                                       std::string_view hostname, std::uint16_t port,
                                       bool verifyPeer, std::optional<mbun::tls::TlsChannel> tls) {
        if (hostname.empty() || hostname.size() > MAX_KEEPALIVE_HOSTNAME || port == 0 ||
            handle < 0) {
            ++stats_.rejected;
            return std::nullopt;
        }
        if (!pooled_socket_alive(handle)) {
            ++stats_.rejected;
            return std::nullopt;  // bun refuses to park a dead socket (:606-608)
        }
        const std::size_t slot{claim_()};
        if (slot >= POOL_SIZE) {
            ++stats_.rejected;
            return std::nullopt;  // full
        }
        PooledSocket& parked{slots_[slot]};
        parked.handle = handle;
        parked.hostnameLen = static_cast<std::uint8_t>(hostname.size());
        std::copy(hostname.begin(), hostname.end(), parked.hostnameBuf.begin());
        parked.port = port;
        parked.verifyPeer = verifyPeer;
        parked.tls = std::move(tls);
        used_.set(slot);
        ++stats_.parked;
        return slot;
    }

    // Find a socket this request may speak on. A dead one found along the way is
    // dropped and the scan continues, which is bun's lazy reaping (:788-798).
    // `onEvict` hands the caller each fd the pool gives up so it can be closed
    // and unrouted — the pool never touches the event loop itself.
    std::optional<PooledSocket> acquire(std::string_view hostname, std::uint16_t port,
                                        bool verifyPeer,
                                        const std::function<void(PooledSocket&&)>& onEvict) {
        if (hostname.size() > MAX_KEEPALIVE_HOSTNAME) {
            ++stats_.misses;
            return std::nullopt;  // never poolable, so never pooled (:702-704)
        }
        for (std::size_t slot{next_used_(0)}; slot < POOL_SIZE; slot = next_used_(slot + 1)) {
            PooledSocket& parked{slots_[slot]};
            if (parked.port != port || parked.verifyPeer != verifyPeer ||
                parked.hostname() != hostname) {
                continue;
            }
            PooledSocket taken{take_(slot)};
            if (!pooled_socket_alive(taken.handle)) {
                ++stats_.evicted;
                onEvict(std::move(taken));
                continue;  // keep looking; another slot may still be good
            }
            ++stats_.reused;
            return taken;
        }
        ++stats_.misses;
        return std::nullopt;
    }

    // Drop a specific parked fd — the engine calls this when the loop reports
    // that an idle socket closed or spoke.
    std::optional<PooledSocket> evict(runtime_socket::NativeHandle handle) {
        for (std::size_t slot{next_used_(0)}; slot < POOL_SIZE; slot = next_used_(slot + 1)) {
            if (slots_[slot].handle == handle) {
                ++stats_.evicted;
                return take_(slot);
            }
        }
        return std::nullopt;
    }

    // Hand every parked socket back so the owner can close it (shutdown path).
    void drain(const std::function<void(PooledSocket&&)>& onEvict) {
        for (std::size_t slot{next_used_(0)}; slot < POOL_SIZE; slot = next_used_(slot + 1)) {
            onEvict(take_(slot));
        }
    }

private:
    // First free slot, or POOL_SIZE when the slab is full.
    std::size_t claim_() {
        if (used_.all()) {
            return POOL_SIZE;
        }
        for (std::size_t i{0}; i < POOL_SIZE; ++i) {
            if (!used_.test(i)) {
                return i;
            }
        }
        return POOL_SIZE;
    }

    // Next occupied slot at or after `from` — the bitmap walk that stands in for
    // bun's `used.iterator::<true, true>()` (:706).
    std::size_t next_used_(std::size_t from) const {
        for (std::size_t i{from}; i < POOL_SIZE; ++i) {
            if (used_.test(i)) {
                return i;
            }
        }
        return POOL_SIZE;
    }

    PooledSocket take_(std::size_t slot) {
        PooledSocket out{std::move(slots_[slot])};
        slots_[slot] = PooledSocket{};
        used_.reset(slot);
        return out;
    }
};

}  // namespace mbun::install::async_http
