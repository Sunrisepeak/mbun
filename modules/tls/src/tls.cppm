// tls.cppm — backend-neutral TLS configuration and handshake state.
//
// ref: bun-ref/src/runtime/socket/{SSLConfig,tls_socket_functions}.rs and
// bun-zig-src/src/runtime/socket/{SSLConfig,tls_socket_functions}.zig.
// Certificate parsing, verification, and BoringSSL calls are DEFERRED(S-net).
export module mbun.tls.tls;

import std;

namespace mbun::tls {

export enum class VerifyMode : std::uint8_t { required, optional, disabled };

export struct Config {
    std::string ca {};
    std::string certificate {};
    std::string key {};
    std::string serverName {};
    VerifyMode verify {VerifyMode::required};
    // ALPN protocol names in preference order (e.g. {"h2","http/1.1"}). Client:
    // offered to the server; server: the set it will select from. Empty = no ALPN.
    std::vector<std::string> alpnProtocols {};
    // Protocol-version pins as raw OpenSSL version constants (TLS1_2_VERSION,
    // TLS1_3_VERSION, …); 0 = unset (backend keeps its default range). node's
    // options.minVersion / options.maxVersion (lib/internal/tls/secure-context).
    int minVersion {0};
    int maxVersion {0};

    [[nodiscard]] bool has_credentials() const noexcept {
        return !certificate.empty() || !key.empty();
    }
};

export enum class HandshakeState : std::uint8_t { not_started, in_progress, complete, failed };

export class Session {
private:
    Config config_ {};
    HandshakeState state_ {HandshakeState::not_started};

public:
    explicit Session(Config config = {}) : config_ {std::move(config)} {}

    [[nodiscard]] const Config& config() const noexcept { return config_; }
    [[nodiscard]] HandshakeState state() const noexcept { return state_; }

    void start() noexcept { state_ = HandshakeState::in_progress; }
    void complete() noexcept { state_ = HandshakeState::complete; }
    void fail() noexcept { state_ = HandshakeState::failed; }
    [[nodiscard]] bool established() const noexcept {
        return state_ == HandshakeState::complete;
    }
};

} // namespace mbun::tls
