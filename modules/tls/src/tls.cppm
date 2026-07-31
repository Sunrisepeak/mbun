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
    // The TLS 1.3 half of node's `ciphers` option (the TLS_-prefixed entries),
    // which OpenSSL keeps in a SEPARATE slot reached through
    // SSL_CTX_set_ciphersuites. Same rule as `ciphers`: empty means "leave the
    // engine's default suites in place", never "allow everything", and a list
    // OpenSSL rejects is a hard construction failure.
    std::string cipherSuites {};
    // `ca` is the caller's COMPLETE trust store (node's
    // tls.setDefaultCACertificates replacing addRootCerts()), so the platform
    // default store must not be added underneath it. Only ever NARROWS what is
    // trusted: with this false the platform store is still the fallback, and
    // with it true nothing is trusted beyond what `ca` names.
    bool caIsComplete {false};
    // Trust anchors ADDED to whatever store `ca` (or the platform default)
    // already established, rather than replacing it. There is exactly one
    // source: the chain certificates inside a PKCS#12 archive
    // (node's `pfx`), which node's SecureContext::SetPFX puts straight into the
    // context store with X509_STORE_add_cert — a store it never switches away
    // from the default for. Routing them through `ca` instead would REVOKE
    // trust, because a non-empty `ca` means "this is the complete store" and
    // the platform anchors (and NODE_EXTRA_CA_CERTS) stop being consulted:
    // measured as test-tls-env-extra-ca-with-options going from pass to
    // "unable to verify the first certificate".
    //
    // This WIDENS what is trusted, so it is deliberately not a general option:
    // it carries only certificates the caller themselves supplied inside their
    // own archive, alongside the private key they are authenticating with.
    std::string caExtra {};
    // Fold the peer-name check into OpenSSL's chain verification
    // (SSL_set1_host). node has no equivalent: it verifies the chain in OpenSSL
    // and the NAME in JS (tls.checkServerIdentity), which a caller may replace.
    // Set false ONLY when such a replacement will run in JS — it stands the name
    // check down in one place so it can run in the other, and never affects
    // chain verification. Default true: the strict path is the default path.
    bool hostCheck {true};
    // SERVER: the 48-byte session-ticket key (16B key name + 16B HMAC key + 16B
    // AES key) — node's tls.createServer({ ticketKeys }) / server.setTicketKeys().
    // Session tickets (RFC 5077) are STATELESS: the session travels back to the
    // client encrypted under this key, which is what makes resumption possible
    // at all in this engine, where every connection owns its own SSL_CTX. Empty
    // leaves OpenSSL's per-context random key in place (tickets are still
    // issued, they are simply not resumable by any other context).
    //
    // Not a relaxation: a ticket is accepted only if it decrypts and
    // authenticates under this key, and the session it restores carries the peer
    // identity that was verified when the session was created.
    std::string ticketKeys {};
    // CLIENT: a previously serialised SSL_SESSION (DER, i2d_SSL_SESSION) offered
    // for resumption — node's tls.connect({ session }) / socket.setSession().
    // Empty = full handshake. A blob OpenSSL cannot parse is ignored, exactly as
    // node's SetSession does; it never weakens the handshake that follows.
    std::string sessionDer {};
    // Passphrase for an encrypted `key` PEM — node's options.passphrase (and the
    // per-entry `key: [{ pem, passphrase }]` form, which the JS layer resolves
    // down to this single field). Empty means the EMPTY password, not "prompt":
    // the backend always installs a password callback, so an encrypted key with
    // no passphrase fails with OpenSSL's decrypt error instead of blocking on a
    // terminal prompt that a server process can never answer.
    std::string passphrase {};
    // SERVER: pick the cipher by the SERVER's preference order rather than the
    // client's (SSL_OP_CIPHER_SERVER_PREFERENCE) — node's
    // tls.createServer({ honorCipherOrder }), which node defaults to TRUE for a
    // server and never sets for a client. Purely an ORDERING choice within the
    // set both peers already offered: it can only pick a suite the client also
    // proposed, and letting the operator's order win is the stronger default.
    bool honorCipherOrder {false};
    // SERVER: node's options.dhparam — "auto" for OpenSSL's own RFC 7919 group
    // selection (SSL_CTX_set_dh_auto), otherwise a PEM DH-parameter block. Empty
    // means no finite-field DH at all, which is why every DHE-* cipher suite was
    // unavailable and a server whose ciphers named only DHE suites answered with
    // a handshake failure. Never widens anything by itself: it only makes the
    // DHE suites the caller already selected usable.
    std::string dhParams {};
    // node's options.ecdhCurve (default "auto"). A named list goes to
    // SSL_CTX_set1_groups_list; "auto"/empty leaves OpenSSL's own group
    // preference in place. Restricting the group list can only NARROW what is
    // negotiable.
    std::string ecdhCurve {};

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
