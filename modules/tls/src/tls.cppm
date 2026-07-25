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
    // TLS1_3_VERSION, …); 0 = unset (the backend imposes node's default floor of
    // TLS 1.2). node's options.minVersion / options.maxVersion
    // (lib/internal/tls/secure-context).
    //
    // -1 means EXPLICITLY UNPINNED — the caller passed `secureProtocol`, which in
    // node hands min=max=0 to SecureContext::Init so the chosen SSL_METHOD's own
    // range applies instead of tls.DEFAULT_MIN/MAX_VERSION
    // (lib/internal/tls/common.js). This is reachable only from an explicit
    // `secureProtocol` option; it does NOT widen the default window, and even
    // then the linked OpenSSL still refuses a legacy version unless the caller's
    // own cipher string lowers the security level (`@SECLEVEL=0`).
    int minVersion {0};
    int maxVersion {0};
    // OpenSSL cipher list string (node's options.ciphers, default
    // tls.DEFAULT_CIPHERS). EMPTY MEANS "leave the context's default in place" —
    // it must never be interpreted as "allow everything". A non-empty list that
    // OpenSSL cannot parse is a hard construction failure, not a fallback to the
    // default: silently ignoring a caller's cipher restriction is how a
    // connection ends up weaker than the operator asked for.
    std::string ciphers {};
    // Fold the peer-name check into OpenSSL's chain verification
    // (SSL_set1_host). node has no equivalent: it verifies the chain in OpenSSL
    // and the NAME in JS (tls.checkServerIdentity), which a caller may replace.
    // Set false ONLY when such a replacement will run in JS — it stands the name
    // check down in one place so it can run in the other, and never affects
    // chain verification. Default true: the strict path is the default path.
    bool hostCheck {true};

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
