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
    [[nodiscard]] std::string peer_server_name() const; // server side: SNI seen
    // The peer's leaf certificate as PEM (client side: the server cert; server
    // side: the client cert when one was requested+sent). Empty when there is no
    // peer certificate. Feeds node's TLSSocket.getPeerCertificate().
    [[nodiscard]] std::string peer_certificate_pem() const;
    // SSL_get_verify_result == X509_V_OK — node's socket.authorized.
    [[nodiscard]] bool verify_ok() const noexcept;
    // Negotiated ALPN protocol (SSL_get0_alpn_selected), empty if none.
    [[nodiscard]] std::string alpn_protocol() const;
};

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
