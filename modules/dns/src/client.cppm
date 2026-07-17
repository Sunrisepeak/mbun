// client.cppm — transport-independent DNS record-query state machine.
//
// Bun delegates these mechanics to c-ares. mbun keeps equivalent retry and
// validation policy in pure C++ so POSIX/Winsock adapters and deterministic
// tests share one implementation.
// PORT-SOURCE: bun src/runtime/dns_jsc/dns.rs (c-ares query/error contract)
// PORT-SOURCE: bun src/dns/lib.rs (channel/server retry ownership)
export module mbun.dns.client;

import std;
import mbun.dns.error;
import mbun.dns.wire;

namespace mbun::dns {

export struct WireDatagram {
    std::size_t sourceServer{0};
    std::vector<std::uint8_t> payload;
};

export struct RecordQueryOptions {
    std::uint64_t timeoutMs{2'000};
    std::size_t attempts{2};
};

namespace client_detail {

inline std::uint64_t deadline_after(std::uint64_t now, std::uint64_t timeout) noexcept {
    constexpr auto MAX{std::numeric_limits<std::uint64_t>::max()};
    return timeout > MAX - now ? MAX : now + timeout;
}

inline bool retryable(AresError error) noexcept {
    return error == AresError::ESERVFAIL;
}

}  // namespace client_detail

// Transport is deliberately structural: production adapters inline without a
// virtual call, while fakes provide the same clock/random/network operations.
export template <typename Transport>
WireParseResult query_record(std::string_view name, RecordType type, std::size_t serverCount,
                             Transport& transport, RecordQueryOptions options = {}) {
    if (serverCount == 0) return std::unexpected(AresError::ENOSERVER);
    const std::size_t attempts{std::max<std::size_t>(options.attempts, 1)};
    std::optional<AresError> lastRetryable;

    for (std::size_t attempt{0}; attempt < attempts; ++attempt) {
        for (std::size_t server{0}; server < serverCount; ++server) {
            const std::uint16_t id{transport.random_id()};
            auto query{make_wire_query(name, type, id)};
            if (!query) return std::unexpected(query.error());
            const std::uint64_t deadline{
                client_detail::deadline_after(transport.now_ms(), options.timeoutMs)};
            if (!transport.send_udp(server, *query, deadline)) continue;

            while (transport.now_ms() < deadline) {
                auto datagram{transport.receive_udp(deadline)};
                if (!datagram) break;
                if (datagram->sourceServer != server) continue;

                auto envelope{inspect_wire_response(datagram->payload, id, name, type)};
                if (!envelope) {
                    if (envelope.error() == AresError::EBADRESP) continue;
                    if (client_detail::retryable(envelope.error())) {
                        lastRetryable = envelope.error();
                        break;
                    }
                    return std::unexpected(envelope.error());
                }

                std::span<const std::uint8_t> response{datagram->payload};
                std::optional<std::vector<std::uint8_t>> tcpResponse;
                if (envelope->truncated) {
                    tcpResponse = transport.query_tcp(server, *query, deadline);
                    if (!tcpResponse) break;
                    response = *tcpResponse;
                }

                auto parsed{parse_wire_response(response, id, name, type)};
                if (parsed) return parsed;
                if (parsed.error() == AresError::EBADRESP) {
                    if (tcpResponse) break;
                    continue;
                }
                if (client_detail::retryable(parsed.error())) {
                    lastRetryable = parsed.error();
                    break;
                }
                return parsed;
            }
        }
    }
    return std::unexpected(lastRetryable.value_or(AresError::ETIMEOUT));
}

}  // namespace mbun::dns
