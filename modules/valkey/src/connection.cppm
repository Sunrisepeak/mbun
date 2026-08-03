export module mbun.valkey.connection;

import std;

export namespace mbun::valkey {

enum class Protocol { standalone, standalone_unix, standalone_tls, standalone_tls_unix };

[[nodiscard]] constexpr auto protocol_from_scheme(std::string_view scheme) noexcept -> std::optional<Protocol> {
    if (scheme == "redis" || scheme == "valkey") return Protocol::standalone;
    if (scheme == "rediss" || scheme == "valkeys" || scheme == "redis+tls" || scheme == "valkey+tls") return Protocol::standalone_tls;
    if (scheme == "redis+unix" || scheme == "valkey+unix") return Protocol::standalone_unix;
    if (scheme == "redis+tls+unix" || scheme == "valkey+tls+unix") return Protocol::standalone_tls_unix;
    return std::nullopt;
}

[[nodiscard]] constexpr auto is_tls(Protocol protocol) noexcept -> bool {
    return protocol == Protocol::standalone_tls || protocol == Protocol::standalone_tls_unix;
}
[[nodiscard]] constexpr auto is_unix(Protocol protocol) noexcept -> bool {
    return protocol == Protocol::standalone_unix || protocol == Protocol::standalone_tls_unix;
}

struct Address {
    std::string host {};
    std::uint16_t port { 6379 };
    std::string unix_path {};
    [[nodiscard]] auto uses_unix_socket() const noexcept -> bool { return !unix_path.empty(); }
};

struct ConnectionOptions {
    std::uint32_t idle_timeout_ms { 0 };
    std::uint32_t connection_timeout_ms { 10'000 };
    std::uint32_t max_retries { 20 };
    bool enable_auto_reconnect { true };
    bool enable_offline_queue { true };
    bool enable_auto_pipelining { true };
    bool enable_debug_logging { false };
};

enum class ConnectionStatus { disconnected, connecting, connected };

struct ConnectionFlags {
    bool authenticated { false };
    bool manually_closed { false };
    bool selecting_database { false };
    bool reconnecting { false };
    bool failed { false };
    bool finalized { false };
};

struct ConnectionState {
    ConnectionStatus status { ConnectionStatus::disconnected };
    Protocol protocol { Protocol::standalone };
    Address address {};
    ConnectionOptions options {};
    ConnectionFlags flags {};
    // DEFERRED(S-net): socket/TLS backend is intentionally not represented here yet.
};

struct ValkeyContext {};

}
