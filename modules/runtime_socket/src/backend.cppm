export module mbun.runtime_socket.backend;

import std;
import mbun.runtime_socket.address;

export namespace mbun::runtime_socket {

using NativeHandle = std::int64_t;

struct BackendError {
    int code { 0 };
    std::string message {};
};

// Peer endpoint of a connected socket.
//
// PORT-SOURCE: bun packages/bun-usockets/src/bsd.c:768-775 (bsd_remote_addr =
// getpeername + internal_finalize_bsd_addr) and bsd.c:743-757
// (internal_finalize_bsd_addr). uSockets keeps the raw in6_addr for AF_INET6
// and does NOT unmap v4-mapped peers, and it reports the family from the
// address length (16 → IPv6, 4 → IPv4). That is why a v4 client on a
// dual-stack listener surfaces as { address: "::ffff:127.0.0.1",
// family: "IPv6" } in Bun.serve's requestIP()/ws.remoteAddress — matching that
// shape is the point of `ipv6` here, so do not normalize the mapped form away.
struct PeerAddress {
    std::string address {};
    std::uint16_t port { 0 };
    bool ipv6 { false };
};

class SocketBackend {
public:
    virtual ~SocketBackend() = default;
    virtual std::expected<NativeHandle, BackendError> connect(const Address&) = 0;
    virtual std::expected<NativeHandle, BackendError> listen(const Address&) = 0;
    virtual std::expected<std::size_t, BackendError> write(NativeHandle, std::span<const std::byte>) = 0;
    virtual bool pause(NativeHandle) = 0;
    virtual bool resume(NativeHandle) = 0;
    virtual bool shutdown(NativeHandle) = 0;
    virtual void close(NativeHandle) = 0;
    // Server-initiated close with FIN semantics: half-close the write side and
    // discard inbound until the peer closes. A plain close() with unread
    // inbound bytes makes the kernel send RST, which the peer observes as
    // ECONNRESET instead of EOF (the HTTP 431/error path promises a clean
    // close). Not pure: synthetic backends have no kernel buffer to drain.
    virtual void close_after_drain(NativeHandle handle) { close(handle); }
    // Not pure: a backend whose handles are not real fds (RecordingBackend) has
    // no peer to report, and every caller already treats "no peer" as valid
    // (a closed/synthetic connection). Mirrors bsd_remote_addr's -1 return.
    virtual std::optional<PeerAddress> remote_address(NativeHandle) { return std::nullopt; }
};

class RecordingBackend final : public SocketBackend {
private:
    NativeHandle next_handle_ { 1 };
    std::vector<std::byte> written_ {};
    std::size_t close_count_ { 0 };

public:
    std::expected<NativeHandle, BackendError> connect(const Address&) override { return next_handle_++; }
    std::expected<NativeHandle, BackendError> listen(const Address&) override { return next_handle_++; }
    std::expected<std::size_t, BackendError> write(NativeHandle, std::span<const std::byte> bytes) override {
        written_.insert(written_.end(), bytes.begin(), bytes.end());
        return bytes.size();
    }
    bool pause(NativeHandle) override { return true; }
    bool resume(NativeHandle) override { return true; }
    bool shutdown(NativeHandle) override { return true; }
    void close(NativeHandle) override { ++close_count_; }
    std::span<const std::byte> written() const { return written_; }
    std::size_t close_count() const { return close_count_; }
};

}
