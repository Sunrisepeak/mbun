// openssl.cppm — real TLS record engine backing the mbun.tls seam.
//
// PORT-SOURCE: bun drives TLS through uSockets' SSL layer
//   (src/uws_sys/us_socket_t.rs + bun-zig-src/src/uws_sys/ssl.zig), where each
//   connection owns an SSL* fed by two memory BIOs: the event loop pushes
//   received ciphertext into the read BIO and flushes the write BIO to the
//   socket, while SSL_read/SSL_write move plaintext. bun links BoringSSL; this
//   engine keeps the identical memory-BIO structure but links the ambient
//   OpenSSL (API-compatible for the handshake/read/write/shutdown surface).
//   Certificate/SNI/verify handling mirrors runtime/socket/SSLConfig.
//
// The engine is transport-agnostic on purpose: feed_encrypted/take_encrypted
// are the network edge (bytes from/to a socket), read/write are the app edge.
// This is the seam T-LOOP/fetch/serve plug real fds into (drive the BIOs from
// the reactor's on_data/on_writable). OpenSSL types never cross this interface
// (opaque pimpl), so importers need no OpenSSL headers.
export module mbun.tls.openssl;

import std;
import mbun.tls.tls;

namespace mbun::tls {

export enum class TlsRole : std::uint8_t { client, server };

// What the engine needs from the transport to make further progress, mirroring
// SSL_ERROR_WANT_READ / SSL_ERROR_WANT_WRITE.
export enum class IoWant : std::uint8_t { none, read, write };

// The peer's ephemeral (forward-secrecy) key for this connection — node's
// TLSSocket.getEphemeralKeyInfo(), which reads SSL_get_server_tmp_key and is
// meaningful on the CLIENT only. `type` empty means the negotiated suite has no
// ephemeral key at all (a static-RSA key exchange), which node reports as `{}`.
export struct EphemeralKeyInfo {
    std::string type {};  // "DH" or "ECDH"
    std::string name {};  // ECDH group short name ("prime256v1", "X25519"); empty for DH
    int size {0};         // key strength in bits
};

export class TlsChannel {
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    TlsChannel(TlsRole role, Config config);
    ~TlsChannel();

    TlsChannel(TlsChannel&&) noexcept;
    TlsChannel& operator=(TlsChannel&&) noexcept;
    TlsChannel(const TlsChannel&) = delete;
    TlsChannel& operator=(const TlsChannel&) = delete;

    // Whether construction succeeded (context/cert/BIO setup). A failed channel
    // reports its reason through last_error().
    [[nodiscard]] bool valid() const noexcept;

    // Advance the handshake as far as the currently buffered input allows.
    HandshakeState handshake() noexcept;
    [[nodiscard]] HandshakeState handshake_state() const noexcept;
    [[nodiscard]] bool established() const noexcept;
    [[nodiscard]] IoWant want() const noexcept;
    [[nodiscard]] std::string_view last_error() const noexcept;
    // node-style error code for the last failure, derived from the OpenSSL error
    // reason (e.g. "ERR_SSL_NO_SUPPORTED_VERSIONS_ENABLED"). Empty when the
    // failure carries no OpenSSL reason (transport-level error). Feeds the JS
    // error.code so node:tls callers see the same code as node.
    [[nodiscard]] std::string_view error_code() const noexcept;

    // Network edge (ciphertext).
    void feed_encrypted(std::span<const std::uint8_t> data);
    [[nodiscard]] bool has_encrypted() const noexcept;
    std::vector<std::uint8_t> take_encrypted();

    // Application edge (plaintext).
    std::size_t write(std::span<const std::uint8_t> plain) noexcept;
    std::vector<std::uint8_t> read();

    // Send close_notify. Returns true once the peer's close_notify was seen.
    bool shutdown() noexcept;
    [[nodiscard]] bool shutdown_complete() const noexcept;

    // Negotiated parameters (empty until the handshake completes).
    [[nodiscard]] std::string tls_version() const;
    [[nodiscard]] std::string cipher() const;
    // The IANA/RFC name of the negotiated suite (SSL_CIPHER_standard_name), e.g.
    // "TLS_RSA_WITH_AES_256_CBC_SHA256" for the OpenSSL name "AES256-SHA256".
    // node's TLSSocket.getCipher().standardName; it is a DIFFERENT string from
    // cipher() for every <=TLS1.2 suite, so it cannot be derived from it.
    [[nodiscard]] std::string cipher_standard_name() const;
    [[nodiscard]] std::string peer_server_name() const; // server side: SNI seen
    // The peer's leaf certificate as PEM (client side: the server cert; server
    // side: the client cert when one was requested+sent). Empty when there is no
    // peer certificate. Feeds node's TLSSocket.getPeerCertificate().
    [[nodiscard]] std::string peer_certificate_pem() const;
    // SSL_get_verify_result == X509_V_OK — node's socket.authorized.
    [[nodiscard]] bool verify_ok() const noexcept;
    // Negotiated ALPN protocol (SSL_get0_alpn_selected), empty if none.
    [[nodiscard]] std::string alpn_protocol() const;
    // Drain the NSS-format key-material lines OpenSSL has produced so far, each
    // WITHOUT its trailing newline, and clear the buffer. Feeds node's
    // TLSSocket/Server 'keylog' event; this is the only way the material leaves
    // the engine, so a caller with no keylog listener never sees it.
    [[nodiscard]] std::vector<std::string> take_keylog();
    // The peer's certificate chain as PEM, leaf first (SSL_get_peer_cert_chain,
    // with the leaf prepended on the client side where OpenSSL omits it). Feeds
    // node's getPeerCertificate(detailed) `issuerCertificate` chain walk.
    [[nodiscard]] std::vector<std::string> peer_certificate_chain_pem() const;
    // The Finished messages of the completed handshake (SSL_get_finished /
    // SSL_get_peer_finished) — node's TLSSocket.getFinished()/getPeerFinished().
    // Empty before the handshake completes.
    [[nodiscard]] std::vector<std::uint8_t> finished() const;
    [[nodiscard]] std::vector<std::uint8_t> peer_finished() const;
    // RFC 5705 exporter (SSL_export_keying_material) — node's
    // TLSSocket.exportKeyingMaterial(). `useContext` distinguishes "no context"
    // from "empty context", which produce different output per the RFC. Empty
    // result = the export failed (e.g. handshake not complete).
    [[nodiscard]] std::vector<std::uint8_t> export_keying_material(
        std::size_t length, std::string_view label,
        std::span<const std::uint8_t> context, bool useContext) const;

    // ---- session resumption (node's 'session' event / tls.connect({session})) --
    // Sessions OpenSSL handed us through SSL_CTX_sess_set_new_cb since the last
    // call, each already serialised with i2d_SSL_SESSION, oldest first. Draining
    // is destructive so a polling caller cannot emit the same session twice.
    // TLS 1.2 delivers its one session during the handshake; TLS 1.3 delivers
    // NewSessionTicket messages AFTER it, so this must keep being polled while
    // the connection reads. Client role only — a server issues tickets, it does
    // not receive them.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> take_new_sessions();
    // The CURRENT session as DER (node's TLSSocket.getSession()). For TLS 1.3
    // the session available immediately after the handshake is the unresumable
    // placeholder node documents; the resumable one arrives via take_new_sessions.
    [[nodiscard]] std::vector<std::uint8_t> session_der() const;
    // SSL_get_server_tmp_key — node's TLSSocket.getEphemeralKeyInfo(). A default
    // (empty `type`) result means the suite carries no ephemeral key.
    [[nodiscard]] EphemeralKeyInfo ephemeral_key_info() const;
    // SSL_session_reused — node's TLSSocket.isSessionReused().
    [[nodiscard]] bool session_reused() const noexcept;
    // The raw session ticket of the current session (SSL_SESSION_get0_ticket) —
    // node's TLSSocket.getTLSTicket(). Empty when the session carries none.
    [[nodiscard]] std::vector<std::uint8_t> tls_ticket() const;
};

// Does this private key belong to this certificate? node's
// SecureContext::SetKey runs SSL_CTX_use_PrivateKey, which fails on a key that
// does not match the already-loaded certificate, and SecureContext::SetCert
// likewise rejects a PEM it cannot read. Returns an OpenSSL reason string on
// failure and an empty string on success, so node:tls can throw at
// createSecureContext() time rather than at first connection.
// `passphrase` decrypts an encrypted private key; an empty one is still USED (an
// empty password), never turned into OpenSSL's interactive terminal prompt.
// `ciphers` is the caller's cipher-list option, applied to the throwaway context
// BEFORE the certificate is loaded — node's SecureContext::Init sets ciphers
// first, and that ORDER is observable: `@SECLEVEL=0` in the list is what lets a
// 1024-bit key load at all (test-tls-reduced-SECLEVEL-in-cipher). Empty leaves
// the context's default list, and therefore its default security level, alone.
export std::string check_key_cert_pair(std::string_view certPem, std::string_view keyPem,
                                       std::string_view passphrase,
                                       std::string_view ciphers = {});

// Every certificate in the platform trust store, as PEM. node ships the Mozilla
// NSS root set in src/node_root_certs.h and exposes it as tls.rootCertificates;
// mbun has no vendored bundle, so it reports the store it actually verifies
// against (the same file/dir configure_default_trust_ picks). Never synthesises
// or accepts anything beyond what that store already contains.
export std::vector<std::string> platform_root_certificates();

// Ephemeral self-signed cert+key (PEM), used to stand up a local TLS server for
// tests and Bun.serve's implicit self-signed path. Mirrors what bun does with
// generateKeyPair + a self-signed X509 for its dev server.
export struct CertKeyPem {
    std::string cert {};
    std::string key {};
};

export std::optional<CertKeyPem> make_self_signed(std::string_view commonName);

// Map a node TLS version name ("TLSv1" / "TLSv1.1" / "TLSv1.2" / "TLSv1.3") to
// the OpenSSL version constant for Config::minVersion / maxVersion. Returns 0
// for an empty or unrecognized name (the backend keeps its default bound).
export int tls_version_from_name(std::string_view name);

} // namespace mbun::tls
